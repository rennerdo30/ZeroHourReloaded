use ash::vk;
use gpu_allocator::vulkan::Allocation;
use raw_window_handle::{RawDisplayHandle, RawWindowHandle};

use crate::context::VkContext;
use crate::error::{Result, VkError};
use crate::offscreen::{create_attachment, create_depth_sample_view};

/// Active swapchain HDR output encoding mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum DisplayHdrMode {
    /// Standard 8-bit sRGB output (`B8G8R8A8_UNORM` + `SRGB_NONLINEAR`).
    Sdr = 0,
    /// Native 10-bit HDR10 PQ output (`A2B10G10R10_UNORM_PACK32` + `HDR10_ST2084_EXT`, Rec.2020).
    Hdr10Pq = 1,
    /// Native 16-bit float scRGB linear HDR output (`R16G16B16A16_SFLOAT` + `EXTENDED_SRGB_LINEAR_EXT`).
    ScRgbLinear = 2,
}

#[derive(Clone, Copy, Debug)]
pub struct HdrDisplayConfig {
    pub mode: DisplayHdrMode,
    pub max_luminance_nits: f32,
    pub paper_white_nits: f32,
}

impl Default for HdrDisplayConfig {
    fn default() -> Self {
        Self {
            mode: DisplayHdrMode::Sdr,
            max_luminance_nits: 250.0,
            paper_white_nits: 200.0,
        }
    }
}

/// Cross-platform Vulkan surface + swapchain + depth attachment.
pub struct SwapchainState {
    pub surface_loader: ash::khr::surface::Instance,
    pub surface: vk::SurfaceKHR,
    pub swapchain_loader: ash::khr::swapchain::Device,
    pub swapchain: vk::SwapchainKHR,
    pub format: vk::Format,
    pub color_space: vk::ColorSpaceKHR,
    pub hdr: HdrDisplayConfig,
    pub extent: vk::Extent2D,
    pub images: Vec<vk::Image>,
    pub views: Vec<vk::ImageView>,
    pub render_finished: Vec<vk::Semaphore>,
    pub depth_image: vk::Image,
    pub depth_view: vk::ImageView,
    pub depth_sample_view: vk::ImageView,
    depth_alloc: Option<Allocation>,
    pub vsync: bool,
    window_hwnd: isize,
}

impl SwapchainState {
    pub fn new(
        ctx: &mut VkContext,
        display: RawDisplayHandle,
        window: RawWindowHandle,
        width: u32,
        height: u32,
        vsync: bool,
    ) -> Result<Self> {
        let surface_loader = ash::khr::surface::Instance::new(&ctx.entry, &ctx.instance);
        let surface = create_platform_surface(&ctx.entry, &ctx.instance, display, window)?;
        let swapchain_loader = ash::khr::swapchain::Device::new(&ctx.instance, &ctx.device);

        // SAFETY: `surface` and `physical_device` belong to `ctx.instance`.
        let supported = unsafe {
            surface_loader.get_physical_device_surface_support(
                ctx.physical_device,
                ctx.graphics_queue_family,
                surface,
            )?
        };
        if !supported {
            return Err(VkError::State(
                "selected graphics queue does not support presentation on this surface",
            ));
        }

        let window_hwnd = match window {
            RawWindowHandle::Win32(win) => win.hwnd.get(),
            _ => 0,
        };

        let built = build_swapchain(
            ctx,
            &surface_loader,
            &swapchain_loader,
            surface,
            width,
            height,
            vsync,
            window_hwnd,
            vk::SwapchainKHR::null(),
        )?;

        let (depth_image, depth_view, depth_alloc) = create_attachment(
            ctx,
            "zhr_swapchain_depth",
            built.extent.width,
            built.extent.height,
            ctx.depth_format,
            vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT | vk::ImageUsageFlags::SAMPLED,
            vk::ImageAspectFlags::DEPTH,
        )?;
        let depth_sample_view =
            create_depth_sample_view(&ctx.device, depth_image, ctx.depth_format)?;

        let mut render_finished = Vec::with_capacity(built.images.len());
        let sem_info = vk::SemaphoreCreateInfo::default();
        for _ in 0..built.images.len() {
            // SAFETY: `ctx.device` is valid.
            render_finished.push(unsafe { ctx.device.create_semaphore(&sem_info, None)? });
        }

        Ok(Self {
            surface_loader,
            surface,
            swapchain_loader,
            swapchain: built.swapchain,
            format: built.format,
            color_space: built.color_space,
            hdr: built.hdr,
            extent: built.extent,
            images: built.images,
            views: built.views,
            render_finished,
            depth_image,
            depth_view,
            depth_sample_view,
            depth_alloc: Some(depth_alloc),
            vsync,
            window_hwnd,
        })
    }

