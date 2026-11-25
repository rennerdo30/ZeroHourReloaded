#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Core runtime info ---
const char* zhr_rust_banner(void);
uint32_t zhr_rust_pointer_bits(void);

// --- WW3D2-compatible vertex layout (matches zhr_vulkan::types::VertexWw3d) ---
typedef struct ZhrVertexWw3d {
    float position[3];
    float normal[3];
    float color[4];
    float uv0[2];
    float uv1[2];
} ZhrVertexWw3d;

// --- Vulkan 1.3 renderer C-ABI (mirrors WW3D2::Renderer) ---
int32_t zhr_vk_init_headless(uint32_t width, uint32_t height, bool enable_validation);

#ifdef _WIN32
int32_t zhr_vk_init_win32(
    void* hwnd,
    void* hinstance,
    uint32_t width,
    uint32_t height,
    bool vsync,
    bool enable_validation);
#elif defined(__APPLE__)
int32_t zhr_vk_init_macos(
    void* ns_view,
    uint32_t width,
    uint32_t height,
    bool vsync,
    bool enable_validation);
#elif defined(__linux__)
int32_t zhr_vk_init_xlib(
    void* display,
    uint64_t window,
    uint32_t width,
    uint32_t height,
    bool vsync,
    bool enable_validation);
int32_t zhr_vk_init_wayland(
    void* display,
    void* surface,
    uint32_t width,
    uint32_t height,
    bool vsync,
    bool enable_validation);
#endif

void zhr_vk_shutdown(void);

int32_t zhr_vk_device_count(void);
int32_t zhr_vk_selected_device(void);
const char* zhr_vk_device_name(int32_t index);
void zhr_vk_get_resolution(uint32_t* out_width, uint32_t* out_height);
bool zhr_vk_is_windowed(void);
bool zhr_vk_is_frame_active(void);
int32_t zhr_vk_resize(uint32_t width, uint32_t height);

int32_t zhr_vk_begin_frame(void);
int32_t zhr_vk_end_frame(void);

int32_t zhr_vk_clear(
    bool clear_color,
    bool clear_depth_stencil,
    float r,
    float g,
    float b,
    float a,
    float depth,
    uint32_t stencil);

void zhr_vk_set_viewport(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    float min_z,
    float max_z);

// kind: 0 = World, 1 = View, 2 = Projection (16 floats, column-major)
void zhr_vk_set_transform(uint32_t kind, const float* matrix16);
void zhr_vk_set_ambient(float r, float g, float b);
void zhr_vk_set_directional_light(
    uint32_t index,
    float dx,
    float dy,
    float dz,
    float r,
    float g,
    float b);
void zhr_vk_set_point_light(
    uint32_t index,
    float px,
    float py,
    float pz,
    float r,
    float g,
    float b,
    float range);
void zhr_vk_clear_light(uint32_t index);
void zhr_vk_set_lighting_enabled(bool enabled);
void zhr_vk_set_alpha_test_ref(float alpha_ref);
void zhr_vk_set_fog(bool enable, float r, float g, float b, float start, float end);
void zhr_vk_set_shader(uint32_t ww3d_shader_bits);
void zhr_vk_set_stencil_state(
    bool enabled,
    uint32_t func,
    uint32_t fail_op,
    uint32_t depth_fail_op,
    uint32_t pass_op,
    uint32_t compare_mask,
    uint32_t write_mask,
    uint32_t reference);

// format: 0=RGBA8, 1=BGRA8, 2=BC1(DXT1), 3=BC2(DXT3), 4=BC3(DXT5)
uint32_t zhr_vk_create_texture_2d(
    uint32_t width,
    uint32_t height,
    uint32_t format,
    const uint8_t* pixels,
    size_t byte_len,
    bool linear_filter,
    bool clamp_edge);
uint32_t zhr_vk_create_texture_2d_mips(
    uint32_t width,
    uint32_t height,
    uint32_t mip_count,
    uint32_t format,
    const uint8_t* pixels,
    size_t byte_len,
    bool linear_filter,
    bool clamp_edge);
void zhr_vk_set_graphics_mode(uint32_t mode, bool pbr_lighting, bool post_fx);
void zhr_vk_set_remaster_textures(bool enabled);
void zhr_vk_set_material_hint(uint32_t hint);
void zhr_vk_add_wind_impulse(float x, float y, float z, float strength);
uint32_t zhr_vk_get_graphics_mode(void);
void zhr_vk_set_freeze_time(bool frozen);
void zhr_vk_set_wireframe(bool enabled);
void zhr_vk_request_capture(const char* png_path, const char* stats_path);
/// Synthesizes an `out_dim * out_dim` 8-bit displacement heightmap (centered at 128) from raw or BC1-BC3 texture data.
int32_t zhr_remaster_build_heightmap(
    uint32_t width,
    uint32_t height,
    uint32_t format_code,
    const uint8_t* data,
    size_t byte_len,
    uint32_t out_dim,
    uint8_t* out_height_r8);
