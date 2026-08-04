// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

// The evaluator: evaluation control, the unwind-protect/FeProtectWithCleanup
// cleanup registry, FeHandleError (moved here with the cleanup registry --
// see doc/fe-upstream.md and sub-plan 03B of
// doc/plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans in kg), the
// frame-driven evaluator, and its public entry points. Split out of fe.c,
// which keeps the object model, garbage collector, reader and writer. Both
// translation units share the private, self-contained fe_internal.h.
//
// Sub-plan 03E of the same set deleted the last recursive evaluation path:
// every special form and primitive is now a frame kind driven by
// `RunEvaluationLoop`, and `RunEvaluationBody` gives `unwind-protect`
// cleanups (and `if`/`while`/`do`'s implicit bodies) a body-frame entry
// point without a second evaluator. There is one evaluator, reached by one
// path, per the parent plan's requirement.

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"
#include "fe_internal.h"

static void ClearEvaluationControl(FeContext* ctx) {
  ctx->evaluation_interrupt = nullptr;
  ctx->evaluation_userdata = nullptr;
  ctx->evaluation_steps = 0;
  ctx->evaluation_poll_interval = 0;
  ctx->evaluation_poll_countdown = 0;
  ctx->evaluation_active = false;
  ctx->evaluation_limited = false;
  ctx->cleanup_step_limit = 0;
  ctx->evaluation_depth = 0;
  ctx->evaluation_depth_limit = 0;
  ctx->native_reentry_depth = 0;
}

void EndEvaluationControl(FeContext* ctx, bool owns_control) {
  if (owns_control) {
    ClearEvaluationControl(ctx);
  }
}

// Forward-declared so `FeHandleError`, near the top of the file for
// historical reasons, can drain pending `unwind-protect` forms; defined with
// the rest of the evaluator below. A cleanup's forms are a nested
// frame-machine run on the unused suffix of the same frame stack, above a
// saved barrier -- see `RunEvaluationBody`'s own comment -- not a second
// evaluator or a separate stack.
static FeObject* RunEvaluationBody(FeContext* ctx,
                                   FeObject* forms,
                                   FeObject* env);

static void PushCleanup(FeContext* ctx, FeCleanupEntry entry) {
  if (ctx->cleanup_stack_index == CleanupStackSize) {
    FeHandleError(ctx, "cleanup stack overflow");
  }
  ctx->cleanup_stack[ctx->cleanup_stack_index++] = entry;
  if (ctx->cleanup_stack_index > ctx->arena_peak_cleanup_stack_depth) {
    ctx->arena_peak_cleanup_stack_depth = ctx->cleanup_stack_index;
  }
}

// Copies an error message raised while a cleanup entry was itself running
// into context-owned storage, since the formatted message `FeHandleError` is
// about to `longjmp` away from lives on that frame's stack and would
// otherwise be gone by the time `RunOneCleanupEntry` reads it.
static void SaveCleanupErrorMessage(FeContext* ctx, const char* msg) {
  const size_t capacity = sizeof(ctx->cleanup_error_message) - 1;
  size_t length = 0;
  while (length < capacity && msg[length] != '\0') {
    length++;
  }
  memcpy(ctx->cleanup_error_message, msg, length);
  ctx->cleanup_error_message[length] = '\0';
}

// Runs one cleanup entry. A cleanup that itself raises does not reach
// `error_fn` -- that would let the host `longjmp` away and abandon the rest
// of the stack -- so `FeHandleError` redirects here instead (see
// `cleanup_catch`) and the failure becomes a printed diagnostic. Per
// `doc/unwind-design.md`, the original error or interrupt that is actually
// unwinding takes priority: it is what every remaining entry, and finally
// the host, still sees.
static void RunOneCleanupEntry(FeContext* ctx, const FeCleanupEntry* entry) {
  jmp_buf local_jump;
  jmp_buf* const saved_catch = ctx->cleanup_catch;
  jmp_buf* const saved_evaluator_catch = ctx->evaluator_catch;
  const size_t saved_frame_stack_index = ctx->frame_stack_index;
  const size_t saved_native_reentry_depth = ctx->native_reentry_depth;
  FeObject* const saved_call_list = ctx->call_list;
  ctx->cleanup_catch = &local_jump;
  if (setjmp(local_jump) == 0) {
    if (entry->kind == FeCleanupNative) {
      entry->as.native.fn(ctx, entry->as.native.data);
    } else {
      RunEvaluationBody(ctx, entry->as.lisp.forms, entry->as.lisp.env);
    }
  } else {
    fprintf(stderr, "fe: unwind-protect cleanup error: %s\n",
            ctx->cleanup_error_message);
  }
  ctx->cleanup_catch = saved_catch;
  // A cleanup error longjmps directly here, bypassing the nested
  // RunEvaluation() that installed its own barrier. Do not leave that
  // automatic jmp_buf, its frames, or its trace link live in the context.
  ctx->evaluator_catch = saved_evaluator_catch;
  ctx->frame_stack_index = saved_frame_stack_index;
  ctx->native_reentry_depth = saved_native_reentry_depth;
  ctx->call_list = saved_call_list;
}

// Drains cleanup entries down to (but not including) `target`, most recently
// pushed first, leaving the ambient evaluation-control record exactly as it
// was found. Used only by `Evaluate`, down to the checkpoint it saved on
// entry, so a call form that returns normally drains what it pushed under
// whatever budget was already ambient -- the same one the rest of the
// program is running under, nothing special. `FeHandleError`'s abnormal
// drain is `RunCleanupsAfterError`, below, not this.
static void RunCleanupsDownTo(FeContext* ctx, size_t target) {
  while (ctx->cleanup_stack_index > target) {
    const FeCleanupEntry entry = ctx->cleanup_stack[--ctx->cleanup_stack_index];
    RunOneCleanupEntry(ctx, &entry);
  }
}

// The evaluation control `RunCleanupsAfterError` re-arms fresh for every
// cleanup entry it runs, captured from the ambient record before
// `FeHandleError` clears it. `step_limit` of 0 means "the host did not set
// `FeEvalOptions.cleanup_step_limit`," resolved to `DefaultCleanupStepLimit`
// where it is used, not here, since the resolved value does not need to
// survive a `longjmp`.
typedef struct FeCleanupBudget {
  FeInterruptFn* interrupt;
  void* userdata;
  size_t poll_interval;
  size_t step_limit;
} FeCleanupBudget;

// Drains the entire cleanup registry after an error, a host interrupt, or
// step-budget exhaustion: every call form still on the C stack is being
// abandoned, so every pending cleanup must run before `FeHandleError`
// reaches the host. Each entry gets its own fresh copy of `budget`, not one
// shared across the whole drain and not the exhausted or cancelled control
// the body was running under: a body that ran out of steps still gets a
// working cleanup (the budget below is new), and a cleanup that does not
// return terminates on its own instead of hanging with no escape (the
// budget below is bounded). Interrupt polling stays live on the same
// re-armed schedule, so a second host interrupt during a runaway cleanup
// aborts that one entry -- caught by its own `RunOneCleanupEntry`, like any
// other cleanup failure -- without a stale poll countdown from the body
// either firing on the first step or (via unsigned underflow) never firing
// again. Leaves the control record cleared when done, matching
// `FeHandleError`'s existing guarantee that the host sees an inactive one.
static void RunCleanupsAfterError(FeContext* ctx,
                                  const FeCleanupBudget* budget) {
  const size_t step_limit =
      budget->step_limit != 0 ? budget->step_limit : DefaultCleanupStepLimit;
  while (ctx->cleanup_stack_index > 0) {
    const FeCleanupEntry entry = ctx->cleanup_stack[--ctx->cleanup_stack_index];
    ctx->evaluation_interrupt = budget->interrupt;
    ctx->evaluation_userdata = budget->userdata;
    ctx->evaluation_poll_interval = budget->poll_interval;
    ctx->evaluation_poll_countdown = budget->poll_interval;
    ctx->evaluation_limited = true;
    ctx->evaluation_steps = step_limit;
    ctx->evaluation_active = true;
    RunOneCleanupEntry(ctx, &entry);
  }
  ClearEvaluationControl(ctx);
}

void FeProtectWithCleanup(FeContext* ctx, FeCleanupFn* fn, void* data) {
  PushCleanup(ctx, (FeCleanupEntry){.kind = FeCleanupNative,
                                    .as.native = {.fn = fn, .data = data}});
}

[[noreturn]] static void TransferEvaluationError(FeContext* ctx,
                                                 const char* msg,
                                                 FeObject* trace) {
  if (ctx->evaluator_catch == nullptr) {
    if (ctx->error_fn != nullptr) {
      ctx->error_fn(ctx, msg, trace);
    }
    abort();
  }
  const size_t length = strlen(msg);
  assert(length < sizeof(ctx->evaluator_error_message));
  memcpy(ctx->evaluator_error_message, msg, length + 1);
  ctx->evaluator_error_trace = trace;
  longjmp(*ctx->evaluator_catch, 1);
}

