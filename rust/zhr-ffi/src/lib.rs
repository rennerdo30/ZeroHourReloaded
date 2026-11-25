//! C-ABI entry points for linking ZeroHourReloaded Rust subsystems (including Vulkan) into C++.

use std::ffi::{CStr, CString, c_char, c_void};
use std::sync::{Mutex, OnceLock};

use glam::{Mat4, Vec3};
use zhr_audio::{AudioEngine, VoiceKind};
use zhr_vulkan::shader::{DepthCompare, ShaderKey, StencilOp, StencilState};
use zhr_vulkan::types::{
    BufferId, ClearState, FogState, GpuLight, GraphicsMode, GraphicsSettings, SamplerDesc,
    TextureFormat, TextureId, VertexWw3d, Viewport,
};
use zhr_vulkan::{RendererConfig, VulkanRenderer};

static BANNER_CSTR: &[u8] = concat!(
    "ZeroHourReloaded Rust Core v",
    env!("CARGO_PKG_VERSION"),
    "\0"
)
.as_bytes();

struct VkGlobalState {
    renderer: Option<VulkanRenderer>,
    device_name_cache: Vec<CString>,
}

fn vk_state() -> &'static Mutex<VkGlobalState> {
    static STATE: OnceLock<Mutex<VkGlobalState>> = OnceLock::new();
    STATE.get_or_init(|| {
        Mutex::new(VkGlobalState {
            renderer: None,
            device_name_cache: Vec::new(),
        })
    })
}

fn cache_device_names(state: &mut VkGlobalState) {
    state.device_name_cache.clear();
    if let Some(r) = &state.renderer {
        for dev in r.available_devices() {
            let cs = CString::new(dev.name.as_str()).unwrap_or_else(|_| c"Vulkan GPU".to_owned());
            state.device_name_cache.push(cs);
        }
    }
}

/// Returns a null-terminated UTF-8 string identifying the Rust core build.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_rust_banner() -> *const c_char {
    BANNER_CSTR.as_ptr().cast::<c_char>()
}

/// Returns the pointer width in bits that the Rust library was compiled for (e.g. 64).
#[unsafe(no_mangle)]
pub extern "C" fn zhr_rust_pointer_bits() -> u32 {
    zhr_core::POINTER_BITS
}

/// Initializes a headless offscreen Vulkan 1.3 renderer. Returns 1 on success, 0 on failure.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_init_headless(width: u32, height: u32, enable_validation: bool) -> i32 {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    guard.renderer = None;
    let config = RendererConfig {
        width,
        height,
        enable_validation,
        ..Default::default()
    };
    match VulkanRenderer::new_headless(config) {
        Ok(r) => {
            guard.renderer = Some(r);
            cache_device_names(&mut guard);
            1
        }
        Err(e) => {
            log::error!("zhr_vk_init_headless failed: {e}");
            0
        }
    }
}

/// Initializes a windowed Vulkan 1.3 renderer from a Win32 HWND (cast to `void*`).
/// Returns 1 on success, 0 on failure.
#[cfg(target_os = "windows")]
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_init_win32(
    hwnd: *mut c_void,
    hinstance: *mut c_void,
    width: u32,
    height: u32,
    vsync: bool,
    enable_validation: bool,
) -> i32 {
    use raw_window_handle::{
        RawDisplayHandle, RawWindowHandle, Win32WindowHandle, WindowsDisplayHandle,
    };
    use std::num::NonZeroIsize;

    let Some(hwnd_nz) = NonZeroIsize::new(hwnd as isize) else {
        return 0;
    };
    let mut win_handle = Win32WindowHandle::new(hwnd_nz);
    win_handle.hinstance = NonZeroIsize::new(hinstance as isize);

    let display = RawDisplayHandle::Windows(WindowsDisplayHandle::new());
    let window = RawWindowHandle::Win32(win_handle);

    let mut guard = vk_state().lock().expect("vk state poisoned");
    guard.renderer = None;
    let config = RendererConfig {
        width,
        height,
        vsync,
        enable_validation,
        ..Default::default()
    };
    match VulkanRenderer::new_windowed(config, display, window) {
        Ok(r) => {
            if let Ok(p) = std::env::var("ZHR_VK_TRACE_LOG") {
                let (rw, rh) = r.resolution();
                let msg = format!(
                    "init_win32 OK device='{}' req={}x{} actual={}x{} val={}\n",
                    r.device_info().name,
                    width,
                    height,
                    rw,
                    rh,
                    r.validation_active()
                );
                let _ = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(p)
                    .and_then(|mut f| std::io::Write::write_all(&mut f, msg.as_bytes()));
            }
            guard.renderer = Some(r);
            cache_device_names(&mut guard);
            1
        }
        Err(e) => {
            if let Ok(p) = std::env::var("ZHR_VK_TRACE_LOG") {
                let msg = format!("init_win32 ERR ({width}x{height}): {e}\n");
                let _ = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(p)
                    .and_then(|mut f| std::io::Write::write_all(&mut f, msg.as_bytes()));
            }
            log::error!("zhr_vk_init_win32 failed: {e}");
            0
        }
    }
}

