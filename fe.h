// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FE_H
#define FE_H

#include <stddef.h>  // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>

// The embedding contract: the C functions, types, and callback signatures
// below. A Lisp-only change such as FE_LANGUAGE_VERSION 3's
// call-position/reader cut does not move this. Version 2 (sub-plan
// 03F of kg's Emacs-subset program) was the frame machine's bound rename:
// `FeEvalOptions.max_depth` split into `max_frames` and
// `max_native_reentry`, and `FeArenaStats.peak_evaluation_depth` split into
// `frame_capacity`, `peak_frame_depth` and `peak_native_reentry`. Every host
// that set the old fields gets a compile error, which was the point -- their
// *meaning* changed, not just their name, so silently keeping the old
// spelling would have been the wrong kind of compatibility.
//
// Version 3 (sub-plan 04D of kg's Emacs-subset program) is the Lisp-2
// namespace cut: `FeDefineNative` now registers into a symbol's function
// cell instead of its value cell, and `FeSetFunction`/`FeGetFunction`/
// `FeIsFBound` (added in 04C under version 2) are part of the same visible
// contract. A host that called `FeDefineNative` for names it then called
// from Lisp must recompile anyway -- the tripwire is kg's own
// `static_assert(FE_API_VERSION == 2)`, which this bump fires.
//
// Version 4 (sub-plan 05D of kg's Emacs-subset program) is the numeric cut:
// 05A's placement (a) Decision inserted `FeTInteger` into the public `FeType`
// enum immediately after `FeTDouble`, renumbering every later constant
// including `FeTPtr`; `FeMakeInteger`/`FeToInteger` (05B) joined the
// constructor/accessor pair; and the reader now produces integers from
// source text, so a host-made and a read number share one meaning. The bump
// was deliberately deferred to the cut so the whole numeric contract moves as
// one visible break -- a "compatible" break that silently reinterprets `42`
// is the worst kind (05D).
//
// Version 5 (sub-plan 06D of kg's Emacs-subset program) is the condition
// cut: `FeCompletion`, `FeGetCompletion`, `FeGetCondition`,
// `FeRaiseCompletion` and `FeResignal` join the surface, and an error a host
// used to see only as text now also carries an `(error-symbol . data)`
// object. Existing calls keep compiling; the bump is the two-axes rule 06D
// set, where the language version moves for programs and this one for hosts,
// and both moved.
//
// Version 6 (sub-plan 07B) removes `FeSetStrictArity` and
// `FeGetStrictArity` outright -- no deprecated no-op, no lax mode -- because
// arity is now unconditional. A host that called either gets a compile
// error, which is the whole notification: there is no runtime answer that
// would still be true.
//
// Version 7 (sub-plan 11C) adds one entry point,
// `FeTryEvaluateStringWithOptions`: the protected *string* evaluation, the
// sibling `FeTryCallWithOptions` (version 5) has needed since a host that
// loads a file from inside an evaluation turned out to have the same problem
// a host that calls a callback has. Nothing is removed and nothing changes
// meaning, so every existing call keeps compiling; the bump exists because a
// version that does not move cannot tell kg whether the fe it is linking
// against has the entry point at all -- the same reasoning version 3's
// language bump used, and the same `static_assert` tripwire.
#define FE_API_VERSION 7

