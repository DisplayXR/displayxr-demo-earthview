# DisplayXR EarthView

Streaming 3D city viewer demo for the [DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime),
built on **Google Map Tiles API — Photorealistic 3D Tiles** (OGC 3D Tiles:
Draco-compressed glTF photogrammetry). Fly a real city camera-centric, or
double-click to frame a neighborhood as a tabletop diorama inside the
display's depth volume.

**Status: M0 spike** — headless tileset→PNG proof (cesium-native build +
API chain validation). See [PRD.md](PRD.md) for the full design and
milestones.

## API key

You need a Google Maps Platform API key with the **Map Tiles API** enabled
(Cloud Console → APIs & Services → enable "Map Tiles API" → Credentials →
API key). Supply it via either:

- env var: `export GOOGLE_MAPS_API_KEY=...`
- or `earthview.ini` at the repo root (gitignored): `key=...`

Usage stays inside the free tier: the app issues one root-tileset request
per launch (1,000 free per month).

## M0 spike

```bash
git clone https://github.com/CesiumGS/cesium-native.git third_party/cesium-native --recurse-submodules
cmake -S spike -B spike/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build spike/build
./spike/build/earthview_m0          # writes /tmp/earthview_m0.png (Paris)
```

Data © Google and partners (attribution rendered by the app per
[Map Tiles API policies](https://developers.google.com/maps/documentation/tile/policies)).