/// Initializes a windowed Vulkan 1.3 renderer from an X11 Display + Window (Linux).
#[cfg(target_os = "linux")]
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_init_xlib(
    display: *mut c_void,
    window: u64,
    width: u32,
    height: u32,
    vsync: bool,
    enable_validation: bool,
) -> i32 {
    use raw_window_handle::{
        RawDisplayHandle, RawWindowHandle, XlibDisplayHandle, XlibWindowHandle,
    };
    use std::ptr::NonNull;

    let display_handle = RawDisplayHandle::Xlib(XlibDisplayHandle::new(NonNull::new(display), 0));
    let window_handle = RawWindowHandle::Xlib(XlibWindowHandle::new(window as std::ffi::c_ulong));
    init_windowed_common(
        display_handle,
        window_handle,
        width,
        height,
        vsync,
        enable_validation,
    )
}

/// Initializes a windowed Vulkan 1.3 renderer from a Wayland wl_display + wl_surface (Linux).
#[cfg(target_os = "linux")]
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_init_wayland(
    display: *mut c_void,
    surface: *mut c_void,
    width: u32,
    height: u32,
    vsync: bool,
    enable_validation: bool,
) -> i32 {
    use raw_window_handle::{
        RawDisplayHandle, RawWindowHandle, WaylandDisplayHandle, WaylandWindowHandle,
    };
    use std::ptr::NonNull;

    let (Some(disp), Some(surf)) = (NonNull::new(display), NonNull::new(surface)) else {
        return 0;
    };
    let display_handle = RawDisplayHandle::Wayland(WaylandDisplayHandle::new(disp));
    let window_handle = RawWindowHandle::Wayland(WaylandWindowHandle::new(surf));
    init_windowed_common(
        display_handle,
        window_handle,
        width,
        height,
        vsync,
        enable_validation,
    )
}

/// Initializes a windowed Vulkan 1.3 renderer from a Cocoa NSView (macOS / MoltenVK).
#[cfg(target_os = "macos")]
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_init_macos(
    ns_view: *mut c_void,
    width: u32,
    height: u32,
    vsync: bool,
    enable_validation: bool,
) -> i32 {
    use raw_window_handle::{
        AppKitDisplayHandle, AppKitWindowHandle, RawDisplayHandle, RawWindowHandle,
    };
    use std::ptr::NonNull;

    let Some(view) = NonNull::new(ns_view) else {
        return 0;
    };
    let display_handle = RawDisplayHandle::AppKit(AppKitDisplayHandle::new());
    let window_handle = RawWindowHandle::AppKit(AppKitWindowHandle::new(view));
    init_windowed_common(
        display_handle,
        window_handle,
        width,
        height,
        vsync,
        enable_validation,
    )
}

#[cfg(any(target_os = "linux", target_os = "macos"))]
fn init_windowed_common(
    display: raw_window_handle::RawDisplayHandle,
    window: raw_window_handle::RawWindowHandle,
    width: u32,
    height: u32,
    vsync: bool,
    enable_validation: bool,
) -> i32 {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    guard.renderer = None;
    let config = RendererConfig {
        width,
        height,
        vsync,
        enable_validation,
        ..Default::default()
    };
    match VulkanRenderer::new_windowed(config, display, window) {
        Ok(r) => {
            guard.renderer = Some(r);
            cache_device_names(&mut guard);
            1
        }
        Err(e) => {
            log::error!("zhr_vk_init_windowed failed: {e}");
            0
        }
    }
}