// The Lisp language Fe evaluates. Version 1 was implicit -- Fe's historical,
// non-Emacs dialect, where `=` assigned and returned nil. Version 2 (sub-plan
// 02C of the Emacs-subset hard cut) was the first explicit contract: `setq`
// and `set` are assignment, and `=` is chained numeric equality, matching
// Emacs Lisp. Version 3 (sub-plan 04D) is the Lisp-2 namespace cut: a symbol
// in call position resolves through its function cell only -- the
// value-cell fallback is deleted -- `#'x` reads as `(function x)`, and
// `boundp`-of-a-callable changed meaning (the bootstrap callables now live
// in function cells, so `(boundp 'car)` is nil while `(fboundp 'car)` is t).
// Version 4 (sub-plan 05D) is the numeric cut: integer literals (`42`, `+5`,
// `1.`) and the Emacs float spellings (`.5`, `1e3`, the nonfinite
// `1.0e+INF`/`0.0e+NaN` family) read as their own types instead of every
// number reading as a double, floats print shortest-round-trip with an
// explicit `.0`, integer division truncates, `(/ 1 0)` is `arith-error`, and
// `eq`/`eql` are core primitives with Emacs' identity semantics.
//
// Version 5 (sub-plan 06D) is the condition cut: `condition-case`, `signal`
// and a condition hierarchy exist, and every raise the evaluator makes now
// carries a symbol and a data list. A program that only ever printed the
// message text still runs; one that branched on that text can now branch on
// the symbol instead.
//
// Version 6 (sub-plan 07B) is the strict-arity cut, and it changes what
// existing programs *mean*. Calls that used to answer are now errors:
// `((lambda (x) x))` bound `x` to nil and is `wrong-number-of-arguments`;
// `((lambda () 1) 2)` dropped the extra argument and is the same; `(car 1 2)`
// and `(quote 1 2)` ignored their surplus operands and now reject them
// before evaluating any of them. Two answers changed rather than
// disappearing: `(and)` was nil and is `t`, and `(signal 'error)` was an
// arity error and is now accepted with nil data. `print` gains a
// one-argument minimum, so the zero-argument blank line is gone. Malformed
// parameter lists raise `invalid-function` instead of prose errors. Fe's
// dotted-tail and bare-symbol rest spellings are unaffected.
// See doc/language.md and doc/c-api.md.
// Version 7 is the whole of Phase 8's language contract, both of its slices
// under one bump. It protects `t`, `nil` and keyword symbols (including `:`
// itself) from value or function assignment and makes keywords
// self-evaluating: programs that assigned `t` now signal `setting-constant`,
// and `:foo` no longer needs quoting. It also makes the reader strict, and
// that half breaks programs too -- a previously readable one may no longer
// read at all. A bare `#` and every `#`-initial symbol are gone (Fe's own
// scripts/concatenate.fe named a function `#`); `[...]`, `#:`, `#s(...)` and
// symbol escapes such as `a\ b` are named read errors rather than symbols;
// an unknown string escape errors where the backslash used to be dropped;
// and a string escape must land in one byte, so `"\0"` and `"\400"` error
// where they used to truncate the string silently. What it adds is the
// measured Emacs subset: UTF-8 character literals with the `\C-`/`\M-`
// modifiers, greedy `\x`, radix integers, and one-based source lines on
// evaluated-file and evaluated-string diagnostics. One language bump for the
// phase, covering the reader break as well as the constants: 8.0's release
// version moves, both compatibility macros stay where 08B put them.
//
// Version 8 (sub-plan 10B) is the reflective-expansion cut: `macroexpand-1`,
// `macroexpand` and `macroexpand-all` are names the language answers.  The
// first two expand a macro call without evaluating it -- one step, and
// Emacs' fixpoint -- and the third names itself as unimplemented instead of
// answering `void-function`.  Nothing an existing program could write breaks;
// what moves is what those three names *answer*, which used to be
// `void-function` for all of them, and `(fboundp 'macroexpand)`, which was
// nil.  The bump is deliberate under this file's own "compatible additions do
// not require a bump" rule, and the reasoning is recorded with the commit:
// under the program's §0.4 (no external constituency) the only consumer of
// this macro is kg's `static_assert`, and a version that does not move is a
// version that cannot tell kg whether the fe it is linking against has these
// names.  `FE_API_VERSION` does not move: no declaration in this header
// changed.
//
// Version 9 (sub-plan 11B) is the special-variable cut, and it is the first
// language bump in this series that changes what an *existing* program
// answers rather than only adding names.  A symbol marked by
// `internal--mark-special` binds dynamically: `let` and `let*` over it swap
// the global value cell instead of extending the lexical environment, a
// function that reads the name free sees the bound value, `setq` inside the
// binding writes the binding, and the previous value -- or the symbol's
// unboundness -- is restored on every completion kind.  Nothing that never
// calls `internal--mark-special` can tell the difference, and closure and
// defun *parameters* stay lexical unconditionally, which is Emacs' own
// answer under `lexical-binding: t`.  `special-variable-p` is the reader for
// the flag.  `FE_API_VERSION` does not move here either -- no declaration in
// this header changed -- though sub-plan 11C moves it for the protected
// string entry, and the two land under one `FeVersion` "10.0".
#define FE_LANGUAGE_VERSION 9

