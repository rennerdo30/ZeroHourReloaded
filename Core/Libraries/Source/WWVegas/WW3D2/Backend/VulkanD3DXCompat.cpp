/*
**	ZeroHourReloaded — Portable D3DX8 Math & Texture/Surface Helpers for 64-bit Vulkan Builds
**
**	Provides portable C++ implementations of D3DXMatrix* math helpers plus D3DXCreateTexture,
**	D3DXCreateCubeTexture, D3DXCreateVolumeTexture, D3DXLoadSurfaceFromSurface, and
**	D3DXFilterTexture operating directly on our VulkanDX8Bridge surfaces/textures.
*/

#include <d3d8.h>
#include <d3dx8math.h>
#include <d3dx8tex.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

bool IsCompressed(D3DFORMAT fmt)
{
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 ||
           fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5;
}

uint32_t PixelBytes(D3DFORMAT fmt)
{
    switch (fmt)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8: return 4;
    case D3DFMT_R8G8B8:   return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:     return 2;
    case D3DFMT_L8:
    case D3DFMT_A8:       return 1;
    default:              return 4;
    }
}

void ReadPixelRGBA(D3DFORMAT fmt, const uint8_t* p, uint8_t out[4])
{
    switch (fmt)
    {
    case D3DFMT_A8R8G8B8:
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
        break;
    case D3DFMT_X8R8G8B8:
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255;
        break;
    case D3DFMT_R8G8B8:
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255;
        break;
    case D3DFMT_R5G6B5: {
        uint16_t v = *reinterpret_cast<const uint16_t*>(p);
        uint8_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        out[0] = (r << 3) | (r >> 2);
        out[1] = (g << 2) | (g >> 4);
        out[2] = (b << 3) | (b >> 2);
        out[3] = 255;
        break;
    }
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5: {
        uint16_t v = *reinterpret_cast<const uint16_t*>(p);
        uint8_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        out[0] = (r << 3) | (r >> 2);
        out[1] = (g << 3) | (g >> 2);
        out[2] = (b << 3) | (b >> 2);
        out[3] = (fmt == D3DFMT_A1R5G5B5) ? ((v & 0x8000) ? 255 : 0) : 255;
        break;
    }
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: {
        uint16_t v = *reinterpret_cast<const uint16_t*>(p);
        out[0] = ((v >> 8) & 0xF) * 17;
        out[1] = ((v >> 4) & 0xF) * 17;
        out[2] = (v & 0xF) * 17;
        out[3] = (fmt == D3DFMT_A4R4G4B4) ? (((v >> 12) & 0xF) * 17) : 255;
        break;
    }
    case D3DFMT_L8:
        out[0] = out[1] = out[2] = p[0]; out[3] = 255;
        break;
    case D3DFMT_A8:
        out[0] = out[1] = out[2] = 255; out[3] = p[0];
        break;
    case D3DFMT_A8L8:
        out[0] = out[1] = out[2] = p[0]; out[3] = p[1];
        break;
    default:
        out[0] = out[1] = out[2] = out[3] = 255;
        break;
    }
}

