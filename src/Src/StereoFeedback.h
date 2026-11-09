#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <string>

//! Per-eye history for the post effect that reads a texture written in an EARLIER frame (Foresight, 2026-11-08).
//!
//! Reported: "the world renders double in Foresight, the same way in each eye". The power watch's texture reads during
//! the flight (PowerWatch: reads, 2026-11-08): the post-processing draw PS 6b05662764a19199 (POSTPROC) reads at t0 a
//! half-resolution RGBA16F image that the DISTORTION pass wrote ONE FRAME BEFORE, in the OTHER eye's frame (every sample,
//! in each power window); everything else the picture effects read was this frame's. In alternating-eye stereo the
//! previous frame is the other eye's picture, so each eye blended in the other eye's view of the world: two worlds a
//! stereo disparity (and a head movement) apart, the same pair in both eyes. Outside the powers the distortion covers a
//! few heat hazes; in Foresight's flight it covers the whole screen (DISTORTION 22 Mpixels a frame against 3).
//!
//! The fix is the one StereoTaa makes for the TAA's history, at the frame boundary: at each Present the texture, as the
//! finished frame left it, is kept for that frame's eye, and the copy kept for the OTHER eye -- the next frame's -- is put
//! back in its place. Whatever order the game writes and reads it in within a frame, each eye then only ever sees its
//! own. Only while that draw has been seen in the last second (two copies of 25 MB per Present then; nothing otherwise).
namespace StereoFeedback
{
    void SetEnabled(bool on);
    //! GpuTrace's draw hooks (any context): finds the texture (only inside the eyes' POSTPROC tasks)
    void OnDraw(ID3D11DeviceContext* c);
    //! Main's Present hook, before the VR submit (the image's eye is still the one this Present shows)
    void OnPresent(IDXGISwapChain* sc);
    std::string Status();
}
