# Tracked fuzz seeds

`fuzz/corpus/` is the *working* corpus: gitignored, grown by every local and
CI fuzz run, and gone after a clean checkout. Anything it discovers is
therefore a guard only until the next machine.

This directory is the durable half. Each file here is an input that once
reproduced a real defect, checked in so `make fuzz-*-smoke` replays it on
every run forever -- the fuzz equivalent of a regression test for a bug whose
trigger depends on accumulated arena state and so cannot practically be
written as a deterministic `test_api.c` case.

Add a seed when a fuzz artifact turns out to be a genuine bug, name it after
the defect rather than its hash, and say in the commit which fix it pins.

A seed is *opaque bytes steering a grammar*, which means a change to
`fuzz/fuzz_eval.c` re-steers every seed in this directory at once, silently.
That has already happened: Phase 9's `MaxDepth` 4 -> 6 and the two arms that
widened `BuildExpression`'s modulus from 34 to 36 left six of these fourteen
files reaching none of the constructs they exist to force, while this README
went on recording the old counts. So every seed now declares what it must
reach in `fuzz/seeds/reachability.json`, and `make fuzz-eval-seed-verify`
(run first by `make fuzz-smoke`, and so by `.ci/ci-06-fuzz-smoke.sh`) replays
each one with `FE_FUZZ_DUMP=1` and checks the forms it really builds. A seed
with no manifest entry fails, and an entry with no seed fails.

A seed that stops steering is re-derived, not deleted: instrument the
builders with temporary counters, search for an input that reaches the shape
again, and shrink it while the property holds. The counts below are measured
against the grammar at the time of writing and are re-measured, not adjusted,
when the grammar moves.

## eval

- `cons-second-operand-gc` -- `ResumeBinary` cleared `frame->callee`, the
  second operand's only collector root, *before* `PCons`'s `FeCons` call.
  When that allocation collected, the operand had already been swept and
  `(cons a b)` produced a pair with a freed cdr, which the writer then hit
  as `FeTFree` and aborted on. Latent while every completed sub-expression's
  result also sat on the GC stack; live once those per-level pushes were
  removed. Needs the 64 KiB harness arena -- a roomier one collects too
  rarely to land on that exact allocation.
- `funcall-apply-redispatch` -- not a crash reproduction but the durable
  half of sub-plan 04C's fuzz gate: allocation-heavy forms interleaved with
  the four funcall/apply redispatch shapes (direct closure, `cons` symbol
  designator through the function cell, and the same two under `apply`'s
  spread). The corpus is gitignored and regenerated, so without a tracked
  seed a fresh checkout's `make fuzz-eval-smoke` would not necessarily fill
  the 64 KiB arena while a redispatch is mid-flight; this file forces that
  exact boundary on every smoke run. The redispatch roots its evaluated
  operand buffer in the EvalList frame's `accumulator` and the relay frame's
  fields -- the 04C instance of the class 03F's `cons-second-operand-gc`
  found, which is why the plan gates this slice on the fuzz lane.
  Re-derived in the Phase 9 fix cycle: the 3000-byte original built 357 forms
  under the current grammar and reached neither `funcall` nor `apply`. The
  359-byte replacement builds 9 forms and reaches all four shapes -- 4
  `funcall`s (2 closure, 2 designator) and 6 `apply`s (4 closure, 2
  designator).
- `strict-arity-optional`, `strict-arity-rest`, `strict-arity-malformed`,
  `strict-arity-primitive`, `strict-arity-native`,
  `strict-arity-native-too-few` -- Phase 7's coverage seeds, one per shape
  the strict-arity work added, in the `funcall-apply-redispatch` tradition:
  not crash reproductions, but inputs that force a gitignored, freshly
  regenerated corpus to reach these constructs on every smoke run.
  Each was found by instrumenting the builders with temporary counters and
  searching for an input that reaches its shape. The single `strict-arity`
  file they replace reached **none** of them: its 13 bytes spell the ASCII
  text "strict-arity", which steers the grammar somewhere else entirely, and
  every counter stayed at zero.

  Five of the six were re-derived in the Phase 9 fix cycle, having stopped
  reaching their shape when the grammar moved. Measured on the current
  grammar, replayed without mutation:

  | seed | bytes | reaches |
  |---|---|---|
  | `strict-arity-optional` | 512 | `(x &optional y)` x1 (was recorded as 3) |
  | `strict-arity-rest` | 97 | `(x &rest y)` x3, `(x &optional y)` x2 |
  | `strict-arity-malformed` | 59 | `(&rest y x)` x2 |
  | `strict-arity-primitive` | 83 | `(car)`, `(not t ...)` x3 operands, `(if ...)` x1 operand |
  | `strict-arity-native` | 53 | `(native-arity nil nil nil)`, i.e. `FeRequireNoArguments`' "too many arguments" |
  | `strict-arity-native-too-few` | 2 | `(native-arity)`, i.e. `FeGetNextArgument`'s "too few arguments" |

  `strict-arity-optional` is the original file, kept: it still reaches its
  shape, only fewer times than recorded. The other five are new bytes.

- `exhaustion-under-condition-case` -- Phase 9 sub-plan 09B's seed, in the
  same tradition: 25 bytes that walk `BuildExhaustionForm`'s four handler
  specs in order. Replaying it without mutation builds the exhaustion form
  five times and catches four -- once by `t`, once by `error`, twice by
  `arena-exhaustion` (with and without a bound handler variable) -- and the
  fifth form's `arith-error` handler deliberately does not match, so the
  escape path runs too. Re-verified against the current grammar. Before the builder existed the shape was
  unreachable: zero of 1500 random inputs raised an exhaustion at all
  (see `doc/FUZZING.md`), which is why this is a reachability seed rather
  than a crash reproduction.

- `deep-car-collection`, `cyclic-collection` -- Phase 9 sub-plan 09C's pair,
  12 bytes each: four rounds of `BuildDeepGraph`, acyclic and cyclic
  respectively. Replayed without mutation each one builds 4 graphs, so a
  fresh checkout's `make fuzz-eval-smoke` walks a deep `car` spine and a
  `setcdr` cycle through the rewritten mark phase on every run. Like the seeds above these are reachability seeds, not
  crash reproductions: before the arm existed the grammar could not build
  either shape at all.

  Not every bucket has a dedicated seed: the empty and single-required
  parameter lists are what the pre-Phase-7 grammar generated unconditionally
  and are reached by any mutation, so they get no file of their own.
