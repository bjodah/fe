// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fex.h"
#include "fex_process.h"

void FexInstallProcess(FeContext* ctx) {
  FexInstallNativeFn(ctx, "execute", FexExecute);
}

static void FreeArguments(char** arguments, size_t count) {
  for (size_t i = 0; i < count; i++) {
    free(arguments[i]);
  }
  free(arguments);
}

// Counts the arguments and checks their types before anything is allocated, so
// the copying loop below cannot raise except on out of memory. The old code
// stopped at argument 32 with a bare `break`, so a longer command line was
// silently executed short.
static size_t CountArguments(FeContext* ctx, FeObject* arg, size_t maximum) {
  size_t count = 0;
  while (!FeIsNil(arg)) {
    const FeObject* item = FeGetNextArgument(ctx, &arg);
    if (FeGetType(item) != FeTString) {
      FeHandleError(ctx, "not a string");
    }
    count++;
    if (count > maximum) {
      FeHandleError(ctx, "too many arguments");
    }
  }
  if (count == 0) {
    FeHandleError(ctx, "not enough arguments");
  }
  return count;
}

// Exit status, or 128 + the terminating signal, as a shell reports it; -1 if
// the child could not be started or waited for.
static int RunChild(char* const* arguments) {
  const pid_t child = fork();
  if (child == 0) {
    execvp(arguments[0], arguments);
    perror("execvp");
    _exit(127);
  }
  if (child == -1) {
    perror("fork");
    return -1;
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  if (waited == -1) {
    perror("waitpid");
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

FeObject* FexExecute(FeContext* ctx, FeObject* arg) {
  enum { MaxArgumentCount = 4096 };
  const size_t count = CountArguments(ctx, arg, MaxArgumentCount);

  char** arguments = calloc(count + 1, sizeof(char*));
  if (arguments == NULL) {
    FeHandleError(ctx, "out of memory");
  }
  for (size_t i = 0; i < count; i++) {
    // Exact bytes, not a 1024-byte rendering of them.
    arguments[i] = FexCopyStringZ(ctx, FeGetNextArgument(ctx, &arg), NULL);
  }

  const int status = RunChild(arguments);
  FreeArguments(arguments, count);
  return FeMakeDouble(ctx, (double)status);
}
