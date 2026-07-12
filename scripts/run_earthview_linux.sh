#!/usr/bin/env bash
#
# scripts/run_earthview_linux.sh — launch the locally-built Linux EarthView
# against a DisplayXR Linux dev runtime.
#
# NOTE (build-green status, issue #19): this is the DEV run harness for the
# on-screen pass that comes LATER. It is NOT exercised on CI — ubuntu-latest
# has no display/GPU, so build-linux.yml only compiles the binary. Running it
# needs the runtime's Linux Phase 1b (on-screen present) + a GPU + an X server.
#
# The Linux binary is a HOSTED-NULL app: it passes no window binding, so the
# runtime self-creates its window (the faithful XR_DXR_xlib_window_binding arm
# is Phase-3 hardware-gated). See linux/main.cpp and the runtime repo's
# docs/guides/linux-demo-port.md.
set -euo pipefail

# --- API key for local dev --------------------------------------------------
# Source a gitignored .env.local (repo root) if present so GOOGLE_MAPS_API_KEY
# is set without exporting by hand. Never committed, never bundled.
ENV_LOCAL="$(cd "$(dirname "$0")/.." && pwd)/.env.local"
if [ -f "$ENV_LOCAL" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$ENV_LOCAL"
    set +a
fi

# --- Locate a runtime: dev build first, then installed ----------------------
# Override with DISPLAYXR_RUNTIME_DIR=/path/to/runtime-package.
RT=""
DEV_RT="$HOME/Documents/GitHub/displayxr-runtime/build"
for d in "${DISPLAYXR_RUNTIME_DIR:-}" "$DEV_RT" \
         "/usr/local/share/displayxr" \
         "$HOME/.local/share/displayxr"; do
    if [ -n "$d" ] && [ -f "$d/openxr_displayxr-dev.json" ]; then RT="$d"; break; fi
    if [ -n "$d" ] && [ -f "$d/openxr_displayxr.json" ]; then RT="$d"; break; fi
done
if [ -z "$RT" ]; then
    echo "Error: DisplayXR Linux runtime not found." >&2
    echo "       Build the runtime dev package or set DISPLAYXR_RUNTIME_DIR." >&2
    exit 1
fi
echo "==> Runtime: $RT"

if [ -f "$RT/openxr_displayxr-dev.json" ]; then
    export XR_RUNTIME_JSON="$RT/openxr_displayxr-dev.json"
else
    export XR_RUNTIME_JSON="$RT/openxr_displayxr.json"
fi

# Plug-in (sim-display) discovery: POSIX uses XRT_PLUGIN_SEARCH_PATH.
for p in "$RT/lib/displayxr/plugins" "$RT/src/xrt/targets/plugins" "$RT/plugins"; do
    if [ -d "$p" ]; then export XRT_PLUGIN_SEARCH_PATH="$p"; break; fi
done

# Native Vulkan compositor path + a headless-friendly default output mode.
export OXR_ENABLE_VK_NATIVE_COMPOSITOR=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"

# --- Launch ----------------------------------------------------------------
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/linux/earthview_handle_vk_linux"
if [ ! -x "$BIN" ]; then
    echo "Error: $BIN not found — build first: ./scripts/build_linux.sh" >&2
    exit 1
fi
echo "==> Launching $BIN"
exec "$BIN" "$@"
