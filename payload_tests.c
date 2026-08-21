// Copyright 2026 the kg authors
// SPDX-License-Identifier: MIT
//
// The payload substrate's own regression test (Phase 23.1 of kg's
// doc/plans/2026-08-18-elisp-data-model.md; the Phase 22 ADR's Design B).
//
// Its own binary rather than a case inside `test_api`, for the same reason
// `gc_stress` is: it needs a build the ordinary one is not.
// `FE_PAYLOAD_TEST_OBJECT` gives fe an owner whose child count and byte tail
// a test can choose independently, which neither release owner is: a vector
// is all children and a string is all bytes.
//
// Since Phase 25 a freshly opened context ALREADY holds blocks -- one per core
// symbol name, one per seeded condition message -- so every offset a case
// below asserts is measured from `payload_used` at open rather than from
// zero. That floor is what `FeMinimumArenaSize` funds and what makes a
// region-less context impossible.
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
#include "fe_perf.h"

static_assert(FE_API_VERSION == 15);
static_assert(FE_LANGUAGE_VERSION == 19);

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
  // kg's own default Lisp arena, so the partition the host surface hands back
  // is asserted at the size a real host opens rather than only at this file's
  // convenient one.
  HostArenaSize = 1024 * 1024,
  // The vector groups' own arena, malloc'd: an 8192-element vector is 65 568
  // payload bytes on its own, which is more than a 25% carve of the 256 KiB
  // static arena above leaves, and the cost table wants room for the
  // transient garbage a construction makes as well as for the vector.
  VectorArenaSize = 8u << 20,
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

// A zero percentage is the FLOOR and not "no region": the core names' blocks
// are funded out of the minimum arena, so the region at 0% holds exactly them
// and has no spare byte at all. A percentage carve adds its share on top and
// takes those bytes out of the cells and out of nothing else, so the two pools
// stay priced against each other and the frame region -- Phase 21's measured
// scarcity -- is untouched by the split.
static bool TestRegionCarve(void) {
  FeContext* plain = OpenPayloadContext(0);
  CHECK(plain != nullptr);
  const FeArenaStats plain_stats = FeGetArenaStats(plain);
  const size_t plain_cells = plain_stats.total_slots;
  const size_t plain_frames = plain_stats.frame_capacity;
  const size_t floor_bytes = plain->payload_capacity;
  CHECK(plain->payload_base != nullptr);
  CHECK(floor_bytes > 0);
  // Exact, the way the exact-fit arena in test_api.c is exact for cells: what
  // the floor funds is what opening spent, to the byte.
  CHECK(plain->payload_used == floor_bytes);
  FeCloseContext(plain);

  FeContext* carved = OpenPayloadContext(PayloadArenaPercent);
  CHECK(carved != nullptr);
  const FeArenaStats carved_stats = FeGetArenaStats(carved);
  CHECK(carved->payload_capacity > floor_bytes);
  CHECK(carved->payload_capacity % FePayloadAlignment == 0);
  CHECK(carved_stats.frame_capacity == plain_frames);
  CHECK(carved->payload_used == floor_bytes);
  // The cells the region cost are the region's DISCRETIONARY bytes, to within
  // the one cell the floor division drops; the floor itself was never the
  // cells' to lose.
  const size_t share = carved->payload_capacity - floor_bytes;
  const size_t lost_cells = plain_cells - carved_stats.total_slots;
  CHECK(lost_cells >= share / sizeof(FeObject));
  CHECK(lost_cells <= share / sizeof(FeObject) + 1);
  (void)printf(
      "payload_tests: carve %d%%: %zu cells beside %zu payload bytes, against "
      "%zu cells at the %zu-byte floor; frames %zu either way\n",
      (int)PayloadArenaPercent, carved_stats.total_slots,
      carved->payload_capacity, plain_cells, floor_bytes, plain_frames);
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
  const size_t floor_bytes = context->payload_used;
  FeObject* const first = MakeAggregate(context, 0, 1);
  FeObject* const second = MakeAggregate(context, 0, 7);
  FeObject* const third = MakeAggregate(context, 0, 8);
  FeObject* const fourth = MakeAggregate(context, 0, 9);
  const size_t block = sizeof(FePayloadBlock);
  CHECK(AggregateHandle(first) == ExpectedHandle(floor_bytes));
  CHECK(AggregateHandle(second) == ExpectedHandle(floor_bytes + block + 8));
  CHECK(AggregateHandle(third) ==
        ExpectedHandle(floor_bytes + 2 * (block + 8)));
  CHECK(AggregateHandle(fourth) ==
        ExpectedHandle(floor_bytes + 3 * (block + 8)));
  // Every payload starts on an alignment unit, which is what lets one block
  // hold `FeObject*` children without a second alignment rule.
  const unsigned char* const tail = AggregateBytes(context, fourth);
  CHECK((uintptr_t)tail % FePayloadAlignment == 0);
  CHECK(context->payload_used == floor_bytes + 3 * (block + 8) + block + 16);
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
  const size_t floor_bytes = context->payload_used;
  const size_t room =
      context->payload_capacity - floor_bytes - sizeof(FePayloadBlock);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  FeObject* const exact = MakeAggregate(context, 0, room);
  CHECK(AggregateHandle(exact) == ExpectedHandle(floor_bytes));
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
  // Refused, not half-done: nothing beyond the floor was taken from the
  // region. The condition object the raise built is cells, not payload.
  CHECK(context->payload_used == floor_bytes);
  FeCloseContext(context);
  return true;
}

// ---------------------------------------------------------------------------
// The host's surface: the options-bearing open, and the payload stats
// ---------------------------------------------------------------------------

// The three numbers a partition IS, read the way a host reads them. Frame
// capacity is here in every comparison below because the Phase 22 ADR's "no
// unpriced split" rule is about this pool: a carve that funded itself out of
// the frames would leave both other numbers looking healthy.
typedef struct Partition {
  size_t cells;
  size_t frames;
  size_t payload_bytes;
} Partition;

static Partition PartitionOf(const FeContext* context) {
  const FeArenaStats stats = FeGetArenaStats(context);
  return (Partition){.cells = stats.total_slots,
                     .frames = stats.frame_capacity,
                     .payload_bytes = stats.payload_capacity_bytes};
}

static bool PartitionsAreEqual(Partition a, Partition b) {
  return a.cells == b.cells && a.frames == b.frames &&
         a.payload_bytes == b.payload_bytes;
}

