#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache gcc"
#export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD

# `coverage` reaches `check` through $(MAKE), so the jobserver -- and with
# it the whole DAG below -- is inherited by the instrumented run.
"${MAKE_PARALLEL[@]}" coverage
