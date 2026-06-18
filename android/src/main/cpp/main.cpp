// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// earthview_vk_android entry point — M3 PLACEHOLDER leg. Brings up the DisplayXR
// runtime out-of-process (ADR-025) exactly like the modelviewer/cube Android
// harness (loader → instance → Vulkan2 device → stereo atlas swapchain →
// session), but the per-frame work is a vkCmdClearColorImage to a globe-blue
// frame instead of a renderer. Its purpose is to prove the harness on NP02J:
// manifest broker discovery, OOP session bring-up, swapchain present + weave.
//
// The cesium-native tile streaming + the shared tiles_common/ Vulkan renderer +
// the macOS-parity focus/orbit camera land in the NEXT step (the §5a spike
// proved the cesium dep tree builds for arm64-android). See docs/m3-android-plan.md.

#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_display_info.h>
#include <openxr/XR_EXT_view_rig.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <jni.h>
#include <unistd.h>
#include <vector>

#define LOG_TAG "earthview_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr uint32_t kViewCount = 2;  // PRIMARY_STEREO

const char *
xr_result_str(XrResult r)
{
	switch (r) {
	case XR_SUCCESS:                   return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:     return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:       return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
	default:                           return nullptr;
	}
}

void
log_xr_result(const char *what, XrResult r)
{
	const char *name = xr_result_str(r);
	if (name) LOGI("%s -> %s", what, name);
	else LOGI("%s -> XrResult(%d)", what, (int)r);
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
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
bool g_session_running = false;
bool g_exit_requested = false;
XrSpace g_app_space = XR_NULL_HANDLE;

VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;
XrSwapchain g_swapchain = XR_NULL_HANDLE;
XrSwapchainImageVulkanKHR g_images[8] = {};
uint32_t g_image_count = 0;
uint32_t g_atlas_w = 0, g_atlas_h = 0;
bool g_has_view_rig = false;
uint64_t g_frame_count = 0;

// ── JNI-shared ───────────────────────────────────────────────────────────
std::atomic<int> g_display_rotation{0};
std::atomic<bool> g_runtime_unavailable{false};
std::atomic<bool> g_xr_ready{false};

// ── Bring-up ─────────────────────────────────────────────────────────────
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	        (PFN_xrVoidFunction *)&xrInitializeLoaderKHR) != XR_SUCCESS ||
	    !xrInitializeLoaderKHR) {
		LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed");
		return false;
	}
	XrLoaderInitInfoAndroidKHR info = {};
	info.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	info.applicationVM = app->activity->vm;
	info.applicationContext = app->activity->clazz;
	XrResult res = xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR *)&info);
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	g_runtime_unavailable.store(false, std::memory_order_relaxed);

	// XR_EXT_view_rig only when advertised (#396 W7).
	g_has_view_rig = false;
	uint32_t n = 0;
	if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr) == XR_SUCCESS && n > 0) {
		std::vector<XrExtensionProperties> props(n);
		for (auto &p : props) { p.type = XR_TYPE_EXTENSION_PROPERTIES; p.next = nullptr; }
		if (xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()) == XR_SUCCESS) {
			for (uint32_t i = 0; i < n; ++i)
				if (std::strcmp(props[i].extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0)
					g_has_view_rig = true;
		}
	}
	LOGI("XR_EXT_view_rig advertised: %s", g_has_view_rig ? "yes" : "no");

	const char *exts[4] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	    XR_EXT_DISPLAY_INFO_EXTENSION_NAME,
	};
	uint32_t ext_count = 3;
	if (g_has_view_rig) exts[ext_count++] = XR_EXT_VIEW_RIG_EXTENSION_NAME;

	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo ci = {};
	ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
	ci.next = &android_info;
	std::strncpy(ci.applicationInfo.applicationName, "earthview_vk_android",
	             XR_MAX_APPLICATION_NAME_SIZE - 1);
	ci.applicationInfo.applicationVersion = 1;
	std::strncpy(ci.applicationInfo.engineName, "displayxr", XR_MAX_ENGINE_NAME_SIZE - 1);
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ci.enabledExtensionCount = ext_count;
	ci.enabledExtensionNames = exts;

	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = xrCreateInstance(&ci, &g_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) break;
		LOGW("xrCreateInstance: runtime unavailable (attempt %d/5)…", attempt + 1);
		usleep(400 * 1000);
	}
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE)
			g_runtime_unavailable.store(true, std::memory_order_relaxed);
		return false;
	}
	g_xr_ready.store(true, std::memory_order_relaxed);
	LOGI("EARTHVIEW_SENTINEL xrCreateInstance=XR_SUCCESS");
	return true;
}

