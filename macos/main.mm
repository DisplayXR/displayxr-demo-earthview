// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  macOS Vulkan OpenXR glTF 2.0 PBR model viewer with external window binding
 *
 * Renders glTF 2.0 models on tracked 3D displays via OpenXR.
 * Based on cube_handle_vk_macos with the cube/grid renderer replaced by
 * the model_common/ModelRenderer PBR pipeline.  Features a "Load…" button overlay.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_EXT_cocoa_window_binding)
 * - Mouse drag camera, WASD/QE movement, scroll zoom
 * - XR_EXT_display_info: Kooima projection, display metrics
 * - V key cycles rendering modes via xrRequestDisplayRenderingModeEXT
 * - 0-3 keys select rendering mode directly
 * - L key or button click: NSOpenPanel to load .glb/.gltf models
 * - Tab: toggle HUD overlay, Space: reset camera, ESC: quit
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_cocoa_window_binding.h>
#include <openxr/XR_EXT_display_info.h>
#include <openxr/XR_EXT_atlas_capture.h>
#include <openxr/XR_EXT_view_rig.h>
#include <openxr/XR_EXT_mcp_tools.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

#include "view_params.h"
#include "mode_switch.h" // dxr::ModeSwitch — smooth 2D<->3D disparity ramp (inline on macOS)
#include "display3d_view.h"
#include "camera3d_view.h"
#include "dxr_view_math.h"  // rig converters (cam<->display) for focus mode
#include "projection_depth.h"
#include "tile_engine.h"
#include "tile_renderer.h"
#include "geo_math.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "atlas_capture.h"

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Input state
// ============================================================================

struct InputState {
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;
    bool resetViewRequested = false;
    ViewParams viewParams;
    bool hudVisible = false;  // Hidden by default; toggle with Tab.
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;
    bool eyeTrackingModeToggleRequested = false;
    bool teleportRequested = false;
    float teleportMouseX = 0.0f, teleportMouseY = 0.0f; // logical points

    // Geo-navigation deltas, accumulated by the Cocoa event handlers and
    // consumed once per frame by UpdateGeoNav (doubles live in geo_math).
    float lookDX = 0.0f, lookDY = 0.0f;   // radians (left-drag)
    float dollySteps = 0.0f;              // scroll steps (exponential zoom)
    bool cycleBookmarkRequested = false;  // 'B'
    bool releaseOrbitRequested = false;   // Esc-like release (Space shares reset)
    bool releaseToFlyRequested = false;   // 'C': orbit -> fly, continuous (no reframe)

    // Auto-orbit (turntable) mode
    bool animateEnabled = true;  // Always on; auto-orbit kicks in after 10 s idle.
    double lastInputTimeSec = 0.0;
    bool animationActive = false;
    bool animateToggleRequested = false;     // set by UI button

    // 'I' key: capture the rendered atlas region (cols × rows × renderW × renderH)
    // of the swapchain to <scene>_<cols>x<rows>.png in the working directory.
    // Skipped for 1×1 (mono) layouts. Useful for grabbing the SBS image
    // intended for shell launcher icons / 3D thumbnails.
    bool captureAtlasRequested = false;

    // Unified rendering mode (V key cycles, 0-8 keys select directly)
    uint32_t currentRenderingMode = 1;   // Default: mode 1 (first 3D mode)
    uint32_t renderingModeCount = 0;     // Set from xrEnumerateDisplayRenderingModesEXT
    bool renderingModeChangeRequested = false;
};

// Virtual-display height in meters (the rig's display plane). The world is
// scaled into XR space by geo_math's stereo-scale knob, so a fixed display
// works at every altitude.
static constexpr float kDefaultVirtualDisplayHeightM = 1.5f;

// Diorama (orbit-acquired) tabletop scale, PRD §6.1: 1:3000 default; knob
// territory down to ~1:8000 to fit deep scenes into the display volume.
static constexpr double kDioramaScale = 1.0 / 3000.0;

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSView *g_metalView = nil;
static InputState g_input;
// Smooth 2D<->3D disparity ramp, driven inline (macOS app-owned mode model).
// g_msLastMode tracks the runtime-confirmed mode (synced on fire + event) so the
// sequencer knows the ramp direction.
static dxr::ModeSwitch g_modeSwitch;
static bool g_modeSwitchConfigured = false;
static uint32_t g_msLastMode = 1; // matches g_input.currentRenderingMode default
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f;

typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

static bool g_fullscreen = false;
static NSRect g_savedWindowFrame = {};
static NSUInteger g_savedWindowStyle = 0;

// EarthView state. Teardown order matters: g_tileEngine.shutdown() (Tileset
// dtor free()s every tile through the renderer) BEFORE g_tileRenderer.cleanup().
static TileRenderer g_tileRenderer;
static TileEngine g_tileEngine;
static geo::GeoNav g_geoNav;
static bool g_tilesActive = false; // engine init'd (API key found)

// Per-frame tile state shared between the update and render sections of the
// frame loop. xrFromEcef is the double world mapping (camera or diorama).
static std::vector<TileRenderer::DrawItem> g_drawList;
static glm::dmat4 g_xrFromEcef(1.0);

// Double-click pick, deferred until after eye 0 renders this frame (the
// depth-readback unproject needs eye 0's depth buffer + matrices).
static bool g_pendingPick = false;
static float g_pickNdcX = 0.0f, g_pickNdcY = 0.0f;
static glm::dvec3 g_viewerPosXr(0.0, 0.1, 0.6); // center-eye, updated per frame

// Smooth double-click transition: the diorama starts centered on the picked
// point's CURRENT on-screen XR position (yaw/tilt/scale are seeded from the
// camera pose, so the switch is visually continuous), then the center glides
// exponentially to the display origin.
static glm::dvec3 g_dioramaCenterXr(0.0);
static constexpr double kDioramaGlideTau = 0.35; // s

// Camera-rig (fly mode) state. The runtime owns the off-axis eyes; the app
// hands it a plain perspective camera (pose + verticalFov + convergence) and
// eye tracking perturbs the frustum. Convergence default = 1/kTargetXrDist (the
// geo target sits 1 XR-m in front of the camera); a centre-ray depth read will
// refine it (matching the Windows leg). See docs/rendering-notes.md.
static constexpr float kCameraVFovRad = 0.6498f;  // ~37.2° — fallback before display info resolves
// Physical (orthoscopic) cam-rig vFOV: the angle the FULL DISPLAY subtends from
// the nominal viewing position. 2*tan(vfov/2) = displayH / nominalZ. Use the full
// physical display height (NOT the window canvas) so the fly framing stays the
// full-screen FOV even windowed. Falls back to kCameraVFovRad until display info
// (height + nominal Z) resolves. See docs/rendering-notes.md §5.
static inline float CamVFovRad(float physHeightM, float nominalZ) {
    return (physHeightM > 1.0e-6f && nominalZ > 1.0e-6f)
               ? 2.0f * atanf(physHeightM / (2.0f * nominalZ))
               : kCameraVFovRad;
}
static float g_convDiopters = 1.0f;               // 1/m to convergence plane
static float g_canvasWM = 0.0f, g_canvasHM = 0.0f; // runtime-resolved canvas size (m)
static float g_viewDistXR = 0.0f;                  // frustum-source eye->focus distance
// Double-click "focus" model: seamless cam->display rig switch (disturbance-free
// converter), frame the picked POI onto the zero-parallax plane via targetDist, and
// orbit the camera around it. World stays camera-centric (no diorama); orbitAcquired
// is NOT used in this path.
static bool g_focusActive = false;
static double g_focusT = 0.0;                      // 0->1 re-aim/reframe transition
static glm::dvec3 g_focusPOIecef(0.0);             // POI in ECEF — orbit pivot + ZDP target
// Smooth re-aim/reframe transition toward a newly picked POI (driven by g_focusT).
static glm::dvec3 g_poiXitFromDir(0.0), g_poiXitToDir(0.0);
static double g_poiXitFromTD = 0.0, g_poiXitToTD = 0.0;
// [FOCUS] stereo "fullness": 1 in orbit (display ipd/par -> 1, full depth on the
// feature), 0 in fly (ipd/par -> the user's original viewParams). Glides toward
// g_focusActive; the rig branches lerp ipd/par by it. display ipd=1 <-> cam ipd=1/f.
static double g_stereoFull = 0.0;
static constexpr double kConvSmoothTau = 0.15;    // s — convergence exp-filter time constant

static double g_avgFrameTime = 0.0;
static uint64_t g_frameCount = 0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_renderW = 0, g_renderH = 0;
static uint32_t g_windowW = 0, g_windowH = 0;

// Atlas capture helpers (filename / Pictures dir / flash overlay / Vulkan
// readback) live in test_apps/common/atlas_capture* — see dxr_capture::*.

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);
    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];
    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Display-local eye distance for the ZDP-anchored clip (#396 W7 consume path):
// z of (rigPose^-1 * eyeWorld). Equals the old eye_display.z so near = ez - vH /
// far = ez + far_offset stays identical. fov is clip-independent — this is all
// the app keeps of the old per-eye Kooima math.
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y,
                         -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv,
                     eyeWorld.x - rig.position.x,
                     eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z,
                     &ox, &oy, &oz);
    return oz;
}

// ============================================================================
// Forward decls for top-bar UI helpers (defined after CreateMacOSWindow)
// ============================================================================

struct AppXrSession;
static void UpdateTopBarButtonTitles(AppXrSession& xr);

// ============================================================================
// Input timestamp helper
// ============================================================================

static double NowSec(void) {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

static void MarkUserInput(InputState& input) {
    input.lastInputTimeSec = NowSec();
    input.animationActive = false;
}

// ============================================================================
// Geo navigation (PRD §7.1) — consumes the per-frame input deltas and drives
// the double-precision GeoNav in geo_math. The XR rig pose stays FIXED; only
// the geo camera moves (camera-centric model, §6.1).
// ============================================================================

static void UpdateGeoNav(InputState& input, float dt) {
    // [FOCUS] smooth POI transition (~0.4 s): re-aim cam.dir toward the new POI
    // (slerp) + reframe targetDist (log-lerp) so the feature glides to centre and
    // onto the zero-parallax plane. Camera position is fixed. Orbit drag cancels it.
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
    // [FOCUS] glide the stereo fullness toward the current mode (~0.4 s).
    {
        double tgt = g_focusActive ? 1.0 : 0.0;
        double rate = (double)dt / 0.4;
        if (g_stereoFull < tgt) g_stereoFull = std::min(tgt, g_stereoFull + rate);
        else if (g_stereoFull > tgt) g_stereoFull = std::max(tgt, g_stereoFull - rate);
    }
    if (input.releaseToFlyRequested) {
        input.releaseToFlyRequested = false;
        if (g_focusActive) {
            // Focus -> fly (cam rig): the cam rig inherits the frozen convergence,
            // so the switch is seamless; the ground auto-focus then resumes. (The
            // WASDQE return below is the primary exit; this is the explicit key.)
            g_focusActive = false;
            g_focusT = 0.0;
            LOG_INFO("Focus released -> fly (cam rig)");
            input.lastInputTimeSec = NowSec();
            input.animationActive = false;
            return;
        }
        if (g_geoNav.orbitAcquired) {
            g_geoNav.releaseToFly(std::max(g_viewerPosXr.z, 0.1));
            LOG_INFO("Released orbit -> fly mode (continuous)");
        }
        input.lastInputTimeSec = NowSec();
        input.animationActive = false;
    }
    if (input.resetViewRequested || input.releaseOrbitRequested) {
        input.resetViewRequested = false;
        input.releaseOrbitRequested = false;
        input.viewParams = ViewParams();
        input.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
        g_geoNav.releaseOrbit();   // back to camera-centric, reframe bookmark
        input.lookDX = input.lookDY = input.dollySteps = 0.0f;
        input.animationActive = false;
        input.lastInputTimeSec = NowSec();
        return;
    }
    if (input.cycleBookmarkRequested) {
        input.cycleBookmarkRequested = false;
        g_geoNav.cycleBookmark();  // instant jump (fly-over interp = M1.x)
        size_t n = 0;
        const geo::Bookmark* bm = geo::bookmarks(&n);
        LOG_INFO("Bookmark: %s", bm[g_geoNav.bookmarkIndex].name);
    }

    // Left-drag look / orbit.
    if (input.lookDX != 0.0f || input.lookDY != 0.0f) {
        if (g_focusActive) {
            // [FOCUS] orbit the CAMERA around the POI (feature fixed) — display-rig
            // navigation, vs the cam rig's look-around-the-eye. World stays
            // camera-centric; only cam.pos/dir revolve about g_focusPOIecef.
            g_focusT = 1.0;  // a drag cancels any in-flight re-aim transition
            glm::dvec3 poi = g_focusPOIecef;
            glm::dvec3 up = glm::normalize(poi);  // geodetic normal ~ radial
            glm::dvec3 v = g_geoNav.cam.pos - poi;
            glm::dmat4 ry = glm::rotate(glm::dmat4(1.0), -(double)input.lookDX, up);
            v = glm::dvec3(ry * glm::dvec4(v, 0.0));
            glm::dvec3 right = glm::normalize(glm::cross(up, glm::normalize(v)));
            glm::dmat4 rp = glm::rotate(glm::dmat4(1.0), -(double)input.lookDY, right);
            glm::dvec3 v2 = glm::dvec3(rp * glm::dvec4(v, 0.0));
            if (std::abs(glm::dot(glm::normalize(v2), up)) < 0.98) v = v2;  // no pole flip
            g_geoNav.cam.pos = poi + v;
            g_geoNav.cam.dir = glm::normalize(poi - g_geoNav.cam.pos);
            g_geoNav.cam.up = up;
            // Keep the POI on the convergence plane: targetDist = radius/vDist
            // (constant during orbit, so no zoom drift).
            g_geoNav.targetDist =
                std::max(glm::length(v) / std::max((double)g_viewDistXR, 0.1), 20.0);
        } else {
            g_geoNav.look((double)input.lookDX, (double)input.lookDY);
        }
        input.lookDX = input.lookDY = 0.0f;
    }
    // Scroll dolly (exponential). Zoom is a CESIUM-WORLD op (scales the tile world
    // via targetDist), NOT a rig op — so it must keep the orbit POI fixed itself (§6).
    if (input.dollySteps != 0.0f) {
        if (g_focusActive) {
            // [FOCUS] zoom = change the orbit RADIUS about the POI, then re-pin the
            // POI on the convergence plane (mirrors the orbit-drag re-pin above). A
            // plain forward dolly moves toward the geo target (not the POI during
            // orbit), so the POI slides off the convergence plane and the physical-FOV
            // full perspective makes that drift visible.
            glm::dvec3 poi = g_focusPOIecef;
            glm::dvec3 v = g_geoNav.cam.pos - poi;          // radius vector
            double k = std::pow(0.9, (double)input.dollySteps);  // match GeoNav::dolly
            v *= k;
            g_geoNav.cam.pos = poi + v;
            g_geoNav.cam.dir = glm::normalize(poi - g_geoNav.cam.pos);
            g_geoNav.targetDist =
                std::max(glm::length(v) / std::max((double)g_viewDistXR, 0.1), 20.0);
        } else {
            g_geoNav.dolly((double)input.dollySteps);
        }
        input.dollySteps = 0.0f;
    }
    // [FOCUS] WASDQE while in orbit/display mode => disturbance-free return to the
    // cam rig. At the switch instant the cam rig uses the frozen g_convDiopters
    // (still the orbit-center convergence), so the display->cam swap is seamless;
    // then the auto-focus (gated on !g_focusActive) smoothly re-tracks the ground.
    // No C key needed.
    if (g_focusActive && (input.keyW || input.keyS || input.keyA || input.keyD ||
                          input.keyE || input.keyQ)) {
        g_focusActive = false;
        g_focusT = 1.0;  // cancel any in-flight re-aim transition
        LOG_INFO("[FOCUS] WASDQE -> return to fly (cam rig); ground auto-focus resumes");
    }

    // WASD pan in the ground tangent plane, E/Q climb. Speeds scale with
    // targetDist inside GeoNav, so dt is the only factor here (~half the
    // target distance per second of held key).
    const double k = 0.5 * (double)dt;
    double panX = 0.0, panY = 0.0, climb = 0.0;
    if (input.keyW) panY += k;
    if (input.keyS) panY -= k;
    if (input.keyD) panX += k;
    if (input.keyA) panX -= k;
    if (input.keyE) climb += k;
    if (input.keyQ) climb -= k;
    if (panX != 0.0 || panY != 0.0) g_geoNav.pan(panX, panY);
    if (climb != 0.0) g_geoNav.elevate(climb);

    // Auto-orbit: idle > 10 s. ONLY in focus/orbit mode, and as a gentle turntable
    // AROUND the POI (not a free-look). In FLY it's disabled: the old idle look()
    // rotated the view every frame, churning tile LOD and making zoomed-in content
    // tremble vertically until the user moved (which reset the idle timer).
    double idleFor = NowSec() - input.lastInputTimeSec;
    input.animationActive = (input.animateEnabled && idleFor > 10.0 && g_focusActive);
    if (input.animationActive) {
        double rate = 6.2831853 / 60.0; // one revolution per 60 seconds
        glm::dvec3 poi = g_focusPOIecef;
        glm::dvec3 up = glm::normalize(poi);
        glm::dvec3 v = g_geoNav.cam.pos - poi;
        glm::dmat4 ry = glm::rotate(glm::dmat4(1.0), -(rate * (double)dt), up);
        v = glm::dvec3(ry * glm::dvec4(v, 0.0));
        g_geoNav.cam.pos = poi + v;
        g_geoNav.cam.dir = glm::normalize(poi - g_geoNav.cam.pos);
        g_geoNav.cam.up = up;
    }
}

// ============================================================================
// HUD overlay (simple NSView with CoreText)
// ============================================================================

@interface HudOverlayView : NSView
@property (nonatomic, strong) NSString *hudText;
@end

@implementation HudOverlayView

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    if (!_hudText) return;
    // No backdrop fill — the enclosing NSVisualEffectView provides frosted
    // vibrancy that auto-adapts to whatever is behind the window.
    NSDictionary *attrs = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor labelColor]
    };
    NSRect textRect = NSInsetRect(self.bounds, 10, 8);
    [_hudText drawInRect:textRect withAttributes:attrs];
}

