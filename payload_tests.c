// Copyright 2026 the kg authors
// SPDX-License-Identifier: MIT
//
// The payload substrate's own regression test (Phase 23.1 of kg's
// doc/plans/2026-08-18-elisp-data-model.md; the Phase 22 ADR's Design B).
//
// Its own binary rather than a case inside `test_api`, for the same reason
// `gc_stress` is: it needs a build the ordinary one is not.
// `FE_PAYLOAD_TEST_OBJECT` gives fe a type that owns a payload, which a
// shipped interpreter deliberately has none of -- strings migrate in Phase 25
// and a Lisp-visible aggregate arrives in Phase 24 -- so without it there is
// nothing to allocate a block for and every assertion below would be vacuous.
//
// What each group proves is stated where it is asserted. The two properties
// the whole file exists for are:
//
//   * a block's ADDRESS is not its identity; the handle is, and the compactor
//     rewrites it. Every test here that asserts an address does so because
//     compaction preserves ALLOCATION ORDER, which is what makes the address
//     predictable at all;
//   * the collector reaches an object reachable only through a payload, and
//     does so through the existing pointer-reversal trampoline, so an
//     aggregate's WIDTH costs no C stack and a CHAIN of aggregates costs none
//     either.
//
// The GC-stack discipline here is the one every C loop in fe uses, and it is
// load-bearing rather than incidental: `MakeObject` pushes each new object
// onto the root stack, so a test that collects without restoring a checkpoint
// first would find every "dead" object alive and prove nothing at all.

#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"
#include "fe_internal.h"

static_assert(FE_API_VERSION == 12);
static_assert(FE_LANGUAGE_VERSION == 15);

#ifndef FE_GC_STRESS
#define FE_GC_STRESS 0
#endif

