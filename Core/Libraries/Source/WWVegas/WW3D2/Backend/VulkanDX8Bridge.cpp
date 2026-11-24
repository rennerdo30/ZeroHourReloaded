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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

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

    void MarkDirty() { m_dirty = true; }

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

        if (m_format == D3DFMT_DXT1)
        {
            m_vk_handle = zhr_vk_create_texture_2d(
                w, h, 2, base->Data(), base->DataSize(), linear_filter, clamp_edge);
        }
        else if (m_format == D3DFMT_DXT2 || m_format == D3DFMT_DXT3)
        {
            m_vk_handle = zhr_vk_create_texture_2d(
                w, h, 3, base->Data(), base->DataSize(), linear_filter, clamp_edge);
        }
        else if (m_format == D3DFMT_DXT4 || m_format == D3DFMT_DXT5)
        {
            m_vk_handle = zhr_vk_create_texture_2d(
                w, h, 4, base->Data(), base->DataSize(), linear_filter, clamp_edge);
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
    if (!m_last_lock_readonly && m_owner_texture != nullptr)
    {
        m_owner_texture->MarkDirty();
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
    STDMETHOD(Lock)(UINT OffsetToLock, UINT /*SizeToLock*/, BYTE** ppbData, DWORD /*Flags*/) override
    {
        if (!ppbData || OffsetToLock > m_data.size()) return D3DERR_INVALIDCALL;
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

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    DWORD m_usage;
    DWORD m_fvf;
    D3DPOOL m_pool;
    DWORD m_priority{0};
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
    STDMETHOD(Lock)(UINT OffsetToLock, UINT /*SizeToLock*/, BYTE** ppbData, DWORD /*Flags*/) override
    {
        if (!ppbData || OffsetToLock > m_data.size()) return D3DERR_INVALIDCALL;
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

private:
    std::atomic<ULONG> m_ref_count{1};
    ZhrVkD3DDevice8* m_device;
    DWORD m_usage;
    D3DFORMAT m_format;
    D3DPOOL m_pool;
    DWORD m_priority{0};
    std::vector<uint8_t> m_data;
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
    }

    ~ZhrVkD3DDevice8()
    {
        if (zhr_vk_is_frame_active())
        {
            zhr_vk_end_frame();
        }
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
        if (zhr_vk_is_frame_active())
        {
            zhr_vk_end_frame();
        }
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

        return SubmitDraw(
            PrimitiveType,
            primCount,
            m_streams[0]->Data() + start_byte,
            stride,
            fvf,
            vert_span,
            raw_indices,
            v_min);
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
        m_render_states[D3DRS_COLORWRITEENABLE] = 0x0000000F;
        m_render_states[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;

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

    void SyncDrawState(bool is_rhw)
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
    }

    HRESULT SubmitDraw(
        D3DPRIMITIVETYPE prim_type,
        UINT prim_count,
        const uint8_t* vb_data,
        uint32_t stride,
        DWORD fvf,
        uint32_t vertex_count,
        const uint16_t* raw_indices,
        uint32_t index_rebase)
    {
        if (vertex_count == 0 || prim_count == 0) return D3D_OK;
        if (m_render_states[D3DRS_STENCILENABLE] != 0 &&
            m_render_states[D3DRS_STENCILFUNC] == D3DCMP_NEVER)
        {
            return D3D_OK;
        }

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

        const bool use_material_color =
            !is_rhw &&
            (m_render_states[D3DRS_LIGHTING] != 0) &&
            (!has_diffuse || m_render_states[D3DRS_COLORVERTEX] == 0);

        const DWORD tci0 = m_tss[0][D3DTSS_TEXCOORDINDEX];
        const DWORD tci0_mode = tci0 & 0xFFFF0000u;
        const DWORD tci0_index = tci0 & 0x7u;
        const bool use_tex0_xform = (m_tss[0][D3DTSS_TEXTURETRANSFORMFLAGS] & 0xFFu) != D3DTTFF_DISABLE;

        const DWORD tci1 = m_tss[1][D3DTSS_TEXCOORDINDEX];
        const DWORD tci1_mode = tci1 & 0xFFFF0000u;
        const DWORD tci1_index = tci1 & 0x7u;
        const bool use_tex1_xform = (m_tss[1][D3DTSS_TEXTURETRANSFORMFLAGS] & 0xFFu) != D3DTTFF_DISABLE;
        const bool stage1_active = (m_tss[0][D3DTSS_COLOROP] != D3DTOP_DISABLE) &&
                                   (m_tss[1][D3DTSS_COLOROP] != D3DTOP_DISABLE);

        const float vp_x = static_cast<float>(m_viewport.X);
        const float vp_y = static_cast<float>(m_viewport.Y);
        const float vp_w = static_cast<float>(m_viewport.Width > 0 ? m_viewport.Width : 1);
        const float vp_h = static_cast<float>(m_viewport.Height > 0 ? m_viewport.Height : 1);

        auto eval_stage_uv = [&](int stage, DWORD mode, DWORD index, bool xform,
                                 const float* p, float raw_u0, float raw_v0, float raw_u1, float raw_v1,
                                 float out_uv[2]) {
            if (!is_rhw && mode == D3DTSS_TCI_CAMERASPACEPOSITION)
            {
                float wp[4], cp[4], tc[4];
                TransformPointRowMajor(m_world, p[0], p[1], p[2], 1.0f, wp);
                TransformPointRowMajor(m_view, wp[0], wp[1], wp[2], 1.0f, cp);
                if (xform)
                {
                    TransformPointRowMajor(m_tex_matrix[stage], cp[0], cp[1], cp[2], 1.0f, tc);
                    out_uv[0] = tc[0];
                    out_uv[1] = tc[1];
                }
                else
                {
                    out_uv[0] = cp[0];
                    out_uv[1] = cp[1];
                }
            }
            else
            {
                float u = (index == 1 && tex_count >= 2) ? raw_u1 : raw_u0;
                float v = (index == 1 && tex_count >= 2) ? raw_v1 : raw_v0;
                if (xform)
                {
                    float tc[4];
                    TransformPointRowMajor(m_tex_matrix[stage], u, v, 1.0f, 1.0f, tc);
                    u = tc[0];
                    v = tc[1];
                }
                out_uv[0] = u;
                out_uv[1] = v;
            }
        };

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

            float raw_u0 = 0.0f, raw_v0 = 0.0f;
            float raw_u1 = 0.0f, raw_v1 = 0.0f;
            if (tex_count >= 1 && uv0_off + 8 <= stride)
            {
                const float* uv = reinterpret_cast<const float*>(vptr + uv0_off);
                raw_u0 = uv[0];
                raw_v0 = uv[1];
            }
            if (tex_count >= 2 && uv1_off + 8 <= stride)
            {
                const float* uv = reinterpret_cast<const float*>(vptr + uv1_off);
                raw_u1 = uv[0];
                raw_v1 = uv[1];
            }

            eval_stage_uv(0, tci0_mode, tci0_index, use_tex0_xform, p, raw_u0, raw_v0, raw_u1, raw_v1, dst.uv0);
            if (stage1_active)
            {
                eval_stage_uv(1, tci1_mode, tci1_index, use_tex1_xform, p, raw_u0, raw_v0, raw_u1, raw_v1, dst.uv1);
            }
            else
            {
                dst.uv1[0] = raw_u1;
                dst.uv1[1] = raw_v1;
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

        SyncDrawState(is_rhw);

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

        return D3D_OK;
    }

    std::atomic<ULONG> m_ref_count{1};
    IDirect3D8* m_parent_d3d;
    HWND m_hwnd;
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

    std::vector<ZhrVertexWw3d> m_scratch_verts;
    std::vector<uint16_t> m_scratch_indices;
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
            // Report depth-only formats until VkStencilOpState is wired in zhr-vulkan;
            // prevents W3DScene/W3DVolumetricShadow from submitting stencil-masked overlay passes.
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

        int32_t ok = 0;
        if (hwnd != nullptr)
        {
            ok = zhr_vk_init_win32(hwnd, GetModuleHandle(nullptr), w, h, true, validation);
        }
        else
        {
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
