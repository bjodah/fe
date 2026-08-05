// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.141592653589793
#endif
#ifndef M_E
#define M_E 2.718281828459045
#endif
#include <stdarg.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

#include "fe.h"
#include "fe_internal.h"

const char* FeVersion = "5.0";

#define COUNT(a) (sizeof((a)) / sizeof((a)[0]))

static const char* primitive_names[] = {[PAssert] = "assert",
                                        [PEnv] = "env",
                                        [PLet] = "let",
                                        [PNumericEqual] = "=",
                                        [PSetq] = "setq",
                                        [PSet] = "set",
                                        [PIf] = "if",
                                        [PFn] = "lambda",
                                        [PMacro] = "macro",
                                        [PWhile] = "while",
                                        [PQuote] = "quote",
                                        [PBoundp] = "boundp",
                                        [PMakeUnbound] = "makunbound",
                                        [PAnd] = "and",
                                        [POr] = "or",
                                        [PDo] = "do",
                                        [PUnwindProtect] = "unwind-protect",
                                        [PCons] = "cons",
                                        [PCar] = "car",
                                        [PCdr] = "cdr",
                                        [PSetCar] = "setcar",
                                        [PSetCdr] = "setcdr",
                                        [PList] = "list",
                                        [PNot] = "not",
                                        [PIs] = "is",
                                        [PEq] = "eq",
                                        [PEql] = "eql",
                                        [PAtom] = "atom",
                                        [PPrint] = "print",
                                        [PLess] = "<",
                                        [PLessEqual] = "<=",
                                        [PGreater] = ">",
                                        [PGreaterEqual] = ">=",
                                        [PNotEqual] = "/=",
                                        [PIntegerp] = "integerp",
                                        [PFloatp] = "floatp",
                                        [PAdd] = "+",
                                        [PSub] = "-",
                                        [PMul] = "*",
                                        [PDiv] = "/",
                                        [PFunction] = "function",
                                        [PFset] = "fset",
                                        [PDefalias] = "defalias",
                                        [PSymbolFunction] = "symbol-function",
                                        [PSymbolValue] = "symbol-value",
                                        [PFboundp] = "fboundp",
                                        [PFmakunbound] = "fmakunbound",
                                        [PFuncall] = "funcall",
                                        [PApply] = "apply"};

typedef struct PrimitiveAlias {
  const char* name;
  Primitive primitive;
} PrimitiveAlias;

// Extra global names bound to the same primitive object as their canonical
// spelling. `fn` is Fe's historical name for `lambda`.
static const PrimitiveAlias primitive_aliases[] = {
    {"fn", PFn},
};

const char* type_names[] = {
    [FeTPair] = "pair",
    [FeTFree] = "free",
    [FeTNil] = "nil",
    [FeTDouble] = "double",
    [FeTInteger] = "integer",
    [FeTSymbol] = "symbol",
    [FeTString] = "string",
    [FeTFn] = "lambda",
    [FeTMacro] = "macro",
    [FeTPrimitive] = "primitive",
    [FeTNativeFn] = "native-fn",
    [FeTPtr] = "ptr",
    [FeTFex0] = "fex0",
    [FeTFex1] = "fex1",
    [FeTFex2] = "fex2",
};

FeObject nil = {.car = {.c = FeTNil << GcMarkBit | OtherCell},
                .cdr = {.o = NULL}};

// The value of a symbol that has never been assigned. Like `nil` it is a
// static object outside the arena, so the collector neither sweeps it nor has
// to mark it, and `FeMark` treats it as a leaf. It is never returned to Lisp or
// to a host: the only place it lives is a fresh symbol's value cell and
// function cell (`CDR(sym)` is `((name . function) . value)`, sub-plan 04B),
// which Lisp cannot reach (`(cdr sym)` is a type error) and which every
// reader of either cell turns into `void-variable`/`void-function`. It is
// tagged `FeTFree` so that an escape aborts in the writer instead of
// impersonating a value.
FeObject unbound = {.car = {.c = FeTFree << GcMarkBit | OtherCell},
                    .cdr = {.o = NULL}};

FeDouble GetDouble(const FeObject* o) {
  return o->cdr.n;
}

FeNativeFn* GetNativeFn(const FeObject* o) {
  return o->cdr.f;
}

void SetType(FeObject* o, FeType type) {
  o->car.c = (char)((type) << GcMarkBit | OtherCell);
}

typedef struct FeArena {
  FeContext context;
} FeArena;

static_assert(sizeof(FeArena) == sizeof(FeContext));
static_assert(alignof(FeArena) >= alignof(FeContext));
static_assert(alignof(FeArena) >= alignof(FeEvalFrame));
static_assert(alignof(FeArena) >= alignof(FeObject));
static_assert(FeTFex0 > FeTPtr, "FeTFex* must be > FeTPtr");

void FeSetUserData(FeContext* ctx, void* userdata) {
  ctx->userdata = userdata;
}

void* FeGetUserData(const FeContext* ctx) {
  return ctx->userdata;
}

void FeSetErrorFn(FeContext* ctx, FeErrorFn* fn) {
  ctx->error_fn = fn;
}

void FeSetMarkFn(FeContext* ctx, FeNativeFn* fn) {
  ctx->mark_fn = fn;
}

void FeSetGCFn(FeContext* ctx, FeNativeFn* fn) {
  ctx->gc_fn = fn;
}

void FeSetStrictArity(FeContext* ctx, bool strict) {
  ctx->strict_arity = strict;
}

bool FeGetStrictArity(const FeContext* ctx) {
  return ctx->strict_arity;
}

void __attribute((format(printf, 3, 4))) Format(char* result,
                                                size_t size,
                                                const char* format,
                                                ...) {
  assert(size < INT_MAX);
  assert(size > 0);
  assert(result != NULL);
  assert(format != NULL);

  va_list arguments;
  va_start(arguments, format);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
  const int count = vsnprintf(result, size, format, arguments);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  va_end(arguments);
  assert(count > 0);
  assert(count < INT_MAX);
}

FeObject* FeGetNextArgument(FeContext* ctx, FeObject** arg) {
  FeObject* a = *arg;
  if (FeGetType(a) != FeTPair) {
    if (FeIsNil(a)) {
      FeHandleError(ctx, "too few arguments");
    }
    FeHandleError(ctx, "dotted pair in argument list");
  }
  *arg = CDR(a);
  return CAR(a);
}

static const char* GetTypeName(FeType type) {
  return type < COUNT(type_names) ? type_names[type] : "unknown";
}

FeObject* CheckType(FeContext* ctx, FeObject* obj, FeType type) {
  if (FeGetType(obj) != type) {
    char message[64];
    Format(message, sizeof(message), "expected %s, got %s", GetTypeName(type),
           GetTypeName(FeGetType(obj)));
    FeHandleError(ctx, message);
  }
  return obj;
}

void FeRequireNoArguments(FeContext* ctx, const FeObject* args) {
  if (!FeIsNil(args)) {
    FeHandleError(ctx, "too many arguments");
  }
}

FeType FeGetType(const FeObject* obj) {
  return (FeType)(TAG(obj) & OtherCell ? TAG(obj) >> GcMarkBit : FeTPair);
}

bool FeIsNil(const FeObject* obj) {
  return obj == &nil;
}

FeObject* FeNil(FeContext*) {
  return &nil;
}

void FePushGC(FeContext* ctx, FeObject* obj) {
  if (ctx->gc_stack_index == GcStackSize) {
    FeHandleError(ctx, "GC stack overflow");
  }
  ctx->gc_stack[ctx->gc_stack_index++] = obj;
  if (ctx->gc_stack_index > ctx->arena_peak_gc_stack_depth) {
    ctx->arena_peak_gc_stack_depth = ctx->gc_stack_index;
  }
}

void FeRestoreGC(FeContext* ctx, size_t index) {
  ctx->gc_stack_index = index;
}

size_t FeSaveGC(const FeContext* ctx) {
  return ctx->gc_stack_index;
}

