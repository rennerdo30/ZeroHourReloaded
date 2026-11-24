//! Native 64-bit cross-platform audio engine for ZeroHourReloaded.
//!
//! Replaces the 32-bit proprietary Miles Sound System (`mss32.dll`) with a pure-Rust
//! `cpal` + `symphonia` real-time stereo mixer supporting WAV, PCM, IMA-ADPCM, and MP3
//! 2D SFX, 3D positional battlefield sounds, and streamed music & speech.

use std::collections::HashMap;
use std::io::Cursor;
use std::sync::{Arc, Mutex};

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::{CODEC_TYPE_NULL, DecoderOptions};
use symphonia::core::errors::Error as SymphError;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

/// Decoded interleaved stereo/mono f32 audio clip.
#[derive(Clone, Debug)]
pub struct DecodedClip {
    /// Interleaved `f32` samples (`frames * channels`).
    pub samples: Vec<f32>,
    pub sample_rate: u32,
    pub channels: u16,
}

impl DecodedClip {
    pub fn frame_count(&self) -> usize {
        if self.channels == 0 {
            0
        } else {
            self.samples.len() / (self.channels as usize)
        }
    }

    pub fn duration_ms(&self) -> u32 {
        if self.sample_rate == 0 {
            return 0;
        }
        ((self.frame_count() as u64 * 1000) / self.sample_rate as u64) as u32
    }
}

