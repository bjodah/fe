// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <sys/types.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "fex.h"
#include "fex_io.h"

static FeObject* GetFile(FeContext* ctx, FeObject** arg) {
  FeObject* file = FeGetNextArgument(ctx, arg);
  if (FeGetType(file) != FexTFile) {
    FeHandleError(ctx, "not a file");
  }
  return file;
}

void FexInstallIO(FeContext* ctx) {
  FexInstallNativeFn(ctx, "close-file", FexCloseFile);
  FexInstallNativeFn(ctx, "open-file", FexOpenFile);
  FexInstallNativeFn(ctx, "read-file", FexReadFile);
  FexInstallNativeFn(ctx, "remove-file", FexRemoveFile);
  FexInstallNativeFn(ctx, "write-file", FexWriteFile);

  FeSet(ctx, FeMakeSymbol(ctx, "stdin"), FeMakePtr(ctx, FexTFile, stdin));
  FeSet(ctx, FeMakeSymbol(ctx, "stdout"), FeMakePtr(ctx, FexTFile, stdout));
  FeSet(ctx, FeMakeSymbol(ctx, "stderr"), FeMakePtr(ctx, FexTFile, stderr));
}

FeObject* FexCloseFile(FeContext* ctx, FeObject* arg) {
  return fclose(FeToPtr(ctx, GetFile(ctx, &arg))) == 0
             ? &nil
             : BuildErrnoError(ctx, errno);
}

FeObject* FexOpenFile(FeContext* ctx, FeObject* arg) {
  char pathname[PATH_MAX + 1];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), pathname,
                   sizeof(pathname));
  char mode[8];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), mode, sizeof(mode));
  FILE* file = fopen(pathname, mode);
  return file != NULL ? FeMakePtr(ctx, FexTFile, file)
                      : BuildErrnoError(ctx, errno);
}

FeObject* FexReadFile(FeContext* ctx, FeObject* arg) {
  FeObject* file = GetFile(ctx, &arg);
  char delimiter[16];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), delimiter,
                   sizeof(delimiter));

  char* record = NULL;
  size_t capacity = 0;
  const ssize_t r =
      getdelim(&record, &capacity, delimiter[0], FeToPtr(ctx, file));
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
  FeObject* file = GetFile(ctx, &arg);
  const size_t arbitrary_limit = 4 * 1024 * 1024;  // TODO
  char* buffer = malloc(arbitrary_limit);
  if (buffer == NULL) {
    FeHandleError(ctx, "out of memory");
  }
  const size_t size =
      FeToString(ctx, FeGetNextArgument(ctx, &arg), buffer, arbitrary_limit);
  // cppcheck-suppress nullPointerOutOfMemory
  const size_t written = fwrite(buffer, 1, size, FeToPtr(ctx, file));
  const int error = errno;
  FeObject* result = written == size ? FeMakeDouble(ctx, (double)written)
                                     : BuildErrnoError(ctx, error);
  free(buffer);
  return result;
}
