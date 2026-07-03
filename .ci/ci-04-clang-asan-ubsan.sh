#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache clang"
export CFLAGS="-Werror -Wall -Wextra -pedantic -std=c2x -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls -O1 -g"

"${MAKE_PARALLEL[@]}" -B check
