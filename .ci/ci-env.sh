#!/bin/bash

JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
# Named GNU_PARALLEL, not PARALLEL: GNU parallel reads $PARALLEL from the
# environment as its own default OPTIONS, so exporting a variable by that
# name to a step makes every `parallel` invocation in it a silent no-op.
GNU_PARALLEL=${GNU_PARALLEL:-parallel}
VALGRIND=${VALGRIND:-valgrind --quiet --tool=memcheck --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,possible --error-exitcode=1}

# PTY_TIMEOUT=${PTY_TIMEOUT:-20}
# PTY_STARTUP_DELAY_ADD=${PTY_STARTUP_DELAY_ADD:-0.3}
# PTY_KEY_DELAY_ADD=${PTY_KEY_DELAY_ADD:-0.01}

MAKE_PARALLEL=(make -j "${JOBS}" --output-sync=target)
