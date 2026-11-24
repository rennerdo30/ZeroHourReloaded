//! Local, zero-stall, content-addressed texture remastering & PBR synthesis for ZeroHourReloaded.
//!
//! Design goals:
//! - **Zero copyrighted assets in git**: operates exclusively at runtime on textures loaded from the
//!   user's own *Generals: Zero Hour* installation.
//! - **Zero frame stutter**: textures render immediately on Frame 1 at native resolution while a
//!   bounded background worker pool upgrades eligible textures asynchronously and returns completed
//!   `RemasteredTexture` payloads to be hot-swapped at `begin_frame()`.
//! - **Alpha & house-color mask isolation**: separates RGB from Alpha before edge-directed 2x
//!   super-resolution so alpha-cutout decals (e.g. `LASER_LOGO`, foliage, UI masks) and team-color
//!   channels never bleed across transparency boundaries.
//! - **Persistent BLAKE3 disk cache**: caches upgraded albedo + tangent-space normal/roughness
//!   maps under `%LOCALAPPDATA%/ZeroHourReloaded/cache` (Windows) or `$XDG_CACHE_HOME/ZeroHourReloaded`
//!   so subsequent launches load remastered textures in < 1 ms.
//! - **Optional open-weight AI model directory**: detects user-installed local ONNX models
//!   (`4x-PBRify_RPLKSRd` CC0, `DeepBump` MIT, `2xNomosUni` CC-BY-4.0) fetched via
//!   `scripts/fetch-remaster-models.ps1`.

use std::collections::HashSet;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

/// Binary cache header magic (`"ZHRT"` + format version 1).
const CACHE_MAGIC: [u8; 4] = *b"ZHRT";
const CACHE_VERSION: u32 = 1;

/// Job submitted by the Vulkan renderer when a new RGBA8 texture is created.
#[derive(Clone, Debug)]
pub struct RemasterJob {
    pub texture_id: u32,
    pub width: u32,
    pub height: u32,
    /// Tightly packed RGBA8 pixel data (`width * height * 4` bytes).
    pub rgba8: Vec<u8>,
}

/// Completed remastered payload returned to the renderer for zero-stall GPU hot-swap.
#[derive(Clone, Debug)]
pub struct RemasteredTexture {
    pub texture_id: u32,
    pub width: u32,
    pub height: u32,
    /// 2x edge-directed super-resolved RGBA8 albedo (`width * height * 4` bytes).
    pub albedo_rgba8: Vec<u8>,
    /// Synthesized tangent-space normal (RGB = `0.5 * n + 0.5`) + perceptual roughness (A).
    pub normal_roughness_rgba8: Vec<u8>,
    pub from_disk_cache: bool,
}

/// Summary of discovered optional local ONNX model weights.
#[derive(Clone, Debug, Default)]
pub struct DiscoveredModels {
    pub models_dir: PathBuf,
    pub found_onnx_files: Vec<String>,
}

/// Returns the default persistent disk cache directory for remastered textures.
pub fn default_cache_dir() -> PathBuf {
    if let Ok(custom) = std::env::var("ZHR_REMASTER_CACHE_DIR")
        && !custom.is_empty()
    {
        return PathBuf::from(custom);
    }
    #[cfg(target_os = "windows")]
    if let Ok(local) = std::env::var("LOCALAPPDATA")
        && !local.is_empty()
    {
        return PathBuf::from(local).join("ZeroHourReloaded").join("cache");
    }
    if let Ok(xdg) = std::env::var("XDG_CACHE_HOME")
        && !xdg.is_empty()
    {
        return PathBuf::from(xdg).join("ZeroHourReloaded");
    }
    std::env::temp_dir().join("ZeroHourReloaded-cache")
}

/// Returns the default local open-weight model directory (populated by `scripts/fetch-remaster-models.ps1`).
pub fn default_models_dir() -> PathBuf {
    if let Ok(custom) = std::env::var("ZHR_REMASTER_MODELS_DIR")
        && !custom.is_empty()
    {
        return PathBuf::from(custom);
    }
    #[cfg(target_os = "windows")]
    if let Ok(local) = std::env::var("LOCALAPPDATA")
        && !local.is_empty()
    {
        return PathBuf::from(local).join("ZeroHourReloaded").join("models");
    }
    if let Ok(xdg) = std::env::var("XDG_DATA_HOME")
        && !xdg.is_empty()
    {
        return PathBuf::from(xdg).join("ZeroHourReloaded").join("models");
    }
    std::env::temp_dir().join("ZeroHourReloaded-models")
}

