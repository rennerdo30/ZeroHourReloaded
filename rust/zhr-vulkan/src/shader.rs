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
};

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
    out.uv0 = in.uv0;
    out.view_dist = length(view_pos.xyz);
    out.uv1 = in.uv1;
    out.world_pos = world_pos.xyz;
    out.world_normal = n_world;
    out.raw_color = in.color;
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

    // Compute screen-space derivatives in uniform control flow before any branches.
    let dpos_dx = dpdx(in.world_pos);
    let dpos_dy = dpdy(in.world_pos);
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

    if (pbr_active && u.render_flags.x > 0.5) {
        // 3D lit geometry: per-pixel micro-normal + Cook-Torrance GGX specular + hemisphere sky/ground IBL.
        let albedo = eval_combiner(in.raw_color, tex0, tex1);
        let n_geom = normalize(in.world_normal);

        // Derivative-based procedural micro-normal relief (Mikkelsen unparametrized bump mapping).
        let r1 = cross(dpos_dy, n_geom);
        let r2 = cross(n_geom, dpos_dx);
        let det = dot(dpos_dx, r1);
        var n_bump = n_geom;
        if (abs(det) > 0.00001) {
            let surf_grad = sign(det) * (dl_dx * r1 + dl_dy * r2);
            let grad_len = length(surf_grad);
            let clamped_grad = surf_grad / max(1.0, grad_len * 0.45);
            n_bump = normalize(abs(det) * n_geom - 0.34 * clamped_grad);
        }

        let to_cam = u.camera_pos_and_pbr.xyz - in.world_pos;
        let v_dir = normalize(select(vec3<f32>(0.0, 0.0, 1.0), to_cam, length(to_cam) > 0.0001));
        let ndotv = max(dot(n_bump, v_dir), 0.001);

        // Estimate military material roughness/F0 from albedo (painted steel/armor vs matte sand/cloth).
        let max_c = max(albedo.r, max(albedo.g, albedo.b));
        let min_c = min(albedo.r, min(albedo.g, albedo.b));
        let sat = (max_c - min_c) / max(max_c, 0.001);
        let metallic_hint = clamp((1.0 - sat * 1.4) * smoothstep(0.18, 0.72, max_c), 0.0, 0.55);
        let roughness = clamp(0.68 - metallic_hint * 0.36, 0.28, 0.80);
        let f0 = mix(vec3<f32>(0.045), albedo.rgb * 0.35 + vec3<f32>(0.06), metallic_hint);

        // Hemisphere sky/ground IBL (Zero Hour uses +Z up).
        let up_factor = clamp(n_bump.z * 0.5 + 0.5, 0.0, 1.0);
        let sky_amb = u.ambient_and_lighting.rgb * vec3<f32>(0.90, 1.02, 1.18);
        let ground_amb = u.ambient_and_lighting.rgb * vec3<f32>(1.04, 0.90, 0.78);
        var diff_accum = mix(ground_amb, sky_amb, up_factor);
        var spec_accum = vec3<f32>(0.0);

        for (var i: u32 = 0u; i < 4u; i = i + 1u) {
            let light = u.lights[i];
            let kind = light.pos_or_dir_and_kind.w;
            if (kind > 0.5 && kind < 1.5) {
                let l_dir = normalize(-light.pos_or_dir_and_kind.xyz);
                let ndotl = max(dot(n_bump, l_dir), 0.0);
                let wrapped = clamp((dot(n_bump, l_dir) + 0.10) / 1.10, 0.0, 1.0);
                diff_accum = diff_accum + light.diffuse_and_range.rgb * mix(ndotl, wrapped, 0.25);
                if (ndotl > 0.0) {
                    let spec = ggx_specular(n_bump, v_dir, l_dir, roughness, f0);
                    spec_accum = spec_accum + light.diffuse_and_range.rgb * spec * ndotl * 1.15;
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
                        spec_accum = spec_accum + light.diffuse_and_range.rgb * spec * (ndotl * atten_smooth * 1.4);
                    }
                }
            }
        }

        // Crisp grazing sky rim for strong unit/building silhouette separation.
        let fresnel_rim = pow(1.0 - ndotv, 4.0) * (0.12 + 0.14 * metallic_hint) * (0.4 + 0.6 * up_factor);
        let pbr_rgb = albedo.rgb * clamp(diff_accum, vec3<f32>(0.0), vec3<f32>(2.1))
            + spec_accum
            + sky_amb * fresnel_rim;
        col = vec4<f32>(pbr_rgb, col.a);
    } else if (pbr_active && u.render_flags.w > 0.5) {
        // 3D unlit opaque terrain/ground: subtle micro-relief lighting + grazing sheen.
        let n_geom = normalize(in.world_normal);
        let r1 = cross(dpos_dy, n_geom);
        let r2 = cross(n_geom, dpos_dx);
        let det = dot(dpos_dx, r1);
        var relief = 0.0;
        if (abs(det) > 0.00001) {
            let surf_grad = sign(det) * (dl_dx * r1 + dl_dy * r2);
            let grad_len = length(surf_grad);
            let n_bump = normalize(abs(det) * n_geom - 0.24 * (surf_grad / max(1.0, grad_len * 0.5)));
            var sun_dir = normalize(vec3<f32>(0.42, 0.28, 0.86));
            if (u.lights[0].pos_or_dir_and_kind.w > 0.5 && u.lights[0].pos_or_dir_and_kind.w < 1.5) {
                sun_dir = normalize(-u.lights[0].pos_or_dir_and_kind.xyz);
            }
            relief = clamp(dot(n_bump, sun_dir) - dot(n_geom, sun_dir), -0.25, 0.25);
        }
        col = vec4<f32>(col.rgb * (1.0 + relief * 0.65), col.a);
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
