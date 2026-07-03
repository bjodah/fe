#!/usr/bin/env python3
"""Check scc JSON output against simple complexity budgets."""

import argparse
import json
import sys


def iter_files(languages):
	for language in languages:
		for file_info in language.get("Files", []):
			yield file_info


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--max-total", type=int, required=True)
	parser.add_argument("--max-file", type=int, required=True)
	parser.add_argument("--top", type=int, default=10)
	args = parser.parse_args()

	try:
		languages = json.load(sys.stdin)
	except json.JSONDecodeError as exc:
		print(f"invalid scc JSON: {exc}", file=sys.stderr)
		return 2

	total = sum(language.get("Complexity", 0) for language in languages)
	files = sorted(
		iter_files(languages),
		key=lambda file_info: file_info.get("Complexity", 0),
		reverse=True,
	)

	print(f"scc total complexity: {total} (limit {args.max_total})")
	print(f"scc max file complexity: {args.max_file}")
	print("scc most complex files:")
	for file_info in files[:args.top]:
		location = file_info.get("Location", file_info.get("Filename", "<unknown>"))
		complexity = file_info.get("Complexity", 0)
		code = file_info.get("Code", 0)
		print(f"  {complexity:4d}  {code:5d} LOC  {location}")

	failed = False
	if total > args.max_total:
		print(
			f"FAIL: total complexity {total} exceeds limit {args.max_total}",
			file=sys.stderr,
		)
		failed = True

	over_limit = [
		file_info for file_info in files
		if file_info.get("Complexity", 0) > args.max_file
	]
	if over_limit:
		print(
			f"FAIL: {len(over_limit)} file(s) exceed per-file limit {args.max_file}",
			file=sys.stderr,
		)
		for file_info in over_limit:
			location = file_info.get("Location", file_info.get("Filename", "<unknown>"))
			complexity = file_info.get("Complexity", 0)
			print(f"  {complexity:4d}  {location}", file=sys.stderr)
		failed = True

	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
