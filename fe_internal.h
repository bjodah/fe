// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FE_INTERNAL_H
#define FE_INTERNAL_H

// The private shared surface between fe.c (object model, GC, reader,
// writer) and fe_eval.c (the evaluator). Only those two translation units
// may include this header; everything here is implementation, and nothing
// of it leaks into the public fe.h. It is one of the headers
// `make test-header` compiles on its own (test_internal_header.c), so it
// includes fe.h and the standard headers it needs and nothing more.

#include <setjmp.h>
#include <stddef.h>

#include "fe.h"

typedef enum Primitive {
  PAssert,
  PEnv,
  PLet,
  // Numeric equality, chained left to right over Fe's existing doubles.
  // `=` was Fe's historical, non-Emacs assignment primitive until sub-plan
  // 02C of the Emacs-subset hard cut repurposed it as numeric equality and
  // moved assignment to `setq`/`set` below; see doc/language.md.
  PNumericEqual,
  PSetq,
  PSet,
  PIf,
  PFn,
  PMacro,
  PWhile,
  PQuote,
  PBoundp,
  PMakeUnbound,
  PAnd,
  POr,
  PDo,
  PUnwindProtect,
  PCons,
  PCar,
  PCdr,
  PSetCar,
  PSetCdr,
  PList,
  PNot,
  PIs,
  PAtom,
  PPrint,
  PLess,
  PLessEqual,
  // TODO: Add > and >=, and document them.
  PAdd,
  PSub,
  PMul,
  PDiv,
  PSentinel
} Primitive;

typedef union {
  FeObject* o;
  FeNativeFn* f;
  FeDouble n;
  // TODO: Might need/want to make this `uintptr_t` someday.
  char c;
} Value;

enum {
  // Stored in the lowest-order bit of `Value.c`:
  ConsCell = 0,
  OtherCell = 1,
  // The 2nd-lowest-order bit of `Value.c` is the mark bit:
  GcMarkBit = 2,
  // TODO: This should scale with arena size?
  // A self-recursive Fe call costs several slots, so this also bounds usable
  // recursion depth, at roughly 450 frames -- but that is a side effect of
  // slot consumption, not a designed bound; `DefaultEvaluationDepth` below
  // is the designed one, and is tuned to fire first.
  GcStackSize = 4096,
  StringBufferSize = (sizeof(FeObject*) - 1),
  DefaultEvalPollInterval = 1024,
  // The default `evaluation_depth` ceiling (see `struct FeContext`) when
  // `FeEvalOptions.max_depth` is left 0. Recursion was previously bounded
  // only by `GcStackSize` slot consumption -- an accident of how many GC
  // stack slots a call happens to use, not a designed limit -- so a build
  // with fatter per-call C frames than the one that measured "roughly 450
  // frames" (any sanitizer, `-O0`, a debug build) could exhaust the real C
  // stack first, crashing instead of raising a catchable error. This is a
  // measured value, not a guessed one, and it is measured in
  // `evaluation_depth` units, which count Evaluate() re-entries -- several
  // per level of ordinary recursion like `(deep n)`'s own `if`/`+`/`-`
  // sub-forms, not one -- so it is not directly comparable to a Lisp
  // recursion count `N`. See the commit that introduced this constant for
  // the full binary search across `-Os`, ASan/UBSan and MSan builds and
  // the conversion between the two units; in short, MSan's fatter frames
  // are what make the C stack the binding constraint, and this default
  // stays comfortably under that measured ceiling while keeping a
  // multiple of the >200 floor `test_recursion_depth` requires, since
  // that measured ceiling turned out closer to the floor than a flat
  // "half the ceiling" rule leaves room for.
  DefaultEvaluationDepth = 1000,
  // Cleanup entries are pushed only by `unwind-protect` and
  // `FeProtectWithCleanup`, not by every object creation the way the GC
  // stack is, so nesting this deep is not a realistic program; it is sized
  // generously rather than to match `GcStackSize` slot for slot.
  CleanupStackSize = 256,
  // The per-entry step budget a cleanup gets when `FeEvalOptions`'s
  // `cleanup_step_limit` is 0 (the common case: most callers never think
  // about this at all). Generous enough that the `test_api.c` regression
  // for "a body that exhausted a tiny budget still gets a working cleanup"
  // needs no per-test override -- a cleanup doing realistic restore-state
  // work is at most a few hundred evaluator steps -- and small enough that
  // a runaway one is a bounded pause, not a hang: at Fe's own evaluator
  // rate this is well under a second even in a debug build. A host that
  // wants a different bound, tighter or looser, sets `cleanup_step_limit`
  // explicitly instead of tuning this constant.
  DefaultCleanupStepLimit = 4096,
};

