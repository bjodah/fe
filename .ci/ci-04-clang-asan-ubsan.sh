#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache clang"
export CFLAGS="-Werror -Wall -Wextra -pedantic -std=c2x -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls -O1 -g"
# Phase 23.0's payload poison mode, the publish protocol's enforcement arm
# (fe_internal.h).  This lane arms it rather than a lane of its own: the
# protocol's failure is a read through storage that moved, which is what a
# sanitizer build is for, and `payload_tests.c`'s
# TestPoisonedPointerFailsLoudly -- compiled only when the knob is on -- is
# where a deliberately stale pointer is proved to read poison.  Since Phase 24
# the lane also runs REAL payload traffic: vectors own payload blocks, so
# every `aref`, `aset` and printed vector in `make check` reads through
# storage this knob moves under it.  Measured by planting one hoisted address
# in `AppendSequence` and running both configurations: `make check` exit 0,
# this lane exit 2 on `vconcat`'s own case.  The knob rides CPPFLAGS (see the
# Makefile), so the standalone header checks compile the armed header too.
export FE_DEBUG_PAYLOAD_MOVE=1

"${MAKE_PARALLEL[@]}" -B check

# Phase 21.2's workload battery, under the same sanitizers.  It drives
# allocation and collection harder than anything in `check` -- 45 collections
# in a 96 KiB arena, a live set at 80% of a 256 KiB one, 8192 interned symbols,
# an 8192-byte string chain -- which is exactly the traffic ASan and UBSan
# exist to watch, and none of it is reachable from an ordinary build (the
# counters compile to nothing there, so the battery has no FePerfRead to call).
# Only the battery and the five core objects it links are built here, not the
# whole `perf-check` lane: that would re-run the script corpus a second time
# under the sanitizers for no new coverage.  The objects land in `perfobj/`,
# and `.build-flags` is their prerequisite, so an ordinary `make perf-check`
# afterwards rebuilds them rather than relinking these.
"${MAKE_PARALLEL[@]}" perf-workloads