void FeMark(FeContext* ctx, FeObject* obj) {
  FeObject* car;
begin:
  if (TAG(obj) & GcMarkBit) {
    return;
  }
  car = CAR(obj);  // Store car before modifying it with GcMarkBit
  TAG(obj) |= GcMarkBit;

  switch (FeGetType(obj)) {
    case FeTPair:
      FeMark(ctx, car);
      // fall through
    case FeTFn:
    case FeTMacro:
    case FeTSymbol:
    case FeTString:
      obj = CDR(obj);
      goto begin;

    case FeTPtr:
    case FeTFex0:
    case FeTFex1:
    case FeTFex2:
      if (ctx->mark_fn) {
        ctx->mark_fn(ctx, obj);
      }
      break;

    case FeTFree:
    case FeTNil:
    case FeTDouble:
    case FeTInteger:
    case FeTPrimitive:
    case FeTNativeFn:
      // Do nothing.
      break;

    case FeTSentinel:
      abort();
  }
}

static void MarkCleanupRoots(FeContext* ctx) {
  for (size_t i = 0; i < ctx->cleanup_stack_index; i++) {
    const FeCleanupEntry* entry = &ctx->cleanup_stack[i];
    if (entry->kind == FeCleanupLisp) {
      FeMark(ctx, entry->as.lisp.forms);
      FeMark(ctx, entry->as.lisp.env);
    }
  }
}

static void CollectGarbage(FeContext* ctx) {
  ctx->arena_collection_count++;
  // Mark:
  for (size_t i = 0; i < ctx->gc_stack_index; i++) {
    FeMark(ctx, ctx->gc_stack[i]);
  }
  FeMark(ctx, ctx->symbol_list);
  FeMark(ctx, ctx->evaluation_result);
  FeMark(ctx, ctx->call_result);
  FeMark(ctx, ctx->root_list);
  FeMarkEvaluatorRoots(ctx);
  MarkCleanupRoots(ctx);

  // Sweep and unmark:
  for (size_t i = 0; i < ctx->object_count; i++) {
    FeObject* obj = &ctx->objects[i];
    if (FeGetType(obj) == FeTFree) {
      continue;
    }
    if (~TAG(obj) & GcMarkBit) {
      if (ctx->gc_fn != nullptr) {
        ctx->gc_fn(ctx, obj);
      }
      SetType(obj, FeTFree);
      CDR(obj) = ctx->free_list;
      ctx->free_list = obj;
      ctx->arena_live_count--;
    } else {
      TAG(obj) &= ~GcMarkBit;
    }
  }
}

// Translated from [the original
// Java](https://floating-point-gui.de/errors/comparison/).
//
// See also
// https://randomascii.wordpress.com/2012/02/25/comparing-floating-point-numbers-2012-edition/.
static bool IsNearlyEqual(double a, double b, double epsilon) {
  const double absA = fabs(a);
  const double absB = fabs(b);
  const double diff = fabs(a - b);
// It's OK to turn this warning off for this limited section of code, because
// it's in the context of handling tiny errors.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
  if (a == b) {
    // Special case; handles infinities.
    return true;
  } else if (a == 0 || b == 0 || (absA + absB < DBL_MIN)) {
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    // Either a or b is zero, or both are extremely close to it. Relative error
    // is less meaningful here.
    return diff < (epsilon * DBL_MIN);
  }
  // Use relative error.
  return diff / fmin((absA + absB), DBL_MAX) < epsilon;
}

// `is`'s cross-type number arm (05A Decision 2, 05C): mathematical value
// across an integer and a double, preserving `(is 1 1.0)` -> t. The int64
// converts exactly to double up to 2^53; beyond that the comparison is the
// value the two numbers share in doubles, the same approximation the
// arithmetic tower's mixed promotion uses. `-Wfloat-equal` is suppressed for
// this intentional exact comparison, the way `IsNearlyEqual`'s own `a == b`
// infinity special case is below.
static bool IntegerAndDoubleEqual(int64_t i, double d) {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
  const bool equal = (FeDouble)i == d;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  return equal;
}

bool Equal(FeObject* a, FeObject* b) {
  if (a == b) {
    return true;
  }
  const FeType a_type = FeGetType(a);
  const FeType b_type = FeGetType(b);
  if (a_type == FeTInteger && b_type == FeTInteger) {
    // 05A Decision 2 (05C): exact within integers.
    return INTEGER(a) == INTEGER(b);
  }
  if (a_type == FeTInteger && b_type == FeTDouble) {
    return IntegerAndDoubleEqual(INTEGER(a), GetDouble(b));
  }
  if (a_type == FeTDouble && b_type == FeTInteger) {
    return IntegerAndDoubleEqual(INTEGER(b), GetDouble(a));
  }
  if (a_type != b_type) {
    return false;
  }
  if (a_type == FeTDouble) {
    return IsNearlyEqual(GetDouble(a), GetDouble(b), DBL_EPSILON);
  } else if (a_type == FeTString) {
    for (; !FeIsNil(a); a = CDR(a), b = CDR(b)) {
      if (CAR(a) != CAR(b)) {
        return false;
      }
    }
    return a == b;
  }
  return false;
}

// Same-type doubles equal by their exact bits (05A Decision 2, rows E4-E5,
// landed 05D): `(eql 0.0 -0.0)` is nil because the sign bit differs, the one
// distinction IEEE `==` folds away. `memcmp` rather than `==`, and `!` on the
// result, because the comparison is exactly what must not be `==`-semantics.
static bool DoublesEqualByBits(double a, double b) {
  return !memcmp(&a, &b, sizeof(a));
}

// `eq`/`eql`'s shared answer (rows E1-E5): pointer identity -- the Emacs rule
// for symbols, strings, and boxed floats -- or both-integers-equal, the
// fixnum rule Emacs' `(eq 3 3)` depends on. Two separately-read `3.0` literals
// are two float objects, so `(eq 3.0 3.0)` is nil; two `3` literals are two
// integer objects but answer t. `eql` (`compare_floats`) adds same-type
// floats equal by bits -- type-strict value equality, so `(eql 3 3.0)` is nil
// (different types).
bool IdentityObjects(FeObject* a, FeObject* b, bool compare_floats) {
  if (a == b) {
    return true;
  }
  const FeType type = FeGetType(a);
  if (type != FeGetType(b)) {
    return false;
  }
  if (type == FeTInteger) {
    return INTEGER(a) == INTEGER(b);
  }
  if (type == FeTDouble) {
    return compare_floats && DoublesEqualByBits(GetDouble(a), GetDouble(b));
  }
  return false;
}

static int IsStringEqual(FeObject* obj, const char* str) {
  while (!FeIsNil(obj)) {
    for (size_t i = 0; i < StringBufferSize; i++) {
      if (STRING_BUFFER(obj)[i] != *str) {
        return 0;
      }
      if (*str) {
        str++;
      }
    }
    obj = CDR(obj);
  }
  return *str == '\0';
}

FeObject* MakeObject(FeContext* ctx) {
  // Run GC if free_list has no more objects:
  if (FeIsNil(ctx->free_list)) {
    CollectGarbage(ctx);
    if (FeIsNil(ctx->free_list)) {
      ctx->arena_allocation_failures++;
      FeHandleError(ctx, "out of memory");
    }
  }
  // Get object from free_list and push it onto the GC stack:
  FeObject* obj = ctx->free_list;
  ctx->free_list = CDR(obj);
  ctx->arena_live_count++;
  if (ctx->arena_live_count > ctx->arena_peak_live_count) {
    ctx->arena_peak_live_count = ctx->arena_live_count;
  }
  FePushGC(ctx, obj);
  return obj;
}

FeObject* FeCons(FeContext* ctx, FeObject* car, FeObject* cdr) {
  FeObject* obj = MakeObject(ctx);
  CAR(obj) = car;
  CDR(obj) = cdr;
  return obj;
}

FeObject* FeMakeBool(FeContext* ctx, bool b) {
  return b ? ctx->t : &nil;
}

