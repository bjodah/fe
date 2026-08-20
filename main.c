// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auto.h"
#include "fe.h"
#include "fe_perf.h"
#include "fex.h"
#include "fex_io.h"
#include "fex_math.h"
#include "fex_process.h"
#include "fex_re.h"
#include "fex_time.h"

static const char* InterpreterVersion = "1.0";

// The escaping-raise trace is one line per live evaluator frame, and a raise
// from deep inside a recursion has as many frames as the arena could hold: an
// out-of-memory in a self-recursive `cons` chain printed 316 lines on a
// 300 KB arena and grows with arena size, burying the message the reader
// actually needs underneath its own stack. The printer already refuses to
// walk deeper than that into a single object for the same reason; this
// applies the printer's own number across the trace --
// `FeWriteDefaultMaxDepth`, not a second copy of it -- and reports how many
// frames it dropped rather than stopping in silence. Sub-plan 09A Decision 5:
// the 1024-byte per-form truncation `FeToString` below applies stays as it is
// -- that is truncation, not failure.
enum { MaxPrintedTraceFrames = FeWriteDefaultMaxDepth };

static jmp_buf top_level;

static FeContext* live_context;
static char* live_arena;
static FILE* live_input;

// `exit()` does not run the `cleanup` attributes, so the fatal path has to
// release the interpreter itself. Otherwise every diagnostic leaves the arena
// behind and, worse, never runs the extension finalizers, so open files are
// never closed.
static void ReleaseInterpreter(void) {
  FeContext* ctx = live_context;
  char* arena = live_arena;
  live_context = nullptr;
  live_arena = nullptr;
  FILE* input = live_input;
  live_input = nullptr;
  if (ctx != nullptr) {
    FeCloseContext(ctx);
  }
  free(arena);
  if (input != nullptr) {
    (void)fclose(input);
  }
}

// The escaping raise's call trace, innermost frame first, bounded per
// `MaxPrintedTraceFrames`.
static void PrintErrorTrace(FeContext* ctx, FeObject* stack) {
  for (size_t printed = 0; !FeIsNil(stack); printed++) {
    if (printed == MaxPrintedTraceFrames) {
      size_t remaining = 0;
      for (FeObject* rest = stack; !FeIsNil(rest); rest = FeCdr(ctx, rest)) {
        remaining++;
      }
      fprintf(stderr, "... %zu more frames\n", remaining);
      return;
    }
    char fn[1024];
    (void)FeToString(ctx, FeCar(ctx, stack), fn, sizeof(fn));
    fprintf(stderr, "%s\n", fn);
    stack = FeCdr(ctx, stack);
  }
}

static void PrintError(FeContext* ctx, const char* message, FeObject* stack) {
  if (getenv("FE_STRUCTURED_ERRORS") != nullptr &&
      FeGetCompletion(ctx) == FeCompletionQuit) {
    fputs("condition: quit\n", stderr);
  } else if (getenv("FE_STRUCTURED_ERRORS") != nullptr &&
             !FeIsNil(FeGetCondition(ctx))) {
    char condition[1024];
    (void)FeToString(ctx, FeCar(ctx, FeGetCondition(ctx)), condition,
                     sizeof(condition));
    fprintf(stderr, "condition: %s\n", condition);
    // The data list too, printed the way the Emacs oracle shim prints
    // `(prin1-to-string (cdr err))`, so the compat runner can compare the
    // whole condition object rather than only its symbol.
    char data[1024];
    (void)FeToString(ctx, FeCdr(ctx, FeGetCondition(ctx)), data, sizeof(data));
    fprintf(stderr, "data: %s\n", data);
  }
  fprintf(stderr, "error: %s\n", message);
  PrintErrorTrace(ctx, stack);
}

[[noreturn]] static void HandleError(FeContext* ctx,
                                     const char* message,
                                     FeObject* stack) {
  PrintError(ctx, message, stack);
  longjmp(top_level, -1);
}

[[noreturn]] static void HandleFatalError(FeContext* ctx,
                                          const char* message,
                                          FeObject* stack) {
  PrintError(ctx, message, stack);
  ReleaseInterpreter();
  exit(EXIT_FAILURE);
}

static FeObject* Handle(FeContext* ctx, FeObject* args, const char* prefix) {
  char marked[1024];
  (void)FeToString(ctx, args, marked, sizeof(marked));
  fprintf(stderr, "%s: %s\n", prefix, marked);
  return &nil;
}

static FeObject* HandleMark(FeContext* ctx, FeObject* args) {
  return Handle(ctx, args, "mark");
}

// Whether `FexInit` installed the extension collector callback that this
// tracer has to chain to. Set once, beside the install itself.
static bool tracing_extensions;

static FeObject* HandleGC(FeContext* ctx, FeObject* args) {
  (void)Handle(ctx, args, "gc");
  // CHAIN, never replace. `FeSetGCFn` holds one function, so `-d` installing
  // this tracer took the extensions' own callback out of the collector -- and
  // with it the close of an owned `FILE*` and the free of a compiled regular
  // expression. Invisible until the debug host joined `make check`, whose
  // valgrind and ASan lanes then named the three `FexTFile` records `-d` was
  // leaking on every run.
  return tracing_extensions ? FexGC(ctx, args) : &nil;
}

