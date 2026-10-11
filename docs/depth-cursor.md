# Depth-aware cursor — how EarthView adopts it

The DisplayXR runtime draws a **depth cursor**: where content pops out of the
glass under the pointer, the arrow is lifted just in front of it, at the right
disparity; over the sky, or over ground on/behind the glass, nothing changes
and the OS cursor stays the cursor (runtime ADR-046 Amendment 2, extension
`XR_DXR_cursor_depth` v5+). The runtime owns the drawing, the margin, the
comfort clamp and the smoothing. **The app owns one thing: the depth under the
pointer.**

The leg integration is the one `displayxr-demo-modelviewer` piloted
(its `docs/depth-cursor.md`, PR #171). What is EarthView-specific is the depth
source: the scene is **streamed** Google Photorealistic 3D Tiles, so there is no
one model to build a BVH for.

## What it costs

| Piece | Where |
|---|---|
| Per-tile BVH (build + traverse) | `tiles_common/tile_raycast.{h,cpp}` |
| Build at tile load, per-frame pick list, `TileRenderer::raycast` | `tiles_common/tile_renderer.{h,cpp}` |
| Shared adoption helper (opt-out, scripted pointer, cost line) | `tiles_common/earthview_cursor_depth.h` |
| Per platform leg | `macos/main.mm`, `linux/main.cpp`, `windows/{main.cpp,xr_session.*}` |
| Vendored header + pins | `openxr_includes/openxr/XR_DXR_cursor_depth.h` (spec v6), `VENDORED.*`, `common/CMakeLists.txt` (displayxr-common v2.29.0) |

## The depth source: a ray cast over the tiles on screen

`TileRenderer::raycast(origin, dir)` — nearest hit, in the locate space.

1. **Build once per tile, off the render thread.** `prepareInLoadThread`
   (cesium's worker thread, CPU-only) already packs the tile's float vertices
   and indices for the GPU; it also builds a `TileBvh` over exactly those
   triangles (median split, ≤ 4 triangles per leaf, indexed triangles,
   Möller–Trumbore two-sided). The BVH rides the tile into `TileGpu` and dies
   with it in `free()`. A frame never pays for a build.
2. **BVH in the tile's east/north/up frame.** Tile meshes are
   ECEF-axis-aligned, so at Paris' 48° latitude a flat street block is a
   tilted slab whose axis-aligned boxes are as tall as they are wide, and a
   ray grazing toward the horizon enters every one of them. The build applies
   the tile's ENU rotation (times the linear part of `yUpToZUp * transform`)
   first; any linear map preserves the ray parameter `t`.
3. **Only the tiles drawn last frame.** `buildDrawList` — which already makes
   one double-precision matrix per selected tile — also stores, per tile, the
   BVH-from-XR 3×4 (`frame * inverse(xrFromEcef * yUpToZUp * tileTransform)`,
   computed in double, cast once) and an XR-space bounding sphere of the
   tile's BVH root box. Never a scan of every loaded triangle.
4. **Per ray:** sphere reject (one dot product per tile) → map the ray into
   the survivors' BVH space and slab-test their root boxes → sort survivors by
   entry `t` → traverse front to back, stopping once a box starts beyond the
   best hit.

**Same space as the views.** EarthView's rig pose is identity; all navigation
lives in the double world mapping `xrFromEcef`, and every view is rendered
straight from the located `XrView`s. So the helper's rays, the views and the
draw matrices share one space. `chain()` casts against the *previous* frame's
views, and the pick list it reads is that same previous frame's (rebuilt after
this frame's locate), so the world mapping always matches the views.

**Covers everything that sticks out.** Google P3DT is one mesh — terrain,
buildings, bridges, the Eiffel Tower. The sky is a clear colour at infinity
and every UI surface (HUD, attribution strip, reticle, header bar, buttons) is
2D, so the tiles are the whole 3D scene.

**Lifetime.** `free()` clears the pick list, so a free outside a frame
(shutdown, API-key re-init) can never leave a dangling tile pointer for the
next cast; inside a frame every free is followed by `buildDrawList`.

## Measured (M-series Mac, Release, macOS vk_native, 20 rays/frame)

| View | Tiles drawn / resident | Ray cast / frame | Note |
|---|---|---|---|
| default Paris bookmark (looking down, 1.5 km) | 94 / 312 | 35–54 µs mean, ≤ 220 µs max | |
| low oblique (`EV_ELEV=6`) | 716 / 1315 | ~250 µs mean | grazing rays cross ~11 tile boxes before the hit |
| near-horizontal (`EV_ELEV=0.5 EV_DIST=1200`), pointer on the city | 844 / 1607 | 150–350 µs mean | the worst case |
| same view, pointer in the sky | 844 / 1607 | ~27 µs mean | spheres + root boxes miss |

The two broad-phase steps (ENU-aligned boxes, sphere reject + front-to-back
order) took the low-oblique case from ~520 µs to ~250 µs per frame.

**Tile BVH build** (load thread, never the frame): 0.3–1.3 ms mean per tile,
5–60 ms worst single tile while a city streams in; ~0.4–2.4 M triangles
resident; **~50 B per resident triangle → 32 MB (Paris top-down) to 116 MB
(horizon view)** of CPU RAM, about 11–13 % on top of the tiles' GPU
residency. Nothing is built unless the extension is enabled
(`TileRenderer::setRaycastEnabled`, set by the adoption helper): Android,
`DISPLAYXR_CURSOR_DEPTH=0` and a runtime without the extension pay nothing.

The cost line, every 300 frames that ran the ray cast:

```
cursor depth: ray cast 35.4 us/frame mean, 162.5 us max, 20.0 rays/frame over 94 tiles, 300 frames
  (hit 300, runtime drew 300); tile BVH build (load thread) 312 tiles / 0.65 Mtri, 1.32 ms mean,
  16.6 ms max, 32 MB resident; placement active=1 disparity=-0.138 ...
```

## Testing it without a 3D display

```bash
DISPLAYXR_RUNTIME_DIR=<runtime>/_package/DisplayXR-macOS \
DISPLAYXR_CURSOR_DEPTH_UV=0.5,0.6 ./scripts/run_macos_dev.sh
touch "$TMPDIR/displayxr_atlas_trigger.earthview_handle_vk_macos"   # vk_native capture
# -> $TMPDIR/displayxr_atlas.earthview_handle_vk_macos.png
```

`DISPLAYXR_CURSOR_DEPTH_UV=u,v` scripts the pointer (canvas UV, top-left
origin) and adds the runtime's placement to the cost line. Use the **per-exe**
trigger: the plain one is consumed by whichever DisplayXR app polls first.
`EV_ELEV` / `EV_DIST` pick the framing. Expected on the default bookmark: at
`0.5,0.6` (ground below the convergence point) the arrow appears in both tiles
with crossed disparity (further right in the first tile); at `0.5,0.08`
(ground beyond it) `hit 300, runtime drew 0` and no arrow; in the sky
(`EV_ELEV=0.5 EV_DIST=1200`, `0.5,0.08`) `hit 0`. `DISPLAYXR_CURSOR_DEPTH=0`
turns it all off (`XR_DXR_cursor_depth: off`).

## Platforms

- **macOS** (Cocoa adapter, `setWindowCocoa`) — verified.
- **Linux** (X11 or Wayland via `displayxr::linux_window`) — the manual path:
  `setPointerUV` from Motion, `clearPointerUV` on PointerLeave, and
  `set_cursor(Hidden/Default)` on `onDepthCursorShownChanged`.
- **Windows** (`setWindowWin32`) — wired but inert until the runtime's
  D3D11/D3D12 compositors draw the cursor (runtime #1944): `willDraw` stays
  false and the OS cursor stays.
- **Android** — not wired (no pointer); `tile_raycast.cpp` compiles there
  (`tiles_common` is shared) but no BVH is built.
