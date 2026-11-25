use bytemuck::{Pod, Zeroable};
use glam::{Mat4, Vec3, Vec4};

/// Standard WW3D2-compatible vertex covering 3D lit meshes, terrain, particles, and 2D UI.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Pod, Zeroable)]
pub struct VertexWw3d {
    pub position: [f32; 3],
    pub normal: [f32; 3],
    pub color: [f32; 4],
    pub uv0: [f32; 2],
    pub uv1: [f32; 2],
}

impl VertexWw3d {
    pub const fn new(position: [f32; 3], normal: [f32; 3], color: [f32; 4], uv0: [f32; 2]) -> Self {
        Self {
            position,
            normal,
            color,
            uv0,
            uv1: [0.0, 0.0],
        }
    }

    pub const fn colored(position: [f32; 3], color: [f32; 4]) -> Self {
        Self::new(position, [0.0, 0.0, 1.0], color, [0.0, 0.0])
    }
}

/// Viewport rectangle (matches `WW3D2::RenderViewport`).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Viewport {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub min_z: f32,
    pub max_z: f32,
}

impl Viewport {
    pub const fn full(width: u32, height: u32) -> Self {
        Self {
            x: 0,
            y: 0,
            width,
            height,
            min_z: 0.0,
            max_z: 1.0,
        }
    }
}

/// Clear parameters (matches `WW3D2::Renderer::Clear`).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClearState {
    pub clear_color: bool,
    pub clear_depth_stencil: bool,
    pub color: [f32; 4],
    pub depth: f32,
    pub stencil: u32,
}

impl Default for ClearState {
    fn default() -> Self {
        Self {
            clear_color: true,
            clear_depth_stencil: true,
            color: [0.0, 0.0, 0.0, 1.0],
            depth: 1.0,
            stencil: 0,
        }
    }
}

/// Fixed-function light type (matches WW3D2 directional & point lights).
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum LightKind {
    #[default]
    Disabled = 0,
    Directional = 1,
    Point = 2,
}

/// GPU-aligned light record (std140 compatible, 48 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Pod, Zeroable)]
pub struct GpuLight {
    /// xyz = world-space direction (directional) or position (point); w = LightKind as f32.
    pub pos_or_dir_and_kind: [f32; 4],
    /// rgb = diffuse color; w = range (point lights).
    pub diffuse_and_range: [f32; 4],
    /// rgb = specular color; w = attenuation.
    pub specular_and_atten: [f32; 4],
}

impl GpuLight {
    pub const DISABLED: Self = Self {
        pos_or_dir_and_kind: [0.0; 4],
        diffuse_and_range: [0.0; 4],
        specular_and_atten: [0.0; 4],
    };

    pub fn directional(direction: Vec3, diffuse: Vec3) -> Self {
        let dir = direction.normalize_or_zero();
        Self {
            pos_or_dir_and_kind: [dir.x, dir.y, dir.z, LightKind::Directional as u32 as f32],
            diffuse_and_range: [diffuse.x, diffuse.y, diffuse.z, 0.0],
            specular_and_atten: [0.0; 4],
        }
    }

    pub fn point(position: Vec3, diffuse: Vec3, range: f32) -> Self {
        Self {
            pos_or_dir_and_kind: [
                position.x,
                position.y,
                position.z,
                LightKind::Point as u32 as f32,
            ],
            diffuse_and_range: [diffuse.x, diffuse.y, diffuse.z, range.max(0.001)],
            specular_and_atten: [0.0, 0.0, 0.0, 1.0],
        }
    }
}

/// Fog state (matches `WW3D2::Renderer::Set_Fog`).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FogState {
    pub enabled: bool,
    pub color: [f32; 3],
    pub start: f32,
    pub end: f32,
}

impl Default for FogState {
    fn default() -> Self {
        Self {
            enabled: false,
            color: [0.0, 0.0, 0.0],
            start: 0.0,
            end: 1000.0,
        }
    }
}

