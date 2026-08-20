// Copyright 2026 Fe contributors
// SPDX-License-Identifier: MIT

#ifndef FE_PERF_H
#define FE_PERF_H

// Compile-time performance counters (kg's Phase 21.1,
// doc/plans/2026-08-18-elisp-data-model.md).
//
// Off unless the build asks for them: every macro below expands to nothing
// when `FE_PERF_COUNTERS` is 0, exactly as `FE_GC_STRESS` compiles the stress
// collector out, so the shipped interpreter carries no counter code and no
// counter storage. A non-counting build has identical `sizeof` results and
// identical generated code at every instrumented site.
//
// The counting build lives in its own object directory (`make perf`, objects
// under `perfobj/`), so a counting object can never be linked into an
// ordinary binary and an ordinary object can never be linked into a counting
// one.
//
// What the counters are for: Phase 21 measures fe's engine before Phase 22
// chooses a storage architecture, and a counter is preferred over a wall
// clock because it is deterministic -- a unit test can assert a relationship
// between two of them, and a sanitizer lane cannot make that flake. Counters
// answer "how much work of this shape", never "how long"; nothing here calls
// the clock.
//
// Storage is one process-wide static array, NOT a field of `FeContext`. The
// context lives inside the caller's arena, so a counting build that widened
// it would move the object/frame partition and measure a different arena
// from the one it is supposed to be describing. The price is that counters
// are totals across every context a process opens, while `FeArenaStats` is
// per context; a measurement that mixes the two uses one context.
//
// Peak live cells, GC-root depth and frame depth are deliberately NOT
// counters: `FeArenaStats` already tracks them in the shipped build, and
// `FePerfWriteJson` reports them beside the totals rather than duplicating
// the tracking.

#ifndef FE_PERF_COUNTERS
#define FE_PERF_COUNTERS 0
#endif

#if FE_PERF_COUNTERS

#include <stdio.h>

#include "fe.h"

typedef enum FePerfCounter {
  // Every cell the arena handed out: one per `MakeObject` that returned.
  // The invariant a test can assert is that this equals the sum of the
  // by-type block below.
  FePerfAllocObject,
  // Cells charged to one type and then given another: `BuildString` takes
  // its cell through `FeCons`, so the cell is a pair for the two statements
  // between the allocation and its `SetType`. `FePerfCountRetype` moves the
  // charge, and this counts the corrections so the move is visible rather
  // than silent.
  FePerfAllocRetyped,

  // Allocations by final type. One slot per `FeType`, contiguous and in
  // `FeType` order, so the counter for a type is `FE_PERF_ALLOC_SLOT(type)`
  // -- arithmetic rather than a switch a new type could silently miss. The
  // three slots no allocation can reach (`FeTFree`, `FeTNil`,
  // `FeTSentinel`) are named anyway, so a zero there reads as "impossible"
  // rather than "unnamed".
  FePerfAllocType,
  FePerfAllocTypeLast = FePerfAllocType + (int)FeTSentinel,

  // The collector. COLLECTION counts `CollectGarbage` calls; MARK_VISIT
  // every object the mark walk descends into, including a revisit that
  // stops immediately at an already-set mark bit; MARK_NEW the subset that
  // was not marked yet, i.e. the live set; SWEEP_EXAMINED every cell the
  // sweep looks at, free ones included, which is what makes the sweep's
  // arena-proportional cost visible; RECLAIMED the cells it returned to the
  // free list.
  FePerfGcCollection,
  FePerfGcMarkVisit,
  FePerfGcMarkNew,
  FePerfGcSweepExamined,
  FePerfGcReclaimed,

  // The payload region (Phase 23). ALLOC counts the blocks the bump
  // allocator handed out and BYTE the region bytes they took, block headers
  // included, so the two together are what a region's capacity is spent on.
  // COMPACT counts `CompactPayloads` calls -- every collection makes one,
  // whether or not anything died -- and COMPACT_MOVED the surviving blocks
  // it slid down over a reclaimed one, which is the memmove work a
  // compaction actually costs. A shipped build leaves all four at zero: no
  // release type owns a payload until Phase 25.
  FePerfPayloadAlloc,
  FePerfPayloadByte,
  FePerfPayloadCompact,
  FePerfPayloadCompactMoved,

  // Vectors (Phase 24), the payload region's first release consumer. REF and
  // SET are element reads and writes through the two accessors every path
  // goes through -- `aref`, `aset`, `elt`, the printer's element loop, and
  // every constructor's fill -- and ELEMENT is the slots construction
  // published, i.e. how wide the vectors a workload built were. The three
  // are what the O(1) gate reads: a fixed number of random accesses must
  // charge the same REF count, and every other counter the same number, at
  // n = 8 and at n = 8192, because a vector's element address is arithmetic
  // on its block and not a walk.
  FePerfVectorRef,
  FePerfVectorSet,
  FePerfVectorElement,

  // Strings, whose representation is a chain of `StringBufferSize`-byte
  // cells. OBJECT is how many strings were made and CELL and BYTE what they
  // cost: cells `BuildString` allocated and payload bytes it stored. OBJECT
  // is the one the Phase 22 ADR had to bound rather than read -- a string of
  // L bytes takes ceil(L/7) cells, so cells and bytes together only bracket
  // the object count between `bytes/7` and `cells`, and the payload pool a
  // string representation would need is `bytes + header * OBJECT`. The WALK
  // trio is traversal -- `CopyStoredStringBytes` calls, the cells they
  // visited and the bytes they measured -- and BYTE_COPIED is the subset
  // actually memcpy'd out, which is smaller than WALK_BYTE because a caller
  // that needs a length first walks the chain twice.
  FePerfStringObject,
  FePerfStringCell,
  FePerfStringByte,
  FePerfStringWalk,
  FePerfStringWalkCell,
  FePerfStringWalkByte,
  FePerfStringByteCopied,

  // Interning. LOOKUP counts `FindInternedSymbol` calls (`FeMakeSymbol` and
  // `intern-soft`), MISS the subset that found nothing, and CANDIDATE the
  // interned symbols examined -- the linear `symbol_list` scan Phase 26
  // exists to index.
  FePerfInternLookup,
  FePerfInternMiss,
  FePerfInternCandidate,

  // Symbol-name comparisons, by cell chain: COMPARE counts `IsStringEqual`
  // calls and BYTE the name bytes they examined (padding included -- the
  // comparison is per cell, not per meaningful byte). Interning is one
  // caller, one comparison per candidate, so COMPARE minus
  // FePerfInternCandidate is `IsNamedSymbol`'s share.
  FePerfNameCompare,
  FePerfNameByte,

  // Lexical environments, which are alists. LOOKUP counts the searches
  // (`GetBound` and `HasLexicalBinding`), CELL the alist cells they
  // examined, and BIND the binding pairs `Bind` allocated for parameters
  // and `let`.
  FePerfEnvLookup,
  FePerfEnvCell,
  FePerfEnvBind,

  // The function namespace. RESOLVE counts `ResolveFunctionCallable` calls
  // and HOP the `defalias` indirections they followed.
  FePerfFunctionResolve,
  FePerfFunctionHop,

  // The evaluator. STEP is the budget charge, the finest unit of evaluator
  // work; DISPATCH is one turn of `RunEvaluationLoop`; FRAME_PUSH is
  // `AllocateFrame`. The four DISPATCH_* counters are the broad callable
  // families: a primitive, an ordinary callable's argument list started
  // (native or lambda, not yet told apart), the native actually invoked,
  // the lambda actually applied, and a macro call entered.
  FePerfEvalStep,
  FePerfEvalDispatch,
  FePerfFramePush,
  FePerfDispatchPrimitive,
  FePerfDispatchCallable,
  FePerfDispatchNative,
  FePerfDispatchLambda,
  FePerfDispatchMacro,
  // Macro transformer bodies entered: an ordinary macro call and a
  // reflective `macroexpand-1`/`macroexpand` step both land here, because
  // both evaluate the transformer once. fe expands on every invocation, so
  // this counter is the one a caching decision would be argued from.
  FePerfMacroExpansion,

  FePerfCounterCount,
} FePerfCounter;