    pub fn resize(&mut self, ctx: &mut VkContext, width: u32, height: u32) -> Result<()> {
        if width == 0 || height == 0 {
            return Ok(());
        }
        // SAFETY: Wait for GPU idle before recreating swapchain and depth attachment.
        unsafe { ctx.device.device_wait_idle()? };

        let old_swapchain = self.swapchain;
        let built = build_swapchain(
            ctx,
            &self.surface_loader,
            &self.swapchain_loader,
            self.surface,
            width,
            height,
            self.vsync,
            self.window_hwnd,
            old_swapchain,
        )?;

        // SAFETY: Old views/semaphores/swapchain/depth are idle.
        unsafe {
            for view in self.views.drain(..) {
                ctx.device.destroy_image_view(view, None);
            }
            for sem in self.render_finished.drain(..) {
                ctx.device.destroy_semaphore(sem, None);
            }
            self.swapchain_loader.destroy_swapchain(old_swapchain, None);
            ctx.device.destroy_image_view(self.depth_sample_view, None);
            ctx.device.destroy_image_view(self.depth_view, None);
            ctx.device.destroy_image(self.depth_image, None);
        }
        if let Some(a) = self.depth_alloc.take() {
            let _ = ctx.allocator_mut().free(a);
        }

        let (depth_image, depth_view, depth_alloc) = create_attachment(
            ctx,
            "zhr_swapchain_depth",
            built.extent.width,
            built.extent.height,
            ctx.depth_format,
            vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT | vk::ImageUsageFlags::SAMPLED,
            vk::ImageAspectFlags::DEPTH,
        )?;
        let depth_sample_view =
            create_depth_sample_view(&ctx.device, depth_image, ctx.depth_format)?;

        let sem_info = vk::SemaphoreCreateInfo::default();
        for _ in 0..built.images.len() {
            // SAFETY: `ctx.device` is valid.
            self.render_finished
                .push(unsafe { ctx.device.create_semaphore(&sem_info, None)? });
        }

        self.swapchain = built.swapchain;
        self.format = built.format;
        self.color_space = built.color_space;
        self.hdr = built.hdr;
        self.extent = built.extent;
        self.images = built.images;
        self.views = built.views;
        self.depth_image = depth_image;
        self.depth_view = depth_view;
        self.depth_sample_view = depth_sample_view;
        self.depth_alloc = Some(depth_alloc);
        Ok(())
    }

    pub fn destroy(mut self, ctx: &mut VkContext) {
        // SAFETY: Caller ensures GPU is idle.
        unsafe {
            for view in self.views.drain(..) {
                ctx.device.destroy_image_view(view, None);
            }
            for sem in self.render_finished.drain(..) {
                ctx.device.destroy_semaphore(sem, None);
            }
            self.swapchain_loader
                .destroy_swapchain(self.swapchain, None);
            ctx.device.destroy_image_view(self.depth_sample_view, None);
            ctx.device.destroy_image_view(self.depth_view, None);
            ctx.device.destroy_image(self.depth_image, None);
            self.surface_loader.destroy_surface(self.surface, None);
        }
        if let Some(a) = self.depth_alloc.take() {
            let _ = ctx.allocator_mut().free(a);
        }
    }
}

struct BuiltSwapchain {
    swapchain: vk::SwapchainKHR,
    format: vk::Format,
    color_space: vk::ColorSpaceKHR,
    hdr: HdrDisplayConfig,
    extent: vk::Extent2D,
    images: Vec<vk::Image>,
    views: Vec<vk::ImageView>,
}