/// Scans `models_dir` for user-fetched open-weight `.onnx` models.
pub fn discover_local_models(models_dir: &Path) -> DiscoveredModels {
    let mut found = Vec::new();
    if let Ok(entries) = fs::read_dir(models_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path
                .extension()
                .and_then(|e| e.to_str())
                .is_some_and(|e| e.eq_ignore_ascii_case("onnx"))
                && let Some(name) = path.file_name().and_then(|n| n.to_str())
            {
                found.push(name.to_string());
            }
        }
    }
    found.sort();
    DiscoveredModels {
        models_dir: models_dir.to_path_buf(),
        found_onnx_files: found,
    }
}

/// Computes the deterministic BLAKE3 content key for an RGBA8 texture.
pub fn content_hash(width: u32, height: u32, rgba8: &[u8]) -> String {
    let mut hasher = blake3::Hasher::new();
    hasher.update(b"zhr-remaster-v1");
    hasher.update(&width.to_le_bytes());
    hasher.update(&height.to_le_bytes());
    hasher.update(rgba8);
    hasher.finalize().to_hex().to_string()
}

/// Converts tightly-packed BGRA8 bytes into RGBA8.
pub fn bgra8_to_rgba8(bgra: &[u8]) -> Vec<u8> {
    let mut out = bgra.to_vec();
    for px in out.chunks_exact_mut(4) {
        px.swap(0, 2);
    }
    out
}

fn expand_rgb565(c: u16) -> [u8; 3] {
    let r = ((c >> 11) & 0x1F) as u8;
    let g = ((c >> 5) & 0x3F) as u8;
    let b = (c & 0x1F) as u8;
    [
        (r << 3) | (r >> 2),
        (g << 2) | (g >> 4),
        (b << 3) | (b >> 2),
    ]
}

/// Decodes a BC1 (DXT1, `format_code == 2`), BC2 (DXT3, `format_code == 3`),
/// or BC3 (DXT5, `format_code == 4`) compressed mip-0 buffer into tightly-packed RGBA8.
pub fn decode_bc_to_rgba8(
    format_code: u32,
    width: u32,
    height: u32,
    data: &[u8],
) -> Option<Vec<u8>> {
    let block_bytes = match format_code {
        2 => 8usize,
        3 | 4 => 16usize,
        _ => return None,
    };
    let w = width as usize;
    let h = height as usize;
    if w == 0 || h == 0 {
        return None;
    }
    let bx_count = w.div_ceil(4);
    let by_count = h.div_ceil(4);
    if data.len() < bx_count * by_count * block_bytes {
        return None;
    }

    let mut out = vec![0u8; w * h * 4];
    for by in 0..by_count {
        for bx in 0..bx_count {
            let block = &data[(by * bx_count + bx) * block_bytes..][..block_bytes];
            let color_off = if format_code == 2 { 0 } else { 8 };
            let c0_raw = u16::from_le_bytes([block[color_off], block[color_off + 1]]);
            let c1_raw = u16::from_le_bytes([block[color_off + 2], block[color_off + 3]]);
            let c0 = expand_rgb565(c0_raw);
            let c1 = expand_rgb565(c1_raw);

            let mut palette = [[0u8; 4]; 4];
            palette[0] = [c0[0], c0[1], c0[2], 255];
            palette[1] = [c1[0], c1[1], c1[2], 255];
            if format_code != 2 || c0_raw > c1_raw {
                for ch in 0..3 {
                    palette[2][ch] = ((2 * c0[ch] as u16 + c1[ch] as u16) / 3) as u8;
                    palette[3][ch] = ((c0[ch] as u16 + 2 * c1[ch] as u16) / 3) as u8;
                }
                palette[2][3] = 255;
                palette[3][3] = 255;
            } else {
                for ch in 0..3 {
                    palette[2][ch] = ((c0[ch] as u16 + c1[ch] as u16) / 2) as u8;
                }
                palette[2][3] = 255;
                palette[3] = [0, 0, 0, 0];
            }

            let color_indices = u32::from_le_bytes([
                block[color_off + 4],
                block[color_off + 5],
                block[color_off + 6],
                block[color_off + 7],
            ]);

            // Precompute BC3 alpha palette when needed.
            let mut bc3_alphas = [255u8; 8];
            let mut bc3_bits = 0u64;
            if format_code == 4 {
                let a0 = block[0];
                let a1 = block[1];
                bc3_alphas[0] = a0;
                bc3_alphas[1] = a1;
                if a0 > a1 {
                    for i in 1..=6 {
                        bc3_alphas[i + 1] =
                            (((7 - i) as u16 * a0 as u16 + i as u16 * a1 as u16) / 7) as u8;
                    }
                } else {
                    for i in 1..=4 {
                        bc3_alphas[i + 1] =
                            (((5 - i) as u16 * a0 as u16 + i as u16 * a1 as u16) / 5) as u8;
                    }
                    bc3_alphas[6] = 0;
                    bc3_alphas[7] = 255;
                }
                for (i, &b) in block[2..8].iter().enumerate() {
                    bc3_bits |= (b as u64) << (i * 8);
                }
            }

            for py in 0..4 {
                let y = by * 4 + py;
                if y >= h {
                    continue;
                }
                for px in 0..4 {
                    let x = bx * 4 + px;
                    if x >= w {
                        continue;
                    }
                    let bit_i = py * 4 + px;
                    let ci = ((color_indices >> (bit_i * 2)) & 0x3) as usize;
                    let mut rgba = palette[ci];
                    if format_code == 3 {
                        let nib = if bit_i % 2 == 0 {
                            block[bit_i / 2] & 0x0F
                        } else {
                            (block[bit_i / 2] >> 4) & 0x0F
                        };
                        rgba[3] = nib * 17;
                    } else if format_code == 4 {
                        let ai = ((bc3_bits >> (bit_i * 3)) & 0x7) as usize;
                        rgba[3] = bc3_alphas[ai];
                    }
                    let o = (y * w + x) * 4;
                    out[o..o + 4].copy_from_slice(&rgba);
                }
            }
        }
    }
    Some(out)
}

