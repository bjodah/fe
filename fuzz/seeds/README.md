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
  `cons` symbol designator through the value-cell fallback, and `apply`'s
  spread). The corpus is gitignored and regenerated, so without a tracked
  seed a fresh checkout's `make fuzz-eval-smoke` would not necessarily fill
  the 64 KiB arena while a redispatch is mid-flight; this file forces that
  exact boundary on every smoke run. The redispatch roots its evaluated
  operand buffer in the EvalList frame's `accumulator` and the relay frame's
  fields -- the 04C instance of the class 03F's `cons-second-operand-gc`
  found, which is why the plan gates this slice on the fuzz lane.
