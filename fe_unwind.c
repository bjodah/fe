// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

// The completion machinery: the ambient evaluation-control record, the
// condition hierarchy and its handler search, the
// unwind-protect/FeProtectWithCleanup cleanup registry, and every raise --
// including the public FeHandleError and FeRaiseCompletion. Split out of
// fe_eval.c along the seam the Makefile's complexity note has named since
// sub-plan 12A; fe_eval.c keeps the frame-driven evaluator itself, fe_run.c
// the run driver, and fe.c the object model, garbage collector, reader and
// writer. All four share the private, self-contained fe_internal.h.
//
// The seam is a data-flow one, not just a line count: nothing here builds or
// resumes an evaluation frame, and the evaluator below the seam reaches this
// file only through the raise helpers, the cleanup pushes and
// `RunCleanupsDownTo`. The two edges back the other way are named in
// fe_internal.h: `PerformThrow`, which `RunOneCleanupEntry` re-issues once
// the enclosing run is back, and `RunEvaluationBody` in fe_run.c, which runs
// a cleanup's own forms.

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fe.h"
#include "fe_internal.h"
#include "fe_perf.h"

FeEvaluationControl SaveEvaluationControl(const FeContext* ctx) {
  return (FeEvaluationControl){
      .interrupt = ctx->evaluation_interrupt,
      .userdata = ctx->evaluation_userdata,
      .steps = ctx->evaluation_steps,
      .poll_interval = ctx->evaluation_poll_interval,
      .poll_countdown = ctx->evaluation_poll_countdown,
      .cleanup_step_limit = ctx->cleanup_step_limit,
      .max_frames_limit = ctx->max_frames_limit,
      .native_reentry_limit = ctx->native_reentry_limit,
      .active = ctx->evaluation_active,
      .limited = ctx->evaluation_limited,
  };
}

void RestoreEvaluationControl(FeContext* ctx,
                              const FeEvaluationControl* control) {
  ctx->evaluation_interrupt = control->interrupt;
  ctx->evaluation_userdata = control->userdata;
  ctx->evaluation_steps = control->steps;
  ctx->evaluation_poll_interval = control->poll_interval;
  ctx->evaluation_poll_countdown = control->poll_countdown;
  ctx->cleanup_step_limit = control->cleanup_step_limit;
  ctx->max_frames_limit = control->max_frames_limit;
  ctx->native_reentry_limit = control->native_reentry_limit;
  ctx->evaluation_active = control->active;
  ctx->evaluation_limited = control->limited;
}

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

// `RunEvaluationBody` -- the body-frame entry point `FeHandleError`, near
// the top of this file for historical reasons, drains pending
// `unwind-protect` forms through -- lives in fe_run.c since sub-plan 11B's
// translation-unit split and is declared in fe_internal.h. A cleanup's forms
// are a nested frame-machine run on the unused suffix of the same frame
// stack, above a saved barrier -- see `RunEvaluationBody`'s own comment --
// not a second evaluator or a separate stack.
//
// `PerformThrow` is the other half of that: `RunOneCleanupEntry` re-issues a
// cleanup's escaping `throw` once it has put the enclosing run's frame
// stack, floor and barriers back. It lives in fe_eval.c, which owns the
// frame stack a throw searches, and is declared in fe_internal.h.
// The two halves of a raise. `RaiseCompletion` applies the ambient
// `error_label`/`error_offset` prefix to `msg` and then hands the finished
// text to `RaiseCompletionCore`, which does the actual work: cleanup drain,
// handler search, transfer. A message that has already been through the
// formatter -- a cleanup's own failure, replayed by `RunOneCleanupEntry`
// after the bounce, or a host's `FeResignal` -- goes straight to the core
// so the label is not prefixed twice.
// The core half is not static: `FeResignal`, in fe_run.c since 11B's split,
// replays an already-formatted message through it.
[[noreturn]] static void RaiseCompletion(FeContext* ctx,
                                         FeCompletion kind,
                                         const char* msg);

