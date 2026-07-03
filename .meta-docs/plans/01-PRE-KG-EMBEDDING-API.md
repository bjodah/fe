# Plan: prepare Fe's embedding API for kg

## Objective

Stabilize Fe as a host-safe, extensible C23 library before kg pins its `fe`
git submodule to a released commit. The work should reduce
integration-specific glue without turning the small interpreter into a
framework.

The initial kg port needs native functions, multi-form evaluation, cancellation,
safe errors, source-labelled diagnostics, and persistent Lisp commands. It does
not need Fe's existing I/O, process, regex, math, or time extensions.

## Consumption model

These integration decisions are settled on the kg side and define what this
plan must deliver:

- kg consumes Fe through its `fe/` git submodule pinned to an exact commit;
  there is no separately copied `vendor/` snapshot. The repository layout must
  stay stable, and `fe.c` plus `fe.h` must build as a standalone translation
  unit without `fex*`, `auto.*`, or `main.c`.
- kg compiles `fe.c` from its own Makefile, so it must be warning-clean under
  both GCC and Clang with strict warnings-as-errors, not only under the
  default Clang `-Weverything` build.
- Lisp support in kg is compile-time optional (default on). Fe must never
  print, exit, or otherwise touch process state on failure, so that the
  feature can fail to initialize without affecting the editor.
- kg loads user configuration (`init.fe`) and explicit extension packages
  through a host-provided `(kg-load ...)` native that re-enters the evaluator.
  Nested evaluation, budget inheritance, and per-source error labels are
  therefore hard requirements, not conveniences.

## Priorities

### Blocking before the kg snapshot

1. Nonfatal context creation and documented arena requirements
2. Context userdata and explicit handler setters
3. Length-aware string/file evaluation helpers
4. Evaluation budgets and cancellation polling
5. Native binding, argument, nil, and string extraction helpers
6. A supported way to call and persist Lisp functions from C
7. Dedicated embedding API tests and documentation

### Recommended before the kg snapshot

8. Per-context custom type registration replacing global extension slots
9. Removal of public mutable globals from `fe.h`
10. A reproducible core-only build target guarding standalone `fe.c` use

If recommended work threatens the port schedule, kg can pin its submodule and
compile only the core after the blocking API is stable, continuing custom-type
work upstream. Do not copy the current global type-registration mechanism into
kg-specific code.

## Phase 1: define compatibility and ownership contracts

Keep Fe and kg on strict C23. Make `fe.h` self-contained under both GCC and
Clang and add a compile-only public-header test. Today the header uses `bool`
and `size_t` without declaring where they come from and includes the
obsolescent `<stdnoreturn.h>`; include exactly what it needs (`<stddef.h>`,
C23 keywords or `[[noreturn]]`) so host inclusion order cannot break it.
Verify `fe.c` compiles as a standalone translation unit, with no `fex*`
dependency, under both compilers' strict flags.

Document these contracts explicitly:

- required arena alignment and minimum usable size
- ownership and lifetime of the caller-provided arena
- which returned objects are protected by the GC stack and for how long
- whether a context may be re-entered from a native callback
- thread-safety (a context should remain single-threaded unless designed
  otherwise)
- callback and userdata lifetimes
- behavior after a recoverable Fe error

Treat API changes in this plan as one intentional pre-1.0 break rather than
accumulating compatibility shims for undocumented behavior.

Since this break re-shapes the public surface anyway, use C23 affordances
deliberately in the new API rather than freezing pre-C23 idioms:

- `[[nodiscard]]` on every constructor, evaluation, extraction, and root
  helper whose ignored result is a bug (`FeOpenContext`, `FeEvaluateString`,
  `FeCreateRoot`, ...); `[[noreturn]]` on `FeHandleError` instead of
  `<stdnoreturn.h>`.
- `nullptr` in documentation and examples; document failure returns as
  `nullptr`, not `0` or `NULL`.
- `constexpr` object-like constants (or plain functions where the value
  depends on layout) for arena alignment and minimums, with `static_assert`
  tying them to the real object layout.
- `<stdckdint.h>` checked arithmetic for arena-size and boundary validation in
  `FeOpenContext` and for any capacity math added by the budget and root APIs.
- A fixed underlying type for `FeType` once Phase 8 lands, with a
  `static_assert` connecting tag capacity to the registration limit.

Do not adopt syntax for its own sake elsewhere in the interpreter; the target
is the new public surface and its validation paths.

## Phase 2: make context creation host-safe

