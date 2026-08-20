// Copyright 2026 Fe contributors
// SPDX-License-Identifier: MIT

// Storage, names and JSON reporting for the counters declared in fe_perf.h.
// Everything here is inside `FE_PERF_COUNTERS`: an ordinary build compiles
// this file to nothing at all.

#include "fe_perf.h"

#if FE_PERF_COUNTERS

#include <stdio.h>
#include <string.h>

#include "fe.h"

unsigned long long fe_perf_counter[FePerfCounterCount];

// Designated initializers so a name can never drift away from the counter it
// labels: adding an enumerator without a name here leaves a null that
// `FePerfWriteJson` prints as "unnamed", rather than silently relabelling
// every counter after it.
const char* const fe_perf_counter_name[FePerfCounterCount] = {
    [FePerfAllocObject] = "alloc_object",
    [FePerfAllocRetyped] = "alloc_retyped",

    [FE_PERF_ALLOC_SLOT(FeTPair)] = "alloc_pair",
    [FE_PERF_ALLOC_SLOT(FeTFree)] = "alloc_free",
    [FE_PERF_ALLOC_SLOT(FeTNil)] = "alloc_nil",
    [FE_PERF_ALLOC_SLOT(FeTDouble)] = "alloc_double",
    [FE_PERF_ALLOC_SLOT(FeTInteger)] = "alloc_integer",
    [FE_PERF_ALLOC_SLOT(FeTSymbol)] = "alloc_symbol",
    [FE_PERF_ALLOC_SLOT(FeTString)] = "alloc_string",
    [FE_PERF_ALLOC_SLOT(FeTVector)] = "alloc_vector",
    [FE_PERF_ALLOC_SLOT(FeTFn)] = "alloc_fn",
    [FE_PERF_ALLOC_SLOT(FeTMacro)] = "alloc_macro",
    [FE_PERF_ALLOC_SLOT(FeTPrimitive)] = "alloc_primitive",
    [FE_PERF_ALLOC_SLOT(FeTNativeFn)] = "alloc_native_fn",
    [FE_PERF_ALLOC_SLOT(FeTPtr)] = "alloc_ptr",
    [FE_PERF_ALLOC_SLOT(FeTFex0)] = "alloc_fex0",
    [FE_PERF_ALLOC_SLOT(FeTFex1)] = "alloc_fex1",
    [FE_PERF_ALLOC_SLOT(FeTFex2)] = "alloc_fex2",
    [FE_PERF_ALLOC_SLOT(FeTSentinel)] = "alloc_sentinel",

    [FePerfGcCollection] = "gc_collection",
    [FePerfGcMarkVisit] = "gc_mark_visit",
    [FePerfGcMarkNew] = "gc_mark_new",
    [FePerfGcSweepExamined] = "gc_sweep_examined",
    [FePerfGcReclaimed] = "gc_reclaimed",

    [FePerfPayloadAlloc] = "payload_alloc",
    [FePerfPayloadByte] = "payload_byte",
    [FePerfPayloadCompact] = "payload_compact",
    [FePerfPayloadCompactMoved] = "payload_compact_moved",

    [FePerfVectorRef] = "vector_ref",
    [FePerfVectorSet] = "vector_set",
    [FePerfVectorElement] = "vector_element",

    [FePerfStringCell] = "string_cell",
    [FePerfStringByte] = "string_byte",
    [FePerfStringWalk] = "string_walk",
    [FePerfStringWalkCell] = "string_walk_cell",
    [FePerfStringWalkByte] = "string_walk_byte",
    [FePerfStringByteCopied] = "string_byte_copied",

    [FePerfInternLookup] = "intern_lookup",
    [FePerfInternMiss] = "intern_miss",
    [FePerfInternCandidate] = "intern_candidate",

    [FePerfNameCompare] = "name_compare",
    [FePerfNameByte] = "name_byte",

    [FePerfEnvLookup] = "env_lookup",
    [FePerfEnvCell] = "env_cell",
    [FePerfEnvBind] = "env_bind",

    [FePerfFunctionResolve] = "function_resolve",
    [FePerfFunctionHop] = "function_hop",

    [FePerfEvalStep] = "eval_step",
    [FePerfEvalDispatch] = "eval_dispatch",
    [FePerfFramePush] = "frame_push",
    [FePerfDispatchPrimitive] = "dispatch_primitive",
    [FePerfDispatchCallable] = "dispatch_callable",
    [FePerfDispatchNative] = "dispatch_native",
    [FePerfDispatchLambda] = "dispatch_lambda",
    [FePerfDispatchMacro] = "dispatch_macro",
    [FePerfMacroExpansion] = "macro_expansion",
};

