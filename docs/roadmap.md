# EarthView roadmap

Living status + execution plan. The full product spec and milestone rationale
live in [`PRD.md`](../PRD.md) (§9). Rendering/coordinate internals:
[`rendering-notes.md`](rendering-notes.md). API-key design:
[`api-key.md`](api-key.md).

## Milestones

| M | Deliverable | Status |
|---|---|---|
| **M0 — spike** | Headless cesium-native tileset → PNG proof (build gate, API chain, Draco/glTF/JPEG decode, float32 RTC, no-leak orbit) | ✅ done |
| **M1 — macOS XR** | `earthview_handle_vk_macos`: stereo on the sim display, camera + diorama navigation, city bookmarks, attribution HUD, installer + release wiring | ✅ done — pending in-app key entry + first tag (see Next) |
| **M2 — Windows** | `earthview_handle_vk_win` on a real Leia panel; Win32 window binding; installer + run scripts; `/make-app-logos` real icons; 60 fps | ⬜ planned |
| **M3 — Android** | NDK leg via the OOP runtime; reduced SSE + texture budget; auto-orbit; 30+ fps on NP02J | ⬜ planned |
| **M4 — release polish** | Coverage/bookmark list, README/docs, bundle-inclusion decision | ⬜ planned |

## What's built (M1)

- **tiles_common** — cesium-native glue (`tile_engine`), Vulkan tile renderer
  (`tile_renderer`, `IPrepareRendererResources`), double-precision geo/RTC math
  (`geo_math`), unlit sRGB shaders.
- **macOS shell** — Cocoa + MoltenVK + OpenXR; one worst-case swapchain, per-mode
  tiles, `XR_EXT_view_rig`; camera-centric + display-centric diorama nav;
  center-eye double-click pick; `C` release-to-fly; bookmarks; always-on Google
  attribution strip; keyless first-run text card.
- **Selection model** — single mono symmetric ViewState per
  [cesium-unity](rendering-notes.md#1) (off-axis stereo is render-only); frustum
  sized to *contain* the off-axis display angle; `enableFogCulling=false`,
  `forbidHoles=true`. Near plane scaled to `ez` (the clipped-bottom fix).
- **Distribution** — public repo, Apache-2.0, macOS `.pkg`
  (`scripts/build_macos.sh --installer`), `build-macos.yml` (build + payload
  guard + release attach + `versions.json` bump dispatch on `v*`).
- **Tooling** — `DXR_DUMP=N` self-capture (mono eye-0 PNG; vk_native has no
  reliable atlas capture); `EV_ELEV`/`EV_DIST` pose overrides; `.env.local`
  dev-key store sourced by `run_macos_dev.sh`.

## Next (close out M1, then tag)

1. **In-app API key entry** (before the first tag) — per-user key entry + app-
   support persistence + late engine init, so the distributed app never needs
   the dev key. Design + invariants: [`api-key.md`](api-key.md). The Leia/dev
   key is verified *not* exposed (gitignored, never committed, not in the
   `.pkg`).
2. **Tag `v0.1.0`** — `/dxr-release earthview v0.1.0` from the runtime hub
   (tags repo → builds `.pkg` → GitHub Release → bumps
   `versions.json[earthview_demo]`). Requires runtime PR #562 merged first.

## Deferred / M1.x knobs (not gate-blocking)

- Bookmark fly-over interpolation (currently instant jump).
- Diorama transition polish / depth auto-fit from the display depth budget.
- Batched/async GPU tile uploads (currently submit-and-wait per tile).
- ASTC/BC texture transcode + `TilesetOptions` byte-budget tuning (matters for
  M3/Android VRAM).
- Real app logos via `/make-app-logos` (placeholder icons today).

## Then M2 / M3

- **M2 Windows:** add the `windows/` shell (Win32 + `XR_EXT_win32_window_binding`),
  a `build-windows.yml`, and the Windows installer; validate weave + 60 fps on a
  Leia panel. The selection/coordinate/renderer code in `tiles_common` is
  platform-neutral and carries over.
- **M3 Android:** NDK leg through the OOP runtime (ADR-025 pattern, as
  modelviewer/mediaplayer); reduced SSE + tile-byte budget for NP02J; auto-orbit.
  Full work breakdown: [`docs/m3-android-plan.md`](m3-android-plan.md).
