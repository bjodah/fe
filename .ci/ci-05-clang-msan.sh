#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache clang"
export CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-memory-param-retval -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g"

"${MAKE_PARALLEL[@]}" -B check