#define CHECK(condition)                                            \
  do {                                                              \
    if (!(condition)) {                                             \
      (void)fprintf(stderr, "payload_tests: failed: %s at %s:%d\n", \
                    #condition, __FILE__, __LINE__);                \
      return false;                                                 \
    }                                                               \
  } while (false)

enum {
  // Big enough that a 25% carve leaves a region worth compacting and cells to
  // spare, small enough that the deliberate-exhaustion cases fill it quickly.
  PayloadArenaSize = 256 * 1024,
  // The C-stack depth proof's own arena, malloc'd rather than static: 100 000
  // aggregates cost a cell, a 32-byte block header and one child word each.
  DepthArenaSize = 64u << 20,
};

typedef struct PayloadArena {
  alignas(max_align_t) unsigned char bytes[PayloadArenaSize];
} PayloadArena;

static PayloadArena arena;

typedef struct HostState {
  jmp_buf jump;
  bool raised;
  char message[256];
} HostState;

static HostState host;

[[noreturn]] static void HandleError(
    // cppcheck-suppress constParameterCallback
    FeContext* context,
    const char* message,
    // cppcheck-suppress constParameterCallback
    FeObject* call_trace) {
  (void)context;
  (void)call_trace;
  host.raised = true;
  (void)snprintf(host.message, sizeof(host.message), "%s", message);
  longjmp(host.jump, 1);
}

static FeContext* OpenPayloadContext(size_t percent) {
  FeContext* const context =
      OpenContextWithPayload(arena.bytes, sizeof(arena.bytes), percent);
  if (context == nullptr) {
    return nullptr;
  }
  host.raised = false;
  host.message[0] = '\0';
  FeSetErrorFn(context, HandleError);
  return context;
}

// The condition the host is handed, rendered after the raise has unwound
// rather than inside the handler: rendering runs the printer, and the printer
// is not something to start from an error callback.
static bool ConditionIs(FeContext* context, const char* expected) {
  char rendered[128];
  (void)FeToString(context, FeGetCondition(context), rendered,
                   sizeof(rendered));
  CHECK(strcmp(rendered, expected) == 0);
  return true;
}

// A handle is a one-based offset to the block's payload, so the first block in
// a fresh region has a known handle and every later one follows from the sizes
// before it. This is the property every address assertion below rests on.
static FePayloadHandle ExpectedHandle(size_t block_offset) {
  return block_offset + sizeof(FePayloadBlock) + 1;
}

// ---------------------------------------------------------------------------
// The region and the allocator
// ---------------------------------------------------------------------------

// `FeOpenContext` carves nothing, which is the behaviour the Phase 22 ADR
// requires it to keep; a percentage carve takes its bytes out of the cells and
// out of nothing else, so the two pools are priced against each other and the
// frame region -- Phase 21's measured scarcity -- is untouched by the split.
static bool TestRegionCarve(void) {
  FeContext* plain = OpenPayloadContext(0);
  CHECK(plain != nullptr);
  const FeArenaStats plain_stats = FeGetArenaStats(plain);
  const size_t plain_cells = plain_stats.total_slots;
  const size_t plain_frames = plain_stats.frame_capacity;
  CHECK(plain->payload_capacity == 0);
  CHECK(plain->payload_base != nullptr);
  FeCloseContext(plain);

  FeContext* carved = OpenPayloadContext(PayloadArenaPercent);
  CHECK(carved != nullptr);
  const FeArenaStats carved_stats = FeGetArenaStats(carved);
  CHECK(carved->payload_capacity > 0);
  CHECK(carved->payload_capacity % FePayloadAlignment == 0);
  CHECK(carved_stats.frame_capacity == plain_frames);
  // The cells the region cost are the region's own bytes, to within the one
  // cell the floor division drops.
  const size_t lost_cells = plain_cells - carved_stats.total_slots;
  CHECK(lost_cells >= carved->payload_capacity / sizeof(FeObject));
  CHECK(lost_cells <= carved->payload_capacity / sizeof(FeObject) + 1);
  (void)printf(
      "payload_tests: carve %d%%: %zu cells beside %zu payload bytes, against "
      "%zu cells with no region; frames %zu either way\n",
      (int)PayloadArenaPercent, carved_stats.total_slots,
      carved->payload_capacity, plain_cells, plain_frames);
  FeCloseContext(carved);
  return true;
}

// Alignment and bump order together: one byte costs a whole unit, seven cost
// the same unit, eight still cost one and nine cost two. The handles are the
// offsets those sizes imply, in the order the blocks were published.
static bool TestBumpAllocationOrder(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  FeObject* const first = MakeAggregate(context, 0, 1);
  FeObject* const second = MakeAggregate(context, 0, 7);
  FeObject* const third = MakeAggregate(context, 0, 8);
  FeObject* const fourth = MakeAggregate(context, 0, 9);
  const size_t block = sizeof(FePayloadBlock);
  CHECK(AggregateHandle(first) == ExpectedHandle(0));
  CHECK(AggregateHandle(second) == ExpectedHandle(block + 8));
  CHECK(AggregateHandle(third) == ExpectedHandle(2 * (block + 8)));
  CHECK(AggregateHandle(fourth) == ExpectedHandle(3 * (block + 8)));
  // Every payload starts on an alignment unit, which is what lets one block
  // hold `FeObject*` children without a second alignment rule.
  const unsigned char* const tail = AggregateBytes(context, fourth);
  CHECK((uintptr_t)tail % FePayloadAlignment == 0);
  CHECK(context->payload_used == 3 * (block + 8) + block + 16);
  CHECK(context->payload_peak_used == context->payload_used);
  FeCloseContext(context);
  return true;
}

// The region's own edge, from both sides: a request that exactly fills it
// succeeds, and the same request plus one byte does not. Sized from the
// capacity at run time rather than from a constant, so the case stays exact
// whatever the arena is.
static bool TestExactFitAndOneByteOver(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  const size_t room = context->payload_capacity - sizeof(FePayloadBlock);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  FeObject* const exact = MakeAggregate(context, 0, room);
  CHECK(AggregateHandle(exact) == ExpectedHandle(0));
  CHECK(context->payload_used == context->payload_capacity);
  CHECK(context->payload_allocation_failures == 0);
  FeCloseContext(context);

  context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) == 0) {
    (void)MakeAggregate(context, 0, room + 1);
    CHECK(false);
  }
  CHECK(host.raised);
  CHECK(ConditionIs(context, "(payload-exhaustion)"));
  CHECK(context->payload_allocation_failures == 1);
  // Refused, not half-done: nothing at all was taken from the region.
  CHECK(context->payload_used == 0);
  FeCloseContext(context);
  return true;
}

// ---------------------------------------------------------------------------
// The collector: marking through a payload, and compaction
// ---------------------------------------------------------------------------

