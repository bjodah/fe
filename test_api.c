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

typedef struct ErrorState {
  jmp_buf jump;
  FeContext* context;
  const char* expected_message;
  bool called;
  bool stack_was_nil;
} ErrorState;

enum { TestArenaSize = 16 * 1024 };

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

int main(void) {
  return TestContextCreation() && TestUserDataAndErrors() &&
                 TestStringInput() && TestFileInput()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
