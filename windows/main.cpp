// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR EarthView — streaming Google Photorealistic 3D Tiles on a
 *         tracked 3D display via OpenXR (Vulkan).
 *
 * Windows shell for the EarthView demo. Cloned from displayxr-demo-modelviewer's
 * windows/main.cpp (the Win32 + XR_EXT_win32_window_binding + Vulkan/OpenXR +
 * D2D window-space-layer HUD scaffold) with the ModelRenderer call sites
 * retargeted to tiles_common (TileEngine + TileRenderer), mirroring macos/main.mm
 * 1:1. The streaming / selection / coordinate / renderer code in tiles_common/
 * is platform-neutral and shared with macOS. See docs/rendering-notes.md.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"

// cesium-native headers (pulled in transitively by tile_renderer.h) define
// CesiumGltf::Material::AlphaMode string constants named OPAQUE / MASK / BLEND.
// <windows.h> (wingdi.h) defines OPAQUE and TRANSPARENT as macros, so include
// order bites: undo those GDI macros before the cesium headers. The modelviewer
// shell never hit this because it links a glTF loader, not cesium.
#ifdef OPAQUE
#undef OPAQUE
#endif
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif

#include "tile_engine.h"
#include "tile_renderer.h"
#include "geo_math.h"
#include "display3d_view.h"
#include "projection_depth.h"

#include "hud_renderer.h"
#include "text_overlay.h"
#include "atlas_capture.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "earthview_handle_vk_win";

static const wchar_t* WINDOW_CLASS = L"DisplayXREarthViewClass";
static const wchar_t* WINDOW_TITLE = L"DisplayXR EarthView";

// HUD overlay fractions. Layer spans full window height so chrome buttons
// can sit at the window top while the info panel anchors to the bottom-left
// (matching the macOS demo's split). The vk_native compositor now uses an
// alpha-blended draw pass for window-space layers, so the empty middle of
// the texture stays invisible. Font sizing is anchored to the legacy
// 0.5-fraction so text doesn't grow with the taller texture.
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 1.0f;
static const float HUD_FONT_BASE_FRACTION = 0.50f;

// ── Top button bar ────────────────────────────────────────────────────────
// All chrome buttons live in ONE full-width window-space layer at the top:
// Open + Mode packed at the left, the Animation pill pinned to the right, and a
// transparent center so the model shows through. This replaces the old split
// (Open/Mode baked into the HUD layer + Animation on its own separate layer) —
// per runtime issue #389: group co-planar controls into a single layer and keep
// the HUD info panel as its own (toggleable) layer. Positions below are absolute
// window-fractions, used both for hit-testing and for placing the pills inside
// the bar texture (the bar layer spans the full window width, so window-x maps
// straight onto bar-texture-x).
static const float OPEN_BTN_X_FRACTION = 0.010f;
static const float OPEN_BTN_WIDTH_FRACTION  = 0.060f;

static const float MODE_BTN_X_FRACTION = 0.075f;
static const float MODE_BTN_WIDTH_FRACTION  = 0.140f;

// Animation pill — right-aligned within the bar. Only drawn/clickable when the
// model has clips. Label = current clip name, or "Paused"; click = next clip
// (same as 'N').
static const float ANIM_BTN_WIDTH_FRACTION  = 0.140f;
static const float ANIM_BTN_MARGIN_FRACTION = 0.010f;
static inline float AnimBtnXFraction() {
    return 1.0f - ANIM_BTN_WIDTH_FRACTION - ANIM_BTN_MARGIN_FRACTION;
}

// Bar swapchain texture (wide + thin) and its window-space layer geometry. The
// layer spans the full window width; its height preserves the texture aspect so
// the pills aren't distorted as the tile is resized. The pills fill ~70% of the
// bar height, vertically centered.
static const uint32_t BTN_BAR_TEX_W = 1920;
static const uint32_t BTN_BAR_TEX_H = 56;
static const uint32_t BTN_BAR_FONT_BASE = BTN_BAR_TEX_H * 14;
static const float    BTN_BAR_Y_FRACTION = 0.008f;
static inline float BtnBarHeightFraction(uint32_t windowW, uint32_t windowH) {
    if (windowW == 0 || windowH == 0) return 0.05f;
    const float windowAR = (float)windowW / (float)windowH;
    const float texAR = (float)BTN_BAR_TEX_W / (float)BTN_BAR_TEX_H;
    return windowAR / texAR;  // layer width fraction = 1.0
}


// ── XR_EXT_view_rig consume-path math (#396 W7) ──────────────────────────────
// View/projection builders for the runtime's render-ready XrView{pose, fov}:
// GL convention, column-major float[16], matching the macOS main.mm helpers
// (per-platform duplication is the accepted pattern for these ~20 lines).

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

// sim_display output mode switching (legacy — replaced by unified rendering mode)
typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

// Global state
static InputState g_inputState;
// Standalone demo: bare TAB toggles the HUD (displayxr::common defaults to
// SHIFT+TAB so runtime test apps don't shadow the workspace shell's
// focus-cycle binding).
static const bool g_inputInit = [] {
    g_inputState.hudToggleRequiresShift = false;
    return true;
}();
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;

// EarthView state. All tile work runs on the render thread (cesium's "main
// thread" — TileEngine::update / TileRenderer::free are dispatched inside
// Tileset::updateView), so no cross-thread load queue / scene mutex is needed
// (unlike the modelviewer, which loaded files off the message-pump thread).
// Teardown order matters: g_tileEngine.shutdown() (Tileset dtor free()s every
// tile through the renderer) BEFORE g_tileRenderer.cleanup() — see WinMain tail.
static TileRenderer g_tileRenderer;
static TileEngine g_tileEngine;
static geo::GeoNav g_geoNav;
static std::atomic<bool> g_tilesActive{false}; // engine init'd (API key found)

// Per-frame tile state, render-thread only. xrFromEcef is the double world
// mapping (camera-centric or diorama); the draw list carries per-tile float
// matrices (RTC).
static std::vector<TileRenderer::DrawItem> g_drawList;
static glm::dmat4 g_xrFromEcef(1.0);

// Double-click pick, deferred until after eye 0 renders this frame (the
// depth-readback unproject needs eye 0's depth buffer + matrices).
static bool g_pendingPick = false;
static float g_pickNdcX = 0.0f, g_pickNdcY = 0.0f;
static glm::dvec3 g_viewerPosXr(0.0, 0.1, 0.6); // center-eye, updated per frame

// Smooth double-click transition: the diorama starts centered on the picked
// point's CURRENT on-screen XR position, then the center glides exponentially
// to the display origin.
static glm::dvec3 g_dioramaCenterXr(0.0);
static constexpr double kDioramaGlideTau = 0.35; // s

// Camera-rig (fly mode) state. The runtime owns the off-axis eyes; the app
// hands it a plain perspective camera (pose + verticalFov + convergence) and
// eye-tracking perturbs the frustum. Convergence auto-focuses on the terrain
// under the centre crosshair: invd = 1/(XR distance the centre ray hits the
// ground), exponentially smoothed across frames (one-frame lag — fed from the
// previous frame's centre depth read).
static constexpr float kCameraVFovRad = 0.6498f;  // ~37.2° full vertical FOV (2*atan(0.3249))
// 1/m to the convergence plane. Default = 1/kTargetXrDist (the geo target sits
// 1 XR-m in front of the camera), refined per frame by the centre-ray depth.
static float g_convDiopters = 1.0f;
static constexpr double kConvSmoothTau = 0.15;    // s — exp filter time constant

// Geo-navigation input deltas. The shared InputState (displayxr-common's
// input_handler.h) can't carry EarthView-specific fields, so the geo deltas
// live here, accumulated by the WndProc and consumed once per frame by
// UpdateGeoNav under g_inputMutex.
static float g_lookDX = 0.0f, g_lookDY = 0.0f;   // radians (left-drag)
static float g_dollySteps = 0.0f;                // scroll steps (exponential zoom)
static bool  g_cycleBookmarkRequested = false;   // 'B'
static bool  g_releaseOrbitRequested = false;    // Esc / Space (release acquired orbit)
static bool  g_releaseToFlyRequested = false;    // 'C': orbit -> fly, continuous
// Left-drag origin tracking (Win32 has no Cocoa-style per-event deltaX).
static int   g_lastDragX = 0, g_lastDragY = 0;
static bool  g_dragValid = false;

// 'I' key: capture the multi-view atlas region via xrCaptureAtlasEXT. Skipped
// for 1×1 (mono) layouts. Helper lives in common/atlas_capture*.
static std::atomic<bool> g_captureAtlasRequested{false};
// 'X' key cycles tile-renderer supersampling 1→2→4→1 for live A/B (default 1 =
// off). Applied to g_tileRenderer each frame on the render thread. Temporary
// dev control — to be exposed via the options panel later.
static std::atomic<uint32_t> g_ssaa{1};
// Ctrl+T transparent-bg toggle is inherited from the scaffold but inert for
// EarthView (opaque streaming) — kept so the window/session transparency setup
// stays identical to the runtime test apps.
static std::atomic<bool> g_transparentBg{false};