// The mark phase reaches objects reachable ONLY through a payload -- one level
// down and two. Without the collector's payload arm they are swept and the
// reads below find free cells, which is the failure this case exists to catch.
static bool TestMarkReachesPayloadChildren(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  const size_t base = FeSaveGC(context);
  FeObject* const holder = MakeAggregate(context, 2, 0);
  SetAggregateChild(context, holder, 0, FeMakeInteger(context, 4242));
  SetAggregateChild(context, holder, 1, MakeAggregate(context, 1, 0));
  SetAggregateChild(context, AggregateChild(context, holder, 1), 0,
                    FeMakeString(context, "nested"));
  // Only the holder stays rooted, so everything below it is reachable through
  // payload words and through nothing else.
  FeRestoreGC(context, base);
  FePushGC(context, holder);
  FeCollectGarbage(context);
  FeCollectGarbage(context);

  CHECK(FeToInteger(context, AggregateChild(context, holder, 0)) == 4242);
  char rendered[64];
  (void)FeToString(
      context, AggregateChild(context, AggregateChild(context, holder, 1), 0),
      rendered, sizeof(rendered));
  CHECK(strcmp(rendered, "nested") == 0);
  FeCloseContext(context);
  return true;
}

// A block whose owner published a REPLACEMENT is dead though its owner is very
// much alive: that is the half of the liveness rule an owner's mark bit cannot
// express, and it is what will make a growing string affordable in Phase 25.
static bool TestReplacedBlockIsReclaimed(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  const size_t base = FeSaveGC(context);
  FeObject* const owner = MakeAggregate(context, 0, 64);
  const size_t one_block = context->payload_used;
  PublishPayload(context, owner, 0, 64);
  CHECK(context->payload_used == 2 * one_block);
  memcpy(AggregateBytes(context, owner), "second", 7);

  FeRestoreGC(context, base);
  FePushGC(context, owner);
  FeCollectGarbage(context);
  // One block's worth left, and the owner names the survivor -- which now sits
  // at the region's base, because compaction preserves order and the block
  // before it died.
  CHECK(context->payload_used == one_block);
  CHECK(AggregateHandle(owner) == ExpectedHandle(0));
  CHECK(strcmp((const char*)AggregateBytes(context, owner), "second") == 0);
  CHECK(context->payload_compaction_count == 1);
  FeCloseContext(context);
  return true;
}

// Compaction's addresses, asserted exactly. Five blocks are published in one
// order; the second and fourth die; the survivors slide down IN THAT ORDER, so
// their handles after the collection are computable from the sizes alone. An
// allocator that reordered survivors, or a compactor that packed them by size,
// would pass every relationship test and fail this one.
static bool TestCompactionAddressesAreDeterministic(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  static const size_t sizes[] = {8, 64, 16, 128, 24};
  FeObject* blocks[5] = {nullptr};
  const size_t block = sizeof(FePayloadBlock);
  const size_t base = FeSaveGC(context);
  size_t offset = 0;
  for (size_t i = 0; i < 5; i++) {
    blocks[i] = MakeAggregate(context, 0, sizes[i]);
    CHECK(AggregateHandle(blocks[i]) == ExpectedHandle(offset));
    // A recognisable pattern per block, so a compaction that copies the wrong
    // bytes is visible rather than merely suspicious.
    memset(AggregateBytes(context, blocks[i]), 'a' + (int)i, sizes[i]);
    offset += block + sizes[i];
  }
  CHECK(context->payload_used == offset);

  FeRestoreGC(context, base);
  FePushGC(context, blocks[0]);
  FePushGC(context, blocks[2]);
  FePushGC(context, blocks[4]);
  FeCollectGarbage(context);

  CHECK(context->payload_used == 3 * block + sizes[0] + sizes[2] + sizes[4]);
  CHECK(AggregateHandle(blocks[0]) == ExpectedHandle(0));
  CHECK(AggregateHandle(blocks[2]) == ExpectedHandle(block + sizes[0]));
  CHECK(AggregateHandle(blocks[4]) ==
        ExpectedHandle(2 * block + sizes[0] + sizes[2]));
  // ...and their bytes came with them.
  for (size_t i = 0; i < sizes[2]; i++) {
    CHECK(AggregateBytes(context, blocks[2])[i] == 'c');
  }
  for (size_t i = 0; i < sizes[4]; i++) {
    CHECK(AggregateBytes(context, blocks[4])[i] == 'e');
  }
  FeCloseContext(context);
  return true;
}

