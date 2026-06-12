# CLAUDE.md — displayxr-demo-earthview

Guidance for Claude Code when working in this repository.

## What this is

**EarthView** — a streaming 3D city viewer for the DisplayXR runtime, built on
Google Map Tiles API **Photorealistic 3D Tiles** via **cesium-native**.
OpenXR `_handle`-class client app, Vulkan, structurally cloned from
`displayxr-demo-modelviewer`. Spec: `PRD.md`. Milestones: PRD §9
(M0 spike ✅ → M1 macOS ✅/in-progress → M2 Windows → M3 Android → M4 release).

## Layout

```
CMakeLists.txt        # root dispatch: cesium-native + common + tiles_common + macos
common/               # displayxr-common FetchContent (HUD, input, leia_math)
tiles_common/         # scene layer: cesium glue + VK tile renderer + geo math
│   tile_engine.{h,cpp}      # Tileset wrapper, accessor, credits, ViewState
│   tile_renderer.{h,cpp}    # IPrepareRendererResources + unlit draw
│   geo_math.{h,cpp}         # ECEF/ENU doubles, RTC, GeoCamera, bookmarks
│   model_vulkan_utils.{h,cpp} # buffer/image upload (from modelviewer, verbatim)
│   shaders/tile.{vert,frag}   # unlit textured, sRGB (glslangValidator → .h)
macos/                # Cocoa + MoltenVK + OpenXR shell (from modelviewer)
openxr_includes/      # vendored OpenXR + DisplayXR extension headers
spike/                # M0 headless proof — STANDALONE project, not in root build
third_party/cesium-native/  # v0.61.0 clone (gitignored; see README)
```

`spike/main.cpp` is the canonical reference for the cesium integration
(accessor, settle, tile iteration, Y-up→Z-up, RTC proof). Don't delete it.

## Non-negotiable integration facts (M0-proven — do not rediscover)

1. `Cesium3DTilesContent::registerAllTileContentTypes()` once at startup or
   glb tiles silently never load.
2. glTF is Y-up, ECEF is Z-up: premultiply `(x,y,z)→(x,-z,y)` onto
   `Tile::getTransform()`; `forEachPrimitiveInScene` does NOT bake it.
3. cesium-native propagates the root `key=` to children itself.
4. Settle = tiles with `isRenderContent()`, not empty queues.
5. Float32 RTC is safe (0.0009 px): tile-local float verts + per-tile double
   matrix product cast to float once per frame. Anchor changes never touch
   GPU memory.
6. Memory self-caps (~517 MB Paris); tune via `TilesetOptions` for Android.
7. v0.61 `IPrepareRendererResources`: `prepareInLoadThread` = worker thread,
   CPU-only staging; `prepareInMainThread` + `free` are **always on the
   `Tileset::updateView` thread** → all Vulkan on the render thread, no locks.

## API key (never commit, never bake into installers)

`GOOGLE_MAPS_API_KEY` env var, else `key=...` in `earthview.ini` at repo root
(gitignored). One root.json request per launch ≈ free tier. Attribution HUD
(Google logo + per-tile copyright) is **launch-blocking policy** — always on.

## Build & run (macOS)

```bash
# one-time: clone cesium-native v0.61.0 into third_party/ (see README)
./scripts/build_macos.sh          # loader + cmake build → build/macos/earthview_handle_vk_macos
./scripts/run_macos_dev.sh        # runs against the runtime dev package
                                  # (~/Documents/GitHub/displayxr-runtime/_package/DisplayXR-macOS)
                                  # or an installed runtime; DISPLAYXR_RUNTIME_DIR overrides
```

Lint before calling work done:
`python3 ~/Documents/GitHub/displayxr-runtime/scripts/check_displayxr_app.py .`

Atlas capture for parallax checks: the **I key** (`xrCaptureAtlasEXT`) — the
macOS vk_native compositor has no `/tmp/dxr_atlas_trigger`.

## Conventions

- DisplayXR app invariants: `displayxr-runtime/docs/guides/displayxr-app-rules.md`
  (one worst-case swapchain, per-mode tiles via `subImage.imageRect`,
  `XR_EXT_view_rig` poses used directly, sRGB).
- Tile selection runs ONCE per frame with a center-eye camera; both views draw
  the same selected set.
- Doubles live in `geo_math` only; the frame loop sees per-tile `float[16]`.
- Releases: `/dxr-release earthview vX.Y.Z` from the runtime hub repo (M4).