extern const char* FeVersion;

typedef double FeDouble;
typedef struct FeObject FeObject;
typedef struct FeContext FeContext;
typedef struct FeRoot FeRoot;
typedef FeObject* FeNativeFn(FeContext* ctx, FeObject* args);
typedef bool FeInterruptFn(FeContext* ctx, void* userdata);
typedef void FeErrorFn(FeContext* ctx, const char* err, FeObject* cl);
// A host cleanup registered with `FeProtectWithCleanup`. It runs at most once,
// must not raise, must not call back into the evaluator, and must not create
// Fe objects -- see `FeProtectWithCleanup`'s comment for the full contract.
typedef void FeCleanupFn(FeContext* ctx, void* data);
typedef void FeWriteFn(FeContext* ctx, void* udata, char chr);
typedef char FeReadFn(FeContext* ctx, void* udata);

// The kind of the most recent evaluation completion, read through
// `FeGetCompletion`. Sub-plan 06B (kg's Emacs-subset program) made the
// previously dormant kinds true at their producers: the interrupt path raises
// Quit, the step-limit/frame-wall/native-re-entry-wall ceilings raise Budget,
// and every ordinary `FeHandleError` stays Error. Throw is still unassigned
// until 06C. A kind is assigned before a completion reaches the host and stays
// valid until the next run's outermost barrier resets it, so a host reads the
// failing kind from inside its error callback and after it recovers. `quit`
// and `budget` are not signalable condition symbols (06A Decision 1 records
// that as a deliberate exclusion); they are completion kinds, observable only
// through this accessor until 06C/06D give them catch semantics.
typedef enum FeCompletion {
  FeCompletionNormal,
  FeCompletionError,
  FeCompletionThrow,
  FeCompletionQuit,
  FeCompletionBudget,
} FeCompletion;

typedef struct FeEvalOptions {
  size_t step_limit;
  size_t poll_interval;
  FeInterruptFn* interrupt;
  void* userdata;
  // The per-entry step budget every `unwind-protect`/`FeProtectWithCleanup`
  // cleanup gets while this call is unwinding through an error, an
  // interrupt, or its own `step_limit` running out. Zero selects a built-in
  // default. It is never the exhausted or cancelled budget the body was
  // running under -- a cleanup always gets a fresh one, so a body that ran
  // out of steps still gets a working cleanup -- and it is never unbounded
  // either, so a runaway cleanup terminates instead of hanging with no
  // escape. `interrupt`/`userdata`/`poll_interval` stay live for the same
  // drain, re-armed fresh per entry. See `doc/c-api.md`'s "Unwinding And
  // Cleanup" for the worst-case bound this implies.
  size_t cleanup_step_limit;
  // Lisp nesting: the maximum number of simultaneously live ordinary
  // evaluator frames (nested calls, nested special forms, self-expanding
  // macros, deep argument lists) the context-owned frame stack may hold at
  // once. This is a slot count, not a C-stack bound -- the frame machine
  // roots Lisp nesting in the arena, not in C recursion, so this limit costs
  // no C stack no matter how large it is. Zero selects the arena's own
  // physical capacity (`FeArenaStats.frame_capacity`); a nonzero value only
  // ever *lowers* that ceiling, never raises it past what the arena
  // partition actually holds. A push that would exceed the effective limit
  // fails before writing, with "evaluation frame limit exceeded". See
  // `doc/c-api.md`'s "Bounding And Cancelling Evaluation".
  size_t max_frames;
  // Native re-entry: the maximum number of nested evaluator runs a native
  // may start synchronously, one below another -- e.g. `internal--
  // with-current-buffer` calling `FeCall` on a body that itself calls
  // `internal--save-excursion`. Unlike `max_frames`, each level here *is* a
  // real C-stack bound: a native's own C activation, `FeCall`, `Evaluate`
  // and the nested run's barrier cannot be moved off the C stack, so this
  // has to stay a small number, not a large one. Calling a native from Lisp
  // is not by itself re-entry; only that native synchronously starting
  // another evaluation is, so an ordinary top-level host call is never
  // counted against this limit no matter how deep the Lisp nesting it
  // drives. Zero selects the built-in default (`DefaultNativeReentry`,
  // derived from the deepest synchronous re-entry any known embedding
  // actually nests, times a comfortable safety margin -- see its own
  // comment in fe_internal.h). Exceeding it raises "native evaluation
  // re-entry limit exceeded" before the nested run starts.
  //
  // Both `max_frames` and `max_native_reentry` are owned by the outermost
  // active evaluation: a nested `FeCallWithOptions` reached from a native
  // does not replace either ambient limit merely because it was handed
  // another `FeEvalOptions` pointer (see `BeginEvaluationControl`). While an
  // error is unwinding, every cleanup's own re-entry runs under the default
  // ceiling for both limits, not the abandoned body's -- but
  // `native_reentry_depth` itself (the live count, not the configured
  // limit) is not reset for the duration of that unwind: the real C
  // activations the abandoned computation was inside are still live below
  // the barrier until `longjmp` actually pops them, one run at a time. See
  // `doc/c-api.md`'s "Unwinding And Cleanup".
  size_t max_native_reentry;
} FeEvalOptions;

