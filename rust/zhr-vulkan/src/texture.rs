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
    pub mip_levels: u32,
    pub format: TextureFormat,
}

impl GpuTexture {
    /// Creates a 2D texture with automatic GPU-blitted mipmaps (`>= 4x4`), recording its upload
    /// directly into an active command buffer (outside dynamic rendering) with zero `vkQueueWaitIdle` stall.
    /// The transient staging buffer is pushed into `retired_buffers` to be freed after the frame fence signals.
    #[allow(clippy::too_many_arguments)]
    pub fn new_2d_recorded(
        ctx: &mut VkContext,
        cmd: vk::CommandBuffer,
        retired_buffers: &mut Vec<GpuBuffer>,
        name: &str,
        width: u32,
        height: u32,
        format: TextureFormat,
        pixels: &[u8],
        sampler: vk::Sampler,
        descriptor_pool: vk::DescriptorPool,
        set_layout: vk::DescriptorSetLayout,
    ) -> Result<Self> {
        let generate_mips = !format.is_compressed() && width >= 4 && height >= 4;
        Self::create_internal(
            ctx,
            Some((cmd, retired_buffers)),
            name,
            width,
            height,
            1,
            generate_mips,
            format,
            pixels,
            sampler,
            descriptor_pool,
            set_layout,
        )
    }

    /// Creates a multi-mip 2D texture, recording its upload directly into an active command buffer
    /// (outside dynamic rendering) with zero `vkQueueWaitIdle` stall.
    #[allow(clippy::too_many_arguments)]
    pub fn new_2d_with_mips_recorded(
        ctx: &mut VkContext,
        cmd: vk::CommandBuffer,
        retired_buffers: &mut Vec<GpuBuffer>,
        name: &str,
        width: u32,
        height: u32,
        provided_mips: u32,
        format: TextureFormat,
        pixels: &[u8],
        sampler: vk::Sampler,
        descriptor_pool: vk::DescriptorPool,
        set_layout: vk::DescriptorSetLayout,
    ) -> Result<Self> {
        let generate_mips =
            provided_mips <= 1 && !format.is_compressed() && width >= 4 && height >= 4;
        Self::create_internal(
            ctx,
            Some((cmd, retired_buffers)),
            name,
            width,
            height,
            provided_mips.max(1),
            generate_mips,
            format,
            pixels,
            sampler,
            descriptor_pool,
            set_layout,
        )
    }

