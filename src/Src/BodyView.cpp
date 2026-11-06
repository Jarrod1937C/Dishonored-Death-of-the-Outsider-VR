#include "pch.h"
#include "BodyView.h"
#include "GpuTrace.h"
#include "VRRuntime.h"
#include "XRSession.h"
#include "CameraHook.h"
#include "BodyHide.h"
#include "GameInfo.h"
#include "Overlay.h"
#include "PipeTrace.h"
#include "ViewMatrixHook.h"
#include <fstream>
#include <cmath>
#include <map>
#include <algorithm>

namespace
{
    // The REAL body the drawn one is compared with. Adult anthropometry (~1.75 m), relative to the HMD's centre eye in
    // its yaw-only frame: the shoulder joint (glenohumeral centre) sits ~23 cm below the eyes, ~18.5 cm to the side and
    // ~9 cm behind them; shoulder->elbow 0.30 m, elbow->wrist 0.26 m. The wrist from the controller's AIM pose: the
    // aim origin is at the front of a Touch controller, the wrist ~11 cm behind it and ~3 cm below. All settable
    // (`vr bodycfg`), because the user's own body is the reference.
    std::map<std::string, float> g_Cfg = {
        { "shfwd", -0.09f }, { "shout", 0.185f }, { "shup", -0.23f },
        { "upper", 0.30f }, { "fore", 0.26f },
        { "wristback", 0.11f }, { "wristdown", 0.03f },
        { "polefwd", -0.2f }, { "poleout", 0.4f }, { "poleup", -0.8f },
    };
    float C(const char* k) { return g_Cfg[k]; }

