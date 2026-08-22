// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FEX_H
#define FEX_H

#include "fe.h"

extern const char* FexVersion;

#define FexTFile FeTFex0
#define FexTRE FeTFex1

void FexInit(FeContext* ctx);
// The collector callback `FexInit` installs, exported so that a host which
// installs its OWN can chain to it instead of replacing it. `FeSetGCFn` holds
// one function, and the extension types own resources nothing else releases
// -- an owned `FILE*`, a compiled regular expression -- so a host that
// overwrites this one silently stops releasing them. `main.c`'s `-d` tracer
// did exactly that until the debug host joined `make check` and the valgrind
// and ASan lanes named the three leaked `FexTFile` records.
FeObject* FexGC(FeContext* ctx, FeObject* o);
FeObject* BuildErrnoError(FeContext* ctx, int error);
void FexInstallNativeFn(FeContext* ctx, const char* name, FeNativeFn fn);
[[nodiscard]] char* FexCopyStringZ(FeContext* ctx,
                                   const FeObject* obj,
                                   void* cleanup);

#endif
