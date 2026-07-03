// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>

#include "fex.h"
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
    case FexTFile:
    case FeTFex2:
      return &nil;
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
  FeGetHandlers(ctx)->gc = FexGC;
}

FeObject* BuildErrnoError(FeContext* ctx, int error) {
  return FeMakeList(ctx,
                    (FeObject*[]){FeMakeDouble(ctx, (double)error),
                                  FeMakeString(ctx, strerror(error))},
                    2);
}

void FexInstallNativeFn(FeContext* ctx, const char* name, FeNativeFn fn) {
  FeSet(ctx, FeMakeSymbol(ctx, name), FeMakeNativeFn(ctx, fn));
}
