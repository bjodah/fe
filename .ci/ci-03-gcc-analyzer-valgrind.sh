#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache gcc -Werror -fanalyzer"
export TEST_RUNNER="${VALGRIND}"
export FE_RUNNER="${VALGRIND}"
# export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD

"${MAKE_PARALLEL[@]}" -B check
