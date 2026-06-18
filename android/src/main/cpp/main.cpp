// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// earthview_vk_android entry point — M3. Brings up the DisplayXR runtime
// out-of-process (ADR-025) like the modelviewer/cube Android harness (loader →
// instance → Vulkan2 device → atlas swapchain → session), then drives the
// SHARED tiles_common Vulkan renderer + cesium-native streaming (Google
// Photorealistic 3D Tiles), exactly as the windows/ + macos/ legs do.
//
// Camera-centric FLY mode via XR_EXT_view_rig's camera rig (the runtime owns the
// off-axis eyes). Touch: 1-finger drag = look, 2-finger pinch = dolly. The
// macOS-parity focus/orbit (double-tap to inspect a landmark) is a follow-up.
//
// API key: keyless → falls back to a globe-blue clear (no streaming). For dev,
// set the key via `adb shell setprop debug.dxr.ev.key <KEY>` (picked up into
// GOOGLE_MAPS_API_KEY before TileEngine::init). See docs/api-key.md.

#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_display_info.h>
#include <openxr/XR_EXT_view_rig.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "geo_math.h"
#include "tile_engine.h"
#include "tile_renderer.h"
#include "dxr_view_math.h"  // cam->display rig converter (focus/orbit display rig)

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <jni.h>
#include <mutex>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

#define LOG_TAG "earthview_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr uint32_t kViewCount = 2;
constexpr float kCameraVFovRad = 0.6498f;  // ~37.2° full vertical FOV
// Physical (orthoscopic) cam-rig vFOV: the angle the display subtends from the
// nominal viewing position. 2*tan(vfov/2) = physHeightM / nominalZ. On Android the
// canvas is always full-screen, so canvasSizeMeters == the display. Falls back to
// kCameraVFovRad until the rig raw channel reports a size + eye Z. See
// docs/rendering-notes.md §5.
static inline float CamVFovRad(float physHeightM, float nominalZ) {
	return (physHeightM > 1.0e-6f && nominalZ > 1.0e-6f)
	           ? 2.0f * atanf(physHeightM / (2.0f * nominalZ))
	           : kCameraVFovRad;
}
constexpr double kTargetXrDist = 1.0;      // XR metres to the geo target (fly)
constexpr double kConvSmoothTau = 0.15;

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS:                   return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:       return "XR_ERROR_INSTANCE_LOST";
	default:                           return nullptr;
	}
}
void
log_xr_result(const char *what, XrResult r)
{
	const char *n = xr_result_str(r);
	if (n) LOGI("%s -> %s", what, n);
	else LOGI("%s -> XrResult(%d)", what, (int)r);
}

// ── float[16] column-major matrix helpers (ported from windows/main.cpp) ──
void mat4_identity(float *m) { memset(m, 0, 16 * sizeof(float)); m[0]=m[5]=m[10]=m[15]=1.0f; }
void mat4_multiply(float *out, const float *a, const float *b) {
	float t[16];
	for (int c=0;c<4;c++) for (int r=0;r<4;r++){ float s=0; for(int k=0;k<4;k++) s+=a[k*4+r]*b[c*4+k]; t[c*4+r]=s; }
	memcpy(out, t, sizeof(t));
}
void mat4_translation(float *m, float x, float y, float z){ mat4_identity(m); m[12]=x; m[13]=y; m[14]=z; }
void mat4_from_xr_fov(float *m, XrFovf fov, float nearZ, float farZ){
	float tanL=tanf(fov.angleLeft),tanR=tanf(fov.angleRight),tanU=tanf(fov.angleUp),tanD=tanf(fov.angleDown);
	float w=tanR-tanL,h=tanU-tanD; memset(m,0,16*sizeof(float));
	m[0]=2.0f/w; m[5]=2.0f/h; m[8]=(tanR+tanL)/w; m[9]=(tanU+tanD)/h;
	m[10]=-(farZ+nearZ)/(farZ-nearZ); m[11]=-1.0f; m[14]=-(2.0f*farZ*nearZ)/(farZ-nearZ);
}
void mat4_view_from_xr_pose(float *vm, XrPosef p){
	float qx=p.orientation.x,qy=p.orientation.y,qz=p.orientation.z,qw=p.orientation.w;
	float rot[16]; mat4_identity(rot);
	rot[0]=1-2*(qy*qy+qz*qz); rot[1]=2*(qx*qy+qz*qw); rot[2]=2*(qx*qz-qy*qw);
	rot[4]=2*(qx*qy-qz*qw); rot[5]=1-2*(qx*qx+qz*qz); rot[6]=2*(qy*qz+qx*qw);
	rot[8]=2*(qx*qz+qy*qw); rot[9]=2*(qy*qz-qx*qw); rot[10]=1-2*(qx*qx+qy*qy);
	float invRot[16]; mat4_identity(invRot);
	for(int i=0;i<3;i++) for(int j=0;j<3;j++) invRot[j*4+i]=rot[i*4+j];
	float invT[16]; mat4_translation(invT, -p.position.x, -p.position.y, -p.position.z);
	mat4_multiply(vm, invRot, invT);
}
// GL clip-z [-1,1] → Vulkan depth [0,1] (third row = (third+fourth)/2).
void convert_projection_gl_to_zero_to_one(float *m){
	m[2]=(m[2]+m[3])*0.5f; m[6]=(m[6]+m[7])*0.5f; m[10]=(m[10]+m[11])*0.5f; m[14]=(m[14]+m[15])*0.5f;
}

// ── State ────────────────────────────────────────────────────────────────
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system_id = XR_NULL_SYSTEM_ID;
XrVersion g_required_vk_version = XR_MAKE_VERSION(1, 1, 0);
VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys_device = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
VkQueue g_vk_queue = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = UINT32_MAX;
VkCommandPool g_cmd_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
bool g_session_running = false;
bool g_exit_requested = false;
XrSpace g_app_space = XR_NULL_HANDLE;
VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;
XrSwapchain g_swapchain = XR_NULL_HANDLE;
XrSwapchainImageVulkanKHR g_images[8] = {};
uint32_t g_image_count = 0;
uint32_t g_atlas_w = 0, g_atlas_h = 0, g_tile_w = 0, g_tile_h = 0;
bool g_has_view_rig = false;
uint64_t g_frame_count = 0;
const char *g_internal_data_path = nullptr;  // NativeActivity internalDataPath

// Tiles.
TileRenderer g_tile_renderer;
TileEngine g_tile_engine;
bool g_tiles_active = false;
geo::GeoNav g_geoNav;
glm::dmat4 g_xrFromEcef(1.0);
float g_convDiopters = 1.0f;
float g_viewDistXR = 0.0f;  // eye->focus distance = 1/convergence
float g_canvasWM = 0.0f;    // runtime canvas width (m), for the rig converter aspect
double g_stereoFull = 0.0;  // [FOCUS] ipd/par glide: 1 in focus (full depth), 0 in fly
auto g_last_t = std::chrono::steady_clock::now();

