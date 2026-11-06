# DOTO port status

The source was ported from the Dishonored 2 VR mod (`Dishonored2.exe` 1.77.9.0) to Death of the
Outsider (`Dishonored_DO.exe` 1.145.0.0, Steam, 42,192,424 bytes, link stamp `0x67AB7505`) on
2026-11-05, by matching the code of the two executables (method and tools: `tools/port/README.md`).
Every changed constant keeps its Dishonored 2 value next to it as `(D2 0x...)`. **Nothing has been
compiled or run in the game yet.**

## Priority systems

| System | State |
|---|---|
| Synchronised stereo (StereoSync: RunFrame hold, Draw-only frames, input-read skips, right-eye pass reuse, sun-shadow sharing) | ported; the hooked functions are the same code (renderer side byte-identical); gameLocal fields +0x146C0 |
| Frame uncapping (`com_disableFrameSpinning`) | works as in Dishonored 2: DOTO's frame function (exe+0x360100) skips its spin when the cvar's int (exe+0x267CEE8) is set |
| Swapchain / V-Sync handling (D3D11Hook) | ported (no addresses in code) |
| Physics step at the headset refresh (SimStep) | ported; clock (exe+0x5A790), rate instruction (exe+0x5AC0D), 120.0 (exe+0x1BCFAF8), step (exe+0x2341A94) checked byte for byte |
| SkipFrameSyncFlush (PaceLog) | **not needed**: DOTO makes no end-of-frame Flush (D2's exe+0x3591FC has no counterpart; DOTO has 3 Flush calls, the other 3 of D2's 4) |
| View / head pose (ViewMatrixHook, CameraHook), render size (RenderRes) | ported; renderView_t fields from the fov on are 0x10 lower; RenderRes hooks GetWindowRect too (DOTO's window procedure uses it) |

## Layout differences found (selection)

- renderView_t: fov 0x8C/0x90, vieworg 0x98, viewaxis 0xBC, explicit projection 0xF0 / flag 0x130 (each 0x10 lower).
- Player camera component: 0x25xxx fields +0x30; eye cam+0xC8, axis cam+0xEC.
- idEntity: id 0xB0, flags 0xD8/0xD9, model 0xF8, origin 0x310, axis 0x31C, scale 0x340, velocity 0x34C, angular 0x358, skeleton 0x370.
- arkSkinnedRenderModel: skeleton 0x1FD8, masks 0x20F0, numVisible 0x1F30, transformSet 0x1F38.
- ARK_MSG ids renumbered (equip 0xA2, unequip 0xA3, fire 0xCE, impulse 0x100, door push 0x13E, has-choked 0x144, ragdoll 0xBE); message queue +0xCFF0; entity table game+0x327770.
- idRenderSystemLocal: 5 more virtuals (slot 0x200 -> 0x228). Player melee vtable: one extra slot at 34.
- PlayerState: 102 states (DOTO's 11 new ones added), resolved by RTTI name. Render passes: 52 + UPDATE_CLOTH.
- GrabHand: DOTO throws a held object from two sites (both patched).
- Config file: `Saved Games\Arkane Studios\Dishonored_DO\base\dishonored_DOConfig.cfg`.

## Open items (`TODO(DOTO)` in the source)

Features whose byte checks fail stay OFF (logged), they do not patch the wrong code.

| Where | What |
|---|---|
| ThrowGesture.cpp:15, :25, :443, ThrowGesture.h:14 | grenade throw by hand: DOTO's Throw (exe+0x687BA0) launches through messages; the velocity hook needs new RE. Mine typeinfo unmapped. **Off.** |
| Undetected.cpp:11 | the "undetected" cheat: DOTO's relationship code is restructured. **Off.** |
| MeleeAim.cpp:1301 | finisher store patched as `EB 14` (DOTO's code differs): check in game |
| GpuTrace.cpp:144, 4115, 4215, 4241, 4262, 5177; PostShare.cpp:12-13; WristHud.cpp:311 | shader hashes measured on Dishonored 2; check with the `gpudump` / `hudprobe` logs |
| SeedsData.inc | learned hand grips, keyed by D2 vtables and Corvo/Emily hand meshes: regenerate with `tools/make_seeds.py` after a DOTO session |
| Collision.cpp:521 | loot / item decl / inventory offsets and item type numbers |
| BodyHide.h:24, :62, :64 | entity model B 0x378 (inferred), weapon model B 0x480, equipment +0x68/+0x70 |
| GunRay.cpp:369, :381; Probes.cpp:261, :358; ObjectiveWatch.cpp:17; TestAids.cpp:447, TestAids.h:26/37/40 | research / cheat offsets |

## Not yet DOTO-specific

- Billie's powers (Displace, Foresight, Semblance) have no dedicated support yet: the Dishonored 2 power code paths that exist in DOTO were ported, the ones that don't are dormant.
- `docs/ARCHITECTURE.md`, `ENGINE.md`, `STATUS.md`, `DEVELOPMENT.md` and `package/README.txt` still describe Dishonored 2's addresses and gameplay.