// The same shape with the sizes chosen so that a survivor's move OVERLAPS its
// own storage: a 128-byte block slides down by 40 bytes, which is a `memmove`
// and would be silently wrong under `memcpy`.
static bool TestSlidingOverlap(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  enum { Length = 128 };
  const size_t base = FeSaveGC(context);
  (void)MakeAggregate(context, 0, 8);
  FeObject* const survivor = MakeAggregate(context, 0, Length);
  for (size_t i = 0; i < Length; i++) {
    // Clause 1: the base is re-derived per write rather than hoisted, which is
    // also what makes this loop correct under the poison knob.
    AggregateBytes(context, survivor)[i] = (unsigned char)(i + 1);
  }
  // The dead block ahead of it is 8 bytes, so the survivor slides down by 40
  // over its own 128: source and destination overlap.
  static_assert(sizeof(FePayloadBlock) + 8 < Length);

  FeRestoreGC(context, base);
  FePushGC(context, survivor);
  FeCollectGarbage(context);

  CHECK(AggregateHandle(survivor) == ExpectedHandle(0));
  for (size_t i = 0; i < Length; i++) {
    CHECK(AggregateBytes(context, survivor)[i] == (unsigned char)(i + 1));
  }
  FeCloseContext(context);
  return true;
}

// Live and dead owners mixed, with the survivors at the FRONT, the MIDDLE and
// the BACK of the run of cells the owners occupy. The sweep walks the object
// array linearly and the compactor walks the region linearly, and those two
// orders are not the same one, so a compactor that assumed they were would
// pass a test whose survivors sat together.
static bool TestLiveDeadMixture(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  enum { Count = 300, Middle = Count / 2 };
  FeObject* survivors[3] = {nullptr, nullptr, nullptr};
  const size_t base = FeSaveGC(context);
  size_t first_index = 0;
  size_t last_index = 0;
  for (size_t i = 0; i < Count; i++) {
    FeObject* const owner = MakeAggregate(context, 1, 16);
    SetAggregateChild(context, owner, 0, FeMakeInteger(context, (int64_t)i));
    const size_t index = (size_t)(owner - context->objects);
    first_index = i == 0 ? index : first_index;
    last_index = i + 1 == Count ? index : last_index;
    survivors[0] = i == 0 ? owner : survivors[0];
    survivors[1] = i == Middle ? owner : survivors[1];
    survivors[2] = i + 1 == Count ? owner : survivors[2];
  }
  // The survivors really are spread through the object array: the free list
  // hands out cells in one direction, so the first and last are far apart.
  CHECK(first_index != last_index);

  FeRestoreGC(context, base);
  for (size_t i = 0; i < 3; i++) {
    FePushGC(context, survivors[i]);
  }
  FeCollectGarbage(context);

  const size_t each = sizeof(FePayloadBlock) + 16 + sizeof(FeObject*);
  CHECK(context->payload_used == 3 * each);
  for (size_t i = 0; i < 3; i++) {
    CHECK(AggregateHandle(survivors[i]) == ExpectedHandle(i * each));
  }
  CHECK(FeToInteger(context, AggregateChild(context, survivors[0], 0)) == 0);
  CHECK(FeToInteger(context, AggregateChild(context, survivors[1], 0)) ==
        Middle);
  CHECK(FeToInteger(context, AggregateChild(context, survivors[2], 0)) ==
        Count - 1);
  FeCloseContext(context);
  return true;
}

// Closing a context with live payloads. `FeCloseContext` drops every root and
// collects, so the compactor sees no surviving owner at all and the region
// drains -- the state a host that reuses its arena buffer must be left in.
static bool TestCloseContextWithLivePayloads(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  for (size_t i = 0; i < 16; i++) {
    (void)MakeAggregate(context, 2, 32);
  }
  CHECK(context->payload_used > 0);
  FeCloseContext(context);
  // No live bytes left. The extent's own offset is not asserted: it is zero in
  // every shipped build and a poison-mode artefact otherwise, and what a host
  // reusing its buffer needs to know is that nothing in the region is live.
  CHECK(context->payload_used == 0);
  return true;
}

// ---------------------------------------------------------------------------
// Exhaustion
// ---------------------------------------------------------------------------

// A native whose only job is to ask for a block the region cannot hold, so the
// raise happens inside an evaluation and a Lisp `condition-case` is what
// catches it.
static FeObject* NativeOverflowPayload(
    FeContext* context,
    // cppcheck-suppress constParameterCallback
    FeObject* arguments) {
  FeRequireNoArguments(context, arguments);
  return MakeAggregate(context, 0, context->payload_capacity + 1);
}

