#pragma once
// Headless verification aid: save a D3D11 texture to a PNG on disk so rendered
// frames / eye images can be inspected without a headset. Debug tool.

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace XRCapture
{
    //! Copies the texture to a CPU-readable staging copy and writes it as PNG.
    //! Handles common RGBA/BGRA 8-bit formats. Returns true on success.
    bool SaveTextureToPng(ID3D11Device* device, ID3D11DeviceContext* ctx,
                          ID3D11Texture2D* tex, const wchar_t* path);
}
