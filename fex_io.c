// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <sys/types.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "fex.h"
#include "fex_io.h"

// An `FexTFile` cell holds one of these rather than a bare `FILE*`, so that a
// closed file stays identifiable, a double close is an error instead of
// undefined behaviour, and the standard streams cannot be closed at all.
typedef struct FexFile {
  FILE* file;
  bool owned;
  bool closed;
} FexFile;

static FexFile* GetFile(FeContext* ctx, FeObject** arg) {
  FeObject* object = FeGetNextArgument(ctx, arg);
  if (FeGetType(object) != FexTFile) {
    FeHandleError(ctx, "not a file");
  }
  FexFile* file = FeToPtr(ctx, object);
  if (file == NULL) {
    FeHandleError(ctx, "not a file");
  }
  return file;
}

static FILE* GetOpenFile(FeContext* ctx, FeObject** arg) {
  FexFile* file = GetFile(ctx, arg);
  if (file->closed) {
    FeHandleError(ctx, "file is closed");
  }
  return file->file;
}

// NOTE: `FeMakePtr` can collect and therefore raise, and `FeHandleError` does
// not return, so the wrapper allocated just above it is lost when it does. That
// is the concrete case for a C-side cleanup stack that survives the longjmp;
// until there is one, the loss is bounded by one wrapper and one descriptor on
// an out-of-memory error.
static FeObject* MakeFile(FeContext* ctx, FILE* stream, bool owned) {
  FexFile* file = malloc(sizeof(FexFile));
  if (file == NULL) {
    if (owned) {
      (void)fclose(stream);
    }
    FeHandleError(ctx, "out of memory");
  }
  *file = (FexFile){.file = stream, .owned = owned, .closed = false};
  return FeMakePtr(ctx, FexTFile, file);
}

// Runs from the collector on every swept `FexTFile`, including the sweep
// `FeCloseContext` performs, so an unclosed file is closed exactly once instead
// of leaking its descriptor for the life of the process. It allocates no Fe
// objects and is not reentrant.
FeObject* FexGCFile(FeContext* ctx, FeObject* o) {
  FexFile* file = FeToPtr(ctx, o);
  if (file != NULL) {
    if (file->owned && !file->closed) {
      (void)fclose(file->file);
    }
    free(file);
  }
  return FeNil(ctx);
}

void FexInstallIO(FeContext* ctx) {
  FexInstallNativeFn(ctx, "close-file", FexCloseFile);
  FexInstallNativeFn(ctx, "open-file", FexOpenFile);
  FexInstallNativeFn(ctx, "read-file", FexReadFile);
  FexInstallNativeFn(ctx, "remove-file", FexRemoveFile);
  FexInstallNativeFn(ctx, "write-file", FexWriteFile);

  // The standard streams belong to the host, so they are not owned and cannot
  // be closed from Lisp.
  FeSet(ctx, FeMakeSymbol(ctx, "stdin"), MakeFile(ctx, stdin, false));
  FeSet(ctx, FeMakeSymbol(ctx, "stdout"), MakeFile(ctx, stdout, false));
  FeSet(ctx, FeMakeSymbol(ctx, "stderr"), MakeFile(ctx, stderr, false));
}

FeObject* FexCloseFile(FeContext* ctx, FeObject* arg) {
  FexFile* file = GetFile(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  if (!file->owned) {
    FeHandleError(ctx, "cannot close a standard stream");
  }
  if (file->closed) {
    FeHandleError(ctx, "file is closed");
  }
  // Marked before the call: whether or not `fclose` reports an error, the
  // stream is gone and must never be handed to stdio again.
  file->closed = true;
  return fclose(file->file) == 0 ? &nil : BuildErrnoError(ctx, errno);
}

FeObject* FexOpenFile(FeContext* ctx, FeObject* arg) {
  char pathname[PATH_MAX + 1];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), pathname,
                   sizeof(pathname));
  char mode[8];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), mode, sizeof(mode));
  FILE* file = fopen(pathname, mode);
  return file != NULL ? MakeFile(ctx, file, true) : BuildErrnoError(ctx, errno);
}

FeObject* FexReadFile(FeContext* ctx, FeObject* arg) {
  FILE* file = GetOpenFile(ctx, &arg);
  char delimiter[16];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), delimiter,
                   sizeof(delimiter));

  char* record = NULL;
  size_t capacity = 0;
  const ssize_t r = getdelim(&record, &capacity, delimiter[0], file);
  const int error = errno;
  FeObject* result =
      r >= 0 ? FeMakeString(ctx, record) : BuildErrnoError(ctx, error);
  free(record);
  return result;
}

FeObject* FexRemoveFile(FeContext* ctx, FeObject* arg) {
  char pathname[PATH_MAX + 1];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), pathname,
                   sizeof(pathname));
  return remove(pathname) == 0 ? &nil : BuildErrnoError(ctx, errno);
}

FeObject* FexWriteFile(FeContext* ctx, FeObject* arg) {
  FILE* file = GetOpenFile(ctx, &arg);
  const size_t arbitrary_limit = 4 * 1024 * 1024;  // TODO
  char* buffer = malloc(arbitrary_limit);
  if (buffer == NULL) {
    FeHandleError(ctx, "out of memory");
  }
  const size_t size =
      FeToString(ctx, FeGetNextArgument(ctx, &arg), buffer, arbitrary_limit);
  // cppcheck-suppress nullPointerOutOfMemory
  const size_t written = fwrite(buffer, 1, size, file);
  const int error = errno;
  FeObject* result = written == size ? FeMakeDouble(ctx, (double)written)
                                     : BuildErrnoError(ctx, error);
  free(buffer);
  return result;
}
