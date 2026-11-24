//! Cross-platform Vulkan 1.3 renderer for ZeroHourReloaded.
//!
//! Designed around `WW3D2::Renderer` semantics (`Core/Libraries/Source/WWVegas/WW3D2/Renderer.h`)
//! with Vulkan 1.3 dynamic rendering, `gpu-allocator`, WGSL->SPIR-V shaders via `naga`,
//! automatic discrete-GPU selection, and both windowed swapchain & headless offscreen targets.

pub mod buffer;
pub mod context;
pub mod error;
pub mod offscreen;
pub mod pipeline;
pub mod shader;
pub mod surface;
pub mod texture;
pub mod types;

use std::collections::HashMap;
use std::path::Path;

use ash::vk;
use glam::{Mat4, Vec3};
use raw_window_handle::{RawDisplayHandle, RawWindowHandle};

use crate::buffer::{DynamicRingBuffer, GpuBuffer};
use crate::context::VkContext;
use crate::error::{Result, VkError};
use crate::offscreen::{OffscreenTarget, write_rgba8_png};
use crate::pipeline::PipelineCache;
use crate::shader::ShaderKey;
use crate::surface::SwapchainState;
use crate::texture::{GpuTexture, SamplerCache};
use crate::types::{
    BufferId, ClearState, DeviceInfo, DrawUniforms, FogState, GpuLight, RenderStats, SamplerDesc,
    TextureFormat, TextureId, VertexWw3d, Viewport,
};

const FRAMES_IN_FLIGHT: usize = 2;
const DYNAMIC_UNIFORM_BYTES: usize = 4 * 1024 * 1024; // 4 MiB / frame (>10k draws)
const DYNAMIC_VERTEX_BYTES: usize = 16 * 1024 * 1024; // 16 MiB / frame
const DYNAMIC_INDEX_BYTES: usize = 4 * 1024 * 1024; // 4 MiB / frame
const MAX_TEXTURES: u32 = 4096;

/// Renderer configuration.
#[derive(Clone, Debug)]
pub struct RendererConfig {
    pub app_name: String,
    pub width: u32,
    pub height: u32,
    pub vsync: bool,
    pub enable_validation: bool,
    pub preferred_device_index: Option<usize>,
}

impl Default for RendererConfig {
    fn default() -> Self {
        Self {
            app_name: "ZeroHourReloaded".to_string(),
            width: 1280,
            height: 720,
            vsync: true,
            enable_validation: cfg!(debug_assertions),
            preferred_device_index: None,
        }
    }
}

enum RenderTarget {
    Headless(OffscreenTarget),
    Windowed(SwapchainState),
}

struct FrameSlot {
    cmd: vk::CommandBuffer,
    in_flight_fence: vk::Fence,
    image_available: vk::Semaphore,
    render_finished: vk::Semaphore,
    uniform_ring: DynamicRingBuffer,
    uniform_set: vk::DescriptorSet,
    dynamic_vb: DynamicRingBuffer,
    dynamic_ib: DynamicRingBuffer,
}

struct ActiveFrame {
    slot_index: usize,
    swapchain_image_index: Option<u32>,
    target_image: vk::Image,
    target_view: vk::ImageView,
    depth_view: vk::ImageView,
    color_format: vk::Format,
    depth_format: vk::Format,
    extent: vk::Extent2D,
    rendering_begun: bool,
    last_pipeline: Option<vk::Pipeline>,
    last_texture: Option<TextureId>,
}

/// Main Vulkan 1.3 renderer instance (stateful interface matching `WW3D2::Renderer`).
pub struct VulkanRenderer {
    ctx: VkContext,
    target: Option<RenderTarget>,
    pipelines: PipelineCache,
    samplers: SamplerCache,
    uniform_pool: vk::DescriptorPool,
    texture_pool: vk::DescriptorPool,
    frames: Vec<FrameSlot>,
    frame_cursor: usize,
    active_frame: Option<ActiveFrame>,

    // GPU resource registries
    next_buffer_id: u32,
    buffers: HashMap<BufferId, GpuBuffer>,
    next_texture_id: u32,
    textures: HashMap<TextureId, GpuTexture>,
    default_white_texture: TextureId,

    // WW3D2-compatible render state
    uniforms: DrawUniforms,
    shader: ShaderKey,
    active_texture: TextureId,
    viewport: Viewport,
    pending_clear: Option<ClearState>,
    alpha_test_ref: f32,
    stats: RenderStats,
}

impl VulkanRenderer {
    /// Creates a headless offscreen Vulkan 1.3 renderer (for tests, screenshots, or tools).
    pub fn new_headless(config: RendererConfig) -> Result<Self> {
        let mut ctx = VkContext::new(
            &config.app_name,
            config.enable_validation,
            None,
            config.preferred_device_index,
        )?;
        let offscreen = OffscreenTarget::new(&mut ctx, config.width, config.height)?;
        let extent = vk::Extent2D {
            width: offscreen.width,
            height: offscreen.height,
        };
        Self::finish_init(ctx, RenderTarget::Headless(offscreen), extent)
    }

