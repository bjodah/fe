// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <sys/types.h>

#include <errno.h>
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
  const FexFile* file = GetFile(ctx, arg);
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
  // Both arguments are taken before either is copied: FeGetNextArgument raises
  // on a missing one, and doing that while holding an allocation would lose it.
  const FeObject* path_object = FeGetNextArgument(ctx, &arg);
  const FeObject* mode_object = FeGetNextArgument(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  char* pathname = FexCopyStringZ(ctx, path_object, NULL);
  char* mode = FexCopyStringZ(ctx, mode_object, pathname);
  FILE* file = fopen(pathname, mode);
  const int error = errno;
  free(mode);
  free(pathname);
  return file != NULL ? MakeFile(ctx, file, true) : BuildErrnoError(ctx, error);
}

FeObject* FexReadFile(FeContext* ctx, FeObject* arg) {
  FILE* file = GetOpenFile(ctx, &arg);
  const FeObject* delimiter_object = FeGetNextArgument(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  // `getdelim` takes one byte, so anything else is refused rather than silently
  // reduced to its first byte -- which used to turn `nil` into `n`, because a
  // non-string was rendered by the printer first.
  if (FeGetType(delimiter_object) != FeTString) {
    FeHandleError(ctx, "delimiter is not a string");
  }
  char delimiter;
  if (!FeCopyStringBytes(ctx, delimiter_object, &delimiter, 1) ||
      FeStringByteLength(ctx, delimiter_object) != 1) {
    FeHandleError(ctx, "delimiter must be one byte");
  }

  char* record = NULL;
  size_t capacity = 0;
  const ssize_t r = getdelim(&record, &capacity, delimiter, file);
  const int error = errno;
  FeObject* result =
      r >= 0 ? FeMakeString(ctx, record) : BuildErrnoError(ctx, error);
  free(record);
  return result;
}

FeObject* FexRemoveFile(FeContext* ctx, FeObject* arg) {
  const FeObject* path_object = FeGetNextArgument(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  char* pathname = FexCopyStringZ(ctx, path_object, NULL);
  const bool removed = remove(pathname) == 0;
  const int error = errno;
  free(pathname);
  return removed ? &nil : BuildErrnoError(ctx, error);
}

// Writes the exact bytes of a string or symbol, not the printer's rendering of
// an arbitrary object, and not through a 4 MiB buffer that silently truncated
// anything longer and read errno even on success. Returns the byte count.
FeObject* FexWriteFile(FeContext* ctx, FeObject* arg) {
  FILE* file = GetOpenFile(ctx, &arg);
  const FeObject* object = FeGetNextArgument(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  const size_t length = FeStringByteLength(ctx, object);
  char* bytes = FexCopyStringZ(ctx, object, NULL);

  size_t written = 0;
  int error = 0;
  while (written < length) {
    const size_t n = fwrite(bytes + written, 1, length - written, file);
    if (n == 0) {
      error = ferror(file) != 0 ? errno : 0;
      break;
    }
    written += n;
  }
  free(bytes);
  if (written == length) {
    return FeMakeDouble(ctx, (double)written);
  }
  return error != 0 ? BuildErrnoError(ctx, error)
                    : FeMakeDouble(ctx, (double)written);
}
