/*
**	ZeroHourReloaded — Native Vulkan 1.3 Backend for WW3D2::Renderer
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "WW3D2/Renderer.h"

#include "WW3D2/light.h"
#include "WW3D2/lightenvironment.h"
#include "WWDebug/wwdebug.h"
#include "WWMath/matrix3d.h"
#include "WWMath/matrix4.h"
#include "WWMath/vector3.h"

#include <zhr_ffi.h>

#include <cstdint>

namespace {

bool g_initialized = false;
bool g_lite = false;
Matrix4x4 g_cached_transforms[3] = {
    Matrix4x4(true),
    Matrix4x4(true),
    Matrix4x4(true),
};

// WWMath Matrix4x4 stores rows (`m[row][col]`); zhr_vk_set_transform expects column-major floats.
void To_Column_Major(const Matrix4x4 & m, float out[16])
{
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            out[col * 4 + row] = m[row][col];
        }
    }
}

uint32_t To_Vk_Transform_Kind(RenderTransform transform)
{
    switch (transform)
    {
    case RenderTransform_World:
        return 0;
    case RenderTransform_View:
        return 1;
    case RenderTransform_Projection:
        return 2;
    }
    return 0;
}

} // namespace

bool Renderer::Init(void * window, bool lite)
{
    if (g_initialized)
    {
        return true;
    }

    int32_t ok = 0;
#ifdef _WIN32
    if (window != nullptr && !lite)
    {
        ok = zhr_vk_init_win32(window, nullptr, 1280, 720, true, false);
    }
    else
#endif
    {
        ok = zhr_vk_init_headless(1280, 720, false);
    }

    if (ok == 0)
    {
        return false;
    }

    g_initialized = true;
    g_lite = lite;
    return true;
}

void Renderer::Shutdown()
{
    if (g_initialized)
    {
        zhr_vk_shutdown();
    }
    g_initialized = false;
    g_lite = false;
}

int Renderer::Get_Render_Device_Count()
{
    return static_cast<int>(zhr_vk_device_count());
}

int Renderer::Get_Render_Device()
{
    return static_cast<int>(zhr_vk_selected_device());
}

const char * Renderer::Get_Render_Device_Name(int device_index)
{
    return zhr_vk_device_name(static_cast<int32_t>(device_index));
}

void Renderer::Get_Device_Resolution(int & width, int & height, int & bits, bool & windowed)
{
    uint32_t w = 0;
    uint32_t h = 0;
    zhr_vk_get_resolution(&w, &h);
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    bits = 32;
    windowed = zhr_vk_is_windowed();
}

void Renderer::Get_Render_Target_Resolution(int & width, int & height, int & bits, bool & windowed)
{
    Get_Device_Resolution(width, height, bits, windowed);
}

int Renderer::Get_Device_Resolution_Width()
{
    uint32_t w = 0;
    uint32_t h = 0;
    zhr_vk_get_resolution(&w, &h);
    return static_cast<int>(w);
}

int Renderer::Get_Device_Resolution_Height()
{
    uint32_t w = 0;
    uint32_t h = 0;
    zhr_vk_get_resolution(&w, &h);
    return static_cast<int>(h);
}

bool Renderer::Is_Windowed()
{
    return zhr_vk_is_windowed();
}

int Renderer::Get_Texture_Bitdepth()
{
    return 32;
}

int Renderer::Get_Swap_Interval()
{
    return 1;
}

bool Renderer::Has_Stencil()
{
    return true;
}

WW3DFormat Renderer::Get_Back_Buffer_Format()
{
    return WW3D_FORMAT_A8R8G8B8;
}

void Renderer::Set_Gamma(float /*gamma*/, float /*bright*/, float /*contrast*/, bool /*calibrate*/, bool /*uselimit*/)
{
    // Gamma is handled in post-process / swapchain presentation on Vulkan.
}

void Renderer::Begin_Scene()
{
    zhr_vk_begin_frame();
}

void Renderer::End_Scene(bool /*flip_frame*/)
{
    zhr_vk_end_frame();
}

void Renderer::Flip_To_Primary()
{
    // Swapchain presentation happens inside zhr_vk_end_frame().
}

void Renderer::Clear(bool clear_color, bool clear_z_stencil,
                     const Vector3 & color,
                     float dest_alpha, float z, unsigned int stencil)
{
    zhr_vk_clear(clear_color, clear_z_stencil, color.X, color.Y, color.Z, dest_alpha, z, stencil);
}