typedef enum FeType {
  FeTPair,
  FeTFree,
  FeTNil,
  FeTDouble,
  // Sub-plan 05A's placement (a) Decision (kg's Emacs-subset program): an
  // integer sits next to the double it was born from, renumbering every
  // later constant. See the version comment above -- the ABI break rides
  // 05D's FE_API_VERSION bump, not this slice.
  FeTInteger,
  FeTSymbol,
  FeTString,
  FeTFn,
  FeTMacro,
  FeTPrimitive,
  FeTNativeFn,
  FeTPtr,

  // This is a disgusting/hilarious way to extend `FeType` in the Fex API: When
  // defining custom types in Fex, use these `FeTFex*` values. Example:
  //
  //   enum {
  //     FexTMyType = FeTFex0,
  //     FexTMyOtherType = FeTFex1,
  //   };
  //
  // This allows us to use `FeType` instead of `int` in the API.
  FeTFex0,
  FeTFex1,
  FeTFex2,
  // Add more as needed:
  //   * add them here
  //   * add cases for them in all relevant `switch`/`case` statements
  //   * add a slot and default name for them in `type_names`
  //   * assign your name for them in `FexInstallNativeFn`
  // TODO: Try to find a way to do this with less toil.

  FeTSentinel,
} FeType;

extern const char* type_names[];

extern FeObject nil;

[[nodiscard]] size_t FeMinimumArenaSize(void);
[[nodiscard]] size_t FeArenaAlignment(void);

// Read-only counters over state Fe already tracks at the sites that change
// it (`MakeObject`, `CollectGarbage`, `FePushGC`, the evaluation-depth and
// cleanup-stack pushes) -- not a live diagnostic surface, and not one this
// call itself grows: `FeGetArenaStats` allocates no Fe object, walks no
// list, and does not mutate `ctx`. It exists to answer "how close is the
// arena to full" questions before and during embedding changes that add to
// every allocation path.
typedef struct FeArenaStats {
  size_t
      total_slots;    // `ctx`'s fixed object capacity (see FeMinimumArenaSize).
  size_t free_slots;  // total_slots minus objects currently live.
  size_t peak_live_objects;    // high-water mark of live objects since
                               // FeOpenContext.
  size_t collection_count;     // CollectGarbage() calls so far.
  size_t peak_gc_stack_depth;  // high-water mark of the FePushGC root stack
                               // (bound: GcStackSize, 4096).
  // The arena's host-usable evaluator frame capacity -- i.e. NOT counting
  // the private cleanup reserve -- the same ceiling FeEvalOptions.max_frames
  // of 0 selects.
  size_t frame_capacity;
  // High-water mark of simultaneously live ordinary evaluator frames
  // (bound: FeEvalOptions.max_frames, default frame_capacity above); always
  // <= frame_capacity for a computation that never triggers cleanup's
  // private reserve.
  size_t peak_frame_depth;
  size_t peak_cleanup_stack_depth;  // high-water mark of the
                                    // unwind-protect/FeProtectWithCleanup
                                    // registry (bound: CleanupStackSize).
  // High-water mark of nested evaluator runs started synchronously from a
  // native (bound: FeEvalOptions.max_native_reentry, default
  // DefaultNativeReentry); zero for a program that never re-enters through
  // a native, no matter how deep its Lisp nesting.
  size_t peak_native_reentry;
  size_t allocation_failures;  // MakeObject() calls that still found no free
                               // slot after a collection.
} FeArenaStats;

