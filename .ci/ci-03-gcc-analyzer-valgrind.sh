#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache gcc"
export CFLAGS="-Wall -Wextra -Werror -pedantic -std=c2x -O0 -g -fanalyzer"
export FE_RUNNER="${VALGRIND}"
export FE_SKIP_SCRIPTS="mandelbrot.fe"
# export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD

"${MAKE_PARALLEL[@]}" -B check
