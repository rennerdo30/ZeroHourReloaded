use ash::vk;
use bytemuck::{Pod, Zeroable};
use gpu_allocator::MemoryLocation;
use gpu_allocator::vulkan::Allocation;

use crate::buffer::GpuBuffer;
use crate::context::VkContext;
use crate::error::Result;
use crate::offscreen::create_attachment;
use crate::shader::{compile_wgsl_to_spirv, create_shader_module};
use crate::types::{GraphicsMode, GraphicsSettings};

pub const HDR_FORMAT: vk::Format = vk::Format::R16G16B16A16_SFLOAT;

#[repr(C)]
#[derive(Clone, Copy, Debug, Pod, Zeroable)]
struct PostParams {
    /// x = post_fx_enable (0/1), y = split_x_uv (<=0 disabled), z = inv_width, w = inv_height.
    params0: [f32; 4],
    /// x = blur_dx, y = blur_dy, z = bloom_threshold, w = bloom_strength.
    params1: [f32; 4],
}

const POSTPROCESS_WGSL: &str = r#"
struct PostParams {
    params0: vec4<f32>,
    params1: vec4<f32>,
};

@group(0) @binding(0) var t_src0: texture_2d<f32>;
@group(0) @binding(1) var t_src1: texture_2d<f32>;
@group(0) @binding(2) var s_linear: sampler;
@group(0) @binding(3) var<uniform> u: PostParams;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_fullscreen(@builtin(vertex_index) vid: u32) -> VsOut {
    // Fullscreen triangle covering NDC [-1, 1] x [-1, 1] with Vulkan UV (0,0) top-left -> (1,1) bottom-right.
    let x = f32((vid << 1u) & 2u) * 2.0 - 1.0;
    let y = f32(vid & 2u) * 2.0 - 1.0;
    var out: VsOut;
    out.pos = vec4<f32>(x, y, 0.0, 1.0);
    out.uv = vec2<f32>(x * 0.5 + 0.5, y * 0.5 + 0.5);
    return out;
}

@fragment
fn fs_bloom_extract(in: VsOut) -> @location(0) vec4<f32> {
    let texel = vec2<f32>(u.params0.z, u.params0.w);
    // 4-tap box downsample from full-res HDR scene to quarter-res bloom buffer.
    let c0 = textureSample(t_src0, s_linear, in.uv + texel * vec2<f32>(-1.0, -1.0)).rgb;
    let c1 = textureSample(t_src0, s_linear, in.uv + texel * vec2<f32>( 1.0, -1.0)).rgb;
    let c2 = textureSample(t_src0, s_linear, in.uv + texel * vec2<f32>(-1.0,  1.0)).rgb;
    let c3 = textureSample(t_src0, s_linear, in.uv + texel * vec2<f32>( 1.0,  1.0)).rgb;
    let c = (c0 + c1 + c2 + c3) * 0.25;

    let luma = max(c.r, max(c.g, c.b));
    let threshold = u.params1.z;
    let knee = 0.28;
    let soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
    let soft_contrib = (soft * soft) / (4.0 * knee + 0.0001);
    let contrib = max(soft_contrib, luma - threshold) / max(luma, 0.0001);
    let bloom = max(c * contrib, vec3<f32>(0.0));
    return vec4<f32>(min(bloom, vec3<f32>(6.0)), 1.0);
}

@fragment
fn fs_bloom_blur(in: VsOut) -> @location(0) vec4<f32> {
    let step = vec2<f32>(u.params1.x, u.params1.y);
    // 9-tap separable Gaussian kernel.
    var sum = textureSample(t_src0, s_linear, in.uv).rgb * 0.227027;
    sum = sum + textureSample(t_src0, s_linear, in.uv + step * 1.0).rgb * 0.1945946;
    sum = sum + textureSample(t_src0, s_linear, in.uv - step * 1.0).rgb * 0.1945946;
    sum = sum + textureSample(t_src0, s_linear, in.uv + step * 2.0).rgb * 0.1216216;
    sum = sum + textureSample(t_src0, s_linear, in.uv - step * 2.0).rgb * 0.1216216;
    sum = sum + textureSample(t_src0, s_linear, in.uv + step * 3.0).rgb * 0.054054;
    sum = sum + textureSample(t_src0, s_linear, in.uv - step * 3.0).rgb * 0.054054;
    sum = sum + textureSample(t_src0, s_linear, in.uv + step * 4.0).rgb * 0.016216;
    sum = sum + textureSample(t_src0, s_linear, in.uv - step * 4.0).rgb * 0.016216;
    return vec4<f32>(sum, 1.0);
}