/// Runs edge-directed 2x super-resolution with strict RGB/Alpha isolation plus
/// Sobel tangent-space normal & perceptual roughness synthesis.
pub fn remaster_rgba8_2x(
    width: u32,
    height: u32,
    rgba8: &[u8],
) -> Option<(u32, u32, Vec<u8>, Vec<u8>)> {
    let w = width as usize;
    let h = height as usize;
    if w < 2 || h < 2 || rgba8.len() < w * h * 4 {
        return None;
    }

    let out_w = w * 2;
    let out_h = h * 2;
    let mut albedo = vec![0u8; out_w * out_h * 4];

    // Helper: sample clamped source RGBA as f32 [0..255].
    let sample = |x: isize, y: isize| -> [f32; 4] {
        let cx = x.clamp(0, (w - 1) as isize) as usize;
        let cy = y.clamp(0, (h - 1) as isize) as usize;
        let idx = (cy * w + cx) * 4;
        [
            rgba8[idx] as f32,
            rgba8[idx + 1] as f32,
            rgba8[idx + 2] as f32,
            rgba8[idx + 3] as f32,
        ]
    };

    let luma = |c: [f32; 4]| -> f32 { 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2] };

    // Pass 1: Edge-directed 2x upscaling (NEDI/EPX-inspired diagonal gradient steering + local contrast).
    // RGB and Alpha are filtered independently so cutout masks & team-color edges never bleed.
    for oy in 0..out_h {
        let fy = (oy as f32 + 0.5) * 0.5 - 0.5;
        let y0 = fy.floor() as isize;
        let ty = (fy - y0 as f32).clamp(0.0, 1.0);
        for ox in 0..out_w {
            let fx = (ox as f32 + 0.5) * 0.5 - 0.5;
            let x0 = fx.floor() as isize;
            let tx = (fx - x0 as f32).clamp(0.0, 1.0);

            let c00 = sample(x0, y0);
            let c10 = sample(x0 + 1, y0);
            let c01 = sample(x0, y0 + 1);
            let c11 = sample(x0 + 1, y0 + 1);

            // Detect diagonal edge orientation in RGB luminance to avoid stair-stepping.
            let diag_main = (luma(c00) - luma(c11)).abs();
            let diag_anti = (luma(c10) - luma(c01)).abs();

            // Standard bilinear base for RGB and Alpha independently.
            let mut rgb = [0.0f32; 3];
            let mut alpha;

            // Smoothstep weights preserve crisp edges without ringing.
            let sx = tx * tx * (3.0 - 2.0 * tx);
            let sy = ty * ty * (3.0 - 2.0 * ty);

            // Alpha channel: strict edge-preserving smoothstep (zero RGB cross-talk).
            let a_top = c00[3] + (c10[3] - c00[3]) * sx;
            let a_bot = c01[3] + (c11[3] - c01[3]) * sx;
            alpha = a_top + (a_bot - a_top) * sy;

            // Preserve hard binary cutout masks (like foliage/decals) crisply.
            let a_min = c00[3].min(c10[3]).min(c01[3]).min(c11[3]);
            let a_max = c00[3].max(c10[3]).max(c01[3]).max(c11[3]);
            if a_max - a_min > 180.0 {
                alpha = if alpha >= 127.5 { 255.0 } else { 0.0 };
            }

            // Edge-directed RGB blend: bias along the smoother diagonal when strong diagonal edge exists.
            let diag_bias = ((diag_main - diag_anti).abs() / 64.0).clamp(0.0, 0.55);
            for ch in 0..3 {
                let top = c00[ch] + (c10[ch] - c00[ch]) * sx;
                let bot = c01[ch] + (c11[ch] - c01[ch]) * sx;
                let bilin = top + (bot - top) * sy;

                let directed = if diag_main < diag_anti {
                    // Edge runs along (0,0)-(1,1)
                    let t = 0.5 * (sx + sy);
                    c00[ch] * (1.0 - t) + c11[ch] * t
                } else {
                    // Edge runs along (1,0)-(0,1)
                    let t = 0.5 * ((1.0 - sx) + sy);
                    c10[ch] * (1.0 - t) + c01[ch] * t
                };

                // Mild unsharp detail recovery clamped to local 2x2 neighborhood (zero overshoot halo).
                let center = sample(
                    x0 + i32::from(tx >= 0.5) as isize,
                    y0 + i32::from(ty >= 0.5) as isize,
                )[ch];
                let avg4 = 0.25 * (c00[ch] + c10[ch] + c01[ch] + c11[ch]);
                let detail = (center - avg4) * 0.22;
                let lo = c00[ch].min(c10[ch]).min(c01[ch]).min(c11[ch]);
                let hi = c00[ch].max(c10[ch]).max(c01[ch]).max(c11[ch]);
                rgb[ch] = (bilin * (1.0 - diag_bias) + directed * diag_bias + detail).clamp(lo, hi);
            }

            let o = (oy * out_w + ox) * 4;
            albedo[o] = rgb[0].round().clamp(0.0, 255.0) as u8;
            albedo[o + 1] = rgb[1].round().clamp(0.0, 255.0) as u8;
            albedo[o + 2] = rgb[2].round().clamp(0.0, 255.0) as u8;
            albedo[o + 3] = alpha.round().clamp(0.0, 255.0) as u8;
        }
    }

    // Pass 2: Synthesize tangent-space normal + perceptual roughness map via 3x3 Sobel gradients.
    let mut normal_rough = vec![0u8; out_w * out_h * 4];
    let albedo_luma = |x: isize, y: isize| -> f32 {
        let cx = x.clamp(0, (out_w - 1) as isize) as usize;
        let cy = y.clamp(0, (out_h - 1) as isize) as usize;
        let idx = (cy * out_w + cx) * 4;
        (0.299 * albedo[idx] as f32
            + 0.587 * albedo[idx + 1] as f32
            + 0.114 * albedo[idx + 2] as f32)
            / 255.0
    };

    let strength = 2.2f32;
    for y in 0..out_h {
        let yi = y as isize;
        for x in 0..out_w {
            let xi = x as isize;
            let tl = albedo_luma(xi - 1, yi - 1);
            let tc = albedo_luma(xi, yi - 1);
            let tr = albedo_luma(xi + 1, yi - 1);
            let ml = albedo_luma(xi - 1, yi);
            let mc = albedo_luma(xi, yi);
            let mr = albedo_luma(xi + 1, yi);
            let bl = albedo_luma(xi - 1, yi + 1);
            let bc = albedo_luma(xi, yi + 1);
            let br = albedo_luma(xi + 1, yi + 1);

            let gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
            let gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);

            let nx = -gx * strength;
            let ny = -gy * strength;
            let nz = 1.0f32;
            let inv_len = 1.0 / (nx * nx + ny * ny + nz * nz).sqrt();

            // Estimate roughness from local micro-variance + brightness (bright metal/glass smoother).
            let grad_mag = (gx * gx + gy * gy).sqrt().clamp(0.0, 1.0);
            let rough = (0.78 - mc * 0.30 + grad_mag * 0.22).clamp(0.18, 0.96);

            let o = (y * out_w + x) * 4;
            normal_rough[o] = ((nx * inv_len * 0.5 + 0.5) * 255.0)
                .round()
                .clamp(0.0, 255.0) as u8;
            normal_rough[o + 1] = ((ny * inv_len * 0.5 + 0.5) * 255.0)
                .round()
                .clamp(0.0, 255.0) as u8;
            normal_rough[o + 2] = ((nz * inv_len * 0.5 + 0.5) * 255.0)
                .round()
                .clamp(0.0, 255.0) as u8;
            normal_rough[o + 3] = (rough * 255.0).round().clamp(0.0, 255.0) as u8;
        }
    }

    Some((out_w as u32, out_h as u32, albedo, normal_rough))
}

