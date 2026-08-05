#include <inttypes.h>
#include <setjmp.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fe.h"

#define CHECK(condition)                                 \
  do {                                                   \
    if (!(condition)) {                                  \
      fprintf(stderr, "check failed: %s\n", #condition); \
      return false;                                      \
    }                                                    \
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
  state->called = state->context == context &&
                  strcmp(message, state->expected_message) == 0;
  state->stack_was_nil = FeIsNil(stack);
  longjmp(state->jump, 1);
}

static bool TestContextCreation(void) {
  const size_t minimum = FeMinimumArenaSize();
  const size_t alignment = FeArenaAlignment();
  CHECK(minimum > 0);
  CHECK(alignment > 0);
  CHECK(FeOpenContext(nullptr, minimum) == nullptr);

  size_t allocation_size;
  CHECK(!ckd_add(&allocation_size, minimum, alignment));
  TestArena storage;
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
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeEvaluateString(context, label, source, length);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
  return true;
}

static bool ExpectEvaluationOptionsError(FeContext* context,
                                         ErrorState* state,
                                         const char* label,
                                         const char* source,
                                         size_t length,
                                         const FeEvalOptions* options,
                                         const char* expected) {
  const size_t gc = FeSaveGC(context);
  state->called = false;
  state->expected_message = expected;
  if (setjmp(state->jump) == 0) {
    (void)FeEvaluateStringWithOptions(context, label, source, length, options);
    CHECK(false);
  }
  FeRestoreGC(context, gc);
  CHECK(state->called);
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

static bool TestUserDataAndErrors(void) {
  TestArena arenas[2];
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
  CHECK(!error_states[1].called);
  CHECK(TriggerAndRecover(contexts[1], &error_states[1]));

  for (size_t i = 0; i < 2; i++) {
    FeCloseContext(contexts[i]);
  }
  return true;
}

static bool TestStringInput(void) {
  TestArena arena;
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
                              "syntax.fe:8: unclosed list"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "after-syntax.fe", "6", 1), "6"));
  CHECK(ExpectEvaluationError(context, &state, "runtime.fe", "1 (car 2) 3", 11,
                              "runtime.fe: expected pair, got double"));
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

static bool TestFileInput(void) {
  TestArena arena;
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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char loop[] = "(while t 1)";
  const FeEvalOptions loop_options = {.step_limit = 32};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "loop.fe", loop, sizeof(loop) - 1, &loop_options,
      "loop.fe: evaluation step limit exceeded"));
  CHECK(IsRendered(context, FeEvaluateString(context, "recovered.fe", "5", 1),
                   "5"));

  static const char recursion[] =
      "(setq recurse (fn (x) (recurse x))) (recurse 1)";
  const FeEvalOptions recursion_options = {.step_limit = 64};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "recursion.fe", recursion, sizeof(recursion) - 1,
      &recursion_options, "recursion.fe: evaluation step limit exceeded"));

  static const char macros[] =
      "(setq expand (macro (x) x)) (expand (expand (expand (expand (expand "
      "(expand (expand (expand 1))))))))";
  const FeEvalOptions macro_options = {.step_limit = 20};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "macros.fe", macros, sizeof(macros) - 1, &macro_options,
      "macros.fe: evaluation step limit exceeded"));

  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3};
  const FeEvalOptions interrupt_options = {
      .poll_interval = 4, .interrupt = Interrupt, .userdata = &interrupt};
  CHECK(ExpectEvaluationOptionsError(context, &state, "interrupt.fe", loop,
                                     sizeof(loop) - 1, &interrupt_options,
                                     "interrupt.fe: evaluation cancelled"));
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
  CHECK(ExpectEvaluationOptionsError(context, &state, "default-poll.fe", loop,
                                     sizeof(loop) - 1, &default_poll_options,
                                     "default-poll.fe: evaluation cancelled"));
  CHECK(default_poll.polls == 1);
  CHECK(default_poll.userdata_seen);

  static const char addition[] = "(+ 1 2)";
  const FeEvalOptions exact_options = {.step_limit = 3};
  for (size_t i = 0; i < 2; i++) {
    CHECK(ExpectEvaluationOptionsError(
        context, &state, "exact.fe", addition, sizeof(addition) - 1,
        &exact_options, "exact.fe: evaluation step limit exceeded"));
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
  FeSet(context, FeMakeSymbol(context, "reenter"),
        FeMakeNativeFn(context, ReenterEvaluation));
  FeRestoreGC(context, gc);
  static const char nested[] = "(reenter)";
  const FeEvalOptions outer_options = {.step_limit = 24};
  state.nested_with_options = false;
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "outer.fe", nested, sizeof(nested) - 1, &outer_options,
      "inner.fe: evaluation step limit exceeded"));
  CHECK(nested_interrupt.polls == 0);
  state.nested_with_options = true;
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "outer.fe", nested, sizeof(nested) - 1, &outer_options,
      "inner.fe: evaluation step limit exceeded"));
  CHECK(nested_interrupt.polls == 0);
  CHECK(IsRendered(context, FeEvaluateString(context, "recovered.fe", "7", 1),
                   "7"));

  FeCloseContext(context);
  return true;
}

