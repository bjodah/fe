#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"

static_assert(FE_API_VERSION == 1);
static_assert(FE_LANGUAGE_VERSION == 2);

typedef struct HostState {
  jmp_buf error_jump;
  FeContext* context;
  FeRoot* function;
  size_t gc_checkpoint;
  char error[128];
  bool expecting_error;
  bool succeeded;
} HostState;

[[noreturn]] static void HandleError(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    const char* message,
    // cppcheck-suppress constParameterCallback
    FeObject* call_trace) {
  HostState* host = FeGetUserData(context);
  (void)call_trace;
  (void)snprintf(host->error, sizeof(host->error), "%s", message);
  longjmp(host->error_jump, 1);
}

static FeObject* Increment(FeContext* context, FeObject* arguments) {
  const double value =
      FeToDouble(context, FeGetNextArgument(context, &arguments));
  FeRequireNoArguments(context, arguments);
  return FeMakeDouble(context, value + 1);
}

int main(void) {
  const size_t alignment = FeArenaAlignment();
  const size_t arena_size = FeMinimumArenaSize() + alignment * 4096;
  void* arena = aligned_alloc(alignment, arena_size);
  if (!arena) {
    return EXIT_FAILURE;
  }
  HostState* host = calloc(1, sizeof(*host));
  if (!host) {
    free(arena);
    return EXIT_FAILURE;
  }

  host->context = FeOpenContext(arena, arena_size);
  if (!host->context) {
    free(host);
    free(arena);
    return EXIT_FAILURE;
  }

  FeSetUserData(host->context, host);
  FeSetErrorFn(host->context, HandleError);
  if (setjmp(host->error_jump) != 0) {
    FeRestoreGC(host->context, host->gc_checkpoint);
    if (!host->expecting_error) {
      fprintf(stderr, "unexpected Fe error: %s\n", host->error);
      goto close;
    }
    printf("recovered: %s\n", host->error);
    goto recovered;
  }

  FeDefineNative(host->context, "increment", Increment);
  host->gc_checkpoint = FeSaveGC(host->context);
  static const char definitions[] =
      "(setq answer 41)\n"
      "(fn (value) (increment value))";
  const FeEvalOptions options = {.step_limit = 100};
  FeObject* function = FeEvaluateStringWithOptions(
      host->context, "example", definitions, sizeof(definitions) - 1, &options);
  host->function = FeCreateRoot(host->context, function);
  FeRestoreGC(host->context, host->gc_checkpoint);

  host->gc_checkpoint = FeSaveGC(host->context);
  host->expecting_error = true;
  static const char invalid[] = "(car 1)";
  (void)FeEvaluateString(host->context, "example", invalid,
                         sizeof(invalid) - 1);
  fputs("expected Fe error was not raised\n", stderr);
  goto close;

recovered:
  host->expecting_error = false;
  host->gc_checkpoint = FeSaveGC(host->context);
  FeObject* arguments[] = {FeMakeDouble(host->context, 41)};
  FeObject* result = FeCall(host->context, FeGetRoot(host->function), arguments,
                            sizeof(arguments) / sizeof(arguments[0]));
  printf("result: %.0f\n", FeToDouble(host->context, result));
  FeReleaseRoot(host->context, host->function);
  host->function = nullptr;
  FeRestoreGC(host->context, host->gc_checkpoint);
  host->succeeded = true;

close:
  FeCloseContext(host->context);
  const bool succeeded = host->succeeded;
  free(host);
  free(arena);
  return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