@end

static HudOverlayView   *g_hudView = nil;      // text view
static NSVisualEffectView *g_hudBackdrop = nil;  // frosted wrapper sized to hudView

// ============================================================================
// Top-bar overlay (Open / Auto-Orbit / Mode buttons) + reticle
// ============================================================================

static NSView   *g_topBar = nil;
static NSButton *g_modeButton = nil;
static NSButton *g_animButton = nil;   // repurposed: bookmark-cycle button
static NSView   *g_animButtonBackdrop = nil;
static NSView   *g_reticleView = nil;
static NSVisualEffectView *g_attributionBar = nil;  // ALWAYS visible (policy)
static NSTextField *g_attributionText = nil;

// First-run API-key entry card (shown when keyless; see docs/api-key.md).
static NSView      *g_keyCard = nil;
static NSTextField *g_keyField = nil;
static NSTextField *g_keyStatus = nil;
// Cross-thread requests from the card's buttons → consumed on the frame loop
// (the render/updateView thread) so the late TileEngine::init is on the right
// thread. std::string guarded trivially: writes happen on the main/Cocoa
// thread, which is the same thread that runs the loop in this app.
static bool g_keySubmitRequested = false;
static std::string g_pendingKey;
static const char *kMapTilesConsoleURL =
    "https://console.cloud.google.com/google/maps-apis/api-list";

// Show the API-key card and give the field keyboard focus (the layer-backed
// Metal view is the window's first responder, so without this a click on the
// field doesn't focus it and ⌘V beeps). Reused by first-run + the ⌘K shortcut.
static void ShowKeyCard() {
    if (!g_keyCard) return;
    [g_keyCard setHidden:NO];
    [g_window makeKeyAndOrderFront:nil];
    [g_window makeFirstResponder:g_keyField];
}

// Translucent dark background view used behind the top bar.
@interface TopBarBackdropView : NSView
@end
@implementation TopBarBackdropView
- (BOOL)isFlipped { return NO; }
- (void)drawRect:(NSRect)r {
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.55] set];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.08] set];
    NSRect hr = NSMakeRect(0, 0, self.bounds.size.width, 1);
    NSRectFill(hr);
}
@end

// Non-interactive crosshair at the center of the window (aim reference for double-click).
// Drawn as a dark outline + bright core so it reads against any background.
@interface ReticleView : NSView
@end
@implementation ReticleView
- (BOOL)isFlipped { return NO; }
- (NSView*)hitTest:(NSPoint)p { (void)p; return nil; } // never steal clicks
- (void)drawRect:(NSRect)r {
    (void)r;
    NSRect b = self.bounds;
    CGFloat cx = b.size.width * 0.5f, cy = b.size.height * 0.5f;
    // Dark outline for contrast on light backgrounds
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.75] set];
    NSRectFill(NSMakeRect(cx - 5.5, cy - 0.75, 11, 1.5));
    NSRectFill(NSMakeRect(cx - 0.75, cy - 5.5, 1.5, 11));
    // Bright core for contrast on dark backgrounds
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.95] set];
    NSRectFill(NSMakeRect(cx - 4.5, cy, 9, 1));
    NSRectFill(NSMakeRect(cx, cy - 4.5, 1, 9));
}
@end

