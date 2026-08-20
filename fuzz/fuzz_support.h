#ifndef FUZZ_SUPPORT_H
#define FUZZ_SUPPORT_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "fe.h"

// The context plus the free space the harness actually fuzzes in.
// What steers this lane is the FREE portion -- how many allocations a
// generated form gets before the arena raises, and therefore how often a
// collection lands inside one -- and `FeMinimumArenaSize()` grows whenever
// the core interns something new at open, or the context itself widens. It
// was 60344 bytes when this was a flat 64 KiB (~324 free slots); Phase 19's
// seeded `error-message` properties took it to 63592, and API version 11's
// per-binding host tag took it to 66264 (256 cleanup entries, one pointer
// each), and Phase 24's vector family plus the `end-of-file` condition row
// took it to 67664 -- 70 more interned slots, 1120 bytes. Each of those
// would have left the free portion shorter and
// silently re-steered every tracked seed by making forms raise sooner, so
// the size here restores the free portion (~340 slots) rather than the
// total, which is what keeps `fuzz/seeds/reachability.json` measuring the
// grammar instead of the arena. Re-measure it when `FeMinimumArenaSize()`
// moves: `strict-arity-rest` is the seed that fails first, and it did.
//
// The harness opens with `FeOpenContext`, which carves NO payload region, so
// a generated `[...]` reads its elements and then raises
// `(payload-exhaustion)` where a carved context would build a vector. That is
// deliberate for this phase: a carve takes a quarter of the cells, which is
// the free portion this constant exists to hold steady, so arming one is a
// re-derivation of every tracked seed and not a one-line change.
enum { FuzzArenaSize = 70 * 1024 + 1120 };

typedef union {
  max_align_t alignment;
  unsigned char bytes[FuzzArenaSize];
} FuzzArena;

typedef struct {
  const uint8_t* data;
  size_t size;
  size_t offset;
} FuzzInput;

extern jmp_buf FuzzErrorJump;

FeContext* FuzzOpenContext(FuzzArena* arena);
char FuzzReadByte(FeContext* ctx, void* udata);
uint8_t FuzzTakeByte(FuzzInput* input);

#endif