FeObject* FeMakeDouble(FeContext* ctx, FeDouble n) {
  FeObject* obj = MakeObject(ctx);
  SetType(obj, FeTDouble);
  DOUBLE(obj) = n;
  return obj;
}

FeObject* FeMakeInteger(FeContext* ctx, int64_t n) {
  FeObject* obj = MakeObject(ctx);
  SetType(obj, FeTInteger);
  INTEGER(obj) = n;
  return obj;
}

static FeObject* BuildString(FeContext* ctx, FeObject* tail, char chr) {
  if (!tail || STRING_BUFFER(tail)[StringBufferSize - 1] != '\0') {
    FeObject* obj = FeCons(ctx, NULL, &nil);
    SetType(obj, FeTString);
    if (tail) {
      CDR(tail) = obj;
      ctx->gc_stack_index--;
    }
    tail = obj;
  }
  STRING_BUFFER(tail)[strlen(STRING_BUFFER(tail))] = chr;
  return tail;
}

FeObject* FeMakeString(FeContext* ctx, const char* str) {
  FeObject* obj = BuildString(ctx, NULL, '\0');
  FeObject* tail = obj;
  while (*str) {
    tail = BuildString(ctx, tail, *str++);
  }
  return obj;
}

FeObject* FeMakeSymbol(FeContext* ctx, const char* name) {
  FeObject* obj;
  // Try to find in symbol_list:
  for (obj = ctx->symbol_list; !FeIsNil(obj); obj = CDR(obj)) {
    if (IsStringEqual(SymbolName(CAR(obj)), name)) {
      return CAR(obj);
    }
  }
  // Create new object, push to symbol_list and return. A symbol's cdr is one
  // cons: `((name . function) . value)` (sub-plan 04B). The function cell is
  // initialized to `&unbound` and written only through
  // `SetSymbolFunction`; the name moves one pair down so the binding cell --
  // the cdr of the outer pair -- is unchanged, which is what keeps the whole
  // value path (`GetBound`, `FeSet`, `FeIsBound`, `ResumeSetq`) untouched.
  obj = MakeObject(ctx);
  SetType(obj, FeTSymbol);
  CDR(obj) =
      FeCons(ctx, FeCons(ctx, FeMakeString(ctx, name), &unbound), &unbound);
  ctx->symbol_list = FeCons(ctx, obj, ctx->symbol_list);
  return obj;
}

FeObject* FeMakeNativeFn(FeContext* ctx, FeNativeFn fn) {
  FeObject* obj = MakeObject(ctx);
  SetType(obj, FeTNativeFn);
  NATIVE_FN(obj) = fn;
  return obj;
}

void FeDefineNative(FeContext* ctx, const char* name, FeNativeFn* fn) {
  // Sub-plan 04D (FE_API_VERSION 3): the function cell, not the value cell.
  // Call position resolves the function cell only since the cut, so a native
  // registered here is reachable as `(name ...)`; `(boundp 'name)` is nil.
  const size_t gc = FeSaveGC(ctx);
  FeObject* symbol = FeMakeSymbol(ctx, name);
  FeObject* native = FeMakeNativeFn(ctx, fn);
  SetSymbolFunction(symbol, native);
  FeRestoreGC(ctx, gc);
}

FeObject* FeMakePtr(FeContext* ctx, FeType type, void* ptr) {
  FeObject* obj = MakeObject(ctx);
  SetType(obj, type);
  CDR(obj) = ptr;
  return obj;
}

FeObject* FeMakeList(FeContext* ctx, FeObject** objs, size_t n) {
  FeObject* res = &nil;
  while (n--) {
    res = FeCons(ctx, objs[n], res);
  }
  return res;
}

static FeObject* GetCar(const FeObject* pair) {
  return CAR(pair);
}

static FeObject* GetCdr(const FeObject* pair) {
  return CDR(pair);
}

static FeObject* GetPairMember(FeContext* ctx,
                               FeObject* obj,
                               FeObject* (*member)(const FeObject*)) {
  if (FeIsNil(obj)) {
    return obj;
  }
  const FeObject* pair = CheckType(ctx, obj, FeTPair);
  return member(pair);
}

FeObject* FeCar(FeContext* ctx, FeObject* obj) {
  return GetPairMember(ctx, obj, GetCar);
}

FeObject* FeCdr(FeContext* ctx, FeObject* obj) {
  return GetPairMember(ctx, obj, GetCdr);
}

// Writer bounds. The spine cycle check terminates `(setcdr x x)` structurally,
// so these are backstops against shared structure that is finite but
// unreasonable, not the cycle defence.
enum {
  DefaultWriteMaxBytes = 64u << 20,
  DefaultWriteMaxNodes = 8u << 20,
  DefaultWriteMaxDepth = 256,
};

typedef struct Writer {
  FeContext* ctx;
  FeWriteFn* fn;
  void* udata;
  size_t bytes;
  size_t nodes;
  bool complete;
} Writer;

static void Emit(Writer* w, char chr) {
  if (w->bytes == 0) {
    w->complete = false;
    return;
  }
  w->bytes--;
  w->fn(w->ctx, w->udata, chr);
}

static void EmitString(Writer* w, const char* s) {
  while (*s) {
    Emit(w, *s++);
  }
}

// A string or a symbol's name. Never recurses: the cells are a cdr chain.
static void EmitStoredString(Writer* w, FeObject* obj, int qt) {
  if (qt) {
    Emit(w, '"');
  }
  while (!FeIsNil(obj)) {
    for (size_t i = 0; i < StringBufferSize && STRING_BUFFER(obj)[i]; i++) {
      if (qt && STRING_BUFFER(obj)[i] == '"') {
        Emit(w, '\\');
      }
      Emit(w, STRING_BUFFER(obj)[i]);
    }
    obj = CDR(obj);
  }
  if (qt) {
    Emit(w, '"');
  }
}

// Emacs' float spelling (05A Decision 4, 05D), byte for byte: nonfinite
// values get the `1.0e+INF`/`-1.0e+INF`/`0.0e+NaN`/`-0.0e+NaN` family, and
// finite values print via the shortest `%.*g` that `strtod` round-trips back
// to the same double -- Emacs starts at `DBL_DIG` and increments (gnulib's
// `dtoastr`), so a short value like `0.1` prints `0.1` while a long one like
// `(log 8)` prints all its digits. The post-pass Emacs' `float_to_string`
// runs is then reproduced exactly: the result must always contain a decimal
// point or an exponent, so a bare integer text gets `.0` appended and a
// trailing `100.` gets a `0` -- the old integral-double shortcut (a double
// that happened to be integral printing bare) dies with the cut, because a
// bare `42` is an integer now and `42.0` must print `42.0`.
static void EmitDouble(Writer* w, const FeObject* obj) {
  const double d = GetDouble(obj);
  char buf[40];
  if (isnan(d)) {
    // The sign bit is the only NaN payload fe ever produces (the canonical
    // `0.0/0.0` family), and it is what Emacs' own mantissa-printing spelling
    // reduces to for that payload; `(sqrt -1)` prints `-0.0e+NaN`.
    Format(buf, sizeof(buf), "%s0.0e+NaN", signbit(d) ? "-" : "");
    EmitString(w, buf);
    return;
  }
  if (isinf(d)) {
    Format(buf, sizeof(buf), "%s1.0e+INF", d < 0 ? "-" : "");
    EmitString(w, buf);
    return;
  }
  const double magnitude = fabs(d);
  int precision = magnitude < DBL_MIN ? 1 : DBL_DIG;
  for (; precision <= 17; precision++) {
    Format(buf, sizeof(buf), "%.*g", precision, d);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
    const bool round_trips = strtod(buf, nullptr) == d;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    if (round_trips) {
      break;
    }
  }
  // The decimal point must always be printed, or a float reads back as an
  // integer: `100` becomes `100.0`, `0.1` already carries its own point, and
  // an exponent (`1e+100`) already marks a float.
  if (strpbrk(buf, ".eE") == nullptr) {
    Format(buf + strlen(buf), sizeof(buf) - strlen(buf), ".0");
  }
  EmitString(w, buf);
}

