#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."

source $(compgen -G "/opt-?/cpython-v3.*-apt-deb/bin/activate")
source .ci/ci-env.sh

export JOBS PARALLEL VALGRIND
#export PTY_TIMEOUT PTY_STARTUP_DELAY_ADD PTY_KEY_DELAY_ADD

for step in .ci/ci-[0-9][0-9]-*.sh; do
	"${step}"
done