static bool TestExtensionAPI(void) {
  TestArena arena;
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
                   "5"));
  CHECK(ExpectEvaluationError(
      context, &state, "native.fe", "(add-exactly 2 3 4)",
      sizeof("(add-exactly 2 3 4)") - 1, "native.fe: too many arguments"));
  CHECK(ExpectEvaluationError(context, &state, "native.fe", "(add-exactly 2)",
                              sizeof("(add-exactly 2)") - 1,
                              "native.fe: too few arguments"));

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
  TestArena arena;
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
                     "((1 2) 3)"));
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
      &options, "call.fe: evaluation step limit exceeded"));
  FeReleaseRoot(context, state.root);

  FeCloseContext(context);
  return true;
}

static bool TestCallWithOptions(void) {
  TestArena arena;
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
      "30"));

  // A controlled call restores its internal GC frame, so repeated calls do
  // not grow the stack and the rooted callable survives each one.
  const size_t call_gc = FeSaveGC(context);
  for (size_t i = 0; i < 16; i++) {
    CHECK(IsRendered(
        context,
        FeCallWithOptions(context, FeGetRoot(root), arguments, 2, &generous),
        "30"));
    CHECK(FeSaveGC(context) == call_gc);
  }

  // Wrong arity raises only under strict arity, like any other call.
  static const char one_arg_function[] = "(fn (x) x)";
  FeRoot* arity_root = FeCreateRoot(
      context, FeEvaluateString(context, "call.fe", one_arg_function,
                                sizeof(one_arg_function) - 1));
  FeSetStrictArity(context, true);
  CHECK(ExpectCallWithOptionsError(context, &state, FeGetRoot(arity_root),
                                   nullptr, 0, &generous,
                                   "wrong-number-of-arguments"));
  FeSetStrictArity(context, false);

  // Error propagation: a body that raises, and a non-callable value.
  static const char raise_function[] = "(fn () (car 1))";
  FeRoot* raise_root =
      FeCreateRoot(context, FeEvaluateString(context, "call.fe", raise_function,
                                             sizeof(raise_function) - 1));
  CHECK(ExpectCallWithOptionsError(context, &state, FeGetRoot(raise_root),
                                   nullptr, 0, &generous,
                                   "expected pair, got double"));
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
  FeSet(context, FeMakeSymbol(context, "reenter-call-with-options"),
        FeMakeNativeFn(context, ReenterCallWithOptions));
  FeRestoreGC(context, gc);
  static const char nested[] = "(reenter-call-with-options)";
  const FeEvalOptions outer = {.step_limit = 24};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "outer.fe", nested, sizeof(nested) - 1, &outer,
      "outer.fe: evaluation step limit exceeded"));
  CHECK(nested_interrupt.polls == 0);

  // One failed invocation does not poison later independent calls.
  CHECK(IsRendered(
      context,
      FeCallWithOptions(context, FeGetRoot(root), arguments, 2, &generous),
      "30"));

  FeReleaseRoot(context, state.root);
  FeReleaseRoot(context, root);
  FeCloseContext(context);
  return true;
}

static bool TestMathNatives(void) {
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);

#define CHK(expr, expected)                                                  \
  CHECK(IsRendered(                                                          \
      context, FeEvaluateString(context, "math.fe", expr, sizeof(expr) - 1), \
      expected))

  CHK("(sin 0)", "0");
  CHK("(cos 0)", "1");
  CHK("(expt 2 8)", "256");
  CHK("(expt 2 10)", "1024");
  CHK("(sqrt 16)", "4");
  CHK("(log (exp 1))", "1");
  CHK("(log 100 10)", "2");
  CHK("(atan 1)", "0.7853982");
  CHK("(atan 1 1)", "0.7853982");

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
  TestArena arena;
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
                                     "cancel.fe: evaluation cancelled"));
  CHECK(render_target.length > 0);

  // The context still works afterwards.
  CHECK(IsRendered(context, FeEvaluateString(context, "after.fe", "(+ 2 3)", 7),
                   "5"));

  FeCloseContext(context);
  return true;
}

