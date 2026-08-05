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
  MaxEvaluationSteps = 10000,
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
  const int16_t value = (int16_t)((uint16_t)FuzzTakeByte(input) |
                                  (uint16_t)FuzzTakeByte(input) << 8);
  // 05B reach-ahead: odd tag bytes make an integer, so the grammar already
  // mixes both numeric types before the reader can produce integers.
  return FuzzTakeByte(input) & 1 ? FeMakeInteger(ctx, value)
                                 : FeMakeDouble(ctx, (double)value);
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

// A macro body. A list expansion is the interesting structural case, but the
// atom expansions are the ones that used to be miscompiled: the expansion was
// copied over the call site, and `nil` and interned symbols are compared by
// address, so a copy of either was a different object.
static FeObject* BuildMacroBody(FeContext* ctx,
                                FuzzInput* input,
                                FeObject* parameter) {
  switch (FuzzTakeByte(input) % 5) {
    case 0:
      return &nil;
    case 1:
      return MakeUnary(ctx, "quote", FeMakeSymbol(ctx, "t"));
    case 2:
      return MakeUnary(ctx, "quote", FeMakeSymbol(ctx, "x"));
    case 3:
      return BuildNumber(ctx, input);
    default: {
      FeObject* quoted_quote =
          MakeUnary(ctx, "quote", FeMakeSymbol(ctx, "quote"));
      return MakeBinary(ctx, "list", quoted_quote, parameter);
    }
  }
}

