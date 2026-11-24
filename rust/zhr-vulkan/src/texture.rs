use std::collections::HashMap;

use ash::vk;
use gpu_allocator::MemoryLocation;
use gpu_allocator::vulkan::{Allocation, AllocationCreateDesc, AllocationScheme};

use crate::buffer::GpuBuffer;
use crate::context::VkContext;
use crate::error::Result;
use crate::types::{SamplerDesc, TextureFormat};

pub struct GpuTexture {
    pub image: vk::Image,
    pub view: vk::ImageView,
    pub allocation: Option<Allocation>,
    pub descriptor_set: vk::DescriptorSet,
    pub width: u32,
    pub height: u32,
    pub format: TextureFormat,
}

impl GpuTexture {
    #[allow(clippy::too_many_arguments)]
    pub fn new_2d(
        ctx: &mut VkContext,
        name: &str,
        width: u32,
        height: u32,
        format: TextureFormat,
        pixels: &[u8],
        sampler: vk::Sampler,
        descriptor_pool: vk::DescriptorPool,
        set_layout: vk::DescriptorSetLayout,
    ) -> Result<Self> {
        let vk_format = format.to_vk();
        let img_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(vk_format)
            .extent(vk::Extent3D {
                width: width.max(1),
                height: height.max(1),
                depth: 1,
            })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::SAMPLED | vk::ImageUsageFlags::TRANSFER_DST)
            .sharing_mode(vk::SharingMode::EXCLUSIVE)
            .initial_layout(vk::ImageLayout::UNDEFINED);

        // SAFETY: `device` is valid.
        let image = unsafe { ctx.device.create_image(&img_info, None)? };
        // SAFETY: `image` was just created on `device`.
        let requirements = unsafe { ctx.device.get_image_memory_requirements(image) };
        let allocation = ctx.allocator_mut().allocate(&AllocationCreateDesc {
            name,
            requirements,
            location: MemoryLocation::GpuOnly,
            linear: false,
            allocation_scheme: AllocationScheme::GpuAllocatorManaged,
        })?;
        // SAFETY: `allocation` satisfies `image` requirements.
        unsafe {
            ctx.device
                .bind_image_memory(image, allocation.memory(), allocation.offset())?;
        }

        // Upload pixels via transient staging buffer.
        let mut staging = GpuBuffer::new(
            ctx,
            "zhr_tex_staging",
            pixels.len().max(16) as u64,
            vk::BufferUsageFlags::TRANSFER_SRC,
            MemoryLocation::CpuToGpu,
        )?;
        staging.write_bytes(0, pixels)?;

        ctx.submit_immediate(|device, cmd| {
            let subresource_range = vk::ImageSubresourceRange::default()
                .aspect_mask(vk::ImageAspectFlags::COLOR)
                .base_mip_level(0)
                .level_count(1)
                .base_array_layer(0)
                .layer_count(1);

            let to_dst = [vk::ImageMemoryBarrier2::default()
                .src_stage_mask(vk::PipelineStageFlags2::TOP_OF_PIPE)
                .src_access_mask(vk::AccessFlags2::NONE)
                .dst_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                .dst_access_mask(vk::AccessFlags2::TRANSFER_WRITE)
                .old_layout(vk::ImageLayout::UNDEFINED)
                .new_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                .image(image)
                .subresource_range(subresource_range)];
            let dep1 = vk::DependencyInfo::default().image_memory_barriers(&to_dst);
            // SAFETY: Valid image & barrier inside active command buffer.
            unsafe { device.cmd_pipeline_barrier2(cmd, &dep1) };

            let copy = [vk::BufferImageCopy::default()
                .buffer_offset(0)
                .image_subresource(
                    vk::ImageSubresourceLayers::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .mip_level(0)
                        .base_array_layer(0)
                        .layer_count(1),
                )
                .image_extent(vk::Extent3D {
                    width: width.max(1),
                    height: height.max(1),
                    depth: 1,
                })];
            // SAFETY: Staging buffer contains valid tightly-packed pixel data for `image`.
            unsafe {
                device.cmd_copy_buffer_to_image(
                    cmd,
                    staging.buffer,
                    image,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    &copy,
                );
            }

            let to_shader = [vk::ImageMemoryBarrier2::default()
                .src_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                .src_access_mask(vk::AccessFlags2::TRANSFER_WRITE)
                .dst_stage_mask(vk::PipelineStageFlags2::FRAGMENT_SHADER)
                .dst_access_mask(vk::AccessFlags2::SHADER_SAMPLED_READ)
                .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                .image(image)
                .subresource_range(subresource_range)];
            let dep2 = vk::DependencyInfo::default().image_memory_barriers(&to_shader);
            // SAFETY: Valid transition to shader-read-only optimal.
            unsafe { device.cmd_pipeline_barrier2(cmd, &dep2) };
        })?;

