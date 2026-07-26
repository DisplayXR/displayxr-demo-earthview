// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// linux/main.cpp — the Linux entry point for the EarthView demo (#19
// build-green harness, upgraded to a real handle app by #34).
//
// WINDOWING — HANDLE app (matching the binary name and the other platforms):
// the app owns a decorated X11 window centered on the 3D panel and passes it
// via XR_DXR_xlib_window_binding, so the runtime weaves window-relative
// (runtime#729/#730) and the app receives keyboard input. The window defaults
// to 1920x1080 centered on the panel (XR_DXR_display_info desktop rect, else
// Xrandr); EARTHVIEW_WINDOW="WxH+X+Y" overrides (X,Y absolute virtual-desktop
// px). When no X server is available (or window creation fails) the app falls
// back to the previous hosted-NULL path — which also keeps this compiling and
// startable on the build-green CI runner.
//
// RENDERING — extension app: enumerates + requests display rendering modes,
// tiles = window x recommendedViewScaleXY, chains the XR_DXR_view_rig CAMERA
// rig on xrLocateViews (the default camera-centric FLY view — the runtime owns
// the off-axis eyes; convergence auto-focuses on the forward ground ray), and
// remaps the GL projection to Vulkan [0,1] depth. Mirrors the windows/main.cpp
// fly path; orbit/focus/HUD/MCP/atlas-capture remain per-platform UI concerns.
//
// INPUT — B cycles city bookmarks (Windows parity); close button exits. NO
// file-open by design: EarthView streams tiles, there is no model to load.

#define XR_USE_GRAPHICS_API_VULKAN

#include <vulkan/vulkan.h>

// X11 window + input (handle app) — before the OpenXR platform header so the
// xlib binding struct sees the real Display/Window types.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// DisplayXR extension headers (vendored openxr_includes/, refreshed from
// displayxr-extensions).
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_xlib_window_binding.h>

#include "projection_depth.h"

#include "geo_math.h"
#include "tile_engine.h"
#include "tile_renderer.h"

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ── logging + result checks ──────────────────────────────────────────────
#define LOG_INFO(...)                                                          \
	do {                                                                       \
		fprintf(stderr, "[earthview] ");                                       \
		fprintf(stderr, __VA_ARGS__);                                          \
		fprintf(stderr, "\n");                                                 \
	} while (0)
#define LOG_WARN(...) LOG_INFO(__VA_ARGS__)
#define LOG_ERROR(...) LOG_INFO(__VA_ARGS__)

