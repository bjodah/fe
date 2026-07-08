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
#include "fex.h"
#include "fex_io.h"
#include "fex_math.h"
#include "fex_process.h"
#include "fex_re.h"
#include "fex_time.h"

static const char* InterpreterVersion = "1.0";

static jmp_buf top_level;

static void PrintError(FeContext* ctx, const char* message, FeObject* stack) {
  fprintf(stderr, "error: %s\n", message);
  while (!FeIsNil(stack)) {
    char fn[1024];
    (void)FeToString(ctx, FeCar(ctx, stack), fn, sizeof(fn));
    fprintf(stderr, "%s\n", fn);
    stack = FeCdr(ctx, stack);
  }
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

static FeObject* HandleGC(FeContext* ctx, FeObject* args) {
  return Handle(ctx, args, "gc");
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
  size_t arena_size = 64 * 1024;
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

  // Initialize the context:
  AUTO(char*, arena, malloc(arena_size), FreeChar);
  FeContext* opened_context = FeOpenContext(arena, arena_size);
  if (opened_context == nullptr) {
    fprintf(stderr,
            "could not initialize Fe: arena must be aligned and at least %zu "
            "bytes\n",
            FeMinimumArenaSize());
    return EXIT_FAILURE;
  }
  AUTO(FeContext*, context, opened_context, CloseContext);
  FeSetErrorFn(context, HandleFatalError);
  if (extensions) {
    FexInit(context);
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
    gc = ReadEvaluatePrint(context, input, gc);
  }
  if (interactive) {
    ReadEvaluatePrint(context, stdin, gc);
  }
}
