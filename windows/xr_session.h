// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for Vulkan with XR_DXR_win32_window_binding
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include "xr_session_common.h"
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_mcp_tools.h>

// XR_DXR_view_rig (W7 of #396): the runtime owns the off-axis Kooima and
// returns render-ready XrView{pose, fov}; the app deletes its own. App-owned
// flag (displayxr::common's XrSessionManager carries no app-named fields,
// #396 W4); set by InitializeOpenXR.
extern bool g_hasViewRigExt;

// XrDisplayDesktopPositionDXR (XR_DXR_display_info v16, runtime#715): the 3D
// panel's top-left in virtual-desktop pixels (top-down, origin = primary
// monitor top-left). (0, 0) = primary/unknown — the safe default an old
// runtime yields by ignoring the chained struct. App-owned globals (common's
// XrSessionManager carries no app-named fields); set by InitializeOpenXR.
extern int32_t g_displayDesktopLeft;
extern int32_t g_displayDesktopTop;

// Initialize OpenXR instance with Vulkan + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// Get Vulkan graphics requirements and set up Vulkan instance/device per OpenXR spec
bool GetVulkanGraphicsRequirements(XrSessionManager& xr);

// Create Vulkan instance with required extensions from the runtime
bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance);

// Get the physical device selected by the runtime
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);

// Get required device extensions from the runtime
bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage);

// Find a graphics queue family
bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);

// Create Vulkan logical device with required extensions
bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue);

// Create OpenXR session with Vulkan binding + win32_window_binding
bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd);