/// Queries Windows DXGI (`IDXGIOutput6::GetDesc1`) to determine whether the display hosting `hwnd`
/// currently has Windows HDR enabled (`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`), returning
/// `Some(max_luminance_nits)` when active and `None` on SDR displays.
#[cfg(target_os = "windows")]
fn detect_windows_hdr_max_nits(hwnd: isize) -> Option<f32> {
    use std::ffi::c_void;

    #[repr(C)]
    struct Guid {
        d1: u32,
        d2: u16,
        d3: u16,
        d4: [u8; 8],
    }

    #[repr(C)]
    struct DxgiOutputDesc {
        device_name: [u16; 32],
        desktop_coordinates: [i32; 4],
        attached_to_desktop: i32,
        rotation: u32,
        monitor: isize,
    }

    #[repr(C)]
    struct DxgiOutputDesc1 {
        device_name: [u16; 32],
        desktop_coordinates: [i32; 4],
        attached_to_desktop: i32,
        rotation: u32,
        monitor: isize,
        bits_per_color: u32,
        color_space: u32,
        red_primary: [f32; 2],
        green_primary: [f32; 2],
        blue_primary: [f32; 2],
        white_point: [f32; 2],
        min_luminance: f32,
        max_luminance: f32,
        max_full_frame_luminance: f32,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn LoadLibraryA(name: *const u8) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, proc_name: *const u8) -> *const c_void;
        fn FreeLibrary(module: *mut c_void) -> i32;
    }

    #[link(name = "user32")]
    unsafe extern "system" {
        fn MonitorFromWindow(hwnd: isize, flags: u32) -> isize;
    }

    const MONITOR_DEFAULTTONEAREST: u32 = 2;
    const DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: u32 = 12;

    const IID_IDXGIFACTORY1: Guid = Guid {
        d1: 0x770a_ae78,
        d2: 0xf26f,
        d3: 0x4dba,
        d4: [0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87],
    };
    const IID_IDXGIOUTPUT6: Guid = Guid {
        d1: 0x0683_46e8,
        d2: 0xaaec,
        d3: 0x4b84,
        d4: [0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1],
    };

    type CreateDxgiFactory1Fn = unsafe extern "system" fn(*const Guid, *mut *mut c_void) -> i32;
    type QiFn = unsafe extern "system" fn(*mut c_void, *const Guid, *mut *mut c_void) -> i32;
    type ReleaseFn = unsafe extern "system" fn(*mut c_void) -> u32;
    type EnumAdapters1Fn = unsafe extern "system" fn(*mut c_void, u32, *mut *mut c_void) -> i32;
    type EnumOutputsFn = unsafe extern "system" fn(*mut c_void, u32, *mut *mut c_void) -> i32;
    type GetDescFn = unsafe extern "system" fn(*mut c_void, *mut DxgiOutputDesc) -> i32;
    type GetDesc1Fn = unsafe extern "system" fn(*mut c_void, *mut DxgiOutputDesc1) -> i32;

    unsafe {
        let target_monitor = if hwnd != 0 {
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
        } else {
            0
        };
        let dxgi = LoadLibraryA(c"dxgi.dll".as_ptr().cast());
        if dxgi.is_null() {
            return None;
        }
        let proc = GetProcAddress(dxgi, c"CreateDXGIFactory1".as_ptr().cast());
        if proc.is_null() {
            FreeLibrary(dxgi);
            return None;
        }
        let create_factory: CreateDxgiFactory1Fn = std::mem::transmute(proc);
        let mut factory: *mut c_void = std::ptr::null_mut();
        if create_factory(&IID_IDXGIFACTORY1, &mut factory) < 0 || factory.is_null() {
            FreeLibrary(dxgi);
            return None;
        }

        let factory_vt = *(factory as *const *const *const c_void);
        let enum_adapters1: EnumAdapters1Fn = std::mem::transmute(*factory_vt.add(12));
        let release_factory: ReleaseFn = std::mem::transmute(*factory_vt.add(2));

        let mut detected_nits: Option<f32> = None;
        let mut ai: u32 = 0;
        loop {
            let mut adapter: *mut c_void = std::ptr::null_mut();
            if enum_adapters1(factory, ai, &mut adapter) < 0 || adapter.is_null() {
                break;
            }
            let adapter_vt = *(adapter as *const *const *const c_void);
            let enum_outputs: EnumOutputsFn = std::mem::transmute(*adapter_vt.add(7));
            let release_adapter: ReleaseFn = std::mem::transmute(*adapter_vt.add(2));

            let mut oi: u32 = 0;
            loop {
                let mut output: *mut c_void = std::ptr::null_mut();
                if enum_outputs(adapter, oi, &mut output) < 0 || output.is_null() {
                    break;
                }
                let output_vt = *(output as *const *const *const c_void);
                let qi: QiFn = std::mem::transmute(*output_vt.add(0));
                let get_desc: GetDescFn = std::mem::transmute(*output_vt.add(7));
                let release_output: ReleaseFn = std::mem::transmute(*output_vt.add(2));

                let mut desc: DxgiOutputDesc = std::mem::zeroed();
                if get_desc(output, &mut desc) >= 0
                    && (target_monitor == 0 || desc.monitor == target_monitor)
                {
                    let mut out6: *mut c_void = std::ptr::null_mut();
                    if qi(output, &IID_IDXGIOUTPUT6, &mut out6) >= 0 && !out6.is_null() {
                        let out6_vt = *(out6 as *const *const *const c_void);
                        let get_desc1: GetDesc1Fn = std::mem::transmute(*out6_vt.add(27));
                        let release_out6: ReleaseFn = std::mem::transmute(*out6_vt.add(2));
                        let mut desc1: DxgiOutputDesc1 = std::mem::zeroed();
                        if get_desc1(out6, &mut desc1) >= 0
                            && desc1.color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                        {
                            let peak = if desc1.max_luminance.is_finite() && desc1.max_luminance >= 250.0
                            {
                                desc1.max_luminance.clamp(300.0, 4000.0)
                            } else {
                                1000.0
                            };
                            detected_nits = Some(peak);
                        }
                        release_out6(out6);
                    }
                    release_output(output);
                    if detected_nits.is_some() || target_monitor != 0 {
                        break;
                    }
                } else {
                    release_output(output);
                }
                oi += 1;
            }
            release_adapter(adapter);
            if detected_nits.is_some() {
                break;
            }
            ai += 1;
        }
        release_factory(factory);
        FreeLibrary(dxgi);
        detected_nits
    }
}

