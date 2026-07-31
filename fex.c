// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

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
