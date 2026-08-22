// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fex.h"
#include "fex_process.h"

void FexInstallProcess(FeContext* ctx) {
  FexInstallNativeFn(ctx, "execute", FexExecute);
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

// Sizes the argv block -- `count + 1` argument pointers, then every argument's
// bytes with its terminator -- and allocates it as ONE block. One block is
// what makes the embedded-NUL refusal in `CopyArguments` cleanup-safe: string
// bytes are reachable only through their object, so the refusal can only fire
// on the copy, and there it has to release the vector and every earlier
// argument before transferring control. Per-argument `FexCopyStringZ` calls
// raised past all of that and lost them.
static char** AllocateArguments(FeContext* ctx, FeObject* arg, size_t count) {
  size_t bytes = 0;
  size_t size = 0;
  bool overflow = false;
  for (FeObject* rest = arg; !FeIsNil(rest);) {
    const size_t length =
        FeStringByteLength(ctx, FeGetNextArgument(ctx, &rest));
    overflow |= ckd_add(&bytes, bytes, length);
    overflow |= ckd_add(&bytes, bytes, 1);
  }
  overflow |= ckd_mul(&size, count + 1, sizeof(char*));
  overflow |= ckd_add(&size, size, bytes);
  if (overflow) {
    FeHandleError(ctx, "string too long");
  }
  char** arguments = malloc(size);
  if (arguments == NULL) {
    FeHandleError(ctx, "out of memory");
  }
  return arguments;
}

// Copies each argument into the block -- exact bytes, not a 1024-byte
// rendering of them -- and refuses an embedded NUL by name, the same refusal
// `FexCopyStringZ` gives the single-string boundaries, after releasing the
// whole vector.
static void CopyArguments(FeContext* ctx,
                          FeObject* arg,
                          char** arguments,
                          size_t count) {
  char* const bytes = (char*)(arguments + count + 1);
  size_t offset = 0;
  for (size_t i = 0; i < count; i++) {
    const FeObject* item = FeGetNextArgument(ctx, &arg);
    const size_t length = FeStringByteLength(ctx, item);
    arguments[i] = bytes + offset;
    offset += length + 1;
    if (!FeCopyStringBytes(ctx, item, arguments[i], length)) {
      free(arguments);
      FeHandleError(ctx, "failed to copy string bytes");
    }
    if (memchr(arguments[i], '\0', length) != NULL) {
      free(arguments);
      FeHandleError(ctx, "string contains an embedded NUL byte");
    }
    arguments[i][length] = '\0';
  }
  arguments[count] = NULL;
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
  char** arguments = AllocateArguments(ctx, arg, count);
  CopyArguments(ctx, arg, arguments, count);

  const int status = RunChild(arguments);
  free(arguments);
  return FeMakeDouble(ctx, (double)status);
}
