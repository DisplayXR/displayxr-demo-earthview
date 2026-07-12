# M1 Kickoff — `earthview_handle_vk_macos`

**Goal:** first real DisplayXR app — stereo Google Photorealistic 3D Tiles on
the macOS sim display, cloned from the modelviewer template, with cesium-native
as the scene source. M0 (headless) is done and proved the engine layer; M1 puts
it on a GPU + OpenXR.

Read first, in order:
1. `PRD.md` §5 (what to take from modelviewer), §6 (rendering + DisplayXR
   invariants), §7 (UX), §9 (milestones + **the 8 M0 integration facts**).
2. `spike/main.cpp` — the **working reference implementation** of the cesium
   integration (accessor, tileset, settle, tile iteration, Y-up→Z-up, RTC).
   M1 reuses this logic; only the rasterizer becomes a Vulkan pass.
3. Memory `project-earthview-demo` — project state + condensed facts.

## Starting state (this machine)
- Repo: `~/Documents/GitHub/displayxr-demo-earthview` (git, M0 committed).
- cesium-native **v0.61.0** cloned at `third_party/cesium-native` (gitignored;
  already built once via ezvcpkg — `spike/build` works). Link targets:
  `Cesium3DTilesSelection` + `Cesium3DTilesContent` + `CesiumCurl`.
- API key in `earthview.ini` (`key=...`, gitignored) — Map Tiles API on the
  immersityAI GCP project.
- Template source: `~/Documents/GitHub/displayxr-demo-modelviewer` — freshest
  Android leg, Vulkan glTF PBR renderer in `model_common/`, three app shells
  (`windows/ macos/ android/`), `common/` = displayxr-common FetchContent.

## Non-negotiable integration facts (from M0 — do not rediscover)
1. Call `Cesium3DTilesContent::registerAllTileContentTypes()` once at startup,
   or glb tiles silently never load (they fall through to the JSON parser).
2. glTF is Y-up, ECEF is Z-up. Premultiply `(x,y,z)→(x,-z,y)` onto
   `Tile::getTransform()`; `forEachPrimitiveInScene` does NOT bake it.
3. cesium-native propagates the root `key=` to children itself; the
   `KeyInjectingAccessor` is a safeguard only.
4. Settle on tiles with `isRenderContent()`, not empty queues.
5. **Float32 RTC is proven safe (0.0009 px):** upload anchor-relative float
   positions + a float anchor-folded MVP. Anchor = camera/orbit target; rebuild
   the relative MVP per frame in double, cast once. (PRD §9.6)
6. Memory self-caps (~517 MB orbiting Paris, no leak); tune via
   `TilesetOptions` later for Android (M3). (PRD §9.8)

## M1 build steps (suggested order)
1. **Scaffold:** clone modelviewer repo structure into earthview —
   `common/`, `macos/`, top-level CMake dispatch, `openxr_includes/`,
   `installer/`, `scripts/`. Keep `model_common/model_vulkan_utils.{h,cpp}`.
   Drop the loader backends (`model_loader_*`) and PBR/IBL shaders.
2. **`tiles_common/`** (new shared lib): port the spike's cesium glue —
   `tile_engine.{h,cpp}` (Tileset wrapper, KeyInjectingAccessor,
   ThreadTaskProcessor, ViewState build, settle, credit feed) and
   `tile_renderer.{h,cpp}` (per-tile VkBuffer/VkImage upload via
   `model_vulkan_utils`, a tile pool keyed by Tile*, draw-list build).
   Unlit textured `tile.vert`/`tile.frag` (sRGB, baked lighting — no PBR).
   Implement `IPrepareRendererResources` to upload glTF → Vulkan on load and
   free on unload (replaces the spike's software rasterizer).
3. **Coordinate model:** `geo_math.{h,cpp}` — ECEF anchor + RTC doubles (PRD
   §6.1). Default bookmark **Paris / Eiffel Tower**; camera-centric nav default.
4. **OpenXR wiring (from modelviewer macos shell):** Cocoa NSWindow +
   `XR_DXR_cocoa_window_binding`, one worst-case swapchain (INV-3.1/4.3),
   per-mode tiles via `subImage.imageRect`, `XR_DXR_view_rig` poses/FOVs used
   directly. **Select tiles once/frame with a center-eye camera** (~0.9× SSE),
   draw the same set into both views (PRD §6.2).
5. **Attribution HUD (policy, launch-blocking):** Google logo + frequency-sorted
   `asset.copyright` from cesium's credit system, via displayxr-common HUD.
   (M0 only surfaced "Google" — implement the real per-tile aggregation.)
6. **Lint + verify:** `python3 <runtime>/scripts/check_displayxr_app.py` clean;
   capture the atlas (macOS `/tmp/dxr_atlas` trigger or in-app `stbi_write_png`,
   see runtime CLAUDE.md "macOS test-app pixels") and confirm L/R parallax.

## Verify-path note
M1 needs the DisplayXR runtime dev build + sim-display plug-in runnable on this
machine (we're in the runtime repo; see its CLAUDE.md run scripts +
`XRT_PLUGIN_SEARCH_PATH`). Confirm that early — it's the only unchecked
prerequisite.

## Definition of done (M1)
`earthview_handle_vk_macos` renders the Paris diorama in stereo on the sim
display, camera-centric navigation works, double-click acquires an orbit center
(gauss-style; display-centric rig is the showcase mode), attribution HUD always
on, linter clean, atlas capture shows correct per-eye parallax.
