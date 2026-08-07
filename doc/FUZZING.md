# Fuzzing

Fe has two complementary libFuzzer targets. Keeping them separate prevents
syntax rejection and intentional nontermination from dominating evaluator
coverage.

## Raw reader target

`make fuzz-reader` builds `fuzz/fuzz_reader`. It passes input bytes directly to
the public `FeRead` callback API, renders every successfully parsed object, and
closes the context to exercise collection. Existing example scripts and
`fuzz/fe.dict` seed valid Lisp tokens, but mutations remain arbitrary bytes.
Since 05D the dict also carries the numeric token families the new
classify-then-convert lexer must get right: integers, floats, trailing/leading
dots, exponents, the nonfinite spellings (`1.0e+INF`, `0.0e+NaN`), and the
tokens that must stay symbols (`0x10`, `inf`, `nan`, `1e`).

Phase 8's reader adds 21 more entries, for the two things it has to get right:
the accepted literals (`?`, `?\C-a`, `?\M-a`, `?\C-?`, `#x`, `#o`, `#b`,
`\x41`, `\x0041`, `\101`, `\e`, `\d`, `\s`) and the prefixes of the
spellings it must reject rather than misread (`?\s-`, `?\x`, `\400`, `\0`,
`a\ b`, `##`, `[1 2]`, `#:s`). The point of the second group is that a reject
arm which is never reached is not evidence of anything: an unreachable arm and
a wrong arm look identical in a coverage report of zero.

Measured, one 45-second run of `fuzz_reader` on this box with this dict and
`scripts/` as extra seeds -- 295050 executions, 6414 exec/s, 2694 new units,
peak RSS 186 MB -- and every corpus file then replayed through `./fe`, counting
its first diagnostic:

```text
21  ? literal without delimiter        3  \x
15  malformed radix integer            3  symbol escape
 5  unknown escape                     2  \x character out of range
 5  invalid UTF-8 character            2  vector brackets
 5  character above 255 in string      2  ? at end of input
 1  \s character modifier              1  NUL character in string
 1  malformed character modifier       1  duplicate character modifier
 1  #                                  1  symbol too long (63-byte limit)
```

That is every named reject arm the reader has except the `\S-`, `\A-` and
`\H-` modifier spellings, which share `\s-`'s code path and differ only in
the letter the message names. The count is "corpus files whose *first*
diagnostic was this arm", so it understates: a file that fails earlier for
another reason hides whatever came after it.

The harness uses a fresh 64 KiB arena for each input. Invalid syntax, excessive
nesting, long symbols, and arena exhaustion are expected Fe errors and recover
through the normal error-handler path. A sanitizer failure, abort outside that
path, timeout, or libFuzzer resource-limit failure is a finding.

The NUL byte is Fe's reader EOF marker, so bytes after the first NUL are
intentionally not consumed.

## Steered evaluator target

`make fuzz-eval` builds `fuzz/fuzz_eval`. Arbitrary bytes select a bounded AST
grammar, built through the public C API. Generated expressions cover atoms,
quoted data, lists, arithmetic, comparisons, conditionals, short-circuiting,
bindings, functions, macros, and non-cyclic pair mutation, plus (05C) the
variadic arithmetic and chained comparators over the grammar's host-made
integer/double mix and (05D) the `eq`/`eql` identity pair. Macro bodies expand
to a list, to `nil`, to `t`, to a symbol, or to a number, so the target reaches
the atom expansions as well as the structural one.

Phase 6's non-local exits are steered the same way, and each arm exists
because the shape before it was unreachable by construction:

- `error` had no coverage at all, which made its directive parser and its
  fixed 1024-byte message buffer the most attack-shaped code in the phase
  with the least evidence behind it. The format strings are a fixed table
  rather than fuzzer bytes, on purpose: the interesting states are the
  parser's, not the alphabet's -- every supported directive, the escape, a
  `%` at the very end with no letter after it, an unsupported letter, and a
  run of directives long enough to press the buffer. Arguments are ordinary
  expressions, so a directive can meet any value the grammar can build.
- `condition-case` used to emit one always-matching shape: `nil` variable,
  a bare `arith-error` spec, and a body that signalled exactly
  `arith-error`. The matcher was therefore fuzzed on its true branch alone.
  The variable is now `nil` or a bound `e`, the spec is a symbol, a
  two-symbol list, or `t`, there may be a second clause, and the body
  signals any registered condition (including `quit`, which an `error`
  handler must *not* catch) or is a full expression -- so the hierarchy
  walk, the textual-order selection and the unmatched re-signal are all
  reachable.