[[noreturn]] void FeHandleError(FeContext* ctx, const char* msg) {
  FeObject* cl = ctx->call_list;
  const char* label = ctx->error_label;
  const size_t offset = ctx->error_offset;
  const bool has_offset = ctx->error_has_offset;
  // The ambient control record is about to be cleared; this is the fresh
  // budget every cleanup entry runs under below, captured while it is still
  // here to capture.
  const FeCleanupBudget cleanup_budget = {
      .interrupt = ctx->evaluation_interrupt,
      .userdata = ctx->evaluation_userdata,
      .poll_interval = ctx->evaluation_poll_interval,
      .step_limit = ctx->cleanup_step_limit,
  };
  char message[1024];
  // Reset ambient reader/evaluation state before either sort of cleanup runs.
  // The old trace stays in `cl`; a cleanup starts with a fresh visible trace.
  ctx->call_list = &nil;
  ctx->error_label = nullptr;
  ctx->error_has_offset = false;
  ctx->nextchr = '\0';
  ClearEvaluationControl(ctx);

  switch ((label != nullptr) * 2 + has_offset) {
    case 3:
      Format(message, sizeof(message), "%s:%zu: %s", label, offset, msg);
      msg = message;
      break;
    case 2:
      Format(message, sizeof(message), "%s: %s", label, msg);
      msg = message;
      break;
    case 1:
      Format(message, sizeof(message), "byte %zu: %s", offset, msg);
      msg = message;
      break;
    default:
      break;
  }

  if (ctx->cleanup_catch != nullptr) {
    // A cleanup entry's own `fn` or unwind-forms raised. Resume at
    // `RunOneCleanupEntry`'s `setjmp` instead of reaching the host: that
    // keeps unwinding the cleanup stack instead of abandoning it, and
    // preserves whatever error is already in flight above this one.
    SaveCleanupErrorMessage(ctx, msg);
    longjmp(*ctx->cleanup_catch, 1);
  }

  if (ctx->evaluator_catch != nullptr) {
    ctx->completion = FeCompletionError;
  }

  // A fresh, bounded budget, not the exhausted or cancelled one the body
  // was running under and not no budget at all: see `RunCleanupsAfterError`.
  RunCleanupsAfterError(ctx, &cleanup_budget);

  TransferEvaluationError(ctx, msg, cl);
}

bool BeginEvaluationControl(FeContext* ctx, const FeEvalOptions* options) {
  if (ctx->evaluation_active) {
    return false;
  }
  ctx->evaluation_active = true;
  if (options == nullptr) {
    return true;
  }
  ctx->evaluation_limited = options->step_limit != 0;
  ctx->evaluation_steps = options->step_limit;
  ctx->evaluation_interrupt = options->interrupt;
  ctx->evaluation_userdata = options->userdata;
  ctx->evaluation_poll_interval = options->poll_interval != 0
                                      ? options->poll_interval
                                      : DefaultEvalPollInterval;
  ctx->evaluation_poll_countdown = ctx->evaluation_poll_interval;
  ctx->cleanup_step_limit = options->cleanup_step_limit;
  ctx->evaluation_depth_limit = options->max_depth;
  return true;
}

void EvaluationStep(FeContext* ctx) {
  if (ctx->evaluation_limited) {
    if (ctx->evaluation_steps == 0) {
      FeHandleError(ctx, "evaluation step limit exceeded");
    }
    ctx->evaluation_steps--;
  }
  if (ctx->evaluation_interrupt != nullptr &&
      --ctx->evaluation_poll_countdown == 0) {
    ctx->evaluation_poll_countdown = ctx->evaluation_poll_interval;
    if (ctx->evaluation_interrupt(ctx, ctx->evaluation_userdata)) {
      FeHandleError(ctx, "evaluation cancelled");
    }
  }
}

// Bounds C-stack recursion explicitly, instead of the accidental bound
// `GcStackSize` slot consumption used to provide (see
// `DefaultEvaluationDepth`). Called once from `Evaluate`'s pair path, which
// brackets exactly the region `call_list` above it also brackets: both
// restore points there (the macro tail call and the ordinary return)
// decrement `evaluation_depth` back down, and `FeHandleError`, via
// `ClearEvaluationControl`, resets it to 0 on any error the way it resets
// `call_list` to `&nil` -- see the field's comment on `struct FeContext`.
static void EnterEvaluationDepth(FeContext* ctx) {
  ctx->evaluation_depth++;
  if (ctx->evaluation_depth > ctx->arena_peak_evaluation_depth) {
    ctx->arena_peak_evaluation_depth = ctx->evaluation_depth;
  }
  const size_t depth_limit = ctx->evaluation_depth_limit != 0
                                 ? ctx->evaluation_depth_limit
                                 : DefaultEvaluationDepth;
  if (ctx->evaluation_depth > depth_limit) {
    FeHandleError(ctx, "evaluation depth limit exceeded");
  }
}

// Bounds the native-re-entry boundary explicitly: `native_reentry_depth`
// counts the currently active native C activations. The `FeFrameNative`
// resume saves both this counter and `evaluation_depth` before calling
// through this helper, and restores both saved values on the ordinary return
// (an owning nested `FeCallWithOptions` can clear the whole control record --
// both counters included -- before the native returns, so the enclosing
// frames' accounting must be put back); an error skips the restore and is the
// enclosing run barrier's job, and `ClearEvaluationControl` resets the
// counter to 0 with the other live-depth state. An ordinary native is one
// level; only a native that synchronously calls `FeCall`/`FeCallWithOptions`
// starts a nested run and another level, which is the one place a fresh C
// frame legitimately enters through this boundary. Until 03F it shares the
// ambient legacy `evaluation_depth_limit`/`DefaultEvaluationDepth` ceiling
// and the old `evaluation depth limit exceeded` text, so no public
// expectation moves mid-migration; 03F gives native re-entry its own public
// option, statistic and message.
static void EnterNativeDepth(FeContext* ctx) {
  const size_t depth_limit = ctx->evaluation_depth_limit != 0
                                 ? ctx->evaluation_depth_limit
                                 : DefaultEvaluationDepth;
  if (ctx->native_reentry_depth >= depth_limit) {
    FeHandleError(ctx, "evaluation depth limit exceeded");
  }
  ctx->native_reentry_depth++;
}

static FeObject* Evaluate(FeContext* ctx,
                          FeObject* obj,
                          FeObject* env,
                          FeObject** bind);

static FeObject* Bind(FeContext* ctx,
                      FeObject* env,
                      FeObject* name,
                      FeObject* value) {
  return FeCons(ctx, FeCons(ctx, name, value), env);
}

// Binds a lambda or macro parameter list to an argument list. Three spellings
// collect the remaining arguments: Fe's dotted tail `(a . r)`, Fe's bare symbol
// `r`, and Emacs Lisp's `(a &rest r)`. Under `FeSetStrictArity()` a parameter
// before `&optional` must have an argument, an argument must have somewhere to
// go, and a parameter must be a symbol; otherwise a missing argument is nil, an
// extra one is dropped, and a non-symbol parameter binds nothing -- Fe's
// historical behaviour.
static FeObject* ArgsToEnv(FeContext* ctx,
                           FeObject* prm,
                           FeObject* arg,
                           FeObject* env) {
  const bool strict = ctx->strict_arity;
  bool optional = false;
  while (!FeIsNil(prm)) {
    EvaluationStep(ctx);
    if (FeGetType(prm) != FeTPair) {
      return Bind(ctx, env, prm, arg);
    }
    FeObject* name = CAR(prm);
    prm = CDR(prm);
    if (IsNamedSymbol(name, "&optional")) {
      optional = true;
      continue;
    }
    if (IsNamedSymbol(name, "&rest")) {
      if (FeGetType(prm) != FeTPair) {
        FeHandleError(ctx, "&rest needs a parameter name");
      }
      if (!FeIsNil(CDR(prm))) {
        FeHandleError(ctx, "&rest must be the last parameter");
      }
      return Bind(ctx, env, CAR(prm), arg);
    }
    if (strict) {
      if (FeGetType(name) != FeTSymbol) {
        FeHandleError(ctx, "parameter is not a symbol");
      }
      if (!optional && FeIsNil(arg)) {
        FeHandleError(ctx, "wrong-number-of-arguments");
      }
    }
    env = Bind(ctx, env, name, FeCar(ctx, arg));
    arg = FeCdr(ctx, arg);
  }
  if (strict && !FeIsNil(arg)) {
    FeHandleError(ctx, "wrong-number-of-arguments");
  }
  return env;
}

// Local to `=`: an honest `wrong-type-argument` message, rather than
// `CheckType()`'s generic "expected double, got X" text, which the compat
// oracle comparator does not recognise. See doc/language.md.
static FeObject* CheckNumericEqualOperand(FeContext* ctx, FeObject* obj) {
  if (FeGetType(obj) != FeTDouble) {
    FeHandleError(ctx, "wrong-type-argument");
  }
  return obj;
}

// `=` (`PNumericEqual` in `ResumeEvalList`): numeric equality over Fe's
// existing doubles, chained left to right, Emacs' ordinary-function
// semantics -- the complete raw argument list is evaluated left to right
// before any value is type-checked, so a type error in an early operand
// never erases a side effect a later operand's form already had -- then
// every operand is validated and compared without short-circuiting, even
// once the chain is already known unequal, so every operand form has both
// run and been checked by the time `=` returns -- one argument is `t`
// without comparing anything. Plain C `==` gives the pinned signed-zero
// (`0.0 = -0.0` is true) and NaN (never `=` to itself) answers;
// -Wfloat-equal is suppressed for this intentional exact comparison, the
// same way `Equal()`'s `IsNearlyEqual()` helper above does for its own
// `a == b` infinity special case. See doc/language.md.

