//! Core shared types and engine metadata for ZeroHourReloaded.

pub const ENGINE_NAME: &str = "ZeroHourReloaded";
pub const ENGINE_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Compile-time target architecture bitness (64 on x86_64/aarch64, 32 on x86).
pub const POINTER_BITS: u32 = usize::BITS;

/// Returns a static summary banner identifying the Rust engine runtime.
pub const fn engine_banner() -> &'static str {
    concat!("ZeroHourReloaded Rust Core v", env!("CARGO_PKG_VERSION"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn banner_contains_name_and_version() {
        let banner = engine_banner();
        assert!(banner.contains(ENGINE_NAME));
        assert!(banner.contains(ENGINE_VERSION));
    }

    #[test]
    fn pointer_bits_is_valid() {
        const { assert!(POINTER_BITS == 32 || POINTER_BITS == 64) };
    }
}