// The third column is Phase 19's `error-message` property text, seeded onto
// each symbol when the context opens (`SeedConditionMessages`) and read back
// by `error-message-string`. Every text down to `no-catch` is Emacs
// 31.0.90's own `(get SYMBOL 'error-message)`, measured rather than
// composed -- including the apostrophes in the two `Symbol's ...` messages,
// which are ASCII in the property and which Emacs curls only when it
// RENDERS one (`text-quoting-style`, which fe does not have; recorded as a
// divergence rather than pre-curled here). The last two are fe's own
// conditions and have no Emacs counterpart, so their text is fe policy.
static const ConditionParent condition_parents[] = {
    {"error", nullptr, "error"},
    {"wrong-type-argument", "error", "Wrong type argument"},
    {"wrong-number-of-arguments", "error", "Wrong number of arguments"},
    {"void-variable", "error", "Symbol's value as variable is void"},
    {"void-function", "error", "Symbol's function definition is void"},
    {"args-out-of-range", "error", "Args out of range"},
    // Phase 24. Emacs' own chain and text, measured on 31.0.91: `(get
    // 'end-of-file 'error-conditions)` is `(end-of-file error)` and its
    // `error-message` is "End of file during parsing". It is the condition
    // the READER raises when input stops inside a form -- `[1 2` and `(1 2`
    // alike, which is what Emacs answers for both -- and it arrived with
    // vectors because the vector contract froze the missing-bracket answer
    // by name and the name turned out to be the generic one rather than a
    // vector-specific invention.
    {"end-of-file", "error", "End of file during parsing"},
    // Phase 20. Emacs' own chain and text, measured on 31.0.90: `(get
    // 'end-of-buffer 'error-conditions)` is `(end-of-buffer error)` and its
    // `error-message` is "End of buffer", the same shape for
    // `beginning-of-buffer`. Both are here because kg's motion and editing
    // commands hit a buffer edge constantly and had no name for it: every
    // such site raised a plain `error`, so a handler could only catch it by
    // catching everything. Like `file-missing` below they need no code --
    // the three consumers are `sizeof`-driven loops -- and like it they earn
    // their place by making `(signal 'end-of-buffer nil)` legal at all,
    // since `IsConditionSymbol` gates `signal` on this table.
    {"end-of-buffer", "error", "End of buffer"},
    {"beginning-of-buffer", "error", "Beginning of buffer"},
    // Emacs' own chain and text, measured on 31.0.91: `(get 'search-failed
    // 'error-conditions)` is `(search-failed error)` and its `error-message`
    // is "Search failed". It is the condition Emacs' search family raises
    // when NOERROR is nil, carrying the pattern as its data, and it is here
    // for the same reason the two buffer edges above are: a host that
    // searches text has to be able to raise a failure a handler can name,
    // and `IsConditionSymbol` gates `signal` on this table, so without the
    // row `(signal 'search-failed '("z"))` is not merely unraised but
    // illegal. One data line and no code -- the three consumers of the table
    // are sizeof-driven loops. Fe raises it nowhere; kg's four search names
    // do.
    {"search-failed", "error", "Search failed"},
    {"arith-error", "error", "Arithmetic error"},
    {"file-error", "error", "File error"},
    // Sub-plan 12C Part 1. Emacs' own chain, measured on 31.0.90: `(get
    // 'file-missing 'error-conditions)` is `(file-missing file-error error)`
    // and `(get 'file-error 'error-conditions)` is `(file-error error)`. It
    // is the first two-deep entry in this table, and it needs no code: the
    // three consumers are `sizeof`-driven loops and
    // `FindConditionParentByName` already walks the chain, so `error`
    // catches it through `file-error`. kg's loader and `require` raise it
    // for a missing file (sub-plan 12D); here it earns its place by making
    // `(signal 'file-missing ...)` legal at all, since `IsConditionSymbol`
    // gates `signal` on this table.
    {"file-missing", "file-error", "File is missing"},
    {"cyclic-function-indirection", "error",
     "Symbol's chain of function indirections contains a loop"},
    {"invalid-function", "error", "Invalid function"},
    {"setting-constant", "error", "Attempt to set a constant symbol"},
    {"no-catch", "error", "No catch for tag"},
    {"evaluation-stack-exhaustion", "error", "Evaluation stack exhausted"},
    {"arena-exhaustion", "error", "Arena exhausted"},
    // Phase 23.1's payload region. Its own condition rather than a second
    // arena-exhaustion, because the two name different pools and a handler
    // that wants to shrink what it is building needs to know which one ran
    // out. Nothing in a shipped interpreter can raise it yet -- no release
    // type owns a payload before Phase 25 -- and it is here now because the
    // substrate that raises it is here now.
    {"payload-exhaustion", "error", "Payload region exhausted"},
    // `quit` is a root of its own, as it is in Emacs -- `(get 'quit
    // 'error-conditions)` is `(quit)` there, so `error` does not catch it --
    // and it is here only for its name and its message: `ConditionMatches`
    // decides a quit by the completion KIND before it reads the condition
    // object at all, so this row is never on that path. Phase 19 added it;
    // before that `IsConditionSymbol` named quit in an `if` of its own,
    // which is what this row replaces.
    {"quit", nullptr, "Quit"},
};

const ConditionParent* ConditionRowAt(size_t index) {
  return index < sizeof(condition_parents) / sizeof(condition_parents[0])
             ? &condition_parents[index]
             : nullptr;
}

bool IsConditionSymbol(const FeContext* ctx, const FeObject* symbol) {
  const FeObject* const property = FindInternedSymbol(ctx, "error-conditions");
  return property != nullptr && FeGetType(symbol) == FeTSymbol &&
         !FeIsNil(ReadPlistGet(ReadSymbolPlist(symbol), property));
}

// Whether SYMBOL is ANCESTOR or a subtype of it, walking the same chain
// `ConditionMatches` walks. Phase 19's renderer asks it one question --
// "is this a `file-error`?" -- because that class alone takes its message
// from the DATA rather than from the property, and prints its items with
// `princ` rather than `prin1`.
bool ConditionInheritsFrom(const FeContext* ctx,
                           const FeObject* symbol,
                           const char* ancestor) {
  const FeObject* const property = FindInternedSymbol(ctx, "error-conditions");
  const FeObject* conditions =
      property == nullptr || FeGetType(symbol) != FeTSymbol
          ? &nil
          : ReadPlistGet(ReadSymbolPlist(symbol), property);
  while (!FeIsNil(conditions)) {
    if (IsNamedSymbol(ctx, CAR(conditions), ancestor)) {
      return true;
    }
    conditions = CDR(conditions);
  }
  return false;
}

static bool ConditionMatches(const FeContext* ctx,
                             const FeObject* condition,
                             const FeObject* spec,
                             FeCompletion kind) {
  if (IsNamedSymbol(ctx, spec, "t")) {
    return true;
  }
  if (FeGetType(spec) != FeTSymbol) {
    return false;
  }
  // Quit is decided by the completion *kind*, before anything looks at the
  // condition object: a real C-g arrives through `EvaluationStep`, where
  // there is no signalled condition to walk, and Emacs still lets `(quit
  // ...)` catch it. Testing the object first is what made a genuine
  // interrupt catchable by `t` but not by the handler that names it.
  if (kind == FeCompletionQuit) {
    return IsNamedSymbol(ctx, spec, "quit");
  }
  if (FeGetType(condition) != FeTPair) {
    return false;
  }
  const FeObject* const property = FindInternedSymbol(ctx, "error-conditions");
  const FeObject* conditions =
      property == nullptr
          ? &nil
          : ReadPlistGet(ReadSymbolPlist(CAR(condition)), property);
  while (!FeIsNil(conditions)) {
    if (spec == CAR(conditions)) {
      return true;
    }
    conditions = CDR(conditions);
  }
  return false;
}

static bool HandlerMatches(const FeContext* ctx,
                           const FeObject* condition,
                           const FeObject* spec,
                           FeCompletion kind) {
  if (FeGetType(spec) != FeTPair) {
    return ConditionMatches(ctx, condition, spec, kind);
  }
  while (!FeIsNil(spec)) {
    if (ConditionMatches(ctx, condition, CAR(spec), kind)) {
      return true;
    }
    spec = CDR(spec);
  }
  return false;
}

static bool FindConditionHandler(const FeContext* ctx,
                                 FeCompletion kind,
                                 size_t* index,
                                 FeObject** clause) {
  if (kind == FeCompletionBudget) {
    return false;
  }
  for (size_t i = ctx->frame_stack_index; i-- > ctx->run_base;) {
    FeEvalFrame* frame = &ctx->frame_stack[i];
    if (frame->kind != FeFrameConditionCase || frame->fn == &unbound) {
      continue;
    }
    for (FeObject* handlers = frame->rest; FeGetType(handlers) == FeTPair;
         handlers = CDR(handlers)) {
      FeObject* candidate = CAR(handlers);
      if (FeGetType(candidate) == FeTPair &&
          HandlerMatches(ctx, ctx->condition, CAR(candidate), kind)) {
        *index = i;
        *clause = candidate;
        return true;
      }
    }
  }
  return false;
}

