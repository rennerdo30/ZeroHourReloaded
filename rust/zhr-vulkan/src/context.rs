use std::ffi::{CStr, CString, c_void};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use ash::vk;
use gpu_allocator::vulkan::{Allocator, AllocatorCreateDesc};
use raw_window_handle::RawDisplayHandle;

use crate::error::{Result, VkError};
use crate::types::DeviceInfo;

const VALIDATION_LAYER_NAME: &CStr = c"VK_LAYER_KHRONOS_validation";

/// Owns the Vulkan 1.3 Instance, Debug Messenger, Physical/Logical Device, Graphics Queue, and GPU Allocator.
pub struct VkContext {
    pub entry: ash::Entry,
    pub instance: ash::Instance,
    pub debug_utils: Option<ash::ext::debug_utils::Instance>,
    pub debug_messenger: Option<vk::DebugUtilsMessengerEXT>,
    pub validation_errors: Arc<AtomicUsize>,
    pub validation_active: bool,
    pub physical_device: vk::PhysicalDevice,
    pub device_info: DeviceInfo,
    pub device: ash::Device,
    pub graphics_queue_family: u32,
    pub graphics_queue: vk::Queue,
    pub command_pool: vk::CommandPool,
    pub min_uniform_offset_alignment: u64,
    pub max_anisotropy: f32,
    pub fill_mode_non_solid: bool,
    pub swapchain_colorspace_supported: bool,
    pub hdr_metadata_supported: bool,
    pub depth_format: vk::Format,
    pub allocator: Option<Allocator>,
    /// All enumerated Vulkan 1.3 capable GPUs (for `Renderer::Get_Render_Device_*`).
    pub available_devices: Vec<DeviceInfo>,
    pub selected_device_index: usize,
}

