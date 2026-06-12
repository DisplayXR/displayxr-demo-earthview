// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// TEMPORARY Checkpoint-A stub of the modelviewer ModelRenderer API.
//
// Lets the carried-over macos/main.mm shell compile and run UNMODIFIED
// (hasModel() is always false → the shell's RenderPlaceholder path draws),
// proving the OpenXR/Vulkan/sim-display plumbing before any cesium code
// lands. Deleted in Step 4 when main.mm is rewired to TileEngine +
// TileRenderer.

#pragma once

#include <vulkan/vulkan.h>
#include <string>

struct ModelRenderer {
    bool init(VkInstance, VkPhysicalDevice, VkDevice, VkQueue,
              uint32_t /*queueFamilyIndex*/, uint32_t /*renderWidth*/,
              uint32_t /*renderHeight*/)
    {
        return true;
    }

    bool loadModel(const char *) { return false; }
    bool hasModel() const { return false; }
    uint32_t primitiveCount() const { return 0; }

    void updateAnimation(float) {}
    void setActiveAnimation(int) {}
    void cycleAnimation() {}
    void togglePaused() {}
    bool isPaused() const { return false; }
    bool hasAnimations() const { return false; }
    bool getPlaybackInfo(std::string &, int &, int &, float &, float &, bool &) const
    {
        return false;
    }
    int animationCount() const { return 0; }
    int activeAnimation() const { return -1; }
    void setPaused(bool) {}
    bool getAnimationInfo(int, std::string &, float &) const { return false; }

    bool getRobustSceneBounds(float, float, float[3], float[3]) const { return false; }
    bool pickSurface(const float[3], const float[3], float[3], float = 100.0f) const
    {
        return false;
    }

    void renderEye(VkImage, VkFormat, uint32_t, uint32_t, uint32_t, uint32_t,
                   uint32_t, uint32_t, const float[16], const float[16],
                   bool = false, float = 0.0f)
    {
    }

    void cleanup() {}
};

// model_loader.h path helpers the shell references (file-open/drag-drop UI —
// removed along with this stub in Step 4).
inline bool
model_validate_file(const std::string &)
{
    return false;
}
inline std::string
model_basename(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
inline std::string
model_filesize_str(const std::string &)
{
    return std::string();
}
