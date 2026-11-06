#include "pch.h"
#include "PointerRay.h"
#include "GpuTrace.h"
#include "HandAimHook.h"
#include "KbmInject.h"
#include "ViewMatrixHook.h"
#include "VRRuntime.h"
#include "XRSession.h"
#include "GrabHand.h"
#include "VRMath.h"
#include <cmath>
#include <sstream>

namespace
{
    constexpr float kR2D = 57.2957795f, kD2R = 0.0174532925f, kTau = 6.28318531f;
    std::atomic<int> g_Mode{ 2 };
    std::atomic<int> g_LogFrames{ 0 };
    std::atomic<bool> g_LogStart{ false };

    // One Euro filter (Casiez et al. 2012): an exponential low-pass whose cut-off rises with the signal's speed. Still,
    // a low cut-off removes the tremor; moving, a high one keeps the lag down. Speeds are rad/s for the direction (a
    // unit vector's rate of change) and m/s for the origin.
    struct Cfg
    {
        float minCutoff = 1.0f, beta = 2.0f, dCutoff = 1.0f;      // direction
        float posMinCutoff = 2.0f, posBeta = 2.0f;                // origin
        float resetDeg = 15.f, resetM = 0.3f;                     // a jump this big in one frame (a snap turn, a load) is taken whole
    } C;
    struct Named { const char* name; float* v; };
    const Named kCfg[] = { { "mincutoff", &C.minCutoff }, { "beta", &C.beta }, { "dcutoff", &C.dCutoff },
                           { "poscutoff", &C.posMinCutoff }, { "posbeta", &C.posBeta }, { "resetdeg", &C.resetDeg }, { "resetm", &C.resetM } };

    float Alpha(float cutoff, float dt) { const float tau = 1.f / (kTau * cutoff); return 1.f / (1.f + tau / dt); }
    struct OneEuro3
    {
        float x[3] = {}, dx[3] = {}; bool init = false;
        void Reset(const float* v) { memcpy(x, v, 12); memset(dx, 0, 12); init = true; }
        void Step(const float* v, float dt, float minCutoff, float beta, float dCutoff)
        {
            if (!init) { Reset(v); return; }
            const float ad = Alpha(dCutoff, dt);
            float speed = 0.f;
            for (int i = 0; i < 3; i++) { const float d = (v[i] - x[i]) / dt; dx[i] += (d - dx[i]) * ad; speed += dx[i] * dx[i]; }
            const float a = Alpha(minCutoff + beta * sqrtf(speed), dt);
            for (int i = 0; i < 3; i++) x[i] += (v[i] - x[i]) * a;
        }
    };

    struct HandState { OneEuro3 org, dir; bool valid = false; float outOrg[3] = {}, outFwd[3] = {}; float rawOrg[3] = {}, rawFwd[3] = {}; };
    HandState g_H[2];                 // Present thread
    std::mutex g_M;                   // the published rays
    struct Pub { bool valid = false; float org[3] = {}, fwd[3] = {}; } g_Pub[2];
    LARGE_INTEGER g_LastT{};
    std::atomic<unsigned long long> g_Frames{ 0 }, g_Anchored{ 0 }, g_Fallback{ 0 }, g_Resets{ 0 };
    std::atomic<float> g_LastOffset{ 0.f };
    struct ScanState
    {
        bool on = false; float yaw0 = 0, yaw1 = 0, pitch0 = 0, pitch1 = 0, step = 5; int settle = 8;
        float y = 0, p = 0; int frame = 0; float baseYaw = 0; float org[3] = {}; int cells = 0, picks = 0;
    } g_Scan;                         // Present thread
    std::atomic<bool> g_ScanReq{ false }; ScanState g_ScanNext;

    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    void Normalize(float* v) { const float n = sqrtf(Dot(v, v)); if (n > 1e-6f) for (int i = 0; i < 3; i++) v[i] /= n; }
    //! Yaw (radians, positive = left) of an XR orientation's forward (-Z).
    float XrYaw(const float* q) { float f[3], r[3], u[3]; VRMath::QuatBasis(q, f, r, u); return atan2f(-f[0], -f[2]); }

