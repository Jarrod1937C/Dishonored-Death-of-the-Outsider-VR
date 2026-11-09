#pragma once
#include <cstdint>
struct XRInputState;

//! Stage 4b: controller aim ("fire ray"). The weapon code resolves a shot from three sources (see docs/ENGINE.md):
//!   - the aim point provider (player component #9): GetAimPoint(exe+0x402980) returns the per-frame crosshair
//!     target (+0x88; +0x58 in Dishonored 2) or eye + aimCpnt.worldDir * dist;
//!   - arkCpntWeaponRanged::ComputeAimTarget (exe+0x68DB40): muzzle = the WEAPON entity's pose (this+0x10) *
//!     m_projectileSpawnPos (this+0x104), traces from the camera eye (cam+0xC8) along the camera axis (cam+0xEC);
//!     (DOTO offsets; Dishonored 2: this+0xF4, cam+0xD8, cam+0xFC)
//!     GunRay now aims all of it along the weapon as drawn (docs/ENGINE.md, "Weapons and combat");
//!   - the projectile then flies from the muzzle towards the aim point.
//! With `vr hand 2` both entry points are detoured: GetAimPoint is re-targeted along the controller ray (same
//! distance), and for the duration of ComputeAimTarget the camera / player / aim-component fields are swapped
//! for the controller pose, so the muzzle and the traces start at the hand. Rendering is untouched.
namespace HandAimHook
{
    // arkCpntPlayerCamera
    constexpr uint32_t kCameraUpdate     = 0x85A010;   //!< camera update (runs once at load) -> learn the component address
    constexpr uint32_t kUpdateAim        = 0x85B340;   //!< per-frame: m_aimAxis = f(m_pitch, m_yaw); m_aimOrigin
    constexpr uint32_t kGetAimOriginAxis = 0x408450;   //!< GetAimOriginAndAxis(this, out org, out axis) (research counter)
    constexpr uint32_t kCamOrigin        = 0xC8;       //!< idVec3 eye origin read by weapons / aim component (D2 0xD8; DOTO's renderView is 0x10 shorter)
    constexpr uint32_t kCamAxis          = 0xEC;       //!< idMat3 view axis read by weapons / aim component (D2 0xFC)
    constexpr uint32_t kAimOrigin        = 0x252AC;    //!< idVec3 (UpdateAim output; not used by weapons)
    constexpr uint32_t kAimAxis          = 0x252B8;    //!< idMat3 (UpdateAim output; not used by weapons)
    constexpr uint32_t kLeftArmAiming    = 0x25334;
    constexpr uint32_t kRightArmAiming   = 0x25358;
    constexpr uint32_t kPitch            = 0x25294;
    constexpr uint32_t kYaw              = 0x252A4;
    // player aim component (vtable exe+0x1CD7268)
    constexpr uint32_t kAimCpntUpdate    = 0x788440;   //!< per-frame update: m_currentAimWorldDir from cam+0xFC (+aim assist)
    constexpr uint32_t kAimCpntLocalDir = 0x20, kAimCpntWorldDir = 0x2C, kAimCpntPos = 0x38, kAimCpntCamera = 0x58;
    // weapons
    constexpr uint32_t kGetAimPoint      = 0x402980;   //!< idVec3* GetAimPoint(provider, out, float dist); provider+0x40 cam, +0x48 aimCpnt, +0x84 hasTarget, +0x88 target (D2 +0x54 / +0x58)
    constexpr uint32_t kComputeAimTarget = 0x68DB40;   //!< bool arkCpntWeaponRanged::ComputeAimTarget(this, out spawnPos, viewAxis, aimPoint, flags, flags2)
    constexpr uint32_t kGunFire          = 0x49C860;
    //! Powers and gadgets do NOT go through GetAimPoint/ComputeAimTarget. Measured with a read-watchpoint on the
    //! camera axis (all threads, mock runtime, Far Reach held vs idle): holding the power adds exactly ONE reader,
    //! the per-frame targeting update of the power component (exe+0x491240, called once per frame per active power
    //! component from the dispatch loop at exe+0x9534D0). It takes the eye origin (exe+0x86D8E0 = cam+0xC8) and the
    //! view axis (exe+0x86D860 = cam+0xEC), and computes the landing marker from them.
    constexpr uint32_t kPowerTargetUpdate = 0x491240;   //!< void arkCpntPowerGrapple(FarReach)::UpdateTarget(this)
    //! The throw preview (mine / grenade landing spot) is a player sensor: arkCpntPlayerSensorThrowHit::
    //! UpdateQueryPhysics. It reads the PLAYER's first-person origin/axis (player+0x310/+0x31C; D2 +0x300/+0x30C) for the launch and
    //! the camera through the accessors, every frame, whether or not a throwable is equipped.
    constexpr uint32_t kThrowHitUpdate    = 0x40F8D0;
    //! Where a MINE / spring razor can be placed: arkCpntPlayerSensorPlacement::UpdateQueryPhysics(this). One ray from
    //! the camera eye (exe+0x86D8E0 on sensor+0x40) along the aim component's m_currentAimWorldDir (sensor+0x48, +0x2C),
    //! range sensor+0x58 (about 2 m: measured, a mine is only placeable looking almost straight down). Both are fields
    //! CamSwap replaces, so wrapped in one (poweraim bit 64) the mine goes where the left hand points.
    constexpr uint32_t kPlacementQuery    = 0x400990;
    //! arkCpntPlayerSensorSearchTarget::UpdateQueryPhysics (shapeCast + rayCast). Reads the camera axis every
    //! frame; the power's landing hit (+0x1B4 of the power component) is fetched from an asynchronous physics query
    //! result, and this sensor is the one that submits camera rays every frame.
    constexpr uint32_t kSearchTargetUpdate = 0x405220;
    //! arkCpntPlayerSensorGrapple: the sensor that actually computes where Far Reach lands. Measured with a write
    //! watch on the power component's landing point (+0xF0): it is copied from this sensor (+0x1CC..) by the
    //! power's targeting update (exe+0x491B60); the sensor copies the camera's m_aimOrigin/m_aimAxis (cam+0x252AC /
    //! +0x252B8), which aimoverride points along the power hand (first read as the player's +0x310/+0x31C). Two entry points: the physics-query submission dispatched with the
    //! other sensors (exe+0x3F5530, "UpdateQueryPhysics") and the landing computation, reached through the
    //! two-argument wrapper exe+0x3F0B20 (sensor, const idVec3* from). The computation itself (exe+0x3F2EB0) takes
    //! six arguments, two of them on the stack: detouring it with a register-only thunk lost those and the game
    //! died on a null read inside it (measured, CRASH at exe+0x3F11F1) -- hook the wrapper, never the worker.
    constexpr uint32_t kGrappleSensorQuery   = 0x3F5530;
    constexpr uint32_t kGrappleSensorCompute = 0x3F0B20;
    //! The power component's main update (exe+0x4909C0, one caller in the component dispatch loop). Its targeting
    //! pass (exe+0x491B60) calls the grapple wrapper and also reads the camera pitch through the accessor.
    constexpr uint32_t kPowerUpdate          = 0x4909C0;
    //! Which engine calls run with the controller pose (bit mask): 1 = power targeting update, 2 = throw-preview
    //! sensor, 4 = search-target sensor, 8 = grapple sensor query, 16 = grapple sensor compute, 32 = the whole
    //! power component update. Research knob; 1..16 on by default.
    void SetPowerAimMask(int mask);
    int  PowerAimMask();
    //! DOTO: the sensors' camera swaps to the hand during Foresight's flight (`vr flightcamswap 0|1`, default 0 = skipped:
    //! the flight's camera read the hand mid-swap every other frame -- the double image)
    void SetFlightCamSwap(bool on);
    unsigned long long FlightSwapsSkipped();
    unsigned long long FlightOverrideSkipped();   //!< frames of the flight without the aim override (m_aimOrigin from the hand)
    //! 1 = after UpdateAim, the camera's m_aimOrigin/m_aimAxis hold the power hand's controller pose for the rest
    //! of the frame (what every power / player sensor aims with). 0 = leave them to the view.
    void SetAimOverride(int on);
    int  AimOverride();
    constexpr uint32_t kCamApplyShake    = 0x8FA6C0;
    constexpr uint32_t kCamCopyView      = 0x858B00;
    constexpr uint32_t kRenderViewCopy   = 0x455C0;    //!< renderView_t copy(dst, src): cam+0x30 -> scene view input (then 0x37B699 view setup)
    constexpr uint32_t kCamRenderView    = 0x30;       //!< renderView_t inside arkCpntPlayerCamera (origin +0x98 = cam+0xC8, axis +0xBC = cam+0xEC; D2 +0xA8/+0xCC = cam+0xD8/+0xFC)   //!< arkCpntPlayerCamera: copies renderView (cam+0x30) origin/axis to cam+0x25494 / cam+0x25470
    constexpr uint32_t kCamFpOrigin      = 0x25494, kCamFpAxis = 0x25470;   //!< (this, renderView* = cam+0x80): last writer of cam+0xD8 / cam+0xFC each frame   //!< arkCpntWeaponGun::Fire (calls 0x68E710 -> GetAimPoint, ComputeAimTarget, 0x68E970 spawn)
    constexpr uint32_t kGunSpawnPos      = 0x49C040;   //!< arkCpntWeaponGun slot 3: m_projectileSpawnPos (+0x104, view space; D2 +0xF4) from the muzzle tag

