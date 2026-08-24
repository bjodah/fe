// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

// The frame-driven evaluator: binding, the call and special-form dispatch,
// every frame kind's resume arm, and the evaluator's own GC roots. Split out
// of fe.c, which keeps the object model, garbage collector, reader and
// writer; fe_run.c holds the run driver and the public entry points, and
// fe_unwind.c the completion machinery -- evaluation control, the condition
// hierarchy, the cleanup registry and the raises. All four translation units
// share the private, self-contained fe_internal.h.
//
// Sub-plan 03E of the same set deleted the last recursive evaluation path:
// every special form and primitive is now a frame kind driven by
// `RunEvaluationLoop`, and `RunEvaluationBody` gives `unwind-protect`
// cleanups (and `if`/`while`/`do`'s implicit bodies) a body-frame entry
// point without a second evaluator. There is one evaluator, reached by one
// path, per the parent plan's requirement.

#include <assert.h>
#include <setjmp.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fe.h"
#include "fe_internal.h"
#include "fe_perf.h"

static FeObject* Bind(FeContext* ctx,
                      FeObject* env,
                      FeObject* name,
                      FeObject* value) {
  FE_PERF_INC(FePerfEnvBind);
  return FeCons(ctx, FeCons(ctx, name, value), env);
}

[[noreturn]] static void RaiseSettingConstant(FeContext* ctx,
                                              FeObject* symbol) {
  RaiseCondition(ctx, FeCompletionError, "setting-constant",
                 FeMakeList(ctx, (FeObject*[]){symbol}, 1), "setting-constant");
}

static FeObject* BindValue(FeContext* ctx,
                           FeObject* env,
                           FeObject* name,
                           FeObject* value) {
  if (FeIsNil(name) || IsConstantSymbol(ctx, name)) {
    RaiseSettingConstant(ctx, name);
  }
  CheckType(ctx, name, FeTSymbol);
  return Bind(ctx, env, name, value);
}

static FeObject* BindLambda(FeContext* ctx,
                            FeObject* env,
                            FeObject* name,
                            FeObject* value) {
  // Lexical Emacs lambdas permit `t` to shadow the global constant.
  if ((FeIsNil(name) || IsConstantSymbol(ctx, name)) &&
      !IsNamedSymbol(ctx, name, "t")) {
    RaiseSettingConstant(ctx, name);
  }
  return Bind(ctx, env, name, value);
}

static bool HasLexicalBinding(FeObject* env, const FeObject* name) {
  FE_PERF_INC(FePerfEnvLookup);
  while (!FeIsNil(env)) {
    FE_PERF_INC(FePerfEnvCell);
    const FeObject* cell = CAR(env);
    if (CAR(cell) == name) {
      return true;
    }
    env = CDR(env);
  }
  return false;
}

static void ValidateSetqTarget(FeContext* ctx,
                               FeObject* env,
                               FeObject* target) {
  if (FeIsNil(target) ||
      (IsConstantSymbol(ctx, target) &&
       !(IsNamedSymbol(ctx, target, "t") && HasLexicalBinding(env, target)))) {
    RaiseSettingConstant(ctx, target);
  }
  if (FeGetType(target) != FeTSymbol) {
    RaiseWrongType(ctx, "symbolp", target);
  }
}

static FeObject* SetEvaluatedValue(FeContext* ctx,
                                   FeObject* symbol,
                                   FeObject* value) {
  if (FeIsNil(symbol) || IsConstantSymbol(ctx, symbol)) {
    RaiseSettingConstant(ctx, symbol);
  }
  if (FeGetType(symbol) != FeTSymbol) {
    RaiseWrongType(ctx, "symbolp", symbol);
  }
  FeSet(ctx, symbol, value);
  return value;
}

static void ValidateValueTarget(FeContext* ctx, FeObject* target) {
  if (FeIsNil(target) || IsConstantSymbol(ctx, target)) {
    RaiseSettingConstant(ctx, target);
  }
  CheckType(ctx, target, FeTSymbol);
}

static void RejectConstantTarget(FeContext* ctx, FeObject* target) {
  if (FeIsNil(target) || IsConstantSymbol(ctx, target)) {
    RaiseSettingConstant(ctx, target);
  }
}

static FeObject* MakeClosure(FeContext* ctx,
                             FeObject* env,
                             FeObject* arguments,
                             FeType type);

static FeObject* LetBindingTarget(FeContext* ctx, FeObject* binding) {
  if (FeGetType(binding) == FeTSymbol || FeIsNil(binding)) {
    return binding;
  }
  if (FeGetType(binding) != FeTPair || FeIsNil(CDR(binding)) ||
      !FeIsNil(CDR(CDR(binding)))) {
    RaiseWrongType(ctx, "listp", binding);
  }
  return CAR(binding);
}

static FeObject* LetBindingValue(FeContext* ctx, FeObject* binding) {
  if (FeGetType(binding) == FeTSymbol || FeIsNil(binding)) {
    return &nil;
  }
  LetBindingTarget(ctx, binding);
  return CAR(CDR(binding));
}

static FeObject* ReverseList(FeObject* list) {
  FeObject* result = &nil;
  while (!FeIsNil(list)) {
    FeObject* next = CDR(list);
    CDR(list) = result;
    result = list;
    list = next;
  }
  return result;
}

// `let` with a binding list compiles into a lambda application, so a
// lambda-list keyword in binding position would reach the parameter decoder
// and be read as one: `(let ((&rest 1) (x 2)) x)` answered `(1 2)` -- one
// value bound to a rest parameter -- where Emacs answers 2, having bound a
// variable literally named `&rest`. Fe cannot bind that name here, so it says
// so instead of quietly meaning something else. Only the two names the
// decoder acts on are refused; `(let ((&foo 1)) &foo)` is 1 in both.
static void ValidateLetBindingTarget(FeContext* ctx, FeObject* target) {
  if (IsNamedSymbol(ctx, target, "&optional") ||
      IsNamedSymbol(ctx, target, "&rest")) {
    FeHandleError(ctx, "lambda-list keyword in let binding");
  }
  ValidateValueTarget(ctx, target);
}

static void ValidateLetBindings(FeContext* ctx, FeObject* bindings) {
  while (!FeIsNil(bindings)) {
    if (FeGetType(bindings) != FeTPair) {
      RaiseWrongType(ctx, "listp", bindings);
    }
    ValidateLetBindingTarget(ctx, LetBindingTarget(ctx, CAR(bindings)));
    bindings = CDR(bindings);
  }
}

// True when at least one of this binding list's targets is marked
// let-dynamic (sub-plan 11B). The answer decides which of two shapes the
// `let` takes, and it is asked here -- once, of a *binding list* -- rather
// than at each parameter binding, which is what keeps the A4 guard
// structural: `ArgsToEnv` never consults the flag, so a defun or lambda
// parameter named after a special is still bound lexically, as measured on
// Emacs 31.0.90 under `lexical-binding: t`.
static bool BindingsHaveDynamic(FeContext* ctx, FeObject* bindings) {
  for (FeObject* rest = bindings; !FeIsNil(rest); rest = CDR(rest)) {
    if (SymbolIsLetDynamic(ctx, LetBindingTarget(ctx, CAR(rest)))) {
      return true;
    }
  }
  return false;
}

// `FeGetNextArgument`'s contract, for a binding list: check the cell,
// advance past it, hand back the element. The dynamic `let` frame is the
// one binding walk that crosses evaluation boundaries -- a value form runs
// between one binding and the next -- so `ValidateLetBindings`' proof about
// the list's shape expires the moment the first value form starts, and a
// form that mutates the list it is a member of steers the rest of the walk
// wherever it likes. Raw `CAR`/`CDR` there dereferenced whatever word the
// mutation stored: `(setcdr blist 5)` inside the first value form made the
// walk read the integer 5 as an `FeObject*`, which is type confusion on a
// wholly caller-controlled value, not a bounded overflow.
//
// The lexical `let` path does not need this -- it builds parameters and
// values in one synchronous loop inside `StartBindingLet`, immediately
// after validation -- and `ResumeArguments`, the frame this one is modelled
// on, has had it all along, in `FeGetNextArgument`. A tail that is no longer
// a pair raises the `(wrong-type-argument listp X)` a literally improper
// binding list raises at validation time, so the mutated form and the
// written-out one answer with the same condition object.
static FeObject* NextLetBinding(FeContext* ctx, FeObject** rest) {
  FeObject* const cell = CheckType(ctx, *rest, FeTPair);
  *rest = CDR(cell);
  return CAR(cell);
}

// The frame's collected values, in binding order, become its bindings
// (sub-plan 11B): a let-dynamic target swaps the global value cell and
// pushes the restore obligation, any other target extends the environment
// the body will see. Every value form has already been evaluated, in the
// frame's entry environment, so this is `let` and not `let*`, and a target
// appearing twice binds twice -- the restores are LIFO, so the outer one
// wins on the way out, as in Emacs.
//
// Rooting: `frame->accumulator` holds the head of the reordered value list
// for the whole walk, so the values survive `BindValue`'s allocations, and
// each environment cell `Bind` makes is on the GC stack until the frame's
// own checkpoint is restored. The cleanup entries hold the shadowed globals
// and `MarkCleanupRoots` marks them.
//
// This is the frame's *second* walk of the source binding list, and every
// value form has run since the first one, so it is driven off the value
// list -- which the frame consed itself and nothing outside the evaluator
// can reach -- and pulls one target per value through `NextLetBinding`
// rather than walking the source list to its own end. A binding list a
// value form shortened, lengthened or made improper therefore raises
// `wrong-type-argument` here instead of pairing a value with whatever the
// mutation left in binding position.
static void InstallLetBindings(FeContext* ctx, FeEvalFrame* frame) {
  frame->accumulator = ReverseList(frame->accumulator);
  FeObject* values = frame->accumulator;
  FeObject* env = frame->env;
  FeObject* rest = CAR(frame->fn);
  while (!FeIsNil(values)) {
    FeObject* const target = LetBindingTarget(ctx, NextLetBinding(ctx, &rest));
    FeObject* const value = CAR(values);
    values = CDR(values);
    if (SymbolIsLetDynamic(ctx, target)) {
      PushDynamicBinding(ctx, target, value);
    } else {
      env = BindValue(ctx, env, target, value);
    }
  }
  // The frame becomes an ordinary sequential body: `env` is what the body
  // forms see, `rest` the raw forms, and `accumulator` the "last completed
  // value" a body with no forms at all completes with (nil, matching
  // `(let ((a 1)))`). Its completion runs `CompletePairFrame`, whose
  // `RunCleanupsDownTo(frame->cleanup_checkpoint)` -- the checkpoint this
  // frame took while it was still `FeFrameExpression`, below every entry
  // pushed above -- is what undoes the dynamic bindings on a normal return.
  frame->kind = FeFrameBody;
  frame->env = env;
  frame->rest = CDR(frame->fn);
  frame->accumulator = &nil;
  frame->callee = &unbound;
}

// One `FeFrameDynamicLet` step: collect the delivered value, start the next
// binding's value form, or -- once every one is in -- install the bindings.
// Deliberately the same shape as `ResumeArguments`, including the GC-stack
// restore per operand (the accumulator is a frame field and therefore a
// mark-phase root already, so a long binding list costs arena, not root
// slots) and including the checked advance: `ResumeArguments` reads its next
// operand through `FeGetNextArgument`, which is where its type check lives,
// and this reads its next binding through `NextLetBinding` for the same
// reason and against the same input -- a form that mutates the list being
// walked while the walk is suspended.
static bool ResumeDynamicLet(FeContext* ctx, FeEvalFrame* frame) {
  if (frame->callee != &unbound) {
    frame->accumulator = FeCons(ctx, frame->callee, frame->accumulator);
    frame->callee = &unbound;
  }
  if (!FeIsNil(frame->rest)) {
    EvaluationStep(ctx);
    FeRestoreGC(ctx, frame->gc_checkpoint);
    PushEvaluationFrame(ctx,
                        LetBindingValue(ctx, NextLetBinding(ctx, &frame->rest)),
                        frame->env, NULL);
    return false;
  }
  InstallLetBindings(ctx, frame);
  return false;
}

