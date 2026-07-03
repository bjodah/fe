#include "fuzz_support.h"

#include <setjmp.h>
#include <stdlib.h>

jmp_buf FuzzErrorJump;

[[noreturn]] static void HandleError(FeContext* ctx,
                                     const char* message,
                                     FeObject* stack) {
  (void)ctx;
  (void)message;
  (void)stack;
  longjmp(FuzzErrorJump, 1);
}

FeContext* FuzzOpenContext(FuzzArena* arena) {
  FeContext* ctx = FeOpenContext(arena->bytes, sizeof(arena->bytes));
  if (ctx == nullptr) {
    abort();
  }
  FeSetErrorFn(ctx, HandleError);
  return ctx;
}

char FuzzReadByte(FeContext* ctx, void* udata) {
  (void)ctx;
  FuzzInput* input = udata;
  if (input->offset == input->size) {
    return '\0';
  }
  return (char)input->data[input->offset++];
}

uint8_t FuzzTakeByte(FuzzInput* input) {
  if (input->offset == input->size) {
    return 0;
  }
  return input->data[input->offset++];
}