void Renderer::Set_Viewport(const RenderViewport & viewport)
{
    zhr_vk_set_viewport(viewport.x, viewport.y, viewport.width, viewport.height, viewport.min_z, viewport.max_z);
}

void Renderer::Invalidate_Cached_Render_States()
{
    // Vulkan backend tracks pipeline/descriptor cache per frame automatically.
}

void Renderer::Set_Transform(RenderTransform transform, const Matrix4x4 & m)
{
    const uint32_t kind = To_Vk_Transform_Kind(transform);
    g_cached_transforms[kind] = m;
    float col_major[16];
    To_Column_Major(m, col_major);
    zhr_vk_set_transform(kind, col_major);
}

void Renderer::Set_Transform(RenderTransform transform, const Matrix3D & m)
{
    Matrix4x4 m4(m);
    Set_Transform(transform, m4);
}

void Renderer::Get_Transform(RenderTransform transform, Matrix4x4 & m)
{
    m = g_cached_transforms[To_Vk_Transform_Kind(transform)];
}

void Renderer::Set_World_Identity()
{
    Set_Transform(RenderTransform_World, Matrix4x4(true));
}

void Renderer::Set_View_Identity()
{
    Set_Transform(RenderTransform_View, Matrix4x4(true));
}

void Renderer::Set_Projection_Transform_With_Z_Bias(const Matrix4x4 & matrix, float znear, float zfar)
{
    Matrix4x4 biased = matrix;
    if (zfar > znear)
    {
        const float scale = (zfar - 0.0001f) / zfar;
        biased[2][2] *= scale;
        biased[2][3] *= scale;
    }
    Set_Transform(RenderTransform_Projection, biased);
}

void Renderer::Set_Ambient(const Vector3 & color)
{
    zhr_vk_set_ambient(color.X, color.Y, color.Z);
}

void Renderer::Set_Light_Environment(LightEnvironmentClass * light_env)
{
    if (light_env == nullptr)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            zhr_vk_clear_light(i);
        }
        return;
    }

    const Vector3 & ambient = light_env->Get_Equivalent_Ambient();
    zhr_vk_set_ambient(ambient.X, ambient.Y, ambient.Z);

    const int count = light_env->Get_Light_Count();
    for (int i = 0; i < 4; ++i)
    {
        if (i >= count)
        {
            zhr_vk_clear_light(static_cast<uint32_t>(i));
            continue;
        }
        if (light_env->isPointLight(i))
        {
            const Vector3 & pos = light_env->getPointCenter(i);
            const Vector3 & diff = light_env->getPointDiffuse(i);
            zhr_vk_set_point_light(
                static_cast<uint32_t>(i),
                pos.X, pos.Y, pos.Z,
                diff.X, diff.Y, diff.Z,
                light_env->getPointOrad(i));
        }
        else
        {
            const Vector3 & dir = light_env->Get_Light_Direction(i);
            const Vector3 & diff = light_env->Get_Light_Diffuse(i);
            zhr_vk_set_directional_light(
                static_cast<uint32_t>(i),
                dir.X, dir.Y, dir.Z,
                diff.X, diff.Y, diff.Z);
        }
    }
}

void Renderer::Set_Light(unsigned index, const LightClass & light)
{
    if (index >= 4)
    {
        return;
    }

    Vector3 diffuse(0.0f, 0.0f, 0.0f);
    light.Get_Diffuse(&diffuse);
    const float intensity = light.Get_Intensity();
    diffuse *= intensity;

    if (light.Get_Type() == LightClass::DIRECTIONAL)
    {
        Vector3 dir(0.0f, 0.0f, -1.0f);
        light.Get_Transform().Get_Z_Vector(&dir);
        zhr_vk_set_directional_light(index, -dir.X, -dir.Y, -dir.Z, diffuse.X, diffuse.Y, diffuse.Z);
    }
    else
    {
        const Vector3 pos = light.Get_Transform().Get_Translation();
        zhr_vk_set_point_light(
            index,
            pos.X, pos.Y, pos.Z,
            diffuse.X, diffuse.Y, diffuse.Z,
            light.Get_Attenuation_Range());
    }
}

void Renderer::Clear_Light(unsigned index)
{
    if (index < 4)
    {
        zhr_vk_clear_light(index);
    }
}

void Renderer::Set_Fog(bool enable, const Vector3 & color, float start, float end)
{
    zhr_vk_set_fog(enable, color.X, color.Y, color.Z, start, end);
}
