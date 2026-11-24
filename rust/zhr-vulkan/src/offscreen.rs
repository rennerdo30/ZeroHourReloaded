use ash::vk;
use gpu_allocator::MemoryLocation;
use gpu_allocator::vulkan::{Allocation, AllocationCreateDesc, AllocationScheme};

use crate::buffer::GpuBuffer;
use crate::context::VkContext;
use crate::error::{Result, VkError};

/// Offscreen RGBA8 color + depth-stencil render target for headless rendering and PNG capture.
pub struct OffscreenTarget {
    pub width: u32,
    pub height: u32,
    pub color_format: vk::Format,
    pub depth_format: vk::Format,
    pub color_image: vk::Image,
    pub color_view: vk::ImageView,
    color_alloc: Option<Allocation>,
    pub depth_image: vk::Image,
    pub depth_view: vk::ImageView,
    pub depth_sample_view: vk::ImageView,
    depth_alloc: Option<Allocation>,
    pub color_layout: vk::ImageLayout,
}

impl OffscreenTarget {
    pub fn new(ctx: &mut VkContext, width: u32, height: u32) -> Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let color_format = vk::Format::R8G8B8A8_UNORM;
        let depth_format = ctx.depth_format;

        let (color_image, color_view, color_alloc) = create_attachment(
            ctx,
            "zhr_offscreen_color",
            w,
            h,
            color_format,
            vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::TRANSFER_SRC,
            vk::ImageAspectFlags::COLOR,
        )?;

        let (depth_image, depth_view, depth_alloc) = create_attachment(
            ctx,
            "zhr_offscreen_depth",
            w,
            h,
            depth_format,
            vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT | vk::ImageUsageFlags::SAMPLED,
            vk::ImageAspectFlags::DEPTH,
        )?;
        let depth_sample_view = create_depth_sample_view(&ctx.device, depth_image, depth_format)?;

        Ok(Self {
            width: w,
            height: h,
            color_format,
            depth_format,
            color_image,
            color_view,
            color_alloc: Some(color_alloc),
            depth_image,
            depth_view,
            depth_sample_view,
            depth_alloc: Some(depth_alloc),
            color_layout: vk::ImageLayout::UNDEFINED,
        })
    }

    /// Reads back the color attachment into tightly packed RGBA8 bytes (`width * height * 4`).
    pub fn readback_rgba8(&mut self, ctx: &mut VkContext) -> Result<Vec<u8>> {
        let byte_len = (self.width as usize) * (self.height as usize) * 4;
        let staging = GpuBuffer::new(
            ctx,
            "zhr_readback_staging",
            byte_len as u64,
            vk::BufferUsageFlags::TRANSFER_DST,
            MemoryLocation::GpuToCpu,
        )?;

        let color_image = self.color_image;
        let old_layout = self.color_layout;
        let width = self.width;
        let height = self.height;

        ctx.submit_immediate(|device, cmd| {
            let subresource = vk::ImageSubresourceRange::default()
                .aspect_mask(vk::ImageAspectFlags::COLOR)
                .base_mip_level(0)
                .level_count(1)
                .base_array_layer(0)
                .layer_count(1);

            let to_src = [vk::ImageMemoryBarrier2::default()
                .src_stage_mask(vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT)
                .src_access_mask(vk::AccessFlags2::COLOR_ATTACHMENT_WRITE)
                .dst_stage_mask(vk::PipelineStageFlags2::TRANSFER)
                .dst_access_mask(vk::AccessFlags2::TRANSFER_READ)
                .old_layout(old_layout)
                .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
                .image(color_image)
                .subresource_range(subresource)];
            let dep = vk::DependencyInfo::default().image_memory_barriers(&to_src);
            // SAFETY: Image is valid and command buffer is recording.
            unsafe { device.cmd_pipeline_barrier2(cmd, &dep) };

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
                    width,
                    height,
                    depth: 1,
                })];
            // SAFETY: Staging buffer is sized to hold width * height * 4 RGBA8 bytes.
            unsafe {
                device.cmd_copy_image_to_buffer(
                    cmd,
                    color_image,
                    vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    staging.buffer,
                    &copy,
                );
            }
        })?;

        self.color_layout = vk::ImageLayout::TRANSFER_SRC_OPTIMAL;
        let pixels = staging.mapped_slice()?[..byte_len].to_vec();
        staging.destroy(ctx);
        Ok(pixels)
    }

    pub fn destroy(mut self, ctx: &mut VkContext) {
        // SAFETY: Caller ensures GPU is idle.
        unsafe {
            ctx.device.destroy_image_view(self.color_view, None);
            ctx.device.destroy_image(self.color_image, None);
            ctx.device.destroy_image_view(self.depth_sample_view, None);
            ctx.device.destroy_image_view(self.depth_view, None);
            ctx.device.destroy_image(self.depth_image, None);
        }
        if let Some(a) = self.color_alloc.take() {
            let _ = ctx.allocator_mut().free(a);
        }
        if let Some(a) = self.depth_alloc.take() {
            let _ = ctx.allocator_mut().free(a);
        }
    }
}