void ValidateConditionHandlers(FeContext* ctx, FeObject* handlers) {
  while (!FeIsNil(handlers)) {
    if (FeGetType(handlers) != FeTPair || FeGetType(CAR(handlers)) != FeTPair) {
      FeHandleError(ctx, "invalid condition handler");
    }
    FeObject* spec = CAR(CAR(handlers));
    if (FeGetType(spec) != FeTSymbol) {
      while (!FeIsNil(spec)) {
        if (FeGetType(spec) != FeTPair || FeGetType(CAR(spec)) != FeTSymbol) {
          FeHandleError(ctx, "invalid condition handler");
        }
        spec = CDR(spec);
      }
    }
    handlers = CDR(handlers);
  }
}

void PushCleanup(FeContext* ctx, FeCleanupEntry entry) {
  if (ctx->cleanup_stack_index == CleanupStackSize) {
    FeHandleError(ctx, "cleanup stack overflow");
  }
  ctx->cleanup_stack[ctx->cleanup_stack_index++] = entry;
  if (ctx->cleanup_stack_index > ctx->arena_peak_cleanup_stack_depth) {
    ctx->arena_peak_cleanup_stack_depth = ctx->cleanup_stack_index;
  }
}

// One shallow dynamic binding (sub-plan 11B, 11A Decision 2): save the
// symbol's current global value -- or its unboundness, which is the same
// `&unbound` object the cell already holds -- as a cleanup entry owing a
// restore, then write the new value into the global cell. Nothing else
// changes: `setq`, `set` and `symbol-value` keep reading and writing that one
// cell, which is why shallow binding makes `(let ((sv 2)) (setq sv 3) (svf))`
// answer 3 (A2a) and a closure read the value in force at *call* time (A5)
// with no further machinery.
//
// The registration comes first so a `cleanup stack overflow` raise leaves the
// cell untouched rather than shadowed with no way back.
//
// The host is asked for its tag BEFORE the cell is read (FE_API_VERSION 11):
// a host whose storage moves answers about the storage the cell holds right
// now, and the whole point of the tag is that this is the moment at which
// that is knowable. `FeSetBindingFns`' contract forbids the callback from
// changing which bindings exist, so `cell` is still this symbol's cell
// afterwards.
void PushDynamicBinding(FeContext* ctx, FeObject* symbol, FeObject* value) {
  const uintptr_t tag =
      ctx->binding_save_fn != nullptr ? ctx->binding_save_fn(ctx, symbol) : 0;
  FeObject* const cell = SymbolBindingCell(symbol);
  PushCleanup(ctx, (FeCleanupEntry){.kind = FeCleanupBinding,
                                    .as.binding = {.symbol = symbol,
                                                   .value = CDR(cell),
                                                   .host_tag = tag}});
  CDR(cell) = value;
}

// The other half, run by `RunCleanups` on every completion kind. Two stores,
// no allocation and nothing that can raise, which is what lets the drain do
// it inline instead of through `RunOneCleanupEntry`'s barrier -- and which
// is why the host callback that may redirect the store lives under the same
// prohibition (see `FeSetBindingFns`).
//
// A null answer from the host is "this storage is gone, drop the value", not
// an error: kg's case is a `let` over a buffer-local binding whose buffer
// was killed inside the form, where Emacs itself discards the saved value
// rather than writing it anywhere.
static void RestoreDynamicBinding(FeContext* ctx, const FeCleanupEntry* entry) {
  FeObject* const target =
      ctx->binding_target_fn != nullptr
          ? ctx->binding_target_fn(ctx, entry->as.binding.symbol,
                                   entry->as.binding.host_tag)
          : entry->as.binding.symbol;
  if (target != nullptr) {
    CDR(SymbolBindingCell(target)) = entry->as.binding.value;
  }
}

// Copies the completion raised while a cleanup entry was itself running
// into context-owned storage, since the formatted message `RaiseCompletion`
// is about to `longjmp` away from lives on that frame's stack and would
// otherwise be gone by the time `RunOneCleanupEntry` reads it. The *kind*
// travels with the text: a cleanup that runs out of its own budget or
// answers a second C-g must reach the host as Budget or Quit, not silently
// become an ordinary Error on the way back out.
static void SaveCleanupCompletion(FeContext* ctx,
                                  FeCompletion kind,
                                  const char* msg) {
  const size_t capacity = sizeof(ctx->cleanup_error_message) - 1;
  size_t length = 0;
  while (length < capacity && msg[length] != '\0') {
    length++;
  }
  memcpy(ctx->cleanup_error_message, msg, length);
  ctx->cleanup_error_message[length] = '\0';
  ctx->cleanup_error_kind = kind;
}

// The evaluation control a cleanup drain re-arms fresh for every entry it
// runs, captured from the ambient record by the raise that started the
// drain. `step_limit` of 0 means "the host did not set
// `FeEvalOptions.cleanup_step_limit`," resolved to `DefaultCleanupStepLimit`
// where it is used, not here, since the resolved value does not need to
// survive a `longjmp`.
typedef struct FeCleanupBudget {
  FeInterruptFn* interrupt;
  void* userdata;
  size_t poll_interval;
  size_t step_limit;
} FeCleanupBudget;