struct FeObject {
  Value car, cdr;
};

// One pending `unwind-protect`/`FeProtectWithCleanup` registration. Lisp and
// C cleanups interleave in one registry so they share a single ordering, per
// `doc/unwind-design.md`: unwinding always drains the most recently pushed
// entry first, whichever kind it is.
typedef enum FeCleanupKind {
  FeCleanupNative,
  FeCleanupLisp,
} FeCleanupKind;

typedef struct FeCleanupEntry {
  FeCleanupKind kind;
  union {
    struct {
      FeCleanupFn* fn;
      void* data;
    } native;
    struct {
      FeObject* forms;  // The unwind forms, evaluated as an implicit `do`.
      FeObject* env;    // The environment `unwind-protect` was entered with.
    } lisp;
  } as;
} FeCleanupEntry;

// The evaluator owns this stack, but the storage is carved out of the host
// arena by fe.c.  Keep the frame deliberately plain: every pointer is an
// explicit GC-rooting decision in FeMarkEvaluatorRoots().
typedef enum FeFrameKind {
  FeFrameExpression,
  FeFrameTemporaryRecursive,
} FeFrameKind;

typedef enum FeCompletion {
  FeCompletionNormal,
  FeCompletionError,
  FeCompletionThrow,
  FeCompletionQuit,
  FeCompletionBudget,
} FeCompletion;

typedef struct FeEvalFrame {
  FeFrameKind kind;
  FeObject* expr;
  FeObject* env;
  FeObject* rest;
  FeObject* accumulator;
  FeObject* callee;
  size_t gc_checkpoint;
  size_t cleanup_checkpoint;
  FeObject trace_cell;
} FeEvalFrame;

enum {
  MinFrameCapacity = 64,
  CleanupFrameReserve = 32,
  FrameArenaPercent = 8,
};

static_assert(sizeof(FeEvalFrame) == 80);
static_assert(alignof(FeEvalFrame) == alignof(FeObject));

// The value of a symbol that has never been assigned. Like `nil` it is a
// static object outside the arena, so the collector neither sweeps it nor has
// to mark it, and `FeMark` treats it as a leaf. It is never returned to Lisp or
// to a host: the only place it lives is a symbol's value cell, which Lisp
// cannot reach (`(cdr sym)` is a type error) and which every reader of a value
// cell turns into `void-variable`. It is tagged `FeTFree` so that an escape
// aborts in the writer instead of impersonating a value. Defined in fe.c;
// both fe.c (installs it in a fresh symbol's value cell) and fe_eval.c (the
// evaluator compares value cells against it) need it, so it is not `static`.
extern FeObject unbound;

#define CAR(x) ((x)->car.o)
#define CDR(x) ((x)->cdr.o)
#define TAG(x) ((x)->car.c)
#define DOUBLE(x) ((x)->cdr.n)
#define PRIM(x) ((x)->cdr.c)
#define NATIVE_FN(x) ((x)->cdr.f)
#define STRING_BUFFER(x) (&(x)->car.c + 1)

// Small object-layer accessors both translation units use. Defined in fe.c,
// declared here instead of kept `static` because fe_eval.c calls them too.
FeDouble GetDouble(const FeObject* o);
FeNativeFn* GetNativeFn(const FeObject* o);
void SetType(FeObject* o, FeType type);
FeObject* CheckType(FeContext* ctx, FeObject* obj, FeType type);
FeObject* GetBound(FeContext* ctx, FeObject* sym, FeObject* env);
FeObject* MakeObject(FeContext* ctx);
bool Equal(FeObject* a, FeObject* b);
bool IsNamedSymbol(const FeObject* v, const char* name);
void __attribute((format(printf, 3, 4))) Format(char* result,
                                                size_t size,
                                                const char* format,
                                                ...);