[[noreturn]] static void HandleVoidSymbol(FeContext* ctx,
                                          FeObject* symbol,
                                          const char* kind) {
  char name[48];
  char message[64];
  (void)FeToString(ctx, symbol, name, sizeof(name));
  Format(message, sizeof(message), "%s %s", kind, name);
  FeHandleError(ctx, message);
}

// `(foo 1)` where `foo` has no function value names the culprit, the way
// Emacs Lisp's `void-function` does; anything else is anonymous.
[[noreturn]] static void HandleNonCallable(FeContext* ctx, FeObject* callee) {
  if (FeGetType(callee) == FeTSymbol) {
    HandleVoidSymbol(ctx, callee, "void-function");
  }
  FeHandleError(ctx, "tried to call non-callable value");
}

// The head of a call is resolved on the frame stack rather than through
// `Evaluate` so that an unassigned name is `void-function`, as in Emacs Lisp,
// even though Fe has one namespace and would otherwise say `void-variable`. A
// symbol head resolves synchronously in `RunEvaluation`'s expression branch;
// a computed head is pushed as a sub-expression and the frame resumes as
// `FeFrameCallHead` once its value is known.

// Forward-declared so `DispatchResolvedCall`, which every resolved-call path
// (symbol head and computed head alike) funnels through, can reach it before
// its own definition below `PushBodyFrame`, which it needs.
static bool DispatchPrimitive(FeContext* ctx,
                              FeEvalFrame* frame,
                              FeObject* fn,
                              FeObject** frame_bind,
                              FeObject** result);

// Dispatches a call whose head has already resolved to `fn`. `quote`
// short-circuits on its first raw argument. An ordinary callable (native
// function or lambda) switches the frame to `FeFrameCallArguments` -- the
// caller resumes the loop, and each argument is evaluated as a sub-expression
// frame rather than through a recursive `EvaluateList` -- and returns false.
// A macro switches the frame to `FeFrameMacro` -- its arguments stay raw and
// unevaluated, bound by `ArgsToEnv` the way the recursive arm bound them --
// and returns false. Every other primitive is set up by `DispatchPrimitive`,
// below; a resolved value that is none of these is not callable at all.
// Every resolved-call path funnels through here so the bookkeeping cannot
// drift between a symbol head and a computed head.
static bool DispatchResolvedCall(FeContext* ctx,
                                 FeEvalFrame* frame,
                                 FeObject* fn,
                                 FeObject** frame_bind,
                                 FeObject** result) {
  if (FeGetType(fn) == FeTPrimitive) {
    if (PRIM(fn) == PQuote) {
      FeObject* arguments = CDR(frame->expr);
      *result = FeGetNextArgument(ctx, &arguments);
      return true;
    }
    return DispatchPrimitive(ctx, frame, fn, frame_bind, result);
  }
  if (FeGetType(fn) == FeTNativeFn || FeGetType(fn) == FeTFn) {
    frame->kind = FeFrameCallArguments;
    frame->fn = fn;
    frame->rest = CDR(frame->expr);
    frame->accumulator = &nil;
    // Sentinel: the argument frame's resume case appends `callee` when it
    // holds a delivered argument value, so `&unbound` marks a frame no
    // argument has completed in yet and the case starts the first argument
    // instead. No expression can evaluate to `&unbound` (reading it is
    // `void-variable`), and the static `FeTFree` object is a GC leaf, so it
    // is safe both as a marker and to be collected over.
    frame->callee = &unbound;
    return false;
  }
  if (FeGetType(fn) == FeTMacro) {
    frame->kind = FeFrameMacro;
    // Root the callable while `ArgsToEnv` below allocates (a collection may
    // run inside it): `frame->fn` is a collector root. It is then
    // overwritten with the caller environment, which the expansion must be
    // evaluated in -- the body otherwise replaces `frame->env` with the
    // argument bindings over the macro's closure environment.
    frame->fn = fn;
    frame->accumulator = &nil;
    frame->callee = &unbound;
    FeObject* va = CDR(fn);  // (env params ...)
    FeObject* vb = CDR(va);  // (params ...)
    // The raw, unevaluated arguments are bound by the same allocating,
    // never-evaluating helper, charging one step per parameter walk exactly
    // as the recursive arm did.
    FeObject* const caller_env = frame->env;
    frame->env = ArgsToEnv(ctx, CAR(vb), CDR(frame->expr), CAR(va));
    frame->rest = CDR(vb);
    frame->fn = caller_env;
    return false;
  }
  // `(void)frame_bind` -- not used here: a raw pair-form head that resolves
  // to a non-callable value is an error regardless of whether the enclosing
  // sequence was going to receive a `let`-style environment update.
  (void)frame_bind;
  HandleNonCallable(ctx, CAR(frame->expr));
}

// Allocates the next frame-stack slot, or raises the same transitional
// "evaluation depth limit exceeded" text 03D's native boundary already uses
// (03F gives the physical frame wall its own message). `CleanupFrameReserve`
// extra slots are available only while a cleanup is draining
// (`ctx->completion != FeCompletionNormal`, set by `FeHandleError` before
// `RunCleanupsAfterError` runs), so a cleanup triggered by exhaustion is not
// itself immediately refused by the same wall the body just hit.
static FeEvalFrame* AllocateFrame(FeContext* ctx) {
  const size_t reserve =
      ctx->completion == FeCompletionNormal ? 0 : CleanupFrameReserve;
  if (ctx->frame_stack_index == ctx->frame_stack_capacity + reserve) {
    FeHandleError(ctx, "evaluation depth limit exceeded");
  }
  return &ctx->frame_stack[ctx->frame_stack_index++];
}

static void PushEvaluationFrame(FeContext* ctx,
                                FeObject* obj,
                                FeObject* env,
                                FeObject** bind) {
  FeEvalFrame* frame = AllocateFrame(ctx);
  *frame = (FeEvalFrame){.kind = FeFrameExpression,
                         .expr = obj,
                         .env = env,
                         .bind = bind,
                         .fn = &nil,
                         .rest = &nil,
                         .accumulator = &nil,
                         .callee = &nil};
}

// Pushes a sequential-body frame directly -- bypassing the call path
// (`FeFrameCallHead`/`FeFrameCallArguments`/`FeFrameLambda`) that normally
// produces one -- for the places a raw form list is evaluated as an implicit
// body with no call involved: `if`'s false branch, `while`'s body each
// iteration, `do`, and a cleanup's unwind forms (`RunEvaluationBody`,
// below). `env` is the environment the forms see (never a fresh callee
// environment; nothing here binds parameters). `FeFrameImplicitBody`, not
// `FeFrameBody`: this frame's push has no preceding `EnterEvaluationDepth`
// or trace-cell link to balance the way a lambda-body frame's dispatch
// already did, so it completes through a lighter path (see
// `CompleteImplicitBodyFrame`) that does not touch `evaluation_depth` or
// `call_list` -- see `FeFrameImplicitBody`'s own comment in fe_internal.h.
// This frame gets its own fresh GC/cleanup checkpoints, exactly as the
// recursive `DoList` took its own `FeSaveGC()` on every call, independent
// of whatever checkpoint the enclosing form already holds; `.expr` is set
// to the harmless `&nil` sentinel rather than left at its zero default
// because `FeMarkEvaluatorRoots` marks it unconditionally for every live
// frame.
static void PushBodyFrame(FeContext* ctx, FeObject* env, FeObject* forms) {
  FeEvalFrame* frame = AllocateFrame(ctx);
  *frame = (FeEvalFrame){.kind = FeFrameImplicitBody,
                         .expr = &nil,
                         .env = env,
                         .bind = NULL,
                         .fn = &nil,
                         .rest = forms,
                         .accumulator = &nil,
                         .callee = &unbound,
                         .gc_checkpoint = FeSaveGC(ctx),
                         .cleanup_checkpoint = ctx->cleanup_stack_index};
}

// The completion for a synthetic `FeFrameImplicitBody`: drains cleanups it
// registered directly (ordinarily a no-op, since each inner pair-form
// already drained its own on the way out) and restores its own GC
// checkpoint, protecting the result -- but does not decrement
// `evaluation_depth` or unlink `call_list`, because pushing this frame never
// incremented or linked either. `CompletePairFrame`, by contrast, always
// pairs with the `EnterEvaluationDepth`/trace-link a real pair-form dispatch
// already did.
static void CompleteImplicitBodyFrame(FeContext* ctx,
                                      const FeEvalFrame* frame,
                                      FeObject* result) {
  RunCleanupsDownTo(ctx, frame->cleanup_checkpoint);
  FeRestoreGC(ctx, frame->gc_checkpoint);
  FePushGC(ctx, result);
}