static void StartBindingLet(FeContext* ctx,
                            FeEvalFrame* frame,
                            FeObject* bindings,
                            FeObject* body) {
  FeObject* parameters = &nil;
  FeObject* values = &nil;
  ValidateLetBindings(ctx, bindings);
  if (BindingsHaveDynamic(ctx, bindings)) {
    // At least one target binds dynamically, so the lambda-application
    // desugaring below cannot be used for this form: a lambda parameter is
    // a lexical environment entry, and a lexical entry for a special name
    // would shadow the global cell that `setq` and every free reference
    // read (A2a). `fn` carries `(BINDINGS . BODY)`, both fixed.
    frame->kind = FeFrameDynamicLet;
    frame->fn = FeCons(ctx, bindings, body);
    frame->rest = bindings;
    frame->accumulator = &nil;
    frame->callee = &unbound;
    return;
  }
  for (FeObject* rest = bindings; !FeIsNil(rest); rest = CDR(rest)) {
    FeObject* binding = CAR(rest);
    parameters = FeCons(ctx, LetBindingTarget(ctx, binding), parameters);
    values = FeCons(ctx, LetBindingValue(ctx, binding), values);
  }
  parameters = ReverseList(parameters);
  values = ReverseList(values);
  FeObject* lambda_arguments = FeCons(ctx, parameters, body);
  FeObject* closure = MakeClosure(ctx, frame->env, lambda_arguments, FeTFn);
  frame->kind = FeFrameCallArguments;
  frame->fn = closure;
  frame->rest = values;
  frame->accumulator = &nil;
  frame->callee = &unbound;
}

static FeObject* CallIdentity(FeObject* expr, FeObject* fn);
static size_t CountRawArguments(FeContext* ctx, FeObject* arguments);

static bool DispatchLet(FeContext* ctx,
                        FeEvalFrame* frame,
                        FeObject* fn,
                        FeObject** frame_bind,
                        FeObject* arguments,
                        FeObject** result) {
  FeObject* const target = FeGetNextArgument(ctx, &arguments);
  if (FeGetType(target) == FeTPair || FeIsNil(target)) {
    StartBindingLet(ctx, frame, target, arguments);
    return false;
  }
  const size_t count = CountRawArguments(ctx, CDR(frame->expr));
  if (count != 2) {
    RaiseWrongNumber(ctx, CallIdentity(frame->expr, fn), count);
  }
  ValidateValueTarget(ctx, target);
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

static bool IsParameterName(const FeObject* name) {
  return FeIsNil(name) || FeGetType(name) == FeTSymbol;
}

static void ValidateParameterName(FeContext* ctx,
                                  const FeObject* name,
                                  bool optional) {
  if (!IsParameterName(name) ||
      (optional && IsNamedSymbol(ctx, name, "&optional"))) {
    RaiseNamedError(ctx, "invalid-function", "invalid-function");
  }
}

static void ValidateParameters(FeContext* ctx, FeObject* prm) {
  bool optional = false;
  while (FeGetType(prm) == FeTPair) {
    const FeObject* name = CAR(prm);
    prm = CDR(prm);
    ValidateParameterName(ctx, name, optional);
    if (IsNamedSymbol(ctx, name, "&optional")) {
      optional = true;
      continue;
    }
    if (IsNamedSymbol(ctx, name, "&rest")) {
      if (FeGetType(prm) != FeTPair || FeGetType(CAR(prm)) != FeTSymbol ||
          !FeIsNil(CDR(prm))) {
        RaiseNamedError(ctx, "invalid-function", "invalid-function");
      }
      return;
    }
  }
  if (!FeIsNil(prm) && FeGetType(prm) != FeTSymbol) {
    RaiseNamedError(ctx, "invalid-function", "invalid-function");
  }
}

// Binds a lambda or macro parameter list to an argument list. Three spellings
// collect the remaining arguments: Fe's dotted tail `(a . r)`, Fe's bare symbol
// `r`, and Emacs Lisp's `(a &rest r)`. Bare symbols and dotted tails are Fe's
// deliberate variadic spellings; proper lambda lists are otherwise strict.
//
// A rest parameter is bound to the argument list itself, never to a copy of
// it. For an ordinary call that list is the freshly consed evaluated-operand
// list `ResumeArguments` just built, so 07A row L9's per-call freshness holds
// without copying anything; for a macro it is the caller's raw source tail,
// which is what Emacs binds too. The copy this used to make cost two conses
// per argument, each one a `FePushGC` on the caller's frame, and overflowed
// the 4096-slot root stack at roughly 1400 arguments.
static FeObject* ArgsToEnv(FeContext* ctx,
                           FeObject* prm,
                           FeObject* arg,
                           FeObject* env,
                           FeObject* function,
                           size_t argc) {
  bool optional = false;
  ValidateParameters(ctx, prm);
  while (!FeIsNil(prm)) {
    EvaluationStep(ctx);
    if (FeGetType(prm) != FeTPair) {
      return BindLambda(ctx, env, prm, arg);
    }
    FeObject* name = CAR(prm);
    prm = CDR(prm);
    if (IsNamedSymbol(ctx, name, "&optional")) {
      optional = true;
      continue;
    }
    if (IsNamedSymbol(ctx, name, "&rest")) {
      if (FeGetType(prm) != FeTPair) {
        RaiseNamedError(ctx, "invalid-function", "invalid-function");
      }
      if (!FeIsNil(CDR(prm))) {
        RaiseNamedError(ctx, "invalid-function", "invalid-function");
      }
      return BindLambda(ctx, env, CAR(prm), arg);
    }
    if (!optional && FeIsNil(arg)) {
      RaiseWrongNumber(ctx, function, argc);
    }
    env = BindLambda(ctx, env, name, FeIsNil(arg) ? &nil : FeCar(ctx, arg));
    if (!FeIsNil(arg)) {
      arg = FeCdr(ctx, arg);
    }
  }
  if (!FeIsNil(arg)) {
    RaiseWrongNumber(ctx, function, argc);
  }
  return env;
}

// The numeric tower's one promotion rule (sub-plan 05C of kg's Emacs-subset
// program, "A numeric-pair helper, once"): two numeric operands become both
// int64_t when they are both integers, both double otherwise -- integer
// arithmetic stays integer, and any double promotes the rest of the
// reduction. Every binary arithmetic and comparison site dispatches through
// here, so the rule is written once and cannot rot. A non-number is the
// Emacs `wrong-type-argument` (05A Decision 5, row C5), which retires the
// old "expected double, got X" texts on the numeric family only; `CheckType`'s
// generic message survives everywhere else.
typedef struct NumericPair {
  bool is_integer;
  int64_t a_i, b_i;
  double a_d, b_d;
} NumericPair;

static NumericPair GetNumericPair(FeContext* ctx, FeObject* a, FeObject* b) {
  const FeType a_type = FeGetType(a);
  const FeType b_type = FeGetType(b);
  if (a_type == FeTInteger && b_type == FeTInteger) {
    return (NumericPair){
        .is_integer = true, .a_i = INTEGER(a), .b_i = INTEGER(b)};
  }
  if ((a_type == FeTInteger || a_type == FeTDouble) &&
      (b_type == FeTInteger || b_type == FeTDouble)) {
    // `FeToDouble` widens an integer to a double, so the mixed promotion
    // loses no code path of its own.
    return (NumericPair){.is_integer = false,
                         .a_d = FeToDouble(ctx, a),
                         .b_d = FeToDouble(ctx, b)};
  }
  RaiseWrongType(ctx, "number-or-marker-p",
                 a_type == FeTInteger || a_type == FeTDouble ? b : a);
}

// `=` (`PNumericEqual` in `ResumeEvalList`): numeric equality over both
// numeric types, chained left to right, Emacs' ordinary-function
// semantics -- the complete raw argument list is evaluated left to right
// before any value is type-checked, so a type error in an early operand
// never erases a side effect a later operand's form already had -- then
// adjacent pairs are compared left to right and the loop stops at the first
// false pair, Emacs' rule, so an operand past a settled answer is not
// type-checked (its form has run either way) -- and one argument is `t`
// without comparing or checking anything. The comparison itself is 05C's
// tower: exact
// within integers, mathematical value across int/float through
// `GetNumericPair`, and -- for double/double -- plain C `==`, which gives
// the pinned signed-zero (`0.0 = -0.0` is true) and NaN (never `=` to
// itself) answers; -Wfloat-equal is suppressed for this intentional exact
// comparison, the same way `Equal()`'s `IsNearlyEqual()` helper above does
// for its own `a == b` infinity special case. See doc/language.md.

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
  RaiseCondition(ctx, FeCompletionError, kind,
                 FeMakeList(ctx, (FeObject*[]){symbol}, 1), message);
}

// `(foo 1)` where `foo` has no function value names the culprit, the way
// Emacs Lisp's `void-function` does; anything else is anonymous.
[[noreturn]] static void HandleNonCallable(FeContext* ctx, FeObject* callee) {
  if (FeGetType(callee) == FeTSymbol) {
    HandleSymbolError(ctx, callee, "void-function");
  }
  FeHandleError(ctx, "tried to call non-callable value");
}

// A cycle in a function-designator chain, reported the way the reader that
// found it asked for: an error for a reader inside a catchable evaluation
// (`cycle` null), and `&unbound` plus a flag for one that is not.
static FeObject* ReportFunctionCycle(FeContext* ctx, bool* cycle) {
  if (cycle == nullptr) {
    RaiseNamedError(ctx, "cyclic-function-indirection",
                    "cyclic-function-indirection");
  }
  *cycle = true;
  return &unbound;
}