/// Per-draw uniform block uploaded to the dynamic uniform buffer (std140 aligned).
#[repr(C)]
#[derive(Clone, Copy, Debug, Pod, Zeroable)]
pub struct DrawUniforms {
    pub world: [[f32; 4]; 4],
    pub view: [[f32; 4]; 4],
    pub projection: [[f32; 4]; 4],
    /// rgb = ambient color; w = lighting_enabled (0.0 or 1.0).
    pub ambient_and_lighting: [f32; 4],
    /// rgb = fog color; w = fog_enabled (0.0 or 1.0).
    pub fog_color_and_enable: [f32; 4],
    /// x = fog_start, y = fog_end, z = alpha_test_ref (<=0 disables), w = texturing_enabled (0/1).
    pub fog_and_shader_params: [f32; 4],
    pub lights: [GpuLight; 4],
    /// RGBA color from `D3DRS_TEXTUREFACTOR`.
    pub tfactor: [f32; 4],
    /// Stage 0 combiner: `[color_op, color_args, alpha_op, alpha_args]` (color_op==0 -> legacy single-tex).
    pub stage0_ops: [u32; 4],
    /// Stage 1 combiner: `[color_op, color_args, alpha_op, alpha_args]` (color_op==0 -> stage 1 disabled).
    pub stage1_ops: [u32; 4],
    /// xyz = world-space camera position; w = pbr_mode (0=classic, 1=remastered, 2=split-screen right half).
    pub camera_pos_and_pbr: [f32; 4],
    /// x = is_3d_lit, y = is_hdr_emissive (additive VFX), z = split_x_pixels, w = is_3d_unlit_surface.
    pub render_flags: [f32; 4],
    /// x = elapsed_time_sec, y = material_hint (0=default, 1=animated water), z/w = reserved.
    pub water_params: [f32; 4],
    /// Stage 0 texture transform matrix (column-major, identity when unused).
    pub tex_matrix0: [[f32; 4]; 4],
    /// Stage 1 texture transform matrix (column-major, identity when unused).
    pub tex_matrix1: [[f32; 4]; 4],
    /// `[s0_mode, s0_src_idx, s1_mode, s1_src_idx]`
    /// mode: 0 = passthrough, 1 = transform raw UV, 2 = camera-space pos, 3 = camera-space pos * tex_matrix.
    pub tex_gen_params: [u32; 4],
    /// `[blast_x, blast_y, blast_z, blast_strength]` — recent explosion shockwave driving reactive cloth sim.
    pub wind_blast: [f32; 4],
}

impl Default for DrawUniforms {
    fn default() -> Self {
        Self {
            world: Mat4::IDENTITY.to_cols_array_2d(),
            view: Mat4::IDENTITY.to_cols_array_2d(),
            projection: Mat4::IDENTITY.to_cols_array_2d(),
            ambient_and_lighting: [1.0, 1.0, 1.0, 0.0],
            fog_color_and_enable: [0.0, 0.0, 0.0, 0.0],
            fog_and_shader_params: [0.0, 1000.0, -1.0, 0.0],
            lights: [GpuLight::DISABLED; 4],
            tfactor: [1.0, 1.0, 1.0, 1.0],
            stage0_ops: [0, 0, 0, 0],
            stage1_ops: [0, 0, 0, 0],
            camera_pos_and_pbr: [0.0, 0.0, 100.0, 0.0],
            render_flags: [0.0, 0.0, 0.0, 0.0],
            water_params: [0.0, 0.0, 0.0, 0.0],
            tex_matrix0: Mat4::IDENTITY.to_cols_array_2d(),
            tex_matrix1: Mat4::IDENTITY.to_cols_array_2d(),
            tex_gen_params: [0, 0, 0, 1],
            wind_blast: [0.0, 0.0, 0.0, 0.0],
        }
    }
}

