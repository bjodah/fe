#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

"${MAKE_PARALLEL[@]}" fuzz-smoke