// Runs one cleanup entry. A cleanup that itself raises does not reach
// `error_fn` -- that would let the host `longjmp` away and abandon the rest
// of the stack -- so `RaiseCompletion` redirects here instead (see
// `cleanup_catch`), and this function replays the completion in the
// *enclosing* context, where the frame stack, the run floor and the
// barriers are the ones that were live before the drain started. 06A
// Decision 4: the cleanup's own completion replaces whatever was already
// unwinding, so the replay is an ordinary raise and can be caught by an
// enclosing `condition-case`.
//
// `budget`, when non-null, is the fresh bounded control record this one
// entry runs under (see the drain's own comment); the ambient record is
// saved here and put back on every exit path, so a drain leaves the
// interrupted program's remaining steps exactly as it found them.
static void RunOneCleanupEntry(FeContext* ctx,
                               const FeCleanupEntry* entry,
                               const FeCleanupBudget* budget) {
  FeContext* const volatile ctx_v = ctx;
  const FeCleanupEntry* const volatile entry_v = entry;
  jmp_buf local_jump;
  jmp_buf* const volatile saved_catch = ctx_v->cleanup_catch;
  jmp_buf* const volatile saved_evaluator_catch = ctx_v->evaluator_catch;
  const size_t volatile saved_frame_stack_index = ctx_v->frame_stack_index;
  const size_t volatile saved_native_reentry_depth =
      ctx_v->native_reentry_depth;
  const size_t volatile saved_run_base = ctx_v->run_base;
  FeObject* const volatile saved_call_list = ctx_v->call_list;
  jmp_buf* const volatile saved_condition_catch = ctx_v->condition_catch;
  const size_t volatile saved_cleanup_frame_floor = ctx_v->cleanup_frame_floor;
  // Saved and put back like every other ambient field: an entry that
  // *handles* its own raise leaves the completion Normal (the handler
  // transfer does), and the drain this entry belongs to still needs its own
  // kind afterwards -- `AllocateFrame`'s `CleanupFrameReserve` gate and
  // `FePushGC`'s reserve both read it as "a completion is in flight", and
  // the entries after this one must not lose that.
  const FeCompletion volatile saved_completion = ctx_v->completion;
  const FeEvaluationControl saved_control = SaveEvaluationControl(ctx_v);
  if (budget != nullptr) {
    ctx_v->evaluation_interrupt = budget->interrupt;
    ctx_v->evaluation_userdata = budget->userdata;
    ctx_v->evaluation_poll_interval = budget->poll_interval;
    ctx_v->evaluation_poll_countdown = budget->poll_interval;
    ctx_v->evaluation_limited = true;
    ctx_v->evaluation_steps =
        budget->step_limit != 0 ? budget->step_limit : DefaultCleanupStepLimit;
    ctx_v->evaluation_active = true;
  }
  ctx_v->cleanup_catch = &local_jump;
  ctx_v->cleanup_frame_floor = ctx_v->frame_stack_index;
  if (setjmp(local_jump) == 0) {
    if (entry_v->kind == FeCleanupNative) {
      entry_v->as.native.fn(ctx_v, entry_v->as.native.data);
    } else {
      RunEvaluationBody(ctx_v, entry_v->as.lisp.forms, entry_v->as.lisp.env);
    }
  } else {
    ctx_v->cleanup_catch = saved_catch;
    ctx_v->cleanup_frame_floor = saved_cleanup_frame_floor;
    ctx_v->completion = saved_completion;
    ctx_v->evaluator_catch = saved_evaluator_catch;
    ctx_v->native_reentry_depth = saved_native_reentry_depth;
    ctx_v->run_base = saved_run_base;
    ctx_v->condition_catch = saved_condition_catch;
    ctx_v->frame_stack_index = saved_frame_stack_index;
    ctx_v->call_list = saved_call_list;
    RestoreEvaluationControl(ctx_v, &saved_control);
    if (ctx_v->pending_throw) {
      // The cleanup threw to a tag whose catch frame is not inside the
      // cleanup's own run. Everything above is back to what the enclosing
      // run had, so re-issuing the throw here searches the frames that were
      // live before the drain started -- which is where the catch is. It
      // either delivers (and resumes that run's loop below), escapes again
      // to a still-further-out cleanup, or finds nothing anywhere and
      // raises `no-catch`.
      ctx_v->pending_throw = false;
      FeObject* const tag = ctx_v->pending_throw_tag;
      FeObject* const value = ctx_v->pending_throw_value;
      ctx_v->pending_throw_tag = &nil;
      ctx_v->pending_throw_value = &nil;
      (void)PerformThrow(ctx_v, tag, value);
      // Delivered: the catch frame holds the value and every frame above it
      // is gone, so abandon the drain -- and whatever raise or completing
      // form started it -- and resume the loop that owns the catch. This is
      // how a cleanup's throw replaces the completion already unwinding,
      // the same way a cleanup's error does.
      longjmp(*ctx_v->condition_catch, 1);
    }
    // Already through the label formatter once, on the way in: replay it
    // verbatim rather than prefixing the source label a second time.
    RaiseCompletionCore(ctx_v, ctx_v->cleanup_error_kind,
                        ctx_v->cleanup_error_message);
  }
  ctx_v->cleanup_catch = saved_catch;
  ctx_v->cleanup_frame_floor = saved_cleanup_frame_floor;
  ctx_v->completion = saved_completion;
  // A cleanup error longjmps directly here, bypassing the nested
  // RunEvaluation() that installed its own barrier. Do not leave that
  // automatic jmp_buf, its frames, or its trace link live in the context.
  ctx_v->evaluator_catch = saved_evaluator_catch;
  ctx_v->frame_stack_index = saved_frame_stack_index;
  ctx->native_reentry_depth = saved_native_reentry_depth;
  ctx->run_base = saved_run_base;
  ctx->condition_catch = saved_condition_catch;
  ctx->call_list = saved_call_list;
  RestoreEvaluationControl(ctx, &saved_control);
}

// Drains cleanup entries down to (but not including) `target`, most recently
// pushed first.
//
// With `budget` null this is the ordinary-return drain: a call form that
// completes normally runs what it pushed under whatever budget was already
// ambient -- the same one the rest of the program is running under, nothing
// special.
//
// With `budget` non-null it is the abnormal drain, after an error, a host
// interrupt, or step-budget exhaustion: every call form still on the C
// stack is being abandoned, so every pending cleanup down to `target` must
// run before the completion reaches its handler or the host. Each entry
// gets its own fresh copy of `budget`, not one shared across the whole
// drain and not the exhausted or cancelled control the body was running
// under: a body that ran out of steps still gets a working cleanup (the
// budget is new), and a cleanup that does not return terminates on its own
// instead of hanging with no escape (the budget is bounded). Interrupt
// polling stays live on the same re-armed schedule, so a second host
// interrupt during a runaway cleanup aborts that one entry -- caught by its
// own `RunOneCleanupEntry`, like any other cleanup failure -- without a
// stale poll countdown from the body either firing on the first step or
// (via unsigned underflow) never firing again.
static void RunCleanups(FeContext* ctx,
                        size_t target,
                        const FeCleanupBudget* budget) {
  while (ctx->cleanup_stack_index > target) {
    const FeCleanupEntry entry = ctx->cleanup_stack[--ctx->cleanup_stack_index];
    if (entry.kind == FeCleanupBinding) {
      // A dynamic binding's restore (11B): two stores that cannot raise and
      // evaluate nothing, so they do not need -- and must not pay for -- a
      // barrier, a fresh control record or a budget of their own. This is
      // the single point at which "restored on all five completion kinds"
      // holds: every drain in the evaluator goes through here.
      RestoreDynamicBinding(ctx, &entry);
      continue;
    }
    RunOneCleanupEntry(ctx, &entry, budget);
  }
}