// Animation-button window-space layer resources: created in main() (when the
// HUD swapchain — i.e. window-space layers — is available), used by the render
// thread. The swapchain is app-owned state (displayxr::common's
// XrSessionManager carries no app-named fields, #396 W4) — created via the
// lib's CreateWindowSpaceSwapchain generic, destroyed before CleanupOpenXR.
static SwapchainInfo  g_animBtnSwapchain;                // app-owned window-space swapchain
static bool           g_hasAnimBtnSwapchain = false;
static HudRenderer    g_animBtnHud = {};                 // own D3D11 text renderer (256×80)
static bool           g_animBtnReady = false;            // all resources created
static VkBuffer       g_animBtnStaging = VK_NULL_HANDLE;
static VkDeviceMemory g_animBtnStagingMem = VK_NULL_HANDLE;
static void*          g_animBtnStagingMapped = nullptr;
static VkCommandPool  g_animBtnCmdPool = VK_NULL_HANDLE;
static std::vector<XrSwapchainImageVulkanKHR> g_animBtnSwapImages;

// Virtual-display height in meters (the rig's display plane). The world is
// scaled into XR space by geo_math's stereo-scale knob, so a fixed display
// works at every altitude. Matches macOS's kDefaultVirtualDisplayHeightM.
static constexpr float kDefaultVirtualDisplayHeightM = 1.5f;

static double NowSec() {
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

// Mark a fresh user interaction so the idle auto-orbit timer restarts.
static void MarkUserInput(InputState& input) {
    input.lastInputTimeSec = NowSec();
    input.animationActive = false;
}

// Refresh the bookmark/city HUD label (drawn in the top button bar).
static std::wstring CurrentBookmarkLabel() {
    size_t n = 0;
    const geo::Bookmark* bm = geo::bookmarks(&n);
    const char* name = (n > 0) ? bm[g_geoNav.bookmarkIndex].name : "—";
    return L"City: " + std::wstring(name, name + strlen(name));
}

// Geo navigation (PRD §7.1) — consumes the per-frame input deltas and drives
// the double-precision GeoNav in geo_math. The XR rig pose stays FIXED; only
// the geo camera moves (camera-centric model, §6.1). Runs on the render thread;
// the geo deltas (lookDX/lookDY/dolly) + the request bools are snapshotted +
// cleared by the caller under g_inputMutex and passed in by value. `input` is
// the per-frame snapshot (read for WASDEQ + the idle-timer fields, written
// back for animationActive/lastInputTimeSec).
static void UpdateGeoNav(InputState& input, float dt,
                         float lookDX, float lookDY, float dollySteps,
                         bool releaseToFly, bool resetOrReleaseOrbit,
                         bool cycleBookmark) {
    if (releaseToFly) {
        if (g_geoNav.orbitAcquired) {
            g_geoNav.releaseToFly(std::max(g_viewerPosXr.z, 0.1));
            LOG_INFO("Released orbit -> fly mode (continuous)");
        }
        input.lastInputTimeSec = NowSec();
        input.animationActive = false;
    }
    if (resetOrReleaseOrbit) {
        input.viewParams = ViewParams();
        input.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
        g_geoNav.releaseOrbit();   // back to camera-centric, reframe bookmark
        input.animationActive = false;
        input.lastInputTimeSec = NowSec();
        return;
    }
    if (cycleBookmark) {
        g_geoNav.cycleBookmark();  // instant jump (fly-over interp = M1.x)
        size_t n = 0;
        const geo::Bookmark* bm = geo::bookmarks(&n);
        LOG_INFO("Bookmark: %s", bm[g_geoNav.bookmarkIndex].name);
    }

    // Left-drag look / orbit.
    if (lookDX != 0.0f || lookDY != 0.0f)
        g_geoNav.look((double)lookDX, (double)lookDY);
    // Scroll dolly (exponential).
    if (dollySteps != 0.0f)
        g_geoNav.dolly((double)dollySteps);

    // WASD pan in the ground tangent plane, E/Q climb. Speeds scale with
    // targetDist inside GeoNav, so dt is the only factor here.
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

    // Auto-orbit: idle > 10 s → slow turntable.
    double idleFor = NowSec() - input.lastInputTimeSec;
    input.animationActive = (input.animateEnabled && idleFor > 10.0);
    if (input.animationActive) {
        double rate = 6.2831853 / 60.0; // one revolution per 60 seconds
        g_geoNav.look(rate * dt, 0.0);
    }
}

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

static void ToggleFullscreen(HWND hwnd) {
    if (g_fullscreen) {
        SetWindowLong(hwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(hwnd, HWND_TOP,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            SWP_FRAMECHANGED);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        g_savedWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_savedWindowRect);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);

        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen mode");
    }
}

static bool PointInFractionRect(int mouseX, int mouseY, int windowW, int windowH,
                                float xf, float yf, float wf, float hf) {
    if (windowW <= 0 || windowH <= 0) return false;
    float fx = (float)mouseX / (float)windowW;
    float fy = (float)mouseY / (float)windowH;
    return (fx >= xf && fx <= xf + wf && fy >= yf && fy <= yf + hf);
}

// All three buttons share the top bar's vertical band [BTN_BAR_Y, +barHeight];
// each owns its own x-column. Keeps hit-testing aligned with the rendered pills.
static bool IsClickOnModeButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        MODE_BTN_X_FRACTION, BTN_BAR_Y_FRACTION,
        MODE_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

// City/bookmark button — right-justified in the bar, always live (replaces the
// modelviewer's clip-gated Animation pill). Click = cycle city ('B' key).
static bool IsClickOnCityButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        AnimBtnXFraction(), BTN_BAR_Y_FRACTION,
        ANIM_BTN_WIDTH_FRACTION, BtnBarHeightFraction(windowW, windowH));
}

// Atlas capture is runtime-owned via xrCaptureAtlasEXT (XR_EXT_atlas_capture).
// App-side helpers (filename numbering + flash overlay) live in
// common/atlas_capture* — see dxr_capture::MakeCaptureAtlasPrefix /
// TriggerCaptureFlash / PostFlashRequest.

// EarthView streams tiles — there is no model/scene to load, so the
// modelviewer's TryAutoLoadBundledScene / QueueSceneLoad / OpenLoadDialog (Win32
// + #228 spatial file-picker) machinery is gone. The Google Map Tiles API key is
// resolved inside TileEngine::init() (GOOGLE_MAPS_API_KEY env or earthview.ini);
// keyless is a supported state (placeholder + how-to HUD card).

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        // UpdateInputState above already set leftButton/dragging=true. For
        // button clicks (which post a message to run a modal dialog or change
        // mode), clear that drag state — otherwise the modal eats the
        // matching WM_LBUTTONUP and subsequent mouse motion is interpreted as
        // a scene drag.
        if (IsClickOnModeButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            // Mode button = cycle request (V-key equivalent). Main loop
            // reads runtime's current mode and computes the target.
            g_inputState.cycleRenderingModeRequested = true;
            return 0;
        }
        if (IsClickOnCityButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            g_cycleBookmarkRequested = true;   // 'B'-key equivalent
            return 0;
        }
        SetCapture(hwnd);
        // Seed the geo-look drag origin (geo deltas are accumulated from
        // successive WM_MOUSEMOVE while the button is held).
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_lastDragX = mx; g_lastDragY = my; g_dragValid = true;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        { std::lock_guard<std::mutex> lock(g_inputMutex); g_dragValid = false; }
        return 0;

    case WM_MOUSEMOVE:
        // Left-drag look / orbit: accumulate geo deltas (UpdateGeoNav consumes
        // them once per frame). Win32 has no Cocoa-style deltaX, so derive it
        // from successive positions. Right/scroll handled separately.
        if (wParam & MK_LBUTTON) {
            int mx = (int)(short)LOWORD(lParam);
            int my = (int)(short)HIWORD(lParam);
            std::lock_guard<std::mutex> lock(g_inputMutex);
            if (g_dragValid) {
                g_lookDX += (float)(mx - g_lastDragX) * 0.005f;
                g_lookDY += (float)(my - g_lastDragY) * 0.005f;
                MarkUserInput(g_inputState);
            }
            g_lastDragX = mx; g_lastDragY = my; g_dragValid = true;
        }
        return 0;

    case WM_MOUSEWHEEL: {
        // Dolly toward/away from the view target (stereo scale follows distance).
        float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_dollySteps += notches * 0.5f;
        MarkUserInput(g_inputState);
        return 0;
    }

    case dxr_capture::kFlashUserMsg:
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            // ESC: first press releases an acquired orbit (back to fly), second
            // quits. orbitAcquired is render-thread state but reads are atomic
            // enough for this UX gate.
            if (g_geoNav.orbitAcquired) {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                g_releaseOrbitRequested = true;
            } else {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        if (wParam == 'B') {   // cycle city bookmarks (Paris default)
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_cycleBookmarkRequested = true;
            MarkUserInput(g_inputState);
            return 0;
        }
        if (wParam == 'C') {   // orbit -> camera-centric fly (continuous)
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_releaseToFlyRequested = true;
            MarkUserInput(g_inputState);
            return 0;
        }
        if (wParam == 'X' && !(lParam & 0x40000000)) {  // initial press only (no auto-repeat)
            // Cycle tile supersampling 1 -> 2 -> 4 -> 1 for live A/B. ('X' is
            // free — 'S' is WASD pan-back.)
            uint32_t n = g_ssaa.load();
            n = (n == 1) ? 2u : (n == 2) ? 4u : 1u;
            g_ssaa.store(n);
            LOG_INFO("Supersampling: %ux", n);
            return 0;
        }
        // I key = capture multi-view atlas
        if (wParam == 'I' || wParam == 'i') {
            g_captureAtlasRequested.store(true);
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height) {
    LOG_INFO("Creating application window (%dx%d)", width, height);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Null background brush + WS_EX_NOREDIRECTIONBITMAP (below) are required
    // by the runtime's transparent-window bridge (DComp + KMT shared texture).
    // Both must be set even when the demo defaults to opaque, because session
    // transparency is wired at xrCreateSession time and cannot be toggled later.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            return nullptr;
        }
    }

    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(WS_EX_NOREDIRECTIONBITMAP, WINDOW_CLASS, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;
    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;
    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
    }
}

