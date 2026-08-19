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

static_assert(FE_API_VERSION == 12);
static_assert(FE_LANGUAGE_VERSION == 15);

// The arena sizes, named rather than spelled at each use so that a record's
// `arena_bytes` can be read against the reason its workload picked it.
enum {
  // Collects several times over a few thousand allocations: 2694 cells, of
  // which a bare context open already holds 892.
  ArenaTight = 96 * 1024,
  // Still collects under the dense-live shape, but with room for a live set
  // worth marking: 11910 cells.
  ArenaSmall = 256 * 1024,
  // kg's own configuration, and the one its bench cases are read against:
  // 56147 cells.
  ArenaHost = 1024 * 1024,
  // 233094 cells. 8192 interned symbols do not fit in kg's arena -- a symbol
  // costs its own object, two cells of the name/function/value spine, its
  // `symbol_list` link and its name chain -- and all three intern tiers use
  // this one size so that a cross-tier subtraction is apples to apples.
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
  CHECK(CounterOf(run, FePerfGcCollection) == 0);
  CHECK(CounterOf(run, FePerfAllocObject) ==
        (unsigned long long)(run->stats.total_slots - run->stats.free_slots));
  CHECK(run->stats.allocation_failures == 0);
  CHECK(run->stats.collection_count == 0);
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
  CHECK(CounterOf(run, FePerfGcCollection) == 1);
  // `FeCloseContext` clears every root before collecting, so nothing is
  // reachable and every cell the open took comes back -- and nothing is
  // marked on the way.
  CHECK(CounterOf(run, FePerfGcReclaimed) == CounterOf(run, FePerfAllocObject));
  CHECK(CounterOf(run, FePerfGcMarkNew) == 0);
  // The open half costs what any other open costs: the same names and the
  // same tables whatever the arena (see `CheckOpenIsArenaIndependent`), so
  // the scratch context's allocations equal the harness context's.
  CHECK(CounterOf(run, FePerfAllocObject) == run->open_cells);
  // The sweep is proportional to the ARENA and not to the live set: it
  // examines every slot, and the scratch arena is the same size as the
  // harness's, so the counter and that arena's capacity are one number.
  // Phase 22 changes this ratio; it should not quietly change the identity.
  CHECK(CounterOf(run, FePerfGcSweepExamined) ==
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
  CHECK(run->stats.collection_count == 0);
  CHECK(run->stats.allocation_failures == 0);
  CHECK((unsigned long long)(run->stats.total_slots - run->stats.free_slots) ==
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
  unsigned long long mark = FePerfRead(FePerfInternCandidate);
  const FeObject* const fresh = FeMakeSymbol(context, "fe-perf-absent-name");
  const unsigned long long miss = FePerfRead(FePerfInternCandidate) - mark;
  FeRestoreGC(context, gc);

  // A hit on the name just interned: a new symbol goes on the head of
  // `symbol_list`, so this examines exactly one candidate.
  mark = FePerfRead(FePerfInternCandidate);
  const FeObject* const again = FeMakeSymbol(context, "fe-perf-absent-name");
  const unsigned long long head = FePerfRead(FePerfInternCandidate) - mark;
  FeRestoreGC(context, gc);

  // A hit on a name the context interned at open, which is therefore past
  // every symbol this workload made.
  mark = FePerfRead(FePerfInternCandidate);
  const FeObject* const core = FeMakeSymbol(context, "car");
  const unsigned long long deep = FePerfRead(FePerfInternCandidate) - mark;
  FeRestoreGC(context, gc);

  // The answer: interning is idempotent on the name, and finds a symbol.
  CHECK(fresh == again);
  CHECK(FeGetType(fresh) == FeTSymbol);
  CHECK(FeGetType(core) == FeTSymbol);
  AddExtra(run, "symbols_before_miss", before);
  AddExtra(run, "miss_candidates", miss);
  AddExtra(run, "hit_head_candidates", head);
  AddExtra(run, "hit_core_candidates", deep);
  (void)snprintf(run->answer, sizeof(run->answer),
                 "%zu interned, miss scanned %llu", n, miss);
  return true;
}

static bool CheckIntern(const Workload* workload, const WorkloadRun* run) {
  const unsigned long long n = (unsigned long long)workload->param;
  CHECK(AllocationIsPartitioned(run));
  CHECK(run->stats.allocation_failures == 0);
  // Nothing collected, so no symbol this workload made was ever reclaimed and
  // the obarray length below is exact.
  CHECK(CounterOf(run, FePerfGcCollection) == 0);
  // n populating misses plus the probe's own, then two hits after it.
  CHECK(CounterOf(run, FePerfInternMiss) == n + 1);
  CHECK(CounterOf(run, FePerfInternLookup) == n + 3);
  // THE SHAPE PHASE 26 EXISTS TO BREAK: a miss examines every interned
  // symbol, because the obarray is a list and the scan has to reach its end
  // before it can conclude anything.
  CHECK(ExtraOf(run, "miss_candidates") == ExtraOf(run, "symbols_before_miss"));
  // A hit on the head is O(1) already; a hit on a name interned before this
  // workload started is past everything it made.
  CHECK(ExtraOf(run, "hit_head_candidates") == 1);
  CHECK(ExtraOf(run, "hit_core_candidates") > n);
  // No lambda is called anywhere in this workload, and `IsNamedSymbol` --
  // which byte-compares "&optional"/"&rest" against every parameter of every
  // call -- is the only other caller of the name comparison. So here the two
  // counters are the same measurement: one comparison per candidate.
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
// 21.2 item 7: strings at 0, 7, 8, 256 and 8192 bytes. 7 and 8 straddle the
// `StringBufferSize` cell boundary, which is the point of both.
// ---------------------------------------------------------------------------

static bool BodyString(const Workload* workload, WorkloadRun* run) {
  FeContext* const context = run->context;
  const size_t length = workload->param;
  const size_t gc = FeSaveGC(context);
  const FeObject* const string = FeMakeString(context, string_source);
  CHECK(FeGetType(string) == FeTString);
  // The answer: the bytes come back, all of them, in order. A string whose
  // chain stopped being walked correctly would allocate exactly the same
  // cells and read exactly as plausible.
  CHECK(FeStringByteLength(context, string) == length);
  CHECK(FeCopyStringBytes(context, string, string_copy, sizeof(string_copy)));
  CHECK(memcmp(string_copy, string_source, length) == 0);
  FeRestoreGC(context, gc);
  (void)snprintf(run->answer, sizeof(run->answer), "%zu bytes round-tripped",
                 length);
  return true;
}

static bool CheckString(const Workload* workload, const WorkloadRun* run) {
  const size_t length = workload->param;
  // The seven-byte cell chain, spelled from `StringBufferSize` rather than
  // from a 7 so that the representation is what is being asserted. Phase 25
  // replaces this with a length-bearing payload, and every line below is
  // meant to fail loudly when it does.
  const unsigned long long cells =
      length == 0 ? 1
                  : (unsigned long long)((length + StringBufferSize - 1) /
                                         StringBufferSize);
  CHECK(AllocationIsPartitioned(run));
  CHECK(CounterOf(run, FePerfStringCell) == cells);
  CHECK(AllocOf(run, FeTString) == cells);
  // Every string cell is a pair the constructor retyped, and the string is
  // the only thing this workload allocates at all.
  CHECK(CounterOf(run, FePerfAllocRetyped) == cells);
  CHECK(CounterOf(run, FePerfAllocObject) == cells);
  CHECK(CounterOf(run, FePerfStringByte) == (unsigned long long)length);
  // A string has no stored length, so a caller that needs one walks the chain
  // to find it and then walks it again to copy: three walks here, one for
  // `FeStringByteLength` and two inside `FeCopyStringBytes`.
  CHECK(CounterOf(run, FePerfStringWalk) == 3);
  CHECK(CounterOf(run, FePerfStringWalkCell) == 3 * cells);
  CHECK(CounterOf(run, FePerfStringWalkByte) == 3 * (unsigned long long)length);
  CHECK(CounterOf(run, FePerfStringByteCopied) == (unsigned long long)length);
  CHECK(run->stats.allocation_failures == 0);
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

static bool CheckSparse(const Workload* workload, const WorkloadRun* run) {
  (void)workload;
  CHECK(CheckCollecting(run));
  // Sparse: the loop keeps nothing, so collection reclaims far more than it
  // marks, and the live set it leaves is a small fraction of the arena.
  CHECK(CounterOf(run, FePerfGcReclaimed) > CounterOf(run, FePerfGcMarkNew));
  CHECK(EndLive(run) * 2 < run->stats.total_slots);
  return true;
}

static bool CheckDense(const Workload* workload, const WorkloadRun* run) {
  (void)workload;
  CHECK(CheckCollecting(run));
  // Dense: the loop keeps everything, so marking dominates reclaiming -- the
  // exact mirror of the sparse case, on the same collector -- and the live
  // set it leaves is most of the arena.
  CHECK(CounterOf(run, FePerfGcMarkNew) > CounterOf(run, FePerfGcReclaimed));
  CHECK(EndLive(run) * 2 > run->stats.total_slots);
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
  // Every tier opens the same arena from the same starting state, so two
  // misses differ by exactly the number of extra symbols interned before
  // them: the scan is O(interned symbols), with a slope of exactly one.
  CHECK(ExtraOf(mid, "miss_candidates") - ExtraOf(tiny, "miss_candidates") ==
        1024 - 128);
  CHECK(ExtraOf(big, "miss_candidates") - ExtraOf(mid, "miss_candidates") ==
        8192 - 1024);
  // Total interning work is quadratic in the tier size. Stated as a ratio
  // rather than a constant, so that it stays true when the number of symbols
  // a context open interns changes.
  CHECK(CounterOf(mid, FePerfInternCandidate) >
        8 * CounterOf(tiny, FePerfInternCandidate));
  CHECK(CounterOf(big, FePerfInternCandidate) >
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
  // The cell boundary, stated as the thing it is: one byte more than a cell
  // holds costs a whole extra cell, and an empty string still costs one.
  CHECK(CounterOf(empty, FePerfStringCell) == 1);
  CHECK(CounterOf(seven, FePerfStringCell) == 1);
  CHECK(CounterOf(eight, FePerfStringCell) == 2);
  // A 1171-cell chain costs no root-stack slots at all: `BuildString` does not
  // leave one entry per cell behind, so a string longer than the 4032-slot
  // practical ceiling is not by itself a GC-root problem. Worth pinning
  // because it is the opposite of what the trap in this file's header would
  // lead one to expect, and because Phase 25 changes the construction.
  CHECK(huge->stats.peak_gc_stack_depth == empty->stats.peak_gc_stack_depth);
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
  return true;
}

// ---------------------------------------------------------------------------
// Reporting. The JSON is what kg consumes; the tables are for a human reading
// a terminal, and their columns are the four questions Phase 21's baseline
// report has to answer.
// ---------------------------------------------------------------------------

static void WriteArenaJson(FILE* out, const FeArenaStats* stats) {
  // The same ten keys, in the same order, that `FePerfWriteJson` writes: a
  // consumer that can read a counting `fe`'s `$FE_PERF_OUT` file can read
  // this one's "arena" object without a second parser.
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
  (void)fprintf(out, "      \"allocation_failures\": %zu\n",
                stats->allocation_failures);
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
  (void)fprintf(out, "{\n  \"schema\": \"fe-perf-workloads/1\",\n");
  (void)fprintf(out, "  \"fe_api_version\": %d,\n", FE_API_VERSION);
  (void)fprintf(out, "  \"fe_language_version\": %d,\n", FE_LANGUAGE_VERSION);
  (void)fprintf(out, "  \"string_buffer_size\": %d,\n", (int)StringBufferSize);
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
  static const FeType reported[] = {FeTPair,   FeTInteger,   FeTSymbol,
                                    FeTString, FeTDouble,    FeTFn,
                                    FeTMacro,  FeTPrimitive, FeTNativeFn};
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
                "  --json PATH  write the machine-readable record set "
                "(\"-\" for stdout)\n"
                "  --list       print the battery and exit\n");
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
