/*
**	ZeroHourReloaded — Native Vulkan 1.3 Bridge for WW3D2 / DX8Wrapper
**
**	Implements IDirect3D8 / IDirect3DDevice8 / resources in-process directly on top of
**	`rust/zhr-vulkan` (`zhr_ffi.h`), enabling all WW3D2 & GeneralsMD subsystems (UI, fonts,
**	terrain, W3D meshes, particles, sorting renderer, texture loader) to run natively on
**	64-bit Vulkan 1.3 without d3d8.dll.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "VulkanDX8Bridge.h"

#include <zhr_ffi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::atomic<bool> g_zhr_mesh_smoothing_enabled{true};
std::atomic<bool> g_zhr_title_banner_dirty{true};
std::atomic<bool> g_zhr_terrain_pass_active{false};
std::atomic<uint64_t> g_zhr_next_buf_id{1};
std::atomic<uint64_t> g_zhr_frame_counter{1};

struct ZhrTerrainHeightmapState
{
    const uint8_t* data{nullptr};
    int32_t width{0};
    int32_t height{0};
    int32_t border{0};
    float xy_factor{10.0f};
    float height_scale{0.625f};
    uint64_t generation{0};
};

ZhrTerrainHeightmapState g_zhr_terrain_hm{};

inline float FetchTerrainGridZ(const ZhrTerrainHeightmapState& hm, int32_t ix, int32_t iy) noexcept
{
    const int32_t cx = std::clamp(ix, 0, hm.width - 1);
    const int32_t cy = std::clamp(iy, 0, hm.height - 1);
    return static_cast<float>(hm.data[static_cast<size_t>(cy) * static_cast<size_t>(hm.width) + static_cast<size_t>(cx)]) * hm.height_scale;
}

inline float SampleTerrainLinearZ(const ZhrTerrainHeightmapState& hm, float wx, float wy) noexcept
{
    const float gx = wx / hm.xy_factor + static_cast<float>(hm.border);
    const float gy = wy / hm.xy_factor + static_cast<float>(hm.border);
    const int32_t ix = static_cast<int32_t>(std::floor(gx));
    const int32_t iy = static_cast<int32_t>(std::floor(gy));
    const float fx = std::clamp(gx - static_cast<float>(ix), 0.0f, 1.0f);
    const float fy = std::clamp(gy - static_cast<float>(iy), 0.0f, 1.0f);
    const float h00 = FetchTerrainGridZ(hm, ix, iy);
    const float h10 = FetchTerrainGridZ(hm, ix + 1, iy);
    const float h01 = FetchTerrainGridZ(hm, ix, iy + 1);
    const float h11 = FetchTerrainGridZ(hm, ix + 1, iy + 1);
    const float hx0 = h00 + (h10 - h00) * fx;
    const float hx1 = h01 + (h11 - h01) * fx;
    return hx0 + (hx1 - hx0) * fy;
}

// Fritsch-Carlson Monotone Cubic Hermite 1D evaluator + exact derivative:
// - C1 continuous and interpolating at all integer grid knots (zero cracks across tiles).
// - Strictly monotone: flat buildable plateaus adjacent to cliffs stay 100.00% dead-flat
//   (zero Gibbs overshoot / zero building or bib clipping).
inline void EvalMonotoneCubic(float p0, float p1, float p2, float p3, float t, float& out_val, float& out_deriv) noexcept
{
    const float d0 = p1 - p0;
    const float d1 = p2 - p1;
    const float d2 = p3 - p2;
    const float m1 = (d0 * d1 > 0.0f) ? ((2.0f * d0 * d1) / (d0 + d1)) : 0.0f;
    const float m2 = (d1 * d2 > 0.0f) ? ((2.0f * d1 * d2) / (d1 + d2)) : 0.0f;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    out_val = h00 * p1 + h10 * m1 + h01 * p2 + h11 * m2;
    out_deriv = (6.0f * t2 - 6.0f * t) * p1
              + (3.0f * t2 - 4.0f * t + 1.0f) * m1
              + (-6.0f * t2 + 6.0f * t) * p2
              + (3.0f * t2 - 2.0f * t) * m2;
}

inline void SampleTerrainSmooth(
    const ZhrTerrainHeightmapState& hm,
    float wx,
    float wy,
    float& out_z,
    float out_normal[3]) noexcept
{
    const float gx = wx / hm.xy_factor + static_cast<float>(hm.border);
    const float gy = wy / hm.xy_factor + static_cast<float>(hm.border);
    const int32_t ix = static_cast<int32_t>(std::floor(gx));
    const int32_t iy = static_cast<int32_t>(std::floor(gy));
    const float fx = std::clamp(gx - static_cast<float>(ix), 0.0f, 1.0f);
    const float fy = std::clamp(gy - static_cast<float>(iy), 0.0f, 1.0f);

    float h[4][4];
    float min_h = 1e9f;
    float max_h = -1e9f;
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            const float v = FetchTerrainGridZ(hm, ix - 1 + c, iy - 1 + r);
            h[r][c] = v;
            if (v < min_h) min_h = v;
            if (v > max_h) max_h = v;
        }
    }

    if (max_h - min_h < 1e-4f)
    {
        out_z = h[1][1];
        out_normal[0] = 0.0f;
        out_normal[1] = 0.0f;
        out_normal[2] = 1.0f;
        return;
    }

    float row_val[4];
    float row_dx[4];
    for (int r = 0; r < 4; ++r)
    {
        EvalMonotoneCubic(h[r][0], h[r][1], h[r][2], h[r][3], fx, row_val[r], row_dx[r]);
    }

    float z_val = 0.0f;
    float dz_dy_grid = 0.0f;
    EvalMonotoneCubic(row_val[0], row_val[1], row_val[2], row_val[3], fy, z_val, dz_dy_grid);

    float dz_dx_grid = 0.0f;
    float dummy_dxy = 0.0f;
    EvalMonotoneCubic(row_dx[0], row_dx[1], row_dx[2], row_dx[3], fy, dz_dx_grid, dummy_dxy);

    const float inv_xy = 1.0f / hm.xy_factor;
    const float nx = -dz_dx_grid * inv_xy;
    const float ny = -dz_dy_grid * inv_xy;
    const float nz = 1.0f;
    const float inv_len = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    out_z = z_val;
    out_normal[0] = nx * inv_len;
    out_normal[1] = ny * inv_len;
    out_normal[2] = nz * inv_len;
}

// --- Helpers ---

bool IsValidationRequested()
{
    const char* env = std::getenv("ZHR_VK_VALIDATION");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool IsCompressedFormat(D3DFORMAT fmt)
{
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 ||
           fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
}

uint32_t CalcBytesPerPixel(D3DFORMAT fmt)
{
    switch (fmt)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D32:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_X8L8V8U8:
        return 4;
    case D3DFMT_R8G8B8:
        return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_D16:
    case D3DFMT_D15S1:
        return 2;
    case D3DFMT_L8:
    case D3DFMT_A8:
    case D3DFMT_P8:
    case D3DFMT_R3G3B2:
    case D3DFMT_A4L4:
        return 1;
    default:
        return 4;
    }
}

uint32_t CalcSurfacePitch(D3DFORMAT fmt, uint32_t width)
{
    const uint32_t w = width > 0 ? width : 1;
    if (fmt == D3DFMT_DXT1)
    {
        return ((w + 3) / 4) * 8;
    }
    if (IsCompressedFormat(fmt))
    {
        return ((w + 3) / 4) * 16;
    }
    return w * CalcBytesPerPixel(fmt);
}

uint32_t CalcSurfaceByteSize(D3DFORMAT fmt, uint32_t width, uint32_t height)
{
    const uint32_t w = width > 0 ? width : 1;
    const uint32_t h = height > 0 ? height : 1;
    if (IsCompressedFormat(fmt))
    {
        return CalcSurfacePitch(fmt, w) * ((h + 3) / 4);
    }
    return CalcSurfacePitch(fmt, w) * h;
}

uint32_t CalcMipCount(uint32_t width, uint32_t height, uint32_t requested)
{
    if (requested > 0)
    {
        return requested;
    }
    uint32_t levels = 1;
    uint32_t w = width > 0 ? width : 1;
    uint32_t h = height > 0 ? height : 1;
    while (w > 1 || h > 1)
    {
        w = w > 1 ? (w >> 1) : 1;
        h = h > 1 ? (h >> 1) : 1;
        ++levels;
    }
    return levels;
}

// Convert any uncompressed D3D8 format surface to tightly-packed RGBA8 bytes.
void ConvertToRgba8(
    D3DFORMAT fmt,
    uint32_t width,
    uint32_t height,
    const uint8_t* src,
    uint32_t src_pitch,
    std::vector<uint8_t>& out_rgba)
{
    out_rgba.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* dst = out_rgba.data();

    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* row = src + static_cast<size_t>(y) * src_pitch;
        uint8_t* drow = dst + static_cast<size_t>(y) * width * 4;
        switch (fmt)
        {
        case D3DFMT_A8R8G8B8:
            for (uint32_t x = 0; x < width; ++x)
            {
                drow[x * 4 + 0] = row[x * 4 + 2];
                drow[x * 4 + 1] = row[x * 4 + 1];
                drow[x * 4 + 2] = row[x * 4 + 0];
                drow[x * 4 + 3] = row[x * 4 + 3];
            }
            break;
        case D3DFMT_X8R8G8B8:
            for (uint32_t x = 0; x < width; ++x)
            {
                drow[x * 4 + 0] = row[x * 4 + 2];
                drow[x * 4 + 1] = row[x * 4 + 1];
                drow[x * 4 + 2] = row[x * 4 + 0];
                drow[x * 4 + 3] = 255;
            }
            break;
        case D3DFMT_R8G8B8:
            for (uint32_t x = 0; x < width; ++x)
            {
                drow[x * 4 + 0] = row[x * 3 + 2];
                drow[x * 4 + 1] = row[x * 3 + 1];
                drow[x * 4 + 2] = row[x * 3 + 0];
                drow[x * 4 + 3] = 255;
            }
            break;
        case D3DFMT_R5G6B5:
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint16_t p = reinterpret_cast<const uint16_t*>(row)[x];
                const uint8_t r = static_cast<uint8_t>((p >> 11) & 0x1F);
                const uint8_t g = static_cast<uint8_t>((p >> 5) & 0x3F);
                const uint8_t b = static_cast<uint8_t>(p & 0x1F);
                drow[x * 4 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
                drow[x * 4 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
                drow[x * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
                drow[x * 4 + 3] = 255;
            }
            break;
        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint16_t p = reinterpret_cast<const uint16_t*>(row)[x];
                const uint8_t a = (fmt == D3DFMT_A1R5G5B5) ? ((p & 0x8000) ? 255 : 0) : 255;
                const uint8_t r = static_cast<uint8_t>((p >> 10) & 0x1F);
                const uint8_t g = static_cast<uint8_t>((p >> 5) & 0x1F);
                const uint8_t b = static_cast<uint8_t>(p & 0x1F);
                drow[x * 4 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
                drow[x * 4 + 1] = static_cast<uint8_t>((g << 3) | (g >> 2));
                drow[x * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
                drow[x * 4 + 3] = a;
            }
            break;
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint16_t p = reinterpret_cast<const uint16_t*>(row)[x];
                const uint8_t a = (fmt == D3DFMT_A4R4G4B4) ? static_cast<uint8_t>(((p >> 12) & 0xF) * 17) : 255;
                const uint8_t r = static_cast<uint8_t>(((p >> 8) & 0xF) * 17);
                const uint8_t g = static_cast<uint8_t>(((p >> 4) & 0xF) * 17);
                const uint8_t b = static_cast<uint8_t>((p & 0xF) * 17);
                drow[x * 4 + 0] = r;
                drow[x * 4 + 1] = g;
                drow[x * 4 + 2] = b;
                drow[x * 4 + 3] = a;
            }
            break;
        case D3DFMT_L8:
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t l = row[x];
                drow[x * 4 + 0] = l;
                drow[x * 4 + 1] = l;
                drow[x * 4 + 2] = l;
                drow[x * 4 + 3] = 255;
            }
            break;
        case D3DFMT_A8:
            for (uint32_t x = 0; x < width; ++x)
            {
                drow[x * 4 + 0] = 255;
                drow[x * 4 + 1] = 255;
                drow[x * 4 + 2] = 255;
                drow[x * 4 + 3] = row[x];
            }
            break;
        case D3DFMT_A8L8:
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t l = row[x * 2 + 0];
                const uint8_t a = row[x * 2 + 1];
                drow[x * 4 + 0] = l;
                drow[x * 4 + 1] = l;
                drow[x * 4 + 2] = l;
                drow[x * 4 + 3] = a;
            }
            break;
        default:
            std::memset(drow, 255, static_cast<size_t>(width) * 4);
            break;
        }
    }
}

void MakeIdentityMatrix(D3DMATRIX& m)
{
    std::memset(&m, 0, sizeof(m));
    m._11 = 1.0f;
    m._22 = 1.0f;
    m._33 = 1.0f;
    m._44 = 1.0f;
}

uint32_t MapD3DCmpToShaderDepth(DWORD cmp)
{
    switch (cmp)
    {
    case D3DCMP_NEVER:        return 0;
    case D3DCMP_LESS:         return 1;
    case D3DCMP_EQUAL:        return 2;
    case D3DCMP_LESSEQUAL:    return 3;
    case D3DCMP_GREATER:      return 4;
    case D3DCMP_NOTEQUAL:     return 5;
    case D3DCMP_GREATEREQUAL: return 6;
    case D3DCMP_ALWAYS:       return 7;
    default:                  return 3;
    }
}

uint32_t MapD3DBlendToSrc(DWORD blend)
{
    switch (blend)
    {
    case D3DBLEND_ZERO:         return 0;
    case D3DBLEND_ONE:          return 1;
    case D3DBLEND_SRCALPHA:     return 2;
    case D3DBLEND_INVSRCALPHA:  return 3;
    case D3DBLEND_DESTCOLOR:    return 4;
    case D3DBLEND_INVDESTCOLOR: return 5;
    case D3DBLEND_SRCCOLOR:     return 6;
    case D3DBLEND_INVSRCCOLOR:  return 7;
    default:                    return 1;
    }
}

void TransformPointRowMajor(const D3DMATRIX& m, float x, float y, float z, float w, float out[4])
{
    out[0] = x * m._11 + y * m._21 + z * m._31 + w * m._41;
    out[1] = x * m._12 + y * m._22 + z * m._32 + w * m._42;
    out[2] = x * m._13 + y * m._23 + z * m._33 + w * m._43;
    out[3] = x * m._14 + y * m._24 + z * m._34 + w * m._44;
}

uint32_t MapD3DTexOp(DWORD op)
{
    switch (op)
    {
    case D3DTOP_DISABLE:              return 0;
    case D3DTOP_SELECTARG1:           return 1;
    case D3DTOP_SELECTARG2:           return 2;
    case D3DTOP_MODULATE:             return 3;
    case D3DTOP_MODULATE2X:           return 4;
    case D3DTOP_ADD:                  return 5;
    case D3DTOP_ADDSIGNED:            return 6;
    case D3DTOP_SUBTRACT:             return 7;
    case D3DTOP_BLENDTEXTUREALPHA:    return 8;
    case D3DTOP_BLENDCURRENTALPHA:    return 9;
    case D3DTOP_MULTIPLYADD:          return 10;
    case D3DTOP_DOTPRODUCT3:          return 11;
    case D3DTOP_MODULATEALPHA_ADDCOLOR: return 12;
    case D3DTOP_ADDSMOOTH:            return 13;
    case D3DTOP_BLENDDIFFUSEALPHA:    return 14;
    case D3DTOP_ADDSIGNED2X:          return 15;
    default:                          return 3; // fallback to MODULATE
    }
}

uint32_t EncodeD3DTexArg(DWORD arg)
{
    uint32_t sel = 0; // DIFFUSE
    switch (arg & D3DTA_SELECTMASK)
    {
    case D3DTA_DIFFUSE:  sel = 0; break;
    case D3DTA_CURRENT:  sel = 1; break;
    case D3DTA_TEXTURE:  sel = 2; break;
    case D3DTA_TFACTOR:  sel = 3; break;
    default:             sel = 1; break;
    }
    if (arg & D3DTA_COMPLEMENT) sel |= 0x10u;
    if (arg & D3DTA_ALPHAREPLICATE) sel |= 0x20u;
    return sel;
}

uint32_t PackD3DTexArgs(DWORD a1, DWORD a2, DWORD a0)
{
    return (EncodeD3DTexArg(a1) & 0xFFu) |
           ((EncodeD3DTexArg(a2) & 0xFFu) << 8) |
           ((EncodeD3DTexArg(a0) & 0xFFu) << 16);
}

uint32_t MapD3DBlendToDst(DWORD blend)
{
    switch (blend)
    {
    case D3DBLEND_ZERO:         return 0;
    case D3DBLEND_ONE:          return 1;
    case D3DBLEND_SRCCOLOR:     return 2;
    case D3DBLEND_INVSRCCOLOR:  return 3;
    case D3DBLEND_SRCALPHA:     return 4;
    case D3DBLEND_INVSRCALPHA:  return 5;
    default:                    return 0;
    }
}

// Forward declarations
class ZhrVkD3DDevice8;
class ZhrVkTexture8;

// ============================================================================
// ZhrVkSurface8 — CPU-backed IDirect3DSurface8 (standalone or texture mip)
// ============================================================================
class ZhrVkSurface8 final : public IDirect3DSurface8
{
public:
    ZhrVkSurface8(
        ZhrVkD3DDevice8* device,
        uint32_t width,
        uint32_t height,
        D3DFORMAT format,
        DWORD usage,
        D3DPOOL pool,
        ZhrVkTexture8* owner_texture = nullptr);

    ~ZhrVkSurface8();

    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override
    {
        if (ppvObj) *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override
    {
        const ULONG next = m_ref_count.fetch_sub(1) - 1;
        if (next == 0) delete this;
        return next;
    }

    STDMETHOD(GetDevice)(IDirect3DDevice8** ppDevice) override;
    STDMETHOD(SetPrivateData)(REFGUID, CONST void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD(GetContainer)(REFIID, void** ppContainer) override
    {
        if (ppContainer) *ppContainer = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD(GetDesc)(D3DSURFACE_DESC* pDesc) override
    {
        if (!pDesc) return D3DERR_INVALIDCALL;
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_SURFACE;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = static_cast<UINT>(m_data.size());
        pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
        pDesc->Width = m_width;
        pDesc->Height = m_height;
        return D3D_OK;
    }
    STDMETHOD(LockRect)(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override
    {
        if (!pLockedRect) return D3DERR_INVALIDCALL;
        pLockedRect->Pitch = static_cast<INT>(m_pitch);
        if (pRect != nullptr && !IsCompressedFormat(m_format))
        {
            const uint32_t bpp = CalcBytesPerPixel(m_format);
            const size_t offset = static_cast<size_t>(pRect->top) * m_pitch +
                                  static_cast<size_t>(pRect->left) * bpp;
            pLockedRect->pBits = m_data.data() + std::min(offset, m_data.size());
        }
        else
        {
            pLockedRect->pBits = m_data.data();
        }
        m_last_lock_readonly = (Flags & D3DLOCK_READONLY) != 0;
        return D3D_OK;
    }
    STDMETHOD(UnlockRect)() override;

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t Pitch() const { return m_pitch; }
    D3DFORMAT Format() const { return m_format; }
    const uint8_t* Data() const { return m_data.data(); }
    uint8_t* MutableData() { return m_data.data(); }
    size_t DataSize() const { return m_data.size(); }
    bool WasWritten() const { return m_written; }

    void DetachOwner() { m_owner_texture = nullptr; }

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    ZhrVkTexture8* m_owner_texture; // non-owning back-pointer; owner outlives or detaches
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pitch;
    D3DFORMAT m_format;
    DWORD m_usage;
    D3DPOOL m_pool;
    bool m_last_lock_readonly{false};
    bool m_written{false};
    std::vector<uint8_t> m_data;
};

// ============================================================================
// ZhrVkTexture8 — CPU-mip-backed IDirect3DTexture8 uploading to zhr-vulkan
// ============================================================================
class ZhrVkTexture8 final : public IDirect3DTexture8
{
public:
    ZhrVkTexture8(
        ZhrVkD3DDevice8* device,
        uint32_t width,
        uint32_t height,
        uint32_t levels,
        DWORD usage,
        D3DFORMAT format,
        D3DPOOL pool)
        : m_device(device)
        , m_width(width > 0 ? width : 1)
        , m_height(height > 0 ? height : 1)
        , m_format(format)
        , m_usage(usage)
        , m_pool(pool)
    {
        const uint32_t count = CalcMipCount(m_width, m_height, levels);
        m_surfaces.reserve(count);
        uint32_t w = m_width;
        uint32_t h = m_height;
        for (uint32_t i = 0; i < count; ++i)
        {
            m_surfaces.push_back(new ZhrVkSurface8(device, w, h, format, usage, pool, this));
            w = w > 1 ? (w >> 1) : 1;
            h = h > 1 ? (h >> 1) : 1;
        }
    }

    ~ZhrVkTexture8()
    {
        if (m_vk_handle != 0)
        {
            zhr_vk_destroy_texture(m_vk_handle);
            m_vk_handle = 0;
        }
        for (ZhrVkSurface8* s : m_surfaces)
        {
            s->DetachOwner();
            s->Release();
        }
        m_surfaces.clear();
    }

    void MarkDirty()
    {
        m_dirty = true;
        m_heightmap_built = false;
        m_heightmap.clear();
        m_height_variance = 0.0f;
    }

    bool EnsureHeightmap()
    {
        if (m_heightmap_built)
        {
            return !m_heightmap.empty();
        }
        m_heightmap_built = true;
        if (m_surfaces.empty() || m_width < 32 || m_height < 32)
        {
            return false;
        }
        ZhrVkSurface8* base = m_surfaces[0];
        if (!base || !base->WasWritten() || base->DataSize() == 0)
        {
            return false;
        }

        constexpr uint32_t kDim = 64u;
        std::vector<uint8_t> hm(kDim * kDim, 128u);
        int32_t ok = 0;

        if (m_format == D3DFMT_DXT1)
        {
            ok = zhr_remaster_build_heightmap(m_width, m_height, 2u, base->Data(), base->DataSize(), kDim, hm.data());
        }
        else if (m_format == D3DFMT_DXT2 || m_format == D3DFMT_DXT3)
        {
            ok = zhr_remaster_build_heightmap(m_width, m_height, 3u, base->Data(), base->DataSize(), kDim, hm.data());
        }
        else if (m_format == D3DFMT_DXT4 || m_format == D3DFMT_DXT5)
        {
            ok = zhr_remaster_build_heightmap(m_width, m_height, 4u, base->Data(), base->DataSize(), kDim, hm.data());
        }
        else if (m_format == D3DFMT_A8R8G8B8 && base->Pitch() == m_width * 4)
        {
            ok = zhr_remaster_build_heightmap(m_width, m_height, 1u, base->Data(), base->DataSize(), kDim, hm.data());
        }
        else
        {
            std::vector<uint8_t> rgba;
            ConvertToRgba8(m_format, m_width, m_height, base->Data(), base->Pitch(), rgba);
            ok = zhr_remaster_build_heightmap(m_width, m_height, 0u, rgba.data(), rgba.size(), kDim, hm.data());
        }

        if (ok == 0)
        {
            return false;
        }

        float sum_abs = 0.0f;
        for (uint8_t v : hm)
        {
            sum_abs += std::fabs((static_cast<float>(v) * (1.0f / 255.0f)) - 0.5f);
        }
        m_height_variance = sum_abs / static_cast<float>(hm.size());
        m_heightmap_dim = kDim;
        m_heightmap = std::move(hm);
        return true;
    }

    float HeightVariance() const { return m_height_variance; }

    // Fast inline bilinear height lookup centered at 0.0 in [-1.0, +1.0].
    float SampleHeight(float u, float v) const
    {
        if (m_heightmap.empty() || m_heightmap_dim == 0)
        {
            return 0.0f;
        }
        const float fu = u - std::floor(u);
        const float fv = v - std::floor(v);
        const uint32_t dim = m_heightmap_dim;
        const float fx = fu * static_cast<float>(dim);
        const float fy = fv * static_cast<float>(dim);
        const uint32_t x0 = static_cast<uint32_t>(fx) % dim;
        const uint32_t y0 = static_cast<uint32_t>(fy) % dim;
        const uint32_t x1 = (x0 + 1u) % dim;
        const uint32_t y1 = (y0 + 1u) % dim;
        const float tx = fx - std::floor(fx);
        const float ty = fy - std::floor(fy);

        const float h00 = static_cast<float>(m_heightmap[y0 * dim + x0]);
        const float h10 = static_cast<float>(m_heightmap[y0 * dim + x1]);
        const float h01 = static_cast<float>(m_heightmap[y1 * dim + x0]);
        const float h11 = static_cast<float>(m_heightmap[y1 * dim + x1]);
        const float top = h00 + (h10 - h00) * tx;
        const float bot = h01 + (h11 - h01) * tx;
        const float h = (top + (bot - top) * ty) * (1.0f / 255.0f);
        return (h - 0.5f) * 2.0f;
    }

    uint32_t EnsureUploaded(bool linear_filter, bool clamp_edge)
    {
        if (m_surfaces.empty())
        {
            return 0;
        }
        if (!m_dirty && m_vk_handle != 0 &&
            m_uploaded_linear == linear_filter &&
            m_uploaded_clamp == clamp_edge)
        {
            return m_vk_handle;
        }

        if (m_vk_handle != 0)
        {
            zhr_vk_destroy_texture(m_vk_handle);
            m_vk_handle = 0;
        }

        ZhrVkSurface8* base = m_surfaces[0];
        const uint32_t w = base->Width();
        const uint32_t h = base->Height();

        auto upload_compressed = [&](uint32_t vk_fmt) -> uint32_t {
            // Collect all contiguous mip levels that were actually populated by the DDS/asset loader.
            uint32_t mip_count = 1;
            size_t total_bytes = base->DataSize();
            for (size_t lvl = 1; lvl < m_surfaces.size(); ++lvl)
            {
                if (!m_surfaces[lvl]->WasWritten())
                {
                    break;
                }
                total_bytes += m_surfaces[lvl]->DataSize();
                ++mip_count;
            }
            if (mip_count == 1)
            {
                return zhr_vk_create_texture_2d(
                    w, h, vk_fmt, base->Data(), base->DataSize(), linear_filter, clamp_edge);
            }
            std::vector<uint8_t> packed;
            packed.reserve(total_bytes);
            for (uint32_t lvl = 0; lvl < mip_count; ++lvl)
            {
                const uint8_t* p = m_surfaces[lvl]->Data();
                packed.insert(packed.end(), p, p + m_surfaces[lvl]->DataSize());
            }
            return zhr_vk_create_texture_2d_mips(
                w, h, mip_count, vk_fmt, packed.data(), packed.size(), linear_filter, clamp_edge);
        };

        if (m_format == D3DFMT_DXT1)
        {
            m_vk_handle = upload_compressed(2);
        }
        else if (m_format == D3DFMT_DXT2 || m_format == D3DFMT_DXT3)
        {
            m_vk_handle = upload_compressed(3);
        }
        else if (m_format == D3DFMT_DXT4 || m_format == D3DFMT_DXT5)
        {
            m_vk_handle = upload_compressed(4);
        }
        else if (m_format == D3DFMT_A8R8G8B8 && base->Pitch() == w * 4)
        {
            m_vk_handle = zhr_vk_create_texture_2d(
                w, h, 1, base->Data(), base->DataSize(), linear_filter, clamp_edge);
        }
        else
        {
            std::vector<uint8_t> rgba;
            ConvertToRgba8(m_format, w, h, base->Data(), base->Pitch(), rgba);
            m_vk_handle = zhr_vk_create_texture_2d(
                w, h, 0, rgba.data(), rgba.size(), linear_filter, clamp_edge);
        }

        m_dirty = false;
        m_uploaded_linear = linear_filter;
        m_uploaded_clamp = clamp_edge;
        return m_vk_handle;
    }

    ZhrVkSurface8* PeekSurface(uint32_t level) const
    {
        return level < m_surfaces.size() ? m_surfaces[level] : nullptr;
    }

    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override
    {
        if (ppvObj) *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override
    {
        const ULONG next = m_ref_count.fetch_sub(1) - 1;
        if (next == 0) delete this;
        return next;
    }

    STDMETHOD(GetDevice)(IDirect3DDevice8** ppDevice) override;
    STDMETHOD(SetPrivateData)(REFGUID, CONST void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD PriorityNew) override
    {
        const DWORD prev = m_priority;
        m_priority = PriorityNew;
        return prev;
    }
    STDMETHOD_(DWORD, GetPriority)() override { return m_priority; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_TEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD LODNew) override
    {
        const DWORD prev = m_lod;
        m_lod = LODNew;
        return prev;
    }
    STDMETHOD_(DWORD, GetLOD)() override { return m_lod; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return static_cast<DWORD>(m_surfaces.size()); }
    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) override
    {
        if (Level >= m_surfaces.size() || !pDesc) return D3DERR_INVALIDCALL;
        return m_surfaces[Level]->GetDesc(pDesc);
    }
    STDMETHOD(GetSurfaceLevel)(UINT Level, IDirect3DSurface8** ppSurfaceLevel) override
    {
        if (Level >= m_surfaces.size() || !ppSurfaceLevel) return D3DERR_INVALIDCALL;
        m_surfaces[Level]->AddRef();
        *ppSurfaceLevel = m_surfaces[Level];
        return D3D_OK;
    }
    STDMETHOD(LockRect)(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override
    {
        if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
        return m_surfaces[Level]->LockRect(pLockedRect, pRect, Flags);
    }
    STDMETHOD(UnlockRect)(UINT Level) override
    {
        if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
        return m_surfaces[Level]->UnlockRect();
    }
    STDMETHOD(AddDirtyRect)(CONST RECT*) override
    {
        m_dirty = true;
        return D3D_OK;
    }

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    uint32_t m_width;
    uint32_t m_height;
    D3DFORMAT m_format;
    DWORD m_usage;
    D3DPOOL m_pool;
    DWORD m_priority{0};
    DWORD m_lod{0};
    std::vector<ZhrVkSurface8*> m_surfaces;
    uint32_t m_vk_handle{0};
    bool m_dirty{true};
    bool m_uploaded_linear{true};
    bool m_uploaded_clamp{false};
    bool m_heightmap_built{false};
    uint32_t m_heightmap_dim{0};
    float m_height_variance{0.0f};
    std::vector<uint8_t> m_heightmap;
};

ZhrVkSurface8::ZhrVkSurface8(
    ZhrVkD3DDevice8* device,
    uint32_t width,
    uint32_t height,
    D3DFORMAT format,
    DWORD usage,
    D3DPOOL pool,
    ZhrVkTexture8* owner_texture)
    : m_device(device)
    , m_owner_texture(owner_texture)
    , m_width(width > 0 ? width : 1)
    , m_height(height > 0 ? height : 1)
    , m_pitch(CalcSurfacePitch(format, m_width))
    , m_format(format)
    , m_usage(usage)
    , m_pool(pool)
    , m_data(CalcSurfaceByteSize(format, m_width, m_height), 0)
{
}

ZhrVkSurface8::~ZhrVkSurface8() = default;

HRESULT STDMETHODCALLTYPE ZhrVkSurface8::UnlockRect()
{
    if (!m_last_lock_readonly)
    {
        m_written = true;
        if (m_owner_texture != nullptr)
        {
            m_owner_texture->MarkDirty();
        }
    }
    return D3D_OK;
}

// ============================================================================
// ZhrVkCubeTexture8 & ZhrVkVolumeTexture8 — Minimal CPU-backed implementations
// ============================================================================
class ZhrVkCubeTexture8 final : public IDirect3DCubeTexture8
{
public:
    ZhrVkCubeTexture8(ZhrVkD3DDevice8* device, uint32_t edge, uint32_t levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : m_device(device)
        , m_edge(edge > 0 ? edge : 1)
        , m_levels(CalcMipCount(m_edge, m_edge, levels))
    {
        for (int face = 0; face < 6; ++face)
        {
            uint32_t s = m_edge;
            for (uint32_t l = 0; l < m_levels; ++l)
            {
                m_faces[face].push_back(new ZhrVkSurface8(device, s, s, fmt, usage, pool));
                s = s > 1 ? (s >> 1) : 1;
            }
        }
    }
    ~ZhrVkCubeTexture8()
    {
        for (int face = 0; face < 6; ++face)
        {
            for (ZhrVkSurface8* s : m_faces[face]) s->Release();
        }
    }

    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override { if (ppvObj) *ppvObj = nullptr; return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override { ULONG n = m_ref_count.fetch_sub(1) - 1; if (n == 0) delete this; return n; }
    STDMETHOD(GetDevice)(IDirect3DDevice8** ppDevice) override;
    STDMETHOD(SetPrivateData)(REFGUID, CONST void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override { DWORD prev = m_priority; m_priority = p; return prev; }
    STDMETHOD_(DWORD, GetPriority)() override { return m_priority; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_CUBETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD l) override { DWORD prev = m_lod; m_lod = l; return prev; }
    STDMETHOD_(DWORD, GetLOD)() override { return m_lod; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return m_levels; }
    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC* pDesc) override
    {
        if (Level >= m_levels) return D3DERR_INVALIDCALL;
        return m_faces[0][Level]->GetDesc(pDesc);
    }
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface8** ppCubeMapSurface) override
    {
        const int f = static_cast<int>(FaceType);
        if (f < 0 || f >= 6 || Level >= m_levels || !ppCubeMapSurface) return D3DERR_INVALIDCALL;
        m_faces[f][Level]->AddRef();
        *ppCubeMapSurface = m_faces[f][Level];
        return D3D_OK;
    }
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override
    {
        const int f = static_cast<int>(FaceType);
        if (f < 0 || f >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
        return m_faces[f][Level]->LockRect(pLockedRect, pRect, Flags);
    }
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES FaceType, UINT Level) override
    {
        const int f = static_cast<int>(FaceType);
        if (f < 0 || f >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
        return m_faces[f][Level]->UnlockRect();
    }
    STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, CONST RECT*) override { return D3D_OK; }

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    uint32_t m_edge;
    uint32_t m_levels;
    DWORD m_priority{0};
    DWORD m_lod{0};
    std::vector<ZhrVkSurface8*> m_faces[6];
};

class ZhrVkVolume8 final : public IDirect3DVolume8
{
public:
    ZhrVkVolume8(ZhrVkD3DDevice8* device, uint32_t w, uint32_t h, uint32_t d, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : m_device(device), m_w(w > 0 ? w : 1), m_h(h > 0 ? h : 1), m_d(d > 0 ? d : 1)
        , m_usage(usage), m_fmt(fmt), m_pool(pool)
        , m_row_pitch(CalcSurfacePitch(fmt, m_w))
        , m_slice_pitch(CalcSurfaceByteSize(fmt, m_w, m_h))
        , m_data(static_cast<size_t>(m_slice_pitch) * m_d, 0)
    {}
    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override { if (ppvObj) *ppvObj = nullptr; return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override { ULONG n = m_ref_count.fetch_sub(1) - 1; if (n == 0) delete this; return n; }
    STDMETHOD(GetDevice)(IDirect3DDevice8** ppDevice) override;
    STDMETHOD(SetPrivateData)(REFGUID, CONST void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD(GetContainer)(REFIID, void** ppContainer) override { if (ppContainer) *ppContainer = nullptr; return E_NOINTERFACE; }
    STDMETHOD(GetDesc)(D3DVOLUME_DESC* pDesc) override
    {
        if (!pDesc) return D3DERR_INVALIDCALL;
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc->Format = m_fmt;
        pDesc->Type = D3DRTYPE_VOLUME;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = static_cast<UINT>(m_data.size());
        pDesc->Width = m_w;
        pDesc->Height = m_h;
        pDesc->Depth = m_d;
        return D3D_OK;
    }
    STDMETHOD(LockBox)(D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX*, DWORD) override
    {
        if (!pLockedVolume) return D3DERR_INVALIDCALL;
        pLockedVolume->RowPitch = static_cast<INT>(m_row_pitch);
        pLockedVolume->SlicePitch = static_cast<INT>(m_slice_pitch);
        pLockedVolume->pBits = m_data.data();
        return D3D_OK;
    }
    STDMETHOD(UnlockBox)() override { return D3D_OK; }

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    uint32_t m_w, m_h, m_d;
    DWORD m_usage;
    D3DFORMAT m_fmt;
    D3DPOOL m_pool;
    uint32_t m_row_pitch;
    uint32_t m_slice_pitch;
    std::vector<uint8_t> m_data;
};

class ZhrVkVolumeTexture8 final : public IDirect3DVolumeTexture8
{
public:
    ZhrVkVolumeTexture8(ZhrVkD3DDevice8* device, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : m_device(device)
    {
        const uint32_t count = CalcMipCount(w, h, levels);
        uint32_t cw = w > 0 ? w : 1;
        uint32_t ch = h > 0 ? h : 1;
        uint32_t cd = d > 0 ? d : 1;
        for (uint32_t i = 0; i < count; ++i)
        {
            m_levels.push_back(new ZhrVkVolume8(device, cw, ch, cd, usage, fmt, pool));
            cw = cw > 1 ? (cw >> 1) : 1;
            ch = ch > 1 ? (ch >> 1) : 1;
            cd = cd > 1 ? (cd >> 1) : 1;
        }
    }
    ~ZhrVkVolumeTexture8() { for (ZhrVkVolume8* v : m_levels) v->Release(); }
    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override { if (ppvObj) *ppvObj = nullptr; return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override { ULONG n = m_ref_count.fetch_sub(1) - 1; if (n == 0) delete this; return n; }
    STDMETHOD(GetDevice)(IDirect3DDevice8** ppDevice) override;
    STDMETHOD(SetPrivateData)(REFGUID, CONST void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override { DWORD prev = m_priority; m_priority = p; return prev; }
    STDMETHOD_(DWORD, GetPriority)() override { return m_priority; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VOLUMETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD l) override { DWORD prev = m_lod; m_lod = l; return prev; }
    STDMETHOD_(DWORD, GetLOD)() override { return m_lod; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return static_cast<DWORD>(m_levels.size()); }
    STDMETHOD(GetLevelDesc)(UINT Level, D3DVOLUME_DESC* pDesc) override
    {
        if (Level >= m_levels.size()) return D3DERR_INVALIDCALL;
        return m_levels[Level]->GetDesc(pDesc);
    }
    STDMETHOD(GetVolumeLevel)(UINT Level, IDirect3DVolume8** ppVolumeLevel) override
    {
        if (Level >= m_levels.size() || !ppVolumeLevel) return D3DERR_INVALIDCALL;
        m_levels[Level]->AddRef();
        *ppVolumeLevel = m_levels[Level];
        return D3D_OK;
    }
    STDMETHOD(LockBox)(UINT Level, D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX* pBox, DWORD Flags) override
    {
        if (Level >= m_levels.size()) return D3DERR_INVALIDCALL;
        return m_levels[Level]->LockBox(pLockedVolume, pBox, Flags);
    }
    STDMETHOD(UnlockBox)(UINT Level) override
    {
        if (Level >= m_levels.size()) return D3DERR_INVALIDCALL;
        return m_levels[Level]->UnlockBox();
    }
    STDMETHOD(AddDirtyBox)(CONST D3DBOX*) override { return D3D_OK; }

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    DWORD m_priority{0};
    DWORD m_lod{0};
    std::vector<ZhrVkVolume8*> m_levels;
};

// ============================================================================
// ZhrVkVertexBuffer8 & ZhrVkIndexBuffer8
// ============================================================================
class ZhrVkVertexBuffer8 final : public IDirect3DVertexBuffer8
{
public:
    ZhrVkVertexBuffer8(ZhrVkD3DDevice8* device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
        : m_device(device)
        , m_usage(usage)
        , m_fvf(fvf)
        , m_pool(pool)
        , m_data(length > 0 ? length : 16, 0)
    {}

    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override { if (ppvObj) *ppvObj = nullptr; return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override { ULONG n = m_ref_count.fetch_sub(1) - 1; if (n == 0) delete this; return n; }
    STDMETHOD(GetDevice)(IDirect3DDevice8** ppDevice) override;
    STDMETHOD(SetPrivateData)(REFGUID, CONST void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override { DWORD prev = m_priority; m_priority = p; return prev; }
    STDMETHOD_(DWORD, GetPriority)() override { return m_priority; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VERTEXBUFFER; }
    STDMETHOD(Lock)(UINT OffsetToLock, UINT /*SizeToLock*/, BYTE** ppbData, DWORD Flags) override
    {
        if (!ppbData || OffsetToLock > m_data.size()) return D3DERR_INVALIDCALL;
        if ((Flags & D3DLOCK_READONLY) == 0)
        {
            ++m_generation;
            m_last_mutated_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
        }
        *ppbData = m_data.data() + OffsetToLock;
        return D3D_OK;
    }
    STDMETHOD(Unlock)() override { return D3D_OK; }
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC* pDesc) override
    {
        if (!pDesc) return D3DERR_INVALIDCALL;
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc->Format = D3DFMT_VERTEXDATA;
        pDesc->Type = D3DRTYPE_VERTEXBUFFER;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = static_cast<UINT>(m_data.size());
        pDesc->FVF = m_fvf;
        return D3D_OK;
    }

    const uint8_t* Data() const { return m_data.data(); }
    size_t Size() const { return m_data.size(); }
    DWORD FVF() const { return m_fvf; }
    DWORD Usage() const { return m_usage; }
    bool IsDynamic() const { return (m_usage & D3DUSAGE_DYNAMIC) != 0; }
    uint64_t Id() const { return m_id; }
    uint64_t Generation() const { return m_generation; }
    uint64_t LastMutatedFrame() const { return m_last_mutated_frame; }

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    DWORD m_usage;
    DWORD m_fvf;
    D3DPOOL m_pool;
    DWORD m_priority{0};
    uint64_t m_id{g_zhr_next_buf_id.fetch_add(1, std::memory_order_relaxed)};
    uint64_t m_generation{1};
    uint64_t m_last_mutated_frame{0};
    std::vector<uint8_t> m_data;
};