void RunCleanupsDownTo(FeContext* ctx, size_t target) {
  RunCleanups(ctx, target, nullptr);
}

void FeProtectWithCleanup(FeContext* ctx, FeCleanupFn* fn, void* data) {
  PushCleanup(ctx, (FeCleanupEntry){.kind = FeCleanupNative,
                                    .as.native = {.fn = fn, .data = data}});
}

[[noreturn]] static void TransferEvaluationError(FeContext* ctx,
                                                 const char* msg,
                                                 FeObject* trace) {
  // Recorded for every completion that reaches a barrier or the host, not
  // only the ones that `longjmp`: `FeGetCompletionMessage` is how a host
  // that contained a completion with `FeTryCallWithOptions` reads the text,
  // and `FeResignal` is how it puts the same text back in flight. The
  // self-copy guard is for exactly that replay, whose `msg` is a copy of
  // this buffer.
  if (msg != ctx->evaluator_error_message) {
    const size_t length = strlen(msg);
    assert(length < sizeof(ctx->evaluator_error_message));
    memcpy(ctx->evaluator_error_message, msg, length + 1);
  }
  ctx->evaluator_error_trace = trace;
  if (ctx->evaluator_catch == nullptr) {
    if (ctx->error_fn != nullptr) {
      ctx->error_fn(ctx, msg, trace);
    }
    abort();
  }
  longjmp(*ctx->evaluator_catch, 1);
}

// Sub-plan 09C's mark-phase contract, broken. Lives here rather than in fe.c
// because this translation unit already reports to `stderr`; `fe.c` reaches
// it through `fe_internal.h`.
//
// The two violations that end here both leave the arena in the mark phase's
// working state, which is not a state anything can continue from: pointer
// reversal keeps the walk's return path inside the objects it is walking, so
// a `car` chain the walk is inside holds tagged parent pointers, not its own
// cars. A raise `longjmp`s past the ascent that would put them back; an
// allocation from a callback re-enters `CollectGarbage`, whose sweep would
// clear the outer walk's mark bits and send it back down into the reversed
// cells. Neither is recoverable, and neither is detectable later by anything
// except a fault somewhere unrelated -- so this names the contract and stops
// here, where the evidence still points at the callback.
//
// `error_fn` is deliberately not consulted: an `error_fn` leaves non-locally,
// which is the failure being reported.
[[noreturn]] void FatalCollectorViolation(const char* what,
                                          const char* detail) {
  fputs("fe: fatal: ", stderr);
  fputs(what, stderr);
  fputs(" from inside garbage collection.\n", stderr);
  if (detail != nullptr) {
    fputs("fe: detail: ", stderr);
    fputs(detail, stderr);
    fputc('\n', stderr);
  }
  fputs(
      "fe: a mark_fn or gc_fn callback must return normally and must not\n"
      "fe: allocate; see FeSetMarkFn in fe.h and doc/c-api.md.\n",
      stderr);
  abort();
}

