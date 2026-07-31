// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <limits.h>
#include <stdlib.h>

#include "fex.h"
#include "fex_re.h"
#include "re.h"

struct FexRegex {
  unsigned char* storage;
  unsigned storage_size;
  re_t regex;
};

struct StatusInfo {
  const char* code;
  const char* message;
};

static void* Allocate(FeContext* ctx, size_t size, void* cleanup) {
  void* allocation = malloc(size);
  if (!allocation) {
    free(cleanup);
    FeHandleError(ctx, "out of memory");
  }
  return allocation;
}

void FexInstallRE(FeContext* ctx) {
  FexInstallNativeFn(ctx, "compile-re", FexCompileRE);
  FexInstallNativeFn(ctx, "match-re", FexMatchRE);
}

static re_flags ParseCompileFlags(FeContext* ctx, FeObject** arg) {
  re_flags flags = RE_FLAG_NONE;

  if (FeIsNil(*arg)) {
    return flags;
  }
  FeObject* list = FeGetNextArgument(ctx, arg);
  const FeObject* icase = FeMakeSymbol(ctx, "icase");
  while (!FeIsNil(list)) {
    const FeObject* flag = FeGetNextArgument(ctx, &list);
    if (flag == icase) {
      flags |= RE_FLAG_ICASE;
    } else {
      FeHandleError(ctx, "unknown regular-expression flag");
    }
  }
  return flags;
}

static struct StatusInfo GetStatusInfo(re_status status) {
  static const struct StatusInfo statuses[] = {
      [RE_STATUS_OK] = {"ok", "success"},
      [RE_STATUS_NO_MATCH] = {"no-match", "no match found"},
      [RE_STATUS_BAD_PATTERN] = {"bad-pattern",
                                 "bad pattern syntax or invalid escapes"},
      [RE_STATUS_TOO_COMPLEX] =
          {"too-complex", "backtracking limit exceeded or pattern too complex"},
      [RE_STATUS_BUFFER_TOO_SMALL] =
          {"buffer-too-small", "compiled regex buffer size was too small"},
  };
  static const struct StatusInfo unknown = {
      "unknown-error", "an unknown regular expression error occurred"};

  if ((unsigned)status >= sizeof(statuses) / sizeof(statuses[0])) {
    return unknown;
  }
  return statuses[status];
}

static FeObject* BuildError(FeContext* ctx,
                            const char* code_str,
                            const char* message_str) {
  return FeMakeList(
      ctx,
      (FeObject*[]){FeMakeSymbol(ctx, "error"), FeMakeSymbol(ctx, code_str),
                    FeMakeString(ctx, message_str)},
      3);
}

static FeObject* BuildStatusError(FeContext* ctx, re_status status) {
  struct StatusInfo info = GetStatusInfo(status);
  return BuildError(ctx, info.code, info.message);
}

FeObject* FexCompileRE(FeContext* ctx, FeObject* arg) {
  const FeObject* pattern_obj = FeGetNextArgument(ctx, &arg);
  re_flags flags = ParseCompileFlags(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  char* pattern = FexCopyStringZ(ctx, pattern_obj, NULL);

  unsigned storage_size = 0;
  re_t regex = NULL;
  /* Query required size first */
  re_status status =
      re_compile_checked(pattern, flags, NULL, &storage_size, &regex);
  if (status != RE_STATUS_BUFFER_TOO_SMALL) {
    free(pattern);
    return BuildStatusError(ctx, status);
  }

  unsigned char* storage = Allocate(ctx, storage_size, pattern);

  status = re_compile_checked(pattern, flags, storage, &storage_size, &regex);
  free(pattern);

  if (status != RE_STATUS_OK) {
    free(storage);
    return BuildStatusError(ctx, status);
  }

  struct FexRegex* rx = Allocate(ctx, sizeof(struct FexRegex), storage);

  rx->storage = storage;
  rx->storage_size = storage_size;
  rx->regex = regex;

  return FeMakePtr(ctx, FexTRE, rx);
}

static FeObject* BuildSpan(FeContext* ctx, const re_span* span) {
  if (span->start < 0) {
    return &nil;
  }
  FeObject* start_obj = FeMakeDouble(ctx, (double)span->start);
  FeObject* end_obj = FeMakeDouble(ctx, (double)span->end);
  return FeMakeList(ctx, (FeObject*[]){start_obj, end_obj}, 2);
}

FeObject* FexMatchRE(FeContext* ctx, FeObject* arg) {
  FeObject* o = FeGetNextArgument(ctx, &arg);
  if (FeGetType(o) != FexTRE) {
    FeHandleError(ctx, "not a regular-expression");
  }
  struct FexRegex* rx = FeToPtr(ctx, o);
  if (!rx) {
    FeHandleError(ctx, "invalid regular-expression pointer");
  }

  const FeObject* text_obj = FeGetNextArgument(ctx, &arg);
  int start_offset = 0;
  if (!FeIsNil(arg)) {
    double offset = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
    if (!(offset >= 0 && offset <= INT_MAX)) {
      FeHandleError(ctx, "invalid regular-expression offset");
    }
    start_offset = (int)offset;
  }
  FeRequireNoArguments(ctx, arg);
  char* text = FexCopyStringZ(ctx, text_obj, NULL);

  re_match_result match_res = {0};
  re_status status = re_exec(rx->regex, text, start_offset, &match_res);
  free(text);

  switch (status) {
    case RE_STATUS_OK: {
      FeObject* spans_list[RE_MAX_SPANS];
      for (int i = 0; i < match_res.nspans; i++) {
        spans_list[i] = BuildSpan(ctx, &match_res.spans[i]);
      }
      return FeMakeList(ctx, spans_list, (size_t)match_res.nspans);
    }
    case RE_STATUS_NO_MATCH:
      return &nil;
    case RE_STATUS_BAD_PATTERN:
    case RE_STATUS_TOO_COMPLEX:
    case RE_STATUS_BUFFER_TOO_SMALL:
      return BuildStatusError(ctx, status);
  }
  abort();
}

FeObject* FexGCRE(FeContext* ctx, FeObject* o) {
  struct FexRegex* rx = FeToPtr(ctx, o);
  if (rx) {
    free(rx->storage);
    free(rx);
  }
  return &nil;
}