#define XR_CHECK(expr)                                                         \
	do {                                                                       \
		XrResult _r = (expr);                                                  \
		if (XR_FAILED(_r)) {                                                   \
			LOG_ERROR("%s failed: %d", #expr, (int)_r);                        \
			return false;                                                      \
		}                                                                      \
	} while (0)

#define VK_CHECK(expr)                                                         \
	do {                                                                       \
		VkResult _r = (expr);                                                  \
		if (_r != VK_SUCCESS) {                                                \
			LOG_ERROR("%s failed: %d", #expr, (int)_r);                        \
			return false;                                                      \
		}                                                                      \
	} while (0)

static volatile sig_atomic_t g_running = 1;
static void
SignalHandler(int)
{
	g_running = 0;
}

// ── minimal matrix helpers (ported from macos/main.mm) ───────────────────
static void
mat4_from_xr_fov(float *m, XrFovf fov, float nearZ, float farZ)
{
	const float l = std::tan(fov.angleLeft);
	const float r = std::tan(fov.angleRight);
	const float d = std::tan(fov.angleDown);
	const float u = std::tan(fov.angleUp);
	const float w = r - l;
	const float h = u - d; // note: XR up is +, down is -
	for (int i = 0; i < 16; i++)
		m[i] = 0.0f;
	m[0] = 2.0f / w;
	m[5] = 2.0f / h;
	m[8] = (r + l) / w;
	m[9] = (u + d) / h;
	m[10] = -(farZ + nearZ) / (farZ - nearZ);
	m[11] = -1.0f;
	m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void
mat4_view_from_xr_pose(float *v, XrPosef pose)
{
	// Rotation from quaternion, transposed (inverse of a unit quat = conj),
	// then translate by -R^T * p. Right-handed, looking down -Z.
	const float x = pose.orientation.x, y = pose.orientation.y,
	            z = pose.orientation.z, w = pose.orientation.w;
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;

	float R[9] = {1 - 2 * (yy + zz), 2 * (xy - wz),     2 * (xz + wy),
	              2 * (xy + wz),     1 - 2 * (xx + zz), 2 * (yz - wx),
	              2 * (xz - wy),     2 * (yz + wx),     1 - 2 * (xx + yy)};
	// view = R^T (world->view rotation) then -R^T * pos.
	const float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
	v[0] = R[0]; v[1] = R[3]; v[2] = R[6]; v[3] = 0;
	v[4] = R[1]; v[5] = R[4]; v[6] = R[7]; v[7] = 0;
	v[8] = R[2]; v[9] = R[5]; v[10] = R[8]; v[11] = 0;
	v[12] = -(R[0] * px + R[1] * py + R[2] * pz);
	v[13] = -(R[3] * px + R[4] * py + R[5] * pz);
	v[14] = -(R[6] * px + R[7] * py + R[8] * pz);
	v[15] = 1;
}

// ── OpenXR session state ─────────────────────────────────────────────────
struct AppXrSession
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE;
	XrViewConfigurationType viewConfigType =
	    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	bool sessionRunning = false;
	bool exitRequested = false;
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
	char systemName[256] = {};

	struct
	{
		XrSwapchain swapchain = XR_NULL_HANDLE;
		uint32_t width = 0, height = 0, imageCount = 0;
		int64_t format = 0;
	} swapchain;

	bool hasDisplayInfoExt = false;
	bool hasViewRigExt = false;
	bool hasXlibBindingExt = false;
	uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
	float displayWidthM = 0, displayHeightM = 0;
	float nominalViewerZ = 0.5f;
	int32_t displayScreenLeft = 0; // 3D-panel top-left in virtual-desktop px
	int32_t displayScreenTop = 0;

	// App-owned X11 window (handle app). Null display = hosted-NULL fallback.
	Display *xDisplay = nullptr;
	::Window xWindow = 0;
	unsigned int xWinW = 0, xWinH = 0;

	PFN_xrRequestDisplayRenderingModeDXR pfnRequestMode = nullptr;
	PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateModes = nullptr;

	uint32_t renderingModeCount = 0;
	uint32_t renderingModeViewCounts[8] = {};
	float renderingModeScaleX[8] = {};
	float renderingModeScaleY[8] = {};
	bool renderingModeDisplay3D[8] = {};
	uint32_t renderingModeTileColumns[8] = {};
	uint32_t renderingModeTileRows[8] = {};
	uint32_t currentRenderingMode = 1; // default: first 3D mode

	uint32_t maxViewCount = 2;
};

// ── OpenXR + Vulkan bootstrap (Linux arm of the macOS harness, no MoltenVK
//    portability bits, no window binding) ─────────────────────────────────
static bool
InitializeOpenXR(AppXrSession &xr)
{
	uint32_t extCount = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
	std::vector<XrExtensionProperties> exts(extCount,
	                                         {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount,
	                                        exts.data());

	bool hasVulkan = false;
	for (const auto &e : exts) {
		if (strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
			hasVulkan = true;
		if (strcmp(e.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0)
			xr.hasDisplayInfoExt = true;
		if (strcmp(e.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0)
			xr.hasViewRigExt = true;
		if (strcmp(e.extensionName, XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0)
			xr.hasXlibBindingExt = true;
	}
	if (!hasVulkan) {
		LOG_ERROR("XR_KHR_vulkan_enable not available");
		return false;
	}

	std::vector<const char *> enabled;
	enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (xr.hasDisplayInfoExt)
		enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
	if (xr.hasViewRigExt)
		enabled.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
	if (xr.hasXlibBindingExt)
		enabled.push_back(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);

	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strncpy(ci.applicationInfo.applicationName, "DisplayXREarthViewLinux",
	        sizeof(ci.applicationInfo.applicationName) - 1);
	ci.applicationInfo.applicationVersion = 1;
	strncpy(ci.applicationInfo.engineName, "None",
	        sizeof(ci.applicationInfo.engineName) - 1);
	ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ci.enabledExtensionCount = (uint32_t)enabled.size();
	ci.enabledExtensionNames = enabled.data();
	XR_CHECK(xrCreateInstance(&ci, &xr.instance));

	XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(xr.instance, &si, &xr.systemId));

	{
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		xrGetSystemProperties(xr.instance, xr.systemId, &sp);
		memcpy(xr.systemName, sp.systemName, sizeof(xr.systemName));
	}

	if (xr.hasDisplayInfoExt) {
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		XrDisplayInfoDXR di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
		XrDisplayDesktopPositionDXR desktopPos = {};
		desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
		di.next = &desktopPos;
		sp.next = &di;
		if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
			xr.displayWidthM = di.displaySizeMeters.width;
			xr.displayHeightM = di.displaySizeMeters.height;
			xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
			xr.displayPixelWidth = di.displayPixelWidth;
			xr.displayPixelHeight = di.displayPixelHeight;
			xr.displayScreenLeft = desktopPos.left;
			xr.displayScreenTop = desktopPos.top;
		}
		xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR",
		                      (PFN_xrVoidFunction *)&xr.pfnRequestMode);
		xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
		                      (PFN_xrVoidFunction *)&xr.pfnEnumerateModes);
	}

	LOG_INFO("OpenXR initialized: %s", xr.systemName);
	return true;
}

static bool
GetVulkanGraphicsRequirements(AppXrSession &xr)
{
	PFN_xrGetVulkanGraphicsRequirementsKHR fn = nullptr;
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR",
	                      (PFN_xrVoidFunction *)&fn);
	if (!fn)
		return false;
	XrGraphicsRequirementsVulkanKHR req = {
	    XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	return XR_SUCCEEDED(fn(xr.instance, xr.systemId, &req));
}

static std::vector<std::string>
SplitSpaceList(const std::string &s)
{
	std::vector<std::string> out;
	size_t i = 0;
	while (i < s.size()) {
		size_t e = s.find(' ', i);
		if (e == std::string::npos)
			e = s.size();
		std::string n = s.substr(i, e - i);
		if (!n.empty() && n[0] != '\0')
			out.push_back(n);
		i = e + 1;
	}
	return out;
}

static bool
CreateVulkanInstance(AppXrSession &xr, VkInstance &vkInstance)
{
	PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR",
	                      (PFN_xrVoidFunction *)&fn);
	if (!fn)
		return false;
	uint32_t bufSize = 0;
	fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
	std::string extStr(bufSize, '\0');
	fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
	std::vector<std::string> extNames = SplitSpaceList(extStr);
	std::vector<const char *> extPtrs;
	for (auto &n : extNames)
		extPtrs.push_back(n.c_str());

	VkApplicationInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	ai.pApplicationName = "DisplayXREarthViewLinux";
	ai.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ci.pApplicationInfo = &ai;
	ci.enabledExtensionCount = (uint32_t)extPtrs.size();
	ci.ppEnabledExtensionNames = extPtrs.data();
	VK_CHECK(vkCreateInstance(&ci, nullptr, &vkInstance));
	return true;
}

static bool
GetVulkanPhysicalDevice(AppXrSession &xr, VkInstance vkInstance,
                        VkPhysicalDevice &pd)
{
	PFN_xrGetVulkanGraphicsDeviceKHR fn = nullptr;
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR",
	                      (PFN_xrVoidFunction *)&fn);
	if (!fn)
		return false;
	XR_CHECK(fn(xr.instance, xr.systemId, vkInstance, &pd));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	LOG_INFO("GPU: %s", props.deviceName);
	return true;
}

static bool
GetVulkanDeviceExtensions(AppXrSession &xr, std::vector<const char *> &exts,
                          std::vector<std::string> &storage)
{
	PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
	xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR",
	                      (PFN_xrVoidFunction *)&fn);
	if (!fn)
		return false;
	uint32_t bufSize = 0;
	fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
	std::string extStr(bufSize, '\0');
	fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
	storage = SplitSpaceList(extStr);
	for (auto &n : storage)
		exts.push_back(n.c_str());
	return true;
}

