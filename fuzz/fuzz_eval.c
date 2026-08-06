#include "fuzz_support.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fe.h"

enum {
  MaxDepth = 6,
  MaxInputSize = 4096,
  MaxEvaluationSteps = 10000,
};

static FeObject* BuildExpression(FeContext* ctx,
                                 FuzzInput* input,
                                 unsigned depth);
static FeObject* BuildBindingForm(FeContext* ctx,
                                  FuzzInput* input,
                                  unsigned depth,
                                  const char* binding);

static FeObject* BuildLambdaParameters(FeContext* ctx, FuzzInput* input) {
  FeObject* x = FeMakeSymbol(ctx, "x");
  FeObject* y = FeMakeSymbol(ctx, "y");
  switch (FuzzTakeByte(input) % 6) {
    case 0:
      return FeMakeList(ctx, nullptr, 0);
    case 1:
      return FeMakeList(ctx, (FeObject*[]){x}, 1);
    case 2:
      return FeMakeList(ctx, (FeObject*[]){x, y}, 2);
    case 3:
      return FeMakeList(ctx,
                        (FeObject*[]){x, FeMakeSymbol(ctx, "&optional"), y}, 3);
    case 4:
      return FeMakeList(ctx, (FeObject*[]){x, FeMakeSymbol(ctx, "&rest"), y},
                        3);
    default:
      // Deliberately malformed declarations exercise invalid-function.
      return FeMakeList(ctx, (FeObject*[]){FeMakeSymbol(ctx, "&rest"), y, x},
                        3);
  }
}