// `FeOpenContextWithOptions` and every value its one knob takes, at kg's own
// arena size so the numbers are the ones the kg side of this phase is read
// against.
//
// ONE contract since Phase 25, where there were two. Every entry point carves
// a region, because a symbol's name is a string and a string's bytes live in
// one: `FeOpenContext`, a null options record and a zero-initialized one are
// all the same request -- Fe's own split, `FeDefaultPayloadPercent` -- and the
// only thing a host can say instead is a different percentage of the surplus.
static bool TestOpenOptionsPartition(void) {
  unsigned char* const storage = malloc(HostArenaSize);
  CHECK(storage != nullptr);

  FeContext* plain = FeOpenContext(storage, HostArenaSize);
  CHECK(plain != nullptr);
  const Partition carved = PartitionOf(plain);
  const size_t floor_bytes = plain->payload_used;
  CHECK(carved.payload_bytes > floor_bytes);
  FeCloseContext(plain);

  // A null record and a zero-initialized one are the same request, and it is
  // what `FeOpenContext` asks for too.
  FeContext* implied =
      FeOpenContextWithOptions(storage, HostArenaSize, nullptr);
  CHECK(implied != nullptr);
  CHECK(PartitionsAreEqual(PartitionOf(implied), carved));
  FeCloseContext(implied);

  const FeOpenOptions defaults = {0};
  FeContext* zeroed =
      FeOpenContextWithOptions(storage, HostArenaSize, &defaults);
  CHECK(zeroed != nullptr);
  CHECK(PartitionsAreEqual(PartitionOf(zeroed), carved));
  FeCloseContext(zeroed);

  const FeOpenOptions spelled = {.payload_percent = FeDefaultPayloadPercent};
  FeContext* explicit_default =
      FeOpenContextWithOptions(storage, HostArenaSize, &spelled);
  CHECK(explicit_default != nullptr);
  CHECK(PartitionsAreEqual(PartitionOf(explicit_default), carved));
  FeCloseContext(explicit_default);

  // The measured split at kg's arena, asserted exactly rather than as a
  // relationship: a change to either number is a change to the budget every
  // host sizing decision in this program was made against.
  //
  // Re-measured at Phase 26, which moves both by what the symbol index costs
  // an open. The arena here is a fixed 1 MiB while `FeMinimumArenaSize` grew
  // by 2096 bytes -- one 2080-byte index block and the one cell of its owner
  // -- so the surplus the three pools split shrank by that much: cells 42059
  // -> 41971, and payload bytes 225928 -> 227536, the region's floor rising
  // by the whole block while its discretionary share falls with the surplus.
  //
  // Re-measured again at the frontier demand phase, which adds the
  // `search-failed` condition row: `FeMinimumArenaSize` grows by 240 bytes
  // (nine cells at 16, plus 96 region bytes for the name's and the message's
  // payload blocks), so this fixed arena moves the same way for the same
  // reason -- cells 41971 -> 41970, payload bytes 227536 -> 227576, the
  // region's floor rising by both blocks while the surplus it shares falls.
  // Phase M2 (master plan 2026-08-21, section 6) adds the `invalid-regexp'
  // condition row, which moves `FeMinimumArenaSize` by one more cell and 24
  // region bytes against this fixed arena -- carved.cells 41997 -> 41998,
  // payload_bytes 227304 -> 227328 -- the region's floor rising while the
  // surplus it shares falls.
  CHECK(carved.cells == 41998);
  CHECK(carved.payload_bytes == 227328);

  // A smaller share moves the same bytes back the other way, and the frame
  // region -- funded before the split -- notices neither.
  const FeOpenOptions tenth = {.payload_percent = 10};
  FeContext* small_context =
      FeOpenContextWithOptions(storage, HostArenaSize, &tenth);
  CHECK(small_context != nullptr);
  const Partition small = PartitionOf(small_context);
  FeCloseContext(small_context);
  CHECK(small.payload_bytes < carved.payload_bytes);
  CHECK(small.cells > carved.cells);
  CHECK(small.frames == carved.frames);
  // ...but never below the floor, which is not this percentage's to withhold.
  CHECK(small.payload_bytes > floor_bytes);
  // The region cost cells and nothing else: what left the cell pool is the
  // share the two partitions differ by, to within the one cell the floor
  // division drops.
  const size_t lost = small.cells - carved.cells;
  const size_t share = carved.payload_bytes - small.payload_bytes;
  CHECK(lost >= share / sizeof(FeObject));
  CHECK(lost <= share / sizeof(FeObject) + 1);

  // Out of range is REFUSED, never clamped: a host that asks for a partition
  // that does not exist gets a null context and finds out, rather than a
  // context divided by a number it did not choose.
  const FeOpenOptions over = {.payload_percent = 101};
  CHECK(FeOpenContextWithOptions(storage, HostArenaSize, &over) == nullptr);
  const FeOpenOptions under = {.payload_percent = -1};
  CHECK(FeOpenContextWithOptions(storage, HostArenaSize, &under) == nullptr);
  // The edge inside the range opens, and is honest about what it leaves: a
  // whole-arena carve is a context with the core objects and nothing spare,
  // which is a bad idea rather than an invalid one.
  const FeOpenOptions whole = {.payload_percent = 100};
  FeContext* full = FeOpenContextWithOptions(storage, HostArenaSize, &whole);
  CHECK(full != nullptr);
  const Partition all_payload = PartitionOf(full);
  CHECK(all_payload.payload_bytes > carved.payload_bytes);
  CHECK(all_payload.cells < carved.cells);
  CHECK(all_payload.frames == carved.frames);
  FeCloseContext(full);

  // The arena's own requirements are unchanged by the new entry point.
  CHECK(FeOpenContextWithOptions(nullptr, HostArenaSize, nullptr) == nullptr);
  CHECK(FeOpenContextWithOptions(storage, FeMinimumArenaSize() - 1, nullptr) ==
        nullptr);

  (void)printf(
      "payload_tests: %d KiB arena: %zu cells beside %zu payload bytes at the "
      "default %d%%, of which %zu are the core names' floor; frames %zu\n",
      (int)(HostArenaSize / 1024), carved.cells, carved.payload_bytes,
      (int)FeDefaultPayloadPercent, floor_bytes, carved.frames);
  free(storage);
  return true;
}