    void Sub(const float a[3], const float b[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a[i] - b[i]; }
    float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float Norm(float v[3]) { const float n = sqrtf(Dot(v, v)); if (n > 1e-9f) for (int i = 0; i < 3; i++) v[i] /= n; return n; }
    void Cross(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

    //! Rotate v by quaternion q (x,y,z,w).
    void QRot(const float q[4], const float v[3], float o[3])
    {
        const float u[3] = { q[0], q[1], q[2] }, w = q[3];
        float c1[3], c2[3]; Cross(u, v, c1); Cross(u, c1, c2);
        for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
    }
    void QRotInv(const float q[4], const float v[3], float o[3]) { const float c[4] = { -q[0], -q[1], -q[2], q[3] }; QRot(c, v, o); }

    //! Two-bone IK, the same construction GpuTrace uses: elbow on the circle where both segments reach, towards the pole.
    void Ik(const float S[3], const float W[3], float L1, float L2, const float poleIn[3], float E[3], float Sout[3], bool& stretched)
    {
        float along[3]; Sub(W, S, along); float d = Norm(along);
        memcpy(Sout, S, 12); stretched = false;
        const float maxr = 0.995f * (L1 + L2);
        if (d > maxr) { for (int i = 0; i < 3; i++) Sout[i] = W[i] - along[i] * maxr; d = maxr; stretched = true; }
        const float ca = std::clamp((L1 * L1 + d * d - L2 * L2) / (2.f * L1 * d), -1.f, 1.f), sa = sqrtf(std::max(0.f, 1.f - ca * ca));
        float pole[3] = { poleIn[0], poleIn[1], poleIn[2] }; Norm(pole);
        float perp[3]; const float pa = Dot(pole, along); for (int i = 0; i < 3; i++) perp[i] = pole[i] - along[i] * pa; Norm(perp);
        for (int i = 0; i < 3; i++) E[i] = Sout[i] + along[i] * (L1 * ca) + perp[i] * (L1 * sa);
    }

    std::string P3(const char* name, const float v[3])
    {
        char b[160]; snprintf(b, sizeof b, "%s %.4f %.4f %.4f", name, v[0], v[1], v[2]); return b;
    }
}

std::string BodyView::Config()
{
    std::string s;
    for (auto& kv : g_Cfg) { char b[64]; snprintf(b, sizeof b, "%s %.3f ", kv.first.c_str(), kv.second); s += b; }
    return s;
}

void BodyView::Dump(const std::string& tag)
{
    std::ostringstream f;
    f.setf(std::ios::fixed); f.precision(5);
    f << "frame " << GpuTrace::PresentFrame() << " eye " << VRRuntime::CurrentEye() << "\n";
    f << "anatomy " << Config() << "\n";

    // the render view: world -> view (x right, y up, z forward)
    float vr[9], vt[3], vorg[3];
    const bool haveView = GpuTrace::GetRenderView(vr, vt, vorg);
    f << "view " << (haveView ? 1 : 0);
    if (haveView) { for (float x : vr) f << " " << x; f << " t"; for (float x : vt) f << " " << x; }
    f << "\n";
    // With alternating-eye stereo the render view sits at ONE eye; every point is reported from the head's centre, so
    // the annotator can place it for either eye. The eye this view was rendered for comes from its own record.
    float eyeShift[3] = {};
    {
        unsigned long long serial = 0, age = 0;
        const ViewMatrixHook::XrPose rp = ViewMatrixHook::RenderPoseXr(GpuTrace::DrawViewSerial(serial, age) ? serial : 0);
        const VRRuntime::HeadPose head = VRRuntime::GetHeadPose();
        if (rp.valid && rp.eye >= 0 && head.valid)
        {   // head centre relative to the rendering eye, in the view frame (x right, y up, z forward)
            const float d[3] = { head.pos[0] - rp.pos[0], head.pos[1] - rp.pos[1], head.pos[2] - rp.pos[2] };
            float l[3]; QRotInv(head.quat, d, l);
            eyeShift[0] = l[0]; eyeShift[1] = l[1]; eyeShift[2] = -l[2];
        }
        f << "render_eye " << rp.eye << " head_from_eye " << eyeShift[0] << " " << eyeShift[1] << " " << eyeShift[2] << " stereo " << VRRuntime::Stereo() << "\n";
    }
    auto toView = [&](const float w[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = vr[i * 3] * w[0] + vr[i * 3 + 1] * w[1] + vr[i * 3 + 2] * w[2] + vt[i] - eyeShift[i]; };

    // the projection the player's own draws used, and the engine's renderView fov
    float P[4][4];
    if (GpuTrace::GetPlayerProjection(P))
        f << "proj_player fx " << P[0][0] << " fy " << P[1][1] << " cx " << P[0][2] << " cy " << P[1][2] << " z " << P[2][2] << " " << P[2][3]
          << " fov " << 2 * atanf(1.f / P[0][0]) * 57.29578f << " " << 2 * atanf(1.f / P[1][1]) * 57.29578f << "\n";
    else f << "proj_player none\n";
    {
        auto cs = CameraHook::GetStats();
        f << "rv_fov " << cs.lastFovX * 57.29578f << " " << cs.lastFovY * 57.29578f << " size " << cs.lastW << " " << cs.lastH << "\n";
        float gx, gy, bx, by, ox, oy; unsigned long long n; CameraHook::GameFov(gx, gy); CameraHook::BuiltFov(bx, by, n); CameraHook::FovOverride(ox, oy);
        f << "fov game " << gx << " " << gy << " built " << bx << " " << by << " override " << ox << " " << oy << " (" << n << " frames) ortho "
          << VRRuntime::Ortho() << " draws " << VRRuntime::OrthoDraws() << "\n";
        const VRRuntime::RenderPoseStats rp = VRRuntime::GetRenderPoseStats();
        f << "renderpose on " << VRRuntime::TruePose() << " used " << rp.used << " fromdraws " << rp.fromDraws << " fromexec " << rp.fromExec << " gameaxis " << rp.gameAxis
          << " behindlatest " << rp.behindLatest << " phasefix " << rp.phaseFix << " stereo " << VRRuntime::Stereo() << " diff " << rp.lastDiff << " max " << rp.maxDiff << " deg\n";
    }
    const std::string census = GpuTrace::ProjCensusText();
    if (!census.empty()) { std::istringstream cs(census); std::string line; while (std::getline(cs, line)) f << "census " << line << "\n"; }

    // XR poses (LOCAL space)
    const VRRuntime::HeadPose hp = VRRuntime::GetHeadPose();
    f << "xr_head " << hp.valid << " q " << hp.quat[0] << " " << hp.quat[1] << " " << hp.quat[2] << " " << hp.quat[3]
      << " p " << hp.pos[0] << " " << hp.pos[1] << " " << hp.pos[2] << "\n";
    for (int e = 0; e < 2; e++)
    {
        const VRRuntime::HeadPose ep = VRRuntime::GetEyePose(e);
        f << "xr_eye " << e << " " << ep.valid << " q " << ep.quat[0] << " " << ep.quat[1] << " " << ep.quat[2] << " " << ep.quat[3]
          << " p " << ep.pos[0] << " " << ep.pos[1] << " " << ep.pos[2] << "\n";
    }
    for (int e = 0; e < 2; e++)
    {
        const VRRuntime::Placement pl = VRRuntime::GetPlacement(e);
        f << "place " << e << " " << pl.valid << " src " << pl.srcW << " " << pl.srcH << " rect " << pl.rect[0] << " " << pl.rect[1] << " "
          << pl.rect[2] << " " << pl.rect[3] << " swap " << pl.swap[0] << " " << pl.swap[1] << " fov " << pl.fov[0] * 57.29578f << " "
          << pl.fov[1] * 57.29578f << " " << pl.fov[2] * 57.29578f << " " << pl.fov[3] * 57.29578f << "\n";
    }
    XRInputState in{}; const bool haveIn = VRRuntime::GetInput(in);

    // the game's eye height: render view origin above the player entity's origin (its feet)
    {
        BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
        if (an[BodyHide::aPlayer].valid && haveView)
            f << "game_eye_height " << vorg[2] - an[BodyHide::aPlayer].org[2] << " feet " << an[BodyHide::aPlayer].org[0] << " "
              << an[BodyHide::aPlayer].org[1] << " " << an[BodyHide::aPlayer].org[2] << " view_org " << vorg[0] << " " << vorg[1] << " " << vorg[2] << "\n";
    }

    // XR LOCAL -> the view frame of the HMD (x right, y up, z forward), which is what the layer pose stands for
    auto xrToView = [&](const float p[3], float o[3]) {
        float d[3]; Sub(p, hp.pos, d); float l[3]; QRotInv(hp.quat, d, l); o[0] = l[0]; o[1] = l[1]; o[2] = -l[2]; };
    // the head's yaw-only frame in XR: right, up, back
    float yawRight[3], yawBack[3]; const float up[3] = { 0, 1, 0 };
    {
        const float fz[3] = { 0, 0, -1 }; float fwd[3]; QRot(hp.quat, fz, fwd); fwd[1] = 0; Norm(fwd);
        yawBack[0] = -fwd[0]; yawBack[1] = 0; yawBack[2] = -fwd[2];
        Cross(fwd, up, yawRight); Norm(yawRight);
    }

    for (int side = 0; side < 2; side++)
    {
        f << "side " << side << "\n";
        GpuTrace::ArmJoints J;
        if (GpuTrace::GetArmJoints(side, J) && haveView)
        {
            float v[3];
            f << "  game L1 " << J.L1 << " L2 " << J.L2 << " reach " << J.reach << " stretched " << J.stretched << "\n";
            const struct { const char* n; const float* p; } pts[] = {
                { "S_anim", J.S }, { "E_anim", J.E }, { "W_anim", J.W }, { "S", J.Sp }, { "E", J.Ep }, { "W", J.Wp }, { "ctrl", J.ctrl }, { "head", J.head } };
            for (auto& q : pts) { toView(q.p, v); f << "  game " << P3(q.n, v) << "\n"; }
        }
        else f << "  game none\n";
        {
            float org[3], rot[9], fing[3];
            if (GpuTrace::PalmFrame(side, org, rot, fing) && haveView) { float v[3]; toView(org, v); f << "  game " << P3("palm", v) << "\n"; }
        }
        if (haveIn && hp.valid && in.hand[side].valid)
        {
            const XRHandPose& h = in.hand[side];
            const float q[4] = { h.quatX, h.quatY, h.quatZ, h.quatW }, cp[3] = { h.posX, h.posY, h.posZ };
            const float sgn = side == 0 ? -1.f : 1.f;    // left shoulder is to the LEFT (-right)
            float S[3], W[3];
            for (int i = 0; i < 3; i++)
                S[i] = hp.pos[i] + yawRight[i] * (sgn * C("shout")) + up[i] * C("shup") - yawBack[i] * C("shfwd");
            {   // wrist: behind and below the aim origin, in the controller's frame (+z is back, +y up)
                const float lw[3] = { 0, -C("wristdown"), C("wristback") }; float o[3]; QRot(q, lw, o);
                for (int i = 0; i < 3; i++) W[i] = cp[i] + o[i];
            }
            float pole[3]; for (int i = 0; i < 3; i++) pole[i] = yawRight[i] * (sgn * C("poleout")) + up[i] * C("poleup") - yawBack[i] * C("polefwd");
            float E[3], S2[3]; bool st = false; Ik(S, W, C("upper"), C("fore"), pole, E, S2, st);
            float dSW[3]; Sub(W, S, dSW);
            f << "  real upper " << C("upper") << " fore " << C("fore") << " reach " << sqrtf(Dot(dSW, dSW)) << " stretched " << st << "\n";
            float v[3];
            xrToView(S, v); f << "  real " << P3("S", v) << "\n";
            xrToView(E, v); f << "  real " << P3("E", v) << "\n";
            xrToView(W, v); f << "  real " << P3("W", v) << "\n";
            xrToView(cp, v); f << "  real " << P3("ctrl", v) << "\n";
        }
        else f << "  real none\n";
    }
    const std::string text = f.str();
    std::ofstream out(GameInfo::GameDir() + L"\\DVR_bodyview_" + std::wstring(tag.begin(), tag.end()) + L".txt");
    out << text;
    LOGI("bodyview '{}':\n{}", tag, text);
}

namespace
{
    // `vr fov auto` is a MODE, not a one-shot: the package can switch on before the headset has given its eyes' fov
    // (seen on the Quest 3, 2026-10-14: the game kept its own 90 deg for the whole session), and a new session can
    // bring another headset. While it is on, OnPresent applies it as soon as the fov is known and again if it changes.
    std::atomic<bool> g_FovAutoOn{ false };
    std::atomic<float> g_FovMargin{ 0.f };
    float g_FovSet[2] = { 0.f, 0.f };   // what was last applied (Present thread)

    bool FovApply()
    {   // the smallest symmetric fov that covers both eyes' fov: the mono image is shown to both, and a stereo image is
        // drawn into each eye at its own angles (XRSession::SetOrtho), so nothing either eye can see is left black.
        // The eyes' fovs are taken into the HEAD's frame (CameraHook::UnionTans): on canted displays each eye looks ~10
        // deg out, and its native fov alone left a black wedge on the outside of each eye.
        float tx = 0, ty = 0;
        CameraHook::Tans u;
        if (CameraHook::UnionTans(u)) { tx = std::max(-u.l, u.r); ty = std::max(u.u, -u.d); }
        // With the asymmetric matrix (per-eye frustum mode 2, research) each eye's view keeps this fov in its renderView:
        // whatever the engine derives from it (culling) must cover that frustum too. (Mode 1 gives each eye's view its
        // own fov in the projection builder's hook; this one is the mono views'.)
        if (CameraHook::EyeProj() == 2 && VRRuntime::Stereo())
            for (int e = 0; e < 2; e++)
            {
                CameraHook::Tans t;
                if (CameraHook::EyeTans(e, t)) { tx = std::max({ tx, -t.l, t.r }); ty = std::max({ ty, t.u, -t.d }); }
            }
        if (tx <= 0.f || ty <= 0.f || !std::isfinite(tx) || !std::isfinite(ty)) return false;
        const float m = g_FovMargin.load();
        // at most 150 deg a side: a very wide headset (Pimax) would otherwise ask the engine for a projection near
        // 180 deg (pixel density at the edges explodes); beyond it the outer edge stays black
        const float x = std::min(2 * atanf(tx) * 57.29578f + m, 150.f), y = std::min(2 * atanf(ty) * 57.29578f + m, 150.f);
        if (fabsf(x - g_FovSet[0]) < 0.05f && fabsf(y - g_FovSet[1]) < 0.05f) return true;
        g_FovSet[0] = x; g_FovSet[1] = y;
        CameraHook::SetFovOverride(x, y);
        return true;
    }
}

bool BodyView::FovAuto(float margin)
{
    g_FovMargin = margin; g_FovSet[0] = g_FovSet[1] = 0.f;
    g_FovAutoOn = true;
    return FovApply();
}

void BodyView::FovAutoOff()
{
    g_FovAutoOn = false; g_FovSet[0] = g_FovSet[1] = 0.f;
}

bool BodyView::FovAutoPending() { return g_FovAutoOn.load() && g_FovSet[0] <= 0.f; }

void BodyView::OnPresent()
{
    static unsigned n = 0;
    if (!g_FovAutoOn.load() || (n++ % 30) != 0) return;
    const bool was = g_FovSet[0] > 0.f;
    if (FovApply() && !was) LOGI("vr fov auto: the headset's fov arrived, render fov applied");
}

bool BodyView::Command(const std::string& what, std::istringstream& ss)
{
    if (what == "bodyview") { std::string tag = "now"; ss >> tag; Dump(tag); return true; }
    if (what == "window")
    {   // vr window <w> <h> (physical px of the game's render) | off
        std::string a; ss >> a; int h = 0; ss >> h;
        if (a == "off" || a == "0") Overlay::SetWindowClient(0, 0); else Overlay::SetWindowClient(atoi(a.c_str()), h);
        LOGI("cmd vr window {} {} (now {})", a, h, Overlay::WindowInfo());
        return true;
    }
    if (what == "pivotfix") { int m = 1; ss >> m; ViewMatrixHook::SetPivotFix(m != 0); return true; }
    if (what == "armlen") { float m = 0.56f, k = 0.f; ss >> m >> k; GpuTrace::SetArmLength(m, k); return true; }
    if (what == "posesource") { int m = 4; ss >> m; VRRuntime::SetPoseSource(m); return true; }
    if (what == "pipetrace") { int n = 6; ss >> n; PipeTrace::Start(n); return true; }
    if (what == "handjit") { int n = 0; ss >> n; GpuTrace::RequestHandJit(n); LOGI("cmd vr handjit {}", n); return true; }
    if (what == "armsolve") { int m = 1; ss >> m; GpuTrace::SetArmSolve(m); return true; }
    if (what == "burst") { int n = 8, k = 1; std::string tag = "burst"; ss >> n >> tag >> k; VRRuntime::RequestBurst(n, tag, k < 1 ? 1 : k); LOGI("cmd vr burst {} {} every {}", n, tag, k); return true; }



    if (what == "ortho") { int m = 1; ss >> m; VRRuntime::SetOrtho(m != 0); return true; }
    if (what == "truepose") { int m = 1; ss >> m; VRRuntime::SetTruePose(m != 0); return true; }
    if (what == "fov")
    {   // vr fov <x> <y> (full angles, degrees) | auto [margin] (cover both eyes' fov) | off (the game's own)
        std::string a; ss >> a;
        if (a == "off" || a == "0") { FovAutoOff(); CameraHook::SetFovOverride(0, 0); return true; }
        if (a == "auto" || a.empty())
        {
            float margin = 0.f; ss >> margin;
            if (!FovAuto(margin)) LOGI("vr fov auto: no eye fov yet, applied when the headset gives it");
            return true;
        }
        FovAutoOff();
        float y = 0; ss >> y;
        CameraHook::SetFovOverride((float)atof(a.c_str()), y);
        return true;
    }
    if (what == "projcensus") { int n = 30; ss >> n; GpuTrace::RequestProjCensus(n); LOGI("cmd vr projcensus {}", n); return true; }
    if (what == "bodycfg")
    {
        std::string k; float v = 0;
        if (ss >> k >> v) { if (g_Cfg.count(k)) g_Cfg[k] = v; else LOGI("bodycfg: unknown {}", k); }
        LOGI("bodycfg: {}", Config());
        return true;
    }
    return false;
}