// Sets up the frame state for a resolved primitive call (`fn`'s `PRIM`,
// anything but `PQuote`, which the caller already handled), or completes
// synchronously for the primitives that evaluate nothing at all (`PEnv`,
// `PFn`, `PMacro`) or whose raw-argument shape already decides the answer
// before any evaluation (`PIf`'s empty form, `PLet`'s `frame_bind == NULL`
// case -- see `FeFrameLet`'s own comment in fe_internal.h for why the value
// form is then never even evaluated). Returns true with `*result` holding
// the completed value, or false after switching `frame`'s kind to one of the
// resumable continuation kinds `ResumeContinuation` drives below. This is
// the frame-machine replacement for the old recursive `EvaluatePrimitive`;
// see 03E's spec (`doc/plans/2026-08-03-elisp-subset-and-fe-evaluator-
// subplans/03e-special-form-frames-and-unwind.md` in kg) for the table of
// which primitives share which evaluation-order rules and why a single
// generic "evaluate every argument first" policy is not an equivalent
// replacement for most of them.
static bool DispatchPrimitive(FeContext* ctx,
                              FeEvalFrame* frame,
                              FeObject* fn,
                              FeObject** frame_bind,
                              FeObject** result) {
  FeObject* arguments = CDR(frame->expr);
  switch (PRIM(fn)) {
    case PEnv:
      *result = ctx->symbol_list;
      return true;
    // `lambda`/`macro`: build the closure object directly from the raw,
    // unevaluated `(params body...)` tail -- nothing here evaluates
    // anything. The discarded `FeGetNextArgument` call is a validation-only
    // arity check (there must be at least a parameter-list slot), exactly as
    // the recursive arm's identical discard was.
    case PFn:
    case PMacro: {
      FeObject* const closure = FeCons(ctx, frame->env, arguments);
      (void)FeGetNextArgument(ctx, &arguments);
      FeObject* const obj = MakeObject(ctx);
      SetType(obj, PRIM(fn) == PFn ? FeTFn : FeTMacro);
      CDR(obj) = closure;
      *result = obj;
      return true;
    }
    // `let`: the raw target check always runs; the value form is evaluated
    // -- and only then bound into `*frame_bind` -- only when this form is
    // extending an enclosing sequence's environment at all. A `NULL`
    // `frame_bind` (e.g. `let` used as an ordinary argument or a lone `if`
    // branch) means the value form is never evaluated, matching the
    // recursive arm's `if (newenv) { ... EVAL_ARG() ... }`.
    case PLet: {
      FeObject* const target =
          CheckType(ctx, FeGetNextArgument(ctx, &arguments), FeTSymbol);
      if (frame_bind == NULL) {
        *result = &nil;
        return true;
      }
      frame->kind = FeFrameLet;
      frame->accumulator = target;
      frame->rest = arguments;
      frame->callee = &unbound;
      return false;
    }
    // `(if COND THEN ELSE...)`, as in Emacs Lisp: the trailing forms are an
    // implicit body. A missing condition is nil without evaluating anything.
    case PIf:
      if (FeIsNil(arguments)) {
        *result = &nil;
        return true;
      }
      frame->kind = FeFrameIf;
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    // `while`: the condition form is fixed for the frame's whole lifetime
    // (held in `fn`, unused for anything else by this kind); an absent
    // condition raises the same "too few arguments" `FeGetNextArgument`
    // always raises on an empty list.
    case PWhile:
      frame->kind = FeFrameWhile;
      frame->fn = FeGetNextArgument(ctx, &arguments);
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    case PAnd:
    case POr:
      frame->kind = FeFrameAndOr;
      frame->fn = fn;
      frame->rest = arguments;
      frame->callee = &unbound;
      return false;
    // `do`: reuses the sequential-body machinery directly -- this frame
    // becomes a relay for whatever the body frame above it delivers.
    case PDo:
      frame->kind = FeFrameRelay;
      PushBodyFrame(ctx, frame->env, arguments);
      return false;
    // `(unwind-protect BODY CLEANUP...)`: registers the cleanup, then
    // relays BODY's value. The cleanup itself runs later, when this pair
    // form's `CompletePairFrame` (on every exit route -- see
    // `RunCleanupsDownTo`/`RunCleanupsAfterError`) or an enclosing abnormal
    // drain reaches it; this case only registers and starts BODY.
    case PUnwindProtect: {
      FeObject* const body = FeGetNextArgument(ctx, &arguments);
      PushCleanup(ctx, (FeCleanupEntry){
                           .kind = FeCleanupLisp,
                           .as.lisp = {.forms = arguments, .env = frame->env}});
      frame->kind = FeFrameRelay;
      PushEvaluationFrame(ctx, body, frame->env, NULL);
      return false;
    }
    case PSetq:
      frame->kind = FeFrameSetq;
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    // `set`: exact two-argument raw arity is rejected before either side
    // effect can run, matching the recursive arm; the two raw forms are
    // then evaluated left to right by the shared `FeFrameEvalList` machinery
    // an ordinary call's argument list also uses.
    case PSet:
      if (FeGetType(arguments) != FeTPair ||
          FeGetType(CDR(arguments)) != FeTPair ||
          !FeIsNil(CDR(CDR(arguments)))) {
        FeHandleError(ctx, "wrong-number-of-arguments");
      }
      frame->kind = FeFrameEvalList;
      frame->fn = fn;
      frame->rest = arguments;
      frame->accumulator = &nil;
      frame->callee = &unbound;
      return false;
    // `=`: zero raw arguments is rejected before anything evaluates; one or
    // more are evaluated as a batch (`FeFrameEvalList`) before any operand
    // is type-checked or compared.
    case PNumericEqual:
      if (FeIsNil(arguments)) {
        FeHandleError(ctx, "wrong-number-of-arguments");
      }
      frame->kind = FeFrameEvalList;
      frame->fn = fn;
      frame->rest = arguments;
      frame->accumulator = &nil;
      frame->callee = &unbound;
      return false;
    case PList:
      frame->kind = FeFrameEvalList;
      frame->fn = fn;
      frame->rest = arguments;
      frame->accumulator = &nil;
      frame->callee = &unbound;
      return false;
    case PAssert:
    case PBoundp:
    case PMakeUnbound:
    case PNot:
    case PAtom:
    case PCar:
    case PCdr:
      frame->kind = FeFrameUnary;
      frame->fn = fn;
      frame->rest = arguments;
      frame->callee = &unbound;
      return false;
    case PCons:
    case PSetCar:
    case PSetCdr:
    case PIs:
    case PLess:
    case PLessEqual:
      frame->kind = FeFrameBinary;
      frame->fn = fn;
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    case PAdd:
    case PSub:
    case PMul:
    case PDiv:
      frame->kind = FeFrameArith;
      frame->fn = fn;
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    case PPrint:
      frame->kind = FeFramePrint;
      frame->rest = arguments;
      frame->callee = &unbound;
      return false;
    case PQuote:
    case PSentinel:
    default:
      // `PQuote` is handled by the caller before `DispatchPrimitive` is ever
      // reached; `PSentinel` is not a real primitive tag. Neither can name a
      // resolved `fn` here.
      abort();
  }
}

// One argument-frame step: append `callee` -- the just-delivered argument
// value, unless it is still the `&unbound` sentinel that marks a freshly set
// up frame -- and then either start the next argument (charging one step,
// taking the raw argument, and pushing it as a sub-expression frame) or, once
// `rest` is exhausted, dispatch the callable. The accumulator is built
// reversed and reordered here. A native callable switches the frame to
// `FeFrameNative` -- the loop's resume invokes it synchronously from that
// explicit state, bounded by `native_reentry_depth` -- while a lambda
// switches the frame to `FeFrameLambda`: `ArgsToEnv` produces the callee
// environment (allocating, never evaluating) and the frame hands it, with
// the raw body forms, to the sequential-body frame. Returns false when a
// frame was pushed or the frame was switched and the loop must continue.
static bool ResumeArguments(FeContext* ctx, FeEvalFrame* frame) {
  if (frame->callee != &unbound) {
    frame->accumulator = FeCons(ctx, frame->callee, frame->accumulator);
    frame->callee = &unbound;
  }
  if (!FeIsNil(frame->rest)) {
    EvaluationStep(ctx);
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  FeObject* arguments = &nil;
  while (!FeIsNil(frame->accumulator)) {
    FeObject* next = CDR(frame->accumulator);
    CDR(frame->accumulator) = arguments;
    arguments = frame->accumulator;
    frame->accumulator = next;
  }
  FeObject* const fn = frame->fn;
  if (FeGetType(fn) == FeTNativeFn) {
    // The evaluated argument list is ready: switch to the native frame and
    // let the loop invoke the callable from the `FeFrameNative` resume, so
    // the native boundary is its own explicit state. The reordered list
    // moves into `accumulator`, a collector root, so it survives the native's
    // own allocations and any nested run it starts.
    frame->kind = FeFrameNative;
    frame->accumulator = arguments;
    frame->callee = &unbound;
    return false;
  }
  FeObject* va = CDR(fn);  // (env params ...)
  FeObject* vb = CDR(va);  // (params ...)
  frame->kind = FeFrameLambda;
  frame->env = ArgsToEnv(ctx, CAR(vb), arguments, CAR(va));
  frame->rest = CDR(vb);
  // Sentinel, as for the argument frame: marks a freshly set up body frame no
  // form has completed in yet.
  frame->callee = &unbound;
  return false;
}

// One sequential-body step: append `callee` -- the just-delivered body form's
// value, unless it is still the `&unbound` sentinel that marks a freshly set
// up frame -- and then either start the next form (charging one step, taking
// the raw form, and pushing it as a sub-expression frame whose `bind` points
// at this frame's `env`, so a `let` in the form updates the environment the
// following forms see) or, once `rest` is exhausted, complete with the last
// completed value. `accumulator` keeps that value across resumes, and the GC
// stack is restored to the call form's checkpoint on every resume so a body
// of unbounded length does not consume GC-stack slots: everything the body
// needs to survive the next form's allocations is a frame field and therefore
// a mark-phase root. Returns false when a next-form frame was pushed and the
// loop must continue.
static bool ResumeBody(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    frame->accumulator = frame->callee;
    frame->callee = &unbound;
  }
  if (!FeIsNil(frame->rest)) {
    EvaluationStep(ctx);
    FeRestoreGC(ctx, frame->gc_checkpoint);
    FePushGC(ctx, frame->env);
    FePushGC(ctx, frame->rest);
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        &frame->env);
    return false;
  }
  *result = frame->accumulator;
  return true;
}

