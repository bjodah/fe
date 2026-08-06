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
  half of sub-plan 04C's fuzz gate: 40 phases of allocation-heavy forms
  interleaved with the four funcall/apply redispatch shapes (direct closure,
  `cons` symbol designator through the function cell, and `apply`'s
  spread). The corpus is gitignored and regenerated, so without a tracked
  seed a fresh checkout's `make fuzz-eval-smoke` would not necessarily fill
  the 64 KiB arena while a redispatch is mid-flight; this file forces that
  exact boundary on every smoke run. The redispatch roots its evaluated
  operand buffer in the EvalList frame's `accumulator` and the relay frame's
  fields -- the 04C instance of the class 03F's `cons-second-operand-gc`
  found, which is why the plan gates this slice on the fuzz lane.
- `strict-arity-optional`, `strict-arity-rest`, `strict-arity-malformed`,
  `strict-arity-primitive`, `strict-arity-native`,
  `strict-arity-native-too-few` -- Phase 7's coverage seeds, one per shape
  the strict-arity work added, in the `funcall-apply-redispatch` tradition:
  not crash reproductions, but inputs that force a gitignored, freshly
  regenerated corpus to reach these constructs on every smoke run.
  Each was found by instrumenting the builders with temporary counters and
  searching for an input that reaches its shape; replaying them without
  mutation reaches, respectively, `&optional` parameter lists (3 calls),
  `&rest` (3), the malformed `(&rest y x)` list (2), the primitive
  over/under-arity form for `cdr`/`not`/`native-arity` with 1, 2 and 3
  operands, the host native's "too many arguments" raise, and its "too few
  arguments" raise. The single `strict-arity` file they replace reached
  **none** of them: its 13 bytes spell the ASCII text "strict-arity", which
  steers the grammar somewhere else entirely, and every counter stayed at
  zero.

- `exhaustion-under-condition-case` -- Phase 9 sub-plan 09B's seed, in the
  same tradition: 25 bytes that walk `BuildExhaustionForm`'s four handler
  specs in order. Replaying it without mutation raises arena exhaustion five
  times and catches it four -- once by `t`, once by `error`, twice by
  `arena-exhaustion` (with and without a bound handler variable) -- and the
  fifth form's `arith-error` handler deliberately does not match, so the
  escape path runs too. Before the builder existed the shape was
  unreachable: zero of 1500 random inputs raised an exhaustion at all
  (see `doc/FUZZING.md`), which is why this is a reachability seed rather
  than a crash reproduction.

  Not every bucket has a dedicated seed: the empty and single-required
  parameter lists are what the pre-Phase-7 grammar generated unconditionally
  and are reached by any mutation, so they get no file of their own.