static bool TestParameterLists(void) {
  TestArena arena;
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
  CHK("((lambda (a &rest r) (list a r)) 1)", "(1 nil)");
  CHK("((lambda (&rest r) r) 1 2 3)", "(1 2 3)");
  CHK("((lambda (a . r) (list a r)) 1 2 3)", "(1 (2 3))");
  CHK("((lambda r r) 1 2 3)", "(1 2 3)");
  CHK("((macro (a &rest r) (cons 'list (cons a r))) 1 2 3)", "(1 2 3)");

#undef CHK

  CHECK(ExpectEvaluationError(context, &state, "rest.fe",
                              "((lambda (a &rest) a) 1)",
                              strlen("((lambda (a &rest) a) 1)"),
                              "rest.fe: &rest needs a parameter name"));
  CHECK(ExpectEvaluationError(context, &state, "rest.fe",
                              "((lambda (a &rest r x) a) 1)",
                              strlen("((lambda (a &rest r x) a) 1)"),
                              "rest.fe: &rest must be the last parameter"));

  // Lax by default: a missing argument is nil, an extra one is dropped, and a
  // non-symbol parameter binds nothing.
  CHECK(!FeGetStrictArity(context));
#define LAX(expr, expected)                                                 \
  CHECK(IsRendered(                                                         \
      context, FeEvaluateString(context, "lax.fe", expr, sizeof(expr) - 1), \
      expected))
  LAX("((lambda (x) x))", "nil");
  LAX("((lambda () 1) 2)", "1");
  LAX("((lambda (1) 5) 2)", "5");
#undef LAX

  FeSetStrictArity(context, true);
  CHECK(FeGetStrictArity(context));
#define STRICT(expr, message)                                     \
  CHECK(ExpectEvaluationError(context, &state, "strict.fe", expr, \
                              strlen(expr), message))
  STRICT("((lambda (x) x))", "strict.fe: wrong-number-of-arguments");
  STRICT("((lambda (a b) a) 1)", "strict.fe: wrong-number-of-arguments");
  STRICT("((lambda () 1) 2)", "strict.fe: wrong-number-of-arguments");
  STRICT("((lambda (a) a) 1 2)", "strict.fe: wrong-number-of-arguments");
  STRICT("((lambda (1) 5) 2)", "strict.fe: parameter is not a symbol");
  STRICT("((macro (a) a))", "strict.fe: wrong-number-of-arguments");
#undef STRICT

  // What strict arity still accepts.
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

  FeSetStrictArity(context, false);
  CHECK(IsRendered(
      context, FeEvaluateString(context, "again.fe", "((lambda (x) x))", 16),
      "nil"));

  FeCloseContext(context);
  return true;
}

static bool TestBinding(void) {
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // A symbol exists as soon as it is interned; having a value is separate.
  FeObject* absent = FeMakeSymbol(context, "absent");
  CHECK(!FeIsBound(context, absent));
  CHECK(FeIsBound(context, FeMakeSymbol(context, "t")));
  CHECK(FeIsBound(context, FeMakeSymbol(context, "car")));

  CHECK(ExpectEvaluationError(context, &state, "void.fe", "absent", 6,
                              "void.fe: void-variable absent"));
  CHECK(ExpectEvaluationError(context, &state, "void.fe", "(absent 1)", 10,
                              "void.fe: void-function absent"));
  CHECK(ExpectEvaluationError(context, &state, "void.fe", "(+ 1 absent)", 12,
                              "void.fe: void-variable absent"));

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
                              "reach.fe: expected pair, got symbol"));
  CHK("(is (car (env)) (car (env)))", "t");

#undef CHK

  CHECK(FeIsBound(context, FeMakeSymbol(context, "holds-nil")));
  CHECK(!FeIsBound(context, absent));

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
  TestArena arena;
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
           "setq.fe: void-variable missing-thing");
  CHK("p1", "1");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "p3")));

  // Odd form count: the earlier pair still stands; the dangling final
  // symbol is diagnosed only once it is reached.
  SETQ_ERR("(setq odd-a 5 odd-b)", "setq.fe: wrong-number-of-arguments");
  CHK("odd-a", "5");

  // A non-symbol target is a type error, checked before any value form
  // would be evaluated.
  SETQ_ERR("(setq 1 2)", "setq.fe: wrong-type-argument");

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
          "set.fe: wrong-number-of-arguments");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "set-probe")));
  SET_ERR("(set 'x)", "set.fe: wrong-number-of-arguments");

  // An arity-correct call evaluates both forms left to right before
  // validating the first value's type, so a type error never erases a side
  // effect the second form already had.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "set-probe2")));
  SET_ERR("(set 1 (do (setq set-probe2 t) 2))", "set.fe: wrong-type-argument");
  CHECK(FeIsBound(context, FeMakeSymbol(context, "set-probe2")));

  // Exact arity for `setq`/`set` does not move with `FeSetStrictArity()`;
  // the lax-arity option applies only to user functions/macros.
  CHECK(!FeGetStrictArity(context));
  FeSetStrictArity(context, true);
  CHECK(FeGetStrictArity(context));
  SET_ERR("(set 'x)", "set.fe: wrong-number-of-arguments");
  SET_ERR("(set 'x 1 2)", "set.fe: wrong-number-of-arguments");
  CHECK(ExpectEvaluationError(context, &state, "setq.fe", "(setq a 1 b)",
                              strlen("(setq a 1 b)"),
                              "setq.fe: wrong-number-of-arguments"));
  FeSetStrictArity(context, false);

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
  TestArena arena;
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
  EQ_ERR("(=)", "eq.fe: wrong-number-of-arguments");
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
  EQ_ERR("(= 1 \"1\")", "eq.fe: wrong-type-argument");
  EQ_ERR("(= 1 nil)", "eq.fe: wrong-type-argument");
  EQ_ERR("(= \"1\" (do (setq eq-probe t) 2))", "eq.fe: wrong-type-argument");
  CHK("eq-probe", "t");

  // The hard cut, proven directly: `=` no longer assigns, so an unbound
  // symbol is void-variable, and it stays unbound afterwards.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "never-bound")));
  EQ_ERR("(= never-bound 3)", "eq.fe: void-variable never-bound");
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