static void WriteObject(Writer* w, FeObject* obj, int qt, size_t depth);

// The elements of the list `obj` (a pair) and its dotted tail, without the
// surrounding parentheses. The spine is walked iteratively -- only `car`
// nesting costs depth -- and Floyd's two pointers over it terminate a cdr
// cycle without a visited set and without allocating.
static void WriteElements(Writer* w, FeObject* obj, size_t depth) {
  FeObject* slow = obj;
  bool move_slow = false;
  while (true) {
    WriteObject(w, CAR(obj), 1, depth);
    FeObject* next = CDR(obj);
    if (FeGetType(next) != FeTPair) {
      if (!FeIsNil(next)) {
        EmitString(w, " . ");
        WriteObject(w, next, 1, depth);
      }
      return;
    }
    if (move_slow) {
      slow = CDR(slow);
    }
    move_slow = !move_slow;
    if (next == slow) {
      EmitString(w, " . #<cycle>");
      w->complete = false;
      return;
    }
    if (w->bytes == 0 || w->nodes == 0) {
      EmitString(w, " #<truncated>");
      w->complete = false;
      return;
    }
    obj = next;
    Emit(w, ' ');
  }
}

// A closure or macro prints as `(lambda PARAMS BODY...)`. This used to cons the
// head onto the body, which meant the writer allocated: printing could collect
// and raise `out of memory` part way through, longjmping out of whatever C
// caller was holding the destination buffer.
static void WriteClosure(Writer* w, FeObject* obj, size_t depth) {
  Emit(w, '(');
  EmitString(w, FeGetType(obj) == FeTFn ? "lambda" : "macro");
  obj = CDR(CDR(obj));
  if (FeGetType(obj) == FeTPair) {
    Emit(w, ' ');
    WriteElements(w, obj, depth);
  } else if (!FeIsNil(obj)) {
    EmitString(w, " . ");
    WriteObject(w, obj, 1, depth);
  }
  Emit(w, ')');
}

static void WriteObject(Writer* w, FeObject* obj, int qt, size_t depth) {
  char buf[32];
  // Printing is work: during a controlled evaluation it spends the step budget
  // and polls the interrupt, so `(print cyclic-thing)` answers C-g.
  if (w->ctx->evaluation_active) {
    EvaluationStep(w->ctx);
  }
  if (w->nodes == 0 || w->bytes == 0) {
    EmitString(w, "#<truncated>");
    w->complete = false;
    return;
  }
  w->nodes--;
  if (depth == 0) {
    EmitString(w, "#<deep>");
    w->complete = false;
    return;
  }

  switch (FeGetType(obj)) {
    case FeTNil:
      EmitString(w, "nil");
      break;

    case FeTDouble:
      EmitDouble(w, obj);
      break;

    case FeTInteger:
      Format(buf, sizeof(buf), "%" PRId64, INTEGER(obj));
      EmitString(w, buf);
      break;

    case FeTPair:
      // `(function X)` prints as `#'X`, the reader macro's abbreviation --
      // the writer half of sub-plan 04D's `#'` change, and the exact shape
      // the pinned `reader-sharp-quote-identity` snapshot compares against.
      // The abbreviation fires only for the single-element proper form, the
      // one the `function` special form accepts; `(function X . tail)` and
      // `(function)` print as ordinary pairs, as they are.
      if (IsNamedSymbol(CAR(obj), "function")) {
        FeObject* const form = CDR(obj);
        if (FeGetType(form) == FeTPair && FeIsNil(CDR(form))) {
          EmitString(w, "#'");
          WriteObject(w, CAR(form), 1, depth - 1);
          break;
        }
      }
      Emit(w, '(');
      WriteElements(w, obj, depth - 1);
      Emit(w, ')');
      break;

    case FeTSymbol:
      EmitStoredString(w, SymbolName(obj), 0);
      break;

    case FeTString:
      EmitStoredString(w, obj, qt);
      break;

    case FeTFn:
    case FeTMacro:
      WriteClosure(w, obj, depth - 1);
      break;

    case FeTPrimitive:
    case FeTNativeFn:
    case FeTPtr:
    case FeTFex0:
    case FeTFex1:
    case FeTFex2:
      Format(buf, sizeof(buf), "[%s]", GetTypeName(FeGetType(obj)));
      EmitString(w, buf);
      break;

    case FeTFree:
    case FeTSentinel:
      abort();
  }
}

static size_t OptionOr(size_t value, size_t fallback) {
  return value != 0 ? value : fallback;
}

bool FeWriteWithOptions(FeContext* ctx,
                        FeObject* obj,
                        FeWriteFn fn,
                        void* udata,
                        int qt,
                        const FeWriteOptions* options) {
  static const FeWriteOptions defaults = {0};
  const FeWriteOptions* o = options != nullptr ? options : &defaults;
  Writer w = {
      .ctx = ctx,
      .fn = fn,
      .udata = udata,
      .bytes = OptionOr(o->max_bytes, DefaultWriteMaxBytes),
      .nodes = OptionOr(o->max_nodes, DefaultWriteMaxNodes),
      .complete = true,
  };
  WriteObject(&w, obj, qt, OptionOr(o->max_depth, DefaultWriteMaxDepth));
  return w.complete;
}

void FeWrite(FeContext* ctx, FeObject* obj, FeWriteFn fn, void* udata, int qt) {
  (void)FeWriteWithOptions(ctx, obj, fn, udata, qt, nullptr);
}

// TODO: See if `void*` is really necessary here, in `WriteBuffer`, et c., or if
// we can use real types.
static void WriteFile(FeContext*, void* udata, char chr) {
  fputc(chr, udata);
}

void FeWriteFile(FeContext* ctx, FeObject* obj, FILE* fp) {
  FeWrite(ctx, obj, WriteFile, fp, 0);
}

typedef struct SizedString {
  char* string;
  size_t size;
} SizedString;

static void WriteBuffer(FeContext*, void* udata, char chr) {
  SizedString* s = udata;
  if (s->size) {
    *s->string++ = chr;
    s->size--;
  }
}

size_t FeToString(FeContext* ctx, FeObject* obj, char* dst, size_t size) {
  // A zero-size destination has no room for the terminator, so nothing is
  // rendered and nothing is written; `dst` may then be null.
  if (size == 0) {
    return 0;
  }
  SizedString s = {.string = dst, .size = size - 1};
  // Nothing beyond the destination can be stored, so nothing beyond it is
  // rendered either: a cyclic object costs `size` bytes of work, not a walk.
  const FeWriteOptions options = {.max_bytes = size};
  (void)FeWriteWithOptions(ctx, obj, WriteBuffer, &s, 0, &options);
  *s.string = '\0';
  return size - s.size - 1;
}

static const FeObject* GetStringObject(FeContext* ctx, const FeObject* obj) {
  const FeType type = FeGetType(obj);
  if (type == FeTSymbol) {
    return SymbolName(obj);
  }
  if (type != FeTString) {
    char message[64];
    Format(message, sizeof(message), "expected string or symbol, got %s",
           GetTypeName(type));
    FeHandleError(ctx, message);
  }
  return obj;
}

static size_t CopyStoredStringBytes(const FeObject* string, char* dst) {
  size_t length = 0;
  while (!FeIsNil(string)) {
    const char* buffer = STRING_BUFFER(string);
    const char* end = memchr(buffer, '\0', StringBufferSize);
    const size_t count =
        end == nullptr ? StringBufferSize : (size_t)(end - buffer);
    if (dst != nullptr) {
      memcpy(dst, buffer, count);
      dst += count;
    }
    length += count;
    string = CDR(string);
  }
  return length;
}

size_t FeStringByteLength(FeContext* ctx, const FeObject* obj) {
  return CopyStoredStringBytes(GetStringObject(ctx, obj), nullptr);
}