// ============================================================================
// macOS Window + Metal Layer
// ============================================================================

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n { (void)n; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (void)windowWillClose:(NSNotification *)n { (void)n; g_running = false; }
@end

@interface MetalView : NSView
@end

@implementation MetalView
- (CALayer*)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    return layer;
}
- (BOOL)wantsLayer { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent *)event {
    MarkUserInput(g_input);
    if ([event clickCount] >= 2) {
        NSPoint loc = [event locationInWindow];
        g_input.teleportRequested = true;
        g_input.teleportMouseX = (float)loc.x;
        g_input.teleportMouseY = (float)loc.y;
    }
    [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent *)event {
    MarkUserInput(g_input);
    // Accumulate look deltas; UpdateGeoNav consumes them once per frame and
    // drives the double-precision geo camera (free-look or diorama orbit).
    g_input.lookDX += (float)[event deltaX] * 0.005f;
    g_input.lookDY += (float)[event deltaY] * 0.005f;
}

- (void)scrollWheel:(NSEvent *)event {
    MarkUserInput(g_input);
    // Dolly toward/away from the view target (stereo scale follows distance).
    g_input.dollySteps += (float)[event scrollingDeltaY] * 0.05f;
}

- (void)keyDown:(NSEvent *)event {
    MarkUserInput(g_input);
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    // ⌘K — open the API-key panel any time (change / remove / re-enter a key).
    if ((event.modifierFlags & NSEventModifierFlagCommand) && (ch == 'k' || ch == 'K')) {
        ShowKeyCard();
        return;
    }
    switch (ch) {
        case 'w': case 'W': g_input.keyW = true; break;
        case 'a': case 'A': g_input.keyA = true; break;
        case 's': case 'S': g_input.keyS = true; break;
        case 'd': case 'D': g_input.keyD = true; break;
        case 'e': case 'E': g_input.keyE = true; break;
        case 'q': case 'Q': g_input.keyQ = true; break;
        case 'v': case 'V':
            // Cycle through all rendering modes
            if (g_input.renderingModeCount > 0) {
                g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
            }
            g_input.renderingModeChangeRequested = true;
            break;
        case 'm': case 'M':
            g_input.animateToggleRequested = true;
            break;
        case 'b': case 'B':   // cycle city bookmarks (Paris default)
            g_input.cycleBookmarkRequested = true;
            break;
        case 'c': case 'C':   // back to camera-centric fly mode, continuously
            g_input.releaseToFlyRequested = true;
            break;
        case 'i': case 'I':
            g_input.captureAtlasRequested = true;
            break;
        case 't': case 'T':
            g_input.eyeTrackingModeToggleRequested = true;
            break;
        case '-': case '_': {
            // Edit steadyIpdFactor (the ModeSwitch ramp target); seed ipdFactor
            // in lockstep for the idle/non-ramp render path.
            float v = g_input.viewParams.steadyIpdFactor - 0.1f;
            if (v < 0.1f) v = 0.1f;
            g_input.viewParams.steadyIpdFactor = v;
            g_input.viewParams.ipdFactor = v;
            g_input.viewParams.parallaxFactor = v;
            break;
        }
        case '=': case '+': {
            float v = g_input.viewParams.steadyIpdFactor + 0.1f;
            if (v > 1.0f) v = 1.0f;
            g_input.viewParams.steadyIpdFactor = v;
            g_input.viewParams.ipdFactor = v;
            g_input.viewParams.parallaxFactor = v;
            break;
        }
        case ' ':
            g_input.resetViewRequested = true;
            break;
        case '0':
            g_input.currentRenderingMode = 0;
            g_input.renderingModeChangeRequested = true;
            break;
        case '1':
            if (g_input.renderingModeCount > 1) g_input.currentRenderingMode = 1;
            g_input.renderingModeChangeRequested = true;
            break;
        case '2':
            if (g_input.renderingModeCount > 2) g_input.currentRenderingMode = 2;
            g_input.renderingModeChangeRequested = true;
            break;
        case '3':
            if (g_input.renderingModeCount > 3) g_input.currentRenderingMode = 3;
            g_input.renderingModeChangeRequested = true;
            break;
        case '\t':
            g_input.hudVisible = !g_input.hudVisible;
            break;
        case 27: // ESC: first press releases an acquired orbit, second quits
            if (g_geoNav.orbitAcquired) {
                g_input.releaseOrbitRequested = true;
            } else {
                g_running = false;
            }
            break;
    }
}

- (void)keyUp:(NSEvent *)event {
    MarkUserInput(g_input);
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    switch (ch) {
        case 'w': case 'W': g_input.keyW = false; break;
        case 'a': case 'A': g_input.keyA = false; break;
        case 's': case 'S': g_input.keyS = false; break;
        case 'd': case 'D': g_input.keyD = false; break;
        case 'e': case 'E': g_input.keyE = false; break;
        case 'q': case 'Q': g_input.keyQ = false; break;
    }
}

- (void)flagsChanged:(NSEvent *)event {
    // Cmd+Ctrl+F = fullscreen toggle
    NSUInteger flags = [event modifierFlags];
    (void)flags;
}
@end

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

static bool CreateMacOSWindow(uint32_t width, uint32_t height) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Minimal menu bar with a standard Edit menu — WITHOUT it a bare AppKit app
    // doesn't route ⌘X/⌘C/⌘V/⌘A to the focused text field's editor, so paste
    // into the API-key card beeps. The selectors dispatch through the responder
    // chain to the field editor.
    {
        NSMenu *mainMenu = [[NSMenu alloc] init];
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [mainMenu addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit EarthView" action:@selector(terminate:)
                    keyEquivalent:@"q"];
        [appItem setSubmenu:appMenu];

        NSMenuItem *editItem = [[NSMenuItem alloc] init];
        [mainMenu addItem:editItem];
        NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
        [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        [[editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:)
                      keyEquivalent:@"a"] setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
        [editItem setSubmenu:editMenu];
        [NSApp setMainMenu:mainMenu];
    }

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    NSRect frame = NSMakeRect(100, 100, width, height);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
    g_window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:style backing:NSBackingStoreBuffered defer:NO];
    [g_window setTitle:@"DisplayXR EarthView"];
    [g_window setDelegate:delegate];
    [g_window center];

    g_metalView = [[MetalView alloc] initWithFrame:frame];
    [g_window setContentView:g_metalView];
    [g_window makeKeyAndOrderFront:nil];
    [g_window makeFirstResponder:g_metalView];

    // Add HUD overlay — frosted backdrop + text view inside
    NSRect hudFrame = NSMakeRect(8, 8, 320, 520);
    g_hudBackdrop = [[NSVisualEffectView alloc] initWithFrame:hudFrame];
    [g_hudBackdrop setMaterial:NSVisualEffectMaterialHUDWindow];
    [g_hudBackdrop setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
    [g_hudBackdrop setState:NSVisualEffectStateActive];
    [g_hudBackdrop setWantsLayer:YES];
    g_hudBackdrop.layer.cornerRadius = 8.0;
    g_hudBackdrop.layer.masksToBounds = YES;
    [g_metalView addSubview:g_hudBackdrop];

    g_hudView = [[HudOverlayView alloc] initWithFrame:g_hudBackdrop.bounds];
    [g_hudView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [g_hudBackdrop addSubview:g_hudView];

    // --- Top bar (Open / Mode) — transparent so the buttons sit directly
    // over the rendered content (no frosted panel hiding the top of the scene).
    const CGFloat barH = 48.0;
    NSRect barFrame = NSMakeRect(0, height - barH, width, barH);
    NSView *topBar = [[NSView alloc] initWithFrame:barFrame];
    [topBar setWantsLayer:YES];
    [[topBar layer] setBackgroundColor:[[NSColor clearColor] CGColor]];
    [topBar setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    g_topBar = topBar;
    [g_metalView addSubview:g_topBar];

    const CGFloat btnH = 32.0, btnY = (barH - btnH) * 0.5f;
    const CGFloat gap = 10.0;
    CGFloat x = 12.0;
    CGFloat openW = 96.0, modeW = 220.0;

    // Helper: wrap a button in a glassy NSVisualEffectView backdrop matching
    // the HUD's HUDWindow material so the controls have the same look.
    NSView * (^makeGlassyButton)(NSRect, NSString*, SEL, NSButton **) =
        ^NSView *(NSRect frame, NSString *title, SEL act, NSButton **outBtn) {
        NSVisualEffectView *bd = [[NSVisualEffectView alloc] initWithFrame:frame];
        [bd setMaterial:NSVisualEffectMaterialHUDWindow];
        [bd setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
        [bd setState:NSVisualEffectStateActive];
        [bd setWantsLayer:YES];
        bd.layer.cornerRadius = 6.0;
        bd.layer.masksToBounds = YES;
        NSButton *b = [[NSButton alloc] initWithFrame:bd.bounds];
        [b setTitle:title];
        [b setBezelStyle:NSBezelStyleInline];
        [b setBordered:NO];
        [b setTarget:nil];
        [b setAction:act];
        [b setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [bd addSubview:b];
        if (outBtn) *outBtn = b;
        return bd;
    };

    (void)openW; (void)gap;
    NSView *modeBd = makeGlassyButton(NSMakeRect(x, btnY, modeW, btnH),
                                       @"Mode: —", @selector(modeButtonClicked:),
                                       &g_modeButton);
    [g_topBar addSubview:modeBd];
    x += modeW + gap;

    // Bookmark button — cycles the city table ('B' key equivalent).
    const CGFloat bmW = 150.0;
    NSView *bmBd = makeGlassyButton(NSMakeRect(x, btnY, bmW, btnH),
                                     @"City: Paris", @selector(bookmarkButtonClicked:),
                                     &g_animButton);
    g_animButtonBackdrop = bmBd;
    [g_topBar addSubview:bmBd];

    // --- Attribution strip (ALWAYS visible — Map Tiles API policy, PRD §7.3):
    // Google logo + frequency-sorted data credits + loading indicator.
    const CGFloat attrH = 26.0;
    NSVisualEffectView *attrBd =
        [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, width, attrH)];
    [attrBd setMaterial:NSVisualEffectMaterialHUDWindow];
    [attrBd setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
    [attrBd setState:NSVisualEffectStateActive];
    [attrBd setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    g_attributionBar = attrBd;

    NSImage *logo = nil;
    {
        // google_logo.png is copied next to the exe by CMake (policy asset).
        NSString *exePath = [[NSBundle mainBundle] executablePath];
        NSString *logoPath = [[exePath stringByDeletingLastPathComponent]
            stringByAppendingPathComponent:@"google_logo.png"];
        logo = [[NSImage alloc] initWithContentsOfFile:logoPath];
    }
    CGFloat textX = 8.0;
    if (logo) {
        const CGFloat logoH = attrH - 8.0;
        CGFloat logoW = logoH * (logo.size.height > 0 ? logo.size.width / logo.size.height : 3.0);
        NSImageView *logoView =
            [[NSImageView alloc] initWithFrame:NSMakeRect(8, 4, logoW, logoH)];
        [logoView setImage:logo];
        [logoView setImageScaling:NSImageScaleProportionallyUpOrDown];
        [attrBd addSubview:logoView];
        textX = 8.0 + logoW + 10.0;
    }
    NSTextField *attrText = [[NSTextField alloc]
        initWithFrame:NSMakeRect(textX, 0, width - textX - 8.0, attrH)];
    [attrText setEditable:NO];
    [attrText setBordered:NO];
    [attrText setDrawsBackground:NO];
    [attrText setFont:[NSFont systemFontOfSize:10]];
    [attrText setTextColor:[NSColor labelColor]];
    [attrText setLineBreakMode:NSLineBreakByTruncatingTail];
    [attrText setAutoresizingMask:NSViewWidthSizable];
    // Vertically center single-line text in the strip.
    [attrText setFrame:NSMakeRect(textX, (attrH - 14.0) * 0.5, width - textX - 8.0, 14.0)];
    [attrText setStringValue:logo ? @"" : @"Google"];
    g_attributionText = attrText;
    [attrBd addSubview:attrText];
    [g_metalView addSubview:attrBd];

    // --- Reticle (non-interactive center crosshair) ---
    const CGFloat retSize = 20.0;
    NSRect retFrame = NSMakeRect((width - retSize) * 0.5f, (height - retSize) * 0.5f, retSize, retSize);
    g_reticleView = [[ReticleView alloc] initWithFrame:retFrame];
    [g_reticleView setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin];
    [g_metalView addSubview:g_reticleView];

    // --- First-run API-key entry card (centered; hidden until keyless) ---
    {
        const CGFloat cw = 460.0, ch = 240.0;
        NSVisualEffectView *card = [[NSVisualEffectView alloc]
            initWithFrame:NSMakeRect((width - cw) * 0.5, (height - ch) * 0.5, cw, ch)];
        [card setMaterial:NSVisualEffectMaterialHUDWindow];
        [card setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
        [card setState:NSVisualEffectStateActive];
        [card setWantsLayer:YES];
        card.layer.cornerRadius = 12.0;
        card.layer.masksToBounds = YES;
        [card setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin |
                                  NSViewMinYMargin | NSViewMaxYMargin];
        [card setHidden:YES];
        g_keyCard = card;

        NSTextField *title = [NSTextField labelWithString:@"Google Map Tiles API key required"];
        [title setFont:[NSFont boldSystemFontOfSize:15]];
        [title setFrame:NSMakeRect(20, ch - 44, cw - 40, 22)];
        [card addSubview:title];

        NSTextField *body = [NSTextField wrappingLabelWithString:
            @"EarthView streams Google Photorealistic 3D Tiles, which needs your own "
             "Map Tiles API key. Paste it below, or get one from the Google Cloud "
             "Console (enable the “Map Tiles API”, then create an API key)."];
        [body setFont:[NSFont systemFontOfSize:11]];
        [body setTextColor:[NSColor secondaryLabelColor]];
        [body setSelectable:NO];
        [body setFrame:NSMakeRect(20, ch - 110, cw - 40, 58)];
        [card addSubview:body];

        NSTextField *field = [[NSTextField alloc]
            initWithFrame:NSMakeRect(20, ch - 146, cw - 40, 24)];
        [field setPlaceholderString:@"AIza…  (paste your Map Tiles API key)"];
        [field setFont:[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular]];
        [[field cell] setUsesSingleLineMode:YES];
        [field setEditable:YES];
        [field setSelectable:YES];
        [field setBezeled:YES];
        [field setRefusesFirstResponder:NO];
        [field setTarget:nil];
        [field setAction:@selector(keySaveClicked:)]; // Enter submits
        g_keyField = field;
        [card addSubview:field];

        NSTextField *status = [NSTextField labelWithString:@""];
        [status setFont:[NSFont systemFontOfSize:10]];
        [status setTextColor:[NSColor systemRedColor]];
        [status setFrame:NSMakeRect(20, ch - 168, cw - 40, 16)];
        g_keyStatus = status;
        [card addSubview:status];

        const CGFloat bh = 30.0, by = 20.0;
        NSButton *getBtn = [NSButton buttonWithTitle:@"Get a Key…"
            target:nil action:@selector(keyGetClicked:)];
        [getBtn setFrame:NSMakeRect(18, by, 92, bh)];
        [card addSubview:getBtn];

        // Remove the SAVED key so nothing persists to next launch ("clean box").
        NSButton *rmBtn = [NSButton buttonWithTitle:@"Remove key"
            target:nil action:@selector(keyRemoveClicked:)];
        [rmBtn setFrame:NSMakeRect(112, by, 96, bh)];
        [card addSubview:rmBtn];

        NSButton *skipBtn = [NSButton buttonWithTitle:@"Close"
            target:nil action:@selector(keySkipClicked:)];
        [skipBtn setFrame:NSMakeRect(212, by, 62, bh)];
        [card addSubview:skipBtn];

        NSButton *saveBtn = [NSButton buttonWithTitle:@"Save & Start"
            target:nil action:@selector(keySaveClicked:)];
        [saveBtn setFrame:NSMakeRect(cw - 132, by, 114, bh)];
        [saveBtn setKeyEquivalent:@"\r"]; // default button
        [card addSubview:saveBtn];

        [g_metalView addSubview:card];
    }

    [NSApp activateIgnoringOtherApps:YES];
    LOG_INFO("macOS window created (%ux%u)", width, height);
    return true;
}

// Button action handlers (added as category on NSApplication)
@interface NSApplication (TopBarActions)
- (void)modeButtonClicked:(id)sender;
- (void)bookmarkButtonClicked:(id)sender;
- (void)keySaveClicked:(id)sender;
- (void)keyGetClicked:(id)sender;
- (void)keySkipClicked:(id)sender;
- (void)keyRemoveClicked:(id)sender;
@end

@implementation NSApplication (TopBarActions)
- (void)modeButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    if (g_input.renderingModeCount > 0) {
        g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
    }
    g_input.renderingModeChangeRequested = true;
}
- (void)bookmarkButtonClicked:(id)sender {
    (void)sender;
    MarkUserInput(g_input);
    g_input.cycleBookmarkRequested = true;   // 'B'-key equivalent
}
// Save & Start: hand the pasted key to the frame loop, which persists it and
// late-inits the tile engine on the render/updateView thread (see main()).
- (void)keySaveClicked:(id)sender {
    (void)sender;
    std::string key([[g_keyField stringValue] UTF8String] ?: "");
    // trim
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t' ||
                            key.back() == '\r' || key.back() == '\n')) key.pop_back();
    size_t b = key.find_first_not_of(" \t");
    if (b != std::string::npos) key = key.substr(b);
    if (key.empty()) {
        [g_keyStatus setStringValue:@"Paste a key first."];
        return;
    }
    [g_keyStatus setTextColor:[NSColor secondaryLabelColor]];
    [g_keyStatus setStringValue:@"Starting…"];
    g_pendingKey = key;
    g_keySubmitRequested = true;
}
- (void)keyGetClicked:(id)sender {
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:kMapTilesConsoleURL]]];
}
- (void)keySkipClicked:(id)sender {
    (void)sender;
    [g_keyCard setHidden:YES];   // stay on the placeholder / keep streaming
}
// Remove the saved key so it won't persist to the next launch ("clean box").
// The current session keeps using whatever key is already live.
- (void)keyRemoveClicked:(id)sender {
    (void)sender;
    bool ok = earthviewClearApiKey();
    [g_keyField setStringValue:@""];
    [g_keyStatus setTextColor:[NSColor secondaryLabelColor]];
    [g_keyStatus setStringValue:ok
        ? @"Saved key removed — it won't persist to the next launch."
        : @"Could not remove the saved key file."];
    [g_window makeFirstResponder:g_keyField];
    LOG_INFO("Saved API key removed (%s)", ok ? "ok" : "failed");
}
@end

// NOTE: UpdateTopBarButtonTitles() body lives after the AppXrSession struct
// definition further below, since it accesses its members.

static void PumpMacOSEvents() {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

static void ToggleBorderlessFullscreen() {
    if (g_fullscreen) {
        [g_window setStyleMask:g_savedWindowStyle];
        [g_window setFrame:g_savedWindowFrame display:YES animate:NO];
        [g_window setLevel:NSNormalWindowLevel];
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen");
    } else {
        g_savedWindowStyle = [g_window styleMask];
        g_savedWindowFrame = [g_window frame];
        NSScreen *screen = [g_window screen] ?: [NSScreen mainScreen];
        [g_window setStyleMask:NSWindowStyleMaskBorderless];
        [g_window setFrame:[screen frame] display:YES animate:NO];
        [g_window setLevel:NSStatusWindowLevel];
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen");
    }
}

// ============================================================================
// OpenXR Session (ported from cube_handle_vk_macos)
// ============================================================================

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool sessionRunning = false;
    bool exitRequested = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    char systemName[256] = {};

    // Swapchain
    struct { XrSwapchain swapchain; uint32_t width, height, imageCount; int64_t format; } swapchain = {};

    // Display info from XR_EXT_display_info
    bool hasDisplayInfoExt = false;
    bool hasCocoaWindowBinding = false;
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerX = 0, nominalViewerY = 0, nominalViewerZ = 0.5f;
    float recommendedViewScaleX = 0.5f, recommendedViewScaleY = 1.0f;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
    // XrDisplayDesktopPositionEXT (display_info v16, runtime#715): panel
    // top-left in top-down global pixels (origin = primary top-left).
    // (0,0) = primary/unknown — the safe default an old runtime yields.
    int32_t displayScreenLeft = 0, displayScreenTop = 0;

    // Eye tracking
    float eyePositions[8][3] = {};  // [view][x,y,z] — raw per-eye positions in display space
    bool eyeTrackingActive = false;
    bool isEyeTracking = false;
    uint32_t activeEyeTrackingMode = 0;
    uint32_t supportedEyeTrackingModes = 0;

    // Function pointers
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT = nullptr;
    PFN_xrRequestEyeTrackingModeEXT pfnRequestEyeTrackingModeEXT = nullptr;
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT = nullptr;

    // XR_EXT_atlas_capture (W6 of #396): runtime-owned 'I' key capture.
    bool hasAtlasCaptureExt = false;
    PFN_xrCaptureAtlasEXT pfnCaptureAtlasEXT = nullptr;

    // XR_EXT_view_rig (W7 of #396): runtime owns the off-axis Kooima and returns
    // render-ready XrView{pose, fov}; the app deletes its own.
    bool hasViewRigExt = false;

    // XR_EXT_mcp_tools (#22): app-defined agent tools on the runtime-hosted
    // per-process MCP server. The whole path is inert when the extension or
    // the MCP capability gate is absent — never load-bearing.
    bool hasMcpToolsExt = false;
    PFN_xrSetMCPAppInfoEXT pfnSetMCPAppInfo = nullptr;
    PFN_xrRegisterMCPToolEXT pfnRegisterMCPTool = nullptr;
    PFN_xrUnregisterMCPToolEXT pfnUnregisterMCPTool = nullptr;
    PFN_xrGetMCPToolCallArgsEXT pfnGetMCPToolCallArgs = nullptr;
    PFN_xrSubmitMCPToolResultEXT pfnSubmitMCPToolResult = nullptr;
    bool mcpToolsReady = false;           // appId declared + base tools registered
    bool mcpAnimToolsRegistered = false;  // list/play/stop_animation currently live

    // Enumerated rendering mode info
    uint32_t renderingModeCount = 0;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE] = {};
    uint32_t renderingModeViewCounts[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};
    uint32_t renderingModeTileColumns[8] = {};  // atlas tile layout (v12)
    uint32_t renderingModeTileRows[8] = {};

    // Max views the runtime may return from xrLocateViews, taken from
    // xrEnumerateViewConfigurationViews at session init. Some runtimes (e.g.
    // sim_display on macOS) report the union across all rendering modes, so
    // this is >= 2 even for PRIMARY_STEREO.
    uint32_t maxViewCount = 2;

    void* windowHandle = nullptr;  // unused on macOS, kept for compatibility
};

