#pragma once
#include <cstdint>
#include <string>

//! The skeletons of the characters around the player, as the engine has them posed THIS frame (2026-10-05, the development notes
//! part 36): what the sword's contact test strikes instead of a box.
//!
//! A character's render-model box is its bind pose (standing, arms out): a guard bending to block, a sleeper, a corpse
//! on the floor all kept a 1.8 m standing column at the entity's origin, and the blade met them only where that column
//! crossed the body (the user: "only a thrust to the middle of the chest hits the corpse"). The engine keeps every joint
//! of a skinned model in a pose array (`obj+0x2D8`, 64-byte entries: three axis rows, then the position at +0x30, in
//! model space -- exe+0x9DAC0), where `obj` is the skeleton object whose +0x38 is the arkSkinnedRenderModel (the same
//! pointer as the entity's model, entity+0xF0); the model's world transform is model+0x1860/+0x186C. A ragdoll writes
//! there too, so a corpse's joints lie where its body is drawn.
namespace NpcSkel
{
    constexpr int kMaxJoints = 256;
    struct Skel
    {
        int n = 0;                          //!< joints read
        float p[kMaxJoints][3] = {};        //!< world positions
        short link[kMaxJoints] = {};        //!< the joint this one is joined to by a bone capsule (-1 = none)
        float r[kMaxJoints] = {};           //!< the body's radius around this joint (and its capsule)
        float lo[3] = {}, hi[3] = {};       //!< world bounds of the joints
        bool humanoid = false;
        bool ragdoll = false;               //!< posed by a ragdoll (a corpse, a body knocked down), not the animation
    };
    //! The world joints of this entity's skinned model, or false (not skinned, not found, unreadable). Present thread;
    //! everything is read under SEH.
    bool Get(void* entity, Skel& out);
    //! 0 = off (boxes for everyone, as before), 1 = skeletons for skinned models (default).
    void SetMode(int m);
    int  Mode();
    //! Research: how an entity reaches its skeleton object, and what the skeleton holds (vr skelprobe <id>).
    void Probe(uint32_t id);
    std::string Stats();
}