/// Shuts down the active Vulkan renderer and releases all GPU resources.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_shutdown() {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    guard.renderer = None;
    guard.device_name_cache.clear();
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_device_count() -> i32 {
    let guard = vk_state().lock().expect("vk state poisoned");
    guard.device_name_cache.len() as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_selected_device() -> i32 {
    let guard = vk_state().lock().expect("vk state poisoned");
    guard
        .renderer
        .as_ref()
        .map(|r| r.selected_device_index() as i32)
        .unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_device_name(index: i32) -> *const c_char {
    let guard = vk_state().lock().expect("vk state poisoned");
    if index < 0 {
        return c"".as_ptr();
    }
    guard
        .device_name_cache
        .get(index as usize)
        .map(|cs| cs.as_ptr())
        .unwrap_or(c"".as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_begin_frame() -> i32 {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(r) = guard.renderer.as_mut() else {
        return 0;
    };
    match r.begin_frame() {
        Ok(()) => 1,
        Err(e) => {
            if let Ok(p) = std::env::var("ZHR_VK_TRACE_LOG") {
                let msg = format!("begin_frame ERR: {e}\n");
                let _ = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(p)
                    .and_then(|mut f| std::io::Write::write_all(&mut f, msg.as_bytes()));
            }
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_end_frame() -> i32 {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(r) = guard.renderer.as_mut() else {
        return 0;
    };
    match r.end_frame() {
        Ok(()) => 1,
        Err(e) => {
            if let Ok(p) = std::env::var("ZHR_VK_TRACE_LOG") {
                let msg = format!("end_frame ERR: {e}\n");
                let _ = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(p)
                    .and_then(|mut f| std::io::Write::write_all(&mut f, msg.as_bytes()));
            }
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_clear(
    clear_color: bool,
    clear_depth_stencil: bool,
    r: f32,
    g: f32,
    b: f32,
    a: f32,
    depth: f32,
    stencil: u32,
) -> i32 {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    let state = ClearState {
        clear_color,
        clear_depth_stencil,
        color: [r, g, b, a],
        depth,
        stencil,
    };
    i32::from(ren.clear(state).is_ok())
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_viewport(
    x: u32,
    y: u32,
    width: u32,
    height: u32,
    min_z: f32,
    max_z: f32,
) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_viewport(Viewport {
            x,
            y,
            width,
            height,
            min_z,
            max_z,
        });
    }
}

/// Sets transform (`kind`: 0=World, 1=View, 2=Projection) from a 16-float column-major matrix.
///
/// # Safety
/// `matrix16` must point to at least 16 readable `f32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_set_transform(kind: u32, matrix16: *const f32) {
    if matrix16.is_null() {
        return;
    }
    // SAFETY: Caller guarantees `matrix16` points to 16 contiguous f32s.
    let slice = unsafe { std::slice::from_raw_parts(matrix16, 16) };
    let mat = Mat4::from_cols_slice(slice);
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        match kind {
            0 => ren.set_world(mat),
            1 => ren.set_view(mat),
            2 => ren.set_projection(mat),
            _ => {}
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_ambient(r: f32, g: f32, b: f32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_ambient(Vec3::new(r, g, b));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_directional_light(
    index: u32,
    dx: f32,
    dy: f32,
    dz: f32,
    r: f32,
    g: f32,
    b: f32,
) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_lighting_enabled(true);
        ren.set_light(
            index as usize,
            GpuLight::directional(Vec3::new(dx, dy, dz), Vec3::new(r, g, b)),
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_point_light(
    index: u32,
    px: f32,
    py: f32,
    pz: f32,
    r: f32,
    g: f32,
    b: f32,
    range: f32,
) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_lighting_enabled(true);
        ren.set_light(
            index as usize,
            GpuLight::point(Vec3::new(px, py, pz), Vec3::new(r, g, b), range),
        );
    }
}

/// Queries the active renderer resolution into optional output pointers.
///
/// # Safety
/// Non-null `out_width` and `out_height` must point to valid writable `u32` locations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_get_resolution(out_width: *mut u32, out_height: *mut u32) {
    let guard = vk_state().lock().expect("vk state poisoned");
    let (w, h) = guard
        .renderer
        .as_ref()
        .map(|r| r.resolution())
        .unwrap_or((0, 0));
    if !out_width.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_width = w };
    }
    if !out_height.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_height = h };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_is_windowed() -> bool {
    let guard = vk_state().lock().expect("vk state poisoned");
    guard
        .renderer
        .as_ref()
        .map(|r| r.is_windowed())
        .unwrap_or(false)
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_is_frame_active() -> bool {
    let guard = vk_state().lock().expect("vk state poisoned");
    guard
        .renderer
        .as_ref()
        .map(|r| r.is_frame_active())
        .unwrap_or(false)
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_resize(width: u32, height: u32) -> i32 {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    i32::from(ren.resize(width, height).is_ok())
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_lighting_enabled(enabled: bool) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_lighting_enabled(enabled);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_alpha_test_ref(alpha_ref: f32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_alpha_test_ref(alpha_ref);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_clear_light(index: u32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.clear_light(index as usize);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_fog(enable: bool, r: f32, g: f32, b: f32, start: f32, end: f32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_fog(FogState {
            enabled: enable,
            color: [r, g, b],
            start,
            end,
        });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_shader(ww3d_shader_bits: u32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_shader(ShaderKey::from_raw(ww3d_shader_bits));
    }
}

/// Sets hardware stencil test/op state (`func`: 0=Never..7=Always; ops: D3DSTENCILOP 1..8).
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_stencil_state(
    enabled: bool,
    func: u32,
    fail_op: u32,
    depth_fail_op: u32,
    pass_op: u32,
    compare_mask: u32,
    write_mask: u32,
    reference: u32,
) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        let cmp = match func {
            0 => DepthCompare::Never,
            1 => DepthCompare::Less,
            2 => DepthCompare::Equal,
            3 => DepthCompare::LessEqual,
            4 => DepthCompare::Greater,
            5 => DepthCompare::NotEqual,
            6 => DepthCompare::GreaterEqual,
            _ => DepthCompare::Always,
        };
        ren.set_stencil(StencilState {
            enabled,
            func: cmp,
            fail_op: StencilOp::from_d3d(fail_op),
            depth_fail_op: StencilOp::from_d3d(depth_fail_op),
            pass_op: StencilOp::from_d3d(pass_op),
            compare_mask: compare_mask & 0xFF,
            write_mask: write_mask & 0xFF,
            reference: reference & 0xFF,
        });
    }
}

/// Uploads a 2D texture in any supported `TextureFormat` and returns its non-zero handle (0 on failure).
/// `format`: 0=RGBA8, 1=BGRA8, 2=BC1(DXT1), 3=BC2(DXT3), 4=BC3(DXT5).
///
/// # Safety
/// `pixels` must point to `byte_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_create_texture_2d(
    width: u32,
    height: u32,
    format: u32,
    pixels: *const u8,
    byte_len: usize,
    linear_filter: bool,
    clamp_edge: bool,
) -> u32 {
    if pixels.is_null() {
        return 0;
    }
    let tex_format = match format {
        0 => TextureFormat::Rgba8Unorm,
        1 => TextureFormat::Bgra8Unorm,
        2 => TextureFormat::Bc1RgbaUnorm,
        3 => TextureFormat::Bc2Unorm,
        4 => TextureFormat::Bc3Unorm,
        _ => return 0,
    };
    // SAFETY: Caller guarantees `pixels` is valid for `byte_len` bytes.
    let data = unsafe { std::slice::from_raw_parts(pixels, byte_len) };
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    ren.create_texture_2d(
        width,
        height,
        tex_format,
        data,
        SamplerDesc {
            linear_filter,
            clamp_edge,
        },
    )
    .map(|id| id.0)
    .unwrap_or(0)
}

/// Uploads a 2D texture with `mip_count` tightly-packed mip levels (for `.dds` BC1/BC2/BC3 mip chains).
///
/// # Safety
/// `pixels` must point to `byte_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_create_texture_2d_mips(
    width: u32,
    height: u32,
    mip_count: u32,
    format: u32,
    pixels: *const u8,
    byte_len: usize,
    linear_filter: bool,
    clamp_edge: bool,
) -> u32 {
    if pixels.is_null() {
        return 0;
    }
    let tex_format = match format {
        0 => TextureFormat::Rgba8Unorm,
        1 => TextureFormat::Bgra8Unorm,
        2 => TextureFormat::Bc1RgbaUnorm,
        3 => TextureFormat::Bc2Unorm,
        4 => TextureFormat::Bc3Unorm,
        _ => return 0,
    };
    // SAFETY: Caller guarantees `pixels` is valid for `byte_len` bytes.
    let data = unsafe { std::slice::from_raw_parts(pixels, byte_len) };
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    ren.create_texture_2d_mips(
        width,
        height,
        mip_count,
        tex_format,
        data,
        SamplerDesc {
            linear_filter,
            clamp_edge,
        },
    )
    .map(|id| id.0)
    .unwrap_or(0)
}

/// Sets the live graphics presentation mode (`mode`: 0=Classic, 1=Remastered, 2=SplitScreen)
/// and individual PBR lighting / HDR post-FX toggles.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_graphics_mode(mode: u32, pbr_lighting: bool, post_fx: bool) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        let prev = ren.graphics_settings();
        let parsed_mode = GraphicsMode::from_u32(mode);
        let remaster_textures = if parsed_mode == GraphicsMode::Classic {
            false
        } else if prev.mode == GraphicsMode::Classic {
            true
        } else {
            prev.remaster_textures
        };
        ren.set_graphics_settings(GraphicsSettings {
            mode: parsed_mode,
            pbr_lighting,
            post_fx,
            remaster_textures,
        });
    }
}

/// Toggles 2x remastered textures on/off live without restarting.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_remaster_textures(enabled: bool) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        let mut s = ren.graphics_settings();
        s.remaster_textures = enabled;
        ren.set_graphics_settings(s);
    }
}

/// Sets a semantic material hint for upcoming draws (`0 = default`, `1 = animated water`, `5/6 = vol particles`, `7/8 = cloth`).
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_material_hint(hint: u32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_material_hint(hint);
    }
}

/// Registers a 3D battlefield explosion shockwave that whips nearby flags & canopies outward.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_add_wind_impulse(x: f32, y: f32, z: f32, strength: f32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.add_wind_impulse(x, y, z, strength);
    }
}

/// Returns packed graphics state: bits 0..1 = mode, bit 2 = pbr_lighting, bit 3 = post_fx, bit 4 = remaster_textures.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_get_graphics_mode() -> u32 {
    let guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_ref() else {
        return 0;
    };
    let s = ren.graphics_settings();
    (s.mode as u32 & 0x3)
        | (u32::from(s.pbr_lighting) << 2)
        | (u32::from(s.post_fx) << 3)
        | (u32::from(s.remaster_textures) << 4)
}

