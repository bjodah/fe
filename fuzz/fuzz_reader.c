#include "fuzz_support.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "fe.h"

enum { MaxInputSize = 64 * 1024 };

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > MaxInputSize) {
    return 0;
  }

  FuzzArena arena;
  FeContext* ctx = FuzzOpenContext(&arena);

  if (setjmp(FuzzErrorJump) == 0) {
    FuzzInput input = {.data = data, .size = size, .offset = 0};
    const size_t gc = FeSaveGC(ctx);
    FeObject* object;
    while ((object = FeRead(ctx, FuzzReadByte, &input)) != NULL) {
      char rendered[256];
      (void)FeToString(ctx, object, rendered, sizeof(rendered));
      FeRestoreGC(ctx, gc);
    }
  }

  FeCloseContext(ctx);
  return 0;
}