    //! The unfiltered pointer: the controller placed against the RENDERED view (so the origin is where the hand is
    //! drawn), turned by the world anchor instead of the camera's current yaw.
    bool RawPose(int hand, int mode, float org[3], float fwd[3])
    {
        float axis[9];
        if (mode == 0)
        {
            if (!HandAimHook::ControllerPose(hand, org, axis)) return false;
            memcpy(fwd, axis, 12); return true;
        }
        float vm[16], vorg[3], vaxis[9];
        unsigned long long serial = 0;
        // (the head's axis, not the eye's: a stereo eye's view is turned to its frustum's centre / canted, see RenderView)
        float eaxis[9];
        const bool haveView = ViewMatrixHook::RenderView(vm, vorg, eaxis, &serial, vaxis);
        // the eye and the poses of THAT view (its record), not the eye of whatever frame is being built now
        const ViewMatrixHook::XrPose rec = haveView ? ViewMatrixHook::RenderPoseXr(serial) : ViewMatrixHook::XrPose();
        const int eye = rec.valid ? rec.eye : VRRuntime::CurrentEye();
        VRRuntime::PoseSet vset;
        const bool fromView = haveView && GpuTrace::HandsFromView() && ViewMatrixHook::ViewPoses(serial, vset) && vset.inputValid;
        const VRRuntime::HeadPose ep = fromView ? (eye >= 0 ? vset.eye[eye] : vset.head) : eye >= 0 ? VRRuntime::GetEyePose(eye) : VRRuntime::GetHeadPose();
        if (!haveView || !ep.valid)
        {   // no rendered view yet: the camera-anchored pose is still a pose
            g_Fallback++;
            if (!HandAimHook::ControllerPose(hand, org, axis)) return false;
            memcpy(fwd, axis, 12); return true;
        }
        float yaw = atan2f(vaxis[1], vaxis[0]);
        float off = 0.f;
        const VRRuntime::HeadPose head = fromView ? vset.head : VRRuntime::GetHeadPose();
        if (head.valid && KbmInject::WorldYawOffset(off)) { yaw = off * kD2R + XrYaw(head.quat); g_Anchored++; g_LastOffset = off; }
        // ControllerPoseAt only takes the YAW of the axis it is given (the hand keeps its own pitch and roll)
        const float c = cosf(yaw), s = sinf(yaw);
        const float ya[9] = { c, s, 0.f, -s, c, 0.f, 0.f, 0.f, 1.f };
        if (!HandAimHook::ControllerPoseAt(hand, vorg, ya, ep.pos, org, axis, fromView ? &vset.input : nullptr, fromView ? vset.head.quat : nullptr)) return false;
        memcpy(fwd, axis, 12);
        return true;
    }

    //! Yaw/pitch (degrees) of a world direction (id axes: x fwd, y left, z up).
    void Angles(const float* d, float& yaw, float& pitch)
    {
        yaw = atan2f(d[1], d[0]) * kR2D;
        pitch = atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * kR2D;
    }
    void XrAngles(const float* q, float& yaw, float& pitch)
    {
        float f[3], r[3], u[3]; VRMath::QuatBasis(q, f, r, u);
        yaw = atan2f(-f[0], -f[2]) * kR2D;
        pitch = asinf(f[1] < -1.f ? -1.f : f[1] > 1.f ? 1.f : f[1]) * kR2D;
    }
    void LogFrame()
    {
        LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        const double ms = (double)q.QuadPart * 1000.0 / (double)f.QuadPart;
        float hy = 0, hp = 0, cy = 0, cp = 0, gy = 0, gp = 0, ky = 0, kp = 0, xy = 0, xp = 0, py = 0, pp = 0, ry = 0, rp = 0;
        const VRRuntime::HeadPose head = VRRuntime::GetHeadPose();
        if (head.valid) XrAngles(head.quat, hy, hp);
        float co[3] = {}, ca[9] = {};
        const bool cam = HandAimHook::RealCamera(co, ca);
        if (cam) Angles(ca, cy, cp);
        float go[3] = {}, gf[3] = {};
        const bool gpu = GpuTrace::HandRay(1, go, gf);
        if (gpu) Angles(gf, gy, gp);
        float ko[3] = {}, ka[9] = {};
        const bool ctrl = HandAimHook::ControllerPose(1, ko, ka);
        if (ctrl) Angles(ka, ky, kp);
        XRInputState in;
        if (VRRuntime::GetInput(in) && in.hand[1].valid)
        {
            const float hq[4] = { in.hand[1].quatX, in.hand[1].quatY, in.hand[1].quatZ, in.hand[1].quatW };
            XrAngles(hq, xy, xp);
        }
        const HandState& h = g_H[1];
        if (h.valid) { Angles(h.outFwd, py, pp); Angles(h.rawFwd, ry, rp); }
        // Where the reticle draws: the pointer's point 2 m out, and a world point frozen at the first logged frame
        // (it must stay put on screen while the head is still, whatever the hand does), in NDC.
        static float fixedW[3];
        float endW[3] = {}, endN[2] = { 9, 9 }, fixN[2] = { 9, 9 };
        if (h.valid) for (int i = 0; i < 3; i++) endW[i] = h.outOrg[i] + h.outFwd[i] * 2.f;
        if (g_LogStart.exchange(false)) memcpy(fixedW, endW, 12);
        const bool pe = h.valid && GpuTrace::ProjectWorld(endW, endN), pfx = GpuTrace::ProjectWorld(fixedW, fixN);
        LOGI("raylog {:.1f} hmd {:.2f} {:.2f} cam {} {:.2f} {:.2f} gpu {} {:.2f} {:.2f} ({:.3f},{:.3f},{:.3f}) ctrl {} {:.2f} {:.2f} ({:.3f},{:.3f},{:.3f}) xr {:.2f} {:.2f} "
             "raw {:.3f} {:.3f} ({:.3f},{:.3f},{:.3f}) ptr {} {:.3f} {:.3f} ({:.3f},{:.3f},{:.3f}) eye {} ndcend {} {:.4f} {:.4f} ndcfix {} {:.4f} {:.4f}",
             ms, hy, hp, cam ? 1 : 0, cy, cp, gpu ? 1 : 0, gy, gp, go[0], go[1], go[2], ctrl ? 1 : 0, ky, kp, ko[0], ko[1], ko[2], xy, xp,
             ry, rp, h.rawOrg[0], h.rawOrg[1], h.rawOrg[2], h.valid ? 1 : 0, py, pp, h.outOrg[0], h.outOrg[1], h.outOrg[2], VRRuntime::CurrentEye(),
             pe ? 1 : 0, endN[0], endN[1], pfx ? 1 : 0, fixN[0], fixN[1]);
    }
}