void WritePixelRGBA(D3DFORMAT fmt, uint8_t* p, const uint8_t in[4])
{
    switch (fmt)
    {
    case D3DFMT_A8R8G8B8:
        p[0] = in[2]; p[1] = in[1]; p[2] = in[0]; p[3] = in[3];
        break;
    case D3DFMT_X8R8G8B8:
        p[0] = in[2]; p[1] = in[1]; p[2] = in[0]; p[3] = 255;
        break;
    case D3DFMT_R8G8B8:
        p[0] = in[2]; p[1] = in[1]; p[2] = in[0];
        break;
    case D3DFMT_R5G6B5:
        *reinterpret_cast<uint16_t*>(p) =
            static_cast<uint16_t>(((in[0] >> 3) << 11) | ((in[1] >> 2) << 5) | (in[2] >> 3));
        break;
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
        *reinterpret_cast<uint16_t*>(p) =
            static_cast<uint16_t>(((in[3] >= 128 ? 1 : 0) << 15) | ((in[0] >> 3) << 10) | ((in[1] >> 3) << 5) | (in[2] >> 3));
        break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
        *reinterpret_cast<uint16_t*>(p) =
            static_cast<uint16_t>(((in[3] >> 4) << 12) | ((in[0] >> 4) << 8) | ((in[1] >> 4) << 4) | (in[2] >> 4));
        break;
    case D3DFMT_L8:
        p[0] = in[0];
        break;
    case D3DFMT_A8:
        p[0] = in[3];
        break;
    case D3DFMT_A8L8:
        p[0] = in[0]; p[1] = in[3];
        break;
    default:
        break;
    }
}

} // namespace

extern "C" {

D3DXMATRIX* WINAPI D3DXMatrixTranspose(D3DXMATRIX* pOut, CONST D3DXMATRIX* pM)
{
    D3DXMATRIX tmp;
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            tmp.m[r][c] = pM->m[c][r];
        }
    }
    *pOut = tmp;
    return pOut;
}

D3DXMATRIX* WINAPI D3DXMatrixMultiply(D3DXMATRIX* pOut, CONST D3DXMATRIX* pM1, CONST D3DXMATRIX* pM2)
{
    D3DXMATRIX tmp;
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            tmp.m[r][c] =
                pM1->m[r][0] * pM2->m[0][c] +
                pM1->m[r][1] * pM2->m[1][c] +
                pM1->m[r][2] * pM2->m[2][c] +
                pM1->m[r][3] * pM2->m[3][c];
        }
    }
    *pOut = tmp;
    return pOut;
}

D3DXMATRIX* WINAPI D3DXMatrixInverse(D3DXMATRIX* pOut, FLOAT* pDeterminant, CONST D3DXMATRIX* pM)
{
    const float* m = &pM->m[0][0];
    float inv[16];

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];

    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (pDeterminant)
    {
        *pDeterminant = det;
    }
    if (std::fabs(det) < 1e-12f)
    {
        return nullptr;
    }

    const float inv_det = 1.0f / det;
    float* out = &pOut->m[0][0];
    for (int i = 0; i < 16; ++i)
    {
        out[i] = inv[i] * inv_det;
    }
    return pOut;
}

D3DXMATRIX* WINAPI D3DXMatrixScaling(D3DXMATRIX* pOut, FLOAT sx, FLOAT sy, FLOAT sz)
{
    std::memset(pOut, 0, sizeof(D3DXMATRIX));
    pOut->m[0][0] = sx;
    pOut->m[1][1] = sy;
    pOut->m[2][2] = sz;
    pOut->m[3][3] = 1.0f;
    return pOut;
}

D3DXMATRIX* WINAPI D3DXMatrixTranslation(D3DXMATRIX* pOut, FLOAT x, FLOAT y, FLOAT z)
{
    std::memset(pOut, 0, sizeof(D3DXMATRIX));
    pOut->m[0][0] = 1.0f;
    pOut->m[1][1] = 1.0f;
    pOut->m[2][2] = 1.0f;
    pOut->m[3][3] = 1.0f;
    pOut->m[3][0] = x;
    pOut->m[3][1] = y;
    pOut->m[3][2] = z;
    return pOut;
}

