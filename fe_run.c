// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

// The run driver and the host entry points: `RunEvaluation` and
// `RunEvaluationBody` (the two places an evaluator run's error barrier is
// installed), the `Evaluate` seam every evaluator-internal caller reaches
// them through, and the public `FeEvaluate*`/`FeCall*` surface -- including
// the protected call `FeTryCallWithOptions` and the `FeResignal` that puts a
// contained completion back in flight.
//
// Split out of fe_eval.c by sub-plan 11B of kg's Emacs-subset program: that
// file was at 517 of its 520 per-file complexity cap with the dynamic-binding
// work still to land in its binding section, and a translation-unit split is
// what keeps the per-file cap binding at full strength rather than raising
// it. The split is complexity-neutral by construction --
// `utils/check_scc_complexity.py` sums per file -- and moves no code: every
// function below is the one fe_eval.c held, unchanged.
//
// The seam this created is the block of declarations in fe_internal.h under
// "The run driver's seam": the loop, the barrier and the frame pushes stay
// in fe_eval.c and are called from here, while `Evaluate` and
// `RunEvaluationBody` live here and are called from there.

#include <setjmp.h>
#include <string.h>
#include "fe.h"
#include "fe_internal.h"

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
  const size_t volatile saved_native_reentry_depth = ctx->native_reentry_depth;
  const size_t volatile saved_run_base = ctx->run_base;
  jmp_buf* const volatile saved_condition_catch = ctx->condition_catch;
  if (base > 0) {
    EnterNativeReentry(ctx);
  }
  jmp_buf jump;
  jmp_buf* const volatile saved_catch = BeginRunBarrier(ctx, &jump);

  if (setjmp(jump) != 0) {
    ctx->frame_stack_index = base;
    ctx->call_list = &nil;
    ctx->evaluator_catch = saved_catch;
    ctx->condition_catch = saved_condition_catch;
    ctx->native_reentry_depth = saved_native_reentry_depth;
    ctx->run_base = saved_run_base;
    TransferRunError(ctx, saved_catch);
  }

  PushEvaluationFrame(ctx, obj, env, bind);
  FeObject* const result = RunEvaluationLoop(ctx, base);
  ctx->evaluator_catch = saved_catch;
  ctx->condition_catch = saved_condition_catch;
  ctx->native_reentry_depth = saved_native_reentry_depth;
  if (saved_catch == nullptr) {
    // This run owned the outermost barrier, so its ordinary return reaches
    // the host: leave the accessor reading Normal, whatever a previous
    // error left behind. A nested run never clears here -- during a cleanup
    // drain its sibling runs must not reset the kind the reserve gate is
    // still testing (`AllocateFrame`'s `completion != FeCompletionNormal`),
    // which is the same reason `BeginRunBarrier`'s reset is outermost-only.
    ctx->completion = FeCompletionNormal;
  }
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
FeObject* RunEvaluationBody(FeContext* ctx, FeObject* forms, FeObject* env) {
  const size_t volatile base = ctx->frame_stack_index;
  const size_t volatile saved_native_reentry_depth = ctx->native_reentry_depth;
  const size_t volatile saved_run_base = ctx->run_base;
  jmp_buf* const volatile saved_condition_catch = ctx->condition_catch;
  jmp_buf jump;
  jmp_buf* const volatile saved_catch = BeginRunBarrier(ctx, &jump);

  if (setjmp(jump) != 0) {
    ctx->frame_stack_index = base;
    ctx->call_list = &nil;
    ctx->evaluator_catch = saved_catch;
    ctx->condition_catch = saved_condition_catch;
    ctx->native_reentry_depth = saved_native_reentry_depth;
    ctx->run_base = saved_run_base;
    TransferRunError(ctx, saved_catch);
  }

  PushBodyFrame(ctx, env, forms);
  FeObject* const result = RunEvaluationLoop(ctx, base);
  ctx->evaluator_catch = saved_catch;
  ctx->condition_catch = saved_condition_catch;
  ctx->native_reentry_depth = saved_native_reentry_depth;
  return result;
}