#[cfg(not(target_os = "windows"))]
fn detect_windows_hdr_max_nits(_hwnd: isize) -> Option<f32> {
    None
}

#[allow(clippy::too_many_arguments)]
fn build_swapchain(
    ctx: &VkContext,
    surface_loader: &ash::khr::surface::Instance,
    swapchain_loader: &ash::khr::swapchain::Device,
    surface: vk::SurfaceKHR,
    width: u32,
    height: u32,
    vsync: bool,
    window_hwnd: isize,
    old_swapchain: vk::SwapchainKHR,
) -> Result<BuiltSwapchain> {
    // SAFETY: Surface and physical device are valid.
    let caps = unsafe {
        surface_loader.get_physical_device_surface_capabilities(ctx.physical_device, surface)?
    };
    let formats = unsafe {
        surface_loader.get_physical_device_surface_formats(ctx.physical_device, surface)?
    };
    let present_modes = unsafe {
        surface_loader.get_physical_device_surface_present_modes(ctx.physical_device, surface)?
    };

    // Determine whether to enable native hardware HDR output:
    // - `ZHR_HDR_OUTPUT=0|sdr|off` -> force SDR
    // - `ZHR_HDR_OUTPUT=1|hdr10`   -> force HDR10 PQ (if surface supports it)
    // - `ZHR_HDR_OUTPUT=scrgb`     -> force 16-bit float scRGB linear (if surface supports it)
    // - default (`auto`/unset)     -> auto-enable HDR10/scRGB whenever Windows HDR is active on the monitor
    let env_hdr = std::env::var("ZHR_HDR_OUTPUT").ok().unwrap_or_default();
    let env_hdr_l = env_hdr.trim().to_ascii_lowercase();
    let want_hdr: Option<(bool, f32)> = if !ctx.swapchain_colorspace_supported {
        None
    } else if matches!(env_hdr_l.as_str(), "0" | "false" | "off" | "sdr") {
        None
    } else if env_hdr_l == "scrgb" {
        Some((true, detect_windows_hdr_max_nits(window_hwnd).unwrap_or(1000.0)))
    } else if matches!(env_hdr_l.as_str(), "1" | "true" | "on" | "hdr" | "hdr10") {
        Some((false, detect_windows_hdr_max_nits(window_hwnd).unwrap_or(1000.0)))
    } else {
        detect_windows_hdr_max_nits(window_hwnd).map(|nits| (false, nits))
    };

    let paper_white_nits = std::env::var("ZHR_HDR_PAPER_WHITE")
        .ok()
        .and_then(|s| s.parse::<f32>().ok())
        .map(|v| v.clamp(100.0, 500.0))
        .unwrap_or(240.0);

    let find_hdr10 = || {
        formats.iter().copied().find(|f| {
            f.format == vk::Format::A2B10G10R10_UNORM_PACK32
                && f.color_space == vk::ColorSpaceKHR::HDR10_ST2084_EXT
        })
    };
    let find_scrgb = || {
        formats.iter().copied().find(|f| {
            f.format == vk::Format::R16G16B16A16_SFLOAT
                && f.color_space == vk::ColorSpaceKHR::EXTENDED_SRGB_LINEAR_EXT
        })
    };

    let (surface_format, hdr) = if let Some((prefer_scrgb, max_nits)) = want_hdr {
        let chosen = if prefer_scrgb {
            find_scrgb().or_else(find_hdr10)
        } else {
            find_hdr10().or_else(find_scrgb)
        };
        if let Some(sf) = chosen {
            let mode = if sf.color_space == vk::ColorSpaceKHR::HDR10_ST2084_EXT {
                DisplayHdrMode::Hdr10Pq
            } else {
                DisplayHdrMode::ScRgbLinear
            };
            log::info!(
                "Vulkan real HDR swapchain enabled: {:?} {:?} (max={:.0} nits, paper_white={:.0} nits)",
                sf.format,
                sf.color_space,
                max_nits,
                paper_white_nits
            );
            (
                sf,
                HdrDisplayConfig {
                    mode,
                    max_luminance_nits: max_nits,
                    paper_white_nits,
                },
            )
        } else {
            let sf = formats
                .iter()
                .copied()
                .find(|f| {
                    f.format == vk::Format::B8G8R8A8_UNORM
                        && f.color_space == vk::ColorSpaceKHR::SRGB_NONLINEAR
                })
                .or_else(|| {
                    formats
                        .iter()
                        .copied()
                        .find(|f| f.format == vk::Format::R8G8B8A8_UNORM)
                })
                .unwrap_or(formats[0]);
            (sf, HdrDisplayConfig::default())
        }
    } else {
        let sf = formats
            .iter()
            .copied()
            .find(|f| {
                f.format == vk::Format::B8G8R8A8_UNORM
                    && f.color_space == vk::ColorSpaceKHR::SRGB_NONLINEAR
            })
            .or_else(|| {
                formats
                    .iter()
                    .copied()
                    .find(|f| f.format == vk::Format::R8G8B8A8_UNORM)
            })
            .unwrap_or(formats[0]);
        (sf, HdrDisplayConfig::default())
    };

    let present_mode = if vsync {
        vk::PresentModeKHR::FIFO
    } else if present_modes.contains(&vk::PresentModeKHR::MAILBOX) {
        vk::PresentModeKHR::MAILBOX
    } else if present_modes.contains(&vk::PresentModeKHR::IMMEDIATE) {
        vk::PresentModeKHR::IMMEDIATE
    } else {
        vk::PresentModeKHR::FIFO
    };

    let extent = if caps.current_extent.width != u32::MAX {
        caps.current_extent
    } else {
        vk::Extent2D {
            width: width.clamp(caps.min_image_extent.width, caps.max_image_extent.width),
            height: height.clamp(caps.min_image_extent.height, caps.max_image_extent.height),
        }
    };

    let mut image_count = caps.min_image_count + 1;
    if caps.max_image_count > 0 && image_count > caps.max_image_count {
        image_count = caps.max_image_count;
    }

    let create_info = vk::SwapchainCreateInfoKHR::default()
        .surface(surface)
        .min_image_count(image_count)
        .image_format(surface_format.format)
        .image_color_space(surface_format.color_space)
        .image_extent(extent)
        .image_array_layers(1)
        .image_usage(vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::TRANSFER_SRC)
        .image_sharing_mode(vk::SharingMode::EXCLUSIVE)
        .pre_transform(caps.current_transform)
        .composite_alpha(vk::CompositeAlphaFlagsKHR::OPAQUE)
        .present_mode(present_mode)
        .clipped(true)
        .old_swapchain(old_swapchain);

    // SAFETY: `create_info` matches queried surface capabilities.
    let swapchain = unsafe { swapchain_loader.create_swapchain(&create_info, None)? };

    // Publish SMPTE ST.2086 mastering display metadata for native HDR10 displays when supported.
    if hdr.mode == DisplayHdrMode::Hdr10Pq && ctx.hdr_metadata_supported {
        let hdr_loader = ash::ext::hdr_metadata::Device::new(&ctx.instance, &ctx.device);
        let metadata = vk::HdrMetadataEXT::default()
            .display_primary_red(vk::XYColorEXT { x: 0.708, y: 0.292 })
            .display_primary_green(vk::XYColorEXT { x: 0.170, y: 0.797 })
            .display_primary_blue(vk::XYColorEXT { x: 0.131, y: 0.046 })
            .white_point(vk::XYColorEXT {
                x: 0.3127,
                y: 0.3290,
            })
            .max_luminance(hdr.max_luminance_nits)
            .min_luminance(0.005)
            .max_content_light_level(hdr.max_luminance_nits)
            .max_frame_average_light_level(hdr.paper_white_nits);
        // SAFETY: `swapchain` is valid and `VK_EXT_hdr_metadata` is enabled on `ctx.device`.
        unsafe {
            hdr_loader.set_hdr_metadata(&[swapchain], &[metadata]);
        }
    }

    let images = unsafe { swapchain_loader.get_swapchain_images(swapchain)? };

    let mut views = Vec::with_capacity(images.len());
    for &img in &images {
        let view_info = vk::ImageViewCreateInfo::default()
            .image(img)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(surface_format.format)
            .subresource_range(
                vk::ImageSubresourceRange::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .base_mip_level(0)
                    .level_count(1)
                    .base_array_layer(0)
                    .layer_count(1),
            );
        // SAFETY: `img` is a valid swapchain image.
        views.push(unsafe { ctx.device.create_image_view(&view_info, None)? });
    }

    Ok(BuiltSwapchain {
        swapchain,
        format: surface_format.format,
        color_space: surface_format.color_space,
        hdr,
        extent,
        images,
        views,
    })
}

