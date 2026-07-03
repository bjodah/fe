#include "fuzz_support.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fe.h"

enum {
  MaxDepth = 4,
  MaxInputSize = 4096,
};

static FeObject* BuildExpression(FeContext* ctx,
                                 FuzzInput* input,
                                 unsigned depth);

static FeObject* MakeForm(FeContext* ctx,
                          const char* name,
                          FeObject** arguments,
                          size_t count) {
  FeObject* items[4];
  items[0] = FeMakeSymbol(ctx, name);
  for (size_t i = 0; i < count; i++) {
    items[i + 1] = arguments[i];
  }
  return FeMakeList(ctx, items, count + 1);
}

static FeObject* MakeUnary(FeContext* ctx,
                           const char* name,
                           FeObject* argument) {
  return MakeForm(ctx, name, (FeObject*[]){argument}, 1);
}

static FeObject* MakeBinary(FeContext* ctx,
                            const char* name,
                            FeObject* first,
                            FeObject* second) {
  return MakeForm(ctx, name, (FeObject*[]){first, second}, 2);
}

static FeObject* BuildNumber(FeContext* ctx, FuzzInput* input) {
  const uint16_t bits =
      (uint16_t)FuzzTakeByte(input) | (uint16_t)FuzzTakeByte(input) << 8;
  return FeMakeDouble(ctx, (double)(int16_t)bits);
}

static FeObject* BuildString(FeContext* ctx, FuzzInput* input) {
  char value[9];
  const size_t size = FuzzTakeByte(input) % sizeof(value);
  for (size_t i = 0; i < size; i++) {
    value[i] = (char)(' ' + FuzzTakeByte(input) % 95);
  }
  value[size] = '\0';
  return FeMakeString(ctx, value);
}

static FeObject* BuildAtom(FeContext* ctx, FuzzInput* input) {
  switch (FuzzTakeByte(input) % 5) {
    case 0:
      return &nil;
    case 1:
      return FeMakeSymbol(ctx, "t");
    case 2:
      return FeMakeSymbol(ctx, "x");
    case 3:
      return BuildString(ctx, input);
    default:
      return BuildNumber(ctx, input);
  }
}

static FeObject* BuildDatum(FeContext* ctx, FuzzInput* input, unsigned depth) {
  if (depth == MaxDepth || FuzzTakeByte(input) % 3 != 0) {
    return BuildAtom(ctx, input);
  }

  FeObject* items[3];
  const size_t count = 1 + FuzzTakeByte(input) % 3;
  for (size_t i = 0; i < count; i++) {
    items[i] = BuildDatum(ctx, input, depth + 1);
  }
  return FeMakeList(ctx, items, count);
}

static FeObject* BuildQuotedDatum(FeContext* ctx,
                                  FuzzInput* input,
                                  unsigned depth) {
  return MakeUnary(ctx, "quote", BuildDatum(ctx, input, depth));
}

static FeObject* BuildNumericExpression(FeContext* ctx,
                                        FuzzInput* input,
                                        unsigned depth) {
  static const char* operators[] = {"+", "-", "*", "/"};
  if (depth == MaxDepth || FuzzTakeByte(input) % 2 == 0) {
    return BuildNumber(ctx, input);
  }
  const char* op = operators[FuzzTakeByte(input) % 4];
  return MakeBinary(ctx, op, BuildNumber(ctx, input), BuildNumber(ctx, input));
}

static FeObject* BuildListForm(FeContext* ctx,
                               FuzzInput* input,
                               unsigned depth) {
  FeObject* arguments[3];
  const size_t count = FuzzTakeByte(input) % 4;
  for (size_t i = 0; i < count; i++) {
    arguments[i] = BuildExpression(ctx, input, depth + 1);
  }
  return MakeForm(ctx, "list", arguments, count);
}

static FeObject* BuildBindingForm(FeContext* ctx,
                                  FuzzInput* input,
                                  unsigned depth,
                                  const char* binding) {
  FeObject* value = BuildExpression(ctx, input, depth + 1);
  FeObject* assign = MakeBinary(ctx, binding, FeMakeSymbol(ctx, "x"), value);
  return MakeBinary(ctx, "do", assign, FeMakeSymbol(ctx, "x"));
}

static FeObject* BuildFunctionCall(FeContext* ctx,
                                   FuzzInput* input,
                                   unsigned depth) {
  FeObject* parameter = FeMakeSymbol(ctx, "x");
  FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
  FeObject* body = MakeBinary(ctx, "cons", parameter,
                              BuildExpression(ctx, input, depth + 1));
  FeObject* function = MakeForm(ctx, "fn", (FeObject*[]){parameters, body}, 2);
  return FeMakeList(
      ctx, (FeObject*[]){function, BuildExpression(ctx, input, depth + 1)}, 2);
}