// Session reachable from xr-free helpers (MCP tool dispatch). Set in main()
// right after CreateSession.
static AppXrSession* g_xrForMcp = nullptr;

static void UpdateTopBarButtonTitles(AppXrSession& xr) {
    if (g_modeButton) {
        const char *name = "Unknown";
        if (xr.renderingModeCount > 0 &&
            g_input.currentRenderingMode < xr.renderingModeCount &&
            xr.renderingModeNames[g_input.currentRenderingMode][0] != '\0') {
            name = xr.renderingModeNames[g_input.currentRenderingMode];
        }
        [g_modeButton setTitle:[NSString stringWithFormat:@"Mode: %s", name]];
    }
}

// Refresh the bookmark button label with the active city.
static void UpdateBookmarkButton() {
    if (!g_animButton) return;
    size_t n = 0;
    const geo::Bookmark* bm = geo::bookmarks(&n);
    [g_animButton setTitle:[NSString stringWithFormat:@"City: %s",
                            bm[g_geoNav.bookmarkIndex].name]];
}

// Forward declarations for OpenXR functions (same as cube_handle_vk_macos)
static bool InitializeOpenXR(AppXrSession& xr);
static bool GetVulkanGraphicsRequirements(AppXrSession& xr);
static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance);
static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);
static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice);
static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);
static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& exts, VkDevice& device, VkQueue& queue);
static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex);
static bool CreateSpaces(AppXrSession& xr);
static bool CreateSwapchains(AppXrSession& xr);
static void PollEvents(AppXrSession& xr);
static bool BeginFrame(AppXrSession& xr, XrFrameState& frameState);
static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex);
static void ReleaseSwapchainImage(AppXrSession& xr);
static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount);
static void CleanupOpenXR(AppXrSession& xr);

// ============================================================================
// OpenXR implementation (abbreviated — same logic as cube_handle_vk_macos)
// ============================================================================

static bool InitializeOpenXR(AppXrSession& xr) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& ext : exts) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0) xr.hasCocoaWindowBinding = true;
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
        if (strcmp(ext.extensionName, XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME) == 0) xr.hasAtlasCaptureExt = true;
        if (strcmp(ext.extensionName, XR_EXT_MCP_TOOLS_EXTENSION_NAME) == 0) xr.hasMcpToolsExt = true;
        if (strcmp(ext.extensionName, XR_EXT_VIEW_RIG_EXTENSION_NAME) == 0) xr.hasViewRigExt = true;
    }

    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasCocoaWindowBinding) enabled.push_back(XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
    if (xr.hasAtlasCaptureExt) enabled.push_back(XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME);
    if (xr.hasMcpToolsExt) enabled.push_back(XR_EXT_MCP_TOOLS_EXTENSION_NAME);
    if (xr.hasViewRigExt) enabled.push_back(XR_EXT_VIEW_RIG_EXTENSION_NAME);
    LOG_INFO("XR_EXT_view_rig: %s", xr.hasViewRigExt ? "AVAILABLE" : "NOT FOUND");

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "DisplayXRModelViewerMacOS", sizeof(ci.applicationInfo.applicationName));
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "None", sizeof(ci.applicationInfo.engineName));
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &xr.instance));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &si, &xr.systemId));

    { XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
      xrGetSystemProperties(xr.instance, xr.systemId, &sp);
      memcpy(xr.systemName, sp.systemName, sizeof(xr.systemName)); }

    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
        XrEyeTrackingModeCapabilitiesEXT ec = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT};
        // INV-1.3: panel desktop position (display_info v16, runtime#715).
        // Zero-initialized so an old runtime that ignores the unknown chain
        // entry yields (0,0) = primary/unknown — the safe default.
        XrDisplayDesktopPositionEXT desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_EXT;
        desktopPos.next = &ec;
        di.next = &desktopPos; sp.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
            xr.recommendedViewScaleX = di.recommendedViewScaleX;
            xr.recommendedViewScaleY = di.recommendedViewScaleY;
            xr.displayWidthM = di.displaySizeMeters.width;
            xr.displayHeightM = di.displaySizeMeters.height;
            xr.nominalViewerX = di.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = di.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = di.displayPixelWidth;
            xr.displayPixelHeight = di.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)ec.supportedModes;
            xr.displayScreenLeft = desktopPos.left;
            xr.displayScreenTop = desktopPos.top;
            LOG_INFO("Display desktop position: (%d, %d)", xr.displayScreenLeft, xr.displayScreenTop);
        }
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
        if (xr.supportedEyeTrackingModes != 0)
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);

        // Load unified rendering mode function pointers (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT", (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    // XR_EXT_atlas_capture (W6 of #396): resolve the runtime-owned capture entry.
    if (xr.hasAtlasCaptureExt) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasEXT", (PFN_xrVoidFunction*)&xr.pfnCaptureAtlasEXT);
        LOG_INFO("xrCaptureAtlasEXT: %s", xr.pfnCaptureAtlasEXT ? "resolved" : "NULL");
    }

    // XR_EXT_mcp_tools (#22): resolve the agent-tool entry points. Tools are
    // registered after session create (CreateSession) and dispatched from
    // PollEvents.
    if (xr.hasMcpToolsExt) {
        xrGetInstanceProcAddr(xr.instance, "xrSetMCPAppInfoEXT", (PFN_xrVoidFunction*)&xr.pfnSetMCPAppInfo);
        xrGetInstanceProcAddr(xr.instance, "xrRegisterMCPToolEXT", (PFN_xrVoidFunction*)&xr.pfnRegisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrUnregisterMCPToolEXT", (PFN_xrVoidFunction*)&xr.pfnUnregisterMCPTool);
        xrGetInstanceProcAddr(xr.instance, "xrGetMCPToolCallArgsEXT", (PFN_xrVoidFunction*)&xr.pfnGetMCPToolCallArgs);
        xrGetInstanceProcAddr(xr.instance, "xrSubmitMCPToolResultEXT", (PFN_xrVoidFunction*)&xr.pfnSubmitMCPToolResult);
        const bool resolved = xr.pfnSetMCPAppInfo && xr.pfnRegisterMCPTool &&
            xr.pfnUnregisterMCPTool && xr.pfnGetMCPToolCallArgs && xr.pfnSubmitMCPToolResult;
        LOG_INFO("XR_EXT_mcp_tools entry points: %s", resolved ? "resolved" : "NULL");
    } else {
        LOG_INFO("XR_EXT_mcp_tools: not advertised by runtime");
    }

    LOG_INFO("OpenXR initialized: %s", xr.systemName);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    return XR_SUCCEEDED(fn(xr.instance, xr.systemId, &req));
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    std::vector<std::string> extNames;
    std::vector<const char*> extPtrs;
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') extNames.push_back(n);
        s = e + 1;
    }}
    // Add portability enumeration for MoltenVK
    extNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    for (auto& n : extNames) extPtrs.push_back(n.c_str());

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "DisplayXRModelViewerMacOS";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)extPtrs.size();
    ci.ppEnabledExtensionNames = extPtrs.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& pd) {
    PFN_xrGetVulkanGraphicsDeviceKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XR_CHECK(fn(xr.instance, xr.systemId, vkInstance, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    LOG_INFO("GPU: %s", props.deviceName);
    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice) {
    PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') storage.push_back(n);
        s = e + 1;
    }}
    // Add portability subset for MoltenVK
    storage.push_back("VK_KHR_portability_subset");
    for (auto& n : storage) exts.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t& idx) {
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
    for (uint32_t i = 0; i < count; i++) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = i; return true; }
    }
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice pd, uint32_t qfi,
    const std::vector<const char*>& exts, VkDevice& dev, VkQueue& queue) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = qfi; qi.queueCount = 1; qi.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures features = {};
    features.shaderInt64 = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    // Anisotropic texture filtering for the tile renderer (oblique terrain
    // anti-aliasing). Only enable if the device advertises it.
    {
        VkPhysicalDeviceFeatures supported = {};
        vkGetPhysicalDeviceFeatures(pd, &supported);
        features.samplerAnisotropy = supported.samplerAnisotropy;
    }

    VkDeviceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(pd, &ci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice pd,
    VkDevice dev, uint32_t qfi) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = pd;
    vkBinding.device = dev;
    vkBinding.queueFamilyIndex = qfi;
    vkBinding.queueIndex = 0;

    XrCocoaWindowBindingCreateInfoEXT macBinding = {(XrStructureType)XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT};
    macBinding.viewHandle = (__bridge void*)g_metalView;
    if (xr.hasCocoaWindowBinding && g_metalView) {
        vkBinding.next = &macBinding;
        LOG_INFO("Using XR_EXT_cocoa_window_binding");
    }

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = &vkBinding; si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));

    // XR_EXT_mcp_tools (#22): declare identity + register the base agent
    // tools. The appId MUST match `id` in
    // displayxr/earthview_handle_vk_macos.displayxr.json (INV-10.1).
    // Failure is non-fatal by design — the MCP capability gate may simply be
    // off on this machine; EarthView runs identically without an agent surface.
    if (xr.hasMcpToolsExt && xr.pfnSetMCPAppInfo && xr.pfnRegisterMCPTool) {
        XrMCPAppInfoEXT mcpAppInfo = {XR_TYPE_MCP_APP_INFO_EXT};
        strncpy(mcpAppInfo.appId, "earthview", sizeof(mcpAppInfo.appId) - 1);
        XrResult ar = xr.pfnSetMCPAppInfo(xr.session, &mcpAppInfo);
        if (XR_SUCCEEDED(ar)) {
            XrMCPToolInfoEXT statusTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
            statusTool.name = "get_status";
            statusTool.description =
                "Read EarthView's live state: active city bookmark, whether an "
                "orbit center is acquired (diorama mode), camera target distance "
                "in meters, render-tile count + GPU-resident MB, active "
                "rendering-mode index, and whether the XR session is running.";
            statusTool.inputSchemaJson = "{\"type\":\"object\"}";
            XrResult t1 = xr.pfnRegisterMCPTool(xr.session, &statusTool);

            XrMCPToolInfoEXT bookmarkTool = {XR_TYPE_MCP_TOOL_INFO_EXT};
            bookmarkTool.name = "set_bookmark";
            bookmarkTool.description =
                "Fly to a city bookmark by 'index' or 'name' (Paris, San "
                "Francisco, New York, Tokyo, Sydney). Omit both to cycle to the "
                "next city. Releases any acquired orbit center. Verify visually "
                "with capture_frame.";
            bookmarkTool.inputSchemaJson =
                "{\"type\":\"object\",\"properties\":{"
                "\"index\":{\"type\":\"integer\",\"description\":\"Bookmark index, 0-based.\"},"
                "\"name\":{\"type\":\"string\",\"description\":\"Bookmark city name.\"}}}";
            XrResult t2 = xr.pfnRegisterMCPTool(xr.session, &bookmarkTool);

            xr.mcpToolsReady = true;
            LOG_INFO("XR_EXT_mcp_tools: appId=earthview get_status=%d set_bookmark=%d",
                     t1, t2);
        } else {
            LOG_INFO("XR_EXT_mcp_tools: appId not accepted (%d) — no agent surface", ar);
        }
    }

    // Enumerate available rendering modes and store names
    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoEXT> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
                modes[i].next = nullptr;
            }
            enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    strncpy(xr.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    xr.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns ? modes[i].tileColumns : 1;
                    xr.renderingModeTileRows[i] = modes[i].tileRows ? modes[i].tileRows : 1;
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

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &ci, &xr.localSpace));

    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, views.data());
    xr.maxViewCount = viewCount;
    LOG_INFO("View config: %u views reported by runtime", viewCount);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(xr.session, fmtCount, &fmtCount, fmts.data());

    int64_t selectedFmt = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0];
    for (auto f : fmts) {
        if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) { selectedFmt = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) selectedFmt = f;
    }

    // Size the swapchain at init from the largest atlas any rendering mode
    // could produce when the app is running full-screen — atlas dims per
    // mode are (cols × scaleX × displayPixelW) × (rows × scaleY × displayPixelH).
    // For sim_display and Leia SR this collapses to the panel resolution
    // (max(cols × scaleX) ≤ 1 across all their advertised modes). The atlas
    // the app actually writes per frame is smaller — driven by the live
    // window size — but the swapchain has to accommodate full-screen so the
    // app can resize / fullscreen at any time without reallocating. Falls
    // back to recommended × (2,1) if display info is unavailable.
    uint32_t w = views[0].recommendedImageRectWidth * 2;
    uint32_t h = views[0].recommendedImageRectHeight;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        w = xr.displayPixelWidth;
        h = xr.displayPixelHeight;
        if (xr.renderingModeCount > 0) {
            uint32_t maxAtlasW = 0, maxAtlasH = 0;
            for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                uint32_t aw = (uint32_t)((double)xr.renderingModeTileColumns[i] *
                                          xr.renderingModeScaleX[i] *
                                          (double)xr.displayPixelWidth);
                uint32_t ah = (uint32_t)((double)xr.renderingModeTileRows[i] *
                                          xr.renderingModeScaleY[i] *
                                          (double)xr.displayPixelHeight);
                if (aw > maxAtlasW) maxAtlasW = aw;
                if (ah > maxAtlasH) maxAtlasH = ah;
            }
            if (maxAtlasW > w) w = maxAtlasW;
            if (maxAtlasH > h) h = maxAtlasH;
        }
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selectedFmt;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
    xr.swapchain.width = w; xr.swapchain.height = h; xr.swapchain.format = selectedFmt;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
    xr.swapchain.imageCount = imgCount;

    LOG_INFO("Swapchain: %ux%u, %u images, format=%lld", w, h, imgCount, selectedFmt);
    return true;
}

