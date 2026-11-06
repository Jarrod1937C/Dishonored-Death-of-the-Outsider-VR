// Minimal mock OpenXR runtime for headless development of the Dishonored VR mod.
// It reports a fake stereo HMD, accepts a D3D11 session, hands out real D3D11
// swapchain textures, and no-ops the compositor. Lets the mod's entire OpenXR
// lifecycle (instance -> session -> swapchains -> frame loop -> submit) be
// exercised in-game with no headset and no real runtime. TEST TOOL ONLY.
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <share.h>
#include <mutex>
#include <deque>
#include <atomic>
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "mock_loader_interfaces.h"

namespace
{
constexpr uint32_t kEyeW = 1024, kEyeH = 1024;
constexpr uint32_t kSwapImages = 2;

// Optional HMD emulation: DVR_MockHmd.txt (cwd) = "w h fovL fovR fovU fovD [cant]"
// (fovs in degrees for the LEFT eye; the right eye is mirrored, as real HMDs
// are). Lets the mock reproduce hardware geometry headless — e.g. Quest 3:
// "1824 1968 -52 42 48 -50". Missing file = legacy 1024x1024 symmetric 90.
// cant (degrees, optional): CANTED displays, as a Pimax without parallel projections: each eye's orientation is the
// head's turned `cant` degrees OUT (left eye to the left, right eye to the right), its fov given in that turned frame.
uint32_t gEyeW = kEyeW, gEyeH = kEyeH;
float gFovDeg[4] = { -45.0f, 45.0f, 45.0f, -45.0f }; // L R U D, left eye
float gCantDeg = 0.f;
bool gHmdCfgRead = false;
void ReadHmdConfig()
{
    if (gHmdCfgRead) return;
    gHmdCfgRead = true;
    FILE* f = nullptr;
    fopen_s(&f, "DVR_MockHmd.txt", "r");
    if (!f) return;
    unsigned w = 0, h = 0;
    float l = 0, r = 0, u = 0, d = 0;
    if (fscanf_s(f, "%u %u %f %f %f %f", &w, &h, &l, &r, &u, &d) == 6 && w && h)
    {
        gEyeW = w; gEyeH = h;
        gFovDeg[0] = l; gFovDeg[1] = r; gFovDeg[2] = u; gFovDeg[3] = d;
        float c = 0.f; if (fscanf_s(f, "%f", &c) == 1) gCantDeg = c;
    }
    fclose(f);
}
XrFovf EyeFov(int eye)
{
    const float k = 3.14159265f / 180.0f;
    if (eye == 0)
        return { gFovDeg[0] * k, gFovDeg[1] * k, gFovDeg[2] * k, gFovDeg[3] * k };
    // Right eye: horizontal mirror of the left.
    return { -gFovDeg[1] * k, -gFovDeg[0] * k, gFovDeg[2] * k, gFovDeg[3] * k };
}

struct MockSwapchain
{
    uint32_t w = kEyeW, h = kEyeH;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::vector<ID3D11Texture2D*> tex;
    uint32_t acquired = 0;
};

// Single instance of everything.
struct XrInstance_T { int _; } gInstance;
struct XrSession_T { int _; } gSession;
struct XrSpace_T { int _; } gSpace, gViewSpace;   // LOCAL; VIEW (the head: xrLocateSpace gives its pose)

ID3D11Device* gDevice = nullptr;

// Session state events, delivered one per xrPollEvent call. A small state machine, as a real runtime's: IDLE + READY on
// creation, SYNCHRONIZED + VISIBLE + FOCUSED once the app begins the session, IDLE once it ends it, READY again when the
// runtime offers the session back. The app polls from more than one thread (its frame thread), hence the lock.
std::mutex gSm;
std::deque<XrSessionState> gEvq;
bool gSessionExists = false, gRunning = false, gStopping = false;
int64_t gReadyAtNs = -1;                 //!< when a stopped session is offered back (READY); -1 = not scheduled
bool gProfileNone = false;               //!< the runtime took the controllers away (xrStop): no interaction profile
void PushStates(std::initializer_list<XrSessionState> s) { for (XrSessionState x : s) gEvq.push_back(x); }
// ---- Connection (fields 53-54, release hardening) ----
//   sessionLoss: a COUNTER; each change = the headset's connection drops (a Link cable pulled, Air Link / Virtual
//                Desktop losing the PC): XR_SESSION_STATE_LOSS_PENDING, then every frame call fails with
//                XR_ERROR_SESSION_LOST until the app destroys that session and makes a new one.
//   noHeadset:   1 = no headset (off, asleep, not streaming): xrGetSystem returns XR_ERROR_FORM_FACTOR_UNAVAILABLE.
float gInSessionLoss = 0, gInNoHeadset = 0;
float gLossDone = 0; bool gLost = false;
// The runtime's controller profile, announced once the action sets are attached (XR_TYPE_EVENT_DATA_INTERACTION_
// PROFILE_CHANGED), as real runtimes do -- the mod logs which profile got bound.
bool gProfileEventPending = false;
// ---- The runtime's own trouble (fields 56-62; the Quest's hourglass, the development notes) ----
// Measured on the user's Quest 3 over Air Link (log 14/10 23:27): the runtime stalled with the app inside xrWaitFrame
// for 33 s, then delivered VISIBLE, SYNCHRONIZED, STOPPING in one poll; twice it took the session away while the app ran
// (controllers' profile to none, the same three states); the stopped session came back READY only 23-169 s later, while
// a NEW session (after a LOSS_PENDING) was READY at once. This emulates all of it.
//   paceHz:     > 0 = xrWaitFrame throttles like a compositor: it blocks to the next vsync at that rate. 0 = at once (old).
//   xrStall:    a COUNTER: each change = the compositor stalls: the next xrWaitFrame blocks `stallSec` seconds, then the
//               runtime gives the session up (VISIBLE, SYNCHRONIZED, STOPPING). A change while no session runs waits for
//               the next one to begin; a NEGATIVE value already there at startup = the session's very first frames stall
//               (the headset never gave a frame, so not its fov either, when the VR package switches on).
//   stallSec:   how long a stall lasts.
//   stallAll:   1 = while stalled EVERY call blocks (the runtime's whole IPC stuck), not only xrWaitFrame.
//   xrStop:     a COUNTER: each change = the runtime takes the session away without a stall (the profile goes to none,
//               VISIBLE, SYNCHRONIZED, STOPPING).
//   readyAfter: seconds after the app ends a stopped session (IDLE) before the runtime offers it back (READY); < 0 =
//               never (a NEW session is still READY at once).
//   starve:     > 0 = the runtime stops the session itself when no frame came for that many seconds.
// Every gap of more than 1 s between two frames of a running session is logged as HOURGLASS: what the headset shows.
float gInPaceHz = 0, gInXrStall = 0, gInStallSec = 0, gInStallAll = 0, gInXrStop = 0, gInReadyAfter = 0, gInStarve = 0;
float gStallDone = 0, gStopDone = 0;
std::atomic<int64_t> gStallUntilNs{ 0 };   //!< a stall in progress (or armed for the next xrWaitFrame) until then
bool gStallArmed = false;
std::atomic<int64_t> gLastEndNs{ 0 };      //!< the last xrEndFrame of a running session
double gHourglassMax = 0; unsigned gHourglasses = 0;
int64_t gFrameTime = 1;          //!< frame COUNTER (the head sweep and the logs); not a time
// Display times are REAL nanoseconds (QPC), like a real runtime's. They used to be the frame counter times 1 ms, which
// was harmless while nothing measured a velocity -- a swing detector reading those would see every motion 10-15x as
// fast as it was.
int64_t gLastDisplay = 0;
int64_t NowNs()
{
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

FILE* gLog = nullptr;
void L(const char* fmt, ...)
{
    // shared, so the log can be read while the game runs (fopen_s denies every other reader)
    // Truncated once per PROCESS: the loader unloads and reloads the runtime when the app makes a new instance (a
    // restart after a loss, a new session), and "w" again would lose everything before it.
    if (!gLog)
    {
        const bool first = GetEnvironmentVariableA("DVR_MOCKXR_LOG_OPEN", nullptr, 0) == 0;
        SetEnvironmentVariableA("DVR_MOCKXR_LOG_OPEN", "1");
        gLog = _fsopen("DVR_MockXR.log", first ? "w" : "a", _SH_DENYNO);
    }
    if (!gLog) return;
    static std::mutex m; std::lock_guard<std::mutex> lk(m);   // the app calls in from more than one thread
    char ts[32]; const int64_t t = NowNs(); snprintf(ts, sizeof ts, "[%9.3f] ", (double)t * 1e-9);
    fputs(ts, gLog);
    va_list a; va_start(a, fmt); vfprintf(gLog, fmt, a); va_end(a);
    fprintf(gLog, "\n"); fflush(gLog);
}
bool gStallActive = false;   //!< under gSm
//! The hourglass that ends in the runtime stopping the session (no frame since the last one when it gives up)
void HourglassAtStop(const char* why)
{
    const int64_t last = gLastEndNs.load();
    const double gap = last ? (double)(NowNs() - last) * 1e-9 : 0.0;
    if (gap > 1.0)
    {
        gHourglasses++; if (gap > gHourglassMax) gHourglassMax = gap;
        L("HOURGLASS #%u: no frame for %.2f s, then %s (longest %.2f s)", gHourglasses, gap, why, gHourglassMax);
    }
}
//! stallAll: while a stall is in progress every call waits it out (the runtime's whole IPC is stuck)
void StallGate() { if (gInStallAll > 0.5f) while (NowNs() < gStallUntilNs.load()) Sleep(5); }
//! Blocks until t (QPC ns), with a high-resolution timer (Sleep's granularity can be 15.6 ms)
void SleepUntilNs(int64_t t)
{
    static thread_local HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    for (;;)
    {
        const int64_t now = NowNs();
        if (now >= t) return;
        if (tm) { LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((t - now) / 100); if (SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(tm, 1000); continue; } }
        Sleep(t - now > 2000000 ? 1 : 0);
    }
}
} // namespace

#define MOCK_FN extern "C" XRAPI_ATTR XrResult XRAPI_CALL

MOCK_FN Mock_xrEnumerateInstanceExtensionProperties(const char*, uint32_t cap, uint32_t* count, XrExtensionProperties* props)
{
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    uint32_t n = 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    strcpy_s(props[0].extensionName, XR_MAX_EXTENSION_NAME_SIZE, exts[0]);
    props[0].extensionVersion = 1;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateInstance(const XrInstanceCreateInfo*, XrInstance* out)
{
    L("xrCreateInstance"); *out = (XrInstance)&gInstance; return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroyInstance(XrInstance) { L("xrDestroyInstance"); return XR_SUCCESS; }

MOCK_FN Mock_xrGetInstanceProperties(XrInstance, XrInstanceProperties* p)
{
    // DVR_MockRuntimeName.txt (cwd), optional: the name to report, to play another runtime (e.g. "SteamVR/OpenXR" for
    // the mod's runtime-specific choices)
    strcpy_s(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "DOTOVR Mock Runtime");
    FILE* f = nullptr;
    if (fopen_s(&f, "DVR_MockRuntimeName.txt", "r") == 0 && f)
    {
        char name[XR_MAX_RUNTIME_NAME_SIZE] = {};
        if (fgets(name, sizeof name, f)) { name[strcspn(name, "\r\n")] = 0; if (name[0]) strcpy_s(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, name); }
        fclose(f);
    }
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    return XR_SUCCESS;
}

void ReadMockInput();
MOCK_FN Mock_xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* id)
{
    ReadMockInput();   // the session is not running yet, so nothing else reads the input file now
    if (gInNoHeadset > 0.5f) { L("xrGetSystem: no headset (noHeadset 1)"); return XR_ERROR_FORM_FACTOR_UNAVAILABLE; }
    *id = (XrSystemId)1; return XR_SUCCESS;
}

MOCK_FN Mock_xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p)
{
    strcpy_s(p->systemName, XR_MAX_SYSTEM_NAME_SIZE, "Mock HMD");
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxLayerCount = 16;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
    uint32_t cap, uint32_t* count, XrViewConfigurationView* views)
{
    ReadHmdConfig();
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; i++)
    {
        views[i].recommendedImageRectWidth = gEyeW;
        views[i].maxImageRectWidth = 4096;
        views[i].recommendedImageRectHeight = gEyeH;
        views[i].maxImageRectHeight = 4096;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

MOCK_FN Mock_xrGetD3D11GraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* req)
{
    req->adapterLuid = LUID{ 0, 0 };
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* out)
{
    const XrBaseInStructure* s = (const XrBaseInStructure*)ci->next;
    while (s)
    {
        if (s->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
        {
            gDevice = ((const XrGraphicsBindingD3D11KHR*)s)->device;
            break;
        }
        s = s->next;
    }
    {
        std::lock_guard<std::mutex> lk(gSm);
        gEvq.clear(); PushStates({ XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY });   // a NEW session: READY at once
        gSessionExists = true; gRunning = false; gStopping = false; gReadyAtNs = -1; gProfileNone = false; gLost = false;
    }
    L("xrCreateSession device=%p", (void*)gDevice);
    *out = (XrSession)&gSession;
    return gDevice ? XR_SUCCESS : XR_ERROR_GRAPHICS_DEVICE_INVALID;
}
MOCK_FN Mock_xrDestroySession(XrSession)
{
    L("xrDestroySession");
    std::lock_guard<std::mutex> lk(gSm);
    gLost = false; gEvq.clear(); gSessionExists = false; gRunning = false; gStopping = false; gReadyAtNs = -1;
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* ci, XrSpace* out)
{
    *out = (ci && ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) ? (XrSpace)&gViewSpace : (XrSpace)&gSpace; return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroySpace(XrSpace) { return XR_SUCCESS; }

MOCK_FN Mock_xrEnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* count, int64_t* formats)
{
    int64_t f[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    formats[0] = f[0]; formats[1] = f[1];
    return XR_SUCCESS;
}

MOCK_FN Mock_xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    MockSwapchain* sc = new MockSwapchain();
    sc->w = ci->width; sc->h = ci->height; sc->fmt = (DXGI_FORMAT)ci->format;
    for (uint32_t i = 0; i < kSwapImages; i++)
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = sc->w; d.Height = sc->h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = sc->fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* t = nullptr;
        HRESULT hr = gDevice ? gDevice->CreateTexture2D(&d, nullptr, &t) : E_FAIL;
        if (FAILED(hr)) { L("CreateTexture2D failed hr=0x%08x", (unsigned)hr); }
        sc->tex.push_back(t);
    }
    L("xrCreateSwapchain %ux%u fmt=%d images=%zu", sc->w, sc->h, (int)sc->fmt, sc->tex.size());
    *out = (XrSwapchain)sc;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroySwapchain(XrSwapchain h)
{
    MockSwapchain* sc = (MockSwapchain*)h;
    for (auto* t : sc->tex) if (t) t->Release();
    delete sc; return XR_SUCCESS;
}

MOCK_FN Mock_xrEnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* imgs)
{
    MockSwapchain* sc = (MockSwapchain*)h;
    *count = (uint32_t)sc->tex.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < sc->tex.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    XrSwapchainImageD3D11KHR* d3d = (XrSwapchainImageD3D11KHR*)imgs;
    for (size_t i = 0; i < sc->tex.size(); i++) d3d[i].texture = sc->tex[i];
    return XR_SUCCESS;
}

// Input field 36 is a recenter COUNTER: each increase delivers one XrEventDataReferenceSpaceChangePending, as the
// Meta runtime does when the user holds the Meta button.
float gInRecenter = 0, gRecenterDone = 0;
// A recentre MOVES the LOCAL space, as the real runtime does: its origin goes to where the head is and its forward to
// where the head faces. Every pose after it (head and hands) is expressed in the new space, so the mod sees the same
// jump it would see on the Quest -- the whole scene of poses shifted and turned by the head's pose at that moment.
bool gRcPending = false; float gRcYaw = 0, gRcO[3] = {};
MOCK_FN Mock_xrPollEvent(XrInstance, XrEventDataBuffer* ev)
{
    ReadMockInput();   // also here: the triggers below must work while the session is idle (no xrSyncActions then)
    StallGate();
    std::lock_guard<std::mutex> lk(gSm);
    const int64_t now = NowNs();
    if (gInXrStall != gStallDone && gRunning)   // a change while no session runs waits for the next one to begin
    {
        gStallDone = gInXrStall;
        if (gInStallSec > 0.f)
        {
            gStallUntilNs = now + (int64_t)((double)gInStallSec * 1e9); gStallActive = true;
            L("STALL #%.0f: the compositor stops for %.1f s%s", gInXrStall, gInStallSec, gInStallAll > 0.5f ? " (every call blocks)" : " (xrWaitFrame blocks)");
        }
    }
    if (gStallActive && now >= gStallUntilNs.load())
    {   // the runtime gives the session up, as Meta's did after its 33 s stall
        gStallActive = false;
        if (gRunning && !gStopping) { HourglassAtStop("the stall's stop"); gStopping = true; PushStates({ XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING }); }
        L("STALL over: the runtime stops the session");
    }
    if (gInXrStop != gStopDone)
    {
        gStopDone = gInXrStop;
        if (gRunning && !gStopping)
        {
            gStopping = true; gProfileNone = true; gProfileEventPending = true;
            PushStates({ XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING });
            L("STOP #%.0f: the runtime takes the session away (ready again after %.1f s)", gInXrStop, gInReadyAfter);
        }
    }
    if (gInStarve > 0.f && gRunning && !gStopping && gLastEndNs.load() && now - gLastEndNs.load() > (int64_t)((double)gInStarve * 1e9))
    {
        HourglassAtStop("STARVED");
        gStopping = true;
        PushStates({ XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING });
        L("STARVED: no frame for %.1f s: the runtime stops the session", (double)(now - gLastEndNs.load()) * 1e-9);
    }
    if (gReadyAtNs >= 0 && now >= gReadyAtNs && gSessionExists && !gRunning)
    {
        gReadyAtNs = -1; PushStates({ XR_SESSION_STATE_READY });
        if (gProfileNone) { gProfileNone = false; gProfileEventPending = true; }
        L("the stopped session is offered back: READY");
    }
    if (gInSessionLoss != gLossDone && !gLost && gSessionExists)
    {
        gLossDone = gInSessionLoss; gLost = true;
        auto* e = (XrEventDataSessionStateChanged*)ev;
        *e = { XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
        e->session = (XrSession)&gSession;
        e->state = XR_SESSION_STATE_LOSS_PENDING;
        e->time = gLastDisplay;
        L("session loss #%.0f: LOSS_PENDING", gInSessionLoss);
        return XR_SUCCESS;
    }
    if (gProfileEventPending)
    {
        gProfileEventPending = false;
        auto* e = (XrEventDataInteractionProfileChanged*)ev;
        *e = { XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED };
        e->session = (XrSession)&gSession;
        L("interaction profile changed");
        return XR_SUCCESS;
    }
    if (gInRecenter != gRecenterDone)
    {
        gRecenterDone = gInRecenter;
        auto* e = (XrEventDataReferenceSpaceChangePending*)ev;
        *e = { XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING };
        e->session = (XrSession)&gSession;
        e->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        e->changeTime = gLastDisplay;             // in effect from the next frame the app waits for
        e->poseValid = XR_TRUE;
        e->poseInPreviousSpace.orientation.w = 1.f;
        gRcPending = true;                        // taken from the head at the next xrLocateViews
        L("recenter event #%.0f", gInRecenter);
        return XR_SUCCESS;
    }
    if (!gEvq.empty())
    {
        auto* e = (XrEventDataSessionStateChanged*)ev;
        e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
        e->next = nullptr;
        e->session = (XrSession)&gSession;
        e->state = gEvq.front(); gEvq.pop_front();
        e->time = gLastDisplay;
        return XR_SUCCESS;
    }
    return XR_EVENT_UNAVAILABLE;
}

MOCK_FN Mock_xrBeginSession(XrSession, const XrSessionBeginInfo*)
{
    L("xrBeginSession");
    std::lock_guard<std::mutex> lk(gSm);
    gRunning = true; gStopping = false; gLastEndNs = NowNs();   // the hourglass clock starts now
    PushStates({ XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED });
    return XR_SUCCESS;
}
MOCK_FN Mock_xrEndSession(XrSession)
{
    std::lock_guard<std::mutex> lk(gSm);
    gRunning = false; gStopping = false;
    PushStates({ XR_SESSION_STATE_IDLE });
    gReadyAtNs = gInReadyAfter >= 0.f ? NowNs() + (int64_t)((double)gInReadyAfter * 1e9) : -1;
    L("xrEndSession (the stopped session %s)", gReadyAtNs >= 0 ? "will be offered back" : "is NOT offered back: only a new session gets READY");
    return XR_SUCCESS;
}

MOCK_FN Mock_xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* fs)
{
    if (gLost) return XR_ERROR_SESSION_LOST;
    if (!gRunning) return XR_ERROR_SESSION_NOT_RUNNING;
    {   // a stall: the compositor does not release the app
        const int64_t until = gStallUntilNs.load();
        if (NowNs() < until) { L("xrWaitFrame blocks (stall)"); SleepUntilNs(until); L("xrWaitFrame returns after the stall"); }
    }
    ++gFrameTime;
    int64_t period = 11111111;
    int64_t t = NowNs() + period;                 // predicted one display period ahead, as a runtime does
    if (gInPaceHz > 1.f)
    {   // a compositor: the app is released once per display period, at its vsync
        period = (int64_t)(1e9 / gInPaceHz);
        static int64_t nextVsync = 0;
        int64_t now = NowNs();
        if (nextVsync == 0 || now > nextVsync + 4 * period) nextVsync = now;   // re-phase after a gap
        SleepUntilNs(nextVsync);
        now = NowNs();
        nextVsync += period; if (nextVsync <= now) nextVsync = now + period;
        t = nextVsync + period;
    }
    if (t <= gLastDisplay) t = gLastDisplay + 1;  // strictly increasing
    fs->predictedDisplayTime = gLastDisplay = t;
    fs->predictedDisplayPeriod = period;
    fs->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrBeginFrame(XrSession, const XrFrameBeginInfo*) { StallGate(); return gRunning ? XR_SUCCESS : XR_ERROR_SESSION_NOT_RUNNING; }

// ---- The compositor, emulated: what each EYE would see ----
// The mock used to drop the submitted layers, so a capture could only show the image the mod RENDERED, never where
// that image lands in the eye. That mapping is the whole question when "the arms look closer than in real life": the
// projection layer says which fov its image covers, and the compositor resamples it into the eye's own fov, with a
// rotation-only reprojection from the pose the image was rendered at to the pose the eye has now (what the Quest's
// timewarp does). Writing DVR_MockCompose.txt (one word: a tag) makes the next xrEndFrame do exactly that, on the
// CPU, for both eyes: DVR_seen_<tag>_eye<i>.bmp covers the eye's whole fov, linear in tan like a real eye buffer,
// with the pixels no layer reaches painted dark blue (a real headset shows black there; blue tells "no image" from a
// black image). DVR_seen_<tag>.txt has the numbers to annotate it with.
XrPosef gLastViewPose[2] = { { { 0, 0, 0, 1 }, { 0, 0, 0 } }, { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
void HeadViewsAt(int64_t t, XrView* views, bool recenter, XrPosef* head = nullptr);   // the head at a display time (below)
// q^-1 * v * q etc. -- small quaternion helpers for the compositor
void QRot(const XrQuaternionf& q, const float v[3], float o[3])
{   // o = q v q*
    const float u[3] = { q.x, q.y, q.z }, w = q.w;
    const float c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
    for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
}
void QRotInv(const XrQuaternionf& q, const float v[3], float o[3]) { const XrQuaternionf c = { -q.x, -q.y, -q.z, q.w }; QRot(c, v, o); }

//! Where a layer's space is right now, in LOCAL (defined after xrLocateSpace).
bool LocateLayerSpace(XrSpace space, XrPosef& out);
bool ReadTexture(ID3D11Texture2D* t, std::vector<uint8_t>& px, UINT& w, UINT& h, bool& bgra)
{
    if (!t || !gDevice) return false;
    D3D11_TEXTURE2D_DESC d = {}; t->GetDesc(&d);
    const DXGI_FORMAT f = d.Format;
    bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!(bgra || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS)) return false;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* s = nullptr;
    if (FAILED(gDevice->CreateTexture2D(&d, nullptr, &s))) return false;
    ID3D11DeviceContext* ctx = nullptr; gDevice->GetImmediateContext(&ctx);
    ctx->CopyResource(s, t);
    D3D11_MAPPED_SUBRESOURCE m = {};
    bool ok = SUCCEEDED(ctx->Map(s, 0, D3D11_MAP_READ, 0, &m));
    if (ok)
    {
        w = d.Width; h = d.Height; px.resize((size_t)w * h * 4);
        for (UINT y = 0; y < h; y++) memcpy(&px[(size_t)y * w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        ctx->Unmap(s, 0);
    }
    ctx->Release(); s->Release();
    return ok;
}
bool WriteBmp(const char* path, const std::vector<uint8_t>& rgb, int w, int h)
{
    FILE* f = nullptr; if (fopen_s(&f, path, "wb") || !f) return false;
    const int row = (w * 3 + 3) & ~3, size = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' };
    auto put32 = [&](int o, int v) { hd[o] = (uint8_t)v; hd[o + 1] = (uint8_t)(v >> 8); hd[o + 2] = (uint8_t)(v >> 16); hd[o + 3] = (uint8_t)(v >> 24); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h); hd[26] = 1; hd[28] = 24; put32(34, row * h);
    fwrite(hd, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; y >= 0; y--)
    {
        for (int x = 0; x < w; x++) { const uint8_t* p = &rgb[((size_t)y * w + x) * 3]; line[x * 3] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0]; }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f); return true;
}
// worldFixed: compose for an eye that does NOT turn (identity orientation, same position). A truthful layer pose then
// gives the same picture of the static world whatever the head does: the swim test.
void Compose(const XrFrameEndInfo* fei, const char* tag, bool worldFixed)
{
    std::string info;
    char b[512];
    for (uint32_t li = 0; li < fei->layerCount; li++)
    {
        const XrCompositionLayerBaseHeader* lh = fei->layers[li];
        snprintf(b, sizeof b, "layer %u type %d\n", li, (int)lh->type); info += b;
    }
    // The projection layer (the mod submits one; a quad HUD may follow and is listed, not drawn).
    const XrCompositionLayerProjection* proj = nullptr;
    for (uint32_t li = 0; li < fei->layerCount && !proj; li++)
        if (fei->layers[li]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) proj = (const XrCompositionLayerProjection*)fei->layers[li];
    const float r2d = 57.2957795f;
    for (int eye = 0; eye < 2; eye++)
    {
        const XrFovf ef = EyeFov(eye);
        // where the eye IS when this frame is shown: the head at the frame's display time (the last located pose was
        // this before the app could locate for other times; a prediction must be judged against the display time)
        XrPosef ep = gLastViewPose[eye];
        { XrView hv[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; HeadViewsAt(fei->displayTime > 0 ? (int64_t)fei->displayTime : gLastDisplay, hv, false); ep = hv[eye].pose; }
        if (worldFixed) ep.orientation = { 0, 0, 0, 1 };
        snprintf(b, sizeof b, "eye %d fov %.4f %.4f %.4f %.4f pose q %.5f %.5f %.5f %.5f p %.4f %.4f %.4f\n", eye,
                 ef.angleLeft * r2d, ef.angleRight * r2d, ef.angleUp * r2d, ef.angleDown * r2d,
                 ep.orientation.x, ep.orientation.y, ep.orientation.z, ep.orientation.w, ep.position.x, ep.position.y, ep.position.z);
        info += b;
        // output: the eye's fov, linear in tan, ~1/2 of the panel's resolution per axis
        const float tL = tanf(ef.angleLeft), tR = tanf(ef.angleRight), tU = tanf(ef.angleUp), tD = tanf(ef.angleDown);
        const int W = (int)(gEyeW / 2), H = (int)(gEyeH / 2);
        std::vector<uint8_t> out((size_t)W * H * 3, 0);
        for (size_t i = 0; i < out.size(); i += 3) { out[i] = 0; out[i + 1] = 0; out[i + 2] = 70; }
        if (proj && (uint32_t)eye < proj->viewCount)
        {
            const XrCompositionLayerProjectionView& v = proj->views[eye];
            MockSwapchain* sc = (MockSwapchain*)v.subImage.swapchain;
            const XrFovf& lf = v.fov;
            snprintf(b, sizeof b, "view %d fov %.4f %.4f %.4f %.4f pose q %.5f %.5f %.5f %.5f p %.4f %.4f %.4f rect %d %d %d %d swap %ux%u\n", eye,
                     lf.angleLeft * r2d, lf.angleRight * r2d, lf.angleUp * r2d, lf.angleDown * r2d,
                     v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w,
                     v.pose.position.x, v.pose.position.y, v.pose.position.z,
                     v.subImage.imageRect.offset.x, v.subImage.imageRect.offset.y, v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height,
                     sc ? sc->w : 0, sc ? sc->h : 0);
            info += b;
            // the image last released on that swapchain is the one the layer shows
            std::vector<uint8_t> px; UINT tw = 0, th = 0; bool bgra = false;
            ID3D11Texture2D* tex = (sc && !sc->tex.empty()) ? sc->tex[(sc->acquired + (uint32_t)sc->tex.size() - 1) % (uint32_t)sc->tex.size()] : nullptr;
            if (ReadTexture(tex, px, tw, th, bgra))
            {
                const float lL = tanf(lf.angleLeft), lR = tanf(lf.angleRight), lU = tanf(lf.angleUp), lD = tanf(lf.angleDown);
                const int rx = v.subImage.imageRect.offset.x, ry = v.subImage.imageRect.offset.y;
                const int rw = v.subImage.imageRect.extent.width, rh = v.subImage.imageRect.extent.height;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                    {
                        const float d[3] = { tL + (x + 0.5f) / W * (tR - tL), tU - (y + 0.5f) / H * (tU - tD), -1.f };
                        float wd[3], ld[3]; QRot(ep.orientation, d, wd); QRotInv(v.pose.orientation, wd, ld);
                        if (ld[2] >= -1e-6f) continue;
                        const float lx = ld[0] / -ld[2], ly = ld[1] / -ld[2];
                        const float s = (lx - lL) / (lR - lL), t = (lU - ly) / (lU - lD);
                        if (s < 0 || s >= 1 || t < 0 || t >= 1) continue;
                        const int sx = rx + (int)(s * rw), sy = ry + (int)(t * rh);
                        if (sx < 0 || sy < 0 || (UINT)sx >= tw || (UINT)sy >= th) continue;
                        const uint8_t* p = &px[((size_t)sy * tw + sx) * 4];
                        uint8_t* o = &out[((size_t)y * W + x) * 3];
                        o[0] = bgra ? p[2] : p[0]; o[1] = p[1]; o[2] = bgra ? p[0] : p[2];
                    }
            }
            else info += "  (texture not readable)\n";
        }
        // Quad layers, over the projection in submission order: a ray from the eye's POSITION (a quad is a surface
        // in the room, so where the eye is matters, unlike the projection's rotation-only resample), the quad's plane
        // in its space's current pose, the last released image, blended as the flags say (premultiplied by default).
        for (uint32_t li = 0; li < fei->layerCount; li++)
        {
            if (fei->layers[li]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) continue;
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)fei->layers[li];
            MockSwapchain* sc = (MockSwapchain*)q->subImage.swapchain;
            XrPosef sp; if (!sc || !LocateLayerSpace(q->space, sp)) continue;
            // quad pose in LOCAL = space pose * layer pose
            float qp[3]; { const float v[3] = { q->pose.position.x, q->pose.position.y, q->pose.position.z }; QRot(sp.orientation, v, qp); }
            qp[0] += sp.position.x; qp[1] += sp.position.y; qp[2] += sp.position.z;
            const XrQuaternionf qa = sp.orientation, qb = q->pose.orientation;
            const XrQuaternionf qo = { qa.w * qb.x + qa.x * qb.w + qa.y * qb.z - qa.z * qb.y, qa.w * qb.y - qa.x * qb.z + qa.y * qb.w + qa.z * qb.x,
                                       qa.w * qb.z + qa.x * qb.y - qa.y * qb.x + qa.z * qb.w, qa.w * qb.w - qa.x * qb.x - qa.y * qb.y - qa.z * qb.z };
            std::vector<uint8_t> px; UINT tw = 0, th = 0; bool bgra = false;
            ID3D11Texture2D* tex = !sc->tex.empty() ? sc->tex[(sc->acquired + (uint32_t)sc->tex.size() - 1) % (uint32_t)sc->tex.size()] : nullptr;
            if (!ReadTexture(tex, px, tw, th, bgra)) { info += "  quad texture not readable\n"; continue; }
            const bool alpha = (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
            const bool unpre = (q->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0;
            // An sRGB swapchain is blended in LINEAR light, as a real compositor does (its texels are decoded on
            // sampling); blending the encoded bytes made every translucent layer look lighter than on the headset.
            const bool srgb = sc->fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || sc->fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            auto toLin = [](float v) { v /= 255.f; return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
            auto toEnc = [](float v) { v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v); return 255.f * (v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f); };
            const float eyeP[3] = { ep.position.x, ep.position.y, ep.position.z };
            unsigned long long hit = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    const float d[3] = { tL + (x + 0.5f) / W * (tR - tL), tU - (y + 0.5f) / H * (tU - tD), -1.f };
                    float wd[3]; QRot(ep.orientation, d, wd);
                    float ld[3], lo[3]; QRotInv(qo, wd, ld);
                    { const float r[3] = { eyeP[0] - qp[0], eyeP[1] - qp[1], eyeP[2] - qp[2] }; QRotInv(qo, r, lo); }
                    if (fabsf(ld[2]) < 1e-6f) continue;
                    const float t = -lo[2] / ld[2];
                    if (t <= 0) continue;
                    const float hx = lo[0] + ld[0] * t, hy = lo[1] + ld[1] * t;
                    const float u = hx / q->size.width + 0.5f, v = 0.5f - hy / q->size.height;
                    if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
                    const int sx = q->subImage.imageRect.offset.x + (int)(u * q->subImage.imageRect.extent.width);
                    const int sy = q->subImage.imageRect.offset.y + (int)(v * q->subImage.imageRect.extent.height);
                    if (sx < 0 || sy < 0 || (UINT)sx >= tw || (UINT)sy >= th) continue;
                    const uint8_t* p = &px[((size_t)sy * tw + sx) * 4];
                    const float sa = alpha ? p[3] / 255.f : 1.f;
                    uint8_t* o = &out[((size_t)y * W + x) * 3];
                    const uint8_t c[3] = { bgra ? p[2] : p[0], p[1], bgra ? p[0] : p[2] };
                    for (int k = 0; k < 3; k++)
                    {
                        if (srgb)
                        {
                            const float cl = toLin(c[k]), ol = toLin(o[k]);
                            o[k] = (uint8_t)(toEnc((unpre ? cl * sa : cl) + ol * (1.f - sa)) + 0.5f);
                            continue;
                        }
                        const float sc = unpre ? c[k] * sa : c[k];
                        const float r = sc + o[k] * (1.f - sa);
                        o[k] = (uint8_t)(r > 255.f ? 255.f : r);
                    }
                    if (sa > 0.02f) hit++;
                }
            snprintf(b, sizeof b, "quad %u eye %d pos %.3f %.3f %.3f size %.3f x %.3f flags %llu covered %llu px\n", li, eye, qp[0], qp[1], qp[2],
                     q->size.width, q->size.height, (unsigned long long)q->layerFlags, hit);
            info += b;
        }
        char path[260]; snprintf(path, sizeof path, "DVR_seen_%s_eye%d.bmp", tag, eye);
        snprintf(b, sizeof b, "out %d %dx%d %s\n", eye, W, H, WriteBmp(path, out, W, H) ? path : "WRITE FAILED"); info += b;
    }
    char path[260]; snprintf(path, sizeof path, "DVR_seen_%s.txt", tag);
    FILE* f = nullptr;
    if (!fopen_s(&f, path, "w") && f) { fputs(info.c_str(), f); fclose(f); }
    L("compose '%s': %u layers", tag, fei->layerCount);
}

MOCK_FN Mock_xrEndFrame(XrSession, const XrFrameEndInfo* fei)
{
    StallGate();
    static uint64_t n = 0;
    if ((n++ % 300) == 0) L("xrEndFrame #%llu layers=%u", (unsigned long long)n, fei->layerCount);
    {   // the hourglass: what the headset shows when a running session's frames stop coming
        const int64_t now = NowNs(), last = gLastEndNs.exchange(now);
        const double gap = last ? (double)(now - last) * 1e-9 : 0.0;
        if (gRunning && !gStopping && gap > 1.0)
        {
            gHourglasses++; if (gap > gHourglassMax) gHourglassMax = gap;
            L("HOURGLASS #%u: no frame for %.2f s (longest %.2f s)", gHourglasses, gap, gHourglassMax);
        }
    }
    if (GetFileAttributesA("DVR_MockCompose.txt") != INVALID_FILE_ATTRIBUTES)
    {
        char tag[64] = "seen", mode[16] = ""; FILE* f = nullptr;
        if (!fopen_s(&f, "DVR_MockCompose.txt", "r") && f)
        {
            if (fscanf_s(f, "%63s", tag, (unsigned)sizeof tag) != 1) strcpy_s(tag, "seen");
            if (fscanf_s(f, "%15s", mode, (unsigned)sizeof mode) != 1) mode[0] = 0;
            fclose(f);
        }
        DeleteFileA("DVR_MockCompose.txt");
        Compose(fei, tag, strcmp(mode, "world") == 0);
    }
    return XR_SUCCESS;
}

// Input file, all fields optional (missing = keep previous):
//   turnX trigR handYaw handPitch headYaw headPitch headX headY headZ
//   moveX moveY handX handY handZ btnA btnB btnX btnY
//   turnY trigL gripL gripR btnThumbL btnThumbR btnMenu
//   lhandYaw lhandPitch lhandX lhandY lhandZ
//   handRoll lhandRoll
//   rhandLost lhandLost headLost   (1 = that device lost tracking: its pose flags go to 0)
//   recenter                       (a counter: each increase = one runtime recenter event)
//   swing swKind swAx swAy swAz swFrom swTo swDur swPivX swPivY swPivZ swRet
//                                  (a scripted hand motion, see the swing generator below)
//   swHand swRelL                  (which hand the motion moves: 0 right, 1 left; and a THROW's release: > 0 = the
//                                   left trigger is held from the motion's start and let go this many seconds into
//                                   the gesture leg, so a release lands at an exact point of the arc)
//   headSway handJit               (realism, degrees: a slow continuous head sway, and a hand tremor; see below)
//   sessionLoss noHeadset          (the headset's connection, see the top of the file)
//   headSpin                       (a steady head turn, degrees per second: motion at one known speed)
//   paceHz xrStall stallSec stallAll xrStop readyAfter starve   (the runtime's own trouble, see the top of the file)
// Angles in degrees; positions in meters (offsets from the defaults);
// buttons are 0/1. New fields appended at the end so old step lists keep working.
// hand* fields drive the RIGHT hand; lhand* the LEFT (psi aim / VR hands).
float gInTurnX = 0, gInTrigR = 0, gInHandYaw = 0, gInHandPitch = 0;
float gInHeadYaw = 0, gInHeadPitch = 0, gInHeadX = 0, gInHeadY = 0, gInHeadZ = 0;
float gInMoveX = 0, gInMoveY = 0, gInHandX = 0, gInHandY = 0, gInHandZ = 0;
float gInBtnA = 0, gInBtnB = 0, gInBtnX = 0, gInBtnY = 0;
float gInTurnY = 0, gInTrigL = 0, gInGripL = 0, gInGripR = 0;
float gInBtnThumbL = 0, gInBtnThumbR = 0, gInBtnMenu = 0;
float gInLHandYaw = 0, gInLHandPitch = 0, gInLHandX = 0, gInLHandY = 0, gInLHandZ = 0;
// Wrist ROLL, about the controller's own pointing axis. Without it the mock could not pose the one thing the hand
// tests are about -- whether the palm faces where the real palm faces -- because palm orientation at a fixed aim
// direction IS a roll. Degrees, positive = the same sense as yaw/pitch here.
float gInHandRoll = 0, gInLHandRoll = 0;
// Tracking loss, for the robustness tests: a controller out of the cameras' view, put down, or asleep; the headset
// taken off. The real runtime clears the VALID/TRACKED bits; so does this.
float gInRHandLost = 0, gInLHandLost = 0, gInHeadLost = 0;
bool gHaveInput = false; //!< once a file was read, head pose is file-driven

// ---- The swing generator (fields 37-48) ----
// A sword gesture is judged by its VELOCITY, and the input file cannot produce one: a pose written by the harness jumps
// from one value to the next between two frames, i.e. at infinite speed. So the motion is generated HERE, per located
// pose, from the display time the app asks for -- smooth, repeatable, with a known peak speed.
// Each increase of `swing` starts one motion of the right hand, on top of its file pose:
//   swKind 0: a ROTATION by an angle (degrees) about the axis (swAx,swAy,swAz) through the pivot (the hand's position
//             + (swPivX,swPivY,swPivZ), raw XR metres; the shoulder for an arm swing, 0 for a pure wrist flick). Both
//             the position and the orientation of the controller turn with it.
//   swKind 1: a TRANSLATION by a distance (metres) along the axis, orientation untouched (a thrust).
// Timeline: 0 -> swFrom in swRet seconds (slow wind-up), swFrom -> swTo in swDur seconds (THE gesture), hold 0.3 s,
// swTo -> 0 in swRet seconds (slow recovery). Every leg follows a minimum-jerk profile (10t^3 - 15t^4 + 6t^5), the
// shape of a human reaching movement; its peak speed is 1.875x the leg's average.
float gInSwing = 0, gInSwKind = 0, gInSwAx = 0, gInSwAy = 1, gInSwAz = 0, gInSwFrom = 0, gInSwTo = 0, gInSwDur = 0.3f;
float gInSwPivX = 0, gInSwPivY = 0, gInSwPivZ = 0, gInSwRet = 1.f;
// A throw is judged by the hand's velocity at the instant the trigger opens, and a trigger written by the harness
// lands wherever PowerShell's sleep puts it (tens of ms off, i.e. anywhere on the arc). So the release is scripted
// with the motion, on the same display clock.
float gInSwHand = 0, gInSwRelL = 0;
float gSwingDone = 0; int64_t gSwingStart = -1;
// ---- Realism (fields 51-52) ----
// A real head is never still and a real hand trembles; a file-driven pose is perfectly still, so anything in the mod
// that couples the head into the hand ray (or amplifies tremor) is invisible here unless it is generated.
//   headSway: amplitude in degrees of a smooth head motion -- yaw at 0.35 Hz, pitch at 0.23 Hz with half the
//             amplitude -- on top of headYaw/headPitch. A person looking around a table moves about this much.
//   handJit:  amplitude in degrees of a physiological tremor on BOTH controllers' yaw and pitch (three incommensurate
//             sines at 5-10 Hz, the band of real hand tremor).
float gInHeadSway = 0, gInHandJit = 0;
//   headSpin (field 55): a steady head turn, degrees per second, accumulated from the moment it is set -- motion at ONE
//             known speed, so image measurements taken at different moments compare (headSway's speed depends on when).
float gInHeadSpin = 0;
float SecondsAt(int64_t t) { return (float)((double)t * 1e-9); }
// The spin is a FUNCTION of the display time asked for, not an integrator over the calls: the app may locate the head at
// any time, earlier or later than the last one (its prediction for when an image will be shown), and an integrator
// stepped only forwards turned a locate for a later time into a permanent jump. Piecewise linear: each change of the
// rate starts a new segment at the moment it is seen (real time); a time before that segment uses the previous one.
struct SpinSeg { int64_t t0 = 0; double a0 = 0, rate = 0; };
std::mutex gSpinM; SpinSeg gSpinCur, gSpinPrev; bool gSpinInit = false;
double SpinAt(int64_t t)
{
    std::lock_guard<std::mutex> lk(gSpinM);
    const double rate = (double)gInHeadSpin;
    if (!gSpinInit) { gSpinInit = true; gSpinCur = { NowNs(), 0.0, rate }; gSpinPrev = gSpinCur; }
    else if (rate != gSpinCur.rate)
    {
        const int64_t now = NowNs();
        const double a = gSpinCur.a0 + gSpinCur.rate * (double)(now - gSpinCur.t0) * 1e-9;
        gSpinPrev = gSpinCur; gSpinCur = { now, fmod(a, 360.0), rate };
    }
    const SpinSeg& sg = (t >= gSpinCur.t0) ? gSpinCur : gSpinPrev;
    return fmod(sg.a0 + sg.rate * (double)(t - sg.t0) * 1e-9, 360.0);
}
void HeadSway(int64_t t, float& yawDeg, float& pitchDeg)
{
    const float s = SecondsAt(t), a = gInHeadSway;
    yawDeg = a * sinf(6.2831853f * 0.35f * s); pitchDeg = 0.5f * a * sinf(6.2831853f * 0.23f * s + 1.f);
    yawDeg += (float)SpinAt(t);
}
void HandJitter(int64_t t, int hand, float& yawDeg, float& pitchDeg)
{
    const float s = SecondsAt(t) + (float)hand * 0.37f, a = gInHandJit / 1.8f;
    yawDeg = a * (sinf(6.2831853f * 5.3f * s) + 0.6f * sinf(6.2831853f * 7.9f * s + 2.f) + 0.4f * sinf(6.2831853f * 9.7f * s + 4.f));
    pitchDeg = a * (sinf(6.2831853f * 6.1f * s + 1.f) + 0.6f * sinf(6.2831853f * 8.3f * s + 3.f) + 0.4f * sinf(6.2831853f * 4.7f * s + 5.f));
}
struct SwingParams { float kind, ax, ay, az, from, to, dur, pivX, pivY, pivZ, ret, hand, relL; } gSw{};
float MinJerk(float t) { t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * t * (10.f + t * (-15.f + 6.f * t)); }
//! The generator's value (degrees or metres) at display time t; false when no motion is running.
bool SwingValue(int64_t t, float& v)
{
    if (gSwingStart < 0) return false;
    const float s = (float)((double)(t - gSwingStart) * 1e-9);
    const float ret = gSw.ret > 0.01f ? gSw.ret : 0.01f, dur = gSw.dur > 0.01f ? gSw.dur : 0.01f;
    if (s < 0) { v = 0; return true; }
    if (s < ret) { v = gSw.from * MinJerk(s / ret); return true; }
    if (s < ret + dur) { v = gSw.from + (gSw.to - gSw.from) * MinJerk((s - ret) / dur); return true; }
    if (s < ret + dur + 0.3f) { v = gSw.to; return true; }
    if (s < 2 * ret + dur + 0.3f) { v = gSw.to * (1.f - MinJerk((s - ret - dur - 0.3f) / ret)); return true; }
    v = 0; return true;
}
//! The left trigger as the running throw holds it: true (and the value in v) while a motion with swRelL > 0 runs.
bool SwingTriggerL(float& v)
{
    if (gSwingStart < 0 || gSw.relL <= 0.f) return false;
    const float s = (float)((double)(gLastDisplay - gSwingStart) * 1e-9);
    const float ret = gSw.ret > 0.01f ? gSw.ret : 0.01f, dur = gSw.dur > 0.01f ? gSw.dur : 0.01f;
    if (s < 0) return false;
    if (s < ret + gSw.relL) { v = 1.f; return true; }
    if (s < 2 * ret + dur + 0.3f) { v = 0.f; return true; }
    return false;
}
//! Apply the running motion to a raw hand pose.
void ApplySwing(int64_t t, XrPosef& pose)
{
    float v;
    if (!SwingValue(t, v) || v == 0.f) return;
    float ax = gSw.ax, ay = gSw.ay, az = gSw.az;
    const float n = sqrtf(ax * ax + ay * ay + az * az); if (n < 1e-6f) return;
    ax /= n; ay /= n; az /= n;
    if (gSw.kind >= 0.5f)
    {
        pose.position.x += ax * v; pose.position.y += ay * v; pose.position.z += az * v;
        return;
    }
    const float h = v * 3.14159265f / 360.f, sh = sinf(h);
    const XrQuaternionf r = { ax * sh, ay * sh, az * sh, cosf(h) };
    // position about the pivot: p' = P + r (p - P) r*
    const float P[3] = { pose.position.x + gSw.pivX, pose.position.y + gSw.pivY, pose.position.z + gSw.pivZ };
    const float d[3] = { -gSw.pivX, -gSw.pivY, -gSw.pivZ };
    // rotate d by r (Rodrigues through the quaternion): d' = d + 2w(u x d) + 2 u x (u x d)
    const float u[3] = { r.x, r.y, r.z }, w = r.w;
    const float c1[3] = { u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0] };
    const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
    pose.position.x = P[0] + d[0] + 2 * (w * c1[0] + c2[0]);
    pose.position.y = P[1] + d[1] + 2 * (w * c1[1] + c2[1]);
    pose.position.z = P[2] + d[2] + 2 * (w * c1[2] + c2[2]);
    const XrQuaternionf q = pose.orientation;   // q' = r * q (the rotation is in the world frame)
    pose.orientation = { r.w * q.x + r.x * q.w + r.y * q.z - r.z * q.y,
                         r.w * q.y - r.x * q.z + r.y * q.w + r.z * q.x,
                         r.w * q.z + r.x * q.y - r.y * q.x + r.z * q.w,
                         r.w * q.w - r.x * q.x - r.y * q.y - r.z * q.z };
}
unsigned long long gHaptics[2] = {};

//! Raw (input) pose -> the current LOCAL space: p' = Ry(-yaw0) (p - o), q' = qy(-yaw0) q.
void ToLocal(XrPosef& pose)
{
    const float c = cosf(-gRcYaw), s = sinf(-gRcYaw);
    const float dx = pose.position.x - gRcO[0], dy = pose.position.y - gRcO[1], dz = pose.position.z - gRcO[2];
    pose.position = { c * dx + s * dz, dy, -s * dx + c * dz };      // rotation about +Y by -yaw0
    const float hw = cosf(-gRcYaw * 0.5f), hy = sinf(-gRcYaw * 0.5f);
    const XrQuaternionf q = pose.orientation;
    pose.orientation = { hw * q.x + hy * q.z, hw * q.y + hy * q.w, hw * q.z - hy * q.x, hw * q.w - hy * q.y };
}

//! The two eyes' poses at display time t (LOCAL space). `recenter`: a pending recenter takes effect here (the app's own
//! locate); the compositor's look at "where the eye is now" must not consume it.
void HeadViewsAt(int64_t t, XrView* views, bool recenter, XrPosef* head)
{
    // Head pose: file-driven once DVR_MockInput.txt exists (deterministic
    // tests); otherwise a slow sine yaw sweep so the pipeline carries changing
    // data. q = yaw(Y) * pitch(X).
    float yaw, pitch = 0;
    if (gHaveInput)
    {
        float swy = 0, swp = 0; HeadSway(t, swy, swp);
        yaw = (gInHeadYaw + swy) * 3.14159265f / 180.0f;
        pitch = (gInHeadPitch + swp) * 3.14159265f / 180.0f;
    }
    else
    {
        yaw = 0.6f * sinf((float)gFrameTime * 0.02f);
    }
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    if (recenter && gRcPending)
    {   // the new LOCAL space: origin at the head, forward where it faces (yaw only: LOCAL stays gravity-aligned)
        gRcPending = false; gRcYaw = yaw; gRcO[0] = gInHeadX; gRcO[1] = 1.6f + gInHeadY; gRcO[2] = gInHeadZ;
        L("recenter: LOCAL origin now (%.2f,%.2f,%.2f), yaw %.1f deg", gRcO[0], gRcO[1], gRcO[2], gRcYaw * 57.2958f);
    }
    const XrQuaternionf hq = { cy * sp, sy * cp, -sy * sp, cy * cp };
    for (int i = 0; i < 2; i++)
    {
        views[i].pose.orientation = hq;
        {   // the eyes are +-IPD/2 along the HEAD's right axis, so they turn with it (they used to stay on the room's
            // x axis whatever the head did, which hid a stereo bug that only shows with the head turned)
            const float e[3] = { i == 0 ? -0.032f : 0.032f, 0.f, 0.f }; float r[3];
            QRot(hq, e, r);
            views[i].pose.position = { r[0] + gInHeadX, r[1] + 1.6f + gInHeadY, r[2] + gInHeadZ };   // standing height + offsets
        }
        if (gCantDeg != 0.f)
        {   // canted displays: the eye turned out from the head (about the head's own up axis): orientation = head * yaw
            const float c = (i == 0 ? gCantDeg : -gCantDeg) * 3.14159265f / 180.0f * 0.5f;
            const XrQuaternionf yq = { 0.f, sinf(c), 0.f, cosf(c) };
            views[i].pose.orientation = { hq.w * yq.x + hq.x * yq.w + hq.y * yq.z - hq.z * yq.y, hq.w * yq.y - hq.x * yq.z + hq.y * yq.w + hq.z * yq.x,
                                          hq.w * yq.z + hq.x * yq.y - hq.y * yq.x + hq.z * yq.w, hq.w * yq.w - hq.x * yq.x - hq.y * yq.y - hq.z * yq.z };
        }
        views[i].fov = EyeFov(i);
        ToLocal(views[i].pose);
    }
    if (head)
    {   // the VIEW space: between the eyes, the head's own orientation
        head->orientation = hq; head->position = { gInHeadX, 1.6f + gInHeadY, gInHeadZ };
        ToLocal(*head);
    }
}
MOCK_FN Mock_xrLocateViews(XrSession, const XrViewLocateInfo* vli, XrViewState* vs,
    uint32_t cap, uint32_t* count, XrView* views)
{
    *count = 2;
    vs->viewStateFlags = gInHeadLost > 0.5f ? 0 : (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT);
    if (cap < 2) return XR_SUCCESS;
    // at the time ASKED for (it used to be the last frame's display time whatever the app asked: a prediction for
    // another time was unmeasurable)
    const int64_t t = vli && vli->displayTime > 0 ? (int64_t)vli->displayTime : gLastDisplay;
    HeadViewsAt(t, views, true);
    for (int i = 0; i < 2; i++)
    {
        // Lost: the WORST case a runtime is allowed to hand back -- the pose is undefined when the VALID bits are
        // clear, and some return zeros. Anything that still uses it would put the head on the floor.
        if (gInHeadLost > 0.5f) { views[i].pose.orientation = { 0, 0, 0, 1 }; views[i].pose.position = { 0, 0, 0 }; }
    }
    return XR_SUCCESS;
}

// ---- Input (controller actions) ----
struct MockActionSet_T { int _; } gActionSet;
struct MockAction { std::string name; XrActionType type; };
struct MockActionSpace { int hand; };

void ReadMockInput()
{
    // Read EVERY frame, sharing the file with the writer. Gating the read on the file's write time lost short
    // pulses (a 0.2 s stick click never arrived, measured 2026-09-30), and fopen's default sharing is what used to
    // collide with the harness writing it. A read that fails or comes back empty keeps the previous values.
    static std::mutex m; std::lock_guard<std::mutex> lk(m);   // read from the app's render thread and its frame thread
    HANDLE h = CreateFileA("DVR_MockInput.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[2048]; DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return;
    buf[got] = 0;
    {
        int n = sscanf_s(buf, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f"
                              " %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
            &gInTurnX, &gInTrigR, &gInHandYaw, &gInHandPitch,
            &gInHeadYaw, &gInHeadPitch, &gInHeadX, &gInHeadY, &gInHeadZ,
            &gInMoveX, &gInMoveY, &gInHandX, &gInHandY, &gInHandZ,
            &gInBtnA, &gInBtnB, &gInBtnX, &gInBtnY,
            &gInTurnY, &gInTrigL, &gInGripL, &gInGripR,
            &gInBtnThumbL, &gInBtnThumbR, &gInBtnMenu,
            &gInLHandYaw, &gInLHandPitch, &gInLHandX, &gInLHandY, &gInLHandZ,
            &gInHandRoll, &gInLHandRoll, &gInRHandLost, &gInLHandLost, &gInHeadLost, &gInRecenter,
            &gInSwing, &gInSwKind, &gInSwAx, &gInSwAy, &gInSwAz, &gInSwFrom, &gInSwTo, &gInSwDur,
            &gInSwPivX, &gInSwPivY, &gInSwPivZ, &gInSwRet, &gInSwHand, &gInSwRelL, &gInHeadSway, &gInHandJit,
            &gInSessionLoss, &gInNoHeadset, &gInHeadSpin,
            &gInPaceHz, &gInXrStall, &gInStallSec, &gInStallAll, &gInXrStop, &gInReadyAfter, &gInStarve);
        if (n >= 1)
            gHaveInput = true;
    }
    // The counters (swing, recenter) act on a CHANGE, and the first read only takes them as the baseline. They used to
    // act on an increase: the value left in the harness's state file then fired a recentre at startup, and after the
    // harness reset the file to zeros no swing would start until the counter climbed past the old value (measured:
    // swings 1 and 2 did nothing after a session that had reached 12).
    static bool baseline = true;
    if (baseline) { baseline = false; gSwingDone = gInSwing; gRecenterDone = gInRecenter; gLossDone = gInSessionLoss; gStallDone = gInXrStall < 0.f ? 0.f : gInXrStall; gStopDone = gInXrStop; }   // xrStall < 0 at startup = the first frames stall
    // A new swing: its parameters are latched now, so the harness can prepare the next one while this one runs.
    if (gInSwing != gSwingDone)
    {
        gSwingDone = gInSwing;
        gSw = { gInSwKind, gInSwAx, gInSwAy, gInSwAz, gInSwFrom, gInSwTo, gInSwDur, gInSwPivX, gInSwPivY, gInSwPivZ, gInSwRet,
                gInSwHand, gInSwRelL };
        gSwingStart = gLastDisplay;
        L("swing #%.0f %s hand kind %.0f axis (%.2f,%.2f,%.2f) %.2f -> %.2f in %.3f s, pivot (%.2f,%.2f,%.2f), wind-up/return %.2f s, L release at %.3f s",
          gInSwing, gSw.hand >= 0.5f ? "LEFT" : "right", gSw.kind, gSw.ax, gSw.ay, gSw.az, gSw.from, gSw.to, gSw.dur,
          gSw.pivX, gSw.pivY, gSw.pivZ, gSw.ret, gSw.relL);
    }
}

// Paths are hashes (xrStringToPath), so the strings the mod may ask back (xrPathToString) are remembered here.
std::vector<std::pair<XrPath, std::string>> gPaths;
MOCK_FN Mock_xrStringToPath(XrInstance, const char* str, XrPath* path)
{
    // FNV-1a hash -> stable non-zero path atom.
    uint64_t h = 1469598103934665603ULL;
    for (const char* p = str; *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    *path = (XrPath)(h ? h : 1);
    bool known = false; for (auto& pr : gPaths) if (pr.first == *path) { known = true; break; }
    if (!known && gPaths.size() < 4096) gPaths.push_back({ *path, str });
    return XR_SUCCESS;
}
MOCK_FN Mock_xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out)
{
    *out = (XrActionSet)&gActionSet; return XR_SUCCESS;
}
MOCK_FN Mock_xrDestroyActionSet(XrActionSet) { return XR_SUCCESS; }
MOCK_FN Mock_xrCreateAction(XrActionSet, const XrActionCreateInfo* ci, XrAction* out)
{
    MockAction* a = new MockAction();
    a->name = ci->actionName;
    a->type = ci->actionType;
    *out = (XrAction)a;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrSuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding*) { return XR_SUCCESS; }
MOCK_FN Mock_xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*) { gProfileEventPending = true; return XR_SUCCESS; }
MOCK_FN Mock_xrPathToString(XrInstance, XrPath path, uint32_t cap, uint32_t* count, char* buf)
{
    for (auto& pr : gPaths)
        if (pr.first == path)
        {
            *count = (uint32_t)pr.second.size() + 1;
            if (cap == 0) return XR_SUCCESS;
            if (cap < *count) return XR_ERROR_SIZE_INSUFFICIENT;
            strcpy_s(buf, cap, pr.second.c_str());
            return XR_SUCCESS;
        }
    return XR_ERROR_PATH_INVALID;
}
MOCK_FN Mock_xrGetCurrentInteractionProfile(XrSession, XrPath, XrInteractionProfileState* st)
{
    XrPath p; Mock_xrStringToPath(nullptr, "/interaction_profiles/oculus/touch_controller", &p);
    st->interactionProfile = gProfileNone ? XR_NULL_PATH : p;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* out)
{
    MockAction* a = (MockAction*)ci->action;
    MockActionSpace* s = new MockActionSpace();
    s->hand = (a && a->name.size() && a->name.back() == 'r') ? 1 : 0;
    *out = (XrSpace)s;
    return XR_SUCCESS;
}
MOCK_FN Mock_xrSyncActions(XrSession, const XrActionsSyncInfo*) { StallGate(); ReadMockInput(); return XR_SUCCESS; }
MOCK_FN Mock_xrGetActionStateFloat(XrSession, const XrActionStateGetInfo* gi, XrActionStateFloat* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = XR_TRUE;
    st->currentState = 0.0f;
    if (a)
    {
        if (a->name == "turn") st->currentState = gInTurnX;
        else if (a->name == "turn_y") st->currentState = gInTurnY;
        else if (a->name == "trigger_r") st->currentState = gInTrigR;
        else if (a->name == "trigger_l") { float v; st->currentState = SwingTriggerL(v) ? v : gInTrigL; }
        else if (a->name == "grip_l") st->currentState = gInGripL;
        else if (a->name == "grip_r") st->currentState = gInGripR;
    }
    return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo* gi, XrActionStateVector2f* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = XR_TRUE;
    st->currentState = { 0.0f, 0.0f };
    if (a && a->name == "move")
        st->currentState = { gInMoveX, gInMoveY };
    return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* st)
{
    st->isActive = XR_TRUE; return XR_SUCCESS;
}
MOCK_FN Mock_xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* st)
{
    MockAction* a = (MockAction*)gi->action;
    st->isActive = XR_TRUE;
    st->currentState = XR_FALSE;
    if (a)
    {
        float v = 0;
        if (a->name == "btn_a") v = gInBtnA;
        else if (a->name == "btn_b") v = gInBtnB;
        else if (a->name == "btn_x") v = gInBtnX;
        else if (a->name == "btn_y") v = gInBtnY;
        else if (a->name == "btn_thumb_l") v = gInBtnThumbL;
        else if (a->name == "btn_thumb_r") v = gInBtnThumbR;
        else if (a->name == "btn_menu") v = gInBtnMenu;
        st->currentState = (v > 0.5f) ? XR_TRUE : XR_FALSE;
    }
    return XR_SUCCESS;
}
MOCK_FN Mock_xrApplyHapticFeedback(XrSession, const XrHapticActionInfo* info, const XrHapticBaseHeader* h)
{
    MockAction* a = info ? (MockAction*)info->action : nullptr;
    const int hand = (a && a->name.size() && a->name.back() == 'r') ? 1 : 0;
    const XrHapticVibration* v = (const XrHapticVibration*)h;
    gHaptics[hand]++;
    L("haptic %s #%llu amp %.2f dur %.3f s freq %.0f", hand ? "R" : "L", gHaptics[hand], v ? v->amplitude : 0.f,
      v ? (double)v->duration * 1e-9 : 0.0, v ? v->frequency : 0.f);
    return XR_SUCCESS;
}
MOCK_FN Mock_xrStopHapticFeedback(XrSession, const XrHapticActionInfo*) { return XR_SUCCESS; }

MOCK_FN Mock_xrLocateSpace(XrSpace space, XrSpace, XrTime time, XrSpaceLocation* loc)
{
    if (space == (XrSpace)&gViewSpace)
    {   // the head (VIEW) in LOCAL
        XrView v[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; XrPosef hp;
        HeadViewsAt(time > 0 ? (int64_t)time : gLastDisplay, v, false, &hp);
        loc->pose = hp;
        loc->locationFlags = gInHeadLost > 0.5f ? 0 : (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                     XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
        return XR_SUCCESS;
    }
    MockActionSpace* s = (MockActionSpace*)space;
    // Simulated hand aim poses: in front of the head, aiming along -Z. The
    // right hand is driven by hand*, the left by lhand* (independent, so
    // left-hand psi aim and VR-hand tests can steer them separately).
    const bool right = (s && s->hand == 1);
    float jy = 0, jp = 0; HandJitter(time, right ? 1 : 0, jy, jp);
    float yaw = ((right ? gInHandYaw : gInLHandYaw) + jy) * 3.14159265f / 180.0f;
    float pitch = ((right ? gInHandPitch : gInLHandPitch) + jp) * 3.14159265f / 180.0f;
    float roll = (right ? gInHandRoll : gInLHandRoll) * 3.14159265f / 180.0f;
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
    // q = yaw(Y) * pitch(X) * roll(Z). The roll is LAST, so it turns the hand about its own pointing axis
    // whatever the aim direction is -- which is what a real wrist does and what the palm tests need.
    const float aw = cy * cp, ax = cy * sp, ay = sy * cp, az = -sy * sp;   // yaw * pitch, as before
    loc->pose.orientation.w = aw * cr - az * sr;
    loc->pose.orientation.x = ax * cr + ay * sr;
    loc->pose.orientation.y = ay * cr - ax * sr;
    loc->pose.orientation.z = az * cr + aw * sr;
    if (right)
        loc->pose.position = { 0.2f + gInHandX, 1.4f + gInHandY, -0.3f + gInHandZ };
    else
        loc->pose.position = { -0.2f + gInLHandX, 1.4f + gInLHandY, -0.3f + gInLHandZ };
    loc->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (right == (gSw.hand < 0.5f)) ApplySwing(time, loc->pose);
    ToLocal(loc->pose);
    if ((right ? gInRHandLost : gInLHandLost) > 0.5f)
    {   // undefined pose with the bits clear: zeros, the worst case (see Mock_xrLocateViews)
        loc->locationFlags = 0;
        loc->pose.orientation = { 0, 0, 0, 1 }; loc->pose.position = { 0, 0, 0 };
    }
    return XR_SUCCESS;
}

bool LocateLayerSpace(XrSpace space, XrPosef& out)
{
    if (space == (XrSpace)&gSpace) { out = { { 0, 0, 0, 1 }, { 0, 0, 0 } }; return true; }   // LOCAL
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    Mock_xrLocateSpace(space, (XrSpace)&gSpace, gLastDisplay, &loc);
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
    out = loc.pose; return true;
}

MOCK_FN Mock_xrAcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
    StallGate();
    MockSwapchain* sc = (MockSwapchain*)h;
    *index = sc->acquired;
    sc->acquired = (sc->acquired + 1) % (uint32_t)sc->tex.size();
    return XR_SUCCESS;
}
// While the compositor is stalled it holds its images: a wait for one blocks (stallAll 0 too), up to the app's timeout.
MOCK_FN Mock_xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo* wi)
{
    StallGate();
    const int64_t until = gStallUntilNs.load(), now = NowNs();
    if (now < until)
    {
        const int64_t to = wi ? (int64_t)wi->timeout : 0;
        if (to >= 0 && to < until - now) { SleepUntilNs(now + to); return XR_TIMEOUT_EXPIRED; }
        SleepUntilNs(until);
    }
    return XR_SUCCESS;
}
MOCK_FN Mock_xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }

// ---- Dispatch ----
extern "C" XRAPI_ATTR XrResult XRAPI_CALL Mock_xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* fn)
{
    *fn = nullptr;
    auto set = [&](void* p) { *fn = (PFN_xrVoidFunction)p; return XR_SUCCESS; };
#define BIND(n) if (strcmp(name, #n) == 0) return set((void*)Mock_##n)
    BIND(xrEnumerateInstanceExtensionProperties);
    BIND(xrCreateInstance);
    BIND(xrDestroyInstance);
    BIND(xrGetInstanceProperties);
    BIND(xrGetSystem);
    BIND(xrGetSystemProperties);
    BIND(xrEnumerateViewConfigurationViews);
    BIND(xrGetD3D11GraphicsRequirementsKHR);
    BIND(xrCreateSession);
    BIND(xrDestroySession);
    BIND(xrCreateReferenceSpace);
    BIND(xrDestroySpace);
    BIND(xrEnumerateSwapchainFormats);
    BIND(xrCreateSwapchain);
    BIND(xrDestroySwapchain);
    BIND(xrEnumerateSwapchainImages);
    BIND(xrPollEvent);
    BIND(xrBeginSession);
    BIND(xrEndSession);
    BIND(xrWaitFrame);
    BIND(xrBeginFrame);
    BIND(xrEndFrame);
    BIND(xrLocateViews);
    BIND(xrAcquireSwapchainImage);
    BIND(xrWaitSwapchainImage);
    BIND(xrReleaseSwapchainImage);
    BIND(xrStringToPath);
    BIND(xrPathToString);
    BIND(xrGetCurrentInteractionProfile);
    BIND(xrCreateActionSet);
    BIND(xrDestroyActionSet);
    BIND(xrCreateAction);
    BIND(xrSuggestInteractionProfileBindings);
    BIND(xrAttachSessionActionSets);
    BIND(xrCreateActionSpace);
    BIND(xrSyncActions);
    BIND(xrGetActionStateFloat);
    BIND(xrGetActionStateVector2f);
    BIND(xrGetActionStatePose);
    BIND(xrGetActionStateBoolean);
    BIND(xrLocateSpace);
    BIND(xrApplyHapticFeedback);
    BIND(xrStopHapticFeedback);
#undef BIND
    if (strcmp(name, "xrGetInstanceProcAddr") == 0)
        return set((void*)Mock_xrGetInstanceProcAddr);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---- Loader negotiation entry point ----
extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* req)
{
    L("xrNegotiateLoaderRuntimeInterface");
    if (!loaderInfo || !req) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;

    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = Mock_xrGetInstanceProcAddr;
    return XR_SUCCESS;
}