bool
query_system_and_graphics_reqs()
{
	XrSystemGetInfo si = {};
	si.type = XR_TYPE_SYSTEM_GET_INFO;
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &si, &g_system_id);
	if (res != XR_SUCCESS) {
		si.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_instance, &si, &g_system_id);
	}
	log_xr_result("xrGetSystem", res);
	if (res != XR_SUCCESS) return false;

	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsRequirements2KHR",
	        (PFN_xrVoidFunction *)&get_reqs) != XR_SUCCESS || !get_reqs)
		return false;
	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = get_reqs(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) return false;
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR fn = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrCreateVulkanInstanceKHR",
	        (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn)
		return false;
	VkApplicationInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	ai.pApplicationName = "earthview_vk_android";
	ai.pEngineName = "displayxr";
	ai.apiVersion = VK_MAKE_VERSION(XR_VERSION_MAJOR(g_required_vk_version),
	                                XR_VERSION_MINOR(g_required_vk_version), 0);
	VkInstanceCreateInfo vci = {};
	vci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vci.pApplicationInfo = &ai;
	XrVulkanInstanceCreateInfoKHR xci = {};
	xci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xci.systemId = g_system_id;
	xci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xci.vulkanCreateInfo = &vci;
	VkResult vr = VK_SUCCESS;
	XrResult res = fn(g_instance, &xci, &g_vk_instance, &vr);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	return res == XR_SUCCESS && vr == VK_SUCCESS;
}

bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR fn = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsDevice2KHR",
	        (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn)
		return false;
	XrVulkanGraphicsDeviceGetInfoKHR info = {};
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = g_system_id;
	info.vulkanInstance = g_vk_instance;
	XrResult res = fn(g_instance, &info, &g_vk_phys_device);
	log_xr_result("xrGetVulkanGraphicsDevice2KHR", res);
	return res == XR_SUCCESS;
}

bool
create_vulkan_device()
{
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, nullptr);
	if (qf_count == 0) return false;
	VkQueueFamilyProperties qf[16] = {};
	if (qf_count > 16) qf_count = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, qf);
	g_vk_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; ++i)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_vk_queue_family = i; break; }
	if (g_vk_queue_family == UINT32_MAX) return false;

	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;

	PFN_xrCreateVulkanDeviceKHR fn = nullptr;
	if (xrGetInstanceProcAddr(g_instance, "xrCreateVulkanDeviceKHR",
	        (PFN_xrVoidFunction *)&fn) != XR_SUCCESS || !fn)
		return false;
	XrVulkanDeviceCreateInfoKHR xci = {};
	xci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xci.systemId = g_system_id;
	xci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xci.vulkanPhysicalDevice = g_vk_phys_device;
	xci.vulkanCreateInfo = &dci;
	VkResult vr = VK_SUCCESS;
	XrResult res = fn(g_instance, &xci, &g_vk_device, &vr);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vr != VK_SUCCESS) return false;
	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);

	// Clear-frame command resources.
	VkCommandPoolCreateInfo pci = {};
	pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g_vk_queue_family;
	if (vkCreateCommandPool(g_vk_device, &pci, nullptr, &g_cmd_pool) != VK_SUCCESS) return false;
	VkCommandBufferAllocateInfo cbi = {};
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbi.commandPool = g_cmd_pool;
	cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbi.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(g_vk_device, &cbi, &g_cmd) != VK_SUCCESS) return false;
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateFence(g_vk_device, &fci, nullptr, &g_fence) != VK_SUCCESS) return false;
	LOGI("Vulkan device ready: queue_family=%u", g_vk_queue_family);
	return true;
}

bool
create_session()
{
	XrGraphicsBindingVulkanKHR b = {};
	b.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	b.instance = g_vk_instance;
	b.physicalDevice = g_vk_phys_device;
	b.device = g_vk_device;
	b.queueFamilyIndex = g_vk_queue_family;
	b.queueIndex = 0;
	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &b;
	ci.systemId = g_system_id;
	XrResult res = xrCreateSession(g_instance, &ci, &g_session);
	log_xr_result("xrCreateSession", res);
	return res == XR_SUCCESS;
}