// Render a simple "no scene" placeholder by clearing to dark gray
static void RenderPlaceholder(VkDevice device, VkQueue queue, VkCommandPool cmdPool,
                               VkImage image, uint32_t width, uint32_t height) {
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue clearColor = {{0.1f, 0.1f, 0.12f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

    // Transition to COLOR_ATTACHMENT
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    VkDevice vkDevice,
    VkQueue graphicsQueue,
    uint32_t queueFamilyIndex,
    VkInstance vkInstance,
    VkPhysicalDevice physDevice,
    std::vector<VkImage>* swapchainVkImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    VkBuffer hudStagingBuffer,
    void* hudStagingMapped,
    VkCommandPool hudCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* hudSwapchainImages,
    VkCommandPool loadBtnCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* loadBtnSwapchainImages,
    uint32_t loadBtnWidth,
    uint32_t loadBtnHeight)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Command pool for placeholder rendering
    VkCommandPool renderCmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &renderCmdPool);
    }

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        bool animateToggle = false;
        bool cycleModeRequested = false;
        int32_t absoluteModeRequest = -1;
        uint32_t windowW, windowH;
        // EarthView geo-nav deltas, snapshotted + cleared under the input lock.
        float gLookDX = 0.0f, gLookDY = 0.0f, gDolly = 0.0f;
        bool gReleaseToFly = false, gReleaseOrbit = false, gCycleBookmark = false;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
        }
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            resetRequested = g_inputState.resetViewRequested;
            animateToggle = g_inputState.animateToggleRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.teleportRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            cycleModeRequested = g_inputState.cycleRenderingModeRequested;
            g_inputState.cycleRenderingModeRequested = false;
            absoluteModeRequest = g_inputState.absoluteRenderingModeRequested;
            g_inputState.absoluteRenderingModeRequested = -1;
            g_inputState.eyeTrackingModeToggleRequested = false;
            g_inputState.animateToggleRequested = false;
            if (animateToggle) {
                g_inputState.animateEnabled = !g_inputState.animateEnabled;
                inputSnapshot.animateEnabled = g_inputState.animateEnabled;
            }
            // Snapshot + clear the EarthView geo deltas / request flags.
            gLookDX = g_lookDX; gLookDY = g_lookDY; gDolly = g_dollySteps;
            g_lookDX = g_lookDY = g_dollySteps = 0.0f;
            gReleaseToFly = g_releaseToFlyRequested; g_releaseToFlyRequested = false;
            gCycleBookmark = g_cycleBookmarkRequested; g_cycleBookmarkRequested = false;
            // Space (resetViewRequested) and Esc-release both fold into a single
            // "release acquired orbit / reframe" action.
            gReleaseOrbit = resetRequested || g_releaseOrbitRequested;
            g_releaseOrbitRequested = false;
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        // Rendering mode requests (V/mode-button=cycle, 0-8=absolute). Single
        // source of truth: runtime owns current mode via xr->currentModeIndex.
        if (cycleModeRequested && xr->pfnRequestDisplayRenderingModeEXT &&
            xr->session != XR_NULL_HANDLE && xr->renderingModeCount > 0) {
            uint32_t next = (xr->currentModeIndex + 1) % xr->renderingModeCount;
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, next);
        }
        if (absoluteModeRequest >= 0 && xr->pfnRequestDisplayRenderingModeEXT &&
            xr->session != XR_NULL_HANDLE &&
            (uint32_t)absoluteModeRequest < xr->renderingModeCount) {
            xr->pfnRequestDisplayRenderingModeEXT(xr->session, (uint32_t)absoluteModeRequest);
        }

        // Handle eye tracking mode toggle (T key)
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_EXT)
                    ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
                XrResult etResult = xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
                LOG_INFO("Eye tracking mode -> %s (%s)",
                    newMode == XR_EYE_TRACKING_MODE_MANUAL_EXT ? "MANUAL" : "MANAGED",
                    XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
            }
        }

        UpdatePerformanceStats(perfStats);
        // Geo navigation (replaces UpdateCameraMovement). The XR rig pose stays
        // FIXED at the origin; only the double-precision geo camera moves. WASDEQ
        // pan/climb, left-drag look, scroll dolly, B/C/Space/Esc all flow through
        // here. The render-loop's selCam block (below) maps the geo camera into
        // XR space via g_xrFromEcef.
        UpdateGeoNav(inputSnapshot, perfStats.deltaTime,
                     gLookDX, gLookDY, gDolly,
                     gReleaseToFly, gReleaseOrbit, gCycleBookmark);

        // Persist the idle-timer state back so the next frame's snapshot sees it
        // (UpdateGeoNav mutates lastInputTimeSec / animationActive). The rig pose
        // fields (cameraPos / yaw / pitch) stay at their origin defaults — the
        // world moves, not the rig.
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.animationActive = inputSnapshot.animationActive;
            g_inputState.lastInputTimeSec = inputSnapshot.lastInputTimeSec;
            if (resetRequested) {
                g_inputState.viewParams = inputSnapshot.viewParams;
                g_inputState.animateEnabled = true;  // auto-orbit always available
            }
        }

        PollEvents(*xr);

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                // Sized to runtime's max possible view count (sim_display Quad mode = 4).
                // Active mode's view count drives how many slots are actually filled and submitted.
                XrCompositionLayerProjectionView projectionViews[8] = {};
                bool rendered = false;
                bool hudSubmitted = false;
                bool loadBtnSubmitted = false;

                // Aspect-preserving HUD layer footprint (fixes demo-gs#8).
                // The HUD swapchain has a fixed pixel aspect (hudWidth × hudHeight,
                // sized once at session create). When the workspace tile is
                // resized to a different aspect, the runtime stretches the
                // swapchain per-axis to fit the layer rect — which distorts
                // glyphs and button shapes. Fix: pick layer-rect fractions
                // (layerFracW × layerFracH, in HWND fractions) that match the
                // swapchain aspect so both axes stretch by the same factor
                // (uniform scaling, no distortion). Same pattern as the runtime
                // test apps (test_apps/cube_handle_d3d11_win/main.cpp ~L800).
                // Prefer layerFracH = 1.0 (full window height, keeps the info
                // panel anchored to the window bottom); on extremely tall tiles
                // where that would push layerFracW past 1.0, clamp width and
                // shrink height instead.
                const float hudAR = (hudHeight > 0)
                    ? (float)hudWidth / (float)hudHeight : 1.0f;
                const float windowAR = (windowW > 0 && windowH > 0)
                    ? (float)windowW / (float)windowH : 1.0f;
                float layerFracH = 1.0f;
                float layerFracW = hudAR / windowAR;
                if (layerFracW > 1.0f) {
                    layerFracW = 1.0f;
                    layerFracH = windowAR / hudAR;
                }

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, inputSnapshot.pitch,
                        inputSnapshot.viewParams)) {

                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};

                        // EarthView rig pose is ALWAYS identity: ALL view rotation lives
                        // in the geo world mapping (g_xrFromEcef via g_geoNav), NOT in the
                        // rig. The shared UpdateInputState() spuriously writes
                        // g_inputState.yaw/pitch on mouse-drag (modelviewer convention) — if
                        // those fed cameraPose, the rotation would be applied TWICE (world +
                        // rig) and the content would counter-rotate as you drag (the
                        // tile-lag bug). So pin the rig pose to identity and ignore the
                        // contaminated yaw/pitch/cameraPos entirely.
                        XrPosef cameraPose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
                        const float rigVH = inputSnapshot.viewParams.virtualDisplayHeight
                            / inputSnapshot.viewParams.scaleFactor;

                        // XR_EXT_view_rig (#396 W7): chain a rig so the runtime owns the
                        // off-axis eyes + window resolve, returning render-ready
                        // XrView{pose, fov}. FLY (camera-centric) uses the CAMERA rig: a
                        // plain perspective camera the runtime perturbs with eye tracking,
                        // converging at convergenceDiopters — the runtime does all the
                        // off-axis math, so the app never anchors content to the tracked
                        // eye (that was the off-centre / zoom-on-rotate bug). ORBIT uses the
                        // DISPLAY rig (portal model). The raw channel carries display-space
                        // eyes for the HUD.
                        const bool useRig =
                            g_hasViewRigExt && xr->displayWidthM > 0 && xr->displayHeightM > 0;
                        const bool rigCamera = useRig && !g_geoNav.orbitAcquired;
                        XrCameraRigEXT cameraRig = {XR_TYPE_CAMERA_RIG_EXT};
                        XrDisplayRigEXT displayRig = {XR_TYPE_DISPLAY_RIG_EXT};
                        XrViewDisplayRawEXT viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_EXT};
                        if (useRig) {
                            if (rigCamera) {
                                // Camera at the XR origin looking -Z; the geo world is
                                // mapped in front of it via g_xrFromEcef (anchor = origin).
                                cameraRig.pose = cameraPose;  // identity (cameraPos=0, yaw=pitch=0)
                                cameraRig.ipdFactor = inputSnapshot.viewParams.ipdFactor;
                                cameraRig.parallaxFactor = inputSnapshot.viewParams.parallaxFactor;
                                cameraRig.convergenceDiopters = g_convDiopters;  // 1/m, auto-focused
                                cameraRig.verticalFov = kCameraVFovRad;
                                locateInfo.next = &cameraRig;
                            } else {
                                displayRig.pose = cameraPose;
                                displayRig.virtualDisplayHeight = rigVH;
                                displayRig.ipdFactor = inputSnapshot.viewParams.ipdFactor;
                                displayRig.parallaxFactor = inputSnapshot.viewParams.parallaxFactor;
                                displayRig.perspectiveFactor = inputSnapshot.viewParams.perspectiveFactor;
                                locateInfo.next = &displayRig;
                            }
                            viewState.next = &viewRigRaw;
                        }

                        // Over-allocate to the runtime's max possible view_count (sim_display
                        // reports 4 for Quad mode; LeiaSR reports 2). Hardcoding 2 here used
                        // to fail with XR_ERROR_SIZE_INSUFFICIENT under sim_display.
                        uint32_t viewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t i = 0; i < 8; i++) rawViews[i] = {XR_TYPE_VIEW};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                        // HUD eye readout. Under the rig, rawViews[] carries render-ready
                        // WORLD eyes, so the display-space eyes come from the raw channel
                        // (XrViewDisplayRawEXT); without the rig, the fill from the common
                        // LocateViews call above stands.
                        if (useRig && viewRigRaw.eyeCountOutput > 0) {
                            for (uint32_t v = 0; v < viewRigRaw.eyeCountOutput && v < 8; v++) {
                                xr->eyePositions[v][0] = viewRigRaw.rawEyes[v].x;
                                xr->eyePositions[v][1] = viewRigRaw.rawEyes[v].y;
                                xr->eyePositions[v][2] = viewRigRaw.rawEyes[v].z;
                            }
                        }

                        bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[xr->currentModeIndex]);

                        // View count for the active rendering mode (1=mono, 2=stereo, 4=quad).
                        // Sized off the runtime's per-mode advertisement so the eye-loop and
                        // per-view buffers (rawEyes / stereoViews / viewMat / projectionViews)
                        // all line up with what xrEndFrame expects.
                        uint32_t activeViewCount = (xr->renderingModeCount > 0)
                            ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2u;
                        if (activeViewCount == 0) activeViewCount = 1u;
                        if (activeViewCount > 8) activeViewCount = 8u;
                        const int eyeCount = monoMode ? 1 : (int)activeViewCount;

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (cols × renderW, rows × renderH) are what gets written to
                        // the swapchain and snapshotted by the 'I' key. Swapchain
                        // creation already sized for the largest atlas, so no clamp.
                        // Falls back to the global recommendedViewScale (and 1.0 for
                        // mono) if the runtime didn't advertise per-mode info.
                        float scaleX, scaleY;
                        uint32_t cols, rows;
                        if (xr->renderingModeCount > 0) {
                            uint32_t mode = xr->currentModeIndex;
                            scaleX = xr->renderingModeScaleX[mode];
                            scaleY = xr->renderingModeScaleY[mode];
                            cols   = xr->renderingModeTileColumns[mode] ? xr->renderingModeTileColumns[mode] : 1u;
                            rows   = xr->renderingModeTileRows[mode]    ? xr->renderingModeTileRows[mode]    : 1u;
                        } else if (monoMode) {
                            scaleX = 1.0f; scaleY = 1.0f; cols = 1u; rows = 1u;
                        } else {
                            scaleX = xr->recommendedViewScaleX;
                            scaleY = xr->recommendedViewScaleY;
                            cols = 2u; rows = 1u;  // legacy SBS default
                        }
                        uint32_t renderW = (uint32_t)((double)windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;

                        // --- Consume the runtime's render-ready XrView{pose, fov} (#396 W7) ---
                        // The runtime owns the off-axis Kooima (window resolve included —
                        // it tracks resize via GetClientRect runtime-side); the app keeps
                        // only the clip policy (fov is clip-independent). near = ez - vH,
                        // far = ez + 1000*vH (opaque recede band; transparent mode's
                        // foreground-only look is the clipFar shader cull below, not a
                        // projection clamp), ez = RigLocalEyeZ (== the display-space eye Z
                        // display3d resolved). The view matrix is the plain clean-frame
                        // mat4_view_from_xr_pose — ModelRenderer owns the Vulkan Y-down
                        // flip via a negative viewport. GL projection → [0,1] depth remap
                        // kept (mesh uses the depth buffer).
                        Display3DView stereoViews[8];
                        bool useAppProjection = useRig;
                        if (useRig) {
                            // Mono: collapse the active views to their centroid (pose + fov).
                            // Clamp to the count the runtime actually wrote (macOS clamps
                            // modeViewCount to runtimeViewCount the same way).
                            uint32_t monoN = activeViewCount > viewCount ? viewCount : activeViewCount;
                            XrView srcViews[8];
                            if (monoMode && monoN >= 1) {
                                XrView cv = rawViews[0];
                                XrVector3f c = {0, 0, 0};
                                XrFovf f = {0, 0, 0, 0};
                                for (uint32_t v = 0; v < monoN; v++) {
                                    c.x += rawViews[v].pose.position.x;
                                    c.y += rawViews[v].pose.position.y;
                                    c.z += rawViews[v].pose.position.z;
                                    f.angleLeft  += rawViews[v].fov.angleLeft;
                                    f.angleRight += rawViews[v].fov.angleRight;
                                    f.angleUp    += rawViews[v].fov.angleUp;
                                    f.angleDown  += rawViews[v].fov.angleDown;
                                }
                                float inv = 1.0f / (float)monoN;
                                cv.pose.position = {c.x * inv, c.y * inv, c.z * inv};
                                cv.fov = {f.angleLeft * inv, f.angleRight * inv,
                                          f.angleUp * inv, f.angleDown * inv};
                                srcViews[0] = cv;
                            } else {
                                for (int e = 0; e < eyeCount; e++)
                                    srcViews[e] = rawViews[e < (int)viewCount ? e : 0];
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                const XrView& sv = srcViews[eye];
                                float ez = RigLocalEyeZ(cameraPose, sv.pose.position);
                                float near_z, far_z;
                                if (rigCamera) {
                                    // Camera rig: a plain perspective camera. The geo target
                                    // is fixed at kTargetXrDist (1 XR-m) regardless of
                                    // altitude (s = kTargetXrDist/targetDist), so a FIXED tight
                                    // near/far around that scene scale keeps depth precision
                                    // (~4000:1, vs the z-fighting 200000:1 of a 0.01/2000
                                    // range). IMPORTANT: near/far are decoupled from the
                                    // convergence — the convergence auto-focus tracks the
                                    // crosshair (which can be at the horizon), and tying near
                                    // to it would clip the foreground.
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
                                mat4_view_from_xr_pose(stereoViews[eye].view_matrix, sv.pose);
                                mat4_from_xr_fov(stereoViews[eye].projection_matrix, sv.fov, near_z, far_z);
                                // GL ([-1,1] clip-z) → Vulkan [0,1] depth for the mesh's depth buffer.
                                convert_projection_gl_to_zero_to_one(stereoViews[eye].projection_matrix);
                                stereoViews[eye].fov = sv.fov;
                                stereoViews[eye].eye_world = sv.pose.position;
                                stereoViews[eye].orientation = sv.pose.orientation;
                                stereoViews[eye].eye_display = {0.0f, 0.0f, ez};
                                stereoViews[eye].near_z = near_z;
                                stereoViews[eye].far_z = far_z;
                            }
                        }

                        // Double-click: defer the orbit-acquire pick until eye 0 has
                        // rendered (depth-readback unproject, PRD §6.1 diorama). Win32
                        // mouse y=0 is at the TOP, so negate to +Y-up NDC.
                        if (inputSnapshot.teleportRequested && useRig) {
                            g_pickNdcX = 2.0f * inputSnapshot.teleportMouseX / (float)windowW - 1.0f;
                            g_pickNdcY = -(2.0f * inputSnapshot.teleportMouseY / (float)windowH - 1.0f);
                            g_pendingPick = true;
                        }

                        // --- Tile streaming update (once per frame, PRD §6.2) ---
                        // Center-eye selection camera: ONE updateView with the geo
                        // camera + a single view tile's resolution + the union FOV
                        // across eyes; both eyes draw the same set. The world mapping
                        // (g_xrFromEcef) is double; the draw list carries per-tile float
                        // matrices (RTC). See docs/rendering-notes.md §§1–2.
                        if (g_tilesActive.load() && useRig && eyeCount > 0) {
                            glm::dvec3 viewerPos(0.0);
                            XrFovf ufov = stereoViews[0].fov;
                            for (int e = 0; e < eyeCount; e++) {
                                viewerPos += glm::dvec3(stereoViews[e].eye_world.x,
                                                        stereoViews[e].eye_world.y,
                                                        stereoViews[e].eye_world.z);
                                ufov.angleLeft = std::min(ufov.angleLeft, stereoViews[e].fov.angleLeft);
                                ufov.angleRight = std::max(ufov.angleRight, stereoViews[e].fov.angleRight);
                                ufov.angleDown = std::min(ufov.angleDown, stereoViews[e].fov.angleDown);
                                ufov.angleUp = std::max(ufov.angleUp, stereoViews[e].fov.angleUp);
                            }
                            viewerPos /= (double)eyeCount;
                            g_viewerPosXr = viewerPos;

                            // The world is anchored to this XR point. FLY (camera rig)
                            // anchors at the CAMERA (origin) — the runtime owns the eyes,
                            // so the geo camera maps to the origin and the look pivot is the
                            // camera (no off-centre, no zoom-on-rotate). ORBIT keeps the
                            // viewer anchor (diorama, display rig).
                            glm::dvec3 anchorXr = viewerPos;
                            double vfov, hfov;
                            if (g_geoNav.orbitAcquired) {
                                // Display-centric diorama around the acquired center;
                                // the center glides to the display origin after a
                                // double-click (exp filter).
                                g_dioramaCenterXr *= std::exp(-(double)perfStats.deltaTime / kDioramaGlideTau);
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
                                // Selection frustum = the camera rig frustum (verticalFov)
                                // widened to the atlas tile aspect, +15% margin.
                                double aspect = (renderH > 0) ? (double)renderW / (double)renderH : 1.0;
                                vfov = (double)kCameraVFovRad * 1.15;
                                double vHalfTan = std::tan(0.5 * (double)kCameraVFovRad);
                                hfov = 2.0 * std::atan(vHalfTan * aspect) * 1.15;

                                // Convergence auto-focus: forward ray → GROUND distance
                                // (geo metres), scaled to XR metres (× s) = the convergence
                                // plane. Using the ground (not a depth read) keeps it smooth —
                                // buildings don't snag it. convergenceDiopters = 1/(XR metres),
                                // clamped to [0.2, 50] XR-m, exp-smoothed; fed to the rig next
                                // frame.
                                double groundM = geo::rayGroundDistanceM(g_geoNav.cam.pos,
                                                                         g_geoNav.cam.dir);
                                if (groundM > 0.0) {
                                    double xrDist = groundM * s;
                                    if (xrDist < 0.2) xrDist = 0.2;
                                    if (xrDist > 50.0) xrDist = 50.0;
                                    float tgt = (float)(1.0 / xrDist);
                                    double a = 1.0 - std::exp(
                                        -(double)perfStats.deltaTime / kConvSmoothTau);
                                    g_convDiopters += (tgt - g_convDiopters) * (float)a;
                                }
                            }

                            // Selection camera = the viewer's HEAD camera in ECEF, from
                            // inverse(g_xrFromEcef). selCam.pos uses the SAME anchor the
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

                            // Streaming diagnostics every ~2 s.
                            static uint64_t s_tileFrame = 0;
                            if ((s_tileFrame++ % 120) == 0) {
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
                        // eyeCount already computed above from active mode's view count

                        // Mono center eye
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                            monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                            monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;

                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];
                                XMVECTOR centerLocalPos = XMVectorSet(
                                    monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                                XMVECTOR localOri = XMVectorSet(
                                    rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                    rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                                float monoM2vView = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    monoM2vView = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                float eyeScale = inputSnapshot.viewParams.perspectiveFactor * monoM2vView / inputSnapshot.viewParams.scaleFactor;
                                XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                    inputSnapshot.pitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);
                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        // Foreground-only clip: in transparent mode, cull splats
                        // behind the virtual display plane so only popping-out
                        // content shows. Suppressed under the shell's external
                        // multi-compositor (non-controller workspace session,
                        // where the per-app transparent bridge is bypassed) —
                        // signalled by renderingModeIsRequestable being false.
                        bool standalone = (xr->renderingModeCount == 0) ||
                            (xr->currentModeIndex < xr->renderingModeCount &&
                             xr->renderingModeIsRequestable[xr->currentModeIndex]);
                        bool foregroundClip = g_transparentBg.load() && standalone;

                        // Build per-eye view/projection matrices (column-major float[16]).
                        // Sized to the runtime's max view count so Quad mode (4 views) fits.
                        float viewMat[8][16], projMat[8][16];
                        float clipFar[8] = {0};  // per-eye view-space far cull (0 = off)
                        for (int eye = 0; eye < eyeCount; eye++) {
                            if (useAppProjection) {
                                int srcEye = monoMode ? 0 : eye;
                                memcpy(viewMat[eye], stereoViews[srcEye].view_matrix, sizeof(float) * 16);
                                memcpy(projMat[eye], stereoViews[srcEye].projection_matrix, sizeof(float) * 16);
                                // eye_display.z = eye->display-plane forward distance,
                                // same world units as the shader's p_view.z.
                                if (foregroundClip) {
                                    float cf = stereoViews[srcEye].eye_display.z;
                                    clipFar[eye] = (cf > 0.2f) ? cf : 0.0f;  // never cull at/behind near
                                }
                            } else {
                                // Fallback: use DirectXMath mono matrices, store as column-major
                                XMMATRIX v = monoMode ? monoViewMatrix :
                                    XMMatrixLookAtRH(XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position),
                                        XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position) + XMVectorSet(0,0,-1,0),
                                        XMVectorSet(0,1,0,0));
                                XMMATRIX p = monoMode ? monoProjMatrix : xr->projMatrices[0];
                                // XMMatrix is row-major; transpose to get column-major for shader
                                XMMATRIX vT = XMMatrixTranspose(v);
                                XMMATRIX pT = XMMatrixTranspose(p);
                                XMStoreFloat4x4((XMFLOAT4X4*)viewMat[eye], vT);
                                XMStoreFloat4x4((XMFLOAT4X4*)projMat[eye], pT);
                            }
                        }

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            VkFormat colorFormat = (VkFormat)xr->swapchain.format;

                            // Apply the live supersample setting ('S' key cycle).
                            g_tileRenderer.setSupersample(g_ssaa.load());

                            const bool tilesActive = g_tilesActive.load();

                            if (tilesActive) {
                                glm::dvec3 pickAccum(0.0);
                                int pickHits = 0;
                                // DXR_DUMP=N: one-shot mono PNG of eye 0 at frame N
                                // (self-verification on vk_native — the D3D11-service
                                // screenshot trigger doesn't apply to a handle app).
                                static long dumpFrame =
                                    getenv("DXR_DUMP") ? atol(getenv("DXR_DUMP")) : 0;
                                static uint64_t s_renderFrame = 0;
                                uint64_t thisFrame = s_renderFrame++;
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    uint32_t col = (uint32_t)eye % cols;
                                    uint32_t row = (uint32_t)eye / cols;
                                    uint32_t vpX = col * renderW;
                                    uint32_t vpY = row * renderH;
                                    g_tileRenderer.renderEye(
                                        (*swapchainVkImages)[imageIndex], colorFormat,
                                        xr->swapchain.width, xr->swapchain.height,
                                        vpX, vpY, renderW, renderH,
                                        viewMat[eye], projMat[eye],
                                        g_drawList);

                                    // Deferred double-click pick, CENTER-eye: after each
                                    // of the first two eyes renders, read its depth at the
                                    // clicked texel and unproject through that eye's
                                    // matrices; the acquired point is the midpoint of the
                                    // per-eye hits.
                                    if (g_pendingPick && eye < 2 && !g_drawList.empty()) {
                                        uint32_t px = (uint32_t)std::min(std::max(
                                            (g_pickNdcX + 1.0f) * 0.5f * (float)renderW, 0.0f),
                                            (float)(renderW - 1));
                                        // Negative-height viewport: ndcY=+1 -> row 0.
                                        uint32_t py = (uint32_t)std::min(std::max(
                                            (1.0f - g_pickNdcY) * 0.5f * (float)renderH, 0.0f),
                                            (float)(renderH - 1));
                                        float d = g_tileRenderer.readDepth(px, py);
                                        if (d < 1.0f) {
                                            glm::dmat4 V = glm::dmat4(glm::make_mat4(viewMat[eye]));
                                            glm::dmat4 P = glm::dmat4(glm::make_mat4(projMat[eye]));
                                            glm::dvec4 clip((double)g_pickNdcX, (double)g_pickNdcY,
                                                            (double)d, 1.0);
                                            glm::dvec4 w = glm::inverse(P * V) * clip;
                                            if (std::abs(w.w) > 1e-12) {
                                                pickAccum += glm::dvec3(w) / w.w;
                                                pickHits++;
                                            }
                                        }
                                    }

                                    // [Convergence auto-focus is computed geometrically in the
                                    // selCam block (forward ray → ground), not from the depth
                                    // buffer — the latter snagged on buildings and spiked.]

                                    if (eye == 0 && dumpFrame > 0 &&
                                        (long)thisFrame >= dumpFrame) {
                                        dumpFrame = 0; // one-shot
                                        char dumpPath[MAX_PATH] = {0};
                                        char tmpDir[MAX_PATH] = {0};
                                        DWORD tn = GetTempPathA(MAX_PATH, tmpDir);
                                        if (tn > 0 && tn < MAX_PATH)
                                            snprintf(dumpPath, sizeof(dumpPath),
                                                     "%searthview_dump.png", tmpDir);
                                        else
                                            snprintf(dumpPath, sizeof(dumpPath),
                                                     "earthview_dump.png");
                                        g_tileRenderer.dumpColorTarget(dumpPath, renderW, renderH);
                                        LOG_INFO("DXR_DUMP wrote %s", dumpPath);
                                    }
                                }
                                // Finalize the center-eye pick once all sampled eyes
                                // have rendered.
                                if (g_pendingPick) {
                                    g_pendingPick = false;
                                    if (pickHits > 0) {
                                        glm::dvec3 xrPos = pickAccum / (double)pickHits;
                                        glm::dvec3 ecef = glm::dvec3(
                                            glm::inverse(g_xrFromEcef) * glm::dvec4(xrPos, 1.0));
                                        g_geoNav.acquireOrbit(ecef, std::max(g_viewerPosXr.z, 0.1));
                                        g_dioramaCenterXr = xrPos; // glide from here
                                        LOG_INFO("Orbit acquired (diorama, %d-eye pick): ECEF (%.1f, %.1f, %.1f)",
                                                 pickHits, ecef.x, ecef.y, ecef.z);
                                    } else {
                                        LOG_INFO("Pick missed (sky) — staying camera-centric");
                                    }
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, renderCmdPool,
                                    (*swapchainVkImages)[imageIndex], xr->swapchain.width, xr->swapchain.height);
                            }

                            // 'I' key: snapshot the multi-view atlas via xrCaptureAtlasEXT
                            // (runtime-owned readback). Skipped for mono (1×1). Stem =
                            // the active city bookmark.
                            if (g_captureAtlasRequested.exchange(false)) {
                                if (!tilesActive) {
                                    LOG_WARN("Capture skipped: tiles inactive (no API key)");
                                } else if (cols <= 1 && rows <= 1) {
                                    LOG_WARN("Capture skipped: mono (1×1) layout");
                                } else if (xr->pfnCaptureAtlasEXT &&
                                           xr->session != XR_NULL_HANDLE) {
                                    size_t bmCount = 0;
                                    const geo::Bookmark* bm = geo::bookmarks(&bmCount);
                                    std::string stem = (bmCount > 0)
                                        ? bm[g_geoNav.bookmarkIndex].name : "earthview";
                                    for (auto& c : stem) c = (char)tolower((unsigned char)c);
                                    for (auto& c : stem) if (c == ' ') c = '_';
                                    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                                        stem, cols, rows);
                                    XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT;
                                    strncpy_s(info.pathPrefix, prefix.c_str(), _TRUNCATE);
                                    XrResult cr = xr->pfnCaptureAtlasEXT(xr->session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::PostFlashRequest(hwnd);
                                    } else {
                                        LOG_WARN("xrCaptureAtlasEXT failed: 0x%x", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_EXT_atlas_capture not available");
                                }
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                uint32_t col = (uint32_t)eye % cols;
                                uint32_t row = (uint32_t)eye / cols;
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(col * renderW), (int32_t)(row * renderH)};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                if (useRig) {
                                    // Render-ready rig views: submit the per-eye world pose
                                    // (mono = the collapsed centroid) + the rig fov.
                                    int srcEye = monoMode ? 0 : eye;
                                    projectionViews[eye].pose.position = stereoViews[srcEye].eye_world;
                                    projectionViews[eye].pose.orientation = cameraPose.orientation;
                                    projectionViews[eye].fov = stereoViews[srcEye].fov;
                                } else {
                                    projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                                    projectionViews[eye].fov = monoMode ? rawViews[0].fov : rawViews[eye].fov;
                                }
                            }
                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render the HUD info-panel window-space layer. Body-only
                        // now (chrome buttons moved to the top-bar layer below).
                        // The TAB toggle hides the body via the `drawBody` flag;
                        // the layer footprint stays the aspect-locked left strip.
                        // Only render/acquire the HUD swapchain when the panel is
                        // visible — when hidden the layer is dropped entirely (true
                        // toggle), so we must NOT acquire its image this frame.
                        if (rendered && hud && xr->hasHudSwapchain && hudSwapchainImages &&
                            inputSnapshot.hudVisible) {
                            uint32_t hudImageIndex;
                            if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_EXT_win32_window_binding: ACTIVE (Vulkan + 3D Tiles)" :
                                    L"XR_EXT_win32_window_binding: NOT AVAILABLE";

                                // EarthView info + Google attribution (Map Tiles API
                                // policy, PRD §7.3). NOTE: the attribution must be ALWAYS
                                // visible; rendering it here in the Tab-toggled panel is a
                                // first-build placeholder — a dedicated always-on
                                // attribution window-space layer (Google logo bitmap +
                                // credits) is a Leia-validation follow-up.
                                std::wstring sceneText = L"\n--- EarthView ---";
                                if (g_tilesActive.load()) {
                                    size_t n = 0;
                                    const geo::Bookmark* bm = geo::bookmarks(&n);
                                    const AttributionInfo& attr = g_tileEngine.attribution();
                                    wchar_t ebuf[256];
                                    swprintf(ebuf, 256,
                                        L"\nCity: %S%s  Tiles: %d  GPU: %.0f MB\nDist: %.0f m  In-flight: %d",
                                        (n > 0 ? bm[g_geoNav.bookmarkIndex].name : "—"),
                                        g_geoNav.orbitAcquired ? L" (diorama)" : L"",
                                        g_tileRenderer.liveTileCount(),
                                        g_tileRenderer.gpuResidentMB(),
                                        g_geoNav.targetDist, attr.tilesInFlight);
                                    sceneText += ebuf;
                                    std::string credits;
                                    for (const auto& c : attr.credits) {
                                        if (!credits.empty()) credits += " \xc2\xb7 ";
                                        credits += c;
                                    }
                                    if (credits.empty()) credits = "Google";
                                    std::wstring wcred(credits.begin(), credits.end());
                                    sceneText += L"\nGoogle  " + wcred;
                                } else {
                                    sceneText += L"\nNo Google Map Tiles API key.\n"
                                                 L"Set GOOGLE_MAPS_API_KEY or put key=... in\n"
                                                 L"earthview.ini next to the exe, then relaunch.";
                                }
                                modeText += sceneText;

                                // Per-view extent for HUD display — same formula as the
                                // render path (window × view_scale of the current mode).
                                float dispScaleX, dispScaleY;
                                if (xr->renderingModeCount > 0) {
                                    uint32_t mode = xr->currentModeIndex;
                                    dispScaleX = xr->renderingModeScaleX[mode];
                                    dispScaleY = xr->renderingModeScaleY[mode];
                                } else {
                                    dispScaleX = xr->recommendedViewScaleX;
                                    dispScaleY = xr->recommendedViewScaleY;
                                }
                                uint32_t dispRenderW = (uint32_t)((double)windowW * dispScaleX);
                                uint32_t dispRenderH = (uint32_t)((double)windowH * dispScaleY);
                                if (dispRenderW == 0) dispRenderW = 1;
                                if (dispRenderH == 0) dispRenderH = 1;
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH, windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(xr->currentModeIndex, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeNames[xr->currentModeIndex] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[xr->currentModeIndex] : true,
                                    xr->renderingModeCount > 0 ? xr->renderingModeIsRequestable[xr->currentModeIndex] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, (uint32_t)eyeCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                float fwdY =  sinf(inputSnapshot.pitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(inputSnapshot.pitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ);
                                float hudM2v = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    hudM2v = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                std::wstring stereoText = FormatViewParams(
                                    inputSnapshot.viewParams.ipdFactor, inputSnapshot.viewParams.parallaxFactor,
                                    inputSnapshot.viewParams.perspectiveFactor, inputSnapshot.viewParams.scaleFactor);
                                {
                                    wchar_t vhBuf[96];
                                    int depthPct = (int)(inputSnapshot.viewParams.ipdFactor * 100.0f + 0.5f);
                                    const wchar_t* orbitLbl = inputSnapshot.animateEnabled
                                        ? (inputSnapshot.animationActive ? L"ON (running)" : L"ON (idle countdown)")
                                        : L"OFF";
                                    swprintf(vhBuf, 96, L"\nvHeight: %.3f  m2v: %.3f\nDepth/IPD: %d%%  Auto-Orbit: %s",
                                        inputSnapshot.viewParams.virtualDisplayHeight, hudM2v, depthPct, orbitLbl);
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = L"[WASD] Pan | [E/Q] Climb | [LMB-drag] Look | [Scroll] Dolly\n"
                                    L"[DblClick] Orbit | [B] City | [C] Fly | [Esc/Space] Release | [-/=] Depth\n"
                                    L"[M] Auto-Orbit | [V] Mode | [I] Capture | [Tab] HUD | [ESC] Quit";

                                // Chrome buttons no longer live here — they are a
                                // separate full-width top-bar window-space layer
                                // (see the button-bar block below). This layer is
                                // the info panel only, toggled by Tab via drawBody.
                                uint32_t srcRowPitch = 0;
                                const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                    cameraText, stereoText, helpText, {},
                                    /*drawBody=*/true,
                                    /*bodyAtBottom=*/true);
                                if (pixels) {
                                    const uint8_t* src = (const uint8_t*)pixels;
                                    uint8_t* dst = (uint8_t*)hudStagingMapped;
                                    for (uint32_t row = 0; row < hudHeight; row++) {
                                        memcpy(dst + row * hudWidth * 4, src + row * srcRowPitch, hudWidth * 4);
                                    }
                                    UnmapHud(*hud);
                                }

                                // Copy staging buffer to HUD swapchain image
                                VkCommandBufferAllocateInfo cmdAllocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                                cmdAllocInfo.commandPool = hudCmdPool;
                                cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                                cmdAllocInfo.commandBufferCount = 1;

                                VkCommandBuffer cmdBuf;
                                vkAllocateCommandBuffers(vkDevice, &cmdAllocInfo, &cmdBuf);

                                VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                vkBeginCommandBuffer(cmdBuf, &beginInfo);

                                VkImage hudImg = (*hudSwapchainImages)[hudImageIndex].image;

                                VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                                barrier.srcAccessMask = 0;
                                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                barrier.image = hudImg;
                                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                VkBufferImageCopy region = {};
                                region.bufferRowLength = hudWidth;
                                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                region.imageOffset = {0, 0, 0};
                                region.imageExtent = {hudWidth, hudHeight, 1};
                                vkCmdCopyBufferToImage(cmdBuf, hudStagingBuffer, hudImg,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                vkCmdPipelineBarrier(cmdBuf,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                                vkEndCommandBuffer(cmdBuf);

                                VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                                submitInfo.commandBufferCount = 1;
                                submitInfo.pCommandBuffers = &cmdBuf;
                                vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                                vkQueueWaitIdle(graphicsQueue);

                                vkFreeCommandBuffers(vkDevice, hudCmdPool, 1, &cmdBuf);

                                ReleaseHudSwapchainImage(*xr);
                                hudSubmitted = true;
                            }
                        }

                    }
                }

                // ── Top button bar: ONE full-width window-space layer holding all
                //    chrome buttons — Open + Mode packed left, Animation pinned
                //    right, transparent center. Always submitted (decoupled from
                //    the Tab-toggled HUD panel); the Animation pill is only added
                //    when the model has clips. Reuses the window-space-layer
                //    machinery (own swapchain / text renderer / staging) widened
                //    to a bar — see runtime issue #389. ──
                XrCompositionLayerWindowSpaceEXT barLayer = {};
                bool barLayerReady = false;
                if (g_animBtnReady && g_hasAnimBtnSwapchain) {
                    const float mxf = (g_windowWidth > 0)
                        ? (float)inputSnapshot.mouseX / (float)g_windowWidth : 0.0f;
                    const float myf = (g_windowHeight > 0)
                        ? (float)inputSnapshot.mouseY / (float)g_windowHeight : 0.0f;
                    const float barY = BTN_BAR_Y_FRACTION;
                    const float barH = BtnBarHeightFraction(windowW, windowH);
                    // Bar layer spans the full window width, so a button at
                    // window-x-fraction xf maps straight onto bar-texture-x. Pills
                    // fill ~70% of the bar height, vertically centered.
                    const float pillY = (float)BTN_BAR_TEX_H * 0.15f;
                    const float pillH = (float)BTN_BAR_TEX_H * 0.70f;
                    auto makeBtn = [&](float xf, float wf, const std::wstring& label) {
                        HudButton b;
                        b.label = label;
                        b.x = xf * (float)BTN_BAR_TEX_W;
                        b.y = pillY;
                        b.width = wf * (float)BTN_BAR_TEX_W;
                        b.height = pillH;
                        b.hovered = (mxf >= xf && mxf <= xf + wf &&
                                     myf >= barY && myf <= barY + barH);
                        return b;
                    };
                    std::vector<HudButton> barButtons;
                    // EarthView chrome: Mode (left) + City/bookmark (right). No Open
                    // pill (there is no model to load — tiles stream).
                    std::wstring modeLabel = L"Mode";
                    if (xr->renderingModeCount > 0 &&
                        xr->currentModeIndex < xr->renderingModeCount &&
                        xr->renderingModeNames[xr->currentModeIndex]) {
                        const char* nm = xr->renderingModeNames[xr->currentModeIndex];
                        modeLabel = L"Mode: " + std::wstring(nm, nm + strlen(nm));
                    }
                    // Surface workspace mode-lock so the user knows clicking Mode
                    // is a no-op in a locked workspace.
                    if (xr->renderingModeCount > 0 &&
                        xr->currentModeIndex < xr->renderingModeCount &&
                        !xr->renderingModeIsRequestable[xr->currentModeIndex]) {
                        modeLabel += L" [locked]";
                    }
                    barButtons.push_back(makeBtn(MODE_BTN_X_FRACTION, MODE_BTN_WIDTH_FRACTION, modeLabel));
                    // City/bookmark pill — always shown (cycles the city table).
                    barButtons.push_back(makeBtn(AnimBtnXFraction(), ANIM_BTN_WIDTH_FRACTION,
                                                 CurrentBookmarkLabel()));

                    uint32_t pitch = 0;
                    const void* px = RenderHudAndMap(g_animBtnHud, &pitch,
                        L"", L"", L"", L"", L"", L"", L"", L"",
                        barButtons, /*drawBody=*/false, /*bodyAtBottom=*/true);
                    uint32_t idx = 0;
                    if (px && AcquireWindowSpaceImage(g_animBtnSwapchain, idx)) {
                        uint8_t* dst = (uint8_t*)g_animBtnStagingMapped;
                        for (uint32_t row = 0; row < BTN_BAR_TEX_H; ++row)
                            memcpy(dst + row * BTN_BAR_TEX_W * 4,
                                   (const uint8_t*)px + row * pitch, BTN_BAR_TEX_W * 4);
                        UnmapHud(g_animBtnHud);

                        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                        cai.commandPool = g_animBtnCmdPool;
                        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                        cai.commandBufferCount = 1;
                        VkCommandBuffer cb = VK_NULL_HANDLE;
                        vkAllocateCommandBuffers(vkDevice, &cai, &cb);
                        VkCommandBufferBeginInfo bgi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                        bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                        vkBeginCommandBuffer(cb, &bgi);
                        VkImage img = g_animBtnSwapImages[idx].image;
                        VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        bar.image = img;
                        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
                        VkBufferImageCopy rg = {};
                        rg.bufferRowLength = BTN_BAR_TEX_W;
                        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        rg.imageOffset = {0, 0, 0};
                        rg.imageExtent = {BTN_BAR_TEX_W, BTN_BAR_TEX_H, 1};
                        vkCmdCopyBufferToImage(cb, g_animBtnStaging, img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
                        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
                        vkEndCommandBuffer(cb);
                        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                        vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE);
                        vkQueueWaitIdle(graphicsQueue);
                        vkFreeCommandBuffers(vkDevice, g_animBtnCmdPool, 1, &cb);
                        ReleaseWindowSpaceImage(g_animBtnSwapchain);

                        barLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
                        barLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        barLayer.subImage.swapchain = g_animBtnSwapchain.swapchain;
                        barLayer.subImage.imageRect.offset = {0, 0};
                        barLayer.subImage.imageRect.extent = {(int32_t)BTN_BAR_TEX_W, (int32_t)BTN_BAR_TEX_H};
                        barLayer.subImage.imageArrayIndex = 0;
                        barLayer.x = 0.0f;
                        barLayer.y = BTN_BAR_Y_FRACTION;
                        barLayer.width = 1.0f;
                        barLayer.height = barH;
                        barLayer.disparity = 0.0f;
                        barLayerReady = true;
                    } else if (px) {
                        UnmapHud(g_animBtnHud);
                    }
                }

                // Submit frame
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2;
                if (submitViewCount == 0) submitViewCount = 1;
                if (submitViewCount > 8) submitViewCount = 8;  // matches projectionViews[8] sizing
                if (rendered) {
                    // Always go through the window-space-layers path so the top
                    // button bar (an extra layer) shows. The HUD info-panel layer
                    // is gated by `submitHud = hudSubmitted`: when the panel is
                    // toggled off it was never rendered/acquired this frame, so we
                    // drop it entirely (true toggle, not a transparent layer).
                    // SOURCE_ALPHA on the projection layer: displayxr::common
                    // defaults projectionLayerFlags to 0, so pass the bit
                    // explicitly (the vendored copy hardcoded it; required for
                    // the Ctrl+T transparent-background path).
                    EndFrameWithWindowSpaceLayers(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, layerFracW, layerFracH, 0.0f, submitViewCount,
                        barLayerReady ? &barLayer : nullptr, barLayerReady ? 1u : 0u,
                        0, 0, -1, -1, /*submitHud=*/hudSubmitted,
                        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
            }
        } else {
            Sleep(100);
        }
    }

    if (renderCmdPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vkDevice, renderCmdPool, nullptr);

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

// Global crash handler
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exInfo) {
    const char* excName = "UNKNOWN";
    switch (exInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:      excName = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:        excName = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    excName = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   excName = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:         excName = "IN_PAGE_ERROR"; break;
        case EXCEPTION_GUARD_PAGE:            excName = "GUARD_PAGE"; break;
    }
    LOG_ERROR("!!! UNHANDLED EXCEPTION: %s (0x%08X) at address 0x%p !!!",
        excName, exInfo->ExceptionRecord->ExceptionCode,
        exInfo->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    // EarthView takes no CLI scene path — it streams tiles. lpCmdLine is ignored.
    (void)lpCmdLine;

    SetUnhandledExceptionFilter(CrashHandler);

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== DisplayXR EarthView (Vulkan) ===");

    HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        ShutdownLogging();
        return 1;
    }

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding DisplayXR install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
    }

    // Initialize OpenXR
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Try to load sim_display_set_output_mode
    {
        HMODULE rtModule = GetModuleHandleA("openxr_displayxr.dll");
        if (!rtModule) rtModule = GetModuleHandleA("openxr_displayxr");
        if (rtModule) {
            g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)GetProcAddress(rtModule, "sim_display_set_output_mode");
        }
        LOG_INFO("sim_display output mode: %s", g_pfnSetOutputMode ? "available" : "not available");
    }

    // Get Vulkan graphics requirements
    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get physical device
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get device extensions
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, vkInstance, physDevice, deviceExtensions, extensionStorage)) {
        LOG_ERROR("Failed to get Vulkan device extensions");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Find graphics queue family
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue family found");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        LOG_ERROR("Vulkan device creation failed");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Enumerate Vulkan swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);

        // Extract VkImage handles for render thread access
    }
    std::vector<VkImage> swapchainVkImages(swapchainImages.size());
    for (uint32_t i = 0; i < (uint32_t)swapchainImages.size(); i++) {
        swapchainVkImages[i] = swapchainImages[i].image;
    }

    // Initialize the tile renderer + cesium engine with the OpenXR Vulkan
    // device. Keyless (no API key) is a supported state: the app stays up on the
    // placeholder and the HUD explains how to supply a key (PRD §7.4).
    {
        uint32_t renderW = xr.swapchain.width;   // Full width — mono uses entire swapchain
        uint32_t renderH = xr.swapchain.height;
        if (!g_tileRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                               queueFamilyIndex, renderW, renderH)) {
            LOG_WARN("tile renderer init failed - scene rendering will not be available");
        } else {
            g_tilesActive.store(g_tileEngine.init(&g_tileRenderer));
            if (!g_tilesActive.load())
                LOG_WARN("No Google Map Tiles API key — set GOOGLE_MAPS_API_KEY or put key=... in earthview.ini");
        }
    }
    // Frame the default bookmark (Paris / Eiffel Tower).
    g_geoNav.frameBookmark(0);

    // Initialize HUD renderer
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    uint32_t hudFontBaseHeight = (uint32_t)(xr.swapchain.height * HUD_FONT_BASE_FRACTION);
    bool hudOk = InitializeHudRenderer(hudRenderer, hudWidth, hudHeight, hudFontBaseHeight);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain
    std::vector<XrSwapchainImageVulkanKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u Vulkan images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create HUD staging buffer
    VkBuffer hudStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory hudStagingMemory = VK_NULL_HANDLE;
    void* hudStagingMapped = nullptr;
    VkCommandPool hudCmdPool = VK_NULL_HANDLE;

    if (hudOk) {
        VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = (VkDeviceSize)hudWidth * hudHeight * 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(vkDevice, &bufInfo, nullptr, &hudStagingBuffer) != VK_SUCCESS) {
            LOG_WARN("Failed to create HUD staging buffer");
            hudOk = false;
        }

        if (hudOk) {
            VkMemoryRequirements memReqs;
            vkGetBufferMemoryRequirements(vkDevice, hudStagingBuffer, &memReqs);

            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memReqs.memoryTypeBits & (1 << i)) &&
                    (memProps.memoryTypes[i].propertyFlags &
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }

            if (memTypeIndex == UINT32_MAX) {
                LOG_WARN("No suitable memory type for HUD staging buffer");
                hudOk = false;
            } else {
                VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = memTypeIndex;
                vkAllocateMemory(vkDevice, &allocInfo, nullptr, &hudStagingMemory);
                vkBindBufferMemory(vkDevice, hudStagingBuffer, hudStagingMemory, 0);
                vkMapMemory(vkDevice, hudStagingMemory, 0, bufInfo.size, 0, &hudStagingMapped);
            }
        }

        if (hudOk) {
            VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &hudCmdPool) != VK_SUCCESS) {
                LOG_WARN("Failed to create HUD command pool");
                hudOk = false;
            }
        }

        if (hudOk) {
            LOG_INFO("HUD Vulkan resources created (%ux%u)", hudWidth, hudHeight);
        }
    }

    // ── Top button-bar window-space layer resources ──────────────────────────
    // Own swapchain + text renderer + staging + cmd pool for the full-width top
    // button bar (Open + Mode + Animation in one layer). Reuses the
    // g_animBtnSwapchain / g_animBtn* slots (named before the buttons were
    // unified into a bar). Only when window-space layers are available.
    if (hudOk && xr.hasHudSwapchain) {
        if (InitializeHudRenderer(g_animBtnHud, BTN_BAR_TEX_W, BTN_BAR_TEX_H, BTN_BAR_FONT_BASE) &&
            CreateWindowSpaceSwapchain(xr, g_animBtnSwapchain, BTN_BAR_TEX_W, BTN_BAR_TEX_H)) {
            g_hasAnimBtnSwapchain = true;
            uint32_t c = g_animBtnSwapchain.imageCount;
            g_animBtnSwapImages.resize(c, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(g_animBtnSwapchain.swapchain, c, &c,
                (XrSwapchainImageBaseHeader*)g_animBtnSwapImages.data());

            VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = (VkDeviceSize)BTN_BAR_TEX_W * BTN_BAR_TEX_H * 4;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            bool ok = vkCreateBuffer(vkDevice, &bi, nullptr, &g_animBtnStaging) == VK_SUCCESS;
            if (ok) {
                VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vkDevice, g_animBtnStaging, &mr);
                VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physDevice, &mp);
                uint32_t mt = UINT32_MAX;
                for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                    if ((mr.memoryTypeBits & (1u << i)) &&
                        (mp.memoryTypes[i].propertyFlags &
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
                if (mt != UINT32_MAX) {
                    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
                    vkAllocateMemory(vkDevice, &ai, nullptr, &g_animBtnStagingMem);
                    vkBindBufferMemory(vkDevice, g_animBtnStaging, g_animBtnStagingMem, 0);
                    vkMapMemory(vkDevice, g_animBtnStagingMem, 0, bi.size, 0, &g_animBtnStagingMapped);
                } else ok = false;
            }
            if (ok) {
                VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pci.queueFamilyIndex = queueFamilyIndex;
                ok = vkCreateCommandPool(vkDevice, &pci, nullptr, &g_animBtnCmdPool) == VK_SUCCESS;
            }
            g_animBtnReady = ok;
            LOG_INFO("Animation-button layer resources %s", ok ? "created" : "FAILED");
        } else {
            LOG_WARN("Animation-button layer init failed — button will not show");
        }
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASD=Pan  E/Q=Climb  LMB-drag=Look  Scroll=Dolly  DblClick=Orbit-acquire");
    LOG_INFO("          B=City  C=Orbit->Fly  Esc/Space=Release/Reset  M=Auto-Orbit  V=Mode");
    LOG_INFO("          I=Capture  Tab=HUD  F11=Fullscreen  ESC=Quit");
    LOG_INFO("");

    g_inputState.viewParams.virtualDisplayHeight = kDefaultVirtualDisplayHeightM;
    g_inputState.renderingModeCount = xr.renderingModeCount;
    // Align runtime active rendering mode with app's default (mode 1 = first 3D mode).
    // The main loop's dispatch picks this up on the first frame and calls
    // xrRequestDisplayRenderingModeEXT(1); the runtime event drives xr.currentModeIndex.
    g_inputState.absoluteRenderingModeRequested = 1;
    g_inputState.hudVisible = false;     // hidden by default; toggle with Tab
    g_inputState.animateEnabled = true;  // auto-orbit always on after 10 s idle
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
    }

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, vkDevice, graphicsQueue,
        queueFamilyIndex, vkInstance, physDevice,
        &swapchainVkImages,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
        hudStagingBuffer, hudStagingMapped, hudCmdPool,
        hudOk ? &hudSwapImages : nullptr,
        (VkCommandPool)VK_NULL_HANDLE, (std::vector<XrSwapchainImageVulkanKHR>*)nullptr,
        (uint32_t)0, (uint32_t)0);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    // Teardown order: the Tileset dtor (shutdown) free()s every live tile
    // through the renderer, so it MUST run before TileRenderer::cleanup().
    g_drawList.clear();
    g_tileEngine.shutdown();
    g_tileRenderer.cleanup();

    if (hudCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, hudCmdPool, nullptr);
    if (hudStagingBuffer != VK_NULL_HANDLE) {
        vkUnmapMemory(vkDevice, hudStagingMemory);
        vkDestroyBuffer(vkDevice, hudStagingBuffer, nullptr);
    }
    if (hudStagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, hudStagingMemory, nullptr);
    if (hudOk) CleanupHudRenderer(hudRenderer);

    // Animation-button layer resources.
    if (g_animBtnCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, g_animBtnCmdPool, nullptr);
    if (g_animBtnStaging != VK_NULL_HANDLE) {
        if (g_animBtnStagingMapped) vkUnmapMemory(vkDevice, g_animBtnStagingMem);
        vkDestroyBuffer(vkDevice, g_animBtnStaging, nullptr);
    }
    if (g_animBtnStagingMem != VK_NULL_HANDLE) vkFreeMemory(vkDevice, g_animBtnStagingMem, nullptr);
    if (g_animBtnReady) CleanupHudRenderer(g_animBtnHud);

    // App-owned animation-button swapchain: destroy before CleanupOpenXR tears
    // the session down (used to live in the vendored XrSessionManager cleanup).
    if (g_animBtnSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_animBtnSwapchain.swapchain);
        g_animBtnSwapchain.swapchain = XR_NULL_HANDLE;
        g_hasAnimBtnSwapchain = false;
    }

    g_xr = nullptr;
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}
