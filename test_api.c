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
} ErrorState;

// Must stay above `FeMinimumArenaSize()`, which is dominated by the
// context's 4096-slot GC stack.
enum { TestArenaSize = 64 * 1024 };

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

  const char forms[] = "(= x 1) (= x (+ x 2)) x";
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
      "(= recurse (fn (x) (recurse x))) (recurse 1)";
  const FeEvalOptions recursion_options = {.step_limit = 64};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "recursion.fe", recursion, sizeof(recursion) - 1,
      &recursion_options, "recursion.fe: evaluation step limit exceeded"));

  static const char macros[] =
      "(= expand (macro (x) x)) (expand (expand (expand (expand (expand "
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
  CHECK(Renders(context, "(do (= a (cons 1 nil)) (setcdr a a) a)", nullptr,
                "(1 . #<cycle>)", false));
  CHECK(Renders(context,
                "(do (= b (cons 1 (cons 2 nil))) (setcdr (cdr b) b) b)",
                nullptr, "(1 2 1 . #<cycle>)", false));

  // A cycle through a car is bounded by depth instead, and depth is spent only
  // on nesting: a long flat list is not deep.
  const FeWriteOptions shallow = {.max_depth = 4};
  CHECK(Renders(context, "(do (= c (cons 1 nil)) (setcar c c) c)", &shallow,
                "((((#<deep>))))", false));
  CHECK(Renders(context, "'(1 2 3 4 5 6 7 8)", &shallow, "(1 2 3 4 5 6 7 8)",
                true));
  CHECK(Renders(context, "'(((1)))", &shallow, "(((1)))", true));
  const FeWriteOptions shallower = {.max_depth = 3};
  CHECK(Renders(context, "'(((1)))", &shallower, "(((#<deep>)))", false));

  // Shared but acyclic structure is printed in full, every time it appears.
  CHECK(Renders(context, "(do (= s '(1 2)) (list s s s))", nullptr,
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
      "(= long nil)"
      "(= n 0)"
      "(while (< n 400) (= long (cons n long)) (= n (+ n 1)))";
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
  CHK("(= absent 7)", "nil");
  CHK("(boundp 'absent)", "t");
  CHK("absent", "7");
  CHK("(makunbound 'absent)", "absent");
  CHK("(boundp 'absent)", "nil");

  // nil is an ordinary value, not an absence.
  CHK("(= holds-nil nil)", "nil");
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
      "(= make (macro (a b) (list 'list a b)))"
      "(= n 0)"
      "(= acc nil)"
      "(while (< n 200) (= acc (make n n)) (= n (+ n 1)))"
      "acc";
  CHECK(IsRendered(context,
                   FeEvaluateString(context, "gc.fe", loop, sizeof(loop) - 1),
                   "(199 199)"));

  // An error raised by the expansion, and one raised while expanding, both
  // leave the context usable.
  CHECK(ExpectEvaluationError(context, &state, "expansion.fe",
                              "(= bad (macro () '(car 1))) (bad)",
                              strlen("(= bad (macro () '(car 1))) (bad)"),
                              "expansion.fe: expected pair, got double"));
  CHECK(ExpectEvaluationError(context, &state, "expander.fe",
                              "(= worse (macro () (car 1))) (worse)",
                              strlen("(= worse (macro () (car 1))) (worse)"),
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
  CHK("(= run-count 0) (unwind-protect 42 (= run-count (+ run-count 1)))",
      "42");
  CHK("run-count", "1");

  // Error: the body raises, the cleanup still runs exactly once, and the
  // error still propagates to the host with its own message intact.
  CHK("(= run-count 0)", "nil");
  static const char erroring[] =
      "(unwind-protect (car 1) (= run-count (+ run-count 1)))";
  CHECK(ExpectEvaluationError(context, &state, "unwind.fe", erroring,
                              sizeof(erroring) - 1,
                              "unwind.fe: expected pair, got double"));
  CHK("run-count", "1");

  // Interrupt: a host `C-g` mid-body still runs the cleanup exactly once.
  CHK("(= run-count 0)", "nil");
  static const char looping[] =
      "(unwind-protect (while t 1) (= run-count (+ run-count 1)))";
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
  CHK("(= run-count 0) (= spin-count 0)", "nil");
  static const char budget_cleanup[] =
      "(unwind-protect (while t 1) "
      "  (do (while (< spin-count 200) (= spin-count (+ spin-count 1))) "
      "      (= run-count (+ run-count 1))))";
  const FeEvalOptions tiny_budget = {.step_limit = 8};
  CHECK(ExpectEvaluationOptionsError(
      context, &state, "unwind.fe", budget_cleanup, sizeof(budget_cleanup) - 1,
      &tiny_budget, "unwind.fe: evaluation step limit exceeded"));
  CHK("run-count", "1");
  CHK("spin-count", "200");

  // Three levels of nesting, inner error: cleanups run innermost first
  // (LIFO), and the nesting is visible in the order they append to `log`.
  CHK("(= log '())", "nil");
  static const char nested[] =
      "(unwind-protect"
      "  (unwind-protect"
      "    (unwind-protect"
      "      (assert nil)"
      "      (= log (cons 'inner log)))"
      "    (= log (cons 'middle log)))"
      "  (= log (cons 'outer log)))";
  CHECK(ExpectEvaluationError(context, &state, "unwind.fe", nested,
                              sizeof(nested) - 1,
                              "unwind.fe: assertion failure"));
  CHK("log", "(outer middle inner)");

  // A cleanup that itself errors: a diagnostic is printed rather than
  // swallowed, the original error ("assertion failure") is still what
  // reaches the host, and the outer cleanup still runs.
  CHK("(= outer-ran nil)", "nil");
  static const char failing_cleanup[] =
      "(unwind-protect"
      "  (unwind-protect"
      "    (assert nil)"
      "    (car 1))"
      "  (= outer-ran t))";
  fflush(stderr);
  const int saved_stderr = dup(STDERR_FILENO);
  CHECK(saved_stderr != -1);
  FILE* capture = tmpfile();
  CHECK(capture != nullptr);
  CHECK(dup2(fileno(capture), STDERR_FILENO) != -1);
  const bool evaluated = ExpectEvaluationError(
      context, &state, "unwind.fe", failing_cleanup,
      sizeof(failing_cleanup) - 1, "unwind.fe: assertion failure");
  fflush(stderr);
  CHECK(dup2(saved_stderr, STDERR_FILENO) != -1);
  close(saved_stderr);
  char captured[512] = {0};
  rewind(capture);
  const size_t captured_length =
      fread(captured, 1, sizeof(captured) - 1, capture);
  captured[captured_length] = '\0';
  fclose(capture);
  CHECK(evaluated);
  CHECK(strstr(captured, "cleanup error") != nullptr);
  CHECK(strstr(captured, "expected pair, got double") != nullptr);
  CHK("outer-ran", "t");

  // Root survival: a lexical binding created in the body -- reachable only
  // through the environment `unwind-protect` captured, not through the
  // global symbol table -- must still resolve to the right object in the
  // cleanup after the body allocates heavily enough to force repeated
  // collections.
  CHK("(= survivor nil)", "nil");
  static const char root_survival[] =
      "(do"
      "  (let x (cons 111 222))"
      "  (unwind-protect"
      "    (do"
      "      (= gc-pressure-i 0)"
      "      (while (< gc-pressure-i 4000)"
      "        (cons gc-pressure-i gc-pressure-i)"
      "        (= gc-pressure-i (+ gc-pressure-i 1)))"
      "      (assert nil))"
      "    (= survivor x)))";
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
  fflush(stderr);
  int saved_stderr = dup(STDERR_FILENO);
  CHECK(saved_stderr != -1);
  FILE* capture = tmpfile();
  CHECK(capture != nullptr);
  CHECK(dup2(fileno(capture), STDERR_FILENO) != -1);
  bool evaluated = ExpectEvaluationOptionsError(
      context, &state, "budget.fe", runaway_cleanup,
      sizeof(runaway_cleanup) - 1, &small_cleanup_budget,
      "budget.fe: expected pair, got double");
  fflush(stderr);
  CHECK(dup2(saved_stderr, STDERR_FILENO) != -1);
  close(saved_stderr);
  char captured[512] = {0};
  rewind(capture);
  size_t captured_length = fread(captured, 1, sizeof(captured) - 1, capture);
  captured[captured_length] = '\0';
  fclose(capture);
  CHECK(evaluated);
  CHECK(strstr(captured, "cleanup error") != nullptr);
  CHECK(strstr(captured, "evaluation step limit exceeded") != nullptr);

  // The regression this whole feature exists to keep passing: a body that
  // exhausted a tiny budget of its own still gets a cleanup that runs to
  // completion, because the fresh budget above is a *replacement*, not a
  // further restriction stacked on top of what the body already spent.
  CHK("(= tiny-budget-ran nil)", "nil");
  static const char tiny_body_budget[] =
      "(unwind-protect (while t 1) (= tiny-budget-ran t))";
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
  CHK("(= outer-ran nil)", "nil");
  static const char runaway_interrupted_cleanup[] =
      "(unwind-protect"
      "  (unwind-protect (while t 1) (while t 1))"
      "  (= outer-ran t))";
  InterruptState interrupt = {.context = context,
                              .expected_userdata = &interrupt,
                              .polls = 0,
                              .cancel_after = 3,
                              .cancel_after_second = 10};
  const FeEvalOptions interrupted_cleanup_budget = {.poll_interval = 4,
                                                    .interrupt = Interrupt,
                                                    .userdata = &interrupt,
                                                    .cleanup_step_limit = 5000};
  fflush(stderr);
  saved_stderr = dup(STDERR_FILENO);
  CHECK(saved_stderr != -1);
  capture = tmpfile();
  CHECK(capture != nullptr);
  CHECK(dup2(fileno(capture), STDERR_FILENO) != -1);
  evaluated = ExpectEvaluationOptionsError(
      context, &state, "budget.fe", runaway_interrupted_cleanup,
      sizeof(runaway_interrupted_cleanup) - 1, &interrupted_cleanup_budget,
      "budget.fe: evaluation cancelled");
  fflush(stderr);
  CHECK(dup2(saved_stderr, STDERR_FILENO) != -1);
  close(saved_stderr);
  memset(captured, 0, sizeof(captured));
  rewind(capture);
  captured_length = fread(captured, 1, sizeof(captured) - 1, capture);
  captured[captured_length] = '\0';
  fclose(capture);
  CHECK(evaluated);
  CHECK(interrupt.polls >= interrupt.cancel_after_second);
  CHECK(strstr(captured, "cleanup error") != nullptr);
  CHECK(strstr(captured, "evaluation cancelled") != nullptr);
  CHK("outer-ran", "t");

#undef CHK

  FeCloseContext(context);
  return true;
}

int main(void) {
  return TestContextCreation() && TestUserDataAndErrors() &&
                 TestStringInput() && TestFileInput() &&
                 TestEvaluationControl() && TestExtensionAPI() &&
                 TestRootsAndCalls() && TestCallWithOptions() &&
                 TestMathNatives() && TestSerialization() &&
                 TestDottedLists() && TestMacroExpansion() && TestWriter() &&
                 TestParameterLists() && TestBinding() && TestUnwindHostAPI() &&
                 TestUnwindLisp() && TestUnwindCleanupBudget()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