[[noreturn]] static void PrintHelp(int status) {
  FILE* outputs[] = {stdout, stderr};
  FILE* out = outputs[status != EXIT_SUCCESS];
  fprintf(out,
          "fe — Fe language interpreter\n\n"
          "Usage:\n\n"
          "  fe -h\n"
          "  fe [-i] [-s size] [program-file ...]\n\n"
          "Options:\n\n"
          "  -d    Verbose debugging\n"
          "  -h    Print this help message and exit\n"
          "  -i    Interactive mode (read from stdin)\n"
          "  -s <size>\n"
          "        Set arena size\n"
          "  -v    Print the version and exit\n"
          "  -x    Do not install the Fex extensions\n");
  exit(status);
}

#if FE_PERF_COUNTERS
// The counting build's report (`make perf`): every counter, and the arena
// gauges beside them, written to $FE_PERF_OUT as JSON when a run completes.
// Unset, or unwritable, means "not measuring" -- a counting interpreter still
// has to be a usable one, so nothing here fails loudly. A run that ends
// through an escaping error exits before this by design: the counters
// describe a completed run.
static void WritePerfCounters(FeContext* ctx) {
  const char* const path = getenv("FE_PERF_OUT");
  if (path == nullptr || *path == '\0') {
    return;
  }
  FILE* const out = fopen(path, "w");
  if (out == nullptr) {
    return;
  }
  const FeArenaStats stats = FeGetArenaStats(ctx);
  FePerfWriteJson(out, &stats);
  (void)fclose(out);
}
#else
#define WritePerfCounters(ctx) ((void)0)
#endif

static size_t ReadEvaluatePrint(FeContext* context, FILE* input, size_t gc) {
  while (true) {
    FeRestoreGC(context, gc);
    if (input == stdin) {
      printf("fe > ");
    }
    FeObject* object = FeReadFile(context, input);
    if (object == NULL) {
      return FeSaveGC(context);
    }
    object = FeEvaluate(context, object);
    if (input == stdin) {
      FeWriteFile(context, object, stdout);
      printf("\n");
    }
  }
}

int main(int count, char* arguments[]) {
  // Parse command line options:
  // The evaluator's frame region shares this arena with objects. Keep the
  // standalone interpreter comfortably above the minimal embedding arena so
  // its shipped recursive examples do not hit the transitional frame wall.
  size_t arena_size = 1024 * 1024;
  bool debugging = false;
  bool program_literal = false;
  bool interactive = false;
  bool extensions = true;
  while (true) {
    int ch = getopt(count, arguments, "dehis:vx");
    if (ch == -1) {
      break;
    }
    switch (ch) {
      case 'd':
        debugging = true;
        break;
      case 'e':
        program_literal = true;
        break;
      case 'h':
        PrintHelp(EXIT_SUCCESS);
      case 'i':
        interactive = true;
        break;
      case 's': {
        char* end = NULL;
        arena_size = strtoul(optarg, &end, 0);
        if (end == optarg) {
          PrintHelp(EXIT_FAILURE);
        }
        break;
      }
      case 'v':
        printf("Fe version: %s\nFex version: %s\nInterpreter version: %s\n",
               FeVersion, FexVersion, InterpreterVersion);
        return 0;
      case 'x':
        extensions = false;
        break;
      default:
        PrintHelp(EXIT_FAILURE);
    }
  }
  count -= optind;
  arguments += optind;
  interactive |= count == 0;

  // Initialize the context. With Fe's own payload split (Phase 24): this
  // interpreter is a host, and a host that wants vectors has to ask for the
  // region their elements live in -- `FeOpenContext` deliberately carves
  // nothing and says so. Default options rather than a number spelled here,
  // so the ADR's split stays one constant in fe.h.
  AUTO(char*, arena, malloc(arena_size), FreeChar);
  FeContext* opened_context =
      FeOpenContextWithOptions(arena, arena_size, nullptr);
  if (opened_context == nullptr) {
    fprintf(stderr,
            "could not initialize Fe: arena must be aligned and at least %zu "
            "bytes\n",
            FeMinimumArenaSize());
    return EXIT_FAILURE;
  }
  AUTO(FeContext*, context, opened_context, CloseContext);
  live_context = opened_context;
  live_arena = arena;
  FeSetErrorFn(context, HandleFatalError);
  if (extensions) {
    FexInit(context);
    tracing_extensions = true;
    FexInstallIO(context);
    FexInstallMath(context);
    FexInstallProcess(context);
    FexInstallRE(context);
    FexInstallTime(context);
  }
  if (debugging) {
    FeSetMarkFn(context, HandleMark);
    FeSetGCFn(context, HandleGC);
  }
  if (interactive) {
    setjmp(top_level);
    FeSetErrorFn(context, HandleError);
  }

  // REPL the inputs:
  size_t gc = FeSaveGC(context);
  for (int i = 0; i < count; i++) {
    char* a = arguments[i];
    AUTO(FILE*, input,
         program_literal ? fmemopen(a, strlen(a), "rb") : fopen(a, "rb"),
         CloseFile);
    if (!input) {
      FeHandleError(context, "could not open input file");
    }
    live_input = input;
    gc = ReadEvaluatePrint(context, input, gc);
    live_input = nullptr;
  }
  if (interactive) {
    ReadEvaluatePrint(context, stdin, gc);
  }
  WritePerfCounters(context);
}