fn aces_curve(x: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn filmic_tonemap_hdr(hdr: vec3<f32>) -> vec3<f32> {
    // Convert sRGB-like scene values to linear, apply normalized ACES filmic shoulder for HDR > 1.0,
    // and map back so UI whites (1.0) remain crisp pure white while HDR explosions & sun glints roll off smoothly.
    let lin = pow(max(hdr, vec3<f32>(0.0)), vec3<f32>(2.2));
    let mapped = aces_curve(lin * 1.12);
    let white_scale = vec3<f32>(1.0) / aces_curve(vec3<f32>(1.12));
    let srgb = pow(clamp(mapped * white_scale, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
    // Blend 65% filmic with 35% soft Reinhard to preserve authentic Zero Hour UI & desert midtones.
    let soft = hdr / (vec3<f32>(1.0) + max(hdr - vec3<f32>(0.85), vec3<f32>(0.0)) * 0.55);
    return clamp(mix(soft, srgb, 0.65), vec3<f32>(0.0), vec3<f32>(1.0));
}

@fragment
fn fs_composite(in: VsOut) -> @location(0) vec4<f32> {
    let raw = textureSample(t_src0, s_linear, in.uv);
    let post_enabled = u.params0.x > 0.5;
    let split_x = u.params0.y;
    let texel = vec2<f32>(u.params0.z, u.params0.w);

    // Split-screen A/B divider line (warm tactical amber-gold).
    if (split_x > 0.0 && abs(in.uv.x - split_x) < texel.x * 1.5) {
        return vec4<f32>(1.0, 0.82, 0.28, 1.0);
    }

    let use_remastered = post_enabled && (split_x <= 0.0 || in.uv.x >= split_x);
    if (!use_remastered) {
        return vec4<f32>(clamp(raw.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), raw.a);
    }

    // AMD FidelityFX-style Contrast-Adaptive Sharpening (5-tap cross, local min/max clamped to avoid ringing).
    let n = textureSample(t_src0, s_linear, in.uv + vec2<f32>(0.0, -texel.y)).rgb;
    let s = textureSample(t_src0, s_linear, in.uv + vec2<f32>(0.0,  texel.y)).rgb;
    let e = textureSample(t_src0, s_linear, in.uv + vec2<f32>( texel.x, 0.0)).rgb;
    let w = textureSample(t_src0, s_linear, in.uv + vec2<f32>(-texel.x, 0.0)).rgb;
    let min_rgb = min(raw.rgb, min(min(n, s), min(e, w)));
    let max_rgb = max(raw.rgb, max(max(n, s), max(e, w)));
    let amp = clamp(min(min_rgb, vec3<f32>(2.0) - max_rgb) / max(max_rgb, vec3<f32>(0.001)), vec3<f32>(0.0), vec3<f32>(1.0));
    let weight = sqrt(amp) * (-0.145); // moderate tactical sharpening (no haloing)
    let sharp = (raw.rgb + (n + s + e + w) * weight) / (vec3<f32>(1.0) + 4.0 * weight);
    let cas_rgb = clamp(sharp, min_rgb * 0.92, max_rgb * 1.08);

    // Add quarter-res HDR bloom.
    let bloom = textureSample(t_src1, s_linear, in.uv).rgb;
    let hdr = max(cas_rgb + bloom * u.params1.w, vec3<f32>(0.0));

    // Normalized filmic HDR tonemapping.
    var col = filmic_tonemap_hdr(hdr);

    // Subtle tactical vibrance (+8% on desaturated colors without clipping already-vivid colors).
    let luma = dot(col, vec3<f32>(0.299, 0.587, 0.114));
    let max_c = max(col.r, max(col.g, col.b));
    let min_c = min(col.r, min(col.g, col.b));
    let sat = max_c - min_c;
    let vib = (1.0 - sat) * 0.08;
    col = clamp(mix(vec3<f32>(luma), col, 1.0 + vib), vec3<f32>(0.0), vec3<f32>(1.0));

    // Subtle cinematic edge vignette (keeps center 100% untouched).
    let centered = in.uv * 2.0 - vec2<f32>(1.0);
    let vig = 1.0 - dot(centered, centered) * 0.055;
    col = col * clamp(vig, 0.88, 1.0);

    return vec4<f32>(col, 1.0);
}
"#;

struct ImageTarget {
    image: vk::Image,
    view: vk::ImageView,
    alloc: Option<Allocation>,
    width: u32,
    height: u32,
}

impl ImageTarget {
    fn new(ctx: &mut VkContext, name: &str, width: u32, height: u32) -> Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let (image, view, alloc) = create_attachment(
            ctx,
            name,
            w,
            h,
            HDR_FORMAT,
            vk::ImageUsageFlags::COLOR_ATTACHMENT
                | vk::ImageUsageFlags::SAMPLED
                | vk::ImageUsageFlags::TRANSFER_DST,
            vk::ImageAspectFlags::COLOR,
        )?;
        Ok(Self {
            image,
            view,
            alloc: Some(alloc),
            width: w,
            height: h,
        })
    }

    fn destroy(mut self, ctx: &mut VkContext) {
        // SAFETY: Caller ensures GPU is idle.
        unsafe {
            ctx.device.destroy_image_view(self.view, None);
            ctx.device.destroy_image(self.image, None);
        }
        if let Some(a) = self.alloc.take() {
            let _ = ctx.allocator_mut().free(a);
        }
    }
}

/// Manages the Vulkan 1.3 HDR scene target (`R16G16B16A16_SFLOAT`), quarter-res bloom ping-pong buffers,
/// and fullscreen composite pass (filmic tonemapping + CAS sharpening + A/B split-screen).
pub struct PostProcessPipeline {
    hdr_scene: ImageTarget,
    bloom_a: ImageTarget,
    bloom_b: ImageTarget,
    sampler: vk::Sampler,
    set_layout: vk::DescriptorSetLayout,
    pipeline_layout: vk::PipelineLayout,
    descriptor_pool: vk::DescriptorPool,
    set_extract: vk::DescriptorSet,
    set_blur_h: vk::DescriptorSet,
    set_blur_v: vk::DescriptorSet,
    set_composite: vk::DescriptorSet,
    params_ubo: GpuBuffer,
    slot_stride: usize,
    vs_module: vk::ShaderModule,
    fs_extract_module: vk::ShaderModule,
    fs_blur_module: vk::ShaderModule,
    fs_composite_module: vk::ShaderModule,
    extract_pipeline: vk::Pipeline,
    blur_pipeline: vk::Pipeline,
    composite_pipeline: vk::Pipeline,
    output_format: vk::Format,
}

impl PostProcessPipeline {
    pub fn new(
        ctx: &mut VkContext,
        width: u32,
        height: u32,
        output_format: vk::Format,
        frames_in_flight: usize,
    ) -> Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let bw = (w / 4).max(1);
        let bh = (h / 4).max(1);

        let hdr_scene = ImageTarget::new(ctx, "zhr_hdr_scene", w, h)?;
        let bloom_a = ImageTarget::new(ctx, "zhr_bloom_a", bw, bh)?;
        let bloom_b = ImageTarget::new(ctx, "zhr_bloom_b", bw, bh)?;

        // Transition bloom buffers to SHADER_READ_ONLY_OPTIMAL initially so passthrough mode is always valid.
        transition_images_to_shader_read(ctx, &[bloom_a.image, bloom_b.image])?;

        let sampler_info = vk::SamplerCreateInfo::default()
            .mag_filter(vk::Filter::LINEAR)
            .min_filter(vk::Filter::LINEAR)
            .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
            .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE);
        // SAFETY: `device` is valid.
        let sampler = unsafe { ctx.device.create_sampler(&sampler_info, None)? };

        let bindings = [
            vk::DescriptorSetLayoutBinding::default()
                .binding(0)
                .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            vk::DescriptorSetLayoutBinding::default()
                .binding(1)
                .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            vk::DescriptorSetLayoutBinding::default()
                .binding(2)
                .descriptor_type(vk::DescriptorType::SAMPLER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            vk::DescriptorSetLayoutBinding::default()
                .binding(3)
                .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
        ];
        let set_layout_info = vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings);
        // SAFETY: `device` is valid.
        let set_layout = unsafe {
            ctx.device
                .create_descriptor_set_layout(&set_layout_info, None)?
        };

        let set_layouts = [set_layout];
        let pl_info = vk::PipelineLayoutCreateInfo::default().set_layouts(&set_layouts);
        // SAFETY: `set_layout` belongs to `device`.
        let pipeline_layout = unsafe { ctx.device.create_pipeline_layout(&pl_info, None)? };

        let pool_sizes = [
            vk::DescriptorPoolSize {
                ty: vk::DescriptorType::SAMPLED_IMAGE,
                descriptor_count: 8,
            },
            vk::DescriptorPoolSize {
                ty: vk::DescriptorType::SAMPLER,
                descriptor_count: 4,
            },
            vk::DescriptorPoolSize {
                ty: vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC,
                descriptor_count: 4,
            },
        ];
        let pool_info = vk::DescriptorPoolCreateInfo::default()
            .max_sets(4)
            .pool_sizes(&pool_sizes);
        // SAFETY: `device` is valid.
        let descriptor_pool = unsafe { ctx.device.create_descriptor_pool(&pool_info, None)? };

        let alloc_layouts = [set_layout; 4];
        let alloc_info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(descriptor_pool)
            .set_layouts(&alloc_layouts);
        // SAFETY: Pool has room for 4 sets.
        let sets = unsafe { ctx.device.allocate_descriptor_sets(&alloc_info)? };
        let (set_extract, set_blur_h, set_blur_v, set_composite) =
            (sets[0], sets[1], sets[2], sets[3]);

        // 4 passes per frame * frames_in_flight, each aligned to `min_uniform_offset_alignment`.
        let align = ctx.min_uniform_offset_alignment as usize;
        let param_size = std::mem::size_of::<PostParams>();
        let slot_stride = param_size.div_ceil(align) * align;
        let total_ubo_bytes = (slot_stride * 4 * frames_in_flight.max(1)) as u64;
        let params_ubo = GpuBuffer::new(
            ctx,
            "zhr_postprocess_ubo",
            total_ubo_bytes,
            vk::BufferUsageFlags::UNIFORM_BUFFER,
            MemoryLocation::CpuToGpu,
        )?;

        let vs_spv =
            compile_wgsl_to_spirv(POSTPROCESS_WGSL, naga::ShaderStage::Vertex, "vs_fullscreen")?;
        let fs_extract_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_bloom_extract",
        )?;
        let fs_blur_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_bloom_blur",
        )?;
        let fs_comp_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_composite",
        )?;

        let vs_module = create_shader_module(&ctx.device, &vs_spv)?;
        let fs_extract_module = create_shader_module(&ctx.device, &fs_extract_spv)?;
        let fs_blur_module = create_shader_module(&ctx.device, &fs_blur_spv)?;
        let fs_composite_module = create_shader_module(&ctx.device, &fs_comp_spv)?;

        let extract_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_extract_module,
            c"fs_bloom_extract",
            HDR_FORMAT,
        )?;
        let blur_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_blur_module,
            c"fs_bloom_blur",
            HDR_FORMAT,
        )?;
        let composite_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_composite_module,
            c"fs_composite",
            output_format,
        )?;

        let me = Self {
            hdr_scene,
            bloom_a,
            bloom_b,
            sampler,
            set_layout,
            pipeline_layout,
            descriptor_pool,
            set_extract,
            set_blur_h,
            set_blur_v,
            set_composite,
            params_ubo,
            slot_stride,
            vs_module,
            fs_extract_module,
            fs_blur_module,
            fs_composite_module,
            extract_pipeline,
            blur_pipeline,
            composite_pipeline,
            output_format,
        };
        me.update_descriptors(&ctx.device);
        Ok(me)
    }

    pub fn hdr_image(&self) -> vk::Image {
        self.hdr_scene.image
    }

    pub fn hdr_view(&self) -> vk::ImageView {
        self.hdr_scene.view
    }

    pub fn resize(
        &mut self,
        ctx: &mut VkContext,
        width: u32,
        height: u32,
        output_format: vk::Format,
    ) -> Result<()> {
        let w = width.max(1);
        let h = height.max(1);
        let bw = (w / 4).max(1);
        let bh = (h / 4).max(1);

        let new_hdr = ImageTarget::new(ctx, "zhr_hdr_scene", w, h)?;
        let new_ba = ImageTarget::new(ctx, "zhr_bloom_a", bw, bh)?;
        let new_bb = ImageTarget::new(ctx, "zhr_bloom_b", bw, bh)?;
        transition_images_to_shader_read(ctx, &[new_ba.image, new_bb.image])?;

        let old_hdr = std::mem::replace(&mut self.hdr_scene, new_hdr);
        let old_ba = std::mem::replace(&mut self.bloom_a, new_ba);
        let old_bb = std::mem::replace(&mut self.bloom_b, new_bb);
        old_hdr.destroy(ctx);
        old_ba.destroy(ctx);
        old_bb.destroy(ctx);

        if output_format != self.output_format {
            let new_comp = create_fullscreen_pipeline(
                &ctx.device,
                self.pipeline_layout,
                self.vs_module,
                self.fs_composite_module,
                c"fs_composite",
                output_format,
            )?;
            // SAFETY: Device is idle during resize.
            unsafe { ctx.device.destroy_pipeline(self.composite_pipeline, None) };
            self.composite_pipeline = new_comp;
            self.output_format = output_format;
        }

        self.update_descriptors(&ctx.device);
        Ok(())
    }

    fn update_descriptors(&self, device: &ash::Device) {
        let write_set = |set: vk::DescriptorSet, view0: vk::ImageView, view1: vk::ImageView| {
            let img0 = [vk::DescriptorImageInfo::default()
                .image_view(view0)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
            let img1 = [vk::DescriptorImageInfo::default()
                .image_view(view1)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
            let samp = [vk::DescriptorImageInfo::default().sampler(self.sampler)];
            let ubo = [vk::DescriptorBufferInfo::default()
                .buffer(self.params_ubo.buffer)
                .offset(0)
                .range(std::mem::size_of::<PostParams>() as u64)];
            let writes = [
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(0)
                    .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                    .image_info(&img0),
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(1)
                    .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                    .image_info(&img1),
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(2)
                    .descriptor_type(vk::DescriptorType::SAMPLER)
                    .image_info(&samp),
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(3)
                    .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC)
                    .buffer_info(&ubo),
            ];
            // SAFETY: Descriptor sets, views, sampler, and UBO belong to `device`.
            unsafe { device.update_descriptor_sets(&writes, &[]) };
        };

        write_set(self.set_extract, self.hdr_scene.view, self.hdr_scene.view);
        write_set(self.set_blur_h, self.bloom_a.view, self.bloom_a.view);
        write_set(self.set_blur_v, self.bloom_b.view, self.bloom_b.view);
        write_set(self.set_composite, self.hdr_scene.view, self.bloom_a.view);
    }

    /// Runs HDR bloom (if enabled) and composites the HDR scene into `target_image`/`target_view`.
    /// Leaves `target_image` in `COLOR_ATTACHMENT_OPTIMAL`.
    #[allow(clippy::too_many_arguments)]
    pub fn execute(
        &mut self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        frame_slot: usize,
        target_image: vk::Image,
        target_view: vk::ImageView,
        target_old_layout: vk::ImageLayout,
        target_extent: vk::Extent2D,
        settings: GraphicsSettings,
    ) -> Result<()> {
        let post_active = settings.mode != GraphicsMode::Classic && settings.post_fx;
        let split_x_uv = if settings.mode == GraphicsMode::SplitScreen {
            0.5
        } else {
            -1.0
        };

        let base_slot = frame_slot * 4;
        let off_extract = (base_slot * self.slot_stride) as u32;
        let off_blur_h = ((base_slot + 1) * self.slot_stride) as u32;
        let off_blur_v = ((base_slot + 2) * self.slot_stride) as u32;
        let off_comp = ((base_slot + 3) * self.slot_stride) as u32;

        let inv_w = 1.0 / (self.hdr_scene.width.max(1) as f32);
        let inv_h = 1.0 / (self.hdr_scene.height.max(1) as f32);
        let inv_bw = 1.0 / (self.bloom_a.width.max(1) as f32);
        let inv_bh = 1.0 / (self.bloom_a.height.max(1) as f32);

        let p_extract = PostParams {
            params0: [1.0, split_x_uv, inv_w, inv_h],
            params1: [0.0, 0.0, 1.02, 0.0],
        };
        let p_blur_h = PostParams {
            params0: [1.0, split_x_uv, inv_bw, inv_bh],
            params1: [inv_bw, 0.0, 0.0, 0.0],
        };
        let p_blur_v = PostParams {
            params0: [1.0, split_x_uv, inv_bw, inv_bh],
            params1: [0.0, inv_bh, 0.0, 0.0],
        };
        let p_comp = PostParams {
            params0: [
                if post_active { 1.0 } else { 0.0 },
                split_x_uv,
                inv_w,
                inv_h,
            ],
            params1: [0.0, 0.0, 1.02, 0.34],
        };

        self.params_ubo
            .write_bytes(off_extract as usize, bytemuck::bytes_of(&p_extract))?;
        self.params_ubo
            .write_bytes(off_blur_h as usize, bytemuck::bytes_of(&p_blur_h))?;
        self.params_ubo
            .write_bytes(off_blur_v as usize, bytemuck::bytes_of(&p_blur_v))?;
        self.params_ubo
            .write_bytes(off_comp as usize, bytemuck::bytes_of(&p_comp))?;

        // 1. Transition HDR scene attachment -> SHADER_READ_ONLY_OPTIMAL.
        cmd_barrier(
            device,
            cmd,
            self.hdr_scene.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        // 2. Optional quarter-res bloom extract + separable Gaussian blur (H + V).
        if post_active {
            let bloom_extent = vk::Extent2D {
                width: self.bloom_a.width,
                height: self.bloom_a.height,
            };

            // Extract HDR brights -> bloom_a.
            cmd_barrier(
                device,
                cmd,
                self.bloom_a.image,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
                vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            );
            draw_fullscreen_pass(
                device,
                cmd,
                self.bloom_a.view,
                bloom_extent,
                self.extract_pipeline,
                self.pipeline_layout,
                self.set_extract,
                off_extract,
            );
            cmd_barrier(
                device,
                cmd,
                self.bloom_a.image,
                vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            );

            // Blur horizontal: bloom_a -> bloom_b.
            cmd_barrier(
                device,
                cmd,
                self.bloom_b.image,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
                vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            );
            draw_fullscreen_pass(
                device,
                cmd,
                self.bloom_b.view,
                bloom_extent,
                self.blur_pipeline,
                self.pipeline_layout,
                self.set_blur_h,
                off_blur_h,
            );
            cmd_barrier(
                device,
                cmd,
                self.bloom_b.image,
                vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            );

            // Blur vertical: bloom_b -> bloom_a.
            cmd_barrier(
                device,
                cmd,
                self.bloom_a.image,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
                vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            );
            draw_fullscreen_pass(
                device,
                cmd,
                self.bloom_a.view,
                bloom_extent,
                self.blur_pipeline,
                self.pipeline_layout,
                self.set_blur_v,
                off_blur_v,
            );
            cmd_barrier(
                device,
                cmd,
                self.bloom_a.image,
                vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
                vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            );
        }

        // 3. Transition final output image -> COLOR_ATTACHMENT_OPTIMAL and composite.
        cmd_barrier(
            device,
            cmd,
            target_image,
            vk::PipelineStageFlags2::TOP_OF_PIPE,
            vk::AccessFlags2::NONE,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            target_old_layout,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        );
        draw_fullscreen_pass(
            device,
            cmd,
            target_view,
            target_extent,
            self.composite_pipeline,
            self.pipeline_layout,
            self.set_composite,
            off_comp,
        );

        Ok(())
    }

    pub fn destroy(self, ctx: &mut VkContext) {
        self.hdr_scene.destroy(ctx);
        self.bloom_a.destroy(ctx);
        self.bloom_b.destroy(ctx);
        self.params_ubo.destroy(ctx);
        // SAFETY: Caller ensures GPU is idle.
        unsafe {
            ctx.device.destroy_pipeline(self.extract_pipeline, None);
            ctx.device.destroy_pipeline(self.blur_pipeline, None);
            ctx.device.destroy_pipeline(self.composite_pipeline, None);
            ctx.device.destroy_shader_module(self.vs_module, None);
            ctx.device
                .destroy_shader_module(self.fs_extract_module, None);
            ctx.device.destroy_shader_module(self.fs_blur_module, None);
            ctx.device
                .destroy_shader_module(self.fs_composite_module, None);
            ctx.device
                .destroy_descriptor_pool(self.descriptor_pool, None);
            ctx.device
                .destroy_pipeline_layout(self.pipeline_layout, None);
            ctx.device
                .destroy_descriptor_set_layout(self.set_layout, None);
            ctx.device.destroy_sampler(self.sampler, None);
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn draw_fullscreen_pass(
    device: &ash::Device,
    cmd: vk::CommandBuffer,
    view: vk::ImageView,
    extent: vk::Extent2D,
    pipeline: vk::Pipeline,
    layout: vk::PipelineLayout,
    set: vk::DescriptorSet,
    dyn_offset: u32,
) {
    let color_att = [vk::RenderingAttachmentInfo::default()
        .image_view(view)
        .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
        .load_op(vk::AttachmentLoadOp::DONT_CARE)
        .store_op(vk::AttachmentStoreOp::STORE)];
    let info = vk::RenderingInfo::default()
        .render_area(vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent,
        })
        .layer_count(1)
        .color_attachments(&color_att);

    let vp = [vk::Viewport {
        x: 0.0,
        y: 0.0,
        width: extent.width.max(1) as f32,
        height: extent.height.max(1) as f32,
        min_depth: 0.0,
        max_depth: 1.0,
    }];
    let scissor = [vk::Rect2D {
        offset: vk::Offset2D { x: 0, y: 0 },
        extent,
    }];

    // SAFETY: All Vulkan handles are valid and command buffer is recording outside dynamic rendering.
    unsafe {
        device.cmd_begin_rendering(cmd, &info);
        device.cmd_set_viewport(cmd, 0, &vp);
        device.cmd_set_scissor(cmd, 0, &scissor);
        device.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipeline);
        device.cmd_bind_descriptor_sets(
            cmd,
            vk::PipelineBindPoint::GRAPHICS,
            layout,
            0,
            &[set],
            &[dyn_offset],
        );
        device.cmd_draw(cmd, 3, 1, 0, 0);
        device.cmd_end_rendering(cmd);
    }
}

#[allow(clippy::too_many_arguments)]
fn cmd_barrier(
    device: &ash::Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    src_stage: vk::PipelineStageFlags2,
    src_access: vk::AccessFlags2,
    dst_stage: vk::PipelineStageFlags2,
    dst_access: vk::AccessFlags2,
    old_layout: vk::ImageLayout,
    new_layout: vk::ImageLayout,
) {
    let barrier = [vk::ImageMemoryBarrier2::default()
        .src_stage_mask(src_stage)
        .src_access_mask(src_access)
        .dst_stage_mask(dst_stage)
        .dst_access_mask(dst_access)
        .old_layout(old_layout)
        .new_layout(new_layout)
        .image(image)
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
    unsafe { device.cmd_pipeline_barrier2(cmd, &dep) };
}

fn transition_images_to_shader_read(ctx: &VkContext, images: &[vk::Image]) -> Result<()> {
    ctx.submit_immediate(|device, cmd| {
        for &img in images {
            cmd_barrier(
                device,
                cmd,
                img,
                vk::PipelineStageFlags2::TOP_OF_PIPE,
                vk::AccessFlags2::NONE,
                vk::PipelineStageFlags2::TRANSFER,
                vk::AccessFlags2::TRANSFER_WRITE,
                vk::ImageLayout::UNDEFINED,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            );
            let clear = vk::ClearColorValue {
                float32: [0.0, 0.0, 0.0, 1.0],
            };
            let range = [vk::ImageSubresourceRange::default()
                .aspect_mask(vk::ImageAspectFlags::COLOR)
                .base_mip_level(0)
                .level_count(1)
                .base_array_layer(0)
                .layer_count(1)];
            // SAFETY: Image is in TRANSFER_DST_OPTIMAL.
            unsafe {
                device.cmd_clear_color_image(
                    cmd,
                    img,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    &clear,
                    &range,
                );
            }
            cmd_barrier(
                device,
                cmd,
                img,
                vk::PipelineStageFlags2::TRANSFER,
                vk::AccessFlags2::TRANSFER_WRITE,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            );
        }
    })
}

fn create_fullscreen_pipeline(
    device: &ash::Device,
    layout: vk::PipelineLayout,
    vs: vk::ShaderModule,
    fs: vk::ShaderModule,
    fs_entry: &std::ffi::CStr,
    color_format: vk::Format,
) -> Result<vk::Pipeline> {
    let stages = [
        vk::PipelineShaderStageCreateInfo::default()
            .stage(vk::ShaderStageFlags::VERTEX)
            .module(vs)
            .name(c"vs_fullscreen"),
        vk::PipelineShaderStageCreateInfo::default()
            .stage(vk::ShaderStageFlags::FRAGMENT)
            .module(fs)
            .name(fs_entry),
    ];
    let vertex_input = vk::PipelineVertexInputStateCreateInfo::default();
    let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
        .topology(vk::PrimitiveTopology::TRIANGLE_LIST);
    let viewport_state = vk::PipelineViewportStateCreateInfo::default()
        .viewport_count(1)
        .scissor_count(1);
    let rasterizer = vk::PipelineRasterizationStateCreateInfo::default()
        .polygon_mode(vk::PolygonMode::FILL)
        .cull_mode(vk::CullModeFlags::NONE)
        .front_face(vk::FrontFace::CLOCKWISE)
        .line_width(1.0);
    let multisampling = vk::PipelineMultisampleStateCreateInfo::default()
        .rasterization_samples(vk::SampleCountFlags::TYPE_1);
    let depth_stencil = vk::PipelineDepthStencilStateCreateInfo::default()
        .depth_test_enable(false)
        .depth_write_enable(false);
    let blend_att = [vk::PipelineColorBlendAttachmentState::default()
        .color_write_mask(vk::ColorComponentFlags::RGBA)
        .blend_enable(false)];
    let color_blending = vk::PipelineColorBlendStateCreateInfo::default().attachments(&blend_att);
    let dynamic_states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
    let dynamic_state =
        vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic_states);

    let color_formats = [color_format];
    let mut rendering =
        vk::PipelineRenderingCreateInfo::default().color_attachment_formats(&color_formats);

    let info = [vk::GraphicsPipelineCreateInfo::default()
        .stages(&stages)
        .vertex_input_state(&vertex_input)
        .input_assembly_state(&input_assembly)
        .viewport_state(&viewport_state)
        .rasterization_state(&rasterizer)
        .multisample_state(&multisampling)
        .depth_stencil_state(&depth_stencil)
        .color_blend_state(&color_blending)
        .dynamic_state(&dynamic_state)
        .layout(layout)
        .push_next(&mut rendering)];

    // SAFETY: All shader modules and pipeline layouts belong to `device`.
    let pipeline = unsafe {
        device
            .create_graphics_pipelines(vk::PipelineCache::null(), &info, None)
            .map_err(|(_, e)| e)?[0]
    };
    Ok(pipeline)
}