- `catch`/`throw` used to put the throw directly in the catch's body, so
  the mid-stack unwind always had zero frames and zero cleanup entries to
  walk. Five arms now fill that gap: nothing, argument frames, a branch
  frame, an `unwind-protect` whose cleanup must run on the way past, and a
  cleanup that itself throws -- the 06D policy where the second throw
  replaces the first and has to be re-issued in the enclosing context.

Phase 9's exhaustion arm (09B) is the same story once more. Every other form
this grammar builds is bounded by `MaxDepth`, and the harness restores the GC
stack between forms, so all of it is collectable again by the next one:
measured before the arm existed, **zero** of 1500 random inputs of 16..128
bytes reached an arena exhaustion at all, and neither did a 4096-byte one, so
the catchable-exhaustion path 09B built was unreachable from this lane.
`BuildExhaustionForm` emits
`(condition-case VAR (let ((l nil)) (while t (setq l (cons 1 l)))) (SPEC ...))`
with SPEC drawn from `t`, `error`, `arena-exhaustion` and `arith-error` -- the
three handlers that must match and one that must not, so the escape path is
generated too. With the arm, 178 of the same 1500 inputs reach it.

That loop is the one place this grammar admits a `while` (see the exclusion
list below), and the exclusion is not weakened by it: the loop terminates only
by exhausting the arena, and the arena is a fixed `FuzzArenaSize`, so the
iteration count is bounded by the object slots it holds -- a few hundred, not
by the input -- with the harness's own `MaxEvaluationSteps` behind that.

Phase 9's second arm (09C) is the collector's. `MaxDepth` bounds the grammar's
own recursion and `BuildMutation` keeps `setcar`/`setcdr` acyclic, so from this
lane the mark phase was only ever asked to walk shallow, acyclic graphs -- the
two shapes 09C's rewrite exists for, a deep `car` spine and a cycle, were
unreachable by construction. `BuildDeepGraph` emits

```clojure
(let ((d nil) (i 0))
  (while (< i LEVELS) (do (setq i (+ i 1)) (setq d (cons d nil))))
  [(setcdr d d)]                       ; half the arms
  (while (< i CHURN) (do (setq i (+ i 1)) (cons i i)))
  i)
```

-- a `car` chain 8..207 levels deep, closed into a cycle half the time, with
enough churn over the top of it to force several collections while it is live.
Both loops are bounded by constants compiled into the form rather than by the
input, and the form returns a number, so nothing deep or cyclic reaches the
harness's own `FeToString`. Measured over the same 800 random inputs: 184 reach
the arm, building 226 graphs of which 118 are cyclic, up to 206 levels deep,
across 838 collections. `MaxDepth` also went 4 -> 6 in the same slice, so the
rest of the grammar builds structures the collector has to walk further into.

