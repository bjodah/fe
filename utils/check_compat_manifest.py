#!/usr/bin/env python3
"""Validate a compat/features.json manifest and its corpus on disk.

Structural checks only -- this never runs Fe or Emacs.  It exists so a
manifest entry cannot silently drift from the cases and snapshots it
claims to own, and so kg's sibling manifest (test/lisp-compat/features.json,
sub-plan 00C) cannot silently claim an id this one already uses.

See ../compat/README.md for the schema this enforces.
"""

import argparse
import json
import sys
from pathlib import Path

SCHEMA = "fe-compat-features/1"
STATUSES = {"supported", "planned", "divergent", "unsupported"}
OWNERS = {"fe-core", "fe-library", "kg"}
COMPARISONS = {"emacs", "kg-policy"}


def load_manifest(path):
	with open(path, "r", encoding="utf-8") as fp:
		data = json.load(fp)
	if data.get("schema") != SCHEMA:
		raise SystemExit(f"FAIL: {path}: schema {data.get('schema')!r}, "
				 f"expected {SCHEMA!r}")
	return data


def check_manifest(path, corpus_root):
	data = load_manifest(path)
	features = data.get("features", [])
	errors = []

	seen_ids = set()
	claimed_cases = set()
	for feature in features:
		fid = feature.get("id")
		where = f"{path}:{fid or '<no id>'}"
		if not fid:
			errors.append(f"{where}: missing id")
			continue
		if fid in seen_ids:
			errors.append(f"{where}: duplicate id within {path}")
		seen_ids.add(fid)

		status = feature.get("status")
		if status not in STATUSES:
			errors.append(f"{where}: status {status!r} not in {sorted(STATUSES)}")
		owner = feature.get("owner")
		if owner not in OWNERS:
			errors.append(f"{where}: owner {owner!r} not in {sorted(OWNERS)}")
		comparison = feature.get("comparison")
		if comparison not in COMPARISONS:
			errors.append(f"{where}: comparison {comparison!r} not in "
				      f"{sorted(COMPARISONS)}")

		cases = feature.get("cases") or []
		if not cases:
			errors.append(f"{where}: names no cases")
		for cid in cases:
			claimed_cases.add(cid)
			case_path = corpus_root / "cases" / f"{cid}.json"
			if not case_path.exists():
				errors.append(f"{where}: case file missing: {case_path}")
			if comparison == "emacs":
				oracle_path = corpus_root / "oracle" / f"{cid}.json"
				if not oracle_path.exists():
					errors.append(
						f"{where}: comparison=emacs but no oracle "
						f"snapshot: {oracle_path} (run "
						"'make compat-oracle')")

		if comparison == "kg-policy" and not feature.get("kg_test"):
			errors.append(f"{where}: comparison=kg-policy names no kg_test")
		if owner == "kg" and not feature.get("kg_test"):
			errors.append(f"{where}: owner=kg names no kg_test")
		if status in ("divergent", "unsupported") and not feature.get("rationale"):
			errors.append(f"{where}: status={status} gives no rationale")

	# Orphan cases: a case file with no feature entry pointing at it is
	# corpus that no manifest row explains -- exactly the drift this
	# check exists to catch.
	cases_dir = corpus_root / "cases"
	if cases_dir.is_dir():
		on_disk = {p.stem for p in cases_dir.glob("*.json")}
		orphans = sorted(on_disk - claimed_cases)
		for cid in orphans:
			errors.append(f"{path}: case {cid!r} has no feature entry")

	return seen_ids, errors


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--manifest", required=True, type=Path,
			    help="path to a features.json to check")
	parser.add_argument("--corpus-root", type=Path,
			    help="corpus root holding cases/ and oracle/ "
				 "(default: --manifest's directory)")
	parser.add_argument("--other-manifest", type=Path, action="append",
			    default=[],
			    help="sibling features.json whose ids must not "
				 "collide with --manifest's (repeatable)")
	args = parser.parse_args()

	corpus_root = args.corpus_root or args.manifest.parent
	own_ids, errors = check_manifest(args.manifest, corpus_root)

	for other in args.other_manifest:
		if not other.exists():
			print(f"# {other}: not present yet, skipping collision check")
			continue
		other_data = load_manifest(other)
		other_ids = {f.get("id") for f in other_data.get("features", [])}
		collisions = sorted(own_ids & other_ids)
		for cid in collisions:
			errors.append(f"id {cid!r} is claimed by both {args.manifest} "
				      f"and {other}")

	print(f"compat manifest check: {len(own_ids)} feature(s) in "
	      f"{args.manifest}, {len(errors)} problem(s)")
	if errors:
		print("FAIL:", file=sys.stderr)
		for line in errors:
			print(f"  {line}", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
