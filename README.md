# DisplayXR EarthView

A streaming **3D city viewer** for the
[DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime), built on
the **Google Map Tiles API — Photorealistic 3D Tiles** (OGC 3D Tiles:
Draco-compressed glTF photogrammetry) via
[cesium-native](https://github.com/CesiumGS/cesium-native).

Fly the full-scale world maps-style, or double-click a point to frame that
neighborhood as a **tabletop diorama** inside the display's depth volume — the
framing a glasses-free 3D display does best and a headset app can't deliver.
OpenXR `_handle`-class app, Vulkan, one codebase across macOS / Windows /
Android.

**Status: M1 — macOS.** `earthview_handle_vk_macos` renders stereo on the sim
display with camera + diorama navigation, city bookmarks, and the mandatory
attribution HUD. Windows (M2) and Android (M3) follow. Full design and
milestones: [PRD.md](PRD.md). Rendering/coordinate internals:
[docs/rendering-notes.md](docs/rendering-notes.md).

## Google Map Tiles API key (required)

EarthView streams tiles from Google and needs a key with the **Map Tiles API**
enabled (Cloud Console → APIs & Services → enable “Map Tiles API” →
Credentials → API key). Supply it via either:

- env var: `export GOOGLE_MAPS_API_KEY=...`
- or `earthview.ini` next to the app / at the repo root (gitignored): `key=...`

Without a key the app opens to a how-to card and does not crash. The key is
never committed and never bundled into installers. Usage stays inside the free
tier: one root-tileset request per launch (1,000 free/month); no per-tile
billing. Data © Google and partners — attribution is rendered on-screen per the
[Map Tiles API policies](https://developers.google.com/maps/documentation/tile/policies).

## Build & run (macOS)

Requires the DisplayXR runtime installed (or a local dev build) — see the
runtime repo. Then:

```bash
# one-time: clone cesium-native v0.61.0 into third_party/
git clone --branch v0.61.0 --recurse-submodules \
    https://github.com/CesiumGS/cesium-native.git third_party/cesium-native

./scripts/build_macos.sh          # builds build/macos/earthview_handle_vk_macos
./scripts/run_macos_dev.sh        # runs against the runtime dev package or an installed runtime
```

`./scripts/build_macos.sh --installer` produces
`_package/DisplayXREarthView-<version>.pkg`.

## Controls

| Input | Action |
|---|---|
| Left-drag | look (free) / spin + tilt the diorama (orbit) |
| Right-drag / WASD | pan; E/Q climb |
| Scroll | dolly (free) / zoom the tabletop (orbit) |
| Double-click | raycast pick → acquire orbit center (diorama) |
| `C` | release orbit → camera-centric fly (continuous) |
| Esc / Space | release orbit / reset framing |
| `B` | cycle city bookmarks (Paris, SF, NYC, Tokyo, Sydney) |
| `V` / `0`–`3`, `T`, Tab, `I` | mode cycle/select, eye-tracking, HUD, atlas capture |

## Layout

```
CMakeLists.txt        # root: cesium-native + common + tiles_common + macos
common/               # displayxr-common (HUD, input, math) via FetchContent
tiles_common/         # cesium glue + Vulkan tile renderer + geo/RTC math
macos/                # Cocoa + MoltenVK + OpenXR shell
spike/                # M0 headless tileset→PNG proof (standalone)
installer/  scripts/  docs/
```

## M0 spike (headless proof)

```bash
cmake -S spike -B spike/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build spike/build
./spike/build/earthview_m0          # writes /tmp/earthview_m0.png (Paris)
```

## License

[BSL-1.0](LICENSE). EarthView consumes Google Photorealistic 3D Tiles under the
Google Maps Platform terms; the imagery is © Google and its data providers.