// ============================================================================
// XR_EXT_mcp_tools dispatch (#22)
// ============================================================================
// Minimal JSON helpers — hand-rolled on purpose, matching the runtime
// reference adopter (cube_handle_metal_macos): tool args are tiny one-level
// objects, so a JSON dependency isn't warranted.

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Extract "key":"value" (string) with backslash-escape handling, incl. a
// basic \uXXXX → UTF-8 decode (no surrogate pairs — file paths don't need
// them). False when the key is absent or its value is not a string.
static bool JsonGetString(const char* json, const char* key, std::string& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (*c != '"') return false;
    c++;
    out.clear();
    while (*c && *c != '"') {
        if (*c == '\\' && c[1]) {
            c++;
            switch (*c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    int ndig = 0;
                    while (ndig < 4 && c[1]) {
                        char h = c[1];
                        unsigned v;
                        if (h >= '0' && h <= '9') v = (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v = (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v = (unsigned)(h - 'A' + 10);
                        else break;
                        cp = (cp << 4) | v;
                        c++;
                        ndig++;
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += *c; break;  // \" \\ \/
            }
        } else {
            out += *c;
        }
        c++;
    }
    return *c == '"';
}

// Extract "key": <number>. False when absent or not numeric (strtod refuses
// a leading quote, so string values correctly fail).
static bool JsonGetNumber(const char* json, const char* key, double& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    char* end = nullptr;
    double v = strtod(c + 1, &end);
    if (end == c + 1) return false;
    out = v;
    return true;
}

// Dispatch one agent tool call. Runs on the main loop (called from
// PollEvents), where app state is naturally consistent — no locking. EVERY
// call is answered — success=XR_FALSE + {"error":…} for bad args — because an
// unanswered call only fails to the agent after the runtime's ~5 s timeout.
static void HandleMcpToolCall(AppXrSession& xr, const XrEventDataMCPToolCallEXT* call) {
    // Two-call idiom: argsSize from the event is the required capacity incl. NUL.
    std::string args;
    if (xr.pfnGetMCPToolCallArgs && call->argsSize > 0) {
        std::vector<char> buf(call->argsSize, '\0');
        uint32_t needed = 0;
        if (XR_SUCCEEDED(xr.pfnGetMCPToolCallArgs(xr.session, call->callId,
                                                  (uint32_t)buf.size(), &needed, buf.data())))
            args.assign(buf.data());
    }
    const char* a = args.c_str();
    std::string result;
    XrBool32 ok = XR_TRUE;
    char buf[1024];

    size_t bmCount = 0;
    const geo::Bookmark* bm = geo::bookmarks(&bmCount);

    if (strcmp(call->toolName, "get_status") == 0) {
        snprintf(buf, sizeof(buf),
                 "{\"bookmark\":\"%s\",\"bookmark_index\":%d,"
                 "\"orbit_acquired\":%s,\"target_distance_m\":%.1f,"
                 "\"tiles_active\":%s,\"render_tiles\":%s,"
                 "\"gpu_resident_mb\":%.1f,"
                 "\"rendering_mode\":%u,\"session_running\":%s}",
                 bm[g_geoNav.bookmarkIndex].name, g_geoNav.bookmarkIndex,
                 g_geoNav.orbitAcquired ? "true" : "false",
                 g_geoNav.targetDist,
                 g_tilesActive ? "true" : "false",
                 g_tileEngine.hasRenderableContent() ? "true" : "false",
                 g_tileRenderer.gpuResidentMB(),
                 g_input.currentRenderingMode,
                 xr.sessionRunning ? "true" : "false");
        result = buf;
    } else if (strcmp(call->toolName, "set_bookmark") == 0) {
        int target = -1;
        double idx; std::string nm;
        if (JsonGetNumber(a, "index", idx)) {
            target = (int)idx;
            if (target < 0 || target >= (int)bmCount) {
                ok = XR_FALSE;
                snprintf(buf, sizeof(buf), "{\"error\":\"index out of range (0..%d)\"}",
                         (int)bmCount - 1);
                result = buf;
            }
        } else if (JsonGetString(a, "name", nm)) {
            for (int i = 0; i < (int)bmCount && target < 0; i++) {
                if (nm == bm[i].name) target = i;
            }
            if (target < 0) {
                ok = XR_FALSE;
                result = "{\"error\":\"no bookmark named '" + JsonEscape(nm) + "'\"}";
            }
        } else {
            target = (g_geoNav.bookmarkIndex + 1) % (int)bmCount;  // cycle
        }
        if (ok == XR_TRUE) {
            g_geoNav.frameBookmark(target);
            UpdateBookmarkButton();
            MarkUserInput(g_input);  // agent input is input: reset the idle timer
            snprintf(buf, sizeof(buf), "{\"bookmark\":\"%s\",\"index\":%d}",
                     bm[target].name, target);
            result = buf;
        }
    } else {
        ok = XR_FALSE;
        result = "{\"error\":\"unhandled tool\"}";
    }

    if (xr.pfnSubmitMCPToolResult)
        xr.pfnSubmitMCPToolResult(xr.session, call->callId, ok, result.c_str());
}

static void PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* ssc = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = ssc->state;
            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = xr.viewConfigType;
                xrBeginSession(xr.session, &bi);
                xr.sessionRunning = true;
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr.session);
                xr.sessionRunning = false;
            } else if (ssc->state == XR_SESSION_STATE_EXITING) {
                xr.exitRequested = true;
            }
        } else if (event.type == (XrStructureType)XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT) {
            // Runtime (or another client / shell) switched rendering mode on us.
            auto* rmc = (XrEventDataRenderingModeChangedEXT*)&event;
            if (rmc->currentModeIndex < xr.renderingModeCount) {
                g_input.currentRenderingMode = rmc->currentModeIndex;
                g_msLastMode = rmc->currentModeIndex; // keep the ramp's from-mode in sync
                UpdateTopBarButtonTitles(xr);
                LOG_INFO("Rendering mode changed: %u -> %u (%s)",
                    rmc->previousModeIndex, rmc->currentModeIndex,
                    xr.renderingModeNames[rmc->currentModeIndex]);
            }
        } else if (event.type == (XrStructureType)XR_TYPE_EVENT_DATA_MCP_TOOL_CALL_EXT) {
            // An agent invoked one of our XR_EXT_mcp_tools tools (#22).
            HandleMcpToolCall(xr, (const XrEventDataMCPToolCallEXT*)&event);
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(xr.session, nullptr, &fs);
    if (XR_FAILED(r)) return false;
    return XR_SUCCEEDED(xrBeginFrame(xr.session, nullptr));
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &ai, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 1000000000;
    return XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &wi));
}

static void ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(xr.swapchain.swapchain, &ri);
}

static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount) {
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = xr.localSpace;
    layer.viewCount = viewCount;
    layer.views = projViews;
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = 1; ei.layers = layers;
    xrEndFrame(xr.session, &ei);
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.localSpace) xrDestroySpace(xr.localSpace);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.instance) xrDestroyInstance(xr.instance);
}

// ============================================================================
// Placeholder rendering (clear to dark gray when no scene loaded)
// ============================================================================

