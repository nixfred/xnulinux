#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The plugin's regression tests (test/test_round*.mm, test_stress_mt.mm, test_carenderer.mm) against a built bundle: each
# is compiled, run under a time limit and listed in a summary; status 1 when one failed. They need a Mac's frameworks and,
# of the build, the bundle alone.
#
#   scripts/test-rounds.sh [arm64|arm64e] [XnuCpuMetal.bundle]        (default: arm64, dist/XnuCpuMetal.bundle)
#
# The tests that make NoCopy buffers run twice: as the bundle is, and with the plugin's own NoCopy path forced
# (XNU_CPU_METAL_FORCE_NOCOPY_FALLBACK=1), which is the path of a Vulkan device that does not import host memory.
# Not run here: test/faultmask.c (it shows the kernel behaviour patches/mesa/0003 works around and checks nothing of
# ours) and test/test_iosurface_bench.mm (a benchmark).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${1:-arm64}"
BUNDLE="${2:-$ROOT/dist/XnuCpuMetal.bundle}"
[ -d "$BUNDLE" ] || { echo "test-rounds.sh: no bundle at $BUNDLE (scripts/build-bundle-universal.sh makes it)" >&2; exit 1; }
BUNDLE="$(cd "$BUNDLE" && pwd)"
SDK="$(xcrun --show-sdk-path)"
FRAMEWORKS="-framework Foundation -framework Metal -framework CoreGraphics -framework CoreImage -framework IOSurface
  -framework QuartzCore -framework AppKit -framework IOKit -framework SceneKit"
mkdir -p "$ROOT/build"
failed=0
summary=()

# build NAME: test/NAME.mm -> build/NAME_ARCH (once)
build() {
  local out="$ROOT/build/$1_$ARCH"
  [ -x "$out" ] && [ "$out" -nt "$ROOT/test/$1.mm" ] && return 0
  # shellcheck disable=SC2086
  /usr/bin/clang++ -arch "$ARCH" -O2 -std=c++17 -fobjc-arc -isysroot "$SDK" "$ROOT/test/$1.mm" $FRAMEWORKS -o "$out"
}

# run LABEL SECONDS ALSO-FINE NAME [ARGUMENT...]: build/NAME_ARCH with the bundle and the arguments, killed after SECONDS.
# ALSO-FINE is an exit status besides 0 that does not fail the run ("-" for none).
run() {
  local label=$1 secs=$2 fine=$3 name=$4 rc=0 verdict; shift 4
  echo "=== $label"
  if ! build "$name"; then
    summary+=("FAIL  $label: does not compile"); failed=1; return
  fi
  "$ROOT/build/${name}_$ARCH" "$BUNDLE" "$@" &
  local pid=$!
  ( sleep "$secs" & s=$!; trap 'kill $s 2>/dev/null; exit 0' TERM; wait $s; kill -9 "$pid" 2>/dev/null ) &
  local watchdog=$!
  wait "$pid" || rc=$?
  kill "$watchdog" 2>/dev/null; wait "$watchdog" 2>/dev/null
  if [ "$rc" = 0 ]; then verdict="ok  "
  elif [ "$rc" = "$fine" ]; then verdict="ok* "
  elif [ "$rc" = 137 ]; then verdict="FAIL"; label="$label: still running after $secs s, killed"; failed=1
  else verdict="FAIL"; label="$label: exit status $rc"; failed=1
  fi
  summary+=("$verdict  $label")
}

run "test_round10" 300 - test_round10
run "test_round13 (heavy command buffers)" 600 - test_round13 normal
XNU_CPU_METAL_WATCHDOG_MS=1000 run "test_round13 hang_uaf (the watchdog parks the device)" 300 - test_round13 hang_uaf
# its test 6 (exit status 7) compares with this Mac's own GPU driver: every selector that driver answers and a system
# framework names must be the plugin's too. The paravirtual GPU of a VM (the CI runner) answers two the plugin has not.
run "test_round14_vfx" 300 7 test_round14_vfx
run "test_round17_direct_driver (lavapipe chosen, blit fill)" 300 - test_round17_direct_driver llvmpipe NONE
VK_ICD_FILENAMES=/nonexistent/icd.json VK_DRIVER_FILES=/nonexistent/icd.json \
  run "test_round17_direct_driver, the environment naming another driver" 300 - test_round17_direct_driver llvmpipe NONE
run "test_round19" 300 - test_round19
run "test_round20" 300 - test_round20
run "test_stress_mt (8 threads, 80 rounds)" 900 - test_stress_mt 8 80
run "test_carenderer" 300 - test_carenderer
export XNU_CPU_METAL_FORCE_NOCOPY_FALLBACK=1
run "test_round10, the plugin's own NoCopy path" 300 - test_round10
run "test_round20, the plugin's own NoCopy path" 300 - test_round20
run "test_stress_mt, the plugin's own NoCopy path" 900 - test_stress_mt 8 80
unset XNU_CPU_METAL_FORCE_NOCOPY_FALLBACK

echo
echo "=== summary ($ARCH, $BUNDLE)"
printf '%s\n' "${summary[@]}"
if printf '%s\n' "${summary[@]}" | grep -q '^ok\* '; then
  echo "(ok*: test_round14_vfx's test 6 alone failed; it depends on this Mac's own GPU driver, see above)"
fi
exit $failed