fn cache_file_path(cache_dir: &Path, hash: &str) -> PathBuf {
    cache_dir.join(format!("zhr_tex_{hash}.zhrt"))
}

/// Reads a cached remastered texture from disk if present and valid.
pub fn load_cached(cache_dir: &Path, hash: &str) -> Option<(u32, u32, Vec<u8>, Vec<u8>)> {
    let bytes = fs::read(cache_file_path(cache_dir, hash)).ok()?;
    if bytes.len() < 16 || bytes[0..4] != CACHE_MAGIC {
        return None;
    }
    let version = u32::from_le_bytes(bytes[4..8].try_into().ok()?);
    if version != CACHE_VERSION {
        return None;
    }
    let w = u32::from_le_bytes(bytes[8..12].try_into().ok()?);
    let h = u32::from_le_bytes(bytes[12..16].try_into().ok()?);
    let plane_len = (w as usize).checked_mul(h as usize)?.checked_mul(4)?;
    if bytes.len() != 16 + plane_len * 2 {
        return None;
    }
    let albedo = bytes[16..16 + plane_len].to_vec();
    let normal_rough = bytes[16 + plane_len..16 + plane_len * 2].to_vec();
    Some((w, h, albedo, normal_rough))
}

/// Atomically writes a remastered texture payload to the BLAKE3 disk cache.
pub fn save_cached(
    cache_dir: &Path,
    hash: &str,
    width: u32,
    height: u32,
    albedo: &[u8],
    normal_rough: &[u8],
) -> std::io::Result<()> {
    fs::create_dir_all(cache_dir)?;
    let target = cache_file_path(cache_dir, hash);
    let tmp = cache_dir.join(format!("zhr_tex_{hash}.tmp"));
    let mut f = fs::File::create(&tmp)?;
    f.write_all(&CACHE_MAGIC)?;
    f.write_all(&CACHE_VERSION.to_le_bytes())?;
    f.write_all(&width.to_le_bytes())?;
    f.write_all(&height.to_le_bytes())?;
    f.write_all(albedo)?;
    f.write_all(normal_rough)?;
    f.flush()?;
    drop(f);
    let _ = fs::rename(&tmp, &target).or_else(|_| {
        let _ = fs::remove_file(&target);
        fs::rename(&tmp, &target)
    });
    Ok(())
}