/// Uploads a 2D RGBA8 texture and returns its non-zero handle (0 on failure).
///
/// # Safety
/// `pixels` must point to `byte_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_create_texture_rgba8(
    width: u32,
    height: u32,
    pixels: *const u8,
    byte_len: usize,
    linear_filter: bool,
    clamp_edge: bool,
) -> u32 {
    // SAFETY: Forwarded with identical pointer/length preconditions.
    unsafe {
        zhr_vk_create_texture_2d(
            width,
            height,
            0,
            pixels,
            byte_len,
            linear_filter,
            clamp_edge,
        )
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_destroy_texture(texture_id: u32) {
    if texture_id == 0 {
        return;
    }
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        let _ = ren.destroy_texture(TextureId(texture_id));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_texture(texture_id: u32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_texture(if texture_id == 0 {
            None
        } else {
            Some(TextureId(texture_id))
        });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_texture_stage(stage: u32, texture_id: u32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_texture_stage(
            stage,
            if texture_id == 0 {
                None
            } else {
                Some(TextureId(texture_id))
            },
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_texture_factor(r: f32, g: f32, b: f32, a: f32) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_texture_factor([r, g, b, a]);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_stage_state(
    stage: u32,
    color_op: u32,
    color_args: u32,
    alpha_op: u32,
    alpha_args: u32,
) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_stage_state(stage, color_op, color_args, alpha_op, alpha_args);
    }
}

/// Configures GPU vertex-shader UV generation & texture transform for `stage` (0 or 1).
///
/// # Safety
/// If `matrix16` is non-null, it must point to at least 16 readable `f32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_set_tex_gen(
    stage: u32,
    mode: u32,
    src_idx: u32,
    matrix16: *const f32,
) {
    let mat = if matrix16.is_null() {
        None
    } else {
        // SAFETY: Caller guarantees `matrix16` points to 16 contiguous f32s when non-null.
        let slice = unsafe { std::slice::from_raw_parts(matrix16, 16) };
        Some(Mat4::from_cols_slice(slice))
    };
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_tex_gen(stage, mode, src_idx, mat);
    }
}

/// Creates a persistent GPU vertex buffer (zero-stall ReBAR/host-visible upload) and returns its ID (0 on failure).
///
/// # Safety
/// `vertices` must point to `vertex_count` valid elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_create_vertex_buffer(
    vertices: *const VertexWw3d,
    vertex_count: usize,
) -> u32 {
    if vertices.is_null() || vertex_count == 0 {
        return 0;
    }
    // SAFETY: Caller guarantees valid slice.
    let verts = unsafe { std::slice::from_raw_parts(vertices, vertex_count) };
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    ren.create_vertex_buffer_cached(verts)
        .map(|id| id.0)
        .unwrap_or(0)
}

/// Creates a persistent GPU 16-bit index buffer (zero-stall ReBAR/host-visible upload) and returns its ID (0 on failure).
///
/// # Safety
/// `indices` must point to `index_count` valid elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_create_index_buffer(
    indices: *const u16,
    index_count: usize,
) -> u32 {
    if indices.is_null() || index_count == 0 {
        return 0;
    }
    // SAFETY: Caller guarantees valid slice.
    let inds = unsafe { std::slice::from_raw_parts(indices, index_count) };
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    ren.create_index_buffer_u16_cached(inds)
        .map(|id| id.0)
        .unwrap_or(0)
}

