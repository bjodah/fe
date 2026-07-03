#ifndef FUZZ_SUPPORT_H
#define FUZZ_SUPPORT_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "fe.h"

enum { FuzzArenaSize = 64 * 1024 };

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
