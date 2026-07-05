// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// linux/main.cpp — the Linux entry point for the EarthView demo (issue #19,
// M8 Linux epic runtime#699). BUILD-GREEN scope: this compiles and links the
// full cross-platform scene layer (tiles_common cesium streaming + Vulkan tile
// renderer) against the OpenXR loader on ubuntu-latest CI. On-screen
// validation is a SEPARATE pass, gated on the runtime's Linux Phase 1b + a GPU
// + an X server, so nothing here is eyeball-verified yet.
//
// Windowing model: HOSTED-NULL. Unlike macos/main.mm (Cocoa window +
// XR_EXT_cocoa_window_binding) and windows/main.cpp (Win32 HWND +
// XR_EXT_win32_window_binding), this harness passes NO window binding — the
// runtime self-creates a window at native resolution. This is deliberate: the
// faithful app-provided-window arm on Linux is XR_EXT_xlib_window_binding
// (runtime Phase 3a), which is Phase-3 hardware-gated. See
// docs/guides/linux-demo-port.md (the runtime repo).
//   TODO(Phase 3): when on-screen validation lands, add an SDL/xlib window and
//   chain XrXlibWindowBindingCreateInfoEXT onto the session, mirroring the
//   cocoa/win32 legs. Until then hosted-NULL is the correct interim.
//
// This is a REDUCED harness: no HUD, no input, no MCP tools, no atlas capture,
// no view-rig / eye-tracking, no double-click picking. Those live in the
// per-platform mains and are UI concerns that require on-screen work anyway.
// The point of this file is that the Vulkan + OpenXR + cesium tile pipeline
// compiles and links on Linux.

#define XR_USE_GRAPHICS_API_VULKAN

#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// DisplayXR extension headers (vendored openxr_includes/, refreshed from
// displayxr-extensions). Only the ones this reduced harness enables.
#include <openxr/XR_EXT_display_info.h>

#include "geo_math.h"
#include "tile_engine.h"
#include "tile_renderer.h"

#include <glm/glm.hpp>

#include <array>
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
	uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
	float displayWidthM = 0, displayHeightM = 0;
	float nominalViewerZ = 0.5f;

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
		if (strcmp(e.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0)
			xr.hasDisplayInfoExt = true;
	}
	if (!hasVulkan) {
		LOG_ERROR("XR_KHR_vulkan_enable not available");
		return false;
	}

	std::vector<const char *> enabled;
	enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (xr.hasDisplayInfoExt)
		enabled.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);

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
		XrDisplayInfoEXT di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
		sp.next = &di;
		if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
			xr.displayWidthM = di.displaySizeMeters.width;
			xr.displayHeightM = di.displaySizeMeters.height;
			xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
			xr.displayPixelWidth = di.displayPixelWidth;
			xr.displayPixelHeight = di.displayPixelHeight;
		}
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
	// HOSTED-NULL: XrGraphicsBindingVulkanKHR with NO window binding chained.
	// The runtime self-creates its own window at native resolution. See the
	// TODO(Phase 3) at the top of this file for the xlib window-binding arm.
	XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	vkBinding.instance = vkInstance;
	vkBinding.physicalDevice = pd;
	vkBinding.device = dev;
	vkBinding.queueFamilyIndex = qfi;
	vkBinding.queueIndex = 0;

	XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
	si.next = &vkBinding;
	si.systemId = xr.systemId;
	XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));
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

	uint32_t w = (views.empty() ? 1024 : views[0].recommendedImageRectWidth) * 2;
	uint32_t h = views.empty() ? 1024 : views[0].recommendedImageRectHeight;
	if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
		w = xr.displayPixelWidth;
		h = xr.displayPixelHeight;
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
}

// ── EarthView scene state ────────────────────────────────────────────────
static TileRenderer g_tileRenderer;
static TileEngine g_tileEngine;
static geo::GeoNav g_geoNav;
static bool g_tilesActive = false;
static std::vector<TileRenderer::DrawItem> g_drawList;

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
	if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
		vkDestroyDevice(vkDevice, nullptr);
		vkDestroyInstance(vkInstance, nullptr);
		CleanupOpenXR(xr);
		return 1;
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

	while (g_running && !xr.exitRequested) {
		PollEvents(xr);
		if (!xr.sessionRunning) {
			continue;
		}

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

			XrViewState viewState = {XR_TYPE_VIEW_STATE};
			uint32_t viewCap = xr.maxViewCount;
			std::vector<XrView> xrViews(viewCap, {XR_TYPE_VIEW});
			uint32_t viewCount = 0;
			XrResult lr = xrLocateViews(xr.session, &locateInfo, &viewState,
			                            viewCap, &viewCount, xrViews.data());

			if (XR_SUCCEEDED(lr) && viewCount > 0) {
				// Camera-centric world mapping (PRD §6.1): map the full-scale
				// ECEF world so the geo camera sits at the viewer position.
				double zdp = (xr.nominalViewerZ > 1e-3f) ? xr.nominalViewerZ
				                                          : 0.5;
				double s = geo::stereoScaleForDistance(g_geoNav.targetDist, zdp);
				glm::dvec3 viewerPosXr(0.0, 0.1, zdp);
				glm::dmat4 xrFromEcef =
				    geo::xrFromEcefCamera(g_geoNav.cam, viewerPosXr, s);

				if (g_tilesActive) {
					const auto &tiles = g_tileEngine.update(
					    g_geoNav.cam, (double)xr.swapchain.width,
					    (double)xr.swapchain.height, 1.2, 0.8);
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

					uint32_t renderW =
					    xr.swapchain.width / (viewCount ? viewCount : 1);
					uint32_t renderH = xr.swapchain.height;
					VkImage targetImage = swapchainImages[imageIndex].image;
					VkFormat swapFormat = (VkFormat)xr.swapchain.format;

					projectionViews.assign(
					    (size_t)viewCount,
					    {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
					for (uint32_t v = 0; v < viewCount; v++) {
						std::array<float, 16> viewMat{}, projMat{};
						mat4_view_from_xr_pose(viewMat.data(), xrViews[v].pose);
						mat4_from_xr_fov(projMat.data(), xrViews[v].fov, 0.01f,
						                 100.0f);

						uint32_t vpX = v * renderW;
						if (g_tilesActive) {
							g_tileRenderer.renderEye(
							    targetImage, swapFormat, xr.swapchain.width,
							    xr.swapchain.height, vpX, 0, renderW, renderH,
							    viewMat.data(), projMat.data(), g_drawList);
						}

						projectionViews[v].subImage.swapchain =
						    xr.swapchain.swapchain;
						projectionViews[v].subImage.imageRect.offset = {
						    (int32_t)vpX, 0};
						projectionViews[v].subImage.imageRect.extent = {
						    (int32_t)renderW, (int32_t)renderH};
						projectionViews[v].subImage.imageArrayIndex = 0;
						projectionViews[v].pose = xrViews[v].pose;
						projectionViews[v].fov = xrViews[v].fov;
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