pub fn create_attachment(
    ctx: &mut VkContext,
    name: &str,
    width: u32,
    height: u32,
    format: vk::Format,
    usage: vk::ImageUsageFlags,
    aspect: vk::ImageAspectFlags,
) -> Result<(vk::Image, vk::ImageView, Allocation)> {
    let img_info = vk::ImageCreateInfo::default()
        .image_type(vk::ImageType::TYPE_2D)
        .format(format)
        .extent(vk::Extent3D {
            width: width.max(1),
            height: height.max(1),
            depth: 1,
        })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(vk::ImageTiling::OPTIMAL)
        .usage(usage)
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED);

    // SAFETY: `device` is valid.
    let image = unsafe { ctx.device.create_image(&img_info, None)? };
    let requirements = unsafe { ctx.device.get_image_memory_requirements(image) };
    let allocation = ctx.allocator_mut().allocate(&AllocationCreateDesc {
        name,
        requirements,
        location: MemoryLocation::GpuOnly,
        linear: false,
        allocation_scheme: AllocationScheme::GpuAllocatorManaged,
    })?;
    // SAFETY: `allocation` matches image requirements.
    unsafe {
        ctx.device
            .bind_image_memory(image, allocation.memory(), allocation.offset())?;
    }

    let view_aspect = if aspect.contains(vk::ImageAspectFlags::DEPTH)
        && matches!(
            format,
            vk::Format::D32_SFLOAT_S8_UINT | vk::Format::D24_UNORM_S8_UINT
        ) {
        vk::ImageAspectFlags::DEPTH | vk::ImageAspectFlags::STENCIL
    } else {
        aspect
    };
    let view_info = vk::ImageViewCreateInfo::default()
        .image(image)
        .view_type(vk::ImageViewType::TYPE_2D)
        .format(format)
        .subresource_range(
            vk::ImageSubresourceRange::default()
                .aspect_mask(view_aspect)
                .base_mip_level(0)
                .level_count(1)
                .base_array_layer(0)
                .layer_count(1),
        );
    // SAFETY: `image` is valid and bound.
    let view = unsafe { ctx.device.create_image_view(&view_info, None)? };

    // Transition depth/stencil attachments once into DEPTH_STENCIL_ATTACHMENT_OPTIMAL so
    // dynamic rendering never observes UNDEFINED layout.
    if aspect.contains(vk::ImageAspectFlags::DEPTH) {
        let mut barrier_aspect = vk::ImageAspectFlags::DEPTH;
        if matches!(
            format,
            vk::Format::D32_SFLOAT_S8_UINT | vk::Format::D24_UNORM_S8_UINT
        ) {
            barrier_aspect |= vk::ImageAspectFlags::STENCIL;
        }
        ctx.submit_immediate(|device, cmd| {
            let barrier = [vk::ImageMemoryBarrier2::default()
                .src_stage_mask(vk::PipelineStageFlags2::TOP_OF_PIPE)
                .src_access_mask(vk::AccessFlags2::NONE)
                .dst_stage_mask(
                    vk::PipelineStageFlags2::EARLY_FRAGMENT_TESTS
                        | vk::PipelineStageFlags2::LATE_FRAGMENT_TESTS,
                )
                .dst_access_mask(vk::AccessFlags2::DEPTH_STENCIL_ATTACHMENT_WRITE)
                .old_layout(vk::ImageLayout::UNDEFINED)
                .new_layout(vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
                .image(image)
                .subresource_range(
                    vk::ImageSubresourceRange::default()
                        .aspect_mask(barrier_aspect)
                        .base_mip_level(0)
                        .level_count(1)
                        .base_array_layer(0)
                        .layer_count(1),
                )];
            let dep = vk::DependencyInfo::default().image_memory_barriers(&barrier);
            // SAFETY: Valid image transition inside one-shot command buffer.
            unsafe { device.cmd_pipeline_barrier2(cmd, &dep) };
        })?;
    }

    Ok((image, view, allocation))
}

/// Creates a `DEPTH`-only `VkImageView` suitable for fragment-shader depth sampling (SSAO),
/// even when the underlying depth attachment also carries a stencil plane.
pub fn create_depth_sample_view(
    device: &ash::Device,
    image: vk::Image,
    format: vk::Format,
) -> Result<vk::ImageView> {
    let view_info = vk::ImageViewCreateInfo::default()
        .image(image)
        .view_type(vk::ImageViewType::TYPE_2D)
        .format(format)
        .subresource_range(
            vk::ImageSubresourceRange::default()
                .aspect_mask(vk::ImageAspectFlags::DEPTH)
                .base_mip_level(0)
                .level_count(1)
                .base_array_layer(0)
                .layer_count(1),
        );
    // SAFETY: `image` is a valid depth(-stencil) image created with `SAMPLED` usage.
    let view = unsafe { device.create_image_view(&view_info, None)? };
    Ok(view)
}

/// Encodes RGBA8 pixels to a PNG file on disk.
pub fn write_rgba8_png(path: &std::path::Path, width: u32, height: u32, rgba: &[u8]) -> Result<()> {
    let file = std::fs::File::create(path).map_err(|e| VkError::Io(e.to_string()))?;
    let writer = std::io::BufWriter::new(file);
    let mut encoder = png::Encoder::new(writer, width, height);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    let mut png_writer = encoder
        .write_header()
        .map_err(|e| VkError::Io(e.to_string()))?;
    png_writer
        .write_image_data(rgba)
        .map_err(|e| VkError::Io(e.to_string()))?;
    Ok(())
}