static bool ExpectCaught(FeContext* context,
                         const char* source,
                         const char* expected) {
  char rendered[128];
  FeObject* const value =
      FeEvaluateString(context, "payload.fe", source, strlen(source));
  (void)FeToString(context, value, rendered, sizeof(rendered));
  CHECK(strcmp(rendered, expected) == 0);
  return true;
}

// `(payload-exhaustion)` is a condition in its own right: catchable by its own
// name, catchable by `error` because that is its parent, and readable as an
// object. It is release code that no release program can reach -- nothing owns
// a payload before Phase 25 -- which is exactly why it is proved here and not
// by a script.
static bool TestPayloadExhaustionIsCatchable(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    (void)fprintf(stderr, "payload_tests: escaped: %s\n", host.message);
    return false;
  }
  FeDefineNative(context, "overflow-payload", NativeOverflowPayload);
  CHECK(ExpectCaught(context, "(condition-case e (overflow-payload) (t e))",
                     "(payload-exhaustion)"));
  CHECK(ExpectCaught(context,
                     "(condition-case nil (overflow-payload) "
                     "(payload-exhaustion (quote caught)))",
                     "caught"));
  CHECK(ExpectCaught(context,
                     "(condition-case nil (overflow-payload) "
                     "(error (quote caught)))",
                     "caught"));
  // A handler naming an unrelated condition does not match, so the two above
  // are a real hierarchy answer rather than a catch-everything.
  CHECK(ExpectCaught(context,
                     "(condition-case nil (condition-case nil "
                     "(overflow-payload) (arith-error (quote wrong))) "
                     "(t (quote outer)))",
                     "outer"));
  CHECK(context->payload_allocation_failures == 4);
  CHECK(context->payload_used == 0);
  FeCloseContext(context);
  return true;
}

// The degradation. Naming `payload-exhaustion` costs cells, so when the cell
// pool is spent too the raise falls back to the one condition a state with no
// cells can signal. That fallback is the pre-built `(arena-exhaustion)`, whose
// catchability `test_api.c` already pins; what is under test here is that the
// PAYLOAD path takes it.
static bool TestPayloadExhaustionDegrades(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  FeObject* const owner = MakeAggregate(context, 0, 8);
  // Fill the cell pool with a list nothing can reclaim, so the collection
  // inside the raise path frees nothing and `ArenaCanAllocate` answers false.
  // The owner sits below this checkpoint and therefore stays rooted too.
  const size_t base = FeSaveGC(context);
  FeObject* list = FeNil(context);
  while (FeGetArenaStats(context).free_slots > 0) {
    FeRestoreGC(context, base);
    FePushGC(context, list);
    list = FeCons(context, FeNil(context), list);
  }
  CHECK(FeGetArenaStats(context).free_slots == 0);

  if (setjmp(host.jump) == 0) {
    PublishPayload(context, owner, 0, context->payload_capacity);
    CHECK(false);
  }
  CHECK(host.raised);
  CHECK(strcmp(host.message, "payload region exhausted") == 0);
  CHECK(ConditionIs(context, "(arena-exhaustion)"));
  // A refused publish changes nothing: the owner still names the block it had.
  CHECK(AggregateHandle(owner) == ExpectedHandle(0));
  FeCloseContext(context);
  return true;
}