    /// Creates a windowed Vulkan 1.3 renderer presenting to a platform window (`raw-window-handle`).
    pub fn new_windowed(
        config: RendererConfig,
        display: RawDisplayHandle,
        window: RawWindowHandle,
    ) -> Result<Self> {
        let mut ctx = VkContext::new(
            &config.app_name,
            config.enable_validation,
            Some(display),
            config.preferred_device_index,
        )?;
        let swapchain = SwapchainState::new(
            &mut ctx,
            display,
            window,
            config.width,
            config.height,
            config.vsync,
        )?;
        let extent = swapchain.extent;
        Self::finish_init(ctx, RenderTarget::Windowed(swapchain), extent)
    }

    fn finish_init(mut ctx: VkContext, target: RenderTarget, extent: vk::Extent2D) -> Result<Self> {
        let pipelines = PipelineCache::new(&ctx.device)?;
        let mut samplers = SamplerCache::default();

        // Descriptor pool for per-frame set 0 (dynamic uniform buffer).
        let ubo_pool_sizes = [vk::DescriptorPoolSize::default()
            .ty(vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC)
            .descriptor_count(FRAMES_IN_FLIGHT as u32)];
        let ubo_pool_info = vk::DescriptorPoolCreateInfo::default()
            .max_sets(FRAMES_IN_FLIGHT as u32)
            .pool_sizes(&ubo_pool_sizes);
        // SAFETY: `ctx.device` is valid.
        let uniform_pool = unsafe { ctx.device.create_descriptor_pool(&ubo_pool_info, None)? };

        // Descriptor pool for set 1 textures (sampled image + sampler).
        let tex_pool_sizes = [
            vk::DescriptorPoolSize::default()
                .ty(vk::DescriptorType::SAMPLED_IMAGE)
                .descriptor_count(MAX_TEXTURES),
            vk::DescriptorPoolSize::default()
                .ty(vk::DescriptorType::SAMPLER)
                .descriptor_count(MAX_TEXTURES),
        ];
        let tex_pool_info = vk::DescriptorPoolCreateInfo::default()
            .flags(vk::DescriptorPoolCreateFlags::FREE_DESCRIPTOR_SET)
            .max_sets(MAX_TEXTURES)
            .pool_sizes(&tex_pool_sizes);
        // SAFETY: `ctx.device` is valid.
        let texture_pool = unsafe { ctx.device.create_descriptor_pool(&tex_pool_info, None)? };

        let mut frames = Vec::with_capacity(FRAMES_IN_FLIGHT);
        for i in 0..FRAMES_IN_FLIGHT {
            let alloc_info = vk::CommandBufferAllocateInfo::default()
                .command_pool(ctx.command_pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1);
            // SAFETY: `ctx.command_pool` is valid.
            let cmd = unsafe { ctx.device.allocate_command_buffers(&alloc_info)?[0] };

            let fence_info = vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED);
            let sem_info = vk::SemaphoreCreateInfo::default();
            // SAFETY: `ctx.device` is valid.
            let (in_flight_fence, image_available, render_finished) = unsafe {
                (
                    ctx.device.create_fence(&fence_info, None)?,
                    ctx.device.create_semaphore(&sem_info, None)?,
                    ctx.device.create_semaphore(&sem_info, None)?,
                )
            };

            let uniform_ring = DynamicRingBuffer::new(
                &mut ctx,
                &format!("zhr_uniform_ring_{i}"),
                DYNAMIC_UNIFORM_BYTES,
                vk::BufferUsageFlags::UNIFORM_BUFFER,
            )?;
            let dynamic_vb = DynamicRingBuffer::new(
                &mut ctx,
                &format!("zhr_dynamic_vb_{i}"),
                DYNAMIC_VERTEX_BYTES,
                vk::BufferUsageFlags::VERTEX_BUFFER,
            )?;
            let dynamic_ib = DynamicRingBuffer::new(
                &mut ctx,
                &format!("zhr_dynamic_ib_{i}"),
                DYNAMIC_INDEX_BYTES,
                vk::BufferUsageFlags::INDEX_BUFFER,
            )?;

            let set_layouts = [pipelines.set0_uniform_layout];
            let ds_alloc = vk::DescriptorSetAllocateInfo::default()
                .descriptor_pool(uniform_pool)
                .set_layouts(&set_layouts);
            // SAFETY: Pool has enough sets.
            let uniform_set = unsafe { ctx.device.allocate_descriptor_sets(&ds_alloc)?[0] };

            let buf_info = [vk::DescriptorBufferInfo::default()
                .buffer(uniform_ring.gpu.buffer)
                .offset(0)
                .range(std::mem::size_of::<DrawUniforms>() as u64)];
            let write = [vk::WriteDescriptorSet::default()
                .dst_set(uniform_set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC)
                .buffer_info(&buf_info)];
            // SAFETY: `uniform_set` and `uniform_ring.gpu.buffer` are valid.
            unsafe { ctx.device.update_descriptor_sets(&write, &[]) };

            frames.push(FrameSlot {
                cmd,
                in_flight_fence,
                image_available,
                render_finished,
                uniform_ring,
                uniform_set,
                dynamic_vb,
                dynamic_ib,
            });
        }

