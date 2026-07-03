// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <errno.h>
#include <time.h>

#include "fex.h"
#include "fex_time.h"

void FexInstallTime(FeContext* ctx) {
  FexInstallNativeFn(ctx, "get-time", FexGetTime);
}

FeObject* FexGetTime(FeContext* ctx, FeObject*) {
  struct timespec time;
  return clock_gettime(CLOCK_REALTIME, &time) == 0
             ? FeMakeList(
                   ctx,
                   (FeObject*[]){FeMakeDouble(ctx, (double)time.tv_sec),
                                 FeMakeDouble(ctx, (double)time.tv_nsec)},
                   2)
             : BuildErrnoError(ctx, errno);
}
