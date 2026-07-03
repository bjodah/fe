#!/usr/bin/env python3
"""Check pmccabe output against a per-function complexity budget."""

import argparse
import sys


def iter_functions(lines):
	for line in lines:
		parts = line.split(None, 5)
		if len(parts) != 6:
			continue
		try:
			complexity = int(parts[0])
			traditional = int(parts[1])
			statements = int(parts[2])
			first_line = int(parts[3])
			line_count = int(parts[4])
		except ValueError:
			continue
		yield {
			"complexity": complexity,
			"traditional": traditional,
			"statements": statements,
			"first_line": first_line,
			"line_count": line_count,
			"location": parts[5].strip(),
		}


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--max-function", type=int, required=True)
	parser.add_argument("--top", type=int, default=10)
	args = parser.parse_args()

	functions = sorted(
		iter_functions(sys.stdin),
		key=lambda func: func["complexity"],
		reverse=True,
	)

	max_complexity = functions[0]["complexity"] if functions else 0
	print(
		f"pmccabe max function complexity: {max_complexity} "
		f"(limit {args.max_function})"
	)
	print("pmccabe most complex functions:")
	for func in functions[:args.top]:
		print(
			f"  {func['complexity']:4d}  {func['line_count']:5d} LOC  "
			f"{func['location']}"
		)

	over_limit = [
		func for func in functions if func["complexity"] > args.max_function
	]
	if over_limit:
		print(
			f"FAIL: {len(over_limit)} function(s) exceed complexity "
			f"limit {args.max_function}",
			file=sys.stderr,
		)
		for func in over_limit:
			print(
				f"  {func['complexity']:4d}  {func['location']}",
				file=sys.stderr,
			)
		return 1

	return 0


if __name__ == "__main__":
	sys.exit(main())
