// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

// The evaluator: evaluation control, the unwind-protect/FeProtectWithCleanup
// cleanup registry, FeHandleError (moved here with the cleanup registry --
// see doc/fe-upstream.md and sub-plan 03B of
// doc/plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans in kg), the
// recursive evaluator itself, and its public entry points. Split out of
// fe.c, which keeps the object model, garbage collector, reader and writer.
// Both translation units share the private, self-contained fe_internal.h.

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
// the rest of the evaluator below.
static FeObject* DoList(FeContext* ctx, FeObject* lst, FeObject* env);

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
      DoList(ctx, entry->as.lisp.forms, entry->as.lisp.env);
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

static FeObject* EvaluateList(FeContext* ctx, FeObject* lst, FeObject* env) {
  FeObject* res = &nil;
  FeObject** tail = &res;
  while (!FeIsNil(lst)) {
    EvaluationStep(ctx);
    *tail = FeCons(ctx, Evaluate(ctx, FeGetNextArgument(ctx, &lst), env, NULL),
                   &nil);
    tail = &CDR(*tail);
  }
  return res;
}

static FeObject* DoList(FeContext* ctx, FeObject* lst, FeObject* env) {
  FeObject* res = &nil;
  const size_t save = FeSaveGC(ctx);
  while (!FeIsNil(lst)) {
    EvaluationStep(ctx);
    FeRestoreGC(ctx, save);
    FePushGC(ctx, lst);
    FePushGC(ctx, env);
    res = Evaluate(ctx, FeGetNextArgument(ctx, &lst), env, &env);
  }
  return res;
}

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

#define EVAL_ARG() Evaluate(ctx, FeGetNextArgument(ctx, &arg), env, NULL)

#define ARITH_OP(op)                          \
  {                                           \
    FeDouble x = FeToDouble(ctx, EVAL_ARG()); \
    while (!FeIsNil(arg)) {                   \
      x = x op FeToDouble(ctx, EVAL_ARG());   \
    }                                         \
    res = FeMakeDouble(ctx, x);               \
  }

#define NUM_CMP_OP(op)                                     \
  {                                                        \
    va = CheckType(ctx, EVAL_ARG(), FeTDouble);            \
    vb = CheckType(ctx, EVAL_ARG(), FeTDouble);            \
    res = FeMakeBool(ctx, GetDouble(va) op GetDouble(vb)); \
  }

// `setq` special form: raw SYMBOL VALUE pairs, evaluated left to right. Each
// target is checked to be a symbol before its value form is evaluated, so a
// non-symbol target is diagnosed without evaluating anything; a dangling
// final SYMBOL is diagnosed only once every earlier complete pair has
// already assigned, so those assignments stand. Assignment goes through
// `GetBound(ctx, target, env)`: an existing lexical binding wins over the
// global cell. See doc/language.md.
static FeObject* EvaluateSetq(FeContext* ctx, FeObject* arg, FeObject* env) {
  FeObject* res = &nil;
  while (!FeIsNil(arg)) {
    if (FeGetType(arg) != FeTPair) {
      FeHandleError(ctx, "wrong-number-of-arguments");
    }
    FeObject* target = CAR(arg);
    if (FeGetType(target) != FeTSymbol) {
      FeHandleError(ctx, "wrong-type-argument");
    }
    arg = CDR(arg);
    if (FeGetType(arg) != FeTPair) {
      FeHandleError(ctx, "wrong-number-of-arguments");
    }
    res = Evaluate(ctx, CAR(arg), env, NULL);
    arg = CDR(arg);
    CDR(GetBound(ctx, target, env)) = res;
  }
  return res;
}