static FeObject* BuildMacroCall(FeContext* ctx,
                                FuzzInput* input,
                                unsigned depth) {
  FeObject* parameter = FeMakeSymbol(ctx, "x");
  FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
  FeObject* quoted_quote = MakeUnary(ctx, "quote", FeMakeSymbol(ctx, "quote"));
  FeObject* body = MakeBinary(ctx, "list", quoted_quote, parameter);
  FeObject* macro = MakeForm(ctx, "macro", (FeObject*[]){parameters, body}, 2);
  return FeMakeList(ctx,
                    (FeObject*[]){macro, BuildDatum(ctx, input, depth + 1)}, 2);
}

static FeObject* BuildMutation(FeContext* ctx,
                               FuzzInput* input,
                               unsigned depth) {
  FeObject* pair =
      MakeBinary(ctx, "cons", BuildExpression(ctx, input, depth + 1), &nil);
  FeObject* bind = MakeBinary(ctx, "let", FeMakeSymbol(ctx, "x"), pair);
  const char* setter = FuzzTakeByte(input) % 2 == 0 ? "setcar" : "setcdr";
  FeObject* replacement = BuildQuotedDatum(ctx, input, depth + 1);
  FeObject* mutate =
      MakeBinary(ctx, setter, FeMakeSymbol(ctx, "x"), replacement);
  return MakeForm(ctx, "do",
                  (FeObject*[]){bind, mutate, FeMakeSymbol(ctx, "x")}, 3);
}

static FeObject* BuildExpression(FeContext* ctx,
                                 FuzzInput* input,
                                 unsigned depth) {
  static const char* arithmetic[] = {"+", "-", "*", "/"};
  if (depth == MaxDepth) {
    return BuildAtom(ctx, input);
  }

  switch (FuzzTakeByte(input) % 20) {
    case 0:
      return BuildAtom(ctx, input);
    case 1:
      return BuildQuotedDatum(ctx, input, depth + 1);
    case 2:
      return BuildListForm(ctx, input, depth);
    case 3:
      return MakeBinary(ctx, "cons", BuildExpression(ctx, input, depth + 1),
                        BuildExpression(ctx, input, depth + 1));
    case 4: {
      FeObject* pair =
          MakeBinary(ctx, "cons", BuildExpression(ctx, input, depth + 1), &nil);
      return MakeUnary(ctx, "car", pair);
    }
    case 5: {
      FeObject* pair =
          MakeBinary(ctx, "cons", BuildExpression(ctx, input, depth + 1), &nil);
      return MakeUnary(ctx, "cdr", pair);
    }
    case 6:
      return MakeUnary(ctx, "atom", BuildExpression(ctx, input, depth + 1));
    case 7:
      return MakeUnary(ctx, "not", BuildExpression(ctx, input, depth + 1));
    case 8:
      return MakeBinary(ctx, "is", BuildExpression(ctx, input, depth + 1),
                        BuildExpression(ctx, input, depth + 1));
    case 9: {
      const char* op = arithmetic[FuzzTakeByte(input) % 4];
      return MakeBinary(ctx, op, BuildNumericExpression(ctx, input, depth + 1),
                        BuildNumericExpression(ctx, input, depth + 1));
    }
    case 10: {
      const char* op = FuzzTakeByte(input) % 2 == 0 ? "<" : "<=";
      return MakeBinary(ctx, op, BuildNumber(ctx, input),
                        BuildNumber(ctx, input));
    }
    case 11:
      return MakeForm(ctx, "if",
                      (FeObject*[]){BuildExpression(ctx, input, depth + 1),
                                    BuildExpression(ctx, input, depth + 1),
                                    BuildExpression(ctx, input, depth + 1)},
                      3);
    case 12: {
      const char* op = FuzzTakeByte(input) % 2 == 0 ? "and" : "or";
      return MakeBinary(ctx, op, BuildExpression(ctx, input, depth + 1),
                        BuildExpression(ctx, input, depth + 1));
    }
    case 13:
      return MakeBinary(ctx, "do", BuildExpression(ctx, input, depth + 1),
                        BuildExpression(ctx, input, depth + 1));
    case 14:
      return BuildBindingForm(ctx, input, depth, "let");
    case 15:
      return BuildBindingForm(ctx, input, depth, "=");
    case 16:
      return BuildFunctionCall(ctx, input, depth);
    case 17:
      return BuildMacroCall(ctx, input, depth);
    case 18:
      return BuildMutation(ctx, input, depth);
    default:
      return BuildNumericExpression(ctx, input, depth + 1);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > MaxInputSize) {
    return 0;
  }

  FuzzArena arena;
  FeContext* ctx = FuzzOpenContext(&arena);

  if (setjmp(FuzzErrorJump) == 0) {
    FuzzInput input = {.data = data, .size = size, .offset = 0};
    while (input.offset < input.size) {
      const size_t gc = FeSaveGC(ctx);
      FeObject* expression = BuildExpression(ctx, &input, 0);
      if (getenv("FE_FUZZ_DUMP") != NULL) {
        FeWriteFile(ctx, expression, stderr);
        fputc('\n', stderr);
      }
      FeObject* result = FeEvaluate(ctx, expression);
      char rendered[256];
      (void)FeToString(ctx, result, rendered, sizeof(rendered));
      FeRestoreGC(ctx, gc);
    }
  }

  FeCloseContext(ctx);
  return 0;
}