/// Schedules deferred GPU buffer destruction (zero pipeline stalls when called mid-frame).
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_destroy_buffer(buffer_id: u32) {
    if buffer_id == 0 {
        return;
    }
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        let _ = ren.destroy_buffer(BufferId(buffer_id));
    }
}

/// Draws indexed triangles directly from persistent GPU vertex and index buffers.
#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_draw_indexed(
    vb_id: u32,
    ib_id: u32,
    index_count: u32,
    first_index: u32,
    vertex_offset: i32,
) -> i32 {
    if vb_id == 0 || ib_id == 0 || index_count == 0 {
        return 0;
    }
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    i32::from(
        ren.draw_indexed(
            BufferId(vb_id),
            BufferId(ib_id),
            index_count,
            first_index,
            vertex_offset,
        )
        .is_ok(),
    )
}

/// Submits transient indexed vertices directly through the dynamic ring buffer.
///
/// # Safety
/// `vertices` and `indices` must point to `vertex_count` / `index_count` valid elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_draw_dynamic(
    vertices: *const VertexWw3d,
    vertex_count: usize,
    indices: *const u16,
    index_count: usize,
) -> i32 {
    if vertices.is_null() || indices.is_null() {
        return 0;
    }
    // SAFETY: Caller guarantees valid slices for the duration of the call.
    let verts = unsafe { std::slice::from_raw_parts(vertices, vertex_count) };
    let inds = unsafe { std::slice::from_raw_parts(indices, index_count) };
    let mut guard = vk_state().lock().expect("vk state poisoned");
    let Some(ren) = guard.renderer.as_mut() else {
        return 0;
    };
    i32::from(ren.draw_dynamic(verts, inds).is_ok())
}