// The five payload fields of `FeArenaStats`, against allocations whose sizes
// are known exactly. This is the surface kg's `arena-stats` command,
// `kgbatch -g` and the prelude census read, so it is asserted as numbers and
// not as bounds.
static bool TestArenaStatsReportsPayload(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  FeArenaStats stats = FeGetArenaStats(context);
  const size_t floor_bytes = context->payload_used;
  CHECK(stats.payload_capacity_bytes == context->payload_capacity);
  CHECK(stats.payload_capacity_bytes > 0);
  // The core names, and nothing this case allocated yet.
  CHECK(stats.payload_live_bytes == floor_bytes);
  CHECK(stats.payload_peak_bytes == floor_bytes);
  CHECK(stats.payload_compaction_count == 0);
  CHECK(stats.payload_allocation_failures == 0);

  // Four blocks of one child and sixteen bytes: the block header, the child
  // word and the bytes, per block, and nothing else.
  enum { Blocks = 4 };
  const size_t each = sizeof(FePayloadBlock) + sizeof(FeObject*) + 16;
  const size_t base = FeSaveGC(context);
  FeObject* survivor = nullptr;
  for (size_t i = 0; i < Blocks; i++) {
    FeObject* const owner = MakeAggregate(context, 1, 16);
    survivor = i == 0 ? owner : survivor;
  }
  stats = FeGetArenaStats(context);
  CHECK(stats.payload_live_bytes == floor_bytes + Blocks * each);
  CHECK(stats.payload_peak_bytes == floor_bytes + Blocks * each);
  CHECK(stats.payload_compaction_count == 0);

  // Three of them die. The high-water mark does not follow the live bytes
  // back down -- that is what makes it the margin figure -- and the
  // compaction that reclaimed them is counted.
  FeRestoreGC(context, base);
  FePushGC(context, survivor);
  FeCollectGarbage(context);
  stats = FeGetArenaStats(context);
  CHECK(stats.payload_live_bytes == floor_bytes + each);
  CHECK(stats.payload_peak_bytes == floor_bytes + Blocks * each);
  CHECK(stats.payload_compaction_count == 1);
  CHECK(stats.payload_allocation_failures == 0);

  // A request the region cannot hold is counted where a host can see it, and
  // the capacity it was measured against has not moved.
  const size_t capacity = stats.payload_capacity_bytes;
  if (setjmp(host.jump) == 0) {
    (void)MakeAggregate(context, 0, capacity + 1);
    CHECK(false);
  }
  CHECK(host.raised);
  stats = FeGetArenaStats(context);
  CHECK(stats.payload_allocation_failures == 1);
  CHECK(stats.payload_capacity_bytes == capacity);
  FeCloseContext(context);

  // At the floor the same five read the floor and nothing else: a full
  // region that has never had to compact and has never refused anything,
  // which is the reading kg's prelude census pins.
  FeContext* floor_only = OpenPayloadContext(0);
  CHECK(floor_only != nullptr);
  stats = FeGetArenaStats(floor_only);
  CHECK(stats.payload_capacity_bytes == floor_only->payload_used);
  CHECK(stats.payload_live_bytes == stats.payload_capacity_bytes);
  CHECK(stats.payload_peak_bytes == stats.payload_capacity_bytes);
  CHECK(stats.payload_compaction_count == 0);
  CHECK(stats.payload_allocation_failures == 0);
  FeCloseContext(floor_only);
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
  const size_t floor_bytes = context->payload_used;
  FeObject* const owner = MakeAggregate(context, 0, 64);
  const size_t one_block = context->payload_used - floor_bytes;
  memcpy(AggregateBytes(context, owner), "first", 6);
  PublishPayload(context, owner, 0, 64);
  CHECK(context->payload_used == floor_bytes + 2 * one_block);
  // The replacement starts as a copy of what it replaces, which is what makes
  // a growing string a growing string rather than a new empty one.
  CHECK(strcmp((const char*)AggregateBytes(context, owner), "first") == 0);
  memcpy(AggregateBytes(context, owner), "second", 7);

  FeRestoreGC(context, base);
  FePushGC(context, owner);
  FeCollectGarbage(context);
  // One block's worth left, and the owner names the survivor -- which now sits
  // at the region's base, because compaction preserves order and the block
  // before it died.
  CHECK(context->payload_used == floor_bytes + one_block);
  CHECK(AggregateHandle(owner) == ExpectedHandle(floor_bytes));
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
  size_t offset = context->payload_used;
  const size_t floor_bytes = offset;
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

  CHECK(context->payload_used ==
        floor_bytes + 3 * block + sizes[0] + sizes[2] + sizes[4]);
  CHECK(AggregateHandle(blocks[0]) == ExpectedHandle(floor_bytes));
  CHECK(AggregateHandle(blocks[2]) ==
        ExpectedHandle(floor_bytes + block + sizes[0]));
  CHECK(AggregateHandle(blocks[4]) ==
        ExpectedHandle(floor_bytes + 2 * block + sizes[0] + sizes[2]));
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
  const size_t floor_bytes = context->payload_used;
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

  CHECK(AggregateHandle(survivor) == ExpectedHandle(floor_bytes));
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
  const size_t floor_bytes = context->payload_used;
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
  CHECK(context->payload_used == floor_bytes + 3 * each);
  for (size_t i = 0; i < 3; i++) {
    CHECK(AggregateHandle(survivors[i]) ==
          ExpectedHandle(floor_bytes + i * each));
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
// object.
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
  // A refused request takes NOTHING: asked a fifth time, with a source whose
  // every symbol is already interned so that reading it publishes no block of
  // its own, the region is exactly where it was.
  const size_t before = context->payload_used;
  CHECK(ExpectCaught(context, "(condition-case e (overflow-payload) (t e))",
                     "(payload-exhaustion)"));
  CHECK(context->payload_allocation_failures == 5);
  CHECK(context->payload_used == before);
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
  const size_t floor_bytes = context->payload_used;
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
  CHECK(AggregateHandle(owner) == ExpectedHandle(floor_bytes));
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
  const size_t floor_bytes = context->payload_used;
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
  CHECK(context->payload_used == floor_bytes);
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
  const size_t block_floor = context->payload_used;
  size_t free_after[2] = {0, 0};
  for (size_t attempt = 0; attempt < 2; attempt++) {
    if (setjmp(host.jump) == 0) {
      (void)MakeAggregate(context, 0, context->payload_capacity);
      CHECK(false);
    }
    CHECK(host.raised);
    CHECK(ConditionIs(context, "(payload-exhaustion)"));
    CHECK(context->payload_used == block_floor);
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
// Phase 24: vectors, on the substrate above
//
// The Lisp-visible contract is `test_api.c`'s `TestVectors` and kg's frozen
// oracle corpus. What is here is what only this build can say: what a vector
// costs the two pools, that the compactor moves one without changing what it
// holds, that a construction the region cannot satisfy publishes NOTHING, and
// -- in the counting build -- that random access is flat in the vector's size.
// ---------------------------------------------------------------------------

// One `FeContext` on a malloc'd arena big enough for the sizes this group
// uses, with the ordinary carve. The caller owns the storage and frees it.
static FeContext* OpenVectorContext(unsigned char* storage, size_t size) {
  FeContext* const context =
      OpenContextWithPayload(storage, size, PayloadArenaPercent);
  if (context == nullptr) {
    return nullptr;
  }
  host.raised = false;
  host.message[0] = '\0';
  FeSetErrorFn(context, HandleError);
  return context;
}

// What a vector of `length` elements takes out of the payload region: its
// block header and one word per element, and nothing else. There is no length
// field and no capacity slack, which is why this is an equality and not a
// bound.
static size_t VectorBlockBytes(size_t length) {
  return sizeof(FePayloadBlock) + length * sizeof(FeObject*);
}

// The same for a STRING of LENGTH bytes (Phase 25), whose block is all bytes
// where a vector's is all children.
static size_t StringBlockBytes(size_t length) {
  return sizeof(FePayloadBlock) + (length + FePayloadAlignment - 1) /
                                      FePayloadAlignment * FePayloadAlignment;
}

// Vectors and byte-bearing aggregates in one region, compacted together. This
// is 23.1's determinism case with a real Lisp type among the blocks: the
// survivors slide down in ALLOCATION ORDER, so their handles are the offsets
// the sizes before them imply, and -- the half only a vector can assert --
// every element a moved vector holds is still the object it held, reached
// through a handle the compactor rewrote and a header it did not.
static bool TestVectorCompactionIsDeterministic(void) {
  unsigned char* const storage = malloc(VectorArenaSize);
  CHECK(storage != nullptr);
  FeContext* const context = OpenVectorContext(storage, VectorArenaSize);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    free(storage);
    return false;
  }
  const size_t base = FeSaveGC(context);
  const size_t floor_bytes = context->payload_used;
  // Interleaved on purpose: a compactor that handled one owner type and not
  // the other would still pass a test whose region held only one of them.
  FeObject* const first = FeMakeVector(context, 2);
  FeObject* const spacer = MakeAggregate(context, 0, 24);
  FeObject* const second = FeMakeVector(context, 5);
  FeObject* const doomed = MakeAggregate(context, 1, 8);  // one child + 8 bytes
  FeObject* const third = FeMakeVector(context, 1);
  memset(AggregateBytes(context, spacer), 'z', 24);
  FeVectorSet(context, first, 0, FeMakeSymbol(context, "kept"));
  FeVectorSet(context, first, 1, FeMakeInteger(context, 7));
  FeVectorSet(context, second, 4, FeMakeString(context, "tail"));
  FeVectorSet(context, third, 0, second);

  const size_t vector2 = VectorBlockBytes(2);
  const size_t aggregate24 = sizeof(FePayloadBlock) + 24;
  const size_t vector5 = VectorBlockBytes(5);
  const size_t aggregate1 = sizeof(FePayloadBlock) + 16;
  // The two strings this case interleaves without meaning to: `kept`'s symbol
  // name and the string `tail`, both published after the five owners above
  // and both alive at the end. Phase 25 is why they are blocks at all.
  const size_t kept_name = StringBlockBytes(strlen("kept"));
  const size_t tail_text = StringBlockBytes(strlen("tail"));
  CHECK(PAYLOAD(first) == ExpectedHandle(floor_bytes));
  CHECK(AggregateHandle(spacer) == ExpectedHandle(floor_bytes + vector2));
  CHECK(PAYLOAD(second) == ExpectedHandle(floor_bytes + vector2 + aggregate24));
  CHECK(AggregateHandle(doomed) ==
        ExpectedHandle(floor_bytes + vector2 + aggregate24 + vector5));
  CHECK(PAYLOAD(third) == ExpectedHandle(floor_bytes + vector2 + aggregate24 +
                                         vector5 + aggregate1));

  // Drop `spacer` and `doomed`; the three vectors survive, and `second`
  // survives only through `third`'s element, which is the payload arm
  // reaching a payload owner through another one's block.
  FeRestoreGC(context, base);
  FePushGC(context, first);
  FePushGC(context, third);
  FeCollectGarbage(context);

  CHECK(PAYLOAD(first) == ExpectedHandle(floor_bytes));
  CHECK(PAYLOAD(second) == ExpectedHandle(floor_bytes + vector2));
  CHECK(PAYLOAD(third) == ExpectedHandle(floor_bytes + vector2 + vector5));
  CHECK(context->payload_used == floor_bytes + vector2 + vector5 +
                                     VectorBlockBytes(1) + kept_name +
                                     tail_text);
  // The contents came with the move.
  CHECK(FeVectorRef(context, first, 0) == FeMakeSymbol(context, "kept"));
  CHECK(FeToInteger(context, FeVectorRef(context, first, 1)) == 7);
  CHECK(FeVectorRef(context, third, 0) == second);
  CHECK(FeVectorLength(context, second) == 5);
  char rendered[32];
  (void)FeToString(context, FeVectorRef(context, second, 4), rendered,
                   sizeof(rendered));
  CHECK(strcmp(rendered, "tail") == 0);
  FeCloseContext(context);
  free(storage);
  return true;
}

// A construction the region cannot satisfy raises and publishes NOTHING: the
// cell it had already retyped names no block, every block that was there is
// untouched, and a collection afterwards walks the half-built owner without
// tripping over it. That last part is the one a half-published vector would
// fail -- the collector reaches a vector through `PayloadSlot`, and a slot
// holding a handle to a block that was never written is how a payload
// substrate corrupts itself quietly.
static bool TestVectorExhaustionLeavesNothingBehind(void) {
  unsigned char* const storage = malloc(HostArenaSize);
  CHECK(storage != nullptr);
  FeContext* const context = OpenVectorContext(storage, HostArenaSize);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    free(storage);
    return false;
  }
  const size_t base = FeSaveGC(context);
  const size_t floor_bytes = context->payload_used;
  FeObject* const survivor = FeMakeVector(context, 3);
  FeVectorSet(context, survivor, 1, FeMakeSymbol(context, "alive"));
  // The vector, and the name block interning `alive` published beside it.
  const size_t live = VectorBlockBytes(3) + StringBlockBytes(strlen("alive"));
  const FeArenaStats before = FeGetArenaStats(context);
  CHECK(before.payload_live_bytes == floor_bytes + live);
  CHECK(before.payload_allocation_failures == 0);

  // One element more than the whole region could hold even if it were empty.
  const size_t impossible = context->payload_capacity / sizeof(FeObject*) + 1;
  if (setjmp(host.jump) == 0) {
    (void)FeMakeVector(context, impossible);
    CHECK(false);
  }
  CHECK(host.raised);
  CHECK(ConditionIs(context, "(payload-exhaustion)"));
  const FeArenaStats after = FeGetArenaStats(context);
  CHECK(after.payload_allocation_failures == 1);
  // Nothing was published: the region holds exactly what it held.
  CHECK(after.payload_live_bytes == before.payload_live_bytes);

  // The half-built owner is still on the root stack (`MakeObject` pushed it
  // and the raise did not pop it), so this collection really does mark a
  // vector with no block. It answers zero length, which is the honest answer
  // for a vector with no elements yet, and it does not disturb the survivor.
  FeCollectGarbage(context);
  FeRestoreGC(context, base);
  FePushGC(context, survivor);
  FeCollectGarbage(context);
  CHECK(FeGetArenaStats(context).payload_live_bytes == floor_bytes + live);
  CHECK(FeVectorRef(context, survivor, 1) == FeMakeSymbol(context, "alive"));
  FeCloseContext(context);
  free(storage);
  return true;
}

// What a vector costs, at the four sizes the master plan named, measured in
// both pools and reported rather than merely bounded. Two routes, because
// they have different transient costs: `FeMakeVector` publishes one block and
// allocates one cell, while READING `[1 1 ...]` also builds one cons per
// element that dies immediately -- the "temporary construction high-water"
// the plan asks for, and the number a caller sizing an arena needs.
static bool TestVectorCostTable(void) {
  static const size_t sizes[] = {0, 1, 1000, 8192};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    const size_t n = sizes[i];
    unsigned char* const storage = malloc(VectorArenaSize);
    CHECK(storage != nullptr);
    FeContext* context = OpenVectorContext(storage, VectorArenaSize);
    CHECK(context != nullptr);
    if (setjmp(host.jump) != 0) {
      free(storage);
      return false;
    }
    const FeArenaStats opened = FeGetArenaStats(context);
    const size_t cells_before = opened.total_slots - opened.free_slots;
    // What the core names already hold, so the numbers below are the
    // vector's own and not the interpreter's.
    const size_t floor_bytes = opened.payload_live_bytes;

    // Route 1: the storage layer alone.
    FeObject* const direct = FeMakeVector(context, n);
    const FeArenaStats made = FeGetArenaStats(context);
    const size_t direct_cells =
        (made.total_slots - made.free_slots) - cells_before;
    CHECK(direct_cells == 1);
    CHECK(made.payload_live_bytes == floor_bytes + VectorBlockBytes(n));
    CHECK(made.payload_peak_bytes == floor_bytes + VectorBlockBytes(n));
    CHECK(FeVectorLength(context, direct) == n);
    FeCloseContext(context);

    // Route 2: the reader, on a fresh context so the numbers are this size's
    // and not the previous route's.
    context = OpenVectorContext(storage, VectorArenaSize);
    CHECK(context != nullptr);
    if (setjmp(host.jump) != 0) {
      free(storage);
      return false;
    }
    const FeArenaStats fresh = FeGetArenaStats(context);
    const size_t fresh_cells = fresh.total_slots - fresh.free_slots;
    char* const source = malloc(2 * n + 8);
    CHECK(source != nullptr);
    size_t at = 0;
    source[at++] = '[';
    for (size_t e = 0; e < n; e++) {
      source[at++] = '1';
      source[at++] = ' ';
    }
    source[at++] = ']';
    FeObject* const read = FeEvaluateString(context, "cost.fe", source, at);
    free(source);
    const FeArenaStats after = FeGetArenaStats(context);
    CHECK(FeVectorLength(context, read) == n);
    (void)printf(
        "payload_tests: vector n=%5zu: retained %zu cell(s) + %zu payload "
        "bytes; read literal leaves %zu cells live, peak %zu, payload peak "
        "%zu\n",
        n, direct_cells, VectorBlockBytes(n),
        (after.total_slots - after.free_slots) - fresh_cells,
        after.peak_live_objects - fresh_cells,
        after.payload_peak_bytes - floor_bytes);
    // The payload cost is the block and nothing else, whichever route built
    // it: a literal's transient garbage is CELLS, never region bytes.
    CHECK(after.payload_peak_bytes == floor_bytes + VectorBlockBytes(n));
    FeCloseContext(context);
    free(storage);
  }
  return true;
}

