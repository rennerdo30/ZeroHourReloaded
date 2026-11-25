use ash::vk;

use crate::error::{Result, VkError};

/// Packed pipeline state key mirroring `WW3D2::ShaderClass` (`Core/Libraries/Source/WWVegas/WW3D2/shader.h`).
///
/// Bit layout matches `ShaderShiftConstants` so `WW3D2::ShaderClass::Get_Bits()` can be passed directly.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ShaderKey(pub u32);

impl ShaderKey {
    pub const SHIFT_DEPTH_COMPARE: u32 = 0;
    pub const SHIFT_DEPTH_MASK: u32 = 3;
    pub const SHIFT_COLOR_MASK: u32 = 4;
    pub const SHIFT_DST_BLEND: u32 = 5;
    pub const SHIFT_FOG: u32 = 8;
    pub const SHIFT_PRI_GRADIENT: u32 = 10;
    pub const SHIFT_SEC_GRADIENT: u32 = 13;
    pub const SHIFT_SRC_BLEND: u32 = 14;
    pub const SHIFT_TEXTURING: u32 = 16;
    pub const SHIFT_ALPHA_TEST: u32 = 18;
    pub const SHIFT_CULL_MODE: u32 = 19;
    /// Optional extension bit (outside WW3D2's 24-bit shader mask): when set with `SHIFT_CULL_MODE`,
    /// culls clockwise triangles (`D3DCULL_CW` -> front face is counter-clockwise).
    pub const SHIFT_CULL_CW: u32 = 28;
    /// Optional extension bits (29..30) for extended `SrcBlend` factors (e.g. `DstColor`).
    pub const SHIFT_SRC_BLEND_EXT: u32 = 29;

    pub const fn from_raw(bits: u32) -> Self {
        Self(bits)
    }

    /// Standard opaque 3D lit/textured shader (depth test LEQUAL, depth write ON, backface cull ON).
    pub const OPAQUE_3D: Self = Self::build(
        DepthCompare::LessEqual,
        true,
        true,
        SrcBlend::One,
        DstBlend::Zero,
        false,
        true,
        false,
        true,
    );

    /// Opaque 3D shader with no texturing (vertex colors + lighting).
    pub const OPAQUE_3D_UNTEXTURED: Self = Self::build(
        DepthCompare::LessEqual,
        true,
        true,
        SrcBlend::One,
        DstBlend::Zero,
        false,
        false,
        false,
        true,
    );

    /// Standard alpha-blended shader (depth test LEQUAL, depth write OFF, SrcAlpha / InvSrcAlpha).
    pub const ALPHA_BLEND: Self = Self::build(
        DepthCompare::LessEqual,
        false,
        true,
        SrcBlend::SrcAlpha,
        DstBlend::OneMinusSrcAlpha,
        false,
        true,
        false,
        false,
    );

    /// Alpha-tested cutout shader (foliage, grates, decals).
    pub const ALPHA_TEST: Self = Self::build(
        DepthCompare::LessEqual,
        true,
        true,
        SrcBlend::One,
        DstBlend::Zero,
        false,
        true,
        true,
        false,
    );

    /// Additive particles/lasers/explosions shader.
    pub const ADDITIVE: Self = Self::build(
        DepthCompare::LessEqual,
        false,
        true,
        SrcBlend::SrcAlpha,
        DstBlend::One,
        false,
        true,
        false,
        false,
    );

    /// 2D UI / HUD overlay shader (no depth test/write, alpha blending, no culling).
    pub const UI_2D: Self = Self::build(
        DepthCompare::Always,
        false,
        true,
        SrcBlend::SrcAlpha,
        DstBlend::OneMinusSrcAlpha,
        false,
        true,
        false,
        false,
    );

    #[allow(clippy::too_many_arguments)]
    pub const fn build(
        depth_compare: DepthCompare,
        depth_write: bool,
        color_write: bool,
        src_blend: SrcBlend,
        dst_blend: DstBlend,
        fog: bool,
        texturing: bool,
        alpha_test: bool,
        cull_back: bool,
    ) -> Self {
        let src_bits = src_blend as u32;
        let bits = ((depth_compare as u32) << Self::SHIFT_DEPTH_COMPARE)
            | ((depth_write as u32) << Self::SHIFT_DEPTH_MASK)
            | ((color_write as u32) << Self::SHIFT_COLOR_MASK)
            | ((dst_blend as u32) << Self::SHIFT_DST_BLEND)
            | ((if fog { 1 } else { 0 }) << Self::SHIFT_FOG)
            | ((src_bits & 0x3) << Self::SHIFT_SRC_BLEND)
            | ((texturing as u32) << Self::SHIFT_TEXTURING)
            | ((alpha_test as u32) << Self::SHIFT_ALPHA_TEST)
            | ((cull_back as u32) << Self::SHIFT_CULL_MODE)
            | (((src_bits >> 2) & 0x3) << Self::SHIFT_SRC_BLEND_EXT);
        Self(bits)
    }

    pub const fn depth_compare(self) -> DepthCompare {
        match (self.0 >> Self::SHIFT_DEPTH_COMPARE) & 0x7 {
            0 => DepthCompare::Never,
            1 => DepthCompare::Less,
            2 => DepthCompare::Equal,
            3 => DepthCompare::LessEqual,
            4 => DepthCompare::Greater,
            5 => DepthCompare::NotEqual,
            6 => DepthCompare::GreaterEqual,
            _ => DepthCompare::Always,
        }
    }

    pub const fn depth_write(self) -> bool {
        ((self.0 >> Self::SHIFT_DEPTH_MASK) & 0x1) != 0
    }

    pub const fn color_write(self) -> bool {
        ((self.0 >> Self::SHIFT_COLOR_MASK) & 0x1) != 0
    }

    pub const fn dst_blend(self) -> DstBlend {
        match (self.0 >> Self::SHIFT_DST_BLEND) & 0x7 {
            1 => DstBlend::One,
            2 => DstBlend::SrcColor,
            3 => DstBlend::OneMinusSrcColor,
            4 => DstBlend::SrcAlpha,
            5 => DstBlend::OneMinusSrcAlpha,
            _ => DstBlend::Zero,
        }
    }

    pub const fn src_blend(self) -> SrcBlend {
        let low = (self.0 >> Self::SHIFT_SRC_BLEND) & 0x3;
        let high = (self.0 >> Self::SHIFT_SRC_BLEND_EXT) & 0x3;
        match low | (high << 2) {
            1 => SrcBlend::One,
            2 => SrcBlend::SrcAlpha,
            3 => SrcBlend::OneMinusSrcAlpha,
            4 => SrcBlend::DstColor,
            5 => SrcBlend::OneMinusDstColor,
            6 => SrcBlend::SrcColor,
            7 => SrcBlend::OneMinusSrcColor,
            _ => SrcBlend::Zero,
        }
    }

    pub const fn fog_enabled(self) -> bool {
        ((self.0 >> Self::SHIFT_FOG) & 0x3) != 0
    }

    pub const fn texturing_enabled(self) -> bool {
        ((self.0 >> Self::SHIFT_TEXTURING) & 0x1) != 0
    }

    pub const fn alpha_test_enabled(self) -> bool {
        ((self.0 >> Self::SHIFT_ALPHA_TEST) & 0x1) != 0
    }

    pub const fn cull_enabled(self) -> bool {
        ((self.0 >> Self::SHIFT_CULL_MODE) & 0x1) != 0
    }

    pub const fn cull_cw(self) -> bool {
        ((self.0 >> Self::SHIFT_CULL_CW) & 0x1) != 0
    }