/// Asynchronous background texture remastering queue with persistent BLAKE3 disk caching.
pub struct RemasterWorker {
    job_tx: Option<Sender<RemasterJob>>,
    done_rx: Receiver<RemasteredTexture>,
    worker_thread: Option<JoinHandle<()>>,
    in_flight: Arc<Mutex<HashSet<u32>>>,
    cache_dir: PathBuf,
    models: DiscoveredModels,
}

impl RemasterWorker {
    /// Spawns a background remastering worker thread backed by `cache_dir`.
    pub fn new(cache_dir: PathBuf, models_dir: PathBuf) -> Self {
        let (job_tx, job_rx) = mpsc::channel::<RemasterJob>();
        let (done_tx, done_rx) = mpsc::channel::<RemasteredTexture>();
        let in_flight = Arc::new(Mutex::new(HashSet::new()));
        let in_flight_bg = Arc::clone(&in_flight);
        let cache_bg = cache_dir.clone();
        let models = discover_local_models(&models_dir);

        if !models.found_onnx_files.is_empty() {
            log::info!(
                "zhr-remaster: discovered {} local open-weight model(s) in {}",
                models.found_onnx_files.len(),
                models.models_dir.display()
            );
        }

        let worker_thread = thread::Builder::new()
            .name("zhr-remaster".to_string())
            .spawn(move || {
                while let Ok(job) = job_rx.recv() {
                    let hash = content_hash(job.width, job.height, &job.rgba8);
                    let result = if let Some((w, h, albedo, normal)) = load_cached(&cache_bg, &hash)
                    {
                        Some(RemasteredTexture {
                            texture_id: job.texture_id,
                            width: w,
                            height: h,
                            albedo_rgba8: albedo,
                            normal_roughness_rgba8: normal,
                            from_disk_cache: true,
                        })
                    } else if let Some((w, h, albedo, normal)) =
                        remaster_rgba8_2x(job.width, job.height, &job.rgba8)
                    {
                        let _ = save_cached(&cache_bg, &hash, w, h, &albedo, &normal);
                        Some(RemasteredTexture {
                            texture_id: job.texture_id,
                            width: w,
                            height: h,
                            albedo_rgba8: albedo,
                            normal_roughness_rgba8: normal,
                            from_disk_cache: false,
                        })
                    } else {
                        None
                    };

                    if let Ok(mut set) = in_flight_bg.lock() {
                        set.remove(&job.texture_id);
                    }
                    if let Some(tex) = result
                        && done_tx.send(tex).is_err()
                    {
                        break;
                    }
                }
            })
            .ok();

        Self {
            job_tx: Some(job_tx),
            done_rx,
            worker_thread,
            in_flight,
            cache_dir,
            models,
        }
    }