fn create_platform_surface(
    entry: &ash::Entry,
    instance: &ash::Instance,
    display: RawDisplayHandle,
    window: RawWindowHandle,
) -> Result<vk::SurfaceKHR> {
    match (display, window) {
        #[cfg(target_os = "windows")]
        (RawDisplayHandle::Windows(_), RawWindowHandle::Win32(win)) => {
            let loader = ash::khr::win32_surface::Instance::new(entry, instance);
            let hinstance = win.hinstance.map(|h| h.get() as vk::HINSTANCE).unwrap_or(0);
            let info = vk::Win32SurfaceCreateInfoKHR::default()
                .hinstance(hinstance)
                .hwnd(win.hwnd.get() as vk::HWND);
            // SAFETY: Caller guarantees the Win32 HWND outlives the Vulkan surface.
            Ok(unsafe { loader.create_win32_surface(&info, None)? })
        }
        #[cfg(target_os = "linux")]
        (RawDisplayHandle::Xlib(dpy), RawWindowHandle::Xlib(win)) => {
            let loader = ash::khr::xlib_surface::Instance::new(entry, instance);
            let info = vk::XlibSurfaceCreateInfoKHR::default()
                .dpy(
                    dpy.display
                        .map(|p| p.as_ptr().cast())
                        .unwrap_or(std::ptr::null_mut()),
                )
                .window(win.window);
            // SAFETY: Caller guarantees Xlib display/window outlive the surface.
            Ok(unsafe { loader.create_xlib_surface(&info, None)? })
        }
        #[cfg(target_os = "linux")]
        (RawDisplayHandle::Xcb(dpy), RawWindowHandle::Xcb(win)) => {
            let loader = ash::khr::xcb_surface::Instance::new(entry, instance);
            let info = vk::XcbSurfaceCreateInfoKHR::default()
                .connection(
                    dpy.connection
                        .map(|p| p.as_ptr().cast())
                        .unwrap_or(std::ptr::null_mut()),
                )
                .window(win.window.get());
            // SAFETY: Caller guarantees XCB connection/window outlive the surface.
            Ok(unsafe { loader.create_xcb_surface(&info, None)? })
        }
        #[cfg(target_os = "linux")]
        (RawDisplayHandle::Wayland(dpy), RawWindowHandle::Wayland(win)) => {
            let loader = ash::khr::wayland_surface::Instance::new(entry, instance);
            let info = vk::WaylandSurfaceCreateInfoKHR::default()
                .display(dpy.display.as_ptr().cast())
                .surface(win.surface.as_ptr().cast());
            // SAFETY: Caller guarantees Wayland display/surface outlive the Vulkan surface.
            Ok(unsafe { loader.create_wayland_surface(&info, None)? })
        }
        #[cfg(target_os = "macos")]
        (RawDisplayHandle::AppKit(_), RawWindowHandle::AppKit(win)) => {
            let loader = ash::ext::metal_surface::Instance::new(entry, instance);
            let info = vk::MetalSurfaceCreateInfoEXT::default().layer(win.ns_view.as_ptr().cast());
            // SAFETY: Caller guarantees NSView/CAMetalLayer outlives the surface.
            Ok(unsafe { loader.create_metal_surface(&info, None)? })
        }
        _ => Err(VkError::UnsupportedWindowHandle),
    }
}