void PointerRay::OnPresent()
{
    g_Frames++;
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    float dt = g_LastT.QuadPart ? (float)((double)(now.QuadPart - g_LastT.QuadPart) / (double)fq.QuadPart) : 0.f;
    g_LastT = now;
    const bool gap = dt <= 0.f || dt > 0.25f;   // first frame, a load, a stall: start the filters over
    dt = dt < 0.001f ? 0.001f : dt > 0.1f ? 0.1f : dt;
    const int mode = g_Mode.load();
    Pub pub[2];
    for (int hd = 0; hd < 2; hd++)
    {
        HandState& h = g_H[hd];
        float o[3], f[3];
        if (!RawPose(hd, mode, o, f)) { h.valid = false; h.org.init = h.dir.init = false; continue; }
        Normalize(f);
        memcpy(h.rawOrg, o, 12); memcpy(h.rawFwd, f, 12);
        if (mode < 2) { memcpy(h.outOrg, o, 12); memcpy(h.outFwd, f, 12); }
        else
        {
            bool jump = gap || !h.dir.init;
            if (!jump)
            {
                const float d[3] = { o[0] - h.org.x[0], o[1] - h.org.x[1], o[2] - h.org.x[2] };
                const float c = Dot(f, h.dir.x) / sqrtf(Dot(h.dir.x, h.dir.x) + 1e-12f);
                jump = sqrtf(Dot(d, d)) > C.resetM || c < cosf(C.resetDeg * kD2R);
                if (jump) g_Resets++;
            }
            if (jump) { h.org.Reset(o); h.dir.Reset(f); }
            else
            {
                h.org.Step(o, dt, C.posMinCutoff, C.posBeta, C.dCutoff);
                h.dir.Step(f, dt, C.minCutoff, C.beta, C.dCutoff);
            }
            memcpy(h.outOrg, h.org.x, 12); memcpy(h.outFwd, h.dir.x, 12); Normalize(h.outFwd);
        }
        h.valid = true;
        pub[hd].valid = true; memcpy(pub[hd].org, h.outOrg, 12); memcpy(pub[hd].fwd, h.outFwd, 12);
    }
    if (g_ScanReq.exchange(false))
    {
        g_Scan = g_ScanNext;
        float co[3], ca[9];
        g_Scan.on = pub[1].valid && HandAimHook::RealCamera(co, ca);
        if (g_Scan.on)
        {
            g_Scan.baseYaw = atan2f(ca[1], ca[0]) * kR2D; memcpy(g_Scan.org, pub[1].org, 12);
            g_Scan.y = g_Scan.yaw0; g_Scan.p = g_Scan.pitch1; g_Scan.frame = 0; g_Scan.cells = g_Scan.picks = 0;
            LOGI("pickscan: from ({:.3f},{:.3f},{:.3f}), camera yaw {:.1f}, yaw {:.0f}..{:.0f} pitch {:.0f}..{:.0f} step {:.1f}, {} frames each",
                 g_Scan.org[0], g_Scan.org[1], g_Scan.org[2], g_Scan.baseYaw, g_Scan.yaw0, g_Scan.yaw1, g_Scan.pitch0, g_Scan.pitch1, g_Scan.step, g_Scan.settle);
        }
        else LOGI("pickscan: no right-hand pointer or camera now");
    }
    if (g_Scan.on)
    {   // the pointer is the grid's current direction, from where the hand was when the scan began
        const float yr = (g_Scan.baseYaw + g_Scan.y) * kD2R, pr = g_Scan.p * kD2R;
        pub[1].valid = true; memcpy(pub[1].org, g_Scan.org, 12);
        pub[1].fwd[0] = cosf(pr) * cosf(yr); pub[1].fwd[1] = cosf(pr) * sinf(yr); pub[1].fwd[2] = sinf(pr);
        if (++g_Scan.frame >= g_Scan.settle)
        {
            float tp[3] = {};
            const int id = GrabHand::TargetId();
            const bool have = GrabHand::TargetPoint(tp);
            g_Scan.cells++; if (have) g_Scan.picks++;
            LOGI("pickscan cell {:.1f} {:.1f} id {:#x} at ({:.3f},{:.3f},{:.3f})", g_Scan.y, g_Scan.p, id, tp[0], tp[1], tp[2]);
            g_Scan.frame = 0;
            g_Scan.y += g_Scan.step;
            if (g_Scan.y > g_Scan.yaw1 + 1e-3f)
            {
                g_Scan.y = g_Scan.yaw0; g_Scan.p -= g_Scan.step;
                if (g_Scan.p < g_Scan.pitch0 - 1e-3f) { g_Scan.on = false; LOGI("pickscan: done, {} cells, {} with a pick", g_Scan.cells, g_Scan.picks); }
            }
        }
    }
    { std::lock_guard lk(g_M); g_Pub[0] = pub[0]; g_Pub[1] = pub[1]; }
    if (g_LogFrames.load() > 0) { g_LogFrames--; LogFrame(); }
}