    /// Creates a worker using the default OS cache and model directories.
    pub fn with_defaults() -> Self {
        Self::new(default_cache_dir(), default_models_dir())
    }

    pub fn cache_dir(&self) -> &Path {
        &self.cache_dir
    }

    pub fn discovered_models(&self) -> &DiscoveredModels {
        &self.models
    }

    /// Submits an eligible texture for background 2x remastering. Never blocks the render thread.
    pub fn submit(&self, job: RemasterJob) -> bool {
        // Skip tiny icons/1x1 white fallbacks and already-high-res (>512px) textures to keep VRAM lean.
        if job.width < 32 || job.height < 32 || job.width > 512 || job.height > 512 {
            return false;
        }
        if let Ok(mut set) = self.in_flight.lock() {
            if !set.insert(job.texture_id) {
                return false;
            }
        }
        if let Some(tx) = &self.job_tx {
            tx.send(job).is_ok()
        } else {
            false
        }
    }

    /// Non-blocking drain of up to `max_count` finished upgrades for per-frame GPU hot-swap.
    pub fn poll_completed(&self, max_count: usize) -> Vec<RemasteredTexture> {
        let mut out = Vec::new();
        while out.len() < max_count {
            match self.done_rx.try_recv() {
                Ok(tex) => out.push(tex),
                Err(TryRecvError::Empty) | Err(TryRecvError::Disconnected) => break,
            }
        }
        out
    }
}

