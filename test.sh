#!/usr/bin/env bash

set -euo pipefail

failed=0

check_results() {
  local expected_out=${1}
  local expected_err=${2}
  local comment=${3}
  if cmp out "$expected_out" && cmp err "$expected_err"; then
    echo "✅ $comment"
  else
    echo "❌ $comment"
    failed=$((failed + 1))
  fi
}

run_test() {
  for s in scripts/*; do
    local b
    b=$(basename "$s")
    case " ${FE_SKIP_SCRIPTS:-} " in
      *" $b "*)
        echo "SKIP: $s"
        continue
        ;;
    esac
    # A script may die on purpose; the golden stderr is what decides.
    ${FE_RUNNER:-} "${FE_BIN}" ${FE_FLAGS:-} scripts/assert.fe "$s" > out 2> err || true
    check_results "tests/$b.out" "tests/$b.err" "$s${FE_COMMENT:-}"
  done
  ${FE_RUNNER:-} "${FE_BIN}" ${FE_FLAGS:-} -e '(print "hello, world!")' > out 2> err || true
  check_results "tests/one-liner.out" "tests/one-liner.err" "one-liner${FE_COMMENT:-}"
  rm out err
}

# FE_BIN names an interpreter the caller has already built -- `make
# perf-check`'s counting build, which lives in its own object directory and
# must not be rebuilt or cleaned away by the suite it is being measured
# through. Unset (every ordinary `make check`) means the two builds below,
# exactly as before.
if [[ -n ${FE_BIN:-} ]]; then
  run_test
else
  FE_BIN=./fe
  make clean
  make fe
  run_test
  make clean
  RELEASE=1 make fe
  run_test
fi

if [[ $failed -eq 0 ]]; then
  echo "✅ all tests passed"
else
  echo "❌ $failed tests failed"
fi
exit "$failed"