    /// Creates a 2D texture with automatic GPU-blitted mipmaps for uncompressed textures (`>= 4x4`),
    /// or single-mip upload for compressed textures.
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
        let generate_mips = !format.is_compressed() && width >= 4 && height >= 4;
        Self::create_internal(
            ctx,
            None,
            name,
            width,
            height,
            1,
            generate_mips,
            format,
            pixels,
            sampler,
            descriptor_pool,
            set_layout,
        )
    }

    /// Creates a 2D texture from a tightly-packed buffer containing `provided_mips` mip levels
    /// (ideal for `.dds` `BC1`/`BC2`/`BC3` mip chains).
    #[allow(clippy::too_many_arguments)]
    pub fn new_2d_with_mips(
        ctx: &mut VkContext,
        name: &str,
        width: u32,
        height: u32,
        provided_mips: u32,
        format: TextureFormat,
        pixels: &[u8],
        sampler: vk::Sampler,
        descriptor_pool: vk::DescriptorPool,
        set_layout: vk::DescriptorSetLayout,
    ) -> Result<Self> {
        let generate_mips =
            provided_mips <= 1 && !format.is_compressed() && width >= 4 && height >= 4;
        Self::create_internal(
            ctx,
            None,
            name,
            width,
            height,
            provided_mips.max(1),
            generate_mips,
            format,
            pixels,
            sampler,
            descriptor_pool,
            set_layout,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn create_internal(
        ctx: &mut VkContext,
        recorded: Option<(vk::CommandBuffer, &mut Vec<GpuBuffer>)>,
        name: &str,
        width: u32,
        height: u32,
        provided_mips: u32,
        generate_mips: bool,
        format: TextureFormat,
        pixels: &[u8],
        sampler: vk::Sampler,
        descriptor_pool: vk::DescriptorPool,
        set_layout: vk::DescriptorSetLayout,
    ) -> Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let vk_format = format.to_vk();

        let max_possible_mips = (w.max(h) as f32).log2().floor() as u32 + 1;
        let mip_levels = if generate_mips {
            // SAFETY: `physical_device` belongs to `ctx.instance`.
            let fmt_props = unsafe {
                ctx.instance
                    .get_physical_device_format_properties(ctx.physical_device, vk_format)
            };
            let can_blit = fmt_props.optimal_tiling_features.contains(
                vk::FormatFeatureFlags::SAMPLED_IMAGE_FILTER_LINEAR
                    | vk::FormatFeatureFlags::BLIT_SRC
                    | vk::FormatFeatureFlags::BLIT_DST,
            );
            if can_blit { max_possible_mips } else { 1 }
        } else {
            provided_mips.clamp(1, max_possible_mips)
        };

        let do_gpu_blit_mips = generate_mips && mip_levels > 1;
        let mut usage = vk::ImageUsageFlags::SAMPLED | vk::ImageUsageFlags::TRANSFER_DST;
        if do_gpu_blit_mips {
            usage |= vk::ImageUsageFlags::TRANSFER_SRC;
        }

        let img_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(vk_format)
            .extent(vk::Extent3D {
                width: w,
                height: h,
                depth: 1,
            })
            .mip_levels(mip_levels)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(usage)
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

        // Compute buffer copy regions for all CPU-provided mip levels.
        let upload_mip_count = if do_gpu_blit_mips { 1 } else { mip_levels };
        let mut copy_regions = Vec::with_capacity(upload_mip_count as usize);
        let mut byte_offset = 0usize;
        let mut mw = w;
        let mut mh = h;
        for level in 0..upload_mip_count {
            let level_bytes = format.expected_byte_size(mw, mh);
            if byte_offset + level_bytes > pixels.len() {
                break;
            }
            copy_regions.push(
                vk::BufferImageCopy::default()
                    .buffer_offset(byte_offset as u64)
                    .image_subresource(
                        vk::ImageSubresourceLayers::default()
                            .aspect_mask(vk::ImageAspectFlags::COLOR)
                            .mip_level(level)
                            .base_array_layer(0)
                            .layer_count(1),
                    )
                    .image_extent(vk::Extent3D {
                        width: mw,
                        height: mh,
                        depth: 1,
                    }),
            );
            byte_offset += level_bytes;
            mw = (mw >> 1).max(1);
            mh = (mh >> 1).max(1);
        }

        let uploaded_levels = copy_regions.len() as u32;
        let effective_mips = if do_gpu_blit_mips {
            mip_levels
        } else {
            uploaded_levels.max(1)
        };

        // Upload pixels via transient staging buffer.
        let mut staging = GpuBuffer::new(
            ctx,
            "zhr_tex_staging",
            byte_offset.max(16) as u64,
            vk::BufferUsageFlags::TRANSFER_SRC,
            MemoryLocation::CpuToGpu,
        )?;
        staging.write_bytes(0, &pixels[..byte_offset])?;

        let record_upload = |device: &ash::Device, cmd: vk::CommandBuffer| {
            let all_mips = vk::ImageSubresourceRange::default()
                .aspect_mask(vk::ImageAspectFlags::COLOR)
                .base_mip_level(0)
                .level_count(mip_levels)
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
                .subresource_range(all_mips)];
            let dep1 = vk::DependencyInfo::default().image_memory_barriers(&to_dst);
            // SAFETY: Valid image & barrier inside active command buffer.
            unsafe { device.cmd_pipeline_barrier2(cmd, &dep1) };

            // SAFETY: Staging buffer contains valid tightly-packed pixel data for `image`.
            unsafe {
                device.cmd_copy_buffer_to_image(
                    cmd,
                    staging.buffer,
                    image,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    &copy_regions,
                );
            }

            if do_gpu_blit_mips {
                let mut src_w = w as i32;
                let mut src_h = h as i32;
                for level in 1..mip_levels {
                    let prev_level = level - 1;
                    let dst_w = (src_w >> 1).max(1);
                    let dst_h = (src_h >> 1).max(1);

                    let prev_to_src = [vk::ImageMemoryBarrier2::default()
                        .src_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                        .src_access_mask(vk::AccessFlags2::TRANSFER_WRITE)
                        .dst_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                        .dst_access_mask(vk::AccessFlags2::TRANSFER_READ)
                        .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                        .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
                        .image(image)
                        .subresource_range(
                            vk::ImageSubresourceRange::default()
                                .aspect_mask(vk::ImageAspectFlags::COLOR)
                                .base_mip_level(prev_level)
                                .level_count(1)
                                .base_array_layer(0)
                                .layer_count(1),
                        )];
                    let dep_src = vk::DependencyInfo::default().image_memory_barriers(&prev_to_src);
                    unsafe { device.cmd_pipeline_barrier2(cmd, &dep_src) };

                    let blit = [vk::ImageBlit::default()
                        .src_subresource(
                            vk::ImageSubresourceLayers::default()
                                .aspect_mask(vk::ImageAspectFlags::COLOR)
                                .mip_level(prev_level)
                                .base_array_layer(0)
                                .layer_count(1),
                        )
                        .src_offsets([
                            vk::Offset3D { x: 0, y: 0, z: 0 },
                            vk::Offset3D {
                                x: src_w,
                                y: src_h,
                                z: 1,
                            },
                        ])
                        .dst_subresource(
                            vk::ImageSubresourceLayers::default()
                                .aspect_mask(vk::ImageAspectFlags::COLOR)
                                .mip_level(level)
                                .base_array_layer(0)
                                .layer_count(1),
                        )
                        .dst_offsets([
                            vk::Offset3D { x: 0, y: 0, z: 0 },
                            vk::Offset3D {
                                x: dst_w,
                                y: dst_h,
                                z: 1,
                            },
                        ])];
                    // SAFETY: Source mip is in TRANSFER_SRC_OPTIMAL, dest mip is in TRANSFER_DST_OPTIMAL.
                    unsafe {
                        device.cmd_blit_image(
                            cmd,
                            image,
                            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                            image,
                            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                            &blit,
                            vk::Filter::LINEAR,
                        );
                    }

                    let prev_to_read = [vk::ImageMemoryBarrier2::default()
                        .src_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                        .src_access_mask(vk::AccessFlags2::TRANSFER_READ)
                        .dst_stage_mask(vk::PipelineStageFlags2::FRAGMENT_SHADER)
                        .dst_access_mask(vk::AccessFlags2::SHADER_SAMPLED_READ)
                        .old_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
                        .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                        .image(image)
                        .subresource_range(
                            vk::ImageSubresourceRange::default()
                                .aspect_mask(vk::ImageAspectFlags::COLOR)
                                .base_mip_level(prev_level)
                                .level_count(1)
                                .base_array_layer(0)
                                .layer_count(1),
                        )];
                    let dep_read =
                        vk::DependencyInfo::default().image_memory_barriers(&prev_to_read);
                    unsafe { device.cmd_pipeline_barrier2(cmd, &dep_read) };

                    src_w = dst_w;
                    src_h = dst_h;
                }

                // Transition final mip level to SHADER_READ_ONLY_OPTIMAL.
                let last_to_read = [vk::ImageMemoryBarrier2::default()
                    .src_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                    .src_access_mask(vk::AccessFlags2::TRANSFER_WRITE)
                    .dst_stage_mask(vk::PipelineStageFlags2::FRAGMENT_SHADER)
                    .dst_access_mask(vk::AccessFlags2::SHADER_SAMPLED_READ)
                    .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                    .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                    .image(image)
                    .subresource_range(
                        vk::ImageSubresourceRange::default()
                            .aspect_mask(vk::ImageAspectFlags::COLOR)
                            .base_mip_level(mip_levels - 1)
                            .level_count(1)
                            .base_array_layer(0)
                            .layer_count(1),
                    )];
                let dep_last = vk::DependencyInfo::default().image_memory_barriers(&last_to_read);
                unsafe { device.cmd_pipeline_barrier2(cmd, &dep_last) };
            } else {
                let to_shader = [vk::ImageMemoryBarrier2::default()
                    .src_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                    .src_access_mask(vk::AccessFlags2::TRANSFER_WRITE)
                    .dst_stage_mask(vk::PipelineStageFlags2::FRAGMENT_SHADER)
                    .dst_access_mask(vk::AccessFlags2::SHADER_SAMPLED_READ)
                    .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                    .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                    .image(image)
                    .subresource_range(all_mips)];
                let dep2 = vk::DependencyInfo::default().image_memory_barriers(&to_shader);
                // SAFETY: Valid transition to shader-read-only optimal.
                unsafe { device.cmd_pipeline_barrier2(cmd, &dep2) };
            }
        };

        if let Some((cmd, retired_buffers)) = recorded {
            record_upload(&ctx.device, cmd);
            retired_buffers.push(staging);
        } else {
            ctx.submit_immediate(record_upload)?;
            staging.destroy(ctx);
        }

        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(vk_format)
            .subresource_range(
                vk::ImageSubresourceRange::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .base_mip_level(0)
                    .level_count(effective_mips)
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
            width: w,
            height: h,
            mip_levels: effective_mips,
            format,
        })
    }

    pub fn destroy(mut self, ctx: &mut VkContext) {
        // SAFETY: Caller ensures GPU is idle.
        unsafe {
            ctx.device.destroy_image_view(self.view, None);
            ctx.device.destroy_image(self.image, None);
        }
        if let Some(alloc) = self.allocation.take() {
            let _ = ctx.allocator_mut().free(alloc);
        }
    }
}

/// Caches `VkSampler` objects by `SamplerDesc` with 16x Anisotropic Filtering when supported.
#[derive(Default)]
pub struct SamplerCache {
    samplers: HashMap<SamplerDesc, vk::Sampler>,
}

impl SamplerCache {
    pub fn get_or_create(
        &mut self,
        device: &ash::Device,
        desc: SamplerDesc,
        max_anisotropy: f32,
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
        let use_aniso = desc.linear_filter && max_anisotropy > 1.0;
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
            .anisotropy_enable(use_aniso)
            .max_anisotropy(if use_aniso { max_anisotropy } else { 1.0 })
            .mip_lod_bias(-0.25)
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
