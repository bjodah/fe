#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

compile_db_files() {
	python3 - <<'PY'
import json
with open("compile_commands.json", "r", encoding="utf-8") as fp:
    for entry in json.load(fp):
        print(entry["file"])
PY
}

run_clang_check() {
	local file=$1
	local out

	cd "$(dirname "${file}")"
	out=$(mktemp)
	trap 'rm -f "${out}"' RETURN
	clang-check -analyze -p "${COMPILE_DB}" "$(basename "${file}")" 2>&1 | tee "${out}"
	if rg -q "warning:" "${out}"; then
		exit 1
	fi
}

run_clang_tidy() {
	local file=$1
	local out

	out=$(mktemp)
	trap 'rm -f "${out}"' RETURN
	if ! clang-tidy --quiet -p "${COMPILE_DB}" "${file}" >"${out}" 2>&1; then
		cat "${out}"
		exit 1
	fi
}

export -f run_clang_check run_clang_tidy
export CC="ccache clang"

# https://btorpey.github.io/blog/2015/04/27/static-analysis-with-clang/
bear -- "${MAKE_PARALLEL[@]}" -B
export COMPILE_DB=$(/bin/pwd)
make iwyu

compile_db_files | "${PARALLEL}" --halt soon,fail=1 --jobs "${JOBS}" --line-buffer run_clang_check

cppcheck --quiet --error-exitcode=1 --suppress=normalCheckLevelMaxBranches -j "${JOBS}" src/*.c

compile_db_files | "${PARALLEL}" --halt soon,fail=1 --jobs "${JOBS}" --line-buffer run_clang_tidy
