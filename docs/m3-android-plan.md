# M3 — Android port plan

Status: **planning**. Tracks the EarthView Android (NDK) leg, the third platform
after macOS (M1) and Windows (M2). Target device: **Lume Pad / NP02J** (Leia SR,
Adreno), 30+ fps.

This plan is grounded in the already-shipped Android legs of
`displayxr-demo-modelviewer/android/` and `displayxr-demo-mediaplayer/android/`
(the structural template) and in EarthView's own M1/M2 code. The OOP-runtime
pattern is ADR-025 (`displayxr-runtime`).

---

## 1. Why most of EarthView ports cleanly

EarthView's desktop legs are `_handle_vk` (Vulkan), and the entire scene layer in
`tiles_common/` is already platform-neutral Vulkan/C++:

- `tile_engine` (cesium-native streaming), `tile_renderer` (Vulkan draw),
  `geo_math` (globe camera + Kooima selection), `model_vulkan_utils`.
- The double-click **depth pick** is `vkCmdCopyImageToBuffer` on a
  `VK_FORMAT_D32_SFLOAT` target (`tiles_common/tile_renderer.cpp:1066`) — pure
  Vulkan, portable as-is.

These compile into the Android NDK build **by relative path**, no fork — exactly
how modelviewer's `android/` leg pulls in `model_common/*.cpp`.

The desktop window/UI legs (`windows/main.cpp` Win32 + `XR_EXT_win32_window_binding`,
`macos/main.mm` Cocoa) do **not** port; Android gets its own `android/` leg.

---

## 2. Structural template (copy from modelviewer)

Android is a **separate Gradle project** (not in the top-level CMake, which hard-
errors off-Apple/off-WIN32). Gradle's AGP `externalNativeBuild` drives
`android/src/main/cpp/CMakeLists.txt`, which compiles the shared `tiles_common/*`
sources directly.

Files to author (mostly verbatim copies with renames):

