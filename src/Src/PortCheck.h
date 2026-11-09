#pragma once
#include <cstdint>
#include <string>

//! DOTO port verification ([System.Troubleshooting] PortCheck = 1; written to DOTOVR.log, so WriteLog or DebugLogging
//! must be on too). The mod's addresses and struct offsets were ported from Dishonored 2 by matching the code of the two
//! executables (tools/port); this module writes what is needed to confirm each part in the running game, and to fill in
//! what could not be ported statically (docs/DOTO_PORT.md lists those). Every line starts with "PortCheck".
//!  - Hook table: every named code address of the mod (PortCheckTable.inc, generated) compared with the running exe's
//!    bytes before any hook is installed (MISMATCH = another game build, or a wrong address), and again after the hooks
//!    are in (which of them the mod patched, per module: a hook that should be there and is not shows as "untouched").
//!  - Shader census: each shader hash measured on Dishonored 2 that the mod relies on, seen or not since the game
//!    started; and at each place one is compared (Site), the hashes actually met there with how often -- when a
//!    Dishonored 2 hash is never seen, its DOTO counterpart is among those.
//!  - Field checks in play: the core ported fields (game, player, camera, render view) checked for plausible values,
//!    with their objects' RTTI class names; plus the values modules report for their unverified offsets (Field).
//!  - Throw probe: who sets an entity's velocity (exe+0x8E9C20) and for what class -- DOTO's grenade launch site, which
//!    ThrowGesture's throw-by-hand still needs.
namespace PortCheck
{
    //! The places where the mod compares a shader with a hash measured on Dishonored 2.
    enum Site : int
    {
        kSiteDepthOnly,      //!< GpuTrace DepthOnlyBegin: PS of the depth pass (expects 48f2037fd43d86c8)
        kSiteDepthCopy,      //!< GpuTrace depth copies: PS (904e7f2d94d6f085 copy, b152af85c03cbbaf DOF depth)
        kSiteSneakOverlay,   //!< GpuTrace SneakFxSkip: PS of a POSTPROC draw while sneaking (2ee2b9822f61d7d1)
        kSiteAo,             //!< GpuTrace SSAO task: PS (7012fd32e80f486e DWAO, ea85def507442b03 fill)
        kSiteTrail,          //!< GpuTrace: VS of draws during the player's Slash (the sword trail's three)
        kSitePostDraw,       //!< PostShare outlines: PS (075df32c84f09477 mask, 2d11c3a17106ea42 blur, d18f70002e2ca13a compose)
        kSitePostDispatch,   //!< PostShare exposure: CS (dc6d8bbce02a1266 hist, 9ba51c0d902365b7 merge, 577946c9f0ee49c9 adapt)
        kSiteHudVs,          //!< WristHud: VS of the interface's untextured fills (7acc1a9a27c4d9e2)
        kSiteCount
    };

    void Configure(bool on);              //!< from the ini, before Startup
    bool On();
    void Startup();                        //!< init thread, before any hook is installed: the code addresses vs the exe
    void AfterHooks(const char* when);     //!< after the hooks are installed: what is patched now
    void OnShaderCreated(uint64_t hash);   //!< GpuTrace's Create*Shader hooks (any thread)
    void Candidate(Site s, uint64_t hash); //!< a shader met where a Dishonored 2 hash is compared (any thread, cheap)
    void OnPresent();                      //!< render thread, every Present: the periodic reports
    //! A module's value for a field whose DOTO offset is not verified: the latest value is kept (with a count) and
    //! written with the next report; the first one of each `what` at once.
    void Field(const char* what, const std::string& value);
    //! The RTTI class name of a polymorphic object of the game (".?AVarkPlayer@@" -> "arkPlayer"); "" = not one / unreadable.
    std::string ClassName(const void* obj);
    std::string Status();                  //!< one line for the heartbeat / overlay
}