struct FeContext {
  FeErrorFn* error_fn;
  FeNativeFn* mark_fn;
  FeNativeFn* gc_fn;
  void* userdata;
  FeObject* gc_stack[GcStackSize];
  size_t gc_stack_index;
  FeObject* objects;
  size_t object_count;
  FeObject* call_list;
  FeObject* free_list;
  FeObject* symbol_list;
  FeObject* evaluation_result;
  FeObject* call_result;
  FeObject* root_list;
  FeObject* t;
  FeEvalFrame* frame_stack;
  size_t frame_stack_capacity;
  size_t frame_stack_index;
  FeInterruptFn* evaluation_interrupt;
  void* evaluation_userdata;
  size_t evaluation_steps;
  size_t evaluation_poll_interval;
  size_t evaluation_poll_countdown;
  // The ambient call's configured `FeEvalOptions.cleanup_step_limit` (0
  // until set, meaning "use `DefaultCleanupStepLimit`"). Captured by
  // `FeHandleError` before it clears the rest of the control record, so
  // the fresh per-entry budget every cleanup runs under while unwinding
  // still reflects what the host asked for.
  size_t cleanup_step_limit;
  // Live recursion depth: incremented and decremented around the pair path
  // of `Evaluate` only (see the comment there), so it counts exactly the
  // recursion that consumes C stack. Reset to 0 by `ClearEvaluationControl`,
  // which `FeHandleError` calls before draining cleanup entries, so a
  // cleanup that recurses after a depth overflow starts from 0 too -- the
  // same fresh-re-arm treatment `RunCleanupsAfterError` already gives
  // `evaluation_steps`.
  size_t evaluation_depth;
  // The ambient call's configured `FeEvalOptions.max_depth` (0 until set,
  // meaning "use `DefaultEvaluationDepth`"), resolved where it is checked,
  // not here, for the same reason `cleanup_step_limit` is not.
  size_t evaluation_depth_limit;
  const char* error_label;
  size_t error_offset;
  FeCleanupEntry cleanup_stack[CleanupStackSize];
  size_t cleanup_stack_index;
  // Non-null while a cleanup entry's own `fn`/unwind-forms are running: the
  // `jmp_buf` of the `RunOneCleanupEntry` frame currently waiting for it.
  // `FeHandleError` checks this first, so a cleanup's own error resumes
  // there instead of reaching the host and abandoning the rest of the
  // cleanup stack.
  jmp_buf* cleanup_catch;
  char cleanup_error_message[256];
  // An evaluator barrier owns the automatic jmp_buf it points at. It is
  // installed only for the duration of RunEvaluation(), and errors copy
  // their text and trace below before jumping to it.
  jmp_buf* evaluator_catch;
  FeObject* evaluator_error_trace;
  FeCompletion completion;
  char evaluator_error_message[1024];
  bool evaluation_active;
  bool evaluation_limited;
  bool strict_arity;
  bool error_has_offset;
  char nextchr;

  // Read-only arena/evaluator statistics, exposed by `FeGetArenaStats`.
  // Every field here is maintained at the one or two existing sites that
  // already change the value it tracks (`MakeObject`, `CollectGarbage`,
  // `FePushGC`, `EnterEvaluationDepth`, `PushCleanup`); nothing here reads
  // back its own state to compute anything, so querying it does not walk
  // the arena or any list. `arena_live_count` is the only running total;
  // the rest are high-water marks or event counts, and `free_slots` in
  // `FeArenaStats` is `object_count - arena_live_count` computed at query
  // time rather than stored.
  size_t arena_live_count;
  size_t arena_peak_live_count;
  size_t arena_collection_count;
  size_t arena_peak_gc_stack_depth;
  size_t arena_peak_evaluation_depth;
  size_t arena_peak_cleanup_stack_depth;
  size_t arena_allocation_failures;
};

// Evaluation-control helpers, defined with the rest of the evaluator in
// fe_eval.c. Declared here instead of kept `static` because `GetBound`
// (above, defined in fe.c) charges its environment walk against the same
// step budget, and fe.c's `FeEvaluateStringWithOptions`/
// `FeEvaluateFileWithOptions` wrappers open and close a control record
// around the reader/evaluator loop they keep.
bool BeginEvaluationControl(FeContext* ctx, const FeEvalOptions* options);
void EndEvaluationControl(FeContext* ctx, bool owns_control);
void EvaluationStep(FeContext* ctx);
void FeMarkEvaluatorRoots(FeContext* ctx);

#endif
