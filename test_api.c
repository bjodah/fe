#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fe.h"
#include "fe_internal.h"

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "check failed: %s\n", #condition);                     \
      printf("check failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
      fflush(stdout);                                                        \
      return false;                                                          \
    }                                                                        \
  } while (false)

typedef struct InterruptState {
  FeContext* context;
  void* expected_userdata;
  size_t polls;
  size_t cancel_after;
  // A second trigger point, for tests that need the interrupt to fire once
  // during the body and again later, during a cleanup that keeps running
  // past the first cancellation. 0 (every existing caller's default) never
  // matches, since `polls` is incremented before the comparison and so is
  // never 0 when checked.
  size_t cancel_after_second;
  bool userdata_seen;
} InterruptState;

typedef struct ErrorState {
  jmp_buf jump;
  FeContext* context;
  const char* expected_message;
  InterruptState* nested_interrupt;
  FeRoot* root;
  bool nested_with_options;
  bool called;
  bool stack_was_nil;
  // 03D stage 5 native-re-entry bookkeeping (renamed for 03F's two-bounds
  // split): how many more nested `FeCallWithOptions` hops `ReentrantNative`
  // may take before returning (so the test never needs the default nested
  // C calls to prove an off-by-one), the `max_native_reentry` it re-enters
  // under, the rooted self-callable for the re-entering native, the
  // callable `OwningReenter` invokes through its owning nested call,
  // whether a registered cleanup ran, and the nesting depth observed on the
  // C side. `reentry_current` is the level of the currently running
  // activation (incremented on entry, restored on the ordinary return) and
  // `reentry_max_seen` the high-water mark; the deepest activation reports
  // its own level as the result, so a success case's value *is* the nesting
  // depth. Error paths longjmp past the decrements, so the test resets both
  // fields before each run it asserts on.
  size_t reentry_remaining;
  size_t reentry_max_native_reentry;
  FeRoot* reentry_self;
  FeRoot* reentry_owning_target;
  bool reentry_cleanup_ran;
  size_t reentry_current;
  size_t reentry_max_seen;
  // Sub-plan 06B: the completion kind the error callback observed, recorded by
  // `HandleError` for `TestCompletionKinds`. Every pre-existing test ignores
  // these fields, which is itself part of the no-behaviour-change proof.
  FeCompletion observed_completion;
  bool completion_seen;
} ErrorState;

// The frame partition must leave the existing 200-level C-stack probe below
// its physical frame wall while retaining TestWriter's 400-cell list.
// TestContextCreation still checks every byte around the true minimum.
enum { TestArenaSize = 1024 * 1024 };

typedef struct TestArena {
  alignas(max_align_t) unsigned char bytes[TestArenaSize];
} TestArena;

static ErrorState error_states[2];

static bool IsRendered(FeContext* context,
                       FeObject* object,
                       const char* expected) {
  char rendered[64];
  (void)FeToString(context, object, rendered, sizeof(rendered));
  return strcmp(rendered, expected) == 0;
}

[[noreturn]] static void HandleError(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    const char* message,
    // cppcheck-suppress constParameterCallback
    FeObject* stack) {
  ErrorState* state = FeGetUserData(context);
  // Every expectation is written out in full, including the `file:LINE:`
  // prefix the raise really carries. An earlier version of this handler
  // inserted `:1` into any expectation that lacked a line number, which made
  // the line part of a message unassertable -- and its "does this already
  // have a line?" test only recognised single-digit lines, so an expectation
  // for line 10 or later was silently corrupted into `file:10:1: ...`.
  const char* expected = state->expected_message;
  state->called = state->context == context && strcmp(message, expected) == 0;
  if (!state->called && state->context == context) {
    fprintf(stderr,
            "unexpected error message\n  expected: %s\n  actual:   %s\n",
            expected, message);
  }
  state->stack_was_nil = FeIsNil(stack);
  state->observed_completion = FeGetCompletion(context);
  state->completion_seen = true;
  longjmp(state->jump, 1);
}

static bool TestContextCreation(void) {
  // The three version handles a downstream pins against move together and are
  // asserted together: the two macros are compile-time (test_header.c states
  // them for the header on its own), `FeVersion` is a runtime string and can
  // only be checked here.
  static_assert(FE_API_VERSION == 6);
  static_assert(FE_LANGUAGE_VERSION == 9);
  CHECK(strcmp(FeVersion, "10.0") == 0);

  const size_t minimum = FeMinimumArenaSize();
  const size_t alignment = FeArenaAlignment();
  CHECK(minimum > 0);
  CHECK(alignment > 0);
  CHECK(FeOpenContext(nullptr, minimum) == nullptr);

  size_t allocation_size;
  CHECK(!ckd_add(&allocation_size, minimum, alignment));
  static TestArena storage;
  unsigned char* arena = storage.bytes;
  CHECK(allocation_size <= sizeof(storage.bytes));
  CHECK((uintptr_t)arena % alignment == 0);
  CHECK(FeOpenContext(arena + 1, minimum) == nullptr);
  CHECK(FeOpenContext(arena, SIZE_MAX) == nullptr);

  const size_t first = minimum > alignment ? minimum - alignment : 0;
  const size_t last = allocation_size;
  for (size_t size = first; size <= last; size++) {
    FeContext* context = FeOpenContext(arena, size);
    CHECK((context != nullptr) == (size >= minimum));
    if (context != nullptr) {
      FeCloseContext(context);
    }
  }

  for (size_t i = 0; i < 4; i++) {
    FeContext* context = FeOpenContext(arena, minimum);
    CHECK(context != nullptr);
    FeCloseContext(context);
  }
  return true;
}

static bool TriggerAndRecover(FeContext* context, ErrorState* state) {
  const size_t gc = FeSaveGC(context);
  if (setjmp(state->jump) == 0) {
    (void)FeCar(context, FeMakeDouble(context, 1));
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  CHECK(state->stack_was_nil);
  CHECK(!FeIsNil(FeMakeBool(context, true)));
  return true;
}

static bool ExpectEvaluationError(FeContext* context,
                                  ErrorState* state,
                                  const char* label,
                                  const char* source,
                                  size_t length,
                                  const char* expected) {
  FeContext* const volatile context_v = context;
  ErrorState* const volatile state_v = state;
  const size_t volatile gc = FeSaveGC(context);
  state_v->called = false;
  state_v->expected_message = expected;
  if (setjmp(state_v->jump) == 0) {
    (void)FeEvaluateString(context_v, label, source, length);
    CHECK(false);
  }
  FeRestoreGC(context_v, gc);
  CHECK(state_v->called);
  return true;
}

static bool ExpectEvaluationOptionsError(FeContext* context,
                                         ErrorState* state,
                                         const char* label,
                                         const char* source,
                                         size_t length,
                                         const FeEvalOptions* options,
                                         const char* expected) {
  FeContext* const volatile context_v = context;
  ErrorState* const volatile state_v = state;
  const size_t volatile gc = FeSaveGC(context);
  state_v->called = false;
  state_v->expected_message = expected;
  if (setjmp(state_v->jump) == 0) {
    (void)FeEvaluateStringWithOptions(context_v, label, source, length,
                                      options);
    CHECK(false);
  }
  FeRestoreGC(context_v, gc);
  CHECK(state_v->called);
  return true;
}

// Sub-plan 06B's expectation helper: like `ExpectEvaluationOptionsError`,
// but additionally pins the completion kind the host's error callback must
// observe, the same kind read *after* recovery (the accessor stays valid
// until the next run's outermost barrier resets it), and the nil condition
// object that is all 06B ever produces. Every existing test goes through the
// helper above and never sees these checks.
static bool ExpectCompletionKind(FeContext* context,
                                 ErrorState* state,
                                 const char* label,
                                 const char* source,
                                 size_t length,
                                 const FeEvalOptions* options,
                                 const char* expected_message,
                                 FeCompletion expected_kind) {
  FeContext* const volatile context_v = context;
  ErrorState* const volatile state_v = state;
  const size_t volatile gc = FeSaveGC(context);
  state_v->called = false;
  state_v->completion_seen = false;
  state_v->expected_message = expected_message;
  if (setjmp(state_v->jump) == 0) {
    (void)FeEvaluateStringWithOptions(context_v, label, source, length,
                                      options);
    CHECK(false);
  }
  FeRestoreGC(context_v, gc);
  CHECK(state_v->called);
  CHECK(state_v->completion_seen);
  CHECK(state_v->observed_completion == expected_kind);
  // The kind survives the host's recovery longjmp: nothing resets it until
  // the next run's outermost barrier or a normal top-level return.
  CHECK(FeGetCompletion(context_v) == expected_kind);
  // The condition object is deliberate for every kind, never left over from
  // an earlier completion: an error and a quit both have one (a quit's is
  // `(quit)`, the object `(signal 'quit nil)` builds), and Budget -- the one
  // kind with no Emacs counterpart and nothing to construct -- has none.
  CHECK(FeIsNil(FeGetCondition(context_v)) ==
        (expected_kind == FeCompletionBudget));
  return true;
}

static bool Interrupt(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    void* userdata) {
  InterruptState* state = userdata;
  state->userdata_seen =
      state->context == context && state->expected_userdata == userdata;
  state->polls++;
  return state->polls == state->cancel_after ||
         state->polls == state->cancel_after_second;
}

static FeObject* ReenterEvaluation(FeContext* context, FeObject* arguments) {
  (void)arguments;
  const ErrorState* state = FeGetUserData(context);
  const FeEvalOptions options = {.poll_interval = 1,
                                 .interrupt = Interrupt,
                                 .userdata = state->nested_interrupt};
  static const char source[] = "(while t 1)";
  if (!state->nested_with_options) {
    return FeEvaluateString(context, "inner.fe", source, sizeof(source) - 1);
  }
  return FeEvaluateStringWithOptions(context, "inner.fe", source,
                                     sizeof(source) - 1, &options);
}

static FeObject* ReenterCallWithOptions(
    FeContext* context,
    // cppcheck-suppress constParameterCallback
    FeObject* arguments) {
  (void)arguments;
  const ErrorState* state = FeGetUserData(context);
  const FeEvalOptions options = {.poll_interval = 1,
                                 .interrupt = Interrupt,
                                 .userdata = state->nested_interrupt};
  return FeCallWithOptions(context, FeGetRoot(state->root), nullptr, 0,
                           &options);
}

// Sub-plan 06C's native re-entry boundary wall, exercised from a native that
// synchronously starts a nested evaluator run which throws. The nested run's
// frame-stack floor sits above every catch frame the outer run holds, so the
// throw search stops at that floor and raises no-catch in the nested run
// instead of honouring the outer catch -- recorded as a divergence.
static FeObject* ReenterThrow(FeContext* context,
                              // cppcheck-suppress constParameterCallback
                              FeObject* arguments) {
  (void)arguments;
  static const char source[] = "(throw 'outer-tag 42)";
  return FeEvaluateString(context, "nested.fe", source, sizeof(source) - 1);
}

// The positive control for the wall: a catch entirely inside the nested run
// is honoured -- re-entry only bounds the search, it does not disable
// catch/throw within the run that holds the catch.
static FeObject* ReenterCatchThrow(FeContext* context,
                                   // cppcheck-suppress constParameterCallback
                                   FeObject* arguments) {
  (void)arguments;
  static const char source[] = "(catch 'inner-tag (throw 'inner-tag 1))";
  return FeEvaluateString(context, "nested.fe", source, sizeof(source) - 1);
}

// The protected-call natives. `ContainCall` is hook-shaped: it runs the
// callable it was handed inside `FeTryCallWithOptions` and swallows whatever
// comes back, which is what kg's hook dispatch and process callbacks must do
// -- an init file's broken hook cannot be allowed to take the editor's own
// evaluation down with it. `WrapCall` is wrapper-shaped: it does its own
// unwinding work and then `FeResignal`s, so an enclosing Lisp
// `condition-case` still matches on the original condition symbol.
static FeCompletion contained_kind;
static bool contained_gc_balanced;
static bool contained_message_seen;
static bool wrap_cleanup_ran;

static FeObject* ContainCall(FeContext* context, FeObject* arguments) {
  FeObject* callable = FeGetNextArgument(context, &arguments);
  FeObject* value = FeNil(context);
  const size_t gc = FeSaveGC(context);
  if (FeTryCallWithOptions(context, callable, nullptr, 0, nullptr, &value)) {
    FeObject* items[] = {FeMakeSymbol(context, "ok"), value};
    return FeMakeList(context, items, 2);
  }
  // Everything the host is promised on the false path, asserted from inside
  // the very frame that would have been skipped by a `longjmp`.
  contained_gc_balanced = FeSaveGC(context) == gc;
  contained_kind = FeGetCompletion(context);
  contained_message_seen = FeGetCompletionMessage(context)[0] != '\0';
  FeObject* items[] = {FeMakeSymbol(context, "contained"),
                       FeGetCondition(context)};
  return FeMakeList(context, items, 2);
}

static FeObject* WrapCall(FeContext* context, FeObject* arguments) {
  FeObject* callable = FeGetNextArgument(context, &arguments);
  FeObject* value = FeNil(context);
  if (FeTryCallWithOptions(context, callable, nullptr, 0, nullptr, &value)) {
    return value;
  }
  wrap_cleanup_ran = true;
  FeResignal(context);
}

// `FeRaiseCompletion`'s three legal kinds, one native each: a host raising
// its own quit (its C-g arrived somewhere fe cannot poll), its own ceiling,
// and an ordinary error.
[[noreturn]] static FeObject* RaiseHostQuit(FeContext* context,
                                            // cppcheck-suppress
                                            // constParameterCallback
                                            FeObject* arguments) {
  (void)arguments;
  FeRaiseCompletion(context, FeCompletionQuit, "host quit");
}

[[noreturn]] static FeObject* RaiseHostBudget(FeContext* context,
                                              // cppcheck-suppress
                                              // constParameterCallback
                                              FeObject* arguments) {
  (void)arguments;
  FeRaiseCompletion(context, FeCompletionBudget, "host budget");
}

[[noreturn]] static FeObject* RaiseHostError(FeContext* context,
                                             // cppcheck-suppress
                                             // constParameterCallback
                                             FeObject* arguments) {
  (void)arguments;
  FeRaiseCompletion(context, FeCompletionError, "host error");
}

static FeObject* AddExactly(FeContext* context, FeObject* arguments) {
  const double x = FeToDouble(context, FeGetNextArgument(context, &arguments));
  const double y = FeToDouble(context, FeGetNextArgument(context, &arguments));
  FeRequireNoArguments(context, arguments);
  return FeMakeDouble(context, x + y);
}

static void MarkReentryCleanup(FeContext* context, void* data) {
  (void)context;
  ErrorState* state = data;
  state->reentry_cleanup_ran = true;
}

// 03D stage 5, renamed for 03F: a native that synchronously re-enters
// evaluation through `FeCallWithOptions` on itself, bounded on the C side by
// `reentry_remaining` (so the test drives a deliberately small
// `max_native_reentry` instead of the default nested C calls) and on the Fe
// side by the ambient `max_native_reentry`. Registers a host cleanup on
// every entry, so both the ordinary return and the error paths are observed
// to run it. The deepest activation (the one that exhausted the budget)
// returns its own nesting level, so a success case's result directly
// reports how many activations were live at the deepest point.
static FeObject* ReentrantNative(FeContext* context,
                                 // cppcheck-suppress constParameterCallback
                                 FeObject* arguments) {
  FeRequireNoArguments(context, arguments);
  ErrorState* state = FeGetUserData(context);
  state->reentry_current++;
  if (state->reentry_current > state->reentry_max_seen) {
    state->reentry_max_seen = state->reentry_current;
  }
  FeProtectWithCleanup(context, MarkReentryCleanup, state);
  if (state->reentry_remaining == 0) {
    const double level = (double)state->reentry_current;
    state->reentry_current--;
    return FeMakeDouble(context, level);
  }
  state->reentry_remaining--;
  const FeEvalOptions options = {.max_native_reentry =
                                     state->reentry_max_native_reentry};
  FeObject* result = FeCallWithOptions(context, FeGetRoot(state->reentry_self),
                                       nullptr, 0, &options);
  state->reentry_current--;
  return result;
}

// 03D stage 5 regression: an ordinary native with no re-entry, and a native
// that performs one *owning* `FeCallWithOptions` (so the test drives it from
// a plain, non-owning `FeEvaluateString`, where no control record is active
// and the nested call takes ownership). The owning call's
// `EndEvaluationControl` clears the ambient limits (`max_frames_limit`,
// `native_reentry_limit`) before this native returns, but 03F deliberately
// leaves `native_reentry_depth` itself alone -- it is a live count, not a
// configured ceiling -- so there is no counter here left needing a restore
// the way the pre-03F shared depth counter did.
static FeObject* OrdinaryNative(FeContext* context,
                                // cppcheck-suppress constParameterCallback
                                FeObject* arguments) {
  FeRequireNoArguments(context, arguments);
  return FeMakeDouble(context, 42);
}

static FeObject* OwningReenter(FeContext* context,
                               // cppcheck-suppress constParameterCallback
                               FeObject* arguments) {
  FeRequireNoArguments(context, arguments);
  const ErrorState* state = FeGetUserData(context);
  const FeEvalOptions options = {.max_native_reentry =
                                     state->reentry_max_native_reentry};
  return FeCallWithOptions(context, FeGetRoot(state->reentry_owning_target),
                           nullptr, 0, &options);
}

// 03D stage 5: holds its one argument across a nested evaluation that
// forces collections, then hands the same value through one nested
// `FeCallWithOptions`, proving the native frame roots the evaluated
// argument list (`accumulator`) while a native is active and while a nested
// run it starts allocates over it.
static FeObject* GCNative(FeContext* context, FeObject* arguments) {
  ErrorState* state = FeGetUserData(context);
  FeObject* value = FeGetNextArgument(context, &arguments);
  FeRequireNoArguments(context, arguments);
  if (state->reentry_remaining == 0) {
    return value;
  }
  state->reentry_remaining--;
  static const char collecting[] =
      "(setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) n";
  (void)FeEvaluateString(context, "native-gc.fe", collecting,
                         sizeof(collecting) - 1);
  const FeEvalOptions options = {.max_native_reentry =
                                     state->reentry_max_native_reentry};
  FeObject* callarg = value;
  return FeCallWithOptions(context, FeGetRoot(state->reentry_self), &callarg, 1,
                           &options);
}

// The C-stack high-water probe (03A of kg's Emacs-subset program). Records
// its own C frame address, invoked from the deepest point of a Lisp
// recursion, so `TestEvaluationStackProbe` can compare a "depth zero" call
// against a "depth n" one and see whether Lisp-level nesting is costing C
// stack. One process-wide slot is fine: `test_api` is single-threaded and
// every caller reads the value immediately after the evaluation that set it.
//
// A single record is not enough for a recursion that fires the probe at
// several depths (03D's computed-head chain probes on the way back out): the
// *last* fire is the shallowest. `stack_probe_deepest_address` therefore
// keeps the lowest address seen -- on the supported toolchains (the same
// documented constraint that makes `__builtin_frame_address(0)` valid here)
// the C stack grows downward, so the minimum address is the high-water mark.
static uintptr_t stack_probe_last_address;
static uintptr_t stack_probe_deepest_address;

// Records the caller's C frame address. `__builtin_frame_address(0)`, never
// a nonzero depth (unsupported and warns), and never the address of a
// `volatile` local: AddressSanitizer may place an address-taken local on its
// fake stack, which would turn the ASan row into a heap-layout measurement
// rather than a C-stack one. The intermediate `void*` avoids casting a
// function call's result straight to an integer type, which
// `-Wbad-function-cast` rejects.
static void RecordStackProbeAddress(void) {
  void* const frame = __builtin_frame_address(0);
  const uintptr_t address = (uintptr_t)frame;
  stack_probe_last_address = address;
  if (stack_probe_deepest_address == 0 ||
      address < stack_probe_deepest_address) {
    stack_probe_deepest_address = address;
  }
}

static FeObject* StackProbe(FeContext* context,
                            // cppcheck-suppress constParameterCallback
                            FeObject* arguments) {
  FeRequireNoArguments(context, arguments);
  RecordStackProbeAddress();
  // Zero is part of the fixture: `deep` sums 1 per level on the way back
  // out, so `(deep n)` returning anything but `n` means the probe ran at
  // the wrong point in the evaluation and the stack assertion below would
  // otherwise still look fine by accident.
  return FeMakeDouble(context, 0);
}

// Like `StackProbe`, but for argument-position recursion (03D stage 2): it
// takes one argument and returns it, so a chain of native calls can be
// generated as `(probe-id (probe-id ... (probe-id 0)))` with the probe
// firing once, at the deepest C point.
static FeObject* ProbeId(FeContext* context,
                         // cppcheck-suppress constParameterCallback
                         FeObject* arguments) {
  FeObject* value = FeGetNextArgument(context, &arguments);
  FeRequireNoArguments(context, arguments);
  RecordStackProbeAddress();
  return value;
}

static FeObject* CallRoot(FeContext* context,
                          // cppcheck-suppress constParameterCallback
                          FeObject* arguments) {
  FeRequireNoArguments(context, arguments);
  const ErrorState* state = FeGetUserData(context);
  return FeCall(context, FeGetRoot(state->root), nullptr, 0);
}

static bool ExpectReadError(FeContext* context,
                            ErrorState* state,
                            const char* source,
                            size_t length,
                            const char* expected) {
  const size_t gc = FeSaveGC(context);
  size_t offset = 0;
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeReadString(context, source, length, &offset);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool ExpectFileError(FeContext* context,
                            ErrorState* state,
                            FILE* file,
                            const char* expected) {
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeEvaluateFile(context, "nul-file.fe", file);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool ExpectStringError(FeContext* context,
                              ErrorState* state,
                              const FeObject* object,
                              const char* expected) {
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeStringByteLength(context, object);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool ExpectCallError(FeContext* context,
                            ErrorState* state,
                            FeObject* callable,
                            const char* expected) {
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeCall(context, callable, nullptr, 0);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool ExpectCallWithOptionsError(FeContext* context,
                                       ErrorState* state,
                                       FeObject* callable,
                                       FeObject* const* arguments,
                                       size_t count,
                                       const FeEvalOptions* options,
                                       const char* expected) {
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeCallWithOptions(context, callable, arguments, count, options);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool ExpectReleaseError(FeContext* context,
                               ErrorState* state,
                               FeRoot* root,
                               const char* expected) {
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    FeReleaseRoot(context, root);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool ExpectIntegerError(FeContext* context,
                               ErrorState* state,
                               FeObject* object,
                               const char* expected) {
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeToInteger(context, object);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool TestUserDataAndErrors(void) {
  static TestArena arenas[2];
  const size_t size = sizeof(arenas[0].bytes);
  CHECK(size > FeMinimumArenaSize());

  FeContext* contexts[2] = {FeOpenContext(arenas[0].bytes, size),
                            FeOpenContext(arenas[1].bytes, size)};
  CHECK(contexts[0] != nullptr);
  CHECK(contexts[1] != nullptr);

  for (size_t i = 0; i < 2; i++) {
    error_states[i] =
        (ErrorState){.context = contexts[i],
                     .expected_message = "expected pair, got double"};
    FeSetUserData(contexts[i], &error_states[i]);
    FeSetErrorFn(contexts[i], HandleError);
    CHECK(FeGetUserData(contexts[i]) == &error_states[i]);
  }
  CHECK(TriggerAndRecover(contexts[0], &error_states[0]));
  CHECK(error_states[0].completion_seen);
  CHECK(error_states[0].observed_completion == FeCompletionError);
  CHECK(FeGetCompletion(contexts[0]) == FeCompletionError);
  CHECK(!error_states[1].called);
  CHECK(TriggerAndRecover(contexts[1], &error_states[1]));
  CHECK(error_states[1].completion_seen);
  CHECK(error_states[1].observed_completion == FeCompletionError);
  CHECK(FeGetCompletion(contexts[1]) == FeCompletionError);

  for (size_t i = 0; i < 2; i++) {
    FeCloseContext(contexts[i]);
  }
  return true;
}

static bool TestStringInput(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  const char forms[] = "(setq x 1) (setq x (+ x 2)) x";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "forms.fe", forms, sizeof(forms) - 1),
      "3"));
  CHECK(FeIsNil(FeEvaluateString(context, "empty.fe", "", 0)));

  const char bounded[] = {'4', '2', '(', 'c', 'a', 'r', ' ', '1', ')'};
  size_t offset = 0;
  CHECK(IsRendered(context, FeReadString(context, bounded, 2, &offset), "42"));
  CHECK(offset == 2);
  CHECK(FeReadString(context, bounded, 2, &offset) == nullptr);

  const char sequential[] = "1 2";
  offset = 0;
  CHECK(IsRendered(
      context,
      FeReadString(context, sequential, sizeof(sequential) - 1, &offset), "1"));
  CHECK(offset == 1);
  CHECK(IsRendered(
      context,
      FeReadString(context, sequential, sizeof(sequential) - 1, &offset), "2"));
  CHECK(FeReadString(context, sequential, sizeof(sequential) - 1, &offset) ==
        nullptr);

  CHECK(ExpectReadError(context, &state, "(", 1, "byte 1: unclosed list"));
  const char truncated_string[] = {'"', '\\'};
  CHECK(ExpectReadError(context, &state, truncated_string,
                        sizeof(truncated_string), "byte 2: unclosed string"));

  const char with_nul[] = {'1', '\0', '2'};
  CHECK(ExpectEvaluationError(context, &state, "nul.fe", with_nul,
                              sizeof(with_nul), "nul.fe:1: embedded NUL byte"));
  CHECK(ExpectEvaluationError(context, &state, "syntax.fe", "1 (+ 2 3", 8,
                              "syntax.fe:1: unclosed list"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "after-syntax.fe", "6", 1), "6"));
  CHECK(ExpectEvaluationError(context, &state, "runtime.fe", "1 (car 2) 3", 11,
                              "runtime.fe:1: expected pair, got integer"));
  CHECK(IsRendered(context, FeReadString(context, "?a", 2, nullptr), "97"));
  CHECK(IsRendered(context, FeReadString(context, "?\\n", 3, nullptr), "10"));
  CHECK(IsRendered(context, FeReadString(context, "?\\t", 3, nullptr), "9"));
  CHECK(IsRendered(context, FeReadString(context, "?\\e", 3, nullptr), "27"));
  CHECK(IsRendered(context, FeReadString(context, "?\\\\", 3, nullptr), "92"));
  CHECK(IsRendered(context, FeReadString(context, "?\\s", 3, nullptr), "32"));
  CHECK(IsRendered(context, FeReadString(context, "?\\d", 3, nullptr), "127"));
  CHECK(IsRendered(context, FeReadString(context, "?\\x41", 5, nullptr), "65"));
  CHECK(IsRendered(context, FeReadString(context, "?\\101", 5, nullptr), "65"));
  CHECK(IsRendered(context, FeReadString(context, "?\xC3\xA9", 3, nullptr),
                   "233"));
  CHECK(IsRendered(context, FeReadString(context, "?\xE2\x82\xAC", 4, nullptr),
                   "8364"));
  CHECK(IsRendered(context,
                   FeReadString(context, "?\xF0\x90\x80\x80", 5, nullptr),
                   "65536"));
  CHECK(IsRendered(
      context, FeReadString(context, "?\\C-a", sizeof("?\\C-a") - 1, nullptr),
      "1"));
  CHECK(IsRendered(
      context, FeReadString(context, "?\\M-a", sizeof("?\\M-a") - 1, nullptr),
      "134217825"));
  CHECK(IsRendered(context, FeReadString(context, "#x10", 4, nullptr), "16"));
  CHECK(IsRendered(context, FeReadString(context, "#o17", 4, nullptr), "15"));
  CHECK(IsRendered(context, FeReadString(context, "#b101", 5, nullptr), "5"));
  CHECK(IsRendered(context, FeReadString(context, "#x-10", 5, nullptr), "-16"));
  CHECK(IsRendered(context,
                   FeReadString(context, "#x7fffffffffffffff", 18, nullptr),
                   "9223372036854775807"));
  CHECK(IsRendered(context,
                   FeReadString(context, "#x8000000000000000", 18, nullptr),
                   "9.223372036854776e+18"));
  CHECK(IsRendered(context,
                   FeReadString(context, "\"\\x41\\101\\e\\d\\s\"",
                                sizeof("\"\\x41\\101\\e\\d\\s\"") - 1, nullptr),
                   "AA\x1b\x7f "));
  CHECK(ExpectReadError(context, &state, "#q", 2,
                        "byte 1: unsupported read syntax: #"));
  CHECK(ExpectReadError(context, &state, "[1 2]", 5,
                        "byte 0: unsupported read syntax: vector brackets"));
  CHECK(ExpectReadError(context, &state, "\"\\q\"", 4,
                        "byte 2: unsupported read syntax: unknown escape"));
  CHECK(ExpectReadError(context, &state, "?\\q", 3,
                        "byte 2: unsupported read syntax: unknown escape"));
  CHECK(ExpectEvaluationError(context, &state, "lines.fe", "\n(car 2)", 8,
                              "lines.fe:2: expected pair, got integer"));
  CHECK(ExpectEvaluationError(
      context, &state, "lines.fe", "\n[", 2,
      "lines.fe:2: unsupported read syntax: vector brackets"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "after-runtime.fe", "7", 1), "7"));

  const size_t gc = FeSaveGC(context);
  FeObject* result = &nil;
  for (size_t i = 0; i < 32; i++) {
    result = FeEvaluateString(context, "repeat.fe", "1 9", 3);
    CHECK(FeSaveGC(context) == gc);
  }
  CHECK(IsRendered(context, result, "9"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 08C's headline is "nothing is silently misread", so every row
// below is one of two things: a spelling Fe reads to the value GNU Emacs
// 31.0.90 reads it to (measured with `emacs -Q --batch` against
// /opt-3/emacs-31-lucid, TERM=xterm-256color), or a spelling Fe refuses with
// an error that names the syntax. Nothing in between.
static bool TestReaderLiterals(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define READS(source, expected)                                                \
  CHECK(IsRendered(context,                                                    \
                   FeReadString(context, source, sizeof(source) - 1, nullptr), \
                   expected))
#define REJECTS(source, message)                                    \
  CHECK(ExpectEvaluationError(context, &state, "reader.fe", source, \
                              sizeof(source) - 1, "reader.fe:1: " message))

  // The control modifier. Emacs' rule is not `& 0x1f`: `?` is DEL, `@`..`_`
  // and `a`..`z` fold, and every other character -- punctuation, digits,
  // space, non-ASCII, and the value of a nested escape -- keeps its value
  // with the 2^26 control bit. `& 0x1f` answered 31 for `?\C-?` and 5 for
  // `?\C-%`.
  READS("?\\C-?", "127");
  READS("?\\C-%", "67108901");
  READS("?\\C-@", "0");
  READS("?\\C-A", "1");
  READS("?\\C-a", "1");
  READS("?\\C-_", "31");
  READS("?\\C-z", "26");
  READS("?\\C-1", "67108913");
  READS("?\\C-s", "19");
  READS("?\\C-\xC3\xA9", "67109097");
  READS("?\\C-\\n", "67108874");
  READS("?\\C-\\t", "67108873");
  READS("?\\C-\\d", "67108991");
  READS("?\\M-a", "134217825");
  READS("?\\M-\\n", "134217738");
  READS("?\\M-\\C-a", "134217729");
  READS("?\\C-\\M-a", "134217729");

  // The accepted plain set.
  READS("?a", "97");
  READS("? ", "32");
  READS("?\\n", "10");
  READS("?\\t", "9");
  READS("?\\e", "27");
  READS("?\\\\", "92");
  READS("?\\s", "32");
  READS("?\\d", "127");
  READS("?\\0", "0");
  READS("?\\1", "1");
  READS("?\\101", "65");
  READS("?\xC3\xA9", "233");

  // `\x` is greedy and variable-width in Emacs. Two fixed digits read
  // `?\x41f` as 65 with an `f` left over and `?\x0041` as 4 with `1` left
  // over -- both silent misreads.
  READS("?\\x41", "65");
  READS("?\\x41f", "1055");
  READS("?\\x0041", "65");
  READS("\"\\x0041\"", "A");
  READS("\"\\x41\\101\\e\\d\\s\"", "AA\x1b\x7f ");
  READS("\"\\377\"", "\xff");

  // Radix integers, including both bounds of the int64 window: INT64_MIN is
  // the one magnitude that only fits with the sign applied, and the first
  // magnitude past UINT64_MAX takes the recorded pre-bignum double fallback.
  READS("#x10", "16");
  READS("#xff", "255");
  READS("#o17", "15");
  READS("#b101", "5");
  READS("#x-10", "-16");
  READS("#x7fffffffffffffff", "9223372036854775807");
  READS("#x-8000000000000000", "-9223372036854775808");
  READS("#x8000000000000000", "9.223372036854776e+18");
  READS("#xFFFFFFFFFFFFFFFF", "1.8446744073709552e+19");

  // Emacs ends a `?` literal at a delimiter and reads each of these as
  // `invalid-read-syntax`. Without that rule they were one character plus a
  // leftover token: `(?\s-a)` read as `(32 -a)` where Emacs reads
  // `(8388705)`, and `(?\1a)` and `(?ab)` read as two forms each.
  REJECTS("(?\\s-a)", "unsupported read syntax: \\s character modifier");
  REJECTS("(?\\1a)", "unsupported read syntax: ? literal without delimiter");
  REJECTS("(?ab)", "unsupported read syntax: ? literal without delimiter");
  REJECTS("?\\S-a", "unsupported read syntax: \\S character modifier");
  REJECTS("?\\A-a", "unsupported read syntax: \\A character modifier");
  REJECTS("?\\H-a", "unsupported read syntax: \\H character modifier");
  REJECTS("?\\^a", "unsupported read syntax: \\^ character modifier");
  REJECTS("?\\C-\\C-a",
          "unsupported read syntax: duplicate character modifier");
  REJECTS("?\\M-\\M-a",
          "unsupported read syntax: duplicate character modifier");
  REJECTS("?\\Ca", "unsupported read syntax: malformed character modifier");
  REJECTS("?\\q", "unsupported read syntax: unknown escape");
  REJECTS("?", "unsupported read syntax: ? at end of input");
  REJECTS("?\\x", "unsupported read syntax: \\x");
  REJECTS("?\\x110000", "unsupported read syntax: \\x character out of range");
  REJECTS("?\\x3fffffff",
          "unsupported read syntax: \\x character out of range");

  // A Fe string is a byte string. `BuildString` writes at `strlen`, so a
  // decoded NUL was a no-op that shifted the rest of the string down --
  // `(list "\0a" "a\0b")` answered `("a" "ab")` -- and a value above 255 was
  // truncated into the same hole, which is what `"\400"` did.
  REJECTS("\"\\0a\"", "unsupported read syntax: NUL character in string");
  REJECTS("\"a\\0b\"", "unsupported read syntax: NUL character in string");
  REJECTS("\"\\x00\"", "unsupported read syntax: NUL character in string");
  REJECTS("\"\\400\"",
          "unsupported read syntax: character above 255 in string");
  REJECTS("\"\\x41f\"",
          "unsupported read syntax: character above 255 in string");
  REJECTS("\"\\q\"", "unsupported read syntax: unknown escape");

  // A backslash was rejected only at the start of a token, so `(cdr '(a\ b))`
  // answered `(b)` -- two symbols -- where Emacs reads the one symbol `a b`
  // and answers nil.
  REJECTS("(cdr '(a\\ b))", "unsupported read syntax: symbol escape");
  REJECTS("\\a", "unsupported read syntax: symbol escape");

  // The remaining named reject arms, each asserted to name its syntax.
  REJECTS("#q", "unsupported read syntax: #");
  REJECTS("[1 2]", "unsupported read syntax: vector brackets");
  REJECTS("]", "unsupported read syntax: vector brackets");
  REJECTS("#xg", "unsupported read syntax: malformed radix integer");
  REJECTS("#x", "unsupported read syntax: malformed radix integer");
  REJECTS("#x0g", "unsupported read syntax: malformed radix integer");
  REJECTS("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "symbol too long (63-byte limit)");
  // The three `ReadUtf8` call sites: an impossible lead byte, a continuation
  // byte that is not one, and a well-formed encoding of a value that is not
  // a character (a surrogate).
  REJECTS("?\x80", "unsupported read syntax: invalid UTF-8 character");
  REJECTS("?\xC3(", "unsupported read syntax: invalid UTF-8 character");
  REJECTS("?\xED\xA0\x80", "unsupported read syntax: invalid UTF-8 character");

#undef READS
#undef REJECTS

  // The top-level form's line, which is what a `(load "init.el")` failure
  // reports. `RecordTopFormLine` latches the first line it is given, and it
  // used to be given one before the comment arm ran, so every form a comment
  // preceded reported the comment's line. Every real init file opens with a
  // comment block.
  static const struct {
    const char* source;
    const char* expected;
  } line_cases[] = {
      {"(car 2)", "lines.fe:1: expected pair, got integer"},
      {"\n(car 2)", "lines.fe:2: expected pair, got integer"},
      {"(+ 1\n 2)\n(car 2)", "lines.fe:3: expected pair, got integer"},
      {"; a comment\n; another\n(car 2)",
       "lines.fe:3: expected pair, got integer"},
      {"(print 1)\n; c\n(car 2)", "lines.fe:3: expected pair, got integer"},
      {";; one\n;; two\n\n(setq x 1)\n(car 2)",
       "lines.fe:5: expected pair, got integer"},
      // Past nine, which is where the harness's old single-digit line test
      // corrupted the expectation.
      {"\n\n\n\n\n\n\n\n\n\n(car 2)",
       "lines.fe:11: expected pair, got integer"},
      {";1\n;2\n;3\n;4\n;5\n;6\n;7\n;8\n;9\n;10\n(car 2)",
       "lines.fe:11: expected pair, got integer"},
      // A *read* error after a comment block, not a runtime one.
      {"; a\n; b\n[", "lines.fe:3: unsupported read syntax: vector brackets"},
  };
  for (size_t i = 0; i < sizeof(line_cases) / sizeof(line_cases[0]); i++) {
    CHECK(ExpectEvaluationError(
        context, &state, "lines.fe", line_cases[i].source,
        strlen(line_cases[i].source), line_cases[i].expected));
  }

  FeCloseContext(context);
  return true;
}

static bool TestFileInput(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  char source[] = "1 (+ 2 5)";
  FILE* file = fmemopen(source, sizeof(source) - 1, "r");
  CHECK(file != nullptr);
  const size_t gc = FeSaveGC(context);
  CHECK(IsRendered(context, FeEvaluateFile(context, "memory.fe", file), "7"));
  CHECK(FeSaveGC(context) == gc);
  rewind(file);
  const FeEvalOptions options = {.step_limit = 16};
  CHECK(IsRendered(
      context, FeEvaluateFileWithOptions(context, "memory.fe", file, &options),
      "7"));
  CHECK(FeSaveGC(context) == gc);
  rewind(file);
  CHECK(fgetc(file) == '1');
  CHECK(fclose(file) == 0);

  char with_nul[] = {'1', '\0', '2'};
  file = fmemopen(with_nul, sizeof(with_nul), "r");
  CHECK(file != nullptr);
  CHECK(ExpectFileError(context, &state, file,
                        "nul-file.fe:1: embedded NUL byte"));
  CHECK(fclose(file) == 0);
  FeCloseContext(context);
  return true;
}

static bool TestEvaluationControl(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char loop[] = "(while t 1)";
  const FeEvalOptions loop_options = {.step_limit = 32};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "loop.fe", loop, sizeof(loop) - 1, &loop_options,
      "loop.fe:1: evaluation step limit exceeded"));
  CHECK(IsRendered(context, FeEvaluateString(context, "recovered.fe", "5", 1),
                   "5"));

  static const char recursion[] =
      "(fset 'recurse (fn (x) (recurse x))) (recurse 1)";
  const FeEvalOptions recursion_options = {.step_limit = 64};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "recursion.fe", recursion, sizeof(recursion) - 1,
      &recursion_options, "recursion.fe:1: evaluation step limit exceeded"));

  static const char macros[] =
      "(fset 'expand (macro (x) x)) (expand (expand (expand (expand (expand "
      "(expand (expand (expand 1))))))))";
  const FeEvalOptions macro_options = {.step_limit = 20};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "macros.fe", macros, sizeof(macros) - 1, &macro_options,
      "macros.fe:1: evaluation step limit exceeded"));

  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3};
  const FeEvalOptions interrupt_options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  CHECK(ExpectEvaluationOptionsError(context, &state, "interrupt.fe", loop,
                                     sizeof(loop) - 1, &interrupt_options,
                                     "interrupt.fe:1: evaluation cancelled"));
  CHECK(interrupt.polls == interrupt.cancel_after);
  CHECK(interrupt.userdata_seen);
  CHECK(IsRendered(context, FeEvaluateString(context, "recovered.fe", "6", 1),
                   "6"));

  InterruptState default_poll = {.context = context,
                                 .expected_userdata = &default_poll,
                                 .polls = 0,
                                 .cancel_after = 1};
  const FeEvalOptions default_poll_options = {.interrupt = Interrupt,
                                              .userdata = &default_poll};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "default-poll.fe", loop, sizeof(loop) - 1,
      &default_poll_options, "default-poll.fe:1: evaluation cancelled"));
  CHECK(default_poll.polls == 1);
  CHECK(default_poll.userdata_seen);

  static const char addition[] = "(+ 1 2)";
  const FeEvalOptions exact_options = {.step_limit = 3};
  for (size_t i = 0; i < 2; i++) {
    CHECK(ExpectEvaluationOptionsError(
        context, &state, "exact.fe", addition, sizeof(addition) - 1,
        &exact_options, "exact.fe:1: evaluation step limit exceeded"));
  }
  const FeEvalOptions sufficient_options = {.step_limit = 4};
  CHECK(IsRendered(
      context,
      FeEvaluateStringWithOptions(context, "exact.fe", addition,
                                  sizeof(addition) - 1, &sufficient_options),
      "3"));
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "plain.fe", addition, sizeof(addition) - 1),
      "3"));

  size_t offset = 0;
  FeObject* addition_object =
      FeReadString(context, addition, sizeof(addition) - 1, &offset);
  CHECK(IsRendered(
      context,
      FeEvaluateWithOptions(context, addition_object, &sufficient_options),
      "3"));

  InterruptState nested_interrupt = {.context = context,
                                     .expected_userdata = &nested_interrupt,
                                     .polls = 0,
                                     .cancel_after = 1};
  state.nested_interrupt = &nested_interrupt;
  const size_t gc = FeSaveGC(context);
  FeSetFunction(context, FeMakeSymbol(context, "reenter"),
                FeMakeNativeFn(context, ReenterEvaluation));
  FeRestoreGC(context, gc);
  static const char nested[] = "(reenter)";
  const FeEvalOptions outer_options = {.step_limit = 24};
  state.nested_with_options = false;
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "outer.fe", nested, sizeof(nested) - 1, &outer_options,
      "inner.fe:1: evaluation step limit exceeded"));
  CHECK(nested_interrupt.polls == 0);
  state.nested_with_options = true;
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "outer.fe", nested, sizeof(nested) - 1, &outer_options,
      "inner.fe:1: evaluation step limit exceeded"));
  CHECK(nested_interrupt.polls == 0);
  CHECK(IsRendered(context, FeEvaluateString(context, "recovered.fe", "7", 1),
                   "7"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 06B: the completion kind is true at its producers and readable
// through Decision 5's accessors. Each case pins the kind the error callback
// observes (step limit -> Budget, interrupt -> Quit, the frame and re-entry
// walls -> Budget, an ordinary error -> Error), the same kind after the
// host's recovery, the condition object that goes with the kind, and the
// reset to Normal that a normal top-level return performs. It also carries the
// `CleanupFrameReserve` coupling from the plan's item 2: a cleanup provoked
// by frame exhaustion must be pushable regardless of which wall tripped,
// because the frame wall now assigns Budget (a non-Normal kind) before the
// drain runs.
static bool TestCompletionKinds(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // A fresh context and a normal evaluation both read Normal: nothing has
  // completed abnormally, and the normal return clears whatever the last
  // error left behind.
  CHECK(FeGetCompletion(context) == FeCompletionNormal);
  CHECK(IsRendered(context, FeEvaluateString(context, "plain.fe", "42", 2),
                   "42"));
  CHECK(FeGetCompletion(context) == FeCompletionNormal);

  // Step limit -> Budget.
  static const char loop[] = "(while t 1)";
  const FeEvalOptions step = {.step_limit = 32};
  CHECK(ExpectCompletionKind(
      context, &state, "steps.fe", loop, sizeof(loop) - 1, &step,
      "steps.fe:1: evaluation step limit exceeded", FeCompletionBudget));

  // Interrupt -> Quit.
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3};
  const FeEvalOptions interrupt_options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  CHECK(ExpectCompletionKind(context, &state, "interrupt.fe", loop,
                             sizeof(loop) - 1, &interrupt_options,
                             "interrupt.fe:1: evaluation cancelled",
                             FeCompletionQuit));
  CHECK(interrupt.polls == interrupt.cancel_after);
  // A real interrupt carries the same `(quit)` condition object
  // `(signal 'quit nil)` constructs, so a host reading `FeGetCondition`
  // after a C-g never sees whatever the last *error* signalled.
  CHECK(IsRendered(context, FeGetCondition(context), "(quit)"));

  // Frame wall -> Budget. The fixed expression measures its own peak first
  // (the same push-before-write trick `TestFrameLimits` uses), so "one below
  // the measured peak" is a real refusal, not a hand-picked frame count.
  static const char fixed[] = "(+ 1 (+ 2 (+ 3 (+ 4 5))))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "measure.fe", fixed, sizeof(fixed) - 1), "15"));
  const size_t peak = FeGetArenaStats(context).peak_frame_depth;
  const FeEvalOptions one_less = {.max_frames = peak - 1};
  CHECK(ExpectCompletionKind(
      context, &state, "frames.fe", fixed, sizeof(fixed) - 1, &one_less,
      "frames.fe:1: evaluation frame limit exceeded", FeCompletionBudget));

  // Re-entry wall -> Budget, through a native that synchronously re-enters
  // `FeCallWithOptions` on itself until `max_native_reentry` blocks it.
  FeObject* native = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, native);
  FeSetFunction(context, FeMakeSymbol(context, "reentrant-native"), native);
  state.reentry_remaining = 9;
  state.reentry_cleanup_ran = false;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  const FeEvalOptions reentry = {.max_native_reentry = 8};
  CHECK(ExpectCompletionKind(
      context, &state, "reentry.fe", "(reentrant-native)",
      sizeof("(reentrant-native)") - 1, &reentry,
      "reentry.fe:1: native evaluation re-entry limit exceeded",
      FeCompletionBudget));

  // An ordinary error -> Error.
  static const char type_error[] = "(car 1)";
  CHECK(ExpectCompletionKind(
      context, &state, "error.fe", type_error, sizeof(type_error) - 1, nullptr,
      "error.fe:1: expected pair, got integer", FeCompletionError));

  // The CleanupFrameReserve coupling (plan item 2): a body that exhausts a
  // tight `max_frames` still gets its `unwind-protect` cleanup -- the frame
  // wall assigns Budget before the drain, so the reserve is granted to a
  // cleanup provoked by frame exhaustion -- and the kind read is Budget.
  static const char reset_flag[] = "(setq cleanup-ran nil)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "reset.fe", reset_flag, sizeof(reset_flag) - 1),
      "nil"));
  static const char overflow_with_cleanup[] =
      "(fset 'loop (fn (x) (loop x))) "
      "(unwind-protect (loop 1)"
      "  (setq cleanup-ran t))";
  const FeEvalOptions tight_frames = {.max_frames = 6};
  CHECK(ExpectCompletionKind(
      context, &state, "reserve.fe", overflow_with_cleanup,
      sizeof(overflow_with_cleanup) - 1, &tight_frames,
      "reserve.fe:1: evaluation frame limit exceeded", FeCompletionBudget));
  static const char check_cleanup_ran[] = "cleanup-ran";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "check.fe", check_cleanup_ran,
                                    sizeof(check_cleanup_ran) - 1),
                   "t"));

  // Recovery: after every kind above, a normal evaluation returns its value
  // and leaves the accessor back at Normal -- the plan's "a host polling the
  // accessor between evaluations reads Normal" invariant.
  static const char deep[] =
      "(fset 'deep (lambda (n) (if (<= n 0) 0 (+ 1 (deep (- n 1)))))) (deep "
      "40)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "recovered.fe", deep, sizeof(deep) - 1), "40"));
  CHECK(FeGetCompletion(context) == FeCompletionNormal);

  FeCloseContext(context);
  return true;
}

static bool TestExtensionAPI(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  CHECK(FeNil(context) ==
        FeEvaluateString(context, "nil.fe", "()", sizeof("()") - 1));
  FeDefineNative(context, "add-exactly", AddExactly);
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "native.fe", "(add-exactly 2 3)",
                                    sizeof("(add-exactly 2 3)") - 1),
                   "5.0"));
  CHECK(ExpectEvaluationError(
      context, &state, "native.fe", "(add-exactly 2 3 4)",
      sizeof("(add-exactly 2 3 4)") - 1, "native.fe:1: too many arguments"));
  CHECK(ExpectEvaluationError(context, &state, "native.fe", "(add-exactly 2)",
                              sizeof("(add-exactly 2)") - 1,
                              "native.fe:1: too few arguments"));

  enum { TextLength = 128 };
  char text[TextLength];
  char source[TextLength + 2];
  source[0] = '"';
  for (size_t i = 0; i < sizeof(text); i++) {
    text[i] = (char)('a' + i % 26);
    source[i + 1] = text[i];
  }
  source[sizeof(source) - 1] = '"';
  const FeObject* string =
      FeEvaluateString(context, "string.fe", source, sizeof(source));
  CHECK(FeStringByteLength(context, string) == sizeof(text));
  char copied[TextLength];
  CHECK(FeCopyStringBytes(context, string, copied, sizeof(copied)));
  CHECK(memcmp(copied, text, sizeof(text)) == 0);
  CHECK(!FeCopyStringBytes(context, string, copied, sizeof(copied) - 1));

  const FeObject* symbol = FeMakeSymbol(context, "dispatch-name");
  static const char symbol_name[] = "dispatch-name";
  char symbol_bytes[sizeof(symbol_name) - 1];
  CHECK(FeStringByteLength(context, symbol) == sizeof(symbol_bytes));
  CHECK(FeCopyStringBytes(context, symbol, symbol_bytes, sizeof(symbol_bytes)));
  CHECK(memcmp(symbol_bytes, symbol_name, sizeof(symbol_bytes)) == 0);
  CHECK(ExpectStringError(context, &state, FeMakeDouble(context, 1),
                          "expected string or symbol, got double"));

  FeCloseContext(context);

  return true;
}

static bool TestRootsAndCalls(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char pair_function[] = "(fn (x y) (list x y))";
  FeRoot* pair_root =
      FeCreateRoot(context, FeEvaluateString(context, "root.fe", pair_function,
                                             sizeof(pair_function) - 1));
  static const char constant_function[] = "(fn () 17)";
  FeRoot* constant_root = FeCreateRoot(
      context, FeEvaluateString(context, "root.fe", constant_function,
                                sizeof(constant_function) - 1));
  CHECK(FeGetRoot(pair_root) != FeGetRoot(constant_root));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "replace-root.fe", "0", 1), "0"));

  char temporary[160];
  memset(temporary, 'x', sizeof(temporary) - 1);
  temporary[sizeof(temporary) - 1] = '\0';
  const size_t allocation_gc = FeSaveGC(context);
  for (size_t i = 0; i < 1000; i++) {
    FeRestoreGC(context, allocation_gc);
    (void)FeMakeString(context, temporary);
  }
  FeRestoreGC(context, allocation_gc);

  FeObject* list =
      FeEvaluateString(context, "argument.fe", "'(1 2)", sizeof("'(1 2)") - 1);
  FeObject* arguments[] = {list, FeMakeDouble(context, 3)};
  const size_t call_gc = FeSaveGC(context);
  for (size_t i = 0; i < 16; i++) {
    CHECK(IsRendered(context,
                     FeCall(context, FeGetRoot(pair_root), arguments,
                            sizeof(arguments) / sizeof(arguments[0])),
                     "((1 2) 3.0)"));
    CHECK(FeSaveGC(context) == call_gc);
  }
  CHECK(IsRendered(
      context, FeCall(context, FeGetRoot(constant_root), nullptr, 0), "17"));
  CHECK(FeSaveGC(context) == call_gc);

  FeReleaseRoot(context, pair_root);
  CHECK(IsRendered(
      context, FeCall(context, FeGetRoot(constant_root), nullptr, 0), "17"));
  CHECK(ExpectReleaseError(context, &state, pair_root, "root is not active"));
  FeReleaseRoot(context, constant_root);

  CHECK(ExpectCallError(context, &state, FeMakeDouble(context, 1),
                        "tried to call non-callable value"));

  static const char looping_function[] = "(fn () (while t 1))";
  state.root = FeCreateRoot(
      context, FeEvaluateString(context, "root.fe", looping_function,
                                sizeof(looping_function) - 1));
  FeDefineNative(context, "call-root", CallRoot);
  const FeEvalOptions options = {.step_limit = 32};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "call.fe", "(call-root)", sizeof("(call-root)") - 1,
      &options, "call.fe:1: evaluation step limit exceeded"));
  FeReleaseRoot(context, state.root);

  FeCloseContext(context);
  return true;
}

static bool TestCallWithOptions(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char add_function[] = "(fn (x y) (+ x y))";
  FeRoot* root =
      FeCreateRoot(context, FeEvaluateString(context, "call.fe", add_function,
                                             sizeof(add_function) - 1));
  const FeEvalOptions generous = {.step_limit = 64};
  FeObject* arguments[] = {FeMakeDouble(context, 10),
                           FeMakeDouble(context, 20)};
  CHECK(IsRendered(
      context,
      FeCallWithOptions(context, FeGetRoot(root), arguments, 2, &generous),
      "30.0"));

  // A controlled call restores its internal GC frame, so repeated calls do
  // not grow the stack and the rooted callable survives each one.
  const size_t call_gc = FeSaveGC(context);
  for (size_t i = 0; i < 16; i++) {
    CHECK(IsRendered(
        context,
        FeCallWithOptions(context, FeGetRoot(root), arguments, 2, &generous),
        "30.0"));
    CHECK(FeSaveGC(context) == call_gc);
  }

  // Wrong arity is unconditional for user functions.
  static const char one_arg_function[] = "(fn (x) x)";
  FeRoot* arity_root = FeCreateRoot(
      context, FeEvaluateString(context, "call.fe", one_arg_function,
                                sizeof(one_arg_function) - 1));
  CHECK(ExpectCallWithOptionsError(context, &state, FeGetRoot(arity_root),
                                   nullptr, 0, &generous,
                                   "wrong-number-of-arguments"));

  // Error propagation: a body that raises, and a non-callable value.
  static const char raise_function[] = "(fn () (car 1))";
  FeRoot* raise_root =
      FeCreateRoot(context, FeEvaluateString(context, "call.fe", raise_function,
                                             sizeof(raise_function) - 1));
  CHECK(ExpectCallWithOptionsError(context, &state, FeGetRoot(raise_root),
                                   nullptr, 0, &generous,
                                   "expected pair, got integer"));
  CHECK(ExpectCallWithOptionsError(context, &state, FeMakeDouble(context, 1),
                                   nullptr, 0, &generous,
                                   "tried to call non-callable value"));
  FeReleaseRoot(context, raise_root);
  FeReleaseRoot(context, arity_root);

  // Deterministic step exhaustion mid-call.
  static const char loop_function[] = "(fn () (while t 1))";
  state.root =
      FeCreateRoot(context, FeEvaluateString(context, "call.fe", loop_function,
                                             sizeof(loop_function) - 1));
  const FeEvalOptions small = {.step_limit = 32};
  CHECK(ExpectCallWithOptionsError(context, &state, FeGetRoot(state.root),
                                   nullptr, 0, &small,
                                   "evaluation step limit exceeded"));

  // Interrupt: the polled host check fires during the call.
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3};
  const FeEvalOptions interrupt_options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  CHECK(ExpectCallWithOptionsError(context, &state, FeGetRoot(state.root),
                                   nullptr, 0, &interrupt_options,
                                   "evaluation cancelled"));
  CHECK(interrupt.polls == interrupt.cancel_after);
  CHECK(interrupt.userdata_seen);
  CHECK(IsRendered(context, FeEvaluateString(context, "recovered.fe", "1", 1),
                   "1"));

  // Nested ambient budget: a native calling FeCallWithOptions inside an
  // active evaluation shares the outer budget; its own options are ignored.
  InterruptState nested_interrupt = {.context = context,
                                     .expected_userdata = &nested_interrupt,
                                     .polls = 0,
                                     .cancel_after = 1};
  state.nested_interrupt = &nested_interrupt;
  const size_t gc = FeSaveGC(context);
  FeSetFunction(context, FeMakeSymbol(context, "reenter-call-with-options"),
                FeMakeNativeFn(context, ReenterCallWithOptions));
  FeRestoreGC(context, gc);
  static const char nested[] = "(reenter-call-with-options)";
  const FeEvalOptions outer = {.step_limit = 24};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "outer.fe", nested, sizeof(nested) - 1, &outer,
      "outer.fe:1: evaluation step limit exceeded"));
  CHECK(nested_interrupt.polls == 0);

  // One failed invocation does not poison later independent calls.
  CHECK(IsRendered(
      context,
      FeCallWithOptions(context, FeGetRoot(root), arguments, 2, &generous),
      "30.0"));

  FeReleaseRoot(context, state.root);
  FeReleaseRoot(context, root);
  FeCloseContext(context);
  return true;
}

static bool TestMathNatives(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);

#define CHK(expr, expected)                                                  \
  CHECK(IsRendered(                                                          \
      context, FeEvaluateString(context, "math.fe", expr, sizeof(expr) - 1), \
      expected))

  CHK("(sin 0)", "0.0");
  CHK("(cos 0)", "1.0");
  CHK("(expt 2 8)", "256");
  CHK("(expt 2 10)", "1024");
  CHK("(sqrt 16)", "4.0");
  CHK("(log (exp 1))", "1.0");
  CHK("(log 100 10)", "2.0");
  CHK("(atan 1)", "0.7853981633974483");
  CHK("(atan 1 1)", "0.7853981633974483");

  CHK("(floor 7)", "7");
  CHK("(floor 7 2)", "3");
  CHK("(floor -7 2)", "-4");
  CHK("(floor -7 -2)", "3");
  CHK("(floor 7.5)", "7");
  CHK("(floor -7.5)", "-8");
  CHK("(floor 7.5 2)", "3");
  CHK("(floor -7.5 2)", "-4");

  CHK("(ceiling 7)", "7");
  CHK("(ceiling 7 2)", "4");
  CHK("(ceiling -7 2)", "-3");
  CHK("(ceiling 7.5)", "8");
  CHK("(ceiling -7.5)", "-7");
  CHK("(ceiling 7.5 2)", "4");
  CHK("(ceiling -7.5 2)", "-3");

  CHK("(round 7 2)", "4");
  CHK("(round -7 2)", "-4");
  CHK("(round 2.5)", "2");
  CHK("(round 3.5)", "4");
  CHK("(round -2.5)", "-2");
  CHK("(round -3.5)", "-4");
  CHK("(round 7.5 2)", "4");
  CHK("(round -7.5 2)", "-4");

  CHK("(truncate 7.5)", "7");
  CHK("(truncate -7.5)", "-7");
  CHK("(truncate 7.5 2)", "3");
  CHK("(truncate -7.5 2)", "-3");

#undef CHK

  FeCloseContext(context);
  return true;
}

typedef struct Rendered {
  char text[512];
  size_t length;
  size_t dropped;
} Rendered;

static Rendered render_target;

static void Collect(FeContext*, void* udata, char chr) {
  Rendered* r = udata;
  if (r->length + 1 < sizeof(r->text)) {
    r->text[r->length++] = chr;
  } else {
    r->dropped++;
  }
  r->text[r->length] = '\0';
}

// Renders through a native function, so the write happens inside a controlled
// evaluation and can be charged and cancelled.
static FeObject* RenderNative(FeContext* context, FeObject* arguments) {
  FeObject* object = FeGetNextArgument(context, &arguments);
  FeRequireNoArguments(context, arguments);
  render_target = (Rendered){0};
  return FeMakeBool(context, FeWriteWithOptions(context, object, Collect,
                                                &render_target, 0, nullptr));
}

static bool Renders(FeContext* context,
                    const char* source,
                    const FeWriteOptions* options,
                    const char* expected,
                    bool expected_complete) {
  const size_t gc = FeSaveGC(context);
  FeObject* object =
      FeEvaluateString(context, "write.fe", source, strlen(source));
  Rendered rendered = {0};
  const bool complete =
      FeWriteWithOptions(context, object, Collect, &rendered, 0, options);
  CHECK(complete == expected_complete);
  CHECK(rendered.dropped == 0);
  CHECK(strcmp(rendered.text, expected) == 0);
  FeRestoreGC(context, gc);
  return true;
}

static bool TestWriter(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // Cycles through the cdr spine terminate, with the two-pointer walk finding
  // both a self-loop and a longer one.
  CHECK(Renders(context, "(do (setq a (cons 1 nil)) (setcdr a a) a)", nullptr,
                "(1 . #<cycle>)", false));
  CHECK(Renders(context,
                "(do (setq b (cons 1 (cons 2 nil))) (setcdr (cdr b) b) b)",
                nullptr, "(1 2 1 . #<cycle>)", false));

  // A cycle through a car is bounded by depth instead, and depth is spent only
  // on nesting: a long flat list is not deep.
  const FeWriteOptions shallow = {.max_depth = 4};
  CHECK(Renders(context, "(do (setq c (cons 1 nil)) (setcar c c) c)", &shallow,
                "((((#<deep>))))", false));
  CHECK(Renders(context, "'(1 2 3 4 5 6 7 8)", &shallow, "(1 2 3 4 5 6 7 8)",
                true));
  CHECK(Renders(context, "'(((1)))", &shallow, "(((1)))", true));
  const FeWriteOptions shallower = {.max_depth = 3};
  CHECK(Renders(context, "'(((1)))", &shallower, "(((#<deep>)))", false));

  // Shared but acyclic structure is printed in full, every time it appears.
  CHECK(Renders(context, "(do (setq s '(1 2)) (list s s s))", nullptr,
                "((1 2) (1 2) (1 2))", true));

  // Byte and node budgets, at the boundary and one below it.
  const FeWriteOptions bytes_exact = {.max_bytes = 7};
  const FeWriteOptions bytes_short = {.max_bytes = 6};
  CHECK(Renders(context, "'(1 2 3)", &bytes_exact, "(1 2 3)", true));
  CHECK(Renders(context, "'(1 2 3)", &bytes_short, "(1 2 3", false));
  const FeWriteOptions nodes_exact = {.max_nodes = 4};
  const FeWriteOptions nodes_short = {.max_nodes = 3};
  CHECK(Renders(context, "'(1 2 3)", &nodes_exact, "(1 2 3)", true));
  CHECK(
      Renders(context, "'(1 2 3)", &nodes_short, "(1 2 #<truncated>)", false));

  // Closures print without allocating, so this cannot collect or raise.
  CHECK(Renders(context, "(lambda (a b) (+ a b))", nullptr,
                "(lambda (a b) (+ a b))", true));
  CHECK(Renders(context, "(macro (a) a)", nullptr, "(macro (a) a)", true));

  // The writer spends the evaluation budget, so a long render answers an
  // interrupt. `long` is built first, then rendered from a native function
  // under an interrupt that cancels well after evaluation has finished.
  FeDefineNative(context, "render", RenderNative);
  static const char build[] =
      "(setq long nil)"
      "(setq n 0)"
      "(while (< n 400) (setq long (cons n long)) (setq n (+ n 1)))";
  (void)FeEvaluateString(context, "build.fe", build, sizeof(build) - 1);
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .cancel_after = SIZE_MAX};
  const FeEvalOptions counted = {
      .poll_interval = 1, .interrupt = Interrupt, .userdata = &interrupt};
  (void)FeEvaluateStringWithOptions(context, "count.fe", "long", 4, &counted);
  const size_t without_render = interrupt.polls;
  interrupt.polls = 0;
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "count.fe",
                                               "(render long)", 13, &counted),
                   "t"));
  CHECK(interrupt.polls > without_render + 400);
  CHECK(render_target.length > 400);

  interrupt.polls = 0;
  interrupt.cancel_after = without_render + 20;
  CHECK(ExpectEvaluationOptionsError(context, &state, "cancel.fe",
                                     "(render long)", 13, &counted,
                                     "cancel.fe:1: evaluation cancelled"));
  CHECK(render_target.length > 0);

  // The context still works afterwards.
  CHECK(IsRendered(context, FeEvaluateString(context, "after.fe", "(+ 2 3)", 7),
                   "5"));

  FeCloseContext(context);
  return true;
}

static bool TestParameterLists(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                  \
  CHECK(IsRendered(                                                          \
      context, FeEvaluateString(context, "args.fe", expr, sizeof(expr) - 1), \
      expected))

  CHK("((lambda (a &optional b) (list a b)) 1)", "(1 nil)");
  CHK("((lambda (a &optional b) (list a b)) 1 2)", "(1 2)");
  CHK("((lambda (a &rest r) (list a r)) 1 2 3)", "(1 (2 3))");
  // 07A's closure constructors take a raw *parameter-list* minimum, not a
  // body: `(lambda (x))` is a valid closure in Emacs 31.0.90 (it prints as
  // `#[(x) (nil) (t)]`) and `((lambda (x)) 1)` answers nil there. Requiring a
  // body would reject a form the oracle accepts.
  CHK("(lambda (x))", "(lambda (x))");
  CHK("((lambda (x)) 1)", "nil");
  CHK("((macro (x)) 1)", "nil");
  CHK("((lambda ()))", "nil");
  CHK("((lambda (a &rest r) (list a r)) 1)", "(1 nil)");
  CHK("((lambda (&rest r) r) 1 2 3)", "(1 2 3)");
  CHK("((lambda (a . r) (list a r)) 1 2 3)", "(1 (2 3))");
  CHK("((lambda r r) 1 2 3)", "(1 2 3)");
  CHK("((macro (a &rest r) (cons 'list (cons a r))) 1 2 3)", "(1 2 3)");
  CHK("(condition-case e ((lambda (x) x) 1 2) "
      "(wrong-number-of-arguments e))",
      "(wrong-number-of-arguments (lambda (x) x) 2)");
  CHK("(condition-case e (funcall (lambda (x) x)) "
      "(wrong-number-of-arguments e))",
      "(wrong-number-of-arguments (lambda (x) x) 0)");
  CHK("(condition-case e ((macro (x) x)) "
      "(wrong-number-of-arguments e))",
      "(wrong-number-of-arguments (macro (x) x) 0)");
  CHK("(condition-case e (car 1 2) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments car 2)");
  // 07A Decision 4: every arity raise carries identity and count, `setq`'s
  // dangling-target one included. Emacs 31.0.90, measured: `(setq a 1 b)` is
  // `(wrong-number-of-arguments setq 3)` -- the whole form's raw count -- and
  // `a` is left assigned to 1, so the count is not what remained unconsumed.
  CHK("(condition-case e (setq q 1 r) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments setq 3)");
  CHK("q", "1");
  CHK("(condition-case e (setq q) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments setq 1)");
  CHK("(condition-case e (boundp 'car 'extra) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments boundp 2)");
  CHK("(condition-case e (integerp) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments integerp 0)");
  // An improper argument list is a type error about its tail, not an arity
  // report: there is no argument count to name. Emacs 31.0.90 answers
  // `(wrong-type-argument listp 2)` for all three.
  CHK("(condition-case e (car 1 . 2) (wrong-type-argument e))",
      "(wrong-type-argument listp 2)");
  CHK("(condition-case e (list 1 . 2) (wrong-type-argument e))",
      "(wrong-type-argument listp 2)");

#undef CHK

  // Recorded divergence, unchanged by Phase 7: an improper argument list to a
  // *closure* is diagnosed by `FeGetNextArgument` while collecting operands,
  // which is a plain `error`, where Emacs 31.0.90 answers
  // `(wrong-type-argument listp 2)` here too. The primitive path above is the
  // one Phase 7 touched; the closure path's text is a host-API message
  // natives share and is left alone.
  CHECK(ExpectEvaluationError(context, &state, "dotted.fe",
                              "((lambda (a) a) 1 . 2)",
                              strlen("((lambda (a) a) 1 . 2)"),
                              "dotted.fe:1: dotted pair in argument list"));
  CHECK(ExpectEvaluationError(
      context, &state, "rest.fe", "((lambda (a &rest) a) 1)",
      strlen("((lambda (a &rest) a) 1)"), "rest.fe:1: invalid-function"));
  CHECK(ExpectEvaluationError(
      context, &state, "rest.fe", "((lambda (a &rest r x) a) 1)",
      strlen("((lambda (a &rest r x) a) 1)"), "rest.fe:1: invalid-function"));
  CHECK(ExpectEvaluationError(
      context, &state, "rest.fe", "((lambda (a . 1) a) 1)",
      strlen("((lambda (a . 1) a) 1)"), "rest.fe:1: invalid-function"));
#define STRICT(expr, message)                                     \
  CHECK(ExpectEvaluationError(context, &state, "strict.fe", expr, \
                              strlen(expr), message))
  STRICT("((lambda (x) x))", "strict.fe:1: wrong-number-of-arguments");
  STRICT("((lambda (a b) a) 1)", "strict.fe:1: wrong-number-of-arguments");
  STRICT("((lambda () 1) 2)", "strict.fe:1: wrong-number-of-arguments");
  STRICT("((lambda (a) a) 1 2)", "strict.fe:1: wrong-number-of-arguments");
  STRICT("((lambda (1) 5) 2)", "strict.fe:1: invalid-function");
  STRICT("((macro (a) a))", "strict.fe:1: wrong-number-of-arguments");
#undef STRICT

  // Optional and rest parameters remain valid.
#define OK(expr, expected)                                                     \
  CHECK(IsRendered(context,                                                    \
                   FeEvaluateString(context, "ok.fe", expr, sizeof(expr) - 1), \
                   expected))
  OK("((lambda (a &optional b) (list a b)) 1)", "(1 nil)");
  OK("((lambda (a &optional b) (list a b)) 1 2)", "(1 2)");
  OK("((lambda (a &rest r) (list a r)) 1)", "(1 nil)");
  OK("((lambda (a &rest r) (list a r)) 1 2 3)", "(1 (2 3))");
  OK("((lambda (a . r) (list a r)) 1)", "(1 nil)");
  OK("((lambda r r) 1 2 3)", "(1 2 3)");
  OK("((lambda (a) a) nil)", "nil");
#undef OK

  FeCloseContext(context);
  return true;
}

static bool TestBinding(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // A symbol exists as soon as it is interned; having a value is separate.
  FeObject* absent = FeMakeSymbol(context, "absent");
  CHECK(!FeIsBound(context, absent));
  CHECK(FeIsBound(context, FeMakeSymbol(context, "t")));
  // Since sub-plan 04D's cut the bootstrap callables live in *function*
  // cells, so a primitive name's value cell is empty: `FeIsBound` (the value
  // namespace) says nil while `(fboundp 'car)` says t.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "car")));

  CHECK(ExpectEvaluationError(context, &state, "void.fe", "absent", 6,
                              "void.fe:1: void-variable absent"));
  CHECK(ExpectEvaluationError(context, &state, "void.fe", "(absent 1)", 10,
                              "void.fe:1: void-function absent"));
  CHECK(ExpectEvaluationError(context, &state, "void.fe", "(+ 1 absent)", 12,
                              "void.fe:1: void-variable absent"));

#define CHK(expr, expected)                                                  \
  CHECK(IsRendered(                                                          \
      context, FeEvaluateString(context, "bind.fe", expr, sizeof(expr) - 1), \
      expected))

  CHK("(boundp 'absent)", "nil");
  // `setq` returns the assigned value, unlike old assignment `=`.
  CHK("(setq absent 7)", "7");
  CHK("(boundp 'absent)", "t");
  CHK("absent", "7");
  CHK("(makunbound 'absent)", "absent");
  CHK("(boundp 'absent)", "nil");

  // nil is an ordinary value, not an absence.
  CHK("(setq holds-nil nil)", "nil");
  CHK("(boundp 'holds-nil)", "t");
  CHK("holds-nil", "nil");

  // boundp and makunbound see lexical bindings; FeIsBound answers about the
  // global one.
  CHK("((lambda (p) (boundp 'p)) 1)", "t");
  CHK("(boundp 'p)", "nil");
  CHK("((lambda (p) (do (makunbound 'p) (boundp 'p))) 1)", "nil");

  // The sentinel is not reachable: a symbol's cell is not a pair to Lisp, and
  // (env) yields symbols, which print as their names.
  CHECK(ExpectEvaluationError(context, &state, "reach.fe", "(cdr 'absent)", 13,
                              "reach.fe:1: expected pair, got symbol"));
  CHK("(is (car (env)) (car (env)))", "t");

#undef CHK

  CHECK(FeIsBound(context, FeMakeSymbol(context, "holds-nil")));
  CHECK(!FeIsBound(context, absent));

  FeCloseContext(context);
  return true;
}

// Sub-plan 04B: the symbol layout lives behind the fe_internal.h accessors,
// and a fresh symbol carries a dormant function cell. The function cell is
// written by this test and read by nothing else until Phase 4's lookup
// slices (04C/04D); the value cell must be untouched by function-cell
// writes, and a function object reachable only through the cell must survive
// a forced collection -- the collector's symbol arm walks `CDR(sym)` into the
// new inner pair, so this is where a missed GC-rooting change would show up.
static bool TestSymbolCells(void) {
  static TestArena arena;
  const size_t size = FeMinimumArenaSize() + 8 * 1024;
  CHECK(size <= sizeof(arena.bytes));
  FeContext* context = FeOpenContext(arena.bytes, size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // Interning still deduplicates, and the scan goes through SymbolName.
  FeObject* first = FeMakeSymbol(context, "cell-probe");
  CHECK(first != nullptr);
  CHECK(FeMakeSymbol(context, "cell-probe") == first);

  // A symbol prints its name through SymbolName; nothing leaks the layout.
  CHECK(IsRendered(context, first, "cell-probe"));

  // A fresh symbol's function cell and value cell are both unbound, and the
  // function cell is independent of the value one.
  CHECK(SymbolFunction(first) == &unbound);
  CHECK(!FeIsBound(context, first));

  // SetSymbolFunction roundtrips. The function object's only root is the
  // cell: it is popped off the GC stack before the collection below.
  const size_t gc = FeSaveGC(context);
  FeObject* native = FeMakeNativeFn(context, OrdinaryNative);
  SetSymbolFunction(first, native);
  CHECK(SymbolFunction(first) == native);
  FeRestoreGC(context, gc);

  // The value cell is untouched by function-cell writes.
  FeSet(context, first, FeMakeDouble(context, 42));
  CHECK(FeIsBound(context, first));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "cell.fe", "cell-probe",
                                    sizeof("cell-probe") - 1),
                   "42.0"));
  CHECK(SymbolFunction(first) == native);

  // A churn loop allocates until the freelist empties, forcing collections;
  // the function object, reachable only through the symbol's cell, survives.
  const size_t collections = FeGetArenaStats(context).collection_count;
  static const char churn[] =
      "(setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) n";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "churn.fe", churn, sizeof(churn) - 1),
      "2000"));
  CHECK(FeGetArenaStats(context).collection_count > collections);
  CHECK(SymbolFunction(first) == native);
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "cell.fe", "cell-probe",
                                    sizeof("cell-probe") - 1),
                   "42.0"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 05B of kg's Emacs-subset program: the integer object, dormant.
// It exists on the host C surface -- constructible, readable, printable,
// typed, collectable -- and is producible by no Lisp program: the reader is
// untouched and no primitive returns one, so every golden stays
// byte-identical. `(type-of ...)` cannot observe it from Lisp (no
// producer); the `type_names` slot is asserted through the C surface
// instead, via the error text `CheckType` names it with.
static bool TestInteger(void) {
  static TestArena arena;
  const size_t size = FeMinimumArenaSize() + 8 * 1024;
  CHECK(size <= sizeof(arena.bytes));
  FeContext* context = FeOpenContext(arena.bytes, size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // FeMakeInteger round-trips through FeToInteger at both extremes, and
  // FeGetType names the new tag.
  FeObject* min_integer = FeMakeInteger(context, INT64_MIN);
  CHECK(FeGetType(min_integer) == FeTInteger);
  CHECK(FeToInteger(context, min_integer) == INT64_MIN);
  FeObject* max_integer = FeMakeInteger(context, INT64_MAX);
  CHECK(FeGetType(max_integer) == FeTInteger);
  CHECK(FeToInteger(context, max_integer) == INT64_MAX);

  // The writer prints integers exactly, at the extreme that would overflow
  // a double and at an ordinary value.
  CHECK(IsRendered(context, min_integer, "-9223372036854775808"));
  CHECK(IsRendered(context, FeMakeInteger(context, 42), "42"));

  // FeToDouble widens, so every current double-taking host read already
  // accepts a host-made integer. The converted double is asserted exactly
  // through the writer (a double prints its `.0`, per 05D's printer).
  CHECK(IsRendered(
      context,
      FeMakeDouble(context, FeToDouble(context, FeMakeInteger(context, 42))),
      "42.0"));
  CHECK(IsRendered(context,
                   FeMakeDouble(context, FeToDouble(context, min_integer)),
                   "-9.223372036854776e+18"));

  // The `type_names` slot, which is what `type-of` will return once the
  // reader can produce integers.
  CHECK(ExpectIntegerError(context, &state, FeMakeDouble(context, 1),
                           "expected integer, got double"));

  // A forced collection with a live integer preserves the value. The only
  // root is the symbol's value cell: the object is popped off the GC stack
  // before the churn, so a collection must reach it through the symbol.
  const size_t gc = FeSaveGC(context);
  FeObject* integer = FeMakeInteger(context, INT64_MAX);
  FeSet(context, FeMakeSymbol(context, "integer-probe"), integer);
  FeRestoreGC(context, gc);
  const size_t collections = FeGetArenaStats(context).collection_count;
  static const char churn[] =
      "(setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) n";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "churn.fe", churn, sizeof(churn) - 1),
      "2000"));
  CHECK(FeGetArenaStats(context).collection_count > collections);
  CHECK(FeToInteger(context, FeEvaluateString(
                                 context, "probe.fe", "integer-probe",
                                 sizeof("integer-probe") - 1)) == INT64_MAX);

  FeCloseContext(context);
  return true;
}

// Sub-plan 04C/04D: the function namespace. This is the full 04A answer
// table under the cut's final semantics: coexistence via fset, funcall on
// values / designators / lambdas, apply spread and its malformed-tail error,
// the symbol-function/symbol-value/fboundp readers, the makunbound/fmakunbound
// independence pair, defalias's designator chains and their late binding,
// the `function` special form, and the cycle that dies with
// `cyclic-function-indirection` -- plus the public
// FeSetFunction/FeGetFunction/FeIsFBound surface and context reuse after
// each error. The compat corpus (fe/compat/) compares the same shapes
// against the Emacs oracle; this is the implementation-focused side a
// process-per-case protocol cannot observe (cell state after recovery,
// host-API behaviour, the exact step budget of the cycle).
static bool TestFunctionCells(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                   \
  CHECK(IsRendered(context,                                                   \
                   FeEvaluateString(context, "lisp2.fe", expr, strlen(expr)), \
                   expected))
#define LISP2_ERR(expr, message)                                               \
  CHECK(ExpectEvaluationError(context, &state, "lisp2.fe", expr, strlen(expr), \
                              message))

  // The parent plan's headline: one symbol holds a value and a function at
  // once, call position resolves the function cell, bare-symbol evaluation
  // the value cell.
  CHK("(setq f 7)", "7");
  CHK("(fset 'f (lambda () 9))", "(lambda nil 9)");
  CHK("(list f (f))", "(7 9)");

  // `fset` evaluates both arguments -- the target is a value, not a quote --
  // and returns the function object.
  CHK("(setq s 'k)", "k");
  CHK("(fset s (lambda () 4))", "(lambda nil 4)");
  CHK("(k)", "4");
  // The target is validated as a symbol before the function form evaluates.
  LISP2_ERR("(fset 1 (lambda () 2))",
            "lisp2.fe:1: expected symbol, got integer");

  // `funcall` takes a value: a closure directly, a lexical value, and a
  // symbol designator resolved through the function cell (04D's cut removed
  // the value-cell fallback, so a value-only name is `void-function`).
  CHK("(funcall (lambda (x) (+ x 1)) 2)", "3");
  CHK("((lambda (g) (funcall g 3)) (lambda (x) (+ x 1)))", "4");
  CHK("(setq g 7)", "7");
  CHK("(fset 'g (lambda () 9))", "(lambda nil 9)");
  CHK("(funcall 'g)", "9");
  CHK("(fset 'myfn (lambda (x) (+ x 1)))", "(lambda (x) (+ x 1))");
  CHK("(funcall 'myfn 5)", "6");
  // A value-only designator is `void-function` since the cut -- matching the
  // pinned `lisp2-void-function-value-only` oracle -- and an unbound one is
  // the same; the context stays reusable after both. A zero-operand
  // funcall/apply is an arity error, never a crash on an empty operand list.
  LISP2_ERR("(setq v 7) (funcall 'v)", "lisp2.fe:1: void-function v");
  LISP2_ERR("(funcall 'no-such)", "lisp2.fe:1: void-function no-such");
  LISP2_ERR("(funcall)", "lisp2.fe:1: wrong-number-of-arguments");
  LISP2_ERR("(apply)", "lisp2.fe:1: wrong-number-of-arguments");
  CHK("(+ 1 2)", "3");

  // `apply` spreads its final list argument; the caller's list is never
  // mutated, so it still holds its original contents afterwards.
  CHK("(apply '+ 1 2 (list 3 4))", "10");
  CHK("(setq lst '(9 8))", "(9 8)");
  CHK("(apply 'cons lst '(7))", "((9 8) . 7)");
  CHK("lst", "(9 8)");
  CHK("(apply 'list 1 2 '(3 4))", "(1 2 3 4)");
  CHK("(apply 'list '())", "nil");
  // A dotted tail is the malformed-tail error, raised only after every
  // operand form has run (a side effect in an earlier operand already ran).
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "apply-probe")));
  LISP2_ERR("(apply '+ 1 (do (setq apply-probe t) '(2 3)) '(4 . 5))",
            "lisp2.fe:1: apply: last argument must be a proper list");
  CHECK(FeIsBound(context, FeMakeSymbol(context, "apply-probe")));
  LISP2_ERR("(apply 'list 1)",
            "lisp2.fe:1: apply: last argument must be a "
            "proper list");
  // 07A's census puts `apply` at 1+, like `funcall`: one operand is a
  // callable with no final list at all, which is that same malformed-tail
  // error and not an arity error. Emacs 31.0.90 agrees that it is not an
  // arity error -- `(apply #'list)` measures as
  // `(wrong-type-argument listp list)`.
  LISP2_ERR("(apply 'list)",
            "lisp2.fe:1: apply: last argument must be a "
            "proper list");
  CHK("(apply '+ 1 2 (list 3 4))", "10");

  // Zero-operand arithmetic is Emacs' identity element -- and, before that,
  // is *a value at all*: an operandless arithmetic frame used to complete
  // with the `&unbound` sentinel, which `ResumeEvalList` then dropped as a
  // non-delivery, so `(funcall (+))` reached the funcall arm with an empty
  // operand list and dereferenced it. Every one of these crashed or escaped
  // the sentinel into Lisp.
  CHK("(+)", "0");
  CHK("(*)", "1");
  CHK("(-)", "0");
  CHK("(funcall '+)", "0");
  CHK("(apply '+ '())", "0");
  CHK("(apply '* '())", "1");
  CHK("(= (+))", "t");
  CHK("(set 'zero-operand (+))", "0");
  CHK("zero-operand", "0");
  LISP2_ERR("(funcall (+))", "lisp2.fe:1: tried to call non-callable value");
  // `/` has no identity to return; nothing has evaluated by then, so this is
  // the same wrong-number-of-arguments Emacs signals. The context stays
  // reusable.
  LISP2_ERR("(/)", "lisp2.fe:1: wrong-number-of-arguments");
  CHK("(/ 8 2)", "4");
  CHK("(+ 1 2)", "3");

  // A macro or a special form is not a funcall/apply target: the redispatch
  // would hand it the `(quote v)` wrappers rather than the values, so
  // `(funcall 'quote 'a)` answered `(quote a)` and `(funcall 'if 1 2 3)`
  // took a branch of the *quoted* forms. Emacs signals invalid-function for
  // all of these, naming the operand the program wrote.
  LISP2_ERR("(funcall 'quote 'a)", "lisp2.fe:1: invalid-function quote");
  LISP2_ERR("(funcall 'if 1 2 3)", "lisp2.fe:1: invalid-function if");
  LISP2_ERR("(funcall 'let 'z 1)", "lisp2.fe:1: invalid-function let");
  LISP2_ERR("(apply 'and '(1 2))", "lisp2.fe:1: invalid-function and");
  CHK("(fset 'inc-macro (macro (x) (list '+ x 1)))",
      "(macro (x) (list (quote +) x 1))");
  LISP2_ERR("(funcall 'inc-macro 5)", "lisp2.fe:1: invalid-function inc-macro");
  LISP2_ERR("(apply 'inc-macro '(5))",
            "lisp2.fe:1: invalid-function inc-macro");
  // A designator chain ending in a macro is rejected at the name the caller
  // used, and the context is reusable after every one of these.
  CHK("(defalias 'macro-alias 'inc-macro)", "macro-alias");
  LISP2_ERR("(funcall 'macro-alias 5)",
            "lisp2.fe:1: invalid-function macro-alias");
  CHK("(inc-macro 5)", "6");
  // `funcall`/`apply` are function-shaped themselves, so they *are* legal
  // targets -- Emacs answers 3 here too.
  CHK("(funcall 'funcall '+ 1 2)", "3");
  CHK("(funcall 'apply '+ 1 '(2))", "3");

  // A dead designator chain is `void-function` at the name the program
  // wrote, not at the last link the resolver reached: Emacs reports
  // `void-function dead-head` for both the call and the funcall.
  CHK("(fset 'dead-head 'dead-middle)", "dead-middle");
  CHK("(fset 'dead-middle 'dead-tail)", "dead-tail");
  LISP2_ERR("(dead-head)", "lisp2.fe:1: void-function dead-head");
  LISP2_ERR("(funcall 'dead-head)", "lisp2.fe:1: void-function dead-head");
  LISP2_ERR("(apply 'dead-head '())", "lisp2.fe:1: void-function dead-head");
  CHK("(fset 'dead-tail (lambda () 5))", "(lambda nil 5)");
  CHK("(funcall 'dead-head)", "5");

  // `symbol-function` returns the raw cell -- a defalias designator stays a
  // symbol -- and an empty cell is void-function NAME.
  CHK("(fset 'h (lambda (x) x))", "(lambda (x) x)");
  CHK("(funcall (symbol-function 'h) 5)", "5");
  CHK("(defalias 'g2 'car)", "g2");
  CHK("(is (symbol-function 'g2) 'car)", "t");
  LISP2_ERR("(symbol-function 'nope)", "lisp2.fe:1: void-function nope");

  // `symbol-value` reads the global value cell, empty one void-variable NAME.
  CHK("(setq sv 5)", "5");
  CHK("(symbol-value 'sv)", "5");
  LISP2_ERR("(symbol-value 'nope)", "lisp2.fe:1: void-variable nope");

  // `fboundp` asks the function cell. The 04D cut moved the bootstrap into
  // function cells, so a primitive name answers t here -- `(boundp 'car)` is
  // nil, the mirror on the value side (compat `one-namespace-boundp`).
  CHK("(fboundp 'fresh-fn)", "nil");
  CHK("(fset 'ff (lambda () 1))", "(lambda nil 1)");
  CHK("(fboundp 'ff)", "t");
  CHK("(fboundp 'car)", "t");

  // The two unbound operations are disjoint: `makunbound` empties the value
  // cell and leaves the function cell callable; `fmakunbound` the reverse.
  CHK("(setq m 1)", "1");
  CHK("(fset 'm (lambda () 2))", "(lambda nil 2)");
  CHK("(makunbound 'm)", "m");
  CHK("(m)", "2");
  CHK("(setq m2 1)", "1");
  CHK("(fset 'm2 (lambda () 3))", "(lambda nil 3)");
  CHK("(fmakunbound 'm2)", "m2");
  CHK("m2", "1");
  LISP2_ERR("(m2)", "lisp2.fe:1: void-function m2");

  // `defalias` points one symbol's function cell at another, returns the
  // aliased symbol, and the chain is resolved at call time -- late binding.
  CHK("(defalias 'first 'car)", "first");
  CHK("(first (list 1 2))", "1");
  CHK("(defalias 'a2 'b2)", "a2");
  LISP2_ERR("(a2)", "lisp2.fe:1: void-function a2");
  CHK("(fset 'b2 (lambda () 1))", "(lambda nil 1)");
  CHK("(a2)", "1");

  // `function` is a raw-form special form: a symbol is the designator
  // itself; a lambda form is the closure, built by the same construction arm
  // `lambda` uses (so it captures the lexical environment); anything else is
  // unsupported-function-form.
  CHK("(is (function c2) 'c2)", "t");
  CHK("(funcall (function (lambda (x) (+ x 1))) 2)", "3");
  CHK("(funcall (function (fn (x) x)) 9)", "9");
  CHK("(funcall ((lambda (z) (function (lambda () z))) 7))", "7");
  CHK("(is (function (lambda (x) x)) (function (lambda (x) x)))", "nil");
  LISP2_ERR("(function 5)", "lisp2.fe:1: unsupported-function-form");
  LISP2_ERR("(function (car 1))", "lisp2.fe:1: unsupported-function-form");
  LISP2_ERR("(function f 1)", "lisp2.fe:1: wrong-number-of-arguments");

  // Cycles in the designator chain are named, in call position, in funcall,
  // and through the public API -- never left to exhaust the step budget.
  CHK("(fset 'x 'x)", "x");
  LISP2_ERR("(x)", "lisp2.fe:1: cyclic-function-indirection");
  LISP2_ERR("(funcall 'x)", "lisp2.fe:1: cyclic-function-indirection");
  const FeEvalOptions small_budget = {.step_limit = 16};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "lisp2.fe", "(funcall 'x)", strlen("(funcall 'x)"),
      &small_budget, "lisp2.fe:1: cyclic-function-indirection"));
  CHK("(defalias 'p 'q)", "p");
  CHK("(defalias 'q 'p)", "q");
  LISP2_ERR("(funcall 'p)", "lisp2.fe:1: cyclic-function-indirection");
  // Context fully reusable after the cycle errors, and the cell can be
  // rebound to something that is merely not callable.
  CHK("(+ 1 2)", "3");
  CHK("(fset 'x nil)", "nil");
  LISP2_ERR("(funcall 'x)", "lisp2.fe:1: tried to call non-callable value");
  LISP2_ERR("(funcall 'q)", "lisp2.fe:1: cyclic-function-indirection");

#undef LISP2_ERR
#undef CHK

  // The public function-namespace surface. A fresh symbol is neither bound
  // nor resolvable; FeSetFunction writes the cell, FeIsFBound sees it, and
  // FeGetFunction follows a symbol designator the way call position does.
  FeObject* api_fn = FeMakeSymbol(context, "api-fn");
  CHECK(!FeIsFBound(context, api_fn));
  CHECK(FeIsNil(FeGetFunction(context, api_fn)));
  const size_t gc = FeSaveGC(context);
  FeObject* const closure =
      FeEvaluateString(context, "api.fe", "(lambda (z) (+ z 1))",
                       sizeof("(lambda (z) (+ z 1))") - 1);
  FeSetFunction(context, api_fn, closure);
  FeRestoreGC(context, gc);
  CHECK(FeIsFBound(context, api_fn));
  CHECK(FeGetFunction(context, api_fn) == closure);
  FeObject* const alias = FeMakeSymbol(context, "api-alias");
  FeSetFunction(context, alias, api_fn);
  CHECK(FeGetFunction(context, alias) == closure);
  // `FeIsFunction` is `functionp`'s question over the same designator chain:
  // a closure, a native and a function-shaped primitive are functions; a
  // macro, a special form, an unbound name and a plain value are not. The
  // answers match Emacs' own `functionp` name for name.
  CHECK(FeIsFunction(context, closure));
  CHECK(FeIsFunction(context, api_fn));
  CHECK(FeIsFunction(context, alias));
  CHECK(FeIsFunction(context, FeMakeSymbol(context, "car")));
  CHECK(FeIsFunction(context, FeMakeSymbol(context, "funcall")));
  CHECK(FeIsFunction(context, FeMakeSymbol(context, "apply")));
  CHECK(FeIsFunction(context, FeMakeSymbol(context, "sqrt")));
  CHECK(!FeIsFunction(context, FeMakeSymbol(context, "if")));
  CHECK(!FeIsFunction(context, FeMakeSymbol(context, "quote")));
  CHECK(!FeIsFunction(context, FeMakeSymbol(context, "lambda")));
  CHECK(!FeIsFunction(context, FeMakeSymbol(context, "function")));
  CHECK(!FeIsFunction(context, FeMakeSymbol(context, "api-not-a-function")));
  CHECK(!FeIsFunction(context, FeMakeDouble(context, 5)));
  CHECK(!FeIsFunction(context, FeNil(context)));
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "api.fe", "(fset 'api-macro (macro (x) x))",
                       sizeof("(fset 'api-macro (macro (x) x))") - 1),
      "(macro (x) x)"));
  CHECK(!FeIsFunction(context, FeMakeSymbol(context, "api-macro")));
  CHECK(
      IsRendered(context,
                 FeEvaluateString(context, "api.fe", "(funcall 'api-alias 10)",
                                  sizeof("(funcall 'api-alias 10)") - 1),
                 "11"));
  // The cut moved callables into the function cell, which is the namespace
  // `FeGetFunction` resolves: a value-cell callable is *not* reachable
  // through the host API, exactly as it is not reachable in call position.
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "api.fe",
                       "(fset 'api-value (lambda (x) (* x 2)))",
                       sizeof("(fset 'api-value (lambda (x) (* x 2)))") - 1),
      "(lambda (x) (* x 2))"));
  FeObject* const api_value = FeMakeSymbol(context, "api-value");
  FeObject* const resolved = FeGetFunction(context, api_value);
  CHECK(!FeIsNil(resolved));
  FeObject* const arg = FeMakeDouble(context, 21);
  FeObject* const* const args = (FeObject* const[]){arg};
  CHECK(IsRendered(context, FeCall(context, resolved, args, 1), "42.0"));
  // Through the public API the very same cycle is `nil` and no error at all
  // (re-establish x's self-link, which the earlier `(fset 'x nil)` recovery
  // step cleared). This is the one reader of the chain that does not raise:
  // its caller is a C frame, so `FeHandleError`'s longjmp would land in an
  // outer evaluation's catch, or in none, rather than being contained. The
  // error handler is not reached, and `FeIsFBound` is what tells this `nil`
  // from the empty cell's `nil` above -- the cell holds `x`.
  FeObject* const cycled = FeMakeSymbol(context, "x");
  FeSetFunction(context, cycled, cycled);
  state.called = false;
  CHECK(FeIsNil(FeGetFunction(context, cycled)));
  CHECK(!state.called);
  CHECK(FeIsFBound(context, cycled));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(+ 1 1)",
                                    sizeof("(+ 1 1)") - 1),
                   "2"));
  // The split is the point, so pin both halves together: the Lisp-level
  // readers of that same cell are inside an evaluation the host can catch and
  // keep naming the cycle.
  CHECK(ExpectEvaluationError(context, &state, "lisp2.fe", "(funcall 'x)",
                              strlen("(funcall 'x)"),
                              "lisp2.fe:1: cyclic-function-indirection"));
  CHECK(ExpectEvaluationError(context, &state, "lisp2.fe", "(x)", strlen("(x)"),
                              "lisp2.fe:1: cyclic-function-indirection"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 04D: the cut's direct assertions, over and above what
// TestFunctionCells already pins -- the facts that only exist once the
// value-cell fallback is deleted. Fe is a Lisp-2 now: call position resolves
// the function cell only, the bootstrap lives in function cells (`t`, `pi`
// and `e` excepted), `#'` reads as `(function x)`, the writer prints
// `(function X)` as `#'X`, and `FeDefineNative` registers into the function
// cell (the FE_API_VERSION 3 meaning change, superseded by 4 in 05D).
static bool TestNamespaceCut(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                 \
  CHECK(IsRendered(context,                                                 \
                   FeEvaluateString(context, "cut.fe", expr, strlen(expr)), \
                   expected))
#define CUT_ERR(expr, message)                                               \
  CHECK(ExpectEvaluationError(context, &state, "cut.fe", expr, strlen(expr), \
                              message))

  // `(boundp 'car)` nil / `(fboundp 'car)` t: the bootstrap moved into
  // function cells, so the two namespaces give opposite answers for a
  // primitive name -- the direct assertions behind the `one-namespace-boundp`
  // and `lisp2-fboundp-primitive` compat flips.
  CHK("(boundp 'car)", "nil");
  CHK("(fboundp 'car)", "t");
  CHK("(boundp 'cons)", "nil");
  CHK("(fboundp 'cons)", "t");
  // `t` stays a value, as does a name that has only ever been setq'd.
  CHK("(boundp 't)", "t");
  CHK("(setq cut-value 7)", "7");
  CHK("(boundp 'cut-value)", "t");
  CHK("(fboundp 'cut-value)", "nil");

  // Value-position use of a primitive name is `void-variable`: the cut left
  // the primitive value cells empty, so `car` is not a value any more.
  CUT_ERR("car", "cut.fe:1: void-variable car");
  CUT_ERR("(setq x car)", "cut.fe:1: void-variable car");

  // A lexical binding never shadows call position: `(let car 5)` binds only
  // the value namespace, and `(car (list 1 2))` still resolves the function
  // cell (the pinned `lisp2-let-no-function-shadow` answer, spelled with
  // fe's one-binding `let`).
  CHK("((lambda () (let car 5) (car (list 1 2))))", "1");

  // `#'x` reads as `(function x)`: the structure, not the printing.
  CHK("(car (quote #'car))", "function");
  CHK("(car (cdr (quote #'car)))", "car");
  CHK("(is #'car 'car)", "t");
  // And the writer's `#'X` abbreviation round-trips the read.
  CHK("(quote #'car)", "#'car");
  CHK("(quote (function \"a\\\"b\"))", "#'\"a\\\"b\"");

  // `FeDefineNative` registers into the function cell (since FE_API_VERSION
  // 3, superseded by 4 in 05D):
  // observable through `symbol-function`, callable in head position, and
  // invisible to `boundp`.
  FeDefineNative(context, "cut-native", OrdinaryNative);
  CHK("(boundp 'cut-native)", "nil");
  CHK("(fboundp 'cut-native)", "t");
  CHK("(symbol-function 'cut-native)", "[native-fn]");
  CHK("(cut-native)", "42.0");
  FeObject* const cut_native = FeMakeSymbol(context, "cut-native");
  CHECK(FeGetType(FeGetFunction(context, cut_native)) == FeTNativeFn);

  // Context reuse after each new error above.
  CHK("(+ 1 2)", "3");

#undef CUT_ERR
#undef CHK

  FeCloseContext(context);
  return true;
}
// Sub-plan 08B: constants are protected at every value/function write seam,
// while keywords self-evaluate from their interned value cell.
static bool TestConstantsAndKeywords(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                              \
  CHECK(IsRendered(                                                      \
      context,                                                           \
      FeEvaluateString(context, "constants.fe", expr, sizeof(expr) - 1), \
      expected))
#define ERR(expr, condition)                                           \
  do {                                                                 \
    CHECK(ExpectEvaluationError(context, &state, "constants.fe", expr, \
                                strlen(expr),                          \
                                "constants.fe:1: setting-constant"));  \
    CHECK(IsRendered(context, FeGetCondition(context), condition));    \
  } while (false)
#define LET_ERR(expr)                                                   \
  CHECK(ExpectEvaluationError(context, &state, "constants.fe", expr,    \
                              strlen(expr),                             \
                              "constants.fe:1: lambda-list keyword in " \
                              "let binding"))

  CHK(":foo", ":foo");
  CHK("(eq :a ':a)", "t");
  CHK("(keywordp :foo)", "t");
  CHK("(keywordp 'foo)", "nil");
  // Measured, GNU Emacs 31.0.90: `(keywordp :)` is t, `:` self-evaluates to
  // `:`, and `(setq : 1)` raises `(setting-constant :)`. 08B guessed a
  // minimum name length of 2 instead of measuring, and asserted `nil` here.
  CHK("(keywordp ':)", "t");
  CHK("(keywordp :)", "t");
  CHK(":", ":");
  CHK("(eq : ':)", "t");
  ERR("(setq : 1)", "(setting-constant :)");
  ERR("(setq t nil)", "(setting-constant t)");
  ERR("(setq nil 1)", "(setting-constant nil)");
  ERR("(setq :foo 1)", "(setting-constant :foo)");
  ERR("(set 't 1)", "(setting-constant t)");
  ERR("(let ((t 1)) t)", "(setting-constant t)");
  ERR("(let ((nil 1)) 1)", "(setting-constant nil)");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "let-side-effect")));
  ERR("(let ((t (setq let-side-effect 1))) 2)", "(setting-constant t)");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "let-side-effect")));
  CHK("(setq let-outer 7)", "7");
  CHK("(let ((let-inner let-outer) (let-outer 9)) let-inner)", "7");
  ERR("(fset 't (lambda () 1))", "(setting-constant t)");
  ERR("(defalias ':foo 'car)", "(setting-constant :foo)");
  // All three lambda-parameter rows, measured rather than assumed. Emacs
  // 31.0.90 binds every one of them: `((lambda (t) t) 5)` is 5, and so are
  // `((lambda (nil) nil) 5)` and `((lambda (:kw) :kw) 5)` -- a lexical
  // binding of `nil` really does shadow the constant in the body. Fe follows
  // Emacs for `t` and is deliberately stricter for the other two: `nil` is
  // not a name any environment here can hold, and letting a keyword be
  // shadowed would undo the self-evaluation this slice just established.
  // Recorded as divergences in compat/features.json and doc/language.md.
  ERR("((lambda (nil) nil) 1)", "(setting-constant nil)");
  ERR("((lambda (:lambda-keyword) :lambda-keyword) 1)",
      "(setting-constant :lambda-keyword)");
  CHK("((lambda (t) t) 1)", "1");
  CHK("((lambda (t) (setq t 2)) 1)", "2");

  // `let` with a binding list compiles into a lambda application, which made
  // a lambda-list keyword in binding position bind as one: `(let ((&rest 1)
  // (x 2)) x)` answered `(1 2)` where Emacs answers 2.
  LET_ERR("(let ((&rest 1) (x 2)) x)");
  LET_ERR("(let ((&optional 1) (x 2)) x)");
  CHK("(let ((&foo 1)) &foo)", "1");
  CHK("(+ 1 2)", "3");

#define API_ERR(call, condition)                                    \
  do {                                                              \
    state.called = false;                                           \
    state.expected_message = "setting-constant";                    \
    const size_t volatile gc = FeSaveGC(context);                   \
    if (setjmp(state.jump) == 0) {                                  \
      call;                                                         \
      CHECK(false);                                                 \
    }                                                               \
    FeRestoreGC(context, gc);                                       \
    CHECK(state.called);                                            \
    CHECK(IsRendered(context, FeGetCondition(context), condition)); \
  } while (false)

  API_ERR(FeSetFunction(context, FeMakeSymbol(context, "t"),
                        FeMakeNativeFn(context, OrdinaryNative)),
          "(setting-constant t)");
  API_ERR(FeSetFunction(context, FeNil(context),
                        FeMakeNativeFn(context, OrdinaryNative)),
          "(setting-constant nil)");
  API_ERR(FeDefineNative(context, ":api-keyword", OrdinaryNative),
          "(setting-constant :api-keyword)");

#undef API_ERR

  // Interned keywords remain self-valued after enough allocation to collect.
  const FeObject* keyword = FeMakeSymbol(context, ":survives-gc");
  for (size_t i = 0; i < 5000; i++) {
    const size_t gc = FeSaveGC(context);
    (void)FeMakeString(context, "garbage");
    FeRestoreGC(context, gc);
  }
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "constants.fe", ":survives-gc",
                                    sizeof(":survives-gc") - 1),
                   ":survives-gc"));
  CHECK(keyword == FeMakeSymbol(context, ":survives-gc"));
  // 08B's outcome is that constancy survives collection, which means the
  // refusal has to survive it too -- re-evaluating the keyword only shows
  // the value cell is intact.
  ERR("(setq :survives-gc 1)", "(setting-constant :survives-gc)");
  ERR("(let ((:survives-gc 1)) 1)", "(setting-constant :survives-gc)");
  ERR("(setq t nil)", "(setting-constant t)");
  CHK("t", "t");

#undef LET_ERR
#undef ERR
#undef CHK
  FeCloseContext(context);
  return true;
}

// Sub-plan 02B: core `setq` (a special form) and `set` (ordinary-function
// semantics) alongside the still-working assignment `=` primitive. The
// compat corpus (fe/compat/) proves agreement with Emacs on the same
// properties; these are the implementation-focused assertions a
// process-per-case protocol cannot observe, e.g. state after a recovered
// error and evaluation order via a side effect.
static bool TestSetqAndSet(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                  \
  CHECK(IsRendered(                                                          \
      context, FeEvaluateString(context, "setq.fe", expr, sizeof(expr) - 1), \
      expected))
#define SETQ_ERR(expr, message)                                               \
  CHECK(ExpectEvaluationError(context, &state, "setq.fe", expr, strlen(expr), \
                              message))

  // Zero pairs is nil; multiple pairs return the last value, and a later
  // pair's value form already observes an earlier pair's new value.
  CHK("(setq)", "nil");
  CHK("(setq a 1 b a)", "1");
  CHK("(list a b)", "(1 1)");

  // A global assignment creates a previously unbound value.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "fresh-global")));
  CHK("(setq fresh-global 42)", "42");
  CHECK(FeIsBound(context, FeMakeSymbol(context, "fresh-global")));

  // A lexical assignment changes the local cell and leaves an existing
  // global cell of the same name unchanged.
  CHK("(setq shared 1)", "1");
  CHK("((lambda (shared) (setq shared 2) shared) 99)", "2");
  CHK("shared", "1");

  // On a value error, prior pairs remain assigned and later pairs do not
  // run; the same context goes on to evaluate other forms afterwards.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "p3")));
  SETQ_ERR("(setq p1 1 p2 missing-thing p3 3)",
           "setq.fe:1: void-variable missing-thing");
  CHK("p1", "1");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "p3")));

  // Odd form count: the earlier pair still stands; the dangling final
  // symbol is diagnosed only once it is reached.
  SETQ_ERR("(setq odd-a 5 odd-b)", "setq.fe:1: wrong-number-of-arguments");
  CHK("odd-a", "5");

  // A non-symbol target is a type error, checked before any value form
  // would be evaluated.
  SETQ_ERR("(setq 1 2)", "setq.fe:1: wrong-type-argument");

#undef SETQ_ERR
#undef CHK

  // `set`: ordinary-function semantics, and it always writes the global
  // cell -- the regression a later cleanup must not turn into an alias for
  // `setq`.
#define SET_CHK(expr, expected)                                             \
  CHECK(IsRendered(                                                         \
      context, FeEvaluateString(context, "set.fe", expr, sizeof(expr) - 1), \
      expected))
#define SET_ERR(expr, message)                                               \
  CHECK(ExpectEvaluationError(context, &state, "set.fe", expr, strlen(expr), \
                              message))

  SET_CHK("(set 'set-fresh 7)", "7");
  CHECK(FeIsBound(context, FeMakeSymbol(context, "set-fresh")));

  SET_CHK("(setq set-shared 9)", "9");
  SET_CHK("((lambda (set-shared) (list (set 'set-shared 2) set-shared)) 1)",
          "(2 1)");
  SET_CHK("set-shared", "2");

  // An arity error is raised before any raw argument form is evaluated.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "set-probe")));
  SET_ERR("(set 'set-target 1 (do (setq set-probe t) 2))",
          "set.fe:1: wrong-number-of-arguments");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "set-probe")));
  SET_ERR("(set 'x)", "set.fe:1: wrong-number-of-arguments");

  // An arity-correct call evaluates both forms left to right before
  // validating the first value's type, so a type error never erases a side
  // effect the second form already had.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "set-probe2")));
  SET_ERR("(set 1 (do (setq set-probe2 t) 2))",
          "set.fe:1: wrong-type-argument");
  CHECK(FeIsBound(context, FeMakeSymbol(context, "set-probe2")));

  SET_ERR("(set 'x)", "set.fe:1: wrong-number-of-arguments");
  SET_ERR("(set 'x 1 2)", "set.fe:1: wrong-number-of-arguments");
  CHECK(ExpectEvaluationError(context, &state, "setq.fe", "(setq a 1 b)",
                              strlen("(setq a 1 b)"),
                              "setq.fe:1: wrong-number-of-arguments"));

#undef SET_ERR
#undef SET_CHK

  FeCloseContext(context);
  return true;
}

// Sub-plan 02C: numeric `=`, replacing the old assignment primitive this
// same slice deletes (formerly pinned here as a regression: `(= old-name
// 7)` used to assign and return nil; `(= never-bound 3)` below now raises
// void-variable instead, proving the old meaning is gone). The compatibility
// corpus (fe/compat/) proves agreement with Emacs on the same properties;
// these are the implementation-focused assertions a process-per-case
// protocol cannot observe, e.g. that argument evaluation truly precedes
// type validation and that comparison never short-circuits.
static bool TestNumericEqual(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                    \
  CHECK(IsRendered(context,                                                    \
                   FeEvaluateString(context, "eq.fe", expr, sizeof(expr) - 1), \
                   expected))
#define EQ_ERR(expr, message)                                               \
  CHECK(ExpectEvaluationError(context, &state, "eq.fe", expr, strlen(expr), \
                              message))

  // Zero arguments is an error; one argument is true without comparing
  // anything; two or more are a left-to-right chain.
  EQ_ERR("(=)", "eq.fe:1: wrong-number-of-arguments");
  CHK("(= 1)", "t");
  CHK("(= 1 1)", "t");
  CHK("(= 1 1 1)", "t");
  CHK("(= 1 1 2)", "nil");
  CHK("(= 1 2 1)", "nil");

  // Ordinary-function semantics: every argument form runs, left to right,
  // even once the chain is already known unequal -- no short-circuit. A
  // later operand's side effect (mutating a counter) is always visible.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "eq-side-effects")));
  CHK("(setq eq-side-effects 0)", "0");
  CHK("(= 1 2 (do (setq eq-side-effects (+ eq-side-effects 1)) 3) "
      "     (do (setq eq-side-effects (+ eq-side-effects 1)) 4))",
      "nil");
  CHK("eq-side-effects", "2");

  // 0.0 and -0.0 compare equal; NaN is never equal to itself. Plain C `==`,
  // not special-cased.
  CHK("(= 0.0 -0.0)", "t");
  CHK("(= (sqrt -1) (sqrt -1))", "nil");

  // A type error in an early operand never erases a side effect a later
  // operand's form already had: argument evaluation precedes type
  // validation for the whole list, not just the failing one.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "eq-probe")));
  EQ_ERR("(= 1 \"1\")", "eq.fe:1: wrong-type-argument");
  EQ_ERR("(= 1 nil)", "eq.fe:1: wrong-type-argument");
  EQ_ERR("(= \"1\" (do (setq eq-probe t) 2))", "eq.fe:1: wrong-type-argument");
  CHK("eq-probe", "t");

  // The hard cut, proven directly: `=` no longer assigns, so an unbound
  // symbol is void-variable, and it stays unbound afterwards.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "never-bound")));
  EQ_ERR("(= never-bound 3)", "eq.fe:1: void-variable never-bound");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "never-bound")));

  // `setq` and `set` still work after the old assignment arm they replaced
  // is gone.
  CHK("(setq still-works 5)", "5");
  CHK("still-works", "5");
  CHK("(set 'set-still-works 6)", "6");
  CHK("set-still-works", "6");

#undef EQ_ERR
#undef CHK

  FeCloseContext(context);
  return true;
}

// Sub-plan 05C: the numeric tower, driven entirely through the host API. The
// reader cannot yet spell an integer (05D's cut), so every integer here is a
// host-made `FeMakeInteger` placed directly in a constructed form -- an
// atomic, non-symbol operand evaluates to itself, so `(+ int int)` needs no
// reader -- and the operator is a symbol resolved through the function
// namespace exactly as an evaluated call would be. The assertions are the
// 05A-pinned Emacs semantics for both numeric types: integer-preserving
// arithmetic with promotion when a double joins, truncating division,
// `arith-error` at the int64 edges (add/sub/mul overflow, division by zero,
// the INT64_MIN/-1 division edge), the unary seeds, variadic chained
// comparators in both directions, strictly-binary `/=`, cross-type `=`, the
// `integerp`/`floatp` predicates, per-function math native return types,
// context reuse after every error, and forced GC across the tower's resume
// states (the either-type arith accumulator, the chained-comparator list,
// the `=` chain, a unary predicate leaf, a math native). Every check here is
// the slice's contract; the pre-05C core satisfies only the double-shaped
// subset, so the rest are the measured blockers this suite's failing lines
// record.

typedef enum OperandKind {
  OperandInteger,
  OperandDouble,
  OperandString,
  OperandValue,
} OperandKind;

typedef struct Operand {
  OperandKind kind;
  union {
    int64_t i;
    double d;
    const char* text;
    FeObject* object;
  } value;
} Operand;

static Operand IntOperand(int64_t i) {
  return (Operand){.kind = OperandInteger, .value.i = i};
}

static Operand DoubleOperand(double d) {
  return (Operand){.kind = OperandDouble, .value.d = d};
}

// A non-number operand the numeric family must reject -- or, in the
// single-operand and settled-chain forms, must never look at.
static Operand StringOperand(const char* text) {
  return (Operand){.kind = OperandString, .value.text = text};
}

static Operand NilOperand(void) {
  return (Operand){.kind = OperandValue, .value.object = &nil};
}

static FeObject* MakeOperand(FeContext* context, const Operand* operand) {
  switch (operand->kind) {
    case OperandInteger:
      return FeMakeInteger(context, operand->value.i);
    case OperandDouble:
      return FeMakeDouble(context, operand->value.d);
    case OperandString:
      return FeMakeString(context, operand->value.text);
    case OperandValue:
      return operand->value.object;
  }
  return nullptr;
}

// `(name op1 op2 ...)`: one fresh cons spine, every node and operand rooted
// on the GC stack for the caller's whole save/restore scope.
static FeObject* MakeCallForm(FeContext* context,
                              const char* name,
                              FeObject* const* operands,
                              size_t count) {
  FeObject* form = &nil;
  for (size_t i = count; i > 0; i--) {
    form = FeCons(context, operands[i - 1], form);
  }
  return FeCons(context, FeMakeSymbol(context, name), form);
}

// Evaluates `(name ops...)`, asserting the result's exact type and rendering.
static bool CheckNumericForm(FeContext* context,
                             const char* name,
                             const Operand* operands,
                             size_t count,
                             FeType result_type,
                             const char* expected) {
  const size_t gc = FeSaveGC(context);
  FeObject* ops[8];
  CHECK(count <= sizeof(ops) / sizeof(ops[0]));
  for (size_t i = 0; i < count; i++) {
    ops[i] = MakeOperand(context, &operands[i]);
  }
  FeObject* form = MakeCallForm(context, name, ops, count);
  FeObject* result = FeEvaluate(context, form);
  const bool ok =
      FeGetType(result) == result_type && IsRendered(context, result, expected);
  FeRestoreGC(context, gc);
  return ok;
}

// Evaluates `(name ops...)`, expecting it to raise `expected`; the context
// must stay reusable afterwards.
static bool CheckNumericError(FeContext* context,
                              ErrorState* state,
                              const char* name,
                              const Operand* operands,
                              size_t count,
                              const char* expected) {
  const size_t gc = FeSaveGC(context);
  FeObject* ops[8];
  CHECK(count <= sizeof(ops) / sizeof(ops[0]));
  for (size_t i = 0; i < count; i++) {
    ops[i] = MakeOperand(context, &operands[i]);
  }
  FeObject* form = MakeCallForm(context, name, ops, count);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeEvaluate(context, form);
    FeRestoreGC(context, gc);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  CHECK(!FeIsNil(FeMakeBool(context, true)));
  return true;
}

// `(name int0 (do (setq n 0) (while (< n 2000) (setq n (+ n 1))
// (cons n n))) int1) int2 ...)`: operand `churn_index` is a collecting `do`
// that returns `integers[churn_index]`, so a collection happens while the
// `name` frame is suspended after an earlier operand, and the host integers
// in the frame's fields must survive to be combined or compared afterwards.
// The churn's last body form *is* the host integer, so the value delivered
// back into the suspended frame is one the caller made, not one the reader
// interned.
static bool GCSurvivesResume(FeContext* context,
                             const char* name,
                             const int64_t* integers,
                             size_t count,
                             size_t churn_index,
                             const char* expected) {
  const size_t gc = FeSaveGC(context);
  static const char churn[] =
      "(do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)))";
  size_t offset = 0;
  FeObject* churn_form =
      FeReadString(context, churn, sizeof(churn) - 1, &offset);
  CHECK(offset == sizeof(churn) - 1);
  FeObject* ops[8];
  CHECK(count <= sizeof(ops) / sizeof(ops[0]));
  for (size_t i = 0; i < count; i++) {
    ops[i] = FeMakeInteger(context, integers[i]);
  }
  FeObject* tail = churn_form;
  while (!FeIsNil(CDR(tail))) {
    tail = CDR(tail);
  }
  CDR(tail) = FeCons(context, ops[churn_index], &nil);
  ops[churn_index] = churn_form;
  FeObject* form = MakeCallForm(context, name, ops, count);
  const size_t before = FeGetArenaStats(context).collection_count;
  FeObject* result = FeEvaluate(context, form);
  const bool ok = FeGetArenaStats(context).collection_count > before &&
                  IsRendered(context, result, expected);
  FeRestoreGC(context, gc);
  return ok;
}

static bool TestNumericTower(void) {
  static TestArena arena;
  const size_t tower_size = FeMinimumArenaSize() + 8 * 1024;
  CHECK(tower_size <= sizeof(arena.bytes));
  FeContext* context = FeOpenContext(arena.bytes, tower_size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHECK_NUM(name, result_type, expected, ...)                \
  do {                                                             \
    const Operand operands[] = {__VA_ARGS__};                      \
    CHECK(CheckNumericForm(context, name, operands,                \
                           sizeof(operands) / sizeof(operands[0]), \
                           result_type, expected));                \
  } while (false)
#define CHECK_NUM_ERR(name, expected, ...)                          \
  do {                                                              \
    const Operand operands[] = {__VA_ARGS__};                       \
    CHECK(CheckNumericError(context, &state, name, operands,        \
                            sizeof(operands) / sizeof(operands[0]), \
                            expected));                             \
  } while (false)

  // int/int preservation: an all-integer reduction never leaves the integer
  // tag, stays exact past 2^53, and the zero-operand identities become
  // integers (invisible through the old printer, pinned by 05A's snapshots
  // after the cut).
  CHECK_NUM("+", FeTInteger, "3", IntOperand(1), IntOperand(2));
  CHECK_NUM("-", FeTInteger, "2", IntOperand(5), IntOperand(3));
  CHECK_NUM("*", FeTInteger, "6", IntOperand(2), IntOperand(3));
  CHECK_NUM("/", FeTInteger, "3", IntOperand(6), IntOperand(2));
  CHECK_NUM("+", FeTInteger, "9007199254740993", IntOperand(9007199254740993LL),
            IntOperand(0));
  CHECK_NUM("+", FeTInteger, "4611686018427387903",
            IntOperand(4611686018427387903LL), IntOperand(0));
  CHECK(CheckNumericForm(context, "+", nullptr, 0, FeTInteger, "0"));
  CHECK(CheckNumericForm(context, "*", nullptr, 0, FeTInteger, "1"));
  CHECK(CheckNumericForm(context, "-", nullptr, 0, FeTInteger, "0"));

  // FeCall itself: integers flow through the quote-wrapped argument path a
  // host call constructs, and the lambda's `+` stays integer end to end.
  {
    static const char add_function[] = "(fn (x y) (+ x y))";
    FeRoot* root = FeCreateRoot(
        context, FeEvaluateString(context, "tower.fe", add_function,
                                  sizeof(add_function) - 1));
    FeObject* arguments[] = {FeMakeInteger(context, 1),
                             FeMakeInteger(context, 2)};
    const size_t gc = FeSaveGC(context);
    FeObject* result = FeCall(context, FeGetRoot(root), arguments, 2);
    CHECK(FeGetType(result) == FeTInteger);
    CHECK(FeToInteger(context, result) == 3);
    FeRestoreGC(context, gc);
    FeReleaseRoot(context, root);
  }

  // Mixed promotion: once a double joins, the whole reduction is double.
  CHECK_NUM("+", FeTDouble, "3.5", IntOperand(1), DoubleOperand(2.5));
  CHECK_NUM("+", FeTDouble, "3.0", IntOperand(1), DoubleOperand(2.0));
  CHECK_NUM("*", FeTDouble, "6.0", IntOperand(2), DoubleOperand(3.0));
  CHECK_NUM("/", FeTDouble, "3.5", IntOperand(7), DoubleOperand(2.0));
  CHECK_NUM("/", FeTDouble, "3.5", DoubleOperand(7.0), IntOperand(2));
  CHECK_NUM("+", FeTDouble, "6.0", IntOperand(1), DoubleOperand(2.0),
            IntOperand(3));

  // Truncating division, toward zero for both signs; the unary seeds flip to
  // negation and reciprocal (05A A1/A2).
  CHECK_NUM("/", FeTInteger, "3", IntOperand(7), IntOperand(2));
  CHECK_NUM("/", FeTInteger, "-3", IntOperand(-7), IntOperand(2));
  CHECK_NUM("/", FeTInteger, "-3", IntOperand(7), IntOperand(-2));
  CHECK_NUM("/", FeTInteger, "3", IntOperand(-7), IntOperand(-2));
  CHECK_NUM("-", FeTInteger, "-5", IntOperand(5));
  CHECK_NUM("-", FeTDouble, "-5.5", DoubleOperand(5.5));
  CHECK_NUM("/", FeTInteger, "0", IntOperand(5));
  CHECK_NUM("/", FeTDouble, "0.5", DoubleOperand(2.0));

  // Zero and overflow errors: integer division by zero is `arith-error`, a
  // double divisor still yields a nonfinite float (05A A5, the printer is
  // 05D's), and every overflow operator refuses at the int64 edges --
  // `__builtin_*_overflow`, never UB, including the unary-negation and
  // INT64_MIN/-1 division edges C would leave undefined.
  CHECK_NUM_ERR("/", "arith-error", IntOperand(1), IntOperand(0));
  CHECK_NUM_ERR("/", "arith-error", IntOperand(-1), IntOperand(0));
  CHECK_NUM_ERR("/", "arith-error", IntOperand(0), IntOperand(0));
  CHECK_NUM("/", FeTDouble, "1.0e+INF", IntOperand(1), DoubleOperand(0.0));
  CHECK_NUM_ERR("+", "arith-error", IntOperand(INT64_MAX), IntOperand(1));
  CHECK_NUM_ERR("-", "arith-error", IntOperand(INT64_MIN), IntOperand(1));
  CHECK_NUM_ERR("*", "arith-error", IntOperand(INT64_MAX), IntOperand(2));
  CHECK_NUM_ERR("*", "arith-error", IntOperand(4611686018427387904LL),
                IntOperand(4));
  CHECK_NUM_ERR("-", "arith-error", IntOperand(INT64_MIN));
  CHECK_NUM_ERR("/", "arith-error", IntOperand(INT64_MIN), IntOperand(-1));

  // Chained comparisons, variadic in both directions; a double in the chain
  // promotes the comparison.
  CHECK_NUM("<", FeTSymbol, "t", IntOperand(1), IntOperand(2), IntOperand(3));
  CHECK_NUM("<", FeTNil, "nil", IntOperand(1), IntOperand(3), IntOperand(2));
  CHECK_NUM(">", FeTSymbol, "t", IntOperand(3), IntOperand(2), IntOperand(1));
  CHECK_NUM(">", FeTNil, "nil", IntOperand(1), IntOperand(2), IntOperand(3));
  CHECK_NUM(">=", FeTSymbol, "t", IntOperand(2), IntOperand(2), IntOperand(1));
  CHECK_NUM(">=", FeTNil, "nil", IntOperand(1), IntOperand(2), IntOperand(2));
  CHECK_NUM("<=", FeTSymbol, "t", IntOperand(1), IntOperand(2), IntOperand(2));
  CHECK_NUM("<=", FeTNil, "nil", IntOperand(2), IntOperand(1), IntOperand(2));
  CHECK_NUM("<", FeTSymbol, "t", IntOperand(2), IntOperand(3));
  CHECK_NUM(">", FeTSymbol, "t", IntOperand(3), IntOperand(2));
  CHECK_NUM("<", FeTSymbol, "t", IntOperand(1), DoubleOperand(2.5),
            IntOperand(3));
  CHECK_NUM("<", FeTNil, "nil", IntOperand(2), DoubleOperand(2.0),
            IntOperand(3));
  CHECK_NUM(">", FeTSymbol, "t", IntOperand(3), DoubleOperand(2.0),
            IntOperand(1));

  // `/=` is strictly binary: a third operand and zero operands are both
  // `wrong-number-of-arguments`, and it compares mathematical value across
  // the types.
  CHECK_NUM("/=", FeTSymbol, "t", IntOperand(1), IntOperand(2));
  CHECK_NUM("/=", FeTNil, "nil", IntOperand(2), IntOperand(2));
  CHECK_NUM("/=", FeTNil, "nil", IntOperand(3), DoubleOperand(3.0));
  CHECK_NUM("/=", FeTSymbol, "t", IntOperand(3), DoubleOperand(3.5));
  CHECK_NUM_ERR("/=", "wrong-number-of-arguments", IntOperand(5));
  CHECK_NUM_ERR("/=", "wrong-number-of-arguments", IntOperand(1), IntOperand(2),
                IntOperand(3));
  CHECK(CheckNumericError(context, &state, "/=", nullptr, 0,
                          "wrong-number-of-arguments"));

  // `=` across types: exact within integers, mathematical value across
  // int/float, and the pinned signed-zero/NaN answers survive the extension.
  CHECK_NUM("=", FeTSymbol, "t", IntOperand(3), IntOperand(3));
  CHECK_NUM("=", FeTSymbol, "t", IntOperand(3), DoubleOperand(3.0));
  CHECK_NUM("=", FeTSymbol, "t", DoubleOperand(3.0), IntOperand(3));
  CHECK_NUM("=", FeTSymbol, "t", IntOperand(1), IntOperand(1),
            DoubleOperand(1.0));
  CHECK_NUM("=", FeTNil, "nil", IntOperand(1), IntOperand(2),
            DoubleOperand(2.0));
  CHECK_NUM("=", FeTSymbol, "t", DoubleOperand(0.0), DoubleOperand(-0.0));
  {
    const size_t gc = FeSaveGC(context);
    FeObject* left_operands[] = {FeMakeInteger(context, -1)};
    FeObject* right_operands[] = {FeMakeInteger(context, -1)};
    FeObject* left = MakeCallForm(context, "sqrt", left_operands, 1);
    FeObject* right = MakeCallForm(context, "sqrt", right_operands, 1);
    FeObject* operands[] = {left, right};
    const FeObject* result =
        FeEvaluate(context, MakeCallForm(context, "=", operands, 2));
    CHECK(FeGetType(result) == FeTNil);
    FeRestoreGC(context, gc);
  }

  // The comparators' single-operand form is `t` with no type check at all,
  // Emacs' answer for every one of them (`(= "a")`, `(< t)` -- the pair loop
  // has no pair to run), and a chain stops at the first false pair, so a
  // non-number *after* a settled answer is never reached. A non-number
  // reached while the chain is still true is still `wrong-type-argument`.
  {
    static const char* const comparators[] = {"=", "<", "<=", ">", ">="};
    for (size_t i = 0; i < sizeof(comparators) / sizeof(comparators[0]); i++) {
      CHECK_NUM(comparators[i], FeTSymbol, "t", StringOperand("a"));
      CHECK_NUM(comparators[i], FeTSymbol, "t", NilOperand());
      CHECK_NUM(comparators[i], FeTSymbol, "t", IntOperand(1));
    }
  }
  CHECK_NUM("<", FeTNil, "nil", IntOperand(2), IntOperand(1),
            StringOperand("a"));
  CHECK_NUM("=", FeTNil, "nil", IntOperand(1), IntOperand(2),
            StringOperand("a"));
  CHECK_NUM(">", FeTNil, "nil", IntOperand(1), IntOperand(2),
            StringOperand("a"));
  CHECK_NUM_ERR("<", "wrong-type-argument", IntOperand(1), IntOperand(2),
                StringOperand("a"));
  CHECK_NUM_ERR("=", "wrong-type-argument", IntOperand(1), IntOperand(1),
                StringOperand("a"));
  CHECK_NUM_ERR("=", "wrong-type-argument", IntOperand(1), StringOperand("1"));

  // `is`'s integer arm (05A Decision 2): mathematical value across int/float,
  // exact within integers, epsilon behaviour kept for double/double.
  CHECK_NUM("is", FeTSymbol, "t", IntOperand(3), IntOperand(3));
  CHECK_NUM("is", FeTSymbol, "t", IntOperand(3), DoubleOperand(3.0));
  CHECK_NUM("is", FeTNil, "nil", IntOperand(3), IntOperand(4));
  CHECK_NUM("is", FeTSymbol, "t", DoubleOperand(0.0), DoubleOperand(0.0));
  // The mixed pair carries the *same* tolerance as a double/double pair, in
  // both orders: 3.0000000000000004 is `(cube-root 27)`, and scripts/math.fe
  // asserts `(is 3 (cube-root 27))` with the integer spelling on the left.
  CHECK_NUM("is", FeTSymbol, "t", IntOperand(3),
            DoubleOperand(3.0000000000000004));
  CHECK_NUM("is", FeTSymbol, "t", DoubleOperand(3.0000000000000004),
            IntOperand(3));
  CHECK_NUM("is", FeTSymbol, "t", DoubleOperand(3.0),
            DoubleOperand(3.0000000000000004));
  CHECK_NUM("is", FeTNil, "nil", IntOperand(3), DoubleOperand(3.001));
  // `eq`/`eql` stay exact over the same pair: they are Emacs' identity and
  // type-strict value equality, not fe's tolerant `is`.
  CHECK_NUM("eql", FeTNil, "nil", DoubleOperand(3.0),
            DoubleOperand(3.0000000000000004));
  CHECK_NUM("eql", FeTNil, "nil", IntOperand(3),
            DoubleOperand(3.0000000000000004));
  CHECK_NUM("eq", FeTNil, "nil", IntOperand(3), DoubleOperand(3.0));

  // The predicates answer by tag and never raise on a non-number.
  CHECK_NUM("integerp", FeTSymbol, "t", IntOperand(3));
  CHECK_NUM("integerp", FeTNil, "nil", DoubleOperand(3.0));
  CHECK_NUM("integerp", FeTNil, "nil", NilOperand());
  CHECK_NUM("floatp", FeTSymbol, "t", DoubleOperand(3.0));
  CHECK_NUM("floatp", FeTNil, "nil", IntOperand(3));
  CHECK_NUM("floatp", FeTNil, "nil", NilOperand());

  // Math native return types, per 05A's M rows: the rounding family returns
  // integers (round half-even), `expt` follows its per-signature rule, the
  // transcendentals stay float.
  CHECK_NUM("floor", FeTInteger, "7", DoubleOperand(7.5));
  CHECK_NUM("floor", FeTInteger, "3", IntOperand(7), IntOperand(2));
  CHECK_NUM("floor", FeTInteger, "-4", DoubleOperand(-7.5), IntOperand(2));
  CHECK_NUM("truncate", FeTInteger, "-7", DoubleOperand(-7.5));
  CHECK_NUM("ceiling", FeTInteger, "8", DoubleOperand(7.5));
  CHECK_NUM("ceiling", FeTInteger, "-3", IntOperand(-7), IntOperand(2));
  CHECK_NUM("round", FeTInteger, "2", DoubleOperand(2.5));
  CHECK_NUM("round", FeTInteger, "4", DoubleOperand(3.5));
  CHECK_NUM("round", FeTInteger, "-2", DoubleOperand(-2.5));
  CHECK_NUM("round", FeTInteger, "-4", DoubleOperand(-3.5));
  CHECK_NUM("expt", FeTInteger, "256", IntOperand(2), IntOperand(8));
  CHECK_NUM("expt", FeTDouble, "0.5", IntOperand(2), IntOperand(-1));
  CHECK_NUM("expt", FeTDouble, "256.0", DoubleOperand(2.0), IntOperand(8));
  CHECK_NUM("expt", FeTDouble, "256.0", IntOperand(2), DoubleOperand(8.0));
  CHECK_NUM("sqrt", FeTDouble, "4.0", IntOperand(16));
  CHECK_NUM("sqrt", FeTDouble, "4.0", DoubleOperand(16.0));
  CHECK_NUM("sin", FeTDouble, "0.0", IntOperand(0));
  CHECK_NUM("sin", FeTDouble, "0.9999999999999997", DoubleOperand(1.5707963));

  // The rounding family's unrepresentable inputs: NaN and both infinities
  // have no integer answer, and the conversion C would perform on them is
  // undefined (the NaN case was the one that reached `(int64_t)x` and
  // returned INT64_MIN; UBSan reports it). Emacs says `overflow-error`,
  // whose condition chain includes `arith-error`, which is the single name
  // fe's message level can carry. `CheckNumericError` re-checks the context
  // after every one of these, so context reuse after each error is covered
  // by the same call.
  static const char* const rounding[] = {"floor", "ceiling", "round",
                                         "truncate"};
  for (size_t i = 0; i < sizeof(rounding) / sizeof(rounding[0]); i++) {
    CHECK(CheckNumericError(context, &state, rounding[i],
                            (const Operand[]){DoubleOperand((double)NAN)}, 1,
                            "arith-error"));
    CHECK(CheckNumericError(context, &state, rounding[i],
                            (const Operand[]){DoubleOperand((double)INFINITY)},
                            1, "arith-error"));
    CHECK(CheckNumericError(context, &state, rounding[i],
                            (const Operand[]){DoubleOperand(-(double)INFINITY)},
                            1, "arith-error"));
    // The two-argument degenerate: a zero divisor of either type is
    // `arith-error` before the division, so `(floor 0 0)` never reaches the
    // NaN conversion and `(floor 5 0)` is refused as a divide by zero rather
    // than as an out-of-range infinity.
    CHECK(CheckNumericError(context, &state, rounding[i],
                            (const Operand[]){IntOperand(0), IntOperand(0)}, 2,
                            "arith-error"));
    CHECK(CheckNumericError(context, &state, rounding[i],
                            (const Operand[]){IntOperand(5), IntOperand(0)}, 2,
                            "arith-error"));
    CHECK(
        CheckNumericError(context, &state, rounding[i],
                          (const Operand[]){IntOperand(5), DoubleOperand(-0.0)},
                          2, "arith-error"));
    // A nonfinite *divisor* is not degenerate: the quotient is a signed zero
    // and the answer is the integer 0, Emacs' answer for `(floor 1 -1.0e+INF)`
    // too.
    CHECK(CheckNumericForm(
        context, rounding[i],
        (const Operand[]){IntOperand(1), DoubleOperand(-(double)INFINITY)}, 2,
        FeTInteger, "0"));
  }
  // Context reuse once more after the whole battery, from source text.
  CHECK(CheckNumericForm(context, "floor",
                         (const Operand[]){DoubleOperand(7.5)}, 1, FeTInteger,
                         "7"));

  // Context reuse after every error: each `arith-error`/arity path above
  // already re-ran the context check; a fresh reduction still answers.
  CHECK(CheckNumericForm(context, "+",
                         (const Operand[]){IntOperand(1), IntOperand(2)}, 2,
                         FeTInteger, "3"));

  // GC across the tower's resume states: a collecting `do` as an operand
  // forces collections while the frame below is suspended, and the host
  // integers held in the frame's fields must survive to be combined after.
  {
    const int64_t arith[] = {1000, 1000};
    CHECK(GCSurvivesResume(context, "+", arith, 2, 1, "2000"));
    const int64_t less[] = {1, 2, 3};
    CHECK(GCSurvivesResume(context, "<", less, 3, 1, "t"));
    const int64_t equal[] = {1, 1, 1};
    CHECK(GCSurvivesResume(context, "=", equal, 3, 1, "t"));
    const int64_t unary[] = {5};
    CHECK(GCSurvivesResume(context, "integerp", unary, 1, 0, "t"));
    const int64_t native[] = {5};
    CHECK(GCSurvivesResume(context, "floor", native, 1, 0, "5"));
    // The `FeFrameBinary` arms take the same treatment: `eq`/`eql` hold the
    // first operand in the frame across the churn and must still find the
    // integer they were given, and `/=` is the comparator arm that is not
    // part of the chained loop above.
    const int64_t identical[] = {1000, 1000};
    CHECK(GCSurvivesResume(context, "eq", identical, 2, 1, "t"));
    CHECK(GCSurvivesResume(context, "eql", identical, 2, 1, "t"));
    CHECK(GCSurvivesResume(context, "is", identical, 2, 1, "t"));
    const int64_t distinct[] = {1000, 2000};
    CHECK(GCSurvivesResume(context, "/=", distinct, 2, 1, "t"));
    CHECK(GCSurvivesResume(context, "eq", distinct, 2, 1, "nil"));
  }

#undef CHECK_NUM_ERR
#undef CHECK_NUM

  FeCloseContext(context);
  return true;
}

// Sub-plan 05D: the numeric cut, driven through the *reader* at last. Every
// 05A R/P row is a read-or-print test (`1.` is the integer 1, `.5` the float
// 0.5, `0x10`/`inf`/`nan`/`1e` symbols, `9007199254740993` exact, `-0.0`
// round-trips, `42.0` prints `42.0`, the nonfinite spellings read and print),
// the E rows are `eq`/`eql` per 05A's collision table, and `(/ 7 2)` is the
// integer 3 through source text at last. Context reuse is checked after each
// new error.
static bool TestNumericCut(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // Reads `source` and asserts the exact type and rendering.
#define READS_AS(source, type, expected)                            \
  do {                                                              \
    const size_t gc = FeSaveGC(context);                            \
    size_t offset = 0;                                              \
    FeObject* object =                                              \
        FeReadString(context, source, sizeof(source) - 1, &offset); \
    CHECK(object != nullptr);                                       \
    CHECK(FeGetType(object) == type);                               \
    CHECK(IsRendered(context, object, expected));                   \
    CHECK(offset == sizeof(source) - 1);                            \
    FeRestoreGC(context, gc);                                       \
  } while (false)
#define EVAL_AS(expr, expected)                                             \
  CHECK(IsRendered(                                                         \
      context, FeEvaluateString(context, "cut.fe", expr, sizeof(expr) - 1), \
      expected))
#define CUT_ERR(expr, message)                                               \
  CHECK(ExpectEvaluationError(context, &state, "cut.fe", expr, strlen(expr), \
                              message))

  // R rows: integer = optional sign, digits, optional trailing dot; float =
  // a fraction and/or an exponent; the nonfinite spellings; everything else
  // is a symbol.
  READS_AS("42", FeTInteger, "42");
  READS_AS("+5", FeTInteger, "5");
  READS_AS("-5", FeTInteger, "-5");
  READS_AS("1.", FeTInteger, "1");
  READS_AS("-0", FeTInteger, "0");
  READS_AS(".5", FeTDouble, "0.5");
  READS_AS("42.0", FeTDouble, "42.0");
  READS_AS("-0.0", FeTDouble, "-0.0");
  READS_AS("1e3", FeTDouble, "1000.0");
  READS_AS("1.e3", FeTDouble, "1000.0");
  READS_AS("0x10", FeTSymbol, "0x10");
  READS_AS("inf", FeTSymbol, "inf");
  READS_AS("nan", FeTSymbol, "nan");
  READS_AS("1e", FeTSymbol, "1e");
  READS_AS("1.0e+", FeTSymbol, "1.0e+");
  READS_AS("1.0e+INF", FeTDouble, "1.0e+INF");
  READS_AS("-1.0e+INF", FeTDouble, "-1.0e+INF");
  READS_AS("0.0e+NaN", FeTDouble, "0.0e+NaN");
  READS_AS("-0.0e+NaN", FeTDouble, "-0.0e+NaN");
  // The nonfinite spellings need the explicit `+`; a missing sign is not a
  // positive one. `1eINF`/`1eNaN`/`1EINF` are symbols in Emacs and now here,
  // and their signed neighbours are re-confirmed on both sides of the rule.
  READS_AS("1eINF", FeTSymbol, "1eINF");
  READS_AS("1eNaN", FeTSymbol, "1eNaN");
  READS_AS("1EINF", FeTSymbol, "1EINF");
  READS_AS("1e-INF", FeTSymbol, "1e-INF");
  READS_AS("1e+INF", FeTDouble, "1.0e+INF");
  READS_AS("1E+INF", FeTDouble, "1.0e+INF");
  READS_AS("1e+NaN", FeTDouble, "0.0e+NaN");
  READS_AS("1e+Inf", FeTSymbol, "1e+Inf");
  READS_AS("1.0e+NAN", FeTSymbol, "1.0e+NAN");

  // R9: int64 exactness past 2^53, read and printed exactly.
  READS_AS("9007199254740993", FeTInteger, "9007199254740993");

  // P rows: the printer's `.0` guarantee and shortest-round-trip, through
  // source text.
  EVAL_AS("42.0", "42.0");
  EVAL_AS("0.1", "0.1");
  EVAL_AS("(/ 1.0 0)", "1.0e+INF");
  EVAL_AS("(- 0 (/ 1.0 0))", "-1.0e+INF");
  EVAL_AS("(sqrt -1)", "-0.0e+NaN");

  // The tower through the reader at last: integer division truncates,
  // integer division by zero errors, mixed arithmetic promotes, and the
  // predicates answer by tag.
  EVAL_AS("(/ 7 2)", "3");
  EVAL_AS("(/ -7 2)", "-3");
  EVAL_AS("(/ 5)", "0");
  EVAL_AS("(+ 1 2.0)", "3.0");
  EVAL_AS("(integerp 42)", "t");
  EVAL_AS("(integerp 42.0)", "nil");
  EVAL_AS("(floatp 42.0)", "t");
  EVAL_AS("(floatp 42)", "nil");
  CUT_ERR("(/ 1 0)", "cut.fe:1: arith-error");

  // E rows: `eq` is pointer identity or both-integers-equal; `eql` is `eq`
  // or same-type numbers equal by bits. Two separately-read float literals
  // are two boxed objects, so `(eq 3.0 3.0)` is nil; two integers answer t.
  EVAL_AS("(eq 3 3)", "t");
  EVAL_AS("(eq 3 4)", "nil");
  EVAL_AS("(eq 3.0 3.0)", "nil");
  EVAL_AS("(eq \"a\" \"a\")", "nil");
  EVAL_AS("(eq 'a 'a)", "t");
  EVAL_AS("(eql 3 3)", "t");
  EVAL_AS("(eql 3 3.0)", "nil");
  EVAL_AS("(eql 1.5 1.5)", "t");
  EVAL_AS("(eql 0.0 -0.0)", "nil");

  // E arity: `eq`/`eql` are strictly binary like `/=` (05A row C3) -- the
  // raw arity is rejected before anything evaluates, so a missing or extra
  // operand is `wrong-number-of-arguments`, never a comparison.
  CUT_ERR("(eq)", "cut.fe:1: wrong-number-of-arguments");
  CUT_ERR("(eq 1)", "cut.fe:1: wrong-number-of-arguments");
  CUT_ERR("(eq 1 2 3)", "cut.fe:1: wrong-number-of-arguments");
  CUT_ERR("(eql)", "cut.fe:1: wrong-number-of-arguments");
  CUT_ERR("(eql 1)", "cut.fe:1: wrong-number-of-arguments");
  CUT_ERR("(eql 1 2 3)", "cut.fe:1: wrong-number-of-arguments");

  // Context reuse after the errors above.
  EVAL_AS("(+ 1 2)", "3");
  EVAL_AS("(eq 'a 'a)", "t");
  EVAL_AS("(eql 1.5 1.5)", "t");

#undef CUT_ERR
#undef EVAL_AS
#undef READS_AS

  FeCloseContext(context);
  return true;
}

static bool TestMacroExpansion(void) {
  // Deliberately tight: the expansion has to survive the collections that
  // evaluating it provokes, and nothing but Fe's GC stack refers to it.
  static TestArena arena;
  const size_t size = FeMinimumArenaSize() + 8192;
  CHECK(size <= sizeof(arena.bytes));
  FeContext* context = FeOpenContext(arena.bytes, size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char loop[] =
      "(fset 'make (macro (a b) (list 'list a b)))"
      "(setq n 0)"
      "(setq acc nil)"
      "(while (< n 200) (setq acc (make n n)) (setq n (+ n 1)))"
      "acc";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "gc.fe", loop, sizeof(loop) - 1),
                   "(199 199)"));

  // An error raised by the expansion, and one raised while expanding, both
  // leave the context usable.
  CHECK(ExpectEvaluationError(context, &state, "expansion.fe",
                              "(fset 'bad (macro () '(car 1))) (bad)",
                              strlen("(fset 'bad (macro () '(car 1))) (bad)"),
                              "expansion.fe:1: expected pair, got integer"));
  CHECK(
      ExpectEvaluationError(context, &state, "expander.fe",
                            "(fset 'worse (macro () (car 1))) (worse)",
                            strlen("(fset 'worse (macro () (car 1))) (worse)"),
                            "expander.fe:1: expected pair, got integer"));
  CHECK(IsRendered(context, FeEvaluateString(context, "after.fe", "(+ 1 2)", 7),
                   "3"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 10B: `macroexpand-1` and `macroexpand`. Every expectation below
// was measured on the pinned oracle first
// (/opt-3/emacs-31-lucid/bin/emacs, GNU Emacs 31.0.90, build 2026-07-09,
// TERM=xterm-256color) and the measured answer is quoted beside it; the
// `comparison: emacs` cases in compat/ pin the same answers mechanically.
// Fe renders `(quote x)` where Emacs prints `'x` -- a recorded printer
// divergence, not a disagreement about the expansion -- so the transformers
// here deliberately expand to forms with no `quote` in them.
static bool TestMacroexpandPrimitives(void) {
  static TestArena arena;
  const size_t size = FeMinimumArenaSize() + 16384;
  CHECK(size <= sizeof(arena.bytes));
  FeContext* context = FeOpenContext(arena.bytes, size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define EXPANDS_AS(source, expected)                                        \
  CHECK(IsRendered(                                                         \
      context,                                                              \
      FeEvaluateString(context, "expand.fe", (source), sizeof(source) - 1), \
      (expected)))

  static const char setup[] =
      // `(my-when COND BODY)` is Emacs' `when` written in fe's own macro
      // spelling, so the two dialects can be compared on the same shape.
      "(fset 'my-when (macro (c b) (list 'if c (list 'do b))))"
      // `outer` expands to a call to `inner`, which expands again: the
      // nested case that tells `macroexpand-1` and `macroexpand` apart.
      "(fset 'inner (macro (x) (list '+ x 1)))"
      "(fset 'outer (macro (x) (list 'inner x)))"
      // A one-parameter transformer, for the strict-arity assertions.
      "(fset 'one-arg (macro (a) (list 'not a)))"
      // Two links of `defalias` indirection over that transformer.
      "(defalias 'ali 'one-arg)"
      "(defalias 'ali2 'ali)"
      // A plain function, not a macro: a call to it is its own expansion,
      // and so is a call through an alias to it, or to nothing at all.
      "(fset 'plain (fn (x) x))"
      "(defalias 'pf 'plain)"
      "(defalias 'dangling 'no-such-name)";
  CHECK(FeEvaluateString(context, "setup.fe", setup, sizeof(setup) - 1) !=
        nullptr);

  // A user macro. Emacs: (macroexpand-1 '(when t 1)) => (if t (progn 1)).
  EXPANDS_AS("(macroexpand-1 '(my-when t 1))", "(if t (do 1))");
  EXPANDS_AS("(macroexpand '(my-when t 1))", "(if t (do 1))");

  // A macro that expands to another macro call: one step versus the
  // fixpoint. Emacs: (macroexpand-1 '(outer 2)) => (inner 2), and
  // (macroexpand '(outer 2)) => (+ 2 1).
  EXPANDS_AS("(macroexpand-1 '(outer 2))", "(inner 2)");
  EXPANDS_AS("(macroexpand '(outer 2))", "(+ 2 1)");

  // A non-macro form and an atom are their own expansions, and so is a call
  // to an unbound name or to a plain function. Emacs answers each of these
  // unchanged, including the improper-looking `(1 2)` whose head is not a
  // symbol at all.
  EXPANDS_AS("(macroexpand-1 '(+ 1 2))", "(+ 1 2)");
  EXPANDS_AS("(macroexpand '(+ 1 2))", "(+ 1 2)");
  EXPANDS_AS("(macroexpand-1 '(plain 3))", "(plain 3)");
  EXPANDS_AS("(macroexpand-1 '(no-such-name 1))", "(no-such-name 1)");
  EXPANDS_AS("(macroexpand-1 '((lambda (x) x) 1))", "((lambda (x) x) 1)");
  EXPANDS_AS("(macroexpand-1 '(1 2))", "(1 2)");
  EXPANDS_AS("(macroexpand-1 'foo)", "foo");
  EXPANDS_AS("(macroexpand-1 42)", "42");
  EXPANDS_AS("(macroexpand-1 nil)", "nil");
  EXPANDS_AS("(macroexpand 'foo)", "foo");

  // Alias-following. Measured, and *not* what "resolve the chain and apply"
  // would give: Emacs stops at each `defalias` link, so one step through a
  // two-link chain answers the middle link -- (macroexpand-1 '(ali2 7)) is
  // (ali 7) -- and only the fixpoint reaches the transformer's own answer.
  EXPANDS_AS("(macroexpand-1 '(ali 7))", "(one-arg 7)");
  EXPANDS_AS("(macroexpand-1 '(ali2 7))", "(ali 7)");
  EXPANDS_AS("(macroexpand '(ali 7))", "(not 7)");
  EXPANDS_AS("(macroexpand '(ali2 7))", "(not 7)");
  // The head is rewritten only when the target is itself a macro (Emacs'
  // `(and (symbolp def) (macrop def))`). An alias to a plain function and an
  // alias to an unbound name both leave the form completely alone --
  // measured: (macroexpand-1 '(pf 1)) is (pf 1), not (plainfn 1), and
  // (macroexpand-1 '(dangling 1)) is (dangling 1), not (no-such-name 1).
  EXPANDS_AS("(macroexpand-1 '(pf 1))", "(pf 1)");
  EXPANDS_AS("(macroexpand '(pf 1))", "(pf 1)");
  EXPANDS_AS("(macroexpand-1 '(dangling 1))", "(dangling 1)");
  EXPANDS_AS("(macroexpand '(dangling 1))", "(dangling 1)");

  // ENVIRONMENT is accepted and must be nil; the nil-default path is the
  // one Emacs was measured on -- (macroexpand-1 '(when t 1) nil) is the
  // same (if t (progn 1)).
  EXPANDS_AS("(macroexpand-1 '(my-when t 1) nil)", "(if t (do 1))");
  EXPANDS_AS("(macroexpand '(my-when t 1) nil)", "(if t (do 1))");

  // An expansion is reachable through the function namespace like any other
  // function: Emacs' `(functionp 'macroexpand-1)` is t.
  EXPANDS_AS("(funcall 'macroexpand-1 '(outer 2))", "(inner 2)");
  EXPANDS_AS("(apply 'macroexpand (list '(outer 2)))", "(+ 2 1)");

  // Strict arity of the transformer *under expansion*: the same
  // `wrong-number-of-arguments` the evaluator raises for the direct call,
  // naming the macro rather than `macroexpand-1`. Emacs agrees on the
  // condition and the count; its FUNCTION element is the transformer object
  // (`#[(a) ((not a)) nil]`) where fe names the symbol, which is the
  // recorded phase7-arity-condition-rendering divergence.
  CHECK(ExpectEvaluationError(context, &state, "arity.fe",
                              "(macroexpand-1 '(one-arg 1 2))",
                              strlen("(macroexpand-1 '(one-arg 1 2))"),
                              "arity.fe:1: wrong-number-of-arguments"));
  CHECK(ExpectEvaluationError(context, &state, "arity.fe",
                              "(macroexpand '(one-arg))",
                              strlen("(macroexpand '(one-arg))"),
                              "arity.fe:1: wrong-number-of-arguments"));
  EXPANDS_AS(
      "(condition-case e (macroexpand-1 '(one-arg 1 2))"
      " (wrong-number-of-arguments (car (cdr (cdr e)))))",
      "2");
  // Through an alias, the fixpoint reaches the same check.
  EXPANDS_AS(
      "(condition-case e (macroexpand '(ali 1 2))"
      " (wrong-number-of-arguments (car (cdr (cdr e)))))",
      "2");
  // An improper argument tail has no count, so it is a type error about the
  // tail -- Emacs: (macroexpand-1 '(one-arg . 3)) is
  // (wrong-type-argument listp 3).
  EXPANDS_AS(
      "(condition-case e (macroexpand-1 '(one-arg . 3)) (error (car e)))",
      "wrong-type-argument");

  // Arity of the expanders themselves: `(FORM &optional ENVIRONMENT)`,
  // measured -- Emacs answers wrong-number-of-arguments for both.
  CHECK(ExpectEvaluationError(context, &state, "self.fe", "(macroexpand-1)",
                              strlen("(macroexpand-1)"),
                              "self.fe:1: wrong-number-of-arguments"));
  CHECK(ExpectEvaluationError(context, &state, "self.fe",
                              "(macroexpand '(a) nil 'extra)",
                              strlen("(macroexpand '(a) nil 'extra)"),
                              "self.fe:1: wrong-number-of-arguments"));

  // A non-nil ENVIRONMENT is rejected by name rather than ignored. Emacs
  // implements the alist (`(macroexpand-1 '(foo) '((foo lambda (&rest _)
  // 99)))` is 99 there); fe says which feature that is instead of answering
  // as if the argument had not been passed.
  CHECK(ExpectEvaluationError(
      context, &state, "env.fe", "(macroexpand-1 '(my-when t 1) '((x . 1)))",
      strlen("(macroexpand-1 '(my-when t 1) '((x . 1)))"),
      "env.fe:1: unsupported feature: macroexpand environment"));
  CHECK(ExpectEvaluationError(
      context, &state, "env.fe", "(macroexpand '(my-when t 1) 'anything)",
      strlen("(macroexpand '(my-when t 1) 'anything)"),
      "env.fe:1: unsupported feature: macroexpand environment"));
  // The rejection is a catchable condition, not a hard exit.
  EXPANDS_AS("(condition-case e (macroexpand-1 'x 'env) (error 'rejected))",
             "rejected");

  // `macroexpand-all` names itself. It is emphatically NOT `void-function`,
  // which is byte-identical to what a typo produces (10A Decision 2 and
  // Decision 5).
  CHECK(ExpectEvaluationError(
      context, &state, "all.fe", "(macroexpand-all '(my-when t 1))",
      strlen("(macroexpand-all '(my-when t 1))"),
      "all.fe:1: unsupported feature: macroexpand-all"));
  CHECK(ExpectEvaluationError(
      context, &state, "all.fe", "(funcall 'macroexpand-all 'x)",
      strlen("(funcall 'macroexpand-all 'x)"),
      "all.fe:1: unsupported feature: macroexpand-all"));
  CHECK(ExpectEvaluationError(
      context, &state, "all.fe", "(apply 'macroexpand-all (list 'x))",
      strlen("(apply 'macroexpand-all (list 'x))"),
      "all.fe:1: unsupported feature: macroexpand-all"));
  EXPANDS_AS("(fboundp 'macroexpand-all)", "t");
  EXPANDS_AS(
      "(condition-case e (macroexpand-all 'x) (void-function 'wrong)"
      " (error 'named))",
      "named");
  // Although the function is a reject-by-name stub, it remains an ordinary
  // function: operands evaluate before it reports that the implementation is
  // missing, including through funcall/apply's evaluate-then-redispatch path.
  EXPANDS_AS(
      "(setq all-args nil)"
      "(condition-case e (macroexpand-all (setq all-args (cons 'direct "
      "all-args))) (error nil))"
      "(condition-case e (funcall 'macroexpand-all (setq all-args (cons "
      "'funcall all-args))) (error nil))"
      "(condition-case e (apply 'macroexpand-all (list (setq all-args (cons "
      "'apply all-args)))) (error nil))"
      "all-args",
      "(apply funcall direct)");

#undef EXPANDS_AS

  FeCloseContext(context);
  return true;
}

// A macro whose expansion is another call to itself has no fixpoint, so
// `macroexpand` must be bounded by the same step budget every other
// evaluation is -- not hang, and not run out of frames either.  The other
// apparent cycle, a `defalias` ring, never enters the fixpoint: resolving
// whether its target is a macro raises `cyclic-function-indirection` first.
// Both termination policies are pinned below.
static bool TestMacroexpandBudget(void) {
  static TestArena arena;
  const size_t size = FeMinimumArenaSize() + 16384;
  CHECK(size <= sizeof(arena.bytes));
  FeContext* context = FeOpenContext(arena.bytes, size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char setup[] =
      "(fset 'selfy (macro args (list 'selfy)))"
      "(defalias 'ring-a 'ring-b)"
      "(defalias 'ring-b 'ring-a)";
  CHECK(FeEvaluateString(context, "setup.fe", setup, sizeof(setup) - 1) !=
        nullptr);

  const FeEvalOptions budget = {.step_limit = 256};
  CHECK(ExpectCompletionKind(
      context, &state, "selfy.fe", "(macroexpand '(selfy))",
      strlen("(macroexpand '(selfy))"), &budget,
      "selfy.fe:1: evaluation step limit exceeded", FeCompletionBudget));

  // It is the *step* budget that stops it, not the frame wall: the fixpoint
  // reuses one frame per pass, so the frame stack stays shallow however many
  // expansions it takes. The peak below is the whole session's high-water
  // mark, including the setup forms.
  const FeArenaStats stats = FeGetArenaStats(context);
  CHECK(stats.peak_frame_depth < 16);

  // The other route to a nonterminating expansion cannot even start: an
  // alias ring is refused by the same `cyclic-function-indirection` call
  // position raises for it, because deciding whether a head's alias target
  // is a macro resolves the chain. Emacs never reaches this shape at all --
  // its `defalias` refuses to close the ring -- so this is fe's own policy,
  // and the load-bearing half is that it is an immediate raise rather than a
  // spin inside the fixpoint loop.
  CHECK(ExpectEvaluationError(context, &state, "ring.fe",
                              "(macroexpand '(ring-a 1))",
                              strlen("(macroexpand '(ring-a 1))"),
                              "ring.fe:1: cyclic-function-indirection"));
  CHECK(ExpectEvaluationError(context, &state, "ring.fe",
                              "(macroexpand-1 '(ring-a 1))",
                              strlen("(macroexpand-1 '(ring-a 1))"),
                              "ring.fe:1: cyclic-function-indirection"));

  // `macroexpand-1` takes exactly one step and terminates, with a budget far
  // smaller than the one the fixpoint exhausted.
  const FeEvalOptions one_step = {.step_limit = 64};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(
                       context, "one.fe", "(macroexpand-1 '(selfy))",
                       strlen("(macroexpand-1 '(selfy))"), &one_step),
                   "(selfy)"));

  // The context is still usable after the budget exhaustion and the raises.
  CHECK(IsRendered(context, FeEvaluateString(context, "after.fe", "(+ 1 2)", 7),
                   "3"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 10B's rooting, proved rather than reasoned about. Every arm of an
// expansion allocates -- `ArgsToEnv` builds the callee environment, an alias
// step conses a new head onto the old tail, the fixpoint re-pushes the form
// on the GC stack each pass -- and the values it is allocating over live in
// exactly one place (a frame field, or the one GC-stack slot the fixpoint
// keeps). `TestArityDataUnderCollection`'s squeeze is reused here: the arena
// is filled to a fixed headroom before each run, so a collection lands
// *inside* the expansion rather than before it, at 32 different points.
//
// This is deliberately a deterministic test and not a fuzz-grammar arm. The
// steered evaluator grammar builds its heads from fixed name lists and cannot
// emit `macroexpand` at all -- measured, 0 of 6639 dumped forms -- and adding
// an arm would re-map every tracked seed's byte stream, which is the failure
// mode `fuzz/seeds/reachability.json` exists to catch. See doc/FUZZING.md.
static bool TestMacroexpandUnderCollection(void) {
  static const char* const forms[] = {
      // A transformer body, whose environment `ArgsToEnv` allocates.
      "'(macroexpand-1 '(outer 2))",
      // A fixpoint: a body, then another body, on one frame.
      "'(macroexpand '(outer 2))",
      // A single alias substitution: the one arm that conses a new form.
      "'(macroexpand-1 '(ali2 7))",
      // Two substitutions and then a transformer, all on one frame.
      "'(macroexpand '(ali2 7))",
  };
  static const char* const expected[] = {
      "(inner 2)",
      "(+ 2 1)",
      "(ali 7)",
      "(not 7)",
  };

  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char setup[] =
      "(fset 'inner (macro (x) (list '+ x 1)))"
      "(fset 'outer (macro (x) (list 'inner x)))"
      "(fset 'one-arg (macro (a) (list 'not a)))"
      "(defalias 'ali 'one-arg)"
      "(defalias 'ali2 'ali)";
  CHECK(FeEvaluateString(context, "setup.fe", setup, sizeof(setup) - 1) !=
        nullptr);

  for (size_t which = 0; which < sizeof(forms) / sizeof(forms[0]); which++) {
    FeRoot* const form =
        FeCreateRoot(context, FeEvaluateString(context, "form.fe", forms[which],
                                               strlen(forms[which])));
    CHECK(form != nullptr);
    size_t collected = 0;
    for (size_t headroom = 1; headroom <= 32; headroom++) {
      const size_t gc = FeSaveGC(context);
      while (FeGetArenaStats(context).free_slots > headroom) {
        (void)FeCons(context, FeNil(context), FeNil(context));
        FeRestoreGC(context, gc);
      }
      const size_t collections = FeGetArenaStats(context).collection_count;
      CHECK(IsRendered(context, FeEvaluate(context, FeGetRoot(form)),
                       expected[which]));
      if (FeGetArenaStats(context).collection_count > collections) {
        collected++;
      }
      FeRestoreGC(context, gc);
    }
    printf("macroexpand under collection: form %zu collected in %zu of 32\n",
           which, collected);
    CHECK(collected >= 4);
    FeReleaseRoot(context, form);
  }

  FeCloseContext(context);
  return true;
}

static bool ReadsAs(FeContext* context,
                    const char* source,
                    const char* expected) {
  const size_t gc = FeSaveGC(context);
  size_t offset = 0;
  FeObject* object = FeReadString(context, source, strlen(source), &offset);
  CHECK(object != nullptr);
  CHECK(IsRendered(context, object, expected));
  CHECK(offset == strlen(source));
  FeRestoreGC(context, gc);
  return true;
}

static bool TestDottedLists(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // Well-formed lists, proper and improper, nested both ways.
  CHECK(ReadsAs(context, "(a b c)", "(a b c)"));
  CHECK(ReadsAs(context, "(a . b)", "(a . b)"));
  CHECK(ReadsAs(context, "(a b . c)", "(a b . c)"));
  CHECK(ReadsAs(context, "(a . (b c))", "(a b c)"));
  CHECK(ReadsAs(context, "(a . nil)", "(a)"));
  CHECK(ReadsAs(context, "((a . b) (c . d))", "((a . b) (c . d))"));
  CHECK(ReadsAs(context, "(a (b . (c . d)) . e)", "(a (b c . d) . e)"));
  CHECK(ReadsAs(context, "()", "nil"));

  // Whitespace and comments around the dot do not change the parse.
  CHECK(ReadsAs(context, "(a\n .\n b)", "(a . b)"));
  CHECK(ReadsAs(context, "(a ; comment\n . b)", "(a . b)"));
  CHECK(ReadsAs(context, "(a . b ; trailing\n )", "(a . b)"));

  // Outside a list, `.` is an ordinary symbol and `.5` is still a number.
  CHECK(ReadsAs(context, ".", "."));
  CHECK(ReadsAs(context, ".5", "0.5"));
  CHECK(ReadsAs(context, "(.5 x)", "(0.5 x)"));

  // Malformed dotted syntax is refused, with the offset of the byte that
  // settled it.
  CHECK(ExpectReadError(context, &state, "(. a)", 5,
                        "byte 2: '.' at start of list"));
  CHECK(ExpectReadError(context, &state, "(a .)", 5,
                        "byte 4: missing value after '.'"));
  CHECK(ExpectReadError(context, &state, "(a . b c)", 9,
                        "byte 8: extra value after dotted tail"));
  CHECK(ExpectReadError(context, &state, "(a . b . c)", 11,
                        "byte 8: extra value after dotted tail"));
  CHECK(ExpectReadError(context, &state, "(a .", 4, "byte 4: unclosed list"));
  CHECK(ExpectReadError(context, &state, "(a . b", 6, "byte 6: unclosed list"));
  CHECK(ExpectReadError(context, &state, "(a . b c", 8,
                        "byte 8: extra value after dotted tail"));

  // The reader is still usable afterwards.
  CHECK(ReadsAs(context, "(1 . 2)", "(1 . 2)"));

  FeCloseContext(context);
  return true;
}

// Renders `source` into a `size`-byte window of a redzoned buffer and checks
// the returned length, the stored bytes, and that nothing outside the window
// was touched.
static bool CheckRendered(FeContext* context,
                          const char* source,
                          size_t size,
                          size_t expected_length,
                          const char* expected) {
  enum { Redzone = 8, BufferSize = 64 };
  static_assert(BufferSize > 2 * Redzone);
  char buffer[BufferSize];
  memset(buffer, '#', sizeof(buffer));
  CHECK(size <= BufferSize - 2 * Redzone);

  FeObject* object =
      FeEvaluateString(context, "render.fe", source, strlen(source));
  const size_t written = FeToString(context, object, buffer + Redzone, size);
  CHECK(written == expected_length);
  for (size_t i = 0; i < Redzone; i++) {
    CHECK(buffer[i] == '#');
    CHECK(buffer[Redzone + size + i] == '#');
  }
  if (size == 0) {
    return true;
  }
  CHECK(buffer[Redzone + written] == '\0');
  CHECK(memcmp(buffer + Redzone, expected, written) == 0);
  return true;
}

static bool TestSerialization(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // A zero-size destination writes nothing and accepts a null pointer.
  CHECK(FeToString(context, FeNil(context), nullptr, 0) == 0);
  CHECK(CheckRendered(context, "\"hello\"", 0, 0, ""));
  CHECK(CheckRendered(context, "'(1 2 3)", 0, 0, ""));

  // A one-byte destination holds only the terminator.
  CHECK(CheckRendered(context, "\"hello\"", 1, 0, ""));

  // Exact fit, one byte short, and generous.
  CHECK(CheckRendered(context, "\"hello\"", 6, 5, "hello"));
  CHECK(CheckRendered(context, "\"hello\"", 5, 4, "hell"));
  CHECK(CheckRendered(context, "\"hello\"", 32, 5, "hello"));

  // Atoms, nested lists, and truncation in the middle of one.
  CHECK(CheckRendered(context, "nil", 8, 3, "nil"));
  CHECK(CheckRendered(context, "42", 8, 2, "42"));
  CHECK(CheckRendered(context, "'sym", 8, 3, "sym"));
  CHECK(CheckRendered(context, "'(1 (2 3) . 4)", 32, 13, "(1 (2 3) . 4)"));
  CHECK(CheckRendered(context, "'(1 (2 3) . 4)", 6, 5, "(1 (2"));

  FeCloseContext(context);
  return true;
}

// State for `with-resource`, below. A plain static rather than context
// userdata because `ErrorState` already occupies that slot in every test
// that also wants error recovery.
typedef struct ResourceState {
  FILE* file;
  bool open;
  int close_result;
  int close_count;
} ResourceState;

static ResourceState resource_state;

static void ResetResourceState(void) {
  resource_state = (ResourceState){0};
}

// The `FeCleanupFn` `with-resource` registers with `FeProtectWithCleanup`.
// Closes the real FILE* opened by `with-resource`, so a failure to run (or a
// double run) is visible as a wrong `fclose` result or count, not just a
// flag.
static void CloseResourceCleanup(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    void* data) {
  (void)context;
  ResourceState* state = data;
  state->close_result = fclose(state->file);
  state->open = false;
  state->close_count++;
}

// `(with-resource THUNK)`: opens a real file, registers its cleanup through
// `FeProtectWithCleanup`, then calls THUNK (a zero-argument closure) and
// returns its result. This is the shape the sub-plan's kg-side consumers
// (`save-excursion`, `with-current-buffer`) are meant to use: a native that
// protects a resource around a Lisp body it was handed, not around itself.
static FeObject* WithResource(FeContext* context, FeObject* args) {
  FeObject* thunk = FeGetNextArgument(context, &args);
  FeRequireNoArguments(context, args);
  resource_state.file = tmpfile();
  if (resource_state.file == nullptr) {
    FeHandleError(context, "tmpfile failed");
  }
  resource_state.open = true;
  FeProtectWithCleanup(context, CloseResourceCleanup, &resource_state);
  return FeCall(context, thunk, nullptr, 0);
}

// Redirects `stderr` into a tempfile for the duration of `body(userdata)`,
// then restores it and copies everything captured into `buffer`
// (NUL-terminated, `size` including the terminator). The cleanup tests use
// this to assert on the diagnostic a failing `unwind-protect` cleanup
// prints.
//
// Every early exit releases whatever it already acquired before returning
// `false`, which is the real fix -- the original inline version of this
// code (one copy per test) leaked `saved_stderr` and the tempfile on
// exactly these paths.
//
// The two `dup2`s are deliberately unchecked. `-fanalyzer` models the
// descriptor `dup2` returns as a fresh leakable resource, so *any* form
// that reads the return value is reported as a leak -- including storing
// it in a variable and testing that, which was measured. It cannot be
// satisfied here, because the descriptor `dup2` returns is `STDERR_FILENO`
// itself and outlives the process. Checking `errno` instead would silence
// it while asserting something POSIX does not promise: `errno` is
// unspecified after a *successful* call, so a libc that probes internally
// would make this report a failure that did not happen. Not checking
// asserts nothing false. A failed redirect still fails the test, one step
// later and for the honest reason -- the diagnostic goes to the real
// stderr, `buffer` comes back empty, and the caller's `strstr` finds
// nothing.
static bool CaptureStderr(void (*body)(void* userdata),
                          void* userdata,
                          char* buffer,
                          size_t size) {
  fflush(stderr);
  const int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr == -1) {
    return false;
  }
  FILE* file = tmpfile();
  // Spelled `!file` rather than `file == nullptr`: cppcheck's exhaustive
  // value-flow analysis does not recognise C23 `nullptr` as the null
  // constant `tmpfile()` returns, and reads the `return false` below as
  // leaking a `file` that is null on exactly that path.
  if (!file) {
    close(saved_stderr);
    return false;
  }
  (void)dup2(fileno(file), STDERR_FILENO);

  body(userdata);

  fflush(stderr);
  (void)dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);
  rewind(file);
  const size_t read_length = fread(buffer, 1, size - 1, file);
  buffer[read_length] = '\0';
  fclose(file);
  return true;
}

// `CaptureStderr`'s `body`: runs one `Expect*Error` call and stashes its
// result, so a capture site only needs to fill in the arguments and read
// `result` back out.
typedef struct EvalCall {
  FeContext* context;
  ErrorState* state;
  const char* label;
  const char* source;
  size_t length;
  const FeEvalOptions* options;  // nullptr selects `ExpectEvaluationError`.
  const char* expected;
  bool result;
} EvalCall;

static void RunEvalCall(void* userdata) {
  EvalCall* call = userdata;
  call->result =
      call->options
          ? ExpectEvaluationOptionsError(
                call->context, call->state, call->label, call->source,
                call->length, call->options, call->expected)
          : ExpectEvaluationError(call->context, call->state, call->label,
                                  call->source, call->length, call->expected);
}

static bool TestUnwindHostAPI(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "with-resource", WithResource);

  // Normal return: the cleanup still runs, exactly once.
  ResetResourceState();
  static const char normal[] = "(with-resource (fn () 42))";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "host.fe", normal, sizeof(normal) - 1),
      "42"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_result == 0);
  CHECK(resource_state.close_count == 1);

  // Error: the body raises, the cleanup still runs exactly once, and the
  // error still propagates to the host.
  ResetResourceState();
  static const char erroring[] = "(with-resource (fn () (car 1)))";
  CHECK(ExpectEvaluationError(context, &state, "host.fe", erroring,
                              sizeof(erroring) - 1,
                              "host.fe:1: expected pair, got integer"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_count == 1);

  // Interrupt: a host `C-g` mid-body still runs the cleanup exactly once.
  ResetResourceState();
  static const char looping[] = "(with-resource (fn () (while t 1)))";
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3};
  const FeEvalOptions interrupt_options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  CHECK(ExpectEvaluationOptionsError(context, &state, "host.fe", looping,
                                     sizeof(looping) - 1, &interrupt_options,
                                     "host.fe:1: evaluation cancelled"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_count == 1);

  // Budget exhaustion: the cleanup is not gated on steps remaining -- it
  // still runs to completion (a bare `fclose`, so this mainly documents the
  // invariant) even though the body's own budget hit zero.
  ResetResourceState();
  const FeEvalOptions tiny_budget = {.step_limit = 8};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "host.fe", looping, sizeof(looping) - 1, &tiny_budget,
      "host.fe:1: evaluation step limit exceeded"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_count == 1);

  FeCloseContext(context);
  return true;
}

static bool TestUnwindLisp(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  static ErrorState state;
  state = (ErrorState){.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                    \
  CHECK(IsRendered(                                                            \
      context, FeEvaluateString(context, "unwind.fe", expr, sizeof(expr) - 1), \
      expected))

  // Normal return: the cleanup runs, exactly once, and the body's value
  // still comes back.
  CHK("(setq run-count 0) (unwind-protect 42 (setq run-count (+ run-count 1)))",
      "42");
  CHK("run-count", "1");

  // Error: the body raises, the cleanup still runs exactly once, and the
  // error still propagates to the host with its own message intact.
  // `setq` returns the assigned value, unlike old assignment `=`.
  CHK("(setq run-count 0)", "0");
  static const char erroring[] =
      "(unwind-protect (car 1) (setq run-count (+ run-count 1)))";
  CHECK(ExpectEvaluationError(context, &state, "unwind.fe", erroring,
                              sizeof(erroring) - 1,
                              "unwind.fe:1: expected pair, got integer"));
  CHK("run-count", "1");

  // Interrupt: a host `C-g` mid-body still runs the cleanup exactly once.
  CHK("(setq run-count 0)", "0");
  static const char looping[] =
      "(unwind-protect (while t 1) (setq run-count (+ run-count 1)))";
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3};
  const FeEvalOptions interrupt_options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  CHECK(ExpectEvaluationOptionsError(context, &state, "unwind.fe", looping,
                                     sizeof(looping) - 1, &interrupt_options,
                                     "unwind.fe:1: evaluation cancelled"));
  CHK("run-count", "1");

  // Budget exhaustion: cleanup is not gated on steps remaining, and still
  // runs exactly once. The cleanup form itself needs more steps than the
  // tiny budget the body exhausted, which would fail immediately if it were
  // still charged against that budget instead of running unbounded, as
  // `doc/unwind-design.md` and the sub-plan both require.
  CHK("(setq run-count 0) (setq spin-count 0)", "0");
  static const char budget_cleanup[] =
      "(unwind-protect (while t 1) "
      "  (do (while (< spin-count 200) (setq spin-count (+ spin-count 1))) "
      "      (setq run-count (+ run-count 1))))";
  const FeEvalOptions tiny_budget = {.step_limit = 8};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "unwind.fe", budget_cleanup, sizeof(budget_cleanup) - 1,
      &tiny_budget, "unwind.fe:1: evaluation step limit exceeded"));
  CHK("run-count", "1");
  CHK("spin-count", "200");

  // Three levels of nesting, inner error: cleanups run innermost first
  // (LIFO), and the nesting is visible in the order they append to `log`.
  CHK("(setq log '())", "nil");
  static const char nested[] =
      "(unwind-protect"
      "  (unwind-protect"
      "    (unwind-protect"
      "      (assert nil)"
      "      (setq log (cons 'inner log)))"
      "    (setq log (cons 'middle log)))"
      "  (setq log (cons 'outer log)))";
  CHECK(ExpectEvaluationError(context, &state, "unwind.fe", nested,
                              sizeof(nested) - 1,
                              "unwind.fe:1: assertion failure"));
  CHK("log", "(outer middle inner)");

  // A cleanup that itself errors: 06A Decision 4 -- the cleanup's own error
  // replaces the one already unwinding, so that is what reaches the host,
  // carrying the same `unwind.fe:` source label any other error from this
  // evaluation carries; nothing is printed to stderr behind the host's
  // back; and the outer cleanup still runs.
  CHK("(setq outer-ran nil)", "nil");
  static const char failing_cleanup[] =
      "(unwind-protect"
      "  (unwind-protect"
      "    (assert nil)"
      "    (car 1))"
      "  (setq outer-ran t))";
  EvalCall failing_cleanup_call = {
      .context = context,
      .state = &state,
      .label = "unwind.fe",
      .source = failing_cleanup,
      .length = sizeof(failing_cleanup) - 1,
      .options = nullptr,
      .expected = "unwind.fe:1: expected pair, got integer"};
  char captured[512];
  CHECK(CaptureStderr(RunEvalCall, &failing_cleanup_call, captured,
                      sizeof(captured)));
  CHECK(failing_cleanup_call.result);
  CHECK(captured[0] == '\0');
  CHK("outer-ran", "t");

  // Root survival: a lexical binding created in the body -- reachable only
  // through the environment `unwind-protect` captured, not through the
  // global symbol table -- must still resolve to the right object in the
  // cleanup after the body allocates heavily enough to force repeated
  // collections.
  CHK("(setq survivor nil)", "nil");
  static const char root_survival[] =
      "(do"
      "  (let x (cons 111 222))"
      "  (unwind-protect"
      "    (do"
      "      (setq gc-pressure-i 0)"
      "      (while (< gc-pressure-i 4000)"
      "        (cons gc-pressure-i gc-pressure-i)"
      "        (setq gc-pressure-i (+ gc-pressure-i 1)))"
      "      (assert nil))"
      "    (setq survivor x)))";
  CHECK(ExpectEvaluationError(context, &state, "unwind.fe", root_survival,
                              sizeof(root_survival) - 1,
                              "unwind.fe:1: assertion failure"));
  CHK("survivor", "(111 . 222)");

#undef CHK

  FeCloseContext(context);
  return true;
}

// A cleanup's fresh budget (`FeEvalOptions.cleanup_step_limit`) is what
// stops it from hanging kg with no escape when it does not return on its
// own -- the property `RunCleanupsAfterError` exists for. These two cases
// are its regression coverage: the step-limit escape hatch and the
// interrupt escape hatch, each exercised in isolation from the other so a
// fix to one path cannot silently rely on the other one also catching it.
static bool TestUnwindCleanupBudget(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                    \
  CHECK(IsRendered(                                                            \
      context, FeEvaluateString(context, "budget.fe", expr, sizeof(expr) - 1), \
      expected))

  // A cleanup that never returns is terminated by its own fresh budget --
  // not left to hang forever, the way leaving evaluation unbounded during
  // the drain would -- and the *original* error (the body's, not the
  // cleanup's own step-limit failure) is still what reaches the host.
  static const char runaway_cleanup[] = "(unwind-protect (car 1) (while t 1))";
  const FeEvalOptions small_cleanup_budget = {.cleanup_step_limit = 50};
  EvalCall runaway_cleanup_call = {
      .context = context,
      .state = &state,
      .label = "budget.fe",
      .source = runaway_cleanup,
      .length = sizeof(runaway_cleanup) - 1,
      .options = &small_cleanup_budget,
      .expected = "budget.fe:1: evaluation step limit exceeded"};
  char captured[512];
  CHECK(CaptureStderr(RunEvalCall, &runaway_cleanup_call, captured,
                      sizeof(captured)));
  CHECK(runaway_cleanup_call.result);
  CHECK(captured[0] == '\0');
  // The kind travels with the replaced completion: the cleanup ran out of
  // its own *budget*, so the host is told Budget, not Error.
  CHECK(state.observed_completion == FeCompletionBudget);

  // The regression this whole feature exists to keep passing: a body that
  // exhausted a tiny budget of its own still gets a cleanup that runs to
  // completion, because the fresh budget above is a *replacement*, not a
  // further restriction stacked on top of what the body already spent.
  CHK("(setq tiny-budget-ran nil)", "nil");
  static const char tiny_body_budget[] =
      "(unwind-protect (while t 1) (setq tiny-budget-ran t))";
  const FeEvalOptions tiny_budget = {.step_limit = 8};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "budget.fe", tiny_body_budget,
      sizeof(tiny_body_budget) - 1, &tiny_budget,
      "budget.fe:1: evaluation step limit exceeded"));
  CHK("tiny-budget-ran", "t");

  // A cleanup that never returns is instead interrupted by a *second* host
  // interrupt -- the first one is what unwound the body in the first
  // place, and must not also immediately abort the cleanup that runs
  // because of it. That one cleanup entry aborts (a printed diagnostic,
  // like any other cleanup failure), and the drain still continues to the
  // outer entry.
  CHK("(setq outer-ran nil)", "nil");
  static const char runaway_interrupted_cleanup[] =
      "(unwind-protect"
      "  (unwind-protect (while t 1) (while t 1))"
      "  (setq outer-ran t))";
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3,
                              .cancel_after_second = 10};
  const FeEvalOptions interrupted_cleanup_budget = {.poll_interval = 4,
                                                    .interrupt = Interrupt,
                                                    .userdata = &interrupt,
                                                    .cleanup_step_limit = 5000};
  EvalCall runaway_interrupted_call = {
      .context = context,
      .state = &state,
      .label = "budget.fe",
      .source = runaway_interrupted_cleanup,
      .length = sizeof(runaway_interrupted_cleanup) - 1,
      .options = &interrupted_cleanup_budget,
      .expected = "budget.fe:1: evaluation cancelled"};
  CHECK(CaptureStderr(RunEvalCall, &runaway_interrupted_call, captured,
                      sizeof(captured)));
  CHECK(runaway_interrupted_call.result);
  CHECK(interrupt.polls >= interrupt.cancel_after_second);
  CHECK(captured[0] == '\0');
  // Same rule for the other escape hatch: a cleanup aborted by a host
  // interrupt reaches the host as Quit.
  CHECK(state.observed_completion == FeCompletionQuit);
  CHK("outer-ran", "t");

#undef CHK

  FeCloseContext(context);
  return true;
}

// `FeRaiseCompletion`, the public raise for the kinds `FeHandleError` cannot
// spell. Its contract is narrow on purpose: Error, Quit and Budget are the
// three a host may legitimately raise, and each is given the condition
// object that kind is defined to carry, so nothing a host raises can leave
// `FeGetCondition` answering with a leftover.
static bool TestHostRaiseCompletion(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "raise-host-quit", RaiseHostQuit);
  FeDefineNative(context, "raise-host-budget", RaiseHostBudget);
  FeDefineNative(context, "raise-host-error", RaiseHostError);

#define CHK(expr, expected)                                                   \
  CHECK(IsRendered(                                                           \
      context, FeEvaluateString(context, "raise.fe", expr, sizeof(expr) - 1), \
      expected))

  // Quit: a `(quit ...)` handler catches it and the object is `(quit)`,
  // exactly as for fe's own interrupt path.
  CHK("(condition-case e (raise-host-quit) (quit (list 'q e)))", "(q (quit))");
  // Error: an ordinary `(error "msg")` condition, catchable as one.
  CHK("(condition-case e (raise-host-error) (error e))",
      "(error \"host error\")");
  // Budget: catchable by nothing, `t` included, and it reaches the host with
  // the kind and with no condition object.
  static const char budget[] =
      "(condition-case nil (raise-host-budget) (t 'nope))";
  CHECK(ExpectCompletionKind(context, &state, "raise.fe", budget,
                             sizeof(budget) - 1, nullptr,
                             "raise.fe:1: host budget", FeCompletionBudget));

#undef CHK

  FeCloseContext(context);
  return true;
}

// The protected call (`FeTryCallWithOptions`) and the re-signal that goes
// with it. The property under test is the one the plain `FeCall` path cannot
// have: a completion raised by a nested run started from inside a native
// stops at a barrier that is still live, instead of transferring to the
// enclosing run's barrier past the native's own C frame.
static bool TestProtectedCall(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "contain-call", ContainCall);
  FeDefineNative(context, "wrap-call", WrapCall);

#define CHK(expr, expected)                                            \
  CHECK(IsRendered(                                                    \
      context,                                                         \
      FeEvaluateString(context, "protect.fe", expr, sizeof(expr) - 1), \
      expected))

  const size_t gc = FeSaveGC(context);

  // A normal completion passes the value back.
  CHK("(contain-call (lambda () (+ 40 2)))", "(ok 42)");
  CHECK(FeGetCompletion(context) == FeCompletionNormal);

  // An error from the nested run is contained: the condition object, the
  // kind and the message are all readable in the native's own frame, and
  // the GC stack there is back where it started.
  contained_gc_balanced = false;
  contained_message_seen = false;
  CHK("(contain-call (lambda () (car 5)))",
      "(contained (wrong-type-argument listp 5))");
  CHECK(contained_kind == FeCompletionError);
  CHECK(contained_gc_balanced);
  CHECK(contained_message_seen);

  // A throw is contained too. It finds no catch inside the protected call
  // (the containment barrier is also a throw wall), becomes `no-catch`, and
  // stops there.
  CHK("(contain-call (lambda () (throw 'nowhere 1)))",
      "(contained (no-catch nowhere 1))");
  CHECK(contained_kind == FeCompletionError);

  // The outer run is unharmed by either: a catch established *outside* the
  // protected call is neither reached by the contained throw nor disturbed
  // by it, and evaluation continues normally afterwards.
  CHK("(catch 'tg (contain-call (lambda () (throw 'tg 'escaped))) 'intact)",
      "intact");
  CHK("(+ 1 2)", "3");
  CHECK(FeGetCompletion(context) == FeCompletionNormal);

  // The host's own cleanups are not drained by a contained completion: only
  // the callee's are.
  CHK("(setq outer-cleanup-ran nil)", "nil");
  CHK("(unwind-protect (contain-call (lambda () (car 5))) "
      "  (setq outer-cleanup-ran t))",
      "(contained (wrong-type-argument listp 5))");
  CHK("outer-cleanup-ran", "t");

  // A cleanup *inside* the protected call does run, as part of containing
  // it, and does not escape past the barrier.
  CHK("(setq inner-cleanup-ran nil)", "nil");
  CHK("(contain-call (lambda () (unwind-protect (car 5) "
      "  (setq inner-cleanup-ran t))))",
      "(contained (wrong-type-argument listp 5))");
  CHK("inner-cleanup-ran", "t");

  // Nothing above leaked a GC-stack entry.
  CHECK(FeSaveGC(context) == gc);

  // Re-signal: the wrapper does its own work and puts the completion back in
  // flight, condition object intact, so the enclosing `condition-case`
  // matches on the *original* condition symbol rather than on a re-worded
  // `error`.
  wrap_cleanup_ran = false;
  CHK("(condition-case e (wrap-call (lambda () (car 5))) "
      "  (wrong-type-argument (list 'caught e)))",
      "(caught (wrong-type-argument listp 5))");
  CHECK(wrap_cleanup_ran);

  // A re-signalled completion that nothing catches reaches the host once,
  // with the message it started with.
  static const char uncaught[] = "(wrap-call (lambda () (car 5)))";
  CHECK(ExpectEvaluationError(context, &state, "protect.fe", uncaught,
                              sizeof(uncaught) - 1,
                              "protect.fe:1: expected pair, got integer"));

#undef CHK

  FeCloseContext(context);
  return true;
}

// A real host interrupt is a `quit` a Lisp program can catch, exactly as
// Emacs' C-g is: `(quit ...)` and `t` handlers see it, an `(error ...)`
// handler does not, and the object bound is the same `(quit)` cons
// `(signal 'quit nil)` builds. The interrupt path has no signalled
// condition object to walk, so the matcher has to decide on the completion
// *kind* before it looks at one -- testing the object first made a genuine
// C-g catchable by `t` alone.
static bool TestQuitIsCatchable(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  InterruptState interrupt = {
      .context = context, .expected_userdata = &interrupt, .cancel_after = 3};
  const FeEvalOptions options = {
      .poll_interval = 2, .interrupt = Interrupt, .userdata = &interrupt};

#define QUIT_CASE(expr, expected)                                             \
  do {                                                                        \
    interrupt.polls = 0;                                                      \
    static const char source[] = expr;                                        \
    CHECK(                                                                    \
        IsRendered(context,                                                   \
                   FeEvaluateStringWithOptions(context, "quit.fe", source,    \
                                               sizeof(source) - 1, &options), \
                   expected));                                                \
  } while (false)

  // A `(quit ...)` handler catches it, and so does `t`.
  QUIT_CASE("(condition-case nil (while t 1) (quit 'caught))", "caught");
  QUIT_CASE("(condition-case nil (while t 1) (t 'caught-t))", "caught-t");
  // The variable is bound to the `(quit)` condition object.
  QUIT_CASE("(condition-case v (while t 1) (quit v))", "(quit)");
  // A handler list containing `quit` matches too.
  QUIT_CASE("(condition-case nil (while t 1) ((arith-error quit) 'listed))",
            "listed");

#undef QUIT_CASE

  // An `(error ...)` handler does not: quit is not under `error`, so the
  // completion passes the handler by and reaches the host as Quit.
  interrupt.polls = 0;
  static const char uncaught[] =
      "(condition-case nil (while t 1) (error 'not-this-one))";
  CHECK(ExpectCompletionKind(
      context, &state, "quit.fe", uncaught, sizeof(uncaught) - 1, &options,
      "quit.fe:1: evaluation cancelled", FeCompletionQuit));

  // Budget is catchable by nothing at all, `t` included: it is fe's own
  // ceiling, not an Emacs condition, and a program must not be able to sit
  // inside the limit the host set.
  static const char budget[] = "(condition-case nil (while t 1) (t 'nope))";
  const FeEvalOptions step = {.step_limit = 64};
  CHECK(ExpectCompletionKind(
      context, &state, "quit.fe", budget, sizeof(budget) - 1, &step,
      "quit.fe:1: evaluation step limit exceeded", FeCompletionBudget));

  FeCloseContext(context);
  return true;
}

// A caught condition *resumes* the interrupted program, so everything the
// host configured for it has to survive the catch: the step budget (with the
// steps already spent still spent), the frame wall, the native-reentry
// ceiling, the C-g interrupt, and the source label errors are prefixed with.
// Clearing the control record on the way into the handler search -- which is
// what this file used to do -- disarmed all five permanently, so the first
// caught condition in a run silently removed every bound the host had asked
// for and the next runaway loop hung forever.
static bool TestConditionCaseResumesControl(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // The step budget: a caught condition, then a runaway loop in the same
  // run, which must still hit the limit rather than spin forever.
  static const char caught_then_loop[] =
      "(condition-case nil (car 5) (error nil)) (while t 1)";
  const FeEvalOptions generous = {.step_limit = 100000};
  CHECK(ExpectCompletionKind(context, &state, "control.fe", caught_then_loop,
                             sizeof(caught_then_loop) - 1, &generous,
                             "control.fe:1: evaluation step limit exceeded",
                             FeCompletionBudget));

  // The steps the body already spent stay spent: the same program under a
  // budget that the `condition-case` alone very nearly exhausts still runs
  // out, rather than starting again from a full budget after the catch.
  static const char caught_then_small[] =
      "(condition-case nil (car 5) (error nil)) (while t 1)";
  const FeEvalOptions small = {.step_limit = 40};
  CHECK(ExpectCompletionKind(context, &state, "control.fe", caught_then_small,
                             sizeof(caught_then_small) - 1, &small,
                             "control.fe:1: evaluation step limit exceeded",
                             FeCompletionBudget));

  // The interrupt: a caught condition must not disarm the host's C-g.
  static const char caught_then_spin[] =
      "(condition-case nil (car 5) (error nil)) (while t 1)";
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3};
  const FeEvalOptions interrupt_options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  CHECK(ExpectCompletionKind(context, &state, "control.fe", caught_then_spin,
                             sizeof(caught_then_spin) - 1, &interrupt_options,
                             "control.fe:1: evaluation cancelled",
                             FeCompletionQuit));
  CHECK(interrupt.polls == interrupt.cancel_after);

  // The frame wall: a caught condition must not remove `max_frames` either.
  static const char caught_then_deep[] =
      "(condition-case nil (car 5) (error nil)) "
      "(fset 'recurse (fn (x) (recurse x))) (recurse 1)";
  const FeEvalOptions tight_frames = {.max_frames = 32};
  CHECK(ExpectCompletionKind(context, &state, "control.fe", caught_then_deep,
                             sizeof(caught_then_deep) - 1, &tight_frames,
                             "control.fe:1: evaluation frame limit exceeded",
                             FeCompletionBudget));

  // The source label: every error raised after a caught condition still
  // carries the `label:` prefix that names where it came from.
  static const char caught_then_error[] =
      "(condition-case nil (car 5) (error nil)) (car 5)";
  CHECK(ExpectEvaluationError(context, &state, "control.fe", caught_then_error,
                              sizeof(caught_then_error) - 1,
                              "control.fe:1: expected pair, got integer"));

  // And the same is true one level down: an error raised inside a handler
  // body, after the catch, keeps the label too.
  static const char error_in_handler[] =
      "(condition-case nil (car 5) (error (car 5)))";
  CHECK(ExpectEvaluationError(context, &state, "control.fe", error_in_handler,
                              sizeof(error_in_handler) - 1,
                              "control.fe:1: expected pair, got integer"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 06C: `catch`/`throw`, the first non-local exit that stops partway
// down the frame stack. Every CT row from the sub-plan through the reader:
// value delivery (CT1), innermost same-tag wins (CT2), an uncaught throw as
// the `no-catch TAG VALUE` message through the ordinary error path (CT3),
// `nil` never matching as a tag (CT4), `eq` tag comparison across
// fixnum/float/string/shared-cons/fresh-cons (CT5), and `unwind-protect`
// cleanups running on the throw path in innermost-first order (CT7/U3).
// Plus the native re-entry boundary wall (a throw from a nested run does not
// honour a catch below its base, recorded as a divergence), `(catch 'a)`'s
// empty body being nil, the zero-operand and wrong-type degenerates of both
// forms, a throw under a tight `max_frames` (the unwind allocates no frames),
// a cleanup's own throw contained by the cleanup isolation barrier, context
// reuse after no-catch, and forced GC across a throw with a freshly
// allocated value.
static bool TestCatchThrow(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                   \
  CHECK(IsRendered(                                                           \
      context, FeEvaluateString(context, "catch.fe", expr, sizeof(expr) - 1), \
      expected))

  // CT1: a throw delivers its value to the matching catch, which returns it;
  // the trailing form is never evaluated.
  CHK("(catch 'tag (throw 'tag 7) 99)", "7");

  // CT2: the innermost same-tag catch wins.
  CHK("(catch 'a (catch 'a (throw 'a 1)) 2)", "2");

  // CT5 fixnum: an equal fixnum matches (Phase 5's integer `eq` is
  // load-bearing here).
  CHK("(catch 5 (throw 5 'hit) 'miss)", "hit");

  // CT5 floats, strings and fresh conses: eq is identity, not content, so
  // each throw propagates as no-catch.
  CHECK(ExpectEvaluationError(context, &state, "catch.fe",
                              "(catch 1.5 (throw 1.5 'hit) 'miss)",
                              sizeof("(catch 1.5 (throw 1.5 'hit) 'miss)") - 1,
                              "catch.fe:1: no-catch 1.5 hit"));
  CHECK(ExpectEvaluationError(
      context, &state, "catch.fe", "(catch \"s\" (throw \"s\" 'hit) 'miss)",
      sizeof("(catch \"s\" (throw \"s\" 'hit) 'miss)") - 1,
      "catch.fe:1: no-catch s hit"));
  CHECK(ExpectEvaluationError(context, &state, "catch.fe",
                              "(catch (list 1) (throw (list 1) 'hit) 'miss)",
                              sizeof("(catch (list 1) (throw (list 1) 'hit) "
                                     "'miss)") -
                                  1,
                              "catch.fe:1: no-catch (1) hit"));

  // CT5 shared cons: the same object as the tag on both sides matches.
  CHK("(setq shared-tag (list 1))", "(1)");
  CHK("(catch shared-tag (throw shared-tag 'hit) 'miss)", "hit");

  // CT3: an uncaught throw raises `no-catch TAG VALUE` through the ordinary
  // error path.
  CHECK(ExpectEvaluationError(context, &state, "catch.fe", "(throw 'nowhere 1)",
                              sizeof("(throw 'nowhere 1)") - 1,
                              "catch.fe:1: no-catch nowhere 1"));

  // CT4: nil never matches as a catch tag.
  CHECK(ExpectEvaluationError(
      context, &state, "catch.fe", "(catch nil (throw nil 5))",
      sizeof("(catch nil (throw nil 5))") - 1, "catch.fe:1: no-catch nil 5"));

  // Distinct tags select the level: a throw from the inner body to the outer
  // tag skips the inner catch entirely. Pinned by the compat cond-ct2 shape
  // and the distinct-tag nesting above.
  CHK("(catch 'outer (catch 'inner (throw 'outer 5) 1) 2)", "5");

  // A throw from inside a catch's *tag* evaluation reaches an outer catch and
  // discards the inner (still-awaiting-tag) catch frame, which must never
  // misinterpret the delivered value as its own tag.
  CHK("(catch 'a (catch (throw 'a 1) 'body))", "1");

  // `(catch 'a)` with an empty body is nil, nothing more.
  CHK("(catch 'a)", "nil");

  // CT7/U3: `unwind-protect` cleanups run on the throw path, innermost
  // first, and the catch still returns the thrown value.
  CHK("(setq log '())", "nil");
  CHK("(catch 'tg (unwind-protect (unwind-protect (throw 'tg 'done) "
      "(setq log (cons 'inner log))) (setq log (cons 'outer log))))",
      "done");
  CHK("log", "(outer inner)");

  // A cleanup's throw with no matching catch anywhere: it is re-issued in
  // each enclosing context in turn, finds nothing in any of them, and
  // becomes `no-catch` -- which then replaces the error that was unwinding,
  // per 06A Decision 4. Measured Emacs: `(unwind-protect (error "orig")
  // (throw 'nope 1))` is `(no-catch nope 1)`. The outer cleanup still runs.
  CHK("(setq outer-ran nil)", "nil");
  static const char cleanup_throw[] =
      "(unwind-protect"
      "  (unwind-protect"
      "    (car 1)"
      "    (throw 'escape 1))"
      "  (setq outer-ran t))";
  EvalCall cleanup_throw_call = {.context = context,
                                 .state = &state,
                                 .label = "catch.fe",
                                 .source = cleanup_throw,
                                 .length = sizeof(cleanup_throw) - 1,
                                 .options = nullptr,
                                 .expected = "catch.fe:1: no-catch escape 1"};
  char captured[512];
  CHECK(CaptureStderr(RunEvalCall, &cleanup_throw_call, captured,
                      sizeof(captured)));
  CHECK(cleanup_throw_call.result);
  CHECK(captured[0] == '\0');
  CHK("outer-ran", "t");

  // A cleanup's throw whose catch *does* exist below the drain: Emacs' rule
  // (06A Decision 4, measured against 31.0.90) is that it wins over
  // whatever completion was already unwinding, whether that was another
  // throw, an error, or nothing at all. The catch frames live in the run
  // being unwound, below the cleanup run's own floor, so the throw has to
  // be re-issued in the enclosing context rather than answered where it was
  // raised.
  CHK("(catch 'tg (unwind-protect (throw 'tg 'a) (throw 'tg 'b)))", "b");
  CHK("(catch 'tg (unwind-protect (error \"orig\") (throw 'tg 'b)))", "b");
  CHK("(catch 'tg (unwind-protect 1 (throw 'tg 'b)))", "b");
  // A different tag selects a different level, and the in-flight throw to
  // the inner tag is abandoned.
  CHK("(catch 'o (catch 'i (unwind-protect (throw 'i 1) (throw 'o 2))))", "2");
  // Frames between the cleanup and its catch that are themselves being
  // abandoned are still live while the cleanup runs, exactly as they are in
  // Emacs: the inner catch is still established, so it takes the throw and
  // the outer one never sees the original.
  CHK("(catch 'o (unwind-protect (catch 'i (unwind-protect (throw 'o 1) "
      "(throw 'i 2))) 'x))",
      "2");
  // An enclosing `condition-case` around all of it sees the value, not the
  // error the cleanup replaced.
  CHK("(condition-case e (catch 'tg (unwind-protect (error \"orig\") "
      "(throw 'tg 'b))) (error (list 'err e)))",
      "b");
  // The cleanups between the throw and its catch still run, innermost
  // first, on the replacing throw's own path.
  CHK("(setq log '())", "nil");
  CHK("(catch 'tg (unwind-protect (unwind-protect (throw 'tg 'a) "
      "(throw 'tg 'b)) (setq log (cons 'outer log))))",
      "b");
  CHK("log", "(outer)");

  // Forced GC across a throw: a freshly allocated value thrown out of a body
  // that collects heavily must survive the unwind and arrive intact.
  CHK("(catch 'tg (do (setq gc-i 0) (while (< gc-i 2000) (cons gc-i gc-i) "
      "(setq gc-i (+ gc-i 1))) (throw 'tg (cons 111 222))))",
      "(111 . 222)");

  // The native re-entry boundary wall (a recorded divergence): a native that
  // synchronously starts a nested run which throws does not honour a catch
  // below that run's base -- the C activations between them are live and
  // cannot be popped by frame-index assignment, so the throw raises no-catch
  // in the nested run.
  FeDefineNative(context, "reenter-throw", ReenterThrow);
  static const char boundary[] = "(catch 'outer-tag (reenter-throw))";
  CHECK(ExpectEvaluationError(context, &state, "outer.fe", boundary,
                              sizeof(boundary) - 1,
                              "nested.fe:1: no-catch outer-tag 42"));

  // Positive control for the wall: a catch entirely inside the nested run is
  // honoured, so re-entry only bounds the throw search, it does not disable
  // catch/throw.
  FeDefineNative(context, "reenter-catch-throw", ReenterCatchThrow);
  static const char nested_ok[] = "(reenter-catch-throw)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "outer.fe", nested_ok, sizeof(nested_ok) - 1),
      "1"));

  // A throw under a tight `max_frames`: the unwind itself allocates no
  // frames, so the exact measured peak of the same expression still
  // succeeds, and one fewer frame refuses it before anything unwinds. On a
  // fresh context, so `peak_frame_depth` (a cumulative high-water mark) is
  // the expression's own peak rather than every run above this one's.
  static TestArena frame_arena;
  FeContext* frame_context =
      FeOpenContext(frame_arena.bytes, sizeof(frame_arena.bytes));
  CHECK(frame_context != nullptr);
  ErrorState frame_state = {.context = frame_context};
  FeSetUserData(frame_context, &frame_state);
  FeSetErrorFn(frame_context, HandleError);
  static const char throw_fixed[] = "(catch 'tg (throw 'tg 42))";
  CHECK(IsRendered(frame_context,
                   FeEvaluateString(frame_context, "measure.fe", throw_fixed,
                                    sizeof(throw_fixed) - 1),
                   "42"));
  const size_t peak = FeGetArenaStats(frame_context).peak_frame_depth;
  const FeEvalOptions exact = {.max_frames = peak};
  CHECK(IsRendered(
      frame_context,
      FeEvaluateStringWithOptions(frame_context, "exact.fe", throw_fixed,
                                  sizeof(throw_fixed) - 1, &exact),
      "42"));
  const FeEvalOptions one_less = {.max_frames = peak - 1};
  CHECK(ExpectEvaluationOptionsError(
      frame_context, &frame_state, "toosmall.fe", throw_fixed,
      sizeof(throw_fixed) - 1, &one_less,
      "toosmall.fe:1: evaluation frame limit exceeded"));
  FeCloseContext(frame_context);

  // The zero-operand and wrong-count degenerates (the Phase 4 lesson): both
  // forms are arity-checked before anything evaluates.
  CHECK(ExpectEvaluationError(context, &state, "catch.fe", "(catch)",
                              sizeof("(catch)") - 1,
                              "catch.fe:1: wrong-number-of-arguments"));
  CHECK(ExpectEvaluationError(context, &state, "catch.fe", "(throw)",
                              sizeof("(throw)") - 1,
                              "catch.fe:1: wrong-number-of-arguments"));
  CHECK(ExpectEvaluationError(context, &state, "catch.fe", "(throw 'x)",
                              sizeof("(throw 'x)") - 1,
                              "catch.fe:1: wrong-number-of-arguments"));
  CHECK(ExpectEvaluationError(context, &state, "catch.fe", "(throw 'x 1 2)",
                              sizeof("(throw 'x 1 2)") - 1,
                              "catch.fe:1: wrong-number-of-arguments"));

  // Context reuse after no-catch: an uncaught throw leaves nothing poisoned,
  // and the completion kind reads Normal again after a normal return.
  CHECK(IsRendered(context, FeEvaluateString(context, "recovered.fe", "6", 1),
                   "6"));
  CHECK(FeGetCompletion(context) == FeCompletionNormal);

#undef CHK

  FeCloseContext(context);
  return true;
}

// Sub-plan 03F: `FeEvalOptions.max_frames` bounds Lisp nesting -- the number
// of simultaneously live ordinary evaluator frames -- replacing the deleted
// transitional `evaluation_depth` counter this test used to exercise. Every
// property `TestEvaluationDepth` used to cover for the old shared counter
// still applies to the frame-specific one: the limit fires with the exact
// "evaluation frame limit exceeded" text, a later legal call still works
// (nothing here is a sticky poison), and both a Lisp `unwind-protect`
// cleanup and a host `FeProtectWithCleanup` cleanup still run during the
// unwind.
//
// The boundary case does not hand-pick a frame count: `AllocateFrame`'s
// exact push-before-write rule (`min(max_frames, frame_capacity)`, checked
// *before* the slot is written) means the same fixed expression, run once
// unrestricted, tells the test its own true peak through
// `FeGetArenaStats().peak_frame_depth` -- so "the last permitted push
// succeeds, the next fails" is asserted against a measured number, not one
// that silently drifts out of date if an internal frame's shape changes.
static bool TestFrameLimits(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // A nested, fixed-shape computation (no recursion, so its frame cost is
  // deterministic) measures its own simultaneous frame peak.
  static const char fixed[] = "(+ 1 (+ 2 (+ 3 (+ 4 5))))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "measure.fe", fixed, sizeof(fixed) - 1), "15"));
  const size_t peak = FeGetArenaStats(context).peak_frame_depth;
  CHECK(peak > 0);

  // Exactly at the measured peak: the last permitted push succeeds.
  const FeEvalOptions exact = {.max_frames = peak};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "exact.fe", fixed,
                                               sizeof(fixed) - 1, &exact),
                   "15"));

  // One below: the exact same expression's push that would reach the same
  // peak is refused before writing, with the exact frame-limit text.
  const FeEvalOptions one_less = {.max_frames = peak - 1};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "toosmall.fe", fixed, sizeof(fixed) - 1, &one_less,
      "toosmall.fe:1: evaluation frame limit exceeded"));

  // A separate, deliberately tiny `max_frames` against unbounded
  // self-recursion -- not tied to the measured peak above, since unbounded
  // recursion overflows any small ceiling regardless of its exact value --
  // proves both a registered Lisp cleanup (`unwind-protect`) and a
  // registered native cleanup (`FeProtectWithCleanup`, through
  // `ReentrantNative`'s own `MarkReentryCleanup`) run during the unwind, and
  // that the context is reusable afterward.
  FeObject* native = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, native);
  FeSetFunction(context, FeMakeSymbol(context, "reentrant-native"), native);
  state.reentry_remaining = 0;
  state.reentry_cleanup_ran = false;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  static const char reset_flag[] = "(setq cleanup-ran nil)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "reset.fe", reset_flag, sizeof(reset_flag) - 1),
      "nil"));
  static const char overflow_with_cleanup[] =
      "(fset 'loop (fn (x) (loop x))) "
      "(unwind-protect (do (reentrant-native) (loop 1))"
      "  (setq cleanup-ran t))";
  const FeEvalOptions tight_frames = {.max_frames = 6};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "cleanup-frames.fe", overflow_with_cleanup,
      sizeof(overflow_with_cleanup) - 1, &tight_frames,
      "cleanup-frames.fe:1: evaluation frame limit exceeded"));
  CHECK(state.reentry_cleanup_ran);
  static const char check_cleanup_ran[] = "cleanup-ran";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "check.fe", check_cleanup_ran,
                                    sizeof(check_cleanup_ran) - 1),
                   "t"));

  // A macro whose expansion is another macro call recurses through the
  // macro frame's own resumption, not a fresh top-level call, so its frame
  // cost has to be covered too: released early, this recursed on the C
  // stack instead of the frame stack and crashed under MSan.
  static const char macro_recursion[] =
      "(fset 'm (macro () (list (quote m)))) (m)";
  const FeEvalOptions tight_macro_frames = {.max_frames = 5};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "macro-frames.fe", macro_recursion,
      sizeof(macro_recursion) - 1, &tight_macro_frames,
      "macro-frames.fe:1: evaluation frame limit exceeded"));

  // The context is reusable after both overflow paths.
  static const char deep[] =
      "(fset 'deep (lambda (n) (if (<= n 0) 0 (+ 1 (deep (- n 1)))))) (deep "
      "40)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "recovered.fe", deep, sizeof(deep) - 1), "40"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 03C's frame substrate keeps the old evaluator's observable
// language behaviour while making live frames explicit GC roots. These cases
// exercise the converted leaves, a collection through a temporary frame, and
// the arena-derived physical frame bound independently of max_frames.
static bool TestFrameSubstrate(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // quote takes exactly one raw argument and rejects extras before evaluation.
  CHECK(ExpectEvaluationError(context, &state, "quote.fe", "(quote 1 2)",
                              sizeof("(quote 1 2)") - 1,
                              "quote.fe:1: wrong-number-of-arguments"));
  // Since 04D's cut, `quote` is a function-cell resident like every other
  // primitive: rebinding its *value* cell with `setq` no longer affects call
  // position, so `(quote (+ 1 2))` still special-forms and returns the form
  // itself, unchanged -- the Lisp-2 rule that a lexical value binding never
  // shadows a callable (the pre-cut one-namespace behavior made the value
  // shadow the callable, which is exactly what the cut deleted).
  static const char quote_rebound[] = "(setq quote (fn (x) x)) (quote (+ 1 2))";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "quote.fe", quote_rebound,
                                    sizeof(quote_rebound) - 1),
                   "(+ 1 2)"));

  // The outer `do` frame is the only reference to its pending forms while
  // the loop repeatedly allocates and collects. A resumed final lookup proves
  // the temporary-dispatch frame was marked, rather than merely surviving by
  // accident on the GC stack.
  static TestArena gc_arena;
  const size_t gc_size = FeMinimumArenaSize() + 8 * 1024;
  FeContext* gc_context = FeOpenContext(gc_arena.bytes, gc_size);
  CHECK(gc_context != nullptr);
  ErrorState gc_state = {.context = gc_context};
  FeSetUserData(gc_context, &gc_state);
  FeSetErrorFn(gc_context, HandleError);
  const size_t collections = FeGetArenaStats(gc_context).collection_count;
  static const char collecting[] =
      "(do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) n)";
  CHECK(IsRendered(gc_context,
                   FeEvaluateString(gc_context, "frame-gc.fe", collecting,
                                    sizeof(collecting) - 1),
                   "2000"));
  CHECK(FeGetArenaStats(gc_context).collection_count > collections);
  FeCloseContext(gc_context);

  // `max_frames == 0` selects the arena's full physical capacity -- the
  // default -- so this is the frame-push wall itself, not a host-configured
  // ceiling: default physical exhaustion, repeated (03F requires it). It
  // fires *before* object-allocation failure -- `allocation_failures` is
  // still 0 once the context is queryable again, confirming 03C's
  // requirement that the frame bound, not arena exhaustion, is what a deep
  // recursion hits first -- and the same context remains usable afterward.
  static TestArena frame_arena;
  FeContext* frame_context = FeOpenContext(frame_arena.bytes, gc_size);
  CHECK(frame_context != nullptr);
  ErrorState frame_state = {.context = frame_context};
  FeSetUserData(frame_context, &frame_state);
  FeSetErrorFn(frame_context, HandleError);
  static const char recurse[] =
      "(fset 'recurse (fn (x) (if (<= x 0) 0 (recurse (- x 1))))) "
      "(recurse 100)";
  const FeEvalOptions physical_only = {.max_frames = 0};
  CHECK(ExpectEvaluationOptionsError(
      frame_context, &frame_state, "frames.fe", recurse, sizeof(recurse) - 1,
      &physical_only, "frames.fe:1: evaluation frame limit exceeded"));
  CHECK(!frame_state.stack_was_nil);
  CHECK(FeGetArenaStats(frame_context).allocation_failures == 0);
  CHECK(IsRendered(frame_context,
                   FeEvaluateString(frame_context, "recovered.fe", "(+ 1 2)",
                                    sizeof("(+ 1 2)") - 1),
                   "3"));
  FeCloseContext(frame_context);

  FeCloseContext(context);
  return true;
}

// `FeGetArenaStats` (sub-plan 00D of kg's Emacs-subset program, fields
// renamed and added by 03F): a read-only accessor over counters
// `MakeObject`, `CollectGarbage`, `FePushGC`, `AllocateFrame`,
// `EnterNativeReentry` and `PushCleanup` already maintain. This exercises
// that querying it neither allocates nor mutates state, that
// `frame_capacity` is nonzero and stable, that each peak moves off zero the
// first time its own event happens (`peak_native_reentry`'s own convention
// is zero-at-top-level: no native re-entry here means it never moves), that
// `peak_frame_depth` never exceeds `frame_capacity` for an ordinary
// computation that never has to reach for the private cleanup reserve, and
// that arena exhaustion is directly observable through it.
static bool TestArenaStats(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // `FeOpenContext` already registered `t`, every primitive and the math
  // natives through the same `MakeObject` any other allocation uses, so
  // the baseline is not zero. `frame_capacity` is fixed at context-open
  // time by the arena partition, so it is already nonzero here too.
  const FeArenaStats initial = FeGetArenaStats(context);
  CHECK(initial.total_slots > 0);
  CHECK(initial.free_slots > 0);
  CHECK(initial.free_slots < initial.total_slots);
  CHECK(initial.peak_live_objects > 0);
  CHECK(initial.allocation_failures == 0);
  CHECK(initial.frame_capacity > 0);
  CHECK(initial.peak_native_reentry == 0);

  // Querying twice with nothing evaluated in between changes nothing: the
  // accessor allocates no Fe object, walks no list, and mutates no
  // counter.
  const FeArenaStats requeried = FeGetArenaStats(context);
  CHECK(requeried.total_slots == initial.total_slots);
  CHECK(requeried.free_slots == initial.free_slots);
  CHECK(requeried.peak_live_objects == initial.peak_live_objects);
  CHECK(requeried.collection_count == initial.collection_count);
  CHECK(requeried.peak_gc_stack_depth == initial.peak_gc_stack_depth);
  CHECK(requeried.frame_capacity == initial.frame_capacity);
  CHECK(requeried.peak_frame_depth == initial.peak_frame_depth);
  CHECK(requeried.peak_cleanup_stack_depth == initial.peak_cleanup_stack_depth);
  CHECK(requeried.peak_native_reentry == initial.peak_native_reentry);

  // Every allocation pushes onto the GC stack (`FePushGC`), and every pushed
  // evaluator frame runs through `AllocateFrame`, so evaluating anything
  // moves both peaks off zero -- and the frame peak stays within the
  // context's own physical capacity, since nothing here is near it.
  static const char one_cons[] = "(cons 1 2)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "cons.fe", one_cons, sizeof(one_cons) - 1),
      "(1 . 2)"));
  const FeArenaStats after_cons = FeGetArenaStats(context);
  CHECK(after_cons.peak_gc_stack_depth > 0);
  CHECK(after_cons.peak_frame_depth > 0);
  CHECK(after_cons.peak_frame_depth <= after_cons.frame_capacity);
  CHECK(after_cons.peak_live_objects >= initial.peak_live_objects);
  CHECK(after_cons.total_slots == initial.total_slots);

  // `unwind-protect` registers a cleanup, moving the cleanup-stack peak
  // off zero the same way.
  static const char cleanup[] = "(unwind-protect 1 2)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "cleanup.fe", cleanup, sizeof(cleanup) - 1),
      "1"));
  CHECK(FeGetArenaStats(context).peak_cleanup_stack_depth > 0);

  // A native that re-opens evaluation through `FeCallWithOptions` moves
  // `peak_native_reentry` off zero -- the one peak nothing above touched --
  // while an ordinary top-level call of the same native does not.
  FeObject* ordinary = FeMakeNativeFn(context, OrdinaryNative);
  FeSetFunction(context, FeMakeSymbol(context, "ordinary-native"), ordinary);
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "ordinary.fe", "(ordinary-native)",
                                    sizeof("(ordinary-native)") - 1),
                   "42.0"));
  CHECK(FeGetArenaStats(context).peak_native_reentry == 0);
  FeObject* reentrant = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, reentrant);
  FeSetFunction(context, FeMakeSymbol(context, "reentrant-native"), reentrant);
  state.reentry_remaining = 2;
  CHECK(
      IsRendered(context,
                 FeEvaluateString(context, "reentrant.fe", "(reentrant-native)",
                                  sizeof("(reentrant-native)") - 1),
                 "3.0"));
  CHECK(FeGetArenaStats(context).peak_native_reentry == 2);

  FeCloseContext(context);

  // A deliberately exact-fit arena -- no slots spare once the core
  // primitives are registered -- turns the very first user allocation
  // into an out-of-memory failure, so `allocation_failures` and
  // `collection_count` (MakeObject always tries a collection before
  // giving up, even with nothing collectible) are both directly
  // observable, and the context is confirmed still queryable afterward.
  static TestArena tight_storage;
  const size_t tight_size = FeMinimumArenaSize();
  CHECK(tight_size <= sizeof(tight_storage.bytes));
  FeContext* tight = FeOpenContext(tight_storage.bytes, tight_size);
  CHECK(tight != nullptr);
  // `HandleError` unconditionally compares against `expected_message`, so
  // this needs a non-null placeholder even though this test does not
  // check `tight_state.called`: what it looks for is that the jump was
  // taken and the counters moved, not the exact message text.
  ErrorState tight_state = {.context = tight,
                            .expected_message = "oom.fe:1: out of memory"};
  FeSetUserData(tight, &tight_state);
  FeSetErrorFn(tight, HandleError);
  CHECK(FeGetArenaStats(tight).free_slots == 0);
  CHECK(FeGetArenaStats(tight).allocation_failures == 0);

  static const char over_budget[] = "(cons 1 2)";
  if (setjmp(tight_state.jump) == 0) {
    (void)FeEvaluateString(tight, "oom.fe", over_budget,
                           sizeof(over_budget) - 1);
    CHECK(false);
  }
  const FeArenaStats after_oom = FeGetArenaStats(tight);
  CHECK(after_oom.allocation_failures == 1);
  CHECK(after_oom.collection_count >= 1);
  FeCloseContext(tight);

  return true;
}

// The C-stack high-water probe (sub-plan 03A of kg's Emacs-subset program),
// turned into the permanent flatness gate by 03F, as the parent plan's own
// document requires: "a measured C-stack high-water mark is flat across
// `(deep 10)`, `(deep 1000)` and `(deep 100000)`". Before 03E's frame
// machine landed in full, nothing could even take this measurement -- only
// crash points (GC-stack overflow, an MSan crash) showed where the
// recursive evaluator stopped, not whether a number stopped growing -- and
// `(deep 100000)` could not run at all under the old shared logical/native
// depth ceiling. Both blockers are gone: the frame machine roots Lisp
// nesting in the arena, not the C stack, and `max_frames` has nothing to do
// with `max_native_reentry`'s small C-stack budget any more, so this probe
// can ask for the real 10/1000/100000 triple directly.
//
// A tight, deliberately small first run measures the "depth zero" baseline
// and confirms `max_frames`' own boundary error and recovery (the same
// shape `TestFrameLimits` already proves in general); the second part is
// the actual gate, run against a dynamically allocated arena sized for
// `(deep 100000)`'s own measured frame cost -- `./fe -s` cannot run the
// test-only `stack-probe` native and kg's 1 MiB arena cannot hold 100000
// frames and is not expected to, so this is deliberately an fe-side-only
// measurement (03A/03F's Decision in kg's plan tree).
static bool TestEvaluationStackProbe(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "stack-probe", StackProbe);

  static const char deep_def[] =
      "(fset 'deep (fn (n) (if (<= n 0) (stack-probe) (+ 1 (deep (- n "
      "1))))))";
  CHECK(FeEvaluateString(context, "deep-def.fe", deep_def,
                         sizeof(deep_def) - 1) != nullptr);

  // The expected frame-limit error, recovered without poisoning the
  // context -- the same shape `TestFrameLimits` already proves for the
  // general case.
  const FeEvalOptions tight_frames = {.max_frames = 5};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "probe-frames.fe", "(deep 50)", sizeof("(deep 50)") - 1,
      &tight_frames, "probe-frames.fe:1: evaluation frame limit exceeded"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(deep 20)",
                                    sizeof("(deep 20)") - 1),
                   "20.0"));
  FeCloseContext(context);

  // The permanent gate: a dynamically sized arena, large enough to hold
  // `(deep 100000)`'s own frame peak. `test_api.c` only ever includes the
  // public `fe.h`, not the private `fe_internal.h` where `FeEvalFrame` and
  // `FrameArenaPercent` actually live, so the byte estimate below is a
  // generous, hand-computed upper bound, not a value derived from the
  // private struct's real size: this canonical chain's peak is ~3 frames
  // per level (the call, `if`, and the arithmetic waiting on its second
  // operand -- kg's 03A Decision derives and cross-validates this), so
  // 100000 levels need on the order of 300000 frames; at a generous 128
  // bytes/frame (the private struct measures smaller) that is ~38 MB of
  // frame region, which is ~10% of the arena beyond the minimum -- so
  // comfortably under 400 MB total covers it with room to spare for the
  // object slots the run's own arithmetic needs too.
  const size_t max_deep = 100000;
  const size_t generous_bytes_per_frame = 128;
  const size_t frame_region_bytes =
      (3 * max_deep + 16) * generous_bytes_per_frame;
  const size_t big_arena_size =
      FeMinimumArenaSize() + frame_region_bytes * 10 + (16u * 1024 * 1024);
  unsigned char* big_arena = malloc(big_arena_size);
  CHECK(big_arena != nullptr);
  FeContext* big_context = FeOpenContext(big_arena, big_arena_size);
  CHECK(big_context != nullptr);
  ErrorState big_state = {.context = big_context};
  FeSetUserData(big_context, &big_state);
  FeSetErrorFn(big_context, HandleError);
  FeDefineNative(big_context, "stack-probe", StackProbe);
  CHECK(FeEvaluateString(big_context, "deep-def.fe", deep_def,
                         sizeof(deep_def) - 1) != nullptr);

  // "Depth zero", measured the same way as every other depth: the same
  // native, called through the same evaluator, with no `deep` wrapper
  // around it. This is the reference the deltas below are taken against,
  // not an automatic in this function -- an automatic here would mix this
  // test's own call-path layout into the number and make a compiler
  // rebuild look like evaluator growth.
  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  static const char bare_probe[] = "(stack-probe)";
  CHECK(IsRendered(big_context,
                   FeEvaluateString(big_context, "bare.fe", bare_probe,
                                    sizeof(bare_probe) - 1),
                   "0.0"));
  CHECK(stack_probe_last_address != 0);
  const uintptr_t baseline = stack_probe_deepest_address;
  const FeArenaStats before_deep = FeGetArenaStats(big_context);

  static const size_t depths[] = {10, 1000, 100000};
  for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
    char source[32];
    const int written =
        snprintf(source, sizeof(source), "(deep %zu)", depths[i]);
    CHECK(written > 0 && (size_t)written < sizeof(source));
    char label[32];
    (void)snprintf(label, sizeof(label), "deep-%zu.fe", depths[i]);
    char expected[16];
    // `deep`'s base case is the double `(stack-probe)`'s 0.0, so the whole
    // sum is a double and renders with its `.0` under 05D's printer.
    (void)snprintf(expected, sizeof(expected), "%zu.0", depths[i]);

    stack_probe_last_address = 0;
    stack_probe_deepest_address = 0;
    CHECK(IsRendered(
        big_context,
        FeEvaluateString(big_context, label, source, (size_t)written),
        expected));
    CHECK(stack_probe_last_address != 0);
    const uintptr_t probed = stack_probe_deepest_address;
    const uintptr_t delta =
        probed > baseline ? probed - baseline : baseline - probed;
    const FeArenaStats after = FeGetArenaStats(big_context);
    printf("stack probe: n=%6zu baseline=%#" PRIxPTR " probe=%#" PRIxPTR
           " delta=%" PRIuPTR " bytes, peak_frame_depth=%zu/%zu\n",
           depths[i], baseline, probed, delta, after.peak_frame_depth,
           after.frame_capacity);
    // Flat: the same < 2 KiB tolerance measured and asserted throughout
    // 03A/03D/03E for this exact property, under both default and
    // sanitizer builds -- not "one frame's slop", a specific measured
    // number that a regression sending Lisp nesting back through a
    // recursive C call would blow through by tens of kilobytes at these
    // depths, not by a few bytes of noise.
    CHECK(delta < 2048);
    CHECK(after.peak_frame_depth <= after.frame_capacity);
    CHECK(after.peak_frame_depth > before_deep.peak_frame_depth);
  }

  FeCloseContext(big_context);
  free(big_arena);
  return true;
}

// Sub-plan 03D, stage 1: computed call-head resolution runs on the frame
// stack. A computed head (a pair form in head position) used to recurse
// through `EvaluateHead` into a nested `Evaluate`; now it is a frame
// transition, so a pure computed-head chain must stop consuming C stack.
//
// `loop` is self-returning, so `(loop)` evaluates to `loop` again and the
// generated chain `(((((...((loop))...))))` is callable at every depth: each
// level's head is the previous level's call and resolves to the same
// function. Lambda application is still the temporary recursive dispatch in
// this stage, but the applications run one at a time as the frame stack
// unwinds, so the probe fires at the same C depth at every level. In the old
// code the nested head evaluations kept every level's frames open
// simultaneously, so the *deepest* probe address (the high-water mark) grew
// ~2 frames per level; this test asserts the high-water mark is flat, and
// that property depends on the call-head frame alone -- not on the argument,
// body, lambda, macro or native frames.
static bool TestCallHeadProbe(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "stack-probe", StackProbe);

  // `loop` is self-returning as a *value* (its body evaluates to the closure
  // again), while call position needs the function cell -- so since 04D's cut
  // it is defined in both namespaces: `setq` seeds the value cell the body
  // returns, and `fset` copies it into the function cell call position reads.
  static const char loop_def[] =
      "(setq loop (fn () (do (stack-probe) loop))) (fset 'loop loop)";
  CHECK(FeEvaluateString(context, "loop-def.fe", loop_def,
                         sizeof(loop_def) - 1) != nullptr);

  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  static const char baseline[] = "(loop)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "call-head.fe", baseline, sizeof(baseline) - 1),
      "(lambda nil (do (stack-probe) loop))"));
  CHECK(stack_probe_last_address != 0);
  CHECK(stack_probe_deepest_address != 0);
  const uintptr_t baseline_address = stack_probe_deepest_address;

  // 150 levels, ~151 simultaneously open computed-head frames; the old code
  // held ~300 head-resolution C activations open here. Well inside the 1 MiB
  // arena's frame capacity (1100) and the default logical depth ceiling
  // (peak ~153), so the chain succeeds and the flatness bound is the only
  // assertion at risk.
  enum { ChainDepth = 150, MaxFlatDelta = 2048 };
  char source[2 * ChainDepth + 8];
  size_t length = 0;
  for (int i = 0; i < ChainDepth + 1; i++) {
    source[length++] = '(';
  }
  source[length++] = 'l';
  source[length++] = 'o';
  source[length++] = 'o';
  source[length++] = 'p';
  for (int i = 0; i < ChainDepth + 1; i++) {
    source[length++] = ')';
  }

  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "call-head.fe", source, length),
                   "(lambda nil (do (stack-probe) loop))"));
  CHECK(stack_probe_last_address != 0);
  CHECK(stack_probe_deepest_address != 0);
  const uintptr_t deepest = stack_probe_deepest_address;
  const uintptr_t delta = baseline_address > deepest
                              ? baseline_address - deepest
                              : deepest - baseline_address;
  printf("call-head probe: depth=%d frames~%d baseline=%#" PRIxPTR
         " deepest=%#" PRIxPTR " delta=%" PRIuPTR " bytes\n",
         ChainDepth, ChainDepth + 1, baseline_address, deepest, delta);
  // One old-style head-resolution level cost ~2 C frames (~400 bytes in this
  // build; 150 levels measured ~62 KB of probe growth on the pre-frame-loop
  // code). A frame-loop regression puts the probe back on the C stack and
  // fails this far past the 2 KiB budget.
  CHECK(delta < MaxFlatDelta);

  // A computed head that raises still unwinds through the new frame kind,
  // and the context stays usable afterwards.
  static const char bad_chain[] = "((((((car-thing))))))";
  CHECK(ExpectEvaluationError(context, &state, "call-head.fe", bad_chain,
                              sizeof(bad_chain) - 1,
                              "call-head.fe:1: void-function car-thing"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(+ 1 2)",
                                    sizeof("(+ 1 2)") - 1),
                   "3"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 03D, stage 2: argument evaluation for ordinary callables (native
// functions and lambdas) is a resumable frame transition, not a recursive
// `EvaluateList`. `EvaluateList` charged exactly one evaluation step before
// each argument; the argument frame must charge the same, so the exact step
// budget for a call does not move, and a call with no arguments still runs
// without raising `too few arguments`. Macros are unaffected: they receive
// their arguments raw on the temporary dispatch and never evaluate them.
static bool TestArgumentFrame(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "add-exactly", AddExactly);

  // A native call costs one step for the pair form, one for the symbol
  // head, and one per argument: `(add-exactly 1 2)` is exactly 6 steps,
  // charged across two resumed argument frames.
  const FeEvalOptions native_tight = {.step_limit = 5};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "arg-native.fe", "(add-exactly 1 2)",
      sizeof("(add-exactly 1 2)") - 1, &native_tight,
      "arg-native.fe:1: evaluation step limit exceeded"));
  const FeEvalOptions native_ok = {.step_limit = 6};
  CHECK(IsRendered(
      context,
      FeEvaluateStringWithOptions(context, "arg-native.fe", "(add-exactly 1 2)",
                                  sizeof("(add-exactly 1 2)") - 1, &native_ok),
      "3.0"));

  // A lambda call adds its own steps on top: closure creation in the head
  // frame, one step per parameter in `ArgsToEnv`, the body's own
  // form/symbol steps, and `GetBound`'s charge for walking the body
  // environment. `((fn (x) x) 1)` is exactly 9 steps -- the same count the
  // recursive `EvaluateList` produced, with the argument step still charged
  // once, at the same point.
  const FeEvalOptions lambda_tight = {.step_limit = 8};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "arg-lambda.fe", "((fn (x) x) 1)",
      sizeof("((fn (x) x) 1)") - 1, &lambda_tight,
      "arg-lambda.fe:1: evaluation step limit exceeded"));
  const FeEvalOptions lambda_ok = {.step_limit = 9};
  CHECK(IsRendered(
      context,
      FeEvaluateStringWithOptions(context, "arg-lambda.fe", "((fn (x) x) 1)",
                                  sizeof("((fn (x) x) 1)") - 1, &lambda_ok),
      "1"));

  // Zero arguments: `EvaluateList` charged nothing for an empty list, and
  // the empty argument frame must skip `FeGetNextArgument` (which would
  // raise "too few arguments") and apply the callable to an empty list.
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "arg-zero.fe", "((fn () 5))",
                                    sizeof("((fn () 5))") - 1),
                   "5"));

  // Left-to-right evaluation with a side effect in an early argument: the
  // later argument must still run, and the values must land in order.
  static const char ordered[] =
      "((fn (a b) (list a b)) (setq mark 1) (setq mark 2))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "arg-order.fe", ordered, sizeof(ordered) - 1),
      "(1 2)"));
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "arg-mark.fe", "mark", sizeof("mark") - 1),
      "2"));

  // A dotted argument list raises the same `FeGetNextArgument` error, at the
  // same point in the walk -- after the earlier arguments were already
  // evaluated.
  static const char dotted[] = "((fn (a b) (list a b)) 1 2 . 3)";
  CHECK(ExpectEvaluationError(context, &state, "arg-dotted.fe", dotted,
                              sizeof(dotted) - 1,
                              "arg-dotted.fe:1: dotted pair in argument list"));

  // An error in an argument position unwinds through the argument frame and
  // the context stays usable afterwards.
  static const char bad_arg[] = "(add-exactly 1 (car 2))";
  CHECK(ExpectEvaluationError(context, &state, "arg-error.fe", bad_arg,
                              sizeof(bad_arg) - 1,
                              "arg-error.fe:1: expected pair, got integer"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(+ 1 2)",
                                    sizeof("(+ 1 2)") - 1),
                   "3"));

  // GC during the resumed accumulation (a collection in a later argument
  // must not sweep the values already accumulated in the frame) is covered
  // by the resumable-frame-state table's `argument` row.

  FeCloseContext(context);
  return true;
}

// Sub-plan 03D, stage 2: argument evaluation runs on the frame stack. A
// chain of native calls nested through argument positions used to recurse
// through `EvaluateList` -> `Evaluate` -> `RunEvaluation` once per level,
// so the innermost probe's C address grew by several activations per level.
// Now the nested calls are frames in one run and the native invocations are
// sequential, so the chain stops consuming C stack. `probe-id` records its
// C frame address and returns its argument; the generated chain
// `(probe-id (probe-id ... (probe-id 0)))` evaluates innermost-first, so
// the probe fires at every level and the deepest fire is the innermost one.
static bool TestArgumentProbe(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "probe-id", ProbeId);

  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  static const char baseline[] = "(probe-id 0)";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "arg-baseline.fe", baseline,
                                    sizeof(baseline) - 1),
                   "0"));
  CHECK(stack_probe_last_address != 0);
  CHECK(stack_probe_deepest_address != 0);
  const uintptr_t baseline_address = stack_probe_deepest_address;

  // 150 levels: each `(probe-id X)` call holds an argument frame with the
  // nested call as its child (~2 frames per level, ~300 frames total), well
  // inside the 1 MiB arena's frame capacity (1100) and the default logical
  // depth ceiling (peak ~151).
  enum { ChainDepth = 150, MaxFlatDelta = 2048 };
  char source[ChainDepth * 16 + 8];
  static const char prefix[] = "(probe-id ";
  size_t length = 0;
  for (int i = 0; i < ChainDepth + 1; i++) {
    memcpy(source + length, prefix, sizeof(prefix) - 1);
    length += sizeof(prefix) - 1;
  }
  source[length++] = '0';
  for (int i = 0; i < ChainDepth + 1; i++) {
    source[length++] = ')';
  }

  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  CHECK(IsRendered(
      context, FeEvaluateString(context, "arg-chain.fe", source, length), "0"));
  CHECK(stack_probe_last_address != 0);
  CHECK(stack_probe_deepest_address != 0);
  const uintptr_t deepest = stack_probe_deepest_address;
  const uintptr_t delta = baseline_address > deepest
                              ? baseline_address - deepest
                              : deepest - baseline_address;
  printf("argument probe: depth=%d frames~%d baseline=%#" PRIxPTR
         " deepest=%#" PRIxPTR " delta=%" PRIuPTR " bytes\n",
         ChainDepth, ChainDepth * 2, baseline_address, deepest, delta);
  // One old-style argument-evaluation level cost several C activations (a
  // nested `RunEvaluation` with its setjmp, `Evaluate`, `EvaluateList`,
  // `EvaluatePair` and the native call), so 150 levels measured tens of KB
  // of probe growth on the pre-frame code. A regression puts the probe back
  // on the C stack and fails far past the 2 KiB budget.
  CHECK(delta < MaxFlatDelta);

  FeCloseContext(context);
  return true;
}

// Sub-plan 03D, stage 3: the sequential-body frame keeps `DoList`'s exact
// step charges, its `let`/`newenv` threading, its error unwinding and its
// GC-rooting of the pending forms and environment, so the observable
// behaviour of a lambda body does not move as its evaluation path changes.
static bool TestLambdaBodyFrame(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // Step budget: `((fn (x) (let y 1) (list x y)) 5)` is exactly 20 steps --
  // one step before every body form, every parameter walk, and every
  // environment walk for the *variable references* (`x`, `y` resolve through
  // `GetBound`, so the `let` adds a level and the later lookups walk one
  // deeper), with none for the lambda/body frame transitions. Since 04D's
  // cut the `let`/`list` call heads resolve the function cell directly, so
  // their heads no longer charge the environment walk the pre-cut value-cell
  // fallback's `GetBound` did; the 23-step pre-cut count was 3 higher for
  // exactly those two heads.
  static const char body[] = "((fn (x) (let y 1) (list x y)) 5)";
  const FeEvalOptions tight = {.step_limit = 19};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "body.fe", body, sizeof(body) - 1, &tight,
      "body.fe:1: evaluation step limit exceeded"));
  const FeEvalOptions ok = {.step_limit = 20};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "body.fe", body,
                                               sizeof(body) - 1, &ok),
                   "(5 1)"));

  // let threading through the body frame: a `let` in a body form extends the
  // environment the following forms see, even when the lambda call itself is
  // an argument to an outer call (so the body frame is not the run's base
  // frame), and a nested lambda's body still sees the outer body's binding.
  // The parameter is called through `funcall` since 04D's cut: a lexical
  // binding is value-namespace, so calling it in head position (`(g)`) is
  // `void-function g`, exactly as in Emacs (the pinned
  // `lisp2-funcall-lexical-value` case calls its lexical value the same way).
  CHECK(
      IsRendered(context,
                 FeEvaluateString(
                     context, "nested-let.fe",
                     "((fn (g) (funcall g)) (fn () (let y 1) y))",
                     sizeof("((fn (g) (funcall g)) (fn () (let y 1) y))") - 1),
                 "1"));
  CHECK(IsRendered(
      context,
      FeEvaluateString(
          context, "closure-let.fe",
          "((fn () (let a 1) ((fn () (let b (+ a 1)) (list a b)))))",
          sizeof("((fn () (let a 1) ((fn () (let b (+ a 1)) (list a b)))))") -
              1),
      "(1 2)"));

  // A body-frame `let` is local: it shadows but does not leak past the
  // lambda, so the global keeps its value and a fresh name stays unbound.
  static const char shadow[] = "(setq y 7) ((fn () (let y 2) y))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "shadow.fe", shadow, sizeof(shadow) - 1), "2"));
  CHECK(
      IsRendered(context, FeEvaluateString(context, "after.fe", "y", 1), "7"));
  static const char local[] = "((fn () (let fresh 5) fresh))";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "local.fe", local, sizeof(local) - 1),
      "5"));
  CHECK(ExpectEvaluationError(context, &state, "leak.fe", "fresh", 5,
                              "leak.fe:1: void-variable fresh"));

  // An error in a body form unwinds through the body frame and the context
  // stays usable afterwards.
  static const char bad[] = "((fn () (car 2)))";
  CHECK(ExpectEvaluationError(context, &state, "body-error.fe", bad,
                              sizeof(bad) - 1,
                              "body-error.fe:1: expected pair, got integer"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(+ 1 2)",
                                    sizeof("(+ 1 2)") - 1),
                   "3"));

  // GC during body evaluation is covered by the resumable-frame-state table
  // (`TestResumableFrameGC`), so the collection-point assertions stay in one
  // place rather than being duplicated per test.

  FeCloseContext(context);
  return true;
}

// Sub-plan 03D, stage 3: lambda application and sequential body evaluation
// run on the frame stack. After `FeFrameCallArguments` has reordered the
// evaluated arguments, `ArgsToEnv` still binds them into the callee
// environment as an ordinary allocating helper, and the lambda's body forms
// are then evaluated one at a time by a resumable `FeFrameBody` instead of a
// recursive `DoList`. A chain of zero-argument lambdas
// `f0 -> f1 -> ... -> stack-probe` therefore stops consuming C stack: each
// body form `(fN)` is a sub-expression frame in the same evaluator run, so
// the probe fires at the same C depth for any chain length.
static bool TestLambdaBodyChain(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "stack-probe", StackProbe);

  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  static const char baseline[] = "(stack-probe)";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "body-baseline.fe", baseline,
                                    sizeof(baseline) - 1),
                   "0.0"));
  CHECK(stack_probe_last_address != 0);
  CHECK(stack_probe_deepest_address != 0);
  const uintptr_t baseline_address = stack_probe_deepest_address;

  // 200 levels, each a call to a zero-argument lambda whose body is the call
  // to the next one. The old code kept a nested `RunEvaluation` (with its
  // setjmp) plus `Evaluate`, `EvaluateList`, `ArgsToEnv` and `DoList` open
  // per level on the C stack; now every level is one body frame in one run.
  // ~202 simultaneously open frames, well inside the 1 MiB arena's frame
  // capacity (1100) and the default logical depth ceiling (peak ~202).
  enum { ChainDepth = 200, MaxFlatDelta = 2048 };
  char source[ChainDepth * 32 + 16];
  size_t length = 0;
  for (int i = 0; i < ChainDepth; i++) {
    const int written = snprintf(source + length, sizeof(source) - length,
                                 "(fset 'f%d (fn () (f%d))) ", i, i + 1);
    CHECK(written > 0);
    length += (size_t)written;
    CHECK(length < sizeof(source));
  }
  {
    const int written =
        snprintf(source + length, sizeof(source) - length,
                 "(fset 'f%d (fn () (stack-probe))) (f0)", ChainDepth);
    CHECK(written > 0);
    length += (size_t)written;
    CHECK(length < sizeof(source));
  }

  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "lambda-chain.fe", source, length),
                   "0.0"));
  CHECK(stack_probe_last_address != 0);
  CHECK(stack_probe_deepest_address != 0);
  const uintptr_t deepest = stack_probe_deepest_address;
  const uintptr_t delta = baseline_address > deepest
                              ? baseline_address - deepest
                              : deepest - baseline_address;
  printf("lambda body probe: depth=%d frames~%d baseline=%#" PRIxPTR
         " deepest=%#" PRIxPTR " delta=%" PRIuPTR " bytes\n",
         ChainDepth, ChainDepth + 2, baseline_address, deepest, delta);
  // One old-style lambda-body level cost a nested evaluator run (setjmp and
  // all) on the C stack; a regression that sends the body back through
  // `DoList` puts the probe back there and fails far past the 2 KiB budget.
  CHECK(delta < MaxFlatDelta);

  FeCloseContext(context);
  return true;
}

// Sub-plan 03D, stage 4: a macro call runs on the frame stack. The recursive
// `FeTMacro` arm of `EvaluatePair` becomes two frame kinds: `FeFrameMacro`
// evaluates the macro's body forms sequentially (the same step charges,
// `let`/`newenv` threading and GC discipline the recursive `DoList` used,
// over the raw, unevaluated arguments `ArgsToEnv` bound), and
// `FeFrameMacroExpansion` evaluates the produced expansion in the caller
// environment after restoring the macro call's cleanup, GC and call-trace
// checkpoints, holding the logical depth across the expansion exactly as the
// recursive arm held it. The physical frame wall, not the C stack, is what
// stops self-expanding macros.
static bool TestMacroFrame(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // Macro arguments stay raw: `x` is bound to the unevaluated `(+ 1 2)`
  // form, and the expansion `(quote (+ 1 2))` evaluates back to the form
  // itself. If the argument had been evaluated at binding time the result
  // would be the number 3.
  static const char raw_args[] =
      "(fset 'm (macro (x) (list 'quote x))) (m (+ 1 2))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "raw.fe", raw_args, sizeof(raw_args) - 1),
      "(+ 1 2)"));

  // `let`/`newenv` threading through the macro body: a `let` in a body form
  // extends the environment the following forms see, exactly as the
  // recursive `DoList` `&env` out-parameter did, and it shadows without
  // leaking past the macro call.
  static const char body_let[] =
      "(fset 'm (macro () (let y 1) (list 'list y y))) (m)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "body-let.fe", body_let, sizeof(body_let) - 1),
      "(1 1)"));
  static const char shadow[] =
      "(setq y 7) (fset 'm (macro () (let y 2) y)) (m)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "shadow.fe", shadow, sizeof(shadow) - 1), "2"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "after.fe", "y", sizeof("y") - 1),
                   "7"));

  // The expansion is evaluated in the caller environment, not the macro's:
  // `x` here is the lambda's lexical binding, which the expansion's `x`
  // must resolve.
  static const char caller_env[] = "(fset 'm (macro () 'x)) ((fn (x) (m)) 42)";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "caller-env.fe", caller_env,
                                    sizeof(caller_env) - 1),
                   "42"));

  // Step budget for the resumed macro frame: `(m)` with `(macro () 5)` is
  // exactly five steps -- the pair form, the symbol head, the body-form
  // charge, the body atom, and the expansion atom -- with no step for the
  // frame transitions or the empty parameter walk, matching what the
  // recursive arm charged.
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "m.fe", "(fset 'm (macro () 5))",
                                    sizeof("(fset 'm (macro () 5))") - 1),
                   "(macro nil 5)"));
  const FeEvalOptions tight = {.step_limit = 4};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "m.step.fe", "(m)", sizeof("(m)") - 1, &tight,
      "m.step.fe:1: evaluation step limit exceeded"));
  const FeEvalOptions ok = {.step_limit = 5};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "m.step.fe", "(m)",
                                               sizeof("(m)") - 1, &ok),
                   "5"));

  // An error in a later body form, after an earlier form ran and a `let`
  // extended the body environment, unwinds through the macro frame and the
  // context stays usable; the earlier form's side effect stands.
  static const char mid_body_error[] =
      "(setq ran nil) (fset 'boom (macro () (setq ran t) (let x 1) (car 2))) "
      "(boom)";
  CHECK(ExpectEvaluationError(context, &state, "mid-body.fe", mid_body_error,
                              sizeof(mid_body_error) - 1,
                              "mid-body.fe:1: expected pair, got integer"));
  CHECK(IsRendered(
      context, FeEvaluateString(context, "ran.fe", "ran", sizeof("ran") - 1),
      "t"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(+ 1 2)",
                                    sizeof("(+ 1 2)") - 1),
                   "3"));

  // GC during the macro body (a collection in a later body form must not
  // sweep the pending forms, a `let` binding, or the caller environment) is
  // covered by the resumable-frame-state table's `macro-body` row.

  // Recursively expanding macros are bounded by the physical frame wall,
  // not by tail-looping forever on the C stack: each expansion level holds
  // its macro frame -- the expansion child reuses the caller's slot and
  // becomes the next level's macro frame -- while the body-form and quote
  // sub-expressions run above it, so the stack grows one held frame per
  // level until `PushEvaluationFrame` refuses. `max_frames == 0` selects the
  // small arena's own physical capacity -- as 03C's exhaustion test did --
  // so the frame push check is what fires, with the exact frame-limit text,
  // the original macro-call trace, and a context still usable afterwards.
  const size_t gc_size = FeMinimumArenaSize() + 8 * 1024;
  static TestArena frame_arena;
  FeContext* frame_context = FeOpenContext(frame_arena.bytes, gc_size);
  CHECK(frame_context != nullptr);
  ErrorState frame_state = {.context = frame_context};
  FeSetUserData(frame_context, &frame_state);
  FeSetErrorFn(frame_context, HandleError);
  static const char self_expanding[] = "(fset 'm (macro () (list 'm))) (m)";
  const FeEvalOptions physical_only = {.max_frames = 0};
  CHECK(ExpectEvaluationOptionsError(
      frame_context, &frame_state, "macro-frames.fe", self_expanding,
      sizeof(self_expanding) - 1, &physical_only,
      "macro-frames.fe:1: evaluation frame limit exceeded"));
  CHECK(!frame_state.stack_was_nil);
  CHECK(IsRendered(frame_context,
                   FeEvaluateString(frame_context, "recovered.fe", "(+ 1 2)",
                                    sizeof("(+ 1 2)") - 1),
                   "3"));
  FeCloseContext(frame_context);

  FeCloseContext(context);
  return true;
}

// Sub-plan 03D, stage 5 (renamed and re-derived for 03F): the native-call
// boundary. After the argument frame has reordered the evaluated arguments,
// a native callable switches to its own `FeFrameNative` kind and the loop
// invokes it synchronously from that explicit state -- one fixed C
// activation, no per-native setjmp, the enclosing run's barrier the only
// one in effect. A native that calls back into `FeCallWithOptions` starts a
// nested run on a fresh C frame; that nested `RunEvaluation` call is what
// `EnterNativeReentry` bounds and counts, against `max_native_reentry`, with
// the exact "native evaluation re-entry limit exceeded" text.
//
// Off-by-one, precisely, and different from before 03F: calling a native is
// not itself re-entry, so `ReentrantNative`'s first (outermost,
// non-nested) activation is never counted -- only its own recursive
// `FeCallWithOptions` calls are, one increment per nested activation. With
// `max_native_reentry` 8, exactly 8 *nested* activations fit (activations 2
// through 9; the deepest, 9, returns its own level as the result); the 9th
// nested activation -- the call from activation 9 attempting to start
// activation 10 -- is the one that raises, so `reentry_max_seen` stays 9
// (activation 10 never starts).
static bool TestNativeReentry(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context, .reentry_max_native_reentry = 8};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeObject* native = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, native);
  FeSetFunction(context, FeMakeSymbol(context, "reentrant-native"), native);

  // Success through the allowed bound: `max_native_reentry` 8 admits 8
  // nested re-entries (9 activations total; the deepest returns its own
  // level), the ordinary-return cleanups run, and the C-side high-water
  // mark agrees.
  state.reentry_remaining = 8;
  const FeEvalOptions options = {.max_native_reentry = 8};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(
                       context, "native-reentry.fe", "(reentrant-native)",
                       sizeof("(reentrant-native)") - 1, &options),
                   "9.0"));
  CHECK(state.reentry_max_seen == 9);
  CHECK(state.reentry_cleanup_ran);
  CHECK(FeGetArenaStats(context).peak_native_reentry == 8);

  // One beyond: the 9th nested re-entry raises the exact native message. The
  // host sees the original nested trace, the registered cleanups ran during
  // the unwind, the C-side high-water mark shows the recursion really did
  // reach 8 live re-entries before the block, and the context -- including
  // a fresh re-entry through the same native boundary, which would fail at
  // level one if `native_reentry_depth` had leaked past the barrier restore
  // -- evaluates again afterwards.
  // cppcheck-suppress redundantAssignment
  state.reentry_remaining = 9;
  state.reentry_cleanup_ran = false;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "native-reentry.fe", "(reentrant-native)",
      sizeof("(reentrant-native)") - 1, &options,
      "native-reentry.fe:1: native evaluation re-entry limit exceeded"));
  CHECK(!state.stack_was_nil);
  CHECK(state.reentry_max_seen == 9);
  CHECK(state.reentry_cleanup_ran);
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(+ 1 2)",
                                    sizeof("(+ 1 2)") - 1),
                   "3"));
  state.reentry_remaining = 8;
  state.reentry_cleanup_ran = false;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(
                       context, "native-reentry.fe", "(reentrant-native)",
                       sizeof("(reentrant-native)") - 1, &options),
                   "9.0"));
  CHECK(state.reentry_max_seen == 9);
  CHECK(state.reentry_cleanup_ran);

  // The ceiling is what bounds re-entry, not a fixed internal cap: raising
  // `max_native_reentry` admits more activations, and one more again fails.
  // cppcheck-suppress redundantAssignment
  state.reentry_remaining = 16;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  const FeEvalOptions deeper = {.max_native_reentry = 16};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(
                       context, "native-reentry.fe", "(reentrant-native)",
                       sizeof("(reentrant-native)") - 1, &deeper),
                   "17.0"));
  CHECK(state.reentry_max_seen == 17);
  // cppcheck-suppress redundantAssignment
  state.reentry_remaining = 17;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "native-reentry.fe", "(reentrant-native)",
      sizeof("(reentrant-native)") - 1, &deeper,
      "native-reentry.fe:1: native evaluation re-entry limit exceeded"));
  CHECK(state.reentry_max_seen == 17);

  FeCloseContext(context);

  // GC during the native boundary (a collection in the nested evaluation a
  // native starts must not sweep the evaluated argument list in the native
  // frame's `accumulator`) is covered by the resumable-frame-state table's
  // `native` row.

  return true;
}

// Sub-plan 03D, stage 5 regression, re-derived for 03F: an owning nested
// evaluation inside a native must not corrupt the run's own
// `native_reentry_depth`. The outer call is a plain, non-owning
// `FeEvaluateString` -- no evaluation-control record is active -- so
// `OwningReenter`'s `FeCallWithOptions` takes ownership, and its
// `EndEvaluationControl` clears the ambient *limits* before the native
// returns. It deliberately does *not* clear `native_reentry_depth` itself
// any more (03F; see the field's own comment on `struct FeContext`), and
// calling `owning-reenter` is not itself re-entry (it is the outer run's
// first, non-nested native activation), so there is no counter here left
// that an owning call could corrupt: this test is the regression guard that
// keeps it that way, proving both `RunEvaluation` barriers -- the owning
// call's own nested one, and any that a *separate* re-entering native
// starts afterward in the same run -- restore their counters correctly and
// do not interfere with each other.
static bool TestNativeOwningReentry(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context, .reentry_max_native_reentry = 8};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeObject* ordinary = FeMakeNativeFn(context, OrdinaryNative);
  state.reentry_owning_target = FeCreateRoot(context, ordinary);
  FeSetFunction(context, FeMakeSymbol(context, "ordinary-native"), ordinary);
  FeDefineNative(context, "owning-reenter", OwningReenter);
  FeObject* reentrant = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, reentrant);
  FeSetFunction(context, FeMakeSymbol(context, "reentrant-native"), reentrant);

  static const char body[] = "((fn () (owning-reenter) (ordinary-native)))";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "owning.fe", body, sizeof(body) - 1),
      "42.0"));
  CHECK(FeGetArenaStats(context).peak_native_reentry == 1);

  // The same shape with the owning call removed succeeds identically, so the
  // assertion above is about the owning call, not about the lambda body.
  static const char plain[] = "((fn () (ordinary-native)))";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "owning.fe", plain, sizeof(plain) - 1),
      "42.0"));

  // A *separate* re-entering native run right after the owning call sees
  // the same numbers `TestNativeReentry` measures standalone: the owning
  // call's own nested run left `native_reentry_depth` at exactly 0
  // afterward (both because nothing increments a non-nested native call at
  // all, and because 03F stopped resetting the depth counter through
  // `ClearEvaluationControl`), so there is no leftover level for this
  // chain to inherit or lose. `max_native_reentry` 8 admits 8 nested
  // re-entries (9 activations; the deepest returns its own level).
  state.reentry_remaining = 8;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  state.reentry_cleanup_ran = false;
  static const char deep_body[] =
      "((fn () (owning-reenter) (reentrant-native)))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "owning.fe", deep_body, sizeof(deep_body) - 1),
      "9.0"));
  CHECK(state.reentry_max_seen == 9);
  CHECK(FeGetArenaStats(context).peak_native_reentry == 8);

  // One beyond the reentrant chain's own limit still raises exactly as it
  // does standalone -- the owning call ahead of it changes nothing.
  // cppcheck-suppress redundantAssignment
  state.reentry_remaining = 9;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  // cppcheck-suppress redundantAssignment
  state.reentry_cleanup_ran = false;
  CHECK(ExpectEvaluationError(
      context, &state, "owning.fe", deep_body, sizeof(deep_body) - 1,
      "owning.fe:1: native evaluation re-entry limit exceeded"));
  CHECK(state.reentry_max_seen == 9);
  CHECK(state.reentry_cleanup_ran);

  // An ordinary native still succeeds after an owning call that raised: the
  // error path is the enclosing barrier's job, and both counters are clean
  // for the next evaluation.
  state.reentry_remaining = 8;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  state.reentry_cleanup_ran = false;
  static const char erroring[] =
      "((fn () (owning-reenter) (car 1) (ordinary-native)))";
  CHECK(ExpectEvaluationError(context, &state, "owning.fe", erroring,
                              sizeof(erroring) - 1,
                              "owning.fe:1: expected pair, got integer"));
  CHECK(IsRendered(
      context, FeEvaluateString(context, "owning.fe", body, sizeof(body) - 1),
      "42.0"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 03D's "GC during every resumable frame state" requirement, as one
// table of state/setup/expected-result cases rather than one bespoke GC block
// per test. Each row names the frame kind whose suspension it isolates, the
// form that suspends it (the setup), and the expected rendered result; the
// shared runner opens a fresh small-arena context per row, runs the setup,
// and asserts both that the result is right and that `collection_count`
// moved -- a collection ran while the frame was suspended, and the resumed
// evaluation still found the objects only that frame held. `prepare`, where
// set, registers the natives a row needs before its setup runs.
typedef struct FrameGCCase {
  const char* state;     // the suspended frame kind under test
  const char* setup;     // the form that suspends it and forces collections
  const char* expected;  // the expected rendered result
  bool (*prepare)(FeContext* context, ErrorState* state);
} FrameGCCase;

// The native row's setup calls `gc-native`, so it must be registered with
// one nested-call hop left (`GCNative` returns its argument without
// re-entering when the budget is exhausted, and the value crosses the one
// nested `FeCallWithOptions` it does start while the collections run).
static bool PrepareGCNative(FeContext* context, ErrorState* state) {
  state->reentry_remaining = 1;
  FeObject* native = FeMakeNativeFn(context, GCNative);
  state->reentry_self = FeCreateRoot(context, native);
  FeSetFunction(context, FeMakeSymbol(context, "gc-native"), native);
  return true;
}

static bool RunFrameGCCase(const FrameGCCase* c) {
  static TestArena arena;
  const size_t gc_size = FeMinimumArenaSize() + 8 * 1024;
  FeContext* context = FeOpenContext(arena.bytes, gc_size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  bool prepared = c->prepare == nullptr || c->prepare(context, &state);
  const size_t collections = FeGetArenaStats(context).collection_count;
  const bool rendered =
      prepared && IsRendered(context,
                             FeEvaluateString(context, "frame-gc.fe", c->setup,
                                              strlen(c->setup)),
                             c->expected);
  const bool collected =
      FeGetArenaStats(context).collection_count > collections;
  FeCloseContext(context);
  if (!rendered) {
    fprintf(stderr, "frame-gc %s: %s did not render %s\n", c->state, c->setup,
            c->expected);
    return false;
  }
  if (!collected) {
    fprintf(stderr, "frame-gc %s: collection_count did not move\n", c->state);
    return false;
  }
  return true;
}

static bool TestResumableFrameGC(void) {
  static const FrameGCCase cases[] = {
      // call-head: the computed head's `do` loop collects while the call-head
      // frame below is suspended waiting for the head's value (the lambda);
      // the whole call form, argument included, must survive to be resumed.
      {"call-head",
       "((do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) "
       "(fn (x) x)) 42)",
       "42", nullptr},
      // argument: a collection inside the third argument must not sweep the
      // first two arguments already accumulated in the argument frame.
      {"argument",
       "((fn (x y z) (list x y z)) 1 2 (do (setq n 0) "
       "(while (< n 2000) (setq n (+ n 1)) (cons n n)) n))",
       "(1 2 2000)", nullptr},
      // lambda/body: a collection inside a body form must not sweep the
      // pending forms or the bindings the resumed last form reads. The lambda
      // application frame itself is a synchronous transition to the body
      // frame, so the body is the resumable state this row isolates.
      {"lambda/body",
       "((fn () (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) n))",
       "2000", nullptr},
      // macro-body: a collection inside a body form must not sweep the
      // pending forms, a `let` binding, or the caller environment.
      {"macro-body",
       "(fset 'm (macro () (let v 7) (setq n 0) "
       "(while (< n 2000) (setq n (+ n 1)) (cons n n)) v)) (m)",
       "7", nullptr},
      // macro-expansion: the macro body produces the collecting `do` form as
      // its expansion with no collection of its own, so the collections run
      // only while the macro-expansion frame below is suspended over the
      // expansion sub-expression -- isolating the expansion state from the
      // body state.
      {"macro-expansion",
       "(fset 'm (macro () (quote (do (setq n 0) "
       "(while (< n 2000) (setq n (+ n 1)) (cons n n)) n)))) (m)",
       "2000", nullptr},
      // native: a collection in the nested evaluation a native starts must
      // not sweep the evaluated argument list in the native frame's
      // `accumulator`; the same value then crosses one nested native call.
      {"native", "(gc-native (list 1 2 3))", "(1 2 3)", PrepareGCNative},
      // Sub-plan 03E's special-form/primitive continuations, added to the
      // same table for the same reason: each row's setup forces a
      // collection while that one frame kind is suspended, waiting on a
      // delivered sub-expression.
      // if: the taken branch's collecting loop must not disturb the
      // suspended `if` frame's own `env`/`rest`.
      {"if",
       "(if t (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) "
       "n) 0)",
       "2000", nullptr},
      // and/or: same property, for the short-circuit chain.
      {"and-or",
       "(and 1 (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n "
       "n)) n))",
       "2000", nullptr},
      // while: the frame's own fixed condition/body forms (`fn`/`rest`) must
      // survive collections forced by every iteration's own body.
      {"while",
       "(do (setq n 0) (setq i 0) (while (< i 3) (setq i (+ i 1)) (setq m "
       "0) (while (< m 2000) (setq m (+ m 1)) (cons m m))) i)",
       "3", nullptr},
      // let: the raw target symbol in `accumulator` must survive collections
      // forced by evaluating the value form.
      {"let",
       "(do (let x (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons "
       "n n)) n)) x)",
       "2000", nullptr},
      // setq: the pending target symbol in `accumulator` must survive
      // collections forced by evaluating the value form, and an earlier
      // pair's binding must still be intact afterwards.
      {"setq",
       "(do (setq a 1) (setq b (do (setq n 0) (while (< n 2000) (setq n (+ "
       "n 1)) (cons n n)) n)) (list a b))",
       "(1 2000)", nullptr},
      // unary (assert/not/atom/car/cdr/boundp/makunbound): no state is held
      // across the single operand, but the frame itself must survive.
      {"unary",
       "(not (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) "
       "nil))",
       "t", nullptr},
      // binary (cons/setcar/setcdr/is/</<=): the checked first operand in
      // `accumulator` must survive collections forced by evaluating the
      // second.
      {"binary",
       "(cons 1 (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n "
       "n)) n))",
       "(1 . 2000)", nullptr},
      // arith (+/-/*//): the running boxed total in `accumulator` must
      // survive collections forced by evaluating a later operand.
      {"arith",
       "(+ 1000 (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n "
       "n)) 1000))",
       "2000", nullptr},
      // eval-list (list/=/set): the partially-built reversed list in
      // `accumulator` must survive collections forced by evaluating a later
      // element.
      {"eval-list",
       "(list 1 (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n "
       "n)) n) 3)",
       "(1 2000 3)", nullptr},
      // relay (do/unwind-protect's body): the pending cleanup entry (03C's
      // direct root over `.forms`/`.env`) must survive collections forced by
      // the protected body.
      {"relay",
       "(unwind-protect"
       "  (do (setq n 0) (while (< n 2000) (setq n (+ n 1)) (cons n n)) n)"
       "  (setq relay-cleanup-ran t))",
       "2000", nullptr},
      // funcall/apply (04C's evaluate-then-redispatch): the evaluated
      // operand buffer, rooted in the EvalList frame's `accumulator` and
      // carried by the relay frame across the redispatch, must survive
      // collections forced by the *called body* -- the whole point of the
      // 03F lesson applied to this new boundary.
      {"funcall",
       "(funcall (fn (x y) (setq n 0) (while (< n 2000) (setq n (+ n 1)) "
       "(cons n n)) (list x y)) 1 2)",
       "(1 2)", nullptr},
      {"apply",
       "(apply (fn (x y) (setq n 0) (while (< n 2000) (setq n (+ n 1)) "
       "(cons n n)) (list x y)) (list 1 2))",
       "(1 2)", nullptr},
      // apply's own rebuild: `SpreadApplyArgs` and `MakeCallForm` are
      // allocation loops that hold their partial results in the frame's
      // `accumulator` and one GC checkpoint apiece -- the shape that keeps
      // their GC-stack cost fixed instead of four slots per element. A spread
      // wide enough to collect *inside* those loops is what pins that
      // rooting: every element has to survive to the sum.
      {"apply-spread-rebuild",
       "(do (setq n 0) (setq xs nil) (while (< n 60) (setq n (+ n 1)) "
       "(setq xs (cons n xs))) (apply '+ 1 2 xs))",
       "1833", nullptr},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (!RunFrameGCCase(&cases[i])) {
      return false;
    }
  }
  return true;
}

// Sub-plan 03E: GC during an actual cleanup *drain* -- `RunEvaluationBody`'s
// synthetic base frame, entered only while a cleanup is running after an
// error, not while `unwind-protect`'s own protected body runs (that is the
// "relay" row of `TestResumableFrameGC`, above). The cleanup allocates
// heavily enough to force collections, and a lexical binding from the
// *body* (`x`, reachable only through the environment `unwind-protect`
// captured, never through the global symbol table) must still resolve
// correctly afterward -- the same root-survival property
// `TestUnwindLisp`'s "Root survival" case already pins for the body itself,
// now pinned for the cleanup's own nested run.
static bool TestCleanupRunGC(void) {
  static TestArena arena;
  const size_t gc_size = FeMinimumArenaSize() + 8 * 1024;
  FeContext* context = FeOpenContext(arena.bytes, gc_size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // 2000 iterations, the same scale `TestResumableFrameGC`'s ordinary-body
  // rows use to force a collection in this small arena -- but this loop
  // runs *inside* the cleanup, under `RunCleanupsAfterError`'s fresh
  // per-entry budget, which defaults to only `DefaultCleanupStepLimit`
  // (4096, far too little for 2000 iterations: measured empirically against
  // this exact form, 150 iterations fit and 180 do not). An explicit,
  // generous `cleanup_step_limit` -- the same option
  // `TestUnwindCleanupBudget` uses for the opposite reason (to prove a
  // *small* one terminates a runaway cleanup) -- avoids that ceiling here.
  // This margin matters: a real regression during this slice's development
  // used 2000 iterations with the *default* budget, silently truncating the
  // cleanup and leaving `cleanup-result` unbound; the follow-up check below
  // then raised `void-variable` into a `setjmp` whose enclosing
  // `ExpectEvaluationError` call had already returned -- a `longjmp` into a
  // dead stack frame, corrupting the C stack instead of failing loudly.
  const size_t collections_before = FeGetArenaStats(context).collection_count;
  static const char source[] =
      "(do"
      "  (let x (cons 111 222))"
      "  (unwind-protect"
      "    (car 1)"
      "    (do (setq n 0)"
      "        (while (< n 2000) (setq n (+ n 1)) (cons n n))"
      "        (setq cleanup-result (list x n)))))";
  const FeEvalOptions generous_cleanup_budget = {.cleanup_step_limit = 100000};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "cleanup-gc.fe", source, sizeof(source) - 1,
      &generous_cleanup_budget, "cleanup-gc.fe:1: expected pair, got integer"));
  CHECK(FeGetArenaStats(context).collection_count > collections_before);
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "check.fe", "cleanup-result", 14),
                   "((111 . 222) 2000)"));

  FeCloseContext(context);
  return true;
}

// Sub-plan 03E's required "primitive evaluation-order regressions" table:
// the distinctions `doc/plans/.../03e-special-form-frames-and-unwind.md` in
// kg names explicitly, that a generic "evaluate every operand first" policy
// would erase. `TestSetqAndSet`, `TestNumericEqual` and `TestBinding`
// already pin `setq`/`set`/`=`'s own order rules; this covers the
// remaining distinctions those tests do not reach.
static bool TestPrimitiveOrder(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

#define CHK(expr, expected)                                                   \
  CHECK(IsRendered(context,                                                   \
                   FeEvaluateString(context, "order.fe", expr, strlen(expr)), \
                   expected))
#define ORDER_ERR(expr, message)                                               \
  CHECK(ExpectEvaluationError(context, &state, "order.fe", expr, strlen(expr), \
                              message))

  // `setcar`/`setcdr` validate the first evaluated operand as a pair before
  // the second is even evaluated: a type error on operand 1 means operand
  // 2's side effect never runs.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "setcar-probe")));
  ORDER_ERR("(setcar 1 (do (setq setcar-probe t) 2))",
            "order.fe:1: expected pair, got integer");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "setcar-probe")));
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "setcdr-probe")));
  ORDER_ERR("(setcdr 1 (do (setq setcdr-probe t) 2))",
            "order.fe:1: expected pair, got integer");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "setcdr-probe")));

  // Arithmetic validates as it walks, unlike `=`'s evaluate-the-whole-list-
  // first policy (`TestNumericEqual`): a type error on an early operand
  // stops evaluation before a later operand's form ever runs.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "arith-probe")));
  ORDER_ERR("(+ 1 \"x\" (do (setq arith-probe t) 3))",
            "order.fe:1: wrong-type-argument");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "arith-probe")));

  // Chained comparators evaluate their whole operand list before checking any
  // numeric type, matching `=`'s EvalList semantics. The third form runs, then
  // its own error is reported during comparison.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "less-probe")));
  ORDER_ERR("(< 1 2 (do (setq less-probe t) (car 1)))",
            "order.fe:1: expected pair, got integer");
  CHECK(FeIsBound(context, FeMakeSymbol(context, "less-probe")));
  CHK("(makunbound 'less-probe)", "less-probe");
  ORDER_ERR("(<= 2 2 (do (setq less-probe t) (car 1)))",
            "order.fe:1: expected pair, got integer");
  CHECK(FeIsBound(context, FeMakeSymbol(context, "less-probe")));
  CHK("(makunbound 'less-probe)", "less-probe");

  // Every primitive sharing this frame kind rejects a leftover argument now,
  // through the one table, with one condition: the pre-Phase-7 split where
  // `boundp`/`makunbound` said "too many arguments" and `not`/`atom`/`car`/
  // `cdr`/`assert` silently dropped extras is gone. Emacs 31.0.90 agrees on
  // both the condition and the data for all of them -- measured,
  // `(boundp 'car 'extra)` is `(wrong-number-of-arguments boundp 2)` and
  // `(car 1 2)` is `(wrong-number-of-arguments car 2)`.
  ORDER_ERR("(boundp 'car 'extra)", "order.fe:1: wrong-number-of-arguments");
  ORDER_ERR("(makunbound 'car 'extra)",
            "order.fe:1: wrong-number-of-arguments");
  ORDER_ERR("(integerp)", "order.fe:1: wrong-number-of-arguments");
  ORDER_ERR("(symbol-value 'a 'b)", "order.fe:1: wrong-number-of-arguments");

  // `cons`'s two operands evaluate left to right.
  CHK("(setq cons-order '())", "nil");
  CHK("(cons (do (setq cons-order (cons 1 cons-order)) 1)"
      "      (do (setq cons-order (cons 2 cons-order)) 2))",
      "(1 . 2)");
  CHK("cons-order", "(2 1)");

  // `if` requires both a condition and a consequent.
  ORDER_ERR("(if)", "order.fe:1: wrong-number-of-arguments");
  ORDER_ERR("(if nil)", "order.fe:1: wrong-number-of-arguments");
  ORDER_ERR("(if t)", "order.fe:1: wrong-number-of-arguments");

  // `let`'s `newenv == NULL` case: used where the enclosing evaluation was
  // never going to extend any sequence's environment -- here, `if`'s
  // then-branch, pushed with `bind=NULL` exactly as the recursive arm's
  // `EVAL_ARG()` was -- the value form is never even evaluated, however odd
  // that looks; the raw target symbol is still never bound to anything.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "let-null-probe")));
  CHK("(if t (let let-target (do (setq let-null-probe t) 99)))", "nil");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "let-null-probe")));
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "let-target")));

#undef ORDER_ERR
#undef CHK

  FeCloseContext(context);
  return true;
}

// One row per distinct primitive/special-form resumption strategy 03E
// added, shared by the budget-exhaustion and cancellation tests below: each
// form suspends that frame kind on an unbounded inner `(while t 1)`, so a
// small step budget or a host interrupt fires while it is live.
typedef struct ResumptionStressCase {
  const char* state;
  const char* form;
} ResumptionStressCase;

static const ResumptionStressCase kResumptionStressCases[] = {
    {"if", "(if t (while t 1))"},       {"and-or", "(and t (while t 1))"},
    {"while", "(while t (while t 1))"}, {"let", "(do (let x (while t 1)))"},
    {"setq", "(setq x (while t 1))"},   {"unary", "(car (while t 1))"},
    {"binary", "(cons 1 (while t 1))"}, {"arith", "(+ 1 (while t 1))"},
    {"print", "(print (while t 1))"},   {"eval-list", "(list 1 (while t 1))"},
    {"relay-do", "(do (while t 1))"},
};

// Sub-plan 03E's "budget exhaustion ... at every distinct resumption
// strategy" requirement: a tiny step budget exhausted while each frame kind
// above is live on the stack, asserting the existing external message and
// that the context is fully reusable afterward -- not a claim about which
// exact form ran when the budget hit zero, since that is an implementation
// detail of how many steps each kind's own bookkeeping charges.
static bool RunResumptionBudgetCase(const ResumptionStressCase* c) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  const FeEvalOptions tiny = {.step_limit = 10};
  const bool raised = ExpectEvaluationOptionsError(
      context, &state, "budget.fe", c->form, strlen(c->form), &tiny,
      "budget.fe:1: evaluation step limit exceeded");
  const bool recovered =
      raised &&
      IsRendered(context,
                 FeEvaluateString(context, "recovered.fe", "(+ 1 2)", 7), "3");
  FeCloseContext(context);
  if (!raised) {
    fprintf(stderr, "resumption-budget %s: did not raise step-limit error\n",
            c->state);
    return false;
  }
  if (!recovered) {
    fprintf(stderr, "resumption-budget %s: context not reusable after\n",
            c->state);
    return false;
  }
  return true;
}

static bool TestResumableFrameBudget(void) {
  for (size_t i = 0;
       i < sizeof(kResumptionStressCases) / sizeof(kResumptionStressCases[0]);
       i++) {
    if (!RunResumptionBudgetCase(&kResumptionStressCases[i])) {
      return false;
    }
  }
  return true;
}

// The same table, cancelled by a host interrupt instead of a step budget --
// 03E's other required resumption axis. `EvaluationStep`'s interrupt check
// and step-budget check are the same call site, so this mainly proves the
// cancellation path (a distinct `FeHandleError` message, and
// `RunCleanupsAfterError`'s same drain) also reaches every one of these
// frame kinds and leaves the context reusable, not a new mechanism per kind.
static bool RunResumptionCancelCase(const ResumptionStressCase* c) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  InterruptState interrupt = {
      .context = context, .expected_userdata = &interrupt, .cancel_after = 3};
  const FeEvalOptions options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  const bool cancelled = ExpectEvaluationOptionsError(
      context, &state, "cancel.fe", c->form, strlen(c->form), &options,
      "cancel.fe:1: evaluation cancelled");
  const bool polled = cancelled && interrupt.polls == interrupt.cancel_after;
  const bool recovered =
      cancelled &&
      IsRendered(context,
                 FeEvaluateString(context, "recovered.fe", "(+ 1 2)", 7), "3");
  FeCloseContext(context);
  if (!cancelled || !polled || !recovered) {
    fprintf(stderr,
            "resumption-cancel %s: failed (cancelled=%d polled=%d "
            "recovered=%d)\n",
            c->state, cancelled, polled, recovered);
    return false;
  }
  return true;
}

static bool TestResumableFrameCancel(void) {
  for (size_t i = 0;
       i < sizeof(kResumptionStressCases) / sizeof(kResumptionStressCases[0]);
       i++) {
    if (!RunResumptionCancelCase(&kResumptionStressCases[i])) {
      return false;
    }
  }
  return true;
}

// LIFO across cleanup kinds: the registry is one shared stack regardless of
// whether an entry is a native (`FeProtectWithCleanup`, via `with-resource`)
// or Lisp (`unwind-protect`) one, per `doc/unwind-design.md`. Both
// nestings are checked, since only one direction was exercised by
// `TestUnwindHostAPI` (native-only) and `TestUnwindLisp` (Lisp-only).
static bool TestMixedCleanupLIFO(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "with-resource", WithResource);

  // The native cleanup is registered first (by `with-resource`, before it
  // calls the thunk); the thunk's own `unwind-protect` registers its Lisp
  // cleanup second, from inside the call. Most recently pushed first means
  // the Lisp cleanup runs before the native one closes the file.
  ResetResourceState();
  static const char native_then_lisp[] =
      "(setq log '())"
      "(with-resource (fn () (unwind-protect 42 (setq log (cons 'lisp "
      "log)))))";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "mixed.fe", native_then_lisp,
                                    sizeof(native_then_lisp) - 1),
                   "42"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_count == 1);
  CHECK(IsRendered(context, FeEvaluateString(context, "log.fe", "log", 3),
                   "(lisp)"));

  // Reversed nesting: the Lisp `unwind-protect` now encloses the native
  // resource, so the native cleanup (registered second, from inside the
  // body) drains first.
  ResetResourceState();
  static const char lisp_then_native[] =
      "(setq log2 '())"
      "(unwind-protect"
      "  (with-resource (fn () 43))"
      "  (setq log2 (cons 'outer log2)))";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "reversed.fe", lisp_then_native,
                                    sizeof(lisp_then_native) - 1),
                   "43"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_count == 1);
  CHECK(IsRendered(context, FeEvaluateString(context, "log2.fe", "log2", 4),
                   "(outer)"));

  FeCloseContext(context);
  return true;
}

// 07B item 6's native-call record, exercised from the three directions the
// slice owns. `InnerArityNative` consumes one argument and rejects the rest,
// so both helper raises are reachable from Lisp; `OuterArityNative` does the
// same but re-enters the evaluator first, with a `condition-case` inside that
// nested run catching an inner native's arity raise -- the case where
// `RaiseCompletionCore` cleared the record and the native arm's own restore
// was jumped over.
static FeObject* InnerArityNative(FeContext* context,
                                  // cppcheck-suppress constParameterCallback
                                  FeObject* arguments) {
  (void)FeGetNextArgument(context, &arguments);
  FeRequireNoArguments(context, arguments);
  return FeNil(context);
}

static FeObject* OuterArityNative(FeContext* context,
                                  // cppcheck-suppress constParameterCallback
                                  FeObject* arguments) {
  (void)FeGetNextArgument(context, &arguments);
  static const char nested[] =
      "(setq nested-condition (condition-case e (inner-arity 1 2 3) "
      "(wrong-number-of-arguments e)))";
  (void)FeEvaluateString(context, "nested.fe", nested, sizeof(nested) - 1);
  // Only now, after a handler in the nested run has run and returned, does
  // the outer native ask for its own record back.
  FeRequireNoArguments(context, arguments);
  return FeNil(context);
}

// Allocates hard inside the nested run so a collection is in flight around
// the enclosing native's record, then raises through it.
static FeObject* ChurningArityNative(FeContext* context,
                                     // cppcheck-suppress constParameterCallback
                                     FeObject* arguments) {
  (void)FeGetNextArgument(context, &arguments);
  static const char churn[] =
      "(do (setq n 0) (while (< n 100000) (setq n (+ n 1)) (cons n n)) n)";
  (void)FeEvaluateString(context, "churn.fe", churn, sizeof(churn) - 1);
  FeRequireNoArguments(context, arguments);
  return FeNil(context);
}

// The `(FUNCTION NARGS)` condition data on the *host* side of 07A Decision 2,
// which nothing tested: the record `FeGetNextArgument` and
// `FeRequireNoArguments` read is published per native call and has to survive
// a nested run, a nested native, and a condition caught inside either.
static bool TestNativeArityRecord(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "inner-arity", InnerArityNative);
  FeDefineNative(context, "outer-arity", OuterArityNative);
  FeDefineNative(context, "churn-arity", ChurningArityNative);

#define CHK(expr, expected)                                                 \
  CHECK(IsRendered(                                                         \
      context, FeEvaluateString(context, "rec.fe", expr, sizeof(expr) - 1), \
      expected))

  // Direct symbol-head calls: the identity is the symbol the program wrote,
  // the count is the actual one, and both directions raise.
  CHK("(condition-case e (inner-arity 1 2) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments inner-arity 2)");
  CHK("(condition-case e (inner-arity) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments inner-arity 0)");
  CHK("(condition-case e (inner-arity 1 2 3 4) "
      "(wrong-number-of-arguments e))",
      "(wrong-number-of-arguments inner-arity 4)");
  // A computed head has no symbol to name, so Decision 2 says the callable
  // object goes in the condition instead.
  CHK("(condition-case e (funcall 'inner-arity 1 2) "
      "(wrong-number-of-arguments e))",
      "(wrong-number-of-arguments [native-fn] 2)");

  // The record is per call, restored around a nested one: the inner native's
  // raise is caught inside a run the *outer* native started, and the outer
  // native's own raise afterwards still names the outer native and its own
  // count. Both halves are asserted, because a restore that put back the
  // wrong record would keep the second one passing on its own.
  CHK("(condition-case e (outer-arity 1 2) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments outer-arity 2)");
  CHK("nested-condition", "(wrong-number-of-arguments inner-arity 3)");

  // And with a collection forced through the nested run first, so the
  // identity the record holds has been through a mark phase.
  const size_t collections = FeGetArenaStats(context).collection_count;
  CHK("(condition-case e (churn-arity 1 2) (wrong-number-of-arguments e))",
      "(wrong-number-of-arguments churn-arity 2)");
  CHECK(FeGetArenaStats(context).collection_count > collections);

  // Decision 2 also pins the rendered text: the condition changed, the
  // message did not.
  CHECK(ExpectEvaluationError(context, &state, "rec.fe", "(inner-arity 1 2)",
                              strlen("(inner-arity 1 2)"),
                              "rec.fe:1: too many arguments"));
  CHECK(ExpectEvaluationError(context, &state, "rec.fe", "(inner-arity)",
                              strlen("(inner-arity)"),
                              "rec.fe:1: too few arguments"));
#undef CHK

  FeCloseContext(context);
  return true;
}

// 07B's "forced GC at each allocation in the data-list construction".
// `RaiseNativeArity` allocates twice -- the count, then the two-element
// `(FUNCTION NARGS)` list -- and both the callable and the count have to stay
// rooted across both. A collection landing between them is the failure mode,
// so this walks one *through* the sequence: the arena is filled with
// unrooted garbage until exactly `headroom` objects are free, and then the
// raise runs. The first `headroom` allocations of the evaluation succeed and
// the next one collects, so sweeping `headroom` moves that collection one
// allocation at a time across the whole raise, the two data allocations
// included.
//
// The form is read once and re-evaluated from the rooted object: reading it
// per iteration would spend the headroom on the reader and never let the
// collection reach the raise at all.
static bool TestArityDataUnderCollection(void) {
  static const char* const raises[] = {
      // A primitive, whose identity is the head symbol.
      "'(condition-case e (car 1 2) (wrong-number-of-arguments e))",
      // A closure, whose identity is the closure object.
      "'(condition-case e ((lambda (x) x) 1 2) (wrong-number-of-arguments e))",
      // A host native, through the argument helpers' own record.
      "'(condition-case e (inner-arity 1 2) (wrong-number-of-arguments e))",
  };
  static const char* const expected[] = {
      "(wrong-number-of-arguments car 2)",
      "(wrong-number-of-arguments (lambda (x) x) 2)",
      "(wrong-number-of-arguments inner-arity 2)",
  };

  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "inner-arity", InnerArityNative);

  for (size_t which = 0; which < sizeof(raises) / sizeof(raises[0]); which++) {
    FeRoot* const form = FeCreateRoot(
        context, FeEvaluateString(context, "form.fe", raises[which],
                                  strlen(raises[which])));
    CHECK(form != nullptr);
    size_t collected = 0;
    for (size_t headroom = 1; headroom <= 32; headroom++) {
      const size_t gc = FeSaveGC(context);
      while (FeGetArenaStats(context).free_slots > headroom) {
        (void)FeCons(context, FeNil(context), FeNil(context));
        // Dropped from the root stack immediately, so it is garbage the
        // collection below reclaims rather than a live object that would
        // turn the squeeze into arena exhaustion.
        FeRestoreGC(context, gc);
      }
      const size_t collections = FeGetArenaStats(context).collection_count;
      CHECK(IsRendered(context, FeEvaluate(context, FeGetRoot(form)),
                       expected[which]));
      if (FeGetArenaStats(context).collection_count > collections) {
        collected++;
      }
      FeRestoreGC(context, gc);
    }
    // A raise this size allocates fewer than 32 objects, so the widest
    // headrooms legitimately finish without collecting at all; what matters
    // is that the narrow ones did, which is what puts a collection inside the
    // raise rather than before it.
    printf("arity data under collection: form %zu collected in %zu of 32\n",
           which, collected);
    CHECK(collected >= 4);
    FeReleaseRoot(context, form);
  }

  FeCloseContext(context);
  return true;
}

// Fills the root stack from inside a native so the overflow report itself is
// the thing under test.
static FeObject* PushRootsPastTheLimit(FeContext* context,
                                       FeObject* arguments) {
  (void)arguments;
  for (size_t i = 0; i <= GcStackSize; i++) {
    FePushGC(context, FeNil(context));
  }
  return FeNil(context);
}

// Builds `HEAD 1 2 ... COUNT TAIL`, evaluates it, and compares the rendering.
static bool EvaluateLongCall(FeContext* context,
                             char* source,
                             size_t size,
                             const char* head,
                             const char* tail,
                             size_t count,
                             const char* expected) {
  int written = snprintf(source, size, "%s", head);
  if (written <= 0 || (size_t)written >= size) {
    return false;
  }
  size_t used = (size_t)written;
  for (size_t i = 1; i <= count; i++) {
    written = snprintf(source + used, size - used, " %zu", i);
    if (written <= 0 || (size_t)written >= size - used) {
      return false;
    }
    used += (size_t)written;
  }
  written = snprintf(source + used, size - used, "%s", tail);
  if (written <= 0 || (size_t)written >= size - used) {
    return false;
  }
  used += (size_t)written;
  return IsRendered(context, FeEvaluateString(context, "long.fe", source, used),
                    expected);
}

// A call's root-stack cost must not grow with its argument count, and the
// overflow report must be an error rather than a crash.
//
// Both halves are the same defect. `ArgsToEnv` consed the argument list twice
// on the way to a `&rest` parameter and `ResumeArguments` left every
// accumulator cell on the root stack, so a call cost three `FePushGC` slots
// per argument and passed 4096 at roughly 1400 arguments -- and the report
// for that was `FeHandleError`, which allocates, which pushes, which
// overflowed again, without bound, until the C stack died with SIGSEGV.
// So the assertion is a *constant*: the 3000-argument calls may not cost one
// root-stack slot more than the 1500-argument ones.
static bool TestLongArgumentLists(void) {
  const size_t arena_size = 32ULL * 1024 * 1024;
  unsigned char* arena = malloc(arena_size);
  CHECK(arena != nullptr);
  FeContext* context = FeOpenContext(arena, arena_size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  const size_t source_size = 64 * 1024;
  char* source = malloc(source_size);
  CHECK(source != nullptr);

  // Every spelling that collects a tail: `&rest`, Fe's dotted tail, and the
  // same shapes reached through `apply`'s spread rather than written out.
  static const size_t counts[] = {1500, 3000};
  size_t peaks[2] = {0, 0};
  for (size_t pass = 0; pass < 2; pass++) {
    const size_t count = counts[pass];
    CHECK(EvaluateLongCall(context, source, source_size,
                           "((fn (a &rest r) (car r))", ")", count, "2"));
    CHECK(EvaluateLongCall(context, source, source_size,
                           "((fn (a . r) (car r))", ")", count, "2"));
    CHECK(EvaluateLongCall(context, source, source_size,
                           "(apply (fn (a &rest r) (car r)) (list", "))", count,
                           "2"));
    CHECK(EvaluateLongCall(context, source, source_size, "((fn (a &rest r) a)",
                           ")", count, "1"));
    peaks[pass] = FeGetArenaStats(context).peak_gc_stack_depth;
  }
  printf("gc stack across argument counts: %zu args=%zu %zu args=%zu\n",
         counts[0], peaks[0], counts[1], peaks[1]);
  CHECK(peaks[0] == peaks[1]);
  CHECK(peaks[1] < GcStackSize - GcStackReserve);

  // And the overflow itself, provoked directly, is a reportable Lisp error.
  FeDefineNative(context, "push-roots", PushRootsPastTheLimit);
  CHECK(ExpectEvaluationError(context, &state, "overflow.fe", "(push-roots)",
                              strlen("(push-roots)"),
                              "overflow.fe:1: GC stack overflow"));
  // The context survives it: the barrier restored the stack, so ordinary
  // evaluation continues in the same context.
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "after.fe", "(+ 1 2)", strlen("(+ 1 2)")),
      "3"));

  free(source);
  FeCloseContext(context);
  free(arena);
  return true;
}

// The GC stack must not grow with Lisp nesting. Every frame is a mark-phase
// root, so an intermediate result needs no separate `FePushGC`: it is
// delivered straight into the frame below's `callee`. Before that held, each
// completed frame left one entry behind, `GcStackSize` (4096, a fixed
// `FeContext` array that no arena size can grow) bounded recursion at ~1021
// levels, and it -- not the frame stack -- was what stopped `(deep 100000)`.
// Asserting a *constant* rather than a threshold is the point: a regression
// that reintroduces per-level retention shows up as growth here however
// small its per-level cost is.
static bool TestGcStackConstantInNesting(void) {
  const size_t arena_size = 32ULL * 1024 * 1024;
  unsigned char* arena = malloc(arena_size);
  CHECK(arena != nullptr);
  FeContext* context = FeOpenContext(arena, arena_size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char deep_def[] =
      "(fset 'deep (fn (n) (if (<= n 0) 0 (+ 1 (deep (- n 1))))))";
  CHECK(FeEvaluateString(context, "deep-def.fe", deep_def,
                         sizeof(deep_def) - 1) != nullptr);

  // Same context for both runs: `peak_gc_stack_depth` is a high-water mark,
  // so the deeper run can only ever raise it, never mask a rise.
  enum { ShallowN = 200, DeepN = 2000 };
  const FeEvalOptions options = {.max_frames = 3 * (size_t)DeepN * 2};
  char source[32];
  char expected[16];

  int written = snprintf(source, sizeof(source), "(deep %d)", ShallowN);
  CHECK(written > 0 && (size_t)written < sizeof(source));
  (void)snprintf(expected, sizeof(expected), "%d", ShallowN);
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "shallow.fe", source,
                                               (size_t)written, &options),
                   expected));
  const size_t shallow_peak = FeGetArenaStats(context).peak_gc_stack_depth;

  written = snprintf(source, sizeof(source), "(deep %d)", DeepN);
  CHECK(written > 0 && (size_t)written < sizeof(source));
  (void)snprintf(expected, sizeof(expected), "%d", DeepN);
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "deep.fe", source,
                                               (size_t)written, &options),
                   expected));
  const FeArenaStats stats = FeGetArenaStats(context);
  printf(
      "gc stack across nesting: deep(%d)=%zu deep(%d)=%zu (peak_frame_depth "
      "%zu)\n",
      ShallowN, shallow_peak, DeepN, stats.peak_gc_stack_depth,
      stats.peak_frame_depth);
  // Ten times the nesting, not one more GC-stack slot.
  CHECK(stats.peak_gc_stack_depth == shallow_peak);
  // And the run really was ten times deeper, so the equality above is not
  // the two runs having quietly done the same amount of work: this
  // canonical chain's single-else-form `if` special case (`ResumeIf`'s own
  // comment) keeps exactly 3 simultaneously-open frames per level -- the
  // call, `if`, and the arithmetic waiting on its second operand, the same
  // count kg's 03A Decision derives and cross-validates -- with no extra
  // implicit-body frame. `+4`, measured: one more than the deleted logical
  // `evaluation_depth` peak's own `3N + 2` offset for this exact
  // 0-returning construct, since a live frame's own base entry (pushed
  // before its kind is known) and completion (popped after delivery) both
  // now cost a frame-stack slot at the instant they happen, where the old
  // counter incremented and decremented around only the pair-form's own
  // body.
  CHECK(stats.peak_frame_depth == 3 * (size_t)DeepN + 4);

  FeCloseContext(context);
  free(arena);

  return true;
}

// ---------------------------------------------------------------------------
// Sub-plan 09A's "before" pins: Table X's catchability column, asserted
// instead of described.
//
// Every case below records what this tree does *today*. Genuine arena
// exhaustion and GC-root-stack overflow cannot build a condition object, so
// `FeHandleError`, `RaiseCondition` and `RaiseGcStackOverflow` degrade it to
// nil, and `ConditionMatches` answers false for every *named* handler once
// the object is not a pair -- so only `(t ...)` catches them, and a handler
// that binds the condition binds nil. The same degradation applies to any
// named condition that happens to be raised while the arena is full.
//
// Sub-plan 09B changes exactly these answers, and flips these assertions in
// the same commit that changes the behaviour -- which is what makes them a
// contract rather than a description. Two rows must survive 09B unchanged
// and are pinned here for that reason: Budget stays uncatchable (09A
// Decision 2), and a quit is matched by completion *kind* before anything
// looks at the condition object's shape, so it stays catchable even while
// the object is nil.

// The exhausting body row 1 is about: the chain is a `let`-local, so it is
// unreachable the instant the handler's frame is entered and a handler is
// free to allocate. A chain built into a *global* pins the arena instead,
// which is the handler-re-entry case (09B), not this one.
#define LocalExhaustion "(let ((l nil)) (while t (setq l (cons 1 l))))"

// Small enough that `LocalExhaustion` fills it in well under a second, large
// enough that the reader, the `condition-case` frame and the handler all fit.
enum { PressureArenaExtra = 16 * 1024 };

static ErrorState pressure_state;

// Fills the arena to the point where `ArenaCanAllocate` -- the predicate the
// raise paths themselves consult -- answers false, keeping only the chain's
// head rooted so the GC stack stays flat, and then raises. `raise_quit`
// picks which completion is raised from that state: a host quit (Table X's
// C-g row, whose condition object is built by `RaiseCondition` and so
// degrades like any other) or an ordinary named error (row 3).
[[noreturn]] static void FillArenaThenRaise(FeContext* ctx, bool raise_quit) {
  const size_t gc = FeSaveGC(ctx);
  FeObject* chain = FeNil(ctx);
  while (ArenaCanAllocate(ctx)) {
    chain = FeCons(ctx, FeNil(ctx), chain);
    FeRestoreGC(ctx, gc);
    FePushGC(ctx, chain);
  }
  if (raise_quit) {
    FeRaiseCompletion(ctx, FeCompletionQuit, "evaluation cancelled");
  }
  FeHandleError(ctx, "deliberate failure while full");
}

[[noreturn]] static FeObject* FillThenQuit(
    FeContext* ctx,
    // cppcheck-suppress constParameterCallback
    FeObject* arguments) {
  FeRequireNoArguments(ctx, arguments);
  FillArenaThenRaise(ctx, true);
}

[[noreturn]] static FeObject* FillThenError(
    FeContext* ctx,
    // cppcheck-suppress constParameterCallback
    FeObject* arguments) {
  FeRequireNoArguments(ctx, arguments);
  FillArenaThenRaise(ctx, false);
}

// The condition object the host saw on the last escape, rendered.
static char pressure_escape_condition[64];

// Evaluates `source` in a fresh, deliberately small context and reports what
// happened: either the form completed, and `rendered` holds its value, or the
// completion escaped every handler and reached the host error callback, which
// is what `*escaped` says. `expected_escape` is the message the host must see
// on that path, so an escape for the wrong reason fails rather than passing
// as "did not catch".
static bool EvaluateUnderPressure(const char* source,
                                  const char* expected_escape,
                                  bool* escaped,
                                  char* rendered,
                                  size_t rendered_size) {
  static TestArena storage;
  const size_t size = FeMinimumArenaSize() + PressureArenaExtra;
  CHECK(size <= sizeof(storage.bytes));
  FeContext* const context = FeOpenContext(storage.bytes, size);
  CHECK(context != nullptr);
  pressure_state =
      (ErrorState){.context = context, .expected_message = expected_escape};
  FeSetUserData(context, &pressure_state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "push-roots", PushRootsPastTheLimit);
  FeDefineNative(context, "fill-then-quit", FillThenQuit);
  FeDefineNative(context, "fill-then-error", FillThenError);

  FeContext* const volatile context_v = context;
  char* const volatile rendered_v = rendered;
  const size_t volatile rendered_size_v = rendered_size;
  bool volatile did_escape = false;
  rendered[0] = '\0';
  if (setjmp(pressure_state.jump) == 0) {
    FeObject* const value =
        FeEvaluateString(context_v, "pressure.fe", source, strlen(source));
    (void)FeToString(context_v, value, rendered_v, rendered_size_v);
  } else {
    did_escape = true;
  }
  *escaped = did_escape;
  // What the *host* is handed on the escape path, rendered before the context
  // closes. `fe.h`'s `FeGetCondition` used to promise nil here for "a
  // completion that cannot construct an object, such as arena exhaustion";
  // since 09B that is exactly the case that has a real object, and this is
  // what checks the corrected claim from the C side rather than from Lisp.
  pressure_escape_condition[0] = '\0';
  if (did_escape) {
    (void)FeToString(context_v, FeGetCondition(context_v),
                     pressure_escape_condition,
                     sizeof(pressure_escape_condition));
  }
  const bool reported_as_expected = !did_escape || pressure_state.called;
  FeCloseContext(context_v);
  CHECK(reported_as_expected);
  return true;
}

// `(condition-case VAR BODY (SPEC HANDLER))` must complete and render
// `expected`.
static bool ExpectPressureCaught(const char* source, const char* expected) {
  bool escaped = true;
  char rendered[64];
  CHECK(EvaluateUnderPressure(source, "unused", &escaped, rendered,
                              sizeof(rendered)));
  CHECK(!escaped);
  CHECK(strcmp(rendered, expected) == 0);
  return true;
}

// ... and the same form must reach the host with `expected_escape` when no
// handler matches.
static bool ExpectPressureEscapes(const char* source,
                                  const char* expected_escape) {
  bool escaped = false;
  char rendered[64];
  CHECK(EvaluateUnderPressure(source, expected_escape, &escaped, rendered,
                              sizeof(rendered)));
  CHECK(escaped);
  return true;
}

static bool TestExhaustionCatchability(void) {
  // Row 1 -- arena objects. `(error ...)` catches, the hierarchy's own name
  // for it catches, and `(t ...)` still catches.
  CHECK(ExpectPressureCaught(
      "(condition-case nil " LocalExhaustion " (t (quote caught)))", "caught"));
  CHECK(ExpectPressureCaught("(condition-case nil " LocalExhaustion
                             " (error (quote caught)))",
                             "caught"));
  CHECK(ExpectPressureCaught("(condition-case nil " LocalExhaustion
                             " (arena-exhaustion (quote caught)))",
                             "caught"));
  // The condition object itself, read from Lisp.
  CHECK(ExpectPressureCaught("(condition-case e " LocalExhaustion " (t e))",
                             "(arena-exhaustion)"));
  CHECK(ExpectPressureCaught("(condition-case e " LocalExhaustion " (error e))",
                             "(arena-exhaustion)"));
  // A handler naming an *unrelated* condition still does not match, so the
  // fallback is a real hierarchy answer and not a "catch everything" switch.
  CHECK(ExpectPressureEscapes("(condition-case nil " LocalExhaustion
                              " (arith-error (quote caught)))",
                              "pressure.fe:1: out of memory"));
  // ... and the host reading `FeGetCondition` after that escape sees the
  // object, not the nil `fe.h` used to promise for exactly this case.
  CHECK(strcmp(pressure_escape_condition, "(arena-exhaustion)") == 0);

  // Row 2 -- the GC root stack, provoked directly from a native so the
  // overflow report is what is under test. Its own name is the second
  // pre-built condition, and `error` -- its parent -- catches it too.
  CHECK(
      ExpectPressureCaught("(condition-case nil (push-roots) (t (quote "
                           "caught)))",
                           "caught"));
  CHECK(ExpectPressureCaught(
      "(condition-case nil (push-roots) (error (quote caught)))", "caught"));
  CHECK(
      ExpectPressureCaught("(condition-case nil (push-roots) "
                           "(evaluation-stack-exhaustion (quote caught)))",
                           "caught"));
  CHECK(ExpectPressureCaught("(condition-case e (push-roots) (t e))",
                             "(evaluation-stack-exhaustion)"));
  CHECK(ExpectPressureEscapes(
      "(condition-case nil (push-roots) (arena-exhaustion (quote caught)))",
      "pressure.fe:1: GC stack overflow"));
  CHECK(strcmp(pressure_escape_condition, "(evaluation-stack-exhaustion)") ==
        0);

  // Row 3 -- a *named* raise that happens while the arena is full. The name
  // it asked for cannot be built, so it arrives as the out-of-memory
  // fallback rather than as nil: `(error ...)` catches it and the object
  // says truthfully what it became. The message is still the raise's own.
  CHECK(ExpectPressureCaught("(condition-case e (fill-then-error) (t e))",
                             "(arena-exhaustion)"));
  CHECK(ExpectPressureCaught("(condition-case e (fill-then-error) (error e))",
                             "(arena-exhaustion)"));
  CHECK(ExpectPressureEscapes(
      "(condition-case nil (fill-then-error) (arith-error (quote caught)))",
      "pressure.fe:1: deliberate failure while full"));

  // Row 8 -- quit, raised from the same exhausted state. `ConditionMatches`
  // decides quit by completion kind *before* it looks at the object, so this
  // one is catchable by name while the object stays nil: an interrupt is not
  // an arena exhaustion and must not claim to be one. 09B changed neither
  // half.
  CHECK(ExpectPressureCaught(
      "(condition-case nil (fill-then-quit) (quit (quote caught)))", "caught"));
  CHECK(ExpectPressureCaught("(condition-case e (fill-then-quit) (quit e))",
                             "nil"));
  CHECK(ExpectPressureEscapes(
      "(condition-case nil (fill-then-quit) (error (quote caught)))",
      "pressure.fe:1: evaluation cancelled"));

  // Rows 4-6 -- Budget. `FindConditionHandler` refuses the whole search for
  // a Budget completion, so not even `(t ...)` catches a step-limit wall
  // (09A Decision 2: pinned so 09B asserts it rather than "fixes" it).
  static TestArena budget_storage;
  FeContext* const budget_context =
      FeOpenContext(budget_storage.bytes, sizeof(budget_storage.bytes));
  CHECK(budget_context != nullptr);
  ErrorState budget_state = {.context = budget_context};
  FeSetUserData(budget_context, &budget_state);
  FeSetErrorFn(budget_context, HandleError);
  static const char budget_source[] =
      "(condition-case nil (while t (cons 1 2)) (t (quote caught)))";
  const FeEvalOptions tiny_budget = {.step_limit = 64};
  CHECK(ExpectEvaluationOptionsError(
      budget_context, &budget_state, "budget.fe", budget_source,
      sizeof(budget_source) - 1, &tiny_budget,
      "budget.fe:1: evaluation step limit exceeded"));

  // The step limit is one of three walls that raise Budget, and it was the
  // only one asserted here -- so "Budget stays uncatchable" was pinned for a
  // third of what it claims. The other two, same `(t ...)` handler, same
  // refusal.
  //
  // The frame wall, measured rather than guessed: run the recursion once
  // unrestricted to learn its peak, then again one frame below it, so the
  // wall trips *inside* the `condition-case` whose handler must not run.
  static const char frame_source[] =
      "(do (fset (quote r) (fn (n) (if (= n 0) 0 (r (- n 1))))) "
      "(condition-case nil (r 20) (t (quote caught))))";
  CHECK(IsRendered(budget_context,
                   FeEvaluateString(budget_context, "measure.fe", frame_source,
                                    sizeof(frame_source) - 1),
                   "0"));
  const size_t frame_peak = FeGetArenaStats(budget_context).peak_frame_depth;
  CHECK(frame_peak > 1);
  const FeEvalOptions one_frame_short = {.max_frames = frame_peak - 1};
  CHECK(ExpectEvaluationOptionsError(
      budget_context, &budget_state, "frames.fe", frame_source,
      sizeof(frame_source) - 1, &one_frame_short,
      "frames.fe:1: evaluation frame limit exceeded"));

  // The native re-entry wall: a native that synchronously re-enters
  // `FeCallWithOptions` on itself once more than `max_native_reentry` allows,
  // called from inside the same `(t ...)`.
  FeObject* const reentrant = FeMakeNativeFn(budget_context, ReentrantNative);
  budget_state.reentry_self = FeCreateRoot(budget_context, reentrant);
  CHECK(budget_state.reentry_self != nullptr);
  FeSetFunction(budget_context,
                FeMakeSymbol(budget_context, "reentrant-native"), reentrant);
  budget_state.reentry_remaining = 9;
  budget_state.reentry_max_native_reentry = 8;
  budget_state.reentry_current = 0;
  budget_state.reentry_max_seen = 0;
  static const char reentry_source[] =
      "(condition-case nil (reentrant-native) (t (quote caught)))";
  const FeEvalOptions reentry_wall = {.max_native_reentry = 8};
  CHECK(ExpectEvaluationOptionsError(
      budget_context, &budget_state, "reentry.fe", reentry_source,
      sizeof(reentry_source) - 1, &reentry_wall,
      "reentry.fe:1: native evaluation re-entry limit exceeded"));

  FeCloseContext(budget_context);
  return true;
}

// Sub-plan 09B's handler re-entry rule, pinned rather than invented: when the
// handler that caught an exhaustion hits one of its own, the second raise is
// an ordinary nested raise. It unwinds to the *next* enclosing handler, and
// to the host when none remains.
//
// The body here builds its chain into a **global**, which is the whole
// difference from `TestExhaustionCatchability`: the chain stays rooted while
// the handler runs, so a handler that allocates cannot succeed. A handler
// that allocates nothing still runs to completion in that state, which is
// what the outer clause below relies on.
#define GlobalExhaustion "(do (setq g nil) (while t (setq g (cons 1 g))))"

static bool TestExhaustionHandlerReentry(void) {
  // One handler, and it allocates: nothing is left to catch the re-raise, so
  // it reaches the host -- as an out-of-memory, since that is what the
  // handler hit.
  CHECK(ExpectPressureEscapes("(condition-case nil " GlobalExhaustion
                              " (error (cons 1 2)))",
                              "pressure.fe:1: out of memory"));
  // Two handlers: the inner one re-raises out of itself and the outer one
  // takes it. The outer handler allocates nothing, which is exactly why it
  // can still run against a permanently full arena.
  CHECK(ExpectPressureCaught(
      "(condition-case nil (condition-case nil " GlobalExhaustion
      " (error (cons 1 2))) "
      "(error (quote outer)))",
      "outer"));
  // The outer handler matches it *by name*, which is how this pins that the
  // re-raise carries the pre-built exhaustion condition rather than a nil
  // object: a nil object would fall through `arena-exhaustion` and escape.
  // (The condition cannot be read into a variable here -- binding one is an
  // allocation, and this arena is pinned by design -- so the name is the
  // observation.)
  CHECK(ExpectPressureCaught(
      "(condition-case nil (condition-case nil " GlobalExhaustion
      " (error (cons 1 2))) "
      "(arena-exhaustion (quote outer-by-name)))",
      "outer-by-name"));
  // A handler that allocates nothing catches at the first level even with the
  // arena permanently pinned: the re-entry rule is about handlers that
  // allocate, not about handlers as such.
  CHECK(ExpectPressureCaught("(condition-case nil " GlobalExhaustion
                             " (error (quote caught)))",
                             "caught"));
  return true;
}

// Sub-plan 09B: a context that has *caught* an exhaustion is a working
// context. The pre-built conditions are permanent roots, so signalling one
// costs no slots; the run's own data is reclaimed by the collection the
// unwind makes possible; and the same context catches the next exhaustion
// exactly as it caught the first.
static bool TestCaughtExhaustionSession(void) {
  static TestArena storage;
  const size_t size = FeMinimumArenaSize() + PressureArenaExtra;
  CHECK(size <= sizeof(storage.bytes));
  FeContext* const context = FeOpenContext(storage.bytes, size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context, .expected_message = "unused"};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  const FeArenaStats before = FeGetArenaStats(context);
  CHECK(before.allocation_failures == 0);

  static const char caught[] =
      "(condition-case e " LocalExhaustion " (error e))";
  for (size_t round = 0; round < 3; round++) {
    CHECK(IsRendered(
        context,
        FeEvaluateString(context, "session.fe", caught, sizeof(caught) - 1),
        "(arena-exhaustion)"));
    const FeArenaStats after = FeGetArenaStats(context);
    // Every round really did exhaust the arena, and every round handed the
    // slots back: the chain was unreachable the moment the handler ran.
    CHECK(after.allocation_failures == round + 1);
    CHECK(after.total_slots == before.total_slots);
    CHECK(after.free_slots > 0);
    // Ordinary evaluation in between, including one that forces collections
    // of its own.
    CHECK(IsRendered(
        context,
        FeEvaluateString(context, "after.fe", "(+ 1 2)", sizeof("(+ 1 2)") - 1),
        "3"));
    static const char collecting[] =
        "(let ((n 0)) (while (< n 2000) (setq n (+ n 1)) (cons n n)) n)";
    CHECK(IsRendered(context,
                     FeEvaluateString(context, "collect.fe", collecting,
                                      sizeof(collecting) - 1),
                     "2000"));
    CHECK(FeGetArenaStats(context).collection_count > after.collection_count);
  }

  // The pre-built conditions survived every one of those collections: they
  // are roots in `CollectGarbage`, not objects that happened to be reachable
  // from the last raise.
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "final.fe", caught, sizeof(caught) - 1),
      "(arena-exhaustion)"));
  FeCloseContext(context);
  return true;
}

// Measured on the *recursive* mark phase, immediately before 09C replaced it,
// with `TestCollectionStatsPinned`'s own fixed corpus and fixed arena. The
// rewrite is invisible to every one of them.
//
// Re-measured at sub-plan 10B, which is the one thing that legitimately
// moves two of these four: the arena here is `FeMinimumArenaSize() + 256K`,
// and three new primitives (`macroexpand-1`, `macroexpand`,
// `macroexpand-all`) grow the *minimum* by 352 bytes -- their symbols,
// names, cells and primitive objects -- so the arena grows with it and
// holds 22 more slots (15210 -> 15232), of which exactly the same 22 are
// live after a collection (1027 -> 1049). The two figures the walk itself
// decides -- how many times it collected, and that its peak is the whole
// arena -- are unchanged, which is the invariance 09C pinned.
//
// Re-measured again at sub-plan 11B for exactly the same reason: two more
// primitives (`internal--mark-special`, `special-variable-p`) grow
// `FeMinimumArenaSize()`, so the arena grows with it and holds 17 more slots
// (15232 -> 15249), of which the same 17 are live after a collection
// (1049 -> 1066). The collection count and the peak are again unchanged.
// Nothing in the corpus marks a symbol special, so `special_list` is empty
// here and the registry costs nothing: these two numbers move because the
// *primitive table* grew, not because dynamic binding did anything.
enum {
  PinnedTotalSlots = 15249,
  PinnedCollectionCount = 3,
  PinnedPeakLive = 15249,
  PinnedLiveAfterCollection = 1066,
};

// ---------------------------------------------------------------------------
// Sub-plan 09C: the mark phase's C stack, and the shapes that used to reach
// past it.
//
// `FeSetMarkFn` is called from inside the mark phase, for `ptr`/`fex` objects
// only, which makes it the one place a test can stand *inside* a collection
// and read the collector's own C-stack high-water mark -- and exercising it
// here is also this slice's "Fex's host mark callback still works" case, the
// path `fe -d` installs.
static size_t mark_probe_calls;
static void* const mark_probe_payload = (void*)&mark_probe_calls;

static FeObject* MarkStackProbe(FeContext* ctx,
                                // cppcheck-suppress constParameterCallback
                                FeObject* obj) {
  // Reading the object it was handed is the one thing a mark callback is
  // allowed to do to the graph while the walk is inside it (`doc/c-api.md`),
  // and it is what makes this a count of *host pointer* marks rather than of
  // callbacks in general.
  mark_probe_calls += FeGetType(obj) == FeTPtr ? 1 : 0;
  RecordStackProbeAddress();
  return FeNil(ctx);
}

// `depth` levels of `car` nesting with the host `ptr` object at the bottom:
// `(((...PTR...)))`, every `cdr` nil. Only the head stays rooted, so the GC
// stack is flat however deep the chain is -- the chain roots itself.
static FeObject* BuildCarChain(FeContext* ctx, size_t depth) {
  const size_t gc = FeSaveGC(ctx);
  FeObject* node = FeMakePtr(ctx, FeTPtr, mark_probe_payload);
  for (size_t i = 0; i < depth; i++) {
    node = FeCons(ctx, node, FeNil(ctx));
    FeRestoreGC(ctx, gc);
    FePushGC(ctx, node);
  }
  return node;
}

// `depth` cons cells down the `cdr` spine, the shape the old walk already
// handled iteratively, with the `ptr` object as the last element's car so the
// probe fires at the far end of the spine too.
static FeObject* BuildCdrChain(FeContext* ctx, size_t depth) {
  const size_t gc = FeSaveGC(ctx);
  FeObject* node =
      FeCons(ctx, FeMakePtr(ctx, FeTPtr, mark_probe_payload), FeNil(ctx));
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, node);
  for (size_t i = 0; i < depth; i++) {
    node = FeCons(ctx, FeNil(ctx), node);
    FeRestoreGC(ctx, gc);
    FePushGC(ctx, node);
  }
  return node;
}

// Alternating: each level nests one deeper in `car` *and* carries a second
// pair in `cdr`, so neither edge is a straight line and the walk has to
// interleave both halves of every pair.
static FeObject* BuildAlternatingChain(FeContext* ctx, size_t depth) {
  const size_t gc = FeSaveGC(ctx);
  FeObject* node = FeMakePtr(ctx, FeTPtr, mark_probe_payload);
  for (size_t i = 0; i < depth; i++) {
    node = FeCons(ctx, node, FeCons(ctx, FeNil(ctx), FeNil(ctx)));
    FeRestoreGC(ctx, gc);
    FePushGC(ctx, node);
  }
  return node;
}

// Consing garbage until the collector runs, without leaving anything behind
// for it to keep: whatever the caller had rooted before the call stays rooted.
static bool ForceCollection(FeContext* ctx) {
  const size_t before = FeGetArenaStats(ctx).collection_count;
  const size_t gc = FeSaveGC(ctx);
  for (size_t i = 0; i < 100000000; i++) {
    if (FeGetArenaStats(ctx).collection_count != before) {
      return true;
    }
    (void)FeCons(ctx, FeNil(ctx), FeNil(ctx));
    FeRestoreGC(ctx, gc);
  }
  return false;
}

// Walks the `car` spine back down, checking every level is a pair with a nil
// `cdr` and that the bottom is the host pointer the chain was built around.
// This is the strongest statement the tests make about pointer reversal: the
// collector wrote a return path through every one of these cells and had to
// put all of it back.
static bool IsCarChainIntact(FeContext* ctx, FeObject* node, size_t depth) {
  for (size_t i = 0; i < depth; i++) {
    CHECK(FeGetType(node) == FeTPair);
    CHECK(FeIsNil(FeCdr(ctx, node)));
    node = FeCar(ctx, node);
  }
  CHECK(FeGetType(node) == FeTPtr);
  CHECK(FeToPtr(ctx, node) == mark_probe_payload);
  return true;
}

enum { MarkProbeArenaSize = 64u << 20 };

// One measurement: a fresh context, a `car` chain `depth` levels deep, the
// probe state cleared, one forced collection, and the C-stack delta the mark
// phase reached against `baseline` -- which is the same probe fired by the
// same callback from the same collector, with no chain above it at all.
static bool MeasureMarkDepth(unsigned char* storage,
                             size_t depth,
                             uintptr_t* address,
                             size_t* peak_live) {
  FeContext* const ctx = FeOpenContext(storage, MarkProbeArenaSize);
  CHECK(ctx != nullptr);
  ErrorState state = {.context = ctx, .expected_message = "unused"};
  FeSetUserData(ctx, &state);
  FeSetErrorFn(ctx, HandleError);
  FeSetMarkFn(ctx, MarkStackProbe);

  FeObject* const chain = BuildCarChain(ctx, depth);
  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  mark_probe_calls = 0;
  CHECK(ForceCollection(ctx));
  CHECK(mark_probe_calls > 0);
  CHECK(stack_probe_last_address != 0);
  CHECK(IsCarChainIntact(ctx, chain, depth));
  *address = stack_probe_deepest_address;
  *peak_live = FeGetArenaStats(ctx).peak_live_objects;
  FeCloseContext(ctx);
  return true;
}

// The 03E stack-probe convention, transferred from the evaluator to the
// collector: the same assertion shape as `TestEvaluationStackProbe`, the same
// 2 KiB tolerance, at the same N = 10 / 1 000 / 100 000.
//
// Before this slice the answer was linear, not flat. `FeMark` recursed once
// per `car` level, so a chain deep enough took the process down inside the
// collector -- and unlike the evaluator's old wall there was no counter in
// front of it: the only bound was the host's stack limit, and the only
// symptom was SIGSEGV.
static bool TestMarkStackProbe(void) {
  unsigned char* const storage = malloc(MarkProbeArenaSize);
  CHECK(storage != nullptr);

  uintptr_t baseline = 0;
  size_t baseline_live = 0;
  CHECK(MeasureMarkDepth(storage, 0, &baseline, &baseline_live));

  static const size_t depths[] = {10, 1000, 100000};
  for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
    uintptr_t probed = 0;
    size_t peak_live = 0;
    CHECK(MeasureMarkDepth(storage, depths[i], &probed, &peak_live));
    const uintptr_t delta =
        probed > baseline ? probed - baseline : baseline - probed;
    printf("mark probe: n=%6zu baseline=%#" PRIxPTR " probe=%#" PRIxPTR
           " delta=%" PRIuPTR " bytes, peak_live=%zu\n",
           depths[i], baseline, probed, delta, peak_live);
    // Flat. The same measured tolerance 03A/03D/03E/03F assert for the
    // evaluator's own probe, under default and sanitizer builds alike: a
    // regression that sent the mark phase back through a recursive C call
    // would miss this by megabytes at n=100000, not by bytes of noise.
    CHECK(delta < 2048);
    // `peak_live` is printed, not asserted on. It used to carry a
    // `CHECK(peak_live > depths[i])` described as proving "the run really was
    // that deep" -- which it never did: `ForceCollection` allocates until the
    // arena collects, so this is the slot count of the arena for every run,
    // n=0 included (3772113 here at all four depths). What actually witnesses
    // the depth is inside `MeasureMarkDepth`: the callback fires only from the
    // `ptr` object at the *bottom* of the chain (`CHECK(mark_probe_calls >
    // 0)`), and `IsCarChainIntact` then walks all `depths[i]` levels back down
    // and checks the pointer at the end.
    (void)peak_live;
  }

  free(storage);
  return true;
}

// Sub-plan 09C's mark-phase contract, both halves (Phase 9 fix-brief F1).
//
// Pointer reversal keeps the walk's return path inside the graph it is
// walking, so a `longjmp` or a raise out of `mark_fn` abandons the arena
// half-reversed: measured on a 2 000-level `car` chain against the tree
// before this fix, the recovered process read a tagged parent pointer out of
// the first cell and took SIGSEGV. The recursive walk this replaced only ever
// set mark bits, so a jump past it left a valid heap and the rule never had
// to exist. It exists now, and this pins it.

static jmp_buf mark_raise_recovery;

// The host behaviour the contract forbids: recover from the raise and carry
// on. If fe ever lets one through, the child reaches this and reports it as a
// distinct exit status rather than as a crash somewhere later.
[[noreturn]] static void MarkRaiseErrorFn(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    const char* message,
    // cppcheck-suppress constParameterCallback
    FeObject* stack) {
  (void)context;
  (void)message;
  (void)stack;
  longjmp(mark_raise_recovery, 1);
}

// Two ways for a callback to break the contract, and they are caught by two
// different guards.
//
// `MarkRaiseAllocating` is the ordinary shape: `FeHandleError` builds a
// condition object, so it asks `ArenaCanAllocate` first -- and the free list
// is empty, because running out of it is what started this collection. That
// re-enters `CollectGarbage`, whose sweep would clear the mark bits the outer
// walk is relying on, so the nested-collection guard stops it.
//
// `MarkRaiseNonAllocating` reaches the raise guard itself: a Budget
// completion carries no condition object, so nothing on the way to
// `RaiseCompletionCore` allocates and the collection is never re-entered.
enum { MarkRaiseAllocating = 0, MarkRaiseNonAllocating = 1 };

static int mark_raise_mode;

static FeObject* MarkRaiseProbe(FeContext* context, FeObject* object) {
  (void)object;
  mark_probe_calls++;
  if (mark_raise_mode == MarkRaiseNonAllocating) {
    FeRaiseCompletion(context, FeCompletionBudget, "host wall from mark_fn");
  }
  FeHandleError(context, "host bail-out from mark_fn");
}

enum {
  MarkRaiseChainDepth = 2000,
  MarkRaiseNoAbort = 80,
  MarkRaiseNoCollection = 81,
  MarkRaiseSetupFailed = 82,
};

// The forked half. Never returns: fe has to abort before it can.
[[noreturn]] static void RunMarkRaiseChild(int mode) {
  unsigned char* const storage = malloc(MarkProbeArenaSize);
  if (storage == nullptr) {
    _exit(MarkRaiseSetupFailed);
  }
  FeContext* const context = FeOpenContext(storage, MarkProbeArenaSize);
  if (context == nullptr) {
    _exit(MarkRaiseSetupFailed);
  }
  FeSetErrorFn(context, MarkRaiseErrorFn);
  if (setjmp(mark_raise_recovery) != 0) {
    _exit(MarkRaiseNoAbort);
  }
  FeObject* const chain = BuildCarChain(context, MarkRaiseChainDepth);
  if (FeCreateRoot(context, chain) == nullptr) {
    _exit(MarkRaiseSetupFailed);
  }
  FeRestoreGC(context, 0);
  mark_probe_calls = 0;
  mark_raise_mode = mode;
  FeSetMarkFn(context, MarkRaiseProbe);
  (void)ForceCollection(context);
  _exit(MarkRaiseNoCollection);
}

// Forks one child with its stderr on a pipe and collects what it said and how
// it died. No `CHECK` between the pipe opening and both ends closing: a
// `CHECK` returns, and `-fanalyzer` is right that a return with a descriptor
// still open is a leak.
static bool RunMarkRaiseCapture(int mode,
                                char* captured,
                                size_t captured_size,
                                int* status) {
  int pipe_fds[2] = {-1, -1};
  CHECK(pipe(pipe_fds) == 0);
  fflush(stdout);
  fflush(stderr);
  const pid_t child = fork();
  if (child == 0) {
    (void)close(pipe_fds[0]);
    (void)dup2(pipe_fds[1], STDERR_FILENO);
    (void)close(pipe_fds[1]);
    RunMarkRaiseChild(mode);
  }
  (void)close(pipe_fds[1]);
  captured[0] = '\0';
  if (child > 0) {
    size_t used = 0;
    while (used + 1 < captured_size) {
      const ssize_t got =
          read(pipe_fds[0], captured + used, captured_size - 1 - used);
      if (got <= 0) {
        break;
      }
      used += (size_t)got;
    }
    captured[used] = '\0';
  }
  (void)close(pipe_fds[0]);
  CHECK(child > 0);
  CHECK(waitpid(child, status, 0) == child);
  return true;
}

// Runs one child and asserts how it died, so the diagnostic is evidence
// rather than noise in the suite's own output.
static bool CheckMarkRaiseAborts(int mode, const char* expected_detail) {
  char captured[512];
  int status = 0;
  CHECK(RunMarkRaiseCapture(mode, captured, sizeof(captured), &status));
  printf("mark raise (mode %d): signalled=%d signal=%d exit=%d\n", mode,
         WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : -1,
         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  // A clean, named abort. Not a SIGSEGV -- which is what the tree before this
  // fix did, once the recovered process read the reversed chain back -- and
  // not a normal return, which would mean the raise was honoured and the
  // arena left half-reversed.
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
  // And it says which contract broke, not merely that something did.
  CHECK(strstr(captured, expected_detail) != nullptr);
  CHECK(strstr(captured, "FeSetMarkFn") != nullptr);
  return true;
}

static bool TestMarkRaiseIsFatal(void) {
  CHECK(CheckMarkRaiseAborts(MarkRaiseAllocating,
                             "fe: fatal: a callback allocated"));
  CHECK(CheckMarkRaiseAborts(MarkRaiseNonAllocating,
                             "fe: fatal: a completion was raised"));
  return true;
}

// The contract's other half: the *in-tree* route to that abort is closed.
// `main.c`'s `mark`/`gc` tracers print the object they were handed, printing
// charges the evaluation step budget and polls the interrupt, and either of
// those raises -- so before this fix a step limit or a C-g arriving during a
// collection turned fe's own `-d` tracing into the fatal path above. The
// writer does not charge while collecting, so the collection here finishes
// and the cancellation is delivered afterwards, as an ordinary catchable quit
// at the next evaluator step.
static bool mark_print_armed;
static size_t mark_print_polls;

static bool MarkPrintInterrupt(FeContext* context, void* userdata) {
  (void)context;
  (void)userdata;
  mark_print_polls++;
  return mark_print_armed;
}

static FeObject* MarkPrintProbe(FeContext* context, FeObject* object) {
  mark_probe_calls++;
  // Anything that polls from here on cancels. If `WriteObject` still charged
  // a step while collecting, the poll below would raise from inside the mark
  // phase and `RaiseCompletionCore` would abort this process.
  mark_print_armed = true;
  char rendered[64];
  (void)FeToString(context, object, rendered, sizeof(rendered));
  return FeNil(context);
}

static bool TestMarkPrintingCallbackIsSafe(void) {
  enum { PrintProbeArenaSize = 512u << 10 };
  unsigned char* const storage = malloc(PrintProbeArenaSize);
  CHECK(storage != nullptr);
  FeContext* const context = FeOpenContext(storage, PrintProbeArenaSize);
  CHECK(context != nullptr);
  ErrorState state = {.context = context,
                      .expected_message = "mark-print:1: evaluation cancelled"};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  FeObject* const chain = BuildCarChain(context, 64);
  CHECK(FeCreateRoot(context, chain) != nullptr);
  FeRestoreGC(context, 0);
  mark_probe_calls = 0;
  mark_print_polls = 0;
  mark_print_armed = false;
  FeSetMarkFn(context, MarkPrintProbe);

  // A loop that allocates until the 512 KiB arena collects, several times
  // over. `poll_interval` of 1 means the very next evaluator step after the
  // callback arms the interrupt delivers the cancellation.
  static const char source[] =
      "(let ((i 0)) (while (< i 100000) (do (setq i (+ i 1)) (cons i i))))";
  const FeEvalOptions options = {.poll_interval = 1,
                                 .interrupt = MarkPrintInterrupt};
  if (setjmp(state.jump) == 0) {
    (void)FeEvaluateStringWithOptions(context, "mark-print", source,
                                      sizeof(source) - 1, &options);
    CHECK(false);
  }
  printf("mark print: mark_calls=%zu polls=%zu completion=%d\n",
         mark_probe_calls, mark_print_polls, (int)state.observed_completion);
  // The callback really did run and really did print from inside the walk...
  CHECK(mark_probe_calls > 0);
  // ... the process survived it, the cancellation arrived as an ordinary
  // quit at the next evaluator step, ...
  CHECK(state.called);
  CHECK(state.observed_completion == FeCompletionQuit);
  // ... and the chain the walk reversed on the way past is whole.
  CHECK(IsCarChainIntact(context, chain, 64));

  FeCloseContext(context);
  free(storage);
  return true;
}

// The shapes the walk has to get right, each built, collected through, and
// then read back: a deep `car` spine, a deep `cdr` spine, an alternating
// structure that uses both halves of every pair, and four cyclic graphs --
// the case a mark phase must terminate on rather than merely survive.
static bool TestMarkGraphShapes(void) {
  enum { ShapeDepth = 50000, CycleNodes = 3 };
  unsigned char* const storage = malloc(MarkProbeArenaSize);
  CHECK(storage != nullptr);
  FeContext* const ctx = FeOpenContext(storage, MarkProbeArenaSize);
  CHECK(ctx != nullptr);
  ErrorState state = {.context = ctx, .expected_message = "unused"};
  FeSetUserData(ctx, &state);
  FeSetErrorFn(ctx, HandleError);
  FeSetMarkFn(ctx, MarkStackProbe);
  const size_t gc = FeSaveGC(ctx);

  FeObject* const car_chain = BuildCarChain(ctx, ShapeDepth);
  FePushGC(ctx, car_chain);
  FeObject* const cdr_chain = BuildCdrChain(ctx, ShapeDepth);
  FePushGC(ctx, cdr_chain);
  FeObject* const alternating = BuildAlternatingChain(ctx, ShapeDepth);
  FePushGC(ctx, alternating);

  // The cycles. `test_api.c` reaches through `fe_internal.h`'s `CAR`/`CDR`
  // for these because Fe has no C spelling of `setcar`/`setcdr` and the
  // point is to build the graph, not to evaluate one: a pair whose car is
  // itself, one whose cdr is itself, one that is both, and a three-node ring
  // whose entry also carries the `ptr` object, so the host callback fires
  // from inside a cycle.
  FeObject* const self_car = FeCons(ctx, FeNil(ctx), FeNil(ctx));
  FePushGC(ctx, self_car);
  CAR(self_car) = self_car;
  FeObject* const self_cdr = FeCons(ctx, FeNil(ctx), FeNil(ctx));
  FePushGC(ctx, self_cdr);
  CDR(self_cdr) = self_cdr;
  FeObject* const self_both = FeCons(ctx, FeNil(ctx), FeNil(ctx));
  FePushGC(ctx, self_both);
  CAR(self_both) = self_both;
  CDR(self_both) = self_both;

  FeObject* ring =
      FeCons(ctx, FeMakePtr(ctx, FeTPtr, mark_probe_payload), FeNil(ctx));
  FePushGC(ctx, ring);
  FeObject* const ring_head = ring;
  for (size_t i = 1; i < CycleNodes; i++) {
    ring = FeCons(ctx, ring_head, ring);
    FePushGC(ctx, ring);
  }
  CDR(ring_head) = ring;

  mark_probe_calls = 0;
  CHECK(ForceCollection(ctx));
  // Three `ptr` objects are live (the three chains' bottoms plus the ring's),
  // and the collector reached every one of them without looping.
  CHECK(mark_probe_calls >= 4);

  // Everything is still exactly what it was built as.
  CHECK(IsCarChainIntact(ctx, car_chain, ShapeDepth));
  FeObject* spine = cdr_chain;
  for (size_t i = 0; i < ShapeDepth; i++) {
    CHECK(FeGetType(spine) == FeTPair);
    CHECK(FeIsNil(FeCar(ctx, spine)));
    spine = FeCdr(ctx, spine);
  }
  CHECK(FeGetType(FeCar(ctx, spine)) == FeTPtr);
  FeObject* alt = alternating;
  for (size_t i = 0; i < ShapeDepth; i++) {
    CHECK(FeGetType(alt) == FeTPair);
    CHECK(FeGetType(FeCdr(ctx, alt)) == FeTPair);
    CHECK(FeIsNil(FeCar(ctx, FeCdr(ctx, alt))));
    alt = FeCar(ctx, alt);
  }
  CHECK(FeGetType(alt) == FeTPtr);
  CHECK(FeCar(ctx, self_car) == self_car);
  CHECK(FeIsNil(FeCdr(ctx, self_car)));
  CHECK(FeCdr(ctx, self_cdr) == self_cdr);
  CHECK(FeIsNil(FeCar(ctx, self_cdr)));
  CHECK(FeCar(ctx, self_both) == self_both);
  CHECK(FeCdr(ctx, self_both) == self_both);
  CHECK(FeCdr(ctx, ring_head) == ring);
  CHECK(FeGetType(FeCar(ctx, ring_head)) == FeTPtr);

  // A second collection over the same graphs, now that the first one has run
  // its sweep: the tags it restored have to be good enough to walk again.
  CHECK(ForceCollection(ctx));
  CHECK(IsCarChainIntact(ctx, car_chain, ShapeDepth));
  CHECK(FeCar(ctx, self_both) == self_both);
  CHECK(FeCdr(ctx, ring_head) == ring);

  // And dropping the roots really does free all of it, so the walk marked
  // what was reachable rather than everything it touched.
  const size_t live_before =
      FeGetArenaStats(ctx).total_slots - FeGetArenaStats(ctx).free_slots;
  FeRestoreGC(ctx, gc);
  CHECK(ForceCollection(ctx));
  const size_t live_after =
      FeGetArenaStats(ctx).total_slots - FeGetArenaStats(ctx).free_slots;
  CHECK(live_after < live_before / 2);

  FeCloseContext(ctx);
  free(storage);
  return true;
}

// 09C's "invisible except to the C stack": a fixed corpus in a fixed arena
// has to produce exactly the same collector figures after the rewrite as
// before it. The numbers below are literals measured on the *recursive* walk,
// in the commit that replaced it; a range would not have caught a new walk
// that marked one object too many or collected one time too few.
static bool TestCollectionStatsPinned(void) {
  static TestArena storage;
  const size_t size = FeMinimumArenaSize() + 256 * 1024;
  CHECK(size <= sizeof(storage.bytes));
  FeContext* const ctx = FeOpenContext(storage.bytes, size);
  CHECK(ctx != nullptr);
  ErrorState state = {.context = ctx, .expected_message = "unused"};
  FeSetUserData(ctx, &state);
  FeSetErrorFn(ctx, HandleError);

  // Pairs, deep recursion, strings, symbols, closures and a macro -- every
  // arm of the mark switch except the host-pointer one, which
  // `TestMarkGraphShapes` owns.
  static const char corpus[] =
      "(do"
      " (fset 'build (lambda (n acc)"
      "   (if (<= n 0) acc (build (- n 1) (cons (cons n \"cell\") acc)))))"
      " (fset 'twice (macro (f x) (list f (list f x))))"
      " (setq deep (build 150 nil))"
      " (setq tagged (list 'a 'b 'c \"a string long enough to span cells\"))"
      " (setq counted 0)"
      " (while (< counted 8000) (setq counted (+ counted 1))"
      "   (cons counted (cons \"cell\" nil)))"
      " (twice car (cons (cons 7 nil) nil)))";
  CHECK(IsRendered(
      ctx, FeEvaluateString(ctx, "stats.fe", corpus, sizeof(corpus) - 1), "7"));

  // One more collection with only the corpus's own globals reachable, so the
  // live count read below is what the mark phase decided to keep -- the
  // figure a walk that marked one object too many, or reached one edge too
  // few, would move. `peak_live_objects` cannot be that figure: `MakeObject`
  // only collects once the free list is empty, so any run that collects at
  // all peaks at `total_slots` by construction.
  CHECK(ForceCollection(ctx));
  const FeArenaStats stats = FeGetArenaStats(ctx);
  const size_t live = stats.total_slots - stats.free_slots;
  printf("collection stats: total=%zu collections=%zu peak_live=%zu live=%zu\n",
         stats.total_slots, stats.collection_count, stats.peak_live_objects,
         live);
  CHECK(stats.total_slots == PinnedTotalSlots);
  CHECK(stats.collection_count == PinnedCollectionCount);
  CHECK(stats.peak_live_objects == PinnedPeakLive);
  CHECK(live == PinnedLiveAfterCollection);
  CHECK(stats.allocation_failures == 0);
  FeCloseContext(ctx);
  return true;
}

// Sub-plan 11B of kg's Emacs-subset program: special variables and shallow
// dynamic binding. The semantics grid itself -- the twelve rows sub-plan 11A
// measured on Emacs 31.0.90, and the A2b/A4/A13 guards -- is
// `scripts/special-variables.fe`, which can spell all of it in Lisp. What is
// here is everything that cannot be: the five completion kinds a binding must
// survive (two of which, Quit and Budget, have no Lisp spelling at all), a
// collection running while bindings are live, and the registry's own C-level
// state.
static FeObject* CollectNow(FeContext* context,
                            // cppcheck-suppress constParameterCallback
                            FeObject* arguments) {
  (void)arguments;
  return FeMakeBool(context, ForceCollection(context));
}

static bool TestDynamicBinding(void) {
  static TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "contain-call", ContainCall);
  FeDefineNative(context, "raise-host-quit", RaiseHostQuit);
  FeDefineNative(context, "raise-host-budget", RaiseHostBudget);
  FeDefineNative(context, "collect-now", CollectNow);

#define CHK(expr, expected)                                                 \
  CHECK(IsRendered(                                                         \
      context, FeEvaluateString(context, "dyn.fe", expr, sizeof(expr) - 1), \
      expected))

  // The registry, read through the same predicates the binding paths use.
  // Nothing is marked until `internal--mark-special` says so, and the two
  // flags are separate: a let-dynamic-only mark (Emacs' one-arg `defvar`)
  // binds dynamically while `special-variable-p` still answers nil.
  FeObject* const full = FeMakeSymbol(context, "kv");
  FeObject* const half = FeMakeSymbol(context, "hv");
  CHECK(!SymbolIsSpecial(context, full) && !SymbolIsLetDynamic(context, full));
  CHK("(internal--mark-special 'kv t)", "kv");
  CHK("(internal--mark-special 'hv nil)", "hv");
  CHECK(SymbolIsSpecial(context, full) && SymbolIsLetDynamic(context, full));
  CHECK(!SymbolIsSpecial(context, half) && SymbolIsLetDynamic(context, half));
  // Marking is idempotent and one-way, so a second full mark neither adds a
  // registry entry nor a demotion path exists to undo one.
  CHK("(internal--mark-special 'kv t)", "kv");
  CHK("(internal--mark-special 'kv nil)", "kv");
  CHECK(SymbolIsSpecial(context, full));

  CHK("(setq kv 'global)", "global");
  const size_t cleanups = context->cleanup_stack_index;

  // 1/5 normal. The binding is in force for the body and gone on return --
  // stated here as the control the other four are measured against.
  CHK("(contain-call (lambda () (let ((kv 'inner)) (symbol-value 'kv))))",
      "(ok inner)");
  CHK("(symbol-value 'kv)", "global");

  // 2/5 error, 3/5 throw, 4/5 quit, 5/5 budget. Each is raised from inside a
  // live binding and contained by `FeTryCallWithOptions`, so the global value
  // is asserted after the completion has already been swallowed -- the exact
  // shape kg's hook dispatch and process callbacks have.
  CHK("(contain-call (lambda () (let ((kv 'inner)) (car 5))))",
      "(contained (wrong-type-argument listp 5))");
  CHECK(contained_kind == FeCompletionError);
  CHK("(symbol-value 'kv)", "global");

  CHK("(contain-call (lambda () (let ((kv 'inner)) (throw 'nowhere 1))))",
      "(contained (no-catch nowhere 1))");
  CHECK(contained_kind == FeCompletionError);
  CHK("(symbol-value 'kv)", "global");

  CHK("(contain-call (lambda () (let ((kv 'inner)) (raise-host-quit))))",
      "(contained (quit))");
  CHECK(contained_kind == FeCompletionQuit);
  CHK("(symbol-value 'kv)", "global");

  CHK("(contain-call (lambda () (let ((kv 'inner)) (raise-host-budget))))",
      "(contained nil)");
  CHECK(contained_kind == FeCompletionBudget);
  CHK("(symbol-value 'kv)", "global");

  // Every one of the five left the cleanup registry exactly as it found it:
  // an entry that was pushed and never drained would show up here as a leak
  // and, later, as a restore performed at the wrong depth.
  CHECK(context->cleanup_stack_index == cleanups);

  // The same for the sequential two-argument `let` (kg's `internal--let`),
  // whose binding belongs to the enclosing body rather than to its own form.
  CHK("(contain-call (lambda () (do (let kv 'inner) (car 5))))",
      "(contained (wrong-type-argument listp 5))");
  CHK("(symbol-value 'kv)", "global");
  CHECK(context->cleanup_stack_index == cleanups);

  // A budget that expires on its own, rather than a host raising one: the
  // binding is live when the last step is spent, and the value cell is back
  // by the time the host's recovery `longjmp` has landed.
  static const char runaway[] = "(let ((kv 'inner)) (while t (setq kv 'spun)))";
  const FeEvalOptions budget = {.step_limit = 200};
  CHECK(ExpectCompletionKind(
      context, &state, "dyn.fe", runaway, sizeof(runaway) - 1, &budget,
      "dyn.fe:1: evaluation step limit exceeded", FeCompletionBudget));
  CHK("(symbol-value 'kv)", "global");
  CHECK(context->cleanup_stack_index == cleanups);

  // Unboundness is a value like any other here (row A10a): a `let` over an
  // unbound special leaves it unbound again, on the abnormal path too.
  CHK("(internal--mark-special 'uv nil)", "uv");
  CHK("(boundp 'uv)", "nil");
  CHK("(contain-call (lambda () (let ((uv 1)) (list (boundp 'uv) (car 5)))))",
      "(contained (wrong-type-argument listp 5))");
  CHK("(boundp 'uv)", "nil");

  // A collection while two dynamic bindings are live. The shadowed globals
  // are reachable from nowhere but the cleanup entries holding them -- that
  // is what shallow binding means -- so a mark phase that did not walk those
  // entries would sweep them, and the restore below would write a swept
  // object back into a value cell. The strings are long enough to span
  // several cells, so a partial sweep shows up as a wrong answer rather than
  // as a coincidence.
  static const char setup[] =
      "(do (internal--mark-special 'g1 t) (internal--mark-special 'g2 t)"
      "    (setq g1 (list 'one \"a string long enough to span several cells\"))"
      "    (setq g2 (list 'two \"another string that also spans cells\"))"
      "    'ready)";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "dyn.fe", setup, sizeof(setup) - 1),
      "ready"));
  static const char under_gc[] =
      "(let ((g1 'shadow-one) (g2 'shadow-two))"
      "  (list (collect-now) g1 g2))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "dyn.fe", under_gc, sizeof(under_gc) - 1),
      "(t shadow-one shadow-two)"));
  CHK("g1", "(one \"a string long enough to span several cells\")");
  CHK("g2", "(two \"another string that also spans cells\")");

  // And the same with the collection happening on the *abnormal* path, after
  // the bindings are in force and before the drain that restores them.
  CHK("(contain-call (lambda () (let ((g1 'x) (g2 'y)) (collect-now)"
      "                            (car 5))))",
      "(contained (wrong-type-argument listp 5))");
  CHK("g1", "(one \"a string long enough to span several cells\")");
  CHK("g2", "(two \"another string that also spans cells\")");

#undef CHK

  FeCloseContext(context);
  return true;
}

int main(void) {
  return TestContextCreation() && TestUserDataAndErrors() &&
                 TestStringInput() && TestReaderLiterals() && TestFileInput() &&
                 TestEvaluationControl() && TestCompletionKinds() &&
                 TestExtensionAPI() && TestRootsAndCalls() &&
                 TestCallWithOptions() && TestMathNatives() &&
                 TestSerialization() && TestDottedLists() &&
                 TestMacroExpansion() && TestMacroexpandPrimitives() &&
                 TestMacroexpandBudget() && TestMacroexpandUnderCollection() &&
                 TestWriter() && TestParameterLists() && TestBinding() &&
                 TestSymbolCells() && TestInteger() && TestFunctionCells() &&
                 TestNamespaceCut() && TestSetqAndSet() &&
                 TestConstantsAndKeywords() && TestNumericEqual() &&
                 TestNumericTower() && TestNumericCut() &&
                 TestUnwindHostAPI() && TestUnwindLisp() &&
                 TestUnwindCleanupBudget() && TestFrameLimits() &&
                 TestFrameSubstrate() && TestArenaStats() &&
                 TestEvaluationStackProbe() && TestCallHeadProbe() &&
                 TestArgumentFrame() && TestArgumentProbe() &&
                 TestLambdaBodyFrame() && TestLambdaBodyChain() &&
                 TestMacroFrame() && TestNativeReentry() &&
                 TestNativeOwningReentry() && TestResumableFrameGC() &&
                 TestCleanupRunGC() && TestPrimitiveOrder() &&
                 TestResumableFrameBudget() && TestResumableFrameCancel() &&
                 TestMixedCleanupLIFO() && TestGcStackConstantInNesting() &&
                 TestLongArgumentLists() && TestNativeArityRecord() &&
                 TestExhaustionCatchability() &&
                 TestExhaustionHandlerReentry() &&
                 TestCaughtExhaustionSession() && TestMarkStackProbe() &&
                 TestMarkRaiseIsFatal() && TestMarkPrintingCallbackIsSafe() &&
                 TestMarkGraphShapes() && TestCollectionStatsPinned() &&
                 TestArityDataUnderCollection() && TestCatchThrow() &&
                 TestConditionCaseResumesControl() && TestQuitIsCatchable() &&
                 TestProtectedCall() && TestHostRaiseCompletion() &&
                 TestDynamicBinding()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
