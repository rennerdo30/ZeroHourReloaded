//! One-command `cargo run` builder & launcher for ZeroHourReloaded (64-bit native Vulkan 1.3).
//!
//! Usage:
//!   cargo run                      # Incremental Rust+C++ build + launch windowed
//!   cargo run -- -NoShellMap       # Skip 3D shell map
//!   cargo run -- -Validation       # Enable Khronos Vulkan Validation Layers
//!   cargo run -- -Fullscreen       # Launch fullscreen
//!   cargo run -- -NoBuild          # Launch existing binary without rebuilding

use std::path::PathBuf;
use std::process::{Command, ExitCode};

fn main() -> ExitCode {
    let repo_root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|p| p.parent())
        .expect("workspace root")
        .to_path_buf();

    let script = repo_root.join("scripts").join("run-win64-vulkan.ps1");
    let extra_args: Vec<String> = std::env::args()
        .skip(1)
        .map(|arg| match arg.as_str() {
            "--no-build" => "-NoBuild".to_string(),
            "--fullscreen" => "-Fullscreen".to_string(),
            "--no-shell-map" | "--noshellmap" => "-NoShellMap".to_string(),
            "--validation" => "-Validation".to_string(),
            "--assets-dir" => "-AssetsDir".to_string(),
            "--config" => "-Config".to_string(),
            _ => arg,
        })
        .collect();

    let status = Command::new("pwsh")
        .arg("-NoProfile")
        .arg("-ExecutionPolicy")
        .arg("Bypass")
        .arg("-File")
        .arg(&script)
        .args(&extra_args)
        .current_dir(&repo_root)
        .status();

    match status {
        Ok(s) if s.success() => ExitCode::SUCCESS,
        Ok(s) => ExitCode::from(s.code().unwrap_or(1) as u8),
        Err(e) => {
            eprintln!("Failed to invoke pwsh ({}): {e}", script.display());
            ExitCode::FAILURE
        }
    }
}
