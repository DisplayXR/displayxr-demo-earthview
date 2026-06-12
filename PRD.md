# PRD — DisplayXR EarthView (Photorealistic 3D Tiles demo)

**Repo (target):** `DisplayXR/displayxr-demo-earthview`
**Status:** Approved — M0 in progress
**Author:** David (with Claude Code)
**Date:** 2026-06-11 (kickoff decisions folded in same day)

---

## 1. Summary

A cross-platform **streaming 3D city viewer** demo for the DisplayXR runtime,
built on **Google Map Tiles API — Photorealistic 3D Tiles** (the same dataset
behind Google's Unity-built *Immersive View for Android XR*). The app streams
OGC 3D Tiles (Draco-compressed glTF photogrammetry meshes). Navigation is
**camera-centric** by default (fly the full-scale world, maps-style);
double-clicking a point acquires it as orbit center and switches to a
**display-centric city diorama** — a real neighborhood at tabletop scale
inside the display's depth volume, the framing lightfield displays do best
and one a headset app can't deliver (§6.1).

It is an OpenXR *client* app (`_handle` class, Vulkan, one codebase for
Windows / macOS / Android), structurally cloned from
**`displayxr-demo-modelviewer`** — which already provides the Vulkan glTF mesh
rendering substrate, the three platform shells, and the freshest Android leg
(single-atlas #29, view-rig verified on NP02J 2026-06-10).

### Why not port Google's app
*Immersive View for Android XR* is a Unity 6 OpenXR app that hard-depends on
Android XR platform pieces (`XR_ANDROID_*` extensions, hand tracking, likely
server-side device gating). The underlying **data** however is public: one API
key gets any spec-compliant 3D Tiles renderer the full photorealistic globe.
We build the renderer; the experience is *designed for* an eye-tracked 3D
display instead of ported to it.

### Non-goals (v1)
- **No globe navigation / geocoding search.** Preset city bookmarks only
  (lat/lon table in the app). Free-text search is v1.x.
- **No offline caching** beyond the in-session memory cache (policy §8).
- **No 2D street-view / 2D map tiles.** Photorealistic 3D Tiles only.
- **No terrain/imagery fallback for uncovered areas.** Bookmarks point at
  covered cities (coverage list is large: most major metros).
- **No hand/gesture input.** Desktop mouse+keyboard; Android auto-orbit
  (touch is v1.x).

---

## 2. The data source (what we consume)

| Item | Value |
|---|---|
| Entry point | `GET https://tile.googleapis.com/v1/3dtiles/root.json?key=API_KEY` |
| Format | OGC **3D Tiles** BVH; tile content = **glTF/GLB**, **Draco**-compressed geometry, JPEG textures |
| Session | Root response yields a `session` id; every subsequent tile/tileset request appends `?session=…&key=…` (child URIs come back relative, without the key) |
| Refinement | Standard 3D Tiles traversal — bounding volumes + `geometricError` → screen-space-error (SSE) driven REPLACE refinement |
| Billing | Per **root tileset request**: 1,000 free, then $6.00/1000; 10k/day quota cap. One root request covers ~3 h of unlimited tile fetches → demo usage ≈ free (one root request per app launch) |
| Attribution (mandatory) | Aggregate `asset.copyright` from every *visible* tile, sort by frequency, display on screen + **Google logo**. Non-negotiable per Map Tiles API policies |

Docs: [3D Tiles overview](https://developers.google.com/maps/documentation/tile/3d-tiles),
[bring-your-own-renderer](https://developers.google.com/maps/documentation/tile/create-renderer),
[usage & billing](https://developers.google.com/maps/documentation/tile/usage-and-billing),
[policies](https://developers.google.com/maps/documentation/tile/policies).

---

## 3. Target platforms & graphics binding

| Platform | App name | OpenXR binding | Window | v1? |
|---|---|---|---|---|
| Windows | `earthview_handle_vk_win` | Vulkan | Win32 HWND via `XR_EXT_win32_window_binding` | ✅ |
| macOS | `earthview_handle_vk_macos` | Vulkan (MoltenVK / CAMetalLayer) | NSWindow via `XR_EXT_cocoa_window_binding` | ✅ |
| Android | `earthview` (NativeActivity) | Vulkan | OOP runtime via broker/service (ADR-025) | ✅ (after desktop) |

Same single-Vulkan-codebase rationale as the mediaplayer PRD: `vk_native` is
maintained on all three platforms, `cube_handle_vk_macos` and the modelviewer
Android leg are working proof points.

---

## 4. Build vs buy: tile streaming engine

**Decision: [cesium-native](https://github.com/CesiumGS/cesium-native)**
(Apache-2.0, C++17, CMake). It powers Cesium for Unreal/Unity/Omniverse, has
explicit Google Photorealistic 3D Tiles support (session-parameter handling,
response caching, per-frame **credit aggregation** that maps 1:1 onto the
attribution requirement), and handles traversal/SSE selection, async loading,
Draco decode, and glTF parsing. Proven on Android (Cesium for Unity on Quest)
and in raw-Vulkan engines (`vsgCs`).

We implement its three integration interfaces:

| Interface | Our implementation |
|---|---|
| `IAssetAccessor` | libcurl HTTP/2 client (platform TLS: Schannel / SecureTransport / BoringSSL-or-mbedTLS on Android) |
| `ITaskProcessor` | small thread pool (std::thread; N = hw_concurrency−2) |
| `IPrepareRendererResources` | upload `CesiumGltf::Model` → Vulkan vertex/index buffers + sampled images (reusing `model_vulkan_utils`) |

**Fallback** (if cesium-native fights the Android NDK build): hand-rolled
minimal 3D Tiles client — root.json traversal + cgltf + google/draco only.
~2–3 weeks extra; decision gate is milestone M0.

---

## 5. What we take from the modelviewer template

Clone `displayxr-demo-modelviewer`, then:

**Keep (unchanged or lightly adapted)**
- Repo skeleton: top-level CMake dispatch, `common/` (displayxr-common
  FetchContent — HUD, input, logging, display-info query), `windows/`,
  `macos/`, `android/` (gradle + NDK, openxr_loader AAR), `installer/`,
  `scripts/`, `openxr_includes/`.
- All three **app shells**: window creation, OpenXR instance/session/swapchain
  skeleton, `XR_EXT_view_rig` + `XR_EXT_display_info` + atlas-capture wiring,
  frame loop, mode handling.
- `model_common/model_vulkan_utils.{h,cpp}` — buffer/image upload helpers.

**Replace**
- `model_loader_*.{cpp,h}` (tinygltf/OBJ/FBX/STL/USD backends) → cesium-native
  is the sole scene source. (Optional v1.x: keep the glTF backend behind a key
  as a "open local .glb" bonus mode.)
- `model_renderer` scene management (single static model + auto-fit) →
  **tile pool**: per-tile GPU resources created/destroyed continuously as
  cesium-native loads/expires tiles; per-frame draw list = cesium-native's
  selected-tiles result.
- PBR/IBL shader stack (`pbr.*`, `irradiance`, `prefilter`, `brdf_lut`,
  `skybox`) → **unlit textured** vert/frag (photogrammetry has baked
  lighting; also cheaper on Adreno). Keep depth-tested opaque pass + sRGB
  swapchain (INV-4.6).

**New (`tiles_common/`)**
- `tile_engine.{h,cpp}` — `Cesium3DTilesSelection::Tileset` wrapper, the three
  interface impls, camera→`ViewState` conversion, credit/attribution feed.
- `tile_renderer.{h,cpp}` — tile pool + draw-list rendering.
- `geo_math.{h,cpp}` — ECEF↔ENU frames, diorama transform (double precision).

```
displayxr-demo-earthview/
├── CMakeLists.txt
├── common/                  # displayxr-common FetchContent (as modelviewer)
├── tiles_common/            # NEW: cesium-native glue + VK tile renderer
│   ├── tile_engine.{h,cpp}
│   ├── tile_renderer.{h,cpp}
│   ├── geo_math.{h,cpp}
│   ├── model_vulkan_utils.{h,cpp}   # carried over from model_common
│   └── shaders/ {tile.vert, tile.frag}
├── windows/   macos/   android/     # shells carried from modelviewer
├── openxr_includes/
├── installer/  scripts/
└── PRD.md
```

---

## 6. Rendering & DisplayXR integration

### 6.1 Coordinate model (the one genuinely new problem)
3D Tiles are in **ECEF** (earth-centered, meter units, ~6.4e6 magnitudes —
unrenderable in float32). Standard globe-renderer solution, applied at
diorama scale:

1. Pick an **anchor** (bookmark lat/lon/alt) → ECEF point `A` + local ENU
   (east-north-up) frame at `A`.
2. CPU-side **doubles**: world matrix per tile = `diorama_transform ×
   ENU(A)⁻¹ × tile_ECEF_transform`, composed in double, converted to float
   *after* subtraction of the anchor (relative-to-center / RTC).
3. **Two view models** (decided at kickoff 2026-06-11):
   - **Camera-centric (default).** The app flies a free camera through the
     full-scale world (maps-style navigation); the view rig follows the
     camera (rig pose = camera pose). Stereo scale (baseline / convergence,
     i.e. an effective world scale for parallax) is tied to camera altitude
     so depth stays comfortable from street level to city overview.
   - **Display-centric diorama (acquired).** **Double-click** raycasts the
     scene to pick a point → acquires it as orbit center → transitions to a
     display-centric rig: the picked neighborhood framed at diorama scale
     `s` (~1:3000, adjustable) inside the display's depth volume — the
     gauss-demo interaction pattern. Exact transition UX is exploratory;
     v1 ships camera-centric as default and treats the diorama rig as the
     showcase mode.
   Depth-volume mapping uses the nominal viewer distance from
   `XR_EXT_display_info` in both models.

### 6.2 Stereo + multiview invariants (from `docs/guides/displayxr-app-rules.md`)
- **One worst-case swapchain at init** — canvas × recommended view scales,
  per-mode tiles via `subImage.imageRect`, `XrView views[8]`, never resized
  (INV-3.1 / INV-4.3; the multiview-tiling invariant).
- **`XR_EXT_view_rig`** poses/FOVs used directly — no app-side Kooima math
  (W7 pattern, same as modelviewer/gauss).
- **Tile selection runs once per frame** with a synthetic **center-eye**
  camera (midpoint of the located views, union FOV, SSE threshold computed
  from one view tile's canvas resolution, slightly tightened ~0.9× to be
  conservative for both eyes). Both views draw the same selected set —
  no per-eye traversal, no per-eye popping.
- 2D mode (single view) renders the same scene mono — free with the above.
- sRGB swapchain format; unlit shader writes sRGB-encoded (INV-4.6).
- `xrCaptureAtlasEXT` for captures (INV-7.x); frame loop gated on
  session-running at READY (F-1).

### 6.3 Frame budget
Plain opaque rasterization — far more tractable than the gauss compute
pipeline. Tunables: SSE threshold, max loaded-tile bytes (texture cache cap),
max concurrent downloads. Desktop targets 60 fps trivially; Android target is
**30+ fps on NP02J** with a reduced SSE + 256 MB tile budget (JPEG decoded to
RGBA8 is the pressure point; ASTC/BC transcode is a v1.x optimization).

---

## 7. UX

### 7.1 Navigation (desktop)

Default is **camera-centric** free navigation (§6.1); double-click acquires
an orbit center and (exploratory) switches to the display-centric diorama rig.

| Input | Action |
|---|---|
| Left-drag | look / orbit (orbit center if acquired, else free-look) |
| Right-drag / WASD | translate camera (pan across the city) |
| Scroll | move toward/away from view target; stereo scale follows altitude |
| **Double-click** | raycast pick → acquire orbit center → display-centric diorama rig (gauss-demo pattern) |
| Esc / Space | release orbit center, back to camera-centric; reset to bookmark framing |
| `B` / HUD | cycle city bookmarks (**Paris default**, SF, NYC, Tokyo, Sydney, …) with a short fly-over interpolation |
| V / 0–3, T, Tab, I | runtime-standard: mode cycle/select, eye-tracking toggle, HUD, capture — as in modelviewer |

### 7.2 Android (v1)
Auto-orbit the current bookmark (gauss-demo pattern); bookmark cycle via a
window-space UI bar later (modelviewer's #506 pattern). Touch orbit is v1.x.

### 7.3 HUD & attribution (always on — policy)
Bottom strip rendered via displayxr-common HUD / window-space layer:
**Google logo** (bundled asset per policy page) + aggregated, frequency-sorted
`asset.copyright` strings from cesium-native's per-frame credit list +
loading-state indicator (tiles in flight / resident MB).

### 7.4 First-run / no key
If no API key is found: friendly HUD card explaining how to get a Map Tiles
API key and where to put it. App stays up (empty starfield/grid), no crash.

---

## 8. API key, quota & policy compliance

- **Key supply:** `GOOGLE_MAPS_API_KEY` env var, else `earthview.ini` next to
  the exe (gitignored), else Android `local.properties`-injected
  BuildConfig. **Never committed, never baked into installers.**
- **Cost envelope:** one `root.json` request per launch (session ≈ 3 h);
  1,000 free root requests/mo covers all dev + demo usage. No per-tile
  billing.
- **Caching:** cesium-native in-memory + per-session response cache only; no
  persistent tile store (policy forbids offline retention). Session id is
  process-lifetime.
- **Attribution:** §7.3; treated as a launch-blocking requirement, verified
  before any public release.
- **ToS note:** display of the data must remain "as provided" (no terrain
  edits); diorama scaling/tilt is a camera/presentation transform, which is
  fine.

---

## 9. Milestones

| M | Deliverable | Gate |
|---|---|---|
| **M0 — spike** ✅ **DONE 2026-06-11** | Headless `spike/` (no window, no GPU): cesium-native v0.61 builds via ezvcpkg, fetches root.json (key+session), SSE tile selection, Draco/glTF/JPEG decode, software-rasterizes the Eiffel Tower neighborhood to PNG + prints Google attribution | **cesium-native build gate PASSED** on macOS arm64 — fallback client not needed. Three integration facts captured (see below) |
| **M1 — macOS XR** | `earthview_handle_vk_macos`: stereo via view rig on sim display, diorama navigation, bookmarks, attribution HUD, linter-clean | §6.2 invariants verified; atlas capture shows correct L/R parallax |
| **M2 — Windows** | `earthview_handle_vk_win` on real Leia panel; installer + run scripts; `/make-app-logos` manifest + icons | Visual check on hardware; weave correctness; 60 fps |
| **M3 — Android** | NDK leg on NP02J via OOP runtime; reduced SSE/texture budget; auto-orbit | 30+ fps, no OOM across bookmark cycle; rig_applied=1 |
| **M4 — release** | `publish-demo-earthview.yml`, versions.json wiring, `/dxr-release earthview` support, README + coverage/bookmark list | First tagged release; bundle inclusion decision deferred |

M0 is deliberately tiny because it retires the only existential risk
(cesium-native build complexity). **It passed first session** — Paris renders.

### M0 integration facts (carry into M1, save M1 from re-discovering them)
1. **`Cesium3DTilesContent::registerAllTileContentTypes()` is mandatory** at
   startup. Without it the `GltfConverters` registry is empty, every `.glb`
   tile falls through to the JSON tileset parser ("Invalid value at byte
   offset 0"), and geometry silently never loads — tiles *select* but have 0
   render content. Highest-value gotcha.
2. **Y-up → Z-up.** 3D Tiles content is glTF (Y-up); the ellipsoid/ECEF camera
   frame is Z-up. `Model::forEachPrimitiveInScene` does **not** bake the
   conversion. Premultiply the rotation `(x,y,z)→(x,-z,y)` onto
   `Tile::getTransform()`.
3. **Key propagation is automatic.** cesium-native carries the root URL's
   `key=` to child requests itself; an injecting `IAssetAccessor` is only a
   safeguard, not required.
4. **Build:** ezvcpkg pulls Draco/KTX/TLS and bootstraps in ~11 min first
   configure; `registerAllTileContentTypes()` lives in the
   `Cesium3DTilesContent` target (link it alongside `Cesium3DTilesSelection`
   + `CesiumCurl`).
5. **Settle detection:** "queues empty" alone fires before any mesh loads
   (upper P3DT tiles are external-tileset pointers). Wait until
   `tilesToRenderThisFrame` contains tiles with `isRenderContent()`.

### M0 follow-up tests (RTC precision + moving camera) — both PASS

Run via `earthview_m0` (static + RTC/diorama report) and `earthview_m0 orbit`
(3× revolution churn + memory).

6. **Float32 RTC is safe at city scale — the §6.1 risk is retired.** Projecting
   each vertex via full-double vs the M1 GPU path (subtract anchor in double →
   cast remainder to `float32` → multiply by a `float32` anchor-relative MVP)
   diverges by **max 0.0009 px** over 8.5k samples (threshold 0.5). So M1 can
   upload anchor-relative float positions + a float anchor-folded MVP with no
   jitter. Anchor = camera target; rebuild the relative MVP per frame in double,
   cast once.
7. **Diorama scale sanity:** at 1:3000 the loaded Paris extent
   (~5.1×8.0×4.5 km) maps to ~**1.7×2.7×1.5 m**. That's the *full loaded set*;
   M1 will crop to a framed neighborhood and/or use a deeper ratio (~1:8000) to
   fit a ~0.3 m display depth budget. Knob, not blocker.
8. **No leak under continuous navigation.** Orbiting Paris for 3 revolutions
   (36 frames) churns 55–72 render tiles/frame with no crash; RSS climbs during
   rev 1 as the cache fills, then **plateaus exactly at 517 MB** and stays flat
   while revisiting viewpoints (0.0 MB growth). cesium's cache is self-capping —
   M1 can tune the cap via `TilesetOptions` for the NP02J budget (M3).

---

## 10. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| cesium-native dependency closure (draco, ktx, s2, openssl-class TLS) hard to build on Android NDK | **High** | M0 gate + hand-rolled fallback (§4); precedents: Cesium for Unity ships Android, vsgCs ships raw Vulkan |
| libcurl + TLS on three platforms | Med | Use platform TLS backends; curl is already vcpkg/Homebrew/NDK-prebuilt territory |
| VRAM/decode pressure on NP02J (RGBA8 textures) | Med | Tile-byte budget + reduced SSE; ASTC transcode v1.x |
| Float precision artifacts (jitter at city scale) | Med | RTC doubles (§6.1) — solved problem, just must be done from day one |
| Google changes tile format details | Low | We consume via cesium-native, which tracks the format upstream |
| Quota surprise (e.g. CI/devs hammering root requests) | Low | One root request per launch by design; key is per-dev |

---

## 11. Open questions

1. ~~App id / display name~~ — **RESOLVED: `earthview` / "EarthView"**
   (kickoff 2026-06-11).
2. ~~Default bookmark~~ — **RESOLVED: Paris** (Eiffel Tower framing)
   (kickoff 2026-06-11).
3. Does the runtime's nominal-viewer depth budget API give us enough to
   auto-fit diorama depth, or do we hard-tune per display class for v1?
4. v1.x: keep modelviewer's local-glTF backend as a hidden "open .glb" mode?
   (Near-free since `model_vulkan_utils` is shared.)
5. Bundle inclusion (meta-installer) — defer to after M4, same as other demos.