static bool TestMacroExpansion(void) {
  // Deliberately tight: the expansion has to survive the collections that
  // evaluating it provokes, and nothing but Fe's GC stack refers to it.
  TestArena arena;
  const size_t size = FeMinimumArenaSize() + 8192;
  CHECK(size <= sizeof(arena.bytes));
  FeContext* context = FeOpenContext(arena.bytes, size);
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  static const char loop[] =
      "(setq make (macro (a b) (list 'list a b)))"
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
                              "(setq bad (macro () '(car 1))) (bad)",
                              strlen("(setq bad (macro () '(car 1))) (bad)"),
                              "expansion.fe: expected pair, got double"));
  CHECK(ExpectEvaluationError(context, &state, "expander.fe",
                              "(setq worse (macro () (car 1))) (worse)",
                              strlen("(setq worse (macro () (car 1))) (worse)"),
                              "expander.fe: expected pair, got double"));
  CHECK(IsRendered(context, FeEvaluateString(context, "after.fe", "(+ 1 2)", 7),
                   "3"));

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
  TestArena arena;
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
  TestArena arena;
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
  TestArena arena;
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
                              "host.fe: expected pair, got double"));
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
                                     "host.fe: evaluation cancelled"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_count == 1);

  // Budget exhaustion: the cleanup is not gated on steps remaining -- it
  // still runs to completion (a bare `fclose`, so this mainly documents the
  // invariant) even though the body's own budget hit zero.
  ResetResourceState();
  const FeEvalOptions tiny_budget = {.step_limit = 8};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "host.fe", looping, sizeof(looping) - 1, &tiny_budget,
      "host.fe: evaluation step limit exceeded"));
  CHECK(!resource_state.open);
  CHECK(resource_state.close_count == 1);

  FeCloseContext(context);
  return true;
}