[[nodiscard]] FeArenaStats FeGetArenaStats(const FeContext* ctx);

[[nodiscard]] FeContext* FeOpenContext(void* ptr, size_t size);
void FeCloseContext(FeContext* ctx);
void FeSetUserData(FeContext* ctx, void* userdata);
[[nodiscard]] void* FeGetUserData(const FeContext* ctx);
void FeSetErrorFn(FeContext* ctx, FeErrorFn* fn);
// The two collector callbacks. `mark_fn` is called once per reachable
// pointer-carrying object (`FeTPtr`, `FeTFex0`..`FeTFex2`) during the mark
// phase so the host can `FeMark()` whatever that object refers to; `gc_fn`
// is called once per object about to be freed, during the sweep.
//
// Both run *inside* collection, and collection is the one window in which
// the object graph is not readable. The mark phase stores its return path in
// the objects it is walking (Deutsch-Schorr-Waite pointer reversal), so
// while it is inside a `car` chain those cells hold parent links rather than
// their own cars. Two rules follow, and neither is advisory:
//
//   - A callback may call `FeMark()` and may read the object it was handed.
//     It must not read `car`/`cdr` of anything else, and must not allocate.
//   - A callback must return normally. It may not `longjmp` out, and it may
//     not raise -- no `FeHandleError()`, no `FeRaiseCompletion()`, and
//     nothing that raises on its behalf, including `FeCar`/`FeCdr` on a
//     non-pair. There is no stack to unwind the walk from: its state *is*
//     the reversed graph, so leaving non-locally abandons the arena
//     half-reversed. fe detects a raise from inside collection and aborts
//     with a message naming this contract rather than continuing on a heap
//     that will fault later somewhere unrelated.
//
// Printing is safe from a callback (`FeToString()` on the object it was
// handed): the writer does not charge the evaluation step budget or poll the
// interrupt while collecting, precisely so that the obvious diagnostic
// callback cannot trip the rule above.
void FeSetMarkFn(FeContext* ctx, FeNativeFn* fn);
void FeSetGCFn(FeContext* ctx, FeNativeFn* fn);
[[noreturn]] void FeHandleError(FeContext* ctx, const char* msg);
[[noreturn]] void FeRaiseCompletion(FeContext* ctx,
                                    FeCompletion kind,
                                    const char* msg);

// The completion kind of the last completion that reached a host boundary:
// `FeCompletionError` for an ordinary error, `FeCompletionQuit` for the
// interrupt path, `FeCompletionBudget` for the step-limit/frame/re-entry
// walls, and `FeCompletionNormal` after any normal top-level return. Always
// valid -- inside the error callback, and until the next run's outermost
// barrier resets it after recovery. This is Decision 5's (sub-plan 06A)
// additive migration path: a host telling quit from a genuine error reads
// this instead of comparing message strings, and the `FeErrorFn` signature
// itself is unchanged, so every existing host compiles and behaves as before
// without edits.
[[nodiscard]] FeCompletion FeGetCompletion(const FeContext* ctx);
// The completion's `(SYMBOL . DATA)` condition object.
//
// Every `FeCompletionError` has one, arena exhaustion included: since
// sub-plan 09B the two raises that cannot allocate signal pre-built
// `(arena-exhaustion)` and `(evaluation-stack-exhaustion)` conditions
// interned once by `FeOpenContext`, both children of `error` in the
// hierarchy, rather than the nil this comment used to promise. (They are
// shared objects re-stamped before each raise; a caught one is yours to read,
// not to keep or mutate. See `doc/c-api.md`.)
//
// It is nil for a completion that has no condition to describe: any
// `FeCompletionBudget` (the step, frame and native-re-entry walls, which
// `condition-case` deliberately cannot catch and which have no Emacs
// counterpart to name), a `FeCompletionQuit` raised while the arena is
// exhausted (`condition-case` decides a quit by completion *kind* before it
// looks at the object, and calling an interrupt an out-of-memory would be
// worse than saying nothing), and `FeCompletionNormal`.
[[nodiscard]] FeObject* FeGetCondition(const FeContext* ctx);
// The fully formatted text of that completion -- source label included --
// the same string `FeErrorFn` is handed. Valid until the next completion in
// this context.
[[nodiscard]] const char* FeGetCompletionMessage(const FeContext* ctx);
// Puts a completion contained by `FeTryCallWithOptions` back in flight in
// the enclosing run, with its kind, condition object and message intact, so
// an enclosing Lisp `condition-case` matches on the original condition
// symbol. Call it from the frame that made the protected call, after it
// returned false.
[[noreturn]] void FeResignal(FeContext* ctx);

