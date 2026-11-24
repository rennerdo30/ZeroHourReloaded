/*
**	ZeroHourReloaded — Portable D3DX8 Math & Transition Shims for 64-bit Vulkan Builds
**
**	Provides portable C++ implementations of the 5 D3DXMatrix* helpers (so 32-bit d3dx8.lib
**	is never needed) plus safe transition stubs for the remaining 6 D3DX8 texture/surface
**	helpers while WW3D2 TextureClass / SurfaceClass finish migrating to zhr-vulkan.
*/

#include <d3d8.h>
#include <d3dx8math.h>
#include <d3dx8tex.h>

#include <cmath>
#include <cstring>

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

// Transition stubs for legacy D3DX8 texture/surface helpers while TextureClass/SurfaceClass
// migrate to zhr-vulkan (see rust/zhr-vulkan/src/texture.rs).

HRESULT WINAPI D3DXFilterTexture(
    LPDIRECT3DBASETEXTURE8 /*pBaseTexture*/,
    CONST PALETTEENTRY* /*pPalette*/,
    UINT /*SrcLevel*/,
    DWORD /*Filter*/)
{
    return D3D_OK;
}

HRESULT WINAPI D3DXLoadSurfaceFromSurface(
    LPDIRECT3DSURFACE8 /*pDestSurface*/,
    CONST PALETTEENTRY* /*pDestPalette*/,
    CONST RECT* /*pDestRect*/,
    LPDIRECT3DSURFACE8 /*pSrcSurface*/,
    CONST PALETTEENTRY* /*pSrcPalette*/,
    CONST RECT* /*pSrcRect*/,
    DWORD /*Filter*/,
    D3DCOLOR /*ColorKey*/)
{
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXCreateTexture(
    LPDIRECT3DDEVICE8 /*pDevice*/,
    UINT /*Width*/,
    UINT /*Height*/,
    UINT /*MipLevels*/,
    DWORD /*Usage*/,
    D3DFORMAT /*Format*/,
    D3DPOOL /*Pool*/,
    LPDIRECT3DTEXTURE8* ppTexture)
{
    if (ppTexture) *ppTexture = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXCreateCubeTexture(
    LPDIRECT3DDEVICE8 /*pDevice*/,
    UINT /*Size*/,
    UINT /*MipLevels*/,
    DWORD /*Usage*/,
    D3DFORMAT /*Format*/,
    D3DPOOL /*Pool*/,
    LPDIRECT3DCUBETEXTURE8* ppCubeTexture)
{
    if (ppCubeTexture) *ppCubeTexture = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXCreateVolumeTexture(
    LPDIRECT3DDEVICE8 /*pDevice*/,
    UINT /*Width*/,
    UINT /*Height*/,
    UINT /*Depth*/,
    UINT /*MipLevels*/,
    DWORD /*Usage*/,
    D3DFORMAT /*Format*/,
    D3DPOOL /*Pool*/,
    LPDIRECT3DVOLUMETEXTURE8* ppVolumeTexture)
{
    if (ppVolumeTexture) *ppVolumeTexture = nullptr;
    return E_NOTIMPL;
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
    return E_NOTIMPL;
}

} // extern "C"