FeObject* Evaluate(FeContext* ctx,
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
  // `nil`, since 04D deleted the transitional value-cell fallback. Outside an
  // active evaluation the per-hop `EvaluationStep` charges are no-ops.
  //
  // A *cyclic* chain is `nil` here too, and this one entry point is the only
  // reader of the chain that does not raise `cyclic-function-indirection`:
  // call position and `funcall`/`apply` still do. That is a host-API design
  // choice rather than a copy of Emacs. Emacs cannot be copied here, because
  // Emacs has no cyclic chain to resolve: `fset` itself signals
  // `cyclic-function-indirection` and leaves the cell untouched, so
  // `indirect-function` never sees one. A C host, meanwhile, cannot catch an
  // Fe error -- `FeHandleError` longjmps to whatever evaluation is running,
  // which for a host resolving a callback name is some *outer* run, or none
  // at all, and a raise from here lands in a C frame that has already
  // returned. The resolver a host calls therefore has to answer rather than
  // raise, and `nil` -- the same answer an empty cell gets -- is that answer;
  // `FeIsFBound` tells the two apart for a host that wants to say which.
  bool cycle = false;
  FeObject* const fn = ResolveFunctionCallable(ctx, sym, &cycle);
  return fn == &unbound ? FeNil(ctx) : fn;
}

