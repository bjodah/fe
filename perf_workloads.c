// Copyright 2026 Fe contributors
// SPDX-License-Identifier: MIT

// The in-process workload battery (Phase 21.2 of kg's
// doc/plans/2026-08-18-elisp-data-model.md).
//
// Phase 21 measures fe's engine before Phase 22 chooses a storage
// architecture. `fe_perf.h` (Phase 21.1) supplies the counters; this is the
// program that drives named shapes past them -- one shape per `FeContext`,
// counters reset around it -- and emits a machine-readable record for each.
//
// Three rules make the numbers mean something:
//
//   * EVERY WORKLOAD CHECKS ITS OWN ANSWER. A workload whose result is not
//     asserted is a workload that can silently stop doing its work while its
//     counters still look plausible.
//   * THE COUNTER ASSERTIONS ARE THE GATE; wall time is a report. A counter
//     is the same number on a loaded box, in a sanitizer lane and under
//     valgrind; elapsed time is not, and a gate that flakes gets switched
//     off. `seconds` is emitted beside every record and nothing reads it.
//   * A COUNTER READ FROM A DIFFERENTLY SIZED ARENA IS A DIFFERENT
//     MEASUREMENT. Each workload names its own arena and its cell capacity
//     travels with its counters. The fixed arena is measured where it
//     collects often (the two `gc-*` shapes, in a 96 KiB and a 256 KiB
//     arena) as well as where it does not.
//
// The assertions prefer RELATIONSHIPS to golden constants, and are chosen so
// that the phases after this one make them fail loudly and meaningfully
// rather than quietly: `string-*` pins the seven-byte cell chain Phase 25
// replaces, `intern-*` pins the linear `symbol_list` scan Phase 26 indexes,
// `env-*` pins the single flat alist Phase 28's first-class environments
// would split, and the two `gc-*` shapes pin the arena-proportional sweep.
//
// The GC-root stack trap, and which side of it each workload is on.
// `MakeObject` pushes every new object onto a fixed root stack (`GcStackSize`
// 4096 less `GcStackReserve` 64, so 4032 in practice), so a C loop that
// allocates a caller-controlled number of times overflows it. The C loops
// here (`intern-*`, `string-*`) take one `FeSaveGC` checkpoint and restore it
// every pass, which is the established idiom and is safe for interning
// because `symbol_list` is a permanent root. Everything whose size is the
// point of the workload -- the two `gc-*` shapes' lists, the arithmetic
// loop's trip count -- is written as a LISP loop instead, whose accumulator
// lives in a value cell the collector marks directly and which therefore has
// no such ceiling. That is a deliberate choice per workload: the C-loop
// workloads measure the cost of an operation, the Lisp-loop workloads
// measure the cost of a shape.

#include <stdio.h>
#include <stdlib.h>

#include "fe_perf.h"

#if FE_PERF_COUNTERS

#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <string.h>
#include <time.h>

#include "fe.h"
#include "fe_internal.h"

static_assert(FE_API_VERSION == 15);
static_assert(FE_LANGUAGE_VERSION == 20);

// The arena sizes, named rather than spelled at each use so that a record's
// `arena_bytes` can be read against the reason its workload picked it.
// Every cell figure below fell by about a quarter in Phase 25 and every byte
// figure stayed: `FeOpenContext` carves the payload region now, because a
// symbol's name is a string and a context without one cannot open. `ArenaSmall`
// is the one size that MOVED, 256 KiB -> 320 KiB, so that the dense-live shape
// keeps the cell budget it was designed around (11 646 against the 11 910 it
// had) rather than running out of memory two thirds of the way through its
// 4000 retained conses. The rest keep their byte sizes: what a byte size buys
// is exactly what this phase changed, and compensating every one of them would
// hide it.
enum {
  // Collects several times over a few thousand allocations: 1970 cells, of
  // which a bare context open already holds 866.
  ArenaTight = 96 * 1024,
  // Still collects under the dense-live shape, but with room for a live set
  // worth marking: 11646 cells.
  ArenaSmall = 320 * 1024,
  // kg's own configuration, and the one its bench cases are read against:
  // 42059 cells beside a 225 928-byte region.
  ArenaHost = 1024 * 1024,
  // 174770 cells. 8192 interned symbols do not fit in kg's arena -- a symbol
  // costs its own object, two cells of the name/function/value spine, its
  // `symbol_list` link and the one string object its name is -- and all three
  // intern tiers use this one size so that a cross-tier subtraction is apples
  // to apples.
  ArenaLarge = 4 * 1024 * 1024,
  ArenaMax = ArenaLarge,
};

static alignas(max_align_t) unsigned char arena_bytes[ArenaMax];

// The second arena. One workload -- `context-open-close` -- opens a context
// of its own INSIDE its measured region, and cannot use `arena_bytes`: the
// harness context it runs under is still open there, and is what `run->stats`
// reads afterwards. The counters, being global, see both. Sized at the arena
// that workload names, and asserted equal to it where it is opened.
static alignas(max_align_t) unsigned char scratch_arena_bytes[ArenaTight];

// Longest generated program: the depth-64 environment form.
enum { SourceMax = 8192 };
// The longest string workload, plus its NUL.
enum { StringMax = 8192 };
enum { ExtraMax = 8 };
enum { WorkloadMax = 32 };
// How many times each `env-*` form reads its first-bound variable.
enum { EnvLookups = 64 };

typedef struct HostState {
  jmp_buf jump;
  char message[256];
  bool raised;
} HostState;

static HostState host_state;

[[noreturn]] static void HandleError(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    const char* message,
    // cppcheck-suppress constParameterCallback
    FeObject* call_trace) {
  HostState* const host = FeGetUserData(context);
  (void)call_trace;
  host->raised = true;
  (void)snprintf(host->message, sizeof(host->message), "%s", message);
  // An `FeErrorFn` that RETURNS aborts the process, so a harness has to leave
  // through `longjmp`.
  longjmp(host->jump, 1);
}