extern unsigned long long fe_perf_counter[FePerfCounterCount];
extern const char* const fe_perf_counter_name[FePerfCounterCount];

#define FE_PERF_INC(counter) ((void)(fe_perf_counter[counter]++))
#define FE_PERF_ADD(counter, n) \
  ((void)(fe_perf_counter[counter] += (unsigned long long)(n)))
// The slot a final type is charged to; see `FePerfAllocType`. Both operands
// are cast, because two enumeration types may not be added directly.
#define FE_PERF_ALLOC_SLOT(type) ((int)FePerfAllocType + (int)(type))
// The by-type allocation charge.
#define FE_PERF_ALLOC(type) \
  ((void)(fe_perf_counter[FE_PERF_ALLOC_SLOT(type)]++))
// Move a cell's charge from the type it was provisionally counted as to the
// type it is being given. A statement, not an expression, so the off build's
// expansion needs the same trailing semicolon at the call site.
#define FE_PERF_RETYPE(object, type) FePerfCountRetype((object), (type))

// Zero every counter. The counting build starts zeroed; this is for a
// measurement that wants one workload rather than a whole process.
void FePerfReset(void);

// One counter, for an in-process reader (a unit test, or Phase 21.2's
// workload runner).
[[nodiscard]] unsigned long long FePerfRead(FePerfCounter counter);

// Move a cell's allocation charge, per `FePerfAllocRetyped`. Not called
// directly: `FE_PERF_RETYPE` is the spelling that disappears in a
// non-counting build.
void FePerfCountRetype(const FeObject* object, FeType type);

// Write every counter, and the arena gauges beside them, as one JSON object.
// `stats` is `FeGetArenaStats` of the context being described and must not be
// null; it is reported rather than copied into counters because the gauges
// are already tracked in the shipped build.
void FePerfWriteJson(FILE* out, const FeArenaStats* stats);

#else

#define FE_PERF_INC(counter) ((void)0)
#define FE_PERF_ADD(counter, n) ((void)0)
#define FE_PERF_ALLOC(type) ((void)0)
#define FE_PERF_RETYPE(object, type) ((void)0)

#endif  // FE_PERF_COUNTERS

#endif  // FE_PERF_H