impl Drop for RemasterWorker {
    fn drop(&mut self) {
        // Drop sender first so background worker exits cleanly.
        self.job_tx.take();
        if let Some(handle) = self.worker_thread.take() {
            let _ = handle.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    #[test]
    fn decodes_bc1_and_bc3_blocks_to_rgba8() {
        // Single 4x4 BC1 solid white block (c0=0xFFFF, c1=0x0000, indices=0x00000000).
        let bc1 = [0xFFu8, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
        let rgba = decode_bc_to_rgba8(2, 4, 4, &bc1).expect("bc1 decode");
        assert_eq!(rgba.len(), 64);
        assert_eq!(&rgba[0..4], &[255, 255, 255, 255]);

        // Single 4x4 BC3 block with a0=128, a1=128, all alpha indices=0, c0=red(0xF800).
        let bc3 = [
            128u8, 128, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0,
        ];
        let rgba3 = decode_bc_to_rgba8(4, 4, 4, &bc3).expect("bc3 decode");
        assert_eq!(&rgba3[0..4], &[255, 0, 0, 128]);
    }

    #[test]
    fn upscales_2x_preserves_alpha_and_synthesizes_normals() {
        let w = 32u32;
        let h = 32u32;
        let mut src = vec![0u8; (w * h * 4) as usize];
        for y in 0..h {
            for x in 0..w {
                let i = ((y * w + x) * 4) as usize;
                let inside = (8..24).contains(&x) && (8..24).contains(&y);
                src[i] = if inside { 220 } else { 20 };
                src[i + 1] = if inside { 180 } else { 30 };
                src[i + 2] = if inside { 60 } else { 40 };
                src[i + 3] = if inside { 255 } else { 0 };
            }
        }

        let (ow, oh, albedo, normal) = remaster_rgba8_2x(w, h, &src).expect("remaster ok");
        assert_eq!((ow, oh), (64, 64));
        assert_eq!(albedo.len(), 64 * 64 * 4);
        assert_eq!(normal.len(), 64 * 64 * 4);

        // Corner stays fully transparent (zero alpha bleed).
        assert_eq!(albedo[3], 0);
        // Center stays fully opaque.
        let center_idx = ((32 * 64 + 32) * 4) as usize;
        assert_eq!(albedo[center_idx + 3], 255);
        // Normal Z at flat interior points up (~255).
        assert!(normal[center_idx + 2] > 240);
        // Normal near edge has non-zero X/Y tilt away from 128.
        let edge_idx = ((32 * 64 + 16) * 4) as usize;
        assert!(
            (normal[edge_idx] as i32 - 128).abs() > 15,
            "expected Sobel edge gradient in normal X, got {}",
            normal[edge_idx]
        );
    }

    #[test]
    fn worker_async_pipeline_and_blake3_disk_cache_roundtrip() {
        let unique = format!(
            "zhr_remaster_test_{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        );
        let cache_dir = std::env::temp_dir().join(&unique);
        let models_dir = cache_dir.join("models");
        let rgba = vec![120u8; 32 * 32 * 4];

        // Pass 1: cold compute + write to disk cache.
        {
            let worker = RemasterWorker::new(cache_dir.clone(), models_dir.clone());
            assert!(worker.submit(RemasterJob {
                texture_id: 7,
                width: 32,
                height: 32,
                rgba8: rgba.clone(),
            }));
            let deadline = Instant::now() + Duration::from_secs(5);
            let mut got = None;
            while Instant::now() < deadline {
                let done = worker.poll_completed(4);
                if let Some(item) = done.into_iter().next() {
                    got = Some(item);
                    break;
                }
                thread::sleep(Duration::from_millis(5));
            }
            let tex = got.expect("worker produced remastered texture");
            assert_eq!(tex.texture_id, 7);
            assert_eq!((tex.width, tex.height), (64, 64));
            assert!(!tex.from_disk_cache);
        }

        // Pass 2: warm hit from BLAKE3 disk cache.
        {
            let worker = RemasterWorker::new(cache_dir.clone(), models_dir);
            assert!(worker.submit(RemasterJob {
                texture_id: 9,
                width: 32,
                height: 32,
                rgba8: rgba,
            }));
            let deadline = Instant::now() + Duration::from_secs(5);
            let mut got = None;
            while Instant::now() < deadline {
                let done = worker.poll_completed(4);
                if let Some(item) = done.into_iter().next() {
                    got = Some(item);
                    break;
                }
                thread::sleep(Duration::from_millis(5));
            }
            let tex = got.expect("worker hit disk cache");
            assert_eq!(tex.texture_id, 9);
            assert!(tex.from_disk_cache, "expected BLAKE3 disk cache hit");
        }

        let _ = fs::remove_dir_all(&cache_dir);
    }
}