#define CHECK(condition)                                             \
  do {                                                               \
    if (!(condition)) {                                              \
      (void)fprintf(stderr, "perf_workloads: failed: %s at %s:%d\n", \
                    #condition, __FILE__, __LINE__);                 \
      return false;                                                  \
    }                                                                \
  } while (false)

typedef struct Extra {
  const char* name;
  unsigned long long value;
} Extra;

typedef struct WorkloadRun {
  FeContext* context;
  // The counters over the measured region, snapshotted before anything the
  // checks do can perturb them.
  unsigned long long counters[FePerfCounterCount];
  FeArenaStats stats;
  // Cells the bare `FeOpenContext` cost. Reported with every record because
  // a counter is comparable only against another one taken from the same
  // starting state, and asserted equal across every arena size (see
  // `CheckOpenIsArenaIndependent`).
  unsigned long long open_cells;
  // Cells allocated before the measured region began -- the open plus
  // whatever `setup` did, or zero when the open IS the measured region. Not
  // a measurement: it is what reconciles the counters, which are a delta,
  // against `FeArenaStats`, which is absolute.
  unsigned long long baseline_cells;
  // Symbols interned before the measured region began, for the same reason:
  // the obarray a miss scans contains them too.
  unsigned long long baseline_symbols;
  // Region bytes held before the measured region began, for the same reason
  // again. Never zero since Phase 25: a symbol's name is a string, so the
  // open leaves one block per core name behind.
  unsigned long long baseline_payload_bytes;
  double seconds;
  // What the workload computed, rendered from C values only -- never through
  // `FeToString`, which would allocate inside the measured region.
  char answer[128];
  Extra extra[ExtraMax];
  size_t extra_count;
} WorkloadRun;

typedef struct Workload Workload;
struct Workload {
  const char* name;
  const char* family;
  const char* note;
  size_t arena;
  // The workload's own scale: symbol count, environment width or depth,
  // string length, or loop trip count.
  size_t param;
  // True for `context-open` alone: its measured region IS the open, so the
  // harness does not reset the counters after it. Every other workload,
  // `context-open-close` included, measures its body.
  bool measure_open;
  // Runs after the open and before the counters are reset: whatever a
  // workload needs in place but does not want to measure.
  bool (*setup)(const Workload* workload, WorkloadRun* run);
  bool (*body)(const Workload* workload, WorkloadRun* run);
  // Reads `run->counters`; may use `run->context` freely, since the snapshot
  // has already been taken.
  bool (*check)(const Workload* workload, const WorkloadRun* run);
};

static WorkloadRun runs[WorkloadMax];
static const Workload* ran[WorkloadMax];
static size_t ran_count;

static char source_buffer[SourceMax];
static char string_source[StringMax + 1];
static char string_copy[StringMax];
// The form the `env-*` workloads evaluate, read during setup so that the
// reader's own interning does not land in the measured region.
static FeRoot* prepared_form;

static void AddExtra(WorkloadRun* run,
                     const char* name,
                     unsigned long long value) {
  if (run->extra_count < ExtraMax) {
    run->extra[run->extra_count].name = name;
    run->extra[run->extra_count].value = value;
    run->extra_count++;
  }
}

static unsigned long long ExtraOf(const WorkloadRun* run, const char* name) {
  for (size_t i = 0; i < run->extra_count; i++) {
    if (strcmp(run->extra[i].name, name) == 0) {
      return run->extra[i].value;
    }
  }
  return ULLONG_MAX;
}

static unsigned long long CounterOf(const WorkloadRun* run,
                                    FePerfCounter counter) {
  return run->counters[counter];
}

static unsigned long long AllocOf(const WorkloadRun* run, FeType type) {
  return run->counters[FE_PERF_ALLOC_SLOT(type)];
}

static unsigned long long LiveAllocOf(FeType type) {
  return FePerfRead((FePerfCounter)FE_PERF_ALLOC_SLOT(type));
}

// Allocations by final type sum to total allocations, and the live arena is
// what was allocated and not yet reclaimed. Asserted for every workload
// rather than once, because it is what makes the by-type block a PARTITION
// of `alloc_object` rather than a set of unrelated numbers -- and therefore
// what makes "the top three sources of cell allocation" a well-posed
// question. `alloc_object` is a delta over the measured region while the
// arena gauge is absolute, which is exactly what `baseline_cells` corrects
// for; the reconciliation is what proves the two agree at all.
// An assertion that assumes an open, or a workload, collects a KNOWN number
// of times. The poison lane (`FE_DEBUG_PAYLOAD_MOVE`, armed by
// `.ci/ci-04-clang-asan-ubsan.sh`) breaks that premise by construction: it
// slides the live payload extent one alignment unit forward per allocation
// and leaves what is behind it unreachable until a collection returns the
// extent to the region's base, so an open -- which publishes one block per
// core symbol name since Phase 25 -- outruns a small region and collects to
// reclaim the drift. That lane is a CORRECTNESS lane and not a measurement
// one; every counter identity in this file still holds there, and only the
// "how many collections" ones cannot.
#if FE_DEBUG_PAYLOAD_MOVE
#define CHECK_UNPOISONED(condition) ((void)0)
#else
#define CHECK_UNPOISONED(condition) CHECK(condition)
#endif

static bool AllocationIsPartitioned(const WorkloadRun* run) {
  unsigned long long total = 0;
  for (int type = 0; type <= (int)FeTSentinel; type++) {
    total += run->counters[(int)FePerfAllocType + type];
  }
  CHECK(total == CounterOf(run, FePerfAllocObject));
  CHECK(CounterOf(run, FePerfAllocObject) + run->baseline_cells -
            CounterOf(run, FePerfGcReclaimed) ==
        (unsigned long long)(run->stats.total_slots - run->stats.free_slots));
  return true;
}

static double Now(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

// ---------------------------------------------------------------------------
// 21.2 item 1: a bare context open, and a bare open together with its close.
// Two workloads rather than one, because `FeCloseContext` is not a
// destructor small enough to hide inside the open's number: it clears every
// root and runs a full `CollectGarbage` over the whole arena, and the
// difference between these two records is what that costs. The harness runs
// the close of an ordinary workload's context AFTER the counters are
// snapshotted, so the open-only shape cannot measure it by accident and the
// open/close shape has to open a context of its own to measure it at all.
// ---------------------------------------------------------------------------

static bool BodyContextOpen(const Workload* workload, WorkloadRun* run) {
  (void)workload;
  // Nothing: `measure_open` makes the open itself the measured region. The
  // harness's own `FeCloseContext` runs after the snapshot, which is why
  // every check below is an open-only identity -- and why the close has a
  // workload of its own.
  (void)snprintf(run->answer, sizeof(run->answer), "opened");
  return true;
}

static bool CheckContextOpen(const Workload* workload, const WorkloadRun* run) {
  (void)workload;
  CHECK(AllocationIsPartitioned(run));
  // Opening a context never collects, so every cell it took is still live and
  // the arena's own free-slot accounting has to agree with the counter.
  CHECK_UNPOISONED(CounterOf(run, FePerfGcCollection) == 0);
  CHECK(CounterOf(run, FePerfAllocObject) ==
        (unsigned long long)(run->stats.total_slots - run->stats.free_slots));
  CHECK(run->stats.allocation_failures == 0);
  CHECK_UNPOISONED(run->stats.collection_count == 0);
  // The three slots no allocation can reach.
  CHECK(AllocOf(run, FeTFree) == 0);
  CHECK(AllocOf(run, FeTNil) == 0);
  CHECK(AllocOf(run, FeTSentinel) == 0);
  // A bare open makes no uninterned symbol, so every miss it takes creates
  // exactly one symbol and the two counters are the same measurement. It
  // also asks for a few names twice -- `fn` aliases `lambda`, and `error` is
  // both a primitive and a condition -- so there are strictly more lookups
  // than misses. Phase 26 changes what a lookup COSTS without changing
  // either relationship.
  CHECK(CounterOf(run, FePerfInternMiss) == AllocOf(run, FeTSymbol));
  CHECK(CounterOf(run, FePerfInternLookup) > CounterOf(run, FePerfInternMiss));
  // No Lisp ran.
  CHECK(CounterOf(run, FePerfEvalDispatch) == 0);
  CHECK(CounterOf(run, FePerfDispatchLambda) == 0);
  CHECK(CounterOf(run, FePerfMacroExpansion) == 0);
  // The answer: a context that opened is one whose primitives are callable.
  // Asked here rather than in the body so that the probe's own interning
  // stays out of the measurement.
  CHECK(FeIsFBound(run->context, FeMakeSymbol(run->context, "car")));
  CHECK(FeIsFBound(run->context, FeMakeSymbol(run->context, "+")));
  return true;
}

static bool BodyContextOpenClose(const Workload* workload, WorkloadRun* run) {
  CHECK(workload->arena == sizeof(scratch_arena_bytes));
  FeContext* const scratch =
      FeOpenContext(scratch_arena_bytes, workload->arena);
  CHECK(scratch != nullptr);
  // The measured region is exactly the pair. Nothing is asked of `scratch`
  // in between, so no probe of the harness's own can be blamed for the
  // difference against `context-open`.
  FeCloseContext(scratch);
  (void)snprintf(run->answer, sizeof(run->answer), "opened and closed");
  return true;
}

static bool CheckContextOpenClose(const Workload* workload,
                                  const WorkloadRun* run) {
  CHECK(AllocationIsPartitioned(run));
  // The close IS the collection, and there is exactly one of it: an open
  // never collects (see `CheckContextOpen`), so this counter is the whole
  // evidence that the close is inside the region rather than after it.
  CHECK_UNPOISONED(CounterOf(run, FePerfGcCollection) == 1);
  // `FeCloseContext` clears every root before collecting, so nothing is
  // reachable and every cell the open took comes back -- and nothing is
  // marked on the way.
  CHECK_UNPOISONED(CounterOf(run, FePerfGcReclaimed) ==
                   CounterOf(run, FePerfAllocObject));
  CHECK_UNPOISONED(CounterOf(run, FePerfGcMarkNew) == 0);
  // The open half costs what any other open costs: the same names and the
  // same tables whatever the arena (see `CheckOpenIsArenaIndependent`), so
  // the scratch context's allocations equal the harness context's.
  CHECK(CounterOf(run, FePerfAllocObject) == run->open_cells);
  // The sweep is proportional to the ARENA and not to the live set: it
  // examines every slot, and the scratch arena is the same size as the
  // harness's, so the counter and that arena's capacity are one number.
  // Phase 22 changes this ratio; it should not quietly change the identity.
  CHECK_UNPOISONED(CounterOf(run, FePerfGcSweepExamined) ==
                   (unsigned long long)run->stats.total_slots);
  CHECK(CounterOf(run, FePerfGcSweepExamined) >
        CounterOf(run, FePerfGcReclaimed));
  // Interning, as in `context-open`: a bare open makes no uninterned symbol,
  // and asks for a few names twice.
  CHECK(CounterOf(run, FePerfInternMiss) == AllocOf(run, FeTSymbol));
  CHECK(CounterOf(run, FePerfInternLookup) > CounterOf(run, FePerfInternMiss));
  // No Lisp ran, in either half.
  CHECK(CounterOf(run, FePerfEvalDispatch) == 0);
  CHECK(CounterOf(run, FePerfDispatchLambda) == 0);
  CHECK(CounterOf(run, FePerfMacroExpansion) == 0);
  // `run->stats` -- and therefore this workload's row in the arena table --
  // describes the HARNESS context, which took no part: it neither collected
  // nor allocated while the scratch pair ran, and still holds exactly what
  // its own open cost.
  CHECK_UNPOISONED(run->stats.collection_count == 0);
  CHECK(run->stats.allocation_failures == 0);
  CHECK_UNPOISONED(
      (unsigned long long)(run->stats.total_slots - run->stats.free_slots) ==
      run->baseline_cells);
  // The answer: the open half really did build a working context. Asked of a
  // THIRD context, opened here, because the body's own is closed by the time
  // a check could ask it and a probe inside the region would intern names
  // the measurement is not about.
  FeContext* const probe = FeOpenContext(scratch_arena_bytes, workload->arena);
  CHECK(probe != nullptr);
  const bool callable = FeIsFBound(probe, FeMakeSymbol(probe, "car")) &&
                        FeIsFBound(probe, FeMakeSymbol(probe, "+"));
  FeCloseContext(probe);
  CHECK(callable);
  return true;
}

// ---------------------------------------------------------------------------
// 21.2 item 4: the four shapes kg's utils/bench.py already benchmarks, so
// that both layers measure the same NAMED shape rather than two unrelated
// programs. Each expression is kg's, respelled for a Lisp-2 without kg's
// prelude: `defun` and `length` are kg's, `fset`/`lambda` are fe's, and a
// macro is reached through the FUNCTION cell.
// ---------------------------------------------------------------------------

// kg's `lisp-list-walk`, n=150: non-tail recursion that builds a 150-element
// list and then walks it. `llen` stands in for kg's `length`. Each source
// below spells its own scale, and the matching `Workload.param` is what the
// checks are written against, so changing one without the other fails
// immediately rather than quietly measuring something else.
static const char list_walk_source[] =
    "(fset 'lw (lambda (n l) (if (<= n 0) l (lw (- n 1) (cons n l)))))"
    "(fset 'llen (lambda (l n) (if l (llen (cdr l) (+ n 1)) n)))"
    "(llen (lw 150 nil) 0)";

// kg's `lisp-arithmetic-loop`, 20000 iterations. Iterative, so frame depth
// does not grow; what grows is the garbage its own boxed integer results
// leave behind.
static const char arithmetic_source[] =
    "(setq i 0) (setq acc 0)"
    "(while (< i 20000) (setq acc (+ acc i)) (setq i (+ i 1))) acc";

// kg's `lisp-macro-heavy`, 2000 iterations: fe re-expands a macro on every
// call rather than rewriting the call site, so this is 2000 expansions.
static const char macro_source[] =
    "(fset 'm (macro (x) (list '+ x 1)))"
    "(setq n 0) (setq i 0)"
    "(while (< i 2000) (setq n (m n)) (setq i (+ i 1))) n";

// kg's `lisp-deep-call-chain`, 300 levels of non-tail self-recursion.
static const char deep_call_source[] =
    "(fset 'dc (lambda (n) (if (<= n 0) 0 (+ 1 (dc (- n 1))))))(dc 300)";

static bool RunIntegerScript(WorkloadRun* run,
                             const char* label,
                             const char* source,
                             size_t length,
                             int64_t expected) {
  FeObject* const result =
      FeEvaluateString(run->context, label, source, length);
  CHECK(FeGetType(result) == FeTInteger);
  const int64_t value = FeToInteger(run->context, result);
  (void)snprintf(run->answer, sizeof(run->answer), "%" PRId64, value);
  CHECK(value == expected);
  return true;
}

static bool BodyListWalk(const Workload* workload, WorkloadRun* run) {
  (void)workload;
  return RunIntegerScript(run, "list-walk", list_walk_source,
                          sizeof(list_walk_source) - 1, 150);
}

static bool BodyArithmetic(const Workload* workload, WorkloadRun* run) {
  (void)workload;
  return RunIntegerScript(run, "arithmetic-loop", arithmetic_source,
                          sizeof(arithmetic_source) - 1, 199990000);
}

static bool BodyMacroHeavy(const Workload* workload, WorkloadRun* run) {
  (void)workload;
  return RunIntegerScript(run, "macro-heavy", macro_source,
                          sizeof(macro_source) - 1, 2000);
}

static bool BodyDeepCall(const Workload* workload, WorkloadRun* run) {
  (void)workload;
  return RunIntegerScript(run, "deep-call-chain", deep_call_source,
                          sizeof(deep_call_source) - 1, 300);
}

// What every evaluated shape has in common, and nothing more: the by-type
// partition, and the dispatch identity that makes the callable-family
// breakdown a partition too.
static bool CheckEvaluated(const WorkloadRun* run) {
  CHECK(AllocationIsPartitioned(run));
  CHECK(run->stats.allocation_failures == 0);
  // Every callable whose argument list was started was afterwards invoked as
  // exactly one of the two ordinary families; a macro is neither.
  CHECK(CounterOf(run, FePerfDispatchCallable) ==
        CounterOf(run, FePerfDispatchNative) +
            CounterOf(run, FePerfDispatchLambda));
  // A frame is pushed for work the loop then has to turn over.
  CHECK(CounterOf(run, FePerfFramePush) <= CounterOf(run, FePerfEvalDispatch));
  return true;
}

static bool CheckListWalk(const Workload* workload, const WorkloadRun* run) {
  const unsigned long long calls =
      2 * ((unsigned long long)workload->param + 1);
  CHECK(CheckEvaluated(run));
  CHECK(CounterOf(run, FePerfDispatchLambda) == calls);
  // Both functions take two parameters, and parameter binding costs one
  // environment pair each: a call's environment cost is its arity, not a
  // constant.
  CHECK(CounterOf(run, FePerfEnvBind) == 2 * calls);
  // Non-tail recursion holds frames open all the way down: about two per
  // level, `lw`'s `if` and the recursive call. That -- and NOT the GC root
  // stack, which does not grow with this shape at all (measured: 40 slots of
  // 4096 at n=150, all of it the context open's own) -- is what bounds n.
  CHECK(run->stats.peak_frame_depth >= 2 * workload->param);
  CHECK(run->stats.peak_frame_depth <= run->stats.frame_capacity);
  CHECK(run->stats.peak_gc_stack_depth < (size_t)GcStackSize);
  return true;
}

static bool CheckArithmetic(const Workload* workload, const WorkloadRun* run) {
  CHECK(CheckEvaluated(run));
  // Each iteration boxes `(+ acc i)` and `(+ i 1)`: every integer result is
  // a fresh 16-byte cell, and this is where that shows.
  CHECK(AllocOf(run, FeTInteger) >= 2 * (unsigned long long)workload->param);
  // The loop calls no lambda and expands no macro, so its whole cost is
  // primitive dispatch and scalar boxing.
  CHECK(CounterOf(run, FePerfDispatchLambda) == 0);
  CHECK(CounterOf(run, FePerfMacroExpansion) == 0);
  // Iterative: `while` does not nest a frame per iteration.
  CHECK(run->stats.peak_frame_depth < 32);
  return true;
}

static bool CheckMacroHeavy(const Workload* workload, const WorkloadRun* run) {
  const unsigned long long calls = (unsigned long long)workload->param;
  CHECK(CheckEvaluated(run));
  // fe expands on every invocation: 2000 calls, 2000 expansions. This is the
  // counter a macro-expansion cache would be argued from, and the equality is
  // what such a cache would break.
  CHECK(CounterOf(run, FePerfMacroExpansion) == calls);
  CHECK(CounterOf(run, FePerfDispatchMacro) == calls);
  return true;
}

static bool CheckDeepCall(const Workload* workload, const WorkloadRun* run) {
  const unsigned long long calls = (unsigned long long)workload->param + 1;
  CHECK(CheckEvaluated(run));
  CHECK(CounterOf(run, FePerfDispatchLambda) == calls);
  // One parameter, so one binding pair per call.
  CHECK(CounterOf(run, FePerfEnvBind) == calls);
  // Non-tail recursion holds frames open all the way down.
  CHECK(run->stats.peak_frame_depth > workload->param);
  CHECK(run->stats.peak_frame_depth <= run->stats.frame_capacity);
  return true;
}

// ---------------------------------------------------------------------------
// 21.2 item 5: intern hits and misses after 128, 1024 and 8192 distinct
// symbols.
// ---------------------------------------------------------------------------

static bool BodyIntern(const Workload* workload, WorkloadRun* run) {
  FeContext* const context = run->context;
  const size_t n = workload->param;
  // One checkpoint, restored every pass: `MakeObject` roots each new cell,
  // and n is caller-controlled and above 4032 in the largest tier. Interned
  // symbols survive the restore because `symbol_list` is a permanent root,
  // which is exactly why this loop may be written in C at all. What is being
  // measured here is therefore the interning, not the rooting.
  const size_t gc = FeSaveGC(context);
  for (size_t i = 0; i < n; i++) {
    char name[32];
    (void)snprintf(name, sizeof(name), "fe-perf-sym-%zu", i);
    (void)FeMakeSymbol(context, name);
    FeRestoreGC(context, gc);
  }

  // Symbols in the obarray now: what the open interned, plus these. Nothing
  // here makes an UNINTERNED symbol, and nothing has collected, so this IS
  // the obarray length -- which is what lets the miss below be asserted
  // exactly rather than approximately.
  const unsigned long long before =
      run->baseline_symbols + LiveAllocOf(FeTSymbol);
  // ...and the index's own count of the same thing, read at the same instant,
  // so the two are comparable rather than merely close.
  const unsigned long long indexed = context->symbol_index_count;
  unsigned long long mark = FePerfRead(FePerfInternCandidate);
  unsigned long long probe_mark = FePerfRead(FePerfInternProbe);
  const FeObject* const fresh = FeMakeSymbol(context, "fe-perf-absent-name");
  const unsigned long long miss = FePerfRead(FePerfInternCandidate) - mark;
  const unsigned long long miss_probes =
      FePerfRead(FePerfInternProbe) - probe_mark;
  FeRestoreGC(context, gc);

  // A hit on the name just interned. Before Phase 26 that was the head of
  // `symbol_list` and cost exactly one candidate; it is now wherever its name
  // hashes, and costs whatever its own probe does.
  mark = FePerfRead(FePerfInternCandidate);
  const FeObject* const again = FeMakeSymbol(context, "fe-perf-absent-name");
  const unsigned long long head = FePerfRead(FePerfInternCandidate) - mark;
  FeRestoreGC(context, gc);

  // A hit on a name the context interned at open, which is therefore past
  // every symbol this workload made -- and which the index makes cost the
  // same as the name interned an instant ago, that being the point.
  mark = FePerfRead(FePerfInternCandidate);
  const FeObject* const core = FeMakeSymbol(context, "car");
  const unsigned long long deep = FePerfRead(FePerfInternCandidate) - mark;
  FeRestoreGC(context, gc);

  // The answer: interning is idempotent on the name, and finds a symbol.
  CHECK(fresh == again);
  CHECK(FeGetType(fresh) == FeTSymbol);
  CHECK(FeGetType(core) == FeTSymbol);
  // Phase 26's debug check, after the largest interning workload fe has: the
  // index still says exactly what `symbol_list` says, N symbols and several
  // table resizes later.
  CHECK(SymbolIndexMatchesSymbolList(context));
  AddExtra(run, "symbols_before_miss", before);
  AddExtra(run, "indexed_symbols", indexed);
  AddExtra(run, "miss_candidates", miss);
  AddExtra(run, "miss_probes", miss_probes);
  AddExtra(run, "hit_head_candidates", head);
  AddExtra(run, "hit_core_candidates", deep);
  (void)snprintf(run->answer, sizeof(run->answer),
                 "%zu interned, miss scanned %llu", n, miss);
  return true;
}

// Phase 26's gate constant, as a literal the measurement fixed: candidates
// examined PER LOOKUP, at every tier. Measured here at 1.55, 1.72 and 1.55
// for the 128, 1024 and 8192 tiers, so 4 leaves the table's own variance
// twice the room it uses without leaving room for anything shaped like a
// scan. The execution plan accepts up to 8 without a written argument, 8
// being about what an average probe costs at a load factor this table never
// reaches. Before the index the same figure was 629.85 at the 1024 tier and
// 4213.98 at 8192.
enum { InternCandidateBound = 4 };

// What ONE probe may examine, which is a different question with a different
// answer. A per-lookup average is the cost; a single probe's cost is the
// length of the linear-probing cluster its name happens to land in, and at a
// load factor of two thirds a cluster of ten is ordinary rather than
// remarkable -- the 128 tier measures exactly that, 10 candidates for the
// miss and 11 for the hit that follows it, while the two larger tiers
// measure 0 and 1. So this is bounded and deliberately not pinned: the exact
// cluster is deterministic, but it is a function of the whole set of names in
// the table, and adding one core primitive would move it for reasons that
// have nothing to do with what the assertion is about. What it does say is
// that a single probe is bounded by a constant the tier's size does not
// appear in, where the entry-pin miss examined 246, 1142 and 8310.
enum { InternProbeBound = 32 };

static bool CheckIntern(const Workload* workload, const WorkloadRun* run) {
  const unsigned long long n = (unsigned long long)workload->param;
  CHECK(AllocationIsPartitioned(run));
  CHECK(run->stats.allocation_failures == 0);
  // Nothing collected, so no symbol this workload made was ever reclaimed and
  // the obarray length below is exact -- and the index's own resizes, which
  // publish a bigger block and orphan the old one, did not need one either.
  //
  // NOT ASSERTED UNDER `FE_DEBUG_PAYLOAD_MOVE`, which is gc_stress.c's own
  // exemption and for its reason: that knob slides the live payload extent
  // one alignment unit per allocation and leaves the bytes behind it
  // unreachable until a collection returns the extent to the region's base,
  // so a workload that publishes one block per interned name drifts by its
  // own length and eventually collects to reclaim the drift. Under the knob
  // a collection here is the knob working, not the workload allocating, and
  // the length this comment protects is protected anyway: an interned symbol
  // is on `symbol_list` and therefore permanently reachable, which is what
  // `SymbolIndexMatchesSymbolList` above checks directly.
#if !FE_DEBUG_PAYLOAD_MOVE
  CHECK(CounterOf(run, FePerfGcCollection) == 0);
#endif
  // n populating misses plus the probe's own, then two hits after it.
  CHECK(CounterOf(run, FePerfInternMiss) == n + 1);
  CHECK(CounterOf(run, FePerfInternLookup) == n + 3);
  // Every symbol on the list is in the index and nothing else is, which is
  // the one-recoverable-publish property counted rather than argued.
  CHECK(ExtraOf(run, "indexed_symbols") == ExtraOf(run, "symbols_before_miss"));
  // THE SHAPE PHASE 26 PUT IN PLACE OF THE SCAN. A miss used to examine every
  // interned symbol -- the obarray was a list, and a scan has to reach the
  // end before it can conclude anything -- so `miss_candidates` was
  // `symbols_before_miss` exactly, at every tier. It is now a bounded handful
  // that the tier's size does not appear in.
  CHECK(ExtraOf(run, "miss_candidates") <= InternProbeBound);
  // A miss stops at the first FREE slot, which is the one slot it looks at
  // and does not count as a candidate. That is the whole difference between
  // the two counters, and it is what makes a miss cost what a hit costs.
  CHECK(ExtraOf(run, "miss_probes") == ExtraOf(run, "miss_candidates") + 1);
  // A hit examines at least the symbol it found. Neither of these is 1 by
  // construction any more: the head of the list was O(1) because it was the
  // head, where both of these are O(1) because they are hashed.
  CHECK(ExtraOf(run, "hit_head_candidates") >= 1);
  CHECK(ExtraOf(run, "hit_head_candidates") <= InternProbeBound);
  CHECK(ExtraOf(run, "hit_core_candidates") >= 1);
  CHECK(ExtraOf(run, "hit_core_candidates") <= InternProbeBound);
  // Clause 1 of the gate: candidates per lookup bounded by a literal, at this
  // tier. Cross-multiplied rather than divided, so the bound is exact integer
  // arithmetic on the counters themselves.
  CHECK(CounterOf(run, FePerfInternCandidate) <=
        InternCandidateBound * CounterOf(run, FePerfInternLookup));
  // Probes are candidates plus exactly one free slot per miss, because a hit
  // stops ON its candidate and a miss stops on the free slot after the last
  // one. Nothing else can move these two apart.
  CHECK(CounterOf(run, FePerfInternProbe) ==
        CounterOf(run, FePerfInternCandidate) +
            CounterOf(run, FePerfInternMiss));
  // Clause 3 of the gate: the comparison counters follow the candidate count
  // down. No lambda is called anywhere in this workload, and `IsNamedSymbol`
  // -- which byte-compares "&optional"/"&rest" against every parameter of
  // every call -- is the only other caller of the name comparison. So here
  // the two counters are the same measurement, exactly as they were before
  // the index: one comparison per candidate. They stay EQUAL rather than
  // improving on it because a slot holds a symbol and no hash beside it, so
  // there is nothing to reject a candidate with before its name is read --
  // which is affordable only because there are now one or two of them per
  // lookup instead of thousands.
  CHECK(CounterOf(run, FePerfNameCompare) ==
        CounterOf(run, FePerfInternCandidate));
  CHECK(CounterOf(run, FePerfNameByte) >= CounterOf(run, FePerfNameCompare));
  return true;
}

// ---------------------------------------------------------------------------
// 21.2 item 6: lexical lookup by environment WIDTH and by DEPTH, separately.
//
// Both forms bind `param` variables and then read the one bound FIRST, which
// is the one furthest from the head of the environment. The width form binds
// them in a single `let`; the depth form nests `param` one-binding `let`s.
// The reader's own interning is kept out of the measurement by reading the
// form during setup and evaluating the already-read object in the body.
// ---------------------------------------------------------------------------

static bool AppendSource(size_t* at, const char* text) {
  const size_t length = strlen(text);
  if (*at + length + 1 > (size_t)SourceMax) {
    return false;
  }
  memcpy(source_buffer + *at, text, length + 1);
  *at += length;
  return true;
}

static bool AppendBinding(size_t* at, size_t index, bool nested) {
  char piece[48];
  const int written =
      nested ? snprintf(piece, sizeof(piece), "(let ((v%zu 1)) ", index)
             : snprintf(piece, sizeof(piece), "(v%zu 1)", index);
  if (written < 0 || (size_t)written >= sizeof(piece)) {
    return false;
  }
  return AppendSource(at, piece);
}

static bool BuildEnvSource(size_t width, bool nested) {
  size_t at = 0;
  source_buffer[0] = '\0';
  if (!nested && !AppendSource(&at, "(let (")) {
    return false;
  }
  for (size_t i = 0; i < width; i++) {
    if (!AppendBinding(&at, i, nested)) {
      return false;
    }
  }
  if (!nested && !AppendSource(&at, ") ")) {
    return false;
  }
  if (!AppendSource(&at, "(+")) {
    return false;
  }
  for (size_t i = 0; i < (size_t)EnvLookups; i++) {
    if (!AppendSource(&at, " v0")) {
      return false;
    }
  }
  if (!AppendSource(&at, ")")) {
    return false;
  }
  for (size_t i = 0; i < (nested ? width : 1); i++) {
    if (!AppendSource(&at, ")")) {
      return false;
    }
  }
  return true;
}

static bool PrepareEnvForm(const Workload* workload,
                           WorkloadRun* run,
                           bool nested) {
  CHECK(BuildEnvSource(workload->param, nested));
  size_t offset = 0;
  FeObject* const form =
      FeReadString(run->context, source_buffer, strlen(source_buffer), &offset);
  CHECK(form != nullptr);
  prepared_form = FeCreateRoot(run->context, form);
  CHECK(prepared_form != nullptr);
  return true;
}

static bool SetupEnvWidth(const Workload* workload, WorkloadRun* run) {
  return PrepareEnvForm(workload, run, false);
}

static bool SetupEnvDepth(const Workload* workload, WorkloadRun* run) {
  return PrepareEnvForm(workload, run, true);
}

static bool BodyEnv(const Workload* workload, WorkloadRun* run) {
  (void)workload;
  FeObject* const result = FeEvaluate(run->context, FeGetRoot(prepared_form));
  CHECK(FeGetType(result) == FeTInteger);
  const int64_t value = FeToInteger(run->context, result);
  (void)snprintf(run->answer, sizeof(run->answer), "%" PRId64, value);
  // Every binding holds 1 and the body sums the first-bound one EnvLookups
  // times, so a lookup that resolved to the wrong binding, or a body that
  // stopped evaluating, changes the answer.
  CHECK(value == (int64_t)EnvLookups);
  return true;
}

static bool CheckEnv(const Workload* workload, const WorkloadRun* run) {
  const unsigned long long width = (unsigned long long)workload->param;
  CHECK(AllocationIsPartitioned(run));
  CHECK(run->stats.allocation_failures == 0);
  // One binding pair per variable, whatever shape the bindings arrive in.
  CHECK(CounterOf(run, FePerfEnvBind) == width);
  // THE SHAPE PHASE 28 WOULD BREAK: the environment is one flat alist, so a
  // lookup of the first-bound variable walks exactly one cell per binding in
  // scope, and reading it EnvLookups times costs EnvLookups times that.
  // Nothing but those references searches an environment here -- the head `+`
  // is resolved in the function namespace, and `let` installs its bindings
  // without looking any up.
  CHECK(CounterOf(run, FePerfEnvLookup) == (unsigned long long)EnvLookups);
  CHECK(CounterOf(run, FePerfEnvCell) ==
        (unsigned long long)EnvLookups * width);
  // A `let` costs one `FeTFn` object apiece regardless of how many variables
  // it binds -- so the width form allocates one and the depth form `width` of
  // them. Recorded because it is a representation fact nobody would guess
  // from the source, and because it is what makes depth and width differ in
  // cells while costing the same to look up.
  CHECK(AllocOf(run, FeTFn) == (workload->setup == SetupEnvDepth ? width : 1));
  // The reader ran during setup, so the measured region interns nothing.
  CHECK(CounterOf(run, FePerfInternLookup) == 0);
  return true;
}

// ---------------------------------------------------------------------------
// 21.2 item 7: strings at 0, 7, 8, 256 and 8192 bytes. 7 and 8 straddled the
// seven-byte cell boundary of the representation Phase 25 replaced, which was
// the point of both; they are kept at exactly those lengths because the
// before/after this phase is read from these five records, and a length that
// moved would make the comparison a different measurement.
// ---------------------------------------------------------------------------

static bool BodyString(const Workload* workload, WorkloadRun* run) {
  FeContext* const context = run->context;
  const size_t length = workload->param;
  const size_t gc = FeSaveGC(context);
  const FeObject* const string = FeMakeString(context, string_source);
  CHECK(FeGetType(string) == FeTString);
  // The answer: the bytes come back, all of them, in order. A string whose
  // length or payload address were read wrongly would cost exactly the same
  // and read exactly as plausible.
  CHECK(FeStringByteLength(context, string) == length);
  CHECK(FeCopyStringBytes(context, string, string_copy, sizeof(string_copy)));
  CHECK(memcmp(string_copy, string_source, length) == 0);
  FeRestoreGC(context, gc);
  (void)snprintf(run->answer, sizeof(run->answer), "%zu bytes round-tripped",
                 length);
  return true;
}

// The region bytes one string of `length` bytes occupies: the block header
// plus its bytes rounded to the allocator's alignment. Spelled from the block
// header rather than from a 32, exactly as `VectorBlockBytes` below is, so
// that it is the representation being asserted.
static unsigned long long StringBlockBytes(size_t length) {
  return (unsigned long long)(sizeof(FePayloadBlock) +
                              (length + FePayloadAlignment - 1) /
                                  FePayloadAlignment * FePayloadAlignment);
}

static bool CheckString(const Workload* workload, const WorkloadRun* run) {
  const size_t length = workload->param;
  CHECK(AllocationIsPartitioned(run));
  // ONE CELL, at every length. That is the phase's headline in a single
  // assertion: the cell cost of a string stopped depending on how long it is,
  // and the length went to the region instead. The five lengths span three
  // orders of magnitude and every one of them charges the same 1.
  CHECK(CounterOf(run, FePerfStringObject) == 1);
  CHECK(AllocOf(run, FeTString) == 1);
  CHECK(CounterOf(run, FePerfAllocObject) == 1);
  CHECK(CounterOf(run, FePerfStringByte) == (unsigned long long)length);
  // ...and the region cost is one block, sized by the length.
  CHECK(CounterOf(run, FePerfPayloadAlloc) == 1);
  CHECK(CounterOf(run, FePerfPayloadByte) == StringBlockBytes(length));
  // Three copy-out calls -- one for `FeStringByteLength` and two inside
  // `FeCopyStringBytes` -- of which exactly ONE reads a byte. The other two
  // ask for the length, which is a field read now rather than a walk of the
  // whole string; that difference is what the two counters together say.
  CHECK(CounterOf(run, FePerfStringCopy) == 3);
  CHECK(CounterOf(run, FePerfStringByteCopied) == (unsigned long long)length);
  CHECK(run->stats.allocation_failures == 0);
  return true;
}

// ---------------------------------------------------------------------------
// Phase 24.1's deferred item: a vector shape, so that the battery exercises
// the payload region with something whose size is the workload's own
// parameter. Since Phase 25 every workload touches the region -- a symbol's
// name is a string -- so what this family adds is a payload cost that is
// large, chosen, and made of children rather than bytes.
//
// Two sizes, bracketing the same boundary `payload_tests`' O(1) gate uses:
// eight elements is one small block, 8192 is a block that dominates the
// region. The access loop's indices come from a multiplicative hash, so an
// implementation that WALKED to an index would charge work proportional to
// the average index and the two sizes would differ by three orders of
// magnitude. The cross-workload check below is that they do not differ at all.
// ---------------------------------------------------------------------------

// Random accesses per vector workload, whatever its length. Fixed on purpose:
// it is what makes "the access half costs the same at both sizes" a
// subtraction rather than a ratio.
enum { VectorAccessCount = 4096 };

// The region bytes one vector of `length` elements occupies: fe's whole
// vector cost table, spelled from the block header rather than from a 32 so
// that it is the representation being asserted.
static unsigned long long VectorBlockBytes(size_t length) {
  return (unsigned long long)(sizeof(FePayloadBlock) +
                              length * sizeof(FeObject*));
}

static bool BodyVector(const Workload* workload, WorkloadRun* run) {
  FeContext* const context = run->context;
  const size_t n = workload->param;
  // Every index below is taken modulo `n`, and the answer is a sum over
  // 0..n-1; an empty vector is a different workload and this one refuses it.
  CHECK(n > 0);
  const size_t gc = FeSaveGC(context);
  FeObject* const vector = FeMakeVector(context, n);
  FePushGC(context, vector);
  CHECK(FeVectorLength(context, vector) == n);
  // Slot i holds the integer i, so the multiset the access loop permutes has
  // a sum nothing else could produce. The checkpoint is restored every pass
  // for the reason this file's header gives: `n` may be twice the root
  // stack's practical ceiling, and each integer is rooted by the vector the
  // moment it is stored.
  const size_t fill = FeSaveGC(context);
  for (size_t i = 0; i < n; i++) {
    FeVectorSet(context, vector, i, FeMakeInteger(context, (int64_t)i));
    FeRestoreGC(context, fill);
  }
  // The access phase: two reads and two writes per pass, swapping a pair of
  // spread-out slots. It allocates NOTHING -- which is why holding `left`
  // across the second read is safe, and why `CheckVector` asserts the
  // collection count is zero rather than trusting the reasoning.
  for (size_t i = 0; i < VectorAccessCount; i++) {
    const size_t left_index = (i * 2654435761u) % n;
    const size_t right_index = ((i + 1) * 2654435761u) % n;
    FeObject* const left = FeVectorRef(context, vector, left_index);
    FeVectorSet(context, vector, left_index,
                FeVectorRef(context, vector, right_index));
    FeVectorSet(context, vector, right_index, left);
  }
  // The answer: swapping preserves the multiset, so the elements still sum to
  // 0 + 1 + ... + (n-1). A vector that lost, duplicated or aliased an element
  // allocates exactly the same cells and reads exactly as plausible.
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    sum += FeToInteger(context, FeVectorRef(context, vector, i));
  }
  CHECK(sum == (int64_t)(n * (n - 1) / 2));
  FeRestoreGC(context, gc);
  // The two payload numbers a terminal reader would otherwise never see: this
  // battery's human tables are all cells, and the region is the thing these
  // two workloads exist to exercise.
  const FeArenaStats stats = FeGetArenaStats(context);
  AddExtra(run, "payload_live_bytes", stats.payload_live_bytes);
  AddExtra(run, "payload_capacity_bytes", stats.payload_capacity_bytes);
  (void)snprintf(run->answer, sizeof(run->answer),
                 "%zu elements permuted, sum %lld", n, (long long)sum);
  return true;
}

static bool CheckVector(const Workload* workload, const WorkloadRun* run) {
  const unsigned long long n = (unsigned long long)workload->param;
  CHECK(AllocationIsPartitioned(run));
  // The payload region: one block over the baseline the open left, of exactly
  // the size the representation implies, and no compaction, because nothing
  // collected.
  CHECK(CounterOf(run, FePerfPayloadAlloc) == 1);
  CHECK(CounterOf(run, FePerfPayloadByte) == VectorBlockBytes(workload->param));
  CHECK(CounterOf(run, FePerfGcCollection) == 0);
  CHECK(CounterOf(run, FePerfPayloadCompact) == 0);
  CHECK(CounterOf(run, FePerfPayloadCompactMoved) == 0);
  CHECK(run->stats.payload_live_bytes ==
        run->baseline_payload_bytes + VectorBlockBytes(workload->param));
  CHECK(run->stats.payload_allocation_failures == 0);
  CHECK(run->stats.allocation_failures == 0);
  // Elements published: the constructor's, once. Reads: the access loop's two
  // per pass plus the answer's walk of the whole vector. Writes: the
  // constructor's nil fill, the integer fill, and the access loop's two per
  // pass.
  CHECK(CounterOf(run, FePerfVectorElement) == n);
  CHECK(CounterOf(run, FePerfVectorRef) ==
        2 * (unsigned long long)VectorAccessCount + n);
  CHECK(CounterOf(run, FePerfVectorSet) ==
        2 * n + 2 * (unsigned long long)VectorAccessCount);
  // The only cells this workload allocates are the vector's own header and
  // the `n` integers it stores: the access loop allocates nothing at all,
  // which is what makes the `gc_collection == 0` above a property and not a
  // coincidence of the arena size.
  CHECK(AllocOf(run, FeTVector) == 1);
  CHECK(AllocOf(run, FeTInteger) == n);
  CHECK(CounterOf(run, FePerfAllocObject) == n + 1);
  return true;
}

// ---------------------------------------------------------------------------
// 21.2 item 8: sparse-garbage and dense-live collections, both in an arena
// small enough that they really collect.
//
// Both loops are written in Lisp: their trip counts are the point of the
// workload and are far past the 4032-slot GC root stack a C loop would have
// to keep restoring, while a Lisp accumulator lives in a value cell the
// collector marks directly.
// ---------------------------------------------------------------------------

// Allocates per iteration and keeps nothing.
static const char sparse_source[] =
    "(setq i 0)"
    "(while (< i 20000) (cons i i) (setq i (+ i 1))) i";

// Keeps every cons: the live set grows until the arena is most of the way
// full, so each collection marks nearly everything and reclaims little. The
// trailing walk is the answer check and also forces the whole retained list
// to be traversed.
static const char dense_source[] =
    "(setq l nil) (setq i 0)"
    "(while (< i 4000) (setq l (cons i l)) (setq i (+ i 1)))"
    "(setq k 0) (setq p l)"
    "(while p (setq k (+ k 1)) (setq p (cdr p))) k";

static bool BodySparse(const Workload* workload, WorkloadRun* run) {
  return RunIntegerScript(run, "gc-sparse-garbage", sparse_source,
                          sizeof(sparse_source) - 1, (int64_t)workload->param);
}

static bool BodyDense(const Workload* workload, WorkloadRun* run) {
  return RunIntegerScript(run, "gc-dense-live", dense_source,
                          sizeof(dense_source) - 1, (int64_t)workload->param);
}

// What both collection shapes have to satisfy. The sweep is the headline: it
// examines the whole arena on every collection, not the live set, so its cost
// is the arena size and not the program.
static bool CheckCollecting(const WorkloadRun* run) {
  CHECK(AllocationIsPartitioned(run));
  CHECK(run->stats.allocation_failures == 0);
  CHECK(CounterOf(run, FePerfGcCollection) > 0);
  CHECK(CounterOf(run, FePerfGcSweepExamined) ==
        CounterOf(run, FePerfGcCollection) *
            (unsigned long long)run->stats.total_slots);
  // A revisit stops at the mark bit, so the walk descends at least as often
  // as it marks.
  CHECK(CounterOf(run, FePerfGcMarkVisit) >= CounterOf(run, FePerfGcMarkNew));
  return true;
}

// The live set when the workload finished. NOT `peak_live_objects`, which is
// a high-water mark taken at allocation time: in any arena that collects at
// all it reaches `total_slots` exactly, because that is the state that
// triggers the collection, so it cannot tell a sparse workload from a dense
// one.
static size_t EndLive(const WorkloadRun* run) {
  return run->stats.total_slots - run->stats.free_slots;
}

// The live set once nothing collectable is left, which is what "the live set
// this shape leaves" means. `EndLive` alone cannot answer that: the free list
// runs out on a schedule set by how much each collection frees, so the
// garbage still standing at the last allocation is a PHASE and not a
// property of the shape.
//
// Measured while landing Phase 23.1's payload substrate, and the reason this
// reads a settled figure rather than the residue: adding fourteen objects to
// what a context open builds (that phase's `payload-exhaustion` condition
// row) moves every collection in `gc-sparse-garbage` fourteen allocations
// earlier. Over 45 collections that drifts the residue from 110 live to 830
// -- with the allocation count (80030), the collection count (45), the
// reclaimed count and the marked count all still exactly what they were, plus
// the 45 x 14 the fourteen new permanent objects are marked for. The counters
// were right and the assertion was reading a coincidence.
static size_t SettledLive(const WorkloadRun* run) {
  FeCollectGarbage(run->context);
  const FeArenaStats settled = FeGetArenaStats(run->context);
  return settled.total_slots - settled.free_slots;
}

static bool CheckSparse(const Workload* workload, const WorkloadRun* run) {
  (void)workload;
  CHECK(CheckCollecting(run));
  // Sparse: the loop keeps nothing, so collection reclaims far more than it
  // marks, and the live set it leaves is a small fraction of the arena.
  CHECK(CounterOf(run, FePerfGcReclaimed) > CounterOf(run, FePerfGcMarkNew));
  CHECK(SettledLive(run) * 2 < run->stats.total_slots);
  return true;
}

static bool CheckDense(const Workload* workload, const WorkloadRun* run) {
  (void)workload;
  CHECK(CheckCollecting(run));
  // Dense: the loop keeps everything, so marking dominates reclaiming -- the
  // exact mirror of the sparse case, on the same collector -- and the live
  // set it leaves is most of the arena.
  CHECK(CounterOf(run, FePerfGcMarkNew) > CounterOf(run, FePerfGcReclaimed));
  CHECK(SettledLive(run) * 2 > run->stats.total_slots);
  return true;
}

// ---------------------------------------------------------------------------
// The battery.
// ---------------------------------------------------------------------------

static const Workload workloads[] = {
    {.name = "context-open",
     .family = "context",
     .note = "21.2/1: FeOpenContext and nothing else; the close is excluded",
     .arena = ArenaTight,
     .param = 0,
     .measure_open = true,
     .setup = nullptr,
     .body = BodyContextOpen,
     .check = CheckContextOpen},
    {.name = "context-open-close",
     .family = "context",
     .note = "21.2/1: FeOpenContext plus FeCloseContext, both measured",
     .arena = ArenaTight,
     .param = 0,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyContextOpenClose,
     .check = CheckContextOpenClose},

    {.name = "list-walk",
     .family = "eval",
     .note = "21.2/4: kg lisp-list-walk, 150 levels of non-tail recursion",
     .arena = ArenaHost,
     .param = 150,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyListWalk,
     .check = CheckListWalk},
    {.name = "arithmetic-loop",
     .family = "eval",
     .note = "21.2/4: kg lisp-arithmetic-loop, 20000 iterations",
     .arena = ArenaHost,
     .param = 20000,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyArithmetic,
     .check = CheckArithmetic},
    {.name = "macro-heavy",
     .family = "eval",
     .note = "21.2/4: kg lisp-macro-heavy, 2000 macro calls",
     .arena = ArenaHost,
     .param = 2000,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyMacroHeavy,
     .check = CheckMacroHeavy},
    {.name = "deep-call-chain",
     .family = "eval",
     .note = "21.2/4: kg lisp-deep-call-chain, 300 levels",
     .arena = ArenaHost,
     .param = 300,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyDeepCall,
     .check = CheckDeepCall},

    {.name = "intern-128",
     .family = "intern",
     .note = "21.2/5: 128 distinct symbols, then a miss and two hits",
     .arena = ArenaLarge,
     .param = 128,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyIntern,
     .check = CheckIntern},
    {.name = "intern-1024",
     .family = "intern",
     .note = "21.2/5: 1024 distinct symbols, then a miss and two hits",
     .arena = ArenaLarge,
     .param = 1024,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyIntern,
     .check = CheckIntern},
    {.name = "intern-8192",
     .family = "intern",
     .note = "21.2/5: 8192 distinct symbols, then a miss and two hits",
     .arena = ArenaLarge,
     .param = 8192,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyIntern,
     .check = CheckIntern},

    {.name = "env-width-8",
     .family = "env",
     .note = "21.2/6: one let of 8 bindings, 64 reads of the first",
     .arena = ArenaHost,
     .param = 8,
     .measure_open = false,
     .setup = SetupEnvWidth,
     .body = BodyEnv,
     .check = CheckEnv},
    {.name = "env-width-64",
     .family = "env",
     .note = "21.2/6: one let of 64 bindings, 64 reads of the first",
     .arena = ArenaHost,
     .param = 64,
     .measure_open = false,
     .setup = SetupEnvWidth,
     .body = BodyEnv,
     .check = CheckEnv},
    {.name = "env-depth-8",
     .family = "env",
     .note = "21.2/6: 8 nested lets, 64 reads of the outermost binding",
     .arena = ArenaHost,
     .param = 8,
     .measure_open = false,
     .setup = SetupEnvDepth,
     .body = BodyEnv,
     .check = CheckEnv},
    {.name = "env-depth-64",
     .family = "env",
     .note = "21.2/6: 64 nested lets, 64 reads of the outermost binding",
     .arena = ArenaHost,
     .param = 64,
     .measure_open = false,
     .setup = SetupEnvDepth,
     .body = BodyEnv,
     .check = CheckEnv},

    {.name = "string-0",
     .family = "string",
     .note = "21.2/7: the empty string",
     .arena = ArenaHost,
     .param = 0,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyString,
     .check = CheckString},
    {.name = "string-7",
     .family = "string",
     .note = "21.2/7: exactly one StringBufferSize cell",
     .arena = ArenaHost,
     .param = 7,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyString,
     .check = CheckString},
    {.name = "string-8",
     .family = "string",
     .note = "21.2/7: one byte past the cell boundary",
     .arena = ArenaHost,
     .param = 8,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyString,
     .check = CheckString},
    {.name = "string-256",
     .family = "string",
     .note = "21.2/7: 256 bytes",
     .arena = ArenaHost,
     .param = 256,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyString,
     .check = CheckString},
    {.name = "string-8192",
     .family = "string",
     .note = "21.2/7: 8192 bytes",
     .arena = ArenaHost,
     .param = 8192,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyString,
     .check = CheckString},

    {.name = "vector-8",
     .family = "vector",
     .note = "24.1: 8 elements, 4096 random swaps, default payload carve",
     .arena = ArenaHost,
     .param = 8,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyVector,
     .check = CheckVector},
    {.name = "vector-8192",
     .family = "vector",
     .note = "24.1: 8192 elements, the same 4096 random swaps",
     .arena = ArenaHost,
     .param = 8192,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyVector,
     .check = CheckVector},

    {.name = "gc-sparse-garbage",
     .family = "gc",
     .note = "21.2/8: 20000 iterations keeping nothing, 96 KiB arena",
     .arena = ArenaTight,
     .param = 20000,
     .measure_open = false,
     .setup = nullptr,
     .body = BodySparse,
     .check = CheckSparse},
    {.name = "gc-dense-live",
     .family = "gc",
     .note = "21.2/8: 4000 retained conses, 256 KiB arena",
     .arena = ArenaSmall,
     .param = 4000,
     .measure_open = false,
     .setup = nullptr,
     .body = BodyDense,
     .check = CheckDense},
};

enum { WorkloadCount = (int)(sizeof(workloads) / sizeof(*workloads)) };
static_assert((int)WorkloadCount <= (int)WorkloadMax);

// The string workloads' source bytes: a repeating printable pattern, so that
// a truncated or misordered copy is visible rather than accidentally equal.
static void FillStringSource(size_t length) {
  for (size_t i = 0; i < length; i++) {
    string_source[i] = (char)('a' + (int)(i % 26));
  }
  string_source[length] = '\0';
}

static bool RunOne(const Workload* workload, WorkloadRun* run) {
  memset(run, 0, sizeof(*run));
  if (strcmp(workload->family, "string") == 0) {
    FillStringSource(workload->param);
  }
  prepared_form = nullptr;

  FePerfReset();
  const double started = Now();
  // One entry point for every workload: since Phase 25 `FeOpenContext` and
  // `FeOpenContextWithOptions` with default options are the same partition,
  // because a symbol's name is a string and a context with no payload region
  // cannot finish opening.
  FeContext* const context = FeOpenContext(arena_bytes, workload->arena);
  CHECK(context != nullptr);
  run->context = context;
  host_state.raised = false;
  FeSetUserData(context, &host_state);
  FeSetErrorFn(context, HandleError);
  if (setjmp(host_state.jump) != 0) {
    (void)fprintf(stderr, "perf_workloads: %s raised: %s\n", workload->name,
                  host_state.message);
    return false;
  }

  run->open_cells = FePerfRead(FePerfAllocObject);
  if (workload->setup != nullptr && !workload->setup(workload, run)) {
    return false;
  }
  if (!workload->measure_open) {
    run->baseline_cells = FePerfRead(FePerfAllocObject);
    run->baseline_symbols = LiveAllocOf(FeTSymbol);
    run->baseline_payload_bytes = FeGetArenaStats(context).payload_live_bytes;
    FePerfReset();
  }
  const double body_started = Now();
  if (!workload->body(workload, run)) {
    return false;
  }
  run->seconds = Now() - (workload->measure_open ? started : body_started);
  // Snapshot before anything the checks do can move a counter.
  for (int i = 0; i < FePerfCounterCount; i++) {
    run->counters[i] = FePerfRead((FePerfCounter)i);
  }
  run->stats = FeGetArenaStats(context);

  if (!workload->check(workload, run)) {
    (void)fprintf(stderr, "perf_workloads: %s: counter check failed\n",
                  workload->name);
    return false;
  }
  FeCloseContext(context);
  run->context = nullptr;
  return true;
}

// ---------------------------------------------------------------------------
// The relationships that need two workloads to state, and which are the point
// of running the tiers as a battery rather than one at a time.
// ---------------------------------------------------------------------------

static const WorkloadRun* Find(const char* name) {
  for (size_t i = 0; i < ran_count; i++) {
    if (strcmp(ran[i]->name, name) == 0) {
      return &runs[i];
    }
  }
  return nullptr;
}

static bool CheckInternTiers(void) {
  const WorkloadRun* const tiny = Find("intern-128");
  const WorkloadRun* const mid = Find("intern-1024");
  const WorkloadRun* const big = Find("intern-8192");
  CHECK(tiny != nullptr && mid != nullptr && big != nullptr);
  // THE TIERS STOP BEING DIFFERENT, which is Phase 26's whole claim and the
  // clause that cannot be passed by making a scan merely faster.
  //
  // Every tier opens the same arena from the same starting state. Two misses
  // used to differ by exactly the number of extra symbols interned before
  // them -- 1024-128 and then 8192-1024, a slope of exactly one -- because
  // the scan was O(interned symbols). All three are now under the same
  // literal, and the differences between them are a slot or two either way.
  CHECK(ExtraOf(tiny, "miss_candidates") <= InternProbeBound);
  CHECK(ExtraOf(mid, "miss_candidates") <= InternProbeBound);
  CHECK(ExtraOf(big, "miss_candidates") <= InternProbeBound);
  // Clause 2 of the gate, stated between the two tiers the plan names: the
  // per-lookup candidate count at 8192 is within a FACTOR OF TWO of the 1024
  // tier's, where the measured entry-pin figure was 6.69x (4213.98 against
  // 629.85). Cross-multiplied, since a per-lookup figure is a ratio of two
  // counters and this has to be integer arithmetic.
  CHECK(CounterOf(big, FePerfInternCandidate) *
            CounterOf(mid, FePerfInternLookup) <=
        2 * CounterOf(mid, FePerfInternCandidate) *
            CounterOf(big, FePerfInternLookup));
  // ...and total interning work is LINEAR in the tier size where it was
  // quadratic: eight times the tier is at most eight times the work. The same
  // step used to cost 53x (34 533 574 candidates against 646 854).
  CHECK(CounterOf(big, FePerfInternCandidate) <=
        8 * CounterOf(mid, FePerfInternCandidate));
  // And the harness's own discipline, asserted rather than assumed: a C loop
  // that restores its `FeSaveGC` checkpoint every pass leaves the root stack
  // exactly where the context open left it, at every tier -- including the one
  // that interns twice the 4032-slot practical ceiling. Without the restore
  // this is where `GC stack overflow` would arrive instead.
  CHECK(tiny->stats.peak_gc_stack_depth == mid->stats.peak_gc_stack_depth);
  CHECK(tiny->stats.peak_gc_stack_depth == big->stats.peak_gc_stack_depth);
  return true;
}

static bool CheckEnvShapes(void) {
  const WorkloadRun* const width = Find("env-width-64");
  const WorkloadRun* const depth = Find("env-depth-64");
  const WorkloadRun* const narrow = Find("env-width-8");
  CHECK(width != nullptr && depth != nullptr && narrow != nullptr);
  // Width and depth are THE SAME LOOKUP COST: fe has one flat alist and a
  // nested `let` extends it rather than making a frame to search. This is the
  // measured finding, and the assertion that records it.
  CHECK(CounterOf(width, FePerfEnvCell) == CounterOf(depth, FePerfEnvCell));
  // They are not the same program, though: nesting opens a frame per level.
  CHECK(CounterOf(depth, FePerfFramePush) > CounterOf(width, FePerfFramePush));
  // And the per-lookup cost is exactly linear in the number of bindings in
  // scope.
  CHECK(CounterOf(width, FePerfEnvCell) - CounterOf(narrow, FePerfEnvCell) ==
        (unsigned long long)EnvLookups * (64 - 8));
  return true;
}

static bool CheckStringBoundary(void) {
  const WorkloadRun* const empty = Find("string-0");
  const WorkloadRun* const seven = Find("string-7");
  const WorkloadRun* const eight = Find("string-8");
  const WorkloadRun* const huge = Find("string-8192");
  CHECK(empty != nullptr && seven != nullptr && eight != nullptr &&
        huge != nullptr);
  // THE BOUNDARY THAT IS NOT THERE ANY MORE. Seven bytes filled a cell and
  // eight needed a second one; the three records now charge the same single
  // cell, and 8192 bytes charges it too. This is the assertion the phase's
  // before/after is argued from, and it is the one that would have been a
  // contradiction in every earlier run of this battery.
  CHECK(CounterOf(empty, FePerfAllocObject) == 1);
  CHECK(CounterOf(seven, FePerfAllocObject) == 1);
  CHECK(CounterOf(eight, FePerfAllocObject) == 1);
  CHECK(CounterOf(huge, FePerfAllocObject) == 1);
  // What DOES scale is the region, and only in whole alignment units: seven
  // bytes and eight take the same eight-byte tail, and the empty string still
  // takes a block header of its own.
  CHECK(CounterOf(empty, FePerfPayloadByte) == sizeof(FePayloadBlock));
  CHECK(CounterOf(seven, FePerfPayloadByte) ==
        CounterOf(eight, FePerfPayloadByte));
  CHECK(CounterOf(huge, FePerfPayloadByte) >
        100 * CounterOf(eight, FePerfPayloadByte));
  // An 8192-byte string costs no root-stack slots at all -- one object is one
  // push -- which the cell chain also managed, by not leaving an entry per
  // cell behind. Worth keeping pinned across the change: it is the property
  // the trap in this file's header would lead one to doubt.
  CHECK(huge->stats.peak_gc_stack_depth == empty->stats.peak_gc_stack_depth);
  return true;
}

// The O(1) statement at the battery's own level, and the reason two sizes are
// run rather than one: the access half of the workload costs the same at n = 8
// and at n = 8192, so the whole difference between the two records' read
// counts is the answer walk, which is n by construction. A vector whose
// element address were a walk rather than arithmetic would blow this apart by
// three orders of magnitude.
static bool CheckVectorSizes(void) {
  const WorkloadRun* const small = Find("vector-8");
  const WorkloadRun* const large = Find("vector-8192");
  CHECK(small != nullptr && large != nullptr);
  CHECK(CounterOf(large, FePerfVectorRef) - CounterOf(small, FePerfVectorRef) ==
        8192 - 8);
  // Payload bytes, on the other hand, are exactly proportional: eight bytes
  // per element and one block header either way.
  CHECK(CounterOf(large, FePerfPayloadByte) -
            CounterOf(small, FePerfPayloadByte) ==
        (unsigned long long)((8192 - 8) * sizeof(FeObject*)));
  // And the carve is what pays for them: these are the only two records in
  // the battery whose region has any capacity at all.
  CHECK(small->stats.payload_capacity_bytes > 0);
  CHECK(large->stats.payload_capacity_bytes ==
        small->stats.payload_capacity_bytes);
  return true;
}

static bool CheckOpenIsArenaIndependent(void) {
  unsigned long long expected = runs[0].open_cells;
  for (size_t i = 0; i < ran_count; i++) {
    // Opening a context interns the same names and builds the same tables
    // whatever the arena size, so a record's counters are comparable with any
    // other record's. If this ever stops holding, every cross-workload
    // subtraction in this file silently starts comparing two different
    // starting states.
    CHECK(runs[i].open_cells == expected);
  }
  return true;
}

static bool CheckAcrossWorkloads(void) {
  CHECK(CheckOpenIsArenaIndependent());
  CHECK(CheckInternTiers());
  CHECK(CheckEnvShapes());
  CHECK(CheckStringBoundary());
  CHECK(CheckVectorSizes());
  return true;
}

// ---------------------------------------------------------------------------
// Reporting. The JSON is what kg consumes; the tables are for a human reading
// a terminal, and their columns are the four questions Phase 21's baseline
// report has to answer.
// ---------------------------------------------------------------------------

static void WriteArenaJson(FILE* out, const FeArenaStats* stats) {
  // The same fifteen keys, in the same order, that `FePerfWriteJson` writes: a
  // consumer that can read a counting `fe`'s `$FE_PERF_OUT` file can read
  // this one's "arena" object without a second parser. The five payload
  // gauges joined that file in Phase 23 and this one only in Phase 25.0,
  // which is the schema `/2` -> `/3` move: until the vector workloads there
  // was nothing in this battery for them to describe.
  (void)fprintf(out, "      \"total_slots\": %zu,\n", stats->total_slots);
  (void)fprintf(out, "      \"free_slots\": %zu,\n", stats->free_slots);
  (void)fprintf(out, "      \"peak_live_objects\": %zu,\n",
                stats->peak_live_objects);
  (void)fprintf(out, "      \"collection_count\": %zu,\n",
                stats->collection_count);
  (void)fprintf(out, "      \"peak_gc_stack_depth\": %zu,\n",
                stats->peak_gc_stack_depth);
  (void)fprintf(out, "      \"frame_capacity\": %zu,\n", stats->frame_capacity);
  (void)fprintf(out, "      \"peak_frame_depth\": %zu,\n",
                stats->peak_frame_depth);
  (void)fprintf(out, "      \"peak_cleanup_stack_depth\": %zu,\n",
                stats->peak_cleanup_stack_depth);
  (void)fprintf(out, "      \"peak_native_reentry\": %zu,\n",
                stats->peak_native_reentry);
  (void)fprintf(out, "      \"allocation_failures\": %zu,\n",
                stats->allocation_failures);
  (void)fprintf(out, "      \"payload_capacity_bytes\": %zu,\n",
                stats->payload_capacity_bytes);
  (void)fprintf(out, "      \"payload_live_bytes\": %zu,\n",
                stats->payload_live_bytes);
  (void)fprintf(out, "      \"payload_peak_bytes\": %zu,\n",
                stats->payload_peak_bytes);
  (void)fprintf(out, "      \"payload_compaction_count\": %zu,\n",
                stats->payload_compaction_count);
  (void)fprintf(out, "      \"payload_allocation_failures\": %zu\n",
                stats->payload_allocation_failures);
}

// The artifact line: which fe tree, and which binary, produced these numbers.
// Neither can be a compiled-in constant -- a describe baked into an object
// file names the tree that last triggered a rebuild, not the tree the run
// measured -- so the driver that starts the measurement passes them in, and a
// value it could not supply is reported as null rather than guessed at.
static const char* artifact_describe = nullptr;
static const char* artifact_sha256 = nullptr;

// A supplied value is a git describe or a hex digest. Anything carrying a
// character JSON would have to escape did not come from either, so it is
// reported as absent rather than written into the file.
static void WriteIdentityJson(FILE* out,
                              const char* key,
                              const char* value,
                              const char* tail) {
  bool plain = value != nullptr && value[0] != '\0';
  for (const char* c = value; plain && *c != '\0'; c++) {
    const unsigned char byte = (unsigned char)*c;
    plain = byte >= ' ' && byte < 0x7f && *c != '"' && *c != '\\';
  }
  if (plain) {
    (void)fprintf(out, "    \"%s\": \"%s\"%s\n", key, value, tail);
  } else {
    (void)fprintf(out, "    \"%s\": null%s\n", key, tail);
  }
}

// The header a reader checks before reading a single number below it: the
// three identifiers the binary knows about itself, then the two the driver
// supplied. A number whose artifact line is not the tree under discussion is
// not evidence about it.
static void WriteArtifactJson(FILE* out) {
  (void)fprintf(out, "  \"artifact\": {\n");
  (void)fprintf(out, "    \"fe_version\": \"%s\",\n", FeVersion);
  (void)fprintf(out, "    \"fe_api_version\": %d,\n", FE_API_VERSION);
  (void)fprintf(out, "    \"fe_language_version\": %d,\n", FE_LANGUAGE_VERSION);
  WriteIdentityJson(out, "fe_git_describe", artifact_describe, ",");
  WriteIdentityJson(out, "binary_sha256", artifact_sha256, "");
  (void)fprintf(out, "  },\n");
}

static void WriteExtraJson(FILE* out, const WorkloadRun* run) {
  (void)fprintf(out, "      \"extra\": {");
  for (size_t i = 0; i < run->extra_count; i++) {
    (void)fprintf(out, "%s\n        \"%s\": %llu", i == 0 ? "" : ",",
                  run->extra[i].name, run->extra[i].value);
  }
  (void)fprintf(out, "%s},\n", run->extra_count == 0 ? "" : "\n      ");
}

static void WriteRunJson(FILE* out,
                         const Workload* workload,
                         const WorkloadRun* run,
                         bool last) {
  (void)fprintf(out, "    {\n");
  (void)fprintf(out, "      \"name\": \"%s\",\n", workload->name);
  (void)fprintf(out, "      \"family\": \"%s\",\n", workload->family);
  (void)fprintf(out, "      \"note\": \"%s\",\n", workload->note);
  (void)fprintf(out, "      \"param\": %zu,\n", workload->param);
  (void)fprintf(out, "      \"arena_bytes\": %zu,\n", workload->arena);
  (void)fprintf(out, "      \"cell_capacity\": %zu,\n", run->stats.total_slots);
  (void)fprintf(out, "      \"context_open_cells\": %llu,\n", run->open_cells);
  (void)fprintf(out, "      \"includes_context_open\": %s,\n",
                workload->measure_open ? "true" : "false");
  (void)fprintf(out, "      \"answer\": \"%s\",\n", run->answer);
  (void)fprintf(out, "      \"seconds\": %.9f,\n", run->seconds);
  WriteExtraJson(out, run);
  (void)fprintf(out, "      \"counters\": {\n");
  for (int i = 0; i < FePerfCounterCount; i++) {
    const char* const name = fe_perf_counter_name[i];
    (void)fprintf(out, "        \"%s\": %llu%s\n",
                  name != nullptr ? name : "unnamed", run->counters[i],
                  i + 1 < FePerfCounterCount ? "," : "");
  }
  (void)fprintf(out, "      },\n      \"arena\": {\n");
  WriteArenaJson(out, &run->stats);
  (void)fprintf(out, "      }\n    }%s\n", last ? "" : ",");
}

static bool WriteJson(const char* path) {
  FILE* const out = strcmp(path, "-") == 0 ? stdout : fopen(path, "w");
  if (out == nullptr) {
    (void)fprintf(stderr, "perf_workloads: cannot write %s\n", path);
    return false;
  }
  (void)fprintf(out, "{\n  \"schema\": \"fe-perf-workloads/4\",\n");
  WriteArtifactJson(out);
  // The representation constant a reader of these records needs, which since
  // Phase 25 is the payload block's header rather than the seven bytes a
  // string cell used to carry: every string and every vector in a record
  // costs one of these before it costs a byte of its own.
  (void)fprintf(out, "  \"payload_block_bytes\": %d,\n",
                (int)sizeof(FePayloadBlock));
  (void)fprintf(out, "  \"workloads\": [\n");
  for (size_t i = 0; i < ran_count; i++) {
    WriteRunJson(out, ran[i], &runs[i], i + 1 == ran_count);
  }
  (void)fprintf(out, "  ]\n}\n");
  if (out != stdout) {
    (void)fclose(out);
  }
  return true;
}

// Which workloads collect, and what margin each one left: the third and
// fourth questions the baseline report asks.
static void PrintArenaTable(void) {
  (void)printf("\n%-20s %9s %9s %9s %9s %6s %5s %10s %10s %8s %9s\n",
               "workload", "cells", "capacity", "end-live", "peak-live",
               "margin", "gcs", "marked", "swept", "frames", "seconds");
  for (size_t i = 0; i < ran_count; i++) {
    const WorkloadRun* const run = &runs[i];
    const double margin =
        run->stats.total_slots == 0
            ? 0.0
            : 100.0 * (double)(run->stats.total_slots - EndLive(run)) /
                  (double)run->stats.total_slots;
    (void)printf(
        "%-20s %9llu %9zu %9zu %9zu %5.1f%% %5llu %10llu %10llu %8zu %9.6f\n",
        ran[i]->name, run->counters[FePerfAllocObject], run->stats.total_slots,
        EndLive(run), run->stats.peak_live_objects, margin,
        run->counters[FePerfGcCollection], run->counters[FePerfGcMarkNew],
        run->counters[FePerfGcSweepExamined], run->stats.peak_frame_depth,
        run->seconds);
  }
}

// Where the lookup and dispatch work comes from: the second question.
static void PrintWorkTable(void) {
  (void)printf("\n%-20s %10s %10s %10s %10s %9s %9s %10s %10s %8s\n",
               "workload", "env-lookup", "env-cell", "intern-cd", "name-cmp",
               "name-byte", "fn-hop", "eval-step", "eval-disp", "expand");
  for (size_t i = 0; i < ran_count; i++) {
    const WorkloadRun* const run = &runs[i];
    (void)printf(
        "%-20s %10llu %10llu %10llu %10llu %9llu %9llu %10llu %10llu %8llu\n",
        ran[i]->name, run->counters[FePerfEnvLookup],
        run->counters[FePerfEnvCell], run->counters[FePerfInternCandidate],
        run->counters[FePerfNameCompare], run->counters[FePerfNameByte],
        run->counters[FePerfFunctionHop], run->counters[FePerfEvalStep],
        run->counters[FePerfEvalDispatch], run->counters[FePerfMacroExpansion]);
  }
}

// Where the cells come from: the first question. Only the types something
// actually allocates get a column, so the table stays readable.
static void PrintAllocationTable(void) {
  static const FeType reported[] = {
      FeTPair,   FeTInteger, FeTSymbol, FeTString,    FeTVector,
      FeTDouble, FeTFn,      FeTMacro,  FeTPrimitive, FeTNativeFn};
  (void)printf("\n%-20s", "allocation by type");
  for (size_t t = 0; t < sizeof(reported) / sizeof(*reported); t++) {
    const char* const name =
        fe_perf_counter_name[FE_PERF_ALLOC_SLOT(reported[t])];
    (void)printf(" %11s", name == nullptr ? "?" : name + strlen("alloc_"));
  }
  (void)printf("\n");
  for (size_t i = 0; i < ran_count; i++) {
    (void)printf("%-20s", ran[i]->name);
    for (size_t t = 0; t < sizeof(reported) / sizeof(*reported); t++) {
      (void)printf(" %11llu", AllocOf(&runs[i], reported[t]));
    }
    (void)printf("\n");
  }
}

static void PrintExtras(void) {
  bool any = false;
  for (size_t i = 0; i < ran_count; i++) {
    for (size_t e = 0; e < runs[i].extra_count; e++) {
      if (!any) {
        (void)printf("\nper-workload probes\n");
        any = true;
      }
      (void)printf("%-20s %-24s %llu\n", ran[i]->name, runs[i].extra[e].name,
                   runs[i].extra[e].value);
    }
  }
}

static void PrintUsage(FILE* out) {
  (void)fprintf(out,
                "usage: perf_workloads [--json PATH] [--list]\n"
                "                      [--git-describe TEXT] "
                "[--binary-sha256 HEX]\n"
                "  --json PATH  write the machine-readable record set "
                "(\"-\" for stdout)\n"
                "  --list       print the battery and exit\n"
                "  --git-describe TEXT  the fe tree these numbers came from\n"
                "  --binary-sha256 HEX  digest of the measured binary\n");
}

static void PrintList(void) {
  for (int w = 0; w < WorkloadCount; w++) {
    (void)printf("%-20s %-8s %9zu bytes  %s\n", workloads[w].name,
                 workloads[w].family, workloads[w].arena, workloads[w].note);
  }
}

int main(int argc, char** argv) {
  const char* json_path = nullptr;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
      i++;
      json_path = argv[i];
    } else if (strcmp(argv[i], "--git-describe") == 0 && i + 1 < argc) {
      i++;
      artifact_describe = argv[i];
    } else if (strcmp(argv[i], "--binary-sha256") == 0 && i + 1 < argc) {
      i++;
      artifact_sha256 = argv[i];
    } else if (strcmp(argv[i], "--list") == 0) {
      PrintList();
      return EXIT_SUCCESS;
    } else {
      PrintUsage(stderr);
      return EXIT_FAILURE;
    }
  }

  for (int w = 0; w < WorkloadCount; w++) {
    if (!RunOne(&workloads[w], &runs[ran_count])) {
      (void)fprintf(stderr, "perf_workloads: FAIL %s\n", workloads[w].name);
      return EXIT_FAILURE;
    }
    ran[ran_count] = &workloads[w];
    ran_count++;
  }
  if (!CheckAcrossWorkloads()) {
    (void)fprintf(stderr, "perf_workloads: FAIL cross-workload checks\n");
    return EXIT_FAILURE;
  }
  PrintArenaTable();
  PrintWorkTable();
  PrintAllocationTable();
  PrintExtras();
  if (json_path != nullptr && !WriteJson(json_path)) {
    return EXIT_FAILURE;
  }
  (void)printf("\nperf_workloads: %zu workload(s) ok\n", ran_count);
  return EXIT_SUCCESS;
}

#else

int main(void) {
  (void)fprintf(stderr,
                "perf_workloads: built without FE_PERF_COUNTERS; "
                "build it with `make perf`\n");
  return EXIT_FAILURE;
}

#endif  // FE_PERF_COUNTERS
