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
"""

import argparse
import json
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
			proc = subprocess.run([fe_bin, tmp_path],
					      capture_output=True, timeout=timeout)
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
	message = stderr
	for line in stderr.splitlines():
		if line.startswith("error: "):
			message = line[len("error: "):]
			break
	return {"kind": "condition", "condition_source": "message",
		"message": message}, None


def records_agree(oracle, fe):
	if oracle["kind"] != fe["kind"]:
		return False
	kind = oracle["kind"]
	if kind == "value":
		return oracle.get("printed") == fe.get("printed")
	if kind == "condition":
		condition = oracle.get("condition", "")
		if fe.get("condition_source") == "message":
			# A weaker claim by design (see the module docstring and
			# 00b's "condition_source" note): fe has no structured
			# condition symbol yet, so this checks whether the
			# oracle's condition name shows up in fe's free-text
			# message rather than comparing symbols.
			return bool(condition) and condition in fe.get("message", "")
		return condition == fe.get("condition")
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

		oracle_path = args.corpus_root / "oracle" / f"{case_id}.json"
		if not oracle_path.exists():
			failures.append(f"{case_id}: no oracle snapshot at "
					f"{oracle_path} (run 'make compat-oracle')")
			continue
		with open(oracle_path, "r", encoding="utf-8") as fp:
			snapshot = json.load(fp)
		oracle_record = snapshot["record"]

		agree = records_agree(oracle_record, fe_record)
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
