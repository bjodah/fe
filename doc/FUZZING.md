# Fuzzing

Fe has two complementary libFuzzer targets. Keeping them separate prevents
syntax rejection and intentional nontermination from dominating evaluator
coverage.

## Raw reader target

`make fuzz-reader` builds `fuzz/fuzz_reader`. It passes input bytes directly to
the public `FeRead` callback API, renders every successfully parsed object, and
closes the context to exercise collection. Existing example scripts and
`fuzz/fe.dict` seed valid Lisp tokens, but mutations remain arbitrary bytes.

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
bindings, functions, macros, and non-cyclic pair mutation.

The grammar deliberately excludes:

- `while` and recursive or self-referential definitions
- output primitives
- I/O, process, regex, time, and other extensions
- cyclic pair construction
- unbounded AST depth

Those exclusions are steering constraints, not claims that the excluded
features are safe. They ensure every generated program should terminate
quickly and perform no external side effects. This target exercises the core
evaluator deeply; it does not replace raw reader fuzzing or end-to-end script
tests.

## Smoke tests

Build and run both targets for 1,000 inputs each:

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
