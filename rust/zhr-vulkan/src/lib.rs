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
pub mod postprocess;
pub mod shader;
pub mod surface;
pub mod texture;
pub mod types;

use std::collections::HashMap;
use std::path::Path;
use std::time::Instant;

use ash::vk;
use glam::{Mat4, Vec3};
use raw_window_handle::{RawDisplayHandle, RawWindowHandle};
use zhr_remaster::{RemasterJob, RemasterWorker};

use crate::buffer::{DynamicRingBuffer, GpuBuffer};
use crate::context::VkContext;
use crate::error::{Result, VkError};
use crate::offscreen::{OffscreenTarget, write_rgba8_png};
use crate::pipeline::PipelineCache;
use crate::postprocess::{HDR_FORMAT, PostProcessPipeline};
use crate::shader::{DepthCompare, DstBlend, ShaderKey, SrcBlend, StencilState};
use crate::surface::SwapchainState;
use crate::texture::{GpuTexture, SamplerCache};
use crate::types::{
    BufferId, ClearState, DeviceInfo, DrawUniforms, FogState, GpuLight, GraphicsMode,
    GraphicsSettings, RenderStats, SamplerDesc, TextureFormat, TextureId, VertexWw3d, Viewport,
};

const FRAMES_IN_FLIGHT: usize = 2;
const DYNAMIC_UNIFORM_BYTES: usize = 8 * 1024 * 1024; // 8 MiB / frame (>20k draws)
const DYNAMIC_VERTEX_BYTES: usize = 32 * 1024 * 1024; // 32 MiB / frame
const DYNAMIC_INDEX_BYTES: usize = 8 * 1024 * 1024; // 8 MiB / frame
const MAX_TEXTURES: u32 = 16384;

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
    uniform_ring: DynamicRingBuffer,
    uniform_set: vk::DescriptorSet,
    dynamic_vb: DynamicRingBuffer,
    dynamic_ib: DynamicRingBuffer,
    retired_textures: Vec<GpuTexture>,
}

struct ActiveFrame {
    slot_index: usize,
    swapchain_image_index: Option<u32>,
    target_image: vk::Image,
    target_view: vk::ImageView,
    target_old_layout: vk::ImageLayout,
    hdr_view: vk::ImageView,
    depth_image: vk::Image,
    depth_view: vk::ImageView,
    color_format: vk::Format,
    depth_format: vk::Format,
    extent: vk::Extent2D,
    rendering_begun: bool,
    has_3d_depth_writes: bool,
    ssao_applied: bool,
    backdrop_captured: bool,
    water_depth_captured: bool,
    last_3d_proj: Mat4,
    last_pipeline: Option<vk::Pipeline>,
    last_texture: Option<(TextureId, bool)>,
    last_texture1: Option<(TextureId, bool)>,
}

/// Main Vulkan 1.3 renderer instance (stateful interface matching `WW3D2::Renderer`).
pub struct VulkanRenderer {
    ctx: VkContext,
    target: Option<RenderTarget>,
    postprocess: Option<PostProcessPipeline>,
    ui_backdrop_set: vk::DescriptorSet,
    water_depth_set: vk::DescriptorSet,
    hdr_initialized: bool,
    pipelines: PipelineCache,
    samplers: SamplerCache,
    uniform_pool: vk::DescriptorPool,
    texture_pool: vk::DescriptorPool,
    frames: Vec<FrameSlot>,
    frame_cursor: usize,
    frame_number: u64,
    start_time: Instant,
    active_frame: Option<ActiveFrame>,

    // GPU resource registries
    next_buffer_id: u32,
    buffers: HashMap<BufferId, GpuBuffer>,
    next_texture_id: u32,
    textures: HashMap<TextureId, GpuTexture>,
    texture_samplers: HashMap<TextureId, SamplerDesc>,
    remastered_textures: HashMap<TextureId, GpuTexture>,
    remaster_worker: Option<RemasterWorker>,
    default_white_texture: TextureId,