    pub const fn blend_enabled(self) -> bool {
        !matches!(
            (self.src_blend(), self.dst_blend()),
            (SrcBlend::One, DstBlend::Zero)
        )
    }

    /// Mask containing only fixed-function Vulkan raster/depth/blend pipeline state bits.
    /// Texturing, fog, and alpha-test are handled dynamically via uniforms, drastically reducing PSO count.
    pub const fn pipeline_state_key(self) -> Self {
        const PIPELINE_MASK: u32 = (0x7 << ShaderKey::SHIFT_DEPTH_COMPARE)
            | (0x1 << ShaderKey::SHIFT_DEPTH_MASK)
            | (0x1 << ShaderKey::SHIFT_COLOR_MASK)
            | (0x7 << ShaderKey::SHIFT_DST_BLEND)
            | (0x3 << ShaderKey::SHIFT_SRC_BLEND)
            | (0x1 << ShaderKey::SHIFT_CULL_MODE)
            | (0x1 << ShaderKey::SHIFT_CULL_CW)
            | (0x3 << ShaderKey::SHIFT_SRC_BLEND_EXT);
        Self(self.0 & PIPELINE_MASK)
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DepthCompare {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterEqual = 6,
    Always = 7,
}

impl DepthCompare {
    pub const fn to_vk(self) -> vk::CompareOp {
        match self {
            Self::Never => vk::CompareOp::NEVER,
            Self::Less => vk::CompareOp::LESS,
            Self::Equal => vk::CompareOp::EQUAL,
            Self::LessEqual => vk::CompareOp::LESS_OR_EQUAL,
            Self::Greater => vk::CompareOp::GREATER,
            Self::NotEqual => vk::CompareOp::NOT_EQUAL,
            Self::GreaterEqual => vk::CompareOp::GREATER_OR_EQUAL,
            Self::Always => vk::CompareOp::ALWAYS,
        }
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SrcBlend {
    Zero = 0,
    One = 1,
    SrcAlpha = 2,
    OneMinusSrcAlpha = 3,
    DstColor = 4,
    OneMinusDstColor = 5,
    SrcColor = 6,
    OneMinusSrcColor = 7,
}

impl SrcBlend {
    pub const fn to_vk(self) -> vk::BlendFactor {
        match self {
            Self::Zero => vk::BlendFactor::ZERO,
            Self::One => vk::BlendFactor::ONE,
            Self::SrcAlpha => vk::BlendFactor::SRC_ALPHA,
            Self::OneMinusSrcAlpha => vk::BlendFactor::ONE_MINUS_SRC_ALPHA,
            Self::DstColor => vk::BlendFactor::DST_COLOR,
            Self::OneMinusDstColor => vk::BlendFactor::ONE_MINUS_DST_COLOR,
            Self::SrcColor => vk::BlendFactor::SRC_COLOR,
            Self::OneMinusSrcColor => vk::BlendFactor::ONE_MINUS_SRC_COLOR,
        }
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DstBlend {
    Zero = 0,
    One = 1,
    SrcColor = 2,
    OneMinusSrcColor = 3,
    SrcAlpha = 4,
    OneMinusSrcAlpha = 5,
}

impl DstBlend {
    pub const fn to_vk(self) -> vk::BlendFactor {
        match self {
            Self::Zero => vk::BlendFactor::ZERO,
            Self::One => vk::BlendFactor::ONE,
            Self::SrcColor => vk::BlendFactor::SRC_COLOR,
            Self::OneMinusSrcColor => vk::BlendFactor::ONE_MINUS_SRC_COLOR,
            Self::SrcAlpha => vk::BlendFactor::SRC_ALPHA,
            Self::OneMinusSrcAlpha => vk::BlendFactor::ONE_MINUS_SRC_ALPHA,
        }
    }
}

/// Hardware stencil operation matching D3D8 `D3DSTENCILOP` values (1..=8).
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum StencilOp {
    #[default]
    Keep = 1,
    Zero = 2,
    Replace = 3,
    IncrSat = 4,
    DecrSat = 5,
    Invert = 6,
    IncrWrap = 7,
    DecrWrap = 8,
}

impl StencilOp {
    pub const fn from_d3d(v: u32) -> Self {
        match v {
            2 => Self::Zero,
            3 => Self::Replace,
            4 => Self::IncrSat,
            5 => Self::DecrSat,
            6 => Self::Invert,
            7 => Self::IncrWrap,
            8 => Self::DecrWrap,
            _ => Self::Keep,
        }
    }

    pub const fn to_vk(self) -> vk::StencilOp {
        match self {
            Self::Keep => vk::StencilOp::KEEP,
            Self::Zero => vk::StencilOp::ZERO,
            Self::Replace => vk::StencilOp::REPLACE,
            Self::IncrSat => vk::StencilOp::INCREMENT_AND_CLAMP,
            Self::DecrSat => vk::StencilOp::DECREMENT_AND_CLAMP,
            Self::Invert => vk::StencilOp::INVERT,
            Self::IncrWrap => vk::StencilOp::INCREMENT_AND_WRAP,
            Self::DecrWrap => vk::StencilOp::DECREMENT_AND_WRAP,
        }
    }
}

/// Static PSO-cached portion of stencil state (masks & reference are dynamic in Vulkan).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct StencilPipelineKey {
    pub enabled: bool,
    pub func: DepthCompare,
    pub fail_op: StencilOp,
    pub depth_fail_op: StencilOp,
    pub pass_op: StencilOp,
}

impl Default for StencilPipelineKey {
    fn default() -> Self {
        Self {
            enabled: false,
            func: DepthCompare::Always,
            fail_op: StencilOp::Keep,
            depth_fail_op: StencilOp::Keep,
            pass_op: StencilOp::Keep,
        }
    }
}

/// Full D3D8-compatible stencil state (static ops + dynamic masks/reference).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StencilState {
    pub enabled: bool,
    pub func: DepthCompare,
    pub fail_op: StencilOp,
    pub depth_fail_op: StencilOp,
    pub pass_op: StencilOp,
    pub compare_mask: u32,
    pub write_mask: u32,
    pub reference: u32,
}

impl Default for StencilState {
    fn default() -> Self {
        Self {
            enabled: false,
            func: DepthCompare::Always,
            fail_op: StencilOp::Keep,
            depth_fail_op: StencilOp::Keep,
            pass_op: StencilOp::Keep,
            compare_mask: 0xFF,
            write_mask: 0xFF,
            reference: 0,
        }
    }
}

impl StencilState {
    pub const fn pipeline_key(self) -> StencilPipelineKey {
        if !self.enabled {
            StencilPipelineKey {
                enabled: false,
                func: DepthCompare::Always,
                fail_op: StencilOp::Keep,
                depth_fail_op: StencilOp::Keep,
                pass_op: StencilOp::Keep,
            }
        } else {
            StencilPipelineKey {
                enabled: true,
                func: self.func,
                fail_op: self.fail_op,
                depth_fail_op: self.depth_fail_op,
                pass_op: self.pass_op,
            }
        }
    }
}

/// Built-in WW3D2 uber-shader (WGSL -> SPIR-V via naga) supporting:
/// - World / View / Projection transforms
/// - Ambient + up to 4 directional/point lights
/// - Vertex color modulation + 2D texturing
/// - Alpha test cutout (discard)
/// - Linear distance fog
pub const WW3D2_UBER_WGSL: &str = r#"
struct Light {
    pos_or_dir_and_kind: vec4<f32>,
    diffuse_and_range: vec4<f32>,
    specular_and_atten: vec4<f32>,
};

struct DrawUniforms {
    world: mat4x4<f32>,
    view: mat4x4<f32>,
    projection: mat4x4<f32>,
    ambient_and_lighting: vec4<f32>,
    fog_color_and_enable: vec4<f32>,
    fog_and_shader_params: vec4<f32>,
    lights: array<Light, 4>,
    tfactor: vec4<f32>,
    stage0_ops: vec4<u32>,
    stage1_ops: vec4<u32>,
    camera_pos_and_pbr: vec4<f32>,
    render_flags: vec4<f32>,
    water_params: vec4<f32>,
    tex_matrix0: mat4x4<f32>,
    tex_matrix1: mat4x4<f32>,
    tex_gen_params: vec4<u32>,
};

@group(0) @binding(0) var<uniform> u: DrawUniforms;
@group(1) @binding(0) var t_diffuse: texture_2d<f32>;
@group(1) @binding(1) var s_diffuse: sampler;
@group(2) @binding(0) var t_diffuse1: texture_2d<f32>;
@group(2) @binding(1) var s_diffuse1: sampler;

struct VsIn {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) color: vec4<f32>,
    @location(3) uv0: vec2<f32>,
    @location(4) uv1: vec2<f32>,
};

struct VsOut {
    @builtin(position) clip_pos: vec4<f32>,
    @location(0) color: vec4<f32>,
    @location(1) uv0: vec2<f32>,
    @location(2) view_dist: f32,
    @location(3) uv1: vec2<f32>,
    @location(4) world_pos: vec3<f32>,
    @location(5) world_normal: vec3<f32>,
    @location(6) raw_color: vec4<f32>,
    @location(7) view_z: f32,
};

fn eval_tex_gen(mode: u32, src_idx: u32, tex_mat: mat4x4<f32>, view_pos: vec4<f32>, raw_uv0: vec2<f32>, raw_uv1: vec2<f32>) -> vec2<f32> {
    if (mode == 3u) {
        return (tex_mat * vec4<f32>(view_pos.xyz, 1.0)).xy;
    }
    if (mode == 2u) {
        return view_pos.xy;
    }
    let base_uv = select(raw_uv0, raw_uv1, src_idx == 1u);
    if (mode == 1u) {
        return (tex_mat * vec4<f32>(base_uv.x, base_uv.y, 1.0, 1.0)).xy;
    }
    return base_uv;
}

@vertex
fn vs_main(in: VsIn) -> VsOut {
    let world_pos = u.world * vec4<f32>(in.position, 1.0);
    let view_pos = u.view * world_pos;
    let clip_pos = u.projection * view_pos;

    let n_world_raw = (u.world * vec4<f32>(in.normal, 0.0)).xyz;
    let n_len = length(n_world_raw);
    let n_world = select(vec3<f32>(0.0, 0.0, 1.0), n_world_raw / max(n_len, 0.0001), n_len > 0.0001);

    var lit_rgb = in.color.rgb;
    if (u.ambient_and_lighting.w > 0.5) {
        var accum = u.ambient_and_lighting.rgb;
        for (var i: u32 = 0u; i < 4u; i = i + 1u) {
            let light = u.lights[i];
            let kind = light.pos_or_dir_and_kind.w;
            if (kind > 0.5 && kind < 1.5) {
                // Directional light (pos_or_dir points along travel direction; light vector is -dir)
                let l_dir = normalize(-light.pos_or_dir_and_kind.xyz);
                let ndotl = max(dot(n_world, l_dir), 0.0);
                accum = accum + light.diffuse_and_range.rgb * ndotl;
            } else if (kind >= 1.5) {
                // Point light
                let to_light = light.pos_or_dir_and_kind.xyz - world_pos.xyz;
                let dist = length(to_light);
                let range = max(light.diffuse_and_range.w, 0.001);
                if (dist < range) {
                    let l_dir = to_light / max(dist, 0.0001);
                    let ndotl = max(dot(n_world, l_dir), 0.0);
                    let atten = 1.0 - (dist / range);
                    accum = accum + light.diffuse_and_range.rgb * (ndotl * atten);
                }
            }
        }
        lit_rgb = in.color.rgb * clamp(accum, vec3<f32>(0.0), vec3<f32>(2.0));
    }

    var out: VsOut;
    out.clip_pos = clip_pos;
    out.color = vec4<f32>(lit_rgb, in.color.a);
    out.uv0 = eval_tex_gen(u.tex_gen_params.x, u.tex_gen_params.y, u.tex_matrix0, view_pos, in.uv0, in.uv1);
    out.view_dist = length(view_pos.xyz);
    out.uv1 = eval_tex_gen(u.tex_gen_params.z, u.tex_gen_params.w, u.tex_matrix1, view_pos, in.uv0, in.uv1);
    out.world_pos = world_pos.xyz;
    out.world_normal = n_world;
    out.raw_color = in.color;
    out.view_z = abs(view_pos.z);
    return out;
}

fn eval_arg(code: u32, diffuse: vec4<f32>, cur: vec4<f32>, tex: vec4<f32>, tfactor: vec4<f32>) -> vec4<f32> {
    let sel = code & 0xFu;
    var v = diffuse;
    if (sel == 1u) {
        v = cur;
    } else if (sel == 2u) {
        v = tex;
    } else if (sel == 3u) {
        v = tfactor;
    }
    if ((code & 0x20u) != 0u) {
        v = vec4<f32>(v.a, v.a, v.a, v.a);
    }
    if ((code & 0x10u) != 0u) {
        v = vec4<f32>(1.0) - v;
    }
    return v;
}

fn eval_rgb_op(op: u32, args: u32, diffuse: vec4<f32>, cur: vec4<f32>, tex: vec4<f32>, tfactor: vec4<f32>) -> vec3<f32> {
    let a1 = eval_arg(args & 0xFFu, diffuse, cur, tex, tfactor);
    let a2 = eval_arg((args >> 8u) & 0xFFu, diffuse, cur, tex, tfactor);
    let a0 = eval_arg((args >> 16u) & 0xFFu, diffuse, cur, tex, tfactor);
    if (op == 1u) { return a1.rgb; }
    if (op == 2u) { return a2.rgb; }
    if (op == 3u) { return a1.rgb * a2.rgb; }
    if (op == 4u) { return clamp(a1.rgb * a2.rgb * 2.0, vec3<f32>(0.0), vec3<f32>(1.0)); }
    if (op == 5u) { return clamp(a1.rgb + a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); }
    if (op == 6u) { return clamp(a1.rgb + a2.rgb - vec3<f32>(0.5), vec3<f32>(0.0), vec3<f32>(1.0)); }
    if (op == 7u) { return clamp(a1.rgb - a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); }
    if (op == 8u) { return mix(a2.rgb, a1.rgb, tex.a); }
    if (op == 9u) { return mix(a2.rgb, a1.rgb, cur.a); }
    if (op == 10u) { return clamp(a0.rgb + a1.rgb * a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); }
    if (op == 11u) {
        let d = clamp(dot(a1.rgb * 2.0 - vec3<f32>(1.0), a2.rgb * 2.0 - vec3<f32>(1.0)), 0.0, 1.0);
        return vec3<f32>(d, d, d);
    }
    if (op == 12u) { return clamp(a1.rgb + a1.a * a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); }
    if (op == 13u) { return clamp(a1.rgb + a2.rgb - a1.rgb * a2.rgb, vec3<f32>(0.0), vec3<f32>(1.0)); }
    if (op == 14u) { return mix(a2.rgb, a1.rgb, diffuse.a); }
    if (op == 15u) { return clamp((a1.rgb + a2.rgb - vec3<f32>(0.5)) * 2.0, vec3<f32>(0.0), vec3<f32>(1.0)); }
    return cur.rgb;
}

fn eval_alpha_op(op: u32, args: u32, diffuse: vec4<f32>, cur: vec4<f32>, tex: vec4<f32>, tfactor: vec4<f32>) -> f32 {
    if (op == 0u) { return cur.a; }
    let a1 = eval_arg(args & 0xFFu, diffuse, cur, tex, tfactor).a;
    let a2 = eval_arg((args >> 8u) & 0xFFu, diffuse, cur, tex, tfactor).a;
    if (op == 1u) { return a1; }
    if (op == 2u) { return a2; }
    if (op == 3u) { return a1 * a2; }
    if (op == 4u) { return clamp(a1 * a2 * 2.0, 0.0, 1.0); }
    if (op == 5u) { return clamp(a1 + a2, 0.0, 1.0); }
    if (op == 6u) { return clamp(a1 + a2 - 0.5, 0.0, 1.0); }
    if (op == 7u) { return clamp(a1 - a2, 0.0, 1.0); }
    if (op == 13u) { return clamp(a1 + a2 - a1 * a2, 0.0, 1.0); }
    if (op == 14u) { return mix(a2, a1, diffuse.a); }
    if (op == 15u) { return clamp((a1 + a2 - 0.5) * 2.0, 0.0, 1.0); }
    return cur.a;
}

fn eval_combiner(diffuse: vec4<f32>, tex0: vec4<f32>, tex1: vec4<f32>) -> vec4<f32> {
    var col = diffuse;
    if (u.stage0_ops.x != 0u) {
        let rgb0 = eval_rgb_op(u.stage0_ops.x, u.stage0_ops.y, diffuse, col, tex0, u.tfactor);
        let a0 = eval_alpha_op(u.stage0_ops.z, u.stage0_ops.w, diffuse, col, tex0, u.tfactor);
        col = vec4<f32>(rgb0, a0);
        if (u.stage1_ops.x != 0u) {
            let rgb1 = eval_rgb_op(u.stage1_ops.x, u.stage1_ops.y, diffuse, col, tex1, u.tfactor);
            let a1 = eval_alpha_op(u.stage1_ops.z, u.stage1_ops.w, diffuse, col, tex1, u.tfactor);
            col = vec4<f32>(rgb1, a1);
        }
    } else if (u.fog_and_shader_params.w > 0.5) {
        col = col * tex0;
    }
    return col;
}

fn hash21(p: vec2<f32>) -> f32 {
    let p3 = fract(vec3<f32>(p.x, p.y, p.x) * 0.1031);
    let d = dot(p3, p3.yzx + vec3<f32>(33.33));
    return fract((p3.x + p3.y) * d);
}

fn value_noise(p: vec2<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (vec2<f32>(3.0) - 2.0 * f);
    let a = hash21(i);
    let b = hash21(i + vec2<f32>(1.0, 0.0));
    let c = hash21(i + vec2<f32>(0.0, 1.0));
    let d = hash21(i + vec2<f32>(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

fn water_fbm(p: vec2<f32>) -> f32 {
    var v = 0.0;
    var amp = 0.52;
    var q = p;
    for (var i: u32 = 0u; i < 3u; i = i + 1u) {
        v = v + amp * value_noise(q);
        q = vec2<f32>(q.x * 1.62 - q.y * 1.18, q.x * 1.18 + q.y * 1.62) + vec2<f32>(13.7, 7.3);
        amp = amp * 0.5;
    }
    return v;
}

fn water_height_field(wp: vec2<f32>, t: f32) -> f32 {
    // Smooth, fluid multi-directional ocean swells + soft wind ripples (liquid modern water, zero grainy noise).
    let p1 = dot(wp, vec2<f32>(0.92, 0.39)) * 0.042 + t * 0.88;
    let p2 = dot(wp, vec2<f32>(-0.48, 0.88)) * 0.064 - t * 1.08;
    let p3 = dot(wp, vec2<f32>(0.34, -0.94)) * 0.095 + t * 1.32;
    let p4 = dot(wp, vec2<f32>(-0.86, -0.51)) * 0.142 - t * 1.62;
    let s1 = sin(p1) * 0.42 + sin(p1 * 2.0 + 0.6) * 0.08;
    let s2 = sin(p2) * 0.30 + sin(p2 * 2.0 - 0.4) * 0.06;
    let s3 = sin(p3) * 0.15;
    let s4 = sin(p4) * 0.07;
    // Broad organic FBM modulation breaks up periodic swells without introducing high-frequency crunch.
    let n1 = water_fbm(wp * 0.036 + vec2<f32>(t * 0.18, t * 0.12));
    let n2 = water_fbm(wp * 0.082 + vec2<f32>(-t * 0.22, t * 0.19));
    return s1 + s2 + s3 + s4 + (n1 - 0.5) * 0.72 + (n2 - 0.5) * 0.30;
}

fn ggx_specular(n: vec3<f32>, v: vec3<f32>, l: vec3<f32>, roughness: f32, f0: vec3<f32>) -> vec3<f32> {
    let h_raw = v + l;
    let h_len = length(h_raw);
    if (h_len < 0.0001) {
        return vec3<f32>(0.0);
    }
    let h = h_raw / h_len;
    let ndoth = max(dot(n, h), 0.0);
    let ndotv = max(dot(n, v), 0.001);
    let ndotl = max(dot(n, l), 0.001);
    let vdoth = max(dot(v, h), 0.0);

    let a = roughness * roughness;
    let a2 = a * a;
    let denom = ndoth * ndoth * (a2 - 1.0) + 1.0;
    let d = a2 / (3.14159265 * denom * denom + 0.0001);

    let k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    let gv = ndotv / (ndotv * (1.0 - k) + k);
    let gl = ndotl / (ndotl * (1.0 - k) + k);
    let g = gv * gl;

    let f = f0 + (vec3<f32>(1.0) - f0) * pow(1.0 - vdoth, 5.0);
    return (d * g * f) / (4.0 * ndotv * ndotl + 0.001);
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let tex0 = textureSample(t_diffuse, s_diffuse, in.uv0);
    let tex1 = textureSample(t_diffuse1, s_diffuse1, in.uv1);
    let backdrop_uv = clamp(in.clip_pos.xy * u.water_params.zw, vec2<f32>(0.002), vec2<f32>(0.998));
    let backdrop_sample = textureSample(t_diffuse1, s_diffuse1, backdrop_uv);

    // Compute screen-space derivatives in uniform control flow before any branches.
    let dpos_dx = dpdx(in.world_pos);
    let dpos_dy = dpdy(in.world_pos);
    let duv_dx = dpdx(in.uv0);
    let duv_dy = dpdy(in.uv0);
    let tex_luma = dot(tex0.rgb, vec3<f32>(0.299, 0.587, 0.114));
    let dl_dx = dpdx(tex_luma);
    let dl_dy = dpdy(tex_luma);

    let pbr_mode = u.camera_pos_and_pbr.w;
    let pbr_active = (pbr_mode > 0.5 && pbr_mode < 1.5) || (pbr_mode >= 1.5 && in.clip_pos.x >= u.render_flags.z);

    var col = eval_combiner(in.color, tex0, tex1);

    let alpha_ref = u.fog_and_shader_params.z;
    if (alpha_ref > 0.0 && col.a < alpha_ref) {
        discard;
    }

    if (u.water_params.y > 1.5) {
        // Modern Vulkan 1.3 Frosted-Glass UI Backdrop (respects F9 split-screen wipe).
        let glass_active = (u.water_params.y < 2.5) || (in.clip_pos.x >= u.render_flags.z);
        if (glass_active) {
            let blurred = backdrop_sample.rgb;
            let blur_luma = dot(blurred, vec3<f32>(0.299, 0.587, 0.114));
            let frosted_bg = mix(vec3<f32>(blur_luma), blurred, 0.62) * 0.54;
            let screen_y = clamp(in.clip_pos.y * u.water_params.w, 0.0, 1.0);
            let sheen = (1.0 - screen_y) * 0.045;
            let glass_rgb = mix(frosted_bg, col.rgb, clamp(col.a, 0.48, 0.86))
                + vec3<f32>(sheen * 0.55, sheen * 0.82, sheen * 1.15);
            col = vec4<f32>(glass_rgb, 0.96);
        }
    } else if (pbr_active && u.water_params.y > 0.5) {
        // Next-Gen Vulkan 1.3 Modern Coastal & Oceanic Water:
        // - True per-pixel depth & screen-space seabed/rock refraction from pre-water HDR snapshot
        // - Smooth fluid multi-directional ocean swells + soft sunlit capillary sheen
        // - Optical Beer-Lambert depth absorption (crystal turquoise shallows -> deep oceanic blue)
        // - Soft sunlit underwater caustics in coastal shallows + wave-crest subsurface scattering (SSS)
        // - Organic broken coastal rock/island foam + gentle lapping beach surf waves
        // - Realistic atmospheric sky/cloud Fresnel reflections + crisp HDR GGX sun glints
        let t = u.water_params.x;
        let wp = in.world_pos.xy;

        let to_cam = u.camera_pos_and_pbr.xyz - in.world_pos;
        let cam_dist = length(to_cam);
        let v_dir = normalize(select(vec3<f32>(0.0, 0.0, 1.0), to_cam, cam_dist > 0.0001));

        // Central-difference fluid wave normals with distance-safe attenuation.
        let detail_fade = clamp(1.0 - (cam_dist - 160.0) / 1100.0, 0.48, 1.0);
        let eps = 0.85;
        let h_c = water_height_field(wp, t);
        let h_px = water_height_field(wp + vec2<f32>(eps, 0.0), t);
        let h_mx = water_height_field(wp - vec2<f32>(eps, 0.0), t);
        let h_py = water_height_field(wp + vec2<f32>(0.0, eps), t);
        let h_my = water_height_field(wp - vec2<f32>(0.0, eps), t);
        let normal_strength = 0.36 * detail_fade;
        let dh_dx = (h_px - h_mx) / (2.0 * eps) * normal_strength;
        let dh_dy = (h_py - h_my) / (2.0 * eps) * normal_strength;
        let n_water = normalize(vec3<f32>(-dh_dx, -dh_dy, 1.0));
        let ndotv = max(dot(n_water, v_dir), 0.04);

        var sun_dir = normalize(vec3<f32>(0.42, 0.28, 0.86));
        var sun_col = vec3<f32>(1.0, 0.96, 0.88);
        if (u.lights[0].pos_or_dir_and_kind.w > 0.5 && u.lights[0].pos_or_dir_and_kind.w < 1.5) {
            sun_dir = normalize(-u.lights[0].pos_or_dir_and_kind.xyz);
            sun_col = max(u.lights[0].diffuse_and_range.rgb, vec3<f32>(0.80));
        }

        // True per-pixel water depth & screen-space seabed refraction from pre-water snapshot.
        let has_snap = u.water_params.z > 0.0 && backdrop_sample.a > 0.01;
        let raw_ground_z = select(in.view_z + 28.0, backdrop_sample.a, has_snap);
        let raw_ray_depth = max(raw_ground_z - in.view_z, 0.0);
        let raw_water_depth = min(raw_ray_depth * 0.72, 60.0);

        // Smooth refracted UV with depth-guarded dry-rock rejection.
        let refract_scale = 0.011 * smoothstep(0.6, 5.5, raw_water_depth) * detail_fade;
        let refract_uv = clamp(backdrop_uv + n_water.xy * refract_scale, vec2<f32>(0.002), vec2<f32>(0.998));
        let refract_sample = textureSampleLevel(t_diffuse1, s_diffuse1, refract_uv, 0.0);
        let refract_valid = has_snap && (refract_sample.a > in.view_z + 0.85);
        let seabed_rgb = select(backdrop_sample.rgb, refract_sample.rgb, refract_valid);
        let eff_ground_z = select(raw_ground_z, refract_sample.a, refract_valid);
        let water_depth = min(max(eff_ground_z - in.view_z, 0.0) * 0.72, 60.0);

        // Optical Beer-Lambert absorption: crystal coastal turquoise -> rich cerulean -> deep oceanic sapphire.
        let transmittance = exp(-water_depth * vec3<f32>(0.22, 0.095, 0.052));
        let shallow_mix = clamp(1.0 - exp(-water_depth * 0.13), 0.0, 1.0);
        let deep_mix = clamp(1.0 - exp(-water_depth * 0.042), 0.0, 1.0);
        let shallow_scatter = vec3<f32>(0.075, 0.39, 0.44);
        let mid_scatter = vec3<f32>(0.028, 0.19, 0.32);
        let deep_scatter = vec3<f32>(0.014, 0.078, 0.165);
        let volume_col = mix(mix(shallow_scatter, mid_scatter, shallow_mix), deep_scatter, deep_mix);

        // Soft, dappled sunlit caustics ONLY in translucent shallow water over seabed/submerged rocks.
        let c_a = value_noise(wp * 0.14 + n_water.xy * 1.4 + vec2<f32>(t * 0.28, -t * 0.18));
        let c_b = value_noise(wp * 0.19 - n_water.xy * 1.2 + vec2<f32>(-t * 0.21, t * 0.25));
        let caustic_pool = smoothstep(0.32, 0.85, c_a * c_b * 2.2);
        let caustics = caustic_pool
            * smoothstep(7.5, 0.7, water_depth)
            * smoothstep(0.08, 0.65, water_depth);

        // Blend refracted underwater seabed + dappled caustics with Beer-Lambert volume scattering.
        let lit_seabed = seabed_rgb * transmittance
            + vec3<f32>(0.26, 0.56, 0.58) * sun_col * caustics * 0.36;
        let seabed_vis = clamp(transmittance.g * 0.86, 0.0, 0.86);
        let underwater = select(
            volume_col,
            mix(volume_col, lit_seabed, seabed_vis),
            has_snap
        );

        // Backlit emerald-aqua subsurface scattering through elevated ocean swell crests.
        let ndotl = max(dot(n_water, sun_dir), 0.0);
        let crest_height = clamp(h_c * 0.65 + 0.40, 0.0, 1.0);
        let sss_view = pow(clamp(dot(v_dir, -sun_dir + n_water * 0.52), 0.0, 1.0), 2.4);
        let sss_amount = (crest_height * 0.48 + sss_view * 0.62) * (0.35 + 0.65 * ndotl);
        let sss_col = vec3<f32>(0.05, 0.44, 0.49) * sss_amount * 0.32;

        // Physical Schlick Fresnel + luminous sky gradient & soft drifting reflected clouds.
        let fresnel = 0.032 + 0.968 * pow(1.0 - ndotv, 4.2);
        let refl_dir = reflect(-v_dir, n_water);
        let sky_t = clamp(refl_dir.z * 0.78 + 0.16, 0.0, 1.0);
        let sky_horizon = vec3<f32>(0.58, 0.76, 0.88);
        let sky_zenith = vec3<f32>(0.10, 0.27, 0.50);
        let cloud_uv = wp * 0.0055 + refl_dir.xy * 1.4 + vec2<f32>(t * 0.014, t * 0.007);
        let cloud_mask = smoothstep(0.45, 0.78, water_fbm(cloud_uv));
        let sky_refl = mix(mix(sky_horizon, sky_zenith, sky_t), vec3<f32>(0.92, 0.96, 1.0), cloud_mask * 0.38);

        // Dual-lobe GGX sun reflections: crisp HDR specular sparkles + glossy liquid sun sheen.
        let spec_sparkle = ggx_specular(n_water, v_dir, sun_dir, 0.042, vec3<f32>(0.065));
        let spec_broad = ggx_specular(n_water, v_dir, sun_dir, 0.20, vec3<f32>(0.035));
        let water_spec = sun_col * (spec_sparkle * 0.95 + spec_broad * 0.42) * ndotl;

        // Organic frothy shoreline foam around rocks/islands + soft breaking coastal surf waves.
        let foam_noise = water_fbm(wp * 0.19 + vec2<f32>(t * 0.12, -t * 0.09));
        let foam_detail = value_noise(wp * 0.52 - vec2<f32>(t * 0.18, t * 0.14));
        let foam_tex = foam_noise * 0.65 + foam_detail * 0.35;

        // Soft, broken organic contact foam collar hugging rocks, hulls, and beaches.
        let contact_band = smoothstep(2.1, 0.08, raw_water_depth);
        let contact_foam = smoothstep(0.42, 0.78, contact_band * 1.02 + (foam_tex - 0.5) * 0.88) * contact_band;

        // Subtle organic surf ripples in coastal shallows (Zero Hour's WaterTrackSystem draws main beach breakers).
        let surf_zone = smoothstep(5.5, 0.8, raw_water_depth) * smoothstep(0.35, 1.4, raw_water_depth);
        let surf_wave = sin(raw_water_depth * 1.15 - t * 1.7 + foam_noise * 4.2);
        let surf_rings = smoothstep(0.62, 0.96, surf_wave) * smoothstep(0.40, 0.76, foam_tex) * surf_zone * 0.28;

        // Subtle wind-swept whitecaps on the tallest deep-water swell peaks.
        let whitecap = smoothstep(0.84, 1.16, h_c + foam_tex * 0.34) * smoothstep(4.5, 14.0, raw_water_depth) * 0.16;

        let total_foam = clamp(contact_foam * 0.64 + surf_rings + whitecap, 0.0, 0.92);
        let foam_col = mix(vec3<f32>(0.80, 0.90, 0.95), vec3<f32>(0.94, 0.97, 0.99), 0.65) * (0.76 + 0.24 * ndotl);

        let water_body = underwater + sss_col;
        let reflected_water = mix(water_body, sky_refl, clamp(fresnel * 0.68, 0.10, 0.76)) + water_spec;
        let surfaced_water = mix(reflected_water, foam_col, total_foam);

        // Soft wet shoreline edge feathering eliminates hard polygon cuts around every rock & island.
        let shore_feather = select(1.0, smoothstep(0.0, 0.95, raw_water_depth), has_snap);
        let wet_shore = backdrop_sample.rgb * 0.86;
        let final_water = select(surfaced_water, mix(wet_shore, surfaced_water, shore_feather), has_snap);
        let final_alpha = select(
            clamp(col.a + fresnel * 0.25 + total_foam * 0.45, 0.55, 0.98),
            clamp(max(shore_feather, total_foam * 0.68), 0.0, 1.0),
            has_snap
        );
        col = vec4<f32>(final_water, final_alpha);
    } else if (pbr_active && u.render_flags.x > 0.5) {
        // 3D lit geometry: per-pixel Parallax Occlusion Mapping (POM) + multi-tap micro-relief normal
        // + micro-cavity self-shadowing + Cook-Torrance GGX specular + hemisphere sky/ground IBL.
        let n_geom = normalize(in.world_normal);
        let to_cam = u.camera_pos_and_pbr.xyz - in.world_pos;
        let cam_dist = length(to_cam);
        let v_dir = normalize(select(vec3<f32>(0.0, 0.0, 1.0), to_cam, cam_dist > 0.0001));

        // Construct cotangent frame from screen-space derivatives for zero-attribute POM.
        var pom_uv = in.uv0;
        var pom_tex0 = tex0;
        var pom_cavity = 1.0;
        var pom_normal = n_geom;
        var pom_grad_mag = 0.0;
        var used_pom_normal = false;

        let uv_det = duv_dx.x * duv_dy.y - duv_dx.y * duv_dy.x;
        let pom_fade = smoothstep(420.0, 110.0, cam_dist);
        if (u.fog_and_shader_params.w > 0.5 && alpha_ref <= 0.0 && pom_fade > 0.02 && abs(uv_det) > 0.0000001) {
            let inv_uv_det = 1.0 / uv_det;
            let t_raw = (dpos_dx * duv_dy.y - dpos_dy * duv_dx.y) * inv_uv_det;
            let b_raw = (dpos_dy * duv_dx.x - dpos_dx * duv_dy.x) * inv_uv_det;
            // Gram-Schmidt orthogonalize T and B against n_geom.
            let t_ortho = t_raw - n_geom * dot(t_raw, n_geom);
            let b_ortho = b_raw - n_geom * dot(b_raw, n_geom);
            let t_len = length(t_ortho);
            let b_len = length(b_ortho);
            if (t_len > 0.0001 && b_len > 0.0001) {
                let t_vec = t_ortho / t_len;
                let b_vec = b_ortho / b_len;
                let vz = max(dot(v_dir, n_geom), 0.28);
                let v_tan = vec2<f32>(dot(v_dir, t_vec), dot(v_dir, b_vec)) / vz;
                // Keep parallax shift physically bounded in UV space (zero swimming on atlases).
                let parallax_amp = 0.0075 * pom_fade * smoothstep(0.18, 0.55, vz);
                let max_uv_shift = clamp(v_tan * parallax_amp, vec2<f32>(-0.011), vec2<f32>(0.011));

                // Fast 2-step Parallax Occlusion march + linear intersection refinement (2x fewer texture taps at 4K).
                let step_uv = max_uv_shift * 0.5;
                var cur_uv = in.uv0;
                var prev_uv = in.uv0;
                var cur_layer = 0.0;
                var prev_diff = 0.0;
                var cur_h = dot(tex0.rgb, vec3<f32>(0.299, 0.587, 0.114));
                for (var s: u32 = 0u; s < 2u; s = s + 1u) {
                    if (cur_layer >= (1.0 - cur_h)) {
                        break;
                    }
                    prev_uv = cur_uv;
                    prev_diff = (1.0 - cur_h) - cur_layer;
                    cur_uv = cur_uv - step_uv;
                    cur_layer = cur_layer + 0.5;
                    let s_rgb = textureSampleGrad(t_diffuse, s_diffuse, cur_uv, duv_dx, duv_dy).rgb;
                    cur_h = dot(s_rgb, vec3<f32>(0.299, 0.587, 0.114));
                }
                let after_diff = cur_layer - (1.0 - cur_h);
                let denom_p = max(after_diff + prev_diff, 0.0001);
                let weight = clamp(after_diff / denom_p, 0.0, 1.0);
                pom_uv = mix(cur_uv, prev_uv, weight);
                pom_tex0 = textureSampleGrad(t_diffuse, s_diffuse, pom_uv, duv_dx, duv_dy);

                // 2-tap forward-difference relief normal reusing center sample `h_c` (same crisp detail, half the fetches).
                let texel_step = clamp(max(length(duv_dx), length(duv_dy)) * 1.15, 0.0012, 0.010);
                let h_c = dot(pom_tex0.rgb, vec3<f32>(0.299, 0.587, 0.114));
                let h_r = dot(textureSampleGrad(t_diffuse, s_diffuse, pom_uv + vec2<f32>(texel_step, 0.0), duv_dx, duv_dy).rgb, vec3<f32>(0.299, 0.587, 0.114));
                let h_u = dot(textureSampleGrad(t_diffuse, s_diffuse, pom_uv + vec2<f32>(0.0, texel_step), duv_dx, duv_dy).rgb, vec3<f32>(0.299, 0.587, 0.114));
                let dh_x = (h_r - h_c) * 1.85;
                let dh_y = (h_u - h_c) * 1.85;
                let relief_str = 0.92 * pom_fade;
                let gx = clamp(dh_x * relief_str, -0.52, 0.52);
                let gy = clamp(dh_y * relief_str, -0.52, 0.52);
                pom_grad_mag = length(vec2<f32>(dh_x, dh_y));
                pom_normal = normalize(n_geom - t_vec * gx - b_vec * gy);
                used_pom_normal = true;
                let local_avg = 0.5 * (h_r + h_u);
                pom_cavity = clamp(1.0 + (h_c - local_avg) * 0.50 * pom_fade, 0.88, 1.015);
            }
        }

        let albedo = eval_combiner(in.raw_color, pom_tex0, tex1);

        // Fallback to Mikkelsen screen-space derivative bump normal at far distances or untextured draws.
        var n_bump = pom_normal;
        if (!used_pom_normal) {
            let r1 = cross(dpos_dy, n_geom);
            let r2 = cross(n_geom, dpos_dx);
            let det = dot(dpos_dx, r1);
            if (abs(det) > 0.00001) {
                let surf_grad = sign(det) * (dl_dx * r1 + dl_dy * r2);
                let grad_len = length(surf_grad);
                let clamped_grad = surf_grad / max(1.0, grad_len * 0.45);
                n_bump = normalize(abs(det) * n_geom - 0.26 * clamped_grad);
            }
        }

        let ndotv = max(dot(n_bump, v_dir), 0.001);

        // Estimate military material roughness/F0 from albedo + micro-roughness:
        // High-frequency textured stone/granite/masonry stays matte dielectric (zero glowing rock highlights),
        // while smooth painted steel armor/turrets/barrels retain crisp metallic GGX specular.
        let max_c = max(albedo.r, max(albedo.g, albedo.b));
        let min_c = min(albedo.r, min(albedo.g, albedo.b));
        let sat = (max_c - min_c) / max(max_c, 0.001);
        let stone_roughness_mask = smoothstep(0.06, 0.26, pom_grad_mag);
        let metallic_hint = clamp((1.0 - sat * 1.4) * smoothstep(0.22, 0.75, max_c), 0.0, 0.45)
            * (1.0 - stone_roughness_mask * 0.92);
        let roughness = clamp(0.70 - metallic_hint * 0.34 + stone_roughness_mask * 0.20, 0.34, 0.92);
        let f0 = mix(vec3<f32>(0.032), albedo.rgb * 0.28 + vec3<f32>(0.05), metallic_hint);

        // Hemisphere sky/ground IBL (Zero Hour uses +Z up).
        let up_factor = clamp(n_bump.z * 0.5 + 0.5, 0.0, 1.0);
        let sky_amb = u.ambient_and_lighting.rgb * vec3<f32>(0.90, 1.01, 1.14);
        let ground_amb = u.ambient_and_lighting.rgb * vec3<f32>(1.02, 0.91, 0.80);
        var diff_accum = mix(ground_amb, sky_amb, up_factor);
        var spec_accum = vec3<f32>(0.0);

        for (var i: u32 = 0u; i < 4u; i = i + 1u) {
            let light = u.lights[i];
            let kind = light.pos_or_dir_and_kind.w;
            if (kind > 0.5 && kind < 1.5) {
                let l_dir = normalize(-light.pos_or_dir_and_kind.xyz);
                let ndotl = max(dot(n_bump, l_dir), 0.0);
                let wrapped = clamp((dot(n_bump, l_dir) + 0.10) / 1.10, 0.0, 1.0);
                diff_accum = diff_accum + light.diffuse_and_range.rgb * mix(ndotl, wrapped, 0.22);
                if (ndotl > 0.0) {
                    let spec = ggx_specular(n_bump, v_dir, l_dir, roughness, f0);
                    spec_accum = spec_accum + light.diffuse_and_range.rgb * spec * ndotl * 0.62;
                }
            } else if (kind >= 1.5) {
                let to_light = light.pos_or_dir_and_kind.xyz - in.world_pos;
                let dist = length(to_light);
                let range = max(light.diffuse_and_range.w, 0.001);
                if (dist < range) {
                    let l_dir = to_light / max(dist, 0.0001);
                    let ndotl = max(dot(n_bump, l_dir), 0.0);
                    let atten = (1.0 - dist / range);
                    let atten_smooth = atten * atten;
                    diff_accum = diff_accum + light.diffuse_and_range.rgb * (ndotl * atten);
                    if (ndotl > 0.0) {
                        let spec = ggx_specular(n_bump, v_dir, l_dir, roughness, f0);
                        spec_accum = spec_accum + light.diffuse_and_range.rgb * spec * (ndotl * atten_smooth * 0.85);
                    }
                }
            }
        }

        // Subtle grazing sky rim (stronger on smooth metallic units, minimal on rough matte stone).
        let fresnel_rim = pow(1.0 - ndotv, 4.2) * (0.025 + 0.095 * metallic_hint) * (0.4 + 0.6 * up_factor);
        let pbr_rgb = albedo.rgb * pom_cavity * clamp(diff_accum, vec3<f32>(0.0), vec3<f32>(1.45))
            + min(spec_accum * pom_cavity, vec3<f32>(0.55))
            + sky_amb * fresnel_rim;
        col = vec4<f32>(pbr_rgb, col.a);
    } else if (pbr_active && u.render_flags.w > 0.5) {
        // 3D unlit opaque terrain/ground: atlas-safe Steep Parallax Occlusion Mapping (POM)
        // + 4-tap finite-difference micro-relief normal aligned to the smooth bicubic terrain normal
        // + micro-cavity self-shadowing (matte dielectric, zero blooming sand/stone).
        let n_geom = normalize(in.world_normal);
        let to_cam = u.camera_pos_and_pbr.xyz - in.world_pos;
        let cam_dist = length(to_cam);
        let v_dir = normalize(select(vec3<f32>(0.0, 0.0, 1.0), to_cam, cam_dist > 0.0001));

        var sun_dir = normalize(vec3<f32>(0.42, 0.28, 0.86));
        if (u.lights[0].pos_or_dir_and_kind.w > 0.5 && u.lights[0].pos_or_dir_and_kind.w < 1.5) {
            sun_dir = normalize(-u.lights[0].pos_or_dir_and_kind.xyz);
        }

        var t_col = col.rgb;
        var t_cavity = 1.0;
        var n_bump = n_geom;
        var used_terrain_pom = false;

        let uv_det = duv_dx.x * duv_dy.y - duv_dx.y * duv_dy.x;
        let t_pom_fade = smoothstep(380.0, 110.0, cam_dist);
        if (u.fog_and_shader_params.w > 0.5 && t_pom_fade > 0.02 && abs(uv_det) > 0.0000001) {
            let inv_uv_det = 1.0 / uv_det;
            let t_raw = (dpos_dx * duv_dy.y - dpos_dy * duv_dx.y) * inv_uv_det;
            let b_raw = (dpos_dy * duv_dx.x - dpos_dx * duv_dy.x) * inv_uv_det;
            let t_ortho = t_raw - n_geom * dot(t_raw, n_geom);
            let b_ortho = b_raw - n_geom * dot(b_raw, n_geom);
            let t_len = length(t_ortho);
            let b_len = length(b_ortho);
            if (t_len > 0.0001 && b_len > 0.0001) {
                let t_vec = t_ortho / t_len;
                let b_vec = b_ortho / b_len;
                let vz = max(dot(v_dir, n_geom), 0.32);
                let v_tan = vec2<f32>(dot(v_dir, t_vec), dot(v_dir, b_vec)) / vz;
                // Strictly bounded single-step height-weighted parallax keeps POM inside 64x64 atlas cells
                // while cutting terrain texture fetches by 3x at 4K (3 extra taps vs 9).
                let parallax_amp = 0.0022 * t_pom_fade * smoothstep(0.20, 0.58, vz);
                let max_uv_shift = clamp(v_tan * parallax_amp, vec2<f32>(-0.0020), vec2<f32>(0.0020));
                let h0 = dot(tex0.rgb, vec3<f32>(0.299, 0.587, 0.114));
                let pom_uv = in.uv0 - max_uv_shift * (1.0 - h0);
                let pom_tex0 = textureSampleGrad(t_diffuse, s_diffuse, pom_uv, duv_dx, duv_dy);
                t_col = eval_combiner(in.raw_color, pom_tex0, tex1).rgb;

                // Atlas-safe 2-tap forward-difference relief normal reusing center `h_c` + micro-cavity AO.
                let texel_step = clamp(max(length(duv_dx), length(duv_dy)) * 1.15, 0.0008, 0.0032);
                let h_c = dot(pom_tex0.rgb, vec3<f32>(0.299, 0.587, 0.114));
                let h_r = dot(textureSampleGrad(t_diffuse, s_diffuse, pom_uv + vec2<f32>(texel_step, 0.0), duv_dx, duv_dy).rgb, vec3<f32>(0.299, 0.587, 0.114));
                let h_u = dot(textureSampleGrad(t_diffuse, s_diffuse, pom_uv + vec2<f32>(0.0, texel_step), duv_dx, duv_dy).rgb, vec3<f32>(0.299, 0.587, 0.114));
                let relief_str = 1.55 * t_pom_fade;
                let gx = clamp((h_r - h_c) * relief_str, -0.46, 0.46);
                let gy = clamp((h_u - h_c) * relief_str, -0.46, 0.46);
                n_bump = normalize(n_geom - t_vec * gx - b_vec * gy);
                used_terrain_pom = true;
                let local_avg = 0.5 * (h_r + h_u);
                t_cavity = clamp(1.0 + (h_c - local_avg) * 0.52 * t_pom_fade, 0.89, 1.02);
            }
        }

        if (!used_terrain_pom) {
            let r1 = cross(dpos_dy, n_geom);
            let r2 = cross(n_geom, dpos_dx);
            let det = dot(dpos_dx, r1);
            if (abs(det) > 0.00001) {
                let surf_grad = sign(det) * (dl_dx * r1 + dl_dy * r2);
                let grad_len = length(surf_grad);
                n_bump = normalize(abs(det) * n_geom - 0.18 * (surf_grad / max(1.0, grad_len * 0.5)));
            }
        }
        let relief = clamp(dot(n_bump, sun_dir) - dot(n_geom, sun_dir), -0.22, 0.14);
        col = vec4<f32>(t_col * t_cavity * (1.0 + relief * 0.45), col.a);
    } else if (pbr_active && u.render_flags.y > 0.5) {
        // 3D additive VFX (explosions, fireballs, lasers, muzzle flashes): push bright core into HDR for soft bloom.
        let luma = max(col.r, max(col.g, col.b));
        let hdr_boost = 1.0 + 0.95 * smoothstep(0.32, 0.95, luma);
        col = vec4<f32>(col.rgb * hdr_boost, col.a);
    }

    if (u.fog_color_and_enable.w > 0.5) {
        let fog_start = u.fog_and_shader_params.x;
        let fog_end = max(u.fog_and_shader_params.y, fog_start + 0.001);
        let fog_factor = clamp((in.view_dist - fog_start) / (fog_end - fog_start), 0.0, 1.0);
        col = vec4<f32>(mix(col.rgb, u.fog_color_and_enable.rgb, fog_factor), col.a);
    }

    // Clamp Classic-side pixels to [0..1] just like D3D8 UNORM render targets so
    // Split-Screen HDR (`R16G16B16A16_SFLOAT`) never overflows negative/signed combiner values.
    if (!pbr_active) {
        col = vec4<f32>(clamp(col.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), clamp(col.a, 0.0, 1.0));
    }

    return col;
}
"#;

/// Compiles a WGSL source string to Vulkan 1.3 SPIR-V words for a specific entry point.
pub fn compile_wgsl_to_spirv(
    wgsl: &str,
    stage: naga::ShaderStage,
    entry_point: &str,
) -> Result<Vec<u32>> {
    let module = naga::front::wgsl::parse_str(wgsl)
        .map_err(|e| VkError::ShaderCompile(e.emit_to_string(wgsl)))?;

    let mut validator = naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    );
    let info = validator
        .validate(&module)
        .map_err(|e| VkError::ShaderCompile(e.emit_to_string(wgsl)))?;

    let options = naga::back::spv::Options {
        lang_version: (1, 3),
        flags: naga::back::spv::WriterFlags::empty(),
        ..Default::default()
    };
    let pipeline_opts = naga::back::spv::PipelineOptions {
        shader_stage: stage,
        entry_point: entry_point.to_string(),
    };

    naga::back::spv::write_vec(&module, &info, &options, Some(&pipeline_opts))
        .map_err(|e| VkError::ShaderCompile(e.to_string()))
}

pub fn create_shader_module(device: &ash::Device, spirv: &[u32]) -> Result<vk::ShaderModule> {
    let info = vk::ShaderModuleCreateInfo::default().code(spirv);
    // SAFETY: `device` is a live Vulkan logical device and `spirv` is valid aligned u32 SPIR-V.
    let module = unsafe { device.create_shader_module(&info, None)? };
    Ok(module)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shader_key_roundtrip_matches_ww3d2_bits() {
        let key = ShaderKey::ALPHA_BLEND;
        assert_eq!(key.depth_compare(), DepthCompare::LessEqual);
        assert!(!key.depth_write());
        assert!(key.color_write());
        assert_eq!(key.src_blend(), SrcBlend::SrcAlpha);
        assert_eq!(key.dst_blend(), DstBlend::OneMinusSrcAlpha);
        assert!(key.blend_enabled());
        assert!(key.texturing_enabled());
        assert!(!key.alpha_test_enabled());
        assert!(!key.cull_enabled());
    }

    #[test]
    fn wgsl_uber_shader_compiles_to_spirv() {
        let vs = compile_wgsl_to_spirv(WW3D2_UBER_WGSL, naga::ShaderStage::Vertex, "vs_main")
            .expect("vertex shader must compile");
        let fs = compile_wgsl_to_spirv(WW3D2_UBER_WGSL, naga::ShaderStage::Fragment, "fs_main")
            .expect("fragment shader must compile");
        // SPIR-V magic number: 0x07230203
        assert_eq!(vs.first().copied(), Some(0x0723_0203));
        assert_eq!(fs.first().copied(), Some(0x0723_0203));
    }
}