class ZhrVkIndexBuffer8 final : public IDirect3DIndexBuffer8
{
public:
    ZhrVkIndexBuffer8(ZhrVkD3DDevice8* device, UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
        : m_device(device)
        , m_usage(usage)
        , m_format(fmt)
        , m_pool(pool)
        , m_data(length > 0 ? length : 16, 0)
    {}

    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override { if (ppvObj) *ppvObj = nullptr; return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override { ULONG n = m_ref_count.fetch_sub(1) - 1; if (n == 0) delete this; return n; }
    STDMETHOD(GetDevice)(IDirect3DDevice8** ppDevice) override;
    STDMETHOD(SetPrivateData)(REFGUID, CONST void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override { DWORD prev = m_priority; m_priority = p; return prev; }
    STDMETHOD_(DWORD, GetPriority)() override { return m_priority; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_INDEXBUFFER; }
    STDMETHOD(Lock)(UINT OffsetToLock, UINT /*SizeToLock*/, BYTE** ppbData, DWORD Flags) override
    {
        if (!ppbData || OffsetToLock > m_data.size()) return D3DERR_INVALIDCALL;
        if ((Flags & D3DLOCK_READONLY) == 0)
        {
            ++m_generation;
            m_last_mutated_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
        }
        *ppbData = m_data.data() + OffsetToLock;
        return D3D_OK;
    }
    STDMETHOD(Unlock)() override { return D3D_OK; }
    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC* pDesc) override
    {
        if (!pDesc) return D3DERR_INVALIDCALL;
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_INDEXBUFFER;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = static_cast<UINT>(m_data.size());
        return D3D_OK;
    }

    const uint16_t* Data16() const { return reinterpret_cast<const uint16_t*>(m_data.data()); }
    size_t Count16() const { return m_data.size() / sizeof(uint16_t); }
    DWORD Usage() const { return m_usage; }
    bool IsDynamic() const { return (m_usage & D3DUSAGE_DYNAMIC) != 0; }
    uint64_t Id() const { return m_id; }
    uint64_t Generation() const { return m_generation; }
    uint64_t LastMutatedFrame() const { return m_last_mutated_frame; }

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    DWORD m_usage;
    D3DFORMAT m_format;
    D3DPOOL m_pool;
    DWORD m_priority{0};
    uint64_t m_id{g_zhr_next_buf_id.fetch_add(1, std::memory_order_relaxed)};
    uint64_t m_generation{1};
    uint64_t m_last_mutated_frame{0};
    std::vector<uint8_t> m_data;
};

struct StaticDrawKey
{
    uint64_t vb_id{0};
    uint64_t vb_gen{0};
    uint64_t ib_id{0};
    uint64_t ib_gen{0};
    uint32_t base_vertex{0};
    uint32_t start_index{0};
    uint32_t prim_count{0};
    uint32_t prim_type{0};
    uint32_t fvf{0};
    uint32_t stride{0};
    uint32_t mat_rgba{0};
    uint32_t tess_mode{0};
    uint64_t extra_key{0};

    bool operator==(const StaticDrawKey& o) const noexcept
    {
        return vb_id == o.vb_id && vb_gen == o.vb_gen &&
               ib_id == o.ib_id && ib_gen == o.ib_gen &&
               base_vertex == o.base_vertex && start_index == o.start_index &&
               prim_count == o.prim_count && prim_type == o.prim_type &&
               fvf == o.fvf && stride == o.stride &&
               mat_rgba == o.mat_rgba && tess_mode == o.tess_mode &&
               extra_key == o.extra_key;
    }
};

struct StaticDrawKeyHash
{
    size_t operator()(const StaticDrawKey& k) const noexcept
    {
        uint64_t h = 14695981039346656037ULL;
        auto mix = [&](uint64_t v) {
            h = (h ^ v) * 1099511628211ULL;
        };
        mix(k.vb_id);
        mix(k.vb_gen);
        mix(k.ib_id);
        mix(k.ib_gen);
        mix((static_cast<uint64_t>(k.base_vertex) << 32) | k.start_index);
        mix((static_cast<uint64_t>(k.prim_count) << 32) | k.prim_type);
        mix((static_cast<uint64_t>(k.fvf) << 32) | k.stride);
        mix((static_cast<uint64_t>(k.mat_rgba) << 32) | k.tess_mode);
        mix(k.extra_key);
        return static_cast<size_t>(h);
    }
};

struct CachedGpuMesh
{
    uint32_t gpu_vb{0};
    uint32_t gpu_ib{0};
    uint32_t index_count{0};
    uint64_t last_used_frame{0};
};

// ============================================================================
// ZhrVkD3DDevice8 — Full IDirect3DDevice8 backed directly by zhr-vulkan
// ============================================================================
class ZhrVkD3DDevice8 final : public IDirect3DDevice8
{
public:
    ZhrVkD3DDevice8(
        IDirect3D8* parent_d3d,
        HWND hwnd,
        const D3DPRESENT_PARAMETERS& pp,
        const D3DCAPS8& caps)
        : m_parent_d3d(parent_d3d)
        , m_hwnd(hwnd)
        , m_pp(pp)
        , m_caps(caps)
    {
        if (m_parent_d3d) m_parent_d3d->AddRef();

        m_width = pp.BackBufferWidth > 0 ? pp.BackBufferWidth : 1280;
        m_height = pp.BackBufferHeight > 0 ? pp.BackBufferHeight : 720;

        MakeIdentityMatrix(m_world);
        MakeIdentityMatrix(m_view);
        MakeIdentityMatrix(m_proj);
        for (int i = 0; i < 4; ++i)
        {
            MakeIdentityMatrix(m_tex_matrix[i]);
        }

        m_viewport.X = 0;
        m_viewport.Y = 0;
        m_viewport.Width = m_width;
        m_viewport.Height = m_height;
        m_viewport.MinZ = 0.0f;
        m_viewport.MaxZ = 1.0f;

        InitDefaultStates();

        m_back_buffer = new ZhrVkSurface8(this, m_width, m_height, D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
        const D3DFORMAT depth_fmt = (pp.AutoDepthStencilFormat != D3DFMT_UNKNOWN)
            ? pp.AutoDepthStencilFormat
            : D3DFMT_D24X8;
        m_depth_surface = new ZhrVkSurface8(this, m_width, m_height, depth_fmt, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
        m_current_rt = m_back_buffer;
        m_current_rt->AddRef();
        m_current_depth = m_depth_surface;
        m_current_depth->AddRef();

        std::memset(m_textures, 0, sizeof(m_textures));
        std::memset(m_streams, 0, sizeof(m_streams));
        std::memset(m_strides, 0, sizeof(m_strides));
        std::memset(m_lights, 0, sizeof(m_lights));
        std::memset(m_light_enabled, 0, sizeof(m_light_enabled));
        std::memset(&m_material, 0, sizeof(m_material));
        m_material.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
        m_material.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};

        if (const char* plog = std::getenv("ZHR_PERF_LOG"); plog != nullptr && plog[0] != '\0')
        {
            std::snprintf(m_perf_log_path, sizeof(m_perf_log_path), "%s", plog);
            m_perf_enabled = true;
        }
    }

    ~ZhrVkD3DDevice8()
    {
        if (zhr_vk_is_frame_active())
        {
            zhr_vk_end_frame();
        }
        ClearStaticGpuCache();
        for (auto*& tex : m_textures)
        {
            if (tex) { tex->Release(); tex = nullptr; }
        }
        for (auto*& vb : m_streams)
        {
            if (vb) { vb->Release(); vb = nullptr; }
        }
        if (m_index_buffer) { m_index_buffer->Release(); m_index_buffer = nullptr; }
        if (m_current_rt) { m_current_rt->Release(); m_current_rt = nullptr; }
        if (m_current_depth) { m_current_depth->Release(); m_current_depth = nullptr; }
        if (m_back_buffer) { m_back_buffer->Release(); m_back_buffer = nullptr; }
        if (m_depth_surface) { m_depth_surface->Release(); m_depth_surface = nullptr; }

        zhr_vk_shutdown();

        if (m_parent_d3d) { m_parent_d3d->Release(); m_parent_d3d = nullptr; }
    }

    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override { if (ppvObj) *ppvObj = nullptr; return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override { ULONG n = m_ref_count.fetch_sub(1) - 1; if (n == 0) delete this; return n; }

    STDMETHOD(TestCooperativeLevel)() override { return D3D_OK; }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override { return 512u * 1024u * 1024u; }
    STDMETHOD(ResourceManagerDiscardBytes)(DWORD) override { return D3D_OK; }
    STDMETHOD(GetDirect3D)(IDirect3D8** ppD3D8) override
    {
        if (!ppD3D8) return D3DERR_INVALIDCALL;
        if (m_parent_d3d) m_parent_d3d->AddRef();
        *ppD3D8 = m_parent_d3d;
        return D3D_OK;
    }
    STDMETHOD(GetDeviceCaps)(D3DCAPS8* pCaps) override
    {
        if (!pCaps) return D3DERR_INVALIDCALL;
        *pCaps = m_caps;
        return D3D_OK;
    }
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE* pMode) override
    {
        if (!pMode) return D3DERR_INVALIDCALL;
        pMode->Width = m_width;
        pMode->Height = m_height;
        pMode->RefreshRate = 60;
        pMode->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS* pParameters) override
    {
        if (!pParameters) return D3DERR_INVALIDCALL;
        pParameters->AdapterOrdinal = 0;
        pParameters->DeviceType = D3DDEVTYPE_HAL;
        pParameters->hFocusWindow = m_hwnd;
        pParameters->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
        return D3D_OK;
    }
    STDMETHOD(SetCursorProperties)(UINT, UINT, IDirect3DSurface8*) override { return D3D_OK; }
    STDMETHOD_(void, SetCursorPosition)(UINT, UINT, DWORD) override {}
    STDMETHOD_(BOOL, ShowCursor)(BOOL) override { return TRUE; }
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain8** ppSwapChain) override
    {
        if (ppSwapChain) *ppSwapChain = nullptr;
        return E_NOTIMPL;
    }
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters) override
    {
        if (!pPresentationParameters) return D3DERR_INVALIDCALL;
        if (zhr_vk_is_frame_active())
        {
            zhr_vk_end_frame();
        }
        m_pp = *pPresentationParameters;
        m_width = m_pp.BackBufferWidth > 0 ? m_pp.BackBufferWidth : m_width;
        m_height = m_pp.BackBufferHeight > 0 ? m_pp.BackBufferHeight : m_height;
        zhr_vk_resize(m_width, m_height);
        m_viewport = {0, 0, m_width, m_height, 0.0f, 1.0f};
        zhr_vk_set_viewport(0, 0, m_width, m_height, 0.0f, 1.0f);
        return D3D_OK;
    }
    STDMETHOD(Present)(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) override
    {
        PollGraphicsHotkeys();
        const auto t_end0 = m_perf_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (zhr_vk_is_frame_active())
        {
            zhr_vk_end_frame();
        }
        if (m_perf_enabled)
        {
            const auto now = std::chrono::steady_clock::now();
            m_perf_present_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - t_end0).count());
            if (m_perf_has_prev_frame)
            {
                m_perf_frame_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_perf_prev_frame).count());
                ++m_perf_frames;
                if (m_perf_frames % 30u == 0u)
                {
                    FlushPerfLog();
                }
            }
            m_perf_prev_frame = now;
            m_perf_has_prev_frame = true;
        }
        g_zhr_frame_counter.fetch_add(1, std::memory_order_relaxed);
        return D3D_OK;
    }
    STDMETHOD(GetBackBuffer)(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface8** ppBackBuffer) override
    {
        if (!ppBackBuffer) return D3DERR_INVALIDCALL;
        m_back_buffer->AddRef();
        *ppBackBuffer = m_back_buffer;
        return D3D_OK;
    }
    STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS* pRasterStatus) override
    {
        if (!pRasterStatus) return D3DERR_INVALIDCALL;
        pRasterStatus->InVBlank = FALSE;
        pRasterStatus->ScanLine = 0;
        return D3D_OK;
    }
    STDMETHOD_(void, SetGammaRamp)(DWORD, CONST D3DGAMMARAMP*) override {}
    STDMETHOD_(void, GetGammaRamp)(D3DGAMMARAMP* pRamp) override
    {
        if (pRamp) std::memset(pRamp, 0, sizeof(*pRamp));
    }

    STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8** ppTexture) override
    {
        if (!ppTexture) return D3DERR_INVALIDCALL;
        *ppTexture = new ZhrVkTexture8(this, Width, Height, Levels, Usage, Format, Pool);
        return D3D_OK;
    }
    STDMETHOD(CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8** ppVolumeTexture) override
    {
        if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
        *ppVolumeTexture = new ZhrVkVolumeTexture8(this, Width, Height, Depth, Levels, Usage, Format, Pool);
        return D3D_OK;
    }
    STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8** ppCubeTexture) override
    {
        if (!ppCubeTexture) return D3DERR_INVALIDCALL;
        *ppCubeTexture = new ZhrVkCubeTexture8(this, EdgeLength, Levels, Usage, Format, Pool);
        return D3D_OK;
    }
    STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8** ppVertexBuffer) override
    {
        if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
        *ppVertexBuffer = new ZhrVkVertexBuffer8(this, Length, Usage, FVF, Pool);
        return D3D_OK;
    }
    STDMETHOD(CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8** ppIndexBuffer) override
    {
        if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
        *ppIndexBuffer = new ZhrVkIndexBuffer8(this, Length, Usage, Format, Pool);
        return D3D_OK;
    }
    STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE, BOOL, IDirect3DSurface8** ppSurface) override
    {
        if (!ppSurface) return D3DERR_INVALIDCALL;
        *ppSurface = new ZhrVkSurface8(this, Width, Height, Format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
        return D3D_OK;
    }
    STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE, IDirect3DSurface8** ppSurface) override
    {
        if (!ppSurface) return D3DERR_INVALIDCALL;
        *ppSurface = new ZhrVkSurface8(this, Width, Height, Format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
        return D3D_OK;
    }
    STDMETHOD(CreateImageSurface)(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8** ppSurface) override
    {
        if (!ppSurface) return D3DERR_INVALIDCALL;
        *ppSurface = new ZhrVkSurface8(this, Width, Height, Format, 0, D3DPOOL_SYSTEMMEM);
        return D3D_OK;
    }

    STDMETHOD(CopyRects)(
        IDirect3DSurface8* pSourceSurface,
        CONST RECT* pSourceRectsArray,
        UINT cRects,
        IDirect3DSurface8* pDestinationSurface,
        CONST POINT* pDestPointsArray) override
    {
        if (!pSourceSurface || !pDestinationSurface) return D3DERR_INVALIDCALL;
        D3DSURFACE_DESC sdesc{}, ddesc{};
        pSourceSurface->GetDesc(&sdesc);
        pDestinationSurface->GetDesc(&ddesc);

        D3DLOCKED_RECT slock{}, dlock{};
        if (FAILED(pSourceSurface->LockRect(&slock, nullptr, D3DLOCK_READONLY))) return D3DERR_INVALIDCALL;
        if (FAILED(pDestinationSurface->LockRect(&dlock, nullptr, 0)))
        {
            pSourceSurface->UnlockRect();
            return D3DERR_INVALIDCALL;
        }

        if (sdesc.Format == ddesc.Format && IsCompressedFormat(sdesc.Format))
        {
            const size_t bytes = std::min<size_t>(sdesc.Size, ddesc.Size);
            std::memcpy(dlock.pBits, slock.pBits, bytes);
        }
        else if (sdesc.Format == ddesc.Format)
        {
            const uint32_t bpp = CalcBytesPerPixel(sdesc.Format);
            const UINT count = (pSourceRectsArray && cRects > 0) ? cRects : 1;
            for (UINT i = 0; i < count; ++i)
            {
                RECT sr = pSourceRectsArray
                    ? pSourceRectsArray[i]
                    : RECT{0, 0, static_cast<LONG>(sdesc.Width), static_cast<LONG>(sdesc.Height)};
                POINT dp = pDestPointsArray ? pDestPointsArray[i] : POINT{sr.left, sr.top};
                const LONG rw = std::min<LONG>(sr.right - sr.left, static_cast<LONG>(ddesc.Width) - dp.x);
                const LONG rh = std::min<LONG>(sr.bottom - sr.top, static_cast<LONG>(ddesc.Height) - dp.y);
                for (LONG y = 0; y < rh; ++y)
                {
                    if (sr.top + y < 0 || dp.y + y < 0) continue;
                    const uint8_t* src_row = static_cast<const uint8_t*>(slock.pBits) +
                        static_cast<size_t>(sr.top + y) * slock.Pitch + static_cast<size_t>(sr.left) * bpp;
                    uint8_t* dst_row = static_cast<uint8_t*>(dlock.pBits) +
                        static_cast<size_t>(dp.y + y) * dlock.Pitch + static_cast<size_t>(dp.x) * bpp;
                    if (rw > 0)
                    {
                        std::memcpy(dst_row, src_row, static_cast<size_t>(rw) * bpp);
                    }
                }
            }
        }

        pDestinationSurface->UnlockRect();
        pSourceSurface->UnlockRect();
        return D3D_OK;
    }

    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture8* pSourceTexture, IDirect3DBaseTexture8* pDestinationTexture) override
    {
        if (!pSourceTexture || !pDestinationTexture) return D3DERR_INVALIDCALL;
        if (pSourceTexture->GetType() != D3DRTYPE_TEXTURE || pDestinationTexture->GetType() != D3DRTYPE_TEXTURE)
        {
            return D3D_OK;
        }
        auto* src_tex = static_cast<ZhrVkTexture8*>(pSourceTexture);
        auto* dst_tex = static_cast<ZhrVkTexture8*>(pDestinationTexture);
        const UINT levels = std::min(src_tex->GetLevelCount(), dst_tex->GetLevelCount());
        for (UINT i = 0; i < levels; ++i)
        {
            IDirect3DSurface8* ss = nullptr;
            IDirect3DSurface8* ds = nullptr;
            src_tex->GetSurfaceLevel(i, &ss);
            dst_tex->GetSurfaceLevel(i, &ds);
            if (ss && ds) CopyRects(ss, nullptr, 0, ds, nullptr);
            if (ss) ss->Release();
            if (ds) ds->Release();
        }
        dst_tex->MarkDirty();
        return D3D_OK;
    }

    STDMETHOD(GetFrontBuffer)(IDirect3DSurface8*) override { return D3D_OK; }

    STDMETHOD(SetRenderTarget)(IDirect3DSurface8* pRenderTarget, IDirect3DSurface8* pNewZStencil) override
    {
        if (pRenderTarget)
        {
            pRenderTarget->AddRef();
            if (m_current_rt) m_current_rt->Release();
            m_current_rt = pRenderTarget;
        }
        if (pNewZStencil)
        {
            pNewZStencil->AddRef();
            if (m_current_depth) m_current_depth->Release();
            m_current_depth = pNewZStencil;
        }
        return D3D_OK;
    }
    STDMETHOD(GetRenderTarget)(IDirect3DSurface8** ppRenderTarget) override
    {
        if (!ppRenderTarget) return D3DERR_INVALIDCALL;
        if (m_current_rt) m_current_rt->AddRef();
        *ppRenderTarget = m_current_rt;
        return D3D_OK;
    }
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface8** ppZStencilSurface) override
    {
        if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
        if (m_current_depth) m_current_depth->AddRef();
        *ppZStencilSurface = m_current_depth;
        return D3D_OK;
    }

    STDMETHOD(BeginScene)() override
    {
        EnsureFrameBegun();
        return D3D_OK;
    }
    STDMETHOD(EndScene)() override
    {
        return D3D_OK;
    }
    STDMETHOD(Clear)(DWORD /*Count*/, CONST D3DRECT* /*pRects*/, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override
    {
        EnsureFrameBegun();
        const bool clear_color = (Flags & D3DCLEAR_TARGET) != 0;
        const bool clear_z = (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0;
        const float r = static_cast<float>((Color >> 16) & 0xFF) / 255.0f;
        const float g = static_cast<float>((Color >> 8) & 0xFF) / 255.0f;
        const float b = static_cast<float>(Color & 0xFF) / 255.0f;
        const float a = static_cast<float>((Color >> 24) & 0xFF) / 255.0f;
        zhr_vk_clear(clear_color, clear_z, r, g, b, a, Z, Stencil);
        return D3D_OK;
    }

    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override
    {
        if (!pMatrix) return D3DERR_INVALIDCALL;
        if (State == D3DTS_WORLD)
        {
            m_world = *pMatrix;
            zhr_vk_set_transform(0, &m_world.m[0][0]);
        }
        else if (State == D3DTS_VIEW)
        {
            m_view = *pMatrix;
            zhr_vk_set_transform(1, &m_view.m[0][0]);
        }
        else if (State == D3DTS_PROJECTION)
        {
            m_proj = *pMatrix;
            zhr_vk_set_transform(2, &m_proj.m[0][0]);
        }
        else if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE3)
        {
            m_tex_matrix[State - D3DTS_TEXTURE0] = *pMatrix;
        }
        return D3D_OK;
    }
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override
    {
        if (!pMatrix) return D3DERR_INVALIDCALL;
        if (State == D3DTS_WORLD) *pMatrix = m_world;
        else if (State == D3DTS_VIEW) *pMatrix = m_view;
        else if (State == D3DTS_PROJECTION) *pMatrix = m_proj;
        else if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE3) *pMatrix = m_tex_matrix[State - D3DTS_TEXTURE0];
        else MakeIdentityMatrix(*pMatrix);
        return D3D_OK;
    }
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override
    {
        if (!pMatrix) return D3DERR_INVALIDCALL;
        D3DMATRIX cur{};
        GetTransform(State, &cur);
        D3DMATRIX res{};
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                res.m[r][c] =
                    cur.m[r][0] * pMatrix->m[0][c] +
                    cur.m[r][1] * pMatrix->m[1][c] +
                    cur.m[r][2] * pMatrix->m[2][c] +
                    cur.m[r][3] * pMatrix->m[3][c];
            }
        }
        return SetTransform(State, &res);
    }

    STDMETHOD(SetViewport)(CONST D3DVIEWPORT8* pViewport) override
    {
        if (!pViewport) return D3DERR_INVALIDCALL;
        m_viewport = *pViewport;
        zhr_vk_set_viewport(
            m_viewport.X,
            m_viewport.Y,
            m_viewport.Width,
            m_viewport.Height,
            m_viewport.MinZ,
            m_viewport.MaxZ);
        return D3D_OK;
    }
    STDMETHOD(GetViewport)(D3DVIEWPORT8* pViewport) override
    {
        if (!pViewport) return D3DERR_INVALIDCALL;
        *pViewport = m_viewport;
        return D3D_OK;
    }

    STDMETHOD(SetMaterial)(CONST D3DMATERIAL8* pMaterial) override
    {
        if (pMaterial) m_material = *pMaterial;
        return D3D_OK;
    }
    STDMETHOD(GetMaterial)(D3DMATERIAL8* pMaterial) override
    {
        if (!pMaterial) return D3DERR_INVALIDCALL;
        *pMaterial = m_material;
        return D3D_OK;
    }

    STDMETHOD(SetLight)(DWORD Index, CONST D3DLIGHT8* pLight) override
    {
        if (Index < 4 && pLight)
        {
            m_lights[Index] = *pLight;
            if (m_light_enabled[Index])
            {
                PushLightToVulkan(Index);
            }
        }
        return D3D_OK;
    }
    STDMETHOD(GetLight)(DWORD Index, D3DLIGHT8* pLight) override
    {
        if (Index >= 4 || !pLight) return D3DERR_INVALIDCALL;
        *pLight = m_lights[Index];
        return D3D_OK;
    }
    STDMETHOD(LightEnable)(DWORD Index, BOOL Enable) override
    {
        if (Index < 4)
        {
            m_light_enabled[Index] = (Enable != FALSE);
            if (m_light_enabled[Index])
            {
                PushLightToVulkan(Index);
            }
            else
            {
                zhr_vk_clear_light(Index);
            }
        }
        return D3D_OK;
    }
    STDMETHOD(GetLightEnable)(DWORD Index, BOOL* pEnable) override
    {
        if (Index >= 4 || !pEnable) return D3DERR_INVALIDCALL;
        *pEnable = m_light_enabled[Index] ? TRUE : FALSE;
        return D3D_OK;
    }

    STDMETHOD(SetClipPlane)(DWORD, CONST float*) override { return D3D_OK; }
    STDMETHOD(GetClipPlane)(DWORD, float* pPlane) override
    {
        if (pPlane) std::memset(pPlane, 0, 4 * sizeof(float));
        return D3D_OK;
    }

    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) override
    {
        const uint32_t idx = static_cast<uint32_t>(State);
        if (idx < 256)
        {
            m_render_states[idx] = Value;
            if (State == D3DRS_AMBIENT)
            {
                const float r = static_cast<float>((Value >> 16) & 0xFF) / 255.0f;
                const float g = static_cast<float>((Value >> 8) & 0xFF) / 255.0f;
                const float b = static_cast<float>(Value & 0xFF) / 255.0f;
                zhr_vk_set_ambient(r, g, b);
            }
        }
        return D3D_OK;
    }
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State, DWORD* pValue) override
    {
        if (!pValue) return D3DERR_INVALIDCALL;
        const uint32_t idx = static_cast<uint32_t>(State);
        *pValue = idx < 256 ? m_render_states[idx] : 0;
        return D3D_OK;
    }

    STDMETHOD(BeginStateBlock)() override { return D3D_OK; }
    STDMETHOD(EndStateBlock)(DWORD* pToken) override { if (pToken) *pToken = 1; return D3D_OK; }
    STDMETHOD(ApplyStateBlock)(DWORD) override { return D3D_OK; }
    STDMETHOD(CaptureStateBlock)(DWORD) override { return D3D_OK; }
    STDMETHOD(DeleteStateBlock)(DWORD) override { return D3D_OK; }
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE, DWORD* pToken) override { if (pToken) *pToken = 1; return D3D_OK; }

    STDMETHOD(SetClipStatus)(CONST D3DCLIPSTATUS8*) override { return D3D_OK; }
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS8* pClipStatus) override
    {
        if (pClipStatus) std::memset(pClipStatus, 0, sizeof(*pClipStatus));
        return D3D_OK;
    }

    STDMETHOD(GetTexture)(DWORD Stage, IDirect3DBaseTexture8** ppTexture) override
    {
        if (!ppTexture || Stage >= 8) return D3DERR_INVALIDCALL;
        if (m_textures[Stage]) m_textures[Stage]->AddRef();
        *ppTexture = m_textures[Stage];
        return D3D_OK;
    }
    STDMETHOD(SetTexture)(DWORD Stage, IDirect3DBaseTexture8* pTexture) override
    {
        if (Stage >= 8) return D3D_OK;
        if (pTexture) pTexture->AddRef();
        if (m_textures[Stage]) m_textures[Stage]->Release();
        m_textures[Stage] = pTexture;
        return D3D_OK;
    }

    STDMETHOD(GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override
    {
        if (!pValue || Stage >= 8 || static_cast<uint32_t>(Type) >= 32) return D3DERR_INVALIDCALL;
        *pValue = m_tss[Stage][static_cast<uint32_t>(Type)];
        return D3D_OK;
    }
    STDMETHOD(SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override
    {
        if (Stage < 8 && static_cast<uint32_t>(Type) < 32)
        {
            m_tss[Stage][static_cast<uint32_t>(Type)] = Value;
        }
        return D3D_OK;
    }

    STDMETHOD(ValidateDevice)(DWORD* pNumPasses) override
    {
        if (pNumPasses) *pNumPasses = 1;
        return D3D_OK;
    }
    STDMETHOD(GetInfo)(DWORD, void*, DWORD) override { return S_FALSE; }
    STDMETHOD(SetPaletteEntries)(UINT, CONST PALETTEENTRY*) override { return D3D_OK; }
    STDMETHOD(GetPaletteEntries)(UINT, PALETTEENTRY*) override { return D3D_OK; }
    STDMETHOD(SetCurrentTexturePalette)(UINT) override { return D3D_OK; }
    STDMETHOD(GetCurrentTexturePalette)(UINT* p) override { if (p) *p = 0; return D3D_OK; }

    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override
    {
        if (PrimitiveCount == 0 || !m_streams[0]) return D3D_OK;
        const uint32_t vcount = CalcVertexCountForPrimitive(PrimitiveType, PrimitiveCount);
        if (vcount == 0) return D3D_OK;
        const DWORD fvf = m_fvf != 0 ? m_fvf : m_streams[0]->FVF();
        const uint32_t stride = m_strides[0] > 0 ? m_strides[0] : CalcFvfStride(fvf);
        if (stride == 0) return D3D_OK;

        const size_t start_byte = static_cast<size_t>(StartVertex) * stride;
        if (start_byte + static_cast<size_t>(vcount) * stride > m_streams[0]->Size())
        {
            return D3D_OK;
        }

        return SubmitDraw(
            PrimitiveType,
            PrimitiveCount,
            m_streams[0]->Data() + start_byte,
            stride,
            fvf,
            vcount,
            nullptr,
            0);
    }

    STDMETHOD(DrawIndexedPrimitive)(
        D3DPRIMITIVETYPE PrimitiveType,
        UINT minIndex,
        UINT NumVertices,
        UINT startIndex,
        UINT primCount) override
    {
        if (primCount == 0 || !m_streams[0] || !m_index_buffer) return D3D_OK;
        const uint32_t idx_count = CalcVertexCountForPrimitive(PrimitiveType, primCount);
        if (idx_count == 0) return D3D_OK;

        const DWORD fvf = m_fvf != 0 ? m_fvf : m_streams[0]->FVF();
        const uint32_t stride = m_strides[0] > 0 ? m_strides[0] : CalcFvfStride(fvf);
        if (stride == 0) return D3D_OK;

        if (startIndex + idx_count > m_index_buffer->Count16())
        {
            return D3D_OK;
        }

        const auto t_sub0 = m_perf_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const DWORD pos_flags = fvf & D3DFVF_POSITION_MASK;
        const bool is_rhw = (pos_flags == D3DFVF_XYZRHW);
        const bool is_dynamic_vb = m_streams[0]->IsDynamic() || m_index_buffer->IsDynamic();
        const uint64_t cur_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
        const bool buffers_stable =
            !is_dynamic_vb &&
            (m_streams[0]->Generation() <= 2u || m_streams[0]->LastMutatedFrame() + 1u < cur_frame) &&
            (m_index_buffer->Generation() <= 2u || m_index_buffer->LastMutatedFrame() + 1u < cur_frame);

        StaticDrawKey static_key{};
        const StaticDrawKey* static_key_ptr = nullptr;

        if (!is_rhw && buffers_stable)
        {
            const bool has_normal = (fvf & D3DFVF_NORMAL) != 0;
            const bool has_diffuse = (fvf & D3DFVF_DIFFUSE) != 0;
            const uint32_t tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
            const bool use_material_color =
                (m_render_states[D3DRS_LIGHTING] != 0) &&
                (!has_diffuse ||
                 m_render_states[D3DRS_COLORVERTEX] == 0 ||
                 m_render_states[D3DRS_DIFFUSEMATERIALSOURCE] == D3DMCS_MATERIAL);

            static const bool s_mesh_env_init = []() -> bool {
                const char* e = std::getenv("ZHR_MESH_SMOOTHING");
                if (e != nullptr && e[0] != '\0')
                {
                    g_zhr_mesh_smoothing_enabled.store(e[0] != '0', std::memory_order_relaxed);
                }
                return true;
            }();
            (void)s_mesh_env_init;

            const uint32_t gfx_packed = zhr_vk_get_graphics_mode();
            const bool remaster_active = (gfx_packed & 0x3u) != 0u;
            const bool mesh_smooth_on = g_zhr_mesh_smoothing_enabled.load(std::memory_order_relaxed) && remaster_active;
            const bool terrain_pass_active = g_zhr_terrain_pass_active.load(std::memory_order_relaxed);
            const bool is_stencil_only_pass =
                (m_render_states[D3DRS_COLORWRITEENABLE] & 0x7u) == 0 ||
                (m_render_states[D3DRS_ZWRITEENABLE] == 0) ||
                (m_render_states[D3DRS_STENCILENABLE] != 0 &&
                 m_render_states[D3DRS_STENCILFUNC] == D3DCMP_NEVER);

            uint32_t tess_mode = 0u;
            uint64_t extra_key = 0u;
            if (mesh_smooth_on && terrain_pass_active &&
                g_zhr_terrain_hm.data != nullptr &&
                g_zhr_terrain_hm.width >= 4 &&
                g_zhr_terrain_hm.height >= 4 &&
                PrimitiveType == D3DPT_TRIANGLELIST &&
                primCount >= 2u && primCount <= 4096u)
            {
                tess_mode = 2u;
                extra_key = g_zhr_terrain_hm.generation;
            }
            else if (mesh_smooth_on && !terrain_pass_active &&
                     has_normal &&
                     (m_render_states[D3DRS_LIGHTING] != 0) &&
                     !is_stencil_only_pass &&
                     (m_render_states[D3DRS_ALPHABLENDENABLE] == 0) &&
                     PrimitiveType == D3DPT_TRIANGLELIST &&
                     primCount >= 6u && primCount <= 4000u)
            {
                tess_mode = 1u;
                ZhrVkTexture8* diffuse_tex =
                    (m_textures[0] != nullptr && m_textures[0]->GetType() == D3DRTYPE_TEXTURE)
                        ? static_cast<ZhrVkTexture8*>(m_textures[0])
                        : nullptr;
                extra_key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(diffuse_tex));
            }

            uint32_t mat_rgba = 0u;
            if (use_material_color)
            {
                const uint32_t mr = static_cast<uint32_t>(std::clamp(m_material.Diffuse.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                const uint32_t mg = static_cast<uint32_t>(std::clamp(m_material.Diffuse.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                const uint32_t mb = static_cast<uint32_t>(std::clamp(m_material.Diffuse.b, 0.0f, 1.0f) * 255.0f + 0.5f);
                const uint32_t ma = static_cast<uint32_t>(std::clamp(m_material.Diffuse.a, 0.0f, 1.0f) * 255.0f + 0.5f);
                mat_rgba = 0x80000000u | (mr << 24) | (mg << 16) | (mb << 8) | ma;
                // Distinct flag in tess_mode high bit so 0x00000000 material color never collides with unused.
                tess_mode |= 0x80u;
            }

            static_key.vb_id = m_streams[0]->Id();
            static_key.vb_gen = m_streams[0]->Generation();
            static_key.ib_id = m_index_buffer->Id();
            static_key.ib_gen = m_index_buffer->Generation();
            static_key.base_vertex = m_base_vertex_index;
            static_key.start_index = startIndex;
            static_key.prim_count = primCount;
            static_key.prim_type = static_cast<uint32_t>(PrimitiveType);
            static_key.fvf = fvf;
            static_key.stride = stride;
            static_key.mat_rgba = mat_rgba;
            static_key.tess_mode = tess_mode;
            static_key.extra_key = extra_key;

            auto it = m_static_gpu_cache.find(static_key);
            if (it != m_static_gpu_cache.end())
            {
                it->second.last_used_frame = cur_frame;
                EnsureFrameBegun();
                SyncDrawState(false, tex_count);
                zhr_vk_draw_indexed(it->second.gpu_vb, it->second.gpu_ib, it->second.index_count, 0u, 0);
                if (m_perf_enabled)
                {
                    ++m_perf_static_hits;
                    const auto t_sub1 = std::chrono::steady_clock::now();
                    m_perf_submit_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(t_sub1 - t_sub0).count());
                }
                return D3D_OK;
            }
            static_key_ptr = &static_key;
        }

        const uint16_t* raw_indices = m_index_buffer->Data16() + startIndex;

        // Compute tight vertex range referenced by indices to avoid unpacking unused vertices.
        uint32_t v_min = minIndex;
        uint32_t v_max = minIndex + (NumVertices > 0 ? NumVertices - 1 : 0);
        uint32_t actual_min = 0xFFFFFFFFu;
        uint32_t actual_max = 0;
        for (uint32_t i = 0; i < idx_count; ++i)
        {
            const uint32_t vi = raw_indices[i];
            if (vi < actual_min) actual_min = vi;
            if (vi > actual_max) actual_max = vi;
        }
        v_min = actual_min;
        v_max = actual_max;

        const uint32_t first_vert = m_base_vertex_index + v_min;
        const uint32_t vert_span = (v_max - v_min) + 1;
        const size_t start_byte = static_cast<size_t>(first_vert) * stride;
        if (start_byte + static_cast<size_t>(vert_span) * stride > m_streams[0]->Size())
        {
            return D3D_OK;
        }

        const HRESULT hr = SubmitDraw(
            PrimitiveType,
            primCount,
            m_streams[0]->Data() + start_byte,
            stride,
            fvf,
            vert_span,
            raw_indices,
            v_min,
            static_key_ptr,
            is_dynamic_vb);
        if (m_perf_enabled)
        {
            const auto t_sub1 = std::chrono::steady_clock::now();
            m_perf_submit_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t_sub1 - t_sub0).count());
        }
        return hr;
    }

    STDMETHOD(DrawPrimitiveUP)(
        D3DPRIMITIVETYPE PrimitiveType,
        UINT PrimitiveCount,
        CONST void* pVertexStreamZeroData,
        UINT VertexStreamZeroStride) override
    {
        if (PrimitiveCount == 0 || !pVertexStreamZeroData || VertexStreamZeroStride == 0) return D3D_OK;
        const uint32_t vcount = CalcVertexCountForPrimitive(PrimitiveType, PrimitiveCount);
        if (vcount == 0) return D3D_OK;
        return SubmitDraw(
            PrimitiveType,
            PrimitiveCount,
            static_cast<const uint8_t*>(pVertexStreamZeroData),
            VertexStreamZeroStride,
            m_fvf,
            vcount,
            nullptr,
            0);
    }

    STDMETHOD(DrawIndexedPrimitiveUP)(
        D3DPRIMITIVETYPE PrimitiveType,
        UINT MinVertexIndex,
        UINT NumVertexIndices,
        UINT PrimitiveCount,
        CONST void* pIndexData,
        D3DFORMAT IndexDataFormat,
        CONST void* pVertexStreamZeroData,
        UINT VertexStreamZeroStride) override
    {
        if (PrimitiveCount == 0 || !pIndexData || !pVertexStreamZeroData || VertexStreamZeroStride == 0) return D3D_OK;
        if (IndexDataFormat != D3DFMT_INDEX16) return D3D_OK;
        const uint32_t idx_count = CalcVertexCountForPrimitive(PrimitiveType, PrimitiveCount);
        if (idx_count == 0) return D3D_OK;
        const uint16_t* raw_indices = static_cast<const uint16_t*>(pIndexData);
        const uint32_t vcount = MinVertexIndex + NumVertexIndices;
        return SubmitDraw(
            PrimitiveType,
            PrimitiveCount,
            static_cast<const uint8_t*>(pVertexStreamZeroData),
            VertexStreamZeroStride,
            m_fvf,
            vcount,
            raw_indices,
            0);
    }

    STDMETHOD(ProcessVertices)(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) override { return D3D_OK; }

    STDMETHOD(CreateVertexShader)(CONST DWORD*, CONST DWORD*, DWORD* pHandle, DWORD) override
    {
        if (pHandle) *pHandle = 0;
        return D3DERR_NOTAVAILABLE;
    }
    STDMETHOD(SetVertexShader)(DWORD Handle) override
    {
        m_fvf = Handle;
        return D3D_OK;
    }
    STDMETHOD(GetVertexShader)(DWORD* pHandle) override
    {
        if (pHandle) *pHandle = m_fvf;
        return D3D_OK;
    }
    STDMETHOD(DeleteVertexShader)(DWORD) override { return D3D_OK; }
    STDMETHOD(SetVertexShaderConstant)(DWORD, CONST void*, DWORD) override { return D3D_OK; }
    STDMETHOD(GetVertexShaderConstant)(DWORD, void*, DWORD) override { return D3D_OK; }
    STDMETHOD(GetVertexShaderDeclaration)(DWORD, void*, DWORD*) override { return D3D_OK; }
    STDMETHOD(GetVertexShaderFunction)(DWORD, void*, DWORD*) override { return D3D_OK; }

    STDMETHOD(SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer8* pStreamData, UINT Stride) override
    {
        if (StreamNumber >= 2) return D3D_OK;
        auto* vb = static_cast<ZhrVkVertexBuffer8*>(pStreamData);
        if (vb) vb->AddRef();
        if (m_streams[StreamNumber]) m_streams[StreamNumber]->Release();
        m_streams[StreamNumber] = vb;
        m_strides[StreamNumber] = Stride;
        return D3D_OK;
    }
    STDMETHOD(GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer8** ppStreamData, UINT* pStride) override
    {
        if (StreamNumber >= 2 || !ppStreamData) return D3DERR_INVALIDCALL;
        if (m_streams[StreamNumber]) m_streams[StreamNumber]->AddRef();
        *ppStreamData = m_streams[StreamNumber];
        if (pStride) *pStride = m_strides[StreamNumber];
        return D3D_OK;
    }

    STDMETHOD(SetIndices)(IDirect3DIndexBuffer8* pIndexData, UINT BaseVertexIndex) override
    {
        auto* ib = static_cast<ZhrVkIndexBuffer8*>(pIndexData);
        if (ib) ib->AddRef();
        if (m_index_buffer) m_index_buffer->Release();
        m_index_buffer = ib;
        m_base_vertex_index = BaseVertexIndex;
        return D3D_OK;
    }
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer8** ppIndexData, UINT* pBaseVertexIndex) override
    {
        if (!ppIndexData) return D3DERR_INVALIDCALL;
        if (m_index_buffer) m_index_buffer->AddRef();
        *ppIndexData = m_index_buffer;
        if (pBaseVertexIndex) *pBaseVertexIndex = m_base_vertex_index;
        return D3D_OK;
    }

    STDMETHOD(CreatePixelShader)(CONST DWORD*, DWORD* pHandle) override
    {
        if (pHandle) *pHandle = 0;
        return D3DERR_NOTAVAILABLE;
    }
    STDMETHOD(SetPixelShader)(DWORD Handle) override
    {
        m_pixel_shader = Handle;
        return D3D_OK;
    }
    STDMETHOD(GetPixelShader)(DWORD* pHandle) override
    {
        if (pHandle) *pHandle = m_pixel_shader;
        return D3D_OK;
    }
    STDMETHOD(DeletePixelShader)(DWORD) override { return D3D_OK; }
    STDMETHOD(SetPixelShaderConstant)(DWORD, CONST void*, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPixelShaderConstant)(DWORD, void*, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPixelShaderFunction)(DWORD, void*, DWORD*) override { return D3D_OK; }

    STDMETHOD(DrawRectPatch)(UINT, CONST float*, CONST D3DRECTPATCH_INFO*) override { return D3D_OK; }
    STDMETHOD(DrawTriPatch)(UINT, CONST float*, CONST D3DTRIPATCH_INFO*) override { return D3D_OK; }
    STDMETHOD(DeletePatch)(UINT) override { return D3D_OK; }

private:
    void InitDefaultStates()
    {
        std::memset(m_render_states, 0, sizeof(m_render_states));
        m_render_states[D3DRS_ZENABLE] = D3DZB_TRUE;
        m_render_states[D3DRS_FILLMODE] = D3DFILL_SOLID;
        m_render_states[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
        m_render_states[D3DRS_ZWRITEENABLE] = TRUE;
        m_render_states[D3DRS_ALPHATESTENABLE] = FALSE;
        m_render_states[D3DRS_SRCBLEND] = D3DBLEND_ONE;
        m_render_states[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
        m_render_states[D3DRS_CULLMODE] = D3DCULL_CCW;
        m_render_states[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
        m_render_states[D3DRS_ALPHAREF] = 0;
        m_render_states[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
        m_render_states[D3DRS_ALPHABLENDENABLE] = FALSE;
        m_render_states[D3DRS_FOGENABLE] = FALSE;
        m_render_states[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
        m_render_states[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
        m_render_states[D3DRS_FOGSTART] = 0;
        {
            const float one = 1.0f;
            std::memcpy(&m_render_states[D3DRS_FOGEND], &one, sizeof(float));
        }
        m_render_states[D3DRS_LIGHTING] = TRUE;
        m_render_states[D3DRS_COLORVERTEX] = TRUE;
        m_render_states[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
        m_render_states[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
        m_render_states[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
        m_render_states[D3DRS_COLORWRITEENABLE] = 0x0000000F;
        m_render_states[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
        m_render_states[D3DRS_STENCILENABLE] = FALSE;
        m_render_states[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
        m_render_states[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
        m_render_states[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
        m_render_states[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
        m_render_states[D3DRS_STENCILREF] = 0;
        m_render_states[D3DRS_STENCILMASK] = 0xFFFFFFFFu;
        m_render_states[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;

        std::memset(m_tss, 0, sizeof(m_tss));
        for (int s = 0; s < 8; ++s)
        {
            m_tss[s][D3DTSS_COLOROP] = (s == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
            m_tss[s][D3DTSS_COLORARG0] = D3DTA_CURRENT;
            m_tss[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            m_tss[s][D3DTSS_COLORARG2] = (s == 0) ? D3DTA_DIFFUSE : D3DTA_CURRENT;
            m_tss[s][D3DTSS_ALPHAOP] = (s == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
            m_tss[s][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
            m_tss[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            m_tss[s][D3DTSS_ALPHAARG2] = (s == 0) ? D3DTA_DIFFUSE : D3DTA_CURRENT;
            m_tss[s][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
            m_tss[s][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
            m_tss[s][D3DTSS_MAGFILTER] = D3DTEXF_LINEAR;
            m_tss[s][D3DTSS_MINFILTER] = D3DTEXF_LINEAR;
            m_tss[s][D3DTSS_TEXCOORDINDEX] = static_cast<DWORD>(s);
            m_tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
        }
    }

    void EnsureFrameBegun()
    {
        if (!zhr_vk_is_frame_active())
        {
            zhr_vk_begin_frame();
            zhr_vk_set_viewport(
                m_viewport.X,
                m_viewport.Y,
                m_viewport.Width,
                m_viewport.Height,
                m_viewport.MinZ,
                m_viewport.MaxZ);
        }
    }

    void PushLightToVulkan(DWORD index)
    {
        const D3DLIGHT8& l = m_lights[index];
        if (l.Type == D3DLIGHT_DIRECTIONAL)
        {
            zhr_vk_set_directional_light(
                index,
                l.Direction.x, l.Direction.y, l.Direction.z,
                l.Diffuse.r, l.Diffuse.g, l.Diffuse.b);
        }
        else if (l.Type == D3DLIGHT_POINT || l.Type == D3DLIGHT_SPOT)
        {
            zhr_vk_set_point_light(
                index,
                l.Position.x, l.Position.y, l.Position.z,
                l.Diffuse.r, l.Diffuse.g, l.Diffuse.b,
                l.Range);
        }
    }

    static uint32_t CalcVertexCountForPrimitive(D3DPRIMITIVETYPE type, UINT prim_count)
    {
        switch (type)
        {
        case D3DPT_TRIANGLELIST:  return prim_count * 3;
        case D3DPT_TRIANGLESTRIP: return prim_count + 2;
        case D3DPT_TRIANGLEFAN:   return prim_count + 2;
        default:                  return 0; // points/lines skipped for now
        }
    }

    static uint32_t CalcFvfStride(DWORD fvf)
    {
        uint32_t stride = 0;
        const DWORD pos = fvf & D3DFVF_POSITION_MASK;
        if (pos == D3DFVF_XYZ) stride += 12;
        else if (pos == D3DFVF_XYZRHW) stride += 16;
        else if (pos == D3DFVF_XYZB1) stride += 16;
        else if (pos == D3DFVF_XYZB2) stride += 20;
        else if (pos == D3DFVF_XYZB3) stride += 24;
        else if (pos == D3DFVF_XYZB4) stride += 28;
        else if (pos == D3DFVF_XYZB5) stride += 32;

        if (fvf & D3DFVF_NORMAL) stride += 12;
        if (fvf & D3DFVF_PSIZE) stride += 4;
        if (fvf & D3DFVF_DIFFUSE) stride += 4;
        if (fvf & D3DFVF_SPECULAR) stride += 4;
        const uint32_t tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        stride += tex_count * 8;
        return stride;
    }

    uint32_t BindStageTexture(int stage)
    {
        if (m_textures[stage] != nullptr && m_textures[stage]->GetType() == D3DRTYPE_TEXTURE)
        {
            auto* tex2d = static_cast<ZhrVkTexture8*>(m_textures[stage]);
            const bool linear = m_tss[stage][D3DTSS_MAGFILTER] != D3DTEXF_POINT;
            const bool clamp = (m_tss[stage][D3DTSS_ADDRESSU] == D3DTADDRESS_CLAMP) ||
                               (m_tss[stage][D3DTSS_ADDRESSV] == D3DTADDRESS_CLAMP);
            const uint32_t handle = tex2d->EnsureUploaded(linear, clamp);
            zhr_vk_set_texture_stage(static_cast<uint32_t>(stage), handle);
            return handle;
        }
        zhr_vk_set_texture_stage(static_cast<uint32_t>(stage), 0);
        return 0;
    }

    void SyncDrawState(bool is_rhw, uint32_t tex_count)
    {
        const bool z_enable = m_render_states[D3DRS_ZENABLE] != D3DZB_FALSE;
        const uint32_t depth_cmp = z_enable ? MapD3DCmpToShaderDepth(m_render_states[D3DRS_ZFUNC]) : 7u;
        const bool depth_write = z_enable && (m_render_states[D3DRS_ZWRITEENABLE] != 0);
        const bool color_write = (m_render_states[D3DRS_COLORWRITEENABLE] & 0x7u) != 0;

        uint32_t src_blend = 1u; // One
        uint32_t dst_blend = 0u; // Zero
        if (m_render_states[D3DRS_ALPHABLENDENABLE] != 0)
        {
            src_blend = MapD3DBlendToSrc(m_render_states[D3DRS_SRCBLEND]);
            dst_blend = MapD3DBlendToDst(m_render_states[D3DRS_DESTBLEND]);
        }

        float fstart = 0.0f, fend = 0.0f;
        std::memcpy(&fstart, &m_render_states[D3DRS_FOGSTART], sizeof(float));
        std::memcpy(&fend, &m_render_states[D3DRS_FOGEND], sizeof(float));
        const bool fog_mode_active =
            (m_render_states[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE) ||
            (m_render_states[D3DRS_FOGTABLEMODE] != D3DFOG_NONE);
        const bool fog_enable = !is_rhw &&
            (m_render_states[D3DRS_FOGENABLE] != 0) &&
            fog_mode_active &&
            (fend > fstart) &&
            (fend > 1.0f);
        const bool alpha_test = m_render_states[D3DRS_ALPHATESTENABLE] != 0;

        const uint32_t s0_cop = MapD3DTexOp(m_tss[0][D3DTSS_COLOROP]);
        const uint32_t s0_aop = MapD3DTexOp(m_tss[0][D3DTSS_ALPHAOP]);
        const uint32_t s1_cop = (s0_cop != 0) ? MapD3DTexOp(m_tss[1][D3DTSS_COLOROP]) : 0u;
        const uint32_t s1_aop = (s0_cop != 0 && s0_aop != 0) ? MapD3DTexOp(m_tss[1][D3DTSS_ALPHAOP]) : 0u;

        if (s0_cop == 0)
        {
            // Stage 0 disabled: pass diffuse RGB + A through and disable stage 1.
            zhr_vk_set_stage_state(0, 1u, 0u, 1u, 0u);
            zhr_vk_set_stage_state(1, 0u, 0u, 0u, 0u);
            zhr_vk_set_texture_stage(0, 0);
            zhr_vk_set_texture_stage(1, 0);
        }
        else
        {
            const uint32_t s0_cargs = PackD3DTexArgs(
                m_tss[0][D3DTSS_COLORARG1],
                m_tss[0][D3DTSS_COLORARG2],
                m_tss[0][D3DTSS_COLORARG0]);
            const uint32_t s0_aargs = PackD3DTexArgs(
                m_tss[0][D3DTSS_ALPHAARG1],
                m_tss[0][D3DTSS_ALPHAARG2],
                m_tss[0][D3DTSS_ALPHAARG0]);
            zhr_vk_set_stage_state(0, s0_cop, s0_cargs, s0_aop, s0_aargs);
            BindStageTexture(0);

            if (s1_cop != 0)
            {
                const uint32_t s1_cargs = PackD3DTexArgs(
                    m_tss[1][D3DTSS_COLORARG1],
                    m_tss[1][D3DTSS_COLORARG2],
                    m_tss[1][D3DTSS_COLORARG0]);
                const uint32_t s1_aargs = PackD3DTexArgs(
                    m_tss[1][D3DTSS_ALPHAARG1],
                    m_tss[1][D3DTSS_ALPHAARG2],
                    m_tss[1][D3DTSS_ALPHAARG0]);
                zhr_vk_set_stage_state(1, s1_cop, s1_cargs, s1_aop, s1_aargs);
                BindStageTexture(1);
            }
            else
            {
                zhr_vk_set_stage_state(1, 0u, 0u, 0u, 0u);
                zhr_vk_set_texture_stage(1, 0);
            }
        }

        const DWORD tf = m_render_states[D3DRS_TEXTUREFACTOR];
        zhr_vk_set_texture_factor(
            static_cast<float>((tf >> 16) & 0xFFu) * (1.0f / 255.0f),
            static_cast<float>((tf >> 8) & 0xFFu) * (1.0f / 255.0f),
            static_cast<float>(tf & 0xFFu) * (1.0f / 255.0f),
            static_cast<float>((tf >> 24) & 0xFFu) * (1.0f / 255.0f));

        const bool texturing = (s0_cop != 0);

        const DWORD cull = m_render_states[D3DRS_CULLMODE];
        const bool cull_enabled = !is_rhw && (cull != D3DCULL_NONE);
        const bool cull_cw = (cull == D3DCULL_CW);

        const uint32_t shader_bits =
            (depth_cmp << 0) |
            (static_cast<uint32_t>(depth_write) << 3) |
            (static_cast<uint32_t>(color_write) << 4) |
            (dst_blend << 5) |
            (static_cast<uint32_t>(fog_enable) << 8) |
            ((src_blend & 0x3u) << 14) |
            (static_cast<uint32_t>(texturing) << 16) |
            (static_cast<uint32_t>(alpha_test) << 18) |
            (static_cast<uint32_t>(cull_enabled) << 19) |
            (static_cast<uint32_t>(cull_cw) << 28) |
            (((src_blend >> 2) & 0x3u) << 29);

        zhr_vk_set_shader(shader_bits);

        const bool stencil_enable = m_render_states[D3DRS_STENCILENABLE] != 0;
        zhr_vk_set_stencil_state(
            stencil_enable,
            MapD3DCmpToShaderDepth(m_render_states[D3DRS_STENCILFUNC]),
            m_render_states[D3DRS_STENCILFAIL],
            m_render_states[D3DRS_STENCILZFAIL],
            m_render_states[D3DRS_STENCILPASS],
            m_render_states[D3DRS_STENCILMASK],
            m_render_states[D3DRS_STENCILWRITEMASK],
            m_render_states[D3DRS_STENCILREF]);

        if (alpha_test)
        {
            const float ref = static_cast<float>(m_render_states[D3DRS_ALPHAREF] & 0xFFu) / 255.0f;
            zhr_vk_set_alpha_test_ref(ref > 0.0f ? ref : (1.0f / 255.0f));
        }

        const bool lighting = !is_rhw && (m_render_states[D3DRS_LIGHTING] != 0);
        zhr_vk_set_lighting_enabled(lighting);

        if (fog_enable)
        {
            const DWORD fc = m_render_states[D3DRS_FOGCOLOR];
            zhr_vk_set_fog(
                true,
                static_cast<float>((fc >> 16) & 0xFFu) / 255.0f,
                static_cast<float>((fc >> 8) & 0xFFu) / 255.0f,
                static_cast<float>(fc & 0xFFu) / 255.0f,
                fstart,
                fend);
        }
        else
        {
            zhr_vk_set_fog(false, 0.0f, 0.0f, 0.0f, 0.0f, 1000.0f);
        }

        // Push GPU vertex-shader UV generation & texture transform matrices (moves TCI_CAMERASPACEPOSITION
        // & scrolling cloud/shroud transforms off the CPU so all terrain passes share one GPU VB/IB).
        if (is_rhw)
        {
            zhr_vk_set_tex_gen(0u, 0u, 0u, nullptr);
            zhr_vk_set_tex_gen(1u, 0u, 1u, nullptr);
        }
        else
        {
            const bool stage1_active = (s0_cop != 0u) && (s1_cop != 0u);
            auto sync_stage_uv = [&](uint32_t stage, bool active) {
                if (!active)
                {
                    zhr_vk_set_tex_gen(stage, 0u, stage, nullptr);
                    return;
                }
                const DWORD tci = m_tss[stage][D3DTSS_TEXCOORDINDEX];
                const DWORD tci_mode = tci & 0xFFFF0000u;
                const DWORD tci_index = tci & 0x7u;
                const bool use_xform =
                    (m_tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS] & 0xFFu) != D3DTTFF_DISABLE;
                if (tci_mode == D3DTSS_TCI_CAMERASPACEPOSITION)
                {
                    const uint32_t mode = use_xform ? 3u : 2u;
                    zhr_vk_set_tex_gen(
                        stage,
                        mode,
                        0u,
                        use_xform ? &m_tex_matrix[stage].m[0][0] : nullptr);
                }
                else
                {
                    const uint32_t src_idx = (tci_index == 1u && tex_count >= 2u) ? 1u : 0u;
                    const uint32_t mode = use_xform ? 1u : 0u;
                    zhr_vk_set_tex_gen(
                        stage,
                        mode,
                        src_idx,
                        use_xform ? &m_tex_matrix[stage].m[0][0] : nullptr);
                }
            };
            sync_stage_uv(0u, true);
            sync_stage_uv(1u, stage1_active);
        }
    }

    HRESULT SubmitDraw(
        D3DPRIMITIVETYPE prim_type,
        UINT prim_count,
        const uint8_t* vb_data,
        uint32_t stride,
        DWORD fvf,
        uint32_t vertex_count,
        const uint16_t* raw_indices,
        uint32_t index_rebase,
        const StaticDrawKey* static_key = nullptr,
        bool is_dynamic_vb = true)
    {
        if (vertex_count == 0 || prim_count == 0) return D3D_OK;

        EnsureFrameBegun();

        const DWORD pos_flags = fvf & D3DFVF_POSITION_MASK;
        const bool is_rhw = (pos_flags == D3DFVF_XYZRHW);
        const bool has_normal = (fvf & D3DFVF_NORMAL) != 0;
        const bool has_psize = (fvf & D3DFVF_PSIZE) != 0;
        const bool has_diffuse = (fvf & D3DFVF_DIFFUSE) != 0;
        const bool has_specular = (fvf & D3DFVF_SPECULAR) != 0;
        const uint32_t tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

        uint32_t normal_off = 0, diffuse_off = 0, uv0_off = 0, uv1_off = 0;
        {
            uint32_t off = is_rhw ? 16u : 12u;
            if (pos_flags == D3DFVF_XYZB1) off = 16u;
            else if (pos_flags == D3DFVF_XYZB2) off = 20u;
            else if (pos_flags == D3DFVF_XYZB3) off = 24u;
            else if (pos_flags == D3DFVF_XYZB4) off = 28u;
            else if (pos_flags == D3DFVF_XYZB5) off = 32u;

            if (has_normal) { normal_off = off; off += 12u; }
            if (has_psize) { off += 4u; }
            if (has_diffuse) { diffuse_off = off; off += 4u; }
            if (has_specular) { off += 4u; }
            if (tex_count >= 1) { uv0_off = off; off += 8u; }
            if (tex_count >= 2) { uv1_off = off; }
        }

        // D3D8 fixed-function lighting rule: when LIGHTING is enabled, use D3DMATERIAL8::Diffuse
        // whenever vertices lack diffuse color, COLORVERTEX is FALSE, or DIFFUSEMATERIALSOURCE
        // is D3DMCS_MATERIAL (skinned .W3D infantry meshes write diffuse=0 with D3DMCS_MATERIAL!).
        const bool use_material_color =
            !is_rhw &&
            (m_render_states[D3DRS_LIGHTING] != 0) &&
            (!has_diffuse ||
             m_render_states[D3DRS_COLORVERTEX] == 0 ||
             m_render_states[D3DRS_DIFFUSEMATERIALSOURCE] == D3DMCS_MATERIAL);

        const float vp_x = static_cast<float>(m_viewport.X);
        const float vp_y = static_cast<float>(m_viewport.Y);
        const float vp_w = static_cast<float>(m_viewport.Width > 0 ? m_viewport.Width : 1);
        const float vp_h = static_cast<float>(m_viewport.Height > 0 ? m_viewport.Height : 1);

        m_scratch_verts.resize(vertex_count);
        for (uint32_t i = 0; i < vertex_count; ++i)
        {
            const uint8_t* vptr = vb_data + static_cast<size_t>(i) * stride;
            ZhrVertexWw3d& dst = m_scratch_verts[i];

            const float* p = reinterpret_cast<const float*>(vptr);
            if (is_rhw)
            {
                dst.position[0] = ((p[0] - vp_x) / vp_w) * 2.0f - 1.0f;
                dst.position[1] = 1.0f - ((p[1] - vp_y) / vp_h) * 2.0f;
                dst.position[2] = p[2];
            }
            else
            {
                dst.position[0] = p[0];
                dst.position[1] = p[1];
                dst.position[2] = p[2];
            }

            if (has_normal && normal_off + 12 <= stride)
            {
                const float* n = reinterpret_cast<const float*>(vptr + normal_off);
                dst.normal[0] = n[0];
                dst.normal[1] = n[1];
                dst.normal[2] = n[2];
            }
            else
            {
                dst.normal[0] = 0.0f;
                dst.normal[1] = 0.0f;
                dst.normal[2] = 1.0f;
            }

            if (has_diffuse && !use_material_color && diffuse_off + 4 <= stride)
            {
                const uint32_t argb = *reinterpret_cast<const uint32_t*>(vptr + diffuse_off);
                dst.color[0] = static_cast<float>((argb >> 16) & 0xFFu) * (1.0f / 255.0f);
                dst.color[1] = static_cast<float>((argb >> 8) & 0xFFu) * (1.0f / 255.0f);
                dst.color[2] = static_cast<float>(argb & 0xFFu) * (1.0f / 255.0f);
                dst.color[3] = static_cast<float>((argb >> 24) & 0xFFu) * (1.0f / 255.0f);
            }
            else if (use_material_color)
            {
                dst.color[0] = m_material.Diffuse.r;
                dst.color[1] = m_material.Diffuse.g;
                dst.color[2] = m_material.Diffuse.b;
                dst.color[3] = m_material.Diffuse.a;
            }
            else
            {
                dst.color[0] = 1.0f;
                dst.color[1] = 1.0f;
                dst.color[2] = 1.0f;
                dst.color[3] = 1.0f;
            }

            if (tex_count >= 1 && uv0_off + 8 <= stride)
            {
                const float* uv = reinterpret_cast<const float*>(vptr + uv0_off);
                dst.uv0[0] = uv[0];
                dst.uv0[1] = uv[1];
            }
            else
            {
                dst.uv0[0] = 0.0f;
                dst.uv0[1] = 0.0f;
            }
            if (tex_count >= 2 && uv1_off + 8 <= stride)
            {
                const float* uv = reinterpret_cast<const float*>(vptr + uv1_off);
                dst.uv1[0] = uv[0];
                dst.uv1[1] = uv[1];
            }
            else
            {
                dst.uv1[0] = 0.0f;
                dst.uv1[1] = 0.0f;
            }
        }

        // Build triangle list indices.
        m_scratch_indices.clear();
        m_scratch_indices.reserve(static_cast<size_t>(prim_count) * 3);

        auto get_idx = [&](uint32_t k) -> uint16_t {
            if (raw_indices != nullptr)
            {
                return static_cast<uint16_t>(raw_indices[k] - index_rebase);
            }
            return static_cast<uint16_t>(k);
        };

        if (prim_type == D3DPT_TRIANGLELIST)
        {
            for (uint32_t i = 0; i < prim_count * 3; ++i)
            {
                m_scratch_indices.push_back(get_idx(i));
            }
        }
        else if (prim_type == D3DPT_TRIANGLESTRIP)
        {
            for (uint32_t i = 0; i < prim_count; ++i)
            {
                const uint16_t i0 = get_idx(i);
                const uint16_t i1 = get_idx(i + 1);
                const uint16_t i2 = get_idx(i + 2);
                if ((i & 1u) == 0)
                {
                    m_scratch_indices.push_back(i0);
                    m_scratch_indices.push_back(i1);
                    m_scratch_indices.push_back(i2);
                }
                else
                {
                    m_scratch_indices.push_back(i0);
                    m_scratch_indices.push_back(i2);
                    m_scratch_indices.push_back(i1);
                }
            }
        }
        else if (prim_type == D3DPT_TRIANGLEFAN)
        {
            const uint16_t root = get_idx(0);
            for (uint32_t i = 0; i < prim_count; ++i)
            {
                m_scratch_indices.push_back(root);
                m_scratch_indices.push_back(get_idx(i + 1));
                m_scratch_indices.push_back(get_idx(i + 2));
            }
        }
        else
        {
            return D3D_OK;
        }

        SyncDrawState(is_rhw, tex_count);

        const uint32_t gfx_packed = zhr_vk_get_graphics_mode();
        const bool remaster_active = (gfx_packed & 0x3u) != 0u;
        // Optional startup override via ZHR_MESH_SMOOTHING=0|1 (still allows runtime F7 / A/B toggles).
        static const bool s_mesh_env_init = []() -> bool {
            const char* e = std::getenv("ZHR_MESH_SMOOTHING");
            if (e != nullptr && e[0] != '\0')
            {
                g_zhr_mesh_smoothing_enabled.store(e[0] != '0', std::memory_order_relaxed);
            }
            return true;
        }();
        (void)s_mesh_env_init;
        const bool is_stencil_only_pass =
            (m_render_states[D3DRS_COLORWRITEENABLE] & 0x7u) == 0 ||
            (m_render_states[D3DRS_ZWRITEENABLE] == 0) ||
            (m_render_states[D3DRS_STENCILENABLE] != 0 &&
             m_render_states[D3DRS_STENCILFUNC] == D3DCMP_NEVER);
        const bool terrain_pass_active = g_zhr_terrain_pass_active.load(std::memory_order_relaxed);
        const bool can_smooth_terrain =
            g_zhr_mesh_smoothing_enabled.load(std::memory_order_relaxed) &&
            remaster_active &&
            terrain_pass_active &&
            g_zhr_terrain_hm.data != nullptr &&
            g_zhr_terrain_hm.width >= 4 &&
            g_zhr_terrain_hm.height >= 4 &&
            !is_rhw &&
            prim_type == D3DPT_TRIANGLELIST &&
            prim_count >= 2u &&
            prim_count <= 4096u &&
            m_scratch_verts.size() <= 16384u;

        // Only tessellate static rigid .W3D meshes (buildings, vents, vehicles, rocks, props);
        // skip dynamic skinned buffers (D3DUSAGE_DYNAMIC) whose vertices mutate every frame.
        const bool can_smooth_mesh =
            !terrain_pass_active &&
            !is_dynamic_vb &&
            g_zhr_mesh_smoothing_enabled.load(std::memory_order_relaxed) &&
            remaster_active &&
            !is_rhw &&
            has_normal &&
            (m_render_states[D3DRS_LIGHTING] != 0) &&
            !is_stencil_only_pass &&
            (m_render_states[D3DRS_ALPHABLENDENABLE] == 0) &&
            prim_type == D3DPT_TRIANGLELIST &&
            prim_count >= 6u &&
            prim_count <= 4000u &&
            m_scratch_verts.size() < 16000u;

        ZhrVkTexture8* diffuse_tex =
            (m_textures[0] != nullptr && m_textures[0]->GetType() == D3DRTYPE_TEXTURE)
                ? static_cast<ZhrVkTexture8*>(m_textures[0])
                : nullptr;

        // Static VB/IB cache miss path: tessellate once (if eligible), upload into persistent
        // GPU vertex & index buffers, and draw directly via `zhr_vk_draw_indexed` forever after.
        if (static_key != nullptr)
        {
            const ZhrVertexWw3d* upload_verts = m_scratch_verts.data();
            size_t upload_vcount = m_scratch_verts.size();
            const uint16_t* upload_indices = m_scratch_indices.data();
            size_t upload_icount = m_scratch_indices.size();

            if (can_smooth_terrain)
            {
                const auto t0 = m_perf_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const bool ok = ApplySmartTerrainTessellation();
                if (m_perf_enabled)
                {
                    m_perf_tess_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                }
                if (ok)
                {
                    upload_verts = m_subdiv_verts.data();
                    upload_vcount = m_subdiv_verts.size();
                    upload_indices = m_subdiv_indices.data();
                    upload_icount = m_subdiv_indices.size();
                }
            }
            else if (can_smooth_mesh)
            {
                const auto t0 = m_perf_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const bool ok = ApplySmartAdaptiveTessellation(diffuse_tex);
                if (m_perf_enabled)
                {
                    m_perf_tess_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                }
                if (ok)
                {
                    upload_verts = m_subdiv_verts.data();
                    upload_vcount = m_subdiv_verts.size();
                    upload_indices = m_subdiv_indices.data();
                    upload_icount = m_subdiv_indices.size();
                }
            }

            if (m_perf_enabled)
            {
                ++m_perf_static_misses;
            }

            if (m_static_gpu_cache.size() >= 2048u)
            {
                EvictStaticGpuCacheLru(256u);
            }

            const uint32_t gpu_vb = zhr_vk_create_vertex_buffer(upload_verts, upload_vcount);
            const uint32_t gpu_ib = zhr_vk_create_index_buffer(upload_indices, upload_icount);
            if (gpu_vb != 0u && gpu_ib != 0u)
            {
                CachedGpuMesh cached{};
                cached.gpu_vb = gpu_vb;
                cached.gpu_ib = gpu_ib;
                cached.index_count = static_cast<uint32_t>(upload_icount);
                cached.last_used_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
                m_static_gpu_cache.emplace(*static_key, cached);
                zhr_vk_draw_indexed(gpu_vb, gpu_ib, cached.index_count, 0u, 0);
                return D3D_OK;
            }
            if (gpu_vb != 0u) zhr_vk_destroy_buffer(gpu_vb);
            if (gpu_ib != 0u) zhr_vk_destroy_buffer(gpu_ib);

            zhr_vk_draw_dynamic(upload_verts, upload_vcount, upload_indices, upload_icount);
            return D3D_OK;
        }

        if (m_perf_enabled)
        {
            ++m_perf_dynamic_draws;
        }

        // Dynamic terrain draws (3-way extra blend tiles, roads, or momentarily muzzle-lit tiles):
        // keep a dedicated small LRU cache so they stay crack-free without ever evicting static GPU buffers.
        if (can_smooth_terrain)
        {
            const TessCacheKey cache_key = ComputeTessCacheKey(diffuse_tex, true);
            auto cache_it = m_tess_cache.find(cache_key);
            if (cache_it != m_tess_cache.end())
            {
                cache_it->second.last_used_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
                zhr_vk_draw_dynamic(
                    cache_it->second.verts.data(),
                    cache_it->second.verts.size(),
                    cache_it->second.indices.data(),
                    cache_it->second.indices.size());
                return D3D_OK;
            }

            const auto t0 = m_perf_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const bool ok = ApplySmartTerrainTessellation();
            if (m_perf_enabled)
            {
                m_perf_tess_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            }
            if (ok)
            {
                if (m_tess_cache.size() >= 256u)
                {
                    EvictDynamicTessCacheLru(64u);
                }
                CachedTessMesh entry;
                entry.verts = m_subdiv_verts;
                entry.indices = m_subdiv_indices;
                entry.last_used_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
                auto inserted = m_tess_cache.emplace(cache_key, std::move(entry));
                zhr_vk_draw_dynamic(
                    inserted.first->second.verts.data(),
                    inserted.first->second.verts.size(),
                    inserted.first->second.indices.data(),
                    inserted.first->second.indices.size());
                return D3D_OK;
            }
        }

        if (can_smooth_mesh)
        {
            const TessCacheKey cache_key = ComputeTessCacheKey(diffuse_tex, false);
            auto cache_it = m_tess_cache.find(cache_key);
            if (cache_it != m_tess_cache.end())
            {
                cache_it->second.last_used_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
                zhr_vk_draw_dynamic(
                    cache_it->second.verts.data(),
                    cache_it->second.verts.size(),
                    cache_it->second.indices.data(),
                    cache_it->second.indices.size());
                return D3D_OK;
            }

            const auto t0 = m_perf_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const bool ok = ApplySmartAdaptiveTessellation(diffuse_tex);
            if (m_perf_enabled)
            {
                m_perf_tess_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            }
            if (ok)
            {
                if (m_tess_cache.size() >= 256u)
                {
                    EvictDynamicTessCacheLru(64u);
                }
                CachedTessMesh entry;
                entry.verts = m_subdiv_verts;
                entry.indices = m_subdiv_indices;
                entry.last_used_frame = g_zhr_frame_counter.load(std::memory_order_relaxed);
                auto inserted = m_tess_cache.emplace(cache_key, std::move(entry));
                zhr_vk_draw_dynamic(
                    inserted.first->second.verts.data(),
                    inserted.first->second.verts.size(),
                    inserted.first->second.indices.data(),
                    inserted.first->second.indices.size());
                return D3D_OK;
            }
        }

        {
            if (is_rhw)
            {
                D3DMATRIX ident{};
                MakeIdentityMatrix(ident);
                zhr_vk_set_transform(0, &ident.m[0][0]);
                zhr_vk_set_transform(1, &ident.m[0][0]);
                zhr_vk_set_transform(2, &ident.m[0][0]);
            }

            zhr_vk_draw_dynamic(
                m_scratch_verts.data(),
                m_scratch_verts.size(),
                m_scratch_indices.data(),
                m_scratch_indices.size());

            if (is_rhw)
            {
                zhr_vk_set_transform(0, &m_world.m[0][0]);
                zhr_vk_set_transform(1, &m_view.m[0][0]);
                zhr_vk_set_transform(2, &m_proj.m[0][0]);
            }
        }

        return D3D_OK;
    }

    // Smart Crack-Free Adaptive Terrain Tessellation (2-Level Red-Green + Fritsch-Carlson Monotone Bicubic):
    // - Evaluates a globally continuous C1 monotone bicubic heightfield & smooth analytic surface normals
    //   directly from WorldHeightMap across all VB tile boundaries, 3-way blends, cloud passes, shroud, and roads.
    // - Flat buildable plains (slope == 0) remain 100% flat with zero extra triangles and zero foundation clipping;
    //   hills, cliffs, shorelines, craters, and embankments adaptively refine from 10u -> 5u -> 2.5u with 0 T-junctions.
    bool ApplySmartTerrainTessellation()
    {
        const ZhrTerrainHeightmapState hm = g_zhr_terrain_hm;
        if (hm.data == nullptr || hm.width < 4 || hm.height < 4)
        {
            return false;
        }
        const size_t in_tris = m_scratch_indices.size() / 3;
        const size_t in_verts = m_scratch_verts.size();
        if (in_tris == 0 || in_verts == 0 || in_verts >= 60000)
        {
            return false;
        }

        m_subdiv_verts = m_scratch_verts;
        m_subdiv_indices = m_scratch_indices;

        // Per-vertex elevation bias above linear terrain (0.0 for base/blend/shroud, ~+0.15 for roads).
        // Also track whether any vertex has non-flat curvature/slope so 100% flat buildable plains
        // exit immediately with zero Red-Green subdivision overhead.
        std::vector<float> v_bias(in_verts, 0.0f);
        v_bias.reserve(in_verts * 3);
        bool any_non_flat = false;
        float first_z = 0.0f;
        for (size_t i = 0; i < in_verts; ++i)
        {
            ZhrVertexWw3d& v = m_subdiv_verts[i];
            const float lin_z = SampleTerrainLinearZ(hm, v.position[0], v.position[1]);
            float bias = v.position[2] - lin_z;
            if (std::fabs(bias) < 0.02f) bias = 0.0f;
            else bias = std::clamp(bias, -2.0f, 4.0f);
            v_bias[i] = bias;

            float smooth_z = v.position[2];
            SampleTerrainSmooth(hm, v.position[0], v.position[1], smooth_z, v.normal);
            v.position[2] = smooth_z + bias;
            if (i == 0)
            {
                first_z = smooth_z;
            }
            else if (std::fabs(smooth_z - first_z) > 0.01f)
            {
                any_non_flat = true;
            }
            if (v.normal[2] < 0.9995f)
            {
                any_non_flat = true;
            }
        }

        if (!any_non_flat)
        {
            return true;
        }

        auto run_red_green_pass = [&](float min_dist_sq, float curv_thresh, float norm_thresh, float slope_thresh) {
            const size_t cur_tris = m_subdiv_indices.size() / 3;
            if (cur_tris == 0 || m_subdiv_verts.size() >= 56000)
            {
                return;
            }
            std::vector<uint16_t> out_indices;
            out_indices.reserve(m_subdiv_indices.size() * 2);
            m_terrain_edge_state.clear();
            m_terrain_edge_state.reserve(cur_tris * 2);

            // Unified single-evaluation edge test + midpoint cache:
            // - Every edge is tested AT MOST ONCE (even when shared by 2 triangles).
            // - SampleTerrainSmooth() is evaluated AT MOST ONCE per edge (reused directly for the midpoint).
            // Returns midpoint vertex index >= 0 if split, or -1 if edge stays unsplit.
            auto eval_edge = [&](uint16_t ia, uint16_t ib) -> int32_t {
                const uint32_t lo = ia < ib ? ia : ib;
                const uint32_t hi = ia < ib ? ib : ia;
                const uint32_t key = (lo << 16) | hi;
                auto it = m_terrain_edge_state.find(key);
                if (it != m_terrain_edge_state.end())
                {
                    return it->second;
                }
                const ZhrVertexWw3d& va = m_subdiv_verts[ia];
                const ZhrVertexWw3d& vb = m_subdiv_verts[ib];
                const float dx = vb.position[0] - va.position[0];
                const float dy = vb.position[1] - va.position[1];
                const float dist_sq = dx * dx + dy * dy;
                if (dist_sq < min_dist_sq || dist_sq > 2500.0f)
                {
                    m_terrain_edge_state.emplace(key, -1);
                    return -1;
                }
                const float za = va.position[2] - v_bias[ia];
                const float zb = vb.position[2] - v_bias[ib];
                bool split = std::fabs(za - zb) > slope_thresh;
                if (!split)
                {
                    const float dot_n = va.normal[0] * vb.normal[0] +
                                        va.normal[1] * vb.normal[1] +
                                        va.normal[2] * vb.normal[2];
                    split = (1.0f - dot_n) > norm_thresh;
                }
                const float mx = 0.5f * (va.position[0] + vb.position[0]);
                const float my = 0.5f * (va.position[1] + vb.position[1]);
                float zm = 0.0f;
                float nm[3];
                SampleTerrainSmooth(hm, mx, my, zm, nm);
                if (!split)
                {
                    split = std::fabs(zm - 0.5f * (za + zb)) > curv_thresh;
                }
                if (!split || m_subdiv_verts.size() >= 62000)
                {
                    m_terrain_edge_state.emplace(key, -1);
                    return -1;
                }

                ZhrVertexWw3d mid{};
                mid.position[0] = mx;
                mid.position[1] = my;
                const float mid_bias = 0.5f * (v_bias[ia] + v_bias[ib]);
                mid.position[2] = zm + mid_bias;
                mid.normal[0] = nm[0];
                mid.normal[1] = nm[1];
                mid.normal[2] = nm[2];
                for (int c = 0; c < 4; ++c) mid.color[c] = 0.5f * (va.color[c] + vb.color[c]);
                mid.uv0[0] = 0.5f * (va.uv0[0] + vb.uv0[0]);
                mid.uv0[1] = 0.5f * (va.uv0[1] + vb.uv0[1]);
                mid.uv1[0] = 0.5f * (va.uv1[0] + vb.uv1[0]);
                mid.uv1[1] = 0.5f * (va.uv1[1] + vb.uv1[1]);

                const int32_t new_idx = static_cast<int32_t>(m_subdiv_verts.size());
                m_subdiv_verts.push_back(mid);
                v_bias.push_back(mid_bias);
                m_terrain_edge_state.emplace(key, new_idx);
                return new_idx;
            };

            for (size_t t = 0; t < cur_tris; ++t)
            {
                const uint16_t i0 = m_subdiv_indices[t * 3 + 0];
                const uint16_t i1 = m_subdiv_indices[t * 3 + 1];
                const uint16_t i2 = m_subdiv_indices[t * 3 + 2];
                if (i0 >= m_subdiv_verts.size() || i1 >= m_subdiv_verts.size() || i2 >= m_subdiv_verts.size())
                {
                    continue;
                }
                const int32_t e01 = eval_edge(i0, i1);
                const int32_t e12 = eval_edge(i1, i2);
                const int32_t e20 = eval_edge(i2, i0);
                const int mask = (e01 >= 0 ? 1 : 0) | (e12 >= 0 ? 2 : 0) | (e20 >= 0 ? 4 : 0);

                if (mask == 0)
                {
                    out_indices.push_back(i0); out_indices.push_back(i1); out_indices.push_back(i2);
                }
                else if (mask == 7)
                {
                    const uint16_t m01 = static_cast<uint16_t>(e01);
                    const uint16_t m12 = static_cast<uint16_t>(e12);
                    const uint16_t m20 = static_cast<uint16_t>(e20);
                    out_indices.push_back(i0);  out_indices.push_back(m01); out_indices.push_back(m20);
                    out_indices.push_back(m01); out_indices.push_back(i1);  out_indices.push_back(m12);
                    out_indices.push_back(m20); out_indices.push_back(m12); out_indices.push_back(i2);
                    out_indices.push_back(m01); out_indices.push_back(m12); out_indices.push_back(m20);
                }
                else if (mask == 1 || mask == 2 || mask == 4)
                {
                    uint16_t a = i0, b = i1, c = i2;
                    uint16_t mab = static_cast<uint16_t>(e01);
                    if (mask == 2) { a = i1; b = i2; c = i0; mab = static_cast<uint16_t>(e12); }
                    else if (mask == 4) { a = i2; b = i0; c = i1; mab = static_cast<uint16_t>(e20); }
                    out_indices.push_back(a);   out_indices.push_back(mab); out_indices.push_back(c);
                    out_indices.push_back(mab); out_indices.push_back(b);   out_indices.push_back(c);
                }
                else
                {
                    uint16_t a = i0, b = i1, c = i2;
                    uint16_t mab = static_cast<uint16_t>(e01);
                    uint16_t mca = static_cast<uint16_t>(e20);
                    if (mask == 3) { a = i1; b = i2; c = i0; mab = static_cast<uint16_t>(e12); mca = static_cast<uint16_t>(e01); }
                    else if (mask == 6) { a = i2; b = i0; c = i1; mab = static_cast<uint16_t>(e20); mca = static_cast<uint16_t>(e12); }
                    out_indices.push_back(a);   out_indices.push_back(mab); out_indices.push_back(mca);
                    out_indices.push_back(mab); out_indices.push_back(b);   out_indices.push_back(c);
                    out_indices.push_back(mab); out_indices.push_back(c);   out_indices.push_back(mca);
                }
            }
            m_subdiv_indices.swap(out_indices);
        };

        // Pass 1: 10u -> 5u on curved hills, shorelines, craters, and steep slopes (flat plains untouched!).
        run_red_green_pass(16.0f, 0.065f, 0.008f, 2.6f);
        // Pass 2: 5u -> 2.5u strictly on sharp cliff lips, ridges, craters, and steep embankments.
        run_red_green_pass(9.0f, 0.13f, 0.022f, 6.5f);

        return true;
    }

    // Smart Adaptive Crack-Free Tessellation + Polygonal Cylinder Rounding + Texture Heightmap Displacement:
    // 0. Pass 0 (Spatial Dihedral Cylinder/Vent Detection): Detects 6-sided hexagonal vents (60 deg), 8-sided
    //            pipes/ducts (45 deg), barrel roofs, wheels, and domes across split UV/normal seams while strictly
    //            ignoring 90-degree architectural walls/boxes (dot < 0.38). Computes welded radial curve normals
    //            and smooths curved side-wall shading while keeping 90-degree flat caps razor-crisp.
    // 1. Pass 1: Crease-preserving Phong + exact circularization (6->12 sides) with crack-free cap-rim lockstep.
    // 2. Pass 2 (Adaptive Red-Green 1->2/1->3/1->4): Refines polygonal cylinders/vents/pipes a second level
    //            (12->24+ sides) plus large curved/high-relief triangles with zero T-junctions or cap cracks.
    // 3. Pass 3 (Crack-Free Heightmap Displacement + Smooth Displaced Normals): Samples the texture's multi-scale
    //            heightmap (`zhr-remaster`) along welded smooth normals, fading displacement cleanly to 0 at open
    //            mesh boundaries, hard creases, and slender antennas/struts.
    bool ApplySmartAdaptiveTessellation(ZhrVkTexture8* diffuse_tex)
    {
        const size_t tri_count = m_scratch_indices.size() / 3;
        if (tri_count == 0 || m_scratch_verts.size() + tri_count * 3 > 60000)
        {
            return false;
        }

        const bool has_hm = (diffuse_tex != nullptr) &&
                            diffuse_tex->EnsureHeightmap() &&
                            (diffuse_tex->HeightVariance() > 0.025f);

        auto quant_pos_key = [](const float p[3]) -> uint64_t {
            // 0.02 world-unit spatial welding tolerance (catches UV & hard-normal seam duplicates cleanly)
            const int64_t qx = static_cast<int64_t>(std::llround(p[0] * 50.0f)) & 0x1FFFFFLL;
            const int64_t qy = static_cast<int64_t>(std::llround(p[1] * 50.0f)) & 0x1FFFFFLL;
            const int64_t qz = static_cast<int64_t>(std::llround(p[2] * 50.0f)) & 0x1FFFFFLL;
            return (static_cast<uint64_t>(qx) << 42) | (static_cast<uint64_t>(qy) << 21) | static_cast<uint64_t>(qz);
        };

        struct SpatialEdgeHash
        {
            size_t operator()(const std::pair<uint64_t, uint64_t>& e) const noexcept
            {
                return std::hash<uint64_t>{}(e.first ^ (e.second * 0x9E3779B97F4A7C15ULL));
            }
        };
        auto make_spatial_edge = [](uint64_t a, uint64_t b) -> std::pair<uint64_t, uint64_t> {
            return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
        };

        struct Vec3f
        {
            float x{0.0f}, y{0.0f}, z{0.0f};
        };

        // --- Pass 0: Spatial Dihedral Analysis for Polygonal Cylinders, Hexagonal Vents, Pipes & Barrel Roofs ---
        const size_t in_vert_count = m_scratch_verts.size();
        std::vector<uint64_t> in_v_keys(in_vert_count);
        for (size_t i = 0; i < in_vert_count; ++i)
        {
            in_v_keys[i] = quant_pos_key(m_scratch_verts[i].position);
        }

        struct EdgeIncidence
        {
            uint32_t t0{UINT32_MAX};
            uint32_t t1{UINT32_MAX};
            uint8_t count{0};
        };
        std::vector<Vec3f> face_normals(tri_count);
        std::vector<uint8_t> face_valid(tri_count, 0);
        std::unordered_map<std::pair<uint64_t, uint64_t>, EdgeIncidence, SpatialEdgeHash> in_edge_map;
        in_edge_map.reserve(tri_count * 2);

        for (size_t t = 0; t < tri_count; ++t)
        {
            const uint16_t i0 = m_scratch_indices[t * 3 + 0];
            const uint16_t i1 = m_scratch_indices[t * 3 + 1];
            const uint16_t i2 = m_scratch_indices[t * 3 + 2];
            if (i0 >= in_vert_count || i1 >= in_vert_count || i2 >= in_vert_count)
            {
                return false;
            }
            const ZhrVertexWw3d& v0 = m_scratch_verts[i0];
            const ZhrVertexWw3d& v1 = m_scratch_verts[i1];
            const ZhrVertexWw3d& v2 = m_scratch_verts[i2];

            const float e1x = v1.position[0] - v0.position[0];
            const float e1y = v1.position[1] - v0.position[1];
            const float e1z = v1.position[2] - v0.position[2];
            const float e2x = v2.position[0] - v0.position[0];
            const float e2y = v2.position[1] - v0.position[1];
            const float e2z = v2.position[2] - v0.position[2];
            float fnx = e1y * e2z - e1z * e2y;
            float fny = e1z * e2x - e1x * e2z;
            float fnz = e1x * e2y - e1y * e2x;
            const float flen = std::sqrt(fnx * fnx + fny * fny + fnz * fnz);
            if (flen > 1e-6f)
            {
                fnx /= flen;
                fny /= flen;
                fnz /= flen;
                // Align geometric face normal orientation with authored vertex normals.
                const float vnx = v0.normal[0] + v1.normal[0] + v2.normal[0];
                const float vny = v0.normal[1] + v1.normal[1] + v2.normal[1];
                const float vnz = v0.normal[2] + v1.normal[2] + v2.normal[2];
                if (fnx * vnx + fny * vny + fnz * vnz < 0.0f)
                {
                    fnx = -fnx;
                    fny = -fny;
                    fnz = -fnz;
                }
                face_normals[t] = {fnx, fny, fnz};
                face_valid[t] = 1;

                const uint64_t k0 = in_v_keys[i0];
                const uint64_t k1 = in_v_keys[i1];
                const uint64_t k2 = in_v_keys[i2];
                auto add_in_edge = [&](uint64_t ka, uint64_t kb) {
                    if (ka == kb) return;
                    EdgeIncidence& inc = in_edge_map[make_spatial_edge(ka, kb)];
                    if (inc.count == 0) inc.t0 = static_cast<uint32_t>(t);
                    else if (inc.count == 1) inc.t1 = static_cast<uint32_t>(t);
                    if (inc.count < 255) inc.count += 1;
                };
                add_in_edge(k0, k1);
                add_in_edge(k1, k2);
                add_in_edge(k2, k0);
            }
        }

        // Accumulate radial curve normals strictly across 7..78 degree polygonal creases
        // (catches 5-sided 72-deg pentagonal vents [dot=0.309], 6-sided 60-deg hexagons [0.50],
        // 8-sided 45-deg pipes [0.71], 12-gons [0.87], barrel roofs & arches,
        // while strictly rejecting 90-deg square walls/boxes [dot~0.0] and 4-sided pyramids [dot<0.18]).
        std::unordered_map<uint64_t, Vec3f> curve_normal_map;
        curve_normal_map.reserve(in_vert_count / 2 + 16);
        std::vector<uint8_t> curved_tri(tri_count, 0);

        for (const auto& kv : in_edge_map)
        {
            const EdgeIncidence& inc = kv.second;
            if (inc.count != 2)
            {
                continue;
            }
            const Vec3f& n0 = face_normals[inc.t0];
            const Vec3f& n1 = face_normals[inc.t1];
            const float d = n0.x * n1.x + n0.y * n1.y + n0.z * n1.z;
            if (d > 0.20f && d < 0.985f)
            {
                const float sx = n0.x + n1.x;
                const float sy = n0.y + n1.y;
                const float sz = n0.z + n1.z;
                Vec3f& ca = curve_normal_map[kv.first.first];
                ca.x += sx; ca.y += sy; ca.z += sz;
                Vec3f& cb = curve_normal_map[kv.first.second];
                cb.x += sx; cb.y += sy; cb.z += sz;
                curved_tri[inc.t0] = 1;
                curved_tri[inc.t1] = 1;
            }
        }

        for (auto it = curve_normal_map.begin(); it != curve_normal_map.end(); )
        {
            const float len = std::sqrt(it->second.x * it->second.x + it->second.y * it->second.y + it->second.z * it->second.z);
            if (len > 0.01f)
            {
                it->second.x /= len;
                it->second.y /= len;
                it->second.z /= len;
                ++it;
            }
            else
            {
                it = curve_normal_map.erase(it);
            }
        }

        // Identify true curved-surface ring edges (excludes flat wall edges touching a single 45-deg bevel
        // and excludes interior chords of flat 90-deg cylinder caps).
        std::unordered_set<std::pair<uint64_t, uint64_t>, SpatialEdgeHash> p0_curved_edges;
        p0_curved_edges.reserve(tri_count);
        std::vector<uint8_t> active_curve_tri(tri_count, 0);
        std::vector<uint8_t> vert_in_active_curve(in_vert_count, 0);

        for (size_t t = 0; t < tri_count; ++t)
        {
            if (!curved_tri[t])
            {
                continue;
            }
            const uint16_t idx[3] = {
                m_scratch_indices[t * 3 + 0],
                m_scratch_indices[t * 3 + 1],
                m_scratch_indices[t * 3 + 2]
            };
            bool tri_has_curve_arc = false;
            for (int e = 0; e < 3; ++e)
            {
                const uint64_t ka = in_v_keys[idx[e]];
                const uint64_t kb = in_v_keys[idx[(e + 1) % 3]];
                if (ka == kb) continue;
                auto ita = curve_normal_map.find(ka);
                auto itb = curve_normal_map.find(kb);
                if (ita != curve_normal_map.end() && itb != curve_normal_map.end())
                {
                    const float dot_c = ita->second.x * itb->second.x +
                                        ita->second.y * itb->second.y +
                                        ita->second.z * itb->second.z;
                    if (dot_c > 0.18f && dot_c < 0.992f)
                    {
                        p0_curved_edges.insert(make_spatial_edge(ka, kb));
                        tri_has_curve_arc = true;
                    }
                }
            }
            if (tri_has_curve_arc)
            {
                active_curve_tri[t] = 1;
                vert_in_active_curve[idx[0]] = 1;
                vert_in_active_curve[idx[1]] = 1;
                vert_in_active_curve[idx[2]] = 1;
            }
        }

        // Carry per-vertex radial curve normals across Pass 1 -> Pass 2 so both the curved side wall
        // AND the flat 90-degree cap rim share the exact same radial projection without softening cap shading.
        m_subdiv_verts = m_scratch_verts;
        std::vector<Vec3f> subdiv_curve_n(in_vert_count);
        std::vector<uint8_t> subdiv_has_curve(in_vert_count, 0);
        for (size_t i = 0; i < in_vert_count; ++i)
        {
            auto it = curve_normal_map.find(in_v_keys[i]);
            if (it != curve_normal_map.end())
            {
                subdiv_curve_n[i] = it->second;
                subdiv_has_curve[i] = 1;
                // Smooth shading normals strictly on curved side-wall faces (eliminates flat hexagonal facet seams)
                // while leaving flat 90-deg cap vertices (|dot| <= 0.35) completely untouched.
                if (vert_in_active_curve[i])
                {
                    ZhrVertexWw3d& v = m_subdiv_verts[i];
                    const float align = v.normal[0] * it->second.x +
                                        v.normal[1] * it->second.y +
                                        v.normal[2] * it->second.z;
                    if (std::fabs(align) > 0.35f)
                    {
                        const float s = align >= 0.0f ? 1.0f : -1.0f;
                        v.normal[0] = it->second.x * s;
                        v.normal[1] = it->second.y * s;
                        v.normal[2] = it->second.z * s;
                    }
                }
            }
        }

        auto project_circular_midpoint = [](const float pa_pos[3], const float pb_pos[3],
                                            const float an_x, const float an_y, const float an_z,
                                            const float bn_x, const float bn_y, const float bn_z,
                                            float dot_n, float alpha, float out_pos[3]) {
            const float mx = 0.5f * (pa_pos[0] + pb_pos[0]);
            const float my = 0.5f * (pa_pos[1] + pb_pos[1]);
            const float mz = 0.5f * (pa_pos[2] + pb_pos[2]);

            const float dx = pb_pos[0] - pa_pos[0];
            const float dy = pb_pos[1] - pa_pos[1];
            const float dz = pb_pos[2] - pa_pos[2];
            const float dist_sq = dx * dx + dy * dy + dz * dz;

            const float da = (mx - pa_pos[0]) * an_x +
                             (my - pa_pos[1]) * an_y +
                             (mz - pa_pos[2]) * an_z;
            const float db = (mx - pb_pos[0]) * bn_x +
                             (my - pb_pos[1]) * bn_y +
                             (mz - pb_pos[2]) * bn_z;
            const float pax = mx - da * an_x;
            const float pay = my - da * an_y;
            const float paz = mz - da * an_z;
            const float pbx = mx - db * bn_x;
            const float pby = my - db * bn_y;
            const float pbz = mz - db * bn_z;

            float ox = alpha * (0.5f * (pax + pbx) - mx);
            float oy = alpha * (0.5f * (pay + pby) - my);
            float oz = alpha * (0.5f * (paz + pbz) - mz);

            // Safety clamp: regular pentagon exact circle sagitta is 0.163*edge_len; cap at 0.24*edge_len.
            const float off_sq = ox * ox + oy * oy + oz * oz;
            const float max_off = 0.24f * std::sqrt(dist_sq);
            if (off_sq > max_off * max_off && off_sq > 1e-12f)
            {
                const float scale = max_off / std::sqrt(off_sq);
                ox *= scale;
                oy *= scale;
                oz *= scale;
            }
            (void)dot_n;
            out_pos[0] = mx + ox;
            out_pos[1] = my + oy;
            out_pos[2] = mz + oz;
        };

        auto make_phong_midpoint = [&](const ZhrVertexWw3d& va, const ZhrVertexWw3d& vb,
                                       bool use_curve_ring, const Vec3f& ca, const Vec3f& cb,
                                       float fallback_alpha, Vec3f& out_curve_n, uint8_t& out_has_curve) -> ZhrVertexWw3d {
            ZhrVertexWw3d mid{};
            mid.position[0] = 0.5f * (va.position[0] + vb.position[0]);
            mid.position[1] = 0.5f * (va.position[1] + vb.position[1]);
            mid.position[2] = 0.5f * (va.position[2] + vb.position[2]);
            out_curve_n = {0.0f, 0.0f, 0.0f};
            out_has_curve = 0;

            const float dx = vb.position[0] - va.position[0];
            const float dy = vb.position[1] - va.position[1];
            const float dz = vb.position[2] - va.position[2];
            const float dist_sq = dx * dx + dy * dy + dz * dz;

            if (use_curve_ring && dist_sq > 1e-4f && dist_sq < 40000.0f)
            {
                const float dot_c = ca.x * cb.x + ca.y * cb.y + ca.z * cb.z;
                if (dot_c > 0.18f && dot_c < 0.995f)
                {
                    // Exact analytic circle sagitta factor: 1 / (cos(theta/2) * (1 + cos(theta/2)))
                    // 72-deg pentagon -> 0.683 (exact 10-gon -> 20-gon), 60-deg hexagon -> 0.619 (12 -> 24), 45-deg -> 0.563.
                    const float cos_half = std::sqrt(std::max(0.5f * (1.0f + dot_c), 0.25f));
                    const float exact_alpha = std::clamp(1.0f / (cos_half * (1.0f + cos_half)), 0.48f, 0.72f);
                    project_circular_midpoint(va.position, vb.position,
                                              ca.x, ca.y, ca.z, cb.x, cb.y, cb.z,
                                              dot_c, exact_alpha, mid.position);
                }
                const float cx = ca.x + cb.x;
                const float cy = ca.y + cb.y;
                const float cz = ca.z + cb.z;
                const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
                if (clen > 1e-4f)
                {
                    out_curve_n = {cx / clen, cy / clen, cz / clen};
                    out_has_curve = 1;
                }
            }
            else if (dist_sq > 1e-4f && dist_sq < 40000.0f)
            {
                const float nax = va.normal[0], nay = va.normal[1], naz = va.normal[2];
                const float nbx = vb.normal[0], nby = vb.normal[1], nbz = vb.normal[2];
                const float na_len = std::sqrt(nax * nax + nay * nay + naz * naz);
                const float nb_len = std::sqrt(nbx * nbx + nby * nby + nbz * nbz);
                if (na_len > 0.001f && nb_len > 0.001f)
                {
                    const float an_x = nax / na_len, an_y = nay / na_len, an_z = naz / na_len;
                    const float bn_x = nbx / nb_len, bn_y = nby / nb_len, bn_z = nbz / nb_len;
                    const float dot_n = an_x * bn_x + an_y * bn_y + an_z * bn_z;
                    // Smooth curved organic edge (< ~44 deg) -> Phong tangent-plane projection; hard crease -> keep flat!
                    if (dot_n > 0.72f && dot_n < 0.9995f)
                    {
                        project_circular_midpoint(va.position, vb.position,
                                                  an_x, an_y, an_z, bn_x, bn_y, bn_z,
                                                  dot_n, fallback_alpha, mid.position);
                    }
                }
            }

            const float nx = va.normal[0] + vb.normal[0];
            const float ny = va.normal[1] + vb.normal[1];
            const float nz = va.normal[2] + vb.normal[2];
            const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nlen > 0.0001f)
            {
                mid.normal[0] = nx / nlen;
                mid.normal[1] = ny / nlen;
                mid.normal[2] = nz / nlen;
            }
            else
            {
                mid.normal[0] = va.normal[0];
                mid.normal[1] = va.normal[1];
                mid.normal[2] = va.normal[2];
            }

            for (int c = 0; c < 4; ++c) mid.color[c] = 0.5f * (va.color[c] + vb.color[c]);
            mid.uv0[0] = 0.5f * (va.uv0[0] + vb.uv0[0]);
            mid.uv0[1] = 0.5f * (va.uv0[1] + vb.uv0[1]);
            mid.uv1[0] = 0.5f * (va.uv1[0] + vb.uv1[0]);
            mid.uv1[1] = 0.5f * (va.uv1[1] + vb.uv1[1]);
            return mid;
        };

        // --- Pass 1: Uniform 1->4 Crease-Preserving Phong + Exact Cylinder/Vent Circularization ---
        m_subdiv_indices.clear();
        m_subdiv_indices.reserve(tri_count * 12);

        m_edge_midpoints.clear();
        m_edge_midpoints.reserve(tri_count * 2);

        std::vector<uint64_t> subdiv_keys = in_v_keys;
        subdiv_keys.reserve(in_vert_count + tri_count * 3);
        subdiv_curve_n.reserve(in_vert_count + tri_count * 3);
        subdiv_has_curve.reserve(in_vert_count + tri_count * 3);

        std::unordered_set<std::pair<uint64_t, uint64_t>, SpatialEdgeHash> p1_curved_edges;
        p1_curved_edges.reserve(p0_curved_edges.size() * 4 + 32);

        auto get_midpoint_p1 = [&](uint16_t ia, uint16_t ib) -> uint16_t {
            const uint32_t lo = ia < ib ? ia : ib;
            const uint32_t hi = ia < ib ? ib : ia;
            const uint32_t key = (lo << 16) | hi;
            auto it = m_edge_midpoints.find(key);
            if (it != m_edge_midpoints.end())
            {
                return it->second;
            }
            const uint64_t ka = subdiv_keys[ia];
            const uint64_t kb = subdiv_keys[ib];
            const bool is_ring_edge = (ka != kb) &&
                                      subdiv_has_curve[ia] && subdiv_has_curve[ib] &&
                                      (p0_curved_edges.find(make_spatial_edge(ka, kb)) != p0_curved_edges.end());

            Vec3f mid_cn{};
            uint8_t mid_hc = 0;
            ZhrVertexWw3d mid = make_phong_midpoint(
                m_subdiv_verts[ia], m_subdiv_verts[ib],
                is_ring_edge, subdiv_curve_n[ia], subdiv_curve_n[ib],
                0.44f, mid_cn, mid_hc);

            // Preserve radial curve normal along straight axial cylinder edges so mid-height rings also refine in Pass 2.
            if (!mid_hc && subdiv_has_curve[ia] && subdiv_has_curve[ib])
            {
                const float dot_c = subdiv_curve_n[ia].x * subdiv_curve_n[ib].x +
                                    subdiv_curve_n[ia].y * subdiv_curve_n[ib].y +
                                    subdiv_curve_n[ia].z * subdiv_curve_n[ib].z;
                if (dot_c >= 0.985f)
                {
                    const float cx = subdiv_curve_n[ia].x + subdiv_curve_n[ib].x;
                    const float cy = subdiv_curve_n[ia].y + subdiv_curve_n[ib].y;
                    const float cz = subdiv_curve_n[ia].z + subdiv_curve_n[ib].z;
                    const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
                    if (clen > 1e-4f)
                    {
                        mid_cn = {cx / clen, cy / clen, cz / clen};
                        mid_hc = 1;
                    }
                }
            }

            const uint16_t new_idx = static_cast<uint16_t>(m_subdiv_verts.size());
            m_subdiv_verts.push_back(mid);
            const uint64_t km = quant_pos_key(mid.position);
            subdiv_keys.push_back(km);
            subdiv_curve_n.push_back(mid_cn);
            subdiv_has_curve.push_back(mid_hc);
            m_edge_midpoints.emplace(key, new_idx);

            if (is_ring_edge)
            {
                if (ka != km) p1_curved_edges.insert(make_spatial_edge(ka, km));
                if (km != kb) p1_curved_edges.insert(make_spatial_edge(km, kb));
            }
            return new_idx;
        };

        auto register_p1_tri_curved_edges = [&](uint16_t a, uint16_t b, uint16_t c) {
            const uint16_t idx[3] = {a, b, c};
            for (int e = 0; e < 3; ++e)
            {
                const uint16_t u = idx[e];
                const uint16_t v = idx[(e + 1) % 3];
                if (!subdiv_has_curve[u] || !subdiv_has_curve[v]) continue;
                const uint64_t ku = subdiv_keys[u];
                const uint64_t kv = subdiv_keys[v];
                if (ku == kv) continue;
                const float dot_c = subdiv_curve_n[u].x * subdiv_curve_n[v].x +
                                    subdiv_curve_n[u].y * subdiv_curve_n[v].y +
                                    subdiv_curve_n[u].z * subdiv_curve_n[v].z;
                if (dot_c > 0.18f && dot_c < 0.992f)
                {
                    p1_curved_edges.insert(make_spatial_edge(ku, kv));
                }
            }
        };

        for (size_t t = 0; t < tri_count; ++t)
        {
            const uint16_t i0 = m_scratch_indices[t * 3 + 0];
            const uint16_t i1 = m_scratch_indices[t * 3 + 1];
            const uint16_t i2 = m_scratch_indices[t * 3 + 2];
            const uint16_t m01 = get_midpoint_p1(i0, i1);
            const uint16_t m12 = get_midpoint_p1(i1, i2);
            const uint16_t m20 = get_midpoint_p1(i2, i0);

            m_subdiv_indices.push_back(i0);  m_subdiv_indices.push_back(m01); m_subdiv_indices.push_back(m20);
            m_subdiv_indices.push_back(m01); m_subdiv_indices.push_back(i1);  m_subdiv_indices.push_back(m12);
            m_subdiv_indices.push_back(m20); m_subdiv_indices.push_back(m12); m_subdiv_indices.push_back(i2);
            m_subdiv_indices.push_back(m01); m_subdiv_indices.push_back(m12); m_subdiv_indices.push_back(m20);

            if (active_curve_tri[t])
            {
                register_p1_tri_curved_edges(i0, m01, m20);
                register_p1_tri_curved_edges(m01, i1, m12);
                register_p1_tri_curved_edges(m20, m12, i2);
                register_p1_tri_curved_edges(m01, m12, m20);
            }
        }

        // --- Pass 2: Smart Adaptive Red-Green Refinement (Level 2 on polygonal vents/pipes & large curved/relief edges) ---
        const size_t p1_tris = m_subdiv_indices.size() / 3;
        if (p1_tris <= 12000 && m_subdiv_verts.size() < 56000)
        {
            std::vector<uint16_t> p2_indices;
            p2_indices.reserve(m_subdiv_indices.size() * 2);
            m_edge_midpoints.clear();

            auto is_p1_curved_ring = [&](uint16_t ia, uint16_t ib) -> bool {
                if (!subdiv_has_curve[ia] || !subdiv_has_curve[ib]) return false;
                const uint64_t ka = subdiv_keys[ia];
                const uint64_t kb = subdiv_keys[ib];
                return (ka != kb) && (p1_curved_edges.find(make_spatial_edge(ka, kb)) != p1_curved_edges.end());
            };

            auto should_split_edge = [&](uint16_t ia, uint16_t ib) -> bool {
                const ZhrVertexWw3d& va = m_subdiv_verts[ia];
                const ZhrVertexWw3d& vb = m_subdiv_verts[ib];
                const float dx = vb.position[0] - va.position[0];
                const float dy = vb.position[1] - va.position[1];
                const float dz = vb.position[2] - va.position[2];
                const float dist_sq = dx * dx + dy * dy + dz * dz;
                if (dist_sq > 40000.0f)
                {
                    return false;
                }
                // Compact polygonal cylinders/vents/pipes/wheels (edge >= 0.5 world units -> 12->24+ sides!)
                if (dist_sq >= 0.25f && is_p1_curved_ring(ia, ib))
                {
                    return true;
                }
                // Broad organic/terrain/hull curvature & high-relief detail (> ~3.0 world units)
                if (dist_sq < 9.0f)
                {
                    return false;
                }
                const float dot_n = va.normal[0] * vb.normal[0] + va.normal[1] * vb.normal[1] + va.normal[2] * vb.normal[2];
                if (dot_n > 0.72f && dot_n < 0.992f)
                {
                    return true;
                }
                if (has_hm && dist_sq >= 14.0f)
                {
                    const float ha = diffuse_tex->SampleHeight(va.uv0[0], va.uv0[1]);
                    const float hb = diffuse_tex->SampleHeight(vb.uv0[0], vb.uv0[1]);
                    const float hm = diffuse_tex->SampleHeight(0.5f * (va.uv0[0] + vb.uv0[0]), 0.5f * (va.uv0[1] + vb.uv0[1]));
                    const float contrast = std::fabs(ha - hb) + std::fabs(hm - 0.5f * (ha + hb));
                    if (contrast > 0.22f)
                    {
                        return true;
                    }
                }
                return false;
            };

            auto get_midpoint_p2 = [&](uint16_t ia, uint16_t ib) -> uint16_t {
                const uint32_t lo = ia < ib ? ia : ib;
                const uint32_t hi = ia < ib ? ib : ia;
                const uint32_t key = (lo << 16) | hi;
                auto it = m_edge_midpoints.find(key);
                if (it != m_edge_midpoints.end())
                {
                    return it->second;
                }
                const bool ring = is_p1_curved_ring(ia, ib);
                Vec3f mid_cn{};
                uint8_t mid_hc = 0;
                ZhrVertexWw3d mid = make_phong_midpoint(
                    m_subdiv_verts[ia], m_subdiv_verts[ib],
                    ring, subdiv_curve_n[ia], subdiv_curve_n[ib],
                    0.34f, mid_cn, mid_hc);
                const uint16_t new_idx = static_cast<uint16_t>(m_subdiv_verts.size());
                m_subdiv_verts.push_back(mid);
                m_edge_midpoints.emplace(key, new_idx);
                return new_idx;
            };

            for (size_t t = 0; t < p1_tris; ++t)
            {
                const uint16_t i0 = m_subdiv_indices[t * 3 + 0];
                const uint16_t i1 = m_subdiv_indices[t * 3 + 1];
                const uint16_t i2 = m_subdiv_indices[t * 3 + 2];
                const bool s01 = should_split_edge(i0, i1);
                const bool s12 = should_split_edge(i1, i2);
                const bool s20 = should_split_edge(i2, i0);
                const int mask = (s01 ? 1 : 0) | (s12 ? 2 : 0) | (s20 ? 4 : 0);

                if (mask == 0 || m_subdiv_verts.size() + 3 >= 60000)
                {
                    p2_indices.push_back(i0); p2_indices.push_back(i1); p2_indices.push_back(i2);
                }
                else if (mask == 7)
                {
                    const uint16_t m01 = get_midpoint_p2(i0, i1);
                    const uint16_t m12 = get_midpoint_p2(i1, i2);
                    const uint16_t m20 = get_midpoint_p2(i2, i0);
                    p2_indices.push_back(i0);  p2_indices.push_back(m01); p2_indices.push_back(m20);
                    p2_indices.push_back(m01); p2_indices.push_back(i1);  p2_indices.push_back(m12);
                    p2_indices.push_back(m20); p2_indices.push_back(m12); p2_indices.push_back(i2);
                    p2_indices.push_back(m01); p2_indices.push_back(m12); p2_indices.push_back(m20);
                }
                else if (mask == 1 || mask == 2 || mask == 4)
                {
                    // Rotate so split edge is (a, b)
                    uint16_t a = i0, b = i1, c = i2;
                    if (mask == 2) { a = i1; b = i2; c = i0; }
                    else if (mask == 4) { a = i2; b = i0; c = i1; }
                    const uint16_t mab = get_midpoint_p2(a, b);
                    p2_indices.push_back(a);   p2_indices.push_back(mab); p2_indices.push_back(c);
                    p2_indices.push_back(mab); p2_indices.push_back(b);   p2_indices.push_back(c);
                }
                else
                {
                    // 2 edges split: rotate so unsplit edge is (b, c)
                    uint16_t a = i0, b = i1, c = i2;
                    if (mask == 3) { a = i1; b = i2; c = i0; }       // s01 & s12 -> unsplit (i2, i0)
                    else if (mask == 6) { a = i2; b = i0; c = i1; }  // s12 & s20 -> unsplit (i0, i1)
                    // mask == 5: s01 & s20 -> unsplit (i1, i2)
                    const uint16_t mab = get_midpoint_p2(a, b);
                    const uint16_t mca = get_midpoint_p2(c, a);
                    p2_indices.push_back(a);   p2_indices.push_back(mab); p2_indices.push_back(mca);
                    p2_indices.push_back(mab); p2_indices.push_back(b);   p2_indices.push_back(c);
                    p2_indices.push_back(mab); p2_indices.push_back(c);   p2_indices.push_back(mca);
                }
            }
            m_subdiv_indices.swap(p2_indices);
        }

        // --- Pass 3: Crack-Free Welded Texture Heightmap Displacement & Displaced Normal Re-evaluation ---
        if (has_hm && m_subdiv_verts.size() <= 24000)
        {
            struct WeldNode
            {
                float nx{0.0f}, ny{0.0f}, nz{0.0f};
                float h_sum{0.0f};
                float min_edge{1e9f};
                float max_edge{0.0f};
                uint32_t v_count{0};
                uint32_t e_count{0};
                bool boundary{false};
                bool crease{false};
                float dnx{0.0f}, dny{0.0f}, dnz{0.0f};
            };

            std::unordered_map<uint64_t, WeldNode> weld;
            weld.reserve(m_subdiv_verts.size());

            std::vector<uint64_t> v_keys(m_subdiv_verts.size());
            for (size_t i = 0; i < m_subdiv_verts.size(); ++i)
            {
                const ZhrVertexWw3d& v = m_subdiv_verts[i];
                const uint64_t k = quant_pos_key(v.position);
                v_keys[i] = k;
                WeldNode& node = weld[k];
                node.nx += v.normal[0];
                node.ny += v.normal[1];
                node.nz += v.normal[2];
                node.h_sum += diffuse_tex->SampleHeight(v.uv0[0], v.uv0[1]);
                node.v_count += 1u;
            }

            // Normalize welded normals and detect hard creases (90-deg walls/armor corners).
            for (auto& kv : weld)
            {
                WeldNode& node = kv.second;
                const float len = std::sqrt(node.nx * node.nx + node.ny * node.ny + node.nz * node.nz);
                if (len > 0.001f)
                {
                    node.nx /= len;
                    node.ny /= len;
                    node.nz /= len;
                }
                else
                {
                    node.crease = true;
                }
            }
            for (size_t i = 0; i < m_subdiv_verts.size(); ++i)
            {
                const ZhrVertexWw3d& v = m_subdiv_verts[i];
                WeldNode& node = weld[v_keys[i]];
                const float dot_n = v.normal[0] * node.nx + v.normal[1] * node.ny + v.normal[2] * node.nz;
                if (dot_n < 0.78f)
                {
                    node.crease = true;
                }
            }

            // Detect open mesh boundary edges in welded position space + measure local edge scale.
            struct SpatialEdgeHash
            {
                size_t operator()(const std::pair<uint64_t, uint64_t>& e) const noexcept
                {
                    return std::hash<uint64_t>{}(e.first ^ (e.second * 0x9E3779B97F4A7C15ULL));
                }
            };
            std::unordered_map<std::pair<uint64_t, uint64_t>, uint32_t, SpatialEdgeHash> edge_counts;
            const size_t final_tris = m_subdiv_indices.size() / 3;
            edge_counts.reserve(final_tris * 2);

            auto accum_edge = [&](uint16_t ia, uint16_t ib) {
                const uint64_t ka = v_keys[ia];
                const uint64_t kb = v_keys[ib];
                if (ka == kb) return;
                const auto pair = ka < kb ? std::make_pair(ka, kb) : std::make_pair(kb, ka);
                edge_counts[pair] += 1u;

                const ZhrVertexWw3d& va = m_subdiv_verts[ia];
                const ZhrVertexWw3d& vb = m_subdiv_verts[ib];
                const float dx = vb.position[0] - va.position[0];
                const float dy = vb.position[1] - va.position[1];
                const float dz = vb.position[2] - va.position[2];
                const float elen = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (elen > 0.001f && elen < 200.0f)
                {
                    WeldNode& na = weld[ka];
                    WeldNode& nb = weld[kb];
                    na.min_edge = std::min(na.min_edge, elen);
                    na.max_edge = std::max(na.max_edge, elen);
                    na.e_count += 1u;
                    nb.min_edge = std::min(nb.min_edge, elen);
                    nb.max_edge = std::max(nb.max_edge, elen);
                    nb.e_count += 1u;
                }
            };

            for (size_t t = 0; t < final_tris; ++t)
            {
                const uint16_t i0 = m_subdiv_indices[t * 3 + 0];
                const uint16_t i1 = m_subdiv_indices[t * 3 + 1];
                const uint16_t i2 = m_subdiv_indices[t * 3 + 2];
                accum_edge(i0, i1);
                accum_edge(i1, i2);
                accum_edge(i2, i0);
            }

            for (const auto& kv : edge_counts)
            {
                if (kv.second == 1u)
                {
                    weld[kv.first.first].boundary = true;
                    weld[kv.first.second].boundary = true;
                }
            }

            // Apply seam-safe welded heightmap displacement strictly to broad, smooth interior surfaces.
            // Using `min_edge` + aspect-ratio guard prevents thin masts, antennas, struts, and gun barrels
            // (which have long axial edges but tiny cross-sectional edges) from ever bloating outward.
            bool any_displaced = false;
            for (size_t i = 0; i < m_subdiv_verts.size(); ++i)
            {
                const WeldNode& node = weld[v_keys[i]];
                if (node.boundary || node.crease || node.v_count == 0 || node.e_count == 0)
                {
                    continue;
                }
                if (node.min_edge < 1.1f || node.max_edge > node.min_edge * 4.5f)
                {
                    continue;
                }
                const float h_avg = node.h_sum / static_cast<float>(node.v_count);
                const float disp_amp = std::min(node.min_edge * 0.16f, 0.85f);
                const float disp = h_avg * disp_amp;
                ZhrVertexWw3d& v = m_subdiv_verts[i];
                v.position[0] += node.nx * disp;
                v.position[1] += node.ny * disp;
                v.position[2] += node.nz * disp;
                any_displaced = true;
            }

            // Recompute area-weighted displaced normals so sunlight & GGX specular catch real 3D ridges.
            if (any_displaced)
            {
                for (size_t t = 0; t < final_tris; ++t)
                {
                    const uint16_t i0 = m_subdiv_indices[t * 3 + 0];
                    const uint16_t i1 = m_subdiv_indices[t * 3 + 1];
                    const uint16_t i2 = m_subdiv_indices[t * 3 + 2];
                    const float* p0 = m_subdiv_verts[i0].position;
                    const float* p1 = m_subdiv_verts[i1].position;
                    const float* p2 = m_subdiv_verts[i2].position;
                    const float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
                    const float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
                    const float fnx = e1y * e2z - e1z * e2y;
                    const float fny = e1z * e2x - e1x * e2z;
                    const float fnz = e1x * e2y - e1y * e2x;

                    for (uint16_t idx : {i0, i1, i2})
                    {
                        WeldNode& node = weld[v_keys[idx]];
                        // Keep winding orientation consistent with original welded normal
                        const float sign = (fnx * node.nx + fny * node.ny + fnz * node.nz) >= 0.0f ? 1.0f : -1.0f;
                        node.dnx += fnx * sign;
                        node.dny += fny * sign;
                        node.dnz += fnz * sign;
                    }
                }

                for (size_t i = 0; i < m_subdiv_verts.size(); ++i)
                {
                    const WeldNode& node = weld[v_keys[i]];
                    if (node.boundary || node.crease)
                    {
                        continue;
                    }
                    const float dlen = std::sqrt(node.dnx * node.dnx + node.dny * node.dny + node.dnz * node.dnz);
                    if (dlen > 1e-5f)
                    {
                        ZhrVertexWw3d& v = m_subdiv_verts[i];
                        const float bnx = v.normal[0] * 0.45f + (node.dnx / dlen) * 0.55f;
                        const float bny = v.normal[1] * 0.45f + (node.dny / dlen) * 0.55f;
                        const float bnz = v.normal[2] * 0.45f + (node.dnz / dlen) * 0.55f;
                        const float blen = std::sqrt(bnx * bnx + bny * bny + bnz * bnz);
                        if (blen > 1e-5f)
                        {
                            v.normal[0] = bnx / blen;
                            v.normal[1] = bny / blen;
                            v.normal[2] = bnz / blen;
                        }
                    }
                }
            }
        }

        return true;
    }

    void PollGraphicsHotkeys()
    {
        if (!m_hwnd || GetForegroundWindow() != m_hwnd)
        {
            return;
        }
        const bool f7_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        const bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

        bool changed = false;
        const uint32_t packed = zhr_vk_get_graphics_mode();
        uint32_t mode = packed & 0x3u;
        bool pbr = ((packed >> 2) & 0x1u) != 0;
        bool post_fx = ((packed >> 3) & 0x1u) != 0;
        bool remaster_tex = ((packed >> 4) & 0x1u) != 0;

        if (f7_down && !m_f7_was_down)
        {
            const bool next = !g_zhr_mesh_smoothing_enabled.load(std::memory_order_relaxed);
            g_zhr_mesh_smoothing_enabled.store(next, std::memory_order_relaxed);
            changed = true;
        }
        if (f8_down && !m_f8_was_down)
        {
            remaster_tex = !remaster_tex;
            if (remaster_tex && mode == 0u) mode = 1u;
            zhr_vk_set_remaster_textures(remaster_tex);
            changed = true;
        }
        if (f9_down && !m_f9_was_down)
        {
            // Cycle Remastered (1) -> Split-Screen A/B (2) -> Classic 2003 (0) -> Remastered (1)
            if (mode == 1u)
            {
                mode = 2u;
                pbr = true;
                post_fx = true;
                remaster_tex = true;
            }
            else if (mode == 2u)
            {
                mode = 0u;
                pbr = false;
                post_fx = false;
                remaster_tex = false;
            }
            else
            {
                mode = 1u;
                pbr = true;
                post_fx = true;
                remaster_tex = true;
            }
            changed = true;
        }
        if (f10_down && !m_f10_was_down)
        {
            pbr = !pbr;
            if (pbr && mode == 0u) mode = 1u;
            changed = true;
        }
        if (f11_down && !m_f11_was_down)
        {
            post_fx = !post_fx;
            if (post_fx && mode == 0u) mode = 1u;
            changed = true;
        }

        m_f7_was_down = f7_down;
        m_f8_was_down = f8_down;
        m_f9_was_down = f9_down;
        m_f10_was_down = f10_down;
        m_f11_was_down = f11_down;

        if (changed)
        {
            zhr_vk_set_graphics_mode(mode, pbr, post_fx);
            zhr_vk_set_remaster_textures(remaster_tex);
            g_zhr_title_banner_dirty.store(false, std::memory_order_relaxed);
            UpdateGraphicsTitleBanner(mode, pbr, post_fx, remaster_tex);
        }
        else if (g_zhr_title_banner_dirty.exchange(false, std::memory_order_relaxed))
        {
            UpdateGraphicsTitleBanner(mode, pbr, post_fx, remaster_tex);
        }
    }

    void UpdateGraphicsTitleBanner(uint32_t mode, bool pbr, bool post_fx, bool remaster_tex)
    {
        if (!m_hwnd) return;
        if (m_base_window_title[0] == '\0')
        {
            GetWindowTextA(m_hwnd, m_base_window_title, static_cast<int>(sizeof(m_base_window_title)));
            if (m_base_window_title[0] == '\0')
            {
                std::snprintf(m_base_window_title, sizeof(m_base_window_title), "%s", "ZeroHourReloaded");
            }
        }
        const char* mode_label =
            (mode == 0u) ? "Classic 2003" :
            (mode == 2u) ? "Split-Screen A/B (Classic | Remastered)" :
                           "Remastered";
        char title[384]{};
        std::snprintf(
            title,
            sizeof(title),
            "%s  [Vulkan 1.3 | %s | Mesh:%s | 2xTex:%s | PBR:%s | PostFX:%s | F7-F11]",
            m_base_window_title,
            mode_label,
            (g_zhr_mesh_smoothing_enabled.load(std::memory_order_relaxed) && mode != 0u) ? "ON" : "OFF",
            (remaster_tex && mode != 0u) ? "ON" : "OFF",
            pbr ? "ON" : "OFF",
            post_fx ? "ON" : "OFF");
        SetWindowTextA(m_hwnd, title);
    }

    std::atomic<ULONG> m_ref_count{1};
    IDirect3D8* m_parent_d3d;
    HWND m_hwnd;
    bool m_f7_was_down{false};
    bool m_f8_was_down{false};
    bool m_f9_was_down{false};
    bool m_f10_was_down{false};
    bool m_f11_was_down{false};
    char m_base_window_title[256]{};
    D3DPRESENT_PARAMETERS m_pp;
    D3DCAPS8 m_caps;
    uint32_t m_width;
    uint32_t m_height;

    D3DMATRIX m_world{};
    D3DMATRIX m_view{};
    D3DMATRIX m_proj{};
    D3DMATRIX m_tex_matrix[4]{};
    D3DVIEWPORT8 m_viewport{};

    DWORD m_render_states[256]{};
    DWORD m_tss[8][32]{};
    IDirect3DBaseTexture8* m_textures[8]{};
    ZhrVkVertexBuffer8* m_streams[2]{};
    UINT m_strides[2]{};
    ZhrVkIndexBuffer8* m_index_buffer{nullptr};
    UINT m_base_vertex_index{0};
    DWORD m_fvf{0};
    DWORD m_pixel_shader{0};

    D3DLIGHT8 m_lights[4]{};
    bool m_light_enabled[4]{};
    D3DMATERIAL8 m_material{};

    ZhrVkSurface8* m_back_buffer{nullptr};
    ZhrVkSurface8* m_depth_surface{nullptr};
    IDirect3DSurface8* m_current_rt{nullptr};
    IDirect3DSurface8* m_current_depth{nullptr};

    void ClearStaticGpuCache()
    {
        for (const auto& kv : m_static_gpu_cache)
        {
            if (kv.second.gpu_vb != 0u) zhr_vk_destroy_buffer(kv.second.gpu_vb);
            if (kv.second.gpu_ib != 0u) zhr_vk_destroy_buffer(kv.second.gpu_ib);
        }
        m_static_gpu_cache.clear();
        m_tess_cache.clear();
    }

    void EvictStaticGpuCacheLru(size_t count)
    {
        if (m_static_gpu_cache.empty()) return;
        std::vector<std::pair<uint64_t, StaticDrawKey>> order;
        order.reserve(m_static_gpu_cache.size());
        for (const auto& kv : m_static_gpu_cache)
        {
            order.emplace_back(kv.second.last_used_frame, kv.first);
        }
        const size_t n = std::min(count, order.size());
        std::nth_element(
            order.begin(),
            order.begin() + static_cast<ptrdiff_t>(n),
            order.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < n; ++i)
        {
            auto it = m_static_gpu_cache.find(order[i].second);
            if (it != m_static_gpu_cache.end())
            {
                if (it->second.gpu_vb != 0u) zhr_vk_destroy_buffer(it->second.gpu_vb);
                if (it->second.gpu_ib != 0u) zhr_vk_destroy_buffer(it->second.gpu_ib);
                m_static_gpu_cache.erase(it);
            }
        }
    }

    void EvictDynamicTessCacheLru(size_t count)
    {
        if (m_tess_cache.empty()) return;
        std::vector<std::pair<uint64_t, TessCacheKey>> order;
        order.reserve(m_tess_cache.size());
        for (const auto& kv : m_tess_cache)
        {
            order.emplace_back(kv.second.last_used_frame, kv.first);
        }
        const size_t n = std::min(count, order.size());
        std::nth_element(
            order.begin(),
            order.begin() + static_cast<ptrdiff_t>(n),
            order.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < n; ++i)
        {
            m_tess_cache.erase(order[i].second);
        }
    }

    void FlushPerfLog()
    {
        if (!m_perf_enabled || m_perf_frames == 0u) return;
        const double inv_f = 1.0 / static_cast<double>(m_perf_frames);
        const double avg_frame_ms = (static_cast<double>(m_perf_frame_ns) * 1e-6) * inv_f;
        const double avg_submit_ms = (static_cast<double>(m_perf_submit_ns) * 1e-6) * inv_f;
        const double avg_tess_ms = (static_cast<double>(m_perf_tess_ns) * 1e-6) * inv_f;
        const double avg_present_ms = (static_cast<double>(m_perf_present_ns) * 1e-6) * inv_f;
        const double fps = avg_frame_ms > 0.001 ? (1000.0 / avg_frame_ms) : 0.0;
        if (FILE* f = std::fopen(m_perf_log_path, "a"))
        {
            std::fprintf(
                f,
                "res=%ux%u frames=%llu fps=%.1f frame_ms=%.2f submit_ms=%.2f tess_ms=%.2f present_ms=%.2f static_hits=%.1f/f static_misses=%llu dyn_draws=%.1f/f cache=%zu\n",
                m_width,
                m_height,
                static_cast<unsigned long long>(m_perf_frames),
                fps,
                avg_frame_ms,
                avg_submit_ms,
                avg_tess_ms,
                avg_present_ms,
                static_cast<double>(m_perf_static_hits) * inv_f,
                static_cast<unsigned long long>(m_perf_static_misses),
                static_cast<double>(m_perf_dynamic_draws) * inv_f,
                m_static_gpu_cache.size());
            std::fclose(f);
        }
        m_perf_frames = 0u;
        m_perf_frame_ns = 0u;
        m_perf_submit_ns = 0u;
        m_perf_tess_ns = 0u;
        m_perf_present_ns = 0u;
        m_perf_static_hits = 0u;
        m_perf_static_misses = 0u;
        m_perf_dynamic_draws = 0u;
    }

    struct TessCacheKey
    {
        uint64_t hash_a{0};
        uint64_t hash_b{0};
        uint32_t v_count{0};
        uint32_t i_count{0};
        uintptr_t tex_ptr{0};

        bool operator==(const TessCacheKey& o) const noexcept
        {
            return hash_a == o.hash_a && hash_b == o.hash_b &&
                   v_count == o.v_count && i_count == o.i_count &&
                   tex_ptr == o.tex_ptr;
        }
    };

    struct TessCacheKeyHash
    {
        size_t operator()(const TessCacheKey& k) const noexcept
        {
            return static_cast<size_t>(k.hash_a ^ (k.hash_b * 0x9E3779B97F4A7C15ULL) ^ k.tex_ptr);
        }
    };

    struct CachedTessMesh
    {
        std::vector<ZhrVertexWw3d> verts;
        std::vector<uint16_t> indices;
        uint64_t last_used_frame{0};
    };

    // Fast 128-bit content hash over object-space vertices & indices (~1.5us per draw) for dynamic terrain overlays.
    TessCacheKey ComputeTessCacheKey(ZhrVkTexture8* diffuse_tex, bool is_terrain = false) const
    {
        TessCacheKey k{};
        k.v_count = static_cast<uint32_t>(m_scratch_verts.size());
        k.i_count = static_cast<uint32_t>(m_scratch_indices.size());
        k.tex_ptr = is_terrain
            ? (static_cast<uintptr_t>(0x7E44A100ULL) ^ static_cast<uintptr_t>(g_zhr_terrain_hm.generation))
            : reinterpret_cast<uintptr_t>(diffuse_tex);

        uint64_t ha = 14695981039346656037ULL ^ (static_cast<uint64_t>(k.v_count) << 32) ^ k.i_count;
        uint64_t hb = 0x517CC1B727220A95ULL ^ static_cast<uint64_t>(k.tex_ptr);

        const uint64_t* vwords = reinterpret_cast<const uint64_t*>(m_scratch_verts.data());
        const size_t vw_count = (m_scratch_verts.size() * sizeof(ZhrVertexWw3d)) / sizeof(uint64_t);
        for (size_t i = 0; i < vw_count; ++i)
        {
            const uint64_t w = vwords[i];
            ha = (ha ^ w) * 1099511628211ULL;
            hb = ((hb << 7) | (hb >> 57)) ^ (w + 0x9E3779B97F4A7C15ULL);
        }

        const uint64_t* iwords = reinterpret_cast<const uint64_t*>(m_scratch_indices.data());
        const size_t iw_count = (m_scratch_indices.size() * sizeof(uint16_t)) / sizeof(uint64_t);
        for (size_t i = 0; i < iw_count; ++i)
        {
            const uint64_t w = iwords[i];
            ha = (ha ^ w) * 1099511628211ULL;
            hb = ((hb << 11) | (hb >> 53)) ^ w;
        }
        for (size_t i = iw_count * 4; i < m_scratch_indices.size(); ++i)
        {
            ha = (ha ^ m_scratch_indices[i]) * 1099511628211ULL;
        }

        k.hash_a = ha;
        k.hash_b = hb;
        return k;
    }

    std::vector<ZhrVertexWw3d> m_scratch_verts;
    std::vector<uint16_t> m_scratch_indices;
    std::vector<ZhrVertexWw3d> m_subdiv_verts;
    std::vector<uint16_t> m_subdiv_indices;
    std::unordered_map<uint32_t, uint16_t> m_edge_midpoints;
    std::unordered_map<uint32_t, int32_t> m_terrain_edge_state;
    std::unordered_map<StaticDrawKey, CachedGpuMesh, StaticDrawKeyHash> m_static_gpu_cache;
    std::unordered_map<TessCacheKey, CachedTessMesh, TessCacheKeyHash> m_tess_cache;

    bool m_perf_enabled{false};
    bool m_perf_has_prev_frame{false};
    char m_perf_log_path[512]{};
    std::chrono::steady_clock::time_point m_perf_prev_frame{};
    uint64_t m_perf_frames{0};
    uint64_t m_perf_frame_ns{0};
    uint64_t m_perf_submit_ns{0};
    uint64_t m_perf_tess_ns{0};
    uint64_t m_perf_present_ns{0};
    uint64_t m_perf_static_hits{0};
    uint64_t m_perf_static_misses{0};
    uint64_t m_perf_dynamic_draws{0};
};

HRESULT STDMETHODCALLTYPE ZhrVkSurface8::GetDevice(IDirect3DDevice8** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    if (m_device) m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE ZhrVkTexture8::GetDevice(IDirect3DDevice8** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    if (m_device) m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE ZhrVkCubeTexture8::GetDevice(IDirect3DDevice8** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    if (m_device) m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE ZhrVkVolume8::GetDevice(IDirect3DDevice8** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    if (m_device) m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE ZhrVkVolumeTexture8::GetDevice(IDirect3DDevice8** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    if (m_device) m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE ZhrVkVertexBuffer8::GetDevice(IDirect3DDevice8** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    if (m_device) m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE ZhrVkIndexBuffer8::GetDevice(IDirect3DDevice8** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    if (m_device) m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}

// ============================================================================
// ZhrVkD3D8 — Top-level IDirect3D8 factory enumerating Vulkan GPU & modes
// ============================================================================
class ZhrVkD3D8 final : public IDirect3D8
{
public:
    ZhrVkD3D8()
    {
        BuildCaps();
        ProbeAdapterName();
        BuildDisplayModes();
    }

    STDMETHOD(QueryInterface)(REFIID, void** ppvObj) override { if (ppvObj) *ppvObj = nullptr; return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref_count.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override { ULONG n = m_ref_count.fetch_sub(1) - 1; if (n == 0) delete this; return n; }

    STDMETHOD(RegisterSoftwareDevice)(void*) override { return D3D_OK; }
    STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
    STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD /*Flags*/, D3DADAPTER_IDENTIFIER8* pIdentifier) override
    {
        if (Adapter != 0 || !pIdentifier) return D3DERR_INVALIDCALL;
        std::memset(pIdentifier, 0, sizeof(*pIdentifier));
        std::snprintf(pIdentifier->Driver, sizeof(pIdentifier->Driver), "%s", "zhr_vulkan.dll");
        std::snprintf(pIdentifier->Description, sizeof(pIdentifier->Description), "%s", m_adapter_name);
        pIdentifier->DriverVersion.HighPart = (1u << 16) | 3u;
        pIdentifier->DriverVersion.LowPart = (0u << 16) | 1u;
        pIdentifier->VendorId = 0x1002; // Generic modern GPU (avoids legacy GeForce/Voodoo workarounds)
        pIdentifier->DeviceId = 0xFFFF; // Unknown modern ID (no legacy Rage/Radeon7500 hacks)
        return D3D_OK;
    }

    STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter) override
    {
        return Adapter == 0 ? static_cast<UINT>(m_modes.size()) : 0;
    }
    STDMETHOD(EnumAdapterModes)(UINT Adapter, UINT Mode, D3DDISPLAYMODE* pMode) override
    {
        if (Adapter != 0 || Mode >= m_modes.size() || !pMode) return D3DERR_INVALIDCALL;
        *pMode = m_modes[Mode];
        return D3D_OK;
    }
    STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE* pMode) override
    {
        if (Adapter != 0 || !pMode) return D3DERR_INVALIDCALL;
        pMode->Width = m_desktop_w;
        pMode->Height = m_desktop_h;
        pMode->RefreshRate = 60;
        pMode->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }

    STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL) override
    {
        if (Adapter != 0) return D3DERR_INVALIDCALL;
        if (DisplayFormat == D3DFMT_X8R8G8B8 || DisplayFormat == D3DFMT_A8R8G8B8 || DisplayFormat == D3DFMT_R5G6B5)
        {
            if (BackBufferFormat == D3DFMT_X8R8G8B8 || BackBufferFormat == D3DFMT_A8R8G8B8 || BackBufferFormat == D3DFMT_R5G6B5)
            {
                return D3D_OK;
            }
        }
        return D3DERR_NOTAVAILABLE;
    }

    STDMETHOD(CheckDeviceFormat)(
        UINT Adapter,
        D3DDEVTYPE,
        D3DFORMAT,
        DWORD Usage,
        D3DRESOURCETYPE,
        D3DFORMAT CheckFormat) override
    {
        if (Adapter != 0) return D3DERR_INVALIDCALL;
        if (Usage & D3DUSAGE_RENDERTARGET)
        {
            // Render-to-texture offscreen passes are deferred until Vulkan RTT is added;
            // returning NOTAVAILABLE lets W3DShaderManager cleanly use its non-RTT path.
            return D3DERR_NOTAVAILABLE;
        }
        if (Usage & D3DUSAGE_DEPTHSTENCIL)
        {
            switch (CheckFormat)
            {
            // Full hardware depth+stencil supported via Vulkan 1.3 VkStencilOpState
            // (enables W3DVolumetricShadow volumes & occluded unit silhouettes).
            case D3DFMT_D24S8:
            case D3DFMT_D24X8:
            case D3DFMT_D32:
            case D3DFMT_D16:
                return D3D_OK;
            default:
                return D3DERR_NOTAVAILABLE;
            }
        }
        switch (CheckFormat)
        {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_R8G8B8:
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_L8:
        case D3DFMT_A8:
        case D3DFMT_A8L8:
        case D3DFMT_DXT1:
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:
            return D3D_OK;
        default:
            return D3DERR_NOTAVAILABLE;
        }
    }

    STDMETHOD(CheckDeviceMultiSampleType)(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType) override
    {
        return (MultiSampleType == D3DMULTISAMPLE_NONE) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }

    STDMETHOD(CheckDepthStencilMatch)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override
    {
        return D3D_OK;
    }

    STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE, D3DCAPS8* pCaps) override
    {
        if (Adapter != 0 || !pCaps) return D3DERR_INVALIDCALL;
        *pCaps = m_caps;
        return D3D_OK;
    }

    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT) override
    {
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    STDMETHOD(CreateDevice)(
        UINT Adapter,
        D3DDEVTYPE,
        HWND hFocusWindow,
        DWORD,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice8** ppReturnedDeviceInterface) override
    {
        if (Adapter != 0 || !pPresentationParameters || !ppReturnedDeviceInterface)
        {
            return D3DERR_INVALIDCALL;
        }
        *ppReturnedDeviceInterface = nullptr;

        const uint32_t w = pPresentationParameters->BackBufferWidth > 0 ? pPresentationParameters->BackBufferWidth : 1280;
        const uint32_t h = pPresentationParameters->BackBufferHeight > 0 ? pPresentationParameters->BackBufferHeight : 720;
        HWND hwnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : hFocusWindow;
        const bool validation = IsValidationRequested();

        const char* headlessEnv = std::getenv("ZHR_HEADLESS_RENDER");
        const bool headlessRender = (headlessEnv != nullptr && headlessEnv[0] != '\0' && headlessEnv[0] != '0');

        if (!headlessRender && hwnd != nullptr && pPresentationParameters->Windowed)
        {
            RECT curClient{};
            GetClientRect(hwnd, &curClient);
            if (static_cast<uint32_t>(curClient.right - curClient.left) != w ||
                static_cast<uint32_t>(curClient.bottom - curClient.top) != h)
            {
                const DWORD style = static_cast<DWORD>(GetWindowLongPtrA(hwnd, GWL_STYLE));
                const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrA(hwnd, GWL_EXSTYLE));
                RECT desired{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
                AdjustWindowRectEx(&desired, style, FALSE, exStyle);
                const int winW = desired.right - desired.left;
                const int winH = desired.bottom - desired.top;
                const int posX = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - winW) / 2);
                const int posY = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - winH) / 2);
                SetWindowPos(hwnd, nullptr, posX, posY, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        int32_t ok = 0;
        if (!headlessRender && hwnd != nullptr)
        {
            // Prefer tear-free MAILBOX / IMMEDIATE so FramePacer controls render FPS (60/120/144/240/Uncapped) cleanly.
            ok = zhr_vk_init_win32(hwnd, GetModuleHandle(nullptr), w, h, false, validation);
        }
        else
        {
            // Offscreen GPU headless UI & 3D render target (zero OS window/focus interruption; supports any resolution).
            ok = zhr_vk_init_headless(w, h, validation);
        }

        if (ok == 0)
        {
            return D3DERR_NOTAVAILABLE;
        }

        *ppReturnedDeviceInterface = new ZhrVkD3DDevice8(this, hwnd, *pPresentationParameters, m_caps);
        return D3D_OK;
    }

private:
    void ProbeAdapterName()
    {
        std::snprintf(m_adapter_name, sizeof(m_adapter_name), "%s", "ZeroHourReloaded Vulkan 1.3 GPU");
        if (zhr_vk_init_headless(64, 64, false) != 0)
        {
            const int32_t sel = zhr_vk_selected_device();
            const char* name = zhr_vk_device_name(sel >= 0 ? sel : 0);
            if (name != nullptr && name[0] != '\0')
            {
                std::snprintf(m_adapter_name, sizeof(m_adapter_name), "%s (Vulkan 1.3)", name);
            }
            zhr_vk_shutdown();
        }
    }

    void BuildDisplayModes()
    {
        m_desktop_w = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
        m_desktop_h = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
        if (m_desktop_w < 800) m_desktop_w = 1920;
        if (m_desktop_h < 600) m_desktop_h = 1080;

        struct Pair { uint32_t w, h; };
        const Pair presets[] = {
            {800, 600},
            {1024, 768},
            {1280, 720},
            {1280, 800},
            {1280, 960},
            {1280, 1024},
            {1366, 768},
            {1440, 900},
            {1600, 900},
            {1600, 1200},
            {1920, 1080},
            {2560, 1440},
            {3840, 2160},
            {m_desktop_w, m_desktop_h},
        };

        for (const Pair& p : presets)
        {
            bool dup = false;
            for (const D3DDISPLAYMODE& m : m_modes)
            {
                if (m.Width == p.w && m.Height == p.h && m.Format == D3DFMT_X8R8G8B8)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
            {
                m_modes.push_back(D3DDISPLAYMODE{p.w, p.h, 60, D3DFMT_X8R8G8B8});
            }
        }
    }

    void BuildCaps()
    {
        std::memset(&m_caps, 0, sizeof(m_caps));
        m_caps.DeviceType = D3DDEVTYPE_HAL;
        m_caps.AdapterOrdinal = 0;
        m_caps.Caps = 0;
        m_caps.Caps2 = 0;
        m_caps.DevCaps =
            D3DDEVCAPS_HWTRANSFORMANDLIGHT |
            D3DDEVCAPS_HWRASTERIZATION |
            D3DDEVCAPS_DRAWPRIMITIVES2 |
            D3DDEVCAPS_DRAWPRIMITIVES2EX |
            D3DDEVCAPS_DRAWPRIMTLVERTEX |
            D3DDEVCAPS_PUREDEVICE;
        m_caps.PrimitiveMiscCaps =
            D3DPMISCCAPS_MASKZ |
            D3DPMISCCAPS_CULLNONE |
            D3DPMISCCAPS_CULLCW |
            D3DPMISCCAPS_CULLCCW |
            D3DPMISCCAPS_COLORWRITEENABLE;
        m_caps.RasterCaps =
            D3DPRASTERCAPS_DITHER |
            D3DPRASTERCAPS_ZTEST |
            D3DPRASTERCAPS_FOGVERTEX |
            D3DPRASTERCAPS_FOGTABLE |
            D3DPRASTERCAPS_FOGRANGE |
            D3DPRASTERCAPS_ZBIAS;
        m_caps.ZCmpCaps = 0xFF;
        m_caps.SrcBlendCaps = 0x1FFF;
        m_caps.DestBlendCaps = 0x1FFF;
        m_caps.AlphaCmpCaps = 0xFF;
        m_caps.ShadeCaps =
            D3DPSHADECAPS_COLORGOURAUDRGB |
            D3DPSHADECAPS_ALPHAGOURAUDBLEND |
            D3DPSHADECAPS_FOGGOURAUD;
        m_caps.TextureCaps =
            D3DPTEXTURECAPS_ALPHA |
            D3DPTEXTURECAPS_PERSPECTIVE |
            D3DPTEXTURECAPS_MIPMAP |
            D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
        m_caps.TextureFilterCaps =
            D3DPTFILTERCAPS_MINFPOINT |
            D3DPTFILTERCAPS_MINFLINEAR |
            D3DPTFILTERCAPS_MAGFPOINT |
            D3DPTFILTERCAPS_MAGFLINEAR |
            D3DPTFILTERCAPS_MIPFPOINT |
            D3DPTFILTERCAPS_MIPFLINEAR;
        m_caps.TextureAddressCaps =
            D3DPTADDRESSCAPS_WRAP |
            D3DPTADDRESSCAPS_CLAMP |
            D3DPTADDRESSCAPS_MIRROR |
            D3DPTADDRESSCAPS_INDEPENDENTUV;
        m_caps.TextureOpCaps =
            D3DTEXOPCAPS_DISABLE |
            D3DTEXOPCAPS_SELECTARG1 |
            D3DTEXOPCAPS_SELECTARG2 |
            D3DTEXOPCAPS_MODULATE |
            D3DTEXOPCAPS_MODULATE2X |
            D3DTEXOPCAPS_ADD |
            D3DTEXOPCAPS_ADDSIGNED |
            D3DTEXOPCAPS_ADDSMOOTH |
            D3DTEXOPCAPS_SUBTRACT |
            D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
            D3DTEXOPCAPS_BLENDTEXTUREALPHA |
            D3DTEXOPCAPS_BLENDCURRENTALPHA |
            D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
            D3DTEXOPCAPS_DOTPRODUCT3 |
            D3DTEXOPCAPS_MULTIPLYADD;
        m_caps.MaxTextureWidth = 4096;
        m_caps.MaxTextureHeight = 4096;
        m_caps.MaxVolumeExtent = 256;
        m_caps.MaxTextureRepeat = 8192;
        m_caps.MaxTextureAspectRatio = 4096;
        m_caps.MaxAnisotropy = 16;
        m_caps.MaxPointSize = 64.0f;
        m_caps.MaxPrimitiveCount = 0xFFFFF;
        m_caps.MaxVertexIndex = 0xFFFFF;
        m_caps.MaxStreams = 2;
        m_caps.MaxStreamStride = 256;
        m_caps.MaxTextureBlendStages = 4;
        m_caps.MaxSimultaneousTextures = 4;
        m_caps.MaxActiveLights = 4;
        m_caps.MaxUserClipPlanes = 6;
        m_caps.MaxVertexBlendMatrices = 4;
        m_caps.VertexShaderVersion = 0; // Fixed-function fallback until custom WGSL shaders replace D3D8 asm
        m_caps.PixelShaderVersion = 0;
    }

    std::atomic<ULONG> m_ref_count{1};
    D3DCAPS8 m_caps{};
    char m_adapter_name[512]{};
    uint32_t m_desktop_w{1920};
    uint32_t m_desktop_h{1080};
    std::vector<D3DDISPLAYMODE> m_modes;
};

} // namespace

IDirect3D8* ZhrCreateVulkanD3D8()
{
    return new ZhrVkD3D8();
}

extern "C" void zhr_vk_set_mesh_smoothing(bool enabled)
{
    g_zhr_mesh_smoothing_enabled.store(enabled, std::memory_order_relaxed);
    g_zhr_title_banner_dirty.store(true, std::memory_order_relaxed);
}

extern "C" bool zhr_vk_get_mesh_smoothing(void)
{
    return g_zhr_mesh_smoothing_enabled.load(std::memory_order_relaxed);
}

extern "C" void zhr_vk_mark_title_banner_dirty(void)
{
    g_zhr_title_banner_dirty.store(true, std::memory_order_relaxed);
}

extern "C" void zhr_vk_set_terrain_heightmap(
    const uint8_t* data,
    int32_t width,
    int32_t height,
    int32_t border,
    float xy_factor,
    float height_scale)
{
    const bool changed =
        g_zhr_terrain_hm.data != data ||
        g_zhr_terrain_hm.width != width ||
        g_zhr_terrain_hm.height != height ||
        g_zhr_terrain_hm.border != border;
    g_zhr_terrain_hm.data = data;
    g_zhr_terrain_hm.width = width;
    g_zhr_terrain_hm.height = height;
    g_zhr_terrain_hm.border = border;
    g_zhr_terrain_hm.xy_factor = (xy_factor > 0.01f) ? xy_factor : 10.0f;
    g_zhr_terrain_hm.height_scale = (height_scale > 0.001f) ? height_scale : 0.625f;
    if (changed)
    {
        g_zhr_terrain_hm.generation += 1u;
    }
}

extern "C" void zhr_vk_set_terrain_pass_active(bool active)
{
    g_zhr_terrain_pass_active.store(active, std::memory_order_relaxed);
}