bool
create_swapchains()
{
	uint32_t vc = 0;
	XrResult res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
	if (res != XR_SUCCESS || vc != kViewCount) {
		LOGE("Expected %u views, runtime reports %u", kViewCount, vc);
		return false;
	}
	XrViewConfigurationView vcfg[kViewCount] = {};
	for (auto &v : vcfg) v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, kViewCount, &vc, vcfg);
	if (res != XR_SUCCESS) { log_xr_result("xrEnumerateViewConfigurationViews", res); return false; }

	uint32_t fc = 0;
	if (xrEnumerateSwapchainFormats(g_session, 0, &fc, nullptr) != XR_SUCCESS || fc == 0) return false;
	int64_t formats[64] = {};
	if (fc > 64) fc = 64;
	if (xrEnumerateSwapchainFormats(g_session, fc, &fc, formats) != XR_SUCCESS) return false;
	const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
	for (int64_t pref : preferred)
		for (uint32_t i = 0; i < fc && g_swapchain_format == VK_FORMAT_UNDEFINED; ++i)
			if (formats[i] == pref) g_swapchain_format = (VkFormat)pref;
	if (g_swapchain_format == VK_FORMAT_UNDEFINED) g_swapchain_format = (VkFormat)formats[0];

	// One atlas swapchain, 2x1 of the recommended rect (placeholder tiling).
	g_atlas_w = vcfg[0].recommendedImageRectWidth * 2;
	g_atlas_h = vcfg[0].recommendedImageRectHeight;

	XrSwapchainCreateInfo ci = {};
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = g_swapchain_format;
	ci.sampleCount = 1;
	ci.width = g_atlas_w;
	ci.height = g_atlas_h;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	res = xrCreateSwapchain(g_session, &ci, &g_swapchain);
	if (res != XR_SUCCESS) { log_xr_result("xrCreateSwapchain", res); return false; }

	uint32_t ic = 0;
	xrEnumerateSwapchainImages(g_swapchain, 0, &ic, nullptr);
	if (ic > 8) ic = 8;
	for (uint32_t i = 0; i < ic; ++i) g_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	res = xrEnumerateSwapchainImages(g_swapchain, ic, &ic,
	    (XrSwapchainImageBaseHeader *)g_images);
	if (res != XR_SUCCESS) { log_xr_result("xrEnumerateSwapchainImages", res); return false; }
	g_image_count = ic;
	LOGI("Atlas swapchain: %ux%u, %u images, fmt 0x%x", g_atlas_w, g_atlas_h, ic,
	     (uint32_t)g_swapchain_format);
	return true;
}

bool
create_reference_space()
{
	XrReferenceSpaceCreateInfo ci = {};
	ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	XrResult res = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
	log_xr_result("xrCreateReferenceSpace(LOCAL)", res);
	return res == XR_SUCCESS;
}

void
handle_session_state(XrSessionState s)
{
	g_session_state = s;
	if (s == XR_SESSION_STATE_READY) {
		XrSessionBeginInfo b = {};
		b.type = XR_TYPE_SESSION_BEGIN_INFO;
		b.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (xrBeginSession(g_session, &b) == XR_SUCCESS) g_session_running = true;
		log_xr_result("xrBeginSession", XR_SUCCESS);
	} else if (s == XR_SESSION_STATE_STOPPING) {
		xrEndSession(g_session);
		g_session_running = false;
	} else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
		g_exit_requested = true;
	}
}

void
poll_xr_events()
{
	for (;;) {
		XrEventDataBuffer ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res == XR_EVENT_UNAVAILABLE) break;
		if (res != XR_SUCCESS) break;
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *e = (const XrEventDataSessionStateChanged *)&ev;
			if (e->session == g_session) { LOGI("session state -> %d", (int)e->state); handle_session_state(e->state); }
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			g_exit_requested = true;
		}
	}
}

