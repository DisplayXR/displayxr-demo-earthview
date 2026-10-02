# EarthView API key — design

EarthView streams Google Photorealistic 3D Tiles, which requires a **Google Map
Tiles API key**. This documents how the key is supplied, why the DisplayXR/dev key is
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
  `earthviewSaveApiKey` / `earthviewClearApiKey` / `TileEngine::probeKey` in
  `tiles_common/tile_engine.cpp`) are shared. **Validated** against Google: the
  pasted key is probed (`root.json`, HTTP-status checked) BEFORE it is saved —
  an invalid key is rejected inline and not persisted. `⌘K` / `Ctrl+K` reopens
  the panel any time; a **Remove key** button deletes the saved key so nothing
  persists ("clean box after use"). `EV_PROBE=<key>` is a CLI validation tool.
- **Windows:** the Win32 entry dialog (`ShowApiKeyDialog` in `windows/main.cpp`)
  mirrors the macOS card using the same shared functions — first-run keyless +
  Ctrl+K, modal, Save validates then defers the late-init to the render thread.
  Implemented and **live-validated on Windows (2026-08-01)** — the last
  outstanding item on this design. Verified end to end: with all three
  resolution steps cleared (no env var, no per-user ini, no cwd ini) the
  first-run card is shown; pasting a key and saving probes it, late-inits the
  tile engine on the render thread so tiles stream **without a relaunch**, and
  persists `%APPDATA%\DisplayXR\EarthView\earthview.ini` — byte-identical to a
  known-good ini from an earlier session. To re-exercise it, move that ini
  aside and launch with `GOOGLE_MAPS_API_KEY` unset.
- **Linux:** the same flow as the Win32 dialog, as an async desktop dialog
  (`keydlg` in `linux/main.cpp`). A keyless start opens it and `Ctrl+K` opens
  it any time. Details below.

## Linux

The Linux app has no toolkit of its own, so the dialog is a **child process**
(the displayxr-demo-modelviewer file-picker pattern): `zenity --entry`, else
`kdialog --inputbox`, spawned with `posix_spawnp` and polled once per frame.
The key probe (`TileEngine::probeKey`, the `EV_PROBE` path) runs on a worker
thread. Neither one blocks the frame loop. The dialog wording matches the macOS
card. The buttons are **Save & Start**, **Close**, **Get a Key…** (runs
`xdg-open` on the Cloud Console) and, when a saved key exists, **Remove key**.

- **Save & Start** strips whitespace and probes the key against Google. If
  Google rejects it, the dialog reopens with the reason above the text and the
  key still in the field. If Google accepts it, the key is saved to
  `$XDG_CONFIG_HOME/displayxr/earthview.ini` (`~/.config/displayxr/…` when
  `XDG_CONFIG_HOME` is unset or relative). The file is created with mode
  `0600`, and an existing file is `fchmod`ed to `0600`. The key is also
  exported as `GOOGLE_MAPS_API_KEY` for this session, and the frame loop
  late-inits the tile engine, so tiles stream **without a restart** (as on
  Windows).
- **Close** with no key leaves the globe untiled. The window title then reads
  *"no API key (Ctrl+K to enter one)"*.
- **No dialog tool** (neither zenity nor kdialog on `PATH`, or no
  `DISPLAY`/`WAYLAND_DISPLAY`): the old log line is printed, and the window
  title reads *"no API key: set GOOGLE_MAPS_API_KEY or install zenity"*. The
  `.deb` has `Recommends: zenity`, so a default install has the dialog.
- At startup the log names the store that supplied the key, and shows only the
  key's last 4 characters (`API key …abcd from <source>`).

Test hooks (dev only, for headless runs):

- `EV_KEY_DIALOG=zenity|kdialog|none|<cmd>` forces a dialog tool. Any other
  value is run as a zenity-compatible command, such as a stub.
- `EV_KEY_PROBE_ACCEPT_FOR_TEST=<key>` makes that exact key pass validation
  without contacting Google. Persistence and the late-init still run for real.

## Android (#46)

Android has no shell env and no writable per-user config dir, so the desktop
chain does not apply. Resolution order, first hit wins:

1. **In-app dialog → `SharedPreferences`** (what users do). Validated against
   Google before it is saved, like the macOS card and the Win32 dialog — a key
   Google rejects is refused inline instead of producing an empty blue sphere.
   If the device cannot *reach* Google the key is saved with a "not verified"
   notice rather than blocked, so a correct key is still enterable offline.
2. **`debug.dxr.ev.key`** — survives uninstall, cleared by reboot.
3. **`persist.dxr.ev.key`** — survives uninstall **and** reboot.

```bash
adb shell setprop persist.dxr.ev.key <YOUR_KEY>   # reinstall- and reboot-proof
```

Why 2/3 exist: `SharedPreferences` is app-private and the OS wipes it on
uninstall, so `install-android.sh --force-reinstall` (and any clean-install
validation or automated E2E pass) loses the key every time. A plain
`adb install -r` upgrade preserves it. The props let a device keep a key across
wipes without re-entering a secret each pass.

Entry note: the key field sets `IME_FLAG_NO_EXTRACT_UI`. Without it the IME
opens a fullscreen extract view in landscape that covers the dialog's Save
button with its own candidate strip — tapping there commits a suggestion and
appends a character, storing a 40-char key that fails exactly like a missing
one. Whitespace is stripped from pasted keys for the same reason.

## Key resolution order (never a baked-in default)

1. `GOOGLE_MAPS_API_KEY` environment variable — dev override.
2. **User config in the OS app-support dir** (where in-app entry persists,
   outside the repo and the .app bundle):
   - macOS: `~/Library/Application Support/DisplayXR/EarthView/earthview.ini`
   - Windows: `%APPDATA%\DisplayXR\EarthView\earthview.ini`
   - Linux: `$XDG_CONFIG_HOME/displayxr/earthview.ini` (else
     `~/.config/displayxr/earthview.ini`)
   - Android: app-private storage.
3. `earthview.ini` next to the exe / cwd — dev convenience (gitignored).
4. None → first-run key-entry UI.

**Dev convenience:** `scripts/run_macos_dev.sh` sources a gitignored
`.env.local` (repo root) if present and exports `GOOGLE_MAPS_API_KEY` from it,
so the local dev key “just works” without hand-exporting. `.env.local`,
`.env`, and `.env.*` are gitignored and never staged into the `.pkg`.

All four steps are implemented in `tiles_common/tile_engine.cpp`:
`earthviewKeyConfigPath()` resolves the per-user path (`%APPDATA%\DisplayXR\
EarthView\earthview.ini` on Windows, `~/Library/Application Support/...` on
macOS, `$XDG_CONFIG_HOME/displayxr/earthview.ini` on desktop Linux), `earthviewGetApiKey()` walks env → per-user ini → cwd ini,
`earthviewSaveApiKey()` writes the per-user ini (creating the directory; on
POSIX it is opened with mode `0600` and `fchmod`ed to it, so no window exists
where it is world-readable — Windows inherits the default `%APPDATA%` ACL), and
`earthviewClearApiKey()` deletes only that file, leaving the dev stores alone.

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
  key; CI builds keyless. The Linux `.deb` (`scripts/package_deb_linux.sh`)
  fails to build if its payload contains an `earthview.ini`, `*.key` or
  `.env*` file, or any `AIza…` key literal. The Windows CI asserts the same of
  its installer listing. The `.pkg` payload assertion lists exactly the
  expected files — a key file appearing there should fail review.
- Each developer's key lives only in their local gitignored `earthview.ini`.
- The persisted **user** key is per-user, mode 600; it is the end user's own
  key, never the project's.