bool FeIsFunction(FeContext* ctx, FeObject* obj) {
  // `functionp`'s question, asked of a resolved callable rather than of a
  // name: a symbol is followed through the same designator chain call
  // position uses, so an unbound name is false and a cycle raises
  // `cyclic-function-indirection`, as call position does and unlike
  // `FeGetFunction`. A host that wants the non-raising answer resolves with
  // `FeGetFunction` first and asks this about the result. Macros and special
  // forms are false -- the answer Emacs' own `functionp` gives for `if`,
  // `quote` and `lambda` -- and so is every value that is not callable at
  // all.
  const FeObject* const fn = ResolveFunctionCallable(ctx, obj, nullptr);
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

// The protected call (06A Decision 5's host surface, completed): run
// `callable` with a barrier whose `setjmp` lives *here*, in a frame that is
// live for as long as the call is, and answer with a bool instead of a
// `longjmp`.
//
// This is what a native that re-enters evaluation needs and could not have
// before. Without it, a completion raised by the nested run transfers to the
// *outer* run's barrier, past the native's own C frame: `error_fn` runs only
// at the outermost barrier, so a host `setjmp` inside the native is already
// dead by the time the host's error callback tries to `longjmp` back to it,
// which is undefined -- and in practice corrupts whatever the native was
// holding.
//
// On a non-normal completion the call returns false without calling
// `error_fn` and without unwinding the host's frame at all. The kind, the
// condition object and the message are readable through `FeGetCompletion`,
// `FeGetCondition` and `FeGetCompletionMessage`, and the host then chooses:
//
//   - swallow it (containment -- a hook or a process callback must not take
//     the editor down with it), or
//   - `FeResignal` it into the enclosing run, kind, condition object and
//     message intact, so an enclosing Lisp `condition-case` matches on the
//     original condition symbol.
//
// What is contained is contained completely: the callee's own cleanup
// entries drain (down to the depth this call started at, never past it into
// the caller's), its frames are discarded, the GC stack is restored to its
// entry depth, and the ambient evaluation-control record -- remaining steps
// included -- is put back exactly as it was found. `options` applies to this
// call, not to whatever run it is nested inside; a null `options` leaves the
// caller's ambient limits in place.
bool FeTryCallWithOptions(FeContext* ctx,
                          FeObject* callable,
                          FeObject* const* arguments,
                          size_t count,
                          const FeEvalOptions* options,
                          FeObject** result) {
  FeContext* const volatile ctx_v = ctx;
  FeObject** const volatile result_v = result;
  FeObject* const volatile callable_v = callable;
  FeObject* const* const volatile arguments_v = arguments;
  const size_t volatile count_v = count;
  const FeEvalOptions* const volatile options_v = options;
  const size_t volatile gc = FeSaveGC(ctx);
  const size_t volatile frame_base = ctx->frame_stack_index;
  const size_t volatile saved_run_base = ctx->run_base;
  const size_t volatile saved_reentry = ctx->native_reentry_depth;
  const size_t volatile saved_cleanup_floor = ctx->cleanup_floor;
  // The input unit (sub-plan 12C Part 2). `EvaluateInput` restores it on its
  // own normal return; this is the abnormal path, and it is the one that
  // matters -- a contained failure is exactly how an outer `load` keeps
  // evaluating after an inner one raised, and without this its own
  // let-dynamic marks would stop matching for the rest of the file.
  const size_t volatile saved_input_scope = ctx->input_scope;
  FeObject* const volatile saved_call_list = ctx->call_list;
  jmp_buf* const volatile saved_evaluator_catch = ctx->evaluator_catch;
  jmp_buf* const volatile saved_condition_catch = ctx->condition_catch;
  jmp_buf* const volatile saved_cleanup_catch = ctx->cleanup_catch;
  const FeEvaluationControl saved_control = SaveEvaluationControl(ctx);
  jmp_buf jump;

  // cppcheck-suppress autoVariables
  ctx_v->evaluator_catch = &jump;
  // Nothing inside may bounce out through the enclosing cleanup drain's
  // `setjmp` or escape as a pending throw past this frame: a completion
  // raised in here stops at the barrier above.
  ctx_v->cleanup_catch = nullptr;
  ctx_v->cleanup_floor = ctx_v->cleanup_stack_index;
  ctx_v->completion = FeCompletionNormal;
  ctx_v->condition = &nil;
  // `options` is meant to bound *this* call; `BeginEvaluationControl`
  // refuses while a record is already active, so hand it an inactive one and
  // put the caller's back below, on both exits.
  ctx_v->evaluation_active = false;

  const bool completed = setjmp(jump) == 0;
  if (completed) {
    FeObject* const value =
        FeCallWithOptions(ctx_v, callable_v, arguments_v, count_v, options_v);
    ctx_v->condition = &nil;
    ctx_v->completion = FeCompletionNormal;
    *result_v = value;
  }
  ctx_v->frame_stack_index = frame_base;
  ctx_v->run_base = saved_run_base;
  ctx_v->native_reentry_depth = saved_reentry;
  ctx_v->cleanup_floor = saved_cleanup_floor;
  ctx_v->input_scope = saved_input_scope;
  ctx_v->call_list = saved_call_list;
  ctx_v->evaluator_catch = saved_evaluator_catch;
  ctx_v->condition_catch = saved_condition_catch;
  ctx_v->cleanup_catch = saved_cleanup_catch;
  ctx_v->pending_throw = false;
  ctx_v->pending_throw_tag = FeNil(ctx_v);
  ctx_v->pending_throw_value = FeNil(ctx_v);
  RestoreEvaluationControl(ctx_v, &saved_control);
  FeRestoreGC(ctx_v, gc);
  return completed;
}

// The protected *string* evaluation (11A Decision 5's Shape A), the exact
// sibling of `FeTryCallWithOptions` above with `FeEvaluateStringWithOptions`
// in place of the call. Everything in that function's comment applies here
// unchanged: a non-normal completion is returned rather than thrown past
// this frame, `error_fn` is not called, the kind, condition object and
// message are readable through `FeGetCompletion`/`FeGetCondition`/
// `FeGetCompletionMessage`, and the host then chooses between swallowing it
// and `FeResignal`-ing it into the enclosing run.
//
// This exists because a host that *loads a file* from inside an evaluation
// has the same problem a host that calls a callback has, and could not solve
// it the same way: kg's `lisp_eval_file()` re-enters through
// `FeEvaluateString`, which is a nested run dressed as a top-level call, so
// a completion raised by the loaded text transferred straight to the
// outermost barrier -- past every `condition-case` between the `load` and
// the raise. With this, kg contains the completion, unwinds its own loader
// bookkeeping while the frame is still live, and re-raises with `FeResignal`
// once the enclosing run's floor is back, which is what makes an enclosing
// `condition-case` find it.
//
// A `throw` out of the evaluated text is contained as the barrier-wall error
// it already is: the containment barrier is a throw wall exactly as the
// protected call's is, so a `catch` outside it is not honoured and the throw
// becomes `no-catch`. Making that reach an enclosing `catch` needs `load` to
// be a primitive with a frame kind of its own and is deliberately out of
// scope (11A Decision 5).
//
// The one thing this must NOT do -- and did, until an acceptance review
// measured it -- is save and restore `ctx->evaluation_result` around the
// containment. That field is the *only* root the value handed back in
// `*result` has. `EvaluateInput` (fe.c) assigns it after every form, which
// is what lets `FeEvaluateString`'s result survive the caller's next
// allocation without costing a GC-stack slot per call (doc/c-api.md: "their
// returned value is held by a context-owned root"); `FeTryCallWithOptions`
// is rooted the same way by `ctx->call_result`, and its epilogue likewise
// leaves that field alone. Restoring the field here, and then popping every
// GC-stack slot the evaluation took with `FeRestoreGC`, strips the value of
// every root it has at once: it survives until the next collection and no
// longer, so a caller that allocates anything at all between this returning
// and using `*result` -- the loader shape doc/c-api.md prescribes, which
// unwinds its own bookkeeping first -- reads a cell on the free list.
//
// So the invariant this entry point cannot have is "the field holds the
// enclosing run's own last value". What it has instead is the one the two
// evaluation helpers already publish: the field holds the last value any
// string or file evaluation produced. That costs an enclosing
// `EvaluateInput` nothing, because it re-assigns the field immediately
// after every `FeEvaluate` it makes, so its own return value is unaffected
// however deeply this is nested inside one.
bool FeTryEvaluateStringWithOptions(FeContext* ctx,
                                    const char* label,
                                    const char* source,
                                    size_t length,
                                    const FeEvalOptions* options,
                                    FeObject** result) {
  FeContext* const volatile ctx_v = ctx;
  FeObject** const volatile result_v = result;
  const char* const volatile label_v = label;
  const char* const volatile source_v = source;
  const size_t volatile length_v = length;
  const FeEvalOptions* const volatile options_v = options;
  const size_t volatile gc = FeSaveGC(ctx);
  const size_t volatile frame_base = ctx->frame_stack_index;
  const size_t volatile saved_run_base = ctx->run_base;
  const size_t volatile saved_reentry = ctx->native_reentry_depth;
  const size_t volatile saved_cleanup_floor = ctx->cleanup_floor;
  // The input unit (sub-plan 12C Part 2). `EvaluateInput` restores it on its
  // own normal return; this is the abnormal path, and it is the one that
  // matters -- a contained failure is exactly how an outer `load` keeps
  // evaluating after an inner one raised, and without this its own
  // let-dynamic marks would stop matching for the rest of the file.
  const size_t volatile saved_input_scope = ctx->input_scope;
  FeObject* const volatile saved_call_list = ctx->call_list;
  jmp_buf* const volatile saved_evaluator_catch = ctx->evaluator_catch;
  jmp_buf* const volatile saved_condition_catch = ctx->condition_catch;
  jmp_buf* const volatile saved_cleanup_catch = ctx->cleanup_catch;
  const FeEvaluationControl saved_control = SaveEvaluationControl(ctx);
  jmp_buf jump;

  // cppcheck-suppress autoVariables
  ctx_v->evaluator_catch = &jump;
  ctx_v->cleanup_catch = nullptr;
  ctx_v->cleanup_floor = ctx_v->cleanup_stack_index;
  ctx_v->completion = FeCompletionNormal;
  ctx_v->condition = &nil;
  ctx_v->evaluation_active = false;

  const bool completed = setjmp(jump) == 0;
  if (completed) {
    FeObject* const value = FeEvaluateStringWithOptions(
        ctx_v, label_v, source_v, length_v, options_v);
    ctx_v->condition = &nil;
    ctx_v->completion = FeCompletionNormal;
    *result_v = value;
  }
  ctx_v->frame_stack_index = frame_base;
  ctx_v->run_base = saved_run_base;
  ctx_v->native_reentry_depth = saved_reentry;
  ctx_v->cleanup_floor = saved_cleanup_floor;
  ctx_v->input_scope = saved_input_scope;
  ctx_v->call_list = saved_call_list;
  ctx_v->evaluator_catch = saved_evaluator_catch;
  ctx_v->condition_catch = saved_condition_catch;
  ctx_v->cleanup_catch = saved_cleanup_catch;
  ctx_v->pending_throw = false;
  ctx_v->pending_throw_tag = FeNil(ctx_v);
  ctx_v->pending_throw_value = FeNil(ctx_v);
  RestoreEvaluationControl(ctx_v, &saved_control);
  FeRestoreGC(ctx_v, gc);
  return completed;
}

// The text of the last completion that reached a barrier or the host, fully
// formatted -- source label and all -- exactly as `FeErrorFn` would have been
// given it. Valid until the next completion in this context.
const char* FeGetCompletionMessage(const FeContext* ctx) {
  return ctx->evaluator_error_message;
}

// Puts a contained completion back in flight in the enclosing run, keeping
// the kind, the condition object (`ctx->condition` is untouched by
// containment, so an enclosing `condition-case` still matches on the
// original condition symbol) and the message. The message is already
// formatted, so it is replayed rather than re-prefixed with the source
// label.
//
// Called after `FeTryCallWithOptions` returned false, from the frame that
// made the call. There is nothing to re-signal after a call that succeeded,
// and a `Normal` kind there is treated as an ordinary error rather than
// asserted, because the useful failure mode for a host is a diagnosable
// error and not an abort.
[[noreturn]] void FeResignal(FeContext* ctx) {
  char message[sizeof(ctx->evaluator_error_message)];
  memcpy(message, ctx->evaluator_error_message, sizeof(message));
  message[sizeof(message) - 1] = '\0';
  const FeCompletion kind = ctx->completion == FeCompletionNormal
                                ? FeCompletionError
                                : ctx->completion;
  RaiseCompletionCore(ctx, kind, message);
}
