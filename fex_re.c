// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "fex.h"
#include "fex_re.h"
#include "re.h"

struct FexRegex {
  unsigned char* storage;
  unsigned storage_size;
  re_t regex;
};

void FexInstallRE(FeContext* ctx) {
  FexInstallNativeFn(ctx, "compile-re", FexCompileRE);
  FexInstallNativeFn(ctx, "match-re", FexMatchRE);
}

static char* CopyStringZ(FeContext* ctx, FeObject* obj) {
  size_t len = FeStringByteLength(ctx, obj);
  char* bytes = malloc(len + 1);
  if (!bytes) {
    FeHandleError(ctx, "out of memory");
  }
  if (!FeCopyStringBytes(ctx, obj, bytes, len + 1)) {
    free(bytes);
    FeHandleError(ctx, "failed to copy string bytes");
  }
  bytes[len] = '\0';
  return bytes;
}

static re_flags ParseCompileFlags(FeContext* ctx, FeObject** arg) {
  re_flags flags = RE_FLAG_NONE;

  if (FeIsNil(*arg)) {
    return flags;
  }
  FeObject* list = FeGetNextArgument(ctx, arg);
  FeObject* icase = FeMakeSymbol(ctx, "icase");
  while (!FeIsNil(list)) {
    FeObject* flag = FeGetNextArgument(ctx, &list);
    if (flag == icase) {
      flags |= RE_FLAG_ICASE;
    } else {
      FeHandleError(ctx, "unknown regular-expression flag");
    }
  }
  return flags;
}

static const char* status_to_code(re_status status) {
  switch (status) {
    case RE_STATUS_BAD_PATTERN:
      return "bad-pattern";
    case RE_STATUS_TOO_COMPLEX:
      return "too-complex";
    case RE_STATUS_BUFFER_TOO_SMALL:
      return "buffer-too-small";
    case RE_STATUS_NO_MATCH:
      return "no-match";
    case RE_STATUS_OK:
      return "ok";
  }
  return "unknown-error";
}

static const char* status_to_message(re_status status) {
  switch (status) {
    case RE_STATUS_BAD_PATTERN:
      return "bad pattern syntax or invalid escapes";
    case RE_STATUS_TOO_COMPLEX:
      return "backtracking limit exceeded or pattern too complex";
    case RE_STATUS_BUFFER_TOO_SMALL:
      return "compiled regex buffer size was too small";
    case RE_STATUS_NO_MATCH:
      return "no match found";
    case RE_STATUS_OK:
      return "success";
  }
  return "an unknown regular expression error occurred";
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

FeObject* FexCompileRE(FeContext* ctx, FeObject* arg) {
  FeObject* pattern_obj = FeGetNextArgument(ctx, &arg);
  re_flags flags = ParseCompileFlags(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  char* pattern = CopyStringZ(ctx, pattern_obj);

  unsigned storage_size = 0;
  re_t regex = NULL;
  /* Query required size first */
  re_status status =
      re_compile_checked(pattern, flags, NULL, &storage_size, &regex);
  if (status != RE_STATUS_BUFFER_TOO_SMALL) {
    free(pattern);
    return BuildError(ctx, status_to_code(status), status_to_message(status));
  }

  unsigned char* storage = malloc(storage_size);
  if (!storage) {
    free(pattern);
    FeHandleError(ctx, "out of memory");
  }

  status = re_compile_checked(pattern, flags, storage, &storage_size, &regex);
  free(pattern);

  if (status != RE_STATUS_OK) {
    free(storage);
    return BuildError(ctx, status_to_code(status), status_to_message(status));
  }

  struct FexRegex* rx = malloc(sizeof(struct FexRegex));
  if (!rx) {
    free(storage);
    FeHandleError(ctx, "out of memory");
  }

  rx->storage = storage;
  rx->storage_size = storage_size;
  rx->regex = regex;

  return FeMakePtr(ctx, FexTRE, rx);
}

static FeObject* BuildSpan(FeContext* ctx, const re_span* span) {
  if (span->start < 0 || span->end < 0) {
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

  FeObject* text_obj = FeGetNextArgument(ctx, &arg);
  int start_offset = 0;
  if (!FeIsNil(arg)) {
    double offset = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
    if (!(offset >= 0 && offset <= INT_MAX)) {
      FeHandleError(ctx, "invalid regular-expression offset");
    }
    start_offset = (int)offset;
  }
  FeRequireNoArguments(ctx, arg);
  char* text = CopyStringZ(ctx, text_obj);

  re_match_result match_res;
  re_status status = re_exec(rx->regex, text, start_offset, &match_res);
  free(text);

  if (status == RE_STATUS_OK) {
    FeObject* spans_list[RE_MAX_SPANS];
    for (int i = 0; i < match_res.nspans; i++) {
      spans_list[i] = BuildSpan(ctx, &match_res.spans[i]);
    }
    return FeMakeList(ctx, spans_list, (size_t)match_res.nspans);
  } else if (status == RE_STATUS_NO_MATCH) {
    return &nil;
  } else {
    return BuildError(ctx, status_to_code(status), status_to_message(status));
  }
}

FeObject* FexGCRE(FeContext* ctx, FeObject* o) {
  struct FexRegex* rx = FeToPtr(ctx, o);
  if (rx) {
    free(rx->storage);
    free(rx);
  }
  return &nil;
}