static bool TestUnwindLisp(void) {
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
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
                              "unwind.fe: expected pair, got double"));
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
                                     "unwind.fe: evaluation cancelled"));
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
      &tiny_budget, "unwind.fe: evaluation step limit exceeded"));
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
                              "unwind.fe: assertion failure"));
  CHK("log", "(outer middle inner)");

  // A cleanup that itself errors: a diagnostic is printed rather than
  // swallowed, the original error ("assertion failure") is still what
  // reaches the host, and the outer cleanup still runs.
  CHK("(setq outer-ran nil)", "nil");
  static const char failing_cleanup[] =
      "(unwind-protect"
      "  (unwind-protect"
      "    (assert nil)"
      "    (car 1))"
      "  (setq outer-ran t))";
  EvalCall failing_cleanup_call = {.context = context,
                                   .state = &state,
                                   .label = "unwind.fe",
                                   .source = failing_cleanup,
                                   .length = sizeof(failing_cleanup) - 1,
                                   .options = nullptr,
                                   .expected = "unwind.fe: assertion failure"};
  char captured[512];
  CHECK(CaptureStderr(RunEvalCall, &failing_cleanup_call, captured,
                      sizeof(captured)));
  CHECK(failing_cleanup_call.result);
  CHECK(strstr(captured, "cleanup error") != nullptr);
  CHECK(strstr(captured, "expected pair, got double") != nullptr);
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
                              "unwind.fe: assertion failure"));
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
  TestArena arena;
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
      .expected = "budget.fe: expected pair, got double"};
  char captured[512];
  CHECK(CaptureStderr(RunEvalCall, &runaway_cleanup_call, captured,
                      sizeof(captured)));
  CHECK(runaway_cleanup_call.result);
  CHECK(strstr(captured, "cleanup error") != nullptr);
  CHECK(strstr(captured, "evaluation step limit exceeded") != nullptr);

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
      "budget.fe: evaluation step limit exceeded"));
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
      .expected = "budget.fe: evaluation cancelled"};
  CHECK(CaptureStderr(RunEvalCall, &runaway_interrupted_call, captured,
                      sizeof(captured)));
  CHECK(runaway_interrupted_call.result);
  CHECK(interrupt.polls >= interrupt.cancel_after_second);
  CHECK(strstr(captured, "cleanup error") != nullptr);
  CHECK(strstr(captured, "evaluation cancelled") != nullptr);
  CHK("outer-ran", "t");

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
  TestArena arena;
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
      "toosmall.fe: evaluation frame limit exceeded"));

  // A separate, deliberately tiny `max_frames` against unbounded
  // self-recursion -- not tied to the measured peak above, since unbounded
  // recursion overflows any small ceiling regardless of its exact value --
  // proves both a registered Lisp cleanup (`unwind-protect`) and a
  // registered native cleanup (`FeProtectWithCleanup`, through
  // `ReentrantNative`'s own `MarkReentryCleanup`) run during the unwind, and
  // that the context is reusable afterward.
  FeObject* native = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, native);
  FeSet(context, FeMakeSymbol(context, "reentrant-native"), native);
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
      "(setq loop (fn (x) (loop x))) "
      "(unwind-protect (do (reentrant-native) (loop 1))"
      "  (setq cleanup-ran t))";
  const FeEvalOptions tight_frames = {.max_frames = 6};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "cleanup-frames.fe", overflow_with_cleanup,
      sizeof(overflow_with_cleanup) - 1, &tight_frames,
      "cleanup-frames.fe: evaluation frame limit exceeded"));
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
      "(setq m (macro () (list (quote m)))) (m)";
  const FeEvalOptions tight_macro_frames = {.max_frames = 5};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "macro-frames.fe", macro_recursion,
      sizeof(macro_recursion) - 1, &tight_macro_frames,
      "macro-frames.fe: evaluation frame limit exceeded"));

  // The context is reusable after both overflow paths.
  static const char deep[] =
      "(setq deep (lambda (n) (if (<= n 0) 0 (+ 1 (deep (- n 1)))))) (deep 40)";
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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // quote is a value in Fe's one namespace, not reader syntax: rebinding it
  // makes its argument evaluate. Its primitive value still takes exactly one
  // raw argument and deliberately ignores extras.
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "quote.fe", "(quote 1 2)",
                                    sizeof("(quote 1 2)") - 1),
                   "1"));
  static const char quote_rebound[] = "(setq quote (fn (x) x)) (quote (+ 1 2))";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "quote.fe", quote_rebound,
                                    sizeof(quote_rebound) - 1),
                   "3"));

  // The outer `do` frame is the only reference to its pending forms while
  // the loop repeatedly allocates and collects. A resumed final lookup proves
  // the temporary-dispatch frame was marked, rather than merely surviving by
  // accident on the GC stack.
  TestArena gc_arena;
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
  TestArena frame_arena;
  FeContext* frame_context = FeOpenContext(frame_arena.bytes, gc_size);
  CHECK(frame_context != nullptr);
  ErrorState frame_state = {.context = frame_context};
  FeSetUserData(frame_context, &frame_state);
  FeSetErrorFn(frame_context, HandleError);
  static const char recurse[] =
      "(setq recurse (fn (x) (if (<= x 0) 0 (recurse (- x 1))))) "
      "(recurse 100)";
  const FeEvalOptions physical_only = {.max_frames = 0};
  CHECK(ExpectEvaluationOptionsError(
      frame_context, &frame_state, "frames.fe", recurse, sizeof(recurse) - 1,
      &physical_only, "frames.fe: evaluation frame limit exceeded"));
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
  TestArena arena;
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
  FeSet(context, FeMakeSymbol(context, "ordinary-native"), ordinary);
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "ordinary.fe", "(ordinary-native)",
                                    sizeof("(ordinary-native)") - 1),
                   "42"));
  CHECK(FeGetArenaStats(context).peak_native_reentry == 0);
  FeObject* reentrant = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, reentrant);
  FeSet(context, FeMakeSymbol(context, "reentrant-native"), reentrant);
  state.reentry_remaining = 2;
  CHECK(
      IsRendered(context,
                 FeEvaluateString(context, "reentrant.fe", "(reentrant-native)",
                                  sizeof("(reentrant-native)") - 1),
                 "3"));
  CHECK(FeGetArenaStats(context).peak_native_reentry == 2);

  FeCloseContext(context);

  // A deliberately exact-fit arena -- no slots spare once the core
  // primitives are registered -- turns the very first user allocation
  // into an out-of-memory failure, so `allocation_failures` and
  // `collection_count` (MakeObject always tries a collection before
  // giving up, even with nothing collectible) are both directly
  // observable, and the context is confirmed still queryable afterward.
  TestArena tight_storage;
  const size_t tight_size = FeMinimumArenaSize();
  CHECK(tight_size <= sizeof(tight_storage.bytes));
  FeContext* tight = FeOpenContext(tight_storage.bytes, tight_size);
  CHECK(tight != nullptr);
  // `HandleError` unconditionally compares against `expected_message`, so
  // this needs a non-null placeholder even though this test does not
  // check `tight_state.called`: what it looks for is that the jump was
  // taken and the counters moved, not the exact message text.
  ErrorState tight_state = {.context = tight,
                            .expected_message = "oom.fe: out of memory"};
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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "stack-probe", StackProbe);

  static const char deep_def[] =
      "(setq deep (fn (n) (if (<= n 0) (stack-probe) (+ 1 (deep (- n "
      "1))))))";
  CHECK(FeEvaluateString(context, "deep-def.fe", deep_def,
                         sizeof(deep_def) - 1) != nullptr);

  // The expected frame-limit error, recovered without poisoning the
  // context -- the same shape `TestFrameLimits` already proves for the
  // general case.
  const FeEvalOptions tight_frames = {.max_frames = 5};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "probe-frames.fe", "(deep 50)", sizeof("(deep 50)") - 1,
      &tight_frames, "probe-frames.fe: evaluation frame limit exceeded"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "recovered.fe", "(deep 20)",
                                    sizeof("(deep 20)") - 1),
                   "20"));
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
                   "0"));
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
    (void)snprintf(expected, sizeof(expected), "%zu", depths[i]);

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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeDefineNative(context, "stack-probe", StackProbe);

  static const char loop_def[] = "(setq loop (fn () (do (stack-probe) loop)))";
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
                              "call-head.fe: void-function car-thing"));
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
  TestArena arena;
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
      "arg-native.fe: evaluation step limit exceeded"));
  const FeEvalOptions native_ok = {.step_limit = 6};
  CHECK(IsRendered(
      context,
      FeEvaluateStringWithOptions(context, "arg-native.fe", "(add-exactly 1 2)",
                                  sizeof("(add-exactly 1 2)") - 1, &native_ok),
      "3"));

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
      "arg-lambda.fe: evaluation step limit exceeded"));
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
                              "arg-dotted.fe: dotted pair in argument list"));

  // An error in an argument position unwinds through the argument frame and
  // the context stays usable afterwards.
  static const char bad_arg[] = "(add-exactly 1 (car 2))";
  CHECK(ExpectEvaluationError(context, &state, "arg-error.fe", bad_arg,
                              sizeof(bad_arg) - 1,
                              "arg-error.fe: expected pair, got double"));
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
  TestArena arena;
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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);

  // Step budget: `((fn (x) (let y 1) (list x y)) 5)` costs the same number
  // of steps the recursive `DoList` body produced -- one step before every
  // body form, every parameter walk, and every environment walk (the `let`
  // adds a level, so the later `list`/`x`/`y` lookups walk one deeper), with
  // none for the lambda/body frame transitions themselves.
  static const char body[] = "((fn (x) (let y 1) (list x y)) 5)";
  const FeEvalOptions tight = {.step_limit = 22};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "body.fe", body, sizeof(body) - 1, &tight,
      "body.fe: evaluation step limit exceeded"));
  const FeEvalOptions ok = {.step_limit = 23};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "body.fe", body,
                                               sizeof(body) - 1, &ok),
                   "(5 1)"));

  // let threading through the body frame: a `let` in a body form extends the
  // environment the following forms see, even when the lambda call itself is
  // an argument to an outer call (so the body frame is not the run's base
  // frame), and a nested lambda's body still sees the outer body's binding.
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "nested-let.fe",
                       "((fn (g) (g)) (fn () (let y 1) y))",
                       sizeof("((fn (g) (g)) (fn () (let y 1) y))") - 1),
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
                              "leak.fe: void-variable fresh"));

  // An error in a body form unwinds through the body frame and the context
  // stays usable afterwards.
  static const char bad[] = "((fn () (car 2)))";
  CHECK(ExpectEvaluationError(context, &state, "body-error.fe", bad,
                              sizeof(bad) - 1,
                              "body-error.fe: expected pair, got double"));
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
  TestArena arena;
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
                   "0"));
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
                                 "(setq f%d (fn () (f%d))) ", i, i + 1);
    CHECK(written > 0);
    length += (size_t)written;
    CHECK(length < sizeof(source));
  }
  {
    const int written =
        snprintf(source + length, sizeof(source) - length,
                 "(setq f%d (fn () (stack-probe))) (f0)", ChainDepth);
    CHECK(written > 0);
    length += (size_t)written;
    CHECK(length < sizeof(source));
  }

  stack_probe_last_address = 0;
  stack_probe_deepest_address = 0;
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "lambda-chain.fe", source, length),
                   "0"));
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
  TestArena arena;
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
      "(setq m (macro (x) (list 'quote x))) (m (+ 1 2))";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "raw.fe", raw_args, sizeof(raw_args) - 1),
      "(+ 1 2)"));

  // `let`/`newenv` threading through the macro body: a `let` in a body form
  // extends the environment the following forms see, exactly as the
  // recursive `DoList` `&env` out-parameter did, and it shadows without
  // leaking past the macro call.
  static const char body_let[] =
      "(setq m (macro () (let y 1) (list 'list y y))) (m)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "body-let.fe", body_let, sizeof(body_let) - 1),
      "(1 1)"));
  static const char shadow[] = "(setq y 7) (setq m (macro () (let y 2) y)) (m)";
  CHECK(IsRendered(
      context,
      FeEvaluateString(context, "shadow.fe", shadow, sizeof(shadow) - 1), "2"));
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "after.fe", "y", sizeof("y") - 1),
                   "7"));

  // The expansion is evaluated in the caller environment, not the macro's:
  // `x` here is the lambda's lexical binding, which the expansion's `x`
  // must resolve.
  static const char caller_env[] = "(setq m (macro () 'x)) ((fn (x) (m)) 42)";
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
                   FeEvaluateString(context, "m.fe", "(setq m (macro () 5))",
                                    sizeof("(setq m (macro () 5))") - 1),
                   "(macro nil 5)"));
  const FeEvalOptions tight = {.step_limit = 4};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "m.step.fe", "(m)", sizeof("(m)") - 1, &tight,
      "m.step.fe: evaluation step limit exceeded"));
  const FeEvalOptions ok = {.step_limit = 5};
  CHECK(IsRendered(context,
                   FeEvaluateStringWithOptions(context, "m.step.fe", "(m)",
                                               sizeof("(m)") - 1, &ok),
                   "5"));

  // An error in a later body form, after an earlier form ran and a `let`
  // extended the body environment, unwinds through the macro frame and the
  // context stays usable; the earlier form's side effect stands.
  static const char mid_body_error[] =
      "(setq ran nil) (setq boom (macro () (setq ran t) (let x 1) (car 2))) "
      "(boom)";
  CHECK(ExpectEvaluationError(context, &state, "mid-body.fe", mid_body_error,
                              sizeof(mid_body_error) - 1,
                              "mid-body.fe: expected pair, got double"));
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
  TestArena frame_arena;
  FeContext* frame_context = FeOpenContext(frame_arena.bytes, gc_size);
  CHECK(frame_context != nullptr);
  ErrorState frame_state = {.context = frame_context};
  FeSetUserData(frame_context, &frame_state);
  FeSetErrorFn(frame_context, HandleError);
  static const char self_expanding[] = "(setq m (macro () (list 'm))) (m)";
  const FeEvalOptions physical_only = {.max_frames = 0};
  CHECK(ExpectEvaluationOptionsError(
      frame_context, &frame_state, "macro-frames.fe", self_expanding,
      sizeof(self_expanding) - 1, &physical_only,
      "macro-frames.fe: evaluation frame limit exceeded"));
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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context, .reentry_max_native_reentry = 8};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeObject* native = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, native);
  FeSet(context, FeMakeSymbol(context, "reentrant-native"), native);

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
                   "9"));
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
      "native-reentry.fe: native evaluation re-entry limit exceeded"));
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
                   "9"));
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
                   "17"));
  CHECK(state.reentry_max_seen == 17);
  // cppcheck-suppress redundantAssignment
  state.reentry_remaining = 17;
  state.reentry_current = 0;
  state.reentry_max_seen = 0;
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "native-reentry.fe", "(reentrant-native)",
      sizeof("(reentrant-native)") - 1, &deeper,
      "native-reentry.fe: native evaluation re-entry limit exceeded"));
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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context, .reentry_max_native_reentry = 8};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  FeObject* ordinary = FeMakeNativeFn(context, OrdinaryNative);
  state.reentry_owning_target = FeCreateRoot(context, ordinary);
  FeSet(context, FeMakeSymbol(context, "ordinary-native"), ordinary);
  FeDefineNative(context, "owning-reenter", OwningReenter);
  FeObject* reentrant = FeMakeNativeFn(context, ReentrantNative);
  state.reentry_self = FeCreateRoot(context, reentrant);
  FeSet(context, FeMakeSymbol(context, "reentrant-native"), reentrant);

  static const char body[] = "((fn () (owning-reenter) (ordinary-native)))";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "owning.fe", body, sizeof(body) - 1),
      "42"));
  CHECK(FeGetArenaStats(context).peak_native_reentry == 1);

  // The same shape with the owning call removed succeeds identically, so the
  // assertion above is about the owning call, not about the lambda body.
  static const char plain[] = "((fn () (ordinary-native)))";
  CHECK(IsRendered(
      context, FeEvaluateString(context, "owning.fe", plain, sizeof(plain) - 1),
      "42"));

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
      "9"));
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
      "owning.fe: native evaluation re-entry limit exceeded"));
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
                              "owning.fe: expected pair, got double"));
  CHECK(IsRendered(
      context, FeEvaluateString(context, "owning.fe", body, sizeof(body) - 1),
      "42"));

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
  FeSet(context, FeMakeSymbol(context, "gc-native"), native);
  return true;
}