    bool Install();
    void Uninstall();
    //! [Gameplay.Powers] FarReachVRComfortOption: Far Reach takes the player to where it landed at once, like Blink, instead of
    //! pulling him there (arkPlayerStateGrapple::InterpolateToTarget hooked: its start speed is raised for the call, so
    //! the game's own arrival -- the player put on the target -- comes on the pull's first frame). Game thread hook.
    void SetFarReachBlink(bool on);
    void SetMode(int mode);     //!< 0 off, 2 = weapon fire follows the controller (1 reserved)
    int  Mode();
    void SetGunHand(int hand);  //!< 0 = left controller (DOTO pistol/crossbow/powers hand), 1 = right
    int  GunHand();
    //! Hand that aims powers (Far Reach and friends) and the throw preview. Separate from the gun hand so the
    //! two can differ later; both default to the left controller, which is where DOTO puts them.
    void SetPowerHand(int hand);
    int  PowerHand();
    //! World-space pose of a controller: origin = game eye + tracked offset, axis = body yaw * controller rotation
    //! (in head-aim mode the HMD yaw is removed first since the game yaw already tracks it). Rows fwd/left/up.
    bool ControllerPose(int hand, float org[3], float axis[9]);
    //! Same, but anchored to an explicit view instead of the game camera: `eyeOrg`/`viewAxis` = the pose the
    //! renderer actually used this frame and `eyePosXr` = the XR-space position it was built from. GpuTrace needs
    //! this because the weapon's MVP lives in the RENDER view's space, which differs from the game camera by the
    //! HMD positional offset — anchoring to the game camera would make the weapon drift whenever the player leans.
    //! `in` / `headQuat` (optional): the controllers and the head's orientation to use instead of the latest located ones --
    //! the poses a VIEW was built from (VRRuntime::PoseSet), so both eyes of a pair draw the hands from one instant.
    bool ControllerPoseAt(int hand, const float eyeOrg[3], const float viewAxis[9], const float eyePosXr[3],
                          float org[3], float axis[9], const XRInputState* in = nullptr, const float* headQuat = nullptr);
    //! An XR-space displacement expressed in world axes, with the same yaw handling ControllerPoseAt applies to the
    //! controller's offset from the eye. GpuTrace hangs the shoulders off the HEAD, which is the eye plus one of these.
    void XrOffsetToWorld(const float dXr[3], const float viewAxis[9], float out[3], const float* headQuat = nullptr);