static bool
FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t &idx)
{
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
	std::vector<VkQueueFamilyProperties> fams(count);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
	for (uint32_t i = 0; i < count; i++) {
		if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			idx = i;
			return true;
		}
	}
	return false;
}

static bool
CreateVulkanDevice(VkPhysicalDevice pd, uint32_t qfi,
                   const std::vector<const char *> &exts, VkDevice &dev,
                   VkQueue &queue)
{
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qi = {};
	qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qi.queueFamilyIndex = qfi;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;

	VkPhysicalDeviceFeatures features = {};
	features.shaderInt64 = VK_TRUE;
	{
		VkPhysicalDeviceFeatures supported = {};
		vkGetPhysicalDeviceFeatures(pd, &supported);
		features.samplerAnisotropy = supported.samplerAnisotropy;
		features.shaderStorageImageWriteWithoutFormat =
		    supported.shaderStorageImageWriteWithoutFormat;
	}

	VkDeviceCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	ci.queueCreateInfoCount = 1;
	ci.pQueueCreateInfos = &qi;
	ci.enabledExtensionCount = (uint32_t)exts.size();
	ci.ppEnabledExtensionNames = exts.data();
	ci.pEnabledFeatures = &features;
	VK_CHECK(vkCreateDevice(pd, &ci, nullptr, &dev));
	vkGetDeviceQueue(dev, qfi, 0, &queue);
	return true;
}

static bool
CreateSession(AppXrSession &xr, VkInstance vkInstance, VkPhysicalDevice pd,
              VkDevice dev, uint32_t qfi)
{
	XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	vkBinding.instance = vkInstance;
	vkBinding.physicalDevice = pd;
	vkBinding.device = dev;
	vkBinding.queueFamilyIndex = qfi;
	vkBinding.queueIndex = 0;

	// Handle app: pass the app-owned X11 window via XR_DXR_xlib_window_binding
	// so the runtime weaves window-relative (runtime#729/#730). Falls back to
	// hosted-NULL (no window binding — the runtime self-creates a window at
	// native resolution) when CreateAppWindow didn't run.
	XrXlibWindowBindingCreateInfoDXR xlibBinding = {
	    XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR};
	xlibBinding.next = &vkBinding;
	xlibBinding.xDisplay = xr.xDisplay;
	xlibBinding.window = xr.xWindow;
	xlibBinding.transparentBackgroundEnabled = XR_FALSE;
	const bool useAppWindow =
	    (xr.hasXlibBindingExt && xr.xDisplay != nullptr && xr.xWindow != 0);

	XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
	si.next = useAppWindow ? (const void *)&xlibBinding : (const void *)&vkBinding;
	si.systemId = xr.systemId;
	XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));
	LOG_INFO("Session created (%s)",
	         useAppWindow ? "app-owned window, handle app" : "hosted-NULL");

	if (xr.pfnEnumerateModes && xr.session != XR_NULL_HANDLE) {
		uint32_t modeCount = 0;
		if (XR_SUCCEEDED(xr.pfnEnumerateModes(xr.session, 0, &modeCount, nullptr)) &&
		    modeCount > 0) {
			std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
			for (auto &m : modes) {
				m.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
				m.next = nullptr;
			}
			if (XR_SUCCEEDED(xr.pfnEnumerateModes(xr.session, modeCount, &modeCount,
			                                      modes.data()))) {
				xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
				LOG_INFO("Display rendering modes (%u):", modeCount);
				for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
					xr.renderingModeViewCounts[i] = modes[i].viewCount;
					xr.renderingModeScaleX[i] = modes[i].viewScaleX;
					xr.renderingModeScaleY[i] = modes[i].viewScaleY;
					xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
					xr.renderingModeTileColumns[i] =
					    modes[i].tileColumns ? modes[i].tileColumns : 1;
					xr.renderingModeTileRows[i] =
					    modes[i].tileRows ? modes[i].tileRows : 1;
					LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, tiles=%ux%u, 3D=%d)",
					         modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
					         modes[i].viewScaleX, modes[i].viewScaleY,
					         xr.renderingModeTileColumns[i], xr.renderingModeTileRows[i],
					         modes[i].hardwareDisplay3D);
				}
			}
		}
	}
	return true;
}

static bool
CreateSpaces(AppXrSession &xr)
{
	XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation.w = 1.0f;
	XR_CHECK(xrCreateReferenceSpace(xr.session, &ci, &xr.localSpace));
	return true;
}

