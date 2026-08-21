// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

#include "fex.h"
#include "fex_io.h"
#include "fex_re.h"

// The extension surface reports the Fe LANGUAGE version it rides on, not just
// its own number: Fex 0.1 stayed put across observable regex and string
// semantic changes, so a downstream user had nothing to assert against. The
// spelling is composed so the two cannot drift apart again.
const char* FexVersion = "0.2 (fe language " FE_LANGUAGE_VERSION_STRING ")";

FeObject* FexGC(FeContext* ctx, FeObject* o) {
  switch (FeGetType(o)) {
    case FeTPair:
    case FeTFree:
    case FeTNil:
    case FeTDouble:
    case FeTInteger:
    case FeTSymbol:
    case FeTString:
    case FeTVector:
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
// not escape, and cannot truncate. A string carrying an embedded NUL is
// refused loudly rather than silently reduced to its first segment at the
// C-string boundary this helper exists to cross -- every caller hands the
// result to an interface (fopen, remove, the regex compiler) that would stop
// at the NUL and behave as if the rest had never been there. `execute`'s argv
// lives in one block of its own instead (fex_process.c), so that the same
// refusal there can release the vector and every earlier argument. `cleanup`
// is freed before raising, which is how a caller that already holds an
// allocation hands it over; `FeHandleError` does not return, so there is no
// other way to release it.
static void RejectEmbeddedNul(FeContext* ctx,
                              char* bytes,
                              size_t length,
                              void* cleanup) {
  if (memchr(bytes, '\0', length) != NULL) {
    free(bytes);
    free(cleanup);
    FeHandleError(ctx, "string contains an embedded NUL byte");
  }
}

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
  RejectEmbeddedNul(ctx, bytes, length, cleanup);
  bytes[length] = '\0';
  return bytes;
}
