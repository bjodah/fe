// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "fex.h"
#include "fex_re.h"

void FexInstallRE(FeContext* ctx) {
  FexInstallNativeFn(ctx, "compile-re", FexCompileRE);
  FexInstallNativeFn(ctx, "match-re", FexMatchRE);

  // TODO: Any constants
}

enum {
  ArbitraryRELengthLimit = 4096,
  ArbitraryDataLengthLimit = 4 * 1024 * 1024,
  ArbitraryMatchCount = 16,
};

static FeObject* BuildError(FeContext* ctx, int error, regex_t* re) {
  char message[1024];
  (void)regerror((error), (re), message, sizeof(message));
  return FeMakeList(
      ctx,
      (FeObject*[]){FeMakeDouble(ctx, (error)), FeMakeString((ctx), message)},
      2);
}

FeObject* FexCompileRE(FeContext* ctx, FeObject* arg) {
  char pattern[ArbitraryRELengthLimit + 1];
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), pattern, sizeof(pattern));

  regex_t* re = calloc(1, sizeof(regex_t));
  if (re == NULL) {
    FeHandleError(ctx, "out of memory");
  }
  const int error = regcomp(re, pattern, REG_EXTENDED);
  if (error == 0) {
    return FeMakePtr(ctx, FexTRE, re);
  }
  FeObject* result = BuildError(ctx, error, re);
  free(re);
  return result;
}

static FeObject* BuildMatchResult(FeContext* ctx,
                                  char* buffer,
                                  const regmatch_t* matches) {
  FeObject* substrings[ArbitraryMatchCount] = {NULL};
  size_t count;
  for (count = 0; count < ArbitraryMatchCount; count++) {
    const regmatch_t m = matches[count];
    if (m.rm_so == -1 || m.rm_eo == -1) {
      break;
    }
    char* s = strndup(buffer + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
    if (s == NULL) {
      FeHandleError(ctx, "out of memory");
    }
    substrings[count] = FeMakeString(ctx, s);
    free(s);
  }
  return FeMakeList(ctx, substrings, count);
}

FeObject* FexMatchRE(FeContext* ctx, FeObject* arg) {
  FeObject* o = FeGetNextArgument(ctx, &arg);
  if (FeGetType(o) != FexTRE) {
    FeHandleError(ctx, "not a regular-expression");
  }
  regex_t* re = FeToPtr(ctx, o);

  char* buffer = calloc(1, ArbitraryDataLengthLimit);
  if (buffer == NULL) {
    FeHandleError(ctx, "out of memory");
  }
  (void)FeToString(ctx, FeGetNextArgument(ctx, &arg), buffer,
                   ArbitraryDataLengthLimit);

  regmatch_t matches[ArbitraryMatchCount];
  const int error = regexec(re, buffer, ArbitraryMatchCount, matches, 0);
  FeObject* result = error == 0 ? BuildMatchResult(ctx, buffer, matches)
                                : BuildError(ctx, error, re);
  free(buffer);
  return result;
}

FeObject* FexGCRE(FeContext* ctx, FeObject* o) {
  regex_t* re = FeToPtr(ctx, o);
  regfree(re);
  free(re);
  return &nil;
}