/// Visual presentation mode for live A/B comparison and player preference.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum GraphicsMode {
    /// Authentic 2003 Zero Hour fixed-function Gouraud shading, no post-processing.
    Classic = 0,
    /// Full modern Vulkan 1.3 visual upgrade (PBR micro-normals, GGX specular, sky IBL, HDR bloom, filmic tonemap, CAS).
    #[default]
    Remastered = 1,
    /// Live 50/50 split-screen comparison: Classic 2003 (left) vs. Remastered (right).
    SplitScreen = 2,
}

impl GraphicsMode {
    pub const fn from_u32(v: u32) -> Self {
        match v {
            0 => Self::Classic,
            2 => Self::SplitScreen,
            _ => Self::Remastered,
        }
    }
}

/// Live-toggleable graphics enhancement switches (F7/F8/F9/F10/F11).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GraphicsSettings {
    pub mode: GraphicsMode,
    pub pbr_lighting: bool,
    pub post_fx: bool,
    pub remaster_textures: bool,
}

impl GraphicsSettings {
    pub const fn classic() -> Self {
        Self {
            mode: GraphicsMode::Classic,
            pbr_lighting: false,
            post_fx: false,
            remaster_textures: false,
        }
    }

    pub const fn remastered() -> Self {
        Self {
            mode: GraphicsMode::Remastered,
            pbr_lighting: true,
            post_fx: true,
            remaster_textures: true,
        }
    }

    pub const fn split_screen() -> Self {
        Self {
            mode: GraphicsMode::SplitScreen,
            pbr_lighting: true,
            post_fx: true,
            remaster_textures: true,
        }
    }

    pub fn from_env(default_remastered: bool) -> Self {
        match std::env::var("ZHR_GRAPHICS_MODE")
            .ok()
            .as_deref()
            .map(str::trim)
            .map(str::to_ascii_lowercase)
            .as_deref()
        {
            Some("classic") | Some("0") => Self::classic(),
            Some("split") | Some("splitscreen") | Some("2") => Self::split_screen(),
            Some("remastered") | Some("1") => Self::remastered(),
            _ => {
                if default_remastered {
                    Self::remastered()
                } else {
                    Self::classic()
                }
            }
        }
    }
}

impl DrawUniforms {
    pub fn set_world(&mut self, m: Mat4) {
        self.world = m.to_cols_array_2d();
    }

    pub fn set_view(&mut self, m: Mat4) {
        self.view = m.to_cols_array_2d();
    }

    pub fn set_projection(&mut self, m: Mat4) {
        self.projection = m.to_cols_array_2d();
    }

    pub fn set_ambient(&mut self, rgb: Vec3) {
        self.ambient_and_lighting[0] = rgb.x;
        self.ambient_and_lighting[1] = rgb.y;
        self.ambient_and_lighting[2] = rgb.z;
    }

    pub fn set_lighting_enabled(&mut self, enabled: bool) {
        self.ambient_and_lighting[3] = if enabled { 1.0 } else { 0.0 };
    }

    pub fn set_fog(&mut self, fog: FogState) {
        self.fog_color_and_enable = [
            fog.color[0],
            fog.color[1],
            fog.color[2],
            if fog.enabled { 1.0 } else { 0.0 },
        ];
        self.fog_and_shader_params[0] = fog.start;
        self.fog_and_shader_params[1] = fog.end;
    }

    pub fn set_alpha_test(&mut self, enabled: bool, threshold: f32) {
        self.fog_and_shader_params[2] = if enabled { threshold } else { -1.0 };
    }

    pub fn set_texturing(&mut self, enabled: bool) {
        self.fog_and_shader_params[3] = if enabled { 1.0 } else { 0.0 };
    }
}

/// Supported pixel formats for Zero Hour textures (including DXT/BC compressed formats).
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TextureFormat {
    Rgba8Unorm = 0,
    Bgra8Unorm = 1,
    Bc1RgbaUnorm = 2,
    Bc2Unorm = 3,
    Bc3Unorm = 4,
}