// Sub-plan 04C/04D's shared function-designator resolver: a function cell may
// hold another symbol (the `defalias` indirection), and every reader of the
// chain -- call position, `funcall`/`apply`, `FeGetFunction` -- follows it
// through here so the rule cannot drift between the sites. One step is
// charged per symbol hop, so a chain that never ends still dies on the step
// budget, and a cycle is detected by two-pointer walk rather than left to
// exhaust the budget (`(fset 'x 'x)` is the canonical case). Since 04D's cut
// the chain dies in an empty function cell with `&unbound` -- the
// transitional value-cell fallback is gone, so a callable stored only in the
// value namespace is *not* reachable in call position. An empty cell anywhere
// along the chain is reported as plain `&unbound`, and every caller names the
// symbol *it* was given rather than the last link reached: `(fset 'a 'b) (a)`
// and `(funcall 'a)` are both `void-function a` in Emacs, the name the program
// wrote. A non-symbol value is already resolved and is returned unchanged.
//
// `cycle` is how the caller asks to be *told* about a cycle instead of
// erroring on one. A null `cycle` raises `cyclic-function-indirection`, which
// is what every evaluator-side reader wants: call position and the
// `funcall`/`apply` arm are inside an evaluation the host can catch. A
// non-null `cycle` is set to true and `&unbound` returned instead, for the
// host-facing `FeGetFunction`, which may be called with no evaluation running
// at all and so has no frame to raise into.
FeObject* ResolveFunctionCallable(FeContext* ctx, FeObject* fn, bool* cycle) {
  FE_PERF_INC(FePerfFunctionResolve);
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
    // One `defalias` link followed. The two-pointer cycle detector below
    // reads function cells of its own; those are deliberately not counted,
    // so this is the length of the chain walked and not the memory traffic.
    FE_PERF_INC(FePerfFunctionHop);
    slow = cell;
    if (FeGetType(fast) == FeTSymbol) {
      FeObject* const f1 = SymbolFunction(fast);
      if (f1 != &unbound && FeGetType(f1) == FeTSymbol) {
        FeObject* const f2 = SymbolFunction(f1);
        if (f2 == slow) {
          return ReportFunctionCycle(ctx, cycle);
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
  return ResolveFunctionCallable(ctx, head, nullptr);
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

// The identity a `(FUNCTION NARGS)` condition names, per 07A Decision 2: the
// symbol the program actually wrote when the head is one, and the resolved
// callable otherwise -- a computed head, or a host `FeCall*` entry. One
// helper rather than the same conditional spelled out at each call site,
// which is how the rule stays the same rule everywhere.
static FeObject* CallIdentity(FeObject* expr, FeObject* fn) {
  FeObject* const head = CAR(expr);
  return FeGetType(head) == FeTSymbol ? head : fn;
}

static bool DispatchPrimitive(FeContext* ctx,
                              FeEvalFrame* frame,
                              FeObject* fn,
                              FeObject** frame_bind,
                              FeObject** result);
static void PreflightPrimitive(FeContext* ctx,
                               Primitive primitive,
                               FeObject* function,
                               FeObject* arguments);

// Binds a macro call's raw, unevaluated arguments into the transformer's
// closure environment and switches `frame` to the body-evaluating
// `FeFrameMacro` kind. This is the *only* place a macro transformer is
// applied: `DispatchResolvedCall` reaches it for a macro call in evaluated
// position, and `MacroexpandStep` (sub-plan 10B) reaches it for a reflective
// `macroexpand-1`/`macroexpand`, so strict arity, environment capture and
// step charging cannot drift between the two.
//
// `call` is the macro call form -- `frame->expr` for an ordinary call, the
// evaluated FORM operand for a reflective expansion, which is why it is a
// parameter rather than read off the frame. `identity` is what a
// `wrong-number-of-arguments` from `ArgsToEnv` names. `caller_env` is what
// `frame->fn` holds for the rest of the frame's life: the environment the
// expansion is evaluated in for an ordinary call, or -- for a reflective
// expansion, which never evaluates the expansion at all -- the resolved
// `macroexpand-1`/`macroexpand` primitive object, which `ResumeMacroBody`
// tells apart by type. Both are collector roots, so both are safe there.
static void EnterMacroBody(FeContext* ctx,
                           FeEvalFrame* frame,
                           FeObject* fn,
                           FeObject* call,
                           FeObject* identity,
                           FeObject* caller_env) {
  FE_PERF_INC(FePerfMacroExpansion);
  frame->kind = FeFrameMacro;
  // Root the callable while `ArgsToEnv` below allocates (a collection may
  // run inside it): `frame->fn` is a collector root. It is then overwritten
  // with `caller_env` -- the body otherwise replaces `frame->env` with the
  // argument bindings over the macro's closure environment.
  frame->fn = fn;
  frame->accumulator = &nil;
  frame->callee = &unbound;
  FeObject* va = CDR(fn);  // (env params ...)
  FeObject* vb = CDR(va);  // (params ...)
  // The raw, unevaluated arguments are bound by the same allocating,
  // never-evaluating helper, charging one step per parameter walk exactly
  // as the recursive arm did.
  const size_t argc = CountRawArguments(ctx, CDR(call));
  frame->env = ArgsToEnv(ctx, CAR(vb), CDR(call), CAR(va), identity, argc);
  frame->rest = CDR(vb);
  frame->fn = caller_env;
}

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
    FE_PERF_INC(FePerfDispatchPrimitive);
    PreflightPrimitive(ctx, (Primitive)(unsigned char)PRIM(fn),
                       CallIdentity(frame->expr, fn), CDR(frame->expr));
    if (PRIM(fn) == PQuote) {
      FeObject* arguments = CDR(frame->expr);
      *result = FeGetNextArgument(ctx, &arguments);
      return true;
    }
    return DispatchPrimitive(ctx, frame, fn, frame_bind, result);
  }
  if (FeGetType(fn) == FeTNativeFn || FeGetType(fn) == FeTFn) {
    // The family the two share: an ordinary callable whose arguments are
    // about to be evaluated. Which of the two it is becomes a
    // `dispatch_native` or a `dispatch_lambda` once that list is complete.
    FE_PERF_INC(FePerfDispatchCallable);
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
    FE_PERF_INC(FePerfDispatchMacro);
    EnterMacroBody(ctx, frame, fn, frame->expr, CallIdentity(frame->expr, fn),
                   frame->env);
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
// `RaiseCompletion` before `RunCleanupsAfterError` runs); by then
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
    // The frame wall is a Budget completion (06B), grouped with the step and
    // re-entry ceilings as host-configured resource exhaustion. Assigning
    // Budget here -- instead of the Error the old shared call did -- is what
    // makes the reserve above load-bearing for a frame-wall drain: this raise
    // sets `completion` before the drain runs, so the cleanup's own pushes
    // are not refused by the same wall the body just hit.
    RaiseBudget(ctx, "evaluation frame limit exceeded");
  }
  FE_PERF_INC(FePerfFramePush);
  FeEvalFrame* const frame = &ctx->frame_stack[ctx->frame_stack_index++];
  if (ctx->frame_stack_index > ctx->arena_peak_frame_depth) {
    ctx->arena_peak_frame_depth = ctx->frame_stack_index;
  }
  return frame;
}

void PushEvaluationFrame(FeContext* ctx,
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
void PushBodyFrame(FeContext* ctx, FeObject* env, FeObject* forms) {
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
// before any evaluation (`PLet`'s `frame_bind == NULL` case -- see
// `FeFrameLet`'s own comment in fe_internal.h for why the value form is then
// never even evaluated). Returns true with `*result` holding
// the completed value, or false after switching `frame`'s kind to one of the
// resumable continuation kinds `ResumeContinuation` drives below. This is
// the frame-machine replacement for the old recursive `EvaluatePrimitive`;
// see 03E's spec (`doc/plans/2026-08-03-elisp-subset-and-fe-evaluator-
// subplans/03e-special-form-frames-and-unwind.md` in kg) for the table of
// which primitives share which evaluation-order rules and why a single
// generic "evaluate every argument first" policy is not an equivalent
// replacement for most of them.
typedef struct PrimitiveArity {
  size_t minimum;
  size_t maximum;
} PrimitiveArity;

// 07A's inventory, one row per `Primitive`. Two rows are worth naming because
// they are easy to over-tighten:
//
// * `lambda`/`fn`/`macro` need a parameter list and nothing else -- 07A's
//   "closure constructors with a raw parameter-list minimum". `(lambda (x))`
//   is a valid closure in Emacs and `((lambda (x)) 1)` answers nil; requiring
//   a body would reject a form Emacs accepts.
// * `apply`, like `funcall`, is 1+ (07A's census). `(apply f)` has a callable
//   but no final list, and Emacs diagnoses that as a *type* error about the
//   missing list -- `(apply #'list)` is `(wrong-type-argument listp list)` --
//   not as an arity error. Fe's own proper-list check is what should speak
//   there, so the table must let one operand through.

static const PrimitiveArity primitive_arities[PSentinel] = {
    [PAssert] = {1, 1},
    [PEnv] = {0, 0},
    [PLet] = {1, SIZE_MAX},
    [PNumericEqual] = {1, SIZE_MAX},
    [PSetq] = {0, SIZE_MAX},
    [PSet] = {2, 2},
    [PIf] = {2, SIZE_MAX},
    [PFn] = {1, SIZE_MAX},
    [PMacro] = {1, SIZE_MAX},
    [PWhile] = {1, SIZE_MAX},
    [PQuote] = {1, 1},
    [PBoundp] = {1, 1},
    [PMakeUnbound] = {1, 1},
    [PAnd] = {0, SIZE_MAX},
    [POr] = {0, SIZE_MAX},
    [PDo] = {0, SIZE_MAX},
    [PUnwindProtect] = {1, SIZE_MAX},
    [PCons] = {2, 2},
    [PCar] = {1, 1},
    [PCdr] = {1, 1},
    [PSetCar] = {2, 2},
    [PSetCdr] = {2, 2},
    [PList] = {0, SIZE_MAX},
    [PNot] = {1, 1},
    [PIs] = {2, 2},
    [PEq] = {2, 2},
    [PEql] = {2, 2},
    [PAtom] = {1, 1},
    [PPrint] = {1, SIZE_MAX},
    [PLess] = {1, SIZE_MAX},
    [PLessEqual] = {1, SIZE_MAX},
    [PGreater] = {1, SIZE_MAX},
    [PGreaterEqual] = {1, SIZE_MAX},
    [PNotEqual] = {2, 2},
    [PIntegerp] = {1, 1},
    [PFloatp] = {1, 1},
    [PKeywordp] = {1, 1},
    [PAdd] = {0, SIZE_MAX},
    [PSub] = {0, SIZE_MAX},
    [PMul] = {0, SIZE_MAX},
    [PDiv] = {1, SIZE_MAX},
    [PFunction] = {1, 1},
    [PFset] = {2, 2},
    [PDefalias] = {2, 2},
    [PSymbolFunction] = {1, 1},
    [PSymbolValue] = {1, 1},
    [PFboundp] = {1, 1},
    [PFmakunbound] = {1, 1},
    [PFuncall] = {1, SIZE_MAX},
    [PApply] = {1, SIZE_MAX},
    [PCatch] = {1, SIZE_MAX},
    [PThrow] = {2, 2},
    [PConditionCase] = {2, SIZE_MAX},
    [PSignal] = {1, 2},
    [PError] = {1, SIZE_MAX},
    // `macroexpand-1`/`macroexpand` are `(FORM &optional ENVIRONMENT)`,
    // measured on the pinned Emacs: `(macroexpand-1)` and
    // `(macroexpand-1 '(a) nil 'extra)` are both
    // `wrong-number-of-arguments`. `macroexpand-all` carries the same row
    // so that a well-formed call reaches its own by-name rejection rather
    // than an arity error about a function nobody can use anyway.
    [PMacroexpand1] = {1, 2},
    [PMacroexpand] = {1, 2},
    [PMacroexpandAll] = {1, 2},
    // Sub-plan 11B: `internal--mark-special` is SYMBOL plus a FULL-P flag,
    // exactly two, because a one-argument spelling would have to guess which
    // of the two `defvar` arities called it. `special-variable-p` is Emacs'
    // own arity, exactly one.
    [PMarkSpecial] = {2, 2},
    [PSpecialVariableP] = {1, 1},
    // Phase 19: `(error-message-string ERROR)`, Emacs' own arity -- measured
    // on the pinned 31.0.90, `(error-message-string)` and
    // `(error-message-string '(error "a") 'extra)` are both
    // `wrong-number-of-arguments`.
    [PErrorMessageString] = {1, 1},
    // Sub-plan 12B: `(eval FORM &optional LEXICAL)`, Emacs' own arity --
    // measured on the pinned 31.0.90, `(eval)` is
    // `(wrong-number-of-arguments eval 0)` and `(eval '(+ 1 2) nil 'extra)`
    // is `(wrong-number-of-arguments eval 3)`. A non-nil LEXICAL is a
    // by-name rejection in the arm, not an arity error.
    [PEval] = {1, 2},
    // Phase 14's symbol family. Emacs' `intern` and `intern-soft` take an
    // optional OBARRAY; fe has exactly one obarray and no way to name a
    // second, so a second operand is `wrong-number-of-arguments` here and a
    // recorded divergence rather than an argument that is accepted and
    // ignored. `gensym`'s PREFIX is optional, as in Emacs.
    [PIntern] = {1, 1},
    [PInternSoft] = {1, 1},
    [PSymbolName] = {1, 1},
    [PMakeSymbol] = {1, 1},
    [PGensym] = {0, 1},
    [PPut] = {3, 3},
    [PGet] = {2, 2},
    [PSymbolPlist] = {1, 1},
    [PDefineError] = {2, 3},
    // Phase 20: `string<`/`string>` are strictly binary, Emacs' own arity --
    // measured on the pinned 31.0.90, `(string<)`, `(string< "a")` and
    // `(string< "a" "b" "c")` are all `wrong-number-of-arguments`.
    [PStringLess] = {2, 2},
    [PStringGreater] = {2, 2},
    // Phase 24's vector family, every row Emacs' own arity measured on the
    // pinned 31.0.91. `vector` and `vconcat` take any number, zero included
    // (`(vector)` is `[]` and `(vconcat)` is `[]`); `make-vector` takes
    // LENGTH and INIT, both required, so `(make-vector 3)` is
    // `wrong-number-of-arguments` rather than a vector of nils; `aset` is
    // the only ternary primitive in this table.
    [PVector] = {0, SIZE_MAX},
    [PMakeVector] = {2, 2},
    [PVectorp] = {1, 1},
    [PAref] = {2, 2},
    [PAset] = {3, 3},
    [PVconcat] = {0, SIZE_MAX},
    [PLength] = {1, 1},
    [PElt] = {2, 2},
};

// An improper argument list has no argument *count*, so it is not an arity
// question at all: `(car 1 . 2)` is `(wrong-type-argument listp 2)` in Emacs,
// naming the tail that is not a list, and was `wrong-type-argument listp`
// here before Phase 7 too. Reporting it as `wrong-number-of-arguments` with
// a nil FUNCTION -- which is what a partially walked list can honestly say
// about the call -- claimed an identity and a count that mean nothing.
static size_t CountRawArguments(FeContext* ctx, FeObject* arguments) {
  size_t count = 0;
  while (!FeIsNil(arguments)) {
    if (FeGetType(arguments) != FeTPair) {
      RaiseWrongType(ctx, "listp", arguments);
    }
    count++;
    arguments = CDR(arguments);
  }
  return count;
}

// One rule for every primitive: the table decides, and a violation is
// `wrong-number-of-arguments` carrying 07A Decision 2's `(FUNCTION NARGS)`.
//
// This deliberately has no exception list. The nine primitives that once kept
// the prose "too few arguments"/"too many arguments" here were exactly the
// nine whose pre-Phase-7 assertions would otherwise have had to change --
// which is a record of the order the work was done in, not a policy anyone
// could state. Those messages are fe-only prose that no oracle row pins, so
// the assertions moved instead. Decision 2's byte-identical native messages
// are a different contract and live where they belong, in
// `FeGetNextArgument`/`FeRequireNoArguments`: a host native declares no
// arity, so its helper-driven checks stay post-evaluation and keep their own
// text.
static void PreflightPrimitive(FeContext* ctx,
                               Primitive primitive,
                               FeObject* function,
                               FeObject* arguments) {
  const size_t count = CountRawArguments(ctx, arguments);
  const PrimitiveArity arity = primitive_arities[primitive];
  if (count < arity.minimum || count > arity.maximum) {
    RaiseWrongNumber(ctx, function, count);
  }
}

static bool DispatchPrimitive(FeContext* ctx,
                              FeEvalFrame* frame,
                              FeObject* fn,
                              FeObject** frame_bind,
                              FeObject** result) {
  // No preflight here: `DispatchResolvedCall`, the only caller, has already
  // run it (and needs it there anyway, before `quote`'s fast path). Running
  // it a second time walked every primitive call's raw argument list twice --
  // about 4% of `scripts/mandelbrot.fe` -- for an answer that cannot have
  // changed in between.
  FeObject* arguments = CDR(frame->expr);
  // Phase 14's symbol family and Phase 24's vector family, routed as one
  // each: sixteen ordinary functions that share the whole setup below and
  // finish in fe.c's `EvaluateSymbolPrimitive`/`EvaluateVectorPrimitive`,
  // beside the storage each family reads. Two range tests rather than
  // sixteen `case` labels here and sixteen more in `ResumeEvalList` -- see
  // the `PIntern` and `PVector` blocks in fe_internal.h.
  if (IsSymbolPrimitive((Primitive)PRIM(fn)) ||
      IsVectorPrimitive((Primitive)PRIM(fn))) {
    frame->kind = FeFrameEvalList;
    frame->fn = fn;
    frame->rest = arguments;
    frame->accumulator = &nil;
    frame->callee = &unbound;
    return false;
  }
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
      if (FeGetType(form) == FeTPair &&
          (IsNamedSymbol(ctx, CAR(form), "lambda") ||
           IsNamedSymbol(ctx, CAR(form), "fn"))) {
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
    case PLet:
      return DispatchLet(ctx, frame, fn, frame_bind, arguments, result);
    // `(if COND THEN ELSE...)`, as in Emacs Lisp: the trailing forms are an
    // implicit body. The condition and the consequent are both required, and
    // the arity table has already refused `(if)` and `(if COND)`.
    case PIf:
      frame->kind = FeFrameIf;
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    // `while`: the condition form is fixed for the frame's whole lifetime
    // (held in `fn`, unused for anything else by this kind); the arity table
    // has already refused a `(while)` with no condition at all.
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
    // `(catch TAG BODY...)` (sub-plan 06C): a special form -- the BODY forms
    // stay raw, evaluated as an implicit sequential body once the TAG
    // delivers. The frame becomes `FeFrameCatch`; `rest` holds the raw TAG
    // and BODY forms, `accumulator` the delivered tag (`&unbound` until
    // then). A bare `(catch)` has no tag to catch anything with and is
    // `wrong-number-of-arguments` before anything evaluates, matching Emacs;
    // the arity table's `{1, SIZE_MAX}` row is what says so.
    case PCatch:
      frame->kind = FeFrameCatch;
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    // `(throw TAG VALUE)` (sub-plan 06C): a function whose two arguments
    // evaluate normally, exact-two-argument raw arity checked before anything
    // evaluates exactly as `set`/`/=` check theirs -- a wrong count is
    // `wrong-number-of-arguments`, never a chain or a dropped operand. The
    // evaluated operands dispatch the mid-stack unwind from
    // `ResumeEvalList`'s PThrow arm.
    case PThrow:
    case PSignal:
    case PError:
      frame->kind = FeFrameEvalList;
      frame->fn = fn;
      frame->rest = arguments;
      frame->accumulator = &nil;
      frame->callee = &unbound;
      return false;
    case PConditionCase: {
      FeObject* variable = FeGetNextArgument(ctx, &arguments);
      if (!FeIsNil(variable) && FeGetType(variable) != FeTSymbol) {
        RaiseCondition(ctx, FeCompletionError, "wrong-type-argument", &nil,
                       "wrong-type-argument");
      }
      FeObject* body = FeGetNextArgument(ctx, &arguments);
      ValidateConditionHandlers(ctx, arguments);
      frame->kind = FeFrameConditionCase;
      frame->fn = &nil;
      frame->accumulator = variable;
      frame->rest = arguments;
      frame->callee = &unbound;
      PushEvaluationFrame(ctx, body, frame->env, NULL);
      return false;
    }
    case PSetq:
      frame->kind = FeFrameSetq;
      // Kept for `RaiseSetqFormCount`'s identity fallback when the head was a
      // computed form rather than the symbol `setq`.
      frame->fn = fn;
      frame->rest = arguments;
      frame->accumulator = &unbound;
      frame->callee = &unbound;
      return false;
    // Every remaining function-shaped primitive: `FeFrameEvalList` evaluates
    // the whole raw operand list as a batch and the arm in `ResumeEvalList`
    // does the rest. The arity contracts differ from label to label and each
    // is noted below, but the arity table has already enforced all of them
    // before this point, so the setup itself is one body rather than six
    // copies of the same six assignments.
    //
    // `set`: exact two-argument raw arity is rejected before either side
    // effect can run, matching the recursive arm; the two raw forms are then
    // evaluated left to right by this same machinery an ordinary call's
    // argument list also uses.
    case PSet:
    // `=` and the chained comparators `<`/`<=`/`>`/`>=` (05C): zero raw
    // arguments is rejected before anything evaluates; one or more are
    // evaluated as a batch before any operand is type-checked or compared.
    case PNumericEqual:
    case PLess:
    case PLessEqual:
    case PGreater:
    case PGreaterEqual:
    // `(/= a b)` is strictly binary (05A row C3, confirmed by the pinned
    // Emacs: `(/= 5)` is `wrong-number-of-arguments` too), so its raw arity
    // -- exactly two operands -- is checked before anything evaluates. A
    // third operand is `wrong-number-of-arguments`, not a chain.
    case PNotEqual:
    case PList:
    // `funcall`/`apply` (sub-plan 04C) are function-shaped special forms:
    // they evaluate every operand here, then dispatch the first result
    // through the designator resolver -- the redispatch shape, chosen over a
    // dedicated apply frame kind because it adds no frame-kind and no new
    // GC-per-state row while costing only the few conses `MakeCallForm`
    // builds (see `ResumeEvalList`'s `PFuncall`/`PApply` arm and
    // doc/implementation.md's note). Zero raw operands has no callable to
    // dispatch, so `(funcall)`/`(apply)` are an arity error before anything
    // evaluates -- the arity table's `{1, SIZE_MAX}` row.
    case PFuncall:
    case PApply:
    // `macroexpand-1`/`macroexpand` (sub-plan 10B) are ordinary functions in
    // Emacs and here: FORM is an evaluated operand, which is why
    // `(macroexpand '(when t 1))` needs the quote. The expansion itself
    // happens in `ResumeEvalList`'s arm below, once both operands are known.
    case PMacroexpand1:
    case PMacroexpand:
    // `macroexpand-all` is a rejected ordinary function, not a special form:
    // evaluate its operands before the named rejection below.  This also
    // keeps it reachable through `funcall`/`apply`, whose redispatch refuses
    // raw-form callables.
    case PMacroexpandAll:
    // `eval` (sub-plan 12B) is function-shaped for the same reason: FORM and
    // the optional LEXICAL are evaluated operands, and the arm below relays
    // the resulting form into this same run.
    case PEval:
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
    case PIntegerp:
    case PFloatp:
    case PKeywordp:
    // `error-message-string` (Phase 19): arity-exact, one evaluated operand,
    // and no evaluator state touched -- the unary frame is the whole of its
    // routing.
    case PErrorMessageString:
    // `special-variable-p` (11B): arity-exact like `boundp` above, and it
    // answers the *special* flag alone, so a symbol marked by a one-arg
    // `defvar` answers nil while still binding dynamically -- the measured
    // A7a/A7b pair.
    case PSpecialVariableP:
      frame->kind = FeFrameUnary;
      frame->fn = fn;
      frame->rest = arguments;
      frame->callee = &unbound;
      return false;
    // `eq`/`eql` (05D): strictly binary, matching the pinned Emacs and the
    // `/=` rule (05A row C3) -- the raw arity, exactly two operands, is
    // checked before anything evaluates, so `(eq 1)` and `(eq 1 2 3)` are
    // `wrong-number-of-arguments`. The rest of the binary family below is
    // the same `{2, 2}` contract and the same frame setup.
    case PEq:
    case PEql:
    case PCons:
    case PSetCar:
    case PSetCdr:
    case PIs:
    case PFset:
    case PDefalias:
    // `internal--mark-special` (11B): SYMBOL then FULL-P, the target
    // validated on delivery before the flag form is evaluated, which is the
    // validate-first ordering the rest of this family already has.
    case PMarkSpecial:
    // `string<`/`string>` (Phase 20): the same `{2, 2}` contract as the rest
    // of this family, and neither operand is validated before the other
    // evaluates -- Emacs raises `(wrong-type-argument stringp 1)` for
    // `(string< 1 "a")` only once both are in hand, which is where the
    // comparison itself checks them.
    case PStringLess:
    case PStringGreater:
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
    // `ResumeBody`'s rule, for the same reason: the accumulator cons just
    // made is a frame field and therefore a mark-phase root, so it does not
    // also need a GC-stack slot. Without this restore a call's root-stack
    // cost grew with its argument count and a long enough argument list
    // overflowed the stack rather than the arena.
    FeRestoreGC(ctx, frame->gc_checkpoint);
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
  FE_PERF_INC(FePerfDispatchLambda);
  FeObject* va = CDR(fn);  // (env params ...)
  FeObject* vb = CDR(va);  // (params ...)
  frame->kind = FeFrameLambda;
  FeObject* const identity = CallIdentity(frame->expr, fn);
  const size_t argc = CountRawArguments(ctx, CDR(frame->expr));
  // Keep the evaluated list rooted across `ArgsToEnv`'s own allocations: the
  // GC-stack slots that used to hold it are released once per delivered
  // argument, above, so this frame field is now its only root.
  frame->accumulator = arguments;
  frame->env = ArgsToEnv(ctx, CAR(vb), arguments, CAR(va), identity, argc);
  frame->rest = CDR(vb);
  // The evaluated arguments have done their work; hand the accumulator back
  // to `ResumeBody`, whose "value of the last completed body form" it now is.
  // A closure with no body forms at all -- legal since the constructors take
  // a parameter-list minimum -- completes immediately from this value, so
  // leaving the argument list here made `((lambda (x)) 1)` answer `(1)`.
  frame->accumulator = &nil;
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

// What one `macroexpand-1` step did to a form (sub-plan 10B).
typedef enum MacroexpandOutcome {
  // The form is not a macro call at all, and is therefore its own expansion.
  MacroexpandNone,
  // The head symbol's function cell held another symbol -- a `defalias`
  // indirection -- and substituting it *is* the step. `*form` is the new
  // form; nothing was evaluated.
  MacroexpandSubstituted,
  // The head named a macro, and the frame is now evaluating its transformer
  // body. The expansion arrives at `ResumeMacroBody`'s reflective tail.
  MacroexpandBody,
} MacroexpandOutcome;

// One `macroexpand-1` step over `*form`, following Emacs 31.0.90's rule as
// measured on the pinned oracle:
//
//   (macroexpand-1 '(+ 1 2))  => (+ 1 2)     not a macro call
//   (macroexpand-1 42)        => 42          not a cons
//   (macroexpand-1 '(ali 7))  => (one-arg 7) one alias indirection is a step
//   (macroexpand-1 '(one-arg 7)) => '7       the transformer's own answer
//   (macroexpand-1 '(pf 1))   => (pf 1)      pf is an alias to a *function*
//   (macroexpand-1 '(a2 1))   => (a2 1)      a2 is an alias to an unbound name
//
// The alias arm is the reason this does not simply resolve the chain and
// apply: the evaluator resolves a whole `defalias` chain before it calls
// anything, but Emacs' `macroexpand-1` (subr.el) rewrites the head one link
// at a time, so through a two-link chain it answers the middle link and only
// `macroexpand`'s fixpoint reaches the transformer. The rewrite happens only
// when the target *is* a macro -- Emacs' `(and (symbolp def) (macrop def))`,
// which is why the last two rows above are unchanged rather than rewritten to
// `(plainfn 1)` and `(nosuch 1)`. That test is what `ResolveFunctionCallable`
// is doing here: it answers the same question `macrop` does, including
// raising `cyclic-function-indirection` on an alias ring, which is what call
// position already raises for the same names (Emacs never gets there -- its
// `defalias` refuses to build the ring in the first place). So no alias shape
// can spin inside the fixpoint; only a transformer that expands to a call to
// itself can, and that charges its body's evaluation steps.
//
// `*form` must be rooted by the caller (a GC-stack slot or a frame field)
// across the call: both arms allocate.
static MacroexpandOutcome MacroexpandStep(FeContext* ctx,
                                          FeEvalFrame* frame,
                                          FeObject** form,
                                          FeObject* mode) {
  if (FeGetType(*form) != FeTPair) {
    return MacroexpandNone;
  }
  FeObject* const head = CAR(*form);
  if (FeGetType(head) != FeTSymbol) {
    return MacroexpandNone;
  }
  FeObject* const cell = SymbolFunction(head);
  if (FeGetType(cell) == FeTSymbol) {
    if (FeGetType(ResolveFunctionCallable(ctx, cell, nullptr)) != FeTMacro) {
      return MacroexpandNone;
    }
    EvaluationStep(ctx);
    *form = FeCons(ctx, cell, CDR(*form));
    return MacroexpandSubstituted;
  }
  if (FeGetType(cell) != FeTMacro) {
    return MacroexpandNone;
  }
  EnterMacroBody(ctx, frame, cell, *form, head, mode);
  return MacroexpandBody;
}

// The whole of a reflective expansion's control flow, for both primitives and
// from both of its entry points -- `DispatchMacroexpand`, which starts one,
// and `ResumeMacroBody`, which re-enters after a transformer body produced
// its expansion. `step` says whether another step is due at all: the starting
// call always steps once, and a resuming call steps again only for
// `macroexpand`, whose rule is Emacs' fixpoint (keep going while the form is
// still a macro call). `macroexpand-1` therefore falls straight through to
// the answer, which is what makes one step one step.
//
// A substitution is taken here, in the loop, because it produces a new form
// without evaluating anything; a transformer body is not, because running it
// is the frame machine's job. Returns true when the frame is now evaluating
// such a body, false when there is nothing left to do and `*result` holds the
// expansion.
//
// The GC stack cost is one slot no matter how many steps a fixpoint takes:
// each pass restores the frame's own checkpoint and re-pushes the current
// form, the same idiom `SpreadApplyArgs` uses.
static bool MacroexpandContinue(FeContext* ctx,
                                FeEvalFrame* frame,
                                FeObject* form,
                                FeObject* mode,
                                bool step,
                                FeObject** result) {
  while (step) {
    const MacroexpandOutcome outcome = MacroexpandStep(ctx, frame, &form, mode);
    if (outcome == MacroexpandBody) {
      return true;
    }
    step = outcome == MacroexpandSubstituted && PRIM(mode) == PMacroexpand;
    FeRestoreGC(ctx, frame->gc_checkpoint);
    FePushGC(ctx, form);
  }
  *result = form;
  return false;
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
// `FeFrameMacroExpansion`.
//
// A *reflective* expansion (sub-plan 10B) is the one exit that completes
// this frame instead of pushing another: `fn` holds the resolved
// `macroexpand-1`/`macroexpand` primitive rather than a caller environment,
// the expansion is the frame's value, and it is never evaluated.
// `macroexpand` re-enters `MacroexpandStep` on the expansion first, so a
// macro that expands to another macro call keeps expanding -- Emacs' rule,
// measured -- and each pass charges at least the body's own evaluation
// steps, which is what stops a self-expanding macro at the step budget
// instead of hanging.
static bool ResumeMacroBody(FeContext* ctx,
                            FeEvalFrame* frame,
                            FeObject** result) {
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
  FeObject* const expansion = frame->accumulator;
  RunCleanupsDownTo(ctx, frame->cleanup_checkpoint);
  FeRestoreGC(ctx, frame->gc_checkpoint);
  FePushGC(ctx, expansion);  // Nothing else refers to the expansion now.
  if (FeGetType(frame->fn) == FeTPrimitive) {
    return !MacroexpandContinue(ctx, frame, expansion, frame->fn,
                                PRIM(frame->fn) == PMacroexpand, result);
  }
  ctx->call_list = CDR(&frame->trace_cell);
  frame->kind = FeFrameMacroExpansion;
  PushEvaluationFrame(ctx, expansion, frame->fn, NULL);
  return false;
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
    *result = PRIM(frame->fn) == PAnd ? ctx->t : &nil;
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
//
// A let-dynamic target (sub-plan 11B) binds shallowly instead: the global
// cell is swapped and the restore obligation pushed, and `*bind` is left
// alone, since a lexical entry for the name would shadow exactly the cell
// the binding just wrote. The scope is the same either way -- the rest of
// the enclosing sequence -- so the restore must *not* run when this little
// form completes, which is what raising this frame's own
// `cleanup_checkpoint` past the new entry says. The entry is then drained by
// whichever frame owns the enclosing body, on every completion kind, exactly
// as the binding-list form's is.
static bool ResumeLet(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    if (SymbolIsLetDynamic(ctx, frame->accumulator)) {
      PushDynamicBinding(ctx, frame->accumulator, frame->callee);
      frame->cleanup_checkpoint = ctx->cleanup_stack_index;
    } else {
      *frame->bind =
          BindValue(ctx, frame->env, frame->accumulator, frame->callee);
    }
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
// `(setq a 1 b)`: the trailing target has no value form. 07A Decision 4 says
// every arity raise carries the call's identity and its actual count, and
// Emacs 31.0.90 measures this one as `(wrong-number-of-arguments setq 3)` --
// the whole form's raw count, which is why it is recomputed from `expr`
// rather than read off `rest`, which the walk has already advanced past.
// Emacs also assigns every complete pair before raising, as `ResumeSetq`
// does; measured, `(setq a 1 b)` leaves `a` at 1.
[[noreturn]] static void RaiseSetqFormCount(FeContext* ctx,
                                            const FeEvalFrame* frame) {
  RaiseWrongNumber(ctx, CallIdentity(frame->expr, frame->fn),
                   CountRawArguments(ctx, CDR(frame->expr)));
}

static bool ResumeSetq(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    ValidateSetqTarget(ctx, frame->env, frame->accumulator);
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
    RaiseSetqFormCount(ctx, frame);
  }
  FeObject* const target = CAR(frame->rest);
  ValidateSetqTarget(ctx, frame->env, target);
  frame->rest = CDR(frame->rest);
  if (FeGetType(frame->rest) != FeTPair) {
    RaiseSetqFormCount(ctx, frame);
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
    case PIntegerp:
      // 05C's numeric predicates are arity-exact like `boundp`/`makunbound`
      // above, and answer from the tag alone -- an integer is a distinct
      // object, not a double in disguise.
      FeRequireNoArguments(ctx, frame->rest);
      *result = FeMakeBool(ctx, FeGetType(value) == FeTInteger);
      break;
    case PFloatp:
      FeRequireNoArguments(ctx, frame->rest);
      *result = FeMakeBool(ctx, FeGetType(value) == FeTDouble);
      break;
    case PKeywordp:
      FeRequireNoArguments(ctx, frame->rest);
      *result = FeMakeBool(ctx, IsKeywordSymbol(ctx, value));
      break;
    // `error-message-string` (Phase 19). The type check is Emacs' own: it
    // takes the ERROR object apart with `car`/`cdr`, so a non-list is
    // `(wrong-type-argument listp X)` there, measured, rather than a
    // message about a condition. The buffer is this arm's own frame, sized
    // like the one `error`'s formatter above uses, and the rendering never
    // allocates -- only the string built from it does.
    case PErrorMessageString: {
      FeRequireNoArguments(ctx, frame->rest);
      char message[1024];
      if (!FeIsNil(value) && FeGetType(value) != FeTPair) {
        RaiseWrongType(ctx, "listp", value);
      }
      (void)RenderErrorMessage(ctx, value, message, sizeof(message));
      *result = FeMakeString(ctx, message);
      break;
    }
    // `special-variable-p` (11B) reads the *special* flag and nothing else,
    // so a symbol marked by a one-arg `defvar` answers nil here while still
    // binding dynamically -- the measured A7a/A7b pair. The constants are
    // the one answer that is not the flag: `(special-variable-p nil)`,
    // `t` and any keyword are all `t` on Emacs 31.0.90, measured, even
    // though nothing can ever bind one. They are tested before the type
    // check because fe's nil is not a symbol object at all. A non-symbol is
    // `(wrong-type-argument symbolp X)`, which is Emacs' answer too.
    case PSpecialVariableP: {
      FeRequireNoArguments(ctx, frame->rest);
      if (FeIsNil(value) || IsConstantSymbol(ctx, value)) {
        *result = FeMakeBool(ctx, true);
        break;
      }
      *result = FeMakeBool(
          ctx, SymbolIsSpecial(ctx, CheckType(ctx, value, FeTSymbol)));
      break;
    }
    // One constancy check, before the type check: `CheckType` returns its
    // argument unchanged, so the second `RejectConstantTarget(ctx, sym)` each
    // arm used to make could never fail where the first had not.
    case PFmakunbound: {
      RejectConstantTarget(ctx, value);
      FeObject* const sym = CheckType(ctx, value, FeTSymbol);
      FeRequireNoArguments(ctx, frame->rest);
      SetSymbolFunction(sym, &unbound);
      *result = sym;
      break;
    }
    default: {  // PMakeUnbound
      RejectConstantTarget(ctx, value);
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
// `setcdr`, `is`. `accumulator` holds the (possibly checked)
// first operand, `&unbound` marking "not yet delivered". `setcar`/`setcdr`
// validate their first operand as a pair immediately on delivery, before
// the second operand is even evaluated -- the ordering the recursive arm
// required. The comparisons `<`/`<=` shared this frame until 05C made them
// chained and variadic (`FeFrameEvalList`); their binary arm died with the
// move.
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
      // `fset`/`defalias` (04C): the target is a symbol, checked before the
      // function form is evaluated, matching the other binary primitives'
      // validate-first ordering.
      case PFset:
      case PDefalias:
      // `internal--mark-special` (11B): nil, `t` and keywords are constants
      // and cannot be `let`-bound at all, so marking one is refused here
      // rather than discovered at the binding site.
      case PMarkSpecial:
        ValidateValueTarget(ctx, first);
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
      *result = FeMakeBool(ctx, Equal(ctx, first, second));
      break;
    case PEq:
      *result = FeMakeBool(ctx, IdentityObjects(first, second, false));
      break;
    case PEql:
      *result = FeMakeBool(ctx, IdentityObjects(first, second, true));
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
    // `internal--mark-special` (11B). A non-nil FULL-P sets both flags (the
    // two-arg `defvar` and `defconst` case), nil sets the let-dynamic flag
    // alone (the one-arg `defvar` case). Returns the symbol, as `defvar`
    // does in Emacs.
    case PMarkSpecial:
      MarkSpecialSymbol(ctx, first, !FeIsNil(second));
      *result = first;
      break;
    // `string<`/`string>` (Phase 20). One order, spent twice: Emacs defines
    // `string-greaterp` as `string-lessp` with the operands the other way
    // round, and so does this.
    case PStringLess:
      *result = FeMakeBool(ctx, StringOperandLess(ctx, first, second));
      break;
    case PStringGreater:
      *result = FeMakeBool(ctx, StringOperandLess(ctx, second, first));
      break;
    default:
      abort();
  }
  frame->callee = &unbound;
  return true;
}

// The value an arithmetic frame completes with: the running total, or -- when
// no operand was combined at all -- Emacs' identity element for the operator,
// `(+)` and `(-)` being 0 and `(*)` 1. Since 05C these identities are
// *integers* (`FeTInteger`), the type Emacs gives them; the double-only
// reader cannot see the difference until the 05D cut, and the printer renders
// integer and integral double alike. `(/)` has no identity to return and is
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
  return FeMakeInteger(ctx, primitive == PMul ? 1 : 0);
}

// The first operand of an arithmetic reduction seeds the accumulator (05C).
// `+` and `*` use it as-is; `-` and `/` apply their unary semantics -- `(- 5)`
// is negation, `(/ 5)` the reciprocal (05A rows A1/A2) -- but only when the
// operand is also the *last* one, i.e. the reduction is unary. A multi-
// operand `(- 7 2)` is ordinary subtraction from 7, not a negated 7, so the
// seed's unary behaviour is conditional on `only_operand` (which `ResumeArith`
// reads from `frame->rest` at seed time). The old behaviour -- "the first
// operand is the running total" -- made `(- 5)` answer 5 and `(/ 5)` answer
// 5, neither of them Emacs'. A negated `INT64_MIN` overflows and is
// `arith-error`, and the reciprocal of an integer truncates toward zero over
// the integers, so `(/ 5)` is 0 and `(/ 1)` is 1, with `(/ 0)` the same
// integer division by zero as `(/ 1 0)`. A non-number seeds with
// `wrong-type-argument`, the numeric family's name.
static FeObject* SeedArith(FeContext* ctx,
                           Primitive primitive,
                           FeObject* operand,
                           bool only_operand) {
  const FeType type = FeGetType(operand);
  if (type == FeTInteger) {
    if (primitive == PSub && only_operand) {
      int64_t negated;
      if (ckd_sub(&negated, 0, INTEGER(operand))) {
        RaiseNamedError(ctx, "arith-error", "arith-error");
      }
      return FeMakeInteger(ctx, negated);
    }
    if (primitive == PDiv && only_operand) {
      if (INTEGER(operand) == 0) {
        RaiseNamedError(ctx, "arith-error", "arith-error");
      }
      return FeMakeInteger(ctx, 1 / INTEGER(operand));
    }
    return operand;
  }
  if (type == FeTDouble) {
    if (primitive == PSub && only_operand) {
      return FeMakeDouble(ctx, -GetDouble(operand));
    }
    if (primitive == PDiv && only_operand) {
      return FeMakeDouble(ctx, 1.0 / GetDouble(operand));
    }
    return operand;
  }
  RaiseWrongType(ctx, "number-or-marker-p", operand);
}

// Combines the accumulated value with a just-delivered operand through
// `GetNumericPair`, the tower's single promotion rule: integer+integer stays
// integer with the int64 overflow detection `ckd_add`/`ckd_sub`/`ckd_mul`
// provide -- overflow is `arith-error`, never undefined behaviour (05A
// Decision 5, row A8) -- integer division truncates toward zero and errors on
// a zero divisor, with `INT64_MIN / -1` an `arith-error` too (it would
// overflow), and any double promotes the whole reduction. The operands' raw
// values are extracted before the result is allocated, so the C locals need
// no collector root of their own across `FeMakeInteger`/`FeMakeDouble`.
static FeObject* CombineNumeric(FeContext* ctx,
                                Primitive primitive,
                                FeObject* accumulator,
                                FeObject* delivered) {
  const NumericPair p = GetNumericPair(ctx, accumulator, delivered);
  if (p.is_integer) {
    int64_t result;
    switch ((char)primitive) {
      case PAdd:
        if (ckd_add(&result, p.a_i, p.b_i)) {
          RaiseNamedError(ctx, "arith-error", "arith-error");
        }
        return FeMakeInteger(ctx, result);
      case PSub:
        if (ckd_sub(&result, p.a_i, p.b_i)) {
          RaiseNamedError(ctx, "arith-error", "arith-error");
        }
        return FeMakeInteger(ctx, result);
      case PMul:
        if (ckd_mul(&result, p.a_i, p.b_i)) {
          RaiseNamedError(ctx, "arith-error", "arith-error");
        }
        return FeMakeInteger(ctx, result);
      default:  // PDiv
        if (p.b_i == 0 || (p.a_i == INT64_MIN && p.b_i == -1)) {
          RaiseNamedError(ctx, "arith-error", "arith-error");
        }
        return FeMakeInteger(ctx, p.a_i / p.b_i);
    }
  }
  const double combined = primitive == PAdd   ? p.a_d + p.b_d
                          : primitive == PSub ? p.a_d - p.b_d
                          : primitive == PMul ? p.a_d * p.b_d
                                              : p.a_d / p.b_d;
  return FeMakeDouble(ctx, combined);
}

// `+`, `-`, `*`, `/`: streams every operand, validating and combining each
// as it arrives, exactly as the recursive `ARITH_OP` macro's own loop did --
// never batching the whole list first the way `FeFrameEvalList` does.
// `accumulator` holds the running total, boxed either as an integer or a
// double (05C) so it stays an ordinary marked field; `&unbound` marks "no
// operand combined yet". The combine and seed paths extract their operands'
// values before allocating, and `accumulator` is a frame field (a collector
// root) throughout, so no operand is ever live only in a C local across a
// possible collection.
static bool ResumeArith(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    FeObject* const delivered = frame->callee;
    frame->callee = &unbound;
    const size_t gc = FeSaveGC(ctx);
    if (frame->accumulator == &unbound) {
      // `only_operand` is whether the just-delivered first operand is also
      // the last: `frame->rest` still holds every not-yet-evaluated operand.
      frame->accumulator = SeedArith(ctx, (Primitive)PRIM(frame->fn), delivered,
                                     FeIsNil(frame->rest));
    } else {
      frame->accumulator = CombineNumeric(ctx, (Primitive)PRIM(frame->fn),
                                          frame->accumulator, delivered);
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

// One adjacent-pair comparison for the chained comparators and `=` (05C).
// Both operands go through `GetNumericPair`, so the promotion rule and the
// `wrong-type-argument` check are shared with arithmetic; the primitive
// selects the comparison. Doubles compare with plain C operators -- for `=`/
// `/=`, `==`/`!=`, preserving the pinned signed-zero (`0.0 = -0.0` is t) and
// NaN (`=` is never t with NaN) answers; `-Wfloat-equal` is suppressed for
// the intentional exact equality, the same way `Equal()`'s helper does.
static bool NumericSatisfies(FeContext* ctx,
                             Primitive primitive,
                             FeObject* a,
                             FeObject* b) {
  const NumericPair p = GetNumericPair(ctx, a, b);
  if (p.is_integer) {
    switch ((char)primitive) {
      case PNumericEqual:
        return p.a_i == p.b_i;
      case PNotEqual:
        return p.a_i != p.b_i;
      case PLess:
        return p.a_i < p.b_i;
      case PLessEqual:
        return p.a_i <= p.b_i;
      case PGreater:
        return p.a_i > p.b_i;
      default:  // PGreaterEqual
        return p.a_i >= p.b_i;
    }
  }
  switch ((char)primitive) {
    case PNumericEqual:
    case PNotEqual: {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
      const bool equal =
          primitive == PNumericEqual ? p.a_d == p.b_d : p.a_d != p.b_d;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
      return equal;
    }
    case PLess:
      return p.a_d < p.b_d;
    case PLessEqual:
      return p.a_d <= p.b_d;
    case PGreater:
      return p.a_d > p.b_d;
    default:  // PGreaterEqual
      return p.a_d >= p.b_d;
  }
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
    [PEq] = true,
    [PEql] = true,
    [PAtom] = true,
    [PPrint] = true,
    [PLess] = true,
    [PLessEqual] = true,
    [PGreater] = true,
    [PGreaterEqual] = true,
    [PNotEqual] = true,
    [PIntegerp] = true,
    [PFloatp] = true,
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
    // Sub-plan 06C: `throw` is function-shaped -- its two operands evaluate
    // normally, exactly as Emacs' `(special-form-p 'throw)` is nil -- while
    // `catch` is a special form and stays out of this table.
    [PThrow] = true,
    // Sub-plan 10B: the reflective expanders are ordinary functions in Emacs
    // (`(funcall 'macroexpand-1 '(when t 1))` works there), and their arms
    // below evaluate every operand. `macroexpand-all` is function-shaped too:
    // after evaluating its operands it raises the named unsupported error.
    [PMacroexpand1] = true,
    [PMacroexpand] = true,
    [PMacroexpandAll] = true,
    // Sub-plan 11B: both are ordinary functions -- their operands evaluate,
    // and Emacs' `special-variable-p` is a function too (`(funcall
    // 'special-variable-p 'x)` works there).
    [PMarkSpecial] = true,
    [PSpecialVariableP] = true,
    // Sub-plan 12B: `eval` is an ordinary function in Emacs --
    // `(special-form-p 'eval)` is nil and `(funcall 'eval '(+ 1 2))` is 3
    // there -- and its arm evaluates both operands before relaying.
    [PEval] = true,
    // Phase 14: every one of the symbol family is an ordinary function in
    // Emacs -- `(special-form-p 'intern)` is nil, `(mapcar 'symbol-name '(a
    // b))` works -- and every one of them evaluates all of its operands here,
    // routed as one family into the eval-list frame.
    [PIntern] = true,
    [PInternSoft] = true,
    [PSymbolName] = true,
    [PMakeSymbol] = true,
    [PGensym] = true,
    [PPut] = true,
    [PGet] = true,
    [PSymbolPlist] = true,
    [PDefineError] = true,
    // Phase 19: `error-message-string` is an ordinary function in Emacs too
    // -- `(special-form-p 'error-message-string)` is nil there and
    // `(mapcar 'error-message-string '((error "a")))` works -- and its arm
    // evaluates its one operand on the unary frame beside `keywordp`.
    [PErrorMessageString] = true,
    // Phase 20: `string<`/`string>` are ordinary functions in Emacs --
    // `(special-form-p 'string<)` is nil and `(funcall 'string< "a" "b")` is
    // t there -- and both operands evaluate here on the binary frame.
    [PStringLess] = true,
    [PStringGreater] = true,
    // Phase 24: every one of the vector family is an ordinary function in
    // Emacs -- `(special-form-p 'aref)` is nil, `(mapcar 'vectorp '([1] 1))`
    // works -- and every one of them evaluates all of its operands here,
    // routed as one family into the eval-list frame.
    [PVector] = true,
    [PMakeVector] = true,
    [PVectorp] = true,
    [PAref] = true,
    [PAset] = true,
    [PVconcat] = true,
    [PLength] = true,
    [PElt] = true,
    // Phase 13: all three are ordinary functions in Emacs -- `(special-form-p
    // 'signal)`, `'error` and `'keywordp` are all nil there, and
    // `(funcall 'signal 'error '("x"))`, `(apply 'error '("boom"))` and
    // `(mapcar 'keywordp '(:a 1))` all work. Their arms here evaluate every
    // operand too: `signal` and `error` route to the eval-list frame beside
    // `throw`, `keywordp` to the unary frame beside `integerp`. Missing rows
    // made `funcall`/`apply` reject them as special forms, so the one name a
    // handler-writing program needs most -- `signal` -- was unreachable
    // through the two entry points a prelude `mapcar`/`apply` is built on.
    [PSignal] = true,
    [PError] = true,
    [PKeywordp] = true,
    // False, listed for the record: `let`, `setq`, `if`, `lambda`, `macro`,
    // `while`, `quote`, `and`, `or`, `do`, `unwind-protect`, `function`,
    // `catch`.
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
bool IsRawFormCallable(const FeObject* fn) {
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
  FeObject* const callable = ResolveFunctionCallable(ctx, operand, nullptr);
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

// `macroexpand-1`/`macroexpand`'s tail, once FORM and the optional
// ENVIRONMENT have been evaluated into `list` (sub-plan 10B).
//
// ENVIRONMENT is accepted and must be nil. Emacs' environments are an
// internal alist of `(NAME . DEFINITION)` entries that shadow the function
// cell -- measured: `(macroexpand-1 '(foo) '((foo lambda (&rest _) 99)))` is
// 99 on the pinned oracle -- and implementing that alist is a second name
// resolution path nothing in this program uses. A non-nil value is rejected
// by name rather than silently ignored, the reader's own convention: a
// caller who passes one gets told the feature is missing instead of getting
// an answer computed as if the argument were not there.
//
// The form is pushed onto the GC stack because `EnterMacroBody` clears
// `accumulator` -- which is the operand list's only root -- before
// `ArgsToEnv` allocates over it. The slot is released by the frame's own
// `gc_checkpoint` restore, on every route out of here.
static bool DispatchMacroexpand(FeContext* ctx,
                                FeEvalFrame* frame,
                                FeObject* list,
                                FeObject** result) {
  FeObject* rest = list;
  FeObject* const form = FeGetNextArgument(ctx, &rest);
  if (!FeIsNil(rest) && !FeIsNil(CAR(rest))) {
    FeHandleError(ctx, "unsupported feature: macroexpand environment");
  }
  FeObject* const mode = frame->fn;
  FePushGC(ctx, form);
  return !MacroexpandContinue(ctx, frame, form, mode, true, result);
}

// `eval`'s tail, once FORM and the optional LEXICAL have been evaluated into
// `list` (sub-plan 12B Part 2).
//
// The whole point is where the form runs: this frame becomes a relay for an
// ordinary expression frame pushed above it, in the SAME run, so nothing
// stands between the evaluated form and the handlers, catches and cleanups
// established around the `eval` call. A condition propagates to an enclosing
// `condition-case`, a `throw` reaches an enclosing `catch`, a quit stays a
// quit, and the steps the form spends come out of the caller's budget. That
// is the `funcall`/`apply` redispatch shape (`DispatchFuncallApply` above)
// minus the call-form construction, and like it, it adds no frame kind.
//
// The environment is the global one, not `frame->env`. Emacs' LEXICAL
// argument selects the environment and never inherits the caller's:
// measured on 31.0.90 under `lexical-binding: t`, `(let ((qq 1)) (eval 'qq))`
// is `(void-variable qq)`, while a `let` over a name `defvar` made dynamic
// is visible to the evaluated form. Fe reproduces both with `&nil` here,
// because a dynamically bound name is read through its global cell.
//
// LEXICAL must be nil. Emacs accepts `t` (lexical binding with an empty
// environment) and an alist, neither of which fe has an environment model
// for; a non-nil value is rejected by name rather than silently ignored --
// `DispatchMacroexpand`'s ENVIRONMENT convention, and the reader's.
//
// `list` is re-rooted in `accumulator` before anything else: `ResumeEvalList`
// empties that field while reversing the operand list, so on entry `list` is
// live only in this C local, and `AllocateFrame` inside
// `PushEvaluationFrame` can raise.
static bool DispatchEval(FeContext* ctx, FeEvalFrame* frame, FeObject* list) {
  frame->accumulator = list;
  FeObject* rest = list;
  FeObject* const form = FeGetNextArgument(ctx, &rest);
  if (!FeIsNil(rest) && !FeIsNil(CAR(rest))) {
    FeHandleError(ctx, "unsupported feature: eval lexical argument");
  }
  frame->kind = FeFrameRelay;
  PushEvaluationFrame(ctx, form, &nil, NULL);
  return false;
}

// `(catch TAG BODY...)` (sub-plan 06C): the frame's `accumulator` holds the
// evaluated tag (`&unbound` until the tag sub-expression delivers it), `rest`
// the raw BODY forms. The tag is pushed as an ordinary sub-expression; once
// it delivers, the BODY forms are pushed as an implicit sequential body frame
// -- the sub-plan's "one expression frame for its body region" cost. The
// body's delivered value is the catch's result, and so is a value a `throw`
// unwound into this frame's `callee` (`PerformThrow`): the two arrivals are
// the same delivery, which is why the unwind marks a still-awaiting-tag catch
// frame's tag phase complete before it sets `callee`.
static bool ResumeCatch(FeContext* ctx, FeEvalFrame* frame, FeObject** result) {
  if (frame->callee != &unbound) {
    if (frame->accumulator != &unbound) {
      *result = frame->callee;
      frame->callee = &unbound;
      return true;
    }
    // The tag sub-expression delivered.
    frame->accumulator = frame->callee;
    frame->callee = &unbound;
    PushBodyFrame(ctx, frame->env, frame->rest);
    return false;
  }
  PushEvaluationFrame(ctx, FeGetNextArgument(ctx, &frame->rest), frame->env,
                      NULL);
  return false;
}

static bool ResumeConditionCase(FeContext* ctx,
                                FeEvalFrame* frame,
                                FeObject** result) {
  if (FeIsNil(frame->fn)) {
    *result = frame->callee;
    return true;
  }
  if (frame->fn == &unbound) {
    *result = frame->callee;
    return true;
  }
  FeObject* const clause = frame->fn;
  // A condition from handler forms must bypass this active handler.
  frame->fn = &unbound;
  FeObject* env = frame->env;
  if (!FeIsNil(frame->accumulator)) {
    env = BindValue(ctx, env, frame->accumulator, frame->callee);
  }
  frame->env = env;
  frame->callee = &unbound;
  PushBodyFrame(ctx, env, CDR(clause));
  return false;
}

// The innermost matching catch frame for a throw's tag, or false when the
// current run (down to its `run_base` floor) holds none. Tag comparison is
// Emacs' `eq` (`IdentityObjects` with `compare_floats` false): fixnums match
// equal fixnums and a shared cons matches itself, while floats, strings and
// freshly-built conses do not -- and `nil` never matches as a tag (a catch
// whose tag is nil catches nothing, and a throw whose tag is nil matches
// nothing), per the sub-plan's CT4/CT5 rows.
static bool FindCatchFrame(const FeContext* ctx, FeObject* tag, size_t* index) {
  for (size_t i = ctx->frame_stack_index; i-- > ctx->run_base;) {
    const FeEvalFrame* frame = &ctx->frame_stack[i];
    if (frame->kind == FeFrameCatch && !FeIsNil(tag) &&
        !FeIsNil(frame->accumulator) &&
        IdentityObjects(tag, frame->accumulator, false)) {
      *index = i;
      return true;
    }
  }
  return false;
}

// The mid-stack unwind a `throw` performs once both operands have been
// evaluated (sub-plan 06C): search down the current run's frame stack for the
// innermost catch whose tag is `eq`; a throw that finds no catch raises
// `no-catch TAG VALUE` through the ordinary error path. A found catch is
// delivered the value -- the drain to the checkpoint, not to zero: cleanups
// registered above the catch frame run (innermost first) through the same
// `RunCleanupsDownTo` every completing pair frame uses, the GC stack is
// restored to the catch frame's own checkpoint, every frame above it is
// discarded, and the value lands in the catch frame's `callee` for the loop
// to resume. Returns false so `RunEvaluationLoop` continues at the catch
// frame instead of completing the throw form, which was just discarded.
//
// The delivered value is rooted on the GC stack from before the drain (a
// cleanup's own allocations may trigger a collection) until it is safe in a
// frame field, re-pushed across the checkpoint restore the way
// `RunEvaluationLoop`'s terminus re-roots its result. The in-flight
// completion is `FeCompletionThrow` for the duration of the drain -- the
// `CleanupFrameReserve` gate (`AllocateFrame`'s `completion != Normal`)
// grants a cleanup provoked by frame exhaustion its working frames, exactly
// as it does for an error drain -- and resets once the value is delivered.
bool PerformThrow(FeContext* ctx, FeObject* tag, FeObject* value) {
  size_t index;
  if (!FindCatchFrame(ctx, tag, &index)) {
    if (ctx->cleanup_catch != nullptr) {
      // Inside a cleanup entry, whose forms run as a nested frame-machine
      // run above a saved barrier: this run's floor sits above every catch
      // frame belonging to the computation being unwound, so "no catch
      // here" is not yet an answer. Park the throw and resume at
      // `RunOneCleanupEntry`, which puts the enclosing run's frame stack and
      // floor back and asks again there -- exactly the route a cleanup's own
      // *error* already takes. Measured Emacs: `(catch 'tg (unwind-protect
      // (throw 'tg 'a) (throw 'tg 'b)))` is `b`, and a cleanup's throw wins
      // over an in-flight error too.
      ctx->pending_throw = true;
      ctx->pending_throw_tag = tag;
      ctx->pending_throw_value = value;
      longjmp(*ctx->cleanup_catch, 1);
    }
    char tag_text[64];
    char value_text[64];
    char message[160];
    (void)FeToString(ctx, tag, tag_text, sizeof(tag_text));
    (void)FeToString(ctx, value, value_text, sizeof(value_text));
    Format(message, sizeof(message), "no-catch %s %s", tag_text, value_text);
    RaiseCondition(ctx, FeCompletionError, "no-catch",
                   FeMakeList(ctx, (FeObject*[]){tag, value}, 2), message);
  }
  FeEvalFrame* const catch_frame = &ctx->frame_stack[index];
  ctx->completion = FeCompletionThrow;
  FePushGC(ctx, value);
  RunCleanupsDownTo(ctx, catch_frame->cleanup_checkpoint);
  FeRestoreGC(ctx, catch_frame->gc_checkpoint);
  FePushGC(ctx, value);
  // A catch whose tag form was itself the throw site is still awaiting the
  // tag; mark the tag phase complete so the catch frame's resume reads the
  // delivered value as the body's result rather than as the tag.
  if (catch_frame->accumulator == &unbound) {
    catch_frame->accumulator = &nil;
  }
  catch_frame->callee = value;
  ctx->frame_stack_index = index + 1;
  ctx->call_list = &catch_frame->trace_cell;
  ctx->completion = FeCompletionNormal;
  return false;
}

static void AppendErrorText(FeContext* ctx,
                            char* message,
                            size_t* length,
                            const char* text) {
  while (*text != '\0') {
    if (*length + 1 >= 1024) {
      FeHandleError(ctx, "error message too long");
    }
    message[(*length)++] = *text++;
  }
}

static void FormatErrorMessage(FeContext* ctx,
                               const FeObject* format,
                               FeObject* values,
                               char message[1024]) {
  const size_t format_length = FeStringByteLength(ctx, format);
  if (format_length >= 1024) {
    FeHandleError(ctx, "error message too long");
  }
  char source[1024];
  (void)FeCopyStringBytes(ctx, format, source, format_length);
  source[format_length] = '\0';
  size_t length = 0;
  for (size_t i = 0; i < format_length; i++) {
    const char byte = source[i];
    if (byte != '%') {
      AppendErrorText(ctx, message, &length, (char[]){byte, '\0'});
      continue;
    }
    if (i + 1 == format_length) {
      FeHandleError(ctx, "unsupported error format directive");
    }
    i++;
    if (source[i] == '%') {
      AppendErrorText(ctx, message, &length, "%");
      continue;
    }
    FeObject* value = FeGetNextArgument(ctx, &values);
    char rendered[128];
    switch (source[i]) {
      case 'd':
        Format(rendered, sizeof(rendered), "%lld",
               (long long)FeToInteger(ctx, value));
        break;
      // `%s` is Emacs' `princ` and `%S` its `prin1`, and the difference is
      // *recursive*: `(error "%s" (list 1 "x"))` is `(1 x)` and
      // `(error "%S" "str")` is `"str"`, both measured against 31.0.90.
      // `FeToString` splits the difference (unquoted at the top, quoted
      // inside), which is neither of them.
      case 's':
        if (FeGetType(value) == FeTString) {
          const size_t value_length = FeStringByteLength(ctx, value);
          if (value_length >= sizeof(rendered)) {
            FeHandleError(ctx, "error message too long");
          }
          (void)FeCopyStringBytes(ctx, value, rendered, value_length);
          rendered[value_length] = '\0';
        } else {
          (void)RenderObject(ctx, value, rendered, sizeof(rendered), 0);
        }
        break;
      case 'S':
        (void)RenderObject(ctx, value, rendered, sizeof(rendered), 1);
        break;
      default:
        FeHandleError(ctx, "unsupported error format directive");
    }
    AppendErrorText(ctx, message, &length, rendered);
  }
  // Arguments the format string never consumed are ignored, not an error:
  // `(error "x" 1 2)` is `(error "x")` in Emacs.
  message[length] = '\0';
}

// `list`, `=`, the chained comparators, `/=`, `set`, `funcall`, `apply`:
// evaluates the complete raw
// argument list first --
// unlike every other primitive continuation above, whose ordering is what
// makes them not this -- accumulating and reordering exactly as
// `FeFrameCallArguments`'s own argument list does, then finishes per
// primitive: `list` returns it as-is; `=`/`<`/`<=`/`>`/`>=` compare
// adjacent pairs left to right and stop at the first false one, so an
// operand past a settled answer is never type-checked (one argument is `t`
// without comparing or checking anything); `/=` is the same shape over its
// arity-checked two, exact
// inequality across types; `set`
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
    // `ResumeArguments`/`ResumeBody`'s rule: the accumulator is a frame
    // field and so a mark-phase root already, and holding a GC-stack slot
    // per operand as well made `(list 1 2 ... N)` overflow the root stack
    // at N around 4000 instead of at the arena's own limit.
    FeRestoreGC(ctx, frame->gc_checkpoint);
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
  // Phase 14's symbol family and Phase 24's vector family: the other half of
  // `DispatchPrimitive`'s two range tests. Each family's answer is computed
  // in fe.c, beside the storage it reads -- the obarray for one, the payload
  // region for the other -- so a family of eight costs this evaluator one
  // decision point.
  if (IsSymbolPrimitive((Primitive)PRIM(frame->fn))) {
    *result = EvaluateSymbolPrimitive(ctx, (Primitive)PRIM(frame->fn), list);
    return true;
  }
  if (IsVectorPrimitive((Primitive)PRIM(frame->fn))) {
    *result = EvaluateVectorPrimitive(ctx, (Primitive)PRIM(frame->fn), list);
    return true;
  }
  switch (PRIM(frame->fn)) {
    case PList:
      *result = list;
      break;
    case PSet: {
      FeObject* const symbol = CAR(list);
      FeObject* const value = CAR(CDR(list));
      *result = SetEvaluatedValue(ctx, symbol, value);
      break;
    }
    case PFuncall:
    case PApply:
      return DispatchFuncallApply(ctx, frame, list);
    case PMacroexpand1:
    case PMacroexpand:
      return DispatchMacroexpand(ctx, frame, list, result);
    case PEval:
      return DispatchEval(ctx, frame, list);
    // `macroexpand-all` (10A Decision 2): expanding every sub-form needs a
    // code walker that knows each special form's shape, which fe does not
    // have.  The operands have still been evaluated, as an ordinary
    // function's are, and the name is callable through `funcall`/`apply`;
    // every route now says which feature is missing instead of reporting
    // `invalid-function` or `void-function` like a typo.
    case PMacroexpandAll:
      FeHandleError(ctx, "unsupported feature: macroexpand-all");
    // `(throw TAG VALUE)` (sub-plan 06C): the two operands are the tag and
    // the value; the unwind discards this frame and every frame above the
    // catch it delivers into, so it returns `PerformThrow`'s "continue"
    // answer rather than completing the pair form.
    case PThrow:
      return PerformThrow(ctx, CAR(list), CAR(CDR(list)));
    case PSignal: {
      FeObject* const name = CAR(list);
      if (FeGetType(name) != FeTSymbol) {
        FeObject* data = FeMakeList(
            ctx, (FeObject*[]){FeMakeSymbol(ctx, "symbolp"), name}, 2);
        RaiseCondition(ctx, FeCompletionError, "wrong-type-argument", data,
                       "wrong-type-argument");
      }
      char symbol[64];
      const size_t symbol_length = FeStringByteLength(ctx, SymbolName(name));
      if (symbol_length >= sizeof(symbol)) {
        FeHandleError(ctx, "Invalid error symbol");
      }
      (void)FeCopyStringBytes(ctx, SymbolName(name), symbol, symbol_length);
      symbol[symbol_length] = '\0';
      if (!IsConditionSymbol(ctx, name)) {
        FeObject* data = FeMakeList(
            ctx, (FeObject*[]){FeMakeString(ctx, "Invalid error symbol"), name},
            2);
        RaiseCondition(ctx, FeCompletionError, "error", data,
                       "Invalid error symbol");
      }
      FeObject* const data = FeIsNil(CDR(list)) ? &nil : CAR(CDR(list));
      RaiseCondition(ctx,
                     IsNamedSymbol(ctx, name, "quit") ? FeCompletionQuit
                                                      : FeCompletionError,
                     symbol, data, symbol);
    }
    case PError: {
      // `(error)` with no format string at all is
      // `(wrong-number-of-arguments error 0)` in Emacs, not an `error`
      // condition whose message is the word "error".
      if (FeIsNil(list)) {
        RaiseCondition(ctx, FeCompletionError, "wrong-number-of-arguments",
                       &nil, "wrong-number-of-arguments");
      }
      const FeObject* const format = CAR(list);
      if (FeGetType(format) != FeTString) {
        RaiseNamedError(ctx, "wrong-type-argument", "wrong-type-argument");
      }
      char message[1024];
      FormatErrorMessage(ctx, format, CDR(list), message);
      RaiseCondition(
          ctx, FeCompletionError, "error",
          FeMakeList(ctx, (FeObject*[]){FeMakeString(ctx, message)}, 1),
          message);
    }
    default: {  // PNumericEqual, PNotEqual, PLess, PLessEqual, PGreater,
                // PGreaterEqual
      // The chained comparators and `=`/`/=` (05C): adjacent pairs are
      // compared left to right and the loop stops at the first false pair,
      // which is Emacs' rule -- `arithcompare_driver` returns `nil` the
      // moment a pair fails, so `(< 2 1 "a")` is `nil` and never looks at
      // the string, while `(< 1 2 "a")` (true up to the bad operand) does
      // reach it and is `wrong-type-argument`. Operand *forms* were all
      // evaluated before this arm ran, so short-circuiting never erases a
      // side effect; only the type check of an operand past a settled answer
      // is skipped.
      //
      // A single operand is `t` with no type check at all -- `(= "a")`,
      // `(< t)` and `(= 3)` are all `t` in Emacs, because the pair loop has
      // no pair to run. The loop below expresses that by construction rather
      // than by validating `CAR(list)` up front, which is what made fe
      // answer `wrong-type-argument` where Emacs answers `t`. Zero operands
      // is `wrong-number-of-arguments`, enforced at dispatch, so `CAR(list)`
      // here always exists.
      FeObject* prev = CAR(list);
      FeObject* rest = CDR(list);
      const Primitive op = (Primitive)PRIM(frame->fn);
      bool ok = true;
      while (ok && !FeIsNil(rest)) {
        ok = NumericSatisfies(ctx, op, prev, CAR(rest));
        prev = CAR(rest);
        rest = CDR(rest);
      }
      *result = FeMakeBool(ctx, ok);
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
    case FeFrameDynamicLet:
      return ResumeDynamicLet(ctx, frame);
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
    case FeFrameCatch:
      return ResumeCatch(ctx, frame, result);
    case FeFrameConditionCase:
      return ResumeConditionCase(ctx, frame, result);
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
    default:
      assert(false && "unhandled switch case for frame->kind");
      abort();
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
      case FeFrameDynamicLet:
      case FeFrameSetq:
      case FeFrameRelay:
      case FeFrameUnary:
      case FeFrameBinary:
      case FeFrameArith:
      case FeFramePrint:
      case FeFrameEvalList:
      case FeFrameCatch:
      case FeFrameConditionCase:
        FeMark(ctx, frame->expr);
        FeMark(ctx, frame->env);
        FeMark(ctx, frame->fn);
        FeMark(ctx, frame->rest);
        FeMark(ctx, frame->accumulator);
        FeMark(ctx, frame->callee);
        break;
      default:
        continue;
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
    case FeFrameDynamicLet:
    case FeFrameSetq:
    case FeFrameRelay:
    case FeFrameUnary:
    case FeFrameBinary:
    case FeFrameArith:
    case FeFramePrint:
    case FeFrameEvalList:
    case FeFrameCatch:
    case FeFrameConditionCase:
      return true;
    case FeFrameExpression:
    case FeFrameLambda:
    case FeFrameNative:
      return false;
    default:
      assert(false && "unhandled switch case for frame->kind");
      abort();
  }
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
[[noreturn]] void TransferRunError(FeContext* ctx, jmp_buf* saved_catch) {
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
jmp_buf* BeginRunBarrier(FeContext* ctx, jmp_buf* jump) {
  jmp_buf* const saved = ctx->evaluator_catch;
  ctx->evaluator_catch = jump;
  if (saved == nullptr) {
    ctx->completion = FeCompletionNormal;
    ctx->condition = &nil;
  }
  return saved;
}

// Drives the frame stack from `base` (already holding this run's one base
// frame -- `RunEvaluation` pushes an expression, `RunEvaluationBody` a
// sequential body) down to `base` again, returning the base frame's result.
// Shared by both entry points so there is exactly one loop implementing
// evaluation, not two near-duplicates that could drift.
FeObject* RunEvaluationLoop(FeContext* ctx, size_t base) {
  // The throw search (sub-plan 06C) needs this run's floor: a catch frame
  // below `base` belongs to an outer run and must not be matchable from
  // inside this one (the native re-entry wall). The save/restore lets a
  // nested run's own loop publish its base while it drives and hand the
  // floor back when it returns -- `RunEvaluation`/`RunEvaluationBody` each
  // call this loop, and only one loop is live at a time.
  const size_t volatile saved_run_base = ctx->run_base;
  // The native-call record (07A Decision 2) belongs to the C activation that
  // published it, and a caught condition longjmps back past every native
  // this run entered -- `RaiseCompletionCore` clears the record on the way
  // out, and the native arm's own restore below is skipped by the jump. What
  // must come back is the record of the native that owns *this* run, if a
  // native started it by calling `FeCall*`: that activation is still on the
  // C stack, below this loop's `setjmp`, and after the handler returns it
  // may still ask `FeGetNextArgument` for another argument. Without this a
  // `condition-case` anywhere inside a host-driven nested evaluation
  // silently stripped the enclosing native's identity and count.
  FeObject* const volatile saved_native_identity = ctx->native_identity;
  const size_t volatile saved_native_argc = ctx->native_argc;
  const bool volatile saved_native_active = ctx->native_call_active;
  jmp_buf condition_jump;
  jmp_buf* const volatile saved_condition_catch = ctx->condition_catch;
  ctx->run_base = base;
  // cppcheck-suppress autoVariables
  ctx->condition_catch = &condition_jump;
  FeObject* result = &nil;
  (void)setjmp(condition_jump);
  // Both on entry (a no-op) and after a caught condition jumped back here.
  ctx->native_identity = saved_native_identity;
  ctx->native_argc = saved_native_argc;
  ctx->native_call_active = saved_native_active;
  while (ctx->frame_stack_index > base) {
    FE_PERF_INC(FePerfEvalDispatch);
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
        // either starts the next form or produces the expansion. An
        // ordinary macro call then pushes the expansion (another frame); a
        // reflective `macroexpand-1`/`macroexpand` completes with it.
        if (!ResumeMacroBody(ctx, frame, &result)) {
          continue;
        }
        CompletePairFrame(ctx, frame);
        break;

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
        FeObject* const saved_identity = ctx->native_identity;
        const size_t saved_argc = ctx->native_argc;
        const bool saved_active = ctx->native_call_active;
        ctx->native_identity = CallIdentity(frame->expr, frame->fn);
        ctx->native_argc = CountRawArguments(ctx, CDR(frame->expr));
        ctx->native_call_active = true;
        FE_PERF_INC(FePerfDispatchNative);
        result = GetNativeFn(frame->fn)(ctx, frame->accumulator);
        ctx->native_identity = saved_identity;
        ctx->native_argc = saved_argc;
        ctx->native_call_active = saved_active;
        CompletePairFrame(ctx, frame);
        break;

      case FeFrameIf:
      case FeFrameWhile:
      case FeFrameAndOr:
      case FeFrameLet:
      case FeFrameDynamicLet:
      case FeFrameSetq:
      case FeFrameRelay:
      case FeFrameUnary:
      case FeFrameBinary:
      case FeFrameArith:
      case FeFramePrint:
      case FeFrameEvalList:
      case FeFrameCatch:
      case FeFrameConditionCase:
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
      default:
        assert(false && "unhandled switch case for frame->kind");
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
  // The run's own result is the one value no frame roots any more: every
  // intermediate result is delivered straight into the frame below's
  // `callee` (a mark-phase root) with no allocation in between, so the
  // per-completion `FePushGC` those completions used to do was one live
  // GC-stack entry per level of Lisp nesting -- the last thing making the
  // fixed 4096-slot GC stack, rather than the frame stack, the bound on
  // recursion depth. Pushing once here keeps the value the caller is about
  // to receive alive without that per-level cost.
  FePushGC(ctx, result);
  ctx->run_base = saved_run_base;
  ctx->condition_catch = saved_condition_catch;
  return result;
}
