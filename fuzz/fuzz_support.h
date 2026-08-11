#ifndef FUZZ_SUPPORT_H
#define FUZZ_SUPPORT_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "fe.h"

// 64 KiB of context plus the free space the harness actually fuzzes in.
// What steers this lane is the FREE portion -- how many allocations a
// generated form gets before the arena raises, and therefore how often a
// collection lands inside one -- and `FeMinimumArenaSize()` grows whenever
// the core interns something new at open. It was 60344 bytes when this was
// a flat 64 KiB (~324 free slots); Phase 19's seeded `error-message`
// properties took it to 63592, which would have left ~120 and silently
// re-steered every tracked seed by making forms raise sooner. The 4 KiB
// here restores the free portion (~340 slots) rather than the total, which
// is what keeps `fuzz/seeds/reachability.json` measuring the grammar
// instead of the arena. Re-measure it when `FeMinimumArenaSize()` moves.
enum { FuzzArenaSize = 68 * 1024 };

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