[[nodiscard]] FeType FeGetType(const FeObject* obj);
[[nodiscard]] bool FeIsNil(const FeObject* obj);
[[nodiscard]] FeObject* FeNil(FeContext* ctx);

void FePushGC(FeContext* ctx, FeObject* obj);
void FeRestoreGC(FeContext* ctx, size_t idx);
[[nodiscard]] size_t FeSaveGC(const FeContext* ctx);
// Marks `obj` and everything reachable from it. The only thing a host calls
// it for is a `mark_fn` callback reporting what its pointer object refers
// to; see `FeSetMarkFn` above for the two rules such a callback lives under.
// It uses no C stack proportional to the graph and allocates nothing, but it
// does temporarily reverse the pointers of the objects it is walking, which
// is why those rules exist.
void FeMark(FeContext* ctx, FeObject* obj);

// Registers a C cleanup that runs exactly once, in the same last-in-first-out
// order as Lisp `unwind-protect` cleanups (both share one registry): on an
// ordinary return, a Lisp error, a host interrupt, or step-budget exhaustion.
// Call it from within an active evaluation -- typically a native function
// that is about to call `FeCall`/`FeEvaluate*` on a body it was handed, the
// way `unwind-protect` itself does. It runs when the nearest enclosing call
// form finishes evaluating (normally the native's own call, since that is
// the form still being evaluated when the native runs), or earlier still if
// some form enclosing that one raises first. Calling it outside any active
// evaluation registers a cleanup with no enclosing form to attach to, so
// nothing drains it on an ordinary return; only a later error will.
//
// `fn` is called with `data` and must not fail, must not call back into the
// evaluator, and must not create Fe objects; it may free non-Fe resources and
// call plain C or extension-internal functions. There is no cancellation:
// register only once ownership of the cleanup's resource is final. The
// registry is a fixed-size array sized like the GC stack; exceeding it
// raises "cleanup stack overflow" before `fn` or `data` are recorded, so the
// caller has allocated nothing through this call that it must now release
// itself.
void FeProtectWithCleanup(FeContext* ctx, FeCleanupFn* fn, void* data);

[[nodiscard]] FeObject* FeCons(FeContext* ctx, FeObject* car, FeObject* cdr);
[[nodiscard]] FeObject* FeMakeBool(FeContext* ctx, bool b);
[[nodiscard]] FeObject* FeMakeDouble(FeContext* ctx, FeDouble n);
// Sub-plan 05B of kg's Emacs-subset program: an `int64_t` number, dormant
// -- constructible and readable from the host API but producible by no Lisp
// program yet. `FeToDouble` accepts it, so a host-made integer already flows
// through every double-taking host read; `FeToInteger` is its mirror.
[[nodiscard]] FeObject* FeMakeInteger(FeContext* ctx, int64_t n);
[[nodiscard]] FeObject* FeMakeString(FeContext* ctx, const char* str);
[[nodiscard]] FeObject* FeMakeSymbol(FeContext* ctx, const char* name);
[[nodiscard]] FeObject* FeMakeNativeFn(FeContext* ctx, FeNativeFn fn);
[[nodiscard]] FeObject* FeMakePtr(FeContext* ctx, FeType type, void* ptr);
[[nodiscard]] FeObject* FeMakeList(FeContext* ctx, FeObject** objs, size_t n);

[[nodiscard]] FeObject* FeCar(FeContext* ctx, FeObject* obj);
[[nodiscard]] FeObject* FeCdr(FeContext* ctx, FeObject* obj);

// The writer's default `car`-nesting bound: how deep `FeWrite()` and
// `FeToString()` descend into one object before emitting `#<truncated>`. It
// is public because it is the only sensible answer to "how much of a chain
// is a reader ever shown", and a host printing a bounded *sequence* of
// objects -- `main.c`'s escaping-raise trace is the one in this repository --
// should apply the printer's own number rather than restate it and let the
// two drift.
enum { FeWriteDefaultMaxDepth = 256 };

