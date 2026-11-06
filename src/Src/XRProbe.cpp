#include "pch.h"
#include "XRProbe.h"
#include <openxr/openxr.h>

namespace { std::string g_Summary = "not probed"; std::atomic<bool> g_Done{ false }; }

bool XRProbe::Done() { return g_Done.load(); }

void XRProbe::Run()
{
    struct MarkDone { ~MarkDone() { g_Done = true; } } markDone;   // every return path, the instance destroyed first
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "DOTOVR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "VoidEngine");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrInstance inst = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance(&ci, &inst);
    if (XR_FAILED(r)) { g_Summary = fmt::format("xrCreateInstance failed ({})", (int)r); LOGI("XRProbe: {}", g_Summary); return; }
    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(inst, &ip);
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(inst, &sgi, &sys);
    if (XR_SUCCEEDED(r))
    {
        XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
        xrGetSystemProperties(inst, sys, &sp);
        g_Summary = fmt::format("{} / {} (max swapchain {}x{})", ip.runtimeName, sp.systemName,
                                sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);
    }
    else g_Summary = fmt::format("{} - no HMD (xrGetSystem {}; headset asleep / Link off?)", ip.runtimeName, (int)r);
    LOGI("XRProbe: {}", g_Summary);
    xrDestroyInstance(inst);
}

const std::string& XRProbe::Summary() { return g_Summary; }
