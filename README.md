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

## Build & run (Linux)

Ubuntu 22.04, 24.04 and 26.04 are supported. Requires the DisplayXR runtime
(`displayxr-runtime` .deb, or a local dev build).

**Toolchain floor** — `scripts/build_linux.sh` resolves both up front:

| | need | 22.04 | 24.04 / 26.04 |
|---|---|---|---|
| CMake | >= 3.25 | 3.22.1 too old → **auto-provisioned** | fine as shipped |
| GCC | >= 12 | **`sudo apt install g++-12`** | fine as shipped |

Neither is optional, and both bite only on 22.04:

* **CMake 3.25** — cesium-native v0.61.0 selects its vcpkg triplet with
  `elseif(LINUX)`, and the `LINUX` variable was introduced in CMake 3.25. With
  jammy's 3.22.1 the configure fails with *"Cannot guess an appropriate value
  for VCPKG_TRIPLET"*. The script downloads a pinned Kitware CMake into
  `/tmp/dxr-cmake` (override with `DXR_CMAKE_DIR`) and uses it for that build
  only; set `DXR_NO_CMAKE_DOWNLOAD=1` to refuse the download and get
  instructions instead.
* **GCC 12** — cesium's ezvcpkg tree builds ada-url v3.3.0, which needs C++20
  `constexpr std::string`, a GCC 12 libstdc++ feature. The script selects
  `g++-12` automatically when the default `g++` is older; `CC` / `CXX`
  override it.

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libvulkan-dev glslang-tools \
    libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev \
    libxcb1-dev libx11-xcb-dev libxcb-glx0-dev libxxf86vm-dev \
    libwayland-dev libxkbcommon-dev libegl-dev libgl-dev libasound2-dev \
    libcurl4-openssl-dev libssl-dev zlib1g-dev \
    autoconf automake autoconf-archive libtool nasm zip unzip
sudo apt install g++-12          # 22.04 only

./scripts/build_linux.sh         # clones cesium-native, builds build/linux/earthview_handle_vk_linux
./scripts/run_earthview_linux.sh # runs against an installed or dev runtime
```

`./scripts/package_deb_linux.sh` produces
`dist/displayxr-earthview_<version>_amd64.deb`. Release artifacts are built in
an `ubuntu:22.04` container so the one package installs and runs on all three
releases; CI then installs it into pristine 22.04 / 24.04 / 26.04 containers
(`scripts/verify_deb_install_linux.sh`) before it may be attached to a release.

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

[Apache-2.0](LICENSE). EarthView consumes Google Photorealistic 3D Tiles under the
Google Maps Platform terms; the imagery is © Google and its data providers.
(Vendored OpenXR extension headers under `openxr_includes/` remain BSL-1.0 —
see their SPDX headers.)