`FeOpenContext` currently exits the process when the arena is too small. A
library constructor must not terminate its host.

Add APIs equivalent to:

```c
size_t FeMinimumArenaSize(void);
size_t FeArenaAlignment(void);
FeContext *FeOpenContext(void *arena, size_t size);
```

`FeOpenContext` should return `NULL` for null, misaligned, or insufficient
storage without printing or exiting. Ensure the documented minimum covers all
objects created while installing core primitives, not merely
`sizeof(FeContext)`.

Consider an options-based constructor if initialization needs callbacks before
the first object allocation:

```c
typedef struct {
  void *userdata;
  FeErrorFn *error;
  FeInterruptFn *interrupt;
} FeOptions;
```

Keep the simple constructor as a convenience only if its failure behavior is
equally safe.

Tests:

- null and deliberately misaligned arenas
- every size around the minimum boundary
- repeated open/close over the same arena
- constructor failure under ASan/UBSan and Valgrind

## Phase 3: context userdata and handler APIs

Add one host pointer per context:

```c
void FeSetUserData(FeContext *ctx, void *userdata);
void *FeGetUserData(const FeContext *ctx);
```

This lets native functions and error/interrupt callbacks reach host state
without process globals or a `FeContext*` lookup table.

Replace mutation through `FeGetHandlers(ctx)->field` with explicit setters (and
getters only where needed). Keep `FeHandlers` private after migrating `fex` and
the CLI. The error callback contract must state that returning is not recovery:
it must perform a nonlocal transfer, or Fe invokes its documented panic path.

Decide the default panic policy explicitly. Embedders must be able to install a
handler before evaluation, and normal library failures must not print to
stderr unexpectedly.

Tests should use two simultaneous contexts with different userdata and error
destinations to catch accidental global state.

## Phase 4: length-aware input and multi-form evaluation

kg should not implement another reader adapter. Add public helpers equivalent
to:

```c
FeObject *FeReadString(FeContext *ctx, const char *source, size_t length,
    size_t *offset);
FeObject *FeEvaluateString(FeContext *ctx, const char *label,
    const char *source, size_t length);
FeObject *FeEvaluateFile(FeContext *ctx, const char *label, FILE *file);
```

Requirements:

- evaluate all top-level forms and return the final value (nil for no forms)
- never read beyond the supplied length
- define embedded-NUL behavior instead of silently treating it as arbitrary EOF
- restore temporary GC protection between forms without invalidating the final
  result
- prefix reader and evaluator errors with the caller-supplied label and, where
  available, an offset; kg reports init-file and `(kg-load ...)` failures
  through this label, so it is required, not opportunistic
- keep file ownership with the caller

If file evaluation adds no value beyond a generic reader plus string helper,
omit it and let hosts read files themselves. Do not require temporary files.

Retain low-level `FeRead` for streaming embedders and fuzzing.

## Phase 5: bounded and cancellable evaluation

An embedded `(while t)` must not hang kg. Add an evaluation-control API rather
than trying to detect loops in the host. One possible shape is:

```c
typedef bool FeInterruptFn(FeContext *ctx, void *userdata);

typedef struct {
  size_t step_limit;       /* zero means unlimited */
  size_t poll_interval;
  FeInterruptFn *interrupt;
  void *userdata;
} FeEvalOptions;

FeObject *FeEvaluateWithOptions(FeContext *ctx, FeObject *object,
    const FeEvalOptions *options);
```

Count steps at evaluator entry and loop back-edges, including `while`, macro
expansion, function calls, and list evaluation. Polling must be deterministic
enough for tests and cheap enough for normal use. Exhaustion and cancellation
should raise distinct Fe errors through the normal recovery path.

Define nested evaluation semantics with inheritance as the default: kg's
`(kg-load ...)` native re-enters the evaluator, and a loaded package must not
be able to reset the limits its caller established. Per-call stacked controls
may exist in addition, but reject undocumented resets that let native
callbacks bypass limits.

Add regression and fuzz cases for infinite loops, deep calls, macro expansion,
and cancellation callbacks.

## Phase 6: extension ergonomics and value safety

Move commonly repeated `fex` glue into the supported core API:

```c
FeObject *FeNil(FeContext *ctx);
void FeDefineNative(FeContext *ctx, const char *name, FeNativeFn *fn);
void FeRequireNoArguments(FeContext *ctx, FeObject *args);
```

Add exact-arity helpers or a consistent consume-and-require-empty pattern so C
extensions reject extra arguments rather than ignoring them.

