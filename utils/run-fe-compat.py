#!/usr/bin/env python3
"""Run the compat corpus against the standalone fe binary and the checked-in
Emacs oracle snapshots.  No Emacs required -- that is what the snapshots
are for; see utils/run-emacs-oracle.py for the target that regenerates them.

For each case this drives `fe` fresh (one process per case: the fe binary's
non-interactive error handler calls exit() on the first error, so isolation
is a subprocess, not a REPL loop) and classifies the result *structurally*:

  * a case whose feature status is "unsupported" is never run at all --
    the manifest already says fe cannot do this, so the record is decided
    from that metadata, not by running fe and pattern-matching whatever
    error a missing primitive happens to produce;
  * otherwise, exit code 0 means the case's wrapping (print EXPR) printed
    a value, exit code != 0 means fe's HandleFatalError() ran (kind
    "condition", condition_source "message" -- Fe has no structured
    condition system before Phase 6);
  * a case that does not return before --timeout reports kind "timeout"
    instead of hanging the run.

A feature's status decides whether a mismatch fails the run: "supported"
means the oracle and fe must agree, and disagreement is a regression.
"planned", "unsupported", and "divergent" are known, expected gaps and are
reported without failing -- that distinction (compatibility work still
pending vs. a regression) is this whole mechanism's reason to exist.

A feature's comparison mode decides whether an oracle is consulted at all:
"comparison": "kg-policy" (00b-oracle-and-differential-corpus.md's split)
means this construct has no Emacs analogue worth snapshotting -- fe still
runs so a crash or a runner-level failure still surfaces, but the result is
reported as pinned by the feature's kg_test rather than compared against
oracle/<id>.json, and no such snapshot is expected to exist.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_TIMEOUT = 5.0


def load_features(manifest_path):
	with open(manifest_path, "r", encoding="utf-8") as fp:
		data = json.load(fp)
	case_to_feature = {}
	for feature in data.get("features", []):
		for case_id in feature.get("cases", []):
			case_to_feature[case_id] = feature
	return case_to_feature


def run_fe_case(fe_bin, case, timeout):
	script = "\n".join(case.get("setup", [])) + f"\n(print {case['expr']})\n"
	with tempfile.NamedTemporaryFile(
			mode="w", suffix=".fe", delete=False) as tmp:
		tmp.write(script)
		tmp_path = tmp.name
	try:
		try:
			env = {**os.environ, "FE_STRUCTURED_ERRORS": "1"}
			proc = subprocess.run([fe_bin, tmp_path], capture_output=True,
					      timeout=timeout, env=env)
		except subprocess.TimeoutExpired:
			return {"kind": "timeout", "seconds": timeout}, None
	finally:
		Path(tmp_path).unlink(missing_ok=True)

	if proc.returncode < 0:
		return None, (f"fe was killed by signal {-proc.returncode} "
			      f"running {case['id']!r}; that is a runner bug or "
			      "a crash, not a documented divergence")
	if proc.returncode == 0:
		printed = proc.stdout.decode("utf-8", "replace").strip()
		return {"kind": "value", "printed": printed}, None

	stderr = proc.stderr.decode("utf-8", "replace")
	lines = stderr.splitlines()
	for index, line in enumerate(lines):
		if line.startswith("condition: "):
			condition = line[len("condition: "):]
			if condition == "quit":
				return {"kind": "quit"}, None
			record = {"kind": "condition",
				  "condition_source": "structured",
				  "condition": condition}
			# The `data:` line is emitted right after `condition:` by the
			# same branch of main.c's PrintError.
			if index + 1 < len(lines) and lines[index + 1].startswith("data: "):
				record["data"] = lines[index + 1][len("data: "):]
			return record, None
	message = stderr
	for line in stderr.splitlines():
		if line.startswith("error: "):
			message = line[len("error: "):]
			break
	return {"kind": "condition", "condition_source": "message",
		"message": message}, None


def data_gap(oracle, fe):
	"""True when the oracle carries condition data and fe carries none.

	Reported per run so the coverage of the data comparison is visible
	rather than assumed; see records_agree().
	"""
	if oracle.get("kind") != "condition" or fe.get("kind") != "condition":
		return False
	if fe.get("condition_source") != "structured":
		return False
	return bool(oracle.get("data")) and fe.get("data", "nil") == "nil"


def records_agree(oracle, fe, compare_data=True):
	if oracle["kind"] != fe["kind"]:
		return False
	kind = oracle["kind"]
	if kind == "value":
		return oracle.get("printed") == fe.get("printed")
	if kind == "condition":
		condition = oracle.get("condition", "")
		if fe.get("condition_source") == "message":
			# A weaker claim by design (see the module docstring and
			# 00b's "condition_source" note): a case whose fe side
			# still arrives as free text -- a raise site that has no
			# condition object to print -- is only checked for the
			# oracle's condition name appearing somewhere in the
			# message, not compared symbol for symbol.
			return bool(condition) and condition in fe.get("message", "")
		if condition != fe.get("condition"):
			return False
		if not compare_data:
			return True
		# The data list, when both sides carry one.  The oracle shim
		# prints `(prin1-to-string (cdr err))` and fe prints the same
		# rendering of the same cdr through its structured channel, so
		# where both are non-nil they must be equal, character for
		# character.
		#
		# `nil` on fe's side is not a disagreement here but a *census*:
		# fe attaches `(PREDICATE VALUE)` data to the conditions that
		# have one to attach, and its `wrong-number-of-arguments` family
		# carries no `(FUNCTION NARGS)` pair at all, which the run
		# reports per case (see `data_gap`) rather than failing on.
		# Narrowing that gap is condition-data work, not runner work.
		oracle_data = oracle.get("data")
		fe_data = fe.get("data")
		if not oracle_data or not fe_data or fe_data == "nil":
			return True
		return oracle_data == fe_data
	if kind == "unsupported":
		return oracle.get("feature") == fe.get("feature")
	# "quit", "timeout", and anything else: kind equality is the whole
	# claim.
	return True


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--fe", default="./fe", help="path to the fe binary")
	parser.add_argument("--corpus-root", type=Path, default=Path("compat"))
	parser.add_argument("--case", action="append", default=[],
			    help="run only this case id (repeatable); "
				 "default is every cases/*.json in the corpus")
	parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
	args = parser.parse_args()

	fe_bin = str(Path(args.fe).resolve())
	manifest_path = args.corpus_root / "features.json"
	case_to_feature = load_features(manifest_path)

	cases_dir = args.corpus_root / "cases"
	case_paths = sorted(cases_dir.glob("*.json"))
	if args.case:
		wanted = set(args.case)
		case_paths = [p for p in case_paths if p.stem in wanted]

	passed = 0
	known_gaps = 0
	failures = []
	data_gaps = []
	for case_path in case_paths:
		with open(case_path, "r", encoding="utf-8") as fp:
			case = json.load(fp)
		case_id = case["id"]
		feature = case_to_feature.get(case_id)
		if feature is None:
			failures.append(f"{case_id}: no features.json entry names "
					f"this case (run check_compat_manifest.py)")
			continue

		if feature["status"] == "unsupported":
			fe_record = {"kind": "unsupported", "feature": feature["id"]}
			runner_error = None
		else:
			fe_record, runner_error = run_fe_case(fe_bin, case, args.timeout)
		if runner_error:
			failures.append(f"{case_id}: {runner_error}")
			continue

		# comparison: kg-policy means this feature has no Emacs oracle at
		# all (see ../compat/README.md and 00b-oracle-and-differential-
		# corpus.md's split) -- its correctness is pinned by kg_test, not
		# by agreement with a snapshot, so there is deliberately no
		# oracle/<id>.json for it and none is expected here.
		if feature.get("comparison") == "kg-policy":
			passed += 1
			print(f"# {case_id} (feature {feature['id']}, comparison "
			      f"kg-policy): pinned by kg_test {feature.get('kg_test')!r}, "
			      f"not compared to an Emacs oracle -- fe={fe_record!r}")
			continue

		oracle_path = args.corpus_root / "oracle" / f"{case_id}.json"
		if not oracle_path.exists():
			failures.append(f"{case_id}: no oracle snapshot at "
					f"{oracle_path} (run 'make compat-oracle')")
			continue
		with open(oracle_path, "r", encoding="utf-8") as fp:
			snapshot = json.load(fp)
		oracle_record = snapshot["record"]

		# A case may opt out of the data comparison, and must say why in
		# its note: the only legitimate reason is an oracle rendering fe
		# cannot produce at all (an Emacs object with no fe analogue),
		# never a disagreement fe could fix.
		compare_data = case.get("compare_data", True)
		agree = records_agree(oracle_record, fe_record, compare_data)
		if data_gap(oracle_record, fe_record):
			data_gaps.append(case_id)
		if feature["status"] == "supported":
			if agree:
				passed += 1
			else:
				failures.append(
					f"{case_id} (feature {feature['id']}, status "
					f"supported): oracle={oracle_record!r} "
					f"fe={fe_record!r}")
		else:
			known_gaps += 1
			tag = "agrees early" if agree else "known gap"
			print(f"# {case_id} (feature {feature['id']}, status "
			      f"{feature['status']}): {tag} -- "
			      f"oracle={oracle_record!r} fe={fe_record!r}")

	if data_gaps:
		print(f"# condition data: {len(data_gaps)} case(s) where the "
		      f"oracle carries data and fe carries none: "
		      f"{' '.join(sorted(data_gaps))}")
	print(f"fe compat: {len(case_paths)} case(s), {passed} passed, "
	      f"{known_gaps} known gap(s) (not required to match yet), "
	      f"{len(failures)} failed")
	if failures:
		print("FAIL:", file=sys.stderr)
		for line in failures:
			print(f"  {line}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
