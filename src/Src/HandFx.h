#pragma once
#include <string>

//! Effects anchored to the first-person arms come out of the hand the player SEES.
//!
//! The mod poses the arms on the GPU only (GpuTrace rewrites each arm draw's joint palette, M' = D * M), so the
//! engine's own skeleton keeps the animated pose. Everything the engine anchors to an arm joint -- Far Reach's tendril
//! and its hand cloud, Blink's hand smoke, the aiming glow of every power -- is placed from that skeleton, and came
//! out of an invisible hand the animation left behind (measured: Far Reach's tendril started ~40 cm left of the drawn
//! hand and did not follow it at all).
//!
//! Every such query goes through two small engine functions that return a joint of a model in MODEL space from the
//! animated pose array (exe+0x58CF70 with a local offset, exe+0x58D220 without); the world-space wrappers
//! (exe+0x58D380, exe+0x58D4C0, and the by-name exe+0x58D640 with its 86 callers) are built on them. They are hooked
//! here, and when the model is one of the player's arms the result gets the same per-segment delta the palette
//! rewrite gives that bone -- so the effect and the drawn hand share one pose and cannot drift apart. Nothing is
//! written to the engine's skeleton, and nothing else (the aim, the power's target) is touched.
namespace HandFx
{
    constexpr uint32_t kJointModelOff = 0x58CF70;   //!< bool (obj, space, short joint, offPos, offAxis, outPos, outAxis)
    constexpr uint32_t kJointModel    = 0x58D220;   //!< bool (obj, space, short joint, outPos, outAxis)
    constexpr uint32_t kBindTransform = 0x7B3150;   //!< an entity bound to a joint: its transform (calls exe+0x58D380 with the bind's joint)
    constexpr uint32_t kObjModel      = 0x38;       //!< obj -> arkSkinnedRenderModel*
    constexpr uint32_t kModelOrigin   = 0x1860;     //!< render model's world origin (3 floats) ...
    constexpr uint32_t kModelAxis     = 0x186C;     //!< ... and axis (idMat3, rows = basis vectors)

    bool Install();
    void Uninstall();
    //! 0 = off (the engine's answer), 1 = measure only, 2 = correct arm joints except for entities bound to them (the
    //! equipment) -- the default, and inert unless GpuTrace is re-posing the arms (vr body / vr hands) --, 3 = those
    //! too (research: it moves the equipment entity, which the weapon identification reads).
    void SetMode(int mode);
    int  Mode();
    //! Research: the callers, joints and space checks seen since the last reset, to the log; then reset. The first
    //! call only switches the bookkeeping on (it walks the stack on every arm query, so it is off until asked for).
    void LogReport();
    //! Research: log the next `n` binds of this entity (exe+0x7B3150) and every joint query made inside them.
    void ProbeEntity(void* entity, int n);
    //! The entity whose bind transform (exe+0x7B3150) is running on this thread -- what a joint query made now will
    //! place -- or null outside a bind.
    void* BoundEntity();
    //! DOTO: the wrist launcher's entity on that arm (remembered from when it was last the held item), or null.
    void* WristLauncherEntity(int side);
    //! DOTO: how many launcher joints this thread has had moved onto the drawn forearm so far (GunRay compares it across a
    //! world-space joint query: the engine's world query asks the model-space one HandFx corrects, and a joint moved
    //! there must not be carried again).
    unsigned WristFixesOnThread();
    std::string Stats();
}
