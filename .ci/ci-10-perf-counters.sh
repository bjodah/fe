#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

# The counting build (fe_perf.h, FE_PERF_COUNTERS=1).  Nothing else in this
# pipeline compiles it: every other stage builds the shipped configuration,
# where the whole facility expands to nothing, so a counting build that stops
# compiling -- or a counter relationship that stops holding -- would be
# invisible until someone came to measure something.  `perf-check` runs the C
# API suite, whose TestPerfCounters is where the relationships are asserted,
# Phase 21.2's workload battery (perf_workloads.c), whose per-workload and
# cross-workload counter assertions are the other half of them, the example
# host, and the whole script corpus against the counting interpreter, so the
# instrumented lines are executed and not merely compiled.
"${MAKE_PARALLEL[@]}" perf-check
