#!/usr/bin/env python3
"""Check that every tracked fuzz seed still reaches the construct it exists for.

A tracked seed under fuzz/seeds/<target>/ is a *steering* input: the grammar
in fuzz/fuzz_eval.c turns its bytes into forms, and the seed is checked in so
that a freshly regenerated, gitignored corpus still walks a named construct on
every `make fuzz-*-smoke`.  That relationship is entirely implicit -- the seed
is opaque bytes, and any change to the grammar (a new `switch` arm, a wider
modulus, a different `MaxDepth`) re-steers every one of them at once.

It has already gone wrong once.  Phase 9's `MaxDepth` 4 -> 6 and the two arms
that widened `BuildExpression`'s modulus from 34 to 36 silently invalidated
six of the fourteen tracked eval seeds: `funcall-apply-redispatch` reached
neither `funcall` nor `apply`, `strict-arity-rest` built no `&rest` list,
`strict-arity-native-too-few` built `(>=)`, and the seeds' README went on
recording the old counts.  Nothing failed, because nothing was checking.

This is the check.  With FE_FUZZ_DUMP=1 the harness writes every form it
builds to stderr, so "does this seed still reach that construct" is a
substring question, and fuzz/seeds/reachability.json is where the answer is
written down.  The manifest is also a census: a seed with no entry fails, and
an entry with no seed fails, so a new seed has to say what it is for.

Usage:
  utils/verify_fuzz_seeds.py --fuzzer ./fuzz/fuzz_eval \\
      --manifest fuzz/seeds/reachability.json --seed-dir fuzz/seeds/eval \\
      --target eval
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

SCHEMA = "fe-fuzz-seed-reachability/1"


def load_manifest(path, target):
    with open(path, encoding="utf-8") as handle:
        manifest = json.load(handle)
    if manifest.get("schema") != SCHEMA:
        sys.exit(f"{path}: expected schema {SCHEMA!r}, got {manifest.get('schema')!r}")
    targets = manifest.get("targets", {})
    if target not in targets:
        sys.exit(f"{path}: no entry for target {target!r}")
    return targets[target]


def run_seed(fuzzer, seed):
    environment = dict(os.environ, FE_FUZZ_DUMP="1")
    # libFuzzer runs an explicitly named file once and exits; the harness's
    # dump and libFuzzer's own chatter both land on stderr.
    completed = subprocess.run(
        [fuzzer, str(seed)],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    return completed.returncode, completed.stdout.decode("utf-8", "replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fuzzer", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--seed-dir", required=True)
    parser.add_argument("--target", required=True)
    arguments = parser.parse_args()

    entries = load_manifest(arguments.manifest, arguments.target)
    seed_dir = Path(arguments.seed_dir)
    on_disk = sorted(path.name for path in seed_dir.iterdir() if path.is_file())

    failures = []
    for name in sorted(set(on_disk) | set(entries)):
        if name not in entries:
            failures.append(f"{name}: on disk but not in {arguments.manifest}")
            continue
        if name not in on_disk:
            failures.append(f"{name}: in the manifest but not in {seed_dir}")
            continue
        required = entries[name].get("requires", [])
        status, output = run_seed(arguments.fuzzer, seed_dir / name)
        if status != 0:
            failures.append(f"{name}: {arguments.fuzzer} exited {status}")
            continue
        missing = [text for text in required if text not in output]
        if missing:
            for text in missing:
                failures.append(f"{name}: no longer reaches {text!r}")
            continue
        reached = "no construct claimed" if not required else f"{len(required)} reached"
        print(f"ok   {name}: {reached}")

    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        print(
            "\nA tracked seed steers the grammar by its bytes, so a grammar change\n"
            "re-steers it. Re-derive the seed (or the manifest entry) rather than\n"
            "deleting the claim: fuzz/seeds/README.md records how.",
            file=sys.stderr,
        )
        return 1
    print(f"{len(entries)} tracked {arguments.target} seeds verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