static FeObject* MakeForm(FeContext* ctx,
                          const char* name,
                          FeObject** arguments,
                          size_t count) {
  FeObject* items[8];
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

// Magnitudes an int16 window cannot reach. Without these the whole grammar
// generated operands in [-32768, 32767], so no reduction it built could ever
// overflow int64: the `ckd_add`/`ckd_sub`/`ckd_mul` arms, the unary negation
// of INT64_MIN and the INT64_MIN/-1 division edge -- every arith-error path
// 05C added -- were unreachable by construction, and the fuzz lane that
// exists to chase exactly that class of change never reached it. Both signs
// of 2^62 and 2^31 are here as well as the two extremes, because overflow at
// the boundary is a different arithmetic than overflow of two mid-sized
// operands, and -1/0/1 are here so a sentinel can pair with the identity and
// zero-divisor edges rather than only with another huge value.
static const int64_t number_sentinels[] = {
    INT64_MIN,
    INT64_MIN + 1,
    INT64_MAX,
    (int64_t)1 << 62,
    -((int64_t)1 << 62),
    (int64_t)1 << 31,
    -((int64_t)1 << 31),
    -1,
    0,
    1,
};

static FeObject* BuildNumber(FeContext* ctx, FuzzInput* input) {
  const int16_t value = (int16_t)((uint16_t)FuzzTakeByte(input) |
                                  (uint16_t)FuzzTakeByte(input) << 8);
  // The host integer mix. 05B's reach-ahead landed the odd-tag-byte integer
  // arm so the grammar mixed both numeric types before the reader could;
  // 05C made every arithmetic, comparison, equality and predicate path
  // dispatch on both tags, so bit 0 of this tag byte is what decides which
  // side of the promotion rule a generated operand lands on. Bit 1 swaps the
  // int16 window for a sentinel, which is what makes mixed-magnitude
  // arithmetic -- a huge operand against a small one -- generatable at all.
  // The sentinel index is a separate byte, taken only on that arm, so the
  // ordinary path consumes exactly the three bytes it always did.
  const uint8_t tag = FuzzTakeByte(input);
  int64_t magnitude = value;
  if ((tag & 2) != 0) {
    magnitude =
        number_sentinels[FuzzTakeByte(input) % (sizeof(number_sentinels) /
                                                sizeof(number_sentinels[0]))];
  }
  return (tag & 1) != 0 ? FeMakeInteger(ctx, magnitude)
                        : FeMakeDouble(ctx, (double)magnitude);
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
  switch (FuzzTakeByte(input) % 7) {
    case 0:
      return &nil;
    case 1:
      return FeMakeSymbol(ctx, "t");
    case 2:
      return FeMakeSymbol(ctx, "x");
    case 3:
      return FeMakeSymbol(ctx, ":fuzz-keyword");
    // The one-character keyword, which is a keyword and a constant here
    // exactly as it is in Emacs.
    case 4:
      return FeMakeSymbol(ctx, ":");
    case 5:
      return BuildString(ctx, input);
    default:
      return BuildNumber(ctx, input);
  }
}

static FeObject* BuildDatum(FeContext* ctx, FuzzInput* input, unsigned depth) {
  if (depth >= MaxDepth || FuzzTakeByte(input) % 3 != 0) {
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
  if (depth >= MaxDepth || FuzzTakeByte(input) % 2 == 0) {
    return BuildNumber(ctx, input);
  }
  // 05C: the tower makes the arithmetic variadic, so 0..3 operands reach the
  // identity elements ((+), (*)), the unary seeds ((- x) is negation, (/ x)
  // the truncated reciprocal), and the reductions whose integer arms can
  // overflow (arith-error) or divide by zero -- the class of change this fuzz
  // lane exists to chase, per the 05C gate.
  const char* op = operators[FuzzTakeByte(input) % 4];
  FeObject* arguments[3];
  const size_t count = FuzzTakeByte(input) % 4;
  for (size_t i = 0; i < count; i++) {
    arguments[i] = BuildNumber(ctx, input);
  }
  return MakeForm(ctx, op, arguments, count);
}

// 05C's comparison family: `<`/`<=`/`>`/`>=` and `=` chain left to right
// through one shared loop, so 0..3 operands reach the up-front zero-arity
// rejection and the vacuous unary `t` as well as the chain. `/=` is strictly
// binary in Emacs, so its arity stays 1..3 to reach the third-operand
// wrong-number-of-arguments on purpose rather than by accident.
static FeObject* BuildComparisonForm(FeContext* ctx, FuzzInput* input) {
  static const char* operators[] = {"<", "<=", ">", ">=", "=", "/="};
  const size_t op_index = FuzzTakeByte(input) % 6;
  FeObject* arguments[3];
  const size_t count =
      op_index == 5 ? 1 + FuzzTakeByte(input) % 3 : FuzzTakeByte(input) % 4;
  for (size_t i = 0; i < count; i++) {
    arguments[i] = BuildNumber(ctx, input);
  }
  return MakeForm(ctx, operators[op_index], arguments, count);
}

// 05D's identity operators `eq`/`eql`, binary leaves in the same
// `FeFrameBinary` family as `is`: the grammar's host-made integer/double mix
// reaches eq's both-integers-equal rule and eql's same-type-bit equality
// (and, through the 05D reader in the source-dict lane, the boxed-float
// identity the two-separately-read `3.0` spellings exercise).
static FeObject* BuildIdentityForm(FeContext* ctx,
                                   FuzzInput* input,
                                   unsigned depth) {
  const char* name = FuzzTakeByte(input) % 2 == 0 ? "eq" : "eql";
  return MakeForm(ctx, name,
                  (FeObject*[]){BuildExpression(ctx, input, depth + 1),
                                BuildExpression(ctx, input, depth + 1)},
                  2);
}

// 05C's numeric predicates: leaves that answer t or nil by tag, fed from the
// grammar's host-made integer/double mix -- the only way to reach an integer
// without the reader.
static FeObject* BuildPredicateForm(FeContext* ctx,
                                    FuzzInput* input,
                                    unsigned depth) {
  const char* name = FuzzTakeByte(input) % 2 == 0 ? "integerp" : "floatp";
  return MakeUnary(ctx, name, BuildExpression(ctx, input, depth + 1));
}

// Sub-plan 06C's catch/throw, bounded so the grammar's nontermination bound
// holds: `throw` terminates paths early rather than extending them, so it
// appears only inside a catch builder and only as that catch's single body
// form -- the shape that drives the mid-stack unwind most directly. The tag
// comes from a small fixed set (two interned symbols -- eq by identity -- or
// an integer, eq by value), and the throw reuses the catch's tag (the match
// path, whose fresh-allocated-value rooting the forced-GC lanes chase) or a
// deliberately different one (the no-catch error path). The thrown value is
// a full expression, so a collection forced while it evaluates finds the
// delivery still rooted.
static FeObject* BuildCatchThrow(FeContext* ctx,
                                 FuzzInput* input,
                                 unsigned depth) {
  FeObject* tag;
  if (FuzzTakeByte(input) % 2 == 0) {
    tag = MakeUnary(
        ctx, "quote",
        FeMakeSymbol(ctx, FuzzTakeByte(input) % 2 == 0 ? "tg0" : "tg1"));
  } else {
    tag = BuildNumber(ctx, input);
  }
  FeObject* thrown_tag = tag;
  if (FuzzTakeByte(input) % 4 == 0) {
    thrown_tag = MakeUnary(ctx, "quote", FeMakeSymbol(ctx, "tg0"));
  }
  FeObject* value = BuildExpression(ctx, input, depth + 1);
  FeObject* body = MakeBinary(ctx, "throw", thrown_tag, value);
  // What sits between the throw and its catch is the whole point of the
  // mid-stack unwind, and the original grammar put nothing there: the throw
  // was the catch's only body form, so the drain always had zero frames and
  // zero cleanup entries to walk. These five arms make the gap real -- plain
  // evaluator frames the unwind must discard, an `unwind-protect` whose
  // cleanup must run on the way past, and (the 06D policy) a cleanup that
  // itself throws, which has to be re-issued in the enclosing context
  // because its catch lies below the cleanup run's own floor.
  switch (FuzzTakeByte(input) % 5) {
    case 0:
      break;
    case 1:
      // Argument frames above the throw.
      body = MakeForm(
          ctx, "list",
          (FeObject*[]){BuildExpression(ctx, input, depth + 1), body}, 2);
      break;
    case 2:
      // A branch frame, so the throw unwinds out of a partially evaluated
      // special form rather than out of a call.
      body = MakeForm(ctx, "if",
                      (FeObject*[]){FeMakeSymbol(ctx, "t"), body,
                                    BuildExpression(ctx, input, depth + 1)},
                      3);
      break;
    case 3:
      // One cleanup entry between the throw and the catch.
      body = MakeBinary(ctx, "unwind-protect", body,
                        BuildBindingForm(ctx, input, depth + 1, "setq"));
      break;
    default: {
      // A cleanup that throws while a throw is already unwinding: the
      // second one replaces the first if its tag matches a live catch, and
      // is `no-catch` otherwise.
      FeObject* cleanup = MakeBinary(ctx, "throw", thrown_tag,
                                     BuildExpression(ctx, input, depth + 1));
      body = MakeBinary(ctx, "unwind-protect", body, cleanup);
      break;
    }
  }
  return MakeForm(ctx, "catch", (FeObject*[]){tag, body}, 2);
}

// The condition symbols the grammar signals and handles, all registered in
// fe's static hierarchy so `signal` accepts them; `quit` is here because it
// is the one condition an `(error ...)` handler must *not* catch, and
// `no-catch` because the catch/throw arm produces it for real.
static const char* const condition_names[] = {
    "arith-error", "wrong-type-argument", "void-variable", "no-catch", "quit",
    "error",
};

static const char* PickCondition(FuzzInput* input) {
  return condition_names[FuzzTakeByte(input) % (sizeof(condition_names) /
                                                sizeof(condition_names[0]))];
}

// One handler clause: `(SPEC BODY)`, where SPEC is a symbol, a two-element
// list of symbols, or `t`. The original grammar emitted one shape only --
// `(arith-error BODY)` against a signalled `arith-error` -- so the matcher
// was fuzzed on its always-true path alone: the list arm, the `t` arm, the
// hierarchy walk and the unmatched re-signal were all unreachable by
// construction.
static FeObject* BuildHandlerClause(FeContext* ctx,
                                    FuzzInput* input,
                                    unsigned depth) {
  FeObject* spec;
  switch (FuzzTakeByte(input) % 4) {
    case 0:
      spec = FeMakeSymbol(ctx, "t");
      break;
    case 1:
      spec = FeMakeList(ctx,
                        (FeObject*[]){FeMakeSymbol(ctx, PickCondition(input)),
                                      FeMakeSymbol(ctx, PickCondition(input))},
                        2);
      break;
    default:
      spec = FeMakeSymbol(ctx, PickCondition(input));
      break;
  }
  return FeMakeList(
      ctx, (FeObject*[]){spec, BuildExpression(ctx, input, depth + 1)}, 2);
}

static FeObject* BuildConditionCase(FeContext* ctx,
                                    FuzzInput* input,
                                    unsigned depth) {
  // The variable: nil (no binding) or `e`, which the handler body may then
  // read, so the binding and the condition object it holds are live.
  FeObject* variable = &nil;
  if (FuzzTakeByte(input) % 2 == 0) {
    variable = FeMakeSymbol(ctx, "e");
  }
  // The body: a `signal` of an arbitrary registered condition (so the
  // handler often does *not* match and the condition re-signals past this
  // frame), or an arbitrary expression, which reaches the real raise sites.
  FeObject* body;
  if (FuzzTakeByte(input) % 2 == 0) {
    body = MakeBinary(
        ctx, "signal",
        MakeUnary(ctx, "quote", FeMakeSymbol(ctx, PickCondition(input))),
        MakeUnary(ctx, "quote", BuildDatum(ctx, input, depth + 1)));
  } else {
    body = BuildExpression(ctx, input, depth + 1);
  }
  FeObject* first = BuildHandlerClause(ctx, input, depth);
  if (FuzzTakeByte(input) % 2 == 0) {
    return MakeForm(ctx, "condition-case", (FeObject*[]){variable, body, first},
                    3);
  }
  // Two clauses: handler selection is textual order among matching specs,
  // and a clause that never matches has to be walked past.
  FeObject* second = BuildHandlerClause(ctx, input, depth);
  return MakeForm(ctx, "condition-case",
                  (FeObject*[]){variable, body, first, second}, 4);
}

// `error`'s format-string parser and its fixed 1024-byte message buffer had
// no fuzz coverage at all, which made them the most attack-shaped new code
// in the phase with the least evidence behind it. The format strings are a
// fixed set rather than fuzzer bytes on purpose: the interesting states are
// the directive parser's, not the alphabet's -- every supported directive,
// the escape, a directive at the very end of the string with no letter
// after it, an unsupported letter, and a run of directives long enough that
// the rendered result presses the message buffer. Arguments are ordinary
// expressions, so a directive can meet any value the rest of the grammar can
// build, including one whose rendering is far longer than the directive.
static const char* const error_formats[] = {
    "",
    "plain",
    "%%",
    "%s",
    "%S",
    "%d",
    "%s %S %d",
    "%",
    "%x",
    "a%sb%Sc%dd",
    "%s%s%s%s%s%s%s%s",
    "%S%S%S%S%S%S%S%S",
};

static FeObject* BuildErrorForm(FeContext* ctx,
                                FuzzInput* input,
                                unsigned depth) {
  FeObject* format = FeMakeString(
      ctx, error_formats[FuzzTakeByte(input) %
                         (sizeof(error_formats) / sizeof(error_formats[0]))]);
  FeObject* arguments[4];
  arguments[0] = format;
  const size_t count = FuzzTakeByte(input) % 3;
  for (size_t i = 0; i < count; i++) {
    arguments[i + 1] = BuildExpression(ctx, input, depth + 1);
  }
  FeObject* form = MakeForm(ctx, "error", arguments, count + 1);
  if (FuzzTakeByte(input) % 2 == 0) {
    // Caught, so the run continues past it and the same input keeps
    // building forms; the handler also exercises the condition object the
    // formatted message was stored in.
    FeObject* handler =
        FeMakeList(ctx,
                   (FeObject*[]){FeMakeSymbol(ctx, "error"),
                                 BuildExpression(ctx, input, depth + 1)},
                   2);
    return MakeForm(ctx, "condition-case",
                    (FeObject*[]){FeMakeSymbol(ctx, "e"), form, handler}, 3);
  }
  return form;
}

// Sub-plan 09B's exhaustion-under-condition-case shape. Nothing else in this
// grammar can fill the arena: every generated form is bounded by `MaxDepth`,
// and the harness restores the GC stack between forms, so all of it is
// collectable again by the next one. Measured before this builder existed:
// **zero** arena-exhaustion raises across 600 random inputs of 32..512 bytes
// and one 4096-byte input -- the whole catchable-exhaustion path 09B builds
// was structurally unreachable from this lane, which is what 09A Decision 6
// asks this slice to fix.
//
// The body is `(let ((l nil)) (while t (setq l (cons 1 l))))`: a loop that
// terminates only by exhausting the arena. That does not breach the
// harness's standing "no nontermination" bound, because the arena is a fixed
// `FuzzArenaSize` and the loop is therefore bounded by the number of object
// slots it holds -- a few hundred iterations, not by the input -- and by the
// harness's own `MaxEvaluationSteps` besides. The chain is a `let`-local, so
// it is unreachable the moment a handler frame is entered and a handler that
// allocates can still run.
//
// The handler spec is drawn from all four interesting answers: `t` (the only
// one that caught before 09B), `error` (the parent in the hierarchy),
// `arena-exhaustion` (the name itself), and `arith-error` (a name that must
// still *not* match, so the escape path is generated too).
static FeObject* BuildExhaustionForm(FeContext* ctx,
                                     FuzzInput* input,
                                     unsigned depth) {
  static const char* const specs[] = {"t", "error", "arena-exhaustion",
                                      "arith-error"};
  FeObject* variable = &nil;
  if (FuzzTakeByte(input) % 2 == 0) {
    variable = FeMakeSymbol(ctx, "e");
  }
  FeObject* bindings =
      FeMakeList(ctx,
                 (FeObject*[]){FeMakeList(
                     ctx, (FeObject*[]){FeMakeSymbol(ctx, "l"), &nil}, 2)},
                 1);
  FeObject* push = MakeBinary(
      ctx, "setq", FeMakeSymbol(ctx, "l"),
      MakeBinary(ctx, "cons", FeMakeInteger(ctx, 1), FeMakeSymbol(ctx, "l")));
  FeObject* loop = MakeBinary(ctx, "while", FeMakeSymbol(ctx, "t"), push);
  FeObject* body = MakeBinary(ctx, "let", bindings, loop);
  FeObject* handler = FeMakeList(
      ctx,
      (FeObject*[]){
          FeMakeSymbol(
              ctx,
              specs[FuzzTakeByte(input) % (sizeof(specs) / sizeof(specs[0]))]),
          BuildExpression(ctx, input, depth + 1)},
      2);
  return MakeForm(ctx, "condition-case", (FeObject*[]){variable, body, handler},
                  3);
}

// Sub-plan 09C's mark-phase shapes. The grammar's own recursion is capped at
// `MaxDepth` and `BuildMutation` deliberately keeps `setcar`/`setcdr` acyclic,
// so before this arm the collector was only ever asked, from this lane, to
// walk shallow acyclic graphs -- the two shapes 09C's rewrite is *about*, a
// deep `car` spine and a cycle, were unreachable by construction.
//
// The generated form grows a `car` chain to an input-chosen depth, optionally
// closes it into a cycle with `setcdr`, and then allocates enough garbage over
// the top of it to force at least one collection while it is live:
//
//   (let ((d nil) (i 0))
//     (while (< i LEVELS) (do (setq i (+ i 1)) (setq d (cons d nil))))
//     [(setcdr d d)]
//     (while (< i CHURN) (do (setq i (+ i 1)) (cons i i)))
//     i)
//
// Both loops are bounded by constants baked into the form, not by the input:
// `LEVELS` is one byte and `CHURN` a fixed count, so this respects the
// harness's termination bound the same way the 09B exhaustion arm does. The
// chain is a `let`-local and the form returns a number, so nothing deep or
// cyclic reaches the harness's own `FeToString`.
static FeObject* BuildDeepGraph(FeContext* ctx, FuzzInput* input) {
  enum { MinimumLevels = 8, ChurnAllocations = 250 };
  const int64_t levels = MinimumLevels + FuzzTakeByte(input) % 200;
  const bool cyclic = FuzzTakeByte(input) % 2 == 0;
  FeObject* const d = FeMakeSymbol(ctx, "d");
  FeObject* const i = FeMakeSymbol(ctx, "i");
  FeObject* const bindings = FeMakeList(
      ctx,
      (FeObject*[]){
          FeMakeList(ctx, (FeObject*[]){d, &nil}, 2),
          FeMakeList(ctx, (FeObject*[]){i, FeMakeInteger(ctx, 0)}, 2)},
      2);
  FeObject* const step = MakeBinary(
      ctx, "setq", i, MakeBinary(ctx, "+", i, FeMakeInteger(ctx, 1)));
  FeObject* const grow = MakeBinary(
      ctx, "while", MakeBinary(ctx, "<", i, FeMakeInteger(ctx, levels)),
      MakeBinary(ctx, "do", step,
                 MakeBinary(ctx, "setq", d, MakeBinary(ctx, "cons", d, &nil))));
  FeObject* const churn = MakeBinary(
      ctx, "while",
      MakeBinary(ctx, "<", i, FeMakeInteger(ctx, levels + ChurnAllocations)),
      MakeBinary(ctx, "do", step, MakeBinary(ctx, "cons", i, i)));
  FeObject* items[6];
  size_t count = 0;
  items[count++] = FeMakeSymbol(ctx, "let");
  items[count++] = bindings;
  items[count++] = grow;
  if (cyclic) {
    items[count++] = MakeBinary(ctx, "setcdr", d, d);
  }
  items[count++] = churn;
  items[count++] = i;
  return FeMakeList(ctx, items, count);
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
  const uint8_t target_choice = FuzzTakeByte(input) % 4;
  FeObject* target = target_choice == 0   ? FeMakeSymbol(ctx, "t")
                     : target_choice == 1 ? FeMakeSymbol(ctx, ":fuzz-keyword")
                     : target_choice == 2 ? &nil
                                          : FeMakeSymbol(ctx, "x");
  FeObject* assign = MakeBinary(ctx, binding, target, value);
  return MakeBinary(ctx, "do", assign, FeMakeSymbol(ctx, "x"));
}

static FeObject* BuildFunctionCall(FeContext* ctx,
                                   FuzzInput* input,
                                   unsigned depth) {
  FeObject* parameter = FeMakeSymbol(ctx, "x");
  FeObject* parameters = BuildLambdaParameters(ctx, input);
  FeObject* body = MakeBinary(ctx, "cons", parameter,
                              BuildExpression(ctx, input, depth + 1));
  FeObject* function = MakeForm(ctx, "fn", (FeObject*[]){parameters, body}, 2);
  // Written in place and read only up to `count`, the shape every other
  // builder here uses: the previous version copied `arguments[0..2]` into
  // `items` unconditionally, reading indeterminate pointers whenever the
  // call had fewer than three arguments.
  FeObject* items[4];
  items[0] = function;
  const size_t count = FuzzTakeByte(input) % 4;
  for (size_t i = 0; i < count; i++) {
    items[i + 1] = BuildExpression(ctx, input, depth + 1);
  }
  return FeMakeList(ctx, items, count + 1);
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
  FeObject* parameters = BuildLambdaParameters(ctx, input);
  FeObject* body = BuildMacroBody(ctx, input, parameter);
  FeObject* macro = MakeForm(ctx, "macro", (FeObject*[]){parameters, body}, 2);
  // Same in-place construction as `BuildFunctionCall`, for the same reason.
  FeObject* items[4];
  items[0] = macro;
  const size_t count = FuzzTakeByte(input) % 4;
  for (size_t i = 0; i < count; i++) {
    items[i + 1] = BuildDatum(ctx, input, depth + 1);
  }
  return FeMakeList(ctx, items, count + 1);
}

// 07B item 9's "primitive fixed/minimum over/under-arity forms", plus the
// host-native helpers that read the per-call `(FUNCTION NARGS)` record. The
// name set spans every arity shape the table has -- exact one (`car`, `cdr`,
// `not`, `quote`), exact two (`cons`, `eq`), minimum (`if`) -- and
// `native-arity` is the registered native that consumes one argument and
// rejects the rest, so `FeGetNextArgument`'s "too few arguments" and
// `FeRequireNoArguments`'s "too many arguments" are both reachable from the
// grammar. Nothing here writes to stdout or can fail to terminate, so the
// harness's standing bounds hold.
static FeObject* BuildArityForm(FeContext* ctx,
                                FuzzInput* input,
                                unsigned depth) {
  static const char* const names[] = {"car",   "cdr", "cons", "not",
                                      "quote", "if",  "eq",   "native-arity"};
  FeObject* items[4];
  items[0] = FeMakeSymbol(
      ctx, names[FuzzTakeByte(input) % (sizeof(names) / sizeof(names[0]))]);
  const size_t count = FuzzTakeByte(input) % 4;
  for (size_t i = 0; i < count; i++) {
    items[i + 1] = BuildExpression(ctx, input, depth + 1);
  }
  return FeMakeList(ctx, items, count + 1);
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
  if (depth >= MaxDepth) {
    return BuildAtom(ctx, input);
  }

  switch (FuzzTakeByte(input) % 36) {
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
    case 9:
      return BuildNumericExpression(ctx, input, depth + 1);
    case 10:
      return BuildComparisonForm(ctx, input);
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
      // `setq` since 02C: `=` is numeric equality, so the grammar's binding
      // arm uses the real assignment special form rather than the always-
      // wrong-type-argument `(= x value)` the pre-cut grammar generated.
      return BuildBindingForm(ctx, input, depth, "setq");
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
    case 25:
      return BuildPredicateForm(ctx, input, depth + 1);
    case 26:
      return BuildIdentityForm(ctx, input, depth + 1);
    case 27:
      return BuildIdentityForm(ctx, input, depth + 1);
    case 28:
      return BuildComparisonForm(ctx, input);
    case 29:
      return BuildCatchThrow(ctx, input, depth);
    case 30:
      return BuildConditionCase(ctx, input, depth);
    case 31:
      return BuildErrorForm(ctx, input, depth);
    case 32:
      return BuildArityForm(ctx, input, depth);
    case 33:
      return BuildExhaustionForm(ctx, input, depth);
    case 34:
      return BuildDeepGraph(ctx, input);
    default:
      return BuildNumericExpression(ctx, input, depth + 1);
  }
}

// The grammar's one host native: it takes exactly one argument through the
// public helpers, so both of them raise from the fuzz lane and the arity
// record they read is exercised under the harness's forced collections.
static FeObject* FuzzArityNative(FeContext* ctx,
                                 // cppcheck-suppress constParameterCallback
                                 FeObject* arguments) {
  FeObject* const first = FeGetNextArgument(ctx, &arguments);
  FeRequireNoArguments(ctx, arguments);
  return first;
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
    FeDefineNative(ctx, "native-arity", FuzzArityNative);
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