// One macro-body step: append `callee` -- the just-delivered body form's
// value, unless it is still the `&unbound` sentinel that marks a freshly set
// up frame -- and then either start the next form (charging one step, taking
// the raw form, and pushing it as a sub-expression frame whose `bind` points
// at this frame's `env`, so a `let` in the form extends the environment the
// following forms see, exactly as `DoList`'s `&env` out-parameter did) or,
// once `rest` is exhausted, hand the last completed value -- the expansion --
// to the caller. The expansion handoff is the recursive `FeTMacro` arm's own
// tail, in its exact order: drain the cleanups the body pushed, restore the
// macro call's GC checkpoint and push the expansion onto the GC stack
// (nothing else refers to the expansion now), and unlink the macro call from
// the call trace -- the expansion is evaluated as if written at the call
// site, so an error inside it must not name the macro. The expansion is then
// pushed, still raw, as a sub-expression frame in the caller environment
// (`fn`) with a NULL `bind`, exactly as the recursive arm's
// `Evaluate(ctx, vb, env, NULL)` did, and this frame switches to
// `FeFrameMacroExpansion`. Every step pushes a frame, so this never
// completes the macro frame itself.
static void ResumeMacroBody(FeContext* ctx, FeEvalFrame* frame) {
  if (frame->callee != &unbound) {
    frame->accumulator = frame->callee;
    frame->callee = &unbound;
  }
  if (!FeIsNil(frame->rest)) {
    EvaluationStep(ctx);
    FeRestoreGC(ctx, frame->gc_checkpoint);
    FePushGC(ctx, frame->env);
    FePushGC(ctx, frame->rest);
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        &frame->env);
    return;
  }
  FeObject* const expansion = frame->accumulator;
  RunCleanupsDownTo(ctx, frame->cleanup_checkpoint);
  FeRestoreGC(ctx, frame->gc_checkpoint);
  FePushGC(ctx, expansion);  // Nothing else refers to the expansion now.
  ctx->call_list = CDR(&frame->trace_cell);
  frame->kind = FeFrameMacroExpansion;
  PushEvaluationFrame(ctx, expansion, frame->fn, NULL);
}

// The expansion sub-expression above this frame completed and delivered its
// value into `callee` -- the macro call's result. `callee` is never the
// `&unbound` sentinel here: this kind is switched to and the expansion pushed
// in the same step, so no freshly set up macro-expansion frame ever reaches
// the loop. The shared `CompletePairFrame` tail then restores the call's
// cleanup, GC and call-trace checkpoints again (idempotent with the handoff
// above) and releases the logical depth the macro call entered, held across
// the expansion exactly as the recursive arm held it.
static bool ResumeMacroExpansion(FeEvalFrame* frame, FeObject** result) {
  *result = frame->callee;
  frame->callee = &unbound;
  return true;
}

// One resumable-step dispatch for the two call continuations that evaluate
// their operands as sub-expression frames: an argument list (`frame->rest`
// holds the raw arguments) and a lambda body (`frame->rest` holds the raw
// forms). Both accumulate the delivered values the same way -- into `callee`,
// then into `accumulator` -- so one resume point in `RunEvaluation` serves
// both; only the completing dispatch differs. Returns false when a
// sub-expression frame was pushed and the loop must continue, true when the
// frame completed into `*result`.
static bool ResumeCallStep(FeContext* ctx,
                           FeEvalFrame* frame,
                           FeObject** result) {
  if (frame->kind == FeFrameCallArguments) {
    return ResumeArguments(ctx, frame);
  }
  return ResumeBody(ctx, frame, result);
}

// `if`: after the condition (`accumulator`, `&unbound` marking "not yet
// known"), evaluates the chosen branch. A truthy condition evaluates
// exactly the first then-form with `bind=NULL`, matching the recursive
// arm's `EVAL_ARG()`; a falsy one discards that form and evaluates the
// remaining forms as an implicit body via `PushBodyFrame`. The second
// delivery, from either branch, is the form's result.
static bool ResumeIf(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    if (frame->accumulator == &unbound) {
      frame->accumulator = frame->callee;
      frame->callee = &unbound;
      if (FeIsNil(frame->rest)) {
        *result = &nil;
        return true;
      }
      if (!FeIsNil(frame->accumulator)) {
        PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest),
                            frame->env, NULL);
        return false;
      }
      (void)FeGetNextArgument(ctx, &frame->rest);
      if (FeIsNil(frame->rest)) {
        *result = &nil;
        return true;
      }
      if (FeIsNil(CDR(frame->rest))) {
        // Exactly one else-form -- overwhelmingly the common shape, and the
        // one the frame-storage Decision's "3 retained frames per `deep`
        // level" derivation assumes (the call, `if`, and the arithmetic
        // form waiting on its second operand -- no fourth). Push it
        // directly instead of through `PushBodyFrame`'s generic
        // implicit-body wrapper: a whole extra retained frame per level for
        // a single form would make the physical frame wall bind before the
        // logical one for the canonical `(deep N)` chain, which is exactly
        // the regression this special case exists to avoid (measured
        // during this slice's development: `(deep 274)` failed physically
        // before the fix, `(deep 332)`/`(deep 333)` are the correct
        // boundary after it). `bind = &frame->env`, exactly as the
        // recursive `DoList`'s own `&env` out-parameter threaded even for
        // a lone body form -- the result is simply never read again once
        // `if` completes.
        PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest),
                            frame->env, &frame->env);
        return false;
      }
      PushBodyFrame(ctx, frame->env, frame->rest);
      return false;
    }
    *result = frame->callee;
    return true;
  }
  PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                      NULL);
  return false;
}

// `while`: `fn` holds the fixed condition form and `rest` the fixed body
// forms, both consumed once by `DispatchPrimitive` and never advanced
// again. `accumulator` distinguishes "awaiting the condition" (`&unbound`)
// from "awaiting the body" (anything else -- `&nil` is used, unread).
// Every pass restores this frame's own `gc_checkpoint` -- the same one
// `CompletePairFrame` eventually restores from, taken once at form entry --
// exactly as the recursive arm's per-call `FeRestoreGC(ctx, n)` did, since
// nothing allocated between that entry and the recursive arm's own
// `FeSaveGC()`.
static bool ResumeWhile(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    if (frame->accumulator == &unbound) {
      const FeObject* const condition = frame->callee;
      frame->callee = &unbound;
      if (FeIsNil(condition)) {
        *result = &nil;
        return true;
      }
      EvaluationStep(ctx);
      frame->accumulator = &nil;
      PushBodyFrame(ctx, frame->env, frame->rest);
      return false;
    }
    frame->callee = &unbound;
    FeRestoreGC(ctx, frame->gc_checkpoint);
    frame->accumulator = &unbound;
    PushEvaluationFrame(ctx, frame->fn, frame->env, NULL);
    return false;
  }
  PushEvaluationFrame(ctx, frame->fn, frame->env, NULL);
  return false;
}

// `and`/`or`: `fn` holds the resolved primitive object (to tell the two
// apart) and `rest` the remaining raw forms. Each delivered value decides
// whether to stop -- the first nil for `and`, the first non-nil for `or`,
// or the raw form list running out either way -- and the last delivered
// value is always the result, matching the recursive loop's shared `res`.
// An empty raw form list is nil without evaluating anything.
static bool ResumeAndOr(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    FeObject* const value = frame->callee;
    frame->callee = &unbound;
    const bool stop =
        PRIM(frame->fn) == PAnd ? FeIsNil(value) : !FeIsNil(value);
    if (stop || FeIsNil(frame->rest)) {
      *result = value;
      return true;
    }
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  if (FeIsNil(frame->rest)) {
    *result = &nil;
    return true;
  }
  PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                      NULL);
  return false;
}

// `let`: `frame->bind` is the `newenv` target this frame was pushed with
// (never NULL here -- `DispatchPrimitive` completes the NULL case
// synchronously without ever creating this frame kind), `accumulator` the
// already-checked raw target symbol. The delivered value form's value
// extends `*bind`, and the result is always nil.
static bool ResumeLet(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    *frame->bind = Bind(ctx, frame->env, frame->accumulator, frame->callee);
    frame->callee = &unbound;
    *result = &nil;
    return true;
  }
  PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                      NULL);
  return false;
}