uint32_t zhr_vk_create_texture_rgba8(
    uint32_t width,
    uint32_t height,
    const uint8_t* pixels,
    size_t byte_len,
    bool linear_filter,
    bool clamp_edge);
void zhr_vk_destroy_texture(uint32_t texture_id);
void zhr_vk_set_texture(uint32_t texture_id);
void zhr_vk_set_texture_stage(uint32_t stage, uint32_t texture_id);
void zhr_vk_set_texture_factor(float r, float g, float b, float a);
void zhr_vk_set_stage_state(
    uint32_t stage,
    uint32_t color_op,
    uint32_t color_args,
    uint32_t alpha_op,
    uint32_t alpha_args);
// stage: 0 or 1; mode: 0=passthrough, 1=transform UV, 2=camera-space pos, 3=camera-space pos * matrix
void zhr_vk_set_tex_gen(uint32_t stage, uint32_t mode, uint32_t src_idx, const float* matrix16);

uint32_t zhr_vk_create_vertex_buffer(const ZhrVertexWw3d* vertices, size_t vertex_count);
uint32_t zhr_vk_create_index_buffer(const uint16_t* indices, size_t index_count);
void zhr_vk_destroy_buffer(uint32_t buffer_id);
int32_t zhr_vk_draw_indexed(
    uint32_t vb_id,
    uint32_t ib_id,
    uint32_t index_count,
    uint32_t first_index,
    int32_t vertex_offset);

int32_t zhr_vk_draw_dynamic(
    const ZhrVertexWw3d* vertices,
    size_t vertex_count,
    const uint16_t* indices,
    size_t index_count);

// --- Native 64-bit Cross-Platform Audio C-ABI (replaces 32-bit mss32.dll) ---
int32_t zhr_audio_init(bool headless);
void zhr_audio_shutdown(void);
const char* zhr_audio_device_name(void);
uint32_t zhr_audio_alloc_voice(uint32_t kind); // 0=2D, 1=3D, 2=Stream
void zhr_audio_release_voice(uint32_t voice_id);
void zhr_audio_reset_voice(uint32_t voice_id);
int32_t zhr_audio_load_bytes(
    uint32_t voice_id,
    const uint8_t* bytes,
    size_t byte_len,
    const char* hint_ext);
void zhr_audio_start(uint32_t voice_id);
void zhr_audio_stop(uint32_t voice_id);
void zhr_audio_pause(uint32_t voice_id, bool paused);
void zhr_audio_set_volume_pan(uint32_t voice_id, float volume, float pan);
void zhr_audio_get_volume_pan(uint32_t voice_id, float* out_volume, float* out_pan);
void zhr_audio_set_loop_count(uint32_t voice_id, uint32_t count);
uint32_t zhr_audio_get_loop_count(uint32_t voice_id);
void zhr_audio_set_playback_rate(uint32_t voice_id, uint32_t rate);
uint32_t zhr_audio_get_playback_rate(uint32_t voice_id);
void zhr_audio_get_ms_position(uint32_t voice_id, uint32_t* out_total_ms, uint32_t* out_current_ms);
void zhr_audio_set_ms_position(uint32_t voice_id, uint32_t ms);
void zhr_audio_set_3d_position(uint32_t voice_id, float x, float y, float z);
void zhr_audio_set_3d_distances(uint32_t voice_id, float d1, float d2);
void zhr_audio_set_3d_occlusion(uint32_t voice_id, float occlusion);
void zhr_audio_set_listener_position(float x, float y, float z);
void zhr_audio_set_listener_orientation(
    float fx, float fy, float fz,
    float ux, float uy, float uz);
void zhr_audio_set_user_data(uint32_t voice_id, uint32_t index, int32_t value);
int32_t zhr_audio_get_user_data(uint32_t voice_id, uint32_t index);
int32_t zhr_audio_pop_completed(uint32_t* out_voice_id, uint32_t* out_kind);
int32_t zhr_audio_decode_wav_pcm16(
    const uint8_t* bytes,
    size_t byte_len,
    uint8_t** out_ptr,
    size_t* out_len);
void zhr_audio_free_buffer(uint8_t* ptr, size_t len);
void zhr_audio_video_start(uint32_t sample_rate, float volume);
void zhr_audio_video_push_f32(const float* samples, size_t sample_count, uint16_t channels);
void zhr_audio_video_set_volume(float volume);
void zhr_audio_video_stop(void);

// --- Cross-Platform SDL3 Input Translation (zhr-platform) ---
uint8_t zhr_platform_sdl_scancode_to_zh_key(uint32_t sdl_scancode);

#ifdef __cplusplus
}
#endif
