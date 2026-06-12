#!/usr/bin/env bash
#
# scripts/run_macos_dev.sh — launch the locally-built EarthView against a
# DisplayXR macOS runtime: the runtime repo's dev build if present (preferred
# for M1 iteration), else an installed runtime (DisplayXRBundle .pkg).
#
# Why the DYLD dance (carried from modelviewer)
# ---------------------------------------------
# The dev binary links Homebrew's Vulkan loader (an absolute install name),
# while the runtime loads ITS libvulkan via @rpath. With two different
# libvulkan images in one process, the VkInstance the app creates is foreign
# to the runtime's loader and xrGetVulkanGraphicsDeviceKHR fails with
# VK_ERROR_INITIALIZATION_FAILED. Fix: converge app AND runtime on one shared
# loader (Homebrew's) via DYLD_LIBRARY_PATH, and select the runtime's bundled
# MoltenVK via VK_ICD_FILENAMES. DYLD_* survives because this script execs the
# binary DIRECTLY (no SIP-protected intermediary like `nohup`).
set -euo pipefail

# --- API key for local dev --------------------------------------------------
# Source a gitignored .env.local (repo root) if present so GOOGLE_MAPS_API_KEY
# is set without exporting it by hand. .env.local is the dev key store on this
# machine; it is never committed (.gitignore) and never staged into the .pkg.
# (The distributed app gets its key from the user, not from here — see
# docs/api-key.md.)
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
DEV_RT="$HOME/Documents/GitHub/displayxr-runtime/_package/DisplayXR-macOS"
for d in "${DISPLAYXR_RUNTIME_DIR:-}" "$DEV_RT" \
         "/Library/Application Support/DisplayXR" \
         "$HOME/Library/Application Support/DisplayXR"; do
    if [ -n "$d" ] && [ -f "$d/openxr_displayxr.json" ]; then RT="$d"; break; fi
done
if [ -z "$RT" ]; then
    echo "Error: DisplayXR runtime not found." >&2
    echo "       Build the runtime dev package ($DEV_RT) or install the" >&2
    echo "       macOS bundle (DisplayXRBundle-*.pkg) from" >&2
    echo "       https://github.com/DisplayXR/displayxr-installer/releases" >&2
    exit 1
fi
echo "==> Runtime: $RT"

export XR_RUNTIME_JSON="$RT/openxr_displayxr.json"
# Dev package keeps plugins under lib/displayxr/plugins; installed runtime
# uses DisplayProcessors.
if [ -d "$RT/lib/displayxr/plugins" ]; then
    export XRT_PLUGIN_SEARCH_PATH="$RT/lib/displayxr/plugins"
else
    export XRT_PLUGIN_SEARCH_PATH="$RT/DisplayProcessors"
fi
export VK_ICD_FILENAMES="$RT/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"

# Converge app + runtime on one libvulkan (see header).
VK_PREFIX="$(brew --prefix vulkan-loader 2>/dev/null || true)"
if [ -z "$VK_PREFIX" ] || [ ! -d "$VK_PREFIX/lib" ]; then
    echo "Error: Homebrew vulkan-loader not found — \`brew install vulkan-loader\`." >&2
    exit 1
fi
export DYLD_LIBRARY_PATH="$VK_PREFIX/lib:${DYLD_LIBRARY_PATH:-}"

# --- Launch ----------------------------------------------------------------
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/macos/earthview_handle_vk_macos"
if [ ! -x "$BIN" ]; then
    echo "Error: $BIN not found — build first: ./scripts/build_macos.sh" >&2
    exit 1
fi
echo "==> Launching $BIN"
exec "$BIN" "$@"