// Failure injection at every allocation a construction makes. `MakeAggregate`
// makes exactly two -- the cell, then the block -- and each one is driven to
// fail here with the other satisfied, because a constructor that half-succeeds
// is the way an owner ends up naming storage it does not have.
static bool TestFailureInjectionAtEveryAllocation(void) {
  // Site 1: THE CELL. `MakeObject` runs first, so an arena with nothing left
  // raises before a single payload byte is taken.
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  const size_t base = FeSaveGC(context);
  FeObject* list = FeNil(context);
  while (FeGetArenaStats(context).free_slots > 0) {
    FeRestoreGC(context, base);
    FePushGC(context, list);
    list = FeCons(context, FeNil(context), list);
  }
  if (setjmp(host.jump) == 0) {
    (void)MakeAggregate(context, 1, 8);
    CHECK(false);
  }
  CHECK(host.raised);
  CHECK(strcmp(host.message, "out of memory") == 0);
  CHECK(ConditionIs(context, "(arena-exhaustion)"));
  CHECK(context->payload_used == 0);
  FeCloseContext(context);

  // Site 2: THE BLOCK. The cell was taken and the retype ran, so the raise
  // happens with an owner that names no block at all. Nothing accumulates
  // from that: the same failure repeated costs the arena nothing the second
  // time, which is what says the half-built owner is reclaimed. (The FIRST
  // failure does cost a few cells for good -- the condition object it builds
  // is a root for as long as it is the completion in flight -- so the
  // comparison is between two failures, not against a pristine arena.)
  context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  const size_t checkpoint = FeSaveGC(context);
  size_t free_after[2] = {0, 0};
  for (size_t attempt = 0; attempt < 2; attempt++) {
    if (setjmp(host.jump) == 0) {
      (void)MakeAggregate(context, 0, context->payload_capacity);
      CHECK(false);
    }
    CHECK(host.raised);
    CHECK(ConditionIs(context, "(payload-exhaustion)"));
    CHECK(context->payload_used == 0);
    FeRestoreGC(context, checkpoint);
    FeCollectGarbage(context);
    free_after[attempt] = FeGetArenaStats(context).free_slots;
  }
  CHECK(free_after[0] == free_after[1]);
  FeCloseContext(context);
  return true;
}

#if !FE_GC_STRESS

// The C-stack depth proof, reproducing the method the Phase 22 ADR records for
// the spike's fourth commit: a chain of 100 000 aggregates, each holding the
// next in its payload, with a `ptr` object at the bottom whose mark callback
// is the one place a test can stand INSIDE the mark phase and read the
// collector's own C frame. Flat means the payload arm reuses the pointer
// reversal rather than recursing; a recursive walk would miss this by
// megabytes at n = 100 000, not by bytes of noise.
//
// Not built under `FE_GC_STRESS`: that knob collects at every allocation, and
// this case allocates 100 000 times into a 64 MiB arena, which is 10^10 sweep
// steps for a property that does not depend on how often the collector runs.
static size_t depth_probe_calls;
static uintptr_t depth_probe_deepest;

static FeObject* DepthMarkProbe(FeContext* context,
                                // cppcheck-suppress constParameterCallback
                                FeObject* obj) {
  (void)context;
  if (FeGetType(obj) == FeTPtr) {
    depth_probe_calls++;
    // `__builtin_frame_address(0)`, never a nonzero depth, and never the
    // address of a local: a sanitizer may place an address-taken local on its
    // fake stack, which would make this a heap measurement instead.
    void* const frame = __builtin_frame_address(0);
    const uintptr_t address = (uintptr_t)frame;
    if (depth_probe_deepest == 0 || address < depth_probe_deepest) {
      depth_probe_deepest = address;
    }
  }
  return obj;
}

static bool MeasurePayloadMarkDepth(unsigned char* storage,
                                    size_t depth,
                                    uintptr_t* address) {
  FeContext* const context =
      OpenContextWithPayload(storage, DepthArenaSize, PayloadArenaPercent);
  CHECK(context != nullptr);
  host.raised = false;
  FeSetErrorFn(context, HandleError);
  FeSetMarkFn(context, DepthMarkProbe);
  if (setjmp(host.jump) != 0) {
    (void)fprintf(stderr, "payload_tests: depth %zu raised: %s\n", depth,
                  host.message);
    return false;
  }
  const size_t base = FeSaveGC(context);
  FeObject* node = FeMakePtr(context, FeTPtr, &depth_probe_calls);
  for (size_t i = 0; i < depth; i++) {
    // One root at a time, whatever the depth: the chain hangs off `node`, so
    // a checkpoint restored per level keeps the GC stack at a constant two.
    FeRestoreGC(context, base);
    FePushGC(context, node);
    FeObject* const parent = MakeAggregate(context, 1, 0);
    SetAggregateChild(context, parent, 0, node);
    node = parent;
  }
  FeRestoreGC(context, base);
  FePushGC(context, node);

  depth_probe_calls = 0;
  depth_probe_deepest = 0;
  FeCollectGarbage(context);
  // The callback fired, so the walk really reached the bottom of the chain...
  CHECK(depth_probe_calls > 0);
  // ...and the chain is intact all the way down afterwards, so the pointer
  // reversal put every reversed link back.
  FeObject* walk = node;
  for (size_t i = 0; i < depth; i++) {
    walk = AggregateChild(context, walk, 0);
  }
  CHECK(FeGetType(walk) == FeTPtr);
  *address = depth_probe_deepest;
  FeCloseContext(context);
  return true;
}