bool FeCopyStringBytes(FeContext* ctx,
                       const FeObject* obj,
                       char* dst,
                       size_t size) {
  const FeObject* string = GetStringObject(ctx, obj);
  const size_t length = CopyStoredStringBytes(string, nullptr);
  if (size < length || (dst == nullptr && length != 0)) {
    return false;
  }
  (void)CopyStoredStringBytes(string, dst);
  return true;
}

FeDouble FeToDouble(FeContext* ctx, FeObject* obj) {
  if (FeGetType(obj) == FeTInteger) {
    return (FeDouble)INTEGER(obj);
  }
  return GetDouble(CheckType(ctx, obj, FeTDouble));
}

int64_t FeToInteger(FeContext* ctx, FeObject* obj) {
  return INTEGER(CheckType(ctx, obj, FeTInteger));
}

void* FeToPtr(FeContext*, FeObject* obj) {
  const FeType type = FeGetType(obj);
  if (type >= FeTSentinel) {
    abort();
  }
  return CDR(obj);
}

FeObject* GetBound(FeContext* ctx, FeObject* sym, FeObject* env) {
  // Try to find the symbol in the environment:
  for (; !FeIsNil(env); env = CDR(env)) {
    EvaluationStep(ctx);
    FeObject* x = CAR(env);
    if (CAR(x) == sym) {
      return x;
    }
  }
  // Otherwise, return a global value:
  return SymbolBindingCell(sym);
}

void FeSet(FeContext* ctx, FeObject* sym, FeObject* v) {
  CDR(GetBound(ctx, sym, &nil)) = v;
}

bool FeIsBound(FeContext* ctx, FeObject* sym) {
  return CDR(GetBound(ctx, CheckType(ctx, sym, FeTSymbol), &nil)) != &unbound;
}

// Sub-plan 04C/04D: the function namespace's public surface. `FeSetFunction`
// and `FeIsFBound` are the cell accessors (`FeGetFunction` lives in fe_eval.c
// with the evaluator, because following the designator chain charges the
// step budget and can raise `cyclic-function-indirection`). Since 04D's cut
// the bootstrap lives in function cells too, so these reach what call
// position resolves.
void FeSetFunction(FeContext* ctx, FeObject* sym, FeObject* fn) {
  SetSymbolFunction(CheckType(ctx, sym, FeTSymbol), fn);
}

bool FeIsFBound(FeContext* ctx, FeObject* sym) {
  return SymbolFunction(CheckType(ctx, sym, FeTSymbol)) != &unbound;
}

// Symbol accessors (sub-plan 04B of kg's Emacs-subset program): a symbol's
// `cdr` is one cons, `((name . function) . value)`, and every reader of that
// private layout goes through these. The value path deliberately has no
// accessor of its own: its unit of currency is the binding cell, and lexical
// environment entries and the global cell share the `CDR(cell)` read/write
// contract `GetBound` depends on. The function cell is the Lisp-2 callable
// home (written by `FeSetFunction`/`FeDefineNative`, read by call position,
// `funcall`/`apply`, `symbol-function` and `FeGetFunction`), and
// `SymbolName`/`SymbolBindingCell` read through a `const FeObject*` because
// `GetStringObject`/`IsNamedSymbol` do.
FeObject* SymbolName(const FeObject* sym) {
  return CAR(CAR(CDR(sym)));
}

FeObject* SymbolBindingCell(FeObject* sym) {
  return CDR(sym);
}

FeObject* SymbolFunction(FeObject* sym) {
  return CDR(CAR(SymbolBindingCell(sym)));
}

void SetSymbolFunction(FeObject* sym, FeObject* fn) {
  CDR(CAR(SymbolBindingCell(sym))) = fn;
}

static FeObject rparen;

// Reader macros — 'x, `x, ,x, ,@x and #'x — all expand to `(NAME form)`;
// `#'`'s NAME is `function` (sub-plan 04D).
static FeObject* ReadWrapped(FeContext* ctx,
                             FeReadFn fn,
                             void* udata,
                             const char* name,
                             const char* stray) {
  FeObject* v = FeRead(ctx, fn, udata);
  if (v == NULL) {
    FeHandleError(ctx, stray);
  }
  return FeCons(ctx, FeMakeSymbol(ctx, name), FeCons(ctx, v, &nil));
}

// Emacs' number lexer (05A Decision 3, 05D), replacing ReadAtom's bare
// `strtod`: classify the token first, then convert only the classified text
// with `strtoll`/`strtod`. Integer = optional sign, digits, optional trailing
// dot (`1.` is the integer 1, R2). Float = a fraction and/or an exponent
// (`.5`, `1e3`, `1.e3`, R3/R5). The nonfinite spellings the printer emits --
// `1.0e+INF`, `-1.0e+INF`, `0.0e+NaN`, `-0.0e+NaN` -- read back as nonfinite
// floats, keeping read/print round-tripping an invariant. Everything else is
// a symbol: `0x10`, `inf`, `nan`, `1e`, `1.0e+` are un-numbered (R6-R8).
// An integer literal that overflows `int64_t` reads as a double (the
// pre-bignum Emacs behaviour), recorded as a divergence row against modern
// Emacs' bignums (05A Decision 3).
typedef enum NumberKind {
  NumberSymbol,
  NumberInteger,
  NumberFloat,
  NumberInf,
  NumberNan,
} NumberKind;

// `p` points at the `e`/`E` of an exponent; the significand has been
// consumed. A `+`/`-` sign, digits, or the exact `INF`/`NaN` spellings decide
// a float from a symbol -- `1e3`, `1e+5`, `1e-7` are floats, `1e`, `1e+`,
// `1e+Inf`, `1.0e+NAN` are symbols, and only `e+INF`/`e+NaN` (never `e-INF`)
// are nonfinite, exactly as the pinned Emacs answers. The sign of a nonfinite
// spelling is read from the token's leading `+`/`-` by `ReadAtom`, not here.
// Digit runs use `strspn`, which the analyzer models as reading a
// NUL-terminated string (a bare `while (digit(*p))` loop it cannot bound).
static NumberKind ClassifyExponent(const char* p) {
  p++;
  bool exponent_negative = false;
  if (*p == '+' || *p == '-') {
    exponent_negative = *p == '-';
    p++;
  }
  if (!exponent_negative) {
    if (strcmp(p, "INF") == 0) {
      return NumberInf;
    }
    if (strcmp(p, "NaN") == 0) {
      return NumberNan;
    }
  }
  if (strspn(p, "0123456789") == 0) {
    return NumberSymbol;
  }
  p += strspn(p, "0123456789");
  return *p == '\0' ? NumberFloat : NumberSymbol;
}

static NumberKind ClassifyNumber(const char* s) {
  const char* p = s;
  if (*p == '+' || *p == '-') {
    p++;
  }
  const char* digits = p;
  p += strspn(p, "0123456789");
  const bool before = p != digits;
  if (*p == '.') {
    p++;
  }
  const char* fraction = p;
  p += strspn(p, "0123456789");
  const bool after = p != fraction;
  if (!before && !after) {
    return NumberSymbol;  // `+`, `-`, `.`, `e5`, `.e3` -- no digits at all
  }
  if (*p == 'e' || *p == 'E') {
    return ClassifyExponent(p);  // `1e3`, `1.e3`, `.5e2`, `1e`->symbol
  }
  if (*p != '\0') {
    return NumberSymbol;  // `0x10`, `5.x`, `1.2.3`, `1.0.`
  }
  if (!after) {
    return NumberInteger;  // `5`, `+5`, `5.`
  }
  return NumberFloat;  // `5.0`, `.5`
}

// The NaN with `signbit` == `negative`. `nan("")` returns an implementation
// quiet NaN (no redundant `0.0/0.0` division), and the sign of any NaN is
// observable and flip-able, so normalize the sign explicitly rather than
// trusting which way a given compiler folds a division.
static double NanWithSign(bool negative) {
  double value = nan("");
  if (signbit(value) != negative) {
    value = -value;
  }
  return value;
}

