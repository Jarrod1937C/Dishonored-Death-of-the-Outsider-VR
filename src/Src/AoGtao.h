#pragma once
#include <d3d11.h>
#include <string>

//! Better ambient occlusion in the headset ([Display.Effects] AmbientOcclusionQuality = high).
//!
//! With AmbientOcclusion = 1 the game's own AO is DWAO: in each eye's SSAO task ONE full-resolution draw (PS
//! 7012fd32e80f486e) takes two depth taps per pixel from a 4x4 rotated pattern and writes an RGBA8 target -- AO in x,
//! 1 in y, the linear depth packed in z / w -- which the SCREENSHADOWS pass copies into the screen mask (R) and blurs
//! with a bilateral filter before the forward passes light with it. Two taps leave a coarse dither that the blur and
//! the TAA have to hide: thin halos round characters and objects, which the TAA sharpening then outlined (2026-11-08).
//!
//! Here that one draw is replaced: the same target, the same inputs the game has bound (t0 the full-resolution depth,
//! t1 the half-resolution linear depth, cb1 the view constants), the same output layout, so everything after it -- the
//! game's blur, the lighting -- is unchanged. The shader is GTAO (Jimenez et al. 2016, after Intel's XeGTAO, MIT):
//! per pixel two slice directions, four steps each way along each (16 taps of the half-resolution depth), the horizon
//! angles integrated against the reconstructed normal -- a cosine-weighted visibility, not a count of occluders, so
//! there is no dark rim at depth edges. The slice angle and the step offsets follow a 4x4 pattern the game's own
//! 4x4-aware blur takes out. About 0.2-0.4 ms per eye at 3500 x 3600 on a fast GPU (HBAO+ was held off: costlier).
namespace AoGtao
{
    //! The setting asks for it (AmbientOcclusion on, quality high) and the shader is ready
    bool Active();
    //! At Present (any thread that has the device): the shader and the constants, made and updated
    void OnPresent(ID3D11Device* dev);
    //! At the game's AO draw (GpuTrace, in an eye's SSAO task, PS DWAO bound): GTAO drawn instead. True = the game's
    //! draw is not to be made.
    bool Replace(ID3D11DeviceContext* c);
    //! The mod's own draw is being made on this thread (the draw hooks let it through untouched)
    bool InDraw();
    std::string Stats();
}