// The core of every completion raise (sub-plan 06B of kg's Emacs-subset
// program): assigns the completion kind, then does what `FeHandleError` has
// always done -- drain the cleanup registry under a fresh budget and transfer
// control to the enclosing barrier or the host. The kind is the parameter so
// the four wall sites (step limit, interrupt, frame limit, native re-entry
// limit) can raise Quit/Budget without touching the shared body, while every
// ordinary raise -- including the public `FeHandleError` wrapper below -- is
// an Error completion. Assignment happens for every raise, including a
// host-level `FeHandleError` outside an evaluator run, so the accessor never
// leaves a stale kind behind. The reserve gate in `AllocateFrame`
// (`completion != FeCompletionNormal`) still means "a cleanup drain is in
// progress" because only an evaluator barrier can reach that cleanup path.
//
// `msg` is final: the label prefix has already been applied by
// `RaiseCompletion`, or deliberately not applied because the text carries
// one from a previous pass through it.
//
// The ambient control record is *not* cleared on the way in. The three exits
// need three different answers and each one is stated where it happens: a
// caught condition resumes the interrupted program and must keep its
// remaining steps, its frame wall and its interrupt (clearing them here is
// what let one caught condition disarm evaluation for the rest of the run);
// a cleanup drain re-arms its own fresh bounded budget per entry inside
// `RunOneCleanupEntry` and puts the ambient record back afterwards; and only
// the host exit clears, keeping the long-standing guarantee that a host sees
// an inactive record.
[[noreturn]] void RaiseCompletionCore(FeContext* ctx,
                                      FeCompletion kind,
                                      const char* msg) {
  // A raise from inside the collector cannot be honoured (sub-plan 09C's
  // contract, stated in `fe.h` at `FeSetMarkFn` and in `doc/c-api.md`).
  // Every route out of here `longjmp`s, and the mark phase keeps its return
  // path in the objects it is walking, so a jump past its ascent leaves a
  // `car` chain holding tagged parent pointers instead of its own cars --
  // and the next reader of that chain dereferences one. There is nothing to
  // recover: the state that would un-reverse the graph is the graph.
  if (ctx->collecting) {
    FatalCollectorViolation("a completion was raised", msg);
  }
  FeObject* cl = ctx->call_list;
  // A longjmp abandons the C activation that published this record. Clearing
  // the identity too, rather than only the flag, keeps `native_identity`'s
  // "live object or nil" invariant (`CollectGarbage` marks it) instead of
  // leaving the abandoned native's callable behind as a stale root.
  //
  // This is not the whole story: the completion may be *caught*, by a
  // handler in a run this native started, in which case the native below is
  // still on the C stack and its record has to come back.
  // `RunEvaluationLoop` restores it after its own `setjmp` for exactly that
  // case -- see the comment there.
  ctx->native_call_active = false;
  ctx->native_identity = &nil;
  ctx->native_argc = 0;
  // The fresh budget every cleanup entry below runs under, taken from the
  // ambient record: a bounded, working budget even when the record it comes
  // from is the exhausted or cancelled one the body just died on.
  const FeCleanupBudget cleanup_budget = {
      .interrupt = ctx->evaluation_interrupt,
      .userdata = ctx->evaluation_userdata,
      .poll_interval = ctx->evaluation_poll_interval,
      .step_limit = ctx->cleanup_step_limit,
  };
  // Reset ambient reader/evaluation state before either sort of cleanup runs.
  // The old trace stays in `cl`; a cleanup starts with a fresh visible trace.
  ctx->call_list = &nil;
  ctx->nextchr = '\0';

  ctx->completion = kind;

  // The handler search runs *before* the `cleanup_catch` bounce below, and
  // `cleanup_frame_floor` is what makes that safe (sub-plan 12B Part 1,
  // 12A Decision 2). The two were the other way round, so while any cleanup
  // entry ran, every raise took the bounce unconditionally and the cleanup's
  // own handler frames were never examined: `(unwind-protect 'body
  // (condition-case c (car 6) (error 'handled)))` is `body` in Emacs 31.0.90
  // and escaped to the host here, drain or no drain. What changes is handler
  // *visibility inside a cleanup*, and nothing else -- a handler below the
  // floor still loses to the bounce, so an unhandled cleanup raise still
  // replaces the completion being unwound and is still caught by an
  // *enclosing* `condition-case` (06A Decision 4), which is measured
  // byte-identical to Emacs for the error, quit and throw unwinds alike.
  size_t handler_index;
  FeObject* handler;
  if (FindConditionHandler(ctx, kind, &handler_index, &handler) &&
      handler_index >= ctx->cleanup_frame_floor) {
    FeEvalFrame* const frame = &ctx->frame_stack[handler_index];
    // The condition that is unwinding is held in a local, not re-read from
    // `ctx->condition`, across the drain below: a cleanup entry can run
    // arbitrary Lisp, and a host native it calls can *contain* a completion
    // of its own (`FeTryCallWithOptions`, `FeTryEvaluateStringWithOptions`),
    // which leaves the contained condition in that field for the host to
    // read. Without this the handler bound the contained call's condition
    // object instead of the one in flight -- handler selection was right,
    // because it is decided above, and only the object `e` named was wrong.
    // The push is the same root it always was: the field is the object's
    // only root, and the field is exactly what a containment overwrites.
    FeObject* const in_flight = ctx->condition;
    FePushGC(ctx, in_flight);
    RunCleanups(ctx, frame->cleanup_checkpoint, &cleanup_budget);
    FeRestoreGC(ctx, frame->gc_checkpoint);
    FePushGC(ctx, in_flight);
    ctx->condition = in_flight;
    frame->fn = handler;
    frame->callee = in_flight;
    ctx->frame_stack_index = handler_index + 1;
    ctx->call_list = &frame->trace_cell;
    ctx->completion = FeCompletionNormal;
    // The control record is exactly the one the body was running under,
    // minus the steps the body spent: the handler and everything after it
    // continue inside the same budget, wall and interrupt.
    longjmp(*ctx->condition_catch, 1);
  }

  if (ctx->cleanup_catch != nullptr) {
    // A cleanup entry's own `fn` or unwind-forms raised, and nothing the
    // entry itself established can handle it. Resume at
    // `RunOneCleanupEntry`'s `setjmp` instead of reaching the host: that
    // keeps unwinding the cleanup stack instead of abandoning it, and lets
    // the replay happen in the enclosing context, where an enclosing
    // `condition-case` can see it (06A Decision 4: the cleanup's completion
    // replaces the one already in flight). Kind and text travel together.
    SaveCleanupCompletion(ctx, kind, msg);
    longjmp(*ctx->cleanup_catch, 1);
  }

  // Nothing here can catch it, so the whole registry drains (down to
  // `cleanup_floor`, which a protected host call raises to its own base) and
  // the host takes over. Only now is the record cleared, and only now is the
  // source label dropped -- keeping a label alive past the host boundary
  // would prefix a later, unrelated raise with a stale file name.
  //
  // The kind and the condition object are held across the drain for the
  // reason the handler path holds them, and for one more: what the host
  // reads through `FeGetCompletion` and `FeGetCondition` after this arrives
  // must describe *this* completion, not one a cleanup entry contained on
  // the way out. The GC-stack slot is this object's only root while the
  // field that normally holds it is at the mercy of the drain, and it is
  // popped again before the local is put back, so the drain leaves the
  // stack exactly as deep as it found it.
  FeObject* const in_flight = ctx->condition;
  const size_t drain_gc = FeSaveGC(ctx);
  FePushGC(ctx, in_flight);
  RunCleanups(ctx, ctx->cleanup_floor, &cleanup_budget);
  FeRestoreGC(ctx, drain_gc);
  ctx->condition = in_flight;
  ctx->completion = kind;
  ClearEvaluationControl(ctx);
  // And only now is the input unit left. This is the whole of the fix for
  // the Phase 12 fix cycle's blocker: `EvaluateInput` puts the enclosing
  // unit back on its normal return and the two containment barriers put it
  // back on a contained abnormal exit, but an UNCONTAINED raise -- one that
  // reaches the host `error_fn` through `FeEvaluateStringWithOptions` or
  // `FeEvaluateFileWithOptions` -- `longjmp`s past both, and left the
  // abandoned unit's scope number in the context for the life of the
  // context. There is nothing to restore here (every unit between the raise
  // and the host is being abandoned at once, which is what the drain above
  // just did), so this leaves for the host context, which is where control
  // is actually going.
  EnterHostInputContext(ctx);

  TransferEvaluationError(ctx, msg, cl);
}

// Applies the ambient source label and reader offset to `msg` and raises.
// Every raise from Lisp or from a native goes through here; the only callers
// of the core directly are the ones replaying an already-formatted message.
[[noreturn]] static void RaiseCompletion(FeContext* ctx,
                                         FeCompletion kind,
                                         const char* msg) {
  char message[1024];
  switch ((ctx->error_label != nullptr) * 4 + ctx->error_has_line * 2 +
          ctx->error_has_offset) {
    // A known line wins over a byte offset, so `label + line` prints the same
    // way whether or not a reader offset also happens to be live: cases 7 and
    // 6 are one arm, not two identical ones.
    case 7:
    case 6:
      Format(message, sizeof(message), "%s:%zu: %s", ctx->error_label,
             ctx->error_line, msg);
      msg = message;
      break;
    case 4:
      Format(message, sizeof(message), "%s: %s", ctx->error_label, msg);
      msg = message;
      break;
    case 5:
      Format(message, sizeof(message), "%s:%zu: %s", ctx->error_label,
             ctx->error_offset, msg);
      msg = message;
      break;
    case 1:
      Format(message, sizeof(message), "byte %zu: %s", ctx->error_offset, msg);
      msg = message;
      break;
    default:
      break;
  }
  RaiseCompletionCore(ctx, kind, msg);
}