// Double-tap "focus" model (macOS parity, commit c95b624): double-tap a landmark
// → the camera re-aims to centre it and orbits it (the forward-ray convergence
// auto-focus then lands the ZDP on it). Camera-rig presentation is kept (the
// runtime owns the off-axis eyes); the macOS display-rig portal switch is a
// follow-up. Double-tap the sky → release back to free fly.
bool g_focusActive = false;
double g_focusT = 0.0;                  // 0->1 re-aim/reframe transition
glm::dvec3 g_focusPOIecef(0.0);
glm::dvec3 g_poiXitFromDir(0.0), g_poiXitToDir(0.0);
double g_poiXitFromTD = 0.0, g_poiXitToTD = 0.0;
// Deferred double-tap pick: resolved after the eyes render (depth-readback
// unproject), set by the JNI tap with the tapped screen NDC.
std::atomic<bool> g_pendingPick{false};
float g_pickNdcX = 0.0f, g_pickNdcY = 0.0f;
uint32_t g_canvas_px_w = 0, g_canvas_px_h = 0;  // from XrViewDisplayRawEXT
// Physical display height (m) + nominal viewer Z (m), latched from the rig raw
// channel (canvasSizeMeters + rawEyes display-space Z) — stable nominal values
// that drive the orthoscopic fly FOV (CamVFovRad). See docs/rendering-notes.md §5.
float g_displayHeightM = 0.0f, g_nominalViewerZ = 0.0f;

// API key (from the Kotlin dialog / SharedPreferences) + attribution feed.
std::mutex g_key_mtx;
std::string g_pending_key;            // set via JNI; seeds GOOGLE_MAPS_API_KEY
std::atomic<bool> g_apply_key{false}; // re-init the tileset on the render thread
std::mutex g_attr_mtx;
std::string g_attr_text;              // joined credit lines for the Kotlin overlay

// JNI-shared.
std::atomic<int> g_display_rotation{0};
std::atomic<bool> g_runtime_unavailable{false};
std::atomic<bool> g_xr_ready{false};
std::atomic<bool> g_reset_view{false};

// Touch → nav deltas (accumulated by JNI, drained by the render thread).
std::mutex g_touch_mtx;
double g_look_dx = 0, g_look_dy = 0, g_dolly = 0, g_pan_dx = 0, g_pan_dy = 0;
float g_last_x0 = 0, g_last_y0 = 0;
float g_last_pinch = -1.0f;
float g_last_cx = 0, g_last_cy = 0;     // two-finger centroid (for pan)
int g_last_count = 0;
// Two-finger tap / double-tap detection (reset view, like the desktop Space).
double g_two_down_t = 0.0, g_last_two_tap_t = 0.0;
float g_two_down_cx = 0, g_two_down_cy = 0;
bool g_two_moved = false;
double g_last_input_t = 0.0;  // for idle auto-orbit
std::atomic<double> g_now_sec{0.0};

// ── Bring-up (unchanged from the placeholder leg) ────────────────────────
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR fn = nullptr;
	if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	        (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn) return false;
	XrLoaderInitInfoAndroidKHR info = {};
	info.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	info.applicationVM = app->activity->vm;
	info.applicationContext = app->activity->clazz;
	XrResult res = fn((const XrLoaderInitInfoBaseHeaderKHR *)&info);
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	g_runtime_unavailable.store(false, std::memory_order_relaxed);
	g_has_view_rig = false;
	uint32_t n = 0;
	if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr) == XR_SUCCESS && n > 0) {
		std::vector<XrExtensionProperties> props(n);
		for (auto &p : props) { p.type = XR_TYPE_EXTENSION_PROPERTIES; p.next = nullptr; }
		if (xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()) == XR_SUCCESS)
			for (uint32_t i = 0; i < n; ++i)
				if (std::strcmp(props[i].extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0)
					g_has_view_rig = true;
	}
	LOGI("XR_EXT_view_rig advertised: %s", g_has_view_rig ? "yes" : "no");

	const char *exts[4] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	                       XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	                       XR_EXT_DISPLAY_INFO_EXTENSION_NAME};
	uint32_t ext_count = 3;
	if (g_has_view_rig) exts[ext_count++] = XR_EXT_VIEW_RIG_EXTENSION_NAME;

	XrInstanceCreateInfoAndroidKHR ainfo = {};
	ainfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	ainfo.applicationVM = app->activity->vm;
	ainfo.applicationActivity = app->activity->clazz;
	XrInstanceCreateInfo ci = {};
	ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
	ci.next = &ainfo;
	std::strncpy(ci.applicationInfo.applicationName, "earthview_vk_android", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ci.applicationInfo.applicationVersion = 1;
	std::strncpy(ci.applicationInfo.engineName, "displayxr", XR_MAX_ENGINE_NAME_SIZE - 1);
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ci.enabledExtensionCount = ext_count;
	ci.enabledExtensionNames = exts;

	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int a = 0; a < 5; ++a) {
		res = xrCreateInstance(&ci, &g_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) break;
		LOGW("xrCreateInstance: runtime unavailable (%d/5)…", a + 1);
		usleep(400 * 1000);
	}
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE) g_runtime_unavailable.store(true, std::memory_order_relaxed);
		return false;
	}
	g_xr_ready.store(true, std::memory_order_relaxed);
	LOGI("EARTHVIEW_SENTINEL xrCreateInstance=XR_SUCCESS");
	return true;
}

bool
query_system_and_graphics_reqs()
{
	XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &si, &g_system_id);
	if (res != XR_SUCCESS) { si.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY; res = xrGetSystem(g_instance, &si, &g_system_id); }
	log_xr_result("xrGetSystem", res);
	if (res != XR_SUCCESS) return false;
	PFN_xrGetVulkanGraphicsRequirements2KHR fn = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn) return false;
	XrGraphicsRequirementsVulkanKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	res = fn(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) return false;
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR fn = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn) return false;
	VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.pApplicationName = "earthview_vk_android"; ai.pEngineName = "displayxr";
	ai.apiVersion = VK_MAKE_VERSION(XR_VERSION_MAJOR(g_required_vk_version), XR_VERSION_MINOR(g_required_vk_version), 0);
	VkInstanceCreateInfo vci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vci.pApplicationInfo = &ai;
	XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xci.systemId = g_system_id; xci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr; xci.vulkanCreateInfo = &vci;
	VkResult vr = VK_SUCCESS;
	XrResult res = fn(g_instance, &xci, &g_vk_instance, &vr);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	return res == XR_SUCCESS && vr == VK_SUCCESS;
}

bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR fn = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn) return false;
	XrVulkanGraphicsDeviceGetInfoKHR info = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	info.systemId = g_system_id; info.vulkanInstance = g_vk_instance;
	XrResult res = fn(g_instance, &info, &g_vk_phys_device);
	log_xr_result("xrGetVulkanGraphicsDevice2KHR", res);
	return res == XR_SUCCESS;
}

bool
create_vulkan_device()
{
	uint32_t qf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf, nullptr);
	if (!qf) return false;
	VkQueueFamilyProperties qfp[16] = {}; if (qf > 16) qf = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf, qfp);
	g_vk_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf; ++i) if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_vk_queue_family = i; break; }
	if (g_vk_queue_family == UINT32_MAX) return false;
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = g_vk_queue_family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
	PFN_xrCreateVulkanDeviceKHR fn = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn) return false;
	XrVulkanDeviceCreateInfoKHR xci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xci.systemId = g_system_id; xci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xci.vulkanPhysicalDevice = g_vk_phys_device; xci.vulkanCreateInfo = &dci;
	VkResult vr = VK_SUCCESS;
	XrResult res = fn(g_instance, &xci, &g_vk_device, &vr);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vr != VK_SUCCESS) return false;
	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = g_vk_queue_family;
	if (vkCreateCommandPool(g_vk_device, &pci, nullptr, &g_cmd_pool) != VK_SUCCESS) return false;
	VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbi.commandPool = g_cmd_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(g_vk_device, &cbi, &g_cmd) != VK_SUCCESS) return false;
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vkCreateFence(g_vk_device, &fci, nullptr, &g_fence) != VK_SUCCESS) return false;
	LOGI("Vulkan device ready: queue_family=%u", g_vk_queue_family);
	return true;
}