// Zero in any field selects that field's default; `max_depth`'s is
// `FeWriteDefaultMaxDepth` above.
typedef struct FeWriteOptions {
  size_t max_bytes;
  size_t max_nodes;
  size_t max_depth;
} FeWriteOptions;

void FeWrite(FeContext* ctx, FeObject* obj, FeWriteFn fn, void* udata, int qt);
[[nodiscard]] bool FeWriteWithOptions(FeContext* ctx,
                                      FeObject* obj,
                                      FeWriteFn fn,
                                      void* udata,
                                      int qt,
                                      const FeWriteOptions* options);
void FeWriteFile(FeContext* ctx, FeObject* obj, FILE* fp);

[[nodiscard]] FeObject* FeRead(FeContext* ctx, FeReadFn fn, void* udata);
[[nodiscard]] FeObject* FeReadFile(FeContext* ctx, FILE* fp);
[[nodiscard]] FeObject* FeReadString(FeContext* ctx,
                                     const char* source,
                                     size_t length,
                                     size_t* offset);

[[nodiscard]] size_t FeToString(FeContext* ctx,
                                FeObject* obj,
                                char* dst,
                                size_t size);
[[nodiscard]] size_t FeStringByteLength(FeContext* ctx, const FeObject* obj);
[[nodiscard]] bool FeCopyStringBytes(FeContext* ctx,
                                     const FeObject* obj,
                                     char* dst,
                                     size_t size);
[[nodiscard]] FeDouble FeToDouble(FeContext* ctx, FeObject* obj);
[[nodiscard]] int64_t FeToInteger(FeContext* ctx, FeObject* obj);
[[nodiscard]] void* FeToPtr(FeContext* ctx, FeObject* obj);
void FeSet(FeContext* ctx, FeObject* sym, FeObject* v);
[[nodiscard]] bool FeIsBound(FeContext* ctx, FeObject* sym);
// The Lisp-2 function namespace (sub-plans 04C/04D of kg's Emacs-subset
// program). `FeSet`/`FeIsBound` above keep their Emacs meaning -- the value
// namespace; these three address the function cell instead. `FeSetFunction`
// writes the cell (the object or symbol designator is stored as-is, so a
// `defalias`-style indirection stays a symbol). `FeIsFBound` asks whether the
// cell holds anything. `FeGetFunction` resolves the cell the way call-position
// lookup does -- following defalias symbol indirection iteratively; it returns
// `nil` when the name has no function binding (04D deleted the transitional
// value-cell fallback, so a value-cell callable is *not* resolvable through
// it). A self-referential chain (`(fset 'x 'x)`) is `nil` here too, and does
// *not* raise: this is the resolver a host calls, possibly with no evaluation
// running, and a C host cannot catch an Fe error, so it must answer rather
// than longjmp into a frame that has already returned. Every other reader of
// the chain -- call position, `funcall`, `apply`, `FeIsFunction` -- still
// raises `cyclic-function-indirection`. Use `FeIsFBound` to tell an empty cell
// (`nil`, not f-bound) from a cycle (`nil`, f-bound).
void FeSetFunction(FeContext* ctx, FeObject* sym, FeObject* fn);
[[nodiscard]] FeObject* FeGetFunction(FeContext* ctx, FeObject* sym);
[[nodiscard]] bool FeIsFBound(FeContext* ctx, FeObject* sym);
// `functionp`'s question: is `obj` something the evaluator will call as an
// ordinary function -- a lambda, a host native, or a function-shaped
// primitive? A symbol is resolved through the same function-cell designator
// chain `FeGetFunction` follows (so this asks about the symbol's binding, not
// about the symbol), an unbound name is false, and a cycle raises
// `cyclic-function-indirection` -- unlike `FeGetFunction`, which answers `nil`;
// resolve with that first if you need the non-raising answer for a name that
// may be cyclic. A macro, a special form (`if`, `quote`,
// `lambda`, ...) and any non-callable value are false, which is what Emacs'
// `functionp` answers for them and what `funcall`/`apply` reject as
// `invalid-function`.
[[nodiscard]] bool FeIsFunction(FeContext* ctx, FeObject* obj);
// Registers `fn` under `name` so that call position resolves it. Since
// sub-plan 04D's cut (FE_API_VERSION 3) this writes the symbol's *function*
// cell, the same cell `FeSetFunction`/`FeGetFunction` address and the one
// Lisp call-position resolution reads; before the cut it wrote the value
// cell. A name registered here is `(fboundp 'name)` t and `(boundp 'name)`
// nil.
void FeDefineNative(FeContext* ctx, const char* name, FeNativeFn* fn);