bool PointerRay::Get(int hand, float org[3], float fwd[3])
{
    std::lock_guard lk(g_M);
    const Pub& p = g_Pub[hand & 1];
    if (!p.valid) return false;
    memcpy(org, p.org, 12); memcpy(fwd, p.fwd, 12);
    return true;
}
void PointerRay::SetMode(int m) { g_Mode = m < 0 ? 0 : m > 2 ? 2 : m; LOGI("PointerRay: mode {}", g_Mode.load()); }
int PointerRay::Mode() { return g_Mode.load(); }
bool PointerRay::Set(const std::string& key, float v)
{
    for (const Named& n : kCfg) if (key == n.name) { *n.v = v; return true; }
    return false;
}
std::string PointerRay::Config()
{
    std::ostringstream o;
    o << "mode " << g_Mode.load();
    for (const Named& n : kCfg) o << "  " << n.name << " " << *n.v;
    return o.str();
}
std::string PointerRay::Stats()
{
    std::ostringstream o;
    o << "mode " << g_Mode.load() << " frames " << g_Frames.load() << " anchored " << g_Anchored.load() << " fallback " << g_Fallback.load()
      << " resets " << g_Resets.load() << " yawoffset " << g_LastOffset.load() << " valid " << (g_H[0].valid ? 1 : 0) << (g_H[1].valid ? 1 : 0);
    return o.str();
}
void PointerRay::Scan(float yawFrom, float yawTo, float pitchFrom, float pitchTo, float step, int settle)
{
    g_ScanNext = {}; g_ScanNext.yaw0 = yawFrom; g_ScanNext.yaw1 = yawTo; g_ScanNext.pitch0 = pitchFrom; g_ScanNext.pitch1 = pitchTo;
    g_ScanNext.step = step > 0.2f ? step : 0.2f; g_ScanNext.settle = settle < 2 ? 2 : settle;
    g_ScanReq = true;
}
void PointerRay::Log(int frames) { g_LogFrames = frames < 0 ? 0 : frames; g_LogStart = true; LOGI("PointerRay: logging {} frames", frames); }