// `set`: ordinary-function semantics, unlike `setq` above -- its symbol
// argument is evaluated like any other. Exact two-argument arity is
// rejected before either raw form is evaluated; the two forms are then
// evaluated left to right via the same `EvaluateList()` path an ordinary
// call uses, so a type error in the resulting first value never erases a
// side effect the second form already had. `FeSet()` looks up with the
// global environment (`&nil`), so a same-named lexical binding is neither
// read nor written.
static FeObject* EvaluateSet(FeContext* ctx, FeObject* arg, FeObject* env) {
  if (FeGetType(arg) != FeTPair || FeGetType(CDR(arg)) != FeTPair ||
      !FeIsNil(CDR(CDR(arg)))) {
    FeHandleError(ctx, "wrong-number-of-arguments");
  }
  FeObject* evaluated = EvaluateList(ctx, arg, env);
  FeObject* symbol = CAR(evaluated);
  FeObject* value = CAR(CDR(evaluated));
  if (FeGetType(symbol) != FeTSymbol) {
    FeHandleError(ctx, "wrong-type-argument");
  }
  FeSet(ctx, symbol, value);
  return value;
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

// `=`: numeric equality over Fe's existing doubles, chained left to right.
// Unlike `setq`/`set` above, `=` has ordinary-function semantics in Emacs:
// the complete raw argument list is evaluated left to right via the
// existing ordinary-call path `EvaluateList()` before any value is type-
// checked, so a type error in an early operand never erases a side effect
// a later operand's form already had. Every operand is then validated and
// compared without short-circuiting, even once the chain is already known
// unequal, so every operand form has both run and been checked by the time
// `=` returns -- one argument is `t` without comparing anything. Plain C
// `==` gives the pinned signed-zero (`0.0 = -0.0` is true) and NaN (never
// `=` to itself) answers; -Wfloat-equal is suppressed for this intentional
// exact comparison, the same way `Equal()`'s `IsNearlyEqual()` helper above
// does for its own `a == b` infinity special case. See doc/language.md.
static FeObject* EvaluateNumericEqual(FeContext* ctx,
                                      FeObject* arg,
                                      FeObject* env) {
  if (FeIsNil(arg)) {
    FeHandleError(ctx, "wrong-number-of-arguments");
  }
  FeObject* evaluated = EvaluateList(ctx, arg, env);
  FeDouble first = GetDouble(CheckNumericEqualOperand(ctx, CAR(evaluated)));
  bool equal = true;
  FeObject* rest = CDR(evaluated);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
  while (!FeIsNil(rest)) {
    FeDouble next = GetDouble(CheckNumericEqualOperand(ctx, CAR(rest)));
    equal = equal && first == next;
    rest = CDR(rest);
  }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  return FeMakeBool(ctx, equal);
}

static FeObject* EvaluatePrimitive(FeContext* ctx,
                                   FeObject* obj,
                                   FeObject* env,
                                   FeObject** newenv,
                                   const FeObject* fn) {
  FeObject* res = &nil;
  FeObject* arg = CDR(obj);
  FeObject* va;
  const FeObject* vb;
  switch (PRIM(fn)) {
    case PAssert:
      va = EVAL_ARG();
      if (FeIsNil(va)) {
        FeHandleError(ctx, "assertion failure");
      }
      return res;
    case PEnv:
      return ctx->symbol_list;
    case PLet:
      va = CheckType(ctx, FeGetNextArgument(ctx, &arg), FeTSymbol);
      if (newenv) {
        *newenv = FeCons(ctx, FeCons(ctx, va, EVAL_ARG()), env);
      }
      return res;
    case PNumericEqual:
      return EvaluateNumericEqual(ctx, arg, env);
    case PSetq:
      return EvaluateSetq(ctx, arg, env);
    case PSet:
      return EvaluateSet(ctx, arg, env);
    // `(if COND THEN ELSE...)`, as in Emacs Lisp: the trailing forms are an
    // implicit `do`. (Fe used to read them as an `elif` chain.)
    case PIf:
      if (FeIsNil(arg)) {
        return res;
      }
      va = EVAL_ARG();
      if (FeIsNil(arg)) {
        return res;
      }
      if (!FeIsNil(va)) {
        return EVAL_ARG();
      }
      (void)FeGetNextArgument(ctx, &arg);
      return DoList(ctx, arg, env);
    case PFn:
    case PMacro:
      va = FeCons(ctx, env, arg);
      (void)FeGetNextArgument(ctx, &arg);
      res = MakeObject(ctx);
      SetType(res, PRIM(fn) == PFn ? FeTFn : FeTMacro);
      CDR(res) = va;
      return res;
    case PWhile: {
      va = FeGetNextArgument(ctx, &arg);
      const size_t n = FeSaveGC(ctx);
      while (!FeIsNil(Evaluate(ctx, va, env, NULL))) {
        EvaluationStep(ctx);
        DoList(ctx, arg, env);
        FeRestoreGC(ctx, n);
      }
      return res;
    }
    case PQuote:
      return FeGetNextArgument(ctx, &arg);
    case PBoundp:
      va = CheckType(ctx, EVAL_ARG(), FeTSymbol);
      FeRequireNoArguments(ctx, arg);
      return FeMakeBool(ctx, CDR(GetBound(ctx, va, env)) != &unbound);
    case PMakeUnbound:
      va = CheckType(ctx, EVAL_ARG(), FeTSymbol);
      FeRequireNoArguments(ctx, arg);
      CDR(GetBound(ctx, va, env)) = &unbound;
      return va;
    case PAnd:
      while (!FeIsNil(arg) && !FeIsNil(res = EVAL_ARG()))
        ;
      return res;
    case POr:
      while (!FeIsNil(arg) && FeIsNil(res = EVAL_ARG()))
        ;
      return res;
    case PDo:
      return DoList(ctx, arg, env);
    // `(unwind-protect BODY CLEANUP...)`: evaluates BODY, and evaluates the
    // CLEANUP forms as an implicit `do` on every exit -- normal return,
    // error, interrupt, or budget exhaustion. This case only registers the
    // cleanup and evaluates BODY; the enclosing `Evaluate` call for this
    // whole form is what actually runs it, on whichever path it takes
    // (see the `cleanup` checkpoint there, and `FeHandleError`).
    case PUnwindProtect: {
      FeObject* body = FeGetNextArgument(ctx, &arg);
      PushCleanup(ctx, (FeCleanupEntry){.kind = FeCleanupLisp,
                                        .as.lisp = {.forms = arg, .env = env}});
      return Evaluate(ctx, body, env, NULL);
    }
    case PCons:
      va = EVAL_ARG();
      return FeCons(ctx, va, EVAL_ARG());
    case PCar:
      return FeCar(ctx, EVAL_ARG());
    case PCdr:
      return FeCdr(ctx, EVAL_ARG());
    case PSetCar:
      va = CheckType(ctx, EVAL_ARG(), FeTPair);
      CAR(va) = EVAL_ARG();
      return res;
    case PSetCdr:
      va = CheckType(ctx, EVAL_ARG(), FeTPair);
      CDR(va) = EVAL_ARG();
      return res;
    case PList:
      return EvaluateList(ctx, arg, env);
    case PNot:
      return FeMakeBool(ctx, FeIsNil(EVAL_ARG()));
    case PIs:
      va = EVAL_ARG();
      return FeMakeBool(ctx, Equal(va, EVAL_ARG()));
    case PAtom:
      return FeMakeBool(ctx, FeGetType(EVAL_ARG()) != FeTPair);
    case PPrint:
      while (!FeIsNil(arg)) {
        FeWriteFile(ctx, EVAL_ARG(), stdout);
        if (!FeIsNil(arg)) {
          printf(" ");
        }
      }
      printf("\n");
      return res;
    case PLess:
      NUM_CMP_OP(<)
      return res;
    case PLessEqual:
      NUM_CMP_OP(<=)
      return res;
    case PAdd:
      ARITH_OP(+)
      return res;
    case PSub:
      ARITH_OP(-)
      return res;
    case PMul:
      ARITH_OP(*)
      return res;
    case PDiv:
      ARITH_OP(/)
      return res;
  }
  abort();
}

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

static FeObject* EvaluatePair(FeContext* ctx,
                              FeObject* obj,
                              FeObject* env,
                              FeObject** newenv,
                              const FeObject* fn) {
  FeObject* res = &nil;

  switch (FeGetType(fn)) {
    case FeTPrimitive:
      res = EvaluatePrimitive(ctx, obj, env, newenv, fn);
      break;

    case FeTPair:
    case FeTFree:
    case FeTNil:
    case FeTDouble:
    case FeTSymbol:
    case FeTString:
    case FeTPtr:
    case FeTFex0:
    case FeTFex1:
    case FeTFex2:
      HandleNonCallable(ctx, CAR(obj));

    // Ordinary callables and macros moved to frame kinds in
    // `DispatchResolvedCall` -- a lambda proceeds through `FeFrameLambda`
    // and `FeFrameBody`, a macro through `FeFrameMacro` and
    // `FeFrameMacroExpansion` -- so none can reach this temporary dispatch.
    case FeTFn:
    case FeTNativeFn:
    case FeTMacro:
    case FeTSentinel:
      abort();
  }

  return res;
}

// Dispatches a call whose head has already resolved to `fn`. `quote`
// short-circuits on its first raw argument. An ordinary callable (native
// function or lambda) switches the frame to `FeFrameCallArguments` -- the
// caller resumes the loop, and each argument is evaluated as a sub-expression
// frame rather than through a recursive `EvaluateList` -- and returns false.
// A macro switches the frame to `FeFrameMacro` -- its arguments stay raw and
// unevaluated, bound by `ArgsToEnv` the way the recursive arm bound them --
// and returns false. Everything else (the remaining primitives, and the
// non-callable values) keeps the temporary recursive dispatch and returns
// true with `*result` holding the completed value. Every resolved-call path
// funnels through here so the bookkeeping cannot drift between a symbol head
// and a computed head.
static bool DispatchResolvedCall(FeContext* ctx,
                                 FeEvalFrame* frame,
                                 FeObject* fn,
                                 FeObject** frame_bind,
                                 FeObject** result) {
  if (FeGetType(fn) == FeTPrimitive && PRIM(fn) == PQuote) {
    FeObject* arguments = CDR(frame->expr);
    *result = FeGetNextArgument(ctx, &arguments);
    return true;
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
  frame->kind = FeFrameTemporaryRecursive;
  *result = EvaluatePair(ctx, frame->expr, frame->env, frame_bind, fn);
  return true;
}

static void PushEvaluationFrame(FeContext* ctx,
                                FeObject* obj,
                                FeObject* env,
                                FeObject** bind) {
  const size_t reserve =
      ctx->completion == FeCompletionNormal ? 0 : CleanupFrameReserve;
  if (ctx->frame_stack_index == ctx->frame_stack_capacity + reserve) {
    FeHandleError(ctx, "evaluation depth limit exceeded");
  }
  FeEvalFrame* frame = &ctx->frame_stack[ctx->frame_stack_index++];
  *frame = (FeEvalFrame){.kind = FeFrameExpression,
                         .expr = obj,
                         .env = env,
                         .bind = bind,
                         .fn = &nil,
                         .rest = &nil,
                         .accumulator = &nil,
                         .callee = &nil};
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
      case FeFrameTemporaryRecursive:
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
// computed-head frame, an argument frame, a body or macro-body frame, and a
// macro frame waiting for its expansion's value. Anything else popping above
// one of these is an internal error.
static bool IsAwaitingDelivery(const FeEvalFrame* frame) {
  return frame->kind == FeFrameCallHead ||
         frame->kind == FeFrameCallArguments || frame->kind == FeFrameBody ||
         frame->kind == FeFrameMacro || frame->kind == FeFrameMacroExpansion;
}

// The ordinary-return tail of a pair form: drain the cleanups pushed while
// this form was being evaluated, restore its GC checkpoint, protect its
// result, and unlink its call-trace cell. Every pair form completes through
// here -- a symbol head, a resolved computed head, or a temporary recursive
// dispatch -- so the evaluation_depth and call_list bookkeeping (and the
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

      case FeFrameTemporaryRecursive:
        // Completed synchronously by the case that set it; it never reaches
        // the top of the loop again.
        abort();
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