impl VkContext {
    pub fn new(
        app_name: &str,
        enable_validation: bool,
        display_handle: Option<RawDisplayHandle>,
        preferred_device_index: Option<usize>,
    ) -> Result<Self> {
        // SAFETY: Dynamically loads the platform Vulkan loader (`vulkan-1.dll` / `libvulkan.so.1`).
        let entry = unsafe { ash::Entry::load()? };

        let app_cstr = CString::new(app_name).unwrap_or_else(|_| c"ZeroHourReloaded".to_owned());
        let engine_cstr = c"ZeroHourReloaded";
        let app_info = vk::ApplicationInfo::default()
            .application_name(&app_cstr)
            .application_version(vk::make_api_version(0, 0, 1, 0))
            .engine_name(engine_cstr)
            .engine_version(vk::make_api_version(0, 0, 1, 0))
            .api_version(vk::API_VERSION_1_3);

        // SAFETY: Enumerates available instance layers/extensions before creating the instance.
        let available_layers = unsafe { entry.enumerate_instance_layer_properties()? };
        let available_exts = unsafe { entry.enumerate_instance_extension_properties(None)? };

        let has_layer = |name: &CStr| {
            available_layers.iter().any(|l| {
                // SAFETY: Vulkan guarantees null-terminated UTF-8/ASCII layer names.
                unsafe { CStr::from_ptr(l.layer_name.as_ptr()) == name }
            })
        };
        let has_ext = |name: &CStr| {
            available_exts.iter().any(|e| {
                // SAFETY: Vulkan guarantees null-terminated extension names.
                unsafe { CStr::from_ptr(e.extension_name.as_ptr()) == name }
            })
        };

        let mut layers: Vec<*const i8> = Vec::new();
        let mut extensions: Vec<*const i8> = Vec::new();
        let mut validation_active = false;

        if enable_validation && has_layer(VALIDATION_LAYER_NAME) {
            layers.push(VALIDATION_LAYER_NAME.as_ptr());
            validation_active = true;
            if has_ext(ash::ext::debug_utils::NAME) {
                extensions.push(ash::ext::debug_utils::NAME.as_ptr());
            }
        } else if enable_validation {
            log::warn!(
                "Vulkan validation requested, but VK_LAYER_KHRONOS_validation was not found (set VK_ADD_LAYER_PATH)."
            );
        }

        let mut swapchain_colorspace_supported = false;
        if let Some(display) = display_handle {
            extensions.push(ash::khr::surface::NAME.as_ptr());
            for ext in required_surface_extensions(display)? {
                extensions.push(ext.as_ptr());
            }
            if has_ext(ash::ext::swapchain_colorspace::NAME) {
                extensions.push(ash::ext::swapchain_colorspace::NAME.as_ptr());
                swapchain_colorspace_supported = true;
            }
        }

        #[cfg(target_os = "macos")]
        let create_flags = {
            if has_ext(ash::khr::portability_enumeration::NAME) {
                extensions.push(ash::khr::portability_enumeration::NAME.as_ptr());
                vk::InstanceCreateFlags::ENUMERATE_PORTABILITY_KHR
            } else {
                vk::InstanceCreateFlags::empty()
            }
        };
        #[cfg(not(target_os = "macos"))]
        let create_flags = vk::InstanceCreateFlags::empty();

        let validation_errors = Arc::new(AtomicUsize::new(0));
        let mut debug_create_info = vk::DebugUtilsMessengerCreateInfoEXT::default()
            .message_severity(
                vk::DebugUtilsMessageSeverityFlagsEXT::ERROR
                    | vk::DebugUtilsMessageSeverityFlagsEXT::WARNING,
            )
            .message_type(
                vk::DebugUtilsMessageTypeFlagsEXT::GENERAL
                    | vk::DebugUtilsMessageTypeFlagsEXT::VALIDATION
                    | vk::DebugUtilsMessageTypeFlagsEXT::PERFORMANCE,
            )
            .pfn_user_callback(Some(vulkan_debug_callback))
            .user_data(Arc::as_ptr(&validation_errors) as *mut c_void);

        let mut instance_info = vk::InstanceCreateInfo::default()
            .application_info(&app_info)
            .enabled_layer_names(&layers)
            .enabled_extension_names(&extensions)
            .flags(create_flags);

        if validation_active && has_ext(ash::ext::debug_utils::NAME) {
            instance_info = instance_info.push_next(&mut debug_create_info);
        }

        // SAFETY: All pointers in `instance_info` live through the call.
        let instance = unsafe { entry.create_instance(&instance_info, None)? };

        let (debug_utils, debug_messenger) =
            if validation_active && has_ext(ash::ext::debug_utils::NAME) {
                let loader = ash::ext::debug_utils::Instance::new(&entry, &instance);
                // SAFETY: `validation_errors` is kept alive in `VkContext` and destroyed after messenger.
                let messenger =
                    unsafe { loader.create_debug_utils_messenger(&debug_create_info, None)? };
                (Some(loader), Some(messenger))
            } else {
                (None, None)
            };

        // Enumerate physical devices supporting Vulkan 1.3 + graphics queue.
        // SAFETY: `instance` is valid.
        let raw_devices = unsafe { instance.enumerate_physical_devices()? };
        let mut candidates: Vec<(vk::PhysicalDevice, u32, DeviceInfo, i32)> = Vec::new();

        for pd in raw_devices {
            // SAFETY: `pd` belongs to `instance`.
            let props = unsafe { instance.get_physical_device_properties(pd) };
            if props.api_version < vk::API_VERSION_1_3 {
                continue;
            }
            // SAFETY: `pd` belongs to `instance`.
            let qfams = unsafe { instance.get_physical_device_queue_family_properties(pd) };
            let Some(gfx_family) = qfams
                .iter()
                .enumerate()
                .find(|(_, q)| q.queue_flags.contains(vk::QueueFlags::GRAPHICS))
                .map(|(i, _)| i as u32)
            else {
                continue;
            };

            let name = unsafe { CStr::from_ptr(props.device_name.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            let is_discrete = props.device_type == vk::PhysicalDeviceType::DISCRETE_GPU;
            let score = match props.device_type {
                vk::PhysicalDeviceType::DISCRETE_GPU => 1000,
                vk::PhysicalDeviceType::INTEGRATED_GPU => 500,
                vk::PhysicalDeviceType::VIRTUAL_GPU => 250,
                vk::PhysicalDeviceType::CPU => 50,
                _ => 100,
            };
            candidates.push((
                pd,
                gfx_family,
                DeviceInfo {
                    name,
                    is_discrete,
                    api_version: props.api_version,
                    vendor_id: props.vendor_id,
                    device_id: props.device_id,
                },
                score,
            ));
        }

        if candidates.is_empty() {
            return Err(VkError::NoSuitableDevice);
        }

        // Sort best GPU first (discrete GPUs beat integrated GPUs).
        candidates.sort_by_key(|a| std::cmp::Reverse(a.3));
        let selected_device_index = preferred_device_index
            .filter(|&idx| idx < candidates.len())
            .unwrap_or(0);
        let available_devices: Vec<DeviceInfo> = candidates.iter().map(|c| c.2.clone()).collect();
        let (physical_device, graphics_queue_family, device_info, _) =
            candidates.swap_remove(selected_device_index);

        // SAFETY: `physical_device` is valid.
        let props = unsafe { instance.get_physical_device_properties(physical_device) };
        let supported_features = unsafe { instance.get_physical_device_features(physical_device) };
        let min_uniform_offset_alignment = props.limits.min_uniform_buffer_offset_alignment.max(64);
        let anisotropy_supported = supported_features.sampler_anisotropy != 0;
        let fill_mode_non_solid = supported_features.fill_mode_non_solid != 0;
        let max_anisotropy = if anisotropy_supported {
            props.limits.max_sampler_anisotropy.clamp(1.0, 16.0)
        } else {
            1.0
        };
        let depth_format = pick_depth_format(&instance, physical_device);

        let queue_priority = [1.0f32];
        let queue_infos = [vk::DeviceQueueCreateInfo::default()
            .queue_family_index(graphics_queue_family)
            .queue_priorities(&queue_priority)];

        let mut device_exts: Vec<*const i8> = Vec::new();
        let mut hdr_metadata_supported = false;
        if display_handle.is_some() {
            device_exts.push(ash::khr::swapchain::NAME.as_ptr());
            // Enable VK_EXT_hdr_metadata when supported so HDR10 monitors receive real ST.2086 mastering metadata.
            if let Ok(avail_dev_exts) = unsafe {
                instance.enumerate_device_extension_properties(physical_device)
            } {
                let has_dev_ext = |name: &CStr| {
                    avail_dev_exts.iter().any(|e| {
                        // SAFETY: Vulkan guarantees null-terminated extension names.
                        unsafe { CStr::from_ptr(e.extension_name.as_ptr()) == name }
                    })
                };
                if has_dev_ext(ash::ext::hdr_metadata::NAME) {
                    device_exts.push(ash::ext::hdr_metadata::NAME.as_ptr());
                    hdr_metadata_supported = true;
                }
            }
        }

        let enabled_features = vk::PhysicalDeviceFeatures::default()
            .sampler_anisotropy(anisotropy_supported)
            .fill_mode_non_solid(fill_mode_non_solid);

        let mut features13 = vk::PhysicalDeviceVulkan13Features::default()
            .dynamic_rendering(true)
            .synchronization2(true);

        let device_info_vk = vk::DeviceCreateInfo::default()
            .queue_create_infos(&queue_infos)
            .enabled_extension_names(&device_exts)
            .enabled_features(&enabled_features)
            .push_next(&mut features13);

        // SAFETY: `physical_device` supports Vulkan 1.3 dynamic rendering and sync2.
        let device = unsafe { instance.create_device(physical_device, &device_info_vk, None)? };
        // SAFETY: Queue index 0 was requested on `graphics_queue_family`.
        let graphics_queue = unsafe { device.get_device_queue(graphics_queue_family, 0) };

        let pool_info = vk::CommandPoolCreateInfo::default()
            .queue_family_index(graphics_queue_family)
            .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER);
        // SAFETY: `device` and `graphics_queue_family` are valid.
        let command_pool = unsafe { device.create_command_pool(&pool_info, None)? };

        let allocator = Allocator::new(&AllocatorCreateDesc {
            instance: instance.clone(),
            device: device.clone(),
            physical_device,
            debug_settings: Default::default(),
            buffer_device_address: false,
            allocation_sizes: Default::default(),
        })?;

        log::info!(
            "Vulkan 1.3 initialized on '{}' (discrete={}, validation={})",
            device_info.name,
            device_info.is_discrete,
            validation_active
        );

        Ok(Self {
            entry,
            instance,
            debug_utils,
            debug_messenger,
            validation_errors,
            validation_active,
            physical_device,
            device_info,
            device,
            graphics_queue_family,
            graphics_queue,
            command_pool,
            min_uniform_offset_alignment,
            max_anisotropy,
            fill_mode_non_solid,
            swapchain_colorspace_supported,
            hdr_metadata_supported,
            depth_format,
            allocator: Some(allocator),
            available_devices,
            selected_device_index,
        })
    }

    pub fn allocator_mut(&mut self) -> &mut Allocator {
        self.allocator
            .as_mut()
            .expect("GPU allocator dropped early")
    }

    pub fn validation_error_count(&self) -> usize {
        self.validation_errors.load(Ordering::Relaxed)
    }

    /// Executes a one-shot command buffer synchronously on the graphics queue (used for uploads/readbacks).
    pub fn submit_immediate<F>(&self, record: F) -> Result<()>
    where
        F: FnOnce(&ash::Device, vk::CommandBuffer),
    {
        let alloc_info = vk::CommandBufferAllocateInfo::default()
            .command_pool(self.command_pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        // SAFETY: `command_pool` belongs to `device`.
        let cmd = unsafe { self.device.allocate_command_buffers(&alloc_info)?[0] };

        let begin = vk::CommandBufferBeginInfo::default()
            .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
        // SAFETY: `cmd` is freshly allocated and in initial state.
        unsafe {
            self.device.begin_command_buffer(cmd, &begin)?;
            record(&self.device, cmd);
            self.device.end_command_buffer(cmd)?;
        }

        let fence = unsafe {
            self.device
                .create_fence(&vk::FenceCreateInfo::default(), None)?
        };
        let cmd_bufs = [cmd];
        let submit = [vk::SubmitInfo::default().command_buffers(&cmd_bufs)];
        // SAFETY: `cmd` is recorded, `fence` is unsignaled, and we wait before freeing.
        unsafe {
            self.device
                .queue_submit(self.graphics_queue, &submit, fence)?;
            self.device.wait_for_fences(&[fence], true, u64::MAX)?;
            self.device.destroy_fence(fence, None);
            self.device
                .free_command_buffers(self.command_pool, &cmd_bufs);
        }
        Ok(())
    }
}

impl Drop for VkContext {
    fn drop(&mut self) {
        // SAFETY: Destroy resources in reverse creation order after device idle.
        unsafe {
            let _ = self.device.device_wait_idle();
            // Drop gpu-allocator BEFORE destroying the Vulkan device.
            drop(self.allocator.take());
            self.device.destroy_command_pool(self.command_pool, None);
            self.device.destroy_device(None);
            if let (Some(loader), Some(messenger)) =
                (self.debug_utils.take(), self.debug_messenger.take())
            {
                loader.destroy_debug_utils_messenger(messenger, None);
            }
            self.instance.destroy_instance(None);
        }
    }
}

fn pick_depth_format(instance: &ash::Instance, pd: vk::PhysicalDevice) -> vk::Format {
    let candidates = [
        vk::Format::D32_SFLOAT_S8_UINT,
        vk::Format::D24_UNORM_S8_UINT,
        vk::Format::D32_SFLOAT,
    ];
    for fmt in candidates {
        // SAFETY: `pd` belongs to `instance`.
        let props = unsafe { instance.get_physical_device_format_properties(pd, fmt) };
        if props
            .optimal_tiling_features
            .contains(vk::FormatFeatureFlags::DEPTH_STENCIL_ATTACHMENT)
        {
            return fmt;
        }
    }
    vk::Format::D32_SFLOAT
}

fn required_surface_extensions(display: RawDisplayHandle) -> Result<Vec<&'static CStr>> {
    match display {
        #[cfg(target_os = "windows")]
        RawDisplayHandle::Windows(_) => Ok(vec![ash::khr::win32_surface::NAME]),
        #[cfg(target_os = "linux")]
        RawDisplayHandle::Xlib(_) => Ok(vec![ash::khr::xlib_surface::NAME]),
        #[cfg(target_os = "linux")]
        RawDisplayHandle::Xcb(_) => Ok(vec![ash::khr::xcb_surface::NAME]),
        #[cfg(target_os = "linux")]
        RawDisplayHandle::Wayland(_) => Ok(vec![ash::khr::wayland_surface::NAME]),
        #[cfg(target_os = "macos")]
        RawDisplayHandle::AppKit(_) => Ok(vec![ash::ext::metal_surface::NAME]),
        _ => Err(VkError::UnsupportedWindowHandle),
    }
}

unsafe extern "system" fn vulkan_debug_callback(
    severity: vk::DebugUtilsMessageSeverityFlagsEXT,
    _msg_type: vk::DebugUtilsMessageTypeFlagsEXT,
    callback_data: *const vk::DebugUtilsMessengerCallbackDataEXT<'_>,
    user_data: *mut c_void,
) -> vk::Bool32 {
    // SAFETY: Vulkan passes a valid callback_data pointer and our Arc counter pointer.
    let msg = if !callback_data.is_null() {
        let data = unsafe { &*callback_data };
        if !data.p_message.is_null() {
            unsafe { CStr::from_ptr(data.p_message) }.to_string_lossy()
        } else {
            std::borrow::Cow::Borrowed("<null message>")
        }
    } else {
        std::borrow::Cow::Borrowed("<null callback_data>")
    };

    if severity.contains(vk::DebugUtilsMessageSeverityFlagsEXT::ERROR) {
        if !user_data.is_null() {
            // SAFETY: `user_data` points to `VkContext::validation_errors`.
            let counter = unsafe { &*(user_data as *const AtomicUsize) };
            counter.fetch_add(1, Ordering::Relaxed);
        }
        log::error!("[Vulkan Validation ERROR] {msg}");
        eprintln!("[Vulkan Validation ERROR] {msg}");
        if let Ok(p) = std::env::var("ZHR_VK_TRACE_LOG") {
            let line = format!("[VALIDATION ERROR] {msg}\n");
            let _ = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(p)
                .and_then(|mut f| std::io::Write::write_all(&mut f, line.as_bytes()));
        }
    } else if severity.contains(vk::DebugUtilsMessageSeverityFlagsEXT::WARNING) {
        log::warn!("[Vulkan Validation WARN] {msg}");
    }

    vk::FALSE
}