// Record + submit a clear of the acquired atlas image to globe-blue.
void
clear_image(VkImage image, float r, float g, float b)
{
	vkResetCommandBuffer(g_cmd, 0);
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(g_cmd, &bi);

	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	auto barrier = [&](VkImageLayout from, VkImageLayout to,
	                   VkAccessFlags srcA, VkAccessFlags dstA,
	                   VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
		VkImageMemoryBarrier mb = {};
		mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		mb.oldLayout = from;
		mb.newLayout = to;
		mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		mb.image = image;
		mb.subresourceRange = range;
		mb.srcAccessMask = srcA;
		mb.dstAccessMask = dstA;
		vkCmdPipelineBarrier(g_cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &mb);
	};

	barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        0, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkClearColorValue color = {};
	color.float32[0] = r; color.float32[1] = g; color.float32[2] = b; color.float32[3] = 1.0f;
	vkCmdClearColorImage(g_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
	barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	vkEndCommandBuffer(g_cmd);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g_cmd;
	vkResetFences(g_vk_device, 1, &g_fence);
	vkQueueSubmit(g_vk_queue, 1, &si, g_fence);
	vkWaitForFences(g_vk_device, 1, &g_fence, VK_TRUE, UINT64_MAX);
}

bool
render_frame()
{
	XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState fs = {XR_TYPE_FRAME_STATE};
	if (xrWaitFrame(g_session, &wi, &fs) != XR_SUCCESS) return false;
	XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
	if (xrBeginFrame(g_session, &bi) != XR_SUCCESS) return false;

	XrCompositionLayerProjectionView pviews[kViewCount] = {};
	bool rendered = false;

	if (fs.shouldRender) {
		XrViewState vs = {XR_TYPE_VIEW_STATE};
		XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
		li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		li.displayTime = fs.predictedDisplayTime;
		li.space = g_app_space;

		// Chain an identity display rig so the runtime returns render-ready
		// off-axis views (server-side Kooima over IPC on Android).
		XrDisplayRigEXT rig = {XR_TYPE_DISPLAY_RIG_EXT};
		XrViewDisplayRawEXT raw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
		if (g_has_view_rig) {
			rig.pose.orientation = {0, 0, 0, 1};
			rig.virtualDisplayHeight = 1.33f;
			rig.ipdFactor = 1.0f;
			rig.parallaxFactor = 1.0f;
			rig.perspectiveFactor = 1.0f;
			li.next = &rig;
			vs.next = &raw;
		}

		XrView views[kViewCount] = {};
		for (auto &v : views) v.type = XR_TYPE_VIEW;
		uint32_t located = 0;
		XrResult res = xrLocateViews(g_session, &li, &vs, kViewCount, &located, views);
		if (res == XR_SUCCESS && located >= 1) {
			// Acquire the atlas image, clear it to a gently pulsing globe-blue.
			uint32_t idx = 0;
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (xrAcquireSwapchainImage(g_swapchain, &ai, &idx) == XR_SUCCESS) {
				XrSwapchainImageWaitInfo wii = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wii.timeout = XR_INFINITE_DURATION;
				if (xrWaitSwapchainImage(g_swapchain, &wii) == XR_SUCCESS) {
					float pulse = 0.35f + 0.15f * (float)std::sin((double)g_frame_count * 0.03);
					clear_image(g_images[idx].image, 0.04f, 0.16f, pulse);
					rendered = true;
				}
				XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(g_swapchain, &ri);
			}

			// Both projection views reference the one atlas; tile = half each (2x1).
			const int32_t tileW = (int32_t)(g_atlas_w / 2);
			const int32_t tileH = (int32_t)g_atlas_h;
			for (uint32_t i = 0; i < kViewCount; ++i) {
				pviews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				pviews[i].pose = views[i].pose;
				pviews[i].fov = views[i].fov;
				pviews[i].subImage.swapchain = g_swapchain;
				pviews[i].subImage.imageArrayIndex = 0;
				pviews[i].subImage.imageRect.offset = {(int32_t)i * tileW, 0};
				pviews[i].subImage.imageRect.extent = {tileW, tileH};
			}
		}
	}

	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.space = g_app_space;
	layer.viewCount = kViewCount;
	layer.views = pviews;
	const XrCompositionLayerBaseHeader *layers[1] = {
	    (const XrCompositionLayerBaseHeader *)&layer};

	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = fs.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = rendered ? 1 : 0;
	ei.layers = rendered ? layers : nullptr;
	XrResult res = xrEndFrame(g_session, &ei);
	if (res != XR_SUCCESS) { log_xr_result("xrEndFrame", res); return false; }

	if ((++g_frame_count % 120) == 0) LOGI("frame %llu", (unsigned long long)g_frame_count);
	return true;
}

void
destroy_all()
{
	if (g_vk_device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_vk_device);
	if (g_fence) vkDestroyFence(g_vk_device, g_fence, nullptr), g_fence = VK_NULL_HANDLE;
	if (g_cmd_pool) vkDestroyCommandPool(g_vk_device, g_cmd_pool, nullptr), g_cmd_pool = VK_NULL_HANDLE;
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
			          create_swapchains() && create_reference_space();
			LOGI(ok ? "Bring-up complete." : "Bring-up failed; see logs.");
		}
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_all();
		break;
	default:
		break;
	}
}

} // namespace

// ─── JNI bridge to MainActivity ──────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeSetRotation(
    JNIEnv *, jobject, jint rotation)
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
    JNIEnv *, jobject, jint, jint, jfloat, jfloat, jfloat, jfloat)
{
	// Placeholder: gesture → globe nav lands with the tile renderer (next step).
}

extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_earthview_1vk_1android_MainActivity_nativeResetView(JNIEnv *, jobject)
{
	// Placeholder: re-frame lands with the focus/orbit camera (next step).
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
