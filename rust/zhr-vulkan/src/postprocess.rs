use ash::vk;
use bytemuck::{Pod, Zeroable};
use glam::Mat4;
use gpu_allocator::MemoryLocation;
use gpu_allocator::vulkan::Allocation;

use crate::buffer::GpuBuffer;
use crate::context::VkContext;
use crate::error::Result;
use crate::offscreen::create_attachment_msaa;
use crate::shader::{compile_wgsl_to_spirv, create_shader_module};
use crate::surface::HdrDisplayConfig;
use crate::types::{GraphicsMode, GraphicsSettings};

pub const HDR_FORMAT: vk::Format = vk::Format::R16G16B16A16_SFLOAT;

const SLOTS_PER_FRAME: usize = 16;

#[repr(C)]
#[derive(Clone, Copy, Debug, Pod, Zeroable)]
struct PostParams {
    /// x = enable (0/1), y = split_x_uv (<=0 disabled), z = inv_width, w = inv_height.
    params0: [f32; 4],
    /// x = blur_dx, y = blur_dy, z = threshold / radius, w = strength.
    params1: [f32; 4],
    /// SSAO projection unpack: x = inv_proj_x, y = inv_proj_y, z = proj_m22, w = proj_m32.
    params2: [f32; 4],
}

const POSTPROCESS_WGSL: &str = r#"
struct PostParams {
    params0: vec4<f32>,
    params1: vec4<f32>,
    params2: vec4<f32>,
};

@group(0) @binding(0) var t_src0: texture_2d<f32>;
@group(0) @binding(1) var t_src1: texture_2d<f32>;
@group(0) @binding(2) var s_samp: sampler;
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

// ============================================================================
// Depth-Based Screen-Space Ambient Occlusion (12-tap Golden-Angle + Bilateral)
// ============================================================================

fn depth_to_view_z(d: f32) -> f32 {
    let m22 = u.params2.z;
    let m32 = u.params2.w;
    let denom = d - m22;
    if (abs(denom) < 0.000001) {
        return 10000.0;
    }
    return abs(m32 / denom);
}

fn reconstruct_view_pos(uv: vec2<f32>, z_view: f32) -> vec3<f32> {
    let ndc_x = uv.x * 2.0 - 1.0;
    let ndc_y = 1.0 - uv.y * 2.0;
    return vec3<f32>(
        ndc_x * z_view * u.params2.x,
        ndc_y * z_view * u.params2.y,
        z_view
    );
}

@fragment
fn fs_ssao_raw(in: VsOut) -> @location(0) vec4<f32> {
    let d_center = textureSample(t_src0, s_samp, in.uv).r;
    // Skip skybox/cleared depth or non-perspective projection.
    if (d_center >= 0.9995 || d_center <= 0.00001 || abs(u.params2.w) < 0.00001) {
        return vec4<f32>(1.0, 10000.0, 0.0, 1.0);
    }

    let z_c = depth_to_view_z(d_center);
    let p_c = reconstruct_view_pos(in.uv, z_c);
    let texel = vec2<f32>(u.params0.z, u.params0.w);

    // Edge-aware neighbor depth finite differences for clean view-space normal without silhouette halos.
    let uv_l = in.uv - vec2<f32>(texel.x, 0.0);
    let uv_r = in.uv + vec2<f32>(texel.x, 0.0);
    let uv_u = in.uv - vec2<f32>(0.0, texel.y);
    let uv_d = in.uv + vec2<f32>(0.0, texel.y);

    let z_l = depth_to_view_z(textureSample(t_src0, s_samp, uv_l).r);
    let z_r = depth_to_view_z(textureSample(t_src0, s_samp, uv_r).r);
    let z_u = depth_to_view_z(textureSample(t_src0, s_samp, uv_u).r);
    let z_d = depth_to_view_z(textureSample(t_src0, s_samp, uv_d).r);

    let p_l = reconstruct_view_pos(uv_l, z_l);
    let p_r = reconstruct_view_pos(uv_r, z_r);
    let p_u = reconstruct_view_pos(uv_u, z_u);
    let p_d = reconstruct_view_pos(uv_d, z_d);

    let dx = select(p_c - p_l, p_r - p_c, abs(z_r - z_c) < abs(z_l - z_c));
    let dy = select(p_c - p_d, p_u - p_c, abs(z_u - z_c) < abs(z_d - z_c));
    let n_raw = cross(dx, dy);
    let n_len = length(n_raw);
    if (n_len < 0.000001) {
        return vec4<f32>(1.0, z_c, 0.0, 1.0);
    }
    var n_view = n_raw / n_len;
    if (n_view.z > 0.0) {
        n_view = -n_view;
    }

    // Distance-adaptive world-space radius (crisp contact AO on small workers/infantry AND large buildings,
    // with tighter max screen_radius so close-up 4K zoom stays cache-friendly at high FPS).
    let world_radius = clamp(z_c * 0.018, 0.22, 9.5);
    let screen_radius = clamp(
        vec2<f32>(
            world_radius / max(z_c * u.params2.x * 2.0, 0.001),
            world_radius / max(z_c * u.params2.y * 2.0, 0.001)
        ),
        texel * 2.0,
        vec2<f32>(0.028, 0.028)
    );

    // Interleaved Gradient Noise per-pixel rotation (zero grid banding).
    let ign = fract(52.9829189 * fract(dot(in.pos.xy, vec2<f32>(0.06711056, 0.00583715))));
    let base_angle = ign * 6.2831853;
    let golden_step = 2.3999632;

    var occ = 0.0;
    let samples: u32 = 8u;
    let bias = world_radius * 0.032;
    for (var i: u32 = 0u; i < samples; i = i + 1u) {
        let fi = (f32(i) + 0.5) / f32(samples);
        let r = sqrt(fi);
        let angle = base_angle + f32(i) * golden_step;
        let offset = vec2<f32>(cos(angle), sin(angle)) * r * screen_radius;
        let sample_uv = clamp(in.uv + offset, vec2<f32>(0.001), vec2<f32>(0.999));

        let d_s = textureSample(t_src0, s_samp, sample_uv).r;
        if (d_s < 0.9995) {
            let z_s = depth_to_view_z(d_s);
            let p_s = reconstruct_view_pos(sample_uv, z_s);
            let diff = p_s - p_c;
            let dist = length(diff);
            if (dist > 0.0001 && dist < world_radius * 2.4) {
                let dir = diff / dist;
                let ndotd = max(dot(n_view, dir) - 0.14, 0.0);
                let falloff = clamp(1.0 - dist / (world_radius * 2.2), 0.0, 1.0);
                // Range check prevents foreground objects from haloing distant terrain behind them.
                let dz_ok = smoothstep(world_radius * 2.4, world_radius * 0.4, abs(z_s - z_c));
                let front_ok = select(0.0, 1.0, (z_c - z_s) > bias);
                occ = occ + ndotd * falloff * dz_ok * front_ok;
            }
        }
    }

    let ao = clamp(1.0 - (occ / f32(samples)) * u.params1.w, 0.35, 1.0);
    return vec4<f32>(ao, z_c, 0.0, 1.0);
}

@fragment
fn fs_ssao_blur(in: VsOut) -> @location(0) vec4<f32> {
    let center = textureSample(t_src0, s_samp, in.uv);
    let z_c = center.g;
    if (z_c >= 9000.0) {
        return center;
    }
    let step = vec2<f32>(u.params1.x, u.params1.y);
    let depth_sigma = max(z_c * 0.035, 0.08);

    var ao_sum = center.r * 0.26;
    var w_sum = 0.26;

    for (var k: i32 = -3; k <= 3; k = k + 1) {
        if (k == 0) {
            continue;
        }
        let fk = f32(k);
        let tap = textureSample(t_src0, s_samp, in.uv + step * fk);
        let spatial_w = exp(-(fk * fk) / 6.0);
        let dz = abs(tap.g - z_c) / depth_sigma;
        let range_w = exp(-(dz * dz));
        let w = spatial_w * range_w;
        ao_sum = ao_sum + tap.r * w;
        w_sum = w_sum + w;
    }

    return vec4<f32>(ao_sum / max(w_sum, 0.0001), z_c, 0.0, 1.0);
}

@fragment
fn fs_ssao_apply(in: VsOut) -> @location(0) vec4<f32> {
    let split_x = u.params0.y;
    if (u.params0.x < 0.5 || (split_x > 0.0 && in.uv.x < split_x)) {
        return vec4<f32>(1.0, 1.0, 1.0, 1.0);
    }
    let ao = clamp(textureSample(t_src0, s_samp, in.uv).r, 0.40, 1.0);
    // Subtle warm-neutral crevice tint so contact shadows feel rich, not cold grey.
    let ao_rgb = mix(vec3<f32>(ao * 0.98, ao * 0.99, ao * 1.02), vec3<f32>(1.0), 0.12);
    return vec4<f32>(clamp(ao_rgb, vec3<f32>(0.42), vec3<f32>(1.0)), 1.0);
}

// ============================================================================
// Quarter-Res HDR Bloom + Normalized Filmic Tonemapping + CAS Sharpening
// ============================================================================

@fragment
fn fs_water_depth_copy(in: VsOut) -> @location(0) vec4<f32> {
    let scene_rgb = clamp(textureSample(t_src1, s_samp, in.uv).rgb, vec3<f32>(0.0), vec3<f32>(16.0));
    let d = textureSample(t_src0, s_samp, in.uv).r;
    if (d >= 0.9995 || d <= 0.00001 || abs(u.params2.w) < 0.00001) {
        return vec4<f32>(scene_rgb, 10000.0);
    }
    let lin_z = clamp(depth_to_view_z(d), 0.01, 20000.0);
    return vec4<f32>(scene_rgb, lin_z);
}

