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
} ErrorState;

enum { TestArenaSize = 16 * 1024 };

typedef struct TestArena {
  alignas(max_align_t) unsigned char bytes[TestArenaSize];
} TestArena;

static ErrorState error_states[2];

[[noreturn]] static void HandleError(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    const char* message,
    // cppcheck-suppress constParameterCallback
    FeObject* stack) {
  ErrorState* state = FeGetUserData(context);
  state->called = state->context == context &&
                  strcmp(message, state->expected_message) == 0 &&
                  FeIsNil(stack);
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
  CHECK(!FeIsNil(FeMakeBool(context, true)));
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

int main(void) {
  return TestContextCreation() && TestUserDataAndErrors() ? EXIT_SUCCESS
                                                          : EXIT_FAILURE;
}
