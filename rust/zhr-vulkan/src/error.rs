use thiserror::Error;

pub type Result<T> = std::result::Result<T, VkError>;

#[derive(Debug, Error)]
pub enum VkError {
    #[error("failed to load Vulkan library: {0}")]
    Load(#[from] ash::LoadingError),

    #[error("Vulkan API error: {0}")]
    Vk(#[from] ash::vk::Result),

    #[error("GPU memory allocation error: {0}")]
    Allocation(#[from] gpu_allocator::AllocationError),

    #[error("no suitable Vulkan 1.3 GPU found")]
    NoSuitableDevice,

    #[error("unsupported window handle for Vulkan surface")]
    UnsupportedWindowHandle,

    #[error("shader compilation error: {0}")]
    ShaderCompile(String),

    #[error("invalid resource handle: {0}")]
    InvalidHandle(&'static str),

    #[error(
        "dynamic frame buffer capacity exceeded (requested {requested} bytes, capacity {capacity} bytes)"
    )]
    DynamicBufferOverflow { requested: usize, capacity: usize },

    #[error("renderer state error: {0}")]
    State(&'static str),

    #[error("image I/O error: {0}")]
    Io(String),
}