void FePerfReset(void) {
  memset(fe_perf_counter, 0, sizeof(fe_perf_counter));
}

unsigned long long FePerfRead(FePerfCounter counter) {
  return fe_perf_counter[counter];
}

// `SetType` is the one place a cell's final type is established, so it is the
// one place the by-type charge can be made without a per-constructor rule
// that a new constructor could forget. Two cases the callers make:
//
//   * `FeTFree` is not an allocation at all. The free-list build at context
//     open and the sweep's reclaim both spell it, and both are counted
//     elsewhere (or not at all).
//   * a cell that currently reads as a pair was charged to `alloc_pair` by
//     `FeCons`. Only `BuildString` does this -- it takes its cell through
//     `FeCons` and then retypes it -- and the charge moves here so
//     `alloc_object` still equals the sum of the by-type slots.
void FePerfCountRetype(const FeObject* object, FeType type) {
  if (type == FeTFree) {
    return;
  }
  fe_perf_counter[FE_PERF_ALLOC_SLOT(type)]++;
  if (FeGetType(object) == FeTPair) {
    fe_perf_counter[FE_PERF_ALLOC_SLOT(FeTPair)]--;
    fe_perf_counter[FePerfAllocRetyped]++;
  }
}

void FePerfWriteJson(FILE* out, const FeArenaStats* stats) {
  fputs("{\n  \"counters\": {\n", out);
  for (int i = 0; i < FePerfCounterCount; i++) {
    const char* const name = fe_perf_counter_name[i];
    fprintf(out, "    \"%s\": %llu%s\n", name != nullptr ? name : "unnamed",
            fe_perf_counter[i], i + 1 < FePerfCounterCount ? "," : "");
  }
  // The arena gauges, reported beside the totals rather than tracked twice.
  fputs("  },\n  \"arena\": {\n", out);
  fprintf(out, "    \"total_slots\": %zu,\n", stats->total_slots);
  fprintf(out, "    \"free_slots\": %zu,\n", stats->free_slots);
  fprintf(out, "    \"peak_live_objects\": %zu,\n", stats->peak_live_objects);
  fprintf(out, "    \"collection_count\": %zu,\n", stats->collection_count);
  fprintf(out, "    \"peak_gc_stack_depth\": %zu,\n",
          stats->peak_gc_stack_depth);
  fprintf(out, "    \"frame_capacity\": %zu,\n", stats->frame_capacity);
  fprintf(out, "    \"peak_frame_depth\": %zu,\n", stats->peak_frame_depth);
  fprintf(out, "    \"peak_cleanup_stack_depth\": %zu,\n",
          stats->peak_cleanup_stack_depth);
  fprintf(out, "    \"peak_native_reentry\": %zu,\n",
          stats->peak_native_reentry);
  fprintf(out, "    \"allocation_failures\": %zu,\n",
          stats->allocation_failures);
  fprintf(out, "    \"payload_capacity_bytes\": %zu,\n",
          stats->payload_capacity_bytes);
  fprintf(out, "    \"payload_live_bytes\": %zu,\n", stats->payload_live_bytes);
  fprintf(out, "    \"payload_peak_bytes\": %zu,\n", stats->payload_peak_bytes);
  fprintf(out, "    \"payload_compaction_count\": %zu,\n",
          stats->payload_compaction_count);
  fprintf(out, "    \"payload_allocation_failures\": %zu\n",
          stats->payload_allocation_failures);
  fputs("  }\n}\n", out);
}

#else

// Counters compiled out. A translation unit needs a declaration.
typedef int FePerfCountersAreDisabled;

#endif  // FE_PERF_COUNTERS