bool
create_session()
{
	XrGraphicsBindingVulkanKHR b = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	b.instance = g_vk_instance; b.physicalDevice = g_vk_phys_device; b.device = g_vk_device;
	b.queueFamilyIndex = g_vk_queue_family; b.queueIndex = 0;
	XrSessionCreateInfo ci = {XR_TYPE_SESSION_CREATE_INFO};
	ci.next = &b; ci.systemId = g_system_id;
	XrResult res = xrCreateSession(g_instance, &ci, &g_session);
	log_xr_result("xrCreateSession", res);
	return res == XR_SUCCESS;
}

bool
create_swapchains()
{
	uint32_t vc = 0;
	if (xrEnumerateViewConfigurationViews(g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr) != XR_SUCCESS || vc != kViewCount) {
		LOGE("Expected %u views, runtime reports %u", kViewCount, vc); return false;
	}
	XrViewConfigurationView vcfg[kViewCount] = {};
	for (auto &v : vcfg) v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	if (xrEnumerateViewConfigurationViews(g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, kViewCount, &vc, vcfg) != XR_SUCCESS) return false;

	uint32_t fc = 0;
	if (xrEnumerateSwapchainFormats(g_session, 0, &fc, nullptr) != XR_SUCCESS || !fc) return false;
	int64_t fmts[64] = {}; if (fc > 64) fc = 64;
	if (xrEnumerateSwapchainFormats(g_session, fc, &fc, fmts) != XR_SUCCESS) return false;
	const int64_t pref[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
	for (int64_t p : pref) for (uint32_t i = 0; i < fc && g_swapchain_format == VK_FORMAT_UNDEFINED; ++i) if (fmts[i] == p) g_swapchain_format = (VkFormat)p;
	if (g_swapchain_format == VK_FORMAT_UNDEFINED) g_swapchain_format = (VkFormat)fmts[0];

	g_tile_w = vcfg[0].recommendedImageRectWidth;
	g_tile_h = vcfg[0].recommendedImageRectHeight;
	g_atlas_w = g_tile_w * 2; g_atlas_h = g_tile_h;

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = g_swapchain_format; ci.sampleCount = 1;
	ci.width = g_atlas_w; ci.height = g_atlas_h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
	XrResult res = xrCreateSwapchain(g_session, &ci, &g_swapchain);
	if (res != XR_SUCCESS) { log_xr_result("xrCreateSwapchain", res); return false; }
	uint32_t ic = 0; xrEnumerateSwapchainImages(g_swapchain, 0, &ic, nullptr); if (ic > 8) ic = 8;
	for (uint32_t i = 0; i < ic; ++i) g_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	if (xrEnumerateSwapchainImages(g_swapchain, ic, &ic, (XrSwapchainImageBaseHeader *)g_images) != XR_SUCCESS) return false;
	g_image_count = ic;
	LOGI("Atlas swapchain: %ux%u (tile %ux%u), %u images, fmt 0x%x", g_atlas_w, g_atlas_h, g_tile_w, g_tile_h, ic, (uint32_t)g_swapchain_format);
	return true;
}

bool
create_reference_space()
{
	XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	XrResult res = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
	log_xr_result("xrCreateReferenceSpace(LOCAL)", res);
	return res == XR_SUCCESS;
}

// Pull a dev key from `debug.dxr.ev.key` into GOOGLE_MAPS_API_KEY so
// TileEngine::init() finds it (Android has no shell env for the app).
void
seed_api_key()
{
	// (1) a key from the in-app dialog / SharedPreferences (set via JNI), else
	// (2) the dev `debug.dxr.ev.key` system property.
	{
		std::lock_guard<std::mutex> lk(g_key_mtx);
		if (!g_pending_key.empty()) {
			setenv("GOOGLE_MAPS_API_KEY", g_pending_key.c_str(), 1);
			LOGI("API key from app (len %zu)", g_pending_key.size());
			return;
		}
	}
	char prop[PROP_VALUE_MAX] = {};
	if (__system_property_get("debug.dxr.ev.key", prop) > 0 && prop[0]) {
		setenv("GOOGLE_MAPS_API_KEY", prop, 1);
		LOGI("API key seeded from debug.dxr.ev.key (len %zu)", strlen(prop));
	}
}

// cesium's bundled curl/OpenSSL 3.x has no CA store on Android, and CURLOPT_CAPATH
// to /system/etc/security/cacerts fails (those are hashed in the old OpenSSL 1.0
// format). Concatenate the device's own PEM certs into one bundle and hand it to
// CesiumCurl as CURLOPT_CAINFO (via EARTHVIEW_CA_BUNDLE → tile_engine curlOptions).
std::string
build_ca_bundle()
{
	if (!g_internal_data_path) return "";
	const char *sys = "/system/etc/security/cacerts";
	DIR *d = opendir(sys);
	if (!d) { LOGW("no system CA dir %s", sys); return ""; }
	std::string out = std::string(g_internal_data_path) + "/cacert.pem";
	FILE *of = fopen(out.c_str(), "wb");
	if (!of) { closedir(d); return ""; }
	int n = 0;
	char buf[8192];
	for (struct dirent *e; (e = readdir(d)) != nullptr;) {
		if (e->d_name[0] == '.') continue;
		std::string p = std::string(sys) + "/" + e->d_name;
		FILE *in = fopen(p.c_str(), "rb");
		if (!in) continue;
		for (size_t r; (r = fread(buf, 1, sizeof(buf), in)) > 0;) fwrite(buf, 1, r, of);
		fputc('\n', of);
		fclose(in);
		++n;
	}
	fclose(of);
	closedir(d);
	LOGI("CA bundle: %d certs -> %s", n, out.c_str());
	return n > 0 ? out : "";
}

bool
tiles_init()
{
	// Route cesium's spdlog to logcat (tag "cesium") so HTTP/TLS/tile errors are
	// visible — tile_engine otherwise mutes spdlog to `critical` with no sink.
	spdlog::set_default_logger(std::make_shared<spdlog::logger>(
	    "cesium", std::make_shared<spdlog::sinks::android_sink_mt>("cesium")));

	std::string ca = build_ca_bundle();
	if (!ca.empty()) setenv("EARTHVIEW_CA_BUNDLE", ca.c_str(), 1);

	seed_api_key();
	if (!g_tile_renderer.init(g_vk_instance, g_vk_phys_device, g_vk_device, g_vk_queue,
	        g_vk_queue_family, g_tile_w, g_tile_h)) {
		LOGE("TileRenderer::init failed");
		return false;
	}
	g_tiles_active = g_tile_engine.init(&g_tile_renderer);
	g_apply_key.store(false, std::memory_order_relaxed);  // consumed by init
	// tile_engine mutes spdlog to `critical`; relax to `err` so HTTP/TLS failures
	// surface in logcat (tag "cesium") without the per-request parse-warning spam.
	spdlog::set_level(spdlog::level::err);
	spdlog::flush_on(spdlog::level::err);
	LOGI("TileEngine::init -> tiles %s (key %s)", g_tiles_active ? "ACTIVE" : "inactive",
	     g_tile_engine.hasKey() ? "present" : "MISSING");
	g_geoNav.frameBookmark(0);  // seed the first city
	return true;
}

// Re-create the tileset with a key entered at runtime (the in-app dialog). Runs
// on the render thread, where the Vulkan device + cesium main thread are valid.
void
apply_pending_key()
{
	if (!g_apply_key.exchange(false, std::memory_order_relaxed)) return;
	vkDeviceWaitIdle(g_vk_device);
	g_tile_engine.shutdown();
	seed_api_key();
	g_tiles_active = g_tile_engine.init(&g_tile_renderer);
	spdlog::set_level(spdlog::level::err);
	LOGI("API key applied -> tiles %s (key %s)", g_tiles_active ? "ACTIVE" : "inactive",
	     g_tile_engine.hasKey() ? "present" : "MISSING");
}

void
handle_session_state(XrSessionState s)
{
	if (s == XR_SESSION_STATE_READY) {
		XrSessionBeginInfo b = {XR_TYPE_SESSION_BEGIN_INFO};
		b.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (xrBeginSession(g_session, &b) == XR_SUCCESS) g_session_running = true;
		LOGI("xrBeginSession -> running");
	} else if (s == XR_SESSION_STATE_STOPPING) {
		xrEndSession(g_session); g_session_running = false;
	} else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
		g_exit_requested = true;
	}
}

