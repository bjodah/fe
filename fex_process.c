// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <sys/types.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
}

FeObject* FexExecute(FeContext* ctx, FeObject* arg) {
  enum { MaxArgumentCount = 31 };
  char* arguments[MaxArgumentCount + 1] = {NULL};
  size_t i;
  for (i = 0; i < MaxArgumentCount; i++) {
    if (FeIsNil(arg)) {
      break;
    }
    FeObject* a = FeGetNextArgument(ctx, &arg);
    const FeType type = FeGetType(a);
    if (type != FeTString) {
      FreeArguments(arguments, i);
      FeHandleError(ctx, "not a string");
    }
    char string[1024];
    FeToString(ctx, a, string, sizeof(string));
    arguments[i] = strdup(string);
    if (arguments[i] == NULL) {
      FreeArguments(arguments, i);
      FeHandleError(ctx, "out of memory");
    }
  }

  if (i == 0) {
    FeHandleError(ctx, "not enough arguments");
  }

  int status = -1;
  const pid_t child = fork();
  if (child == 0) {
    execvp(arguments[0], arguments);
    perror("execvp");
    _exit(127);
  } else if (child == -1) {
    perror("fork");
  } else {
    if (waitpid(child, &status, 0) == -1) {
      perror("waitpid");
    } else {
      status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
  }

  FreeArguments(arguments, i);
  return FeMakeDouble(ctx, (double)status);
}
