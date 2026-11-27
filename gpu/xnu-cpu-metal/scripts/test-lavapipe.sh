#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# lavapipe by itself (scripts/build-lavapipe.sh), under the Vulkan loader built here: test/test_lavapipe_compute.c runs
# test/vecadd.comp on it. The lowest of the three test layers (then scripts/test-m2v.sh, scripts/test-bundle.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VK_HDR="$ROOT/build/vk-deps/vulkan-headers/1.4.363.0/include"
VK_LIB="$ROOT/build/vk-deps/vulkan-loader/1.4.363.0/lib"
mkdir -p "$ROOT/build"

# Fix @@HOMEBREW_PREFIX@@ install_name on our local copy of libvulkan.1.dylib if needed
if [ -e "$VK_LIB/libvulkan.1.dylib" ] && otool -D "$VK_LIB/libvulkan.1.dylib" | grep -q '@@HOMEBREW_PREFIX@@'; then
  chmod u+w "$VK_LIB/libvulkan.1.4.363.dylib" 2>/dev/null || true
  install_name_tool -id "@rpath/libvulkan.1.dylib" "$VK_LIB/libvulkan.1.4.363.dylib"
  codesign -s - --force "$VK_LIB/libvulkan.1.4.363.dylib" 2>/dev/null || true
fi

# The loader under the name programs load it by: where scripts/build-vkloader-arm64e.sh installs it, a copy of Homebrew's
# in build/vk-deps, or the one scripts/build-m2v.sh puts beside the driver (build/vk-deps/vulkan-loader as the builds
# leave it holds libvulkan.1.4.363.dylib alone, which nothing can link by name).
LOADER_DIR=
for d in "$ROOT/build/vkloader-arm64-install/lib" "$VK_LIB" "$ROOT/dist/lavapipe/lib"; do
  if [ -e "$d/libvulkan.1.dylib" ]; then LOADER_DIR=$d; break; fi
done
[ -n "$LOADER_DIR" ] || { echo "test-lavapipe.sh: no libvulkan.1.dylib (scripts/build-vkloader-arm64e.sh builds it)" >&2; exit 1; }

# glslangValidator: Homebrew's glslang (the build needs it too), or one put in build/vk-deps/bin
GLSLANG="$ROOT/build/vk-deps/bin/glslangValidator"
[ -x "$GLSLANG" ] || GLSLANG="$(PATH="/opt/homebrew/bin:$PATH" command -v glslangValidator || true)"
[ -n "$GLSLANG" ] || { echo "test-lavapipe.sh: no glslangValidator (brew install glslang)" >&2; exit 1; }
"$GLSLANG" -V "$ROOT/test/vecadd.comp" -o "$ROOT/build/vecadd.spv"

/usr/bin/clang -O2 -I"$VK_HDR" "$ROOT/test/test_lavapipe_compute.c" \
  "$LOADER_DIR/libvulkan.1.dylib" -Wl,-rpath,"$LOADER_DIR" \
  -o "$ROOT/build/test_lavapipe_compute"

ICD_JSON="$(ls "$ROOT/dist/lavapipe/share/vulkan/icd.d/"lvp_icd*.json | head -n 1)"
echo "Using ICD JSON: $ICD_JSON"
cat "$ICD_JSON"
echo ""

VK_ICD_FILENAMES="$ICD_JSON" "$ROOT/build/test_lavapipe_compute" "$ROOT/build/vecadd.spv"