static bool
CreateSwapchains(AppXrSession &xr)
{
	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(xr.instance, xr.systemId,
	                                  xr.viewConfigType, 0, &viewCount, nullptr);
	std::vector<XrViewConfigurationView> views(
	    viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	xrEnumerateViewConfigurationViews(xr.instance, xr.systemId,
	                                  xr.viewConfigType, viewCount, &viewCount,
	                                  views.data());
	xr.maxViewCount = viewCount ? viewCount : 2;

	uint32_t fmtCount = 0;
	xrEnumerateSwapchainFormats(xr.session, 0, &fmtCount, nullptr);
	std::vector<int64_t> fmts(fmtCount);
	xrEnumerateSwapchainFormats(xr.session, fmtCount, &fmtCount, fmts.data());
	int64_t selectedFmt = fmts.empty() ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM
	                                    : fmts[0];
	for (auto f : fmts) {
		if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) {
			selectedFmt = f;
			break;
		}
		if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM)
			selectedFmt = f;
	}

	// Worst-case-size across advertised modes (see swapchain-model.md).
	uint32_t w = (views.empty() ? 1024 : views[0].recommendedImageRectWidth) * 2;
	uint32_t h = views.empty() ? 1024 : views[0].recommendedImageRectHeight;
	if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
		w = xr.displayPixelWidth;
		h = xr.displayPixelHeight;
		for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
			uint32_t aw = (uint32_t)((double)xr.renderingModeTileColumns[i] *
			                         xr.renderingModeScaleX[i] *
			                         (double)xr.displayPixelWidth);
			uint32_t ah = (uint32_t)((double)xr.renderingModeTileRows[i] *
			                         xr.renderingModeScaleY[i] *
			                         (double)xr.displayPixelHeight);
			if (aw > w)
				w = aw;
			if (ah > h)
				h = ah;
		}
	}

	XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                 XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sci.format = selectedFmt;
	sci.sampleCount = 1;
	sci.width = w;
	sci.height = h;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = 1;
	XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
	xr.swapchain.width = w;
	xr.swapchain.height = h;
	xr.swapchain.format = selectedFmt;

	uint32_t imgCount = 0;
	xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
	xr.swapchain.imageCount = imgCount;
	LOG_INFO("Swapchain: %ux%u, %u images", w, h, imgCount);
	return true;
}

static void
PollEvents(AppXrSession &xr)
{
	XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto *ssc = (XrEventDataSessionStateChanged *)&event;
			xr.sessionState = ssc->state;
			if (ssc->state == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = xr.viewConfigType;
				xrBeginSession(xr.session, &bi);
				xr.sessionRunning = true;
			} else if (ssc->state == XR_SESSION_STATE_STOPPING) {
				xrEndSession(xr.session);
				xr.sessionRunning = false;
			} else if (ssc->state == XR_SESSION_STATE_EXITING ||
			           ssc->state == XR_SESSION_STATE_LOSS_PENDING) {
				xr.exitRequested = true;
			}
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

static void
CleanupOpenXR(AppXrSession &xr)
{
	if (xr.swapchain.swapchain)
		xrDestroySwapchain(xr.swapchain.swapchain);
	if (xr.localSpace)
		xrDestroySpace(xr.localSpace);
	if (xr.session)
		xrDestroySession(xr.session);
	if (xr.instance)
		xrDestroyInstance(xr.instance);
	// Tear down the app-owned X11 window after the runtime has released it.
	if (xr.xWindow != 0 && xr.xDisplay != nullptr)
		XDestroyWindow(xr.xDisplay, xr.xWindow);
	if (xr.xDisplay != nullptr)
		XCloseDisplay(xr.xDisplay);
	xr.xWindow = 0;
	xr.xDisplay = nullptr;
}

// ── EarthView scene state ────────────────────────────────────────────────
static TileRenderer g_tileRenderer;
static TileEngine g_tileEngine;
static geo::GeoNav g_geoNav;
static bool g_tilesActive = false;
static std::vector<TileRenderer::DrawItem> g_drawList;

// Tile basis for a handle app: the app window (window × viewScale, #729-style).
// Falls back to the display size on the hosted-NULL path.
static uint32_t g_windowW = 1920, g_windowH = 1080;

// ── app-owned X11 window (handle app) ────────────────────────────────────
static const unsigned int kDefaultWindowW = 1920;
static const unsigned int kDefaultWindowH = 1080;
static Atom g_wmDeleteAtom = None;

// Find the target panel rect (virtual-desktop px). Prefer the RandR PRIMARY
// output; else the largest connected NON-eDP/LVDS output (the 3D display is
// an external panel, not the laptop's built-in). Same helper as the other
// demos.
static bool
GetPanelRect(Display *dpy, ::Window root, int &x, int &y, int &w, int &h)
{
	XRRScreenResources *res = XRRGetScreenResources(dpy, root);
	if (res == nullptr)
		return false;

	auto tryOutput = [&](RROutput out) -> bool {
		XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, out);
		if (oi == nullptr)
			return false;
		bool ok = false;
		if (oi->connection == RR_Connected && oi->crtc != 0) {
			XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
			if (ci != nullptr && ci->width > 0 && ci->height > 0) {
				x = ci->x;
				y = ci->y;
				w = (int)ci->width;
				h = (int)ci->height;
				ok = true;
			}
			if (ci != nullptr)
				XRRFreeCrtcInfo(ci);
		}
		XRRFreeOutputInfo(oi);
		return ok;
	};

	bool found = false;
	RROutput primary = XRRGetOutputPrimary(dpy, root);
	if (primary != 0 && tryOutput(primary))
		found = true;
	if (!found) {
		long bestArea = 0;
		for (int i = 0; i < res->noutput; i++) {
			XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
			if (oi == nullptr)
				continue;
			const bool isBuiltin =
			    oi->name != nullptr && (strncasecmp(oi->name, "eDP", 3) == 0 ||
			                            strncasecmp(oi->name, "LVDS", 4) == 0);
			if (oi->connection == RR_Connected && oi->crtc != 0 && !isBuiltin) {
				XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
				if (ci != nullptr && ci->width > 0 && ci->height > 0) {
					const long area = (long)ci->width * (long)ci->height;
					if (area > bestArea) {
						bestArea = area;
						x = ci->x;
						y = ci->y;
						w = (int)ci->width;
						h = (int)ci->height;
						found = true;
					}
				}
				if (ci != nullptr)
					XRRFreeCrtcInfo(ci);
			}
			XRRFreeOutputInfo(oi);
		}
	}
	XRRFreeScreenResources(res);
	return found;
}