        // Built-in 1x1 opaque white texture so untextured draws always have a valid descriptor.
        let default_sampler = samplers.get_or_create(&ctx.device, SamplerDesc::default())?;
        let white_tex = GpuTexture::new_2d(
            &mut ctx,
            "zhr_default_white",
            1,
            1,
            TextureFormat::Rgba8Unorm,
            &[255, 255, 255, 255],
            default_sampler,
            texture_pool,
            pipelines.set1_texture_layout,
        )?;
        let default_white_texture = TextureId(1);
        let mut textures = HashMap::new();
        textures.insert(default_white_texture, white_tex);

        Ok(Self {
            ctx,
            target: Some(target),
            pipelines,
            samplers,
            uniform_pool,
            texture_pool,
            frames,
            frame_cursor: 0,
            active_frame: None,
            next_buffer_id: 1,
            buffers: HashMap::new(),
            next_texture_id: 2,
            textures,
            default_white_texture,
            uniforms: DrawUniforms::default(),
            shader: ShaderKey::OPAQUE_3D,
            active_texture: default_white_texture,
            viewport: Viewport::full(extent.width, extent.height),
            pending_clear: None,
            alpha_test_ref: 0.5,
            stats: RenderStats::default(),
        })
    }

    // --- Device queries (maps 1:1 to WW3D2::Renderer::Get_Render_Device_*) ---

    pub fn device_info(&self) -> &DeviceInfo {
        &self.ctx.device_info
    }

    pub fn available_devices(&self) -> &[DeviceInfo] {
        &self.ctx.available_devices
    }

    pub fn selected_device_index(&self) -> usize {
        self.ctx.selected_device_index
    }

    pub fn resolution(&self) -> (u32, u32) {
        match self.target.as_ref().expect("render target alive") {
            RenderTarget::Headless(t) => (t.width, t.height),
            RenderTarget::Windowed(s) => (s.extent.width, s.extent.height),
        }
    }

    pub fn is_windowed(&self) -> bool {
        matches!(self.target, Some(RenderTarget::Windowed(_)))
    }

    pub fn validation_active(&self) -> bool {
        self.ctx.validation_active
    }

    pub fn validation_error_count(&self) -> usize {
        self.ctx.validation_error_count()
    }

    pub fn last_frame_stats(&self) -> RenderStats {
        self.stats
    }

    pub fn resize(&mut self, width: u32, height: u32) -> Result<()> {
        if self.active_frame.is_some() {
            return Err(VkError::State("cannot resize while a frame is active"));
        }
        match self.target.as_mut().expect("render target alive") {
            RenderTarget::Headless(old) => {
                // SAFETY: Ensure GPU idle before replacing offscreen attachments.
                unsafe { self.ctx.device.device_wait_idle()? };
                let new_target = OffscreenTarget::new(&mut self.ctx, width, height)?;
                let prev = std::mem::replace(old, new_target);
                prev.destroy(&mut self.ctx);
            }
            RenderTarget::Windowed(sc) => {
                sc.resize(&mut self.ctx, width, height)?;
            }
        }
        self.viewport = Viewport::full(width.max(1), height.max(1));
        Ok(())
    }

    // --- Resource creation ---

    pub fn create_vertex_buffer(&mut self, vertices: &[VertexWw3d]) -> Result<BufferId> {
        let bytes = bytemuck::cast_slice(vertices);
        let id = BufferId(self.next_buffer_id);
        self.next_buffer_id += 1;
        let buf = GpuBuffer::new_static_with_data(
            &mut self.ctx,
            &format!("zhr_vb_{}", id.0),
            vk::BufferUsageFlags::VERTEX_BUFFER,
            bytes,
        )?;
        self.buffers.insert(id, buf);
        Ok(id)
    }

    pub fn create_index_buffer_u16(&mut self, indices: &[u16]) -> Result<BufferId> {
        let bytes = bytemuck::cast_slice(indices);
        let id = BufferId(self.next_buffer_id);
        self.next_buffer_id += 1;
        let buf = GpuBuffer::new_static_with_data(
            &mut self.ctx,
            &format!("zhr_ib_{}", id.0),
            vk::BufferUsageFlags::INDEX_BUFFER,
            bytes,
        )?;
        self.buffers.insert(id, buf);
        Ok(id)
    }

    pub fn destroy_buffer(&mut self, id: BufferId) -> Result<()> {
        // SAFETY: Ensure GPU work referencing this buffer has completed.
        unsafe { self.ctx.device.device_wait_idle()? };
        if let Some(buf) = self.buffers.remove(&id) {
            buf.destroy(&mut self.ctx);
        }
        Ok(())
    }

    pub fn create_texture_2d(
        &mut self,
        width: u32,
        height: u32,
        format: TextureFormat,
        pixels: &[u8],
        sampler_desc: SamplerDesc,
    ) -> Result<TextureId> {
        let expected = format.expected_byte_size(width, height);
        if pixels.len() < expected {
            return Err(VkError::State("texture pixel buffer smaller than expected"));
        }
        let sampler = self
            .samplers
            .get_or_create(&self.ctx.device, sampler_desc)?;
        let id = TextureId(self.next_texture_id);
        self.next_texture_id += 1;
        let tex = GpuTexture::new_2d(
            &mut self.ctx,
            &format!("zhr_tex_{}", id.0),
            width,
            height,
            format,
            &pixels[..expected],
            sampler,
            self.texture_pool,
            self.pipelines.set1_texture_layout,
        )?;
        self.textures.insert(id, tex);
        Ok(id)
    }

    pub fn destroy_texture(&mut self, id: TextureId) -> Result<()> {
        if id == self.default_white_texture {
            return Ok(());
        }
        // SAFETY: Wait idle before freeing texture/descriptor set.
        unsafe { self.ctx.device.device_wait_idle()? };
        if let Some(tex) = self.textures.remove(&id) {
            let ds = tex.descriptor_set;
            tex.destroy(&mut self.ctx);
            // SAFETY: `texture_pool` was created with FREE_DESCRIPTOR_SET.
            unsafe {
                let _ = self
                    .ctx
                    .device
                    .free_descriptor_sets(self.texture_pool, &[ds]);
            }
        }
        if self.active_texture == id {
            self.active_texture = self.default_white_texture;
        }
        Ok(())
    }

    // --- WW3D2 Render State Setters ---

    pub fn set_world(&mut self, m: Mat4) {
        self.uniforms.set_world(m);
    }

    pub fn set_view(&mut self, m: Mat4) {
        self.uniforms.set_view(m);
    }

    pub fn set_projection(&mut self, m: Mat4) {
        self.uniforms.set_projection(m);
    }

    pub fn set_world_identity(&mut self) {
        self.uniforms.set_world(Mat4::IDENTITY);
    }

    pub fn set_view_identity(&mut self) {
        self.uniforms.set_view(Mat4::IDENTITY);
    }

    pub fn set_ambient(&mut self, color: Vec3) {
        self.uniforms.set_ambient(color);
    }

    pub fn set_lighting_enabled(&mut self, enabled: bool) {
        self.uniforms.set_lighting_enabled(enabled);
    }

    pub fn set_light(&mut self, index: usize, light: GpuLight) {
        if index < 4 {
            self.uniforms.lights[index] = light;
        }
    }

    pub fn clear_light(&mut self, index: usize) {
        if index < 4 {
            self.uniforms.lights[index] = GpuLight::DISABLED;
        }
    }

    pub fn set_fog(&mut self, fog: FogState) {
        self.uniforms.set_fog(fog);
    }

    pub fn set_shader(&mut self, shader: ShaderKey) {
        self.shader = shader;
    }

    pub fn set_alpha_test_ref(&mut self, alpha_ref: f32) {
        self.alpha_test_ref = alpha_ref.clamp(0.0, 1.0);
    }

    pub fn set_texture(&mut self, tex: Option<TextureId>) {
        self.active_texture = tex.unwrap_or(self.default_white_texture);
    }

    pub fn set_viewport(&mut self, viewport: Viewport) {
        self.viewport = viewport;
        if let Some(af) = &self.active_frame
            && af.rendering_begun
        {
            let cmd = self.frames[af.slot_index].cmd;
            apply_viewport_and_scissor(&self.ctx.device, cmd, viewport);
        }
    }

    // --- Frame Lifecycle ---

    pub fn begin_frame(&mut self) -> Result<()> {
        if self.active_frame.is_some() {
            return Err(VkError::State(
                "begin_frame called while a frame is already active",
            ));
        }

        let slot_index = self.frame_cursor;
        self.frame_cursor = (self.frame_cursor + 1) % self.frames.len();
        self.stats = RenderStats::default();

        let fence = self.frames[slot_index].in_flight_fence;
        let image_available = self.frames[slot_index].image_available;
        // SAFETY: `fence` belongs to `ctx.device`.
        unsafe {
            self.ctx.device.wait_for_fences(&[fence], true, u64::MAX)?;
            self.ctx.device.reset_fences(&[fence])?;
        }

        let (
            swapchain_image_index,
            target_image,
            target_view,
            depth_view,
            color_format,
            depth_format,
            extent,
            old_layout,
        ) = match self.target.as_ref().expect("render target alive") {
            RenderTarget::Headless(t) => (
                None,
                t.color_image,
                t.color_view,
                t.depth_view,
                t.color_format,
                t.depth_format,
                vk::Extent2D {
                    width: t.width,
                    height: t.height,
                },
                t.color_layout,
            ),
            RenderTarget::Windowed(sc) => {
                // SAFETY: `swapchain` and `image_available` are valid.
                let (idx, _suboptimal) = unsafe {
                    sc.swapchain_loader.acquire_next_image(
                        sc.swapchain,
                        u64::MAX,
                        image_available,
                        vk::Fence::null(),
                    )?
                };
                (
                    Some(idx),
                    sc.images[idx as usize],
                    sc.views[idx as usize],
                    sc.depth_view,
                    sc.format,
                    self.ctx.depth_format,
                    sc.extent,
                    vk::ImageLayout::UNDEFINED,
                )
            }
        };

        let slot = &mut self.frames[slot_index];
        slot.uniform_ring.reset();
        slot.dynamic_vb.reset();
        slot.dynamic_ib.reset();

        let cmd = slot.cmd;
        // SAFETY: Fence was waited, so `cmd` is no longer in flight.
        unsafe {
            self.ctx
                .device
                .reset_command_buffer(cmd, vk::CommandBufferResetFlags::empty())?;
            let begin = vk::CommandBufferBeginInfo::default()
                .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
            self.ctx.device.begin_command_buffer(cmd, &begin)?;
        }

        // Transition color attachment -> COLOR_ATTACHMENT_OPTIMAL.
        let barrier = [vk::ImageMemoryBarrier2::default()
            .src_stage_mask(vk::PipelineStageFlags2::TOP_OF_PIPE)
            .src_access_mask(vk::AccessFlags2::NONE)
            .dst_stage_mask(vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT)
            .dst_access_mask(vk::AccessFlags2::COLOR_ATTACHMENT_WRITE)
            .old_layout(old_layout)
            .new_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
            .image(target_image)
            .subresource_range(
                vk::ImageSubresourceRange::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .base_mip_level(0)
                    .level_count(1)
                    .base_array_layer(0)
                    .layer_count(1),
            )];
        let dep = vk::DependencyInfo::default().image_memory_barriers(&barrier);
        // SAFETY: Valid image barrier on recording command buffer.
        unsafe { self.ctx.device.cmd_pipeline_barrier2(cmd, &dep) };

        if let Some(RenderTarget::Headless(t)) = &mut self.target {
            t.color_layout = vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL;
        }

        self.pending_clear = None;
        self.active_frame = Some(ActiveFrame {
            slot_index,
            swapchain_image_index,
            target_image,
            target_view,
            depth_view,
            color_format,
            depth_format,
            extent,
            rendering_begun: false,
            last_pipeline: None,
            last_texture: None,
        });
        Ok(())
    }

    /// Queues or executes a clear of the color and/or depth-stencil target.
    pub fn clear(&mut self, clear: ClearState) -> Result<()> {
        let Some(af) = &mut self.active_frame else {
            return Err(VkError::State("clear called outside begin_frame/end_frame"));
        };
        if !af.rendering_begun {
            self.pending_clear = Some(clear);
            self.ensure_rendering_begun()?;
        } else {
            // Mid-pass clear via vkCmdClearAttachments.
            let cmd = self.frames[af.slot_index].cmd;
            let mut attachments = Vec::new();
            if clear.clear_color {
                attachments.push(
                    vk::ClearAttachment::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .color_attachment(0)
                        .clear_value(vk::ClearValue {
                            color: vk::ClearColorValue {
                                float32: clear.color,
                            },
                        }),
                );
            }
            if clear.clear_depth_stencil {
                attachments.push(
                    vk::ClearAttachment::default()
                        .aspect_mask(vk::ImageAspectFlags::DEPTH)
                        .clear_value(vk::ClearValue {
                            depth_stencil: vk::ClearDepthStencilValue {
                                depth: clear.depth,
                                stencil: clear.stencil,
                            },
                        }),
                );
            }
            if !attachments.is_empty() {
                let rects = [vk::ClearRect::default()
                    .rect(vk::Rect2D {
                        offset: vk::Offset2D { x: 0, y: 0 },
                        extent: af.extent,
                    })
                    .base_array_layer(0)
                    .layer_count(1)];
                // SAFETY: Dynamic rendering is active with matching attachments.
                unsafe {
                    self.ctx
                        .device
                        .cmd_clear_attachments(cmd, &attachments, &rects);
                }
            }
        }
        Ok(())
    }

    fn ensure_rendering_begun(&mut self) -> Result<()> {
        let Some(af) = &mut self.active_frame else {
            return Err(VkError::State("draw called outside begin_frame/end_frame"));
        };
        if af.rendering_begun {
            return Ok(());
        }

        let clear = self.pending_clear.take().unwrap_or(ClearState {
            clear_color: false,
            clear_depth_stencil: false,
            ..Default::default()
        });

        let color_att = [vk::RenderingAttachmentInfo::default()
            .image_view(af.target_view)
            .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
            .load_op(if clear.clear_color {
                vk::AttachmentLoadOp::CLEAR
            } else {
                vk::AttachmentLoadOp::LOAD
            })
            .store_op(vk::AttachmentStoreOp::STORE)
            .clear_value(vk::ClearValue {
                color: vk::ClearColorValue {
                    float32: clear.color,
                },
            })];

        let depth_att = vk::RenderingAttachmentInfo::default()
            .image_view(af.depth_view)
            .image_layout(vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
            .load_op(if clear.clear_depth_stencil {
                vk::AttachmentLoadOp::CLEAR
            } else {
                vk::AttachmentLoadOp::LOAD
            })
            .store_op(vk::AttachmentStoreOp::STORE)
            .clear_value(vk::ClearValue {
                depth_stencil: vk::ClearDepthStencilValue {
                    depth: clear.depth,
                    stencil: clear.stencil,
                },
            });

        let has_stencil = matches!(
            af.depth_format,
            vk::Format::D32_SFLOAT_S8_UINT | vk::Format::D24_UNORM_S8_UINT
        );

        let mut rendering_info = vk::RenderingInfo::default()
            .render_area(vk::Rect2D {
                offset: vk::Offset2D { x: 0, y: 0 },
                extent: af.extent,
            })
            .layer_count(1)
            .color_attachments(&color_att)
            .depth_attachment(&depth_att);
        if has_stencil {
            rendering_info = rendering_info.stencil_attachment(&depth_att);
        }

        let cmd = self.frames[af.slot_index].cmd;
        // SAFETY: Color and depth views are valid for the duration of the command buffer.
        unsafe {
            self.ctx.device.cmd_begin_rendering(cmd, &rendering_info);
        }
        apply_viewport_and_scissor(&self.ctx.device, cmd, self.viewport);
        af.rendering_begun = true;
        Ok(())
    }

    fn bind_draw_state(&mut self) -> Result<(vk::CommandBuffer, usize)> {
        self.ensure_rendering_begun()?;
        let af = self
            .active_frame
            .as_mut()
            .ok_or(VkError::State("no active frame"))?;
        let slot_index = af.slot_index;

        let pso = self.pipelines.get_or_create(
            &self.ctx.device,
            self.shader,
            af.color_format,
            af.depth_format,
        )?;
        let cmd = self.frames[slot_index].cmd;

        if af.last_pipeline != Some(pso) {
            // SAFETY: Pipeline matches active dynamic rendering formats.
            unsafe {
                self.ctx
                    .device
                    .cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pso);
            }
            af.last_pipeline = Some(pso);
            self.stats.pipeline_binds += 1;
        }

        // Sync dynamic shader flags into the per-draw uniform snapshot.
        let mut draw_uniforms = self.uniforms;
        draw_uniforms.set_texturing(self.shader.texturing_enabled());
        draw_uniforms.set_alpha_test(self.shader.alpha_test_enabled(), self.alpha_test_ref);
        if !self.shader.fog_enabled() {
            draw_uniforms.fog_color_and_enable[3] = 0.0;
        }

        let alignment = self.ctx.min_uniform_offset_alignment as usize;
        let dyn_offset = self.frames[slot_index]
            .uniform_ring
            .push_bytes(bytemuck::bytes_of(&draw_uniforms), alignment)?
            as u32;

        let uniform_set = self.frames[slot_index].uniform_set;
        // SAFETY: `uniform_set` is valid and `dyn_offset` is aligned to device limits.
        unsafe {
            self.ctx.device.cmd_bind_descriptor_sets(
                cmd,
                vk::PipelineBindPoint::GRAPHICS,
                self.pipelines.pipeline_layout,
                0,
                &[uniform_set],
                &[dyn_offset],
            );
        }

        let tex_id = if self.shader.texturing_enabled() {
            self.active_texture
        } else {
            self.default_white_texture
        };
        if af.last_texture != Some(tex_id) {
            let tex = self
                .textures
                .get(&tex_id)
                .ok_or(VkError::InvalidHandle("texture"))?;
            // SAFETY: Texture descriptor set is valid for this frame.
            unsafe {
                self.ctx.device.cmd_bind_descriptor_sets(
                    cmd,
                    vk::PipelineBindPoint::GRAPHICS,
                    self.pipelines.pipeline_layout,
                    1,
                    &[tex.descriptor_set],
                    &[],
                );
            }
            af.last_texture = Some(tex_id);
            self.stats.texture_binds += 1;
        }

        Ok((cmd, slot_index))
    }

    /// Draws indexed triangles from static GPU vertex & index buffers.
    pub fn draw_indexed(
        &mut self,
        vb: BufferId,
        ib: BufferId,
        index_count: u32,
        first_index: u32,
        vertex_offset: i32,
    ) -> Result<()> {
        let vb_vk = self
            .buffers
            .get(&vb)
            .ok_or(VkError::InvalidHandle("vertex buffer"))?
            .buffer;
        let ib_vk = self
            .buffers
            .get(&ib)
            .ok_or(VkError::InvalidHandle("index buffer"))?
            .buffer;

        let (cmd, _) = self.bind_draw_state()?;
        // SAFETY: Buffers exist and state is bound inside active rendering pass.
        unsafe {
            self.ctx
                .device
                .cmd_bind_vertex_buffers(cmd, 0, &[vb_vk], &[0]);
            self.ctx
                .device
                .cmd_bind_index_buffer(cmd, ib_vk, 0, vk::IndexType::UINT16);
            self.ctx
                .device
                .cmd_draw_indexed(cmd, index_count, 1, first_index, vertex_offset, 0);
        }
        self.stats.draw_calls += 1;
        self.stats.triangles += index_count / 3;
        Ok(())
    }

    /// Uploads transient vertices & 16-bit indices into the per-frame ring buffer and draws immediately.
    /// Ideal for WW3D2 `DynamicVBAccessClass` / `DynamicIBAccessClass`, particles, UI, and debug geometry.
    pub fn draw_dynamic(&mut self, vertices: &[VertexWw3d], indices: &[u16]) -> Result<()> {
        if vertices.is_empty() || indices.is_empty() {
            return Ok(());
        }
        let (cmd, slot_index) = self.bind_draw_state()?;

        let slot = &mut self.frames[slot_index];
        let vb_offset = slot
            .dynamic_vb
            .push_bytes(bytemuck::cast_slice(vertices), 16)?;
        let ib_offset = slot
            .dynamic_ib
            .push_bytes(bytemuck::cast_slice(indices), 4)?;
        let vb_vk = slot.dynamic_vb.gpu.buffer;
        let ib_vk = slot.dynamic_ib.gpu.buffer;

        // SAFETY: Offsets lie inside the frame's dynamic ring buffers and match bound pipeline.
        unsafe {
            self.ctx
                .device
                .cmd_bind_vertex_buffers(cmd, 0, &[vb_vk], &[vb_offset]);
            self.ctx
                .device
                .cmd_bind_index_buffer(cmd, ib_vk, ib_offset, vk::IndexType::UINT16);
            self.ctx
                .device
                .cmd_draw_indexed(cmd, indices.len() as u32, 1, 0, 0, 0);
        }
        self.stats.draw_calls += 1;
        self.stats.triangles += (indices.len() as u32) / 3;
        Ok(())
    }

    pub fn end_frame(&mut self) -> Result<()> {
        // Ensure a frame with only `begin_frame` + `clear` still executes the clear pass.
        if self
            .active_frame
            .as_ref()
            .is_some_and(|af| !af.rendering_begun)
            && self.pending_clear.is_some()
        {
            self.ensure_rendering_begun()?;
        }

        let af = self.active_frame.take().ok_or(VkError::State(
            "end_frame called without matching begin_frame",
        ))?;
        let slot = &self.frames[af.slot_index];
        let cmd = slot.cmd;

        if af.rendering_begun {
            // SAFETY: `cmd_begin_rendering` was called for this frame.
            unsafe { self.ctx.device.cmd_end_rendering(cmd) };
        }

        // For windowed targets, transition swapchain image to PRESENT_SRC_KHR.
        if af.swapchain_image_index.is_some() {
            let to_present = [vk::ImageMemoryBarrier2::default()
                .src_stage_mask(vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT)
                .src_access_mask(vk::AccessFlags2::COLOR_ATTACHMENT_WRITE)
                .dst_stage_mask(vk::PipelineStageFlags2::BOTTOM_OF_PIPE)
                .dst_access_mask(vk::AccessFlags2::NONE)
                .old_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                .new_layout(vk::ImageLayout::PRESENT_SRC_KHR)
                .image(af.target_image)
                .subresource_range(
                    vk::ImageSubresourceRange::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .base_mip_level(0)
                        .level_count(1)
                        .base_array_layer(0)
                        .layer_count(1),
                )];
            let dep = vk::DependencyInfo::default().image_memory_barriers(&to_present);
            // SAFETY: Valid image transition before present.
            unsafe { self.ctx.device.cmd_pipeline_barrier2(cmd, &dep) };
        }

        // SAFETY: Recording is finished.
        unsafe { self.ctx.device.end_command_buffer(cmd)? };

        let cmd_bufs = [cmd];
        if let (Some(img_idx), Some(RenderTarget::Windowed(sc))) =
            (af.swapchain_image_index, &self.target)
        {
            let wait_sems = [slot.image_available];
            let wait_stages = [vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT];
            let signal_sems = [slot.render_finished];
            let submit = [vk::SubmitInfo::default()
                .wait_semaphores(&wait_sems)
                .wait_dst_stage_mask(&wait_stages)
                .command_buffers(&cmd_bufs)
                .signal_semaphores(&signal_sems)];
            // SAFETY: Sync primitives belong to `ctx.device`.
            unsafe {
                self.ctx.device.queue_submit(
                    self.ctx.graphics_queue,
                    &submit,
                    slot.in_flight_fence,
                )?;
            }

            let swapchains = [sc.swapchain];
            let image_indices = [img_idx];
            let present_info = vk::PresentInfoKHR::default()
                .wait_semaphores(&signal_sems)
                .swapchains(&swapchains)
                .image_indices(&image_indices);
            // SAFETY: Swapchain image is in PRESENT_SRC_KHR and signaled.
            match unsafe {
                sc.swapchain_loader
                    .queue_present(self.ctx.graphics_queue, &present_info)
            } {
                Ok(_) | Err(vk::Result::SUBOPTIMAL_KHR) => {}
                Err(vk::Result::ERROR_OUT_OF_DATE_KHR) => {
                    log::info!("Swapchain out of date on present; caller should resize");
                }
                Err(e) => return Err(e.into()),
            }
        } else {
            let submit = [vk::SubmitInfo::default().command_buffers(&cmd_bufs)];
            // SAFETY: Headless submission synchronized by `in_flight_fence`.
            unsafe {
                self.ctx.device.queue_submit(
                    self.ctx.graphics_queue,
                    &submit,
                    slot.in_flight_fence,
                )?;
                self.ctx
                    .device
                    .wait_for_fences(&[slot.in_flight_fence], true, u64::MAX)?;
            }
        }

        Ok(())
    }

    /// Reads back the last rendered headless frame as tightly-packed RGBA8 bytes (`width * height * 4`).
    pub fn readback_rgba8(&mut self) -> Result<Vec<u8>> {
        if self.active_frame.is_some() {
            return Err(VkError::State("call end_frame before readback_rgba8"));
        }
        match self.target.as_mut().expect("render target alive") {
            RenderTarget::Headless(offscreen) => offscreen.readback_rgba8(&mut self.ctx),
            RenderTarget::Windowed(_) => Err(VkError::State(
                "readback_rgba8 is currently implemented for headless render targets",
            )),
        }
    }

    /// Saves the last rendered headless frame directly to a PNG file.
    pub fn save_screenshot_png(&mut self, path: impl AsRef<Path>) -> Result<()> {
        let (w, h) = self.resolution();
        let rgba = self.readback_rgba8()?;
        write_rgba8_png(path.as_ref(), w, h, &rgba)
    }
}