// The three host-configured ceilings (step budget, frame wall, native
// re-entry) raise Budget, which Emacs has no counterpart for and which
// `condition-case` deliberately cannot catch. There is no condition object
// to construct for one, so the field is cleared rather than left holding
// whatever the last error signalled: `FeGetCondition` must never answer with
// a stale object for a completion that has none.
[[noreturn]] void RaiseBudget(FeContext* ctx, const char* msg) {
  ctx->condition = &nil;
  RaiseCompletion(ctx, FeCompletionBudget, msg);
}

// Publishes one of the two pre-built exhaustion conditions (09B), after
// putting it back the way `FeOpenContext` built it.
//
// The objects are shared for the life of the context and a handler receives
// the object itself, so `(condition-case e BIG (error (setcar e 'poisoned)))`
// used to disable the whole mechanism permanently: measured, the *next*
// out-of-memory then escaped `(error ...)` and `(arena-exhaustion ...)`
// alike, because `ConditionMatches` reads the condition's `car` to find its
// place in the hierarchy. `(setcdr e (list 9 9 9))` was the other half --
// the next unrelated exhaustion signalled `(arena-exhaustion 9 9 9)`, with
// that list rooted for good through a context-lifetime root.
//
// Re-stamping here is the whole defence, and it fits the one constraint
// these objects exist under: two stores, no allocation, so it is still safe
// on the path where allocation has already failed. It runs before every
// publish rather than after every catch, because a handler is not the only
// thing that can reach the object -- `FeGetCondition` hands it to the host
// too -- and because "the object is correct when it is signalled" is a
// property one place can own.
static void PublishExhaustion(FeContext* ctx,
                              FeObject* condition,
                              FeObject* name) {
  CAR(condition) = name;
  CDR(condition) = &nil;
  ctx->condition = condition;
}

// The public raise entry point: an ordinary Error completion, which is what
// every one of the 100+ call sites through fe.c/fex_*.c and kg's 112 raise
// sites means. The signature is pinned (kg calls it directly); the four wall
// sites that need a different kind call `RaiseCompletion` directly instead.
[[noreturn]] void FeHandleError(FeContext* ctx, const char* msg) {
  // An exhausted arena cannot allocate its own condition object, so it
  // signals the one `FeOpenContext` already built: `(arena-exhaustion)`,
  // whose parent in the hierarchy is `error`. Before 09B this set the field
  // to nil, and `ConditionMatches` -- which must walk a pair to reach the
  // hierarchy -- then answered false for every named handler, so an
  // out-of-memory escaped `(condition-case e BIG (error ...))` entirely and
  // only `(t ...)` could contain it. `ArenaCanAllocate` -- not a bare
  // free-list test -- is what "exhausted" means here; see its own comment.
  if (!ArenaCanAllocate(ctx)) {
    PublishExhaustion(ctx, ctx->arena_exhaustion_condition,
                      ctx->arena_exhaustion_name);
    RaiseCompletion(ctx, FeCompletionError, msg);
  }
  RaiseCondition(ctx, FeCompletionError, "error",
                 FeMakeList(ctx, (FeObject*[]){FeMakeString(ctx, msg)}, 1),
                 msg);
}

[[noreturn]] void RaiseNamedError(FeContext* ctx,
                                  const char* name,
                                  const char* message) {
  RaiseCondition(ctx, FeCompletionError, name, &nil, message);
}

// The payload region is full and the compacting collection `PublishPayload`
// ran did not free enough of it. Naming that takes cells -- `RaiseNamedError`
// builds a condition object -- so when the cell pool is spent too this
// degrades to the one condition a state with no cells can signal, the
// pre-built `(arena-exhaustion)`. Both are catchable by name and both are
// caught by `error`, which is the whole point of the degradation being a
// different condition rather than a bare message.
[[noreturn]] void RaisePayloadExhaustion(FeContext* ctx) {
  static const char message[] = "payload region exhausted";
  if (!ArenaCanAllocate(ctx)) {
    PublishExhaustion(ctx, ctx->arena_exhaustion_condition,
                      ctx->arena_exhaustion_name);
    RaiseCompletion(ctx, FeCompletionError, message);
  }
  RaiseNamedError(ctx, "payload-exhaustion", message);
}

// The condition data is deliberately built in one place.  Keep both values
// rooted while the two list allocations run: callers commonly pass a symbol
// or a freshly resolved callable that has no other reference at this point.
[[noreturn]] void RaiseNativeArity(FeContext* ctx,
                                   FeObject* function,
                                   size_t argc,
                                   const char* message) {
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, function);
  FeObject* count = FeMakeInteger(ctx, (int64_t)argc);
  FePushGC(ctx, count);
  FeObject* data = FeMakeList(ctx, (FeObject*[]){function, count}, 2);
  FeRestoreGC(ctx, gc);
  RaiseCondition(ctx, FeCompletionError, "wrong-number-of-arguments", data,
                 message);
}

[[noreturn]] void RaiseWrongNumber(FeContext* ctx,
                                   FeObject* function,
                                   size_t argc) {
  RaiseNativeArity(ctx, function, argc, "wrong-number-of-arguments");
}

// The one raise that may not allocate. Every allocation pushes a root onto
// the very stack that has just filled, so the ordinary route out
// (`FeHandleError` -> `FeMakeString` -> `MakeObject` -> `FePushGC`) re-enters
// the overflow check and recurses until the C stack dies -- which is how a
// 1500-argument `&rest` call reached SIGSEGV instead of an error. This
// completion therefore carries `(evaluation-stack-exhaustion)`, the second
// condition `FeOpenContext` interned and consed once (09B) precisely so that
// signalling it costs no allocation at all; nothing on the way to
// `RaiseCompletion` allocates either. Before 09B the field was set to nil
// here, which made the overflow catchable only by `(t ...)`.
//
// `GcStackReserve` slots stay free for the unwind itself, so the raise below
// can root what it needs -- and it now needs strictly less than it did, since
// the condition object it signals already exists. Past the reserve there is
// nothing left to unwind with and no way to widen a fixed array, so that case
// takes the same host-notify-and-abort exit `RaiseCompletion` takes when no
// barrier can catch at all.
[[noreturn]] void RaiseGcStackOverflow(FeContext* ctx) {
  if (ctx->gc_stack_index >= GcStackSize) {
    if (ctx->error_fn != nullptr) {
      ctx->error_fn(ctx, "GC stack overflow while unwinding", &nil);
    }
    abort();
  }
  PublishExhaustion(ctx, ctx->evaluation_stack_exhaustion_condition,
                    ctx->evaluation_stack_exhaustion_name);
  RaiseCompletion(ctx, FeCompletionError, "GC stack overflow");
}