static bool RunFrameGCCase(const FrameGCCase* c) {
  TestArena arena;
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
       "(setq m (macro () (let v 7) (setq n 0) "
       "(while (< n 2000) (setq n (+ n 1)) (cons n n)) v)) (m)",
       "7", nullptr},
      // macro-expansion: the macro body produces the collecting `do` form as
      // its expansion with no collection of its own, so the collections run
      // only while the macro-expansion frame below is suspended over the
      // expansion sub-expression -- isolating the expansion state from the
      // body state.
      {"macro-expansion",
       "(setq m (macro () (quote (do (setq n 0) "
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
  TestArena arena;
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
      &generous_cleanup_budget, "cleanup-gc.fe: expected pair, got double"));
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
  TestArena arena;
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
            "order.fe: expected pair, got double");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "setcar-probe")));
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "setcdr-probe")));
  ORDER_ERR("(setcdr 1 (do (setq setcdr-probe t) 2))",
            "order.fe: expected pair, got double");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "setcdr-probe")));

  // Arithmetic validates as it walks, unlike `=`'s evaluate-the-whole-list-
  // first policy (`TestNumericEqual`): a type error on an early operand
  // stops evaluation before a later operand's form ever runs.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "arith-probe")));
  ORDER_ERR("(+ 1 \"x\" (do (setq arith-probe t) 3))",
            "order.fe: expected double, got string");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "arith-probe")));

  // `<`/`<=` consume exactly two operands and never evaluate extras: if the
  // third form here ran, it would itself raise a type error.
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "less-probe")));
  CHK("(< 1 2 (do (setq less-probe t) (car 1)))", "t");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "less-probe")));
  CHK("(<= 2 2 (do (setq less-probe t) (car 1)))", "t");
  CHECK(!FeIsBound(context, FeMakeSymbol(context, "less-probe")));

  // `boundp`/`makunbound` reject a leftover extra argument, unlike the
  // other primitives sharing their frame kind (`not`/`atom`/`car`/`cdr`/
  // `assert`), which silently ignore extras.
  ORDER_ERR("(boundp 'car 'extra)", "order.fe: too many arguments");
  ORDER_ERR("(makunbound 'car 'extra)", "order.fe: too many arguments");

  // `cons`'s two operands evaluate left to right.
  CHK("(setq cons-order '())", "nil");
  CHK("(cons (do (setq cons-order (cons 1 cons-order)) 1)"
      "      (do (setq cons-order (cons 2 cons-order)) 2))",
      "(1 . 2)");
  CHK("cons-order", "(2 1)");

  // `if`'s missing-form cases: no condition, a nil condition with no
  // then-form, and a truthy condition with no then-form are all nil,
  // without an error.
  CHK("(if)", "nil");
  CHK("(if nil)", "nil");
  CHK("(if t)", "nil");

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
  TestArena arena;
  FeContext* context = FeOpenContext(arena.bytes, sizeof(arena.bytes));
  CHECK(context != nullptr);
  ErrorState state = {.context = context};
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  const FeEvalOptions tiny = {.step_limit = 10};
  const bool raised = ExpectEvaluationOptionsError(
      context, &state, "budget.fe", c->form, strlen(c->form), &tiny,
      "budget.fe: evaluation step limit exceeded");
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
  TestArena arena;
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
      "cancel.fe: evaluation cancelled");
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
  TestArena arena;
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
      "(setq deep (fn (n) (if (<= n 0) 0 (+ 1 (deep (- n 1))))))";
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