[[nodiscard]] FeObject* FeGetNextArgument(FeContext* ctx, FeObject** arg);
void FeRequireNoArguments(FeContext* ctx, const FeObject* args);
[[nodiscard]] FeRoot* FeCreateRoot(FeContext* ctx, FeObject* object);
[[nodiscard]] FeObject* FeGetRoot(const FeRoot* root);
void FeReleaseRoot(FeContext* ctx, FeRoot* root);
[[nodiscard]] FeObject* FeCall(FeContext* ctx,
                               FeObject* callable,
                               FeObject* const* arguments,
                               size_t count);
[[nodiscard]] FeObject* FeCallWithOptions(FeContext* ctx,
                                          FeObject* callable,
                                          FeObject* const* arguments,
                                          size_t count,
                                          const FeEvalOptions* options);
// The protected call: like `FeCallWithOptions`, but a non-normal completion
// is *returned*, not thrown past this frame. On true, `*result` holds the
// value. On false, nothing was written to `*result`, `error_fn` was not
// called, the host's own frame was not unwound, and `FeGetCompletion`,
// `FeGetCondition` and `FeGetCompletionMessage` describe what happened; the
// host either swallows it or `FeResignal`s it. The callee's cleanups run,
// its frames and GC-stack entries are discarded, and the caller's ambient
// evaluation-control record -- remaining steps included -- is restored.
// This is the entry point a native that re-enters evaluation should use:
// `FeCall`/`FeCallWithOptions` transfer a nested run's completion to the
// *enclosing* run's barrier, past the native's own C frame.
[[nodiscard]] bool FeTryCallWithOptions(FeContext* ctx,
                                        FeObject* callable,
                                        FeObject* const* arguments,
                                        size_t count,
                                        const FeEvalOptions* options,
                                        FeObject** result);
[[nodiscard]] FeObject* FeEvaluate(FeContext* ctx, FeObject* obj);
[[nodiscard]] FeObject* FeEvaluateWithOptions(FeContext* ctx,
                                              FeObject* obj,
                                              const FeEvalOptions* options);
[[nodiscard]] FeObject* FeEvaluateString(FeContext* ctx,
                                         const char* label,
                                         const char* source,
                                         size_t length);
[[nodiscard]] FeObject* FeEvaluateStringWithOptions(
    FeContext* ctx,
    const char* label,
    const char* source,
    size_t length,
    const FeEvalOptions* options);
// The protected string evaluation: `FeEvaluateStringWithOptions` under
// exactly the containment `FeTryCallWithOptions` gives a call. On true,
// `*result` holds the value of the last form evaluated. On false, nothing
// was written to `*result`, `error_fn` was not called, the host's own frame
// was not unwound, and `FeGetCompletion`/`FeGetCondition`/
// `FeGetCompletionMessage` describe what happened; the host either swallows
// it or `FeResignal`s it into the enclosing run. Forms before the raising
// one have already run and their side effects stand.
//
// This is the entry point a host that *loads* Lisp from inside an evaluation
// should use. `FeEvaluateString` is a nested run dressed as a top-level
// call, so a completion raised by the loaded text transfers to the outermost
// barrier -- past every `condition-case` between the load and the raise.
// A `throw` out of the loaded text is contained as the barrier-wall error it
// is; the containment barrier is a throw wall, as the protected call's is.
[[nodiscard]] bool FeTryEvaluateStringWithOptions(FeContext* ctx,
                                                  const char* label,
                                                  const char* source,
                                                  size_t length,
                                                  const FeEvalOptions* options,
                                                  FeObject** result);
[[nodiscard]] FeObject* FeEvaluateFile(FeContext* ctx,
                                       const char* label,
                                       FILE* file);
[[nodiscard]] FeObject* FeEvaluateFileWithOptions(FeContext* ctx,
                                                  const char* label,
                                                  FILE* file,
                                                  const FeEvalOptions* options);

#endif