@fragment
fn fs_backdrop_copy(in: VsOut) -> @location(0) vec4<f32> {
    let texel = vec2<f32>(u.params0.z, u.params0.w);
    let c0 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>(-1.0, -1.0)).rgb;
    let c1 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>( 1.0, -1.0)).rgb;
    let c2 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>(-1.0,  1.0)).rgb;
    let c3 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>( 1.0,  1.0)).rgb;
    let c = clamp((c0 + c1 + c2 + c3) * 0.25, vec3<f32>(0.0), vec3<f32>(2.5));
    return vec4<f32>(c, 1.0);
}

@fragment
fn fs_bloom_extract(in: VsOut) -> @location(0) vec4<f32> {
    let texel = vec2<f32>(u.params0.z, u.params0.w);
    let c0 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>(-1.0, -1.0)).rgb;
    let c1 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>( 1.0, -1.0)).rgb;
    let c2 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>(-1.0,  1.0)).rgb;
    let c3 = textureSample(t_src0, s_samp, in.uv + texel * vec2<f32>( 1.0,  1.0)).rgb;
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
    var sum = textureSample(t_src0, s_samp, in.uv).rgb * 0.227027;
    sum = sum + textureSample(t_src0, s_samp, in.uv + step * 1.0).rgb * 0.1945946;
    sum = sum + textureSample(t_src0, s_samp, in.uv - step * 1.0).rgb * 0.1945946;
    sum = sum + textureSample(t_src0, s_samp, in.uv + step * 2.0).rgb * 0.1216216;
    sum = sum + textureSample(t_src0, s_samp, in.uv - step * 2.0).rgb * 0.1216216;
    sum = sum + textureSample(t_src0, s_samp, in.uv + step * 3.0).rgb * 0.054054;
    sum = sum + textureSample(t_src0, s_samp, in.uv - step * 3.0).rgb * 0.054054;
    sum = sum + textureSample(t_src0, s_samp, in.uv + step * 4.0).rgb * 0.016216;
    sum = sum + textureSample(t_src0, s_samp, in.uv - step * 4.0).rgb * 0.016216;
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
    let lin = pow(max(hdr, vec3<f32>(0.0)), vec3<f32>(2.2));
    let mapped = aces_curve(lin * 1.12);
    let white_scale = vec3<f32>(1.0) / aces_curve(vec3<f32>(1.12));
    let srgb = pow(clamp(mapped * white_scale, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
    let soft = hdr / (vec3<f32>(1.0) + max(hdr - vec3<f32>(0.85), vec3<f32>(0.0)) * 0.55);
    return clamp(mix(soft, srgb, 0.65), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn rec709_to_rec2020(c: vec3<f32>) -> vec3<f32> {
    let r = 0.6274040 * c.r + 0.3292820 * c.g + 0.0433136 * c.b;
    let g = 0.0690970 * c.r + 0.9195400 * c.g + 0.0113612 * c.b;
    let b = 0.0163916 * c.r + 0.0880132 * c.g + 0.8955950 * c.b;
    return max(vec3<f32>(r, g, b), vec3<f32>(0.0));
}

fn linear_to_st2084_pq(lin_10k: vec3<f32>) -> vec3<f32> {
    // SMPTE ST.2084 (PQ) OETF for normalized luminance in [0..1] where 1.0 = 10,000 nits.
    let y = clamp(lin_10k, vec3<f32>(0.0), vec3<f32>(1.0));
    let m1 = 0.1593017578125;
    let m2 = 78.84375;
    let c1 = 0.8359375;
    let c2 = 18.8515625;
    let c3 = 18.6875;
    let ym1 = pow(y, vec3<f32>(m1));
    return pow((vec3<f32>(c1) + c2 * ym1) / (vec3<f32>(1.0) + c3 * ym1), vec3<f32>(m2));
}

fn encode_display_hdr(lin_rec709: vec3<f32>, hdr_mode: f32, paper_white_nits: f32, max_nits: f32) -> vec3<f32> {
    let peak_ratio = max(max_nits / max(paper_white_nits, 80.0), 1.5);
    // Smooth luminance-preserving HDR highlight shoulder: diffuse [0..1] stays faithful at paper white,
    // while HDR explosions, fireballs, sun glints, and lasers roll smoothly up to display peak nits.
    let luma = max(dot(lin_rec709, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0001);
    var mapped = lin_rec709;
    if (luma > 0.85) {
        let excess = luma - 0.85;
        let headroom = max(peak_ratio - 0.85, 0.5);
        let compressed = 0.85 + headroom * (excess / (excess + headroom));
        mapped = lin_rec709 * (compressed / luma);
    }
    if (hdr_mode < 1.5) {
        // Native HDR10 PQ (`A2B10G10R10_UNORM_PACK32` + `HDR10_ST2084_EXT`)
        let rec2020 = rec709_to_rec2020(mapped);
        let nits_norm = rec2020 * (paper_white_nits / 10000.0);
        return linear_to_st2084_pq(nits_norm);
    }
    // Windows scRGB linear (`R16G16B16A16_SFLOAT` + `EXTENDED_SRGB_LINEAR_EXT`, 1.0 = 80 nits)
    return mapped * (paper_white_nits / 80.0);
}

@fragment
fn fs_composite(in: VsOut) -> @location(0) vec4<f32> {
    let raw = textureSample(t_src0, s_samp, in.uv);
    let post_enabled = u.params0.x > 0.5;
    let split_x = u.params0.y;
    let texel = vec2<f32>(u.params0.z, u.params0.w);
    let hdr_mode = u.params2.x;
    let paper_white_nits = max(u.params2.y, 100.0);
    let max_nits = max(u.params2.z, 250.0);

    if (split_x > 0.0 && abs(in.uv.x - split_x) < texel.x * 1.5) {
        let divider = vec3<f32>(1.0, 0.82, 0.28);
        if (hdr_mode > 0.5) {
            return vec4<f32>(encode_display_hdr(pow(divider, vec3<f32>(2.2)), hdr_mode, paper_white_nits, max_nits), 1.0);
        }
        return vec4<f32>(divider, 1.0);
    }

    let use_remastered = post_enabled && (split_x <= 0.0 || in.uv.x >= split_x);
    if (!use_remastered) {
        let sdr = clamp(raw.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
        if (hdr_mode > 0.5) {
            return vec4<f32>(encode_display_hdr(pow(sdr, vec3<f32>(2.2)), hdr_mode, paper_white_nits, max_nits), raw.a);
        }
        return vec4<f32>(sdr, raw.a);
    }

    // AMD FidelityFX-style Contrast-Adaptive Sharpening (5-tap cross, clamped to avoid halos).
    let n = textureSample(t_src0, s_samp, in.uv + vec2<f32>(0.0, -texel.y)).rgb;
    let s = textureSample(t_src0, s_samp, in.uv + vec2<f32>(0.0,  texel.y)).rgb;
    let e = textureSample(t_src0, s_samp, in.uv + vec2<f32>( texel.x, 0.0)).rgb;
    let w = textureSample(t_src0, s_samp, in.uv + vec2<f32>(-texel.x, 0.0)).rgb;
    let min_rgb = min(raw.rgb, min(min(n, s), min(e, w)));
    let max_rgb = max(raw.rgb, max(max(n, s), max(e, w)));
    let amp = clamp(min(min_rgb, vec3<f32>(2.0) - max_rgb) / max(max_rgb, vec3<f32>(0.001)), vec3<f32>(0.0), vec3<f32>(1.0));
    let weight = sqrt(amp) * (-0.145);
    let sharp = (raw.rgb + (n + s + e + w) * weight) / (vec3<f32>(1.0) + 4.0 * weight);
    let cas_rgb = clamp(sharp, min_rgb * 0.92, max_rgb * 1.08);

    let bloom = textureSample(t_src1, s_samp, in.uv).rgb;
    let hdr = max(cas_rgb + bloom * u.params1.w, vec3<f32>(0.0));

    let centered = in.uv * 2.0 - vec2<f32>(1.0);
    let vig = clamp(1.0 - dot(centered, centered) * 0.055, 0.88, 1.0);

    if (hdr_mode > 0.5) {
        // Real Hardware HDR Display Output (HDR10 ST.2084 PQ or scRGB linear):
        // Preserve full linear HDR headroom for explosions, fireballs, lasers, and sunlit water glints!
        let lin_base = pow(max(hdr, vec3<f32>(0.0)), vec3<f32>(2.2));
        let luma = dot(lin_base, vec3<f32>(0.2126, 0.7152, 0.0722));
        let vib_lin = max(mix(vec3<f32>(luma), lin_base, 1.07), vec3<f32>(0.0)) * vig;
        let out_hdr = encode_display_hdr(vib_lin, hdr_mode, paper_white_nits, max_nits);
        return vec4<f32>(out_hdr, 1.0);
    }

    var col = filmic_tonemap_hdr(hdr);

    let luma = dot(col, vec3<f32>(0.299, 0.587, 0.114));
    let max_c = max(col.r, max(col.g, col.b));
    let min_c = min(col.r, min(col.g, col.b));
    let sat = max_c - min_c;
    let vib = (1.0 - sat) * 0.08;
    col = clamp(mix(vec3<f32>(luma), col, 1.0 + vib), vec3<f32>(0.0), vec3<f32>(1.0));
    col = col * vig;

    return vec4<f32>(col, 1.0);
}

// ============================================================================
// 3-Pass SMAA 1x (Subpixel Morphological Anti-Aliasing) on Resolved 3D Scene
// Cleans alpha-tested barbed wire, fences, palm fronds, shadows & specular edges
// with zero desert-sand texture blur and zero softening of 2D HUD/UI.
// ============================================================================

fn smaa_perceptual(c: vec3<f32>) -> vec3<f32> {
    // Reinhard-like perceptual compression so HDR highlights never cause false super-edges.
    let clamped = max(c, vec3<f32>(0.0));
    return clamped / (vec3<f32>(1.0) + clamped);
}

fn smaa_color_delta(a: vec3<f32>, b: vec3<f32>) -> f32 {
    let d = abs(a - b);
    return max(max(d.r, d.g), d.b);
}

@fragment
fn fs_smaa_edges(in: VsOut) -> @location(0) vec4<f32> {
    let split_x = u.params0.y;
    if (u.params0.x < 0.5 || (split_x > 0.0 && in.uv.x < split_x)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let texel = vec2<f32>(u.params0.z, u.params0.w);
    let threshold = u.params1.z; // ~0.075 perceptual delta

    let c   = smaa_perceptual(textureSampleLevel(t_src0, s_samp, in.uv, 0.0).rgb);
    let c_l = smaa_perceptual(textureSampleLevel(t_src0, s_samp, in.uv + vec2<f32>(-texel.x, 0.0), 0.0).rgb);
    let c_t = smaa_perceptual(textureSampleLevel(t_src0, s_samp, in.uv + vec2<f32>(0.0, -texel.y), 0.0).rgb);

    let d_left = smaa_color_delta(c, c_l);
    let d_top  = smaa_color_delta(c, c_t);

    var edge_l = select(0.0, 1.0, d_left >= threshold);
    var edge_t = select(0.0, 1.0, d_top  >= threshold);

    if (edge_l + edge_t < 0.5) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    // Local Contrast Adaptation (Jimenez SMAA): suppress weak noisy edges next to strong edges
    // so fine sand grain and tank rivets stay crisp while true silhouettes/wires get smoothed.
    let c_r  = smaa_perceptual(textureSampleLevel(t_src0, s_samp, in.uv + vec2<f32>( texel.x, 0.0), 0.0).rgb);
    let c_b  = smaa_perceptual(textureSampleLevel(t_src0, s_samp, in.uv + vec2<f32>(0.0,  texel.y), 0.0).rgb);
    let c_ll = smaa_perceptual(textureSampleLevel(t_src0, s_samp, in.uv + vec2<f32>(-2.0 * texel.x, 0.0), 0.0).rgb);
    let c_tt = smaa_perceptual(textureSampleLevel(t_src0, s_samp, in.uv + vec2<f32>(0.0, -2.0 * texel.y), 0.0).rgb);

    let max_d = max(
        max(max(d_left, d_top), max(smaa_color_delta(c, c_r), smaa_color_delta(c, c_b))),
        max(smaa_color_delta(c_l, c_ll), smaa_color_delta(c_t, c_tt))
    );
    let adapt = max_d * 0.5;
    edge_l = select(0.0, edge_l, d_left >= adapt);
    edge_t = select(0.0, edge_t, d_top  >= adapt);
    return vec4<f32>(edge_l, edge_t, 0.0, 1.0);
}

fn smaa_search_x_left(uv: vec2<f32>, texel: vec2<f32>) -> f32 {
    var d = 0.0;
    for (var i: i32 = 1; i <= 14; i = i + 1) {
        let e = textureSampleLevel(t_src0, s_samp, uv + vec2<f32>(-f32(i) * texel.x, 0.0), 0.0);
        if (e.g < 0.5) {
            break;
        }
        d = f32(i);
        if (e.r > 0.5) {
            break;
        }
    }
    return d;
}

fn smaa_search_x_right(uv: vec2<f32>, texel: vec2<f32>) -> f32 {
    var d = 0.0;
    for (var i: i32 = 1; i <= 14; i = i + 1) {
        let e = textureSampleLevel(t_src0, s_samp, uv + vec2<f32>(f32(i) * texel.x, 0.0), 0.0);
        if (e.g < 0.5) {
            break;
        }
        d = f32(i);
        if (e.r > 0.5) {
            break;
        }
    }
    return d;
}

fn smaa_search_y_up(uv: vec2<f32>, texel: vec2<f32>) -> f32 {
    var d = 0.0;
    for (var i: i32 = 1; i <= 14; i = i + 1) {
        let e = textureSampleLevel(t_src0, s_samp, uv + vec2<f32>(0.0, -f32(i) * texel.y), 0.0);
        if (e.r < 0.5) {
            break;
        }
        d = f32(i);
        if (e.g > 0.5) {
            break;
        }
    }
    return d;
}

fn smaa_search_y_down(uv: vec2<f32>, texel: vec2<f32>) -> f32 {
    var d = 0.0;
    for (var i: i32 = 1; i <= 14; i = i + 1) {
        let e = textureSampleLevel(t_src0, s_samp, uv + vec2<f32>(0.0, f32(i) * texel.y), 0.0);
        if (e.r < 0.5) {
            break;
        }
        d = f32(i);
        if (e.g > 0.5) {
            break;
        }
    }
    return d;
}

/// Analytical trapezoid coverage for SMAA 1x (exact equivalent of AreaTex for L/Z/U crossing shapes).
/// `c1_pos`/`c1_neg` indicate crossing edges at left/top end (above/below), `c2_pos`/`c2_neg` at right/bottom end.
fn smaa_area_analytic(d1: f32, d2: f32, c1_pos: f32, c1_neg: f32, c2_pos: f32, c2_neg: f32) -> vec2<f32> {
    let total = d1 + d2 + 1.0;
    let t = (d1 + 0.5) / total;
    // Height on the positive side (+0.5 at crossing, 0 at non-crossing) vs negative side (-0.5 at crossing).
    let y1 = (c1_pos - c1_neg) * 0.5;
    let y2 = (c2_pos - c2_neg) * 0.5;
    let h = mix(y1, y2, t);
    // Clamp coverage and dampen isolated 1px crossings so fine 1px details never vanish.
    let w_pos = clamp(h, 0.0, 0.5);
    let w_neg = clamp(-h, 0.0, 0.5);
    return vec2<f32>(w_pos, w_neg);
}

@fragment
fn fs_smaa_weights(in: VsOut) -> @location(0) vec4<f32> {
    let e = textureSampleLevel(t_src0, s_samp, in.uv, 0.0).rg;
    if (e.r + e.g < 0.5) {
        return vec4<f32>(0.0);
    }
    let texel = vec2<f32>(u.params0.z, u.params0.w);
    var weights = vec4<f32>(0.0); // (top, bottom, left, right)

    // Horizontal edge (top border of current pixel): search left & right along X.
    if (e.g > 0.5) {
        let dl = smaa_search_x_left(in.uv, texel);
        let dr = smaa_search_x_right(in.uv, texel);
        let uv_l = in.uv + vec2<f32>(-dl * texel.x, 0.0);
        let uv_r = in.uv + vec2<f32>((dr + 1.0) * texel.x, 0.0);
        // Crossing vertical edges above (-Y) and below (+0) each endpoint.
        let c1_up   = textureSampleLevel(t_src0, s_samp, uv_l + vec2<f32>(0.0, -texel.y), 0.0).r;
        let c1_down = textureSampleLevel(t_src0, s_samp, uv_l, 0.0).r;
        let c2_up   = textureSampleLevel(t_src0, s_samp, uv_r + vec2<f32>(0.0, -texel.y), 0.0).r;
        let c2_down = textureSampleLevel(t_src0, s_samp, uv_r, 0.0).r;
        let area = smaa_area_analytic(dl, dr, c1_up, c1_down, c2_up, c2_down);
        weights.x = area.x;
        weights.y = area.y;
    }

    // Vertical edge (left border of current pixel): search up & down along Y.
    if (e.r > 0.5) {
        let du = smaa_search_y_up(in.uv, texel);
        let dd = smaa_search_y_down(in.uv, texel);
        let uv_u = in.uv + vec2<f32>(0.0, -du * texel.y);
        let uv_d = in.uv + vec2<f32>(0.0, (dd + 1.0) * texel.y);
        // Crossing horizontal edges left (-X) and right (+0) of each endpoint.
        let c1_left  = textureSampleLevel(t_src0, s_samp, uv_u + vec2<f32>(-texel.x, 0.0), 0.0).g;
        let c1_right = textureSampleLevel(t_src0, s_samp, uv_u, 0.0).g;
        let c2_left  = textureSampleLevel(t_src0, s_samp, uv_d + vec2<f32>(-texel.x, 0.0), 0.0).g;
        let c2_right = textureSampleLevel(t_src0, s_samp, uv_d, 0.0).g;
        let area = smaa_area_analytic(du, dd, c1_left, c1_right, c2_left, c2_right);
        weights.z = area.x;
        weights.w = area.y;
    }

    return weights;
}

@fragment
fn fs_smaa_blend(in: VsOut) -> @location(0) vec4<f32> {
    let texel = vec2<f32>(u.params0.z, u.params0.w);
    let w_cur   = textureSampleLevel(t_src1, s_samp, in.uv, 0.0);
    let w_right = textureSampleLevel(t_src1, s_samp, in.uv + vec2<f32>(texel.x, 0.0), 0.0).w;
    let w_down  = textureSampleLevel(t_src1, s_samp, in.uv + vec2<f32>(0.0, texel.y), 0.0).y;

    // Neighborhood weights: a.x = right, a.y = bottom, a.z = left, a.w = top.
    let a = vec4<f32>(w_right, w_down, w_cur.z, w_cur.x);
    if (dot(a, vec4<f32>(1.0)) < 0.0001) {
        return textureSampleLevel(t_src0, s_samp, in.uv, 0.0);
    }

    let horiz = max(a.x, a.z) > max(a.y, a.w);
    var blend_uv0 = in.uv;
    var blend_uv1 = in.uv;
    var blend_w = vec2<f32>(0.0);

    if (horiz) {
        blend_uv0 = in.uv + vec2<f32>(a.x * texel.x, 0.0);
        blend_uv1 = in.uv - vec2<f32>(a.z * texel.x, 0.0);
        blend_w = vec2<f32>(a.x, a.z);
    } else {
        blend_uv0 = in.uv + vec2<f32>(0.0, a.y * texel.y);
        blend_uv1 = in.uv - vec2<f32>(0.0, a.w * texel.y);
        blend_w = vec2<f32>(a.y, a.w);
    }

    let w_sum = max(blend_w.x + blend_w.y, 0.0001);
    blend_w = blend_w / w_sum;

    let c0 = textureSampleLevel(t_src0, s_samp, blend_uv0, 0.0);
    let c1 = textureSampleLevel(t_src0, s_samp, blend_uv1, 0.0);
    return c0 * blend_w.x + c1 * blend_w.y;
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
        Self::new_with_format(ctx, name, width, height, HDR_FORMAT)
    }

    fn new_with_format(
        ctx: &mut VkContext,
        name: &str,
        width: u32,
        height: u32,
        format: vk::Format,
    ) -> Result<Self> {
        Self::new_with_usage(
            ctx,
            name,
            width,
            height,
            format,
            vk::ImageUsageFlags::COLOR_ATTACHMENT
                | vk::ImageUsageFlags::SAMPLED
                | vk::ImageUsageFlags::TRANSFER_DST,
            vk::ImageAspectFlags::COLOR,
            vk::SampleCountFlags::TYPE_1,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn new_with_usage(
        ctx: &mut VkContext,
        name: &str,
        width: u32,
        height: u32,
        format: vk::Format,
        usage: vk::ImageUsageFlags,
        aspect: vk::ImageAspectFlags,
        samples: vk::SampleCountFlags,
    ) -> Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let (image, view, alloc) =
            create_attachment_msaa(ctx, name, w, h, format, usage, aspect, samples)?;
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

/// Manages the Vulkan 1.3 HDR scene target (`R16G16B16A16_SFLOAT`), optional 4x MSAA scene+depth targets,
/// half-res bilateral SSAO buffers, 3-pass SMAA 1x buffers, quarter-res bloom ping-pong buffers, and composite pass.
pub struct PostProcessPipeline {
    hdr_scene: ImageTarget,
    hdr_msaa: Option<ImageTarget>,
    depth_msaa: Option<ImageTarget>,
    msaa_samples: vk::SampleCountFlags,
    bloom_a: ImageTarget,
    bloom_b: ImageTarget,
    ui_backdrop: ImageTarget,
    water_depth: ImageTarget,
    ssao_a: ImageTarget,
    ssao_b: ImageTarget,
    smaa_edges: ImageTarget,
    smaa_weights: ImageTarget,
    smaa_temp: ImageTarget,
    depth_sample_view: vk::ImageView,
    linear_sampler: vk::Sampler,
    nearest_sampler: vk::Sampler,
    set_layout: vk::DescriptorSetLayout,
    pipeline_layout: vk::PipelineLayout,
    descriptor_pool: vk::DescriptorPool,
    set_extract: vk::DescriptorSet,
    set_blur_h: vk::DescriptorSet,
    set_blur_v: vk::DescriptorSet,
    set_composite: vk::DescriptorSet,
    set_ssao_raw: vk::DescriptorSet,
    set_ssao_blur_h: vk::DescriptorSet,
    set_ssao_blur_v: vk::DescriptorSet,
    set_ssao_apply: vk::DescriptorSet,
    set_water_snapshot: vk::DescriptorSet,
    set_smaa_edges: vk::DescriptorSet,
    set_smaa_weights: vk::DescriptorSet,
    set_smaa_blend: vk::DescriptorSet,
    params_ubo: GpuBuffer,
    slot_stride: usize,
    vs_module: vk::ShaderModule,
    fs_water_depth_module: vk::ShaderModule,
    fs_backdrop_copy_module: vk::ShaderModule,
    fs_extract_module: vk::ShaderModule,
    fs_blur_module: vk::ShaderModule,
    fs_composite_module: vk::ShaderModule,
    fs_ssao_raw_module: vk::ShaderModule,
    fs_ssao_blur_module: vk::ShaderModule,
    fs_ssao_apply_module: vk::ShaderModule,
    fs_smaa_edges_module: vk::ShaderModule,
    fs_smaa_weights_module: vk::ShaderModule,
    fs_smaa_blend_module: vk::ShaderModule,
    water_depth_pipeline: vk::Pipeline,
    backdrop_copy_pipeline: vk::Pipeline,
    extract_pipeline: vk::Pipeline,
    blur_pipeline: vk::Pipeline,
    composite_pipeline: vk::Pipeline,
    ssao_raw_pipeline: vk::Pipeline,
    ssao_blur_pipeline: vk::Pipeline,
    ssao_apply_pipeline: vk::Pipeline,
    smaa_edges_pipeline: vk::Pipeline,
    smaa_weights_pipeline: vk::Pipeline,
    smaa_blend_pipeline: vk::Pipeline,
    output_format: vk::Format,
    water_depth_format: vk::Format,
}

impl PostProcessPipeline {
    pub fn new(
        ctx: &mut VkContext,
        width: u32,
        height: u32,
        output_format: vk::Format,
        depth_sample_view: vk::ImageView,
        frames_in_flight: usize,
    ) -> Result<Self> {
        let w = width.max(1);
        let h = height.max(1);
        let bw = (w / 4).max(1);
        let bh = (h / 4).max(1);
        let sw = (w / 2).max(1);
        let sh = (h / 2).max(1);

        // Prefer full 32-bit float RGBA for `water_depth` so linearized seabed view-Z (~500..2000)
        // never quantizes into f16 (~0.5-unit) horizontal beach stair-steps.
        let water_depth_format = {
            let candidate = vk::Format::R32G32B32A32_SFLOAT;
            // SAFETY: Valid physical device handle on `ctx.instance`.
            let props = unsafe {
                ctx.instance
                    .get_physical_device_format_properties(ctx.physical_device, candidate)
            };
            let needed = vk::FormatFeatureFlags::COLOR_ATTACHMENT
                | vk::FormatFeatureFlags::SAMPLED_IMAGE
                | vk::FormatFeatureFlags::SAMPLED_IMAGE_FILTER_LINEAR;
            if props.optimal_tiling_features.contains(needed) {
                candidate
            } else {
                HDR_FORMAT
            }
        };

        let msaa_samples = ctx.msaa_samples;
        let hdr_scene = ImageTarget::new(ctx, "zhr_hdr_scene", w, h)?;
        let (hdr_msaa, depth_msaa) = if msaa_samples != vk::SampleCountFlags::TYPE_1 {
            let hc = ImageTarget::new_with_usage(
                ctx,
                "zhr_hdr_msaa",
                w,
                h,
                HDR_FORMAT,
                vk::ImageUsageFlags::COLOR_ATTACHMENT,
                vk::ImageAspectFlags::COLOR,
                msaa_samples,
            )?;
            let dm = ImageTarget::new_with_usage(
                ctx,
                "zhr_depth_msaa",
                w,
                h,
                ctx.depth_format,
                vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT,
                vk::ImageAspectFlags::DEPTH,
                msaa_samples,
            )?;
            (Some(hc), Some(dm))
        } else {
            (None, None)
        };
        let bloom_a = ImageTarget::new(ctx, "zhr_bloom_a", bw, bh)?;
        let bloom_b = ImageTarget::new(ctx, "zhr_bloom_b", bw, bh)?;
        let ui_backdrop = ImageTarget::new(ctx, "zhr_ui_backdrop", bw, bh)?;
        let water_depth =
            ImageTarget::new_with_format(ctx, "zhr_water_snapshot", w, h, water_depth_format)?;
        let ssao_a = ImageTarget::new(ctx, "zhr_ssao_a", sw, sh)?;
        let ssao_b = ImageTarget::new(ctx, "zhr_ssao_b", sw, sh)?;
        let smaa_edges =
            ImageTarget::new_with_format(ctx, "zhr_smaa_edges", w, h, vk::Format::R8G8_UNORM)?;
        let smaa_weights = ImageTarget::new_with_format(
            ctx,
            "zhr_smaa_weights",
            w,
            h,
            vk::Format::R8G8B8A8_UNORM,
        )?;
        let smaa_temp = ImageTarget::new_with_usage(
            ctx,
            "zhr_smaa_temp",
            w,
            h,
            HDR_FORMAT,
            vk::ImageUsageFlags::COLOR_ATTACHMENT
                | vk::ImageUsageFlags::SAMPLED
                | vk::ImageUsageFlags::TRANSFER_SRC
                | vk::ImageUsageFlags::TRANSFER_DST,
            vk::ImageAspectFlags::COLOR,
            vk::SampleCountFlags::TYPE_1,
        )?;

        transition_images_to_shader_read(
            ctx,
            &[
                bloom_a.image,
                bloom_b.image,
                ui_backdrop.image,
                water_depth.image,
                ssao_a.image,
                ssao_b.image,
                smaa_edges.image,
                smaa_weights.image,
                smaa_temp.image,
            ],
        )?;

        let linear_info = vk::SamplerCreateInfo::default()
            .mag_filter(vk::Filter::LINEAR)
            .min_filter(vk::Filter::LINEAR)
            .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
            .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE);
        let nearest_info = vk::SamplerCreateInfo::default()
            .mag_filter(vk::Filter::NEAREST)
            .min_filter(vk::Filter::NEAREST)
            .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
            .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE);
        // SAFETY: `device` is valid.
        let linear_sampler = unsafe { ctx.device.create_sampler(&linear_info, None)? };
        let nearest_sampler = unsafe { ctx.device.create_sampler(&nearest_info, None)? };

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
                descriptor_count: 24,
            },
            vk::DescriptorPoolSize {
                ty: vk::DescriptorType::SAMPLER,
                descriptor_count: 12,
            },
            vk::DescriptorPoolSize {
                ty: vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC,
                descriptor_count: 12,
            },
        ];
        let pool_info = vk::DescriptorPoolCreateInfo::default()
            .max_sets(12)
            .pool_sizes(&pool_sizes);
        // SAFETY: `device` is valid.
        let descriptor_pool = unsafe { ctx.device.create_descriptor_pool(&pool_info, None)? };

        let alloc_layouts = [set_layout; 12];
        let alloc_info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(descriptor_pool)
            .set_layouts(&alloc_layouts);
        // SAFETY: Pool has room for 12 sets.
        let sets = unsafe { ctx.device.allocate_descriptor_sets(&alloc_info)? };

        let align = ctx.min_uniform_offset_alignment as usize;
        let param_size = std::mem::size_of::<PostParams>();
        let slot_stride = param_size.div_ceil(align) * align;
        let total_ubo_bytes = (slot_stride * SLOTS_PER_FRAME * frames_in_flight.max(1)) as u64;
        let params_ubo = GpuBuffer::new(
            ctx,
            "zhr_postprocess_ubo",
            total_ubo_bytes,
            vk::BufferUsageFlags::UNIFORM_BUFFER,
            MemoryLocation::CpuToGpu,
        )?;

        let vs_spv =
            compile_wgsl_to_spirv(POSTPROCESS_WGSL, naga::ShaderStage::Vertex, "vs_fullscreen")?;
        let fs_water_depth_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_water_depth_copy",
        )?;
        let fs_backdrop_copy_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_backdrop_copy",
        )?;
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
        let fs_ssao_raw_spv =
            compile_wgsl_to_spirv(POSTPROCESS_WGSL, naga::ShaderStage::Fragment, "fs_ssao_raw")?;
        let fs_ssao_blur_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_ssao_blur",
        )?;
        let fs_ssao_apply_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_ssao_apply",
        )?;
        let fs_smaa_edges_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_smaa_edges",
        )?;
        let fs_smaa_weights_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_smaa_weights",
        )?;
        let fs_smaa_blend_spv = compile_wgsl_to_spirv(
            POSTPROCESS_WGSL,
            naga::ShaderStage::Fragment,
            "fs_smaa_blend",
        )?;

        let vs_module = create_shader_module(&ctx.device, &vs_spv)?;
        let fs_water_depth_module = create_shader_module(&ctx.device, &fs_water_depth_spv)?;
        let fs_backdrop_copy_module = create_shader_module(&ctx.device, &fs_backdrop_copy_spv)?;
        let fs_extract_module = create_shader_module(&ctx.device, &fs_extract_spv)?;
        let fs_blur_module = create_shader_module(&ctx.device, &fs_blur_spv)?;
        let fs_composite_module = create_shader_module(&ctx.device, &fs_comp_spv)?;
        let fs_ssao_raw_module = create_shader_module(&ctx.device, &fs_ssao_raw_spv)?;
        let fs_ssao_blur_module = create_shader_module(&ctx.device, &fs_ssao_blur_spv)?;
        let fs_ssao_apply_module = create_shader_module(&ctx.device, &fs_ssao_apply_spv)?;
        let fs_smaa_edges_module = create_shader_module(&ctx.device, &fs_smaa_edges_spv)?;
        let fs_smaa_weights_module = create_shader_module(&ctx.device, &fs_smaa_weights_spv)?;
        let fs_smaa_blend_module = create_shader_module(&ctx.device, &fs_smaa_blend_spv)?;

        let water_depth_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_water_depth_module,
            c"fs_water_depth_copy",
            water_depth_format,
            false,
        )?;
        let backdrop_copy_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_backdrop_copy_module,
            c"fs_backdrop_copy",
            HDR_FORMAT,
            false,
        )?;
        let extract_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_extract_module,
            c"fs_bloom_extract",
            HDR_FORMAT,
            false,
        )?;
        let blur_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_blur_module,
            c"fs_bloom_blur",
            HDR_FORMAT,
            false,
        )?;
        let composite_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_composite_module,
            c"fs_composite",
            output_format,
            false,
        )?;
        let ssao_raw_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_ssao_raw_module,
            c"fs_ssao_raw",
            HDR_FORMAT,
            false,
        )?;
        let ssao_blur_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_ssao_blur_module,
            c"fs_ssao_blur",
            HDR_FORMAT,
            false,
        )?;
        let ssao_apply_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_ssao_apply_module,
            c"fs_ssao_apply",
            HDR_FORMAT,
            true,
        )?;
        let smaa_edges_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_smaa_edges_module,
            c"fs_smaa_edges",
            vk::Format::R8G8_UNORM,
            false,
        )?;
        let smaa_weights_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_smaa_weights_module,
            c"fs_smaa_weights",
            vk::Format::R8G8B8A8_UNORM,
            false,
        )?;
        let smaa_blend_pipeline = create_fullscreen_pipeline(
            &ctx.device,
            pipeline_layout,
            vs_module,
            fs_smaa_blend_module,
            c"fs_smaa_blend",
            HDR_FORMAT,
            false,
        )?;

        let me = Self {
            hdr_scene,
            hdr_msaa,
            depth_msaa,
            msaa_samples,
            bloom_a,
            bloom_b,
            ui_backdrop,
            water_depth,
            ssao_a,
            ssao_b,
            smaa_edges,
            smaa_weights,
            smaa_temp,
            depth_sample_view,
            linear_sampler,
            nearest_sampler,
            set_layout,
            pipeline_layout,
            descriptor_pool,
            set_extract: sets[0],
            set_blur_h: sets[1],
            set_blur_v: sets[2],
            set_composite: sets[3],
            set_ssao_raw: sets[4],
            set_ssao_blur_h: sets[5],
            set_ssao_blur_v: sets[6],
            set_ssao_apply: sets[7],
            set_water_snapshot: sets[8],
            set_smaa_edges: sets[9],
            set_smaa_weights: sets[10],
            set_smaa_blend: sets[11],
            params_ubo,
            slot_stride,
            vs_module,
            fs_water_depth_module,
            fs_backdrop_copy_module,
            fs_extract_module,
            fs_blur_module,
            fs_composite_module,
            fs_ssao_raw_module,
            fs_ssao_blur_module,
            fs_ssao_apply_module,
            fs_smaa_edges_module,
            fs_smaa_weights_module,
            fs_smaa_blend_module,
            water_depth_pipeline,
            backdrop_copy_pipeline,
            extract_pipeline,
            blur_pipeline,
            composite_pipeline,
            ssao_raw_pipeline,
            ssao_blur_pipeline,
            ssao_apply_pipeline,
            smaa_edges_pipeline,
            smaa_weights_pipeline,
            smaa_blend_pipeline,
            output_format,
            water_depth_format,
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

    pub fn hdr_msaa_view(&self) -> Option<vk::ImageView> {
        self.hdr_msaa.as_ref().map(|t| t.view)
    }

    pub fn depth_msaa_view(&self) -> Option<vk::ImageView> {
        self.depth_msaa.as_ref().map(|t| t.view)
    }

    pub fn msaa_samples(&self) -> vk::SampleCountFlags {
        self.msaa_samples
    }

    pub fn ui_backdrop_view(&self) -> vk::ImageView {
        self.ui_backdrop.view
    }

    pub fn water_depth_view(&self) -> vk::ImageView {
        self.water_depth.view
    }

    pub fn linear_sampler(&self) -> vk::Sampler {
        self.linear_sampler
    }

    pub fn resize(
        &mut self,
        ctx: &mut VkContext,
        width: u32,
        height: u32,
        output_format: vk::Format,
        depth_sample_view: vk::ImageView,
    ) -> Result<()> {
        let w = width.max(1);
        let h = height.max(1);
        let bw = (w / 4).max(1);
        let bh = (h / 4).max(1);
        let sw = (w / 2).max(1);
        let sh = (h / 2).max(1);

        let new_hdr = ImageTarget::new(ctx, "zhr_hdr_scene", w, h)?;
        let (new_hdr_msaa, new_depth_msaa) = if self.msaa_samples != vk::SampleCountFlags::TYPE_1 {
            let hc = ImageTarget::new_with_usage(
                ctx,
                "zhr_hdr_msaa",
                w,
                h,
                HDR_FORMAT,
                vk::ImageUsageFlags::COLOR_ATTACHMENT,
                vk::ImageAspectFlags::COLOR,
                self.msaa_samples,
            )?;
            let dm = ImageTarget::new_with_usage(
                ctx,
                "zhr_depth_msaa",
                w,
                h,
                ctx.depth_format,
                vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT,
                vk::ImageAspectFlags::DEPTH,
                self.msaa_samples,
            )?;
            (Some(hc), Some(dm))
        } else {
            (None, None)
        };
        let new_ba = ImageTarget::new(ctx, "zhr_bloom_a", bw, bh)?;
        let new_bb = ImageTarget::new(ctx, "zhr_bloom_b", bw, bh)?;
        let new_ui = ImageTarget::new(ctx, "zhr_ui_backdrop", bw, bh)?;
        let new_wd =
            ImageTarget::new_with_format(ctx, "zhr_water_snapshot", w, h, self.water_depth_format)?;
        let new_sa = ImageTarget::new(ctx, "zhr_ssao_a", sw, sh)?;
        let new_sb = ImageTarget::new(ctx, "zhr_ssao_b", sw, sh)?;
        let new_se =
            ImageTarget::new_with_format(ctx, "zhr_smaa_edges", w, h, vk::Format::R8G8_UNORM)?;
        let new_sw = ImageTarget::new_with_format(
            ctx,
            "zhr_smaa_weights",
            w,
            h,
            vk::Format::R8G8B8A8_UNORM,
        )?;
        let new_st = ImageTarget::new_with_usage(
            ctx,
            "zhr_smaa_temp",
            w,
            h,
            HDR_FORMAT,
            vk::ImageUsageFlags::COLOR_ATTACHMENT
                | vk::ImageUsageFlags::SAMPLED
                | vk::ImageUsageFlags::TRANSFER_SRC
                | vk::ImageUsageFlags::TRANSFER_DST,
            vk::ImageAspectFlags::COLOR,
            vk::SampleCountFlags::TYPE_1,
        )?;
        transition_images_to_shader_read(
            ctx,
            &[
                new_ba.image,
                new_bb.image,
                new_ui.image,
                new_wd.image,
                new_sa.image,
                new_sb.image,
                new_se.image,
                new_sw.image,
                new_st.image,
            ],
        )?;

        let old_hdr = std::mem::replace(&mut self.hdr_scene, new_hdr);
        let old_hdr_msaa = std::mem::replace(&mut self.hdr_msaa, new_hdr_msaa);
        let old_depth_msaa = std::mem::replace(&mut self.depth_msaa, new_depth_msaa);
        let old_ba = std::mem::replace(&mut self.bloom_a, new_ba);
        let old_bb = std::mem::replace(&mut self.bloom_b, new_bb);
        let old_ui = std::mem::replace(&mut self.ui_backdrop, new_ui);
        let old_wd = std::mem::replace(&mut self.water_depth, new_wd);
        let old_sa = std::mem::replace(&mut self.ssao_a, new_sa);
        let old_sb = std::mem::replace(&mut self.ssao_b, new_sb);
        let old_se = std::mem::replace(&mut self.smaa_edges, new_se);
        let old_sw = std::mem::replace(&mut self.smaa_weights, new_sw);
        let old_st = std::mem::replace(&mut self.smaa_temp, new_st);
        old_hdr.destroy(ctx);
        if let Some(t) = old_hdr_msaa {
            t.destroy(ctx);
        }
        if let Some(t) = old_depth_msaa {
            t.destroy(ctx);
        }
        old_ba.destroy(ctx);
        old_bb.destroy(ctx);
        old_ui.destroy(ctx);
        old_wd.destroy(ctx);
        old_sa.destroy(ctx);
        old_sb.destroy(ctx);
        old_se.destroy(ctx);
        old_sw.destroy(ctx);
        old_st.destroy(ctx);

        self.depth_sample_view = depth_sample_view;

        if output_format != self.output_format {
            let new_comp = create_fullscreen_pipeline(
                &ctx.device,
                self.pipeline_layout,
                self.vs_module,
                self.fs_composite_module,
                c"fs_composite",
                output_format,
                false,
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
        let write_set = |set: vk::DescriptorSet,
                         view0: vk::ImageView,
                         layout0: vk::ImageLayout,
                         view1: vk::ImageView,
                         sampler: vk::Sampler| {
            let img0 = [vk::DescriptorImageInfo::default()
                .image_view(view0)
                .image_layout(layout0)];
            let img1 = [vk::DescriptorImageInfo::default()
                .image_view(view1)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
            let samp = [vk::DescriptorImageInfo::default().sampler(sampler)];
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

        let sro = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;
        let dsro = vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        write_set(
            self.set_extract,
            self.hdr_scene.view,
            sro,
            self.bloom_a.view,
            self.linear_sampler,
        );
        write_set(
            self.set_blur_h,
            self.bloom_a.view,
            sro,
            self.bloom_a.view,
            self.linear_sampler,
        );
        write_set(
            self.set_blur_v,
            self.bloom_b.view,
            sro,
            self.bloom_b.view,
            self.linear_sampler,
        );
        write_set(
            self.set_composite,
            self.hdr_scene.view,
            sro,
            self.bloom_a.view,
            self.linear_sampler,
        );

        // SSAO passes: bind bloom_a (always in SHADER_READ_ONLY_OPTIMAL during SSAO) as dummy t_src1.
        write_set(
            self.set_ssao_raw,
            self.depth_sample_view,
            dsro,
            self.bloom_a.view,
            self.nearest_sampler,
        );
        write_set(
            self.set_ssao_blur_h,
            self.ssao_a.view,
            sro,
            self.bloom_a.view,
            self.linear_sampler,
        );
        write_set(
            self.set_ssao_blur_v,
            self.ssao_b.view,
            sro,
            self.bloom_a.view,
            self.linear_sampler,
        );
        write_set(
            self.set_ssao_apply,
            self.ssao_a.view,
            sro,
            self.bloom_a.view,
            self.linear_sampler,
        );
        // Pre-water snapshot pass: reads opaque depth (t_src0) + opaque HDR seabed/rocks (t_src1).
        write_set(
            self.set_water_snapshot,
            self.depth_sample_view,
            dsro,
            self.hdr_scene.view,
            self.nearest_sampler,
        );
        // 3-pass SMAA 1x descriptors (edges reads hdr_scene; weights reads smaa_edges; blend reads hdr_scene + smaa_weights).
        write_set(
            self.set_smaa_edges,
            self.hdr_scene.view,
            sro,
            self.bloom_a.view,
            self.nearest_sampler,
        );
        write_set(
            self.set_smaa_weights,
            self.smaa_edges.view,
            sro,
            self.bloom_a.view,
            self.nearest_sampler,
        );
        write_set(
            self.set_smaa_blend,
            self.hdr_scene.view,
            sro,
            self.smaa_weights.view,
            self.linear_sampler,
        );
    }

    /// Runs 3-pass SMAA 1x morphological anti-aliasing directly on the resolved 3D `hdr_scene`
    /// (before 2D HUD/UI draws on top, so fonts & icons remain 100% razor-sharp while alpha-tested
    /// fences, barbed wire, palm fronds, shadows, and specular edges are smoothed with zero blur).
    /// Expects `hdr_scene` in `COLOR_ATTACHMENT_OPTIMAL` and leaves it in `COLOR_ATTACHMENT_OPTIMAL`.
    pub fn apply_smaa(
        &mut self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        frame_slot: usize,
        settings: GraphicsSettings,
    ) -> Result<()> {
        if settings.mode == GraphicsMode::Classic || !settings.post_fx {
            return Ok(());
        }
        let split_x_uv = if settings.mode == GraphicsMode::SplitScreen {
            0.5
        } else {
            -1.0
        };
        let inv_w = 1.0 / (self.hdr_scene.width.max(1) as f32);
        let inv_h = 1.0 / (self.hdr_scene.height.max(1) as f32);

        let base_slot = frame_slot * SLOTS_PER_FRAME + 12;
        let off_edges = (base_slot * self.slot_stride) as u32;
        let off_weights = ((base_slot + 1) * self.slot_stride) as u32;
        let off_blend = ((base_slot + 2) * self.slot_stride) as u32;

        let p_edges = PostParams {
            params0: [1.0, split_x_uv, inv_w, inv_h],
            params1: [0.0, 0.0, 0.075, 1.0],
            params2: [0.0; 4],
        };
        let p_weights = PostParams {
            params0: [1.0, split_x_uv, inv_w, inv_h],
            params1: [0.0; 4],
            params2: [0.0; 4],
        };
        let p_blend = PostParams {
            params0: [1.0, split_x_uv, inv_w, inv_h],
            params1: [0.0; 4],
            params2: [0.0; 4],
        };
        self.params_ubo
            .write_bytes(off_edges as usize, bytemuck::bytes_of(&p_edges))?;
        self.params_ubo
            .write_bytes(off_weights as usize, bytemuck::bytes_of(&p_weights))?;
        self.params_ubo
            .write_bytes(off_blend as usize, bytemuck::bytes_of(&p_blend))?;

        let full_extent = vk::Extent2D {
            width: self.hdr_scene.width,
            height: self.hdr_scene.height,
        };

        // 1. Transition hdr_scene -> SHADER_READ_ONLY_OPTIMAL.
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

        // 2. Pass 1: Edge Detection -> smaa_edges.
        cmd_barrier(
            device,
            cmd,
            self.smaa_edges.image,
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
            self.smaa_edges.view,
            full_extent,
            self.smaa_edges_pipeline,
            self.pipeline_layout,
            self.set_smaa_edges,
            off_edges,
            false,
        );
        cmd_barrier(
            device,
            cmd,
            self.smaa_edges.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        // 3. Pass 2: Blending Weight Calculation -> smaa_weights.
        cmd_barrier(
            device,
            cmd,
            self.smaa_weights.image,
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
            self.smaa_weights.view,
            full_extent,
            self.smaa_weights_pipeline,
            self.pipeline_layout,
            self.set_smaa_weights,
            off_weights,
            false,
        );
        cmd_barrier(
            device,
            cmd,
            self.smaa_weights.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        // 4. Pass 3: Neighborhood Blending -> smaa_temp.
        cmd_barrier(
            device,
            cmd,
            self.smaa_temp.image,
            vk::PipelineStageFlags2::FRAGMENT_SHADER | vk::PipelineStageFlags2::TRANSFER,
            vk::AccessFlags2::SHADER_SAMPLED_READ | vk::AccessFlags2::TRANSFER_READ,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        );
        draw_fullscreen_pass(
            device,
            cmd,
            self.smaa_temp.view,
            full_extent,
            self.smaa_blend_pipeline,
            self.pipeline_layout,
            self.set_smaa_blend,
            off_blend,
            false,
        );

        // 5. Copy anti-aliased smaa_temp -> hdr_scene and restore layouts.
        cmd_barrier(
            device,
            cmd,
            self.smaa_temp.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::TRANSFER,
            vk::AccessFlags2::TRANSFER_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
        );
        cmd_barrier(
            device,
            cmd,
            self.hdr_scene.image,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::PipelineStageFlags2::TRANSFER,
            vk::AccessFlags2::TRANSFER_WRITE,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        );
        let region = [vk::ImageCopy::default()
            .src_subresource(
                vk::ImageSubresourceLayers::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .mip_level(0)
                    .base_array_layer(0)
                    .layer_count(1),
            )
            .dst_subresource(
                vk::ImageSubresourceLayers::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .mip_level(0)
                    .base_array_layer(0)
                    .layer_count(1),
            )
            .extent(vk::Extent3D {
                width: full_extent.width,
                height: full_extent.height,
                depth: 1,
            })];
        // SAFETY: Both images match format/extent and are in TRANSFER_SRC/TRANSFER_DST layouts.
        unsafe {
            device.cmd_copy_image(
                cmd,
                self.smaa_temp.image,
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                self.hdr_scene.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &region,
            );
        }
        cmd_barrier(
            device,
            cmd,
            self.smaa_temp.image,
            vk::PipelineStageFlags2::TRANSFER,
            vk::AccessFlags2::TRANSFER_READ,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );
        cmd_barrier(
            device,
            cmd,
            self.hdr_scene.image,
            vk::PipelineStageFlags2::TRANSFER,
            vk::AccessFlags2::TRANSFER_WRITE,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_READ | vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        );

        Ok(())
    }

    /// Computes half-res depth-based bilateral SSAO and multiplies it directly into `hdr_scene`.
    /// Must be called outside dynamic rendering while `hdr_scene` is in `COLOR_ATTACHMENT_OPTIMAL`
    /// and `depth_image` is in `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`. Leaves both images in those same layouts.
    #[allow(clippy::too_many_arguments)]
    pub fn apply_ssao(
        &mut self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        frame_slot: usize,
        depth_image: vk::Image,
        depth_format: vk::Format,
        proj: Mat4,
        settings: GraphicsSettings,
    ) -> Result<()> {
        if settings.mode == GraphicsMode::Classic || !settings.pbr_lighting {
            return Ok(());
        }
        // Only run SSAO for perspective 3D projections (m32 != 0 and m23 != 0).
        let cols = proj.to_cols_array_2d();
        let m00 = cols[0][0];
        let m11 = cols[1][1];
        let m22 = cols[2][2];
        let m23 = cols[2][3];
        let m32 = cols[3][2];
        if m00.abs() < 1e-4 || m11.abs() < 1e-4 || m32.abs() < 1e-5 || m23.abs() < 1e-5 {
            return Ok(());
        }
        // Normalize by m23 so `depth_to_view_z` is exact for both RH (m23=-1) and LH (m23=+1).
        let m22_eff = m22 / m23;
        let m32_eff = m32 / m23;

        let split_x_uv = if settings.mode == GraphicsMode::SplitScreen {
            0.5
        } else {
            -1.0
        };
        let base_slot = frame_slot * SLOTS_PER_FRAME + 4;
        let off_raw = (base_slot * self.slot_stride) as u32;
        let off_bh = ((base_slot + 1) * self.slot_stride) as u32;
        let off_bv = ((base_slot + 2) * self.slot_stride) as u32;
        let off_apply = ((base_slot + 3) * self.slot_stride) as u32;

        let inv_sw = 1.0 / (self.ssao_a.width.max(1) as f32);
        let inv_sh = 1.0 / (self.ssao_a.height.max(1) as f32);
        let proj_unpack = [1.0 / m00.abs(), 1.0 / m11.abs(), m22_eff, m32_eff];

        let p_raw = PostParams {
            params0: [1.0, split_x_uv, inv_sw, inv_sh],
            params1: [0.0, 0.0, 0.0, 1.85],
            params2: proj_unpack,
        };
        let p_bh = PostParams {
            params0: [1.0, split_x_uv, inv_sw, inv_sh],
            params1: [inv_sw, 0.0, 0.0, 0.0],
            params2: proj_unpack,
        };
        let p_bv = PostParams {
            params0: [1.0, split_x_uv, inv_sw, inv_sh],
            params1: [0.0, inv_sh, 0.0, 0.0],
            params2: proj_unpack,
        };
        let p_apply = PostParams {
            params0: [1.0, split_x_uv, inv_sw, inv_sh],
            params1: [0.0, 0.0, 0.0, 1.0],
            params2: proj_unpack,
        };

        self.params_ubo
            .write_bytes(off_raw as usize, bytemuck::bytes_of(&p_raw))?;
        self.params_ubo
            .write_bytes(off_bh as usize, bytemuck::bytes_of(&p_bh))?;
        self.params_ubo
            .write_bytes(off_bv as usize, bytemuck::bytes_of(&p_bv))?;
        self.params_ubo
            .write_bytes(off_apply as usize, bytemuck::bytes_of(&p_apply))?;

        let depth_aspect = depth_barrier_aspect(depth_format);

        // Transition depth attachment -> DEPTH_STENCIL_READ_ONLY_OPTIMAL for SSAO sampling.
        cmd_depth_barrier(
            device,
            cmd,
            depth_image,
            depth_aspect,
            vk::PipelineStageFlags2::EARLY_FRAGMENT_TESTS
                | vk::PipelineStageFlags2::LATE_FRAGMENT_TESTS,
            vk::AccessFlags2::DEPTH_STENCIL_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        );

        let ssao_extent = vk::Extent2D {
            width: self.ssao_a.width,
            height: self.ssao_a.height,
        };

        // Pass 1: Raw 12-tap spiral SSAO -> ssao_a.
        cmd_barrier(
            device,
            cmd,
            self.ssao_a.image,
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
            self.ssao_a.view,
            ssao_extent,
            self.ssao_raw_pipeline,
            self.pipeline_layout,
            self.set_ssao_raw,
            off_raw,
            false,
        );
        cmd_barrier(
            device,
            cmd,
            self.ssao_a.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        // Pass 2: Bilateral horizontal blur -> ssao_b.
        cmd_barrier(
            device,
            cmd,
            self.ssao_b.image,
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
            self.ssao_b.view,
            ssao_extent,
            self.ssao_blur_pipeline,
            self.pipeline_layout,
            self.set_ssao_blur_h,
            off_bh,
            false,
        );
        cmd_barrier(
            device,
            cmd,
            self.ssao_b.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        // Pass 3: Bilateral vertical blur -> ssao_a.
        cmd_barrier(
            device,
            cmd,
            self.ssao_a.image,
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
            self.ssao_a.view,
            ssao_extent,
            self.ssao_blur_pipeline,
            self.pipeline_layout,
            self.set_ssao_blur_v,
            off_bv,
            false,
        );
        cmd_barrier(
            device,
            cmd,
            self.ssao_a.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        // Pass 4: Multiply smooth SSAO directly into hdr_scene (load_op = LOAD, DST_COLOR * ZERO).
        let hdr_extent = vk::Extent2D {
            width: self.hdr_scene.width,
            height: self.hdr_scene.height,
        };
        draw_fullscreen_pass(
            device,
            cmd,
            self.hdr_scene.view,
            hdr_extent,
            self.ssao_apply_pipeline,
            self.pipeline_layout,
            self.set_ssao_apply,
            off_apply,
            true,
        );

        // Transition depth back -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL for remaining passes / next frame.
        cmd_depth_barrier(
            device,
            cmd,
            depth_image,
            depth_aspect,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::PipelineStageFlags2::EARLY_FRAGMENT_TESTS
                | vk::PipelineStageFlags2::LATE_FRAGMENT_TESTS,
            vk::AccessFlags2::DEPTH_STENCIL_ATTACHMENT_READ
                | vk::AccessFlags2::DEPTH_STENCIL_ATTACHMENT_WRITE,
            vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        );

        Ok(())
    }

    /// Snapshots the opaque pre-water `hdr_scene` (RGB) and linearized view-space depth (A) into
    /// `water_depth` (`R16G16B16A16_SFLOAT`) so the water shader gets true screen-space seabed/rock
    /// refraction, Beer-Lambert coastal absorption, soft shoreline feathering, animated rock foam,
    /// and shallow underwater caustics.
    #[allow(clippy::too_many_arguments)]
    pub fn capture_water_depth(
        &mut self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        frame_slot: usize,
        depth_image: vk::Image,
        depth_format: vk::Format,
        proj: Mat4,
    ) -> Result<()> {
        let cols = proj.to_cols_array_2d();
        let m00 = cols[0][0];
        let m11 = cols[1][1];
        let m22 = cols[2][2];
        let m23 = cols[2][3];
        let m32 = cols[3][2];
        if m00.abs() < 1e-4 || m11.abs() < 1e-4 || m32.abs() < 1e-5 || m23.abs() < 1e-5 {
            return Ok(());
        }
        let m22_eff = m22 / m23;
        let m32_eff = m32 / m23;

        let off_wd = ((frame_slot * SLOTS_PER_FRAME + 11) * self.slot_stride) as u32;
        let p_wd = PostParams {
            params0: [
                1.0,
                -1.0,
                1.0 / (self.water_depth.width.max(1) as f32),
                1.0 / (self.water_depth.height.max(1) as f32),
            ],
            params1: [0.0; 4],
            params2: [1.0 / m00.abs(), 1.0 / m11.abs(), m22_eff, m32_eff],
        };
        self.params_ubo
            .write_bytes(off_wd as usize, bytemuck::bytes_of(&p_wd))?;

        let depth_aspect = depth_barrier_aspect(depth_format);
        cmd_depth_barrier(
            device,
            cmd,
            depth_image,
            depth_aspect,
            vk::PipelineStageFlags2::EARLY_FRAGMENT_TESTS
                | vk::PipelineStageFlags2::LATE_FRAGMENT_TESTS,
            vk::AccessFlags2::DEPTH_STENCIL_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        );
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

        cmd_barrier(
            device,
            cmd,
            self.water_depth.image,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        );
        let wd_extent = vk::Extent2D {
            width: self.water_depth.width,
            height: self.water_depth.height,
        };
        draw_fullscreen_pass(
            device,
            cmd,
            self.water_depth.view,
            wd_extent,
            self.water_depth_pipeline,
            self.pipeline_layout,
            self.set_water_snapshot,
            off_wd,
            false,
        );
        cmd_barrier(
            device,
            cmd,
            self.water_depth.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        cmd_barrier(
            device,
            cmd,
            self.hdr_scene.image,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_READ | vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        );
        cmd_depth_barrier(
            device,
            cmd,
            depth_image,
            depth_aspect,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::PipelineStageFlags2::EARLY_FRAGMENT_TESTS
                | vk::PipelineStageFlags2::LATE_FRAGMENT_TESTS,
            vk::AccessFlags2::DEPTH_STENCIL_ATTACHMENT_READ
                | vk::AccessFlags2::DEPTH_STENCIL_ATTACHMENT_WRITE,
            vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        );
        Ok(())
    }

    /// Captures and Gaussian-blurs the finished 3D `hdr_scene` into quarter-res `ui_backdrop`
    /// for real-time frosted-glass UI panels. Expects `hdr_scene` in `COLOR_ATTACHMENT_OPTIMAL`
    /// outside dynamic rendering, and returns it in `COLOR_ATTACHMENT_OPTIMAL` with `ui_backdrop`
    /// in `SHADER_READ_ONLY_OPTIMAL`.
    pub fn capture_ui_backdrop(
        &mut self,
        device: &ash::Device,
        cmd: vk::CommandBuffer,
        frame_slot: usize,
    ) -> Result<()> {
        let base_slot = frame_slot * SLOTS_PER_FRAME + 8;
        let off_copy = (base_slot * self.slot_stride) as u32;
        let off_bh = ((base_slot + 1) * self.slot_stride) as u32;
        let off_bv = ((base_slot + 2) * self.slot_stride) as u32;

        let inv_w = 1.0 / (self.hdr_scene.width.max(1) as f32);
        let inv_h = 1.0 / (self.hdr_scene.height.max(1) as f32);
        let inv_bw = 1.0 / (self.bloom_a.width.max(1) as f32);
        let inv_bh = 1.0 / (self.bloom_a.height.max(1) as f32);

        let p_copy = PostParams {
            params0: [1.0, -1.0, inv_w, inv_h],
            params1: [0.0; 4],
            params2: [0.0; 4],
        };
        let p_bh = PostParams {
            params0: [1.0, -1.0, inv_bw, inv_bh],
            params1: [inv_bw * 1.65, 0.0, 0.0, 0.0],
            params2: [0.0; 4],
        };
        let p_bv = PostParams {
            params0: [1.0, -1.0, inv_bw, inv_bh],
            params1: [0.0, inv_bh * 1.65, 0.0, 0.0],
            params2: [0.0; 4],
        };

        self.params_ubo
            .write_bytes(off_copy as usize, bytemuck::bytes_of(&p_copy))?;
        self.params_ubo
            .write_bytes(off_bh as usize, bytemuck::bytes_of(&p_bh))?;
        self.params_ubo
            .write_bytes(off_bv as usize, bytemuck::bytes_of(&p_bv))?;

        let blur_extent = vk::Extent2D {
            width: self.bloom_a.width,
            height: self.bloom_a.height,
        };

        // 1. hdr_scene -> SHADER_READ_ONLY_OPTIMAL.
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

        // 2. Downsample hdr_scene -> bloom_a.
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
            blur_extent,
            self.backdrop_copy_pipeline,
            self.pipeline_layout,
            self.set_extract,
            off_copy,
            false,
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

        // 3. Horizontal Gaussian blur bloom_a -> bloom_b.
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
            blur_extent,
            self.blur_pipeline,
            self.pipeline_layout,
            self.set_blur_h,
            off_bh,
            false,
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

        // 4. Vertical Gaussian blur bloom_b -> ui_backdrop.
        cmd_barrier(
            device,
            cmd,
            self.ui_backdrop.image,
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
            self.ui_backdrop.view,
            blur_extent,
            self.blur_pipeline,
            self.pipeline_layout,
            self.set_blur_v,
            off_bv,
            false,
        );
        cmd_barrier(
            device,
            cmd,
            self.ui_backdrop.image,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );

        // 5. Restore hdr_scene -> COLOR_ATTACHMENT_OPTIMAL for 2D UI drawing.
        cmd_barrier(
            device,
            cmd,
            self.hdr_scene.image,
            vk::PipelineStageFlags2::FRAGMENT_SHADER,
            vk::AccessFlags2::SHADER_SAMPLED_READ,
            vk::PipelineStageFlags2::COLOR_ATTACHMENT_OUTPUT,
            vk::AccessFlags2::COLOR_ATTACHMENT_WRITE,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        );

        Ok(())
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
        hdr_display: HdrDisplayConfig,
    ) -> Result<()> {
        let post_active = settings.mode != GraphicsMode::Classic && settings.post_fx;
        let split_x_uv = if settings.mode == GraphicsMode::SplitScreen {
            0.5
        } else {
            -1.0
        };

        let base_slot = frame_slot * SLOTS_PER_FRAME;
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
            params1: [0.0, 0.0, 1.14, 0.0],
            params2: [0.0; 4],
        };
        let p_blur_h = PostParams {
            params0: [1.0, split_x_uv, inv_bw, inv_bh],
            params1: [inv_bw, 0.0, 0.0, 0.0],
            params2: [0.0; 4],
        };
        let p_blur_v = PostParams {
            params0: [1.0, split_x_uv, inv_bw, inv_bh],
            params1: [0.0, inv_bh, 0.0, 0.0],
            params2: [0.0; 4],
        };
        let p_comp = PostParams {
            params0: [
                if post_active { 1.0 } else { 0.0 },
                split_x_uv,
                inv_w,
                inv_h,
            ],
            params1: [0.0, 0.0, 1.14, 0.24],
            params2: [
                (hdr_display.mode as u32) as f32,
                hdr_display.paper_white_nits,
                hdr_display.max_luminance_nits,
                0.0,
            ],
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
                false,
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
                false,
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
                false,
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
            false,
        );

        Ok(())
    }

    pub fn destroy(self, ctx: &mut VkContext) {
        self.hdr_scene.destroy(ctx);
        if let Some(t) = self.hdr_msaa {
            t.destroy(ctx);
        }
        if let Some(t) = self.depth_msaa {
            t.destroy(ctx);
        }
        self.bloom_a.destroy(ctx);
        self.bloom_b.destroy(ctx);
        self.ui_backdrop.destroy(ctx);
        self.water_depth.destroy(ctx);
        self.ssao_a.destroy(ctx);
        self.ssao_b.destroy(ctx);
        self.smaa_edges.destroy(ctx);
        self.smaa_weights.destroy(ctx);
        self.smaa_temp.destroy(ctx);
        self.params_ubo.destroy(ctx);
        // SAFETY: Caller ensures GPU is idle.
        unsafe {
            ctx.device.destroy_pipeline(self.water_depth_pipeline, None);
            ctx.device
                .destroy_pipeline(self.backdrop_copy_pipeline, None);
            ctx.device.destroy_pipeline(self.extract_pipeline, None);
            ctx.device.destroy_pipeline(self.blur_pipeline, None);
            ctx.device.destroy_pipeline(self.composite_pipeline, None);
            ctx.device.destroy_pipeline(self.ssao_raw_pipeline, None);
            ctx.device.destroy_pipeline(self.ssao_blur_pipeline, None);
            ctx.device.destroy_pipeline(self.ssao_apply_pipeline, None);
            ctx.device.destroy_pipeline(self.smaa_edges_pipeline, None);
            ctx.device
                .destroy_pipeline(self.smaa_weights_pipeline, None);
            ctx.device.destroy_pipeline(self.smaa_blend_pipeline, None);
            ctx.device.destroy_shader_module(self.vs_module, None);
            ctx.device
                .destroy_shader_module(self.fs_water_depth_module, None);
            ctx.device
                .destroy_shader_module(self.fs_backdrop_copy_module, None);
            ctx.device
                .destroy_shader_module(self.fs_extract_module, None);
            ctx.device.destroy_shader_module(self.fs_blur_module, None);
            ctx.device
                .destroy_shader_module(self.fs_composite_module, None);
            ctx.device
                .destroy_shader_module(self.fs_ssao_raw_module, None);
            ctx.device
                .destroy_shader_module(self.fs_ssao_blur_module, None);
            ctx.device
                .destroy_shader_module(self.fs_ssao_apply_module, None);
            ctx.device
                .destroy_shader_module(self.fs_smaa_edges_module, None);
            ctx.device
                .destroy_shader_module(self.fs_smaa_weights_module, None);
            ctx.device
                .destroy_shader_module(self.fs_smaa_blend_module, None);
            ctx.device
                .destroy_descriptor_pool(self.descriptor_pool, None);
            ctx.device
                .destroy_pipeline_layout(self.pipeline_layout, None);
            ctx.device
                .destroy_descriptor_set_layout(self.set_layout, None);
            ctx.device.destroy_sampler(self.linear_sampler, None);
            ctx.device.destroy_sampler(self.nearest_sampler, None);
        }
    }
}

fn depth_barrier_aspect(format: vk::Format) -> vk::ImageAspectFlags {
    if matches!(
        format,
        vk::Format::D32_SFLOAT_S8_UINT | vk::Format::D24_UNORM_S8_UINT
    ) {
        vk::ImageAspectFlags::DEPTH | vk::ImageAspectFlags::STENCIL
    } else {
        vk::ImageAspectFlags::DEPTH
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
    preserve_dst: bool,
) {
    let color_att = [vk::RenderingAttachmentInfo::default()
        .image_view(view)
        .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
        .load_op(if preserve_dst {
            vk::AttachmentLoadOp::LOAD
        } else {
            vk::AttachmentLoadOp::DONT_CARE
        })
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

#[allow(clippy::too_many_arguments)]
fn cmd_depth_barrier(
    device: &ash::Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    aspect: vk::ImageAspectFlags,
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
                .aspect_mask(aspect)
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
    multiply_blend: bool,
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
    let blend_att = [if multiply_blend {
        vk::PipelineColorBlendAttachmentState::default()
            .color_write_mask(
                vk::ColorComponentFlags::R
                    | vk::ColorComponentFlags::G
                    | vk::ColorComponentFlags::B,
            )
            .blend_enable(true)
            .src_color_blend_factor(vk::BlendFactor::DST_COLOR)
            .dst_color_blend_factor(vk::BlendFactor::ZERO)
            .color_blend_op(vk::BlendOp::ADD)
            .src_alpha_blend_factor(vk::BlendFactor::ZERO)
            .dst_alpha_blend_factor(vk::BlendFactor::ONE)
            .alpha_blend_op(vk::BlendOp::ADD)
    } else {
        vk::PipelineColorBlendAttachmentState::default()
            .color_write_mask(vk::ColorComponentFlags::RGBA)
            .blend_enable(false)
    }];
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