static bool TestMarkDepthIsFlat(void) {
  unsigned char* const storage = malloc(DepthArenaSize);
  CHECK(storage != nullptr);
  bool ok = true;
  uintptr_t baseline = 0;
  ok = ok && MeasurePayloadMarkDepth(storage, 0, &baseline);
  static const size_t depths[] = {10, 1000, 100000};
  for (size_t i = 0; ok && i < sizeof(depths) / sizeof(depths[0]); i++) {
    uintptr_t probed = 0;
    ok = ok && MeasurePayloadMarkDepth(storage, depths[i], &probed);
    const uintptr_t delta =
        probed > baseline ? probed - baseline : baseline - probed;
    (void)printf("payload_tests: mark probe n=%6zu baseline=%#" PRIxPTR
                 " probe=%#" PRIxPTR " delta=%" PRIuPTR " bytes\n",
                 depths[i], baseline, probed, delta);
    // The same measured tolerance the pair chain's probe asserts.
    ok = ok && delta < 2048;
  }
  free(storage);
  CHECK(ok);
  return true;
}

#else

static bool TestMarkDepthIsFlat(void) {
  return true;
}

#endif  // !FE_GC_STRESS

// ---------------------------------------------------------------------------
// The publish protocol's poison mode
// ---------------------------------------------------------------------------

#if FE_DEBUG_PAYLOAD_MOVE

// Clause 2 has teeth: an address held across ONE allocation reads poison
// afterwards, and re-deriving it from the owner reads the data. Runs only in
// the lane that arms the knob (`.ci/ci-04-clang-asan-ubsan.sh`); an ordinary
// build never slides the extent, so there is nothing here to assert.
static bool TestPoisonedPointerFailsLoudly(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  FeObject* const owner = MakeAggregate(context, 0, 7);
  // Written through the accessor, immediately before use, as clause 1 says.
  unsigned char* const held = AggregateBytes(context, owner);
  memcpy(held, "abcdefg", 7);

  // ...and then held across an allocation, which clause 1 says not to do.
  (void)FeCons(context, FeNil(context), FeNil(context));
  // The held address no longer names the data. It names the bytes that used
  // to sit one unit behind it, which is the whole failure mode: a stale
  // payload pointer reads SOMETHING, and that something is not the payload.
  CHECK(memcmp(held, "abcdefg", 7) != 0);
  // The unit the extent vacated is pattern-filled, so a pointer to the front
  // of the extent reads `FePayloadPoisonByte` rather than plausible data.
  const unsigned char* const vacated =
      context->payload_base + context->payload_start - FePayloadAlignment;
  for (size_t i = 0; i < FePayloadAlignment; i++) {
    CHECK(vacated[i] == FePayloadPoisonByte);
  }
  // The extent moved by exactly one unit and the bytes came with it:
  // re-deriving from the owner is the fix, and the only one.
  const unsigned char* const rederived = AggregateBytes(context, owner);
  CHECK(rederived == held + FePayloadAlignment);
  CHECK(memcmp(rederived, "abcdefg", 7) == 0);
  FeCloseContext(context);
  return true;
}

#else

static bool TestPoisonedPointerFailsLoudly(void) {
  return true;
}

#endif  // FE_DEBUG_PAYLOAD_MOVE

int main(void) {
  const bool ok =
      TestRegionCarve() && TestBumpAllocationOrder() &&
      TestExactFitAndOneByteOver() && TestMarkReachesPayloadChildren() &&
      TestReplacedBlockIsReclaimed() &&
      TestCompactionAddressesAreDeterministic() && TestSlidingOverlap() &&
      TestLiveDeadMixture() && TestCloseContextWithLivePayloads() &&
      TestPayloadExhaustionIsCatchable() && TestPayloadExhaustionDegrades() &&
      TestFailureInjectionAtEveryAllocation() && TestMarkDepthIsFlat() &&
      TestPoisonedPointerFailsLoudly();
  (void)printf("payload_tests: FE_GC_STRESS=%d FE_DEBUG_PAYLOAD_MOVE=%d: %s\n",
               FE_GC_STRESS, FE_DEBUG_PAYLOAD_MOVE, ok ? "ok" : "FAILED");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
