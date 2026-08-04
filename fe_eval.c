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
  // reset context state:
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

  // A fresh, bounded budget, not the exhausted or cancelled one the body
  // was running under and not no budget at all: see `RunCleanupsAfterError`.
  RunCleanupsAfterError(ctx, &cleanup_budget);

  if (ctx->error_fn) {
    ctx->error_fn(ctx, msg, cl);
  }
  abort();
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

// The head of a call is looked up here rather than through `Evaluate` so that
// an unassigned name is `void-function`, as in Emacs Lisp, even though Fe has
// one namespace and would otherwise say `void-variable`.
static FeObject* EvaluateHead(FeContext* ctx, FeObject* head, FeObject* env) {
  if (FeGetType(head) != FeTSymbol) {
    return Evaluate(ctx, head, env, NULL);
  }
  EvaluationStep(ctx);
  FeObject* value = CDR(GetBound(ctx, head, env));
  if (value == &unbound) {
    HandleNonCallable(ctx, head);
  }
  return value;
}

static FeObject* Evaluate(FeContext* ctx,
                          FeObject* obj,
                          FeObject* env,
                          FeObject** newenv) {
  EvaluationStep(ctx);
  if (FeGetType(obj) == FeTSymbol) {
    FeObject* value = CDR(GetBound(ctx, obj, env));
    if (value == &unbound) {
      HandleVoidSymbol(ctx, obj, "void-variable");
    }
    return value;
  }
  if (FeGetType(obj) != FeTPair) {
    return obj;
  }

  FeObject cl;
  CAR(&cl) = obj;
  CDR(&cl) = ctx->call_list;
  // This stack link is restored below or reset by FeHandleError before longjmp.
  // cppcheck-suppress autoVariables
  ctx->call_list = &cl;
  EnterEvaluationDepth(ctx);

  const size_t gc = FeSaveGC(ctx);
  // Every cleanup entry pushed while this call form is being evaluated --
  // by a nested `unwind-protect`, or by a native calling
  // `FeProtectWithCleanup` -- is drained back to this checkpoint below on an
  // ordinary return, the same way `gc` bounds the GC stack. An error drains
  // the whole registry instead, from `FeHandleError`, since nothing between
  // here and there runs on that path.
  const size_t cleanup = ctx->cleanup_stack_index;
  FeObject* fn = EvaluateHead(ctx, CAR(obj), env);
  FeObject* arg = CDR(obj);
  FeObject* res = &nil;
  FeObject* va;
  FeObject* vb;

  switch (FeGetType(fn)) {
    case FeTPrimitive:
      res = EvaluatePrimitive(ctx, obj, env, newenv, fn);
      break;

    case FeTNativeFn:
      res = GetNativeFn(fn)(ctx, EvaluateList(ctx, arg, env));
      break;

    case FeTFn:
      arg = EvaluateList(ctx, arg, env);
      va = CDR(fn);  // (env params ...)
      vb = CDR(va);  // (params ...)
      res = DoList(ctx, CDR(vb), ArgsToEnv(ctx, CAR(vb), arg, CAR(va)));
      break;

    case FeTMacro:
      va = CDR(fn);  // (env params ...)
      vb = CDR(va);  // (params ...)
      // Expand, then evaluate the expansion in the caller's environment. The
      // expansion is not copied over the call site: doing that cloned whatever
      // atom the macro returned, and `nil` and interned symbols are compared by
      // address.
      vb = DoList(ctx, CDR(vb), ArgsToEnv(ctx, CAR(vb), arg, CAR(va)));
      RunCleanupsDownTo(ctx, cleanup);
      FeRestoreGC(ctx, gc);
      FePushGC(ctx, vb);  // Nothing else refers to the expansion now.
      ctx->call_list = CDR(&cl);
      // Evaluating the expansion is a tail call in Fe but not in C: the
      // sanitizer lanes build with `-fno-optimize-sibling-calls`, and those
      // are exactly the builds where the C stack is the binding constraint,
      // so this frame is still live underneath. Hold the depth across the
      // call and drop it after, rather than before: decrementing first let
      // a macro whose expansion is another macro call recurse on the C
      // stack without the counter ever moving, and it crashed with a
      // MemorySanitizer stack-overflow instead of raising.
      res = Evaluate(ctx, vb, env, NULL);
      ctx->evaluation_depth--;
      return res;

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

    case FeTSentinel:
      abort();
  }

  RunCleanupsDownTo(ctx, cleanup);
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, res);
  ctx->call_list = CDR(&cl);
  ctx->evaluation_depth--;
  return res;
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
