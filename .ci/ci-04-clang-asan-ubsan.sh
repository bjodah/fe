#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache clang"
export CFLAGS="-Werror -Wall -Wextra -pedantic -std=c2x -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls -O1 -g"

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
