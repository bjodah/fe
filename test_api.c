#include <setjmp.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  return state->polls == state->cancel_after;
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

int main(void) {
  return TestContextCreation() && TestUserDataAndErrors() &&
                 TestStringInput() && TestFileInput() &&
                 TestEvaluationControl() && TestExtensionAPI() &&
                 TestRootsAndCalls() && TestMathNatives() &&
                 TestSerialization() && TestDottedLists()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
