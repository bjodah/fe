# Fe contributor guide

Fe is a small, embeddable Lisp-like language implemented in C23. Read
`README.md` first for the project goals, then consult the relevant document in
`doc/` before changing language behavior, the public C API, or object layout.

## Repository map

- `fe.c` and `fe.h`: core interpreter, object model, garbage collector,
  reader/writer, and public embedding API.
- `fe_eval.c`: the frame-driven evaluator, the cleanup registry and the raise
  machinery, split out of `fe.c`.
- `fe_run.c`: the run driver -- the two places a run's error barrier is
  installed -- and the public `FeEvaluate*`/`FeCall*` entry points, split out
  of `fe_eval.c`.
- `fe_internal.h`: the private surface those three translation units share.
- `fex.c`, `fex.h`, and `fex_*.c`: optional standard extensions for I/O, math,
  processes, regular expressions, and time.
- `main.c`: command-line interpreter and recoverable REPL error handling.
- `auto.[ch]`: cleanup helpers based on the compiler `cleanup` attribute.
- `scripts/*.fe`: executable examples and regression-test inputs.
- `tests/*.out` and `tests/*.err`: exact golden output for the scripts.
- `doc/`: language, implementation, C API and fuzzing documentation, plus
  `unwind-design.md`, which is a design for cleanup/unwinding that is
  deliberately not implemented yet.
- `.ci/`: the numbered CI stages and their shared environment.
- `utils/`: complexity-budget checks used by CI.
- `fuzz/`: raw-reader and grammar-steered evaluator fuzz harnesses.

The sources intentionally live in the repository root; do not introduce a
`src/` or `test/` layout as an incidental part of another change.

## Build and test

- Build: `make`
- Run the interpreter: `./fe`, `./fe script.fe`, or `./fe -e '(print 42)'`
- Run the regression suite: `make check`
- Remove build products: `make clean`
- Apply formatting: `make format`
- Check formatting without changing files: `make format-check`
- Generate coverage: `make coverage` (HTML is written to `coverage/html/`)
- Run the complete local CI pipeline: `.ci/run-ci-steps.sh`
- Build fuzzers: `make fuzz`; smoke-test them with `make fuzz-smoke`

`CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, and `LDLIBS` are overridable. The default
build uses Clang with strict warnings as errors. Use `JOBS=8` (or another
reasonable value) to limit CI parallelism.

The full pipeline runs these independent gates:

1. `scc` and `pmccabe` complexity budgets.
2. GCC coverage with an 80% minimum line-coverage threshold and branch data.
3. GCC `-fanalyzer` plus the script suite under Valgrind.
4. Clang AddressSanitizer and UndefinedBehaviorSanitizer.
5. Clang MemorySanitizer.
6. libFuzzer smoke runs for the raw reader and steered evaluator targets.
7. Include-What-You-Use, Clang analyzer, exhaustive cppcheck, and clang-tidy.
8. clang-format verification.

The full local run expects GCC, Clang/LLVM, ccache, Valgrind, `scc`, `pmccabe`,
lcov, Bear, Include-What-You-Use, GNU parallel, cppcheck, and Python 3. The
Woodpecker image is the canonical toolchain when local versions differ.

The number is the execution order, and it is a total order: the runner
globs `.ci/ci-[0-9][0-9]-*.sh`, so two stages sharing a number are
sequenced by whatever their names sort as, which is not something anyone
chose. The same rule holds in the parent repository and in
`tiny-regex-c`. Give a new stage the next free number, or renumber the
ones after it.

Run a numbered `.ci/ci-NN-*.sh` directly while iterating on one class of
failure. Before handing work over, run the complete pipeline. Valgrind and MSan
skip the computationally expensive Mandelbrot example; it remains covered by
the normal, coverage, and ASan/UBSan stages.

See `doc/FUZZING.md` before changing a fuzz harness or triaging an artifact.
The evaluator grammar intentionally excludes known sources of nontermination
and external side effects; do not broaden it without preserving those bounds.

## Regression tests

`test.sh` runs every `scripts/*.fe` file with `scripts/assert.fe` loaded first,
then compares stdout and stderr byte-for-byte with the matching files in
`tests/`. It also tests command-line evaluation with `-e`.

For a behavior change:

1. Add or update a focused script in `scripts/`.
2. Add or update its exact `tests/<name>.out` and `tests/<name>.err` files.
3. Run `make check` and inspect unexpected output rather than blindly replacing
   golden files.

`FE_RUNNER` and `FE_SKIP_SCRIPTS` are test-harness controls used by CI. Do not
set them for the normal regression suite, and do not use exclusions to hide a
correctness failure. The ordinary and RELEASE passes are both unconditional
strict-arity runs; `scripts/arity.fe` records accepted optional/rest forms and
the deliberate errors.

## Engineering expectations

- Keep changes small and preserve the implementation's deliberately compact
  design. Avoid new dependencies unless the task requires one.
- Match the existing C23 and clang-format style. Keep warnings and analyzer
  findings fatal; prefer fixing ownership, portability, and type issues over
  adding suppressions.
- The complexity limits in `Makefile` are ratchets, not aspirational numbers.
  Refactor code that exceeds them; change a limit only when reviewed structural
  growth makes that unavoidable.
- `.ci/pmccabe-baseline.json` is a per-symbol ratchet on top of those funded
  totals, and `make pmccabe-baseline` records *improvements*. It refuses to
  rewrite the file when that would raise an individual symbol, even inside the
  funded envelopes; banking an increase needs
  `make pmccabe-baseline PMCCABE_BASELINE_ARGS=--allow-regressions`, and the
  reason belongs in the commit message. State the per-symbol deltas there
  either way.
- Maintain direct includes. The IWYU stage is authoritative, and POSIX feature
  macros belong in build flags rather than individual source files.
- Preserve public API compatibility unless a breaking change is explicitly
  intended. Update `doc/c-api.md` for API changes and `doc/language.md` for
  language changes.
- Update `doc/implementation.md` when changing representation, evaluation,
  garbage collection, or error-handling invariants.
- Treat `TODO.md` as context, not as permission to expand the scope of a task.

## Memory and error invariants

The core interpreter stores its context and objects in the fixed-size arena
provided to `FeOpenContext`; core object allocation must not move to the heap.
Extensions may own external resources, but every allocation needs an explicit
failure path and a clear release point. Pointer-backed Fe objects must also be
handled by the extension GC callback.

Any object that must survive a possible allocation needs to remain reachable or
be protected through the GC stack. Balance `FeSaveGC` with `FeRestoreGC`, and
remember that object creation can trigger a collection.

`FeHandleError` does not return. In the interactive interpreter its handler
uses `longjmp`; otherwise the process exits. Do not assume ordinary stack
unwinding or compiler cleanup attributes release resources across a `longjmp`.
Free temporary external allocations before raising an error where practical,
and do not allocate Fe objects inside an error handler.

## Generated files

`fe`, object files, coverage data, `compile_commands.json`, and sanitizer
coverage files are generated and ignored. Static analysis should not leave
`.plist` files or other reports in the source tree. Do not include generated
files in a review or commit. The locally available `unrelated-pkg-kg/`
directory, when present, is reference material outside this repository and
must remain untracked and untouched.