// Create a normal decorated X11 window (opaque, default visual), landscape
// 1920x1080 centered on the 3D panel — the app passes it via
// XR_DXR_xlib_window_binding so the runtime weaves window-relative. Override
// with EARTHVIEW_WINDOW="WxH+X+Y" (X,Y absolute virtual-desktop px; WxH alone
// re-centers). Returns false (xDisplay left null) when no X server is
// available, so the caller falls back to hosted-NULL (also the CI-safe path).
static bool
CreateAppWindow(AppXrSession &xr)
{
	Display *dpy = XOpenDisplay(nullptr);
	if (dpy == nullptr) {
		LOG_INFO("XOpenDisplay failed (no X server) — using hosted-NULL windowing");
		return false;
	}
	int screen = DefaultScreen(dpy);
	::Window root = RootWindow(dpy, screen);

	XSetWindowAttributes attrs = {};
	attrs.background_pixel = BlackPixel(dpy, screen);
	attrs.event_mask = StructureNotifyMask | KeyPressMask;

	unsigned int w = kDefaultWindowW, h = kDefaultWindowH;
	int px = 0, py = 0;
	int prx = 0, pry = 0, prw = 0, prh = 0;
	if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
		// Authoritative: XR_DXR_display_info reports the 3D panel's desktop
		// rect. Prefer it — on a multi-monitor box the RandR PRIMARY is often
		// NOT the 3D panel.
		prx = xr.displayScreenLeft;
		pry = xr.displayScreenTop;
		prw = (int)xr.displayPixelWidth;
		prh = (int)xr.displayPixelHeight;
		px = prx + (prw - (int)w) / 2;
		py = pry + (prh - (int)h) / 2;
		LOG_INFO("3D panel (display_info) %dx%d at (%d,%d) — centering %ux%u window at (%d,%d)",
		         prw, prh, prx, pry, w, h, px, py);
	} else if (GetPanelRect(dpy, root, prx, pry, prw, prh)) {
		px = prx + (prw - (int)w) / 2;
		py = pry + (prh - (int)h) / 2;
		LOG_INFO("Panel rect (Xrandr) %dx%d at (%d,%d) — centering %ux%u window at (%d,%d)",
		         prw, prh, prx, pry, w, h, px, py);
	} else {
		const int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);
		px = (sw - (int)w) / 2;
		py = (sh - (int)h) / 2;
		LOG_INFO("Xrandr panel query failed — centering %ux%u on default screen %dx%d at (%d,%d)",
		         w, h, sw, sh, px, py);
	}

	// EARTHVIEW_WINDOW="WxH+X+Y" override (X,Y absolute virtual-desktop px);
	// WxH alone re-centers on the same panel/screen origin.
	if (const char *wenv = getenv("EARTHVIEW_WINDOW")) {
		unsigned int ow = 0, oh = 0;
		int ox = 0, oy = 0;
		int n = sscanf(wenv, "%ux%u+%d+%d", &ow, &oh, &ox, &oy);
		if (n >= 2 && ow > 0 && oh > 0) {
			w = ow;
			h = oh;
			if (n >= 4) {
				px = ox;
				py = oy;
				LOG_INFO("EARTHVIEW_WINDOW override: %ux%u at absolute (%d,%d)", w, h, px, py);
			} else {
				if (prw > 0 && prh > 0) {
					px = prx + (prw - (int)w) / 2;
					py = pry + (prh - (int)h) / 2;
				}
				LOG_INFO("EARTHVIEW_WINDOW override: %ux%u (re-centered at %d,%d)", w, h, px, py);
			}
		}
	}

	::Window win = XCreateWindow(dpy, root, px, py, w, h, 0, CopyFromParent,
	                             InputOutput, CopyFromParent,
	                             CWBackPixel | CWEventMask, &attrs);
	if (win == 0) {
		LOG_ERROR("XCreateWindow failed — using hosted-NULL windowing");
		XCloseDisplay(dpy);
		return false;
	}
	XStoreName(dpy, win, "DisplayXR EarthView");

	// Close button → clean exit (ClientMessage in the event pump).
	g_wmDeleteAtom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	if (g_wmDeleteAtom != None)
		XSetWMProtocols(dpy, win, &g_wmDeleteAtom, 1);

	// WM_NORMAL_HINTS with USPosition|PPosition so the WM honors the
	// create-time position instead of auto-placing (GNOME/Mutter).
	{
		XSizeHints hints = {};
		hints.flags = USPosition | PPosition;
		hints.x = px;
		hints.y = py;
		XSetWMNormalHints(dpy, win, &hints);
	}

	XMapWindow(dpy, win);
	XFlush(dpy);
	// Re-assert the position after mapping — Mutter ignores the create-time
	// x/y of a freshly-mapped toplevel but honors a post-map move.
	XMoveWindow(dpy, win, px, py);
	XFlush(dpy);

	xr.xDisplay = dpy;
	xr.xWindow = win;
	xr.xWinW = w;
	xr.xWinH = h;
	LOG_INFO("Created %ux%u app window at (%d,%d) — XR_DXR_xlib_window_binding",
	         w, h, px, py);
	return true;
}

// Pump the app window's X11 events: B = cycle city bookmarks (Windows
// parity), close button = clean exit, ConfigureNotify = track live window
// size. NO file-open by design (EarthView streams tiles).
static void
PumpXEvents(AppXrSession &xr)
{
	if (xr.xDisplay == nullptr)
		return;
	while (XPending(xr.xDisplay) > 0) {
		XEvent ev;
		XNextEvent(xr.xDisplay, &ev);
		switch (ev.type) {
		case KeyPress: {
			KeySym sym = XLookupKeysym(&ev.xkey, 0);
			if (sym == XK_b || sym == XK_B) {
				g_geoNav.cycleBookmark();
				size_t n = 0;
				const geo::Bookmark *bm = geo::bookmarks(&n);
				if (n > 0)
					LOG_INFO("Bookmark: %s", bm[g_geoNav.bookmarkIndex].name);
			}
			break;
		}
		case ConfigureNotify:
			if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
				xr.xWinW = (unsigned int)ev.xconfigure.width;
				xr.xWinH = (unsigned int)ev.xconfigure.height;
				g_windowW = xr.xWinW;
				g_windowH = xr.xWinH;
			}
			break;
		case ClientMessage:
			if (g_wmDeleteAtom != None && (Atom)ev.xclient.data.l[0] == g_wmDeleteAtom) {
				LOG_INFO("Window closed — exiting");
				g_running = 0;
			}
			break;
		default:
			break;
		}
	}
}

