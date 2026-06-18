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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// Tiles.
TileRenderer g_tile_renderer;
TileEngine g_tile_engine;
bool g_tiles_active = false;
geo::GeoNav g_geoNav;
glm::dmat4 g_xrFromEcef(1.0);
float g_convDiopters = 1.0f;
auto g_last_t = std::chrono::steady_clock::now();

// JNI-shared.
std::atomic<int> g_display_rotation{0};
std::atomic<bool> g_runtime_unavailable{false};
std::atomic<bool> g_xr_ready{false};
std::atomic<bool> g_reset_view{false};

// Touch → nav deltas (accumulated by JNI, drained by the render thread).
std::mutex g_touch_mtx;
double g_look_dx = 0, g_look_dy = 0, g_dolly = 0;
float g_last_x0 = 0, g_last_y0 = 0;
float g_last_pinch = -1.0f;
int g_last_count = 0;

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
seed_api_key_from_prop()
{
	char prop[PROP_VALUE_MAX] = {};
	if (__system_property_get("debug.dxr.ev.key", prop) > 0 && prop[0]) {
		setenv("GOOGLE_MAPS_API_KEY", prop, 1);
		LOGI("API key seeded from debug.dxr.ev.key (len %zu)", strlen(prop));
	}
}

bool
tiles_init()
{
	seed_api_key_from_prop();
	if (!g_tile_renderer.init(g_vk_instance, g_vk_phys_device, g_vk_device, g_vk_queue,
	        g_vk_queue_family, g_tile_w, g_tile_h)) {
		LOGE("TileRenderer::init failed");
		return false;
	}
	g_tiles_active = g_tile_engine.init(&g_tile_renderer);
	LOGI("TileEngine::init -> tiles %s (key %s)", g_tiles_active ? "ACTIVE" : "inactive",
	     g_tile_engine.hasKey() ? "present" : "MISSING");
	g_geoNav.frameBookmark(0);  // seed the first city
	return true;
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

// Drain accumulated touch deltas into the geo camera (render thread).
void
apply_nav(float dt)
{
	double look_dx, look_dy, dolly;
	{
		std::lock_guard<std::mutex> lk(g_touch_mtx);
		look_dx = g_look_dx; look_dy = g_look_dy; dolly = g_dolly;
		g_look_dx = g_look_dy = g_dolly = 0;
	}
	if (g_reset_view.exchange(false)) g_geoNav.frameBookmark(g_geoNav.bookmarkIndex);
	if (look_dx != 0 || look_dy != 0) g_geoNav.look(look_dx, look_dy);
	if (dolly != 0) g_geoNav.dolly(dolly);
	(void)dt;
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

		// Camera rig (fly): the runtime owns the off-axis eyes; convergence
		// auto-focuses on the ground under the crosshair.
		XrCameraRigEXT rig = {XR_TYPE_CAMERA_RIG_EXT};
		XrViewDisplayRawEXT raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
		if (g_has_view_rig) {
			rig.pose.orientation = {0, 0, 0, 1};
			rig.ipdFactor = 1.0f; rig.parallaxFactor = 1.0f;
			rig.convergenceDiopters = g_convDiopters;
			rig.verticalFov = kCameraVFovRad;
			li.next = &rig;
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
			double groundM = geo::rayGroundDistanceM(g_geoNav.cam.pos, g_geoNav.cam.dir);
			if (groundM > 0.0) {
				double xrDist = std::min(std::max(groundM * s, 0.2), 50.0);
				float tgt = (float)(1.0 / xrDist);
				double a = 1.0 - std::exp(-(double)dt / kConvSmoothTau);
				g_convDiopters += (tgt - g_convDiopters) * (float)a;
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
			double vfov = (double)kCameraVFovRad * 1.15;
			double hfov = 2.0 * std::atan(std::tan(0.5 * (double)kCameraVFovRad) * aspect) * 1.15;

			const auto &result = g_tile_engine.update(selCam, (double)g_tile_w, (double)g_tile_h, hfov, vfov);
			auto drawList = g_tile_renderer.buildDrawList(result, g_xrFromEcef);

			uint32_t idx = 0;
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (xrAcquireSwapchainImage(g_swapchain, &ai, &idx) == XR_SUCCESS) {
				XrSwapchainImageWaitInfo wii = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wii.timeout = XR_INFINITE_DURATION;
				if (xrWaitSwapchainImage(g_swapchain, &wii) == XR_SUCCESS) {
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
	// action: 0=DOWN 1=UP 2=MOVE 5=PTR_DOWN 6=PTR_UP. Map to geo nav deltas.
	std::lock_guard<std::mutex> lk(g_touch_mtx);
	if (action == 0 || action == 5 || action == 6) {  // (re)start tracking
		g_last_x0 = x0; g_last_y0 = y0; g_last_count = count;
		g_last_pinch = (count > 1) ? std::hypot(x1 - x0, y1 - y0) : -1.0f;
		return;
	}
	if (action == 1) { g_last_count = 0; g_last_pinch = -1.0f; return; }
	if (action == 2) {  // MOVE
		if (count > 1) {
			float d = std::hypot(x1 - x0, y1 - y0);
			if (g_last_pinch > 0) g_dolly += (double)(d - g_last_pinch) * 0.01;  // pinch → dolly
			g_last_pinch = d;
		} else {
			g_look_dx += (double)(x0 - g_last_x0) * 0.004;  // drag → look
			g_look_dy += (double)(y0 - g_last_y0) * 0.004;
		}
		g_last_x0 = x0; g_last_y0 = y0; g_last_count = count;
	}
}
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeResetView(JNIEnv *, jobject)
{
	g_reset_view.store(true, std::memory_order_relaxed);
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("earthview_vk_android: android_main entered");
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
			if (app->window != nullptr && g_session_running) render_frame();
		}
	}
}