#if FE_PERF_COUNTERS && !FE_GC_STRESS

// THE O(1) GATE (Phase 24.3): a fixed number of random accesses charges the
// same counters at n = 8 and at n = 8192 -- every counter, not only the
// vector ones, because "nothing else happened either" is the claim. A counter
// and not a clock, so a loaded box, a sanitizer lane and valgrind all agree.
//
// The indices are spread across the whole vector by a multiplicative hash, so
// an implementation that walked to the index would charge work proportional
// to the average index and the two sizes would differ by three orders of
// magnitude. They do not differ at all: a vector's element address is its
// block's base plus `index * sizeof(FeObject*)`.
enum { RandomAccessCount = 4096 };

static bool MeasureRandomAccess(FeContext* context,
                                size_t n,
                                unsigned long long* out) {
  const size_t gc = FeSaveGC(context);
  FeObject* const vector = FeMakeVector(context, n);
  FePushGC(context, vector);
  // Built and filled BEFORE the reset: what is being measured is the
  // accesses, not the construction.
  for (size_t i = 0; i < n; i++) {
    FeVectorSet(context, vector, i, FeNil(context));
  }
  FePerfReset();
  for (size_t i = 0; i < RandomAccessCount; i++) {
    const size_t index = (i * 2654435761u) % n;
    (void)FeVectorRef(context, vector, index);
    FeVectorSet(context, vector, index, FeNil(context));
    CHECK(FeVectorLength(context, vector) == n);
  }
  for (int i = 0; i < FePerfCounterCount; i++) {
    out[i] = FePerfRead((FePerfCounter)i);
  }
  FeRestoreGC(context, gc);
  return true;
}

static bool TestRandomAccessIsFlat(void) {
  unsigned char* const storage = malloc(VectorArenaSize);
  CHECK(storage != nullptr);
  FeContext* const context = OpenVectorContext(storage, VectorArenaSize);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    free(storage);
    return false;
  }
  static unsigned long long small[FePerfCounterCount];
  static unsigned long long large[FePerfCounterCount];
  CHECK(MeasureRandomAccess(context, 8, small));
  CHECK(MeasureRandomAccess(context, 8192, large));
  for (int i = 0; i < FePerfCounterCount; i++) {
    if (small[i] != large[i]) {
      (void)fprintf(stderr,
                    "payload_tests: counter %s differs: n=8 gives %llu, "
                    "n=8192 gives %llu\n",
                    fe_perf_counter_name[i], small[i], large[i]);
    }
    CHECK(small[i] == large[i]);
  }
  // ...and the numbers are the ones the workload implies, so a gate that
  // passed because both sides did nothing would fail here.
  CHECK(small[FePerfVectorRef] == RandomAccessCount);
  CHECK(small[FePerfVectorSet] == RandomAccessCount);
  (void)printf(
      "payload_tests: %d random accesses: vector_ref=%llu vector_set=%llu, "
      "every counter identical at n=8 and n=8192\n",
      (int)RandomAccessCount, small[FePerfVectorRef], small[FePerfVectorSet]);
  FeCloseContext(context);
  free(storage);
  return true;
}

#else

static bool TestRandomAccessIsFlat(void) {
  return true;
}

