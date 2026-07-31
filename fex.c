// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

#include "fex.h"
#include "fex_io.h"
#include "fex_re.h"

const char* FexVersion = "0.1";

static FeObject* FexGC(FeContext* ctx, FeObject* o) {
  switch (FeGetType(o)) {
    case FeTPair:
    case FeTFree:
    case FeTNil:
    case FeTDouble:
    case FeTSymbol:
    case FeTString:
    case FeTFn:
    case FeTMacro:
    case FeTPrimitive:
    case FeTNativeFn:
    case FeTPtr:
    case FeTFex2:
      return &nil;
    case FexTFile:
      return FexGCFile(ctx, o);
    case FexTRE:
      return FexGCRE(ctx, o);
    case FeTSentinel:
      abort();
  }
  abort();
}

void FexInit(FeContext* ctx) {
  type_names[FexTFile] = "file";
  type_names[FexTRE] = "regular-expression";
  FeSetGCFn(ctx, FexGC);
}

FeObject* BuildErrnoError(FeContext* ctx, int error) {
  return FeMakeList(ctx,
                    (FeObject*[]){FeMakeDouble(ctx, (double)error),
                                  FeMakeString(ctx, strerror(error))},
                    2);
}

void FexInstallNativeFn(FeContext* ctx, const char* name, FeNativeFn fn) {
  FeDefineNative(ctx, name, fn);
}

// The exact bytes of a string or symbol, NUL terminated, in storage the caller
// frees. Unlike `FeToString` this is not the printer: it does not quote, does
// not escape, and cannot truncate. `cleanup` is freed before raising, which is
// how a caller that already holds an allocation hands it over; `FeHandleError`
// does not return, so there is no other way to release it.
char* FexCopyStringZ(FeContext* ctx, const FeObject* obj, void* cleanup) {
  const size_t length = FeStringByteLength(ctx, obj);
  size_t size;
  if (ckd_add(&size, length, 1)) {
    free(cleanup);
    FeHandleError(ctx, "string too long");
  }
  char* bytes = malloc(size);
  if (bytes == NULL) {
    free(cleanup);
    FeHandleError(ctx, "out of memory");
  }
  if (!FeCopyStringBytes(ctx, obj, bytes, length)) {
    free(bytes);
    free(cleanup);
    FeHandleError(ctx, "failed to copy string bytes");
  }
  bytes[length] = '\0';
  return bytes;
}
