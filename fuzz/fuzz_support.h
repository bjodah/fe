#ifndef FUZZ_SUPPORT_H
#define FUZZ_SUPPORT_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "fe.h"

// The context plus the free space the harness actually fuzzes in.
// What steers this lane is the FREE portion -- how many allocations a
// generated form gets before the arena raises, and therefore how often a
// collection lands inside one -- and both `FeMinimumArenaSize()` and the
// payload carve below take bytes away from it. `FeMinimumArenaSize()` grows
// whenever the core interns something new at open, or the context itself
// widens: it was 60344 bytes when this was a flat 64 KiB; Phase 19's seeded
// `error-message` properties took it to 63592, API version 11's per-binding
// host tag to 66264, and Phase 24's vector family plus the `end-of-file`
// condition row to 67664. Each of those would have left the free portion
// shorter and silently re-steered every tracked seed by making forms raise
// sooner, so this constant is re-derived to restore the free portion rather
// than the total, which is what keeps `fuzz/seeds/reachability.json`
// measuring the grammar instead of the arena. Re-measure it whenever either
// term moves: `strict-arity-rest` is the seed that fails first, and it did.
//
// The harness opens with `FeOpenContextWithOptions` and null options, which
// is Fe's own split -- a quarter of what is left once frames are funded goes
// to the payload region -- so a generated `[...]` BUILDS a vector here
// instead of raising `(payload-exhaustion)` the way an `FeOpenContext` arena
// makes it. That is the whole reason the size moved in Phase 25.0: the carve
// takes 25% of the cells, and holding the free portion steady across it is a
// re-derivation of the constant, not a one-line change. Measured, at this
// size: 288 free cell slots either way -- exactly what the uncarved arena had
// before the carve -- 71 frames, and 1536 payload bytes, room for a vector of
// up to 188 elements.
enum { FuzzArenaSize = 72 * 1024 + 768 };

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
