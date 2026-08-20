#!/usr/bin/env bash

set -euo pipefail

failed=0
# One interpreter per core by default. The cases are independent -- each one
# reads its own `out`/`err` under a private directory rather than the two
# shared files in the working directory this suite used to reuse -- so the
# suite costs its slowest case rather than the sum of all of them.
jobs=${FE_TEST_JOBS:-$(nproc 2>/dev/null || echo 1)}

# One case, run in the background by the pool below. A script may die on
# purpose; the golden stderr is what decides, so the run's own status is
# ignored. The verdict is written beside the output instead of printed:
# the cases finish out of order and the report is in script order.
run_case() {
	local dir=${1}
	local expected_out=${2}
	local expected_err=${3}
	local comment=${4}
	shift 4
	mkdir -p "${dir}"
	${FE_RUNNER:-} "${FE_BIN}" ${FE_FLAGS:-} "$@" \
		>"${dir}/out" 2>"${dir}/err" || true
	if cmp "${dir}/out" "${expected_out}" && cmp "${dir}/err" "${expected_err}"; then
		echo "✅ ${comment}"
	else
		echo "❌ ${comment}"
		: >"${dir}/failed"
	fi >"${dir}/result" 2>&1
}

run_test() {
	local work running=0 n=0 i s b
	local -a dir=() skip=()
	work=$(mktemp -d)

	for s in scripts/*; do
		b=$(basename "${s}")
		dir+=("${work}/${n}")
		case " ${FE_SKIP_SCRIPTS:-} " in
			*" ${b} "*)
				skip+=("SKIP: ${s}")
				n=$((n + 1))
				continue
				;;
		esac
		skip+=("")
		if [[ ${running} -ge ${jobs} ]]; then
			wait -n || true
			running=$((running - 1))
		fi
		run_case "${dir[n]}" "tests/${b}.out" "tests/${b}.err" \
			"${s}${FE_COMMENT:-}" scripts/assert.fe "${s}" &
		running=$((running + 1))
		n=$((n + 1))
	done

	# Command-line evaluation, the one case that is not a script file.
	dir+=("${work}/${n}")
	skip+=("")
	run_case "${dir[n]}" "tests/one-liner.out" "tests/one-liner.err" \
		"one-liner${FE_COMMENT:-}" -e '(print "hello, world!")' &
	n=$((n + 1))
	wait

	for ((i = 0; i < n; i++)); do
		if [[ -n ${skip[i]} ]]; then
			echo "${skip[i]}"
			continue
		fi
		cat "${dir[i]}/result"
		if [[ -e ${dir[i]}/failed ]]; then
			failed=$((failed + 1))
		fi
	done
	rm -rf "${work}"
}

# FE_BIN names an interpreter the caller has already built -- `make
# perf-check`'s counting build, which lives in its own object directory and
# must not be rebuilt by the suite it is being measured through. Unset means
# the interpreter in the working directory.
#
# Building it is make's job whenever make is the caller: `run-scripts` names
# $(TARGET) as a prerequisite, and a second make started from inside a recipe
# would race the first one -- under -B both of them rebuild fe.o and
# tiny-regex-c/re.o, which the first one may be linking into gc_stress or
# payload_tests at that moment. MAKELEVEL is set in every recipe environment
# and nowhere else, so a standalone `./test.sh` still builds for itself. It
# no longer cleans first: $(BUILD_STAMP) is a prerequisite of every object,
# so a changed flag set already forces the rebuild the clean was there for.
if [[ -z ${FE_BIN:-} ]]; then
	FE_BIN=./fe
	[[ -n ${MAKELEVEL:-} ]] || make fe
fi
run_test

if [[ $failed -eq 0 ]]; then
	echo "✅ all tests passed"
else
	echo "❌ $failed tests failed"
fi
exit "$failed"
