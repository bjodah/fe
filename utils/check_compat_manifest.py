#!/usr/bin/env python3
"""Validate a compat/features.json manifest and its corpus on disk.

Structural checks only -- this never runs Fe or Emacs.  It exists so a
manifest entry cannot silently drift from the cases and snapshots it
claims to own, and so kg's sibling manifest (test/lisp-compat/features.json,
sub-plan 00C) cannot silently claim an id this one already uses.

With --primitive-source, it also checks the manifest against the *source*:
every name in fe.c's primitive_names[] and primitive_aliases[] has to be
claimed by some entry's "source_name", and no entry may claim a name that
is not there.  That check is the reason this option exists at all.  It used
to live only in kg's utils/check_lisp_compat.py, which fe's own `make
compat` does not run, so a primitive could -- and did -- ship unclaimed from
inside this repository and only be caught minutes before kg moved its
submodule pin (commit 17ac959 added `eq` and `eql` with no manifest entry;
66ff904 fixed it).  Keeping a copy here is deliberate duplication: fe is
usable standalone and its gate cannot depend on a script in the parent
checkout.

Coverage, not uniqueness, exactly as kg's version settled on: one source
declaration may legitimately have several separately pinned behaviours,
each with its own case and snapshot, which is how this manifest splits
`funcall` into lisp2-funcall-designator and lisp2-funcall-callable-kind.
The invariant worth gating is the "no entry at all" clause.

See ../compat/README.md for the schema this enforces.
"""

import argparse
import json
import re
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


def parse_primitives(source):
	"""Every name fe.c binds as a primitive, plus its aliases.

	Two array literals, read as text rather than by compiling anything:
	primitive_names[] is designated-initializer entries of the form
	`[PFoo] = "foo"`, and primitive_aliases[] is `{"fn", PFn}` pairs.  A
	missing array is a hard failure, not an empty pool -- an empty pool
	would make the coverage check below pass vacuously, which is the one
	way this gate could quietly stop gating.
	"""
	text = re.sub(r"/\*.*?\*/", "", source.read_text(encoding="utf-8"), flags=re.S)
	names_block = re.search(
		r"static const char\* primitive_names\[\] = \{(.*?)\};", text, re.S)
	if not names_block:
		raise SystemExit(f"FAIL: could not find primitive_names[] in {source}")
	names = set(re.findall(r'=\s*"([^"]+)"', names_block.group(1)))

	aliases_block = re.search(
		r"static const PrimitiveAlias primitive_aliases\[\] = \{(.*?)\};",
		text, re.S)
	if not aliases_block:
		raise SystemExit(
			f"FAIL: could not find primitive_aliases[] in {source}")
	aliases = set(re.findall(r'\{"([^"]+)"', aliases_block.group(1)))
	if not names:
		raise SystemExit(f"FAIL: primitive_names[] in {source} parsed empty")
	return names | aliases


def check_primitive_coverage(path, features, source):
	"""Every primitive claimed by an entry, and every claim a primitive."""
	pool = parse_primitives(source)
	claims = {}
	for feature in features:
		name = feature.get("source_name")
		if name is None:
			continue
		claims.setdefault(name, []).append(feature.get("id"))

	errors = []
	for name in sorted(pool):
		if not claims.get(name):
			errors.append(
				f"{path}: primitive {name!r} ({source}) has no feature "
				"entry naming it as source_name")
	for name in sorted(set(claims) - pool):
		errors.append(
			f"{path}: source_name {name!r} ({claims[name]}) is not a "
			f"primitive or alias in {source} -- stale entry or a typo")
	print(f"source inventory: {len(pool)} fe primitives/aliases in {source}, "
	      f"{len(claims)} claimed by {path}")
	return errors


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
	parser.add_argument("--primitive-source", type=Path,
			    help="fe.c, whose primitive_names[]/"
				 "primitive_aliases[] every entry's source_name "
				 "is checked against; omit for a manifest that "
				 "does not own that pool (kg's)")
	args = parser.parse_args()

	corpus_root = args.corpus_root or args.manifest.parent
	own_ids, errors = check_manifest(args.manifest, corpus_root)

	# Opt-in, and loud when it is off: kg's check_lisp_compat.py runs this
	# same script over *its* manifest, which does not claim fe's primitives
	# and must not be failed for that.  fe's own `make compat` always passes
	# the flag.
	if args.primitive_source:
		errors += check_primitive_coverage(
			args.manifest, load_manifest(args.manifest).get("features", []),
			args.primitive_source)
	else:
		print("# --primitive-source not given: source_name coverage "
		      "against fe.c's primitive_names[] NOT checked")

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
