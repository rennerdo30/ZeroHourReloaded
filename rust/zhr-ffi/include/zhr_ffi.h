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
#endif

void zhr_vk_shutdown(void);

int32_t zhr_vk_device_count(void);
int32_t zhr_vk_selected_device(void);
const char* zhr_vk_device_name(int32_t index);
void zhr_vk_get_resolution(uint32_t* out_width, uint32_t* out_height);
bool zhr_vk_is_windowed(void);

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
void zhr_vk_set_fog(bool enable, float r, float g, float b, float start, float end);
void zhr_vk_set_shader(uint32_t ww3d_shader_bits);

uint32_t zhr_vk_create_texture_rgba8(
    uint32_t width,
    uint32_t height,
    const uint8_t* pixels,
    size_t byte_len,
    bool linear_filter,
    bool clamp_edge);
void zhr_vk_set_texture(uint32_t texture_id);

int32_t zhr_vk_draw_dynamic(
    const ZhrVertexWw3d* vertices,
    size_t vertex_count,
    const uint16_t* indices,
    size_t index_count);

#ifdef __cplusplus
}
#endif