HRESULT WINAPI D3DXLoadSurfaceFromSurface(
    LPDIRECT3DSURFACE8 pDestSurface,
    CONST PALETTEENTRY* /*pDestPalette*/,
    CONST RECT* pDestRect,
    LPDIRECT3DSURFACE8 pSrcSurface,
    CONST PALETTEENTRY* /*pSrcPalette*/,
    CONST RECT* pSrcRect,
    DWORD /*Filter*/,
    D3DCOLOR /*ColorKey*/)
{
    if (!pDestSurface || !pSrcSurface) return D3DERR_INVALIDCALL;

    D3DSURFACE_DESC sdesc{}, ddesc{};
    pSrcSurface->GetDesc(&sdesc);
    pDestSurface->GetDesc(&ddesc);

    D3DLOCKED_RECT slock{}, dlock{};
    if (FAILED(pSrcSurface->LockRect(&slock, nullptr, D3DLOCK_READONLY))) return D3DERR_INVALIDCALL;
    if (FAILED(pDestSurface->LockRect(&dlock, nullptr, 0)))
    {
        pSrcSurface->UnlockRect();
        return D3DERR_INVALIDCALL;
    }

    if (IsCompressed(sdesc.Format) && sdesc.Format == ddesc.Format &&
        sdesc.Width == ddesc.Width && sdesc.Height == ddesc.Height)
    {
        std::memcpy(dlock.pBits, slock.pBits, std::min(sdesc.Size, ddesc.Size));
    }
    else if (!IsCompressed(sdesc.Format) && !IsCompressed(ddesc.Format))
    {
        const RECT sr = pSrcRect ? *pSrcRect : RECT{0, 0, static_cast<LONG>(sdesc.Width), static_cast<LONG>(sdesc.Height)};
        const RECT dr = pDestRect ? *pDestRect : RECT{0, 0, static_cast<LONG>(ddesc.Width), static_cast<LONG>(ddesc.Height)};
        const LONG sw = std::max<LONG>(1, sr.right - sr.left);
        const LONG sh = std::max<LONG>(1, sr.bottom - sr.top);
        const LONG dw = std::max<LONG>(1, dr.right - dr.left);
        const LONG dh = std::max<LONG>(1, dr.bottom - dr.top);
        const uint32_t sbpp = PixelBytes(sdesc.Format);
        const uint32_t dbpp = PixelBytes(ddesc.Format);

        for (LONG y = 0; y < dh; ++y)
        {
            const LONG sy = sr.top + (y * sh) / dh;
            const LONG dy = dr.top + y;
            if (sy < 0 || sy >= static_cast<LONG>(sdesc.Height) || dy < 0 || dy >= static_cast<LONG>(ddesc.Height)) continue;
            const uint8_t* srow = static_cast<const uint8_t*>(slock.pBits) + static_cast<size_t>(sy) * slock.Pitch;
            uint8_t* drow = static_cast<uint8_t*>(dlock.pBits) + static_cast<size_t>(dy) * dlock.Pitch;
            for (LONG x = 0; x < dw; ++x)
            {
                const LONG sx = sr.left + (x * sw) / dw;
                const LONG dx = dr.left + x;
                if (sx < 0 || sx >= static_cast<LONG>(sdesc.Width) || dx < 0 || dx >= static_cast<LONG>(ddesc.Width)) continue;
                uint8_t rgba[4];
                ReadPixelRGBA(sdesc.Format, srow + static_cast<size_t>(sx) * sbpp, rgba);
                WritePixelRGBA(ddesc.Format, drow + static_cast<size_t>(dx) * dbpp, rgba);
            }
        }
    }

    pDestSurface->UnlockRect();
    pSrcSurface->UnlockRect();
    return D3D_OK;
}

HRESULT WINAPI D3DXFilterTexture(
    LPDIRECT3DBASETEXTURE8 pBaseTexture,
    CONST PALETTEENTRY* pPalette,
    UINT SrcLevel,
    DWORD Filter)
{
    if (!pBaseTexture || pBaseTexture->GetType() != D3DRTYPE_TEXTURE)
    {
        return D3D_OK;
    }
    auto* tex = static_cast<IDirect3DTexture8*>(pBaseTexture);
    const DWORD levels = tex->GetLevelCount();
    const UINT start = (SrcLevel == D3DX_DEFAULT) ? 0 : SrcLevel;
    for (UINT lvl = start + 1; lvl < levels; ++lvl)
    {
        IDirect3DSurface8* src = nullptr;
        IDirect3DSurface8* dst = nullptr;
        if (SUCCEEDED(tex->GetSurfaceLevel(lvl - 1, &src)) &&
            SUCCEEDED(tex->GetSurfaceLevel(lvl, &dst)))
        {
            D3DXLoadSurfaceFromSurface(dst, pPalette, nullptr, src, pPalette, nullptr, Filter, 0);
        }
        if (src) src->Release();
        if (dst) dst->Release();
    }
    return D3D_OK;
}

