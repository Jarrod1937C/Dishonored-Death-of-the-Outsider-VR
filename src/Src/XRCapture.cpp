#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "XRCapture.h"

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace
{
// Map a DXGI format to the matching WIC pixel format (8-bit RGBA/BGRA only).
bool WicFormatFor(DXGI_FORMAT fmt, GUID& wic, bool& isBGRA)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:      // the bytes as stored (the HUD texture is typeless since 2026-10-14)
        wic = GUID_WICPixelFormat32bppRGBA; isBGRA = false; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        wic = GUID_WICPixelFormat32bppBGRA; isBGRA = true; return true;
    default:
        return false;
    }
}
} // namespace

bool XRCapture::SaveTextureToPng(ID3D11Device* device, ID3D11DeviceContext* ctx,
                                 ID3D11Texture2D* tex, const wchar_t* path)
{
    if (!device || !ctx || !tex)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);

    GUID wicFmt;
    bool isBGRA;
    if (!WicFormatFor(desc.Format, wicFmt, isBGRA))
        return false;

    // Create a CPU-readable staging copy (source may be multisampled/GPU-only).
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)))
        return false;

    if (desc.SampleDesc.Count > 1)
        ctx->ResolveSubresource(staging.Get(), 0, tex, 0, desc.Format);
    else
        ctx->CopyResource(staging.Get(), tex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    bool ok = false;
    // WIC is used from the render thread; init COM in a mode that is safe here.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;

        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)))
        {
            frame->SetSize(desc.Width, desc.Height);
            GUID pf = wicFmt;
            frame->SetPixelFormat(&pf);   // in/out: the PNG encoder may answer with BGRA

            const UINT bpp = 4;
            const UINT rowBytes = desc.Width * bpp;
            const bool srcBGRA = isBGRA;
            const bool dstBGRA = IsEqualGUID(pf, GUID_WICPixelFormat32bppBGRA) || IsEqualGUID(pf, GUID_WICPixelFormat32bppBGR);
            std::vector<BYTE> pixels((size_t)rowBytes * desc.Height);
            for (UINT y = 0; y < desc.Height; y++)
            {
                const BYTE* src = (const BYTE*)mapped.pData + (size_t)y * mapped.RowPitch;
                BYTE* dst = pixels.data() + (size_t)y * rowBytes;
                if (srcBGRA == dstBGRA) memcpy(dst, src, rowBytes);
                else for (UINT x = 0; x < desc.Width; x++)   // swap R<->B
                { dst[x * 4 + 0] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4 + 0]; dst[x * 4 + 3] = src[x * 4 + 3]; }
            }
            if (SUCCEEDED(frame->WritePixels(desc.Height, rowBytes, (UINT)pixels.size(), pixels.data())))
            {
                ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
            }
        }
    }

    ctx->Unmap(staging.Get(), 0);
    if (SUCCEEDED(hrInit))
        CoUninitialize();
    return ok;
}