        staging.destroy(ctx);

        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(vk_format)
            .subresource_range(
                vk::ImageSubresourceRange::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .base_mip_level(0)
                    .level_count(1)
                    .base_array_layer(0)
                    .layer_count(1),
            );
        // SAFETY: `image` is bound and initialized.
        let view = unsafe { ctx.device.create_image_view(&view_info, None)? };

        let layouts = [set_layout];
        let alloc_info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(descriptor_pool)
            .set_layouts(&layouts);
        // SAFETY: `descriptor_pool` has capacity and matches `set_layout`.
        let descriptor_set = unsafe { ctx.device.allocate_descriptor_sets(&alloc_info)?[0] };

        let img_desc = [vk::DescriptorImageInfo::default()
            .image_view(view)
            .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
        let samp_desc = [vk::DescriptorImageInfo::default().sampler(sampler)];
        let writes = [
            vk::WriteDescriptorSet::default()
                .dst_set(descriptor_set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                .image_info(&img_desc),
            vk::WriteDescriptorSet::default()
                .dst_set(descriptor_set)
                .dst_binding(1)
                .descriptor_type(vk::DescriptorType::SAMPLER)
                .image_info(&samp_desc),
        ];
        // SAFETY: `descriptor_set`, `view`, and `sampler` are valid.
        unsafe { ctx.device.update_descriptor_sets(&writes, &[]) };

        Ok(Self {
            image,
            view,
            allocation: Some(allocation),
            descriptor_set,
            width,
            height,
            format,
        })
    }

    pub fn destroy(mut self, ctx: &mut VkContext) {
        // SAFETY: Caller ensures GPU work referencing this texture is complete.
        unsafe {
            ctx.device.destroy_image_view(self.view, None);
            ctx.device.destroy_image(self.image, None);
        }
        if let Some(alloc) = self.allocation.take() {
            let _ = ctx.allocator_mut().free(alloc);
        }
    }
}

/// Caches `VkSampler` objects by `SamplerDesc`.
#[derive(Default)]
pub struct SamplerCache {
    samplers: HashMap<SamplerDesc, vk::Sampler>,
}

impl SamplerCache {
    pub fn get_or_create(
        &mut self,
        device: &ash::Device,
        desc: SamplerDesc,
    ) -> Result<vk::Sampler> {
        if let Some(&s) = self.samplers.get(&desc) {
            return Ok(s);
        }
        let filter = if desc.linear_filter {
            vk::Filter::LINEAR
        } else {
            vk::Filter::NEAREST
        };
        let addr = if desc.clamp_edge {
            vk::SamplerAddressMode::CLAMP_TO_EDGE
        } else {
            vk::SamplerAddressMode::REPEAT
        };
        let info = vk::SamplerCreateInfo::default()
            .mag_filter(filter)
            .min_filter(filter)
            .mipmap_mode(if desc.linear_filter {
                vk::SamplerMipmapMode::LINEAR
            } else {
                vk::SamplerMipmapMode::NEAREST
            })
            .address_mode_u(addr)
            .address_mode_v(addr)
            .address_mode_w(addr)
            .max_lod(vk::LOD_CLAMP_NONE);
        // SAFETY: `device` is valid.
        let sampler = unsafe { device.create_sampler(&info, None)? };
        self.samplers.insert(desc, sampler);
        Ok(sampler)
    }

    pub fn destroy_all(&mut self, device: &ash::Device) {
        for (_, s) in self.samplers.drain() {
            // SAFETY: Called on shutdown after device idle.
            unsafe { device.destroy_sampler(s, None) };
        }
    }
}