// ---------------------------------------------------------------------------
// Native 64-bit Cross-Platform Audio C-ABI (replaces 32-bit mss32.dll)
// ---------------------------------------------------------------------------

struct AudioGlobalState {
    engine: Option<AudioEngine>,
    device_name_cstr: CString,
}

fn audio_state() -> &'static Mutex<AudioGlobalState> {
    static STATE: OnceLock<Mutex<AudioGlobalState>> = OnceLock::new();
    STATE.get_or_init(|| {
        Mutex::new(AudioGlobalState {
            engine: None,
            device_name_cstr: c"Uninitialized".to_owned(),
        })
    })
}

fn voice_kind_from_u32(kind: u32) -> VoiceKind {
    match kind {
        1 => VoiceKind::Sample3D,
        2 => VoiceKind::Stream,
        _ => VoiceKind::Sample2D,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_init(headless: bool) -> i32 {
    let mut guard = audio_state().lock().expect("audio state poisoned");
    if guard.engine.is_none() {
        let env_headless = std::env::var("ZHR_AUDIO_HEADLESS")
            .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
            .unwrap_or(false)
            || std::env::var("ZHR_HEADLESS_RENDER")
                .map(|v| !v.is_empty() && v != "0")
                .unwrap_or(false);
        let engine = AudioEngine::new(headless || env_headless);
        let name =
            CString::new(engine.device_name()).unwrap_or_else(|_| c"Audio Device".to_owned());
        guard.device_name_cstr = name;
        guard.engine = Some(engine);
    }
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_shutdown() {
    let mut guard = audio_state().lock().expect("audio state poisoned");
    guard.engine = None;
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_device_name() -> *const c_char {
    let guard = audio_state().lock().expect("audio state poisoned");
    guard.device_name_cstr.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_alloc_voice(kind: u32) -> u32 {
    let guard = audio_state().lock().expect("audio state poisoned");
    guard
        .engine
        .as_ref()
        .map(|e| e.alloc_voice(voice_kind_from_u32(kind)))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_release_voice(voice_id: u32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.release_voice(voice_id);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_reset_voice(voice_id: u32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.reset_voice(voice_id);
    }
}

/// Decodes and attaches an in-memory WAV/PCM/ADPCM/MP3 buffer to `voice_id`.
///
/// # Safety
/// `bytes` must point to `byte_len` readable bytes; `hint_ext` may be null or a valid NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_audio_load_bytes(
    voice_id: u32,
    bytes: *const u8,
    byte_len: usize,
    hint_ext: *const c_char,
) -> i32 {
    if bytes.is_null() || byte_len == 0 {
        return 0;
    }
    // SAFETY: Caller guarantees `bytes` is valid for `byte_len` bytes.
    let slice = unsafe { std::slice::from_raw_parts(bytes, byte_len) };
    let hint = if hint_ext.is_null() {
        None
    } else {
        // SAFETY: Caller passed a valid NUL-terminated C string.
        unsafe { CStr::from_ptr(hint_ext) }.to_str().ok()
    };
    let guard = audio_state().lock().expect("audio state poisoned");
    guard
        .engine
        .as_ref()
        .map(|e| i32::from(e.load_voice_bytes(voice_id, slice, hint)))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_start(voice_id: u32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.start_voice(voice_id);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_stop(voice_id: u32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.stop_voice(voice_id);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_pause(voice_id: u32, paused: bool) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.pause_voice(voice_id, paused);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_volume_pan(voice_id: u32, volume: f32, pan: f32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_volume_pan(voice_id, volume, pan);
    }
}

/// Reads a voice's current volume and pan into optional output pointers.
///
/// # Safety
/// Non-null `out_volume` and `out_pan` must point to writable `f32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_audio_get_volume_pan(
    voice_id: u32,
    out_volume: *mut f32,
    out_pan: *mut f32,
) {
    let guard = audio_state().lock().expect("audio state poisoned");
    let (vol, pan) = guard
        .engine
        .as_ref()
        .map(|e| e.get_volume_pan(voice_id))
        .unwrap_or((1.0, 0.5));
    if !out_volume.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_volume = vol };
    }
    if !out_pan.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_pan = pan };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_loop_count(voice_id: u32, count: u32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_loop_count(voice_id, count);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_get_loop_count(voice_id: u32) -> u32 {
    let guard = audio_state().lock().expect("audio state poisoned");
    guard
        .engine
        .as_ref()
        .map(|e| e.get_loop_count(voice_id))
        .unwrap_or(1)
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_playback_rate(voice_id: u32, rate: u32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_playback_rate(voice_id, rate);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_get_playback_rate(voice_id: u32) -> u32 {
    let guard = audio_state().lock().expect("audio state poisoned");
    guard
        .engine
        .as_ref()
        .map(|e| e.get_playback_rate(voice_id))
        .unwrap_or(22050)
}

/// Reads `(total_ms, current_ms)` for a voice into optional output pointers.
///
/// # Safety
/// Non-null pointers must be valid writable `u32` locations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_audio_get_ms_position(
    voice_id: u32,
    out_total_ms: *mut u32,
    out_current_ms: *mut u32,
) {
    let guard = audio_state().lock().expect("audio state poisoned");
    let (total, cur) = guard
        .engine
        .as_ref()
        .map(|e| e.get_ms_position(voice_id))
        .unwrap_or((0, 0));
    if !out_total_ms.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_total_ms = total };
    }
    if !out_current_ms.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_current_ms = cur };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_ms_position(voice_id: u32, ms: u32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_ms_position(voice_id, ms);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_3d_position(voice_id: u32, x: f32, y: f32, z: f32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_3d_position(voice_id, x, y, z);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_3d_distances(voice_id: u32, d1: f32, d2: f32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_3d_distances(voice_id, d1, d2);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_3d_occlusion(voice_id: u32, occlusion: f32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_3d_occlusion(voice_id, occlusion);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_listener_position(x: f32, y: f32, z: f32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_listener_position(x, y, z);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_listener_orientation(
    fx: f32,
    fy: f32,
    fz: f32,
    ux: f32,
    uy: f32,
    uz: f32,
) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_listener_orientation(fx, fy, fz, ux, uy, uz);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_set_user_data(voice_id: u32, index: u32, value: i32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.set_user_data(voice_id, index as usize, value);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_get_user_data(voice_id: u32, index: u32) -> i32 {
    let guard = audio_state().lock().expect("audio state poisoned");
    guard
        .engine
        .as_ref()
        .map(|e| e.get_user_data(voice_id, index as usize))
        .unwrap_or(0)
}

/// Pops one completed voice ID and kind for main-thread EOS callback dispatch. Returns 1 if popped, 0 if empty.
///
/// # Safety
/// Non-null output pointers must point to valid writable `u32` locations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_audio_pop_completed(
    out_voice_id: *mut u32,
    out_kind: *mut u32,
) -> i32 {
    let guard = audio_state().lock().expect("audio state poisoned");
    let Some(e) = guard.engine.as_ref() else {
        return 0;
    };
    let Some((id, kind)) = e.pop_completed() else {
        return 0;
    };
    if !out_voice_id.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_voice_id = id };
    }
    if !out_kind.is_null() {
        // SAFETY: Caller passed a valid writable pointer.
        unsafe { *out_kind = kind as u32 };
    }
    1
}

/// Decodes an IMA-ADPCM (or any WAV) buffer into a newly allocated 16-bit PCM RIFF/WAVE buffer.
/// Free with `zhr_audio_free_buffer(ptr, len)`. Returns 1 on success, 0 on failure.
///
/// # Safety
/// `bytes` must point to `byte_len` readable bytes; `out_ptr` and `out_len` must be valid writable pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_audio_decode_wav_pcm16(
    bytes: *const u8,
    byte_len: usize,
    out_ptr: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if bytes.is_null() || byte_len == 0 || out_ptr.is_null() || out_len.is_null() {
        return 0;
    }
    // SAFETY: Caller guarantees valid slice and output pointers.
    let slice = unsafe { std::slice::from_raw_parts(bytes, byte_len) };
    let Some(wav) = zhr_audio::decode_to_wav_pcm16(slice) else {
        unsafe {
            *out_ptr = std::ptr::null_mut();
            *out_len = 0;
        }
        return 0;
    };
    let mut boxed = wav.into_boxed_slice();
    let len = boxed.len();
    let ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    // SAFETY: Output pointers verified non-null above.
    unsafe {
        *out_ptr = ptr;
        *out_len = len;
    }
    1
}

/// Frees a buffer previously returned by `zhr_audio_decode_wav_pcm16`.
///
/// # Safety
/// `ptr` and `len` must match a live allocation returned by `zhr_audio_decode_wav_pcm16`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_audio_free_buffer(ptr: *mut u8, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    // SAFETY: Reconstructs the boxed slice forgotten in `zhr_audio_decode_wav_pcm16`.
    let slice_ptr = std::ptr::slice_from_raw_parts_mut(ptr, len);
    let _ = unsafe { Box::from_raw(slice_ptr) };
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_video_start(sample_rate: u32, volume: f32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.video_audio_start(sample_rate, volume);
    }
}

/// Pushes interleaved `f32` PCM samples from an FFmpeg video decoder into `zhr-audio`.
///
/// # Safety
/// `samples` must point to `sample_count` readable `f32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_audio_video_push_f32(
    samples: *const f32,
    sample_count: usize,
    channels: u16,
) {
    if samples.is_null() || sample_count == 0 || channels == 0 {
        return;
    }
    // SAFETY: Caller guarantees `samples` is valid for `sample_count` floats.
    let slice = unsafe { std::slice::from_raw_parts(samples, sample_count) };
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.video_audio_push_f32(slice, channels);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_video_set_volume(volume: f32) {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.video_audio_set_volume(volume);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_audio_video_stop() {
    let guard = audio_state().lock().expect("audio state poisoned");
    if let Some(e) = guard.engine.as_ref() {
        e.video_audio_stop();
    }
}

// ---------------------------------------------------------------------------
// Cross-Platform SDL3 Input Translation (zhr-platform)
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "C" fn zhr_platform_sdl_scancode_to_zh_key(sdl_scancode: u32) -> u8 {
    zhr_platform::sdl_scancode_to_zh_key(sdl_scancode)
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_freeze_time(frozen: bool) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_freeze_time(frozen);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zhr_vk_set_wireframe(enabled: bool) {
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.set_wireframe(enabled);
    }
}

/// Schedules a one-shot PNG (+ optional stats log) capture on the very next `end_frame`.
///
/// # Safety
/// `png_path` must be a valid NUL-terminated C string; `stats_path` may be null or a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_vk_request_capture(
    png_path: *const std::ffi::c_char,
    stats_path: *const std::ffi::c_char,
) {
    if png_path.is_null() {
        return;
    }
    let Ok(png) = unsafe { std::ffi::CStr::from_ptr(png_path) }.to_str() else {
        return;
    };
    if png.is_empty() {
        return;
    }
    let stats = if stats_path.is_null() {
        None
    } else {
        unsafe { std::ffi::CStr::from_ptr(stats_path) }
            .to_str()
            .ok()
            .filter(|s| !s.is_empty())
            .map(ToOwned::to_owned)
    };
    let mut guard = vk_state().lock().expect("vk state poisoned");
    if let Some(ren) = guard.renderer.as_mut() {
        ren.request_capture(png.to_owned(), stats);
    }
}

// ---------------------------------------------------------------------------
// Real-Time Texture Heightmap Synthesis for Smart Tessellation (zhr-remaster)
// ---------------------------------------------------------------------------

/// Synthesizes a seam-safe, alpha-safe `out_dim * out_dim` 8-bit displacement heightmap
/// directly from an original Zero Hour texture buffer (`format_code`: 0=RGBA8, 1=BGRA8,
/// 2=BC1, 3=BC2, 4=BC3).
///
/// # Safety
/// - `data` must point to at least `byte_len` readable bytes.
/// - `out_height_r8` must point to at least `(out_dim.clamp(16, 128)^2)` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn zhr_remaster_build_heightmap(
    width: u32,
    height: u32,
    format_code: u32,
    data: *const u8,
    byte_len: usize,
    out_dim: u32,
    out_height_r8: *mut u8,
) -> i32 {
    if data.is_null() || out_height_r8.is_null() || byte_len == 0 {
        return 0;
    }
    let dim = out_dim.clamp(16, 128) as usize;
    let src = unsafe { std::slice::from_raw_parts(data, byte_len) };
    let Some(hm) = zhr_remaster::synthesize_heightmap_r8(format_code, width, height, src, out_dim)
    else {
        return 0;
    };
    if hm.len() != dim * dim {
        return 0;
    }
    let dst = unsafe { std::slice::from_raw_parts_mut(out_height_r8, dim * dim) };
    dst.copy_from_slice(&hm);
    1
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn c_banner_is_valid_utf8() {
        let ptr = zhr_rust_banner();
        assert!(!ptr.is_null());
        // SAFETY: BANNER_CSTR is a static null-terminated UTF-8 byte slice.
        let s = unsafe { CStr::from_ptr(ptr) }.to_str().expect("valid utf8");
        assert!(s.starts_with("ZeroHourReloaded Rust Core v"));
    }

    #[test]
    fn c_pointer_bits_matches_core() {
        assert_eq!(zhr_rust_pointer_bits(), zhr_core::POINTER_BITS);
    }

    #[test]
    fn c_abi_headless_lifecycle_works() {
        assert_eq!(zhr_vk_init_headless(64, 64, false), 1);
        assert!(zhr_vk_device_count() >= 1);
        assert!(zhr_vk_selected_device() >= 0);
        let name_ptr = zhr_vk_device_name(zhr_vk_selected_device());
        let name = unsafe { CStr::from_ptr(name_ptr) }.to_str().unwrap();
        assert!(!name.is_empty());
        assert_eq!(zhr_vk_begin_frame(), 1);
        assert_eq!(zhr_vk_clear(true, true, 0.1, 0.2, 0.3, 1.0, 1.0, 0), 1);
        assert_eq!(zhr_vk_end_frame(), 1);
        zhr_vk_shutdown();
    }
}