// `setq`: `rest` holds the remaining raw SYMBOL VALUE pairs; `accumulator`
// holds the pending pair's raw target symbol between validating it and the
// value form's delivery. Each target is checked to be a symbol before its
// value form is evaluated, so a non-symbol target is diagnosed without
// evaluating anything; a dangling final SYMBOL is diagnosed only once every
// earlier complete pair has already assigned. Assignment goes through
// `GetBound(ctx, target, env)`, exactly as the recursive arm's did.
static bool ResumeSetq(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    CDR(GetBound(ctx, frame->accumulator, frame->env)) = frame->callee;
    *result = frame->callee;
    frame->callee = &unbound;
    if (FeIsNil(frame->rest)) {
      return true;
    }
  } else if (FeIsNil(frame->rest)) {
    *result = &nil;
    return true;
  }
  if (FeGetType(frame->rest) != FeTPair) {
    FeHandleError(ctx, "wrong-number-of-arguments");
  }
  FeObject* const target = CAR(frame->rest);
  if (FeGetType(target) != FeTSymbol) {
    FeHandleError(ctx, "wrong-type-argument");
  }
  frame->rest = CDR(frame->rest);
  if (FeGetType(frame->rest) != FeTPair) {
    FeHandleError(ctx, "wrong-number-of-arguments");
  }
  frame->accumulator = target;
  PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                      NULL);
  return false;
}

// The primitives that evaluate exactly one operand and finish from it:
// `assert`, `not`, `atom`, `car`, `cdr`, `boundp`, `makunbound`. Extra raw
// forms are left unevaluated for every one of them except `boundp`/
// `makunbound`, which reject a leftover one via `FeRequireNoArguments` --
// `frame->rest` is what remains once the one operand has been consumed.
static bool ResumeUnary(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee == &unbound) {
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  FeObject* const value = frame->callee;
  frame->callee = &unbound;
  switch (PRIM(frame->fn)) {
    case PAssert:
      if (FeIsNil(value)) {
        FeHandleError(ctx, "assertion failure");
      }
      *result = &nil;
      break;
    case PNot:
      *result = FeMakeBool(ctx, FeIsNil(value));
      break;
    case PAtom:
      *result = FeMakeBool(ctx, FeGetType(value) != FeTPair);
      break;
    case PCar:
      *result = FeCar(ctx, value);
      break;
    case PCdr:
      *result = FeCdr(ctx, value);
      break;
    case PBoundp: {
      FeObject* const sym = CheckType(ctx, value, FeTSymbol);
      FeRequireNoArguments(ctx, frame->rest);
      *result =
          FeMakeBool(ctx, CDR(GetBound(ctx, sym, frame->env)) != &unbound);
      break;
    }
    default: {  // PMakeUnbound
      FeObject* const sym = CheckType(ctx, value, FeTSymbol);
      FeRequireNoArguments(ctx, frame->rest);
      CDR(GetBound(ctx, sym, frame->env)) = &unbound;
      *result = sym;
      break;
    }
  }
  return true;
}

