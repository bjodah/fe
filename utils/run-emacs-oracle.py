#!/usr/bin/env python3
"""Regenerate and verify the Emacs oracle snapshots for a compat corpus.

Drives `emacs -Q --batch -l oracle/emacs-shim.el CASE.json` once per
Emacs-compared case and writes oracle/<id>.json: the shim's one-line JSON
record plus the exact Emacs version that produced it.  Which cases those
are comes from features.json, not from globbing cases/ -- see
emacs_compared_cases().  "Emacs 31" is not a pin -- a
branch build's behaviour can move -- so an existing snapshot is never
silently overwritten by a run under a different version; that fails
loudly instead (--allow-version-change is the deliberate override).

Takes an explicit corpus root (a directory holding cases/, oracle/, and
oracle/emacs-shim.el) rather than assuming fe/compat, so sub-plan 00C can
point this at kg's test/lisp-compat/ without copying the runner.

Resolution order mirrors utils/pty_accept.py's resolve_emacs() and
utils/regex_differential.py exactly, on purpose (see
00b-oracle-and-differential-corpus.md, "The oracle is already
resolvable"): --emacs, then $KG_PTY_EMACS, then `emacs` on PATH, then the
/opt-3 developer-box pin.  Missing Emacs SKIPs (exit 0) unless
--require-tools is given, which turns it into a named failure.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

SCHEMA = "fe-compat-oracle/1"
# Last resort only; anything with `emacs` on PATH (a CI image, a distro
# install) or $KG_PTY_EMACS is served long before this is reached.
EMACS_FALLBACK = "/opt-3/emacs-31-lucid/bin/emacs"
DEFAULT_TIMEOUT = 5.0


def resolve_emacs(explicit):
	"""Find the Emacs the oracle runs under, or None.

	Same order as utils/pty_accept.py's resolve_emacs(): an explicit
	--emacs is taken at its word (a wrong path says so, rather than
	silently falling back to something that happens to work), then
	$KG_PTY_EMACS, then PATH, then the developer-box pin.
	"""
	if explicit:
		return explicit if os.access(explicit, os.X_OK) else None
	env = os.environ.get("KG_PTY_EMACS")
	if env:
		return env if os.access(env, os.X_OK) else None
	found = shutil.which("emacs")
	if found:
		return found
	if os.access(EMACS_FALLBACK, os.X_OK):
		return EMACS_FALLBACK
	return None


def emacs_version(emacs):
	proc = subprocess.run(
		[emacs, "-Q", "--batch", "--eval", "(princ (emacs-version))"],
		capture_output=True, timeout=DEFAULT_TIMEOUT)
	if proc.returncode != 0:
		raise SystemExit(
			f"FAIL: {emacs} --eval '(emacs-version)' exited "
			f"{proc.returncode}: {proc.stderr.decode('utf-8', 'replace')}")
	return proc.stdout.decode("utf-8").strip()


def emacs_compared_cases(corpus_root):
	"""Case ids the oracle owns: those of `comparison: "emacs"` features.

	A case belongs to a feature, and a feature says what it is compared
	against.  Only `emacs` features have an Emacs answer at all: a
	`kg-policy` feature records a decision this project made, so running
	Emacs over its case would write a snapshot the corpus deliberately
	does not have -- and `primitive-print`, whose expression writes to
	stdout itself, would break the shim's one-record-per-line protocol
	and abort the whole run.  Enumerating from the manifest rather than
	globbing cases/ is what keeps those two facts in one place.
	"""
	manifest = corpus_root / "features.json"
	with open(manifest, "r", encoding="utf-8") as fp:
		features = json.load(fp)["features"]
	return {case
		for feature in features if feature["comparison"] == "emacs"
		for case in feature["cases"]}


def run_case(emacs, shim, case_path, timeout):
	try:
		proc = subprocess.run(
			[emacs, "-Q", "--batch", "-l", str(shim), str(case_path)],
			capture_output=True, timeout=timeout)
	except subprocess.TimeoutExpired:
		return {"kind": "timeout", "seconds": timeout}
	if proc.returncode != 0:
		stderr = proc.stderr.decode("utf-8", "replace")
		raise SystemExit(
			f"FAIL: oracle shim exited {proc.returncode} for {case_path}, "
			f"which the shim itself should never do (it catches error "
			f"and quit); stderr:\n{stderr}")
	lines = [line for line in proc.stdout.decode("utf-8").splitlines() if line]
	if len(lines) != 1:
		raise SystemExit(
			f"FAIL: oracle shim printed {len(lines)} line(s) for "
			f"{case_path}, expected exactly 1: {lines!r}")
	return json.loads(lines[0])


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("corpus_root", type=Path,
			    help="directory with cases/, oracle/, "
				 "oracle/emacs-shim.el (e.g. compat, or kg's "
				 "test/lisp-compat)")
	parser.add_argument("--emacs", default="",
			    help="path to the Emacs oracle binary "
				 "(default: $KG_PTY_EMACS, then PATH, then "
				 "the /opt-3 pin)")
	parser.add_argument("--require-tools", action="store_true",
			    help="fail naming the tool instead of skipping "
				 "when Emacs is not found")
	parser.add_argument("--allow-version-change", action="store_true",
			    help="deliberately re-pin snapshots to a "
				 "different Emacs version instead of failing")
	parser.add_argument("--case", action="append", default=[],
			    help="regenerate only this case id (repeatable); "
				 "default is every cases/*.json in the corpus")
	parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
			    help="per-case wall-clock bound; a case that "
				 "does not terminate reports kind=timeout "
				 "instead of hanging the run")
	args = parser.parse_args()

	emacs = resolve_emacs(args.emacs)
	if emacs is None:
		msg = ("emacs (set --emacs, $KG_PTY_EMACS, put it on PATH, or "
		       f"install it at {EMACS_FALLBACK})")
		if args.require_tools:
			print(f"FAIL: missing tool: {msg}", file=sys.stderr)
			return 1
		print(f"SKIP: {msg}, oracle snapshots not regenerated")
		return 0

	shim = args.corpus_root / "oracle" / "emacs-shim.el"
	if not shim.exists():
		raise SystemExit(f"FAIL: oracle shim missing: {shim}")

	version = emacs_version(emacs)
	print(f"# oracle: {emacs} ({version})")

	cases_dir = args.corpus_root / "cases"
	case_paths = sorted(cases_dir.glob("*.json"))
	oracle_cases = emacs_compared_cases(args.corpus_root)
	skipped = [p for p in case_paths if p.stem not in oracle_cases]
	case_paths = [p for p in case_paths if p.stem in oracle_cases]
	if args.case:
		wanted = set(args.case)
		not_compared = sorted(wanted & {p.stem for p in skipped})
		if not_compared:
			raise SystemExit(
				f"FAIL: --case {not_compared} belongs to a feature "
				"that is not compared against Emacs; it has no "
				"oracle snapshot by design")
		case_paths = [p for p in case_paths if p.stem in wanted]
		missing = wanted - {p.stem for p in case_paths}
		if missing:
			raise SystemExit(f"FAIL: --case not found: {sorted(missing)}")

	written = 0
	unchanged = 0
	failed = []
	for case_path in case_paths:
		case_id = case_path.stem
		oracle_path = args.corpus_root / "oracle" / f"{case_id}.json"
		record = run_case(emacs, shim, case_path, args.timeout)

		existing = None
		if oracle_path.exists():
			with open(oracle_path, "r", encoding="utf-8") as fp:
				existing = json.load(fp)

		if (existing is not None and existing.get("emacs_version") != version
				and not args.allow_version_change):
			failed.append(
				f"{case_id}: snapshot recorded under "
				f"{existing.get('emacs_version')!r}, running "
				f"Emacs is {version!r}; refusing to overwrite "
				"(pass --allow-version-change to intentionally "
				"re-pin)")
			continue

		snapshot = {
			"schema": SCHEMA,
			"case": case_id,
			"emacs_version": version,
			"record": record,
		}
		if existing == snapshot:
			unchanged += 1
			continue
		with open(oracle_path, "w", encoding="utf-8") as fp:
			json.dump(snapshot, fp, indent=1, sort_keys=False)
			fp.write("\n")
		written += 1

	print(f"oracle: {len(case_paths)} case(s), {written} written/updated, "
	      f"{unchanged} unchanged, {len(failed)} failed, "
	      f"{len(skipped)} not compared against Emacs")
	if failed:
		print("FAIL:", file=sys.stderr)
		for line in failed:
			print(f"  {line}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
