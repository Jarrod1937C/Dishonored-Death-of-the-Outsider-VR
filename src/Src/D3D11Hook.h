#pragma once
#include <d3d11.h>
#include <dxgi.h>

//! Hooks IDXGISwapChain::Present / ResizeBuffers via a dummy device's vtable
//! (works regardless of how/when the game creates its swapchain).
namespace D3D11Hook
{
    struct Callbacks
    {
        void (*onPresent)(IDXGISwapChain* sc) = nullptr;     //!< before the real Present
        void (*onResize)(IDXGISwapChain* sc) = nullptr;      //!< before ResizeBuffers (release RTVs)
    };
    bool Install(const Callbacks& cb);
    void Uninstall();
}