static FeObject* BuildMacroCall(FeContext* ctx,
                                FuzzInput* input,
                                unsigned depth) {
  FeObject* parameter = FeMakeSymbol(ctx, "x");
  FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
  FeObject* body = BuildMacroBody(ctx, input, parameter);
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

// Sub-plan 04C's `funcall`/`apply` exercised through the evaluate-then-
// redispatch path: `FeFrameEvalList` evaluates the complete operand list, the
// first result is resolved through the designator chain, and the call is
// redispatched via the quoted-argument call form. The fuzz lane is what
// vouches for the rooting of that evaluated-operand buffer across the
// redispatch (03F's lesson) -- a collection forced by the *called body* must
// not sweep a value the relay frame is still building the call from -- so
// these shapes build the boundary on purpose rather than leaving the grammar
// to reach it by accident. Half the shapes pass a direct closure, half a
// `cons` symbol designator, which resolves through the function cell -- the
// callables' home since the 04D namespace cut.
static FeObject* BuildFuncallForm(FeContext* ctx,
                                  FuzzInput* input,
                                  unsigned depth) {
  if (FuzzTakeByte(input) % 2 == 0) {
    FeObject* parameter = FeMakeSymbol(ctx, "x");
    FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
    FeObject* body = MakeBinary(ctx, "cons", parameter,
                                BuildExpression(ctx, input, depth + 1));
    FeObject* function =
        MakeForm(ctx, "fn", (FeObject*[]){parameters, body}, 2);
    return MakeForm(
        ctx, "funcall",
        (FeObject*[]){function, BuildExpression(ctx, input, depth + 1)}, 2);
  }
  return MakeForm(ctx, "funcall",
                  (FeObject*[]){FeMakeSymbol(ctx, "cons"),
                                BuildExpression(ctx, input, depth + 1),
                                BuildExpression(ctx, input, depth + 1)},
                  3);
}

static FeObject* BuildApplyForm(FeContext* ctx,
                                FuzzInput* input,
                                unsigned depth) {
  if (FuzzTakeByte(input) % 2 == 0) {
    FeObject* parameter = FeMakeSymbol(ctx, "x");
    FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
    FeObject* body = MakeBinary(ctx, "cons", parameter,
                                BuildExpression(ctx, input, depth + 1));
    FeObject* function =
        MakeForm(ctx, "fn", (FeObject*[]){parameters, body}, 2);
    FeObject* spread =
        MakeUnary(ctx, "list", BuildExpression(ctx, input, depth + 1));
    return MakeForm(ctx, "apply", (FeObject*[]){function, spread}, 2);
  }
  return MakeForm(
      ctx, "apply",
      (FeObject*[]){
          FeMakeSymbol(ctx, "cons"), BuildExpression(ctx, input, depth + 1),
          MakeUnary(ctx, "list", BuildExpression(ctx, input, depth + 1))},
      3);
}

// Sub-plan 04D's `fset`: the function cell is written and call position
// reads it back in the same generated form, so the name is re-filled every
// time it appears and an earlier form's `fmakunbound` (or a later form's own
// re-`fset`) can never unpin the call this form makes.
static FeObject* BuildFsetForm(FeContext* ctx,
                               FuzzInput* input,
                               unsigned depth) {
  FeObject* parameter = FeMakeSymbol(ctx, "x");
  FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
  FeObject* body = MakeBinary(ctx, "cons", parameter,
                              BuildExpression(ctx, input, depth + 1));
  FeObject* fn = MakeForm(ctx, "fn", (FeObject*[]){parameters, body}, 2);
  FeObject* assign = MakeBinary(
      ctx, "fset", MakeUnary(ctx, "quote", FeMakeSymbol(ctx, "f")), fn);
  FeObject* call =
      FeMakeList(ctx,
                 (FeObject*[]){FeMakeSymbol(ctx, "f"),
                               BuildExpression(ctx, input, depth + 1)},
                 2);
  return MakeForm(ctx, "do", (FeObject*[]){assign, call}, 2);
}

// Sub-plan 04D designator chains: `defalias` points a fresh name at another
// fresh name whose function cell an `fset` filled, and call position follows
// the two-hop chain. The self-alias arm (`(defalias 'x 'x)`) covers the
// cyclic-function-indirection error path, which must stay a caught error
// rather than a hang.
static FeObject* BuildDefaliasForm(FeContext* ctx,
                                   FuzzInput* input,
                                   unsigned depth) {
  FeObject* parameter = FeMakeSymbol(ctx, "x");
  FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
  FeObject* body = MakeBinary(ctx, "cons", parameter,
                              BuildExpression(ctx, input, depth + 1));
  FeObject* fn = MakeForm(ctx, "fn", (FeObject*[]){parameters, body}, 2);
  FeObject* target = FeMakeSymbol(ctx, "b");
  FeObject* alias = FeMakeSymbol(ctx, "a");
  FeObject* fset = MakeBinary(ctx, "fset", MakeUnary(ctx, "quote", target), fn);
  FeObject* defalias =
      MakeBinary(ctx, "defalias", MakeUnary(ctx, "quote", alias),
                 FuzzTakeByte(input) % 2 == 0 ? MakeUnary(ctx, "quote", target)
                                              : MakeUnary(ctx, "quote", alias));
  FeObject* call = FeMakeList(
      ctx, (FeObject*[]){alias, BuildExpression(ctx, input, depth + 1)}, 2);
  return MakeForm(ctx, "do", (FeObject*[]){fset, defalias, call}, 3);
}

// Sub-plan 04D's `function`, the raw-form special form `#'x` reads as: a
// lambda wrapped in `function` produces the closure `funcall` then invokes,
// and a bare symbol is the designator itself, resolved through the function
// cell on the call.
static FeObject* BuildFunctionForm(FeContext* ctx,
                                   FuzzInput* input,
                                   unsigned depth) {
  if (FuzzTakeByte(input) % 2 == 0) {
    FeObject* parameter = FeMakeSymbol(ctx, "x");
    FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
    FeObject* body = MakeBinary(ctx, "cons", parameter,
                                BuildExpression(ctx, input, depth + 1));
    FeObject* function =
        MakeUnary(ctx, "function",
                  MakeForm(ctx, "lambda", (FeObject*[]){parameters, body}, 2));
    return MakeForm(
        ctx, "funcall",
        (FeObject*[]){function, BuildExpression(ctx, input, depth + 1)}, 2);
  }
  return MakeForm(
      ctx, "funcall",
      (FeObject*[]){MakeUnary(ctx, "function", FeMakeSymbol(ctx, "cons")),
                    BuildExpression(ctx, input, depth + 1),
                    BuildExpression(ctx, input, depth + 1)},
      3);
}

// Sub-plan 04D's `fmakunbound` under a live callable: the function cell is
// filled, then emptied, then called, so the follow-up resolves to
// void-function. Half the shapes route the call through a `defalias`
// designator chain first, so resolution walks past the emptied cell.
static FeObject* BuildFmakunboundForm(FeContext* ctx,
                                      FuzzInput* input,
                                      unsigned depth) {
  FeObject* parameter = FeMakeSymbol(ctx, "x");
  FeObject* parameters = FeMakeList(ctx, (FeObject*[]){parameter}, 1);
  FeObject* body = MakeBinary(ctx, "cons", parameter,
                              BuildExpression(ctx, input, depth + 1));
  FeObject* fn = MakeForm(ctx, "fn", (FeObject*[]){parameters, body}, 2);
  FeObject* name = FeMakeSymbol(ctx, "k");
  FeObject* fset = MakeBinary(ctx, "fset", MakeUnary(ctx, "quote", name), fn);
  FeObject* fmakunbound =
      MakeUnary(ctx, "fmakunbound", MakeUnary(ctx, "quote", name));
  if (FuzzTakeByte(input) % 2 == 0) {
    FeObject* call = FeMakeList(
        ctx, (FeObject*[]){name, BuildExpression(ctx, input, depth + 1)}, 2);
    return MakeForm(ctx, "do", (FeObject*[]){fset, fmakunbound, call}, 3);
  }
  FeObject* alias = FeMakeSymbol(ctx, "ak");
  FeObject* defalias =
      MakeBinary(ctx, "defalias", MakeUnary(ctx, "quote", alias),
                 MakeUnary(ctx, "quote", name));
  FeObject* call = FeMakeList(
      ctx, (FeObject*[]){alias, BuildExpression(ctx, input, depth + 1)}, 2);
  return MakeForm(
      ctx, "do",
      (FeObject*[]){fset, defalias,
                    MakeForm(ctx, "do", (FeObject*[]){fmakunbound, call}, 2)},
      3);
}

static FeObject* BuildExpression(FeContext* ctx,
                                 FuzzInput* input,
                                 unsigned depth) {
  static const char* arithmetic[] = {"+", "-", "*", "/"};
  if (depth == MaxDepth) {
    return BuildAtom(ctx, input);
  }

  switch (FuzzTakeByte(input) % 26) {
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
    case 19:
      return BuildFuncallForm(ctx, input, depth);
    case 20:
      return BuildApplyForm(ctx, input, depth);
    case 21:
      return BuildFsetForm(ctx, input, depth);
    case 22:
      return BuildDefaliasForm(ctx, input, depth);
    case 23:
      return BuildFunctionForm(ctx, input, depth);
    case 24:
      return BuildFmakunboundForm(ctx, input, depth);
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
    // The grammar uses `x` as its one free variable, and an unassigned symbol
    // is now an error rather than nil.
    static const char preamble[] = "(setq x nil)";
    (void)FeEvaluateString(ctx, "preamble", preamble, sizeof(preamble) - 1);
    FuzzInput input = {.data = data, .size = size, .offset = 0};
    while (input.offset < input.size) {
      const size_t gc = FeSaveGC(ctx);
      FeObject* expression = BuildExpression(ctx, &input, 0);
      if (getenv("FE_FUZZ_DUMP") != NULL) {
        FeWriteFile(ctx, expression, stderr);
        fputc('\n', stderr);
      }
      const FeEvalOptions options = {.step_limit = MaxEvaluationSteps};
      FeObject* result = FeEvaluateWithOptions(ctx, expression, &options);
      char rendered[256];
      (void)FeToString(ctx, result, rendered, sizeof(rendered));
      FeRestoreGC(ctx, gc);
    }
  }

  FeCloseContext(ctx);
  return 0;
}