The harness's arena stays 64 KiB deliberately, and is *not* enlarged for this:
the tracked `cons-second-operand-gc` seed reproduces only at that arena's
collection rate ("a roomier one collects too rarely to land on that exact
allocation"), and the depth this arm needs comes from its own loop rather than
from arena size.

The atom pool includes `t`, `:fuzz-keyword`, and the ordinary `:` symbol;
binding targets independently choose `t`, `nil`, the keyword, or `x`. This
makes protected `setq`/`let` writes and keyword self-evaluation reachable
rather than relying on incidental symbol generation.

`fuzz/seeds/eval` carries one hand-built seed per group
(`error-format-directives`, `condition-case-handlers`,
`catch-throw-cleanup-gap`, the six `strict-arity-*` shapes,
`funcall-apply-redispatch`, `exhaustion-under-condition-case`,
`deep-car-collection`, `cyclic-collection`);
`FE_FUZZ_DUMP=1 ./fuzz/fuzz_eval SEED` prints the forms each one builds.

**A grammar change invalidates seeds.** A seed is opaque bytes that steer this
grammar, so a new `switch` arm, a wider modulus or a different `MaxDepth`
re-steers all of them at once -- and nothing about that is visible in a diff.
Phase 9 did exactly this: `MaxDepth` 4 -> 6 plus the two arms that took
`BuildExpression`'s modulus from 34 to 36 left six of the fourteen tracked
eval seeds reaching none of the constructs they exist to force, for a whole
phase, while `fuzz/seeds/README.md` went on recording the old counts.
`fuzz/seeds/reachability.json` now states what each seed must reach and
`make fuzz-eval-seed-verify` -- which `make fuzz-smoke`, and so
`.ci/ci-06-fuzz-smoke.sh`, runs first -- replays each seed with
`FE_FUZZ_DUMP=1` and checks. Touching the grammar means running that target
and re-deriving whatever it reports, in the same commit.

The evaluator grammar's lambda builder covers zero, one, and two required
parameters, `&optional`, `&rest`, malformed declarations, and deliberate
under/over-arity calls. Macro calls use the same builder and receive raw forms.

The grammar deliberately excludes:

- `while` and recursive or self-referential definitions, except the one
  arena-bounded loop the exhaustion arm above needs
- output primitives
- I/O, process, regex, time, and other extensions
- cyclic pair construction
- unbounded AST depth

Those exclusions are steering constraints, not claims that the excluded
features are safe. They ensure every generated program should terminate
quickly and perform no external side effects. This target exercises the core
evaluator deeply through `FeEvaluateWithOptions()` with a finite step budget as
a final termination backstop; it does not replace raw reader fuzzing or
end-to-end script tests.

## Writer target

`make fuzz-write` builds `fuzz/fuzz_write`. It generates a program that conses
up to 32 pairs and then `setcar`/`setcdr`s each half to `nil`, an atom, or
another node, so the resulting graph can contain cdr-spine cycles, car cycles,
shared subgraphs and improper tails. The graph is built by evaluating generated
source rather than through the C API, because pair mutation has no public C
spelling.

It then renders the graph twice: once through `FeWriteWithOptions()` with
fuzzer-chosen `max_bytes`, `max_nodes` and `max_depth` (zero, meaning
"default", included), and once through `FeToString()` into a fixed buffer. The
properties checked are that rendering terminates at all, that it never emits
more than `max_bytes`, that a rendering reported complete emitted something,
and that `FeToString()` leaves exactly one NUL inside its destination at the
offset it returned.

This is the target that owns the writer's cycle and bound behaviour; the
steered evaluator target below still excludes cycles, so it does not cover it.

## Smoke tests

Build and run all three targets for 1,000 inputs each:

```sh
make fuzz-smoke
```

The smoke targets use the existing scripts as seed inputs and write evolving
corpora and findings beneath `fuzz/corpus/` and `fuzz/artifacts/`. They enforce
a 4 KiB input limit, two-second per-input timeout, 512 MiB RSS limit, and fatal
ASan/UBSan findings. Override `FUZZ_RUNS`, `FUZZ_MAX_LEN`, `FUZZ_TIMEOUT`,
`FUZZ_RSS_LIMIT_MB`, or `FUZZ_VERBOSITY` when needed. `make clean` preserves
campaign state; `make fuzz-clean` removes it explicitly.

## Longer campaigns

After `make fuzz`, a parallel 15-minute campaign can be run with:

```sh
mkdir -p fuzz/corpus/reader fuzz/corpus/eval \
  fuzz/artifacts/reader fuzz/artifacts/eval

./fuzz/fuzz_reader -max_total_time=900 -fork=8 -max_len=4096 \
  -timeout=2 -rss_limit_mb=512 -dict=fuzz/fe.dict \
  -artifact_prefix=fuzz/artifacts/reader/ fuzz/corpus/reader scripts

./fuzz/fuzz_eval -max_total_time=900 -fork=8 -max_len=4096 \
  -timeout=2 -rss_limit_mb=512 \
  -artifact_prefix=fuzz/artifacts/eval/ fuzz/corpus/eval scripts
```

Choose the worker count for the machine. Run one target at a time when memory
is constrained.

## Triage

Replay a reader artifact directly:

```sh
./fuzz/fuzz_reader -runs=1 fuzz/artifacts/reader/crash-*
```

Evaluator artifacts encode grammar choices rather than source. Print the
generated expressions while replaying one:

```sh
FE_FUZZ_DUMP=1 ./fuzz/fuzz_eval -runs=1 fuzz/artifacts/eval/crash-*
```

Reduce the artifact with libFuzzer, identify the generated expression, and add
the smallest equivalent `.fe` case to `scripts/` with golden files in `tests/`
before fixing a user-reachable defect. Keep an artifact as a harness regression
when no equivalent source program exists.
