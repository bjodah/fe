#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"

static_assert(FE_API_VERSION == 10);
static_assert(FE_LANGUAGE_VERSION == 13);

typedef struct HostState {
  jmp_buf error_jump;
  FeContext* context;
  FeRoot* function;
  size_t gc_checkpoint;
  char error[128];
  char condition[128];
  FeCompletion completion;
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
  // Decision 5's accessors, read where a host actually needs them: inside
  // the error callback, to tell an ordinary error from a quit or from one of
  // fe's own ceilings without parsing the message text. The condition object
  // is `(SYMBOL . DATA)`; a quit's is `(quit)`; a budget completion has none.
  host->completion = FeGetCompletion(context);
  FeObject* condition = FeGetCondition(context);
  (void)FeToString(context, condition, host->condition,
                   sizeof(host->condition));
  longjmp(host->error_jump, 1);
}

// The hook-shaped native: it runs a callable it was handed inside a
// protected call, so a broken hook cannot unwind the host. A completion is
// returned rather than thrown past this frame, and the accessors describe
// it; here the host simply reports and swallows it, which is containment.
static FeObject* CallContained(FeContext* context, FeObject* arguments) {
  FeObject* callable = FeGetNextArgument(context, &arguments);
  FeRequireNoArguments(context, arguments);
  FeObject* value = FeNil(context);
  if (FeTryCallWithOptions(context, callable, nullptr, 0, nullptr, &value)) {
    return value;
  }
  char rendered[128];
  (void)FeToString(context, FeGetCondition(context), rendered,
                   sizeof(rendered));
  const FeCompletion kind = FeGetCompletion(context);
  printf("contained: %s (kind %d, condition %s)\n",
         FeGetCompletionMessage(context), (int)kind, rendered);
  return FeNil(context);
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
    printf("recovered: %s (kind %d, condition %s)\n", host->error,
           (int)host->completion, host->condition);
    goto recovered;
  }

  FeDefineNative(host->context, "increment", Increment);
  FeDefineNative(host->context, "call-contained", CallContained);
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

  // The protected call in context: the hook raises, the host reports it and
  // carries on, and the evaluation that invoked the hook still returns.
  static const char contained[] =
      "(do (call-contained (lambda () (car 1))) 'still-running)";
  FeObject* after = FeEvaluateString(host->context, "example", contained,
                                     sizeof(contained) - 1);
  char rendered[64];
  (void)FeToString(host->context, after, rendered, sizeof(rendered));
  printf("after containment: %s\n", rendered);

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
