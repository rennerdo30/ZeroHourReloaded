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
};

@group(0) @binding(0) var<uniform> u: DrawUniforms;
@group(1) @binding(0) var t_diffuse: texture_2d<f32>;
@group(1) @binding(1) var s_diffuse: sampler;

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
};

@vertex
fn vs_main(in: VsIn) -> VsOut {
    let world_pos = u.world * vec4<f32>(in.position, 1.0);
    let view_pos = u.view * world_pos;
    let clip_pos = u.projection * view_pos;

    var lit_rgb = in.color.rgb;
    if (u.ambient_and_lighting.w > 0.5) {
        let n_world = normalize((u.world * vec4<f32>(in.normal, 0.0)).xyz);
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
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    var col = in.color;
    if (u.fog_and_shader_params.w > 0.5) {
        let tex = textureSample(t_diffuse, s_diffuse, in.uv0);
        col = col * tex;
    }

    let alpha_ref = u.fog_and_shader_params.z;
    if (alpha_ref > 0.0 && col.a < alpha_ref) {
        discard;
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
