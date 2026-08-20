#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

usage() {
	cat <<'EOF'
usage: run-ci-steps.sh [--serial] [--lanes N]

The numbered .ci/ci-NN-*.sh steps run concurrently. Every step gets a
throwaway copy of this working tree -- uncommitted changes included, build
products left behind -- so that no two of them fight over the same object
files, and each writes its own log under .ci/.run/logs/. The terminal gets a
PASS/FAIL line per step and a replay of every failing log; the exit status is
non-zero when any step failed.

  --serial   Run the steps one after another in this working tree, streaming
             to the terminal and stopping at the first failure. That is what
             this runner did before it learned to use more than one core, and
             it is the escape hatch when a step's own output, in the tree you
             are editing, is what you want.
  --lanes N  How many steps run at once. The default is nproc/4 clamped to
             2..5 and never more than there are steps; each lane then builds
             with JOBS/lanes, so the lanes together ask for the box once.

A run takes a lock in .ci/.run: a second run in the same tree is refused,
since the two would fight over the objects in it. A lock whose process is
gone is reported as stale and taken over.
EOF
}

serial=0
lanes=""
while [ "$#" -gt 0 ]; do
	case "$1" in
	--serial) serial=1 ;;
	--lanes)
		shift
		lanes=${1:-}
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "run-ci-steps.sh: unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
	shift
done

python_activate=$(compgen -G "/opt-?/cpython-v3.*-apt-deb/bin/activate" | head -n 1 || true)
if [[ -n ${python_activate} ]]; then
	source "${python_activate}"
fi
# What the caller asked for, before ci-env.sh defaults it to JOBS: a lane
# divides JOBS below, and the script suite's pool has to follow it down
# unless someone named a size of their own.
test_jobs=${FE_TEST_JOBS:-}
source .ci/ci-env.sh

steps=(.ci/ci-[0-9][0-9]-*.sh)
run_dir=.ci/.run

step_name() {
	local base=${1##*/}

	echo "${base%.sh}"
}

# Both modes take the lock: two runs in one tree fight over its objects
# whether or not either of them is using more than one core.
mkdir -p "${run_dir}/logs"
lock_file=${run_dir}/lock
lane_root=""
keep_lanes=0
if ! (
	set -o noclobber
	echo $$ >"${lock_file}"
) 2>/dev/null; then
	holder=$(cat "${lock_file}" 2>/dev/null || true)
	if [ -n "${holder}" ] && kill -0 "${holder}" 2>/dev/null; then
		echo "run-ci-steps.sh: pid ${holder} is already running in this" \
			"tree (${lock_file})" >&2
		exit 2
	fi
	echo "run-ci-steps.sh: stale lock from pid ${holder:-?}; taking it over" >&2
	echo $$ >"${lock_file}"
fi
trap 'rm -f "${lock_file}"
	if [ -n "${lane_root}" ] && [ "${keep_lanes}" -eq 0 ]; then
		rm -rf "${lane_root}"
	fi' EXIT

if [ "${serial}" -eq 1 ]; then
	export JOBS GNU_PARALLEL VALGRIND
	for step in "${steps[@]}"; do
		echo "== $(step_name "${step}")"
		"${step}"
	done
	exit 0
fi

if [ -z "${lanes}" ]; then
	lanes=$(($(nproc 2>/dev/null || echo 2) / 4))
fi
if [ "${lanes}" -lt 2 ]; then
	lanes=2
fi
if [ "${lanes}" -gt 5 ]; then
	lanes=5
fi
if [ "${lanes}" -gt "${#steps[@]}" ]; then
	lanes=${#steps[@]}
fi
JOBS=$((JOBS / lanes))
if [ "${JOBS}" -lt 1 ]; then
	JOBS=1
fi
export JOBS GNU_PARALLEL VALGRIND
export FE_TEST_JOBS=${test_jobs:-${JOBS}}

lane_root=$(mktemp -d "${TMPDIR:-/tmp}/fe-ci-lanes-XXXXXX")

# A lane starts from the working tree, uncommitted changes and all -- that is
# what is under test -- but not from its build products: every heavy step
# passes -B, and the ones that do not would otherwise link objects this tree
# was left with. The named excludes are the cheap half; `make clean` in the
# copy is the rest of the list, without duplicating it here.
copy_tree() {
	local dest=$1

	mkdir -p "${dest}"
	tar -cf - --exclude=./.ci/.run --exclude=./coverage \
		--exclude=./compile_commands.json --exclude=./perfobj \
		--exclude='*.o' --exclude='*.gcda' --exclude='*.gcno' \
		--exclude='*.plist' . | tar -xf - -C "${dest}"
	make -C "${dest}" -s clean coverage-clean
}

run_step() {
	local step=$1 name=$2 lane=$3 log=$4
	local rc=0 start=${SECONDS}

	{ copy_tree "${lane}" && "${lane}/${step}"; } >"${log}" 2>&1 || rc=$?
	echo "${rc} $((SECONDS - start))" >"${run_dir}/${name}.result"
	if [ "${rc}" -eq 0 ]; then
		# The coverage report is the one artifact a lane produces that
		# someone reads afterwards; a serial run leaves it in the tree,
		# so a parallel one puts it back there too.
		if [ -d "${lane}/coverage" ]; then
			rm -rf coverage
			mv "${lane}/coverage" coverage
		fi
		# A passing step's tree has nothing left to look at, and ten of
		# them at once is a box worth of disk on a small builder.
		rm -rf "${lane}"
		printf 'PASS %-30s %5ds\n' "${name}" "$((SECONDS - start))" >&3
	else
		printf 'FAIL %-30s %5ds (exit %d) %s\n' "${name}" \
			"$((SECONDS - start))" "${rc}" "${log}" >&3
	fi
}

# fd 3 is the terminal; everything a step prints goes to its own log.
exec 3>&1
echo "running ${#steps[@]} steps, ${lanes} at a time, JOBS=${JOBS} per lane"
echo "logs: ${run_dir}/logs   lane trees: ${lane_root}"

for step in "${steps[@]}"; do
	name=$(step_name "${step}")
	rm -f "${run_dir}/${name}.result"
	while [ "$(jobs -rp | wc -l)" -ge "${lanes}" ]; do
		wait -n || true
	done
	run_step "${step}" "${name}" "${lane_root}/${name}" \
		"${run_dir}/logs/${name}.log" &
done
wait

failed=0
for step in "${steps[@]}"; do
	name=$(step_name "${step}")
	rc=127
	read -r rc _ <"${run_dir}/${name}.result" || true
	if [ -z "${rc}" ]; then
		rc=127
	fi
	if [ "${rc}" -ne 0 ]; then
		failed=$((failed + 1))
		echo
		echo "===== ${name} failed (exit ${rc}) ====="
		cat "${run_dir}/logs/${name}.log"
	fi
done

if [ "${failed}" -ne 0 ]; then
	keep_lanes=1
	echo
	echo "${failed} of ${#steps[@]} steps failed; their trees are in ${lane_root}"
	exit 1
fi
echo "all ${#steps[@]} steps passed"