/// Decodes an in-memory WAV (PCM / IMA-ADPCM) or MP3 audio buffer using `symphonia`.
pub fn decode_audio_bytes(bytes: &[u8], hint_ext: Option<&str>) -> Option<DecodedClip> {
    if bytes.len() < 12 {
        return None;
    }
    let cursor = Cursor::new(bytes.to_vec());
    let mss = MediaSourceStream::new(Box::new(cursor), Default::default());

    let mut hint = Hint::new();
    if let Some(ext) = hint_ext {
        hint.with_extension(ext);
    } else if bytes.starts_with(b"RIFF") {
        hint.with_extension("wav");
    } else {
        hint.with_extension("mp3");
    }

    let probed = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .ok()?;
    let mut format = probed.format;
    let track = format
        .tracks()
        .iter()
        .find(|t| t.codec_params.codec != CODEC_TYPE_NULL)?;
    let track_id = track.id;
    let sample_rate = track.codec_params.sample_rate.unwrap_or(22050);
    let mut channels = track
        .codec_params
        .channels
        .map(|c| c.count() as u16)
        .unwrap_or(1);

    let mut decoder = symphonia::default::get_codecs()
        .make(&track.codec_params, &DecoderOptions::default())
        .ok()?;

    let mut out_samples: Vec<f32> = Vec::new();
    let mut sample_buf: Option<SampleBuffer<f32>> = None;

    loop {
        let packet = match format.next_packet() {
            Ok(p) => p,
            Err(SymphError::IoError(ref e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => {
                break;
            }
            Err(SymphError::ResetRequired) => {
                decoder.reset();
                continue;
            }
            Err(_) => break,
        };
        if packet.track_id() != track_id {
            continue;
        }
        let decoded = match decoder.decode(&packet) {
            Ok(d) => d,
            Err(SymphError::DecodeError(_)) => continue,
            Err(_) => break,
        };
        let spec = *decoded.spec();
        channels = spec.channels.count().max(1) as u16;
        let capacity = decoded.capacity() as u64;
        let buf = sample_buf.get_or_insert_with(|| SampleBuffer::<f32>::new(capacity, spec));
        if buf.capacity() < decoded.frames() * (channels as usize) {
            *buf = SampleBuffer::<f32>::new(capacity, spec);
        }
        buf.copy_interleaved_ref(decoded);
        out_samples.extend_from_slice(buf.samples());
    }

    if out_samples.is_empty() {
        return None;
    }

    Some(DecodedClip {
        samples: out_samples,
        sample_rate,
        channels: channels.max(1),
    })
}

/// Decodes any supported audio buffer (including IMA-ADPCM WAV) into a clean 16-bit PCM RIFF/WAVE file.
pub fn decode_to_wav_pcm16(bytes: &[u8]) -> Option<Vec<u8>> {
    let clip = decode_audio_bytes(bytes, Some("wav"))?;
    let channels = clip.channels.max(1);
    let sample_rate = clip.sample_rate.max(8000);
    let data_bytes = (clip.samples.len() * 2) as u32;
    let riff_size = 36u32.saturating_add(data_bytes);
    let block_align = channels * 2;
    let byte_rate = sample_rate * block_align as u32;

    let mut out = Vec::with_capacity(44 + data_bytes as usize);
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&riff_size.to_le_bytes());
    out.extend_from_slice(b"WAVE");
    out.extend_from_slice(b"fmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes()); // WAVE_FORMAT_PCM
    out.extend_from_slice(&channels.to_le_bytes());
    out.extend_from_slice(&sample_rate.to_le_bytes());
    out.extend_from_slice(&byte_rate.to_le_bytes());
    out.extend_from_slice(&block_align.to_le_bytes());
    out.extend_from_slice(&16u16.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data_bytes.to_le_bytes());
    for &s in &clip.samples {
        let v = (s.clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
        out.extend_from_slice(&v.to_le_bytes());
    }
    Some(out)
}

/// Kind of voice managed by the mixer (mirrors Miles 2D samples, 3D samples, and streams).
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VoiceKind {
    Sample2D = 0,
    Sample3D = 1,
    Stream = 2,
}

#[derive(Clone, Copy, Debug)]
struct Listener3D {
    pos: [f32; 3],
    face: [f32; 3],
    up: [f32; 3],
}

impl Default for Listener3D {
    fn default() -> Self {
        Self {
            pos: [0.0, 0.0, 0.0],
            face: [0.0, 1.0, 0.0],
            up: [0.0, 0.0, 1.0],
        }
    }
}

struct Voice {
    kind: VoiceKind,
    clip: Option<Arc<DecodedClip>>,
    /// Playback position in source frames (fractional for arbitrary pitch/sample-rate resampling).
    cursor: f64,
    playing: bool,
    paused: bool,
    /// Miles loop semantics: `0` = infinite loop, `1` = play once, `n` = play `n` times.
    loop_count: u32,
    volume: f32,
    /// Miles pan: `0.0` = full left, `0.5` = center, `1.0` = full right.
    pan: f32,
    playback_rate: u32,
    /// 3D spatial attributes (only used for `VoiceKind::Sample3D`).
    pos3d: [f32; 3],
    min_dist: f32,
    max_dist: f32,
    occlusion: f32,
    user_data: [i32; 8],
}

impl Voice {
    fn new(kind: VoiceKind) -> Self {
        Self {
            kind,
            clip: None,
            cursor: 0.0,
            playing: false,
            paused: false,
            loop_count: 1,
            volume: 1.0,
            pan: 0.5,
            playback_rate: 22050,
            pos3d: [0.0, 0.0, 0.0],
            min_dist: 50.0,
            max_dist: 600.0,
            occlusion: 0.0,
            user_data: [0; 8],
        }
    }
}

struct MixerState {
    output_rate: u32,
    voices: HashMap<u32, Voice>,
    next_id: u32,
    listener: Listener3D,
    completed: Vec<(u32, VoiceKind)>,
}

impl MixerState {
    fn new(output_rate: u32) -> Self {
        Self {
            output_rate: output_rate.max(8000),
            voices: HashMap::new(),
            next_id: 1,
            listener: Listener3D::default(),
            completed: Vec::new(),
        }
    }

    fn compute_gains(&self, v: &Voice) -> (f32, f32) {
        let vol = v.volume.clamp(0.0, 2.0) * (1.0 - v.occlusion.clamp(0.0, 0.9) * 0.65);
        if v.kind != VoiceKind::Sample3D {
            let p = v.pan.clamp(0.0, 1.0);
            // Constant-power stereo pan law.
            let angle = p * std::f32::consts::FRAC_PI_2;
            return (
                vol * angle.cos() * std::f32::consts::SQRT_2,
                vol * angle.sin() * std::f32::consts::SQRT_2,
            );
        }

        let dx = v.pos3d[0] - self.listener.pos[0];
        let dy = v.pos3d[1] - self.listener.pos[1];
        let dz = v.pos3d[2] - self.listener.pos[2];
        let dist = (dx * dx + dy * dy + dz * dz).sqrt();

        let min_d = v.min_dist.min(v.max_dist).max(1.0);
        let max_d = v.min_dist.max(v.max_dist).max(min_d + 1.0);
        let atten = if dist <= min_d {
            1.0
        } else if dist >= max_d {
            0.0
        } else {
            let t = (dist - min_d) / (max_d - min_d);
            // Smooth quadratic rolloff matching RTS camera scale.
            (1.0 - t) * (1.0 - t)
        };

        // Compute listener right vector = cross(face, up).
        let f = normalize3(self.listener.face, [0.0, 1.0, 0.0]);
        let u = normalize3(self.listener.up, [0.0, 0.0, 1.0]);
        let right = normalize3(
            [
                f[1] * u[2] - f[2] * u[1],
                f[2] * u[0] - f[0] * u[2],
                f[0] * u[1] - f[1] * u[0],
            ],
            [1.0, 0.0, 0.0],
        );

        let pan = if dist > 0.001 {
            let dir = [dx / dist, dy / dist, dz / dist];
            let dot_r =
                (dir[0] * right[0] + dir[1] * right[1] + dir[2] * right[2]).clamp(-1.0, 1.0);
            (dot_r * 0.45 + 0.5).clamp(0.05, 0.95)
        } else {
            0.5
        };

        let angle = pan * std::f32::consts::FRAC_PI_2;
        let g = vol * atten;
        (
            g * angle.cos() * std::f32::consts::SQRT_2,
            g * angle.sin() * std::f32::consts::SQRT_2,
        )
    }

    /// Mixes `frames` interleaved stereo (or N-channel) f32 samples into `out`.
    fn mix_into(&mut self, out: &mut [f32], out_channels: usize) {
        out.fill(0.0);
        if out_channels == 0 {
            return;
        }
        let frames = out.len() / out_channels;
        let out_rate = self.output_rate as f64;
        let mut finished: Vec<(u32, VoiceKind)> = Vec::new();

        // Snapshot listener-relative gains before mutating voices.
        let gains: Vec<(u32, f32, f32)> = self
            .voices
            .iter()
            .filter(|(_, v)| v.playing && !v.paused && v.clip.is_some())
            .map(|(&id, v)| {
                let (gl, gr) = self.compute_gains(v);
                (id, gl, gr)
            })
            .collect();

        for (id, gain_l, gain_r) in gains {
            let Some(v) = self.voices.get_mut(&id) else {
                continue;
            };
            let Some(clip) = v.clip.as_ref() else {
                continue;
            };
            let total_frames = clip.frame_count();
            if total_frames == 0 {
                v.playing = false;
                finished.push((id, v.kind));
                continue;
            }
            let src_ch = clip.channels as usize;
            let rate = if v.playback_rate > 0 {
                v.playback_rate as f64
            } else {
                clip.sample_rate as f64
            };
            let step = (rate / out_rate).clamp(0.05, 8.0);

            for frame_i in 0..frames {
                let idx0 = v.cursor as usize;
                if idx0 >= total_frames {
                    if v.loop_count == 0 {
                        v.cursor %= total_frames as f64;
                    } else if v.loop_count > 1 {
                        v.loop_count -= 1;
                        v.cursor %= total_frames as f64;
                    } else {
                        v.playing = false;
                        finished.push((id, v.kind));
                        break;
                    }
                }
                let idx0 = (v.cursor as usize).min(total_frames - 1);
                let idx1 = (idx0 + 1).min(total_frames - 1);
                let frac = (v.cursor - idx0 as f64) as f32;

                let (sl, sr) = if src_ch == 1 {
                    let s0 = clip.samples[idx0];
                    let s1 = clip.samples[idx1];
                    let s = s0 + (s1 - s0) * frac;
                    (s, s)
                } else {
                    let b0 = idx0 * src_ch;
                    let b1 = idx1 * src_ch;
                    let l = clip.samples[b0] + (clip.samples[b1] - clip.samples[b0]) * frac;
                    let r =
                        clip.samples[b0 + 1] + (clip.samples[b1 + 1] - clip.samples[b0 + 1]) * frac;
                    (l, r)
                };

                let o = frame_i * out_channels;
                out[o] += sl * gain_l;
                if out_channels >= 2 {
                    out[o + 1] += sr * gain_r;
                }
                v.cursor += step;
            }
        }

        // Soft limiter to prevent digital clipping during intense battles.
        for s in out.iter_mut() {
            let x = *s;
            if x > 0.95 {
                *s = 0.95 + (x - 0.95).tanh() * 0.05;
            } else if x < -0.95 {
                *s = -0.95 + (x + 0.95).tanh() * 0.05;
            }
        }

        self.completed.extend(finished);
    }
}

fn normalize3(v: [f32; 3], fallback: [f32; 3]) -> [f32; 3] {
    let len = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
    if len > 1e-5 {
        [v[0] / len, v[1] / len, v[2] / len]
    } else {
        fallback
    }
}

/// Top-level native 64-bit audio engine holding the live `cpal::Stream` and shared mixer state.
pub struct AudioEngine {
    state: Arc<Mutex<MixerState>>,
    _stream: Option<cpal::Stream>,
    device_name: String,
}

// SAFETY: `cpal::Stream` is kept alive for the lifetime of `AudioEngine` behind a global `Mutex`
// and only dropped on shutdown.
unsafe impl Send for AudioEngine {}

impl AudioEngine {
    /// Initializes the default OS audio output device (WASAPI / CoreAudio / ALSA) with a live mixer.
    /// Falls back cleanly to a silent headless mixer when no audio output device is available.
    pub fn new(headless: bool) -> Self {
        if headless {
            return Self {
                state: Arc::new(Mutex::new(MixerState::new(44100))),
                _stream: None,
                device_name: "Headless Null Audio".to_string(),
            };
        }

        let host = cpal::default_host();
        let Some(device) = host.default_output_device() else {
            log::warn!("zhr-audio: no default audio output device found; running silent mixer");
            return Self {
                state: Arc::new(Mutex::new(MixerState::new(44100))),
                _stream: None,
                device_name: "Silent Fallback".to_string(),
            };
        };

        let device_name = device
            .name()
            .unwrap_or_else(|_| "Default Audio Device".to_string());
        let Ok(default_cfg) = device.default_output_config() else {
            return Self {
                state: Arc::new(Mutex::new(MixerState::new(44100))),
                _stream: None,
                device_name,
            };
        };

        let out_rate = default_cfg.sample_rate().0;
        let out_channels = default_cfg.channels() as usize;
        let sample_format = default_cfg.sample_format();
        let config: cpal::StreamConfig = default_cfg.into();
        let state = Arc::new(Mutex::new(MixerState::new(out_rate)));
        let mixer_ref = Arc::clone(&state);

        let err_fn = |err| log::warn!("zhr-audio stream error: {err}");
        let stream_res = match sample_format {
            cpal::SampleFormat::F32 => device.build_output_stream(
                &config,
                move |data: &mut [f32], _| {
                    if let Ok(mut m) = mixer_ref.try_lock() {
                        m.mix_into(data, out_channels);
                    } else {
                        data.fill(0.0);
                    }
                },
                err_fn,
                None,
            ),
            cpal::SampleFormat::I16 => device.build_output_stream(
                &config,
                move |data: &mut [i16], _| {
                    let mut tmp = vec![0.0f32; data.len()];
                    if let Ok(mut m) = mixer_ref.try_lock() {
                        m.mix_into(&mut tmp, out_channels);
                    }
                    for (dst, &src) in data.iter_mut().zip(tmp.iter()) {
                        *dst = (src.clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
                    }
                },
                err_fn,
                None,
            ),
            cpal::SampleFormat::U16 => device.build_output_stream(
                &config,
                move |data: &mut [u16], _| {
                    let mut tmp = vec![0.0f32; data.len()];
                    if let Ok(mut m) = mixer_ref.try_lock() {
                        m.mix_into(&mut tmp, out_channels);
                    }
                    for (dst, &src) in data.iter_mut().zip(tmp.iter()) {
                        *dst = ((src.clamp(-1.0, 1.0) * 0.5 + 0.5) * u16::MAX as f32) as u16;
                    }
                },
                err_fn,
                None,
            ),
            _ => {
                log::warn!("zhr-audio: unsupported sample format {sample_format:?}");
                return Self {
                    state,
                    _stream: None,
                    device_name,
                };
            }
        };

        let stream = match stream_res {
            Ok(s) => {
                if let Err(e) = s.play() {
                    log::warn!("zhr-audio: failed to start output stream: {e}");
                    None
                } else {
                    log::info!(
                        "zhr-audio: native 64-bit audio active on '{device_name}' ({out_rate} Hz, {out_channels} ch)"
                    );
                    Some(s)
                }
            }
            Err(e) => {
                log::warn!("zhr-audio: failed to build output stream: {e}");
                None
            }
        };

        Self {
            state,
            _stream: stream,
            device_name,
        }
    }

    pub fn device_name(&self) -> &str {
        &self.device_name
    }

    pub fn alloc_voice(&self, kind: VoiceKind) -> u32 {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        let id = m.next_id;
        m.next_id = m.next_id.wrapping_add(1).max(1);
        m.voices.insert(id, Voice::new(kind));
        id
    }

    pub fn release_voice(&self, id: u32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        m.voices.remove(&id);
    }

    pub fn reset_voice(&self, id: u32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.playing = false;
            v.paused = false;
            v.cursor = 0.0;
            v.loop_count = 1;
            v.volume = 1.0;
            v.pan = 0.5;
            v.occlusion = 0.0;
        }
    }

    pub fn load_voice_bytes(&self, id: u32, bytes: &[u8], hint_ext: Option<&str>) -> bool {
        let Some(clip) = decode_audio_bytes(bytes, hint_ext) else {
            return false;
        };
        let rate = clip.sample_rate;
        let arc = Arc::new(clip);
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.clip = Some(arc);
            v.cursor = 0.0;
            v.playback_rate = rate;
            v.playing = false;
            v.paused = false;
            true
        } else {
            false
        }
    }

    pub fn start_voice(&self, id: u32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.cursor = 0.0;
            v.paused = false;
            v.playing = v.clip.is_some();
        }
    }

    pub fn stop_voice(&self, id: u32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.playing = false;
            v.paused = false;
        }
    }

    pub fn pause_voice(&self, id: u32, paused: bool) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.paused = paused;
            if !paused && v.clip.is_some() {
                v.playing = true;
            }
        }
    }

    pub fn set_volume_pan(&self, id: u32, volume: f32, pan: f32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.volume = volume.clamp(0.0, 2.0);
            v.pan = pan.clamp(0.0, 1.0);
        }
    }

    pub fn get_volume_pan(&self, id: u32) -> (f32, f32) {
        let m = self.state.lock().expect("audio mixer poisoned");
        m.voices
            .get(&id)
            .map(|v| (v.volume, v.pan))
            .unwrap_or((1.0, 0.5))
    }

    pub fn set_loop_count(&self, id: u32, count: u32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.loop_count = count;
        }
    }

    pub fn get_loop_count(&self, id: u32) -> u32 {
        let m = self.state.lock().expect("audio mixer poisoned");
        m.voices.get(&id).map(|v| v.loop_count).unwrap_or(1)
    }

    pub fn set_playback_rate(&self, id: u32, rate: u32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.playback_rate = rate.clamp(4000, 192_000);
        }
    }

    pub fn get_playback_rate(&self, id: u32) -> u32 {
        let m = self.state.lock().expect("audio mixer poisoned");
        m.voices.get(&id).map(|v| v.playback_rate).unwrap_or(22050)
    }

    pub fn get_ms_position(&self, id: u32) -> (u32, u32) {
        let m = self.state.lock().expect("audio mixer poisoned");
        let Some(v) = m.voices.get(&id) else {
            return (0, 0);
        };
        let Some(clip) = v.clip.as_ref() else {
            return (0, 0);
        };
        let total = clip.duration_ms();
        let cur = if clip.sample_rate > 0 {
            ((v.cursor as u64 * 1000) / clip.sample_rate as u64) as u32
        } else {
            0
        };
        (total, cur.min(total))
    }

    pub fn set_ms_position(&self, id: u32, ms: u32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id)
            && let Some(clip) = v.clip.as_ref()
        {
            let target = (ms as f64 * clip.sample_rate as f64) / 1000.0;
            v.cursor = target.clamp(0.0, clip.frame_count().saturating_sub(1) as f64);
        }
    }

    pub fn set_3d_position(&self, id: u32, x: f32, y: f32, z: f32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.pos3d = [x, y, z];
        }
    }

    pub fn set_3d_distances(&self, id: u32, d1: f32, d2: f32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.min_dist = d1.min(d2).max(1.0);
            v.max_dist = d1.max(d2).max(v.min_dist + 1.0);
        }
    }

    pub fn set_3d_occlusion(&self, id: u32, occlusion: f32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id) {
            v.occlusion = occlusion.clamp(0.0, 1.0);
        }
    }

    pub fn set_listener_position(&self, x: f32, y: f32, z: f32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        m.listener.pos = [x, y, z];
    }

    pub fn set_listener_orientation(&self, fx: f32, fy: f32, fz: f32, ux: f32, uy: f32, uz: f32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        m.listener.face = [fx, fy, fz];
        m.listener.up = [ux, uy, uz];
    }

    pub fn set_user_data(&self, id: u32, index: usize, value: i32) {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if let Some(v) = m.voices.get_mut(&id)
            && index < v.user_data.len()
        {
            v.user_data[index] = value;
        }
    }

    pub fn get_user_data(&self, id: u32, index: usize) -> i32 {
        let m = self.state.lock().expect("audio mixer poisoned");
        m.voices
            .get(&id)
            .and_then(|v| v.user_data.get(index).copied())
            .unwrap_or(0)
    }

    /// Drains one finished voice `(voice_id, kind)` for safe main-thread EOS callback dispatch.
    pub fn pop_completed(&self) -> Option<(u32, VoiceKind)> {
        let mut m = self.state.lock().expect("audio mixer poisoned");
        if m.completed.is_empty() {
            None
        } else {
            Some(m.completed.remove(0))
        }
    }

    /// Renders `frames` stereo samples synchronously (for deterministic unit tests).
    pub fn render_offline_stereo(&self, frames: usize) -> Vec<f32> {
        let mut buf = vec![0.0f32; frames * 2];
        let mut m = self.state.lock().expect("audio mixer poisoned");
        m.mix_into(&mut buf, 2);
        buf
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_test_wav(sample_rate: u32, freq_hz: f32, num_frames: usize) -> Vec<u8> {
        let data_bytes = (num_frames * 2) as u32;
        let riff_size = 36 + data_bytes;
        let mut out = Vec::with_capacity((riff_size + 8) as usize);
        out.extend_from_slice(b"RIFF");
        out.extend_from_slice(&riff_size.to_le_bytes());
        out.extend_from_slice(b"WAVE");
        out.extend_from_slice(b"fmt ");
        out.extend_from_slice(&16u32.to_le_bytes());
        out.extend_from_slice(&1u16.to_le_bytes()); // PCM
        out.extend_from_slice(&1u16.to_le_bytes()); // Mono
        out.extend_from_slice(&sample_rate.to_le_bytes());
        out.extend_from_slice(&(sample_rate * 2).to_le_bytes());
        out.extend_from_slice(&2u16.to_le_bytes());
        out.extend_from_slice(&16u16.to_le_bytes());
        out.extend_from_slice(b"data");
        out.extend_from_slice(&data_bytes.to_le_bytes());
        for i in 0..num_frames {
            let t = i as f32 / sample_rate as f32;
            let s = (t * freq_hz * std::f32::consts::TAU).sin();
            let v = (s * 24000.0) as i16;
            out.extend_from_slice(&v.to_le_bytes());
        }
        out
    }

    #[test]
    fn decodes_wav_and_mixes_3d_panning_and_eos() {
        let wav = make_test_wav(22050, 440.0, 2205); // 100ms tone
        let engine = AudioEngine::new(true);
        let id = engine.alloc_voice(VoiceKind::Sample3D);
        assert!(engine.load_voice_bytes(id, &wav, Some("wav")));
        let (total_ms, _) = engine.get_ms_position(id);
        assert!((total_ms as i32 - 100).abs() <= 2);

        // Place sound to the right of listener (+X).
        engine.set_listener_position(0.0, 0.0, 0.0);
        engine.set_listener_orientation(0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
        engine.set_3d_distances(id, 10.0, 200.0);
        engine.set_3d_position(id, 40.0, 0.0, 0.0);
        engine.start_voice(id);

        let stereo = engine.render_offline_stereo(4410); // 100ms at 44100 Hz
        let mut energy_l = 0.0f32;
        let mut energy_r = 0.0f32;
        for ch in stereo.chunks_exact(2) {
            energy_l += ch[0].abs();
            energy_r += ch[1].abs();
        }
        assert!(
            energy_r > energy_l * 2.0,
            "sound at +X should pan right: L={energy_l}, R={energy_r}"
        );

        // Render slightly past end -> EOS should fire.
        let _ = engine.render_offline_stereo(256);
        assert_eq!(engine.pop_completed(), Some((id, VoiceKind::Sample3D)));
    }
}
