#!/usr/bin/env bash
#
# Hardware-free check for ARKP-1546: an interpipesrc with
# stream-sync=compensate-ts must not read an NVMM surface its producer's
# buffer pool has already reused.
#
# It builds gst-interpipe twice: as released, at the upstream tag this checkout
# is based on, and from this checkout. Then it runs nvmm-reuse.c against each
# in both attach modes:
#
#   stock    compensate-ts   must read reused surfaces: the harness still
#                            reproduces the defect
#   fixed    compensate-ts   must not
#   either   passthrough-ts  must not: the mode that never copies, the control
#
# Exits 0 only when all four hold.
#
# Usage: tests/nvmm-reuse/run.sh [SECONDS_PER_RUN]   (default 10)
#        STOCK_REF=<tag> overrides the upstream tag, which defaults to the
#        nearest tag that is not one of this fork's -manta.<n> releases.
# Needs: git, cc, pkg-config with gstreamer-1.0 and gstreamer-app-1.0, meson,
#        ninja, python3.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SECONDS_PER_RUN=${1:-10}
STOCK_REF=${STOCK_REF:-$(git -C "$ROOT" describe --tags --abbrev=0 --exclude '*-manta.*')}
# The consumer keeps up on average and stalls 120 ms every 0.5 s, as a
# recording's video branch does while qtmux waits on its audio pad.
FRAME_US=6000
STALL_US=120000

# Canonical, so it matches the plugin path gst-inspect prints (macOS /var is a
# symlink to /private/var).
WORK=$(cd "$(mktemp -d)" && pwd -P)
trap 'rm -rf "${WORK:?}"' EXIT

build_interpipe() { # NAME SOURCE_DIR
  # meson writes its errors to the log, which the trap deletes: show it.
  meson setup "$WORK/$1" "$2" -Denable-gtk-doc=false >"$WORK/$1-setup.log" ||
    { cat "$WORK/$1-setup.log" >&2; return 1; }
  meson compile -C "$WORK/$1" >"$WORK/$1-build.log" ||
    { cat "$WORK/$1-build.log" >&2; return 1; }
}

# Runs one attach mode against one build; prints the harness's JSON line.
run() { # NAME MODE
  local plugins="$WORK/$1/gst/interpipe" loaded
  # GST_PLUGIN_PATH is searched before the system path, but an interpipe
  # installed there too would make the result ambiguous: check which loaded.
  loaded=$(GST_PLUGIN_PATH="$plugins" GST_REGISTRY="$WORK/$1.registry" \
    gst-inspect-1.0 interpipesrc 2>/dev/null | sed -n 's/^ *Filename *//p')
  if [ "$(dirname "$loaded")" != "$plugins" ]; then
    echo "interpipesrc loaded from '$loaded', not $plugins" >&2
    exit 1
  fi
  GST_PLUGIN_PATH="$plugins" GST_REGISTRY="$WORK/$1.registry" \
    "$WORK/nvmm-reuse" "$2" "$SECONDS_PER_RUN" "$FRAME_US" "$STALL_US"
}

echo "gst-interpipe $STOCK_REF (stock) and $(git -C "$ROOT" describe --tags --always --dirty) (fixed), $(gst-launch-1.0 --version | sed -n 2p)"
git -c advice.detachedHead=false clone -q --branch "$STOCK_REF" --depth 1 \
  https://github.com/RidgeRun/gst-interpipe.git "$WORK/stock-src"
build_interpipe stock "$WORK/stock-src"
build_interpipe fixed "$ROOT"
# shellcheck disable=SC2046 # pkg-config prints several flags on purpose
cc -O2 -o "$WORK/nvmm-reuse" "$HERE/nvmm-reuse.c" \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)

failed=0
for build in stock fixed; do
  for mode in passthrough-ts compensate-ts; do
    result=$(run "$build" "$mode")
    read -r recorded copies wrong < <(python3 -c 'import json, sys; r = json.loads(sys.argv[1]); print(r["recorded"], r["descriptorCopies"], r["wrongContent"])' "$result")
    expect_wrong=$([ "$build/$mode" = stock/compensate-ts ] && echo yes || echo no)
    # No wrong frames proves nothing unless frames arrived and, in
    # compensate-ts, every one came through the copy. Not just "some": the base
    # class copies the first buffer in every mode, to flag it DISCONT.
    if [ "$recorded" -eq 0 ] ||
      { [ "$mode" = compensate-ts ] && [ "$copies" -ne "$recorded" ]; } ||
      { [ "$expect_wrong" = yes ] && [ "$wrong" -eq 0 ]; } ||
      { [ "$expect_wrong" = no ] && [ "$wrong" -ne 0 ]; }; then
      verdict=FAIL
      failed=1
    else
      verdict=ok
    fi
    printf '%-4s %-7s %-14s %s\n' "$verdict" "$build" "$mode" "$result"
  done
done
exit "$failed"