    // WW3D2-compatible render state
    uniforms: DrawUniforms,
    view_matrix: Mat4,
    proj_matrix: Mat4,
    graphics: GraphicsSettings,
    material_hint: u32,
    shader: ShaderKey,
    stencil: StencilState,
    active_texture: TextureId,
    active_texture1: TextureId,
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
            let (in_flight_fence, image_available) = unsafe {
                (
                    ctx.device.create_fence(&fence_info, None)?,
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
                uniform_ring,
                uniform_set,
                dynamic_vb,
                dynamic_ib,
                retired_textures: Vec::new(),
            });
        }

        let (output_format, depth_sample_view, default_remastered) = match &target {
            RenderTarget::Headless(t) => (t.color_format, t.depth_sample_view, false),
            RenderTarget::Windowed(s) => (s.format, s.depth_sample_view, true),
        };
        let postprocess = PostProcessPipeline::new(
            &mut ctx,
            extent.width,
            extent.height,
            output_format,
            depth_sample_view,
            FRAMES_IN_FLIGHT,
        )?;
        let backdrop_layouts = [pipelines.set1_texture_layout, pipelines.set1_texture_layout];
        let backdrop_alloc = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(texture_pool)
            .set_layouts(&backdrop_layouts);
        // SAFETY: Pool has plenty of sets (`MAX_TEXTURES`).
        let extra_sets = unsafe { ctx.device.allocate_descriptor_sets(&backdrop_alloc)? };
        let ui_backdrop_set = extra_sets[0];
        let water_depth_set = extra_sets[1];
        write_sampled_texture_set(
            &ctx.device,
            ui_backdrop_set,
            postprocess.ui_backdrop_view(),
            postprocess.linear_sampler(),
        );
        write_sampled_texture_set(
            &ctx.device,
            water_depth_set,
            postprocess.water_depth_view(),
            postprocess.linear_sampler(),
        );
        let graphics = GraphicsSettings::from_env(default_remastered);

        // Built-in 1x1 opaque white texture so untextured draws always have a valid descriptor.
        let default_sampler =
            samplers.get_or_create(&ctx.device, SamplerDesc::default(), ctx.max_anisotropy)?;
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

        let enable_remaster = default_remastered
            && std::env::var("ZHR_REMASTER_TEXTURES")
                .map(|v| v != "0" && !v.eq_ignore_ascii_case("false"))
                .unwrap_or(true);
        let remaster_worker = if enable_remaster {
            Some(RemasterWorker::with_defaults())
        } else {
            None
        };

        Ok(Self {
            ctx,
            target: Some(target),
            postprocess: Some(postprocess),
            ui_backdrop_set,
            water_depth_set,
            hdr_initialized: false,
            pipelines,
            samplers,
            uniform_pool,
            texture_pool,
            frames,
            frame_cursor: 0,
            frame_number: 0,
            start_time: Instant::now(),
            active_frame: None,
            next_buffer_id: 1,
            buffers: HashMap::new(),
            next_texture_id: 2,
            textures,
            texture_samplers: HashMap::new(),
            remastered_textures: HashMap::new(),
            remaster_worker,
            default_white_texture,
            uniforms: DrawUniforms::default(),
            view_matrix: Mat4::IDENTITY,
            proj_matrix: Mat4::IDENTITY,
            graphics,
            material_hint: 0,
            shader: ShaderKey::OPAQUE_3D,
            stencil: StencilState::default(),
            active_texture: default_white_texture,
            active_texture1: default_white_texture,
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

    pub fn is_frame_active(&self) -> bool {
        self.active_frame.is_some()
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
        let (actual_w, actual_h, output_format, depth_sample_view) =
            match self.target.as_mut().expect("render target alive") {
                RenderTarget::Headless(old) => {
                    // SAFETY: Ensure GPU idle before replacing offscreen attachments.
                    unsafe { self.ctx.device.device_wait_idle()? };
                    let new_target = OffscreenTarget::new(&mut self.ctx, width, height)?;
                    let res = (
                        new_target.width,
                        new_target.height,
                        new_target.color_format,
                        new_target.depth_sample_view,
                    );
                    let prev = std::mem::replace(old, new_target);
                    prev.destroy(&mut self.ctx);
                    res
                }
                RenderTarget::Windowed(sc) => {
                    sc.resize(&mut self.ctx, width, height)?;
                    (
                        sc.extent.width,
                        sc.extent.height,
                        sc.format,
                        sc.depth_sample_view,
                    )
                }
            };
        if let Some(pp) = &mut self.postprocess {
            pp.resize(
                &mut self.ctx,
                actual_w,
                actual_h,
                output_format,
                depth_sample_view,
            )?;
            write_sampled_texture_set(
                &self.ctx.device,
                self.ui_backdrop_set,
                pp.ui_backdrop_view(),
                pp.linear_sampler(),
            );
            write_sampled_texture_set(
                &self.ctx.device,
                self.water_depth_set,
                pp.water_depth_view(),
                pp.linear_sampler(),
            );
            self.hdr_initialized = false;
        }
        self.viewport = Viewport::full(actual_w.max(1), actual_h.max(1));
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
        let sampler =
            self.samplers
                .get_or_create(&self.ctx.device, sampler_desc, self.ctx.max_anisotropy)?;
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
        self.texture_samplers.insert(id, sampler_desc);
        self.submit_texture_for_remaster(id, width, height, format, &pixels[..expected]);
        Ok(id)
    }

    /// Creates a 2D texture from a tightly-packed multi-mip pixel buffer (`mip_count` levels).
    pub fn create_texture_2d_mips(
        &mut self,
        width: u32,
        height: u32,
        mip_count: u32,
        format: TextureFormat,
        pixels: &[u8],
        sampler_desc: SamplerDesc,
    ) -> Result<TextureId> {
        let expected_base = format.expected_byte_size(width, height);
        if pixels.len() < expected_base {
            return Err(VkError::State("texture pixel buffer smaller than expected"));
        }
        let sampler =
            self.samplers
                .get_or_create(&self.ctx.device, sampler_desc, self.ctx.max_anisotropy)?;
        let id = TextureId(self.next_texture_id);
        self.next_texture_id += 1;
        let tex = GpuTexture::new_2d_with_mips(
            &mut self.ctx,
            &format!("zhr_tex_{}", id.0),
            width,
            height,
            mip_count,
            format,
            pixels,
            sampler,
            self.texture_pool,
            self.pipelines.set1_texture_layout,
        )?;
        self.textures.insert(id, tex);
        self.texture_samplers.insert(id, sampler_desc);
        self.submit_texture_for_remaster(id, width, height, format, &pixels[..expected_base]);
        Ok(id)
    }

    fn submit_texture_for_remaster(
        &self,
        id: TextureId,
        width: u32,
        height: u32,
        format: TextureFormat,
        base_pixels: &[u8],
    ) {
        let Some(worker) = self.remaster_worker.as_ref() else {
            return;
        };
        if !(32..=512).contains(&width) || !(32..=512).contains(&height) {
            return;
        }
        let rgba8 = match format {
            TextureFormat::Rgba8Unorm => Some(base_pixels.to_vec()),
            TextureFormat::Bgra8Unorm => Some(zhr_remaster::bgra8_to_rgba8(base_pixels)),
            TextureFormat::Bc1RgbaUnorm => {
                zhr_remaster::decode_bc_to_rgba8(2, width, height, base_pixels)
            }
            TextureFormat::Bc2Unorm => {
                zhr_remaster::decode_bc_to_rgba8(3, width, height, base_pixels)
            }
            TextureFormat::Bc3Unorm => {
                zhr_remaster::decode_bc_to_rgba8(4, width, height, base_pixels)
            }
        };
        if let Some(rgba8) = rgba8 {
            if is_font_alpha_mask(&rgba8) {
                return;
            }
            worker.submit(RemasterJob {
                texture_id: id.0,
                width,
                height,
                rgba8,
            });
        }
    }

    /// Enables the background 2x texture remastering worker (used in tests or runtime opt-in).
    pub fn enable_texture_remastering(&mut self) {
        if self.remaster_worker.is_none() {
            self.remaster_worker = Some(RemasterWorker::with_defaults());
        }
    }

    pub fn remastered_texture_count(&self) -> usize {
        self.remastered_textures.len()
    }

    pub fn destroy_texture(&mut self, id: TextureId) -> Result<()> {
        if id == self.default_white_texture {
            return Ok(());
        }
        if self.active_texture == id {
            self.active_texture = self.default_white_texture;
        }
        if self.active_texture1 == id {
            self.active_texture1 = self.default_white_texture;
        }
        self.texture_samplers.remove(&id);
        if let Some(af) = &mut self.active_frame {
            if af.last_texture.is_some_and(|(t, _)| t == id) {
                af.last_texture = None;
            }
            if af.last_texture1.is_some_and(|(t, _)| t == id) {
                af.last_texture1 = None;
            }
            if let Some(tex) = self.textures.remove(&id) {
                self.frames[af.slot_index].retired_textures.push(tex);
            }
            if let Some(rtex) = self.remastered_textures.remove(&id) {
                self.frames[af.slot_index].retired_textures.push(rtex);
            }
            return Ok(());
        }
        // SAFETY: Outside an active frame, wait idle before freeing texture/descriptor set.
        unsafe { self.ctx.device.device_wait_idle()? };
        for tex in [
            self.textures.remove(&id),
            self.remastered_textures.remove(&id),
        ]
        .into_iter()
        .flatten()
        {
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
        Ok(())
    }

    // --- WW3D2 Render State Setters ---

    pub fn set_world(&mut self, m: Mat4) {
        self.uniforms.set_world(m);
    }

    pub fn set_view(&mut self, m: Mat4) {
        self.view_matrix = m;
        self.uniforms.set_view(m);
    }

    pub fn set_projection(&mut self, m: Mat4) {
        self.proj_matrix = m;
        self.uniforms.set_projection(m);
    }

    pub fn set_stencil(&mut self, stencil: StencilState) {
        self.stencil = stencil;
    }

    pub fn set_world_identity(&mut self) {
        self.uniforms.set_world(Mat4::IDENTITY);
    }

    pub fn set_view_identity(&mut self) {
        self.view_matrix = Mat4::IDENTITY;
        self.uniforms.set_view(Mat4::IDENTITY);
    }

    pub fn set_graphics_settings(&mut self, settings: GraphicsSettings) {
        self.graphics = settings;
    }

    pub fn graphics_settings(&self) -> GraphicsSettings {
        self.graphics
    }

    pub fn set_material_hint(&mut self, hint: u32) {
        self.material_hint = hint;
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

    pub fn set_texture_stage(&mut self, stage: u32, tex: Option<TextureId>) {
        let id = tex.unwrap_or(self.default_white_texture);
        match stage {
            0 => self.active_texture = id,
            1 => self.active_texture1 = id,
            _ => {}
        }
    }

    pub fn set_texture_factor(&mut self, rgba: [f32; 4]) {
        self.uniforms.tfactor = rgba;
    }

    pub fn set_stage_state(
        &mut self,
        stage: u32,
        color_op: u32,
        color_args: u32,
        alpha_op: u32,
        alpha_args: u32,
    ) {
        let ops = [color_op, color_args, alpha_op, alpha_args];
        match stage {
            0 => self.uniforms.stage0_ops = ops,
            1 => self.uniforms.stage1_ops = ops,
            _ => {}
        }
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
            depth_image,
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
                t.depth_image,
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
                    sc.depth_image,
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
        let retired: Vec<GpuTexture> = std::mem::take(&mut slot.retired_textures);
        let cmd = slot.cmd;
        for tex in retired {
            let ds = tex.descriptor_set;
            tex.destroy(&mut self.ctx);
            // SAFETY: Fence was waited, so no in-flight commands reference `ds`.
            unsafe {
                let _ = self
                    .ctx
                    .device
                    .free_descriptor_sets(self.texture_pool, &[ds]);
            }
        }

        // Zero-stall async texture hot-swap: upload up to 2 completed 2x remastered textures per frame.
        if let Some(worker) = self.remaster_worker.as_ref() {
            let finished = worker.poll_completed(2);
            for item in finished {
                let id = TextureId(item.texture_id);
                if !self.textures.contains_key(&id) {
                    continue;
                }
                let sdesc = self.texture_samplers.get(&id).copied().unwrap_or_default();
                if let Ok(sampler) =
                    self.samplers
                        .get_or_create(&self.ctx.device, sdesc, self.ctx.max_anisotropy)
                    && let Ok(up_tex) = GpuTexture::new_2d(
                        &mut self.ctx,
                        &format!("zhr_tex_2x_{}", id.0),
                        item.width,
                        item.height,
                        TextureFormat::Rgba8Unorm,
                        &item.albedo_rgba8,
                        sampler,
                        self.texture_pool,
                        self.pipelines.set1_texture_layout,
                    )
                    && let Some(prev) = self.remastered_textures.insert(id, up_tex)
                {
                    self.frames[slot_index].retired_textures.push(prev);
                }
            }
        }
        // SAFETY: Fence was waited, so `cmd` is no longer in flight.
        unsafe {
            self.ctx
                .device
                .reset_command_buffer(cmd, vk::CommandBufferResetFlags::empty())?;
            let begin = vk::CommandBufferBeginInfo::default()
                .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
            self.ctx.device.begin_command_buffer(cmd, &begin)?;
        }

        // Transition HDR scene color target -> COLOR_ATTACHMENT_OPTIMAL.
        let pp = self.postprocess.as_ref().expect("postprocess alive");
        let hdr_image = pp.hdr_image();
        let hdr_view = pp.hdr_view();
        let hdr_old_layout = if self.hdr_initialized {
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
        } else {
            vk::ImageLayout::UNDEFINED
        };
        let hdr_src_stage = if self.hdr_initialized {
            vk::PipelineStageFlags2::FRAGMENT_SHADER
        } else {
            vk::PipelineStageFlags2::TOP_OF_PIPE
        };
        let hdr_src_access = if self.hdr_initialized {
            vk::AccessFlags2::SHADER_SAMPLED_READ
        } else {
            vk::AccessFlags2::NONE
        };
        let barrier = [vk::ImageMemoryBarrier2::default()
            .src_stage_mask(hdr_src_stage)
            .src_access_mask(hdr_src_access)
            .dst_stage_mask(vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT)
            .dst_access_mask(vk::AccessFlags2::COLOR_ATTACHMENT_WRITE)
            .old_layout(hdr_old_layout)
            .new_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
            .image(hdr_image)
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

        self.pending_clear = None;
        self.active_frame = Some(ActiveFrame {
            slot_index,
            swapchain_image_index,
            target_image,
            target_view,
            target_old_layout: old_layout,
            hdr_view,
            depth_image,
            depth_view,
            color_format,
            depth_format,
            extent,
            rendering_begun: false,
            has_3d_depth_writes: false,
            ssao_applied: false,
            backdrop_captured: false,
            water_depth_captured: false,
            last_3d_proj: Mat4::IDENTITY,
            last_pipeline: None,
            last_texture: None,
            last_texture1: None,
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
                let has_stencil = matches!(
                    af.depth_format,
                    vk::Format::D32_SFLOAT_S8_UINT | vk::Format::D24_UNORM_S8_UINT
                );
                let mut aspect = vk::ImageAspectFlags::DEPTH;
                if has_stencil {
                    aspect |= vk::ImageAspectFlags::STENCIL;
                }
                attachments.push(
                    vk::ClearAttachment::default()
                        .aspect_mask(aspect)
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
            .image_view(af.hdr_view)
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

    fn flush_ssao_if_ready(&mut self, capture_backdrop: bool) -> Result<()> {
        let do_ssao = self.active_frame.as_ref().is_some_and(|af| {
            af.rendering_begun
                && af.has_3d_depth_writes
                && !af.ssao_applied
                && self.graphics.mode != GraphicsMode::Classic
                && self.graphics.pbr_lighting
        });
        let do_backdrop = capture_backdrop
            && self.graphics.mode != GraphicsMode::Classic
            && self.active_frame.as_ref().is_some_and(|af| {
                af.rendering_begun
                    && !af.backdrop_captured
                    && (af.has_3d_depth_writes || self.material_hint == 2)
            });
        if !do_ssao && !do_backdrop {
            return Ok(());
        }
        let (slot_index, depth_image, depth_format, last_3d_proj) = {
            let af = self.active_frame.as_mut().expect("active frame");
            if do_ssao {
                af.ssao_applied = true;
            }
            if do_backdrop {
                af.backdrop_captured = true;
            }
            af.rendering_begun = false;
            af.last_pipeline = None;
            af.last_texture = None;
            af.last_texture1 = None;
            (
                af.slot_index,
                af.depth_image,
                af.depth_format,
                af.last_3d_proj,
            )
        };
        let cmd = self.frames[slot_index].cmd;
        // SAFETY: `cmd_begin_rendering` was active for the 3D scene pass.
        unsafe { self.ctx.device.cmd_end_rendering(cmd) };
        if let Some(pp) = &mut self.postprocess {
            if do_ssao {
                pp.apply_ssao(
                    &self.ctx.device,
                    cmd,
                    slot_index,
                    depth_image,
                    depth_format,
                    last_3d_proj,
                    self.graphics,
                )?;
            }
            if do_backdrop {
                pp.capture_ui_backdrop(&self.ctx.device, cmd, slot_index)?;
            }
        }
        Ok(())
    }

    fn bind_draw_state(&mut self) -> Result<(vk::CommandBuffer, usize)> {
        let is_3d_pass =
            self.shader.depth_compare() != DepthCompare::Always || self.shader.depth_write();
        // Snapshot opaque 3D depth before the first water draw so water gets exact per-pixel depth
        // for Beer-Lambert coastal absorption, soft shoreline edges, animated rock foam, and caustics.
        if is_3d_pass
            && self.material_hint == 1
            && self.graphics.mode != GraphicsMode::Classic
            && self.graphics.pbr_lighting
            && self.active_frame.as_ref().is_some_and(|af| {
                af.rendering_begun && af.has_3d_depth_writes && !af.water_depth_captured
            })
        {
            let (slot_index, depth_image, depth_format, proj) = {
                let af = self.active_frame.as_mut().expect("active frame");
                af.water_depth_captured = true;
                af.rendering_begun = false;
                af.last_pipeline = None;
                af.last_texture = None;
                af.last_texture1 = None;
                (
                    af.slot_index,
                    af.depth_image,
                    af.depth_format,
                    if self.proj_matrix.col(3).z.abs() > 1e-5 {
                        self.proj_matrix
                    } else {
                        af.last_3d_proj
                    },
                )
            };
            let cmd = self.frames[slot_index].cmd;
            // SAFETY: `cmd_begin_rendering` was active before suspending for depth linearization.
            unsafe { self.ctx.device.cmd_end_rendering(cmd) };
            if let Some(pp) = &mut self.postprocess {
                pp.capture_water_depth(
                    &self.ctx.device,
                    cmd,
                    slot_index,
                    depth_image,
                    depth_format,
                    proj,
                )?;
            }
        }

        // When transitioning from 3D world (including stencil shadows) to 2D HUD/UI,
        // apply depth-based bilateral SSAO and capture the frosted-glass backdrop before UI draws on top.
        if !is_3d_pass && !self.stencil.enabled {
            self.flush_ssao_if_ready(true)?;
        }

        self.ensure_rendering_begun()?;
        let af = self
            .active_frame
            .as_mut()
            .ok_or(VkError::State("no active frame"))?;
        if is_3d_pass && self.shader.depth_write() && self.proj_matrix.col(3).z.abs() > 1e-5 {
            af.has_3d_depth_writes = true;
            af.last_3d_proj = self.proj_matrix;
        }
        let slot_index = af.slot_index;

        let pso = self.pipelines.get_or_create(
            &self.ctx.device,
            self.shader,
            self.stencil.pipeline_key(),
            HDR_FORMAT,
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

        // Dynamic stencil masks & reference (zero PSO churn across shadow/occluder passes).
        // SAFETY: All pipelines enable STENCIL_COMPARE_MASK, STENCIL_WRITE_MASK, STENCIL_REFERENCE.
        unsafe {
            let face = vk::StencilFaceFlags::FRONT_AND_BACK;
            self.ctx
                .device
                .cmd_set_stencil_compare_mask(cmd, face, self.stencil.compare_mask);
            self.ctx
                .device
                .cmd_set_stencil_write_mask(cmd, face, self.stencil.write_mask);
            self.ctx
                .device
                .cmd_set_stencil_reference(cmd, face, self.stencil.reference);
        }

        // Sync dynamic shader flags into the per-draw uniform snapshot.
        let mut draw_uniforms = self.uniforms;
        draw_uniforms.set_texturing(self.shader.texturing_enabled());
        draw_uniforms.set_alpha_test(self.shader.alpha_test_enabled(), self.alpha_test_ref);
        if !self.shader.fog_enabled() {
            draw_uniforms.fog_color_and_enable[3] = 0.0;
        }

        // Populate 3D PBR lighting & split-screen uniforms.
        let pbr_mode = if !self.graphics.pbr_lighting || self.graphics.mode == GraphicsMode::Classic
        {
            0.0
        } else if self.graphics.mode == GraphicsMode::SplitScreen {
            2.0
        } else {
            1.0
        };
        let cam_pos = if self.view_matrix.determinant().abs() > 1e-6 {
            self.view_matrix.inverse().w_axis.truncate()
        } else {
            Vec3::new(0.0, 0.0, 100.0)
        };
        draw_uniforms.camera_pos_and_pbr = [cam_pos.x, cam_pos.y, cam_pos.z, pbr_mode];

        let is_additive = self.shader.dst_blend() == DstBlend::One
            && matches!(self.shader.src_blend(), SrcBlend::One | SrcBlend::SrcAlpha);
        let is_multiplicative =
            matches!(self.shader.src_blend(), SrcBlend::Zero | SrcBlend::DstColor)
                || self.shader.dst_blend() == DstBlend::SrcColor;
        let lighting_on = draw_uniforms.ambient_and_lighting[3] > 0.5;

        let is_3d_lit = is_3d_pass && lighting_on && !is_additive && !is_multiplicative;
        let is_hdr_emissive = is_3d_pass && is_additive;
        let is_3d_unlit_surface = is_3d_pass
            && !lighting_on
            && !is_additive
            && !is_multiplicative
            && !self.shader.blend_enabled();
        let split_x_pixels = (af.extent.width as f32) * 0.5;
        draw_uniforms.render_flags = [
            if is_3d_lit { 1.0 } else { 0.0 },
            if is_hdr_emissive { 1.0 } else { 0.0 },
            split_x_pixels,
            if is_3d_unlit_surface { 1.0 } else { 0.0 },
        ];
        let use_ui_glass = !is_3d_pass
            && self.material_hint == 2
            && self.graphics.mode != GraphicsMode::Classic
            && af.backdrop_captured;
        let use_water_depth = is_3d_pass
            && self.material_hint == 1
            && self.graphics.mode != GraphicsMode::Classic
            && self.graphics.pbr_lighting
            && af.water_depth_captured;
        let glass_hint = if use_ui_glass {
            if self.graphics.mode == GraphicsMode::SplitScreen {
                3.0
            } else {
                2.0
            }
        } else if is_3d_pass {
            self.material_hint as f32
        } else {
            0.0
        };
        let screen_inv_w = if use_ui_glass || use_water_depth {
            1.0 / (af.extent.width.max(1) as f32)
        } else {
            0.0
        };
        let screen_inv_h = if use_ui_glass || use_water_depth {
            1.0 / (af.extent.height.max(1) as f32)
        } else {
            0.0
        };
        draw_uniforms.water_params = [
            self.start_time.elapsed().as_secs_f32(),
            glass_hint,
            screen_inv_w,
            screen_inv_h,
        ];

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

        // Keep 2D UI, HUD, and GDI font glyph atlases pixel-sharp; apply 2x remastering to 3D world geometry.
        let use_remastered_tex = is_3d_pass
            && self.graphics.remaster_textures
            && self.graphics.mode != GraphicsMode::Classic;

        let use_combiner = self.uniforms.stage0_ops[0] != 0;
        let tex_id = if use_combiner || self.shader.texturing_enabled() {
            self.active_texture
        } else {
            self.default_white_texture
        };
        let tex0_has_remaster =
            use_remastered_tex && self.remastered_textures.contains_key(&tex_id);
        if af.last_texture != Some((tex_id, tex0_has_remaster)) {
            let tex = if tex0_has_remaster {
                self.remastered_textures.get(&tex_id)
            } else {
                self.textures.get(&tex_id)
            }
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
            af.last_texture = Some((tex_id, tex0_has_remaster));
            self.stats.texture_binds += 1;
        }

        if use_ui_glass {
            let glass_sentinel = TextureId(u32::MAX);
            if af.last_texture1 != Some((glass_sentinel, false)) {
                // SAFETY: `ui_backdrop_set` is valid and `ui_backdrop` is in SHADER_READ_ONLY_OPTIMAL.
                unsafe {
                    self.ctx.device.cmd_bind_descriptor_sets(
                        cmd,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pipelines.pipeline_layout,
                        2,
                        &[self.ui_backdrop_set],
                        &[],
                    );
                }
                af.last_texture1 = Some((glass_sentinel, false));
                self.stats.texture_binds += 1;
            }
        } else if use_water_depth {
            let wd_sentinel = TextureId(u32::MAX - 1);
            if af.last_texture1 != Some((wd_sentinel, false)) {
                // SAFETY: `water_depth_set` is valid and `water_depth` is in SHADER_READ_ONLY_OPTIMAL.
                unsafe {
                    self.ctx.device.cmd_bind_descriptor_sets(
                        cmd,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pipelines.pipeline_layout,
                        2,
                        &[self.water_depth_set],
                        &[],
                    );
                }
                af.last_texture1 = Some((wd_sentinel, false));
                self.stats.texture_binds += 1;
            }
        } else {
            let tex1_id = if self.uniforms.stage1_ops[0] != 0 {
                self.active_texture1
            } else {
                self.default_white_texture
            };
            let tex1_has_remaster =
                use_remastered_tex && self.remastered_textures.contains_key(&tex1_id);
            if af.last_texture1 != Some((tex1_id, tex1_has_remaster)) {
                let tex1 = if tex1_has_remaster {
                    self.remastered_textures.get(&tex1_id)
                } else {
                    self.textures.get(&tex1_id)
                }
                .ok_or(VkError::InvalidHandle("texture1"))?;
                // SAFETY: Texture descriptor set is valid for this frame.
                unsafe {
                    self.ctx.device.cmd_bind_descriptor_sets(
                        cmd,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pipelines.pipeline_layout,
                        2,
                        &[tex1.descriptor_set],
                        &[],
                    );
                }
                af.last_texture1 = Some((tex1_id, tex1_has_remaster));
                self.stats.texture_binds += 1;
            }
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

        // For pure-3D frames with no 2D UI draws, apply SSAO before bloom + composite.
        self.flush_ssao_if_ready(false)?;

        let af = self.active_frame.take().ok_or(VkError::State(
            "end_frame called without matching begin_frame",
        ))?;
        let slot = &self.frames[af.slot_index];
        let cmd = slot.cmd;

        if af.rendering_begun {
            // SAFETY: `cmd_begin_rendering` was called for this frame.
            unsafe { self.ctx.device.cmd_end_rendering(cmd) };
        }

        // Execute HDR bloom + filmic tonemapping + CAS sharpening + A/B split-screen composite into target.
        if let Some(pp) = &mut self.postprocess {
            pp.execute(
                &self.ctx.device,
                cmd,
                af.slot_index,
                af.target_image,
                af.target_view,
                af.target_old_layout,
                af.extent,
                self.graphics,
            )?;
            self.hdr_initialized = true;
        }
        if let Some(RenderTarget::Headless(t)) = &mut self.target {
            t.color_layout = vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL;
        }

        self.frame_number += 1;
        let capture_path = std::env::var("ZHR_VK_CAPTURE_PNG")
            .ok()
            .filter(|s| !s.is_empty());
        let capture_frame: u64 = std::env::var("ZHR_VK_CAPTURE_FRAME")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(30);
        let do_capture = capture_path.is_some() && self.frame_number == capture_frame;

        let capture_staging = if do_capture {
            let byte_len = (af.extent.width as usize) * (af.extent.height as usize) * 4;
            let staging = GpuBuffer::new(
                &mut self.ctx,
                "zhr_swapchain_capture",
                byte_len as u64,
                vk::BufferUsageFlags::TRANSFER_DST,
                gpu_allocator::MemoryLocation::GpuToCpu,
            )?;
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
                .old_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
                .image(af.target_image)
                .subresource_range(subresource)];
            let dep = vk::DependencyInfo::default().image_memory_barriers(&to_src);
            // SAFETY: Valid barrier inside active command buffer.
            unsafe { self.ctx.device.cmd_pipeline_barrier2(cmd, &dep) };

            if let Some(RenderTarget::Headless(t)) = &mut self.target {
                t.color_layout = vk::ImageLayout::TRANSFER_SRC_OPTIMAL;
            }

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
                    width: af.extent.width,
                    height: af.extent.height,
                    depth: 1,
                })];
            // SAFETY: Staging buffer fits `extent.width * extent.height * 4` bytes.
            unsafe {
                self.ctx.device.cmd_copy_image_to_buffer(
                    cmd,
                    af.target_image,
                    vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    staging.buffer,
                    &copy,
                );
            }
            Some((staging, byte_len))
        } else {
            None
        };

        // For windowed targets, transition swapchain image to PRESENT_SRC_KHR.
        if af.swapchain_image_index.is_some() {
            let src_layout = if capture_staging.is_some() {
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL
            } else {
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL
            };
            let src_stage = if capture_staging.is_some() {
                vk::PipelineStageFlags2::TRANSFER
            } else {
                vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT
            };
            let src_access = if capture_staging.is_some() {
                vk::AccessFlags2::TRANSFER_READ
            } else {
                vk::AccessFlags2::COLOR_ATTACHMENT_WRITE
            };
            let to_present = [vk::ImageMemoryBarrier2::default()
                .src_stage_mask(src_stage)
                .src_access_mask(src_access)
                .dst_stage_mask(vk::PipelineStageFlags2::BOTTOM_OF_PIPE)
                .dst_access_mask(vk::AccessFlags2::NONE)
                .old_layout(src_layout)
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
            let signal_sems = [sc.render_finished[img_idx as usize]];
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

        if let (Some(path), Some((staging, byte_len))) = (capture_path, capture_staging) {
            if af.swapchain_image_index.is_some() {
                // SAFETY: Wait for fence before reading back the captured staging buffer.
                unsafe {
                    self.ctx
                        .device
                        .wait_for_fences(&[slot.in_flight_fence], true, u64::MAX)?;
                }
            }
            let mut pixels = staging.mapped_slice()?[..byte_len].to_vec();
            let swap_rb = af.color_format == vk::Format::B8G8R8A8_UNORM
                || af.color_format == vk::Format::B8G8R8A8_SRGB;
            for px in pixels.chunks_exact_mut(4) {
                if swap_rb {
                    px.swap(0, 2);
                }
                px[3] = 255;
            }
            let _ = write_rgba8_png(Path::new(&path), af.extent.width, af.extent.height, &pixels);
            staging.destroy(&mut self.ctx);
            if let Ok(stats_path) = std::env::var("ZHR_VK_STATS_LOG")
                && !stats_path.is_empty()
            {
                let report = format!(
                    "device={}\nresolution={}x{}\nframe={}\ndraw_calls={}\ntriangles={}\npipeline_binds={}\ntexture_binds={}\nlive_textures={}\nremastered_textures={}\nvalidation_active={}\nvalidation_errors={}\n",
                    self.ctx.device_info.name,
                    af.extent.width,
                    af.extent.height,
                    self.frame_number,
                    self.stats.draw_calls,
                    self.stats.triangles,
                    self.stats.pipeline_binds,
                    self.stats.texture_binds,
                    self.textures.len(),
                    self.remastered_textures.len(),
                    self.ctx.validation_active,
                    self.ctx.validation_error_count(),
                );
                let _ = std::fs::write(stats_path, report);
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
        self.remaster_worker = None;
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
        let remastered: Vec<GpuTexture> =
            self.remastered_textures.drain().map(|(_, t)| t).collect();
        for t in remastered {
            t.destroy(&mut self.ctx);
        }
        for frame in self.frames.drain(..) {
            for tex in frame.retired_textures {
                tex.destroy(&mut self.ctx);
            }
            frame.uniform_ring.destroy(&mut self.ctx);
            frame.dynamic_vb.destroy(&mut self.ctx);
            frame.dynamic_ib.destroy(&mut self.ctx);
            unsafe {
                self.ctx.device.destroy_fence(frame.in_flight_fence, None);
                self.ctx
                    .device
                    .destroy_semaphore(frame.image_available, None);
            }
        }
        if let Some(pp) = self.postprocess.take() {
            pp.destroy(&mut self.ctx);
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

/// Returns true when an RGBA8 texture is a pure white alpha mask (e.g. `Render2DSentenceClass`
/// GDI font glyph atlases), which must stay at native pixel-accurate resolution instead of being
/// passed through the 2x edge-directed texture upscaler.
fn is_font_alpha_mask(rgba8: &[u8]) -> bool {
    let mut has_transparent = false;
    let mut has_visible = false;
    for px in rgba8.chunks_exact(4) {
        if px[3] == 0 {
            has_transparent = true;
        } else {
            has_visible = true;
            if px[0] < 248 || px[1] < 248 || px[2] < 248 {
                return false;
            }
        }
    }
    has_transparent && has_visible
}

fn write_sampled_texture_set(
    device: &ash::Device,
    set: vk::DescriptorSet,
    view: vk::ImageView,
    sampler: vk::Sampler,
) {
    let img_info = [vk::DescriptorImageInfo::default()
        .image_view(view)
        .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
    let samp_info = [vk::DescriptorImageInfo::default().sampler(sampler)];
    let writes = [
        vk::WriteDescriptorSet::default()
            .dst_set(set)
            .dst_binding(0)
            .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
            .image_info(&img_info),
        vk::WriteDescriptorSet::default()
            .dst_set(set)
            .dst_binding(1)
            .descriptor_type(vk::DescriptorType::SAMPLER)
            .image_info(&samp_info),
    ];
    // SAFETY: `set`, `view`, and `sampler` belong to `device`.
    unsafe { device.update_descriptor_sets(&writes, &[]) };
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
