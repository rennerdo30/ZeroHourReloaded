# ZeroHourReloaded — Architecture & Modernization Roadmap

**ZeroHourReloaded** is a public GPL-3.0-or-later fork of [`TheSuperHackers/GeneralsGameCode`](https://github.com/TheSuperHackers/GeneralsGameCode) aiming to modernize *Generals: Zero Hour* into a native **64-bit, Vulkan 1.3, cross-platform, Rust-augmented** RTS engine while keeping original gameplay fidelity and requiring players' own legal game assets (zero copyrighted EA assets are stored in this repository).

---

## 1. Fast Build Setup (Rust + C++20 + Ninja + `sccache`)

### One-Time Machine Setup
```powershell
# Generates gitignored .cargo/config.toml with sccache + rust-lld + external target-dir
pwsh -File scripts/setup-rust.ps1
```
Linux/macOS:
```bash
./scripts/setup-rust.sh
```

### Rust Commands (Sub-Second Incremental, Shared `sccache`)
```powershell
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
cargo build -p zhr-ffi --profile release-fast
```

### Build & Play (One Command / Double-Click)
ZeroHourReloaded auto-discovers your locally installed Zero Hour assets (`INIZH.big`) from `ZHR_ASSETS_DIR`, sibling/local folders, Steam (`libraryfolders.vdf` across all drives), or EA App/Origin Registry keys — zero copyrighted assets are shipped in this repo.

```powershell
# Always incrementally rebuilds latest Rust + C++20 and launches 64-bit Vulkan 1.3:
cargo run

# Or double-click `play.bat` in the repository root, or run PowerShell directly:
pwsh -File scripts/run-win64-vulkan.ps1
pwsh -File scripts/run-win64-vulkan.ps1 -NoShellMap -Validation
pwsh -File scripts/run-win64-vulkan.ps1 -AssetsDir "C:\Path\To\Zero Hour"
```

### Full Native 64-bit (`x64`) Vulkan Build (C++20 + Rust via Ninja + `sccache`)
```powershell
pwsh -File scripts/build-win64-vulkan.ps1 -Config RelWithDebInfo -Target z_generals
```
Output: `build/win64-vulkan/GeneralsMD/RelWithDebInfo/generalszh.exe` (`PE32+` x64, zero `d3d8.dll`/`d3dx8` imports, linked against `zhr_ffi.lib`). The launcher copies it alongside your assets as `generalszh-vulkan64.exe` so any existing 32-bit `generalszh.exe` stays untouched.

### Build-Speed Conventions
- **`sccache`** wraps both Rust (`rustc-wrapper`) and C/C++ (`CMAKE_C/CXX_COMPILER_LAUNCHER` with `/Z7` embedded debug info).
- **`rust-lld.exe`** links Rust artifacts on Windows (`mold`/`lld` on Linux).
- **`Cargo.toml` profiles:** workspace crates keep incremental + `debug = "line-tables-only"`; external dependencies compile once at `opt-level = 2, debug = false` and stay cached in `sccache`. `RelWithDebInfo` uses `--profile=release-fast` (no thin-LTO overhead during iteration).

---

## 2. Engine Architecture (Hybrid C++20 Sim + Rust Engine Subsystems)

We retain TSH's battle-tested C++20 simulation, INI/BIG/W3D/map loaders, AI, scripts, replays, and decoupled high-FPS loop, while replacing the legacy 32-bit Windows-only engine layers with clean, self-contained Rust crates linked via C-ABI (`rust/zhr-ffi`):

| Legacy Blocker | Replacement Crate | Status |
|---|---|---|
| DirectX 8 (`DX8Backend`, 32-bit Windows) | `rust/zhr-vulkan` (`ash` Vulkan 1.3 dynamic rendering + `gpu-allocator` + `naga` WGSL→SPIR-V) + `WW3D2/Backend/VulkanDX8Bridge.cpp` | **Working** — native 64-bit Zero Hour boots & renders Main Menu, 3D Shell Map, and Skirmish on Vulkan 1.3 with 0 Khronos validation errors (`ZHR-2`) |
| Miles Sound System (`mss32.dll`, 32-bit) | `rust/zhr-audio` (`cpal` WASAPI/CoreAudio/ALSA + `symphonia` WAV/PCM/IMA-ADPCM/MP3 stereo & 3D spatial mixer) wired via `Dependencies/Miles/MilesLoader.cpp` | **Working** — native 64-bit Music, Speech, 2D UI SFX, and 3D positional battlefield audio with zero `mss32.dll` (`ZHR-3`) |
| Bink Video (`binkw32.dll`, 32-bit) | `FFmpegVideoPlayer` + `FFmpegFile` (`RTS_BUILD_OPTION_FFMPEG`, auto-fetched LGPL FFmpeg 8.1) + `rust/zhr-audio` `VideoAudioQueue` | **Working** — native 64-bit `.bik` intro & cutscene decoding with seekable `.big` AVIO and synchronized Rust PCM audio mixing (`ZHR-3`) |
| Win32 Window + DirectInput (`Win32Device`) | SDL3 3.4.20 (`Core/GameEngineDevice/Source/SDLDevice/`) + `rust/zhr-platform` (USB HID `SDL_Scancode` → Zero Hour `KeyDefType`) | **Working** — native 64-bit resizable Vulkan SDL3 window & keyboard/mouse input with zero `DirectInput8` (`ZHR-4`) |
| 2003 Low-Res Textures, Flat Water & Low-Poly Silhouettes | `rust/zhr-vulkan` (16× AF + full mipmaps, 3D PBR micro-normals/GGX/IBL, hardware `VkStencilOpState` volumetric shadows & occluded silhouettes, half-res bilateral depth SSAO, animated 5-octave domain-warped water with Fresnel reflections & GGX glints, HDR Bloom + Filmic Tonemap + CAS) + `VulkanDX8Bridge.cpp` crease-preserving Phong `.W3D` mesh smoothing + `rust/zhr-remaster` zero-stall async 2× texture remastering & BLAKE3 disk cache + live hotkeys (`F7` Mesh, `F8` 2× Textures, `F9` Remastered/Split-Screen/Classic, `F10` PBR, `F11` Post-FX) | **Working (`ZHR-5`, `ZHR-6`, `ZHR-8`)** |
| 2003 Stretched Menu UI & 4-bit `A4R4G4B4` Fonts | Real-time Vulkan quarter-res frosted-glass backdrop blur (`PostProcessPipeline::capture_ui_backdrop` + `WW3D2_UBER_WGSL`), `ZhrMenuStyle` vector tactical panels/buttons & widescreen shell frame, and full 8-bit `A8R8G8B8` `Render2DSentenceClass` HD typography (`F9` live toggleable) | **Working (`ZHR-9`)** |

---

## 3. On-the-Fly Local Asset Remastering Design (No Copyrighted Assets in Git)

Because copyrighted `.big` assets stay strictly on the player's machine, visual remastering runs **locally on the player's GPU** with an async background worker + persistent disk cache keyed by `blake3(original_asset)`:

### Textures & PBR Synthesis (Permissive Open-Weight Models)
- **World / Units / Terrain Diffuse (4× + DXT1 De-blocking):** `4x-PBRify_RPLKSRd` (`Kim2091/PBRify_Remix`, **CC0-1.0 Public Domain**, `RealPLKSR` architecture trained on CC0 `ambientCG`/`Poly Haven` PBR materials with `BC1` degradation). Preserves Zero Hour's gritty military look (weathered steel, concrete, desert sand, camo) without plastic smoothing.
- **Albedo → Normal / Roughness / Height Maps:** `PBRify_Remix` companion PBR models (**CC0-1.0**) and/or `DeepBump` (**MIT**, lightweight tiled ONNX CNN).
- **UI / HUD Icons / Fine Line Art:** `2xNomosUni` (`SPAN` architecture by Phhofm, **CC-BY-4.0**).
- **Zero Hour–Specific Pipeline Rules:**
  1. **Isolate Alpha / House-Color Channels:** Run AI super-resolution on **RGB only**; process alpha/team-color masks separately so dynamic player faction colors never bleed or wash out.
  2. **Modern GPU Compression:** Cache outputs as **`BC7`** (albedo), **`BC5`** (normals), **`BC4`** (roughness/height) instead of lossy 2003 `DXT1`/`DXT5`.
  3. **Zero-Stall Async Hot-Swap:** Show original texture on frame 1, run FP16 ONNX / Vulkan compute asynchronously, hot-swap the Vulkan descriptor in-place, and persist to `%LOCALAPPDATA%/ZeroHourReloaded/cache/`.
  4. **Instant Runtime A/B Testing Toggles:** Built-in live hotkeys (`F9` Remastered / 50/50 Split-Screen / Classic, `F10` PBR/Water/SSAO, `F11` HDR Bloom/ACES/CAS, `F7` Mesh Smoothing, `F8` 2× Remastered Textures) to compare original vs. remastered visuals live in-game without restarting.
  5. **License Discipline:** Do **not** use non-commercial/research-only weights (e.g., Ubisoft CHORD).

### Geometry-Preserving `.W3D` Mesh & Renderer Upgrades
- **Never** use generative 3D mesh-replacement AI on-the-fly (breaks W3D bone hierarchies `TURRET01`/`MUZZLEFX01`, animations, damage states, and RTS readability).
- Instead apply deterministic, rig-safe enhancements:
  - **Crease-preserving subdivision / PN-Triangles** (keeps tank armor plates crisp while smoothing barrels, wheels, radar dishes, aircraft, and infantry).
  - **Mikktspace tangent generation** for crisp normal mapping + Parallax Occlusion Mapping (POM).
  - **Modern Vulkan lighting:** PBR (GGX + sky IBL + dynamic explosion point lights), cascaded soft shadows (PCF/PCSS), GTAO/SSAO, bicubic visual terrain upsampling, modern water (Gerstner waves + reflections + foam), HDR/bloom, and 16× AF + CAS/TAA.

---

## 4. Licensing

- **Repository license:** `GPL-3.0-or-later` + EA GPL Section 7 Additional Terms (see [LICENSE.md](../LICENSE.md)).
- No EA trademarks (`Command & Conquer`, `EA`, etc.) in project branding or distribution.
- Players provide their own legal game installation (e.g., Steam Ultimate Collection).