impl Drop for VulkanRenderer {
    fn drop(&mut self) {
        // SAFETY: Wait for GPU idle, then destroy all owned Vulkan objects before `ctx` drops.
        unsafe {
            let _ = self.ctx.device.device_wait_idle();
        }
        let buffers: Vec<GpuBuffer> = self.buffers.drain().map(|(_, b)| b).collect();
        for b in buffers {
            b.destroy(&mut self.ctx);
        }
        let textures: Vec<GpuTexture> = self.textures.drain().map(|(_, t)| t).collect();
        for t in textures {
            t.destroy(&mut self.ctx);
        }
        for frame in self.frames.drain(..) {
            frame.uniform_ring.destroy(&mut self.ctx);
            frame.dynamic_vb.destroy(&mut self.ctx);
            frame.dynamic_ib.destroy(&mut self.ctx);
            unsafe {
                self.ctx.device.destroy_fence(frame.in_flight_fence, None);
                self.ctx
                    .device
                    .destroy_semaphore(frame.image_available, None);
                self.ctx
                    .device
                    .destroy_semaphore(frame.render_finished, None);
            }
        }
        self.pipelines.destroy_all(&self.ctx.device);
        self.samplers.destroy_all(&self.ctx.device);
        unsafe {
            self.ctx
                .device
                .destroy_descriptor_pool(self.uniform_pool, None);
            self.ctx
                .device
                .destroy_descriptor_pool(self.texture_pool, None);
        }

        if let Some(target) = self.target.take() {
            match target {
                RenderTarget::Headless(off) => off.destroy(&mut self.ctx),
                RenderTarget::Windowed(sc) => sc.destroy(&mut self.ctx),
            }
        }
    }
}

fn apply_viewport_and_scissor(device: &ash::Device, cmd: vk::CommandBuffer, vp: Viewport) {
    // Negative height flips Vulkan NDC Y to match DirectX / WW3D2 Y-up clip coordinates
    // while keeping clockwise front-face winding identical to D3D8.
    let vk_vp = [vk::Viewport {
        x: vp.x as f32,
        y: (vp.y + vp.height) as f32,
        width: vp.width.max(1) as f32,
        height: -(vp.height.max(1) as f32),
        min_depth: vp.min_z,
        max_depth: vp.max_z,
    }];
    let vk_scissor = [vk::Rect2D {
        offset: vk::Offset2D {
            x: vp.x as i32,
            y: vp.y as i32,
        },
        extent: vk::Extent2D {
            width: vp.width.max(1),
            height: vp.height.max(1),
        },
    }];
    // SAFETY: Dynamic viewport and scissor states are enabled on all pipelines.
    unsafe {
        device.cmd_set_viewport(cmd, 0, &vk_vp);
        device.cmd_set_scissor(cmd, 0, &vk_scissor);
    }
}
