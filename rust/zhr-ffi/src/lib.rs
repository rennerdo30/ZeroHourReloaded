//! C-ABI entry points for linking ZeroHourReloaded Rust subsystems (including Vulkan) into C++.

use std::ffi::{CString, c_char, c_void};
use std::sync::{Mutex, OnceLock};

use glam::{Mat4, Vec3};
use zhr_vulkan::shader::ShaderKey;
use zhr_vulkan::types::{
    ClearState, FogState, GpuLight, SamplerDesc, TextureFormat, TextureId, VertexWw3d, Viewport,
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
