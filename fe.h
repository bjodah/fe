// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FE_H
#define FE_H

#include <stddef.h>  // IWYU pragma: keep
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
#define FE_API_VERSION 3

// The Lisp language Fe evaluates. Version 1 was implicit -- Fe's historical,
// non-Emacs dialect, where `=` assigned and returned nil. Version 2 (sub-plan
// 02C of the Emacs-subset hard cut) was the first explicit contract: `setq`
// and `set` are assignment, and `=` is chained numeric equality, matching
// Emacs Lisp. Version 3 (sub-plan 04D) is the Lisp-2 namespace cut: a symbol
// in call position resolves through its function cell only -- the
// value-cell fallback is deleted -- `#'x` reads as `(function x)`, and
// `boundp`-of-a-callable changed meaning (the bootstrap callables now live
// in function cells, so `(boundp 'car)` is nil while `(fboundp 'car)` is t).
// See doc/language.md and doc/c-api.md.
#define FE_LANGUAGE_VERSION 3

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
void FeSetMarkFn(FeContext* ctx, FeNativeFn* fn);
void FeSetGCFn(FeContext* ctx, FeNativeFn* fn);
void FeSetStrictArity(FeContext* ctx, bool strict);
[[nodiscard]] bool FeGetStrictArity(const FeContext* ctx);
[[noreturn]] void FeHandleError(FeContext* ctx, const char* msg);

[[nodiscard]] FeType FeGetType(const FeObject* obj);
[[nodiscard]] bool FeIsNil(const FeObject* obj);
[[nodiscard]] FeObject* FeNil(FeContext* ctx);

void FePushGC(FeContext* ctx, FeObject* obj);
void FeRestoreGC(FeContext* ctx, size_t idx);
[[nodiscard]] size_t FeSaveGC(const FeContext* ctx);
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
[[nodiscard]] FeObject* FeMakeString(FeContext* ctx, const char* str);
[[nodiscard]] FeObject* FeMakeSymbol(FeContext* ctx, const char* name);
[[nodiscard]] FeObject* FeMakeNativeFn(FeContext* ctx, FeNativeFn fn);
[[nodiscard]] FeObject* FeMakePtr(FeContext* ctx, FeType type, void* ptr);
[[nodiscard]] FeObject* FeMakeList(FeContext* ctx, FeObject** objs, size_t n);

[[nodiscard]] FeObject* FeCar(FeContext* ctx, FeObject* obj);
[[nodiscard]] FeObject* FeCdr(FeContext* ctx, FeObject* obj);

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
// it), and raises `cyclic-function-indirection` for a self-referential chain.
void FeSetFunction(FeContext* ctx, FeObject* sym, FeObject* fn);
[[nodiscard]] FeObject* FeGetFunction(FeContext* ctx, FeObject* sym);
[[nodiscard]] bool FeIsFBound(FeContext* ctx, FeObject* sym);
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
[[nodiscard]] FeObject* FeEvaluateFile(FeContext* ctx,
                                       const char* label,
                                       FILE* file);
[[nodiscard]] FeObject* FeEvaluateFileWithOptions(FeContext* ctx,
                                                  const char* label,
                                                  FILE* file,
                                                  const FeEvalOptions* options);

#endif
