#pragma once
#include <cstdint>

//! Stage 4c: the equipment (weapon) entities follow the controllers. Every entity's world transform is produced by
//! its transform component's update (exe+0x7B6700: this = transform cpnt, entity = this+0x10, parent provider =
//! this+0xD8; writes entity+0x310 origin / +0x31C axis / +0x340 scale from this+0x30 / +0x3C / +0x60; DOTO -- Dishonored 2:
//! entity+0x300 / +0x30C / +0x330). The detour
//! recognises the children of the PlayerLeftArm / PlayerRightArm entities (see BodyHide) and rewrites their world
//! transform with the controller pose plus a configurable grip offset. The arms themselves are left alone (hidden).
namespace WeaponPose
{
    constexpr uint32_t kTransformUpdate  = 0x7B6700;
    constexpr uint32_t kSetOriginAxis    = 0x9F7F0;   //!< arkTransformSet::SetOriginAndAxis(this, const idVec3*, const idMat3*) -> +0xC4 origin, +0xA0 axis, +0x60 4x4 (what the renderer uses)
    constexpr uint32_t kCpntEntity       = 0x10;
    constexpr uint32_t kCpntParent       = 0xD8;     //!< transform provider of the parent; == this+0x20 when unparented
    constexpr uint32_t kCpntOrigin       = 0x30;
    constexpr uint32_t kCpntAxis         = 0x3C;
    constexpr uint32_t kEntityOrigin     = 0x310;   // (D2 0x300)
    constexpr uint32_t kEntityAxis       = 0x31C;   // (D2 0x30C)

    bool Install();
    void Uninstall();
    void SetEnabled(int mode);   //!< 0 off, 1 weapon models, 2 arm models, 3 both
    int  Enabled();
    //! Grip offset applied in the controller frame: rotation (pitch, yaw, roll in degrees) then translation (fwd, left, up in m).
    void SetGrip(int hand, float pitchDeg, float yawDeg, float rollDeg, float fwd, float left, float up);
    void SetResearchLog(bool on);
    void ApplyGrip(int hand, float org[3], float axis[9]);   //!< in place: axis = grip.rot * axis, org += axis * grip.t   //!< log the transform hierarchy of entities near the camera (once per entity)
    struct Stats { unsigned long long calls = 0, appliedL = 0, appliedR = 0, tsetCalls = 0, tsetApplied = 0; void* lastL = nullptr; void* lastR = nullptr; };
    Stats GetStats();
}