#endif  // FE_PERF_COUNTERS && !FE_GC_STRESS

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
  // The extent moved and the bytes came with it: re-deriving from the owner
  // is the fix, and the only one. WHICH WAY it moved is not asserted, and
  // must not be: an allocation that collects first -- every one of them in
  // the `FE_GC_STRESS` build -- returns the extent to the region's base
  // before the poison step slides it forward again, so the net motion is
  // backward there and forward here. What the protocol says is that the
  // address is invalid, not which direction it went.
  const unsigned char* const rederived = AggregateBytes(context, owner);
  CHECK(rederived != held);
  CHECK(memcmp(rederived, "abcdefg", 7) == 0);
  FeCloseContext(context);
  return true;
}

#else

static bool TestPoisonedPointerFailsLoudly(void) {
  return true;
}

#endif  // FE_DEBUG_PAYLOAD_MOVE

// ---------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------

#if FE_PERF_COUNTERS && !FE_GC_STRESS

// fe_perf.h's four payload counters, counted. This is the one build where
// they can be: the shipped counting build (`make perf`) has no type that owns
// a payload, so every payload counter in it is zero by construction, and the
// GC-stress build collects before every allocation, which makes a compaction
// count a function of the allocator's schedule rather than of the workload.
//
// Exact numbers, per fe's own counter-test rule: what a counter is FOR is
// being the same number on a loaded box, so a bound would give up the
// property being asserted.
static bool TestPayloadCountersCount(void) {
  FeContext* context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  // After the open, which allocates: the counters are process-wide, and what
  // is being measured is the three blocks below. The GAUGE is absolute and
  // the open leaves one block per core symbol name in it, so its own floor
  // is what the counter has to be read against.
  const size_t floor_bytes = FeGetArenaStats(context).payload_live_bytes;
  FePerfReset();
  enum { Blocks = 3 };
  const size_t each = sizeof(FePayloadBlock) + sizeof(FeObject*) + 16;
  const size_t base = FeSaveGC(context);
  FeObject* survivors[2] = {nullptr, nullptr};
  for (size_t i = 0; i < Blocks; i++) {
    FeObject* const owner = MakeAggregate(context, 1, 16);
    survivors[0] = i == 1 ? owner : survivors[0];
    survivors[1] = i == 2 ? owner : survivors[1];
  }
  CHECK(FePerfRead(FePerfPayloadAlloc) == Blocks);
  CHECK(FePerfRead(FePerfPayloadByte) == Blocks * each);
  // The counter and the gauge are the same bytes counted two ways, which is
  // what makes either one readable on its own.
  CHECK(FePerfRead(FePerfPayloadByte) + floor_bytes ==
        FeGetArenaStats(context).payload_live_bytes);
  CHECK(FePerfRead(FePerfPayloadCompact) == 0);
  CHECK(FePerfRead(FePerfPayloadCompactMoved) == 0);

  // The first block dies, so both survivors slide down over it.
  FeRestoreGC(context, base);
  FePushGC(context, survivors[0]);
  FePushGC(context, survivors[1]);
  FeCollectGarbage(context);
  CHECK(FePerfRead(FePerfPayloadCompact) == 1);
  CHECK(FePerfRead(FePerfPayloadCompactMoved) == 2);

  // A collection with nothing to reclaim still compacts -- that is a call,
  // and it is counted -- and moves nothing, which is the difference the two
  // counters exist to show.
  FeCollectGarbage(context);
  CHECK(FePerfRead(FePerfPayloadCompact) == 2);
  CHECK(FePerfRead(FePerfPayloadCompactMoved) == 2);
  CHECK(FePerfRead(FePerfPayloadAlloc) == Blocks);
  FeCloseContext(context);
  return true;
}

#else

static bool TestPayloadCountersCount(void) {
  return true;
}

#endif  // FE_PERF_COUNTERS && !FE_GC_STRESS

// ---------------------------------------------------------------------------
// Strings, the payload region's second release owner (Phase 25). What is here
// is the three gates the master plan names for it, all of which are about the
// storage MOVING under something that has to keep working: the five lengths
// surviving a compaction, a symbol still being found by name after its name's
// bytes have slid, and a replacement block leaving the header alone.
//
// The lane that arms `FE_DEBUG_PAYLOAD_MOVE` runs all three with the extent
// sliding at every allocation, which is what turns "the compactor happened not
// to move this" into "it moved and the answer is still right".
// ---------------------------------------------------------------------------

// A recognisable byte pattern of `length` bytes, NUL every third, so that a
// string read back through a stale address or a wrong length is visibly wrong
// rather than plausibly short.
static void FillStringPattern(char* buffer, size_t length) {
  for (size_t at = 0; at < length; at++) {
    buffer[at] = at % 3 == 1 ? '\0' : (char)('a' + (int)(at % 26));
  }
}

// The master plan's second gate: 0, 7, 8, 256 and 8192 bytes survive
// compaction. Each string is published with a dead block in front of it, so
// the collection really has somewhere to slide them down TO -- a compaction
// that reclaimed nothing would prove nothing here.
static bool TestStringsSurviveCompaction(void) {
  unsigned char* const storage = malloc(VectorArenaSize);
  CHECK(storage != nullptr);
  FeContext* const context = OpenVectorContext(storage, VectorArenaSize);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    free(storage);
    return false;
  }
  static const size_t lengths[] = {0, 7, 8, 256, 8192};
  enum { Count = sizeof(lengths) / sizeof(lengths[0]) };
  static char pattern[8192];
  static char copy[8192];
  FeObject* strings[Count] = {nullptr};
  FePayloadHandle before[Count] = {0};

  const size_t base = FeSaveGC(context);
  const size_t floor_bytes = context->payload_used;
  for (size_t i = 0; i < Count; i++) {
    // The doomed neighbour, unrooted the moment the next allocation happens
    // because nothing but the GC stack holds it and the checkpoint below
    // drops that.
    (void)FeMakeStringBytes(context, "doomed", 6);
    FillStringPattern(pattern, lengths[i]);
    strings[i] = FeMakeStringBytes(context, pattern, lengths[i]);
    before[i] = PAYLOAD(strings[i]);
  }
  FeRestoreGC(context, base);
  for (size_t i = 0; i < Count; i++) {
    FePushGC(context, strings[i]);
  }
  FeCollectGarbage(context);
  CHECK(context->payload_compaction_count == 1);

  size_t expected = floor_bytes;
  for (size_t i = 0; i < Count; i++) {
    // Every one of them MOVED -- five dead blocks came out from between them
    // -- and every one of them still reads back byte for byte.
    CHECK(PAYLOAD(strings[i]) != before[i]);
    CHECK(PAYLOAD(strings[i]) == ExpectedHandle(expected));
    expected += StringBlockBytes(lengths[i]);
    FillStringPattern(pattern, lengths[i]);
    CHECK(FeStringByteLength(context, strings[i]) == lengths[i]);
    CHECK(FeStringBytes(context, strings[i], copy, sizeof(copy)) == lengths[i]);
    CHECK(memcmp(copy, pattern, lengths[i]) == 0);
  }
  CHECK(context->payload_used == expected);
  FeCloseContext(context);
  free(storage);
  return true;
}

