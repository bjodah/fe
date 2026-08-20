#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

# The compat corpus runs against checked-in Emacs oracle snapshots, never
# against Emacs itself -- that is what the snapshots are for. Regenerating
# them (make compat-oracle) is a separate, manual/on-demand target that
# needs a resolved Emacs and is not part of this or any other numbered
# stage; see compat/README.md.
"${MAKE_PARALLEL[@]}" compat