// A number, `nil`, or a symbol. `chr` is the first character; a character
// already pushed back into `ctx->nextchr` is consumed before the input.
static FeObject* ReadAtom(FeContext* ctx, FeReadFn fn, void* udata, char chr) {
  char buf[64];
  char* p = buf;
  const char* delimiter = " \n\t\r();`,";
  do {
    if (p == buf + sizeof(buf) - 1) {
      FeHandleError(ctx, "symbol too long");
    }
    *p++ = chr;
    chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
    ctx->nextchr = '\0';
  } while (chr && !strchr(delimiter, chr));
  *p = '\0';
  ctx->nextchr = chr;
  switch (ClassifyNumber(buf)) {
    case NumberInteger: {
      errno = 0;
      // The classifier has already pinned the token as `[+-]digits`, with an
      // optional trailing dot, so `strtoll` reads the whole number and
      // `ERANGE` is set only when the literal overflows int64 -- the recorded
      // pre-bignum fallback to a double (Decision 3).
      const int64_t value = strtoll(buf, nullptr, 10);
      if (errno == ERANGE) {
        return FeMakeDouble(ctx, strtod(buf, nullptr));
      }
      return FeMakeInteger(ctx, value);
    }
    case NumberFloat:
      return FeMakeDouble(ctx, strtod(buf, nullptr));
    case NumberInf:
      return FeMakeDouble(ctx, buf[0] == '-' ? -HUGE_VAL : HUGE_VAL);
    case NumberNan:
      return FeMakeDouble(ctx, NanWithSign(buf[0] == '-'));
    case NumberSymbol:
      break;
  }
  // Try to read it as nil:
  if (!strcmp(buf, "nil")) {
    return &nil;
  }
  // It's a symbol:
  return FeMakeSymbol(ctx, buf);
}

static FeObject* Read(FeContext* ctx, FeReadFn fn, void* udata);

bool IsNamedSymbol(const FeObject* v, const char* name) {
  return FeGetType(v) == FeTSymbol && IsStringEqual(SymbolName(v), name);
}

static bool IsDot(const FeObject* v) {
  return IsNamedSymbol(v, ".");
}

// Reads the rest of a list, the opening '(' already consumed. A '.' is the
// dotted-pair marker only inside a list; elsewhere it is the ordinary symbol
// `.` that `ReadAtom` interns. The grammar this accepts is exactly
//
//   list := '(' element* [ '.' element ] ')'
//
// with at least one element before a '.'; anything else is a syntax error
// rather than a list that quietly means something else.
static FeObject* ReadList(FeContext* ctx, FeReadFn fn, void* udata) {
  FeObject* res = &nil;
  FeObject** tail = &res;
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, res);  // To cause error on too-deep nesting
  FeObject* v;
  while ((v = Read(ctx, fn, udata)) != &rparen) {
    if (v == NULL) {
      FeHandleError(ctx, "unclosed list");
    }
    if (IsDot(v)) {
      if (FeIsNil(res)) {
        FeHandleError(ctx, "'.' at start of list");
      }
      // Only the internal `Read` reports ')' as `&rparen`; `FeRead` would turn
      // it into the misleading `stray ')'`.
      v = Read(ctx, fn, udata);
      if (v == NULL) {
        FeHandleError(ctx, "unclosed list");
      }
      if (v == &rparen) {
        FeHandleError(ctx, "missing value after '.'");
      }
      *tail = v;
      FeRestoreGC(ctx, gc);
      FePushGC(ctx, res);
      v = Read(ctx, fn, udata);
      if (v == NULL) {
        FeHandleError(ctx, "unclosed list");
      }
      if (v != &rparen) {
        FeHandleError(ctx, "extra value after dotted tail");
      }
      break;
    }
    *tail = FeCons(ctx, v, &nil);
    tail = &CDR(*tail);
    FeRestoreGC(ctx, gc);
    FePushGC(ctx, res);
  }
  return res;
}

static FeObject* Read(FeContext* ctx, FeReadFn fn, void* udata) {
  // Get next character:
  char chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
  ctx->nextchr = '\0';

  // Skip whitespace:
  while (chr && strchr(" \n\t\r", chr)) {
    chr = fn(ctx, udata);
  }

  switch (chr) {
    case '\0':
      return NULL;

    case ';':
      while (chr && chr != '\n') {
        chr = fn(ctx, udata);
      }
      return Read(ctx, fn, udata);

    case ')':
      return &rparen;

    case '(':
      return ReadList(ctx, fn, udata);

    case '\'':
      return ReadWrapped(ctx, fn, udata, "quote", "stray '''");

    case '`':
      return ReadWrapped(ctx, fn, udata, "quasiquote", "stray '`'");

    case ',': {
      const char next = fn(ctx, udata);
      if (next == '@') {
        return ReadWrapped(ctx, fn, udata, "unquote-splicing", "stray ',@'");
      }
      ctx->nextchr = next;
      return ReadWrapped(ctx, fn, udata, "unquote", "stray ','");
    }

    // `#` is an ordinary symbol character, so only `#'` is a reader macro.
    // Since sub-plan 04D's Lisp-2 cut, `#'x` reads as `(function x)` -- the
    // same `ReadWrapped` construction every other reader macro uses, so a
    // missing operand keeps the `stray '#''` diagnostic.
    case '#': {
      const char next = fn(ctx, udata);
      if (next == '\'') {
        return ReadWrapped(ctx, fn, udata, "function", "stray '#''");
      }
      ctx->nextchr = next;
      return ReadAtom(ctx, fn, udata, chr);
    }

    case '"': {
      FeObject* res = BuildString(ctx, NULL, '\0');
      FeObject* v = res;
      chr = fn(ctx, udata);
      while (chr != '"') {
        if (chr == '\0') {
          FeHandleError(ctx, "unclosed string");
        }
        if (chr == '\\') {
          chr = fn(ctx, udata);
          if (memchr("nrt", chr, 3) != nullptr) {
            chr = strchr("n\nr\rt\t", chr)[1];
          }
        }
        v = BuildString(ctx, v, chr);
        chr = fn(ctx, udata);
      }
      return res;
    }

    default:
      return ReadAtom(ctx, fn, udata, chr);
  }
}

FeObject* FeRead(FeContext* ctx, FeReadFn fn, void* udata) {
  FeObject* obj = Read(ctx, fn, udata);
  if (obj == &rparen) {
    FeHandleError(ctx, "stray ')'");
  }
  return obj;
}

static char ReadFile(FeContext*, void* udata) {
  const int c = fgetc(udata);
  return c == EOF ? '\0' : (char)c;
}

FeObject* FeReadFile(FeContext* ctx, FILE* fp) {
  return FeRead(ctx, ReadFile, fp);
}

typedef struct StringInput {
  const char* source;
  size_t length;
  size_t* offset;
} StringInput;

static char ReadString(FeContext* ctx, void* udata) {
  StringInput* input = udata;
  ctx->error_offset = *input->offset;
  if (*input->offset == input->length) {
    return '\0';
  }
  if (input->source == nullptr) {
    FeHandleError(ctx, "null source");
  }
  const char chr = input->source[(*input->offset)++];
  if (chr == '\0') {
    FeHandleError(ctx, "embedded NUL byte");
  }
  return chr;
}

FeObject* FeReadString(FeContext* ctx,
                       const char* source,
                       size_t length,
                       size_t* offset) {
  size_t local_offset = 0;
  size_t* position = offset != nullptr ? offset : &local_offset;
  const char* saved_label = ctx->error_label;
  const size_t saved_offset = ctx->error_offset;
  const bool saved_has_offset = ctx->error_has_offset;
  ctx->error_label = nullptr;
  ctx->error_offset = *position;
  ctx->error_has_offset = true;
  ctx->nextchr = '\0';
  if (source == nullptr && length != 0) {
    FeHandleError(ctx, "null source");
  }
  if (*position > length) {
    FeHandleError(ctx, "offset exceeds source length");
  }

  StringInput input = {.source = source, .length = length, .offset = position};
  FeObject* result = FeRead(ctx, ReadString, &input);
  if (ctx->nextchr != '\0') {
    (*position)--;
    ctx->nextchr = '\0';
  }
  ctx->error_label = saved_label;
  ctx->error_offset = saved_offset;
  ctx->error_has_offset = saved_has_offset;
  return result;
}

