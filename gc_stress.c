// Copyright 2026 the kg authors
// SPDX-License-Identifier: MIT
//
// The collector's own regression test, in the two builds that make it one.
//
// Built twice by `make check-gc-stress`: once with the ordinary flags, once
// with `-DFE_GC_STRESS=1`, which makes `MakeObject` collect before every
// allocation. The two runs assert different things about the *same* script,
// and it is the pair that carries the value:
//
//   * both builds: a heavy-allocation script drives the collection count in
//     `FeGetArenaStats` above zero, and answers correctly while doing it.
//     This is the cheap standing assertion that the collector is invoked at
//     all -- a GC that is shipped but never actually runs looks exactly like
//     a working one to every other test in the suite.
//   * the stress build only: the collection count exceeds the allocation
//     count of the script by construction, and every answer is still right.
//     An object that is live only through an unrooted C local survives an
//     ordinary run by luck -- nothing collected between its creation and its
//     last use -- and dies here on the next allocation, which is the whole
//     point of the knob.
//
// Deliberately its own binary rather than a case inside `test_api`: stress
// makes each allocation O(arena), so it is affordable over a few thousand
// allocations in a small arena and not over `test_api`'s hundreds of
// thousands in a megabyte one.

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"

static_assert(FE_API_VERSION == 12);
static_assert(FE_LANGUAGE_VERSION == 14);

#ifndef FE_GC_STRESS
#define FE_GC_STRESS 0
#endif

// Small on purpose: every collection walks the whole arena, so the arena
// size is this program's running time. Large enough that the churn script
// below needs several collections in an ordinary build rather than exactly
// one.
enum { StressArenaSize = 96 * 1024 };

typedef struct StressState {
  jmp_buf error_jump;
  char error[256];
} StressState;

static StressState state;

[[noreturn]] static void HandleError(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    const char* message,
    // cppcheck-suppress constParameterCallback
    FeObject* call_trace) {
  StressState* host = FeGetUserData(context);
  (void)call_trace;
  (void)snprintf(host->error, sizeof(host->error), "%s", message);
  longjmp(host->error_jump, 1);
}

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      (void)fprintf(stderr, "gc_stress: failed: %s at %s:%d\n", #condition, \
                    __FILE__, __LINE__);                                    \
      return false;                                                         \
    }                                                                       \
  } while (false)

// The script: a loop that conses per iteration and keeps nothing, plus a
// list built up across the loop and read afterwards. The garbage is what
// drives the collector; the retained list is what proves a collection under
// stress did not take something that was still live.
static const char churn[] =
    "(setq kept nil)\n"
    "(setq n 0)\n"
    "(while (< n 400)\n"
    "  (setq n (+ n 1))\n"
    "  (cons (list n n n) (list n n))\n"
    "  (if (= (- n (* (/ n 100) 100)) 0) (setq kept (cons n kept)) nil))\n"
    "(list n kept (car kept))";

// Phase 14's own seams, which are the reason this lane exists at all rather
// than only the reason it was built: an UNINTERNED symbol is the first
// symbol fe has that the collector may reclaim, so `make-symbol`/`gensym`
// manufacture live-then-dead symbol objects by the hundred here, and `put`
// appends two fresh pairs onto a plist whose symbol is reachable only
// through the caller's operand list. What is asserted afterwards is that the
// properties written across all that churn are still readable and that the
// name a retained uninterned symbol carries is still its own.
static const char symbols[] =
    "(setq acc nil)\n"
    "(setq keeper (make-symbol \"kept-name\"))\n"
    "(setq k 0)\n"
    "(while (< k 200)\n"
    "  (setq k (+ k 1))\n"
    "  (gensym \"tmp\")\n"
    "  (make-symbol \"throwaway\")\n"
    "  (intern-soft \"never-interned-here\")\n"
    "  (if (= (- k (* (/ k 50) 50)) 0)\n"
    "      (do (put 'gcprobe (intern \"p\") k)\n"
    "          (setq acc (cons (get 'gcprobe 'p) acc)))\n"
    "    nil))\n"
    "(list k acc (get 'gcprobe 'p) (intern-soft \"never-interned-here\")\n"
    "      (symbol-name keeper) (intern-soft keeper))";

static bool RunStress(void) {
  static alignas(max_align_t) unsigned char arena[StressArenaSize];
  FeContext* context = FeOpenContext(arena, sizeof(arena));
  CHECK(context != nullptr);
  FeSetUserData(context, &state);
  FeSetErrorFn(context, HandleError);
  if (setjmp(state.error_jump) != 0) {
    (void)fprintf(stderr, "gc_stress: unexpected Fe error: %s\n", state.error);
    return false;
  }

  // Not `== 0`: the stress build has already collected some thousands of
  // times bootstrapping the context, which is itself the knob working.
  const FeArenaStats before = FeGetArenaStats(context);
#if !FE_GC_STRESS
  CHECK(before.collection_count == 0);
#endif

  FeObject* const result =
      FeEvaluateString(context, "gc-stress.fe", churn, sizeof(churn) - 1);
  char rendered[128];
  (void)FeToString(context, result, rendered, sizeof(rendered));
  CHECK(strcmp(rendered, "(400 (400 300 200 100) 400)") == 0);

  FeObject* const symbol_result =
      FeEvaluateString(context, "gc-stress.fe", symbols, sizeof(symbols) - 1);
  (void)FeToString(context, symbol_result, rendered, sizeof(rendered));
  CHECK(strcmp(rendered, "(200 (200 150 100 50) 200 nil \"kept-name\" nil)") ==
        0);

  const FeArenaStats after = FeGetArenaStats(context);
  // The standing assertion, true in both builds: the collector ran over the
  // script, not merely at some point during bootstrap.
  CHECK(after.collection_count > before.collection_count);
  CHECK(after.allocation_failures == 0);
#if FE_GC_STRESS
  // And the knob is really on: 400 iterations allocate far more than 400
  // objects, and under stress each of those is its own collection, so the
  // count over the script cannot be in the handful an ordinary run needs
  // (measured: 2).
  CHECK(after.collection_count - before.collection_count > 400);
#endif
  (void)printf("gc_stress: FE_GC_STRESS=%d, %zu collection(s), peak %zu live\n",
               FE_GC_STRESS, after.collection_count, after.peak_live_objects);

  FeCloseContext(context);
  return true;
}

int main(void) {
  return RunStress() ? EXIT_SUCCESS : EXIT_FAILURE;
}