| Path | Source / action |
|---|---|
| repo-root `build.gradle` / `settings.gradle` / `gradle.properties` / `gradlew*` | copy from modelviewer; `include ':android'` |
| `android/build.gradle` | copy; change `applicationId`/`namespace` → `com.displayxr.earthview_vk_android`, `ndk { abiFilters 'arm64-v8a' }`, `implementation 'org.khronos.openxr:openxr_loader_for_android:1.1.41'` |
| `android/src/main/AndroidManifest.xml` | **copy the `<queries>` broker block + `WAKE_LOCK` + NativeActivity meta VERBATIM** — this is the OOP-discovery contract (omit → `XR_ERROR_RUNTIME_UNAVAILABLE` black screen, runtime#510). Only change `label`/`applicationId`/`android.app.lib_name`. |
| `android/src/main/java/.../MainActivity.kt` | adapt: keep rotation push, `dispatchTouchEvent→nativeOnTouch`, `wakeRuntime`, `watchForRuntimeUnavailable`. Replace the glTF SAF model-picker with EarthView's API-key dialog + (optional) location UI. |
| `android/src/main/cpp/CMakeLists.txt` | adapt: swap tinygltf → cesium-native + `tiles_common/*.cpp`; bake `tiles_common/shaders/*` to SPIR-V headers; link `native_app_glue`/`OpenXR`/`vulkan`/`android`/`log`. |
| `android/src/main/cpp/main.cpp` | the `cube_handle_vk_android` OpenXR/Vulkan/`android_main`/JNI/render-loop skeleton; swap `ModelRenderer` → `TileRenderer`/`TileEngine`; carry the focus/orbit model (§4). |
| `android/src/main/cpp/stb_impl.cpp`, `res/mipmap-*` icons | copy / `/make-app-logos` |

### OOP-runtime contract (ADR-025)
- The app is a **vendor-neutral OOP OpenXR client** — no CNSDK / SR SDK / plugin
  in the APK. The **DisplayXR runtime APK must be pre-installed** (owns the weave
  SurfaceView / MonadoView, the vendor DP, eye tracking).
- The app does **not** create or hand a surface to the runtime, and uses **no
  `XR_EXT_android_surface_binding`**. It renders into OpenXR swapchain images the
  runtime composites OOP. `app->window` is only a "can render" gate.
- `xrCreateInstance` chains `XrInstanceCreateInfoAndroidKHR{VM, activity}` and
  **retries on `XR_ERROR_RUNTIME_UNAVAILABLE`** (cold service start).
- Touch reaches native only via Java `dispatchTouchEvent → nativeOnTouch` JNI
  (MonadoView overlay swallows the NativeActivity input queue — runtime#499).
  Use standard JNI auto-binding (`Java_…` symbols), **not** `RegisterNatives`
  (unreliable under multi-classloader MonadoView).
- **No Android CI** initially (org policy: local `./gradlew :android:installDebug`).

---

## 3. New work the template can't cover

### Task 1 — cesium-native on the Android NDK (HIGH RISK, gates everything)
EarthView's reason for being. cesium-native v0.61.0 + ezvcpkg bootstraps
**Draco / KTX / curl / OpenSSL / libwebp**; `tile_renderer` links
`Cesium3DTilesSelection`, `Cesium3DTilesContent`, `CesiumCurl`. Never built for
`arm64-v8a` here. Unknowns:
- vcpkg `arm64-android` triplet resolution through ezvcpkg (ezvcpkg clones its own
  pinned vcpkg into `~/.ezvcpkg`; the Android toolchain must chainload correctly).
- A working **curl + TLS backend on Android** (OpenSSL via vcpkg, CA bundle).
- `tiles_common/` currently has **zero `__ANDROID__` gating** (unlike
  `model_common/`); expect to add guards for networking/TLS and file I/O
  (AAssetManager vs `std::ifstream`).

**De-risk this in isolation first** (standalone `arm64-v8a` configure + one tile
fetch over TLS) before any UI work. See §5 for the live spike status.

### Task 2 — port the focus/orbit camera model (MEDIUM)
The `g_focusActive` focus state machine + `dxr_view_rig_camera_to_display`
converter live **per-platform** in `windows/main.cpp` / `macos/main.mm`, **not**
in `tiles_common`. Android needs its own copy — **mirror the macOS reference**
(`macos/main.mm`), not the deprecated diorama `acquireOrbit` path, or it
re-introduces the exact fly↔orbit bug fixed on Windows in
`feat/windows-focus-orbit-parity` (PR #2). Carries: POI re-aim/reframe transition,
stereo-fullness glide, display-rig-via-converter, camera-orbit-around-POI,
convergence freeze, WASDQE/“fly” release (mapped to a gesture on Android).

### Task 3 — touch → globe camera (LOW)
modelviewer's `dispatchTouchEvent → nativeOnTouch → atomic camera state` plumbing
transfers 1:1. Remap gestures onto `geo_math`: 1-finger drag → pan/look,
2-finger pinch → dolly, double-tap → focus-acquire (the pick), long idle →
auto-orbit turntable.

### Task 4 — API-key UX (MEDIUM)
Desktop has a native first-run key card + re-entry shortcut, gating
`g_tilesActive` (keyless = no streaming). Android needs a **Kotlin `EditText`
dialog + `SharedPreferences`**, key passed to native via JNI, same keyless gate.
See `docs/api-key.md`.

### Task 5 — HUD + Google attribution (MEDIUM)
Desktop HUD = DWrite (win) / CoreText (mac) — not portable. Android: **stb_truetype**
(modelviewer's approach) or a Kotlin overlay. **The Google logo + "Google"
attribution string is legally required by the Map Tiles ToS** — cannot be
dropped; it must be visible in the rendered/overlaid output.

### Task 6 — NP02J performance budget (MEDIUM)
Roadmap M3: **reduced screen-space-error (SSE) + tile-byte budget** via
`TilesetOptions`, plus ASTC/BC texture transcode, to hit **30+ fps** on Adreno +
constrained VRAM. Tuning pass once it streams.

### Task 7 — packaging (LOW)
Launcher adaptive icons (`/make-app-logos`), `displayxr.json` manifest, final
manifest metadata.

---

## 4. Sequencing

1. **Spike cesium-native on NDK** (Task 1) standalone — **gate the effort on this.**
2. Scaffold Gradle/NDK harness from modelviewer; render a placeholder via the OOP
   runtime (proves manifest/broker/session bring-up on NP02J).
3. Wire `tiles_common` + cesium into the NDK build → first streamed globe frame.
4. Port focus/orbit (Task 2) + touch mapping (Task 3) from the macOS reference.
5. API-key dialog (Task 4) + HUD/attribution (Task 5).
6. Perf tuning (Task 6) → 30 fps on NP02J.
7. Icons/manifest (Task 7); on-device verification; flip `docs/roadmap.md` M3.

## 5. Risks & dependencies

- **cesium-native Android buildability (Task 1)** is the only item that can
  fundamentally change scope. Everything else is well-trodden by
  modelviewer/mediaplayer.
- DisplayXR runtime APK pre-installed on NP02J (weave + DP + eye tracking).
- `tiles_common/` needs `__ANDROID__` gating that `model_common/` already has.

## 6. Reuse vs author-fresh summary

**Inherited (compile by path, no fork):** `tiles_common/{tile_engine,tile_renderer,
geo_math,model_vulkan_utils}.cpp`, `tiles_common/shaders/*`, `common/leia_math.h`,
`openxr_includes/`, `third_party/cesium-native` source (fresh NDK build config).

**Author fresh:** the entire `android/` Gradle+NDK harness, `MainActivity.kt`,
`AndroidManifest.xml`, `cpp/main.cpp` (skeleton from `cube_handle_vk_android`),
`cpp/CMakeLists.txt`, the focus/orbit copy (§4 Task 2), the Kotlin API-key dialog,
the HUD/attribution backend.