FeRoot* FeCreateRoot(FeContext* ctx, FeObject* object) {
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, object);
  FeObject* root = FeCons(ctx, object, ctx->root_list);
  ctx->root_list = root;
  FeRestoreGC(ctx, gc);
  return (FeRoot*)root;
}

FeObject* FeGetRoot(const FeRoot* root) {
  return CAR((const FeObject*)root);
}

void FeReleaseRoot(FeContext* ctx, FeRoot* root) {
  FeObject* object = (FeObject*)root;
  FeObject** link = &ctx->root_list;
  while (!FeIsNil(*link) && *link != object) {
    link = &CDR(*link);
  }
  if (FeIsNil(*link)) {
    FeHandleError(ctx, "root is not active");
  }
  *link = CDR(object);
}

static FeObject* EvaluateInput(FeContext* ctx,
                               const char* label,
                               FeReadFn* read,
                               void* input) {
  const char* saved_label = ctx->error_label;
  const size_t saved_offset = ctx->error_offset;
  const bool saved_has_offset = ctx->error_has_offset;
  const size_t gc = FeSaveGC(ctx);
  ctx->error_label = label;
  ctx->error_has_offset = true;
  ctx->nextchr = '\0';
  ctx->evaluation_result = &nil;

  while (true) {
    FeRestoreGC(ctx, gc);
    ctx->error_has_offset = true;
    FeObject* object = FeRead(ctx, read, input);
    if (object == nullptr) {
      break;
    }
    ctx->error_has_offset = false;
    ctx->evaluation_result = FeEvaluate(ctx, object);
  }

  FeRestoreGC(ctx, gc);
  ctx->error_label = saved_label;
  ctx->error_offset = saved_offset;
  ctx->error_has_offset = saved_has_offset;
  return ctx->evaluation_result;
}

FeObject* FeEvaluateString(FeContext* ctx,
                           const char* label,
                           const char* source,
                           size_t length) {
  size_t offset = 0;
  StringInput input = {.source = source, .length = length, .offset = &offset};
  return EvaluateInput(ctx, label, ReadString, &input);
}

FeObject* FeEvaluateStringWithOptions(FeContext* ctx,
                                      const char* label,
                                      const char* source,
                                      size_t length,
                                      const FeEvalOptions* options) {
  const bool owns_control = BeginEvaluationControl(ctx, options);
  FeObject* result = FeEvaluateString(ctx, label, source, length);
  EndEvaluationControl(ctx, owns_control);
  return result;
}

typedef struct FileInput {
  FILE* file;
  size_t offset;
} FileInput;

static char ReadEvaluatedFile(FeContext* ctx, void* udata) {
  FileInput* input = udata;
  ctx->error_offset = input->offset;
  if (input->file == nullptr) {
    FeHandleError(ctx, "null file");
  }
  const int chr = fgetc(input->file);
  if (chr == EOF) {
    if (ferror(input->file)) {
      FeHandleError(ctx, "file read error");
    }
    return '\0';
  }
  if (chr == '\0') {
    FeHandleError(ctx, "embedded NUL byte");
  }
  input->offset++;
  return (char)chr;
}

FeObject* FeEvaluateFile(FeContext* ctx, const char* label, FILE* file) {
  FileInput input = {.file = file, .offset = 0};
  return EvaluateInput(ctx, label, ReadEvaluatedFile, &input);
}

FeObject* FeEvaluateFileWithOptions(FeContext* ctx,
                                    const char* label,
                                    FILE* file,
                                    const FeEvalOptions* options) {
  const bool owns_control = BeginEvaluationControl(ctx, options);
  FeObject* result = FeEvaluateFile(ctx, label, file);
  EndEvaluationControl(ctx, owns_control);
  return result;
}

static size_t GetSymbolObjectCount(const char* name) {
  // A symbol object plus its one `(name . function) . value` cons chain:
  // the symbol, the name string cells, and the single extra pair the 04B
  // function cell added. `StringBufferSize` name characters fit in one
  // string cell.
  const size_t length = strlen(name);
  assert(length > 0);
  return 5 + (length - 1) / StringBufferSize;
}

static FeObject* native_sin(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, sin(x));
}

static FeObject* native_cos(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, cos(x));
}

static FeObject* native_tan(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, tan(x));
}

static FeObject* native_asin(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, asin(x));
}

static FeObject* native_acos(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, acos(x));
}

static FeObject* native_atan(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeDouble(ctx, atan(x));
  }
  double y = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, atan2(y, x));
}

// `expt`'s all-integer arm (05A row M2, 05C): fast exponentiation over
// int64 with the tower's overflow policy -- a power that outgrows the type
// is `arith-error`, never a truncated or undefined result.
static int64_t IntegerPower(FeContext* ctx, int64_t base, int64_t exponent) {
  int64_t result = 1;
  int64_t factor = base;
  int64_t e = exponent;
  while (e > 0) {
    if (e & 1) {
      if (ckd_mul(&result, result, factor)) {
        FeHandleError(ctx, "arith-error");
      }
    }
    e >>= 1;
    if (e > 0 && ckd_mul(&factor, factor, factor)) {
      FeHandleError(ctx, "arith-error");
    }
  }
  return result;
}

static FeObject* native_expt(FeContext* ctx, FeObject* arg) {
  FeObject* const base = FeGetNextArgument(ctx, &arg);
  FeObject* const exponent = FeGetNextArgument(ctx, &arg);
  FeRequireNoArguments(ctx, arg);
  // 05A row M2's per-signature rule (05C): two integers with a non-negative
  // exponent stay integer -- `(expt 2 8)` is 256 -- with the tower's int64
  // overflow policy, `arith-error`, when the power outgrows the type. Any
  // double (a float base or exponent, or a negative exponent) promotes the
  // whole call to the double `pow`.
  if (FeGetType(base) == FeTInteger && FeGetType(exponent) == FeTInteger &&
      INTEGER(exponent) >= 0) {
    return FeMakeInteger(ctx,
                         IntegerPower(ctx, INTEGER(base), INTEGER(exponent)));
  }
  return FeMakeDouble(ctx,
                      pow(FeToDouble(ctx, base), FeToDouble(ctx, exponent)));
}

static FeObject* native_sqrt(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, sqrt(x));
}

static FeObject* native_exp(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, exp(x));
}

static FeObject* native_log(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeDouble(ctx, log(x));
  }
  double base = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, log(x) / log(base));
}

// The rounding family's integer conversion (05A rows M3/M4, 05C): the double
// is already rounded per the function (`floor`, `ceil`, `nearbyint`,
// `trunc`) and the answer is the int64 that rounded value names -- Emacs'
// integer return type for `floor`/`ceiling`/`round`/`truncate`. A value
// outside int64's range has no integer answer in a no-bignums program, the
// same refusal as int64 overflow (05A Decision 5), so it is `arith-error`
// rather than a cast whose behaviour is undefined; NaN and ±Infinity are out
// of range too.
static int64_t IntegerRound(FeContext* ctx, double x) {
  // `(double)INT64_MAX` rounds up to exactly 2^63, so `x >= (double)INT64_MAX`
  // is `x >= 2^63` -- the same bounds as the hex-float `0x1p63` spelling, in
  // a form the static analyzer does not mis-solve.
  if (x < (double)INT64_MIN || x >= (double)INT64_MAX) {
    FeHandleError(ctx, "arith-error");
  }
  return (int64_t)x;
}

