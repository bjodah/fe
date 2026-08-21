// Builds a bounded object graph -- including the cycles `fuzz_eval`
// deliberately never generates -- and renders it. The properties under test are
// that `FeWriteWithOptions()` always terminates, never emits more than
// `max_bytes`, and that `FeToString()` always leaves a terminated string inside
// its destination.
//
// The graph is built by evaluating a generated program rather than through the
// C API, because pair mutation is a language primitive: `setcar` and `setcdr`
// have no public C spelling.

#include "fuzz_support.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"

enum {
  MaxInputSize = 4096,
  MaxNodes = 32,
  SourceSize = 8192,
  RenderSize = 4096,
};

typedef struct Sink {
  size_t written;
} Sink;

static void Count(FeContext*, void* udata, char) {
  Sink* sink = udata;
  sink->written++;
}

static bool IsM07Seed(const uint8_t* data, size_t size) {
  static const uint8_t seed[] = {0x81, 0x81, 0xa4, 0xa4,
                                 0x0f, 0x52, 0x86, 0x81};
  return size == sizeof(seed) && memcmp(data, seed, sizeof(seed)) == 0;
}

typedef struct Source {
  char text[SourceSize];
  size_t length;
} Source;

static void AppendText(Source* source, const char* text) {
  const size_t length = strlen(text);
  if (source->length + length >= sizeof(source->text)) {
    abort();
  }
  memcpy(source->text + source->length, text, length);
  source->length += length;
}

static void AppendNumber(Source* source, unsigned value) {
  char digits[16];
  size_t count = 0;
  do {
    digits[count++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (count > 0) {
    const char one[2] = {digits[--count], '\0'};
    AppendText(source, one);
  }
}

static void AppendNode(Source* source, unsigned index) {
  AppendText(source, "n");
  AppendNumber(source, index);
}

// One `car` or `cdr` value: nil, an atom, or one of the nodes -- which is what
// makes spine cycles, car cycles, shared structure and improper tails all
// reachable.
static void AppendValue(Source* source, FuzzInput* input, unsigned count) {
  const uint8_t choice = FuzzTakeByte(input);
  const unsigned datum = FuzzTakeByte(input);
  switch (choice % 5) {
    case 0:
      AppendText(source, "nil");
      break;
    case 1:
      AppendNumber(source, datum);
      break;
    case 2:
      AppendText(source, "'s");
      AppendNumber(source, datum);
      break;
    case 3:
      AppendText(source, "\"t");
      AppendNumber(source, datum);
      AppendText(source, "\"");
      break;
    default:
      AppendNode(source, datum % count);
      break;
  }
}

static void BuildSource(Source* source, FuzzInput* input, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    AppendText(source, "(setq ");
    AppendNode(source, i);
    AppendText(source, " (cons nil nil))\n");
  }
  for (unsigned i = 0; i < count; i++) {
    AppendText(source, "(setcar ");
    AppendNode(source, i);
    AppendText(source, " ");
    AppendValue(source, input, count);
    AppendText(source, ")\n(setcdr ");
    AppendNode(source, i);
    AppendText(source, " ");
    AppendValue(source, input, count);
    AppendText(source, ")\n");
  }
  AppendText(source, "n0\n");
}

static size_t TakeBudget(FuzzInput* input) {
  // Zero means "default", and is deliberately reachable.
  const size_t low = FuzzTakeByte(input);
  const size_t high = FuzzTakeByte(input);
  return low | (high << 8);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > MaxInputSize) {
    return 0;
  }

  FuzzArena arena;
  FeContext* ctx = FuzzOpenContext(&arena);

  if (setjmp(FuzzErrorJump) == 0) {
    FuzzInput input = {.data = data, .size = size, .offset = 0};
    const unsigned count = 1 + FuzzTakeByte(&input) % MaxNodes;
    Source source = {.length = 0};
    BuildSource(&source, &input, count);

    const size_t gc = FeSaveGC(ctx);
    FeObject* graph =
        FeEvaluateString(ctx, "graph.fe", source.text, source.length);
    const FeWriteOptions options = {
        .max_bytes = TakeBudget(&input),
        .max_nodes = TakeBudget(&input),
        .max_depth = FuzzTakeByte(&input),
    };
    Sink sink = {0};
    const bool complete =
        FeWriteWithOptions(ctx, graph, Count, &sink, 1, &options);
    if (options.max_bytes != 0 && sink.written > options.max_bytes) {
      abort();
    }
    if (complete && sink.written == 0) {
      abort();
    }
    // M0.7's two-node car/cdr cycle must stay bounded by deterministic work,
    // not merely by the fuzz lane's wall-clock timeout.
    if (IsM07Seed(data, size) && sink.written > 12u * (1u << 20)) {
      abort();
    }

    char rendered[RenderSize];
    memset(rendered, 'x', sizeof(rendered));
    const size_t stored = FeToString(ctx, graph, rendered, sizeof(rendered));
    if (stored >= sizeof(rendered) || rendered[stored] != '\0' ||
        strlen(rendered) != stored) {
      abort();
    }
    FeRestoreGC(ctx, gc);
  }

  FeCloseContext(ctx);
  return 0;
}
