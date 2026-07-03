// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FEX_H
#define FEX_H

#include "fe.h"

extern const char* FexVersion;

#define FexTFile FeTFex0
#define FexTRE FeTFex1

void FexInit(FeContext* ctx);
FeObject* BuildErrnoError(FeContext* ctx, int error);
void FexInstallNativeFn(FeContext* ctx, const char* name, FeNativeFn fn);

#endif