// THE STOPPING RULE'S TEST. A symbol's name is a string and a string's bytes
// move, so a lookup that read a stale address -- or an interning comparison
// that cached one across the scan -- would answer with the wrong symbol or
// with none. The move is CONSTRUCTED rather than hoped for: the garbage is
// allocated BEFORE the symbols, so reclaiming it leaves a hole the names have
// to slide down into, and the handles are asserted to have changed before any
// lookup is attempted.
static bool TestSymbolNamesSurviveMovingPayloads(void) {
  unsigned char* const storage = malloc(VectorArenaSize);
  CHECK(storage != nullptr);
  FeContext* const context = OpenVectorContext(storage, VectorArenaSize);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    free(storage);
    return false;
  }
  enum { Count = 64, Garbage = 128 };
  FeObject* symbols[Count] = {nullptr};
  FePayloadHandle names[Count] = {0};
  char name[SymbolNameLimit + 1];

  const size_t base = FeSaveGC(context);
  // The hole, first, so that what follows it has room to move.
  for (size_t i = 0; i < Garbage; i++) {
    (void)FeMakeStringBytes(context, "garbage-that-will-be-reclaimed", 30);
  }
  // Names of every length from 12 to 43 bytes, so the length test that
  // interning starts with is exercised in both directions.
  for (size_t i = 0; i < Count; i++) {
    (void)snprintf(name, sizeof(name), "moving-name-%zu-%.*s", i, (int)(i % 32),
                   "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    symbols[i] = FeMakeSymbol(context, name);
    names[i] = PAYLOAD(SymbolName(symbols[i]));
  }
  // Only the symbol list holds anything now, which is what makes the garbage
  // above garbage. The symbols themselves are interned and need no root.
  //
  // Counted as a DIFFERENCE rather than as a total since Phase 26: interning
  // 64 names past the 118 an open interns crosses the symbol index's growth
  // threshold, so the loop above orphans an index block of its own, and under
  // `FE_GC_STRESS` the next allocation's collection reclaims it. That is a
  // real compaction and this case is not the one that measures it; what it
  // asserts is that the collection IT forces reclaims something, which is
  // what makes the names below move.
  const size_t compactions_before = context->payload_compaction_count;
  FeRestoreGC(context, base);
  FeCollectGarbage(context);
  CHECK(context->payload_compaction_count == compactions_before + 1);

  for (size_t i = 0; i < Count; i++) {
    // The name's bytes are somewhere else than they were.
    CHECK(PAYLOAD(SymbolName(symbols[i])) != names[i]);
  }
  static const char miss[] = "(intern-soft \"no-such-name-anywhere\")";
  for (size_t i = 0; i < Count; i++) {
    // ...and the index still finds exactly the symbol that name belongs to,
    // by hashing the name bytes and comparing length-then-bytes through the
    // accessor, at an address it derives per candidate.
    (void)snprintf(name, sizeof(name), "moving-name-%zu-%.*s", i, (int)(i % 32),
                   "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK(FeMakeSymbol(context, name) == symbols[i]);
    // An evaluation between two lookups, so the names keep moving THROUGH
    // the loop rather than only before it: it allocates, which is a
    // compaction under `FE_GC_STRESS` and a slide of the whole extent under
    // `FE_DEBUG_PAYLOAD_MOVE`. Its own answer is the other half of the
    // contract -- a miss must not intern, whatever the storage did.
    CHECK(FeIsNil(
        FeEvaluateString(context, "intern.fe", miss, sizeof(miss) - 1)));
  }
  // A name whose LENGTH matches an interned one but whose bytes do not is a
  // miss, which is the half a length-only comparison would get wrong.
  (void)snprintf(name, sizeof(name), "moving-name-0-");
  name[strlen(name) - 1] = 'Z';
  CHECK(FeMakeSymbol(context, name) != symbols[0]);
  FeCloseContext(context);
  free(storage);
  return true;
}

// ---------------------------------------------------------------------------
// THE SYMBOL INDEX (Phase 26). Every case here is about the PAIR --
// `symbol_list` and the index -- rather than about either one alone, because
// a symbol visible in one and not the other is the only way this design can
// be wrong. `SymbolIndexMatchesSymbolList` is the question asked after each
// of them; it allocates nothing, so it can be asked in an exhausted arena
// and between two allocations in the poison lane alike.
// ---------------------------------------------------------------------------

// The name of the `i`th generated symbol. One spelling in one place, so that
// a loop that interns them and a loop that looks them up cannot drift, and a
// fixed length, so that what a name costs the region is arithmetic.
static void SymbolIndexName(char* buffer, size_t size, size_t i) {
  (void)snprintf(buffer, size, "fe-index-sym-%06zu", i);
}

// Is NAME on `symbol_list` at all? The AUTHORITY, asked directly, walked
// rather than probed, and allocating nothing -- which is what a case standing
// in an arena with no free cell needs, and what makes this an independent
// answer rather than the index's own opinion repeated back.
static bool SymbolListHasName(FeContext* context, const char* name) {
  char buffer[SymbolNameLimit + 1];
  const size_t length = strlen(name);
  for (const FeObject* rest = context->symbol_list; !FeIsNil(rest);
       rest = CDR(rest)) {
    const size_t held =
        FeStringBytes(context, SymbolName(CAR(rest)), buffer, sizeof(buffer));
    if (held == length && memcmp(buffer, name, length) == 0) {
      return true;
    }
  }
  return false;
}

// Free region bytes, which is what tells one payload failure site from
// another: a name's own block is 48 bytes and a table's is thousands, so a
// raise with room to spare is a raise the TABLE could not fit in.
static size_t FreePayloadBytes(const FeContext* context) {
  const FeArenaStats stats = FeGetArenaStats(context);
  return stats.payload_capacity_bytes - stats.payload_live_bytes;
}

// `intern-soft`'s double-probe contract, asked through the Lisp surface that
// has it: probe, miss, probe again, still miss -- then intern, and the probe
// after that finds it. A miss that interned would make the idiom this
// contract exists for (`(while (setq x (intern-soft (format ...))) ...)`)
// never terminate, and the index must not have made it possible.
static bool CheckDoubleProbe(FeContext* context, const char* name) {
  char source[128];
  const size_t before = context->symbol_index_count;
  const int written =
      snprintf(source, sizeof(source), "(intern-soft \"%s\")", name);
  CHECK(written > 0 && (size_t)written < sizeof(source));
  const size_t length = (size_t)written;
  CHECK(FeIsNil(FeEvaluateString(context, "probe.fe", source, length)));
  CHECK(FeIsNil(FeEvaluateString(context, "probe.fe", source, length)));
  // Neither probe interned anything, in either structure.
  CHECK(context->symbol_index_count == before);
  CHECK(!SymbolListHasName(context, name));
  const FeObject* const interned = FeMakeSymbol(context, name);
  CHECK(FeGetType(interned) == FeTSymbol);
  CHECK(context->symbol_index_count == before + 1);
  CHECK(SymbolListHasName(context, name));
  CHECK(FeEvaluateString(context, "probe.fe", source, length) == interned);
  CHECK(SymbolIndexMatchesSymbolList(context));
  return true;
}

// The index's OWN STORAGE under compaction, which is the moving-name case
// aimed one level down: a symbol's name moves and the table that finds it by
// name moves too, and neither knows about the other's address. The move is
// CONSTRUCTED rather than hoped for -- the garbage is allocated before the
// symbols, so reclaiming it leaves a hole the table has to slide down into --
// and the table's handle is asserted to have changed before a single lookup
// is attempted.
static bool TestSymbolIndexStorageSurvivesCompaction(void) {
  FeContext* const context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  // The smallest arena and count that still take the table through several
  // resizes with one of them AFTER the hole below. Sized rather than
  // generous because the stress build collects at every allocation, and
  // every collection sweeps the whole arena.
  enum { Count = 300, Rooted = 150, Garbage = 128 };
  static FeObject* symbols[Count];
  char name[SymbolNameLimit + 1];

  const size_t base = FeSaveGC(context);
  const size_t opened = context->symbol_index_count;
  const FePayloadHandle first_table = PAYLOAD(context->symbol_index);
  for (size_t i = 0; i < Rooted; i++) {
    SymbolIndexName(name, sizeof(name), i);
    symbols[i] = FeMakeSymbol(context, name);
    FeRestoreGC(context, base);
  }
  // The hole, and it is ROOTED until the table has been republished past it.
  // That is what makes this case say the same thing in both builds: under
  // `FE_GC_STRESS` every allocation collects, so garbage dropped before the
  // next resize would be gone long before the collection this case forces,
  // and the table would have nothing to slide down into.
  for (size_t i = 0; i < Garbage; i++) {
    (void)FeMakeStringBytes(context, "garbage-that-will-be-reclaimed", 30);
  }
  const size_t held = FeSaveGC(context);
  bool resized = false;
  for (size_t i = Rooted; i < Count; i++) {
    const size_t before = FeGetArenaStats(context).payload_live_bytes;
    SymbolIndexName(name, sizeof(name), i);
    symbols[i] = FeMakeSymbol(context, name);
    FeRestoreGC(context, held);
    resized =
        resized || FeGetArenaStats(context).payload_live_bytes > before + 1024;
  }
  // Far past the room the table opened with, so it has been REPLACED several
  // times over -- a resize publishes a new block and the owner stops naming
  // the old one, which is the substrate's ordinary replacement rule and not a
  // rule the index taught it -- and the last of those replacements landed
  // after the hole above. A table's block is thousands of bytes where a
  // name's is 48, so a jump in live payload bytes is a resize and nothing
  // else is: that is how this knows rather than assumes.
  CHECK(context->symbol_index_count == opened + Count);
  CHECK(PAYLOAD(context->symbol_index) != first_table);
  CHECK(resized);
  CHECK(SymbolIndexMatchesSymbolList(context));

  // Only `symbol_list` holds anything now: the hole is garbage, and so is
  // every table the resizes left behind.
  const FePayloadHandle grown = PAYLOAD(context->symbol_index);
  FeRestoreGC(context, base);
  FeCollectGarbage(context);
  // THE TABLE'S OWN BYTES ARE SOMEWHERE ELSE THAN THEY WERE.
  CHECK(PAYLOAD(context->symbol_index) != grown);
  CHECK(SymbolIndexMatchesSymbolList(context));

  static const char miss[] = "(intern-soft \"no-such-name-anywhere\")";
  for (size_t i = 0; i < Count; i++) {
    // ...and every name still finds exactly its own symbol, by hashing bytes
    // that moved, in a table that moved, to a header that did not.
    SymbolIndexName(name, sizeof(name), i);
    CHECK(FeMakeSymbol(context, name) == symbols[i]);
    if (i % 64 == 0) {
      // An evaluation between two lookups, so the storage keeps moving
      // THROUGH the loop rather than only before it: it allocates, which is a
      // compaction under `FE_GC_STRESS` and a slide of the whole extent under
      // `FE_DEBUG_PAYLOAD_MOVE`. Its own answer is the other half of the
      // contract -- a miss must not intern, whatever the storage did.
      CHECK(FeIsNil(
          FeEvaluateString(context, "intern.fe", miss, sizeof(miss) - 1)));
      FeRestoreGC(context, base);
    }
  }
  CHECK(SymbolIndexMatchesSymbolList(context));
  FeCloseContext(context);
  return true;
}

// The double-probe contract at the three table states that can tell it
// apart: a table nothing but the open has touched, a run of interns that
// straddles a growth threshold -- so one of them is the probe either side of
// the publish that replaces the whole table -- and a table with 8192 symbols
// in it, which is the tier the plan names.
static bool TestInternSoftDoubleProbe(void) {
  FeContext* const context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  enum { Straddle = 120 };
  char name[SymbolNameLimit + 1];
  const size_t base = FeSaveGC(context);

  // An empty table, in the only sense a table of fe's can be empty: nothing
  // but the interpreter's own names is in it.
  CHECK(CheckDoubleProbe(context, "probe-at-an-empty-table"));

  // Across a growth threshold. A resize publishes a block of thousands of
  // bytes where a name's own is 48, so a jump in live payload bytes is a
  // resize and nothing else is -- which is how this asserts that the span
  // really did straddle one rather than assuming the constants it would take
  // to work that out.
  bool resized = false;
  for (size_t i = 0; i < Straddle; i++) {
    const size_t before = FeGetArenaStats(context).payload_live_bytes;
    SymbolIndexName(name, sizeof(name), i);
    CHECK(CheckDoubleProbe(context, name));
    FeRestoreGC(context, base);
    resized =
        resized || FeGetArenaStats(context).payload_live_bytes > before + 1024;
  }
  CHECK(resized);

  CHECK(SymbolIndexMatchesSymbolList(context));
  FeCloseContext(context);
  return true;
}

#if !FE_GC_STRESS

// The same contract at the tier the plan names: 8192 symbols, so the probes
// below walk a table at its full size rather than a nearly empty one.
//
// Not built under `FE_GC_STRESS`, for `TestMarkDepthIsFlat`'s reason: that
// knob collects at every allocation, this case allocates about fifty thousand
// times into a 8 MiB arena, and what a probe answers does not depend on how
// often the collector ran while the table was filling. The case that DOES
// depend on that is `TestSymbolIndexStorageSurvivesCompaction` above, which
// is built both ways.
static bool TestInternSoftDoubleProbeAtTheTier(void) {
  enum { Tier = 8192 };
  unsigned char* const storage = malloc(VectorArenaSize);
  CHECK(storage != nullptr);
  FeContext* const context = OpenVectorContext(storage, VectorArenaSize);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    free(storage);
    return false;
  }
  char name[SymbolNameLimit + 1];
  const size_t base = FeSaveGC(context);
  for (size_t i = 0; i < Tier; i++) {
    SymbolIndexName(name, sizeof(name), i);
    (void)FeMakeSymbol(context, name);
    FeRestoreGC(context, base);
  }
  CHECK(context->symbol_index_count > Tier);
  CHECK(CheckDoubleProbe(context, "probe-at-the-8192-tier"));
  CHECK(SymbolIndexMatchesSymbolList(context));
  FeCloseContext(context);
  free(storage);
  return true;
}

