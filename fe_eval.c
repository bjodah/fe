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

// Clears the ambient *limits* a cleanup should not inherit from the
// abandoned body (`max_frames_limit`, `native_reentry_limit`, and the rest
// of the ordinary control record), but deliberately leaves
// `native_reentry_depth` alone: that counter is a census of live C
// activations, not a configured ceiling, and those activations are still
// real and still on the C stack when this runs -- see its own comment on
// `struct FeContext`.
static void ClearEvaluationControl(FeContext* ctx) {
  ctx->evaluation_interrupt = nullptr;
  ctx->evaluation_userdata = nullptr;
  ctx->evaluation_steps = 0;
  ctx->evaluation_poll_interval = 0;
  ctx->evaluation_poll_countdown = 0;
  ctx->evaluation_active = false;
  ctx->evaluation_limited = false;
  ctx->cleanup_step_limit = 0;
  ctx->max_frames_limit = 0;
  ctx->native_reentry_limit = 0;
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
  ctx->max_frames_limit = options->max_frames;
  ctx->native_reentry_limit = options->max_native_reentry;
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

// Bounds native re-entry: a nested evaluator run reached only because a
// native, synchronously, started `FeCall`/`FeCallWithOptions`/`FeEvaluate*`
// while another run was already live below it -- see `native_reentry_depth`'s
// own comment on `struct FeContext` for exactly which runs that is (not
// every native invocation, and not a cleanup drain). Called from
// `RunEvaluation` itself, once, only when its own frame stack was already
// non-empty at entry; the caller's local `saved_native_reentry_depth` (its
// value from just before this call) is what every exit path -- ordinary
// return or `longjmp` error -- restores, so the increment this makes is
// exactly undone as that one `RunEvaluation` activation unwinds, with no
// separate reset anywhere else (`ClearEvaluationControl` deliberately leaves
// this counter alone; see its own comment).
static void EnterNativeReentry(FeContext* ctx) {
  const size_t limit = ctx->native_reentry_limit != 0
                           ? ctx->native_reentry_limit
                           : DefaultNativeReentry;
  if (ctx->native_reentry_depth >= limit) {
    FeHandleError(ctx, "native evaluation re-entry limit exceeded");
  }
  ctx->native_reentry_depth++;
  if (ctx->native_reentry_depth > ctx->arena_peak_native_reentry) {
    ctx->arena_peak_native_reentry = ctx->native_reentry_depth;
  }
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

// An error that names the object the program wrote: `void-variable x`,
// `void-function x`, `invalid-function x`. `kind` is the Emacs condition name
// the compat comparator looks for in the message; `symbol` is normally a
// symbol, and is rendered by the writer either way.
[[noreturn]] static void HandleSymbolError(FeContext* ctx,
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
    HandleSymbolError(ctx, callee, "void-function");
  }
  FeHandleError(ctx, "tried to call non-callable value");
}

// Sub-plan 04C/04D's shared function-designator resolver: a function cell may
// hold another symbol (the `defalias` indirection), and every reader of the
// chain -- call position, `funcall`/`apply`, `FeGetFunction` -- follows it
// through here so the rule cannot drift between the sites. One step is
// charged per symbol hop, so a chain that never ends still dies on the step
// budget, and a cycle is named `cyclic-function-indirection` by two-pointer
// detection rather than left to exhaust the budget (`(fset 'x 'x)` is the
// canonical case). Since 04D's cut the chain dies in an empty function cell
// with `&unbound` -- the transitional value-cell fallback is gone, so a
// callable stored only in the value namespace is *not* reachable in call
// position. An empty cell anywhere along the chain is reported as plain
// `&unbound`, and every caller names the symbol *it* was given rather than
// the last link reached: `(fset 'a 'b) (a)` and `(funcall 'a)` are both
// `void-function a` in Emacs, the name the program wrote. A non-symbol value
// is already resolved and is returned unchanged.
static FeObject* ResolveFunctionCallable(FeContext* ctx, FeObject* fn) {
  FeObject* slow = fn;
  FeObject* fast = fn;
  while (FeGetType(slow) == FeTSymbol) {
    FeObject* const cell = SymbolFunction(slow);
    if (cell == &unbound) {
      return &unbound;
    }
    if (FeGetType(cell) != FeTSymbol) {
      return cell;
    }
    EvaluationStep(ctx);
    slow = cell;
    if (FeGetType(fast) == FeTSymbol) {
      FeObject* const f1 = SymbolFunction(fast);
      if (f1 != &unbound && FeGetType(f1) == FeTSymbol) {
        FeObject* const f2 = SymbolFunction(f1);
        if (f2 == slow) {
          FeHandleError(ctx, "cyclic-function-indirection");
        }
        fast = f2;
      } else {
        fast = f1;
      }
    }
  }
  return slow;
}

// A symbol in call position (the `FeFrameExpression` symbol-head arm, and
// only there -- variable reference still goes through `GetBound` directly).
// The function cell is consulted, through `ResolveFunctionCallable`'s
// designator chain, and an unbound cell means the name is not callable; the
// one upfront `EvaluationStep` is the charge the pre-04C head resolution
// shared with `CDR(GetBound(head, env))`, so the step pins hold. A lexical
// binding never shadows call position: `(let ((car 5)) (car x))` still
// resolves `car`'s function cell, per the pinned 04A snapshot.
static FeObject* ResolveCallHead(FeContext* ctx, FeObject* head) {
  EvaluationStep(ctx);
  return ResolveFunctionCallable(ctx, head);
}

// The head of a call is resolved on the frame stack rather than through
// `Evaluate` so that an unassigned name is `void-function`, as in Emacs Lisp,
// and -- since 04D's cut -- a name with only a value binding is the same
// error: call position sees the function cell and nothing else, so the old
// fiction of reporting `void-function` for a name Fe *did* have a value for
// is a fact now. A symbol head resolves synchronously in
// `RunEvaluation`'s expression branch; a computed head is pushed as a
// sub-expression and the frame resumes as `FeFrameCallHead` once its value
// is known.

// Forward-declared so `DispatchResolvedCall`, which every resolved-call path
// (symbol head and computed head alike) funnels through, can reach it before
// its own definition below `PushBodyFrame`, which it needs.
static FeObject* MakeClosure(FeContext* ctx,
                             FeObject* env,
                             FeObject* arguments,
                             FeType type) {
  FeObject* const closure = FeCons(ctx, env, arguments);
  (void)FeGetNextArgument(ctx, &arguments);
  FeObject* const obj = MakeObject(ctx);
  SetType(obj, type);
  CDR(obj) = closure;
  return obj;
}

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

// Allocates the next frame-stack slot, or raises "evaluation frame limit
// exceeded" before writing it. The effective ceiling is
// `min(max_frames_limit, frame_stack_capacity)` with 0 meaning "no
// configured limit, use the full physical capacity" -- `max_frames_limit`
// can only lower the bound the arena partition already sized, never raise
// it. `CleanupFrameReserve` extra slots are available only while a cleanup
// is draining (`ctx->completion != FeCompletionNormal`, set by
// `FeHandleError` before `RunCleanupsAfterError` runs); by then
// `ClearEvaluationControl` has already zeroed `max_frames_limit`, so a
// cleanup pushes against the full physical capacity plus the reserve, not
// whatever tight body limit the abandoned computation was configured with,
// and is not itself immediately refused by the same wall the body just hit.
static FeEvalFrame* AllocateFrame(FeContext* ctx) {
  size_t limit = ctx->frame_stack_capacity;
  if (ctx->max_frames_limit != 0 && ctx->max_frames_limit < limit) {
    limit = ctx->max_frames_limit;
  }
  const size_t reserve =
      ctx->completion == FeCompletionNormal ? 0 : CleanupFrameReserve;
  if (ctx->frame_stack_index == limit + reserve) {
    FeHandleError(ctx, "evaluation frame limit exceeded");
  }
  FeEvalFrame* const frame = &ctx->frame_stack[ctx->frame_stack_index++];
  if (ctx->frame_stack_index > ctx->arena_peak_frame_depth) {
    ctx->arena_peak_frame_depth = ctx->frame_stack_index;
  }
  return frame;
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
// `FeFrameBody`: this frame's push has no preceding trace-cell link to
// balance the way a lambda-body frame's dispatch already did, so it
// completes through a lighter path (see `CompleteImplicitBodyFrame`) that
// does not touch `call_list` -- see `FeFrameImplicitBody`'s own comment in
// fe_internal.h.
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
// checkpoint, protecting the result -- but does not unlink `call_list`,
// because pushing this frame never linked it. `CompletePairFrame`, by
// contrast, always pairs with the trace-link a real pair-form dispatch
// already did.
static void CompleteImplicitBodyFrame(FeContext* ctx,
                                      const FeEvalFrame* frame) {
  RunCleanupsDownTo(ctx, frame->cleanup_checkpoint);
  FeRestoreGC(ctx, frame->gc_checkpoint);
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
      *result = MakeClosure(ctx, frame->env, arguments,
                            PRIM(fn) == PFn ? FeTFn : FeTMacro);
      return true;
    }
    // `function` (sub-plan 04C): a raw-form special form like `quote`, but
    // restricted -- `(function SYM)` is the symbol designator itself, and
    // `(function (lambda ...))`/`(function (fn ...))` is the closure, built
    // by the same helper `lambda`/`fn` themselves use (same validation,
    // object layout, and environment capture). Any other form is an error, per
    // the `unsupported-function-form` spelling pinned in 04A.
    case PFunction: {
      FeObject* const form = FeGetNextArgument(ctx, &arguments);
      FeRequireNoArguments(ctx, arguments);
      if (FeGetType(form) == FeTSymbol) {
        *result = form;
        return true;
      }
      if (FeGetType(form) == FeTPair && (IsNamedSymbol(CAR(form), "lambda") ||
                                         IsNamedSymbol(CAR(form), "fn"))) {
        *result = MakeClosure(ctx, frame->env, CDR(form), FeTFn);
        return true;
      }
      FeHandleError(ctx, "unsupported-function-form");
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
    // `funcall`/`apply` (sub-plan 04C) are function-shaped special forms:
    // they evaluate every operand with the shared EvalList machinery an
    // ordinary call's argument list uses, then dispatch the first result
    // through the designator resolver -- the redispatch shape, chosen over a
    // dedicated apply frame kind because it adds no frame-kind and no new
    // GC-per-state row while costing only the few conses `MakeCallForm`
    // builds (see `ResumeEvalList`'s `PFuncall`/`PApply` arm and
    // doc/implementation.md's note).
    case PFuncall:
    case PApply:
      // Zero raw operands has no callable to dispatch, so it is an arity
      // error before anything evaluates (`(funcall)`/`(apply)`), matching
      // Emacs' wrong-number-of-arguments for both.
      if (FeIsNil(arguments)) {
        FeHandleError(ctx, "wrong-number-of-arguments");
      }
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
    case PSymbolFunction:
    case PSymbolValue:
    case PFboundp:
    case PFmakunbound:
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
    case PFset:
    case PDefalias:
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
    // Sub-plan 04C's function-namespace readers, arity-exact like `boundp`/
    // `makunbound` above. `symbol-function` returns the raw cell -- the
    // designator chain is *not* followed here, exactly as Emacs returns the
    // aliased symbol from `(symbol-function 'a)` -- and an empty cell is the
    // `void-function NAME` error. `symbol-value` reads the *global* value
    // cell (the value namespace's own reader), an empty one `void-variable
    // NAME`. `fboundp`/`fmakunbound` address only the function cell, so the
    // `makunbound`-keeps-function/fmakunbound-keeps-value independence the
    // 04A cases pin is structural: the two unary families never touch each
    // other's cell.
    case PSymbolFunction: {
      FeObject* const sym = CheckType(ctx, value, FeTSymbol);
      FeRequireNoArguments(ctx, frame->rest);
      FeObject* const cell = SymbolFunction(sym);
      if (cell == &unbound) {
        HandleSymbolError(ctx, sym, "void-function");
      }
      *result = cell;
      break;
    }
    case PSymbolValue: {
      FeObject* const sym = CheckType(ctx, value, FeTSymbol);
      FeRequireNoArguments(ctx, frame->rest);
      FeObject* const cell = CDR(GetBound(ctx, sym, &nil));
      if (cell == &unbound) {
        HandleSymbolError(ctx, sym, "void-variable");
      }
      *result = cell;
      break;
    }
    case PFboundp: {
      FeObject* const sym = CheckType(ctx, value, FeTSymbol);
      FeRequireNoArguments(ctx, frame->rest);
      *result = FeMakeBool(ctx, SymbolFunction(sym) != &unbound);
      break;
    }
    case PFmakunbound: {
      FeObject* const sym = CheckType(ctx, value, FeTSymbol);
      FeRequireNoArguments(ctx, frame->rest);
      SetSymbolFunction(sym, &unbound);
      *result = sym;
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
      // `fset`/`defalias` (04C): the target is a symbol, checked before the
      // function form is evaluated, matching the other binary primitives'
      // validate-first ordering.
      case PFset:
      case PDefalias:
        first = CheckType(ctx, first, FeTSymbol);
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
  // `frame->callee` is deliberately *not* cleared before the switch: it is
  // `second`'s only collector root, and `PCons` allocates. Clearing first and
  // relying on the C local left `second` invisible to the mark phase across
  // `FeCons`'s possible collection -- reachable only from a register or a
  // stack slot the collector does not scan -- so a `(cons a b)` whose
  // allocation happened to trigger a GC produced a pair with a freed cdr.
  // That was survivable only while every completed sub-expression's result
  // also sat on the GC stack; once those per-level pushes went away (they
  // were what made the fixed 4096-slot GC stack, not the frame stack, bound
  // recursion depth) the latent hazard became a live use-after-free, found
  // by `fuzz_eval` under the 64 KiB arena that collects often enough to hit
  // it. The general rule for every resume: a frame field stays live until
  // the last operation that might allocate has finished with it.
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
    // `fset`/`defalias` (04C): both write the function cell; `fset` returns
    // the function object, `defalias` the aliased symbol (the 04A snapshot's
    // answer). `defalias` stores `second` as-is, so a symbol designator stays
    // a symbol and is resolved at call time -- the late-binding case.
    case PFset:
      SetSymbolFunction(first, second);
      *result = second;
      break;
    case PDefalias:
      SetSymbolFunction(first, second);
      *result = first;
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
  frame->callee = &unbound;
  return true;
}

// The value an arithmetic frame completes with: the running total, or -- when
// no operand was combined at all -- Emacs' identity element for the operator,
// `(+)` and `(-)` being 0 and `(*)` 1. `(/)` has no identity to return and is
// `wrong-number-of-arguments`, as in Emacs; nothing has evaluated by then, so
// raising here rather than at dispatch is the same observable order.
// Without this the frame completed with the `&unbound` sentinel its
// accumulator still held, and the sentinel escaped into Lisp: `(print (+))`
// reached the writer, which is meant to abort on `FeTFree`, and
// `(funcall (+))` reached the funcall arm with an *empty* evaluated operand
// list, because `ResumeEvalList` reads an `&unbound` delivery as "no operand
// delivered yet" and drops it -- so `CAR(&nil)` walked off a static object.
// "No expression can evaluate to `&unbound`" (see `DispatchResolvedCall`) is
// an invariant of the whole evaluator, not just of the argument frames.
static FeObject* ArithResult(FeContext* ctx, const FeEvalFrame* frame) {
  if (frame->accumulator != &unbound) {
    return frame->accumulator;
  }
  const Primitive primitive = (Primitive)PRIM(frame->fn);
  if (primitive == PDiv) {
    FeHandleError(ctx, "wrong-number-of-arguments");
  }
  return FeMakeDouble(ctx, primitive == PMul ? 1 : 0);
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
    const size_t gc = FeSaveGC(ctx);
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
    FeRestoreGC(ctx, gc);
  }
  if (!FeIsNil(frame->rest)) {
    PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                        NULL);
    return false;
  }
  *result = ArithResult(ctx, frame);
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

// Which primitives are *function-shaped* -- every operand evaluated before
// the primitive itself acts, exactly as an ordinary call's argument list is --
// and which are special forms, whose operands reach them raw. The split is
// read off `DispatchPrimitive`'s routing and nothing else: a row is true when
// that primitive's arm evaluates every operand (the unary, binary, arith,
// print and eval-list frame kinds), false when its arm consumes a raw form.
// `env` is true because it consumes no operand at all, so handing it
// evaluated ones changes nothing. `funcall`/`apply` are themselves
// function-shaped, which is why `(funcall 'funcall '+ 1 2)` works, as it does
// in Emacs.
//
// A primitive with no row here reads as false -- not callable through
// `funcall`/`apply` -- which is the safe side of the mistake: the redispatch
// would otherwise hand quote-wrapped operands to an arm that never evaluates
// them.
static const bool primitive_is_function[PSentinel] = {
    [PAssert] = true,
    [PEnv] = true,
    [PNumericEqual] = true,
    [PSet] = true,
    [PBoundp] = true,
    [PMakeUnbound] = true,
    [PCons] = true,
    [PCar] = true,
    [PCdr] = true,
    [PSetCar] = true,
    [PSetCdr] = true,
    [PList] = true,
    [PNot] = true,
    [PIs] = true,
    [PAtom] = true,
    [PPrint] = true,
    [PLess] = true,
    [PLessEqual] = true,
    [PAdd] = true,
    [PSub] = true,
    [PMul] = true,
    [PDiv] = true,
    [PFset] = true,
    [PDefalias] = true,
    [PSymbolFunction] = true,
    [PSymbolValue] = true,
    [PFboundp] = true,
    [PFmakunbound] = true,
    [PFuncall] = true,
    [PApply] = true,
    // False, listed for the record: `let`, `setq`, `if`, `lambda`, `macro`,
    // `while`, `quote`, `and`, `or`, `do`, `unwind-protect`, `function`.
};

// Whether `fn` is a callable whose operands stay raw -- a macro, or one of
// the special-form primitives above. `funcall`/`apply` reject these: their
// evaluate-then-redispatch shape hands the callable a `(callable (quote v)
// ...)` form, and a raw-form callable sees the wrappers rather than the
// values, so `(funcall 'quote 'a)` used to answer `(quote a)` and
// `(funcall 'if 1 2 3)` used to take a branch of the *quoted* forms. Emacs
// signals `invalid-function` for both. Every other non-callable value keeps
// the `tried to call non-callable value` the redispatched call already
// raises for it.
static bool IsRawFormCallable(const FeObject* fn) {
  const FeType type = FeGetType(fn);
  if (type == FeTMacro) {
    return true;
  }
  return type == FeTPrimitive && !primitive_is_function[(Primitive)PRIM(fn)];
}

// Builds `(callable (quote v) ...)` from a list of already-evaluated values
// -- the same quoted-argument construction `FeCall`'s host path uses -- so a
// resume arm can hand evaluated operands to the ordinary call machinery by
// pushing the result as a sub-expression frame, without re-entering
// evaluation. That is 04C's evaluate-then-redispatch shape for
// `funcall`/`apply` (see `ResumeEvalList`): it reuses every existing frame
// kind and adds no new invariants, at the price of the few conses this
// builds per call. The caller keeps `args` rooted in a frame field across
// this construction (the 03F lesson: these conses can trigger a collection,
// and the values being wrapped are live only through that field).
//
// The GC stack cost is one slot, not three per argument. Every `FeCons` here
// pushes its result (`MakeObject` does), so leaving them pushed until the
// caller's own restore made a wide `apply` die on "GC stack overflow" where
// the equivalent direct call did not. Instead each pass restores the
// checkpoint this function took -- *its own*, above the caller's
// `FePushGC(list)`, which stays -- and re-pushes the chain's head, which
// roots every pair built so far; the same idiom `ReadList` uses. `quote` is
// an interned symbol reachable from `ctx->symbol_list`, a mark-phase root,
// so it needs no slot of its own after the first restore.
static FeObject* MakeCallForm(FeContext* ctx,
                              FeObject* callable,
                              FeObject* args) {
  const size_t gc = FeSaveGC(ctx);
  FeObject* const quote = FeMakeSymbol(ctx, "quote");
  FeObject* forms = &nil;
  while (!FeIsNil(args)) {
    FeObject* const wrapped = FeCons(ctx, CAR(args), &nil);
    FeObject* const quoted = FeCons(ctx, quote, wrapped);
    forms = FeCons(ctx, quoted, forms);
    args = CDR(args);
    FeRestoreGC(ctx, gc);
    FePushGC(ctx, forms);
  }
  // `forms` was built by prepending, so it holds the quote-wrapped arguments
  // reversed; reorder it into argument order (`FeCall`'s host construction
  // cancels the reversal by iterating its array backward -- a singly-linked
  // list cannot do that, so the reorder here is the equivalent).
  FeObject* ordered = &nil;
  while (!FeIsNil(forms)) {
    FeObject* const next = CDR(forms);
    CDR(forms) = ordered;
    ordered = forms;
    forms = next;
  }
  // The reversal left the *last* pair on the GC stack, which roots nothing
  // ahead of it; re-root the head before the final allocation.
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, ordered);
  return FeCons(ctx, callable, ordered);
}

// `apply`'s spread: rebuilds the fixed operands after the callable plus the
// spread list's elements into a fresh argument list -- never mutating the
// caller's list -- and reorders the reversed build into argument order.
// `last` is the final operand's pair and `spread` its value, both already
// validated proper by the caller. The frame's `accumulator` is the
// mark-phase root the buffer is built in (and returned through), so every
// `FeCons` below -- each of which may trigger a collection -- finds the
// partially built list still rooted, the 03F rule. That is also what makes
// the GC stack cost one slot rather than one per spread element: because
// `accumulator` is a root already, each pass can restore the checkpoint
// taken here -- above the caller's `FePushGC(list)`, which stays, and which
// is what keeps the *source* elements alive -- instead of leaving every
// pair `MakeObject` pushed there until the caller's own restore.
// `list` and `last` are walked, never written, but their elements are handed
// to `FeCons`, which takes them mutably: the `const` is on the traversal,
// the same way `SymbolName`'s is (fe.c).
static FeObject* SpreadApplyArgs(FeContext* ctx,
                                 FeEvalFrame* frame,
                                 const FeObject* list,
                                 const FeObject* last,
                                 FeObject* spread) {
  const size_t gc = FeSaveGC(ctx);
  frame->accumulator = &nil;
  for (const FeObject* p = CDR(list); p != last; p = CDR(p)) {
    frame->accumulator = FeCons(ctx, CAR(p), frame->accumulator);
    FeRestoreGC(ctx, gc);
  }
  for (FeObject* p = spread; !FeIsNil(p); p = CDR(p)) {
    frame->accumulator = FeCons(ctx, CAR(p), frame->accumulator);
    FeRestoreGC(ctx, gc);
  }
  FeObject* ordered = &nil;
  FeObject* acc = frame->accumulator;
  while (!FeIsNil(acc)) {
    FeObject* const next = CDR(acc);
    CDR(acc) = ordered;
    ordered = acc;
    acc = next;
  }
  frame->accumulator = ordered;
  return ordered;
}

// `apply`'s final operand. Walks to the last pair of the evaluated operands
// after the callable -- the operand list an EvalList frame builds is always
// proper, so this finds the final operand's pair -- and returns the spread
// value that pair holds, reporting the pair itself through `last`. There must
// *be* such a pair: a callable-only `(apply 'f)` has no final operand and is
// the malformed-tail error, not an empty spread. The spread value must then
// be a proper list in its own right, which the operand list being proper does
// not imply (`(apply '+ 1 '(2 . 3))`).
static FeObject* ApplySpread(FeContext* ctx, FeObject* args, FeObject** last) {
  FeObject* pair = args;
  while (FeGetType(pair) == FeTPair && FeGetType(CDR(pair)) == FeTPair) {
    pair = CDR(pair);
  }
  if (FeGetType(pair) != FeTPair) {
    FeHandleError(ctx, "apply: last argument must be a proper list");
  }
  *last = pair;
  FeObject* const spread = CAR(pair);
  FeObject* walk = spread;
  while (FeGetType(walk) == FeTPair) {
    walk = CDR(walk);
  }
  if (walk != &nil) {
    FeHandleError(ctx, "apply: last argument must be a proper list");
  }
  return spread;
}

// `funcall`/`apply`'s tail, once every operand has been evaluated into
// `list`: resolve the first value through the designator chain and redispatch
// the rest through the ordinary call path (04C's evaluate-then-redispatch
// shape). `list` always holds at least the callable -- `DispatchPrimitive`
// rejects zero raw operands before anything evaluates -- but the emptiness is
// checked rather than assumed: an operand that evaluated to the `&unbound`
// sentinel used to be dropped silently here (`ResumeEvalList` reads such a
// delivery as "nothing delivered yet"), which is how `(funcall (+))` reached
// this arm with nothing to call and dereferenced `&nil`.
static bool DispatchFuncallApply(FeContext* ctx,
                                 FeEvalFrame* frame,
                                 FeObject* list) {
  if (FeIsNil(list)) {
    FeHandleError(ctx, "wrong-number-of-arguments");
  }
  // Root the operand list -- and the callable it starts with -- across the
  // resolution below (the 03F lesson: an evaluated operand buffer is live
  // across every later allocation, and `ResolveFunctionCallable` follows
  // symbol cells while only this frame field keeps the values alive).
  frame->accumulator = list;
  FeObject* const operand = CAR(list);
  FeObject* const callable = ResolveFunctionCallable(ctx, operand);
  // Both errors name the operand the program wrote, not the last link of a
  // designator chain: `(fset 'a 'b) (funcall 'a)` is `void-function a`, as in
  // Emacs, the same name call position already reports.
  if (callable == &unbound) {
    HandleSymbolError(ctx, operand, "void-function");
  }
  if (IsRawFormCallable(callable)) {
    HandleSymbolError(ctx, operand, "invalid-function");
  }
  FeObject* args = CDR(list);
  FeObject* last = &nil;
  FeObject* spread = &nil;
  if (PRIM(frame->fn) == PApply) {
    spread = ApplySpread(ctx, args, &last);
  }
  // `list` roots the callable when it is a direct operand value and the
  // operand buffer while it is rebuilt; keep it on the GC stack across every
  // allocation below. `MakeCallForm`'s conses and the rebuild's conses can
  // trigger a collection, and once `frame->accumulator` becomes the (rebuilt)
  // argument list, the callable -- which is not an element of it -- has no
  // other root until the call form is pushed.
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, list);
  if (PRIM(frame->fn) == PApply) {
    args = SpreadApplyArgs(ctx, frame, list, last, spread);
  }
  // Root the (possibly rebuilt) argument list, then hand the evaluated values
  // to the ordinary call path: this frame becomes a relay for the call form
  // pushed above it.
  frame->accumulator = args;
  frame->kind = FeFrameRelay;
  PushEvaluationFrame(ctx, MakeCallForm(ctx, callable, args), frame->env, NULL);
  FeRestoreGC(ctx, gc);
  return false;
}