impl TextureFormat {
    pub fn to_vk(self) -> ash::vk::Format {
        match self {
            Self::Rgba8Unorm => ash::vk::Format::R8G8B8A8_UNORM,
            Self::Bgra8Unorm => ash::vk::Format::B8G8R8A8_UNORM,
            Self::Bc1RgbaUnorm => ash::vk::Format::BC1_RGBA_UNORM_BLOCK,
            Self::Bc2Unorm => ash::vk::Format::BC2_UNORM_BLOCK,
            Self::Bc3Unorm => ash::vk::Format::BC3_UNORM_BLOCK,
        }
    }

    pub fn is_compressed(self) -> bool {
        matches!(self, Self::Bc1RgbaUnorm | Self::Bc2Unorm | Self::Bc3Unorm)
    }

    pub fn expected_byte_size(self, width: u32, height: u32) -> usize {
        let w = width.max(1) as usize;
        let h = height.max(1) as usize;
        match self {
            Self::Rgba8Unorm | Self::Bgra8Unorm => w * h * 4,
            Self::Bc1RgbaUnorm => w.div_ceil(4) * h.div_ceil(4) * 8,
            Self::Bc2Unorm | Self::Bc3Unorm => w.div_ceil(4) * h.div_ceil(4) * 16,
        }
    }
}

/// Sampler filtering & addressing parameters.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SamplerDesc {
    pub linear_filter: bool,
    pub clamp_edge: bool,
}

impl Default for SamplerDesc {
    fn default() -> Self {
        Self {
            linear_filter: true,
            clamp_edge: false,
        }
    }
}

/// GPU device description exposed to `WW3D2::Renderer::Get_Render_Device_*`.
#[derive(Clone, Debug)]
pub struct DeviceInfo {
    pub name: String,
    pub is_discrete: bool,
    pub api_version: u32,
    pub vendor_id: u32,
    pub device_id: u32,
}

/// Per-frame render statistics.
#[derive(Clone, Copy, Debug, Default)]
pub struct RenderStats {
    pub draw_calls: u32,
    pub triangles: u32,
    pub pipeline_binds: u32,
    pub texture_binds: u32,
}

/// Opaque GPU resource handles.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct BufferId(pub u32);

#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TextureId(pub u32);

/// Helper to construct a 2D orthographic projection for WW3D2 screen-space UI (`0..w`, `0..h`).
pub fn ortho_2d(width: f32, height: f32) -> Mat4 {
    let sx = 2.0 / width.max(1.0);
    let sy = -2.0 / height.max(1.0);
    Mat4::from_cols(
        Vec4::new(sx, 0.0, 0.0, 0.0),
        Vec4::new(0.0, sy, 0.0, 0.0),
        Vec4::new(0.0, 0.0, 1.0, 0.0),
        Vec4::new(-1.0, 1.0, 0.0, 1.0),
    )
}

/// Left-handed perspective projection mapping `[z_near, z_far]` -> `[0, 1]` (matches D3D8/WW3D2).
pub fn perspective_lh(fov_y_radians: f32, aspect_ratio: f32, z_near: f32, z_far: f32) -> Mat4 {
    let f = 1.0 / (0.5 * fov_y_radians).tan();
    let range = z_far / (z_far - z_near);
    Mat4::from_cols(
        Vec4::new(f / aspect_ratio, 0.0, 0.0, 0.0),
        Vec4::new(0.0, f, 0.0, 0.0),
        Vec4::new(0.0, 0.0, range, 1.0),
        Vec4::new(0.0, 0.0, -range * z_near, 0.0),
    )
}

/// Left-handed look-at view matrix (matches D3D8/WW3D2).
pub fn look_at_lh(eye: Vec3, center: Vec3, up: Vec3) -> Mat4 {
    let f = (center - eye).normalize();
    let s = up.cross(f).normalize();
    let u = f.cross(s);
    Mat4::from_cols(
        Vec4::new(s.x, u.x, f.x, 0.0),
        Vec4::new(s.y, u.y, f.y, 0.0),
        Vec4::new(s.z, u.z, f.z, 0.0),
        Vec4::new(-s.dot(eye), -u.dot(eye), -f.dot(eye), 1.0),
    )
}
