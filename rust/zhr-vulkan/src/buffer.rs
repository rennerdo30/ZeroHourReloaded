use ash::vk;
use gpu_allocator::MemoryLocation;
use gpu_allocator::vulkan::{Allocation, AllocationCreateDesc, AllocationScheme};

use crate::context::VkContext;
use crate::error::{Result, VkError};

/// GPU buffer backed by `gpu-allocator`.
pub struct GpuBuffer {
    pub buffer: vk::Buffer,
    pub allocation: Option<Allocation>,
    pub size: u64,
    pub usage: vk::BufferUsageFlags,
}

impl GpuBuffer {
    pub fn new(
        ctx: &mut VkContext,
        name: &str,
        size: u64,
        usage: vk::BufferUsageFlags,
        location: MemoryLocation,
    ) -> Result<Self> {
        let alloc_size = size.max(16);
        let create_info = vk::BufferCreateInfo::default()
            .size(alloc_size)
            .usage(usage)
            .sharing_mode(vk::SharingMode::EXCLUSIVE);
        // SAFETY: `device` is valid.
        let buffer = unsafe { ctx.device.create_buffer(&create_info, None)? };
        // SAFETY: `buffer` was just created on `device`.
        let requirements = unsafe { ctx.device.get_buffer_memory_requirements(buffer) };

        let allocation = ctx.allocator_mut().allocate(&AllocationCreateDesc {
            name,
            requirements,
            location,
            linear: true,
            allocation_scheme: AllocationScheme::GpuAllocatorManaged,
        })?;

        // SAFETY: `allocation` matches `buffer` requirements.
        unsafe {
            ctx.device
                .bind_buffer_memory(buffer, allocation.memory(), allocation.offset())?;
        }

        Ok(Self {
            buffer,
            allocation: Some(allocation),
            size: alloc_size,
            usage,
        })
    }

    /// Creates a GPU-local static buffer and populates it via a transient staging buffer.
    pub fn new_static_with_data(
        ctx: &mut VkContext,
        name: &str,
        usage: vk::BufferUsageFlags,
        bytes: &[u8],
    ) -> Result<Self> {
        let size = bytes.len().max(16) as u64;
        let mut staging = Self::new(
            ctx,
            "zhr_staging_buf",
            size,
            vk::BufferUsageFlags::TRANSFER_SRC,
            MemoryLocation::CpuToGpu,
        )?;
        staging.write_bytes(0, bytes)?;

        let gpu_buf = Self::new(
            ctx,
            name,
            size,
            usage | vk::BufferUsageFlags::TRANSFER_DST,
            MemoryLocation::GpuOnly,
        )?;

        ctx.submit_immediate(|device, cmd| {
            let region = [vk::BufferCopy::default().size(bytes.len() as u64)];
            // SAFETY: Both buffers are valid and non-overlapping.
            unsafe {
                device.cmd_copy_buffer(cmd, staging.buffer, gpu_buf.buffer, &region);
            }
        })?;

        staging.destroy(ctx);
        Ok(gpu_buf)
    }

    pub fn write_bytes(&mut self, offset: usize, bytes: &[u8]) -> Result<()> {
        if bytes.is_empty() {
            return Ok(());
        }
        let alloc = self
            .allocation
            .as_mut()
            .ok_or(VkError::State("buffer allocation already freed"))?;
        let slice = alloc
            .mapped_slice_mut()
            .ok_or(VkError::State("buffer is not host-visible"))?;
        let end = offset
            .checked_add(bytes.len())
            .ok_or(VkError::DynamicBufferOverflow {
                requested: bytes.len(),
                capacity: slice.len(),
            })?;
        if end > slice.len() {
            return Err(VkError::DynamicBufferOverflow {
                requested: end,
                capacity: slice.len(),
            });
        }
        slice[offset..end].copy_from_slice(bytes);
        Ok(())
    }

    pub fn mapped_slice(&self) -> Result<&[u8]> {
        let alloc = self
            .allocation
            .as_ref()
            .ok_or(VkError::State("buffer allocation already freed"))?;
        alloc
            .mapped_slice()
            .ok_or(VkError::State("buffer is not host-visible"))
    }

    pub fn destroy(mut self, ctx: &mut VkContext) {
        if let Some(alloc) = self.allocation.take() {
            let _ = ctx.allocator_mut().free(alloc);
        }
        // SAFETY: Caller ensures GPU work referencing this buffer is complete.
        unsafe {
            ctx.device.destroy_buffer(self.buffer, None);
        }
    }
}

/// Linear bump allocator backed by a persistently mapped host-visible `GpuBuffer`.
/// Reset at the start of each frame slot after waiting on that frame's fence.
pub struct DynamicRingBuffer {
    pub gpu: GpuBuffer,
    pub offset: usize,
    pub capacity: usize,
}

impl DynamicRingBuffer {
    pub fn new(
        ctx: &mut VkContext,
        name: &str,
        capacity: usize,
        usage: vk::BufferUsageFlags,
    ) -> Result<Self> {
        let gpu = GpuBuffer::new(ctx, name, capacity as u64, usage, MemoryLocation::CpuToGpu)?;
        Ok(Self {
            gpu,
            offset: 0,
            capacity,
        })
    }

    pub fn reset(&mut self) {
        self.offset = 0;
    }

    /// Allocates aligned bytes in the ring buffer and writes `data`, returning the byte offset.
    pub fn push_bytes(&mut self, data: &[u8], alignment: usize) -> Result<u64> {
        let align = alignment.max(1);
        let aligned = (self.offset + align - 1) & !(align - 1);
        let end = aligned.saturating_add(data.len());
        if end > self.capacity {
            return Err(VkError::DynamicBufferOverflow {
                requested: end,
                capacity: self.capacity,
            });
        }
        self.gpu.write_bytes(aligned, data)?;
        self.offset = end;
        Ok(aligned as u64)
    }

    pub fn destroy(self, ctx: &mut VkContext) {
        self.gpu.destroy(ctx);
    }
}