Separate raw string extraction from Lisp serialization. `FeToString` currently
serves both roles and fixed buffers can truncate host commands silently. Add an
API that validates `FeTString`, reports the required byte length, and either
copies with an explicit truncation result or streams through a callback. kg
needs to allocate exactly and pass the full text to editor operations. The
extraction API must cover symbol names as well as strings; kg needs symbol
text for command dispatch and error messages.

Const-correct read-only APIs such as `FeGetType`. Remove the need for embedders
to reference `extern FeObject nil` or mutate `type_names` directly.

## Phase 7: calling and retaining Lisp values

Lisp-defined kg commands need to survive after the init form's temporary GC
frame is restored. Add an opaque persistent-root mechanism, for example:

```c
typedef struct FeRoot FeRoot;

FeRoot *FeCreateRoot(FeContext *ctx, FeObject *object);
FeObject *FeGetRoot(const FeRoot *root);
void FeReleaseRoot(FeContext *ctx, FeRoot *root);
```

The implementation should use arena-managed storage or another bounded design;
do not introduce hidden unbounded heap allocation into the core. Stale/double
release behavior must be testable.

Add a supported call helper:

```c
FeObject *FeCall(FeContext *ctx, FeObject *callable,
    FeObject *const *arguments, size_t count);
```

It must share evaluator budget/error semantics and protect the callable and
arguments across allocations. Hosts should not have to construct an internal
pair layout merely to invoke a registered command.

Context userdata is sufficient for kg's initial native wrappers. Investigate
per-native userdata only if it can be added without bloating every Fe object;
do not distort the compact representation solely to avoid a few wrapper
functions.

## Phase 8: replace global custom-type slots

The current `FeTFex0..2`, mutable global `type_names`, and single context-wide
mark/GC callbacks are fragile: modules compete for three compile-time slots,
type names leak across contexts, and one dispatcher must know every extension.

Design per-context registration around a descriptor such as:

```c
typedef struct {
  const char *name;
  void (*mark)(FeContext *ctx, void *pointer, void *userdata);
  void (*finalize)(FeContext *ctx, void *pointer, void *userdata);
  void *userdata;
} FePointerType;

FeType FeRegisterPointerType(FeContext *ctx, const FePointerType *descriptor);
```

Requirements:

- bounded number of registered types derived from the actual tag capacity
- registration failure reported without aborting
- descriptors scoped to one context
- clear descriptor/name lifetime
- mark/finalize dispatch by registered type
- finalizers run exactly once during collection or context close
- no global mutable type-name array in the public API

Migrate regex and file extensions as proof. kg does not need custom pointer
types for its initial bridge, so this phase may remain nonblocking if the core
API can be consumed without exposing the old mechanism.

## Phase 9: tests, packaging, and documentation

Add native C tests rather than relying only on `.fe` golden scripts. Cover:

- two independent contexts and userdata values
- constructor failure and arena boundaries
- error recovery followed by successful reuse
- string evaluation with multiple forms and exact lengths
- budget exhaustion and cancellation
- native arity/type errors
- exact large-string extraction
- root create/get/release and `FeCall`
- custom type finalization if Phase 8 lands

Provide a Makefile target that builds only the embeddable core (`fe.c` as a
lone object or `libfe.a`) and a tiny host example under GCC and Clang. kg
compiles `fe.c` directly from its own Makefile, so this target is the guard
that keeps standalone compilation working. Keep the public header
warning-clean under the same strict C23 flags kg uses. Continue running reader/evaluator fuzz smoke and add
new APIs to the bounded evaluator grammar where useful.

Update `doc/c-api.md` and `doc/implementation.md` with complete ownership,
error, cancellation, rooting, and type-registration contracts. Add an API
version macro or function so kg can assert the pinned interface it expects.

## Exit criteria for starting the kg port

- Blocking phases are implemented and covered by native tests.
- `.ci/run-ci-steps.sh` and fuzz smoke pass under GCC and Clang toolchains.
- No public API used by kg requires `extern nil`, mutable `type_names`, direct
  `FeHandlers` mutation, or a host-global jump buffer.
- A sample host can initialize, bind a native function, evaluate multiple
  forms with a finite budget, recover from an error, call a retained Lisp
  function, and shut down without leaks.
- The Fe commit intended for kg's submodule pin is tagged, the branch kg
  tracks is documented (rename or retire `analyzers-etc` if a stabler branch
  is warranted), and the license, attribution, and update procedure for
  downstream embedding are documented.