// The primitives that evaluate exactly two operands in sequence, with a
// per-primitive check or side effect at each delivery: `cons`, `setcar`,
// `setcdr`, `is`, `<`, `<=`. `accumulator` holds the (possibly checked)
// first operand, `&unbound` marking "not yet delivered". `setcar`/`setcdr`
// validate their first operand as a pair immediately on delivery, before
// the second operand is even evaluated -- the ordering the recursive arm
// required; `<`/`<=` validate both operands as doubles and ignore any
// operands beyond the two, exactly as the recursive `NUM_CMP_OP` macro did.
static bool ResumeBinary(FeContext* ctx,
                         FeEvalFrame* frame,
                         FeObject** result) {
  if (frame->callee == &unbound) {
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  if (frame->accumulator == &unbound) {
    FeObject* first = frame->callee;
    switch (PRIM(frame->fn)) {
      case PSetCar:
      case PSetCdr:
        first = CheckType(ctx, first, FeTPair);
        break;
      case PLess:
      case PLessEqual:
        first = CheckType(ctx, first, FeTDouble);
        break;
      default:
        break;
    }
    frame->accumulator = first;
    frame->callee = &unbound;
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  FeObject* const first = frame->accumulator;
  FeObject* const second = frame->callee;
  frame->callee = &unbound;
  switch (PRIM(frame->fn)) {
    case PCons:
      *result = FeCons(ctx, first, second);
      break;
    case PSetCar:
      CAR(first) = second;
      *result = &nil;
      break;
    case PSetCdr:
      CDR(first) = second;
      *result = &nil;
      break;
    case PIs:
      *result = FeMakeBool(ctx, Equal(first, second));
      break;
    default: {  // PLess, PLessEqual
      const FeObject* const checked_second = CheckType(ctx, second, FeTDouble);
      *result =
          FeMakeBool(ctx, PRIM(frame->fn) == PLessEqual
                              ? GetDouble(first) <= GetDouble(checked_second)
                              : GetDouble(first) < GetDouble(checked_second));
      break;
    }
  }
  return true;
}

// `+`, `-`, `*`, `/`: streams every operand, validating and combining each
// as it arrives, exactly as the recursive `ARITH_OP` macro's own loop did --
// never batching the whole list first the way `FeFrameEvalList` does.
// `accumulator` holds the running total, boxed (`FeMakeDouble`) so it stays
// an ordinary marked field; `&unbound` marks "no operand combined yet".
static bool ResumeArith(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    const FeDouble delivered = FeToDouble(ctx, frame->callee);
    frame->callee = &unbound;
    if (frame->accumulator == &unbound) {
      frame->accumulator = FeMakeDouble(ctx, delivered);
    } else {
      const FeDouble x = GetDouble(frame->accumulator);
      FeDouble combined;
      switch (PRIM(frame->fn)) {
        case PAdd:
          combined = x + delivered;
          break;
        case PSub:
          combined = x - delivered;
          break;
        case PMul:
          combined = x * delivered;
          break;
        default:  // PDiv
          combined = x / delivered;
          break;
      }
      frame->accumulator = FeMakeDouble(ctx, combined);
    }
  }
  if (!FeIsNil(frame->rest)) {
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  *result = frame->accumulator;
  return true;
}

// `print`: streams every operand, writing each as it arrives and printing a
// separating space only when another operand remains, so evaluation, output
// and separators stay interleaved exactly as the recursive arm's loop was.
// The trailing newline is unconditional, including for zero operands.
static bool ResumePrint(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    FeWriteFile(ctx, frame->callee, stdout);
    frame->callee = &unbound;
    if (!FeIsNil(frame->rest)) {
      printf(" ");
    }
  }
  if (!FeIsNil(frame->rest)) {
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  printf("\n");
  *result = &nil;
  return true;
}

// `list`, `=`, `set`: evaluates the complete raw argument list first --
// unlike every other primitive continuation above, whose ordering is what
// makes them not this -- accumulating and reordering exactly as
// `FeFrameCallArguments`'s own argument list does, then finishes per
// primitive: `list` returns it as-is; `=` validates and compares every
// element without short-circuiting, even once the chain is already known
// unequal, so every operand form has both run and been checked by the time
// it returns (one argument is `t` without comparing anything); `set`
// (arity already validated by `DispatchPrimitive` before this frame was
// even created) checks the first element is a symbol and assigns through
// `FeSet`. Plain C `==` gives the pinned signed-zero and NaN answers for
// `=`; -Wfloat-equal is suppressed for this intentional exact comparison.
static bool ResumeEvalList(FeContext* ctx,
                           FeEvalFrame* frame,
                           FeObject** result) {
  if (frame->callee != &unbound) {
    frame->accumulator = FeCons(ctx, frame->callee, frame->accumulator);
    frame->callee = &unbound;
  }
  if (!FeIsNil(frame->rest)) {
    EvaluationStep(ctx);
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  FeObject* list = &nil;
  while (!FeIsNil(frame->accumulator)) {
    FeObject* const next = CDR(frame->accumulator);
    CDR(frame->accumulator) = list;
    list = frame->accumulator;
    frame->accumulator = next;
  }
  switch (PRIM(frame->fn)) {
    case PList:
      *result = list;
      break;
    case PSet: {
      FeObject* const symbol = CAR(list);
      FeObject* const value = CAR(CDR(list));
      if (FeGetType(symbol) != FeTSymbol) {
        FeHandleError(ctx, "wrong-type-argument");
      }
      FeSet(ctx, symbol, value);
      *result = value;
      break;
    }
    default: {  // PNumericEqual
      FeDouble first = GetDouble(CheckNumericEqualOperand(ctx, CAR(list)));
      bool equal = true;
      FeObject* rest = CDR(list);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
      while (!FeIsNil(rest)) {
        const FeDouble next =
            GetDouble(CheckNumericEqualOperand(ctx, CAR(rest)));
        equal = equal && first == next;
        rest = CDR(rest);
      }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
      *result = FeMakeBool(ctx, equal);
      break;
    }
  }
  return true;
}

// One resumable-step dispatch for every primitive continuation above,
// mirroring `ResumeCallStep`'s role for the call continuations: one switch
// here, rather than one `case`-and-`CompletePairFrame` block per kind
// repeated in `RunEvaluationLoop`, is cheaper in aggregate complexity for
// the same reason a single primitive dispatch switch was cheaper than many
// small handlers (see `DispatchPrimitive`'s own comment). `FeFrameRelay`
// has no dedicated `ResumeX`: the delivered value already is its result.
static bool ResumeContinuation(FeContext* ctx,
                               FeEvalFrame* frame,
                               FeObject** result) {
  switch (frame->kind) {
    case FeFrameIf:
      return ResumeIf(ctx, frame, result);
    case FeFrameWhile:
      return ResumeWhile(ctx, frame, result);
    case FeFrameAndOr:
      return ResumeAndOr(ctx, frame, result);
    case FeFrameLet:
      return ResumeLet(ctx, frame, result);
    case FeFrameSetq:
      return ResumeSetq(ctx, frame, result);
    case FeFrameUnary:
      return ResumeUnary(ctx, frame, result);
    case FeFrameBinary:
      return ResumeBinary(ctx, frame, result);
    case FeFrameArith:
      return ResumeArith(ctx, frame, result);
    case FeFramePrint:
      return ResumePrint(ctx, frame, result);
    case FeFrameEvalList:
      return ResumeEvalList(ctx, frame, result);
    case FeFrameRelay:
      *result = frame->callee;
      return true;
    // Every other kind is driven by its own dedicated case in
    // `RunEvaluationLoop`'s switch and never reaches this dispatcher.
    case FeFrameExpression:
    case FeFrameCallHead:
    case FeFrameCallArguments:
    case FeFrameLambda:
    case FeFrameBody:
    case FeFrameImplicitBody:
    case FeFrameMacro:
    case FeFrameMacroExpansion:
    case FeFrameNative:
      break;
  }
  abort();
}

void FeMarkEvaluatorRoots(FeContext* ctx) {
  for (size_t i = 0; i < ctx->frame_stack_index; i++) {
    const FeEvalFrame* frame = &ctx->frame_stack[i];
    switch (frame->kind) {
      case FeFrameExpression:
      case FeFrameCallHead:
      case FeFrameCallArguments:
      case FeFrameLambda:
      case FeFrameBody:
      case FeFrameMacro:
      case FeFrameMacroExpansion:
      case FeFrameNative:
      case FeFrameImplicitBody:
      case FeFrameIf:
      case FeFrameWhile:
      case FeFrameAndOr:
      case FeFrameLet:
      case FeFrameSetq:
      case FeFrameRelay:
      case FeFrameUnary:
      case FeFrameBinary:
      case FeFrameArith:
      case FeFramePrint:
      case FeFrameEvalList:
        FeMark(ctx, frame->expr);
        FeMark(ctx, frame->env);
        FeMark(ctx, frame->fn);
        FeMark(ctx, frame->rest);
        FeMark(ctx, frame->accumulator);
        FeMark(ctx, frame->callee);
        break;
    }
  }
}

// The frame kinds that wait for a delivered sub-expression value: a
// computed-head frame, an argument frame, a body or macro-body frame, a
// macro frame waiting for its expansion's value, and every primitive
// continuation kind above (each pushes at least one sub-expression or
// implicit-body frame and resumes from what it delivers). Anything else
// popping above one of these is an internal error.
static bool IsAwaitingDelivery(const FeEvalFrame* frame) {
  switch (frame->kind) {
    case FeFrameCallHead:
    case FeFrameCallArguments:
    case FeFrameBody:
    case FeFrameImplicitBody:
    case FeFrameMacro:
    case FeFrameMacroExpansion:
    case FeFrameIf:
    case FeFrameWhile:
    case FeFrameAndOr:
    case FeFrameLet:
    case FeFrameSetq:
    case FeFrameRelay:
    case FeFrameUnary:
    case FeFrameBinary:
    case FeFrameArith:
    case FeFramePrint:
    case FeFrameEvalList:
      return true;
    case FeFrameExpression:
    case FeFrameLambda:
    case FeFrameNative:
      break;
  }
  return false;
}

// The ordinary-return tail of a pair form: drain the cleanups pushed while
// this form was being evaluated, restore its GC checkpoint, protect its
// result, and unlink its call-trace cell. Every pair form completes through
// here -- a symbol head, a resolved computed head, a call, or a primitive
// continuation -- so the evaluation_depth and call_list bookkeeping (and the
// `macro` arm's own internal restore) cannot drift between the paths.
static void CompletePairFrame(FeContext* ctx,
                              FeEvalFrame* frame,
                              FeObject* result) {
  RunCleanupsDownTo(ctx, frame->cleanup_checkpoint);
  FeRestoreGC(ctx, frame->gc_checkpoint);
  FePushGC(ctx, result);
  ctx->call_list = CDR(&frame->trace_cell);
  ctx->evaluation_depth--;
}

// Transfers an evaluator-run error to the enclosing barrier, or to the host
// when there is none. Only the outermost `RunEvaluation`'s error path calls
// this, so the extra C call never sits on a recursion's persistent depth.
[[noreturn]] static void TransferRunError(FeContext* ctx,
                                          jmp_buf* saved_catch) {
  if (saved_catch != nullptr) {
    longjmp(*saved_catch, 1);
  }
  if (ctx->error_fn != nullptr) {
    ctx->error_fn(ctx, ctx->evaluator_error_message,
                  ctx->evaluator_error_trace);
  }
  abort();
}

// Installs a run's own error barrier: saves the enclosing catch, makes
// `jump` the active one, and -- only at the outermost barrier -- resets the
// completion state, which a nested run must leave alone. Called once per
// run, before the run's own `setjmp`, so the extra C call is never on the
// recursion's persistent depth. The returned pointer is valid only while
// the caller's `jump` is live, and the caller restores it on every exit.
static jmp_buf* BeginRunBarrier(FeContext* ctx, jmp_buf* jump) {
  jmp_buf* const saved = ctx->evaluator_catch;
  ctx->evaluator_catch = jump;
  if (saved == nullptr) {
    ctx->completion = FeCompletionNormal;
  }
  return saved;
}

// Drives the frame stack from `base` (already holding this run's one base
// frame -- `RunEvaluation` pushes an expression, `RunEvaluationBody` a
// sequential body) down to `base` again, returning the base frame's result.
// Shared by both entry points so there is exactly one loop implementing
// evaluation, not two near-duplicates that could drift.
static FeObject* RunEvaluationLoop(FeContext* ctx, size_t base) {
  FeObject* result = &nil;
  while (ctx->frame_stack_index > base) {
    FeEvalFrame* frame = &ctx->frame_stack[ctx->frame_stack_index - 1];
    FeObject* const expr = frame->expr;
    FeObject* const frame_env = frame->env;
    // The `bind`/`newenv` target this frame carries: the run's own for the
    // base frame, `&frame->env` for a body form (so a `let` in the form
    // extends the environment the following body forms see), and NULL for a
    // computed-head sub-frame or an argument sub-frame, exactly as the
    // recursive `Evaluate` called each with.
    FeObject** const frame_bind = frame->bind;
    switch (frame->kind) {
      case FeFrameExpression:
        if (FeGetType(expr) == FeTPair) {
          EvaluationStep(ctx);
          CAR(&frame->trace_cell) = expr;
          CDR(&frame->trace_cell) = ctx->call_list;
          ctx->call_list = &frame->trace_cell;
          EnterEvaluationDepth(ctx);
          frame->gc_checkpoint = FeSaveGC(ctx);
          frame->cleanup_checkpoint = ctx->cleanup_stack_index;
          FeObject* const head = CAR(expr);
          if (FeGetType(head) != FeTSymbol) {
            // Computed head: evaluate the head expression on the frame stack
            // and resume this frame as `FeFrameCallHead` when its value is
            // known. Previously `EvaluateHead` recursed into `Evaluate` here.
            frame->kind = FeFrameCallHead;
            PushEvaluationFrame(ctx, head, frame_env, NULL);
            continue;
          }
          EvaluationStep(ctx);
          FeObject* fn = CDR(GetBound(ctx, head, frame_env));
          if (fn == &unbound) {
            HandleNonCallable(ctx, head);
          }
          if (!DispatchResolvedCall(ctx, frame, fn, frame_bind, &result)) {
            // The frame switched to `FeFrameCallArguments`; the resume case
            // at the top of the loop starts the first argument.
            continue;
          }
          CompletePairFrame(ctx, frame, result);
        } else if (FeGetType(expr) == FeTSymbol) {
          EvaluationStep(ctx);
          result = CDR(GetBound(ctx, expr, frame_env));
          if (result == &unbound) {
            HandleVoidSymbol(ctx, expr, "void-variable");
          }
        } else {
          EvaluationStep(ctx);
          result = expr;
        }
        break;

      case FeFrameCallHead:
        // The head expression above this frame completed and delivered its
        // value into `callee`; resume the call with it.
        {
          FeObject* fn = frame->callee;
          if (!DispatchResolvedCall(ctx, frame, fn, frame_bind, &result)) {
            // The frame switched to `FeFrameCallArguments`; the resume case
            // at the top of the loop starts the first argument.
            continue;
          }
          CompletePairFrame(ctx, frame, result);
        }
        break;

      case FeFrameLambda:
        // The lambda application is complete: `ArgsToEnv` already produced
        // the callee environment (into `env`) and `rest` holds the raw body
        // forms, so the frame becomes a sequential-body frame and its resume
        // below starts the first form. This is its own named kind so the two
        // steps -- binding the parameters, then evaluating the body -- stay
        // separately documented, as the plan requires.
        frame->kind = FeFrameBody;
        // fall through
      case FeFrameCallArguments:
      case FeFrameBody:
        // An argument or body sub-expression above this frame completed and
        // delivered its value into `callee`; the `&unbound` sentinel means
        // the frame was just set up and no operand has completed in yet.
        if (!ResumeCallStep(ctx, frame, &result)) {
          // A next-argument or next-form frame was pushed; resume at the top
          // of the loop.
          continue;
        }
        CompletePairFrame(ctx, frame, result);
        break;

      case FeFrameImplicitBody:
        // `ResumeBody`'s own logic is exactly right for this synthetic
        // body -- append `callee`, push the next form or complete -- but
        // this frame's own push had no `EnterEvaluationDepth`/trace-link to
        // balance, so it completes through the lighter
        // `CompleteImplicitBodyFrame` instead of `CompletePairFrame`. See
        // `FeFrameImplicitBody`'s comment in fe_internal.h.
        if (!ResumeBody(ctx, frame, &result)) {
          continue;
        }
        CompleteImplicitBodyFrame(ctx, frame, result);
        break;

      case FeFrameMacro:
        // A body-form sub-expression above this frame completed and
        // delivered its value into `callee` (or the frame was just set up
        // and `callee` is still the `&unbound` sentinel); the resume case
        // either starts the next form or produces the expansion and pushes
        // it, always pushing a frame.
        ResumeMacroBody(ctx, frame);
        continue;

      case FeFrameMacroExpansion:
        // The expansion above this frame completed and delivered the macro
        // call's result into `callee`; the frame completes with it.
        ResumeMacroExpansion(frame, &result);
        CompletePairFrame(ctx, frame, result);
        break;

      case FeFrameNative:
        // The argument frame just switched to this kind and moved the
        // reordered argument list into `accumulator`. Invoke the native
        // synchronously from the loop -- no per-native setjmp, the run's own
        // barrier is the one in effect -- bounded by `native_reentry_depth`.
        // A native that calls `FeCall`/`FeCallWithOptions` starts a nested
        // run above this frame on a fresh C activation and counts as another
        // active level until it returns, which is why the counter, not the
        // pair-depth accounting, owns this seam.
        //
        // Both active-depth values are restored to their pre-call values on
        // the ordinary return, before `CompletePairFrame`: an owning nested
        // `FeCallWithOptions` (started when no control record is active,
        // e.g. under a plain `FeEvaluateString`) runs `EndEvaluationControl`
        // on its way out, which clears the whole control record --
        // `native_reentry_depth` *and* `evaluation_depth` included -- while
        // the enclosing frames are still live. Restoring both keeps the
        // enclosing evaluation's logical max_depth accounting intact (a blind
        // decrement would wrap `native_reentry_depth` to SIZE_MAX, and
        // merely leaving `evaluation_depth` at 0 would under-count the live
        // pair forms for every later form in this run). `CompletePairFrame`
        // then performs its own unconditional decrement for the enclosing
        // pair form. An error inside the native skips this restore entirely
        // and is the enclosing run barrier's job, exactly as for
        // `evaluator_catch`.
        {
          const size_t saved_depth = ctx->evaluation_depth;
          const size_t saved_native_depth = ctx->native_reentry_depth;
          EnterNativeDepth(ctx);
          result = GetNativeFn(frame->fn)(ctx, frame->accumulator);
          ctx->native_reentry_depth = saved_native_depth;
          ctx->evaluation_depth = saved_depth;
          CompletePairFrame(ctx, frame, result);
        }
        break;

      case FeFrameIf:
      case FeFrameWhile:
      case FeFrameAndOr:
      case FeFrameLet:
      case FeFrameSetq:
      case FeFrameRelay:
      case FeFrameUnary:
      case FeFrameBinary:
      case FeFrameArith:
      case FeFramePrint:
      case FeFrameEvalList:
        // A special-form or primitive continuation: an operand or implicit
        // body sub-frame above this one delivered its value into `callee`
        // (or, for a freshly set-up frame, `callee` is still the `&unbound`
        // sentinel). `ResumeContinuation` either pushes the next
        // sub-expression frame and the loop resumes at the top, or the form
        // is complete.
        if (!ResumeContinuation(ctx, frame, &result)) {
          continue;
        }
        CompletePairFrame(ctx, frame, result);
        break;
    }

    ctx->frame_stack_index--;
    if (ctx->frame_stack_index == base) {
      break;
    }
    // Deliver the completed sub-expression's value to the frame below it: a
    // computed-head frame waiting for its head expression's value, an
    // argument frame waiting for an argument's value, a body frame waiting
    // for a body form's value, or a macro frame waiting for a body form's or
    // the expansion's value.
    frame = &ctx->frame_stack[ctx->frame_stack_index - 1];
    assert(IsAwaitingDelivery(frame));
    frame->callee = result;
  }
  ctx->frame_stack_index = base;
  return result;
}

// One evaluator run: installs the barrier, pushes `obj` as the base
// expression frame, drives `RunEvaluationLoop`, and restores the enclosing
// run's catch and native-reentry depth on every exit -- normal or by
// `longjmp` on error. `bind` is the `newenv` target a `let` at the top of
// `obj` writes into, exactly as the recursive `Evaluate`'s own parameter
// was.
static FeObject* RunEvaluation(FeContext* ctx,
                               FeObject* obj,
                               FeObject* env,
                               FeObject** bind) {
  const size_t base = ctx->frame_stack_index;
  const size_t saved_native_reentry_depth = ctx->native_reentry_depth;
  jmp_buf jump;
  jmp_buf* const saved_catch = BeginRunBarrier(ctx, &jump);

  if (setjmp(jump) != 0) {
    ctx->frame_stack_index = base;
    ctx->call_list = &nil;
    ctx->evaluator_catch = saved_catch;
    ctx->native_reentry_depth = saved_native_reentry_depth;
    TransferRunError(ctx, saved_catch);
  }

  PushEvaluationFrame(ctx, obj, env, bind);
  FeObject* const result = RunEvaluationLoop(ctx, base);
  ctx->evaluator_catch = saved_catch;
  ctx->native_reentry_depth = saved_native_reentry_depth;
  return result;
}

// A cleanup's unwind forms (`unwind-protect`, `FeProtectWithCleanup`'s Lisp
// counterpart), and nothing else, run through here: `RunOneCleanupEntry`
// calls this once per cleanup entry instead of the old recursive `DoList`'s
// one nested `Evaluate()` call per form. 03C's decision was a nested run on
// the unused suffix of the *same* frame stack, above a saved barrier, not a
// second stack -- `base` is exactly `ctx->frame_stack_index` at entry, the
// same value `RunOneCleanupEntry` separately saves and restores around this
// call, so a failing cleanup's `longjmp` past this function's own barrier
// (see `RunOneCleanupEntry`'s `cleanup_catch` comment) leaves nothing of
// this run behind for the caller to clean up beyond that restore. Otherwise
// identical to `RunEvaluation`, except the base frame is a sequential body
// (`PushBodyFrame`) rather than a single expression, so `let` threads
// between the cleanup's own forms exactly as it did through the recursive
// `DoList`'s `&env` out-parameter.
static FeObject* RunEvaluationBody(FeContext* ctx,
                                   FeObject* forms,
                                   FeObject* env) {
  const size_t base = ctx->frame_stack_index;
  const size_t saved_native_reentry_depth = ctx->native_reentry_depth;
  jmp_buf jump;
  jmp_buf* const saved_catch = BeginRunBarrier(ctx, &jump);

  if (setjmp(jump) != 0) {
    ctx->frame_stack_index = base;
    ctx->call_list = &nil;
    ctx->evaluator_catch = saved_catch;
    ctx->native_reentry_depth = saved_native_reentry_depth;
    TransferRunError(ctx, saved_catch);
  }

  PushBodyFrame(ctx, env, forms);
  FeObject* const result = RunEvaluationLoop(ctx, base);
  ctx->evaluator_catch = saved_catch;
  ctx->native_reentry_depth = saved_native_reentry_depth;
  return result;
}

static FeObject* Evaluate(FeContext* ctx,
                          FeObject* obj,
                          FeObject* env,
                          FeObject** bind) {
  return RunEvaluation(ctx, obj, env, bind);
}

FeObject* FeEvaluate(FeContext* ctx, FeObject* obj) {
  return Evaluate(ctx, obj, &nil, NULL);
}

FeObject* FeCall(FeContext* ctx,
                 FeObject* callable,
                 FeObject* const* arguments,
                 size_t count) {
  if (FeGetType(callable) != FeTFn && FeGetType(callable) != FeTNativeFn) {
    FeHandleError(ctx, "tried to call non-callable value");
  }
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, callable);
  for (size_t i = 0; i < count; i++) {
    FePushGC(ctx, arguments[i]);
  }

  FeObject* quote = FeMakeSymbol(ctx, "quote");
  FeObject* forms = &nil;
  for (size_t i = count; i > 0; i--) {
    FeObject* value = FeCons(ctx, arguments[i - 1], &nil);
    value = FeCons(ctx, quote, value);
    forms = FeCons(ctx, value, forms);
  }
  ctx->call_result = Evaluate(ctx, FeCons(ctx, callable, forms), &nil, nullptr);
  FeRestoreGC(ctx, gc);
  return ctx->call_result;
}

FeObject* FeCallWithOptions(FeContext* ctx,
                            FeObject* callable,
                            FeObject* const* arguments,
                            size_t count,
                            const FeEvalOptions* options) {
  const bool owns_control = BeginEvaluationControl(ctx, options);
  FeObject* result = FeCall(ctx, callable, arguments, count);
  EndEvaluationControl(ctx, owns_control);
  return result;
}

FeObject* FeEvaluateWithOptions(FeContext* ctx,
                                FeObject* obj,
                                const FeEvalOptions* options) {
  const bool owns_control = BeginEvaluationControl(ctx, options);
  FeObject* result = FeEvaluate(ctx, obj);
  EndEvaluationControl(ctx, owns_control);
  return result;
}
