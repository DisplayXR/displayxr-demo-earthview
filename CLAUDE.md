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
common/               # displayxr-common FetchContent (HUD, input, dxr_math)
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

### Build (Linux — build-green only, issue #19)

```bash
# one-time cesium-native clone happens automatically inside the script
./scripts/build_linux.sh          # loader (release-1.1.43) + cmake → build/linux/earthview_handle_vk_linux
./scripts/run_earthview_linux.sh  # dev run vs a Linux runtime (on-screen pass only — Phase-3 gated)
```

`linux/main.cpp` is a **HOSTED-NULL, reduced harness** — Vulkan (system
`libvulkan`) + OpenXR session + cesium tile streaming, with **no app-provided
window** (the runtime self-creates one) and no HUD/input/MCP. This is
BUILD-GREEN scope: `.github/workflows/build-linux.yml` (mirrors mediaplayer's,
NOT a required check; triggers on `linux*` branches + manual dispatch) compiles
the cross-platform scene layer on `ubuntu-latest`. **On-screen validation is a
separate pass**, gated on the runtime's Linux Phase 1b + a GPU + an X server.
The faithful app-window arm is `XR_EXT_xlib_window_binding` (runtime Phase 3a) —
see the `TODO(Phase 3)` in `linux/main.cpp` and the runtime repo's
`docs/guides/linux-demo-port.md`. The OpenXR **loader pin is `1.1.43`** (equal in
`scripts/build_linux.sh` + `linux/CMakeLists.txt` FetchContent fallback + CI);
the vendored `openxr_includes/` headers are newer (1.1.51) — that drift is
expected on Linux, unlike the macOS/Windows legs which pin the loader to the
header rev.

### Self-capture for autonomous verification (macOS vk_native)
`xrCaptureAtlasEXT` (the **I** key) is unreliable on the macOS vk_native
runtime and `screencapture` needs TCC the agent lacks. Use the built-in
**in-app PNG dump** instead:
```bash
rm -f /tmp/earthview_dump.png
DXR_DUMP=400 ./scripts/run_macos_dev.sh > /tmp/log 2>&1 &   # dump eye 0 at frame N
until [ -f /tmp/earthview_dump.png ] || ! pgrep -qf earthview_handle; do sleep 1; done
pkill -f earthview_handle      # then read /tmp/earthview_dump.png
```
`TileRenderer::dumpColorTarget()` (`tiles_common/tile_renderer.cpp`) copies the
internal color target → `stbi_write_png` (impl comes from displayxr-common —
forward-declare, never re-`#define`). The dump is **mono eye 0** (no anaglyph
overlay → clearer for geometry/coverage debugging). Pair with the per-frame
`tiles:` LOG_INFO (scale `s`, `targetDist`, near/far, alt, drawn/skip counts).
Frame 400 ≈ 10 s (cold); use larger N for a warmed view. `EV_ELEV` / `EV_DIST`
env vars force a bookmark framing to reproduce a reported pose.

## Conventions

- DisplayXR app invariants: `displayxr-runtime/docs/guides/displayxr-app-rules.md`
  (one worst-case swapchain, per-mode tiles via `subImage.imageRect`,
  `XR_EXT_view_rig` poses used directly, sRGB).
- Tile selection runs ONCE per frame with a center-eye camera; both views draw
  the same selected set.
- Doubles live in `geo_math` only; the frame loop sees per-tile `float[16]`.
- Releases: `/dxr-release earthview vX.Y.Z` from the runtime hub repo (M4).
- **Dev-build dependency rule (don't regress).** `scripts/build_windows.bat` +
  `scripts/build_macos.sh` **auto-provision the OpenXR loader**, pinned to the
  vendored `openxr_includes/` spec rev (`XR_CURRENT_API_VERSION`, currently
  1.1.51) — never hardcode an SDK path (`C:/dev/openxr_sdk`, `C:/VulkanSDK/<ver>`);
  Vulkan defaults to the `VULKAN_SDK` env (the `OpenXR_ROOT` / `VULKAN_SDK_PREFIX`
  overrides still win). A fresh clone must build with only the toolchain + Vulkan
  SDK (+ the one-time cesium-native clone). Keep CI (`build-windows.yml`) == dev
  script == header rev. Dev-only — the released installer always provisioned via
  CI and bundles `openxr_loader.dll`, so it was never affected. (Fixed in #11.)