static void RenderPlaceholder(VkDevice dev, VkQueue queue, VkCommandPool pool,
                               VkImage image, uint32_t w, uint32_t h,
                               float yaw, float pitch) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Tint color based on camera direction so drag-rotation gives visual feedback
    float ny = (yaw / 3.14159f) * 0.5f + 0.5f;   // 0..1 over ±π
    float np = (pitch / 1.5f) * 0.5f + 0.5f;       // 0..1 over ±1.5 rad
    VkClearColorValue cc = {{0.05f + ny * 0.15f, 0.08f + np * 0.12f, 0.15f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

// ============================================================================
// Bundled-scene auto-load
// ============================================================================

static std::string ExeDir() {
    char buf[PATH_MAX]; uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return "";
    char resolved[PATH_MAX];
    if (!realpath(buf, resolved)) return std::string(buf);
    return std::string(dirname(resolved));
}

static bool FileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // EV_PROBE=<key>: validate a Map Tiles API key against Google and exit
    // (0 = valid). Support/diagnostic tool — no window, runtime, or GPU needed.
    if (const char *pk = getenv("EV_PROBE")) {
        std::string err;
        bool ok = g_tileEngine.probeKey(pk, err);
        fprintf(stderr, "EV_PROBE: %s%s%s\n", ok ? "VALID" : "INVALID",
                ok ? "" : " — ", ok ? "" : err.c_str());
        return ok ? 0 : 1;
    }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== DisplayXR 3D Model Viewer (Vulkan) + External macOS Window ===");

    // Initialize rendering mode from env var (legacy fallback)
    {
        const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
        if (mode_str) {
            if (strcmp(mode_str, "anaglyph") == 0) g_input.currentRenderingMode = 1;
            else if (strcmp(mode_str, "sbs") == 0) g_input.currentRenderingMode = 2;
            else if (strcmp(mode_str, "blend") == 0) g_input.currentRenderingMode = 3;
            else g_input.currentRenderingMode = 1; // default to anaglyph
        }
    }

    // Step 1: Create macOS window
    g_windowW = 1280; g_windowH = 720;
    if (!CreateMacOSWindow(g_windowW, g_windowH)) {
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    { NSSize cs = [[g_window contentView] bounds].size;
      CGFloat bs = [g_window backingScaleFactor];
      g_windowW = (uint32_t)(cs.width * bs);
      g_windowH = (uint32_t)(cs.height * bs);
      LOG_INFO("Window drawable: %ux%u", g_windowW, g_windowH); }

    // Step 2: Initialize OpenXR
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }

    // INV-1.3 / runtime#715: open on the 3D panel. The window is created (and
    // centered) before the OpenXR instance, so one-shot move it to the panel's
    // top-left before the session binds it. XrDisplayDesktopPositionEXT is
    // top-down global pixels (origin = primary top-left); AppKit is bottom-up,
    // so flip against the primary screen height. (0,0) = primary/unknown —
    // keep the centered placement, matching an old runtime's behavior.
    if (xr.displayScreenLeft != 0 || xr.displayScreenTop != 0) {
        NSScreen *primary = [NSScreen screens].firstObject;
        if (primary != nil) {
            NSRect wf = [g_window frame];
            CGFloat topY = primary.frame.size.height - (CGFloat)xr.displayScreenTop;
            [g_window setFrameOrigin:NSMakePoint((CGFloat)xr.displayScreenLeft,
                                                 topY - wf.size.height)];
            LOG_INFO("Moved window to 3D panel at (%d, %d)", xr.displayScreenLeft, xr.displayScreenTop);
        }
    }

    // Try to find sim_display_set_output_mode
    { void *rtHandle = NULL;
      uint32_t ic = _dyld_image_count();
      for (uint32_t i = 0; i < ic; i++) {
          const char *name = _dyld_get_image_name(i);
          if (name && strstr(name, "openxr_displayxr")) {
              rtHandle = dlopen(name, RTLD_NOLOAD); break;
          }
      }
      if (rtHandle) g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)dlsym(rtHandle, "sim_display_set_output_mode");
      LOG_INFO("sim_display hot-reload: %s", g_pfnSetOutputMode ? "available" : "not available"); }

    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }

    // Model-load paths can now flip the agent animation-tool registration
    // (XR_EXT_mcp_tools late registration, #22). Set before the bundled-scene
    // auto-load below so it too funnels through UpdateMcpAnimationTools.
    g_xrForMcp = &xr;

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    // Enumerate swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    { uint32_t count = xr.swapchain.imageCount;
      swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    // Initialize the tile renderer + cesium engine. Keyless (no API key) is a
    // supported state: the app stays up on the placeholder and the attribution
    // strip explains how to supply a key (PRD §7.4).
    { uint32_t rw = xr.swapchain.width;   // Full width — mono uses entire swapchain
      uint32_t rh = xr.swapchain.height;
      if (!g_tileRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue, queueFamilyIndex, rw, rh)) {
          LOG_WARN("tile renderer init failed");
      } else {
          g_tilesActive = g_tileEngine.init(&g_tileRenderer);
          if (!g_tilesActive) {
              LOG_WARN("No Google Map Tiles API key — showing the in-app entry card "
                       "(or set GOOGLE_MAPS_API_KEY / earthview.ini)");
              ShowKeyCard();   // first-run key entry (docs/api-key.md)
          }
      }
    }

    // Command pool for placeholder rendering
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      ci.queueFamilyIndex = queueFamilyIndex;
      vkCreateCommandPool(vkDevice, &ci, nullptr, &cmdPool); }

    g_input.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
    g_input.nominalViewerZ = xr.nominalViewerZ;
    g_input.renderingModeCount = xr.renderingModeCount;
    // Align the runtime's active rendering mode with the app's default
    // (currentRenderingMode = 1, the first 3D mode) at startup. The sim display
    // boots in 2D (mode 0); without this the display stays 2D until the user
    // toggles. The main-loop dispatch holds this request until the session is
    // running, so it isn't issued to a not-yet-begun session and lost.
    g_input.renderingModeChangeRequested = true;
    g_input.lastInputTimeSec = NowSec();

    // Reflect initial state in top-bar buttons.
    UpdateTopBarButtonTitles(xr);

    // Frame the default bookmark (Paris / Eiffel Tower).
    g_geoNav.frameBookmark(0);
    UpdateBookmarkButton();

    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Pan  E/Q=Climb  LMB-drag=Look  Scroll=Dolly  DblClick=Orbit-acquire");
    LOG_INFO("          B=City  C=Orbit->Fly  Esc/Space=Release/Reset  -/= Depth  M=Auto-Orbit  V=Mode");
    LOG_INFO("          I=Capture  T=EyeTracking  Cmd+K=API key  Tab=HUD  ESC=Quit");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PumpMacOSEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        g_frameCount++;
        g_avgFrameTime = g_avgFrameTime * 0.95 + deltaTime * 0.05;

        // API-key entry: persist the pasted key and late-init the tile engine
        // HERE (the frame-loop thread == the updateView thread cesium's
        // prepareInMainThread/free require). Save-to-config + env so it sticks
        // across relaunches.
        if (g_keySubmitRequested) {
            g_keySubmitRequested = false;
            // Show progress + force a repaint before the blocking network probe.
            [g_keyStatus setTextColor:[NSColor secondaryLabelColor]];
            [g_keyStatus setStringValue:@"Checking your key with Google…"];
            [g_keyCard display];

            std::string err;
            if (!g_tileEngine.probeKey(g_pendingKey, err)) {
                // Reject — do NOT persist a bad key.
                [g_keyStatus setTextColor:[NSColor systemRedColor]];
                [g_keyStatus setStringValue:[NSString stringWithUTF8String:err.c_str()]];
                LOG_WARN("API key rejected: %s", err.c_str());
            } else {
                // Validated → persist (mode 600) + activate the engine.
                if (!earthviewSaveApiKey(g_pendingKey))
                    LOG_WARN("Could not persist key to %s (using it this session only)",
                             earthviewKeyConfigPath().c_str());
                setenv("GOOGLE_MAPS_API_KEY", g_pendingKey.c_str(), 1);
                g_tilesActive = g_tileEngine.init(&g_tileRenderer);
                if (g_tilesActive) {
                    [g_keyCard setHidden:YES];
                    LOG_INFO("API key validated — streaming started");
                } else {
                    [g_keyStatus setTextColor:[NSColor systemRedColor]];
                    [g_keyStatus setStringValue:@"Key validated but the engine failed to start."];
                }
            }
            g_pendingKey.clear();
        }

        // Handle Auto-Orbit toggle (M key or button)
        if (g_input.animateToggleRequested) {
            g_input.animateToggleRequested = false;
            g_input.animateEnabled = !g_input.animateEnabled;
            g_input.lastInputTimeSec = NowSec(); // don't snap-start
            UpdateTopBarButtonTitles(xr);
        }

        const bool hadBookmarkCycle = g_input.cycleBookmarkRequested;
        UpdateGeoNav(g_input, deltaTime);
        if (hadBookmarkCycle) UpdateBookmarkButton();

        // Handle rendering mode change (V=cycle, 0-3=direct, Mode button, or the
        // startup default-mode request) through the dxr::ModeSwitch sequencer:
        // it eases g_input.viewParams.ipdFactor around the switch and fires the
        // request on the right frame. Held until the session is running so the
        // request reaches a begun session (this handler runs before PollEvents).
        if (!g_modeSwitchConfigured) {
            g_modeSwitch.configure(0.18f, dxr::ModeSwitchEasing::SmoothStep);
            g_modeSwitchConfigured = true;
        }
        {
            const float steady = g_input.viewParams.steadyIpdFactor;
            auto vcOf = [&](uint32_t m) -> uint32_t {
                return (m < xr.renderingModeCount && xr.renderingModeViewCounts[m] > 0)
                           ? xr.renderingModeViewCounts[m] : 1;
            };
            if (g_input.renderingModeChangeRequested && xr.sessionRunning &&
                xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
                g_input.renderingModeChangeRequested = false;
                const uint32_t target = g_input.currentRenderingMode;
                if (target == g_msLastMode) {
                    // Startup / re-assert / same mode: fire directly, no ramp.
                    xr.pfnRequestDisplayRenderingModeEXT(xr.session, target);
                    g_msLastMode = target;
                    UpdateTopBarButtonTitles(xr);
                } else {
                    const float curIpd = g_modeSwitch.active() ? g_modeSwitch.ipd() : steady;
                    g_modeSwitch.request(target, vcOf(target), g_msLastMode, vcOf(g_msLastMode),
                                         curIpd, steady);
                }
            }
            if (g_modeSwitch.active()) {
                float ipd = steady; bool fire = false; uint32_t mode = g_msLastMode;
                g_modeSwitch.update(deltaTime, &ipd, &fire, &mode);
                g_input.viewParams.ipdFactor = ipd;
                if (fire && xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
                    xr.pfnRequestDisplayRenderingModeEXT(xr.session, mode);
                    g_msLastMode = mode;
                    UpdateTopBarButtonTitles(xr);
                }
            } else {
                g_input.viewParams.ipdFactor = steady;
            }
        }

        // Handle eye tracking mode toggle
        if (g_input.eyeTrackingModeToggleRequested) {
            g_input.eyeTrackingModeToggleRequested = false;
            if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                    ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
                xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            }
        }

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                std::vector<XrCompositionLayerProjectionView> projectionViews;
                bool rendered = false;

                if (frameState.shouldRender) {
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    XrViewEyeTrackingStateEXT eyeTrackingState = {};
                    eyeTrackingState.type = (XrStructureType)XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT;
                    viewState.next = &eyeTrackingState;

                    // Clean +Y-up world camera pose (no Y negation — the ModelRenderer
                    // now flips Vulkan-Y via a negative viewport, not a view/world
                    // reflection; see tile_renderer.cpp).
                    // EarthView rig pose is ALWAYS identity: ALL view rotation lives in
                    // the geo world mapping (g_xrFromEcef via g_geoNav), NOT in the rig.
                    // Feeding yaw/pitch here would double-apply rotation (world + rig) and
                    // counter-rotate the content as you drag (the tile-lag bug seen on the
                    // Windows leg). macOS's local InputState keeps yaw/pitch at 0 so this is
                    // already identity, but pin it explicitly to match the model.
                    XrPosef cameraPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
                    const float rigVH =
                        g_input.viewParams.virtualDisplayHeight / g_input.viewParams.scaleFactor;

                    // XR_EXT_view_rig (#396 W7): chain a rig so the runtime owns the
                    // off-axis eyes. FLY (camera-centric) uses the CAMERA rig — a plain
                    // perspective camera the runtime perturbs with eye tracking, converging
                    // at convergenceDiopters; the app never anchors content to the tracked
                    // eye (that caused off-centre / zoom-on-rotate on a real tracked panel,
                    // which sim_display never exposed). ORBIT uses the DISPLAY rig (portal).
                    const bool useRig =
                        xr.hasViewRigExt && xr.displayWidthM > 0 && xr.displayHeightM > 0;
                    const bool naturalRigCamera = useRig && !g_geoNav.orbitAcquired;
                    // Focus mode forces the display rig; the world stays camera-centric.
                    const bool rigCamera = naturalRigCamera && !g_focusActive;
                    XrCameraRigEXT cameraRig = {XR_TYPE_CAMERA_RIG_EXT};
                    XrDisplayRigEXT displayRig = {XR_TYPE_DISPLAY_RIG_EXT};
                    XrViewDisplayRawEXT viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
                    if (useRig) {
                        // physical_height_m MUST be the runtime's CANVAS height (window
                        // client area), NOT the full display height — the cube C-toggle
                        // proved this (canvasH 0.1434 vs dispH 0.1956 = the 1.36x zoom).
                        float canvasH = (g_canvasHM > 1.0e-6f) ? g_canvasHM : xr.displayHeightM;
                        float canvasW = (g_canvasWM > 1.0e-6f) ? g_canvasWM : xr.displayWidthM;
                        dxr_rig_display_info dinfo = {
                            canvasH,
                            (canvasH > 1.0e-6f) ? (canvasW / canvasH) : 1.0f,
                            xr.nominalViewerZ};
                        if (rigCamera) {
                            cameraRig.pose = cameraPose;  // identity
                            cameraRig.ipdFactor = g_input.viewParams.ipdFactor;
                            cameraRig.parallaxFactor = g_input.viewParams.parallaxFactor;
                            cameraRig.convergenceDiopters = g_convDiopters;  // 1/m, auto-focused
                            // Orthoscopic vFOV from the FULL display's physical subtense so the
                            // fly view matches the full-screen panel even windowed (§5).
                            cameraRig.verticalFov = CamVFovRad(xr.displayHeightM, xr.nominalViewerZ);
                            // [FOCUS] on the orbit->fly return, glide ipd/par from the value
                            // that matches the orbit's display rig (ipd=1 <-> cam ipd=1/f,
                            // f=convDiopters*N => seamless at the switch instant) back to the
                            // user's original viewParams. g_stereoFull drops 1->0 over ~0.4 s.
                            if (g_stereoFull > 1.0e-4) {
                                double f = (double)cameraRig.convergenceDiopters * (double)xr.nominalViewerZ;
                                double invF = (f > 1.0e-4) ? (1.0 / f) : 1.0;
                                double sf = g_stereoFull;
                                cameraRig.ipdFactor =
                                    (float)((1.0 - sf) * g_input.viewParams.ipdFactor + sf * invF);
                                cameraRig.parallaxFactor =
                                    (float)((1.0 - sf) * g_input.viewParams.parallaxFactor + sf * invF);
                            }
                            // Frustum-source viewing distance = eye->focus = 1/convergence
                            // (the cam rig's ZDP distance). Drives the focus framing + orbit.
                            g_viewDistXR = (cameraRig.convergenceDiopters > 1.0e-6f)
                                               ? (1.0f / cameraRig.convergenceDiopters) : 0.0f;
                            locateInfo.next = &cameraRig;
                        } else {
                            displayRig.pose = cameraPose;
                            displayRig.virtualDisplayHeight = rigVH;
                            displayRig.ipdFactor = g_input.viewParams.ipdFactor;
                            displayRig.parallaxFactor = g_input.viewParams.parallaxFactor;
                            displayRig.perspectiveFactor = g_input.viewParams.perspectiveFactor;
                            if (g_focusActive) {
                                // Focus: convert the live camera rig -> display rig.
                                dxr_camera_rig crig0 = {};
                                crig0.pose.orientation = {0, 0, 0, 1};
                                crig0.ipd_factor = g_input.viewParams.ipdFactor;
                                crig0.parallax_factor = g_input.viewParams.parallaxFactor;
                                crig0.inv_convergence_distance = g_convDiopters;
                                // Source half-tan-vfov = the live PHYSICAL fly FOV, so the
                                // cam->display conversion reproduces the fly framing exactly =>
                                // disturbance-free switch. (MUST match cameraRig.verticalFov; §5.)
                                crig0.half_tan_vfov = tanf(0.5f * CamVFovRad(xr.displayHeightM, xr.nominalViewerZ));
                                crig0.m2v = 1.0f;
                                dxr_display_rig drig = {};
                                dxr_view_rig_camera_to_display(&crig0, &dinfo, &drig);
                                displayRig.pose.orientation = {drig.pose.orientation.x, drig.pose.orientation.y,
                                                               drig.pose.orientation.z, drig.pose.orientation.w};
                                displayRig.pose.position = {drig.pose.position.x, drig.pose.position.y,
                                                            drig.pose.position.z};
                                displayRig.virtualDisplayHeight = drig.virtual_display_height;
                                displayRig.ipdFactor = drig.ipd_factor;
                                displayRig.parallaxFactor = drig.parallax_factor;
                                displayRig.perspectiveFactor = drig.perspective_factor;
                                // [FOCUS] glide ipd/par from the converter's disturbance-free
                                // value (t=0, seamless switch) to FULL 1.0 (t=1) for full
                                // stereo depth on the inspected feature.
                                if (g_focusActive) {
                                    double sf = g_stereoFull;
                                    displayRig.ipdFactor =
                                        (float)((1.0 - sf) * drig.ipd_factor + sf * 1.0);
                                    displayRig.parallaxFactor =
                                        (float)((1.0 - sf) * drig.parallax_factor + sf * 1.0);
                                }
                            }
                            // [FOCUS] NO rig-pose translation. The POI is placed on the
                            // convergence/zero-parallax plane purely by the targetDist framing
                            // (set at acquire + orbit), so the display eye stays at the origin
                            // (consistent with selCam + the camera-centric world) and the depth
                            // pick stays accurate. Translating the pose moved the eye and made
                            // the pick drift in orbit mode after a few clicks.
                            // Frustum-source viewing distance = eye->display plane = es*N
                            // = persp * m2v * N (m2v = vH/canvasH); matches the cam rig's
                            // 1/convergence. Drives the focus framing + orbit.
                            {
                                float m2v_eff = (canvasH > 1.0e-6f)
                                                    ? (displayRig.virtualDisplayHeight / canvasH) : 0.0f;
                                g_viewDistXR = displayRig.perspectiveFactor * m2v_eff * xr.nominalViewerZ;
                            }
                            locateInfo.next = &displayRig;
                        }
                        eyeTrackingState.next = &viewRigRaw;
                    }

                    uint32_t runtimeViewCount = xr.maxViewCount > 0 ? xr.maxViewCount : 2;
                    if (runtimeViewCount > 8) runtimeViewCount = 8;
                    XrView views[8] = {};
                    for (uint32_t v = 0; v < runtimeViewCount; v++) views[v].type = XR_TYPE_VIEW;

                    XrResult locResult = xrLocateViews(xr.session, &locateInfo, &viewState,
                        runtimeViewCount, &runtimeViewCount, views);
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                        // --- Per-frame mode metadata ---
                        uint32_t modeViewCount = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount)
                            ? xr.renderingModeViewCounts[g_input.currentRenderingMode] : 2u;
                        if (modeViewCount < 1) modeViewCount = 1;
                        if (modeViewCount > runtimeViewCount) modeViewCount = runtimeViewCount;
                        bool display3D = (xr.renderingModeCount > 0)
                            ? xr.renderingModeDisplay3D[g_input.currentRenderingMode] : true;
                        bool monoMode = !display3D;
                        uint32_t tileColumns = (xr.renderingModeCount > 0 && xr.renderingModeTileColumns[g_input.currentRenderingMode] > 0)
                            ? xr.renderingModeTileColumns[g_input.currentRenderingMode]
                            : (monoMode ? 1u : 2u);
                        uint32_t tileRows = (xr.renderingModeCount > 0 && xr.renderingModeTileRows[g_input.currentRenderingMode] > 0)
                            ? xr.renderingModeTileRows[g_input.currentRenderingMode]
                            : 1u;

                        int eyeCount = monoMode ? 1 : (int)modeViewCount;

                        // HUD eye readout. Under the rig, views[] carries render-ready
                        // WORLD eyes, so the display-space eyes come from the raw channel
                        // (XrViewDisplayRawEXT); without the rig, fall back to views[].
                        // Capture the runtime-resolved CANVAS size (window client area in
                        // meters) — the physical height the runtime runs the Kooima/rig
                        // math on. The cam<->display converter MUST be fed this (not the
                        // full display height), else the rig switch zooms ~1.36x.
                        if (viewRigRaw.canvasSizeMeters.height > 1.0e-6f) {
                            g_canvasWM = viewRigRaw.canvasSizeMeters.width;
                            g_canvasHM = viewRigRaw.canvasSizeMeters.height;
                        }
                        if (useRig && viewRigRaw.eyeCountOutput > 0) {
                            for (uint32_t v = 0; v < viewRigRaw.eyeCountOutput && v < 8; v++) {
                                xr.eyePositions[v][0] = viewRigRaw.rawEyes[v].x;
                                xr.eyePositions[v][1] = viewRigRaw.rawEyes[v].y;
                                xr.eyePositions[v][2] = viewRigRaw.rawEyes[v].z;
                            }
                        } else {
                            for (uint32_t v = 0; v < modeViewCount && v < 8; v++) {
                                xr.eyePositions[v][0] = views[v].pose.position.x;
                                xr.eyePositions[v][1] = views[v].pose.position.y;
                                xr.eyePositions[v][2] = views[v].pose.position.z;
                            }
                        }
                        xr.isEyeTracking = (eyeTrackingState.isTracking == XR_TRUE);
                        xr.activeEyeTrackingMode = (uint32_t)eyeTrackingState.activeMode;

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (used for the projection viewport per eye and for the 'I'
                        // capture region) follow as cols × renderW × rows × renderH.
                        // The swapchain was sized at creation time to fit the
                        // largest atlas across all advertised modes, so no clamp
                        // is needed here.
                        float scaleX = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleX[g_input.currentRenderingMode] : 1.0f;
                        float scaleY = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleY[g_input.currentRenderingMode] : 1.0f;
                        uint32_t renderW = (uint32_t)((double)g_windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)g_windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;
                        g_renderW = renderW; g_renderH = renderH;

                        // --- Consume the runtime's render-ready XrView{pose, fov} (#396 W7) ---
                        // The runtime owns the off-axis Kooima (window resolve included); the
                        // app keeps only the clip policy (fov is clip-independent). near =
                        // ez - vH, far = ez + 1000*vH (macOS modelviewer is always opaque),
                        // ez = RigLocalEyeZ (== the display-space eye Z display3d resolved).
                        // The view matrix is the plain clean-frame mat4_view_from_xr_pose —
                        // ModelRenderer owns the Vulkan Y-down flip via a negative viewport.
                        // GL projection → [0,1] depth remap kept (mesh uses the depth buffer).
                        std::vector<Display3DView> eyeViews((size_t)eyeCount);
                        bool hasKooima = useRig;
                        if (useRig) {
                            // Mono: collapse the active views to their centroid (pose + fov).
                            std::vector<XrView> srcViews;
                            if (monoMode && modeViewCount >= 1) {
                                XrView cv = views[0];
                                XrVector3f c = {0, 0, 0};
                                XrFovf f = {0, 0, 0, 0};
                                for (uint32_t v = 0; v < modeViewCount; v++) {
                                    c.x += views[v].pose.position.x;
                                    c.y += views[v].pose.position.y;
                                    c.z += views[v].pose.position.z;
                                    f.angleLeft  += views[v].fov.angleLeft;
                                    f.angleRight += views[v].fov.angleRight;
                                    f.angleUp    += views[v].fov.angleUp;
                                    f.angleDown  += views[v].fov.angleDown;
                                }
                                float inv = 1.0f / (float)modeViewCount;
                                cv.pose.position = {c.x * inv, c.y * inv, c.z * inv};
                                cv.fov = {f.angleLeft * inv, f.angleRight * inv,
                                          f.angleUp * inv, f.angleDown * inv};
                                srcViews.assign(1, cv);
                            } else {
                                for (int e = 0; e < eyeCount; e++)
                                    srcViews.push_back(views[e < (int)runtimeViewCount ? e : 0]);
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                const XrView& sv = srcViews[eye];
                                float ez = RigLocalEyeZ(cameraPose, sv.pose.position);
                                float near_z, far_z;
                                if (rigCamera || g_focusActive) {
                                    // Camera rig — AND the focus-mode converted display rig,
                                    // which frames the scene at the SAME XR scale (POI at
                                    // ~1 XR-m) and has eye_display.z (ez) ~= 0. Using the
                                    // ez-based display range below would give near=1e-4,
                                    // far=1500 (1.5e7 ratio) => total depth-precision collapse
                                    // / z-fighting. Use the fixed tight range instead.
                                    near_z = 0.05f;
                                    far_z  = 200.0f;
                                } else {
                                    // Display rig: EarthView's streamed ground pops out close
                                    // to the eye, so the modelviewer near = ez - rigVH clipped
                                    // it to sky. Put the near plane just in front of the eye,
                                    // scaled to ez. See docs/rendering-notes.md §3.
                                    near_z = (ez * 0.01f > 1.0e-4f) ? (ez * 0.01f) : 1.0e-4f;
                                    far_z  = ez + 1000.0f * rigVH;
                                }
                                mat4_view_from_xr_pose(eyeViews[eye].view_matrix, sv.pose);
                                mat4_from_xr_fov(eyeViews[eye].projection_matrix, sv.fov, near_z, far_z);
                                // GL ([-1,1] clip-z) → Vulkan [0,1] depth for the mesh's depth buffer.
                                convert_projection_gl_to_zero_to_one(eyeViews[eye].projection_matrix);
                                eyeViews[eye].fov = sv.fov;
                                eyeViews[eye].eye_world = sv.pose.position;
                                eyeViews[eye].orientation = sv.pose.orientation;
                                eyeViews[eye].eye_display = {0.0f, 0.0f, ez};
                                eyeViews[eye].near_z = near_z;
                                eyeViews[eye].far_z = far_z;
                            }
                        }

                        // Double-click focus: ray from CENTER physical eyes through the
                        // physical mouse location on the display surface, pick nearest surface,
                        // then smoothly move & re-orient the virtual display to face back
                        // along the ray.
                        // Double-click: defer the orbit-acquire pick until eye 0
                        // has rendered (depth-readback unproject, PRD §6.1 diorama).
                        if (g_input.teleportRequested && useRig) {
                            g_input.teleportRequested = false;
                            NSSize viewSize = [[g_window contentView] bounds].size;
                            g_pickNdcX = 2.0f * g_input.teleportMouseX / (float)viewSize.width - 1.0f;
                            // Cocoa locationInWindow has y=0 at the BOTTOM of the window,
                            // matching +Y-up NDC directly (no negation).
                            g_pickNdcY = 2.0f * g_input.teleportMouseY / (float)viewSize.height - 1.0f;
                            g_pendingPick = true;
                        } else if (g_input.teleportRequested) {
                            g_input.teleportRequested = false; // consume without Kooima
                        }

                        // --- Tile streaming update (once per frame, PRD §6.2) ---
                        // Center-eye selection camera: ONE updateView with the
                        // geo camera + a single view tile's resolution + the
                        // union FOV across eyes; both eyes draw the same set.
                        // The world mapping (g_xrFromEcef) is double; the draw
                        // list carries per-tile float matrices (RTC, M0 fact 5).
                        if (g_tilesActive && useRig && eyeCount > 0) {
                            glm::dvec3 viewerPos(0.0);
                            XrFovf ufov = eyeViews[0].fov;
                            for (int e = 0; e < eyeCount; e++) {
                                viewerPos += glm::dvec3(eyeViews[e].eye_world.x,
                                                        eyeViews[e].eye_world.y,
                                                        eyeViews[e].eye_world.z);
                                ufov.angleLeft = std::min(ufov.angleLeft, eyeViews[e].fov.angleLeft);
                                ufov.angleRight = std::max(ufov.angleRight, eyeViews[e].fov.angleRight);
                                ufov.angleDown = std::min(ufov.angleDown, eyeViews[e].fov.angleDown);
                                ufov.angleUp = std::max(ufov.angleUp, eyeViews[e].fov.angleUp);
                            }
                            viewerPos /= (double)eyeCount;
                            g_viewerPosXr = viewerPos;

                            // The world is anchored to this XR point. FLY (camera rig)
                            // anchors at the CAMERA (origin) — the runtime owns the eyes, so
                            // the geo camera maps to the origin and the look pivot is the
                            // camera (no off-centre, no zoom-on-rotate). ORBIT keeps the
                            // viewer anchor (diorama, display rig).
                            glm::dvec3 anchorXr = viewerPos;
                            double vfov, hfov;
                            if (g_geoNav.orbitAcquired) {
                                // Display-centric diorama around the acquired
                                // center; the center glides to the display
                                // origin after a double-click (exp filter).
                                g_dioramaCenterXr *= std::exp(-(double)deltaTime / kDioramaGlideTau);
                                g_xrFromEcef = geo::xrFromEcefDiorama(
                                    g_geoNav.orbitCenter, g_dioramaCenterXr,
                                    g_geoNav.dioramaScale, g_geoNav.dioramaYaw,
                                    g_geoNav.dioramaTilt);
                                // Display-rig selection frustum: smallest symmetric cone that
                                // CONTAINS the off-axis per-eye frustums (+15% margin).
                                double vHalf = std::max(std::fabs((double)ufov.angleUp),
                                                        std::fabs((double)ufov.angleDown));
                                double hHalf = std::max(std::fabs((double)ufov.angleLeft),
                                                        std::fabs((double)ufov.angleRight));
                                vfov = 2.0 * vHalf * 1.15;
                                hfov = 2.0 * hHalf * 1.15;
                            } else {
                                // Camera-centric FLY: anchor the geo camera at the XR origin
                                // and place the target a fixed kTargetXrDist in front (scale
                                // s = kTargetXrDist/targetDist). The camera rig's verticalFov
                                // + convergenceDiopters own the stereo; selection just needs
                                // to match the camera frustum.
                                const double kTargetXrDist = 1.0;  // XR metres to the geo target
                                anchorXr = glm::dvec3(0.0, 0.0, 0.0);
                                double s = kTargetXrDist / std::max(g_geoNav.targetDist, 1.0);
                                g_xrFromEcef = geo::xrFromEcefCamera(g_geoNav.cam, anchorXr, s);
                                double aspect = (renderH > 0) ? (double)renderW / (double)renderH : 1.0;
                                // Match the selection frustum to the cam rig's physical vFOV (§5).
                                double camVFov = (double)CamVFovRad(xr.displayHeightM, xr.nominalViewerZ);
                                vfov = camVFov * 1.15;
                                double vHalfTan = std::tan(0.5 * camVFov);
                                hfov = 2.0 * std::atan(vHalfTan * aspect) * 1.15;

                                // Convergence auto-focus: forward ray → GROUND distance (geo
                                // metres), scaled to XR metres (× s) = the convergence plane.
                                // Using the ground (not a depth read) keeps it smooth — it
                                // won't snag on buildings. 1/(XR metres), clamped [0.2, 50]
                                // XR-m, exp-smoothed; fed to the rig next frame.
                                double groundM = geo::rayGroundDistanceM(g_geoNav.cam.pos,
                                                                         g_geoNav.cam.dir);
                                if (groundM > 0.0 && !g_focusActive) {  // [FOCUS] freeze conv during focus
                                    double xrDist = groundM * s;
                                    if (xrDist < 0.2) xrDist = 0.2;
                                    if (xrDist > 50.0) xrDist = 50.0;
                                    float tgt = (float)(1.0 / xrDist);
                                    double a = 1.0 - std::exp(
                                        -(double)deltaTime / kConvSmoothTau);
                                    g_convDiopters += (tgt - g_convDiopters) * (float)a;
                                }
                            }

                            // Selection camera = the viewer's HEAD camera in ECEF, from
                            // inverse(g_xrFromEcef). cesium-unity's mono "main camera"
                            // approach: off-axis stereo is render-only; selection uses a
                            // plain symmetric frustum. selCam.pos uses the SAME anchor the
                            // world was built from (maps to the geo camera origin).
                            geo::GeoCamera selCam;
                            {
                                glm::dmat4 invWorld = glm::inverse(g_xrFromEcef);
                                glm::dmat3 invRot = glm::dmat3(invWorld); // incl. 1/s — normalize after
                                selCam.pos = glm::dvec3(invWorld * glm::dvec4(anchorXr, 1.0));
                                selCam.dir = glm::normalize(invRot * glm::dvec3(0.0, 0.0, -1.0));
                                selCam.up = glm::normalize(invRot * glm::dvec3(0.0, 1.0, 0.0));
                            }

                            const auto& tiles = g_tileEngine.update(
                                selCam, (double)renderW, (double)renderH, hfov, vfov);
                            g_drawList = g_tileRenderer.buildDrawList(tiles, g_xrFromEcef);

                            // Streaming diagnostics every ~2 s: drawn vs
                            // selected-but-unprepared (persistent skips =
                            // upload starvation = visible holes).
                            if (g_frameCount % 120 == 0) {
                                double s = geo::stereoScaleForDistance(
                                    g_geoNav.targetDist, std::max((double)viewerPos.z, 0.1));
                                LOG_INFO("tiles: drawn=%zu skip=%d live=%d gpu=%.0fMB "
                                         "fov=%.0fx%.0f dist=%.0fm s=%.6f orbit=%d alt=%.0fm",
                                         g_drawList.size(),
                                         g_tileRenderer.lastStagingSkipped(),
                                         g_tileRenderer.liveTileCount(),
                                         g_tileRenderer.gpuResidentMB(),
                                         hfov * 57.2958, vfov * 57.2958,
                                         g_geoNav.targetDist, s,
                                         g_geoNav.orbitAcquired ? 1 : 0,
                                         geo::heightAboveEllipsoid(selCam.pos));
                            }
                        } else {
                            g_drawList.clear();
                        }

                        rendered = true;
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            projectionViews.assign((size_t)eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                            std::vector<std::array<float, 16>> viewMat((size_t)eyeCount);
                            std::vector<std::array<float, 16>> projMat((size_t)eyeCount);
                            std::vector<std::pair<uint32_t, uint32_t>> tileOffsets((size_t)eyeCount);
                            for (int eye = 0; eye < eyeCount; eye++) {
                                int srcView = eye < (int)runtimeViewCount ? eye : 0;
                                if (hasKooima) {
                                    memcpy(viewMat[eye].data(), eyeViews[eye].view_matrix, sizeof(float) * 16);
                                    memcpy(projMat[eye].data(), eyeViews[eye].projection_matrix, sizeof(float) * 16);
                                    views[srcView].pose.position = eyeViews[eye].eye_world;
                                    views[srcView].pose.orientation = cameraPose.orientation;
                                } else {
                                    mat4_view_from_xr_pose(viewMat[eye].data(), views[srcView].pose);
                                    mat4_from_xr_fov(projMat[eye].data(), views[srcView].fov, 0.01f, 100.0f);
                                }

                                // Tile-aware viewport: row-major eye layout in the atlas.
                                // For mono (cols=rows=1) this collapses to (0, 0).
                                uint32_t tileX = (uint32_t)(eye % (int)tileColumns);
                                uint32_t tileY = (uint32_t)(eye / (int)tileColumns);
                                uint32_t vpX = tileX * renderW;
                                uint32_t vpY = tileY * renderH;
                                tileOffsets[eye] = {vpX, vpY};

                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                                projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = views[srcView].pose;
                                projectionViews[eye].fov = hasKooima ? eyeViews[eye].fov : views[srcView].fov;
                            }

                            // Render model or placeholder
                            VkImage targetImage = swapchainImages[imageIndex].image;
                            VkFormat swapFormat = (VkFormat)xr.swapchain.format;

                            if (g_tilesActive) {
                                glm::dvec3 pickAccum(0.0);
                                int pickHits = 0;
                                // DXR_DUMP=N: one-shot mono PNG of eye 0 at
                                // frame N (self-verification on vk_native).
                                static long dumpFrame =
                                    getenv("DXR_DUMP") ? atol(getenv("DXR_DUMP")) : 0;
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    g_tileRenderer.renderEye(
                                        targetImage, swapFormat,
                                        xr.swapchain.width, xr.swapchain.height,
                                        tileOffsets[eye].first, tileOffsets[eye].second,
                                        renderW, renderH,
                                        viewMat[eye].data(), projMat[eye].data(),
                                        g_drawList);

                                    // Deferred double-click pick, CENTER-eye:
                                    // after each of the first two eyes renders,
                                    // read its depth at the clicked texel and
                                    // unproject through that eye's matrices;
                                    // the acquired point is the midpoint of
                                    // the per-eye hits (a left-eye-only ray
                                    // lands visibly right of the aimed spot).
                                    if (g_pendingPick && eye < 2 && !g_drawList.empty()) {
                                        // Negative-height viewport: ndcY=+1 -> row 0.
                                        uint32_t px = (uint32_t)std::min(std::max(
                                            (g_pickNdcX + 1.0f) * 0.5f * (float)renderW, 0.0f),
                                            (float)(renderW - 1));
                                        uint32_t py = (uint32_t)std::min(std::max(
                                            (1.0f - g_pickNdcY) * 0.5f * (float)renderH, 0.0f),
                                            (float)(renderH - 1));
                                        float d = g_tileRenderer.readDepth(px, py);
                                        if (d < 1.0f) {
                                            glm::dmat4 V = glm::dmat4(glm::make_mat4(viewMat[eye].data()));
                                            glm::dmat4 P = glm::dmat4(glm::make_mat4(projMat[eye].data()));
                                            glm::dvec4 clip((double)g_pickNdcX, (double)g_pickNdcY,
                                                            (double)d, 1.0);
                                            glm::dvec4 w = glm::inverse(P * V) * clip;
                                            if (std::abs(w.w) > 1e-12) {
                                                pickAccum += glm::dvec3(w) / w.w;
                                                pickHits++;
                                            }
                                        }
                                    }

                                    if (eye == 0 && dumpFrame > 0 &&
                                        (long)g_frameCount >= dumpFrame) {
                                        dumpFrame = 0; // one-shot
                                        g_tileRenderer.dumpColorTarget(
                                            "/tmp/earthview_dump.png", renderW, renderH);
                                    }
                                }
                                // Finalize the center-eye pick once all sampled
                                // eyes have rendered.
                                if (g_pendingPick) {
                                    g_pendingPick = false;
                                    if (pickHits > 0) {
                                        glm::dvec3 xrPos = pickAccum / (double)pickHits;
                                        glm::dvec3 ecef = glm::dvec3(
                                            glm::inverse(g_xrFromEcef) * glm::dvec4(xrPos, 1.0));
                                        // Smoothly re-aim the camera to center the new POI and
                                        // reframe it onto the zero-parallax plane (targetDist =
                                        // eye->POI / vDist => POI at XR depth vDist). Camera
                                        // POSITION stays put; only dir + zoom lerp (g_focusT).
                                        // Eye stays at the origin (no rig translation) => the pick
                                        // stays as accurate as fly mode.
                                        double poiDist = glm::length(g_geoNav.cam.pos - ecef);
                                        g_focusPOIecef = ecef;
                                        g_poiXitFromDir = g_geoNav.cam.dir;
                                        g_poiXitToDir = glm::normalize(ecef - g_geoNav.cam.pos);
                                        g_poiXitFromTD = g_geoNav.targetDist;
                                        g_poiXitToTD =
                                            std::max(poiDist / std::max((double)g_viewDistXR, 0.1), 20.0);
                                        g_focusT = 0.0;  // start the transition
                                        if (g_focusActive) {
                                            LOG_INFO("[FOCUS] shift POI -> ECEF (%.1f, %.1f, %.1f) dist=%.0f",
                                                     ecef.x, ecef.y, ecef.z, poiDist);
                                        } else {
                                            g_focusActive = true;
                                            LOG_INFO("[FOCUS] acquired POI ECEF (%.1f, %.1f, %.1f) dist=%.0f vDist=%.3f",
                                                     ecef.x, ecef.y, ecef.z, poiDist, g_viewDistXR);
                                        }
                                    } else {
                                        LOG_INFO("Pick missed (sky) — staying camera-centric");
                                    }
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, cmdPool,
                                    targetImage, xr.swapchain.width, xr.swapchain.height,
                                    g_input.yaw, g_input.pitch);
                            }

                            // 'I' key: snapshot the multi-view atlas the runtime
                            // composes for this session via xrCaptureAtlasEXT
                            // (XR_EXT_atlas_capture, W6 of #396). The runtime owns
                            // the readback — no app-side staging texture. Skipped
                            // for mono (1×1). The prefix has no ".png"; the runtime
                            // appends "_atlas.png".
                            // DXR_AUTOCAP=N: fire one atlas capture at frame N
                            // (autonomous verification — no keyboard needed).
                            static long autocapFrame =
                                getenv("DXR_AUTOCAP") ? atol(getenv("DXR_AUTOCAP")) : 0;
                            if (autocapFrame > 0 && (long)g_frameCount >= autocapFrame) {
                                autocapFrame = 0; // one-shot
                                g_input.captureAtlasRequested = true;
                            }
                            if (g_input.captureAtlasRequested) {
                                g_input.captureAtlasRequested = false;
                                uint32_t cols = tileColumns > 0 ? tileColumns : 1u;
                                uint32_t rows = tileRows > 0 ? tileRows : 1u;
                                if (!g_tilesActive) {
                                    LOG_WARN("Capture skipped: tiles inactive (no API key)");
                                } else if (cols <= 1 && rows <= 1) {
                                    LOG_WARN("Capture skipped: mono (1×1) layout");
                                } else if (xr.pfnCaptureAtlasEXT &&
                                           xr.session != XR_NULL_HANDLE) {
                                    // Stem = active bookmark, lowercased.
                                    size_t bmCount = 0;
                                    const geo::Bookmark* bm = geo::bookmarks(&bmCount);
                                    std::string stem = bm[g_geoNav.bookmarkIndex].name;
                                    for (auto& c : stem) c = (char)tolower((unsigned char)c);
                                    for (auto& c : stem) if (c == ' ') c = '_';
                                    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                                        stem, cols, rows);
                                    XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT;
                                    strncpy(info.pathPrefix, prefix.c_str(),
                                            XR_ATLAS_CAPTURE_PATH_MAX_EXT - 1);
                                    XrResult cr = xr.pfnCaptureAtlasEXT(xr.session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::TriggerCaptureFlash(
                                            (__bridge void*)g_metalView);
                                    } else {
                                        LOG_WARN("xrCaptureAtlasEXT failed: 0x%x", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_EXT_atlas_capture not available");
                                }
                            }

                            ReleaseSwapchainImage(xr);
                        } else {
                            rendered = false;
                        }
                    }
                }

                if (rendered) {
                    EndFrame(xr, frameState.predictedDisplayTime,
                        projectionViews.data(), (uint32_t)projectionViews.size());
                } else {
                    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
                    ei.displayTime = frameState.predictedDisplayTime;
                    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    ei.layerCount = 0; ei.layers = nullptr;
                    xrEndFrame(xr.session, &ei);
                }
            }
        } else {
            usleep(100000);
        }

        // Update HUD + attribution strip (~2 Hz)
        g_hudUpdateTimer += deltaTime;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                if (g_attributionText != nil) {
                    if (g_tilesActive) {
                        const AttributionInfo& attr = g_tileEngine.attribution();
                        std::string line;
                        for (const auto& c : attr.credits) {
                            if (!line.empty()) line += " · ";
                            line += c;
                        }
                        if (line.empty()) line = "Google";
                        char tail[96];
                        snprintf(tail, sizeof(tail), "   —  %d tiles in flight / %.0f MB",
                                 attr.tilesInFlight, g_tileRenderer.gpuResidentMB());
                        line += tail;
                        [g_attributionText setStringValue:
                            [NSString stringWithUTF8String:line.c_str()]];
                    } else {
                        [g_attributionText setStringValue:
                            @"EarthView needs a Google Map Tiles API key — enter it in "
                             "the panel (or press the key icon). Data © Google."];
                    }
                }
                if (g_input.hudVisible && g_hudView != nil) {
                    double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                    size_t bmCount = 0;
                    const geo::Bookmark* bm = geo::bookmarks(&bmCount);
                    NSString *sceneInfo = g_tilesActive
                        ? [NSString stringWithFormat:@"City: %s%s  Tiles: %d  GPU: %.0f MB  Dist: %.0f m",
                            bm[g_geoNav.bookmarkIndex].name,
                            g_geoNav.orbitAcquired ? " (diorama)" : "",
                            g_tileRenderer.liveTileCount(),
                            g_tileRenderer.gpuResidentMB(),
                            g_geoNav.targetDist]
                        : @"No API key (GOOGLE_MAPS_API_KEY or earthview.ini)";

                    int depthPct = (int)(g_input.viewParams.ipdFactor * 100.0f + 0.5f);
                    const char *orbitLabel = g_input.animateEnabled
                        ? (g_input.animationActive ? "ON (running)" : "ON (idle countdown)")
                        : "OFF";
                    uint32_t activeViewCount = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount)
                        ? xr.renderingModeViewCounts[g_input.currentRenderingMode] : 2u;
                    NSMutableString *eyesStr = [NSMutableString string];
                    for (uint32_t v = 0; v < activeViewCount && v < 8; v++) {
                        [eyesStr appendFormat:@"View %u: (%.3f, %.3f, %.3f)\n", v,
                            xr.eyePositions[v][0], xr.eyePositions[v][1], xr.eyePositions[v][2]];
                    }
                    NSString *text = [NSString stringWithFormat:
                        @"%s\nSession: %d\n"
                        "Mode: %s (%s, %u view%s)\n"
                        "%@\n"
                        "Depth/IPD: %d%%  Zoom: %.2fx  Auto-Orbit: %s\n"
                        "FPS: %.0f (%.1f ms)\n"
                        "Render: %ux%u  Window: %ux%u\n"
                        "Display: %.3f x %.3f m\n"
                        "%@"
                        "Vdisplay: (%.2f, %.2f, %.2f)\n"
                        "\nWASD=Pan  E/Q=Climb  LMB-drag=Look  Scroll=Dolly\n"
                        "DblClick=Orbit  B=City  C=Fly  Esc/Space=Release  -/= Depth\n"
                        "M=Auto-Orbit  V=Mode  I=Capture  Tab=HUD  ESC=Quit",
                        xr.systemName, (int)xr.sessionState,
                        (xr.renderingModeCount > 0 && xr.renderingModeNames[g_input.currentRenderingMode][0] != '\0') ? xr.renderingModeNames[g_input.currentRenderingMode] : "Unknown",
                        (xr.renderingModeCount > 0 ? (xr.renderingModeDisplay3D[g_input.currentRenderingMode] ? "3D" : "2D") : "3D"),
                        activeViewCount, activeViewCount == 1 ? "" : "s",
                        sceneInfo,
                        depthPct, g_input.viewParams.scaleFactor, orbitLabel,
                        fps, g_avgFrameTime * 1000.0,
                        g_renderW, g_renderH, g_windowW, g_windowH,
                        xr.displayWidthM, xr.displayHeightM,
                        eyesStr,
                        g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ];
                    g_hudView.hudText = text;
                    // Auto-size the frosted backdrop to fit the text; inner view auto-resizes.
                    NSDictionary *attrs = @{
                        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]
                    };
                    NSRect textBounds = [text boundingRectWithSize:NSMakeSize(420, CGFLOAT_MAX)
                                         options:NSStringDrawingUsesLineFragmentOrigin
                                         attributes:attrs];
                    CGFloat pad = 20.0;
                    NSRect hudFrame = NSMakeRect(8, 8,
                        ceilf(textBounds.size.width + pad),
                        ceilf(textBounds.size.height + pad));
                    [g_hudBackdrop setFrame:hudFrame];
                    [g_hudView setNeedsDisplay:YES];
                    [g_hudBackdrop setHidden:NO];
                } else if (g_hudBackdrop != nil) {
                    [g_hudBackdrop setHidden:YES];
                }
            }
        }
    }

    // Clean exit
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        xrRequestExitSession(xr.session);
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr); usleep(10000);
        }
    }

    LOG_INFO("=== Shutting down ===");
    g_xrForMcp = nullptr;  // session is going away; stop touching MCP tools
    g_drawList.clear();
    g_tileEngine.shutdown();   // Tileset dtor free()s every tile via the renderer
    g_tileRenderer.cleanup();  // then the renderer's own Vulkan objects
    if (cmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
    CleanupOpenXR(xr);
    // MoltenVK may throw std::system_error ("mutex lock failed") during device/instance
    // destruction due to internal threading cleanup.  Catch and ignore since we're exiting.
    try {
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
    } catch (const std::exception& e) {
        LOG_WARN("Vulkan cleanup exception (ignored): %s", e.what());
    }
    LOG_INFO("Application shutdown complete");
    return 0;
}