// Orthoscopic camera-rig vFOV: the FULL display's physical subtense
// (displayH / nominalZ) — matches windows/main.cpp CamVFovRad.
static constexpr float kCameraVFovRad = 0.6498f; // ~37.2° fallback
static inline float
CamVFovRad(float physHeightM, float nominalZ)
{
	return (physHeightM > 1.0e-6f && nominalZ > 1.0e-6f)
	           ? 2.0f * atanf(physHeightM / (2.0f * nominalZ))
	           : kCameraVFovRad;
}

int
main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	// EV_PROBE=<key>: validate a Map Tiles API key against Google and exit
	// (0 = valid). No window / runtime / GPU needed — support tool, shared
	// with the macOS + Windows legs.
	if (const char *pk = getenv("EV_PROBE")) {
		std::string err;
		bool ok = g_tileEngine.probeKey(pk, err);
		fprintf(stderr, "EV_PROBE: %s%s%s\n", ok ? "VALID" : "INVALID",
		        ok ? "" : " - ", ok ? "" : err.c_str());
		return ok ? 0 : 1;
	}

	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);

	LOG_INFO("=== DisplayXR EarthView (Vulkan, Linux hosted-NULL) ===");

	AppXrSession xr = {};
	if (!InitializeOpenXR(xr)) {
		LOG_ERROR("OpenXR init failed");
		return 1;
	}
	if (!GetVulkanGraphicsRequirements(xr)) {
		CleanupOpenXR(xr);
		return 1;
	}

	VkInstance vkInstance = VK_NULL_HANDLE;
	if (!CreateVulkanInstance(xr, vkInstance)) {
		CleanupOpenXR(xr);
		return 1;
	}
	VkPhysicalDevice physDevice = VK_NULL_HANDLE;
	if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
		vkDestroyInstance(vkInstance, nullptr);
		CleanupOpenXR(xr);
		return 1;
	}
	std::vector<const char *> devExts;
	std::vector<std::string> extStorage;
	if (!GetVulkanDeviceExtensions(xr, devExts, extStorage)) {
		vkDestroyInstance(vkInstance, nullptr);
		CleanupOpenXR(xr);
		return 1;
	}
	uint32_t queueFamilyIndex = 0;
	if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
		vkDestroyInstance(vkInstance, nullptr);
		CleanupOpenXR(xr);
		return 1;
	}
	VkDevice vkDevice = VK_NULL_HANDLE;
	VkQueue graphicsQueue = VK_NULL_HANDLE;
	if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice,
	                        graphicsQueue)) {
		vkDestroyInstance(vkInstance, nullptr);
		CleanupOpenXR(xr);
		return 1;
	}
	// Handle app: own an X11 window on the 3D panel (display_info queried in
	// InitializeOpenXR gives the panel rect). Falls back to hosted-NULL when
	// no X server is available (also the CI-safe path — CI never runs this).
	CreateAppWindow(xr);

	if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
		vkDestroyDevice(vkDevice, nullptr);
		vkDestroyInstance(vkInstance, nullptr);
		CleanupOpenXR(xr);
		return 1;
	}

	// Tile basis: the app window when we own one (window × scaleXY,
	// runtime#729); else the full panel (hosted-NULL renders display-sized).
	if (xr.xWinW > 0 && xr.xWinH > 0) {
		g_windowW = xr.xWinW;
		g_windowH = xr.xWinH;
	} else {
		if (xr.displayPixelWidth > 0)
			g_windowW = xr.displayPixelWidth;
		if (xr.displayPixelHeight > 0)
			g_windowH = xr.displayPixelHeight;
	}
	if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
		CleanupOpenXR(xr);
		vkDestroyDevice(vkDevice, nullptr);
		vkDestroyInstance(vkInstance, nullptr);
		return 1;
	}

	std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
	{
		uint32_t count = xr.swapchain.imageCount;
		swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
		xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
		                           (XrSwapchainImageBaseHeader *)
		                               swapchainImages.data());
	}

	// Tile renderer + cesium engine. Keyless is a supported state: the app
	// stays up (the macOS/Windows legs show a how-to-get-a-key card; this
	// reduced harness just logs it).
	{
		uint32_t rw = xr.swapchain.width;
		uint32_t rh = xr.swapchain.height;
		if (!g_tileRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
		                         queueFamilyIndex, rw, rh)) {
			LOG_WARN("tile renderer init failed");
		} else {
			g_tilesActive = g_tileEngine.init(&g_tileRenderer);
			if (!g_tilesActive)
				LOG_WARN("No Google Map Tiles API key — set GOOGLE_MAPS_API_KEY "
				         "or earthview.ini to stream tiles.");
		}
	}

	g_geoNav.frameBookmark(0); // Paris

	// EV_MAX_FRAMES: bound the loop for headless smoke runs (0 = run until the
	// session exits / a signal). CI never runs the binary (no display), so this
	// is only for a future on-screen pass.
	long maxFrames = getenv("EV_MAX_FRAMES") ? atol(getenv("EV_MAX_FRAMES")) : 0;
	long frame = 0;

	// Convergence auto-focus state (mirrors windows/main.cpp): forward ray →
	// ground distance in geo metres, scaled to XR metres, exp-smoothed.
	float convDiopters = 1.0f; // 1/m; default = 1/kTargetXrDist
	constexpr double kConvSmoothTau = 0.15;
	auto lastTime = std::chrono::high_resolution_clock::now();

	while (g_running && !xr.exitRequested) {
		PollEvents(xr);
		PumpXEvents(xr); // B = cycle bookmark; close button = exit

		// Assert the app's default 3D rendering mode once the session runs.
		if (xr.sessionRunning && xr.pfnRequestMode && xr.session != XR_NULL_HANDLE) {
			uint32_t mode = xr.currentRenderingMode < xr.renderingModeCount
			                    ? xr.currentRenderingMode
			                    : 0;
			xr.pfnRequestMode(xr.session, mode);
		}

		if (!xr.sessionRunning) {
			continue;
		}

		auto now = std::chrono::high_resolution_clock::now();
		double deltaTime = std::chrono::duration<double>(now - lastTime).count();
		lastTime = now;

		XrFrameState frameState = {XR_TYPE_FRAME_STATE};
		if (XR_FAILED(xrWaitFrame(xr.session, nullptr, &frameState)))
			break;
		xrBeginFrame(xr.session, nullptr);

		std::vector<XrCompositionLayerProjectionView> projectionViews;
		bool rendered = false;

		if (frameState.shouldRender) {
			XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
			locateInfo.viewConfigurationType = xr.viewConfigType;
			locateInfo.displayTime = frameState.predictedDisplayTime;
			locateInfo.space = xr.localSpace;

			// XR_DXR_view_rig CAMERA rig (camera-centric FLY, the default
			// view): a plain perspective camera at the XR origin looking -Z;
			// the runtime owns the off-axis eyes + window resolve and returns
			// render-ready XrView{pose, fov}. Mirrors windows/main.cpp's fly
			// path (orbit/focus are per-platform UI, not ported here).
			const bool useRig =
			    xr.hasViewRigExt && xr.displayWidthM > 0 && xr.displayHeightM > 0;
			XrCameraRigDXR cameraRig = {XR_TYPE_CAMERA_RIG_DXR};
			if (useRig) {
				cameraRig.pose = {{0, 0, 0, 1}, {0, 0, 0}};
				cameraRig.ipdFactor = 1.0f;
				cameraRig.parallaxFactor = 1.0f;
				cameraRig.convergenceDiopters = convDiopters;
				cameraRig.verticalFov =
				    CamVFovRad(xr.displayHeightM, xr.nominalViewerZ);
				locateInfo.next = &cameraRig;
			}

			XrViewState viewState = {XR_TYPE_VIEW_STATE};
			// Over-allocate to the runtime's max view count (sim_display Quad
			// reports 4); hardcoding 2 fails with XR_ERROR_SIZE_INSUFFICIENT.
			uint32_t viewCap = xr.maxViewCount > 8 ? 8 : (xr.maxViewCount ? xr.maxViewCount : 2);
			std::vector<XrView> xrViews(viewCap, {XR_TYPE_VIEW});
			uint32_t viewCount = 0;
			XrResult lr = xrLocateViews(xr.session, &locateInfo, &viewState,
			                            viewCap, &viewCount, xrViews.data());

			if (XR_SUCCEEDED(lr) && viewCount > 0) {
				// Active rendering mode → eye count + per-view tile extent
				// (window × viewScale, runtime#729).
				uint32_t mode = xr.currentRenderingMode < xr.renderingModeCount
				                    ? xr.currentRenderingMode
				                    : 0;
				bool monoMode = (xr.renderingModeCount > 0 &&
				                 !xr.renderingModeDisplay3D[mode]);
				uint32_t activeViewCount = (xr.renderingModeCount > 0)
				                               ? xr.renderingModeViewCounts[mode]
				                               : 2u;
				if (activeViewCount == 0)
					activeViewCount = 1;
				if (activeViewCount > viewCount)
					activeViewCount = viewCount;
				const uint32_t eyeCount = monoMode ? 1 : activeViewCount;
				float scaleX = 1.0f, scaleY = 1.0f;
				uint32_t cols = monoMode ? 1u : 2u;
				if (xr.renderingModeCount > 0) {
					scaleX = xr.renderingModeScaleX[mode];
					scaleY = xr.renderingModeScaleY[mode];
					cols = xr.renderingModeTileColumns[mode];
					if (cols == 0)
						cols = 1;
				}
				uint32_t renderW = (uint32_t)((double)g_windowW * scaleX);
				uint32_t renderH = (uint32_t)((double)g_windowH * scaleY);
				if (renderW == 0)
					renderW = 1;
				if (renderH == 0)
					renderH = 1;

				// Per-eye view/projection from the render-ready views. Camera
				// rig: fixed tight near/far around the ~1 XR-m scene scale
				// (depth precision — see windows/main.cpp). GL → [0,1] depth
				// remap (the tile mesh uses the depth buffer).
				struct EyeView {
					std::array<float, 16> viewMat{}, projMat{};
					XrView src;
				};
				std::vector<EyeView> eyes((size_t)eyeCount);
				for (uint32_t e = 0; e < eyeCount; e++) {
					XrView sv = xrViews[e < viewCount ? e : 0];
					if (monoMode && viewCount > 1) {
						// Collapse the located views to their centroid.
						XrVector3f c = {0, 0, 0};
						XrFovf f = {0, 0, 0, 0};
						for (uint32_t v = 0; v < activeViewCount; v++) {
							c.x += xrViews[v].pose.position.x;
							c.y += xrViews[v].pose.position.y;
							c.z += xrViews[v].pose.position.z;
							f.angleLeft += xrViews[v].fov.angleLeft;
							f.angleRight += xrViews[v].fov.angleRight;
							f.angleUp += xrViews[v].fov.angleUp;
							f.angleDown += xrViews[v].fov.angleDown;
						}
						float inv = 1.0f / (float)activeViewCount;
						sv.pose.position = {c.x * inv, c.y * inv, c.z * inv};
						sv.fov = {f.angleLeft * inv, f.angleRight * inv,
						          f.angleUp * inv, f.angleDown * inv};
					}
					const float near_z = 0.05f, far_z = 200.0f;
					mat4_view_from_xr_pose(eyes[e].viewMat.data(), sv.pose);
					mat4_from_xr_fov(eyes[e].projMat.data(), sv.fov, near_z, far_z);
					if (useRig)
						convert_projection_gl_to_zero_to_one(eyes[e].projMat.data());
					eyes[e].src = sv;
				}

				// Camera-centric FLY world mapping: anchor the geo camera at
				// the XR origin, target a fixed kTargetXrDist in front
				// (s = kTargetXrDist/targetDist). Selection camera = the
				// head camera in ECEF from the inverse mapping; frustum = the
				// cam-rig vFOV widened to the tile aspect, +15% margin.
				if (g_tilesActive) {
					const double kTargetXrDist = 1.0;
					glm::dvec3 anchorXr(0.0);
					double s = kTargetXrDist / std::max(g_geoNav.targetDist, 1.0);
					glm::dmat4 xrFromEcef =
					    geo::xrFromEcefCamera(g_geoNav.cam, anchorXr, s);

					double camVFov =
					    (double)CamVFovRad(xr.displayHeightM, xr.nominalViewerZ);
					double aspect =
					    (renderH > 0) ? (double)renderW / (double)renderH : 1.0;
					double vfov = camVFov * 1.15;
					double hfov =
					    2.0 * std::atan(std::tan(0.5 * camVFov) * aspect) * 1.15;

					// Convergence auto-focus: forward ray → ground distance,
					// scaled to XR metres, clamped, exp-smoothed.
					double groundM =
					    geo::rayGroundDistanceM(g_geoNav.cam.pos, g_geoNav.cam.dir);
					if (groundM > 0.0) {
						double xrDist = groundM * s;
						if (xrDist < 0.2)
							xrDist = 0.2;
						if (xrDist > 50.0)
							xrDist = 50.0;
						float tgt = (float)(1.0 / xrDist);
						double a = 1.0 - std::exp(-deltaTime / kConvSmoothTau);
						convDiopters += (tgt - convDiopters) * (float)a;
					}

					geo::GeoCamera selCam;
					{
						glm::dmat4 invWorld = glm::inverse(xrFromEcef);
						glm::dmat3 invRot = glm::dmat3(invWorld);
						selCam.pos = glm::dvec3(invWorld * glm::dvec4(anchorXr, 1.0));
						selCam.dir = glm::normalize(invRot * glm::dvec3(0.0, 0.0, -1.0));
						selCam.up = glm::normalize(invRot * glm::dvec3(0.0, 1.0, 0.0));
					}
					const auto &tiles = g_tileEngine.update(
					    selCam, (double)renderW, (double)renderH, hfov, vfov);
					g_drawList = g_tileRenderer.buildDrawList(tiles, xrFromEcef);
				}

				uint32_t imageIndex = 0;
				XrSwapchainImageAcquireInfo ai = {
				    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				if (XR_SUCCEEDED(xrAcquireSwapchainImage(xr.swapchain.swapchain,
				                                          &ai, &imageIndex))) {
					XrSwapchainImageWaitInfo wi = {
					    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
					wi.timeout = XR_INFINITE_DURATION;
					xrWaitSwapchainImage(xr.swapchain.swapchain, &wi);

					VkImage targetImage = swapchainImages[imageIndex].image;
					VkFormat swapFormat = (VkFormat)xr.swapchain.format;

					projectionViews.assign(
					    (size_t)eyeCount,
					    {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
					for (uint32_t e = 0; e < eyeCount; e++) {
						uint32_t tileX = e % cols;
						uint32_t tileY = e / cols;
						uint32_t vpX = tileX * renderW;
						uint32_t vpY = tileY * renderH;
						if (g_tilesActive) {
							g_tileRenderer.renderEye(
							    targetImage, swapFormat, xr.swapchain.width,
							    xr.swapchain.height, vpX, vpY, renderW, renderH,
							    eyes[e].viewMat.data(), eyes[e].projMat.data(),
							    g_drawList);
						}

						projectionViews[e].subImage.swapchain =
						    xr.swapchain.swapchain;
						projectionViews[e].subImage.imageRect.offset = {
						    (int32_t)vpX, (int32_t)vpY};
						projectionViews[e].subImage.imageRect.extent = {
						    (int32_t)renderW, (int32_t)renderH};
						projectionViews[e].subImage.imageArrayIndex = 0;
						projectionViews[e].pose = eyes[e].src.pose;
						projectionViews[e].fov = eyes[e].src.fov;
					}

					XrSwapchainImageReleaseInfo ri = {
					    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
					xrReleaseSwapchainImage(xr.swapchain.swapchain, &ri);
					rendered = true;
				}
			}
		}

		XrCompositionLayerProjection layer = {
		    XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		layer.space = xr.localSpace;
		layer.viewCount = (uint32_t)projectionViews.size();
		layer.views = projectionViews.data();
		const XrCompositionLayerBaseHeader *layers[] = {
		    (const XrCompositionLayerBaseHeader *)&layer};

		XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
		ei.displayTime = frameState.predictedDisplayTime;
		ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		ei.layerCount = rendered ? 1 : 0;
		ei.layers = rendered ? layers : nullptr;
		xrEndFrame(xr.session, &ei);

		if (maxFrames > 0 && ++frame >= maxFrames) {
			LOG_INFO("EV_MAX_FRAMES=%ld reached — exiting.", maxFrames);
			break;
		}
	}

	// Teardown order (tile_renderer.h): engine FIRST (Tileset dtor free()s
	// every live tile through the renderer), THEN the renderer, THEN Vulkan.
	g_tileEngine.shutdown();
	g_tileRenderer.cleanup();
	CleanupOpenXR(xr);
	if (vkDevice)
		vkDestroyDevice(vkDevice, nullptr);
	if (vkInstance)
		vkDestroyInstance(vkInstance, nullptr);
	LOG_INFO("EarthView Linux exited cleanly.");
	return 0;
}