    void* CameraObject();       //!< arkCpntPlayerCamera
    void* AimCpnt();            //!< player aim component
    void  SetAimCpnt(void* p);  //!< research override
    void* GunCpnt();            //!< last arkCpntWeaponGun seen
    unsigned long long GetAimCalls();
    //! FP anchor: after the camera is final, cam+0xD8/+0xFC take the controller pose (arms/weapons + fire ray follow it);
    //! the renderView copy feeding the scene gets the real camera back. 0 off, 1 on. Base pose for controllers = real camera.
    void  SetFpAnchor(int on);
    void  SetAnchorHand(int h);
    void  SetArmTest(int mode);
    //! Arms follow the anchor-hand controller orientation: m_aimAxis = controller axis after UpdateAim and the arm aiming
    //! limits (m_leftArmAiming/m_rightArmAiming, radians) are widened while active (originals restored when disabled).
    void  SetArmsFollow(bool on);
    bool  ArmsFollow();   //!< research: 1 = m_armsPitch from controller pitch, 2 = m_left/rightArmAiming = controller axis (view-relative), 3 = both
    constexpr uint32_t kArmsPitch = 0x25298;
    constexpr uint32_t kRightArmFollow = 0x251E8;   //!< bool m_rightArmFollow   //!< -1 auto (right while the right trigger/melee is used, else gun hand), 0 left, 1 right
    int   AnchorHand();           //!< hand currently anchoring the FP models
    int   FpAnchor();
    bool  RealCamera(float org[3], float axis[9]);
    void  SetCamTest2(float dz);  //!< research: lift cam+0x25494.z after the copy
    void  SetCamTest(float dz);   //!< research: lift the camera eye (cam+0xD8.z) after the final camera writer; ViewMatrixHook compensates
    float CamTest();
    struct Stats
    {
        unsigned long long calls = 0, aimPointCalls = 0, aimPointApplied = 0, targetCalls = 0, targetApplied = 0, gunCalls = 0;
        // Power targeting update and throw-preview sensor: calls seen / calls that ran with the controller pose.
        unsigned long long powerCalls = 0, powerApplied = 0, throwCalls = 0, throwApplied = 0, placeCalls = 0, placeApplied = 0, searchCalls = 0, searchApplied = 0, grappleCalls = 0, grappleApplied = 0, aimOverrides = 0;
        float powerHit[3] = {}, powerTarget[3] = {}, powerEnd[3] = {}; int powerActive = 0;
        float powerOrg[3] = {}, powerFwd[3] = {};
        float ctrlOrg[3] = {}, ctrlFwd[3] = {}, lastAimPoint[3] = {}, gunSpawn[3] = {};
        // FP anchor (renderView copy) path: without these there is no way to tell an inactive anchor from a
        // mis-computed one — the rv_* lines in the heartbeat belong to CameraHook, not to this hook.
        unsigned long long rvCalls = 0, rvMatched = 0, rvApplied = 0, rvPoseFail = 0, shakeCalls = 0;
        float rvOrg[3] = {}, rvFwd[3] = {};
    };
    Stats GetStats();
    //! Times the engine's player-camera aim update has run. It stops dead in the pause menu, in a loading screen and
    //! during a cutscene, which is the only reliable "the game is simulating" signal the mod has.
    unsigned long long AimTicks();
}
