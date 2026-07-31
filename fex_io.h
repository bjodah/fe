// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FEX_IO_H
#define FEX_IO_H

#include "fe.h"

void FexInstallIO(FeContext* ctx);

FeObject* FexCloseFile(FeContext* ctx, FeObject* arg);
FeObject* FexOpenFile(FeContext* ctx, FeObject* arg);
FeObject* FexReadFile(FeContext* ctx, FeObject* arg);
FeObject* FexRemoveFile(FeContext* ctx, FeObject* arg);
FeObject* FexWriteFile(FeContext* ctx, FeObject* arg);
FeObject* FexGCFile(FeContext* ctx, FeObject* o);

#endif