#else

static bool TestInternSoftDoubleProbeAtTheTier(void) {
  return true;
}

#endif  // !FE_GC_STRESS

// What every failure injection below asks afterwards, and the reason the
// stopping rule this phase carries could be checked at all: the raise left
// the name in NEITHER structure, and the two structures still agree with each
// other. Neither question allocates, which matters -- three of the four
// sites leave an arena with no free cell to answer them in.
static bool PublishLeftNothingBehind(FeContext* context,
                                     const char* name,
                                     size_t before) {
  CHECK(host.raised);
  CHECK(!SymbolListHasName(context, name));
  CHECK(context->symbol_index_count == before);
  CHECK(SymbolIndexMatchesSymbolList(context));
  return true;
}

// A symbol costs six cells: the symbol object, the three pairs of its
// `((name . plist) . function) . value` chain, the one string object its name
// is, and the `symbol_list` cell that interns it. So an arena with five free
// cells or fewer cannot publish one -- and each of those six is a DIFFERENT
// allocation failing, the last being `FeMakeSymbol`'s own cons, which is the
// step after the symbol object exists and before anything names it.
//
// The sweep runs the whole range and then asserts that SIX succeeds, which is
// what says the five below it were the failing region rather than five
// arbitrary numbers.
static bool TestSymbolPublishFailsCleanlyOnCells(void) {
  enum { SymbolCells = 6, Surplus = 8192 };
  // A small arena on purpose: the fill below takes one cons per free cell, and
  // under `FE_GC_STRESS` every one of them sweeps the whole arena. What this
  // case needs is a few hundred spare cells and a region with room for one
  // name, which is what the minimum plus a small surplus is.
  unsigned char* const storage = malloc(FeMinimumArenaSize() + Surplus);
  CHECK(storage != nullptr);
  for (size_t spare = 0; spare <= SymbolCells; spare++) {
    FeContext* const context = OpenContextWithPayload(
        storage, FeMinimumArenaSize() + Surplus, PayloadArenaPercent);
    CHECK(context != nullptr);
    host.raised = false;
    FeSetErrorFn(context, HandleError);
    if (setjmp(host.jump) != 0) {
      free(storage);
      return false;
    }
    static const char name[] = "a-name-no-arena-had-room-for";
    const size_t before = context->symbol_index_count;
    const size_t base = FeSaveGC(context);
    FeObject* filler = FeNil(context);
    while (FeGetArenaStats(context).free_slots > spare) {
      FeRestoreGC(context, base);
      FePushGC(context, filler);
      filler = FeCons(context, FeNil(context), filler);
    }
    if (spare < SymbolCells) {
      host.raised = false;
      if (setjmp(host.jump) == 0) {
        (void)FeMakeSymbol(context, name);
        CHECK(false);
      }
      CHECK(strcmp(host.message, "out of memory") == 0);
      CHECK(ConditionIs(context, "(arena-exhaustion)"));
      CHECK(PublishLeftNothingBehind(context, name, before));
    } else {
      const FeObject* const symbol = FeMakeSymbol(context, name);
      CHECK(FeGetType(symbol) == FeTSymbol);
      CHECK(SymbolListHasName(context, name));
      CHECK(context->symbol_index_count == before + 1);
      CHECK(SymbolIndexMatchesSymbolList(context));
    }
    FeCloseContext(context);
  }
  free(storage);
  return true;
}