HRESULT WINAPI D3DXCreateTexture(
    LPDIRECT3DDEVICE8 pDevice,
    UINT Width,
    UINT Height,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    LPDIRECT3DTEXTURE8* ppTexture)
{
    if (!pDevice || !ppTexture) return D3DERR_INVALIDCALL;
    if (Width == 0 || Width == D3DX_DEFAULT) Width = 1;
    if (Height == 0 || Height == D3DX_DEFAULT) Height = 1;
    if (MipLevels == D3DX_DEFAULT) MipLevels = 0;
    if (Format == D3DFMT_UNKNOWN) Format = D3DFMT_A8R8G8B8;
    return pDevice->CreateTexture(Width, Height, MipLevels, Usage, Format, Pool, ppTexture);
}

HRESULT WINAPI D3DXCreateCubeTexture(
    LPDIRECT3DDEVICE8 pDevice,
    UINT Size,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    LPDIRECT3DCUBETEXTURE8* ppCubeTexture)
{
    if (!pDevice || !ppCubeTexture) return D3DERR_INVALIDCALL;
    if (Size == 0 || Size == D3DX_DEFAULT) Size = 1;
    if (MipLevels == D3DX_DEFAULT) MipLevels = 0;
    if (Format == D3DFMT_UNKNOWN) Format = D3DFMT_A8R8G8B8;
    return pDevice->CreateCubeTexture(Size, MipLevels, Usage, Format, Pool, ppCubeTexture);
}

HRESULT WINAPI D3DXCreateVolumeTexture(
    LPDIRECT3DDEVICE8 pDevice,
    UINT Width,
    UINT Height,
    UINT Depth,
    UINT MipLevels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    LPDIRECT3DVOLUMETEXTURE8* ppVolumeTexture)
{
    if (!pDevice || !ppVolumeTexture) return D3DERR_INVALIDCALL;
    if (Width == 0 || Width == D3DX_DEFAULT) Width = 1;
    if (Height == 0 || Height == D3DX_DEFAULT) Height = 1;
    if (Depth == 0 || Depth == D3DX_DEFAULT) Depth = 1;
    if (MipLevels == D3DX_DEFAULT) MipLevels = 0;
    if (Format == D3DFMT_UNKNOWN) Format = D3DFMT_A8R8G8B8;
    return pDevice->CreateVolumeTexture(Width, Height, Depth, MipLevels, Usage, Format, Pool, ppVolumeTexture);
}

HRESULT WINAPI D3DXCreateTextureFromFileExA(
    LPDIRECT3DDEVICE8 /*pDevice*/,
    LPCSTR /*pSrcFile*/,
    UINT /*Width*/,
    UINT /*Height*/,
    UINT /*MipLevels*/,
    DWORD /*Usage*/,
    D3DFORMAT /*Format*/,
    D3DPOOL /*Pool*/,
    DWORD /*Filter*/,
    DWORD /*MipFilter*/,
    D3DCOLOR /*ColorKey*/,
    D3DXIMAGE_INFO* /*pSrcInfo*/,
    PALETTEENTRY* /*pPalette*/,
    LPDIRECT3DTEXTURE8* ppTexture)
{
    if (ppTexture) *ppTexture = nullptr;
    return D3DERR_NOTFOUND;
}

} // extern "C"