void
poll_xr_events()
{
	for (;;) {
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res != XR_SUCCESS) break;
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *e = (const XrEventDataSessionStateChanged *)&ev;
			if (e->session == g_session) { LOGI("session state -> %d", (int)e->state); handle_session_state(e->state); }
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_exit_requested = true;
		}
	}
}

void
clear_atlas(VkImage image, float r, float g, float b)
{
	vkResetCommandBuffer(g_cmd, 0);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(g_cmd, &bi);
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	auto bar = [&](VkImageLayout f, VkImageLayout t, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
		VkImageMemoryBarrier m = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		m.oldLayout = f; m.newLayout = t; m.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		m.image = image; m.subresourceRange = range; m.srcAccessMask = sa; m.dstAccessMask = da;
		vkCmdPipelineBarrier(g_cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &m);
	};
	bar(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkClearColorValue c = {}; c.float32[0]=r; c.float32[1]=g; c.float32[2]=b; c.float32[3]=1.0f;
	vkCmdClearColorImage(g_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
	bar(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	vkEndCommandBuffer(g_cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
	vkResetFences(g_vk_device, 1, &g_fence);
	vkQueueSubmit(g_vk_queue, 1, &si, g_fence);
	vkWaitForFences(g_vk_device, 1, &g_fence, VK_TRUE, UINT64_MAX);
}

// Orbit the camera around a POI (focus mode) or auto-orbit turntable. Mirrors
// the macOS focus drag: world stays camera-centric, only cam.pos/dir revolve.
void
orbit_camera_around_poi(double dYaw, double dPitch)
{
	const glm::dvec3 poi = g_focusPOIecef;
	const glm::dvec3 up = glm::normalize(poi);  // geodetic normal ~ radial
	glm::dvec3 v = g_geoNav.cam.pos - poi;
	glm::dmat4 ry = glm::rotate(glm::dmat4(1.0), -dYaw, up);
	v = glm::dvec3(ry * glm::dvec4(v, 0.0));
	glm::dvec3 right = glm::normalize(glm::cross(up, glm::normalize(v)));
	glm::dmat4 rp = glm::rotate(glm::dmat4(1.0), -dPitch, right);
	glm::dvec3 v2 = glm::dvec3(rp * glm::dvec4(v, 0.0));
	if (std::abs(glm::dot(glm::normalize(v2), up)) < 0.98) v = v2;  // no pole flip
	g_geoNav.cam.pos = poi + v;
	g_geoNav.cam.dir = glm::normalize(poi - g_geoNav.cam.pos);
	g_geoNav.cam.up = up;
	// Keep the POI on the convergence plane: targetDist = radius / vDist.
	g_geoNav.targetDist = std::max(glm::length(v) / std::max((double)g_viewDistXR, 0.1), 20.0);
}

// Drain accumulated touch deltas into the geo camera (render thread).
void
apply_nav(float dt)
{
	double look_dx, look_dy, dolly, pan_dx, pan_dy;
	{
		std::lock_guard<std::mutex> lk(g_touch_mtx);
		look_dx = g_look_dx; look_dy = g_look_dy; dolly = g_dolly;
		pan_dx = g_pan_dx; pan_dy = g_pan_dy;
		g_look_dx = g_look_dy = g_dolly = g_pan_dx = g_pan_dy = 0;
	}

	// [FOCUS] smooth POI transition (~0.4 s): re-aim cam.dir (slerp) + reframe
	// targetDist (log-lerp) so the feature glides to centre and onto the ZDP.
	if (g_focusActive && g_focusT < 1.0) {
		g_focusT += (double)dt / 0.4;
		double t = g_focusT > 1.0 ? 1.0 : g_focusT;
		double e = t * t * (3.0 - 2.0 * t);  // smoothstep
		double cosA = glm::clamp(glm::dot(g_poiXitFromDir, g_poiXitToDir), -1.0, 1.0);
		double a = std::acos(cosA);
		if (a < 1.0e-4) {
			g_geoNav.cam.dir = g_poiXitToDir;
		} else {
			double s0 = std::sin((1.0 - e) * a) / std::sin(a);
			double s1 = std::sin(e * a) / std::sin(a);
			g_geoNav.cam.dir = glm::normalize(s0 * g_poiXitFromDir + s1 * g_poiXitToDir);
		}
		g_geoNav.cam.up = glm::normalize(g_geoNav.cam.pos);  // radial up
		g_geoNav.targetDist = g_poiXitFromTD * std::pow(g_poiXitToTD / g_poiXitFromTD, e);
		if (g_focusT >= 1.0) g_focusT = 1.0;
	}

	// [FOCUS] glide the stereo fullness toward the current mode (~0.4 s) — drives
	// the camera<->display rig ipd/parallax crossfade so the switch is seamless.
	{
		double tgt = g_focusActive ? 1.0 : 0.0;
		double rate = (double)dt / 0.4;
		if (g_stereoFull < tgt) g_stereoFull = std::min(tgt, g_stereoFull + rate);
		else if (g_stereoFull > tgt) g_stereoFull = std::max(tgt, g_stereoFull - rate);
	}

	if (g_reset_view.exchange(false)) {
		g_focusActive = false;
		g_geoNav.frameBookmark(g_geoNav.bookmarkIndex);
	}

	bool active = false;

	// Two-finger drag → pan (the desktop WASD translate), and it EXITS focus back
	// to free fly — the natural way out of orbit mode (mirrors the macOS WASDQE
	// auto-exit). Applied before look so the focus exit lands this frame.
	if (pan_dx != 0 || pan_dy != 0) {
		if (g_focusActive) {
			g_focusActive = false;
			g_focusT = 1.0;
			LOGI("[FOCUS] two-finger pan -> fly");
		}
		g_geoNav.pan(pan_dx, pan_dy);
		active = true;
	}

	if (look_dx != 0 || look_dy != 0) {
		if (g_focusActive) {
			g_focusT = 1.0;  // a drag cancels any in-flight re-aim
			orbit_camera_around_poi(look_dx, look_dy);
		} else {
			g_geoNav.look(look_dx, look_dy);
		}
		active = true;
	}
	// Zoom is a CESIUM-WORLD op (scales the tile world via targetDist), NOT a rig
	// op — so in focus it must keep the orbit POI fixed itself (§6). FLY = forward
	// dolly. FOCUS = change the orbit RADIUS about the POI + re-pin on the
	// convergence plane (same re-pin orbit_camera_around_poi uses); a plain forward
	// dolly moves toward the geo target, not the POI, and the physical-FOV full
	// perspective makes that drift visible.
	if (dolly != 0) {
		if (g_focusActive) {
			const glm::dvec3 poi = g_focusPOIecef;
			glm::dvec3 v = g_geoNav.cam.pos - poi;          // radius vector
			v *= std::pow(0.9, dolly);                      // match GeoNav::dolly
			g_geoNav.cam.pos = poi + v;
			g_geoNav.cam.dir = glm::normalize(poi - g_geoNav.cam.pos);
			g_geoNav.targetDist =
			    std::max(glm::length(v) / std::max((double)g_viewDistXR, 0.1), 20.0);
		} else {
			g_geoNav.dolly(dolly);
		}
		active = true;
	}
	if (active) g_last_input_t = g_now_sec.load(std::memory_order_relaxed);

	// [FOCUS] idle auto-orbit (turntable around the POI) after 10 s — focus only.
	double idleFor = g_now_sec.load(std::memory_order_relaxed) - g_last_input_t;
	if (g_focusActive && idleFor > 10.0 && g_focusT >= 1.0) {
		orbit_camera_around_poi((6.2831853 / 60.0) * (double)dt, 0.0);  // 1 rev / 60 s
	}
}

bool
render_frame()
{
	XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState fs = {XR_TYPE_FRAME_STATE};
	if (xrWaitFrame(g_session, &wi, &fs) != XR_SUCCESS) return false;
	XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
	if (xrBeginFrame(g_session, &bi) != XR_SUCCESS) return false;

	auto now = std::chrono::steady_clock::now();
	float dt = std::chrono::duration<float>(now - g_last_t).count();
	g_now_sec.store(std::chrono::duration<double>(now.time_since_epoch()).count(), std::memory_order_relaxed);
	g_last_t = now;
	if (dt > 0.1f) dt = 0.1f;
	apply_nav(dt);

	XrCompositionLayerProjectionView pviews[kViewCount] = {};
	bool rendered = false;

	if (fs.shouldRender) {
		XrViewState vs = {XR_TYPE_VIEW_STATE};
		XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
		li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		li.displayTime = fs.predictedDisplayTime;
		li.space = g_app_space;

		// FLY uses the CAMERA rig (runtime owns the off-axis eyes, convergence
		// auto-focuses on the ground). FOCUS uses the DISPLAY rig (portal) built
		// from the live camera rig via the cam->display converter — the macOS/
		// Windows model (PR #4): the world stays camera-centric, the POI is framed
		// onto the zero-parallax plane by targetDist, and the display eye sits at
		// the convergence anchor so the pick lands on the inspected feature.
		XrCameraRigEXT camRig = {XR_TYPE_CAMERA_RIG_EXT};
		XrDisplayRigEXT dispRig = {XR_TYPE_DISPLAY_RIG_EXT};
		XrViewDisplayRawEXT raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
		if (g_has_view_rig) {
			const float camVFov = CamVFovRad(g_displayHeightM, g_nominalViewerZ);
			if (!g_focusActive) {
				camRig.pose.orientation = {0, 0, 0, 1};
				camRig.ipdFactor = 1.0f;
				camRig.parallaxFactor = 1.0f;
				camRig.convergenceDiopters = g_convDiopters;
				camRig.verticalFov = camVFov;
				// On the focus->fly return, glide ipd/par from the focus value back
				// to 1.0 over ~0.4 s (g_stereoFull drops 1->0).
				if (g_stereoFull > 1.0e-4) {
					double f = (double)g_convDiopters * (double)g_nominalViewerZ;
					double invF = (f > 1.0e-4) ? (1.0 / f) : 1.0;
					camRig.ipdFactor = (float)((1.0 - g_stereoFull) * 1.0 + g_stereoFull * invF);
					camRig.parallaxFactor = camRig.ipdFactor;
				}
				g_viewDistXR = (g_convDiopters > 1.0e-6f) ? (1.0f / g_convDiopters) : 0.0f;
				li.next = &camRig;
			} else {
				float canvasH = (g_displayHeightM > 1.0e-6f) ? g_displayHeightM : 1.0f;
				float canvasW = (g_canvasWM > 1.0e-6f) ? g_canvasWM : canvasH;
				dxr_rig_display_info dinfo = {
				    canvasH, (canvasH > 1.0e-6f) ? (canvasW / canvasH) : 1.0f, g_nominalViewerZ};
				dxr_camera_rig crig0 = {};
				crig0.pose.orientation = {0, 0, 0, 1};
				crig0.ipd_factor = 1.0f;
				crig0.parallax_factor = 1.0f;
				crig0.inv_convergence_distance = g_convDiopters;
				crig0.half_tan_vfov = tanf(0.5f * camVFov);
				crig0.m2v = 1.0f;
				dxr_display_rig drig = {};
				dxr_view_rig_camera_to_display(&crig0, &dinfo, &drig);
				dispRig.pose.orientation = {drig.pose.orientation.x, drig.pose.orientation.y,
				                            drig.pose.orientation.z, drig.pose.orientation.w};
				dispRig.pose.position = {drig.pose.position.x, drig.pose.position.y,
				                         drig.pose.position.z};
				dispRig.virtualDisplayHeight = drig.virtual_display_height;
				dispRig.perspectiveFactor = drig.perspective_factor;
				// Glide ipd/par from the converter's seamless value to full 1.0.
				double sf = g_stereoFull;
				dispRig.ipdFactor = (float)((1.0 - sf) * drig.ipd_factor + sf * 1.0);
				dispRig.parallaxFactor = (float)((1.0 - sf) * drig.parallax_factor + sf * 1.0);
				float m2v_eff = (canvasH > 1.0e-6f) ? (drig.virtual_display_height / canvasH) : 0.0f;
				g_viewDistXR = drig.perspective_factor * m2v_eff * g_nominalViewerZ;
				li.next = &dispRig;
			}
			vs.next = &raw;
		}

		XrView views[kViewCount] = {};
		for (auto &v : views) v.type = XR_TYPE_VIEW;
		uint32_t located = 0;
		XrResult res = xrLocateViews(g_session, &li, &vs, kViewCount, &located, views);
		if (res == XR_SUCCESS && located >= kViewCount && g_tiles_active) {
			// Per-eye matrices (cam rig: tight near/far around the 1-XR-m scene).
			float viewMat[kViewCount][16], projMat[kViewCount][16];
			glm::dvec3 viewerPos(0.0);
			XrFovf ufov = views[0].fov;
			for (uint32_t e = 0; e < kViewCount; ++e) {
				mat4_view_from_xr_pose(viewMat[e], views[e].pose);
				mat4_from_xr_fov(projMat[e], views[e].fov, 0.05f, 200.0f);
				convert_projection_gl_to_zero_to_one(projMat[e]);
				viewerPos += glm::dvec3(views[e].pose.position.x, views[e].pose.position.y, views[e].pose.position.z);
				ufov.angleLeft = std::min(ufov.angleLeft, views[e].fov.angleLeft);
				ufov.angleRight = std::max(ufov.angleRight, views[e].fov.angleRight);
				ufov.angleDown = std::min(ufov.angleDown, views[e].fov.angleDown);
				ufov.angleUp = std::max(ufov.angleUp, views[e].fov.angleUp);
			}
			viewerPos /= (double)kViewCount;

			// Camera-centric FLY world mapping (anchor = XR origin).
			const double s = kTargetXrDist / std::max(g_geoNav.targetDist, 1.0);
			g_xrFromEcef = geo::xrFromEcefCamera(g_geoNav.cam, glm::dvec3(0.0), s);

			// Convergence auto-focus: ground distance under the forward ray.
			// FROZEN during focus (macOS parity — the `!g_focusActive` gate): while
			// inspecting, the POI was framed onto the ZDP at acquire, so the orbit
			// must keep that convergence FIXED. Letting the auto-focus run in focus
			// drifts convD every frame (the "auto IPD shrinkage over an orbit
			// session" — Android-only because Win/Mac freeze it + use the display rig).
			double groundM = geo::rayGroundDistanceM(g_geoNav.cam.pos, g_geoNav.cam.dir);
			if (groundM > 0.0 && !g_focusActive) {
				double xrDist = std::min(std::max(groundM * s, 0.2), 50.0);
				float tgt = (float)(1.0 / xrDist);
				double a = 1.0 - std::exp(-(double)dt / kConvSmoothTau);
				g_convDiopters += (tgt - g_convDiopters) * (float)a;
			}
			// (g_viewDistXR is set per-rig in the rig block above: 1/convergence for
			// the camera rig in fly, the display-plane distance in focus.)
			// Canvas px (for the tap→NDC pick), from the rig raw channel.
			if (raw.canvasRectPx.extent.width > 0 && raw.canvasRectPx.extent.height > 0) {
				g_canvas_px_w = (uint32_t)raw.canvasRectPx.extent.width;
				g_canvas_px_h = (uint32_t)raw.canvasRectPx.extent.height;
			}
			// Latch physical display height (m) + nominal viewer Z for the orthoscopic
			// FOV (§5): canvasSizeMeters = physical canvas (== full display on Android);
			// rawEyes[].z = display-space viewer distance. Refresh only on nominal
			// (non-tracking) samples so the FOV doesn't jitter with head tracking.
			if (raw.canvasSizeMeters.height > 0.0f && raw.eyeCountOutput > 0 &&
			    (g_displayHeightM <= 0.0f || !raw.isTracking)) {
				g_displayHeightM = raw.canvasSizeMeters.height;
				g_canvasWM = raw.canvasSizeMeters.width;
				g_nominalViewerZ = std::fabs(raw.rawEyes[0].z);
			}

			// Selection camera = the head camera in ECEF (inverse world).
			geo::GeoCamera selCam;
			{
				glm::dmat4 inv = glm::inverse(g_xrFromEcef);
				glm::dmat3 invRot = glm::dmat3(inv);
				selCam.pos = glm::dvec3(inv * glm::dvec4(0.0, 0.0, 0.0, 1.0));
				selCam.dir = glm::normalize(invRot * glm::dvec3(0.0, 0.0, -1.0));
				selCam.up = glm::normalize(invRot * glm::dvec3(0.0, 1.0, 0.0));
			}
			double aspect = (g_tile_h > 0) ? (double)g_tile_w / (double)g_tile_h : 1.0;
			double camVFov = (double)CamVFovRad(g_displayHeightM, g_nominalViewerZ);
			double vfov = camVFov * 1.15;  // selection frustum matches the render FOV (§5)
			double hfov = 2.0 * std::atan(std::tan(0.5 * camVFov) * aspect) * 1.15;

			const auto &result = g_tile_engine.update(selCam, (double)g_tile_w, (double)g_tile_h, hfov, vfov);
			auto drawList = g_tile_renderer.buildDrawList(result, g_xrFromEcef);

			// Attribution feed for the Kotlin overlay (~2 Hz). The Google Map Tiles
			// ToS REQUIRES showing the Google logo + the per-tile data-provider
			// credits; the logo is in the overlay, the credits come from here.
			if ((g_frame_count % 30) == 0) {
				const auto &att = g_tile_engine.attribution();
				std::string s;
				for (size_t i = 0; i < att.credits.size() && i < 8; ++i) {
					if (!s.empty()) s += " • ";
					s += att.credits[i];
				}
				{
					std::lock_guard<std::mutex> lk(g_attr_mtx);
					g_attr_text = s;
				}
				if ((g_frame_count % 300) == 0)
					LOGI("tiles draw=%zu rt=%d convD=%.3f tgtDist=%.0f focus=%d",
					     drawList.size(), att.renderTiles, g_convDiopters, g_geoNav.targetDist,
					     (int)g_focusActive);
			}

			// [FOCUS] depth pick — render a dedicated CAMERA-CENTRIC mono view (XR
			// origin, identity orientation = looking -Z, symmetric physical FOV)
			// into tile 0, read depth at the tap NDC, unproject through it. This is
			// the EXACT frame g_xrFromEcef maps to: the geo camera is always pinned
			// to the XR origin (xrFromEcefCamera(cam, origin, s)), every frame, in
			// fly AND focus. So the pick stays correct regardless of which rig is
			// PRESENTING. The bug it fixes: once focused, the located views[] are the
			// DISPLAY-rig eyes (back-offset pose + perspective/ipd scaling); reading
			// their depth and unprojecting against the camera-centric world map threw
			// the POI high into the sky (worse the more you zoomed — first tap from
			// fly was fine, every tap while focused drifted up). A centred mono view
			// also puts the tapped object AT the NDC (no off-axis disparity that a
			// per-eye read would shift past). Tile 0 is overwritten by the
			// presentation eyes immediately below.
			const bool pendingPick = g_pendingPick.load(std::memory_order_relaxed);
			glm::dvec3 pickAccum(0.0);
			int pickHits = 0;

			uint32_t idx = 0;
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (xrAcquireSwapchainImage(g_swapchain, &ai, &idx) == XR_SUCCESS) {
				XrSwapchainImageWaitInfo wii = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wii.timeout = XR_INFINITE_DURATION;
				if (xrWaitSwapchainImage(g_swapchain, &wii) == XR_SUCCESS) {
					if (pendingPick && !drawList.empty()) {
						// Cyclopean pick from the CAMERA ORIGIN (the geo camera is pinned
						// there by g_xrFromEcef) looking -Z, symmetric physical fov. This
						// recovers the object's XR position with the least error: the
						// display-rig presentation centre is backed off ~1 unit, so
						// casting from there threw a large translation ("way on top").
						// A centred symmetric view also cancels off-axis disparity.
						XrPosef cyc;
						cyc.position = {0.0f, 0.0f, 0.0f};
						cyc.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
						float vHalf = 0.5f * (float)camVFov;
						float hHalf = std::atan(std::tan(vHalf) * (float)aspect);
						XrFovf cfov = {-hHalf, hHalf, vHalf, -vHalf};  // symmetric centre frustum
						float cV[16], cP[16];
						mat4_view_from_xr_pose(cV, cyc);
						mat4_from_xr_fov(cP, cfov, 0.05f, 200.0f);
						convert_projection_gl_to_zero_to_one(cP);
						g_tile_renderer.renderEye(g_images[idx].image, g_swapchain_format,
						    g_atlas_w, g_atlas_h, 0, 0, g_tile_w, g_tile_h, cV, cP, drawList);
						uint32_t px = (uint32_t)std::min(std::max(
						    (g_pickNdcX + 1.0f) * 0.5f * (float)g_tile_w, 0.0f), (float)(g_tile_w - 1));
						uint32_t py = (uint32_t)std::min(std::max(
						    (1.0f - g_pickNdcY) * 0.5f * (float)g_tile_h, 0.0f), (float)(g_tile_h - 1));
						float d = g_tile_renderer.readDepth(px, py);
						if (d < 1.0f) {
							glm::dmat4 V = glm::dmat4(glm::make_mat4(cV));
							glm::dmat4 P = glm::dmat4(glm::make_mat4(cP));
							glm::dvec4 w = glm::inverse(P * V) *
							    glm::dvec4((double)g_pickNdcX, (double)g_pickNdcY, (double)d, 1.0);
							if (std::abs(w.w) > 1e-12) { pickAccum = glm::dvec3(w) / w.w; pickHits = 1; }
						}
					}
					for (uint32_t e = 0; e < kViewCount; ++e) {
						g_tile_renderer.renderEye(g_images[idx].image, g_swapchain_format,
						    g_atlas_w, g_atlas_h, e * g_tile_w, 0, g_tile_w, g_tile_h,
						    viewMat[e], projMat[e], drawList);
					}
					rendered = true;
				}
				XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(g_swapchain, &ri);
			}

			// Finalize the pick: hit → acquire/shift focus POI; miss (sky) →
			// release focus back to fly (or stay fly).
			if (pendingPick) {
				g_pendingPick.store(false, std::memory_order_relaxed);
				if (pickHits > 0) {
					glm::dvec3 xrPos = pickAccum / (double)pickHits;
					glm::dvec3 ecef = glm::dvec3(glm::inverse(g_xrFromEcef) * glm::dvec4(xrPos, 1.0));
					double poiDist = glm::length(g_geoNav.cam.pos - ecef);
					g_focusPOIecef = ecef;
					g_poiXitFromDir = g_geoNav.cam.dir;
					g_poiXitToDir = glm::normalize(ecef - g_geoNav.cam.pos);
					g_poiXitFromTD = g_geoNav.targetDist;
					g_poiXitToTD = std::max(poiDist / std::max((double)g_viewDistXR, 0.1), 20.0);
					g_focusT = 0.0;
					g_last_input_t = g_now_sec.load(std::memory_order_relaxed);
					LOGI("[FOCUS] %s POI dist=%.0f alt=%.1f",
					     g_focusActive ? "shift" : "acquired", poiDist,
					     geo::heightAboveEllipsoid(ecef));
					g_focusActive = true;
				} else if (g_focusActive) {
					g_focusActive = false;
					g_focusT = 0.0;
					LOGI("[FOCUS] sky tap -> release to fly");
				}
			}

			for (uint32_t e = 0; e < kViewCount; ++e) {
				pviews[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				pviews[e].pose = views[e].pose;
				pviews[e].fov = views[e].fov;
				pviews[e].subImage.swapchain = g_swapchain;
				pviews[e].subImage.imageArrayIndex = 0;
				pviews[e].subImage.imageRect.offset = {(int32_t)(e * g_tile_w), 0};
				pviews[e].subImage.imageRect.extent = {(int32_t)g_tile_w, (int32_t)g_tile_h};
			}
		} else if (res == XR_SUCCESS && located >= kViewCount) {
			// Keyless (no streaming): globe-blue clear so the app still shows life.
			uint32_t idx = 0;
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (xrAcquireSwapchainImage(g_swapchain, &ai, &idx) == XR_SUCCESS) {
				XrSwapchainImageWaitInfo wii = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wii.timeout = XR_INFINITE_DURATION;
				if (xrWaitSwapchainImage(g_swapchain, &wii) == XR_SUCCESS) { clear_atlas(g_images[idx].image, 0.04f, 0.16f, 0.38f); rendered = true; }
				XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(g_swapchain, &ri);
			}
			for (uint32_t e = 0; e < kViewCount; ++e) {
				pviews[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				pviews[e].pose = views[e].pose; pviews[e].fov = views[e].fov;
				pviews[e].subImage.swapchain = g_swapchain; pviews[e].subImage.imageArrayIndex = 0;
				pviews[e].subImage.imageRect.offset = {(int32_t)(e * g_tile_w), 0};
				pviews[e].subImage.imageRect.extent = {(int32_t)g_tile_w, (int32_t)g_tile_h};
			}
		}
	}

	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.space = g_app_space; layer.viewCount = kViewCount; layer.views = pviews;
	const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&layer};
	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = fs.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = rendered ? 1 : 0;
	ei.layers = rendered ? layers : nullptr;
	XrResult res = xrEndFrame(g_session, &ei);
	if (res != XR_SUCCESS) { log_xr_result("xrEndFrame", res); return false; }

	if ((++g_frame_count % 120) == 0)
		LOGI("frame %llu  tiles=%d", (unsigned long long)g_frame_count, (int)g_tile_engine.hasRenderableContent());
	return true;
}

void
destroy_all()
{
	if (g_vk_device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_vk_device);
	g_tile_engine.shutdown();
	g_tile_renderer.cleanup();
	if (g_fence) { vkDestroyFence(g_vk_device, g_fence, nullptr); g_fence = VK_NULL_HANDLE; }
	if (g_cmd_pool) { vkDestroyCommandPool(g_vk_device, g_cmd_pool, nullptr); g_cmd_pool = VK_NULL_HANDLE; }
	if (g_session != XR_NULL_HANDLE) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
	if (g_swapchain != XR_NULL_HANDLE) { xrDestroySwapchain(g_swapchain); g_swapchain = XR_NULL_HANDLE; }
	if (g_app_space != XR_NULL_HANDLE) { xrDestroySpace(g_app_space); g_app_space = XR_NULL_HANDLE; }
	if (g_vk_device != VK_NULL_HANDLE) { vkDestroyDevice(g_vk_device, nullptr); g_vk_device = VK_NULL_HANDLE; }
	if (g_vk_instance != VK_NULL_HANDLE) { vkDestroyInstance(g_vk_instance, nullptr); g_vk_instance = VK_NULL_HANDLE; }
	if (g_instance != XR_NULL_HANDLE) { xrDestroyInstance(g_instance); g_instance = XR_NULL_HANDLE; }
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", (void *)app->window);
		if (g_instance == XR_NULL_HANDLE) {
			bool ok = create_instance(app) && query_system_and_graphics_reqs() &&
			          create_vulkan_instance() && pick_physical_device() &&
			          create_vulkan_device() && create_session() &&
			          create_swapchains() && create_reference_space() && tiles_init();
			LOGI(ok ? "Bring-up complete." : "Bring-up failed; see logs.");
		}
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY"); destroy_all(); break;
	default: break;
	}
}

} // namespace

// ─── JNI bridge to MainActivity ──────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeSetRotation(JNIEnv *, jobject, jint rotation)
{
	g_display_rotation.store(rotation & 3, std::memory_order_relaxed);
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeRuntimeUnavailable(JNIEnv *, jobject)
{
	return g_runtime_unavailable.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeXrReady(JNIEnv *, jobject)
{
	return g_xr_ready.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeOnTouch(
    JNIEnv *, jobject, jint action, jint count, jfloat x0, jfloat y0, jfloat x1, jfloat y1)
{
	// action: 0=DOWN 1=UP 2=MOVE 5=PTR_DOWN 6=PTR_UP. 1 finger = look/orbit,
	// 2 fingers = pinch-zoom + pan (pan exits focus, like the desktop WASDQE);
	// a quick 2-finger double-tap resets the view (like the desktop Space).
	const double now = std::chrono::duration<double>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	std::lock_guard<std::mutex> lk(g_touch_mtx);
	const float cx = (count > 1) ? (x0 + x1) * 0.5f : x0;
	const float cy = (count > 1) ? (y0 + y1) * 0.5f : y0;

	if (action == 0 || action == 5 || action == 6) {  // DOWN / PTR_DOWN / PTR_UP
		if (action == 5 && count == 2) {  // 2-finger gesture begins
			g_two_down_t = now; g_two_down_cx = cx; g_two_down_cy = cy; g_two_moved = false;
		}
		if (action == 6 && count == 2) {  // 2-finger gesture ends (one lifted)
			if (!g_two_moved && (now - g_two_down_t) < 0.30) {  // a 2-finger tap
				if ((now - g_last_two_tap_t) < 0.40) {
					g_reset_view.store(true, std::memory_order_relaxed);  // double → reset
					g_last_two_tap_t = 0.0;
				} else {
					g_last_two_tap_t = now;
				}
			}
		}
		g_last_x0 = x0; g_last_y0 = y0; g_last_count = count;
		g_last_pinch = (count > 1) ? std::hypot(x1 - x0, y1 - y0) : -1.0f;
		g_last_cx = cx; g_last_cy = cy;
		return;
	}
	if (action == 1) { g_last_count = 0; g_last_pinch = -1.0f; return; }  // UP
	if (action == 2) {  // MOVE
		if (count > 1) {
			float d = std::hypot(x1 - x0, y1 - y0);
			float pinchMag = std::fabs(d - g_last_pinch);
			float panMag = std::hypot(cx - g_last_cx, cy - g_last_cy);
			if (g_last_pinch > 0) g_dolly += (double)(d - g_last_pinch) * 0.01;  // pinch → dolly
			// Centroid translation → pan, when it dominates the pinch (a real
			// two-finger drag). Inverted so the world follows the fingers.
			if (panMag > pinchMag) {
				g_pan_dx += -(double)(cx - g_last_cx) * 0.0016;
				g_pan_dy += (double)(cy - g_last_cy) * 0.0016;
			}
			if (panMag > 6.0f || pinchMag > 6.0f) g_two_moved = true;
			g_last_pinch = d;
		} else {
			g_look_dx += (double)(x0 - g_last_x0) * 0.004;  // 1-finger → look/orbit
			g_look_dy += (double)(y0 - g_last_y0) * 0.004;
		}
		g_last_x0 = x0; g_last_y0 = y0; g_last_cx = cx; g_last_cy = cy; g_last_count = count;
	}
}
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeResetView(JNIEnv *, jobject)
{
	g_reset_view.store(true, std::memory_order_relaxed);
}

// Double-tap at (x,y) view pixels → defer a depth-pick (resolved on the render
// thread): hit a landmark = focus/orbit it; miss (sky) = release focus → fly.
// (viewW,viewH) are the TOUCH SURFACE (View) dims, passed from Kotlin. We must
// derive the pick NDC from these, NOT from XrViewDisplayRawEXT::canvasRectPx:
// on a portrait-native panel the runtime reports canvasRectPx in the panel's
// unrotated (portrait) basis (e.g. 1600x2560), while the app runs landscape and
// the touch coords are in the rotated (landscape) basis (2560x1600). Dividing
// landscape coords by portrait dims TRANSPOSES the NDC — a centre tap mapped to
// (0.6,0.375), so every pick sampled depth up-and-right of the tap and landed on
// the ground past the tapped object. The View dims share the rendered tile's
// orientation (both landscape, aspect 1.6), so the tap↔render NDC is coherent —
// exactly how Windows divides mouse coords by its (landscape) windowW/H.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeOnDoubleTap(
    JNIEnv *, jobject, jfloat x, jfloat y, jfloat viewW, jfloat viewH)
{
	float w = viewW, h = viewH;
	if (w <= 0.0f || h <= 0.0f) { w = (float)g_canvas_px_w; h = (float)g_canvas_px_h; }
	if (w <= 0.0f || h <= 0.0f) return;  // no surface dims yet
	g_pickNdcX = 2.0f * x / w - 1.0f;
	g_pickNdcY = -(2.0f * y / h - 1.0f);  // Android y-down -> +Y-up NDC
	LOGI("TAP x=%.0f y=%.0f view=%.0fx%.0f -> ndc=(%.3f,%.3f)", x, y, w, h, g_pickNdcX, g_pickNdcY);
	g_pendingPick.store(true, std::memory_order_relaxed);
}

// The user's Map Tiles API key, from the in-app dialog (persisted in Kotlin
// SharedPreferences). Re-creates the tileset on the render thread.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeSetApiKey(
    JNIEnv *env, jobject, jstring key)
{
	const char *k = key ? env->GetStringUTFChars(key, nullptr) : nullptr;
	{
		std::lock_guard<std::mutex> lk(g_key_mtx);
		g_pending_key = (k != nullptr) ? k : "";
	}
	if (k) env->ReleaseStringUTFChars(key, k);
	g_apply_key.store(true, std::memory_order_relaxed);
}

// True once the tileset is streaming (Kotlin shows the key dialog if false).
extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeTilesActive(JNIEnv *, jobject)
{
	return g_tiles_active ? JNI_TRUE : JNI_FALSE;
}

// Joined data-provider credits (the Google logo is rendered by the overlay).
extern "C" JNIEXPORT jstring JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeGetAttribution(JNIEnv *env, jobject)
{
	std::string s;
	{
		std::lock_guard<std::mutex> lk(g_attr_mtx);
		s = g_attr_text;
	}
	return env->NewStringUTF(s.c_str());
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("earthview_vk_android: android_main entered");
	g_internal_data_path = app->activity->internalDataPath;
	app->onAppCmd = handle_cmd;
	if (!initialize_loader(app)) LOGE("OpenXR loader init failed");
	while (true) {
		const int timeout_ms = g_session_running ? 0 : 250;
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(timeout_ms, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) source->process(app, source);
			if (app->destroyRequested != 0) { destroy_all(); return; }
		}
		if (g_instance != XR_NULL_HANDLE) {
			poll_xr_events();
			if (g_exit_requested) { destroy_all(); return; }
			apply_pending_key();  // re-create the tileset if a key was just entered
			if (app->window != nullptr && g_session_running) render_frame();
		}
	}
}