static FeObject* native_floor(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, floor(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(ctx, IntegerRound(ctx, floor(x / d)));
}

static FeObject* native_ceiling(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, ceil(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(ctx, IntegerRound(ctx, ceil(x / d)));
}

static FeObject* native_round(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, nearbyint(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(ctx, IntegerRound(ctx, nearbyint(x / d)));
}

static FeObject* native_truncate(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, trunc(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(ctx, IntegerRound(ctx, trunc(x / d)));
}

static size_t GetCoreObjectCount(void) {
  static const char* math_names[] = {
      "sin", "cos", "tan",   "asin",    "acos",  "atan",     "expt", "sqrt",
      "exp", "log", "floor", "ceiling", "round", "truncate", "pi",   "e",
  };
  size_t count = GetSymbolObjectCount("t");
  for (Primitive i = PAssert; i < PSentinel; i++) {
    count += 1 + GetSymbolObjectCount(primitive_names[i]);
  }
  for (size_t i = 0; i < COUNT(primitive_aliases); i++) {
    count += GetSymbolObjectCount(primitive_aliases[i].name);
  }
  for (size_t i = 0; i < COUNT(math_names); i++) {
    count += 1 + GetSymbolObjectCount(math_names[i]);
  }
  return count;
}

static size_t GetMinimumArenaSize(void) {
  size_t frames = 0;
  size_t objects = 0;
  size_t minimum = 0;
  const bool overflow =
      ckd_add(&frames, MinFrameCapacity, CleanupFrameReserve) ||
      ckd_mul(&frames, frames, sizeof(FeEvalFrame)) ||
      ckd_mul(&objects, GetCoreObjectCount(), sizeof(FeObject)) ||
      ckd_add(&minimum, sizeof(FeArena), frames) ||
      ckd_add(&minimum, minimum, objects);
  assert(!overflow);
  return minimum;
}

size_t FeMinimumArenaSize(void) {
  return GetMinimumArenaSize();
}

size_t FeArenaAlignment(void) {
  return alignof(FeArena);
}

FeArenaStats FeGetArenaStats(const FeContext* ctx) {
  return (FeArenaStats){
      .total_slots = ctx->object_count,
      .free_slots = ctx->object_count - ctx->arena_live_count,
      .peak_live_objects = ctx->arena_peak_live_count,
      .collection_count = ctx->arena_collection_count,
      .peak_gc_stack_depth = ctx->arena_peak_gc_stack_depth,
      .frame_capacity = ctx->frame_stack_capacity,
      .peak_frame_depth = ctx->arena_peak_frame_depth,
      .peak_cleanup_stack_depth = ctx->arena_peak_cleanup_stack_depth,
      .peak_native_reentry = ctx->arena_peak_native_reentry,
      .allocation_failures = ctx->arena_allocation_failures,
  };
}

static bool InitializeArenaLayout(FeContext* ctx, void* arena, size_t size) {
  const size_t minimum = GetMinimumArenaSize();
  const size_t remainder = size - minimum;
  const size_t frame_bonus_bytes = (remainder / 100) * FrameArenaPercent +
                                   (remainder % 100) * FrameArenaPercent / 100;
  size_t frame_capacity = 0;
  size_t frame_storage_capacity = 0;
  size_t frame_bytes = 0;
  size_t object_count = 0;
  bool layout_overflow =
      ckd_add(&frame_capacity, MinFrameCapacity,
              frame_bonus_bytes / sizeof(FeEvalFrame)) ||
      ckd_add(&frame_storage_capacity, frame_capacity, CleanupFrameReserve) ||
      ckd_mul(&frame_bytes, frame_storage_capacity, sizeof(FeEvalFrame)) ||
      ckd_add(&object_count, GetCoreObjectCount(),
              (remainder - frame_bonus_bytes) / sizeof(FeObject));
  if (layout_overflow) {
    return false;
  }

  // Initialize the context and its two arena-resident regions. Frames precede
  // objects; both types have the same alignment and each region's element size
  // is a multiple of it, so no implicit padding is needed at this seam.
  memset(ctx, 0, sizeof(FeContext));
  void* const frame_region = (unsigned char*)arena + sizeof(FeArena);
  void* const object_region = (unsigned char*)frame_region + frame_bytes;
  ctx->frame_stack = frame_region;
  ctx->frame_stack_capacity = frame_capacity;
  ctx->objects = object_region;
  ctx->object_count = object_count;
  return true;
}

static FeContext* OpenContext(void* arena, size_t size) {
  uintptr_t arena_end;
  const uintptr_t arena_address = (uintptr_t)arena;
  bool invalid = arena == nullptr;
  invalid |= size < FeMinimumArenaSize();
  invalid |= arena_address % FeArenaAlignment() != 0;
  invalid |= ckd_add(&arena_end, arena_address, size);
  if (invalid) {
    return nullptr;
  }

  FeArena* storage = arena;
  FeContext* ctx = &storage->context;
  if (!InitializeArenaLayout(ctx, arena, size)) {
    return nullptr;
  }

  // Initialize the lists:
  ctx->call_list = &nil;
  ctx->free_list = &nil;
  ctx->symbol_list = &nil;
  ctx->evaluation_result = &nil;
  ctx->call_result = &nil;
  ctx->root_list = &nil;

  // Populate the free_list:
  for (size_t i = 0; i < ctx->object_count; i++) {
    FeObject* obj = &ctx->objects[i];
    SetType(obj, FeTFree);
    CDR(obj) = ctx->free_list;
    ctx->free_list = obj;
  }

  // Initialize the objects:
  ctx->t = FeMakeSymbol(ctx, "t");
  FeSet(ctx, ctx->t, ctx->t);

  // Register the built-in primitives (sub-plan 04D's cut): every callable --
  // the primitives, the `fn` alias, and the math natives registered through
  // `FeDefineNative` below -- lands in the *function* cell, the only cell
  // call position resolves since the cut. `t`, `pi` and `e` stay values.
  const size_t save = FeSaveGC(ctx);
  for (Primitive i = PAssert; i < PSentinel; i++) {
    FeObject* v = MakeObject(ctx);
    SetType(v, FeTPrimitive);
    PRIM(v) = (char)i;
    SetSymbolFunction(FeMakeSymbol(ctx, primitive_names[i]), v);
    FeRestoreGC(ctx, save);
  }
  for (size_t i = 0; i < COUNT(primitive_aliases); i++) {
    const Primitive p = primitive_aliases[i].primitive;
    FeObject* canonical = FeMakeSymbol(ctx, primitive_names[p]);
    SetSymbolFunction(FeMakeSymbol(ctx, primitive_aliases[i].name),
                      SymbolFunction(canonical));
    FeRestoreGC(ctx, save);
  }

  FeDefineNative(ctx, "sin", native_sin);
  FeDefineNative(ctx, "cos", native_cos);
  FeDefineNative(ctx, "tan", native_tan);
  FeDefineNative(ctx, "asin", native_asin);
  FeDefineNative(ctx, "acos", native_acos);
  FeDefineNative(ctx, "atan", native_atan);
  FeDefineNative(ctx, "expt", native_expt);
  FeDefineNative(ctx, "sqrt", native_sqrt);
  FeDefineNative(ctx, "exp", native_exp);
  FeDefineNative(ctx, "log", native_log);
  FeDefineNative(ctx, "floor", native_floor);
  FeDefineNative(ctx, "ceiling", native_ceiling);
  FeDefineNative(ctx, "round", native_round);
  FeDefineNative(ctx, "truncate", native_truncate);
  FeSet(ctx, FeMakeSymbol(ctx, "pi"), FeMakeDouble(ctx, M_PI));
  FeSet(ctx, FeMakeSymbol(ctx, "e"), FeMakeDouble(ctx, M_E));
  FeRestoreGC(ctx, save);
  return ctx;
}

FeContext* FeOpenContext(void* arena, size_t size) {
  return OpenContext(arena, size);
}

void FeCloseContext(FeContext* ctx) {
  // Clear the GC stack and symbol list: this makes all objects unreachable:
  ctx->gc_stack_index = 0;
  ctx->symbol_list = &nil;
  ctx->evaluation_result = &nil;
  ctx->call_result = &nil;
  ctx->root_list = &nil;
  ctx->frame_stack_index = 0;
  CollectGarbage(ctx);
}
