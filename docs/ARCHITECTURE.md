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

### Full Native 64-bit (`x64`) Vulkan Build (C++20 + Rust via Ninja + `sccache`)
```powershell
pwsh -File scripts/build-win64-vulkan.ps1 -Config RelWithDebInfo -Target z_generals
```
Output: `build/win64-vulkan/GeneralsMD/RelWithDebInfo/generalszh.exe` (`PE32+` x64, zero `d3d8.dll`/`d3dx8` imports, linked against `zhr_ffi.lib`).

### Build-Speed Conventions
- **`sccache`** wraps both Rust (`rustc-wrapper`) and C/C++ (`CMAKE_C/CXX_COMPILER_LAUNCHER` with `/Z7` embedded debug info).
- **`rust-lld.exe`** links Rust artifacts on Windows (`mold`/`lld` on Linux).
- **`Cargo.toml` profiles:** workspace crates keep incremental + `debug = "line-tables-only"`; external dependencies compile once at `opt-level = 2, debug = false` and stay cached in `sccache`. `RelWithDebInfo` uses `--profile=release-fast` (no thin-LTO overhead during iteration).

---

## 2. Engine Architecture (Hybrid C++20 Sim + Rust Engine Subsystems)

We retain TSH's battle-tested C++20 simulation, INI/BIG/W3D/map loaders, AI, scripts, replays, and decoupled high-FPS loop, while replacing the legacy 32-bit Windows-only engine layers with clean, self-contained Rust crates linked via C-ABI (`rust/zhr-ffi`):

| Legacy Blocker | Replacement Crate | Status |
|---|---|---|
| DirectX 8 (`DX8Backend`, 32-bit Windows) | `rust/zhr-vulkan` (`ash` Vulkan 1.3 dynamic rendering + `gpu-allocator` + `naga` WGSL→SPIR-V) + `WW3D2/Backend/VulkanBackend.cpp` | Core Vulkan 1.3 renderer & C-ABI built + GPU-tested (0 validation errors); `DX8Wrapper` draw/buffer/texture routing in progress |
| Miles Sound System (`mss32.dll`, 32-bit) | `rust/zhr-audio` (cross-platform Rust 3D audio + WAV/MP3 decoding) | Planned (`ZHR-3`) |
| Bink Video (`binkw32.dll`, 32-bit) | Existing TSH FFmpeg video backend (`RTS_BUILD_OPTION_FFMPEG`) | Planned (`ZHR-3`) |
| Win32 Window + DirectInput (`Win32Device`) | `rust/zhr-platform` (SDL3 cross-platform window/input) | Planned (`ZHR-4`) |
| 2003 Low-Res Textures & Flat Lighting | `rust/zhr-remaster` (local async AI upscale + PBR map synthesis + BLAKE3 BC7/BC5 cache) | Planned (`ZHR-5`, `ZHR-6`) |

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
  4. **License Discipline:** Do **not** use non-commercial/research-only weights (e.g., Ubisoft CHORD).

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