int main(void) {
  return TestContextCreation() && TestUserDataAndErrors() &&
                 TestStringInput() && TestFileInput() &&
                 TestEvaluationControl() && TestExtensionAPI() &&
                 TestRootsAndCalls() && TestCallWithOptions() &&
                 TestMathNatives() && TestSerialization() &&
                 TestDottedLists() && TestMacroExpansion() && TestWriter() &&
                 TestParameterLists() && TestBinding() && TestSetqAndSet() &&
                 TestNumericEqual() && TestUnwindHostAPI() &&
                 TestUnwindLisp() && TestUnwindCleanupBudget() &&
                 TestFrameLimits() && TestFrameSubstrate() &&
                 TestArenaStats() && TestEvaluationStackProbe() &&
                 TestCallHeadProbe() && TestArgumentFrame() &&
                 TestArgumentProbe() && TestLambdaBodyFrame() &&
                 TestLambdaBodyChain() && TestMacroFrame() &&
                 TestNativeReentry() && TestNativeOwningReentry() &&
                 TestResumableFrameGC() && TestCleanupRunGC() &&
                 TestPrimitiveOrder() && TestResumableFrameBudget() &&
                 TestResumableFrameCancel() && TestMixedCleanupLIFO() &&
                 TestGcStackConstantInNesting()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