// `wrong-type-argument` with Emacs' `(PREDICATE VALUE)` data. `CheckType`
// builds the same shape from the type it wanted; this is for the sites that
// know the predicate by name because they accept more than one type.
[[noreturn]] void RaiseWrongType(FeContext* ctx,
                                 const char* predicate,
                                 FeObject* value) {
  FeObject* items[] = {FeMakeSymbol(ctx, predicate), value};
  RaiseCondition(ctx, FeCompletionError, "wrong-type-argument",
                 FeMakeList(ctx, items, sizeof(items) / sizeof(items[0])),
                 "wrong-type-argument");
}

// The host's way to raise a completion that is not an ordinary error: a
// `quit` because the embedder's own C-g arrived somewhere fe cannot poll, or
// a `budget` because the embedder's own ceiling tripped. See `doc/c-api.md`.
//
// The two kinds a host may not raise are refused rather than half-honoured.
// `FeCompletionNormal` is not a raise at all, and `FeCompletionThrow` is an
// evaluator-internal state that only ever exists between a matching catch
// frame being found and the value being delivered to it: raising it from
// outside would leave the frame stack claiming a delivery that never
// happens. Both are asserted, and treated as an ordinary error when
// assertions are compiled out, because a raise cannot return to report a
// bad argument.
[[noreturn]] void FeRaiseCompletion(FeContext* ctx,
                                    FeCompletion kind,
                                    const char* msg) {
  assert(kind == FeCompletionError || kind == FeCompletionQuit ||
         kind == FeCompletionBudget);
  if (kind == FeCompletionQuit) {
    RaiseCondition(ctx, FeCompletionQuit, "quit", &nil, msg);
  }
  if (kind == FeCompletionBudget) {
    RaiseBudget(ctx, msg);
  }
  FeHandleError(ctx, msg);
}

// Decision 5's (sub-plan 06A) additive host surface. The kind is always
// valid: the value `RaiseCompletion` assigned, which survives the host's
// recovery `longjmp` and stays readable until the next run's outermost
// barrier resets it (or a normal top-level return clears it -- see
// `RunEvaluation`). A host polls `FeGetCompletion` from inside its error
// callback to tell quit/budget from a genuine error without reading message
// strings, and reads the same answer after it recovers.
FeCompletion FeGetCompletion(const FeContext* ctx) {
  return ctx->completion;
}

// Nil only for a completion with no condition to describe: a Budget (which
// has no name in Emacs and which `condition-case` cannot catch), a Quit
// raised from an exhausted arena, or Normal. An Error always has one -- an
// exhausted arena signals a pre-built `(arena-exhaustion)` or
// `(evaluation-stack-exhaustion)` rather than nil since 09B.
FeObject* FeGetCondition(const FeContext* ctx) {
  return ctx->condition;
}

[[noreturn]] void RaiseCondition(FeContext* ctx,
                                 FeCompletion kind,
                                 const char* name,
                                 FeObject* data,
                                 const char* message) {
  const size_t gc = FeSaveGC(ctx);
  // `data` is rooted first: it is usually a list the caller has just built
  // and nothing else refers to, and both `ArenaCanAllocate`'s collection and
  // `FeMakeSymbol`'s can sweep it otherwise.
  FePushGC(ctx, data);
  // Under memory pressure a *named* condition cannot be built, so an Error
  // falls back to the pre-built `(arena-exhaustion)` rather than to nil
  // (09B): the handler then sees a truthful "this became an out-of-memory"
  // instead of an object it cannot match on at all. Quit keeps the nil
  // object, and needs no fallback: `ConditionMatches` decides quit by
  // completion *kind* before it ever looks at the object's shape, so
  // `(quit ...)` still catches, and claiming an interrupt was an arena
  // exhaustion would be the one thing worse than saying nothing.
  if (!ArenaCanAllocate(ctx)) {
    FeRestoreGC(ctx, gc);
    if (kind == FeCompletionError) {
      PublishExhaustion(ctx, ctx->arena_exhaustion_condition,
                        ctx->arena_exhaustion_name);
    } else {
      ctx->condition = &nil;
    }
    RaiseCompletion(ctx, kind, message);
  }
  FeObject* symbol = FeMakeSymbol(ctx, name);
  FePushGC(ctx, symbol);
  ctx->condition = FeCons(ctx, symbol, data);
  FeRestoreGC(ctx, gc);
  RaiseCompletion(ctx, kind, message);
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
  FE_PERF_INC(FePerfEvalStep);
  if (ctx->evaluation_limited) {
    if (ctx->evaluation_steps == 0) {
      // Step-budget exhaustion is a Budget completion (06B): the host set a
      // ceiling and the program hit it. Message text is pinned verbatim.
      RaiseBudget(ctx, "evaluation step limit exceeded");
    }
    ctx->evaluation_steps--;
  }
  if (ctx->evaluation_interrupt != nullptr &&
      --ctx->evaluation_poll_countdown == 0) {
    ctx->evaluation_poll_countdown = ctx->evaluation_poll_interval;
    if (ctx->evaluation_interrupt(ctx, ctx->evaluation_userdata)) {
      // The interrupt path is a Quit completion (06B): the user asked to
      // stop, which the host must be able to tell from a genuine error.
      // It carries a real `(quit)` condition object, the same one
      // `(signal 'quit nil)` builds, so a host or a `(quit ...)` handler
      // never reads a stale object left by an earlier error.
      RaiseCondition(ctx, FeCompletionQuit, "quit", &nil,
                     "evaluation cancelled");
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
void EnterNativeReentry(FeContext* ctx) {
  const size_t limit = ctx->native_reentry_limit != 0
                           ? ctx->native_reentry_limit
                           : DefaultNativeReentry;
  if (ctx->native_reentry_depth >= limit) {
    // The re-entry wall is a Budget completion (06B), grouped with the
    // other host-configured ceilings: the parent's resource-exhaustion rule
    // puts it beside the step and frame limits, not beside ordinary errors.
    RaiseBudget(ctx, "native evaluation re-entry limit exceeded");
  }
  ctx->native_reentry_depth++;
  if (ctx->native_reentry_depth > ctx->arena_peak_native_reentry) {
    ctx->arena_peak_native_reentry = ctx->native_reentry_depth;
  }
}