// The two REGION sites, which are the ones the index itself owns.
//
// Site 1 is the name's own block, and it is reached at a region carved at 0%:
// that is exactly the core names' floor, with no spare byte at all, so the
// first name a program interns fails inside `MakeStringObject` -- after the
// symbol's cell was taken and before the index or the list was told anything.
//
// Site 2 is THE INDEX'S OWN RESIZE, the one allocation the index makes.
// Reaching it needs a region with room for the names that carry the table to
// its growth threshold and no room for the doubled table itself, which is a
// wide window -- a name's block is 48 bytes and the table's next one is
// thousands -- and the filler string below is sized to land in it. What
// proves the site is the free region AFTER the raise: a name would still have
// fitted, and the table would not.
static bool TestSymbolPublishFailsCleanlyOnRegion(void) {
  enum { TargetSpare = 4000, Attempts = 1000 };
  static const char absent[] = "a-name-the-region-had-no-room-for";
  FeContext* context = OpenPayloadContext(0);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  size_t before = context->symbol_index_count;
  CHECK(FreePayloadBytes(context) == 0);
  host.raised = false;
  if (setjmp(host.jump) == 0) {
    (void)FeMakeSymbol(context, absent);
    CHECK(false);
  }
  CHECK(ConditionIs(context, "(payload-exhaustion)"));
  CHECK(PublishLeftNothingBehind(context, absent, before));
  FeCloseContext(context);

  context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  static FeObject* interned[Attempts];
  char name[SymbolNameLimit + 1];
  const size_t base = FeSaveGC(context);
  before = context->symbol_index_count;
  // The filler, sized from what the region has rather than from what this
  // arena is expected to have, and rooted for the rest of the case: a
  // collection that could reclaim it would undo the whole construction.
  FeObject* const filler = FeMakeStringBytes(context, "filler", 6);
  const size_t free_bytes = FreePayloadBytes(context);
  CHECK(free_bytes > TargetSpare + sizeof(FePayloadBlock));
  PublishPayload(context, filler, 0,
                 free_bytes - TargetSpare - sizeof(FePayloadBlock));
  CHECK(FreePayloadBytes(context) <= TargetSpare);

  // `volatile` because it is written between the `setjmp` and the `longjmp`
  // and read after it, which is the one case where an ordinary automatic has
  // an indeterminate value. Not theoretical: without it this case passed in
  // the ordinary build and failed in the ASan lane at -O1, naming the symbol
  // interned one before the failing one.
  volatile size_t made = 0;
  host.raised = false;
  if (setjmp(host.jump) == 0) {
    while (made < Attempts) {
      SymbolIndexName(name, sizeof(name), made);
      interned[made] = FeMakeSymbol(context, name);
      made++;
      FeRestoreGC(context, base);
      FePushGC(context, filler);
    }
    CHECK(false);
  }
  SymbolIndexName(name, sizeof(name), made);
  CHECK(ConditionIs(context, "(payload-exhaustion)"));
  CHECK(PublishLeftNothingBehind(context, name, before + made));
  // THE SITE, pinned by what was left: room for a name's 48-byte block many
  // times over, and none for the table's next one. A raise here is the resize
  // refusing, not a string.
  CHECK(FreePayloadBytes(context) >= 256);
  CHECK(made > 0);
  // ...and the table the failed resize did not replace still answers for
  // every name that was in it. This is the half a repair pass would be hiding:
  // there is nothing to repair, because the old block was never disowned.
  for (size_t i = 0; i < made; i++) {
    SymbolIndexName(name, sizeof(name), i);
    CHECK(FeMakeSymbol(context, name) == interned[i]);
  }
  CHECK(SymbolIndexMatchesSymbolList(context));
  FeCloseContext(context);
  return true;
}

// The master plan's fourth gate: a string mutation that needs a REPLACEMENT
// block leaves the header alone. This is the reader's growing literal in one
// statement -- `PublishPayload` is what `AppendStringByte` calls when the
// capacity runs out -- asked here rather than through the reader because what
// has to be observed is the object's identity ACROSS the replacement, which
// the reader only hands back afterwards.
static bool TestStringReplacementKeepsHeader(void) {
  FeContext* const context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  FeObject* const string = FeMakeStringBytes(context, "ab\0cdefgh", 9);
  FeObject* const holder = FeCons(context, string, &nil);
  const FePayloadHandle before = PAYLOAD(string);
  char copy[32];

  PublishPayload(context, string, 0, 4096);
  CHECK(PAYLOAD(string) != before);
  // The same object, still: `holder` was never told anything, and the length
  // word never moved -- it is in the header, which is what did not change.
  CHECK(FeCar(context, holder) == string);
  CHECK(FeStringByteLength(context, string) == 9);
  CHECK(FeStringBytes(context, string, copy, sizeof(copy)) == 9);
  CHECK(memcmp(copy, "ab\0cdefgh", 9) == 0);

  // ...and the same in the other direction, which is the reader's trim: a
  // SMALLER replacement keeps every byte the string actually has.
  PublishPayload(context, string, 0, 9);
  CHECK(FeStringBytes(context, string, copy, sizeof(copy)) == 9);
  CHECK(memcmp(copy, "ab\0cdefgh", 9) == 0);
  FeCloseContext(context);
  return true;
}

// The release path the two above stand in for: the reader, growing a literal
// one byte at a time. What it costs is several blocks of which all but the
// last are dead, and what it leaves is a string whose block is exactly the
// size `FeMakeStringBytes` would have given it -- the trim, which is why the
// reader's growth policy is nothing the rest of fe has to know about.
static bool TestReaderLiteralGrowsAndTrims(void) {
  FeContext* const context = OpenPayloadContext(PayloadArenaPercent);
  CHECK(context != nullptr);
  if (setjmp(host.jump) != 0) {
    return false;
  }
  enum { Length = 300 };
  static char source[Length + 3];
  static char copy[Length + 1];
  source[0] = '"';
  FillStringPattern(source + 1, Length);
  for (size_t i = 0; i < Length; i++) {
    // A NUL cannot be written literally in source -- it is the reader's end
    // of input -- so the pattern's NULs are spelled as escapes, which is what
    // makes this the reader's own NUL path as well.
    source[1 + i] = source[1 + i] == '\0' ? 'N' : source[1 + i];
  }
  source[1 + Length] = '"';

  const size_t before_used = context->payload_used;
  FeObject* const string = FeReadString(context, source, Length + 2, nullptr);
  const size_t grown_used = context->payload_used;
  CHECK(string != nullptr && FeGetType(string) == FeTString);
  CHECK(FeStringByteLength(context, string) == Length);
  CHECK(FeStringBytes(context, string, copy, sizeof(copy)) == Length);
  CHECK(memcmp(copy, source + 1, Length) == 0);
  // More than one block was published -- the growth -- and after a collection
  // reclaims the dead ones the survivor is exactly one trimmed block.
  CHECK(grown_used > before_used + StringBlockBytes(Length));
  const size_t gc = FeSaveGC(context);
  FePushGC(context, string);
  FeCollectGarbage(context);
  FeRestoreGC(context, gc);
  CHECK(context->payload_used == before_used + StringBlockBytes(Length));
  CHECK(FeStringBytes(context, string, copy, sizeof(copy)) == Length);
  CHECK(memcmp(copy, source + 1, Length) == 0);
  FeCloseContext(context);
  return true;
}

int main(void) {
  const bool ok =
      TestRegionCarve() && TestBumpAllocationOrder() &&
      TestExactFitAndOneByteOver() && TestOpenOptionsPartition() &&
      TestArenaStatsReportsPayload() && TestMarkReachesPayloadChildren() &&
      TestReplacedBlockIsReclaimed() &&
      TestCompactionAddressesAreDeterministic() && TestSlidingOverlap() &&
      TestLiveDeadMixture() && TestCloseContextWithLivePayloads() &&
      TestPayloadExhaustionIsCatchable() && TestPayloadExhaustionDegrades() &&
      TestFailureInjectionAtEveryAllocation() && TestMarkDepthIsFlat() &&
      TestVectorCompactionIsDeterministic() &&
      TestVectorExhaustionLeavesNothingBehind() && TestVectorCostTable() &&
      TestStringsSurviveCompaction() &&
      TestSymbolNamesSurviveMovingPayloads() &&
      TestSymbolIndexStorageSurvivesCompaction() &&
      TestInternSoftDoubleProbe() && TestInternSoftDoubleProbeAtTheTier() &&
      TestSymbolPublishFailsCleanlyOnCells() &&
      TestSymbolPublishFailsCleanlyOnRegion() &&
      TestStringReplacementKeepsHeader() && TestReaderLiteralGrowsAndTrims() &&
      TestRandomAccessIsFlat() && TestPoisonedPointerFailsLoudly() &&
      TestPayloadCountersCount();
  (void)printf(
      "payload_tests: FE_GC_STRESS=%d FE_DEBUG_PAYLOAD_MOVE=%d "
      "FE_PERF_COUNTERS=%d: %s\n",
      FE_GC_STRESS, FE_DEBUG_PAYLOAD_MOVE, FE_PERF_COUNTERS,
      ok ? "ok" : "FAILED");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
