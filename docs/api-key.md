# EarthView API key — design

EarthView streams Google Photorealistic 3D Tiles, which requires a **Google Map
Tiles API key**. This documents how the key is supplied, why the Leia/dev key is
never exposed, and the planned in-app key-entry flow.

## Status

- **Implemented (macOS):** in-app key entry + per-user persistence. Keyless
  launch shows a centered entry card (paste field, *Get a Key…*, *Save & Start*,
  *Continue without*); on save the key is written to the per-user app-support
  config (mode 600) and the tile engine is late-initialized on the frame-loop
  thread — no relaunch. Resolution order below. **No key is committed or
  bundled** (verified: `earthview.ini`/`.env*` gitignored and never in history;
  the `.pkg` payload contains no ini/key — only the binary, Google logo, dylibs).
  Cross-platform pieces (`earthviewKeyConfigPath` / `earthviewGetApiKey` /
  `earthviewSaveApiKey` in `tiles_common/tile_engine.cpp`) are shared; the
  Windows entry **dialog** (Win32) is the remaining follow-up — env/`.env.local`/
  ini already unblock Windows.

## Key resolution order (never a baked-in default)

1. `GOOGLE_MAPS_API_KEY` environment variable — dev override.
2. **User config in the OS app-support dir** (where in-app entry persists,
   outside the repo and the .app bundle):
   - macOS: `~/Library/Application Support/DisplayXR/EarthView/earthview.ini`
   - Windows: `%APPDATA%\DisplayXR\EarthView\earthview.ini`
   - Android: app-private storage.
3. `earthview.ini` next to the exe / cwd — dev convenience (gitignored).
4. None → first-run key-entry UI.

**Dev convenience:** `scripts/run_macos_dev.sh` sources a gitignored
`.env.local` (repo root) if present and exports `GOOGLE_MAPS_API_KEY` from it,
so the local dev key “just works” without hand-exporting. `.env.local`,
`.env`, and `.env.*` are gitignored and never staged into the `.pkg`.

`earthviewGetApiKey()` (`tiles_common/tile_engine.cpp`) currently implements 1+3;
extend it with 2, and add a writer for the app-support path.

## First-run entry UI (macOS, Cocoa)

When no key resolves, show a card/panel instead of just the text strip:
- short explanation + a paste field for the key,
- **Get a key** button → opens the Google Cloud Console Map Tiles API page,
- **Save & Start** → (optionally validate with one `root.json` request),
  persist to the app-support config (`chmod 600`), then **late-init the tile
  engine** and begin streaming,
- **Continue without** → stays on the placeholder + how-to.

`TileEngine::init()` runs once at startup today; make it re-invokable when a key
arrives mid-session (guard `g_tilesActive`; the first `updateView` is just the
next frame, so late init is safe). A small `KeyStore` helper handles read/write
of the app-support ini.

## Security invariants (do not regress)

- **Never** bake or default a key in source, CI, or installers.
- `.gitignore` excludes `earthview.ini` and `*.key` — keep it.
- The installer (`installer/macos/`, `scripts/build_macos.sh`) never stages a
  key; CI builds keyless. The `.pkg` payload assertion lists exactly the
  expected files — a key file appearing there should fail review.
- Each developer's key lives only in their local gitignored `earthview.ini`.
- The persisted **user** key is per-user, mode 600; it is the end user's own
  key, never Leia's.