// `list`, `=`, `set`, `funcall`, `apply`: evaluates the complete raw
// argument list first --
// unlike every other primitive continuation above, whose ordering is what
// makes them not this -- accumulating and reordering exactly as
// `FeFrameCallArguments`'s own argument list does, then finishes per
// primitive: `list` returns it as-is; `=` validates and compares every
// element without short-circuiting, even once the chain is already known
// unequal, so every operand form has both run and been checked by the time
// it returns (one argument is `t` without comparing anything); `set`
// (arity already validated by `DispatchPrimitive` before this frame was
// even created) checks the first element is a symbol and assigns through
// `FeSet`. `funcall`/`apply` (04C) resolve the first result through the
// designator chain and redispatch the remaining evaluated values. Plain C
// `==` gives the pinned signed-zero and NaN answers for `=`; -Wfloat-equal
// is suppressed for this intentional exact comparison.
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
    case PFuncall:
    case PApply:
      return DispatchFuncallApply(ctx, frame, list);
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
// continuation -- so the `call_list` bookkeeping (and the `macro` arm's own
// internal restore) cannot drift between the paths.
static void CompletePairFrame(FeContext* ctx, FeEvalFrame* frame) {
  RunCleanupsDownTo(ctx, frame->cleanup_checkpoint);
  FeRestoreGC(ctx, frame->gc_checkpoint);
  ctx->call_list = CDR(&frame->trace_cell);
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
          FeObject* fn = ResolveCallHead(ctx, head);
          if (fn == &unbound) {
            HandleNonCallable(ctx, head);
          }
          if (!DispatchResolvedCall(ctx, frame, fn, frame_bind, &result)) {
            // The frame switched to `FeFrameCallArguments`; the resume case
            // at the top of the loop starts the first argument.
            continue;
          }
          CompletePairFrame(ctx, frame);
        } else if (FeGetType(expr) == FeTSymbol) {
          EvaluationStep(ctx);
          result = CDR(GetBound(ctx, expr, frame_env));
          if (result == &unbound) {
            HandleSymbolError(ctx, expr, "void-variable");
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
          CompletePairFrame(ctx, frame);
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
        CompletePairFrame(ctx, frame);
        break;

      case FeFrameImplicitBody:
        // `ResumeBody`'s own logic is exactly right for this synthetic
        // body -- append `callee`, push the next form or complete -- but
        // this frame's own push had no trace-link to balance, so it
        // completes through the lighter `CompleteImplicitBodyFrame` instead
        // of `CompletePairFrame`. See `FeFrameImplicitBody`'s comment in
        // fe_internal.h.
        if (!ResumeBody(ctx, frame, &result)) {
          continue;
        }
        CompleteImplicitBodyFrame(ctx, frame);
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
        CompletePairFrame(ctx, frame);
        break;

      case FeFrameNative:
        // The argument frame just switched to this kind and moved the
        // reordered argument list into `accumulator`. Invoke the native
        // synchronously from the loop -- no per-native setjmp, the run's own
        // barrier is the one in effect. Calling the native is not itself
        // bounded here: it is not re-entry (see `native_reentry_depth`'s
        // comment on `struct FeContext`). If this native synchronously
        // starts a nested run of its own -- `FeCall`/`FeCallWithOptions`/
        // `FeEvaluate*` -- that nested `RunEvaluation` call sees a non-empty
        // frame stack at its own entry and bounds and accounts for itself
        // through `EnterNativeReentry`, restoring its own pre-entry counter
        // on every exit path; nothing here needs to save or restore
        // anything around the call.
        result = GetNativeFn(frame->fn)(ctx, frame->accumulator);
        CompletePairFrame(ctx, frame);
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
        CompletePairFrame(ctx, frame);
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
  // The run's own result is the one value no frame roots any more: every
  // intermediate result is delivered straight into the frame below's
  // `callee` (a mark-phase root) with no allocation in between, so the
  // per-completion `FePushGC` those completions used to do was one live
  // GC-stack entry per level of Lisp nesting -- the last thing making the
  // fixed 4096-slot GC stack, rather than the frame stack, the bound on
  // recursion depth. Pushing once here keeps the value the caller is about
  // to receive alive without that per-level cost.
  FePushGC(ctx, result);
  return result;
}

// One evaluator run: installs the barrier, pushes `obj` as the base
// expression frame, drives `RunEvaluationLoop`, and restores the enclosing
// run's catch and native-reentry depth on every exit -- normal or by
// `longjmp` on error. `bind` is the `newenv` target a `let` at the top of
// `obj` writes into, exactly as the recursive `Evaluate`'s own parameter
// was. A non-empty frame stack at entry is only possible if a native,
// somewhere below, synchronously started this call while its own run was
// still live -- see `native_reentry_depth`'s comment on `struct FeContext`
// -- so that is exactly the condition `EnterNativeReentry` bounds and
// counts; a fresh top-level host call always sees an empty stack and is
// never counted.
static FeObject* RunEvaluation(FeContext* ctx,
                               FeObject* obj,
                               FeObject* env,
                               FeObject** bind) {
  const size_t base = ctx->frame_stack_index;
  const size_t saved_native_reentry_depth = ctx->native_reentry_depth;
  if (base > 0) {
    EnterNativeReentry(ctx);
  }
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
// `DoList`'s `&env` out-parameter -- and, deliberately, this function never
// calls `EnterNativeReentry`: a cleanup drain is not native re-entry,
// however non-empty the frame stack it runs on top of is (see
// `native_reentry_depth`'s comment on `struct FeContext`). A native invoked
// *from within* the cleanup's own forms that itself re-enters evaluation is
// still counted, through its own nested `RunEvaluation` call, same as
// anywhere else.
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

FeObject* FeGetFunction(FeContext* ctx, FeObject* sym) {
  // The host's way to resolve a callable name the way call position does
  // (04C/04D): the function cell, defalias indirection followed. A name with
  // no function binding -- even one whose value cell holds a callable -- is
  // `nil`, since 04D deleted the transitional value-cell fallback; a cycle
  // raises `cyclic-function-indirection`. Outside an active evaluation the
  // per-hop `EvaluationStep` charges are no-ops.
  FeObject* const fn = ResolveFunctionCallable(ctx, sym);
  return fn == &unbound ? FeNil(ctx) : fn;
}

bool FeIsFunction(FeContext* ctx, FeObject* obj) {
  // `functionp`'s question, asked of a resolved callable rather than of a
  // name: a symbol is followed through the same designator chain call
  // position uses, so an unbound name is false and a cycle raises
  // `cyclic-function-indirection` exactly as `FeGetFunction` does. Macros and
  // special forms are false -- the answer Emacs' own `functionp` gives for
  // `if`, `quote` and `lambda` -- and so is every value that is not callable
  // at all.
  const FeObject* const fn = ResolveFunctionCallable(ctx, obj);
  const FeType type = FeGetType(fn);
  if (type == FeTFn || type == FeTNativeFn) {
    return true;
  }
  return type == FeTPrimitive && !IsRawFormCallable(fn);
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
