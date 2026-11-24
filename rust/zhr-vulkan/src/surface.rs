use ash::vk;
use gpu_allocator::vulkan::Allocation;
use raw_window_handle::{RawDisplayHandle, RawWindowHandle};

use crate::context::VkContext;
use crate::error::{Result, VkError};
use crate::offscreen::{create_attachment, create_depth_sample_view};

/// Cross-platform Vulkan surface + swapchain + depth attachment.
pub struct SwapchainState {
    pub surface_loader: ash::khr::surface::Instance,
    pub surface: vk::SurfaceKHR,
    pub swapchain_loader: ash::khr::swapchain::Device,
    pub swapchain: vk::SwapchainKHR,
    pub format: vk::Format,
    pub extent: vk::Extent2D,
    pub images: Vec<vk::Image>,
    pub views: Vec<vk::ImageView>,
    pub render_finished: Vec<vk::Semaphore>,
    pub depth_image: vk::Image,
    pub depth_view: vk::ImageView,
    pub depth_sample_view: vk::ImageView,
    depth_alloc: Option<Allocation>,
    pub vsync: bool,
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

        let built = build_swapchain(
            ctx,
            &surface_loader,
            &swapchain_loader,
            surface,
            width,
            height,
            vsync,
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
            extent: built.extent,
            images: built.images,
            views: built.views,
            render_finished,
            depth_image,
            depth_view,
            depth_sample_view,
            depth_alloc: Some(depth_alloc),
            vsync,
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
    extent: vk::Extent2D,
    images: Vec<vk::Image>,
    views: Vec<vk::ImageView>,
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

    let surface_format = formats
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
