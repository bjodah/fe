// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#include <assert.h>
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

const char* FeVersion = "1.1";

#define COUNT(a) (sizeof((a)) / sizeof((a)[0]))

typedef enum Primitive {
  PAssert,
  PEnv,
  PLet,
  PSet,
  PIf,
  PFn,
  PMacro,
  PWhile,
  PQuote,
  PAnd,
  POr,
  PDo,
  PCons,
  PCar,
  PCdr,
  PSetCar,
  PSetCdr,
  PList,
  PNot,
  PIs,
  PAtom,
  PPrint,
  PLess,
  PLessEqual,
  // TODO: Add > and >=, and document them.
  PAdd,
  PSub,
  PMul,
  PDiv,
  PSentinel
} Primitive;

static const char* primitive_names[] = {
    [PAssert] = "assert", [PEnv] = "env",       [PLet] = "let",
    [PSet] = "=",         [PIf] = "if",         [PFn] = "fn",
    [PMacro] = "macro",   [PWhile] = "while",   [PQuote] = "quote",
    [PAnd] = "and",       [POr] = "or",         [PDo] = "do",
    [PCons] = "cons",     [PCar] = "car",       [PCdr] = "cdr",
    [PSetCar] = "setcar", [PSetCdr] = "setcdr", [PList] = "list",
    [PNot] = "not",       [PIs] = "is",         [PAtom] = "atom",
    [PPrint] = "print",   [PLess] = "<",        [PLessEqual] = "<=",
    [PAdd] = "+",         [PSub] = "-",         [PMul] = "*",
    [PDiv] = "/"};

const char* type_names[] = {
    [FeTPair] = "pair",
    [FeTFree] = "free",
    [FeTNil] = "nil",
    [FeTDouble] = "double",
    [FeTSymbol] = "symbol",
    [FeTString] = "string",
    [FeTFn] = "fn",
    [FeTMacro] = "macro",
    [FeTPrimitive] = "primitive",
    [FeTNativeFn] = "native-fn",
    [FeTPtr] = "ptr",
    [FeTFex0] = "fex0",
    [FeTFex1] = "fex1",
    [FeTFex2] = "fex2",
};

typedef union {
  FeObject* o;
  FeNativeFn* f;
  FeDouble n;
  // TODO: Might need/want to make this `uintptr_t` someday.
  char c;
} Value;

enum {
  // Stored in the lowest-order bit of `Value.c`:
  ConsCell = 0,
  OtherCell = 1,
  // The 2nd-lowest-order bit of `Value.c` is the mark bit:
  GcMarkBit = 2,
  // TODO: This should scale with arena size?
  GcStackSize = 512,
  StringBufferSize = (sizeof(FeObject*) - 1),
  DefaultEvalPollInterval = 1024,
};

struct FeObject {
  Value car, cdr;
};

FeObject nil = {.car = {.c = FeTNil << GcMarkBit | OtherCell},
                .cdr = {.o = NULL}};

#define CAR(x) ((x)->car.o)
#define CDR(x) ((x)->cdr.o)
#define TAG(x) ((x)->car.c)
#define DOUBLE(x) ((x)->cdr.n)
#define PRIM(x) ((x)->cdr.c)
#define NATIVE_FN(x) ((x)->cdr.f)
#define STRING_BUFFER(x) (&(x)->car.c + 1)

static FeDouble GetDouble(const FeObject* o) {
  return o->cdr.n;
}

static FeNativeFn* GetNativeFn(const FeObject* o) {
  return o->cdr.f;
}

static void SetType(FeObject* o, FeType type) {
  o->car.c = (char)((type) << GcMarkBit | OtherCell);
}

struct FeContext {
  FeErrorFn* error_fn;
  FeNativeFn* mark_fn;
  FeNativeFn* gc_fn;
  void* userdata;
  FeObject* gc_stack[GcStackSize];
  size_t gc_stack_index;
  FeObject* objects;
  size_t object_count;
  FeObject* call_list;
  FeObject* free_list;
  FeObject* symbol_list;
  FeObject* evaluation_result;
  FeObject* call_result;
  FeObject* root_list;
  FeObject* t;
  FeInterruptFn* evaluation_interrupt;
  void* evaluation_userdata;
  size_t evaluation_steps;
  size_t evaluation_poll_interval;
  size_t evaluation_poll_countdown;
  const char* error_label;
  size_t error_offset;
  bool evaluation_active;
  bool evaluation_limited;
  bool error_has_offset;
  char nextchr;
};

typedef struct FeArena {
  FeContext context;
  FeObject objects[];
} FeArena;

static_assert(offsetof(FeArena, objects) == sizeof(FeContext));
static_assert(alignof(FeArena) >= alignof(FeContext));
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

static void __attribute((format(printf, 3, 4))) Format(char* result,
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

static void ClearEvaluationControl(FeContext* ctx) {
  ctx->evaluation_interrupt = nullptr;
  ctx->evaluation_userdata = nullptr;
  ctx->evaluation_steps = 0;
  ctx->evaluation_poll_interval = 0;
  ctx->evaluation_poll_countdown = 0;
  ctx->evaluation_active = false;
  ctx->evaluation_limited = false;
}

static void EndEvaluationControl(FeContext* ctx, bool owns_control) {
  if (owns_control) {
    ClearEvaluationControl(ctx);
  }
}

[[noreturn]] void FeHandleError(FeContext* ctx, const char* msg) {
  FeObject* cl = ctx->call_list;
  const char* label = ctx->error_label;
  const size_t offset = ctx->error_offset;
  const bool has_offset = ctx->error_has_offset;
  char message[1024];
  // reset context state:
  ctx->call_list = &nil;
  ctx->error_label = nullptr;
  ctx->error_has_offset = false;
  ctx->nextchr = '\0';
  ClearEvaluationControl(ctx);

  switch ((label != nullptr) * 2 + has_offset) {
    case 3:
      Format(message, sizeof(message), "%s:%zu: %s", label, offset, msg);
      msg = message;
      break;
    case 2:
      Format(message, sizeof(message), "%s: %s", label, msg);
      msg = message;
      break;
    case 1:
      Format(message, sizeof(message), "byte %zu: %s", offset, msg);
      msg = message;
      break;
    default:
      break;
  }

  if (ctx->error_fn) {
    ctx->error_fn(ctx, msg, cl);
  }
  abort();
}

static bool BeginEvaluationControl(FeContext* ctx,
                                   const FeEvalOptions* options) {
  if (ctx->evaluation_active) {
    return false;
  }
  ctx->evaluation_active = true;
  if (options == nullptr) {
    return true;
  }
  ctx->evaluation_limited = options->step_limit != 0;
  ctx->evaluation_steps = options->step_limit;
  ctx->evaluation_interrupt = options->interrupt;
  ctx->evaluation_userdata = options->userdata;
  ctx->evaluation_poll_interval = options->poll_interval != 0
                                      ? options->poll_interval
                                      : DefaultEvalPollInterval;
  ctx->evaluation_poll_countdown = ctx->evaluation_poll_interval;
  return true;
}

static void EvaluationStep(FeContext* ctx) {
  if (ctx->evaluation_limited) {
    if (ctx->evaluation_steps == 0) {
      FeHandleError(ctx, "evaluation step limit exceeded");
    }
    ctx->evaluation_steps--;
  }
  if (ctx->evaluation_interrupt != nullptr &&
      --ctx->evaluation_poll_countdown == 0) {
    ctx->evaluation_poll_countdown = ctx->evaluation_poll_interval;
    if (ctx->evaluation_interrupt(ctx, ctx->evaluation_userdata)) {
      FeHandleError(ctx, "evaluation cancelled");
    }
  }
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

static FeObject* CheckType(FeContext* ctx, FeObject* obj, FeType type) {
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
    case FeTPrimitive:
    case FeTNativeFn:
      // Do nothing.
      break;

    case FeTSentinel:
      abort();
  }
}

static void CollectGarbage(FeContext* ctx) {
  // Mark:
  for (size_t i = 0; i < ctx->gc_stack_index; i++) {
    FeMark(ctx, ctx->gc_stack[i]);
  }
  FeMark(ctx, ctx->symbol_list);
  FeMark(ctx, ctx->evaluation_result);
  FeMark(ctx, ctx->call_result);
  FeMark(ctx, ctx->root_list);

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

static bool Equal(FeObject* a, FeObject* b) {
  if (a == b) {
    return true;
  }
  if (FeGetType(a) != FeGetType(b)) {
    return false;
  }
  if (FeGetType(a) == FeTDouble) {
    return IsNearlyEqual(GetDouble(a), GetDouble(b), DBL_EPSILON);
  } else if (FeGetType(a) == FeTString) {
    for (; !FeIsNil(a); a = CDR(a), b = CDR(b)) {
      if (CAR(a) != CAR(b)) {
        return false;
      }
    }
    return a == b;
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

static FeObject* MakeObject(FeContext* ctx) {
  // Run GC if free_list has no more objects:
  if (FeIsNil(ctx->free_list)) {
    CollectGarbage(ctx);
    if (FeIsNil(ctx->free_list)) {
      FeHandleError(ctx, "out of memory");
    }
  }
  // Get object from free_list and push it onto the GC stack:
  FeObject* obj = ctx->free_list;
  ctx->free_list = CDR(obj);
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
    if (IsStringEqual(CAR(CDR(CAR(obj))), name)) {
      return CAR(obj);
    }
  }
  // Create new object, push to symbol_list and return:
  obj = MakeObject(ctx);
  SetType(obj, FeTSymbol);
  CDR(obj) = FeCons(ctx, FeMakeString(ctx, name), &nil);
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
  const size_t gc = FeSaveGC(ctx);
  FeObject* symbol = FeMakeSymbol(ctx, name);
  FeObject* native = FeMakeNativeFn(ctx, fn);
  FeSet(ctx, symbol, native);
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

static void WriteString(FeContext* ctx,
                        FeWriteFn fn,
                        void* udata,
                        const char* s) {
  while (*s) {
    fn(ctx, udata, *s++);
  }
}

void FeWrite(FeContext* ctx, FeObject* obj, FeWriteFn fn, void* udata, int qt) {
  char buf[32];
  switch (FeGetType(obj)) {
    case FeTNil:
      WriteString(ctx, fn, udata, "nil");
      break;

    case FeTDouble: {
      const double d = GetDouble(obj);
      // cppcheck-suppress incorrectLogicOperator
      if (d >= -0x1p63 && d < 0x1p63 && floor(d) == d) {
        Format(buf, sizeof(buf), "%" PRId64, (int64_t)d);
      } else {
        Format(buf, sizeof(buf), "%.7g", GetDouble(obj));
      }
      WriteString(ctx, fn, udata, buf);
      break;
    }

    case FeTPair:
      fn(ctx, udata, '(');
      while (true) {
        FeWrite(ctx, CAR(obj), fn, udata, 1);
        obj = CDR(obj);
        if (FeGetType(obj) != FeTPair) {
          break;
        }
        fn(ctx, udata, ' ');
      }
      if (!FeIsNil(obj)) {
        WriteString(ctx, fn, udata, " . ");
        FeWrite(ctx, obj, fn, udata, 1);
      }
      fn(ctx, udata, ')');
      break;

    case FeTSymbol:
      FeWrite(ctx, CAR(CDR(obj)), fn, udata, 0);
      break;

    case FeTString:
      if (qt) {
        fn(ctx, udata, '"');
      }
      while (!FeIsNil(obj)) {
        for (size_t i = 0; i < StringBufferSize && STRING_BUFFER(obj)[i]; i++) {
          if (qt && STRING_BUFFER(obj)[i] == '"') {
            fn(ctx, udata, '\\');
          }
          fn(ctx, udata, STRING_BUFFER(obj)[i]);
        }
        obj = CDR(obj);
      }
      if (qt) {
        fn(ctx, udata, '"');
      }
      break;

    case FeTFn:
      // TODO: Write a pretty-printer, and use it here and elsewhere.
      FeWrite(ctx, FeCons(ctx, FeMakeSymbol(ctx, "fn"), CDR(CDR(obj))), fn,
              udata, qt);
      break;

    case FeTMacro:
      // TODO: Write a pretty-printer, and use it here and elsewhere.
      FeWrite(ctx, FeCons(ctx, FeMakeSymbol(ctx, "macro"), CDR(CDR(obj))), fn,
              udata, qt);
      break;

    case FeTPrimitive:
    case FeTNativeFn:
    case FeTPtr:
    case FeTFex0:
    case FeTFex1:
    case FeTFex2:
      Format(buf, sizeof(buf), "[%s]", GetTypeName(FeGetType(obj)));
      WriteString(ctx, fn, udata, buf);
      break;

    case FeTFree:
    case FeTSentinel:
      abort();
  }
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
  SizedString s = {.string = dst, .size = size - 1};
  FeWrite(ctx, obj, WriteBuffer, &s, 0);
  *s.string = '\0';
  return size - s.size - 1;
}

static const FeObject* GetStringObject(FeContext* ctx, const FeObject* obj) {
  const FeType type = FeGetType(obj);
  if (type == FeTSymbol) {
    return CAR(CDR(obj));
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
  return GetDouble(CheckType(ctx, obj, FeTDouble));
}

void* FeToPtr(FeContext*, FeObject* obj) {
  const FeType type = FeGetType(obj);
  if (type >= FeTSentinel) {
    abort();
  }
  return CDR(obj);
}

static FeObject* GetBound(FeContext* ctx, FeObject* sym, FeObject* env) {
  // Try to find the symbol in the environment:
  for (; !FeIsNil(env); env = CDR(env)) {
    EvaluationStep(ctx);
    FeObject* x = CAR(env);
    if (CAR(x) == sym) {
      return x;
    }
  }
  // Otherwise, return a global value:
  return CDR(sym);
}

void FeSet(FeContext* ctx, FeObject* sym, FeObject* v) {
  CDR(GetBound(ctx, sym, &nil)) = v;
}

static FeObject rparen;

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

    case '(': {
      FeObject* res = &nil;
      FeObject** tail = &res;
      size_t gc = FeSaveGC(ctx);
      FePushGC(ctx, res);  // To cause error on too-deep nesting
      FeObject* v;
      while ((v = Read(ctx, fn, udata)) != &rparen) {
        if (v == NULL) {
          FeHandleError(ctx, "unclosed list");
        }
        if (FeGetType(v) == FeTSymbol && IsStringEqual(CAR(CDR(v)), ".")) {
          // Dotted pair
          *tail = FeRead(ctx, fn, udata);
        } else {
          // Proper pair
          *tail = FeCons(ctx, v, &nil);
          tail = &CDR(*tail);
        }
        FeRestoreGC(ctx, gc);
        FePushGC(ctx, res);
      }
      return res;
    }

    case '\'': {
      FeObject* v = FeRead(ctx, fn, udata);
      if (v == NULL) {
        FeHandleError(ctx, "stray '''");
      }
      return FeCons(ctx, FeMakeSymbol(ctx, "quote"), FeCons(ctx, v, &nil));
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

    default: {
      char buf[64];
      char* p = buf;
      const char* delimiter = " \n\t\r();";
      do {
        if (p == buf + sizeof(buf) - 1) {
          FeHandleError(ctx, "symbol too long");
        }
        *p++ = chr;
        chr = fn(ctx, udata);
      } while (chr && !strchr(delimiter, chr));
      *p = '\0';
      ctx->nextchr = chr;
      // Try to read it as a double:
      FeDouble n = strtod(buf, &p);
      if (p != buf && strchr(delimiter, *p)) {
        return FeMakeDouble(ctx, n);
      }
      // Try to read it as nil:
      if (!strcmp(buf, "nil")) {
        return &nil;
      }
      // It's a symbol:
      return FeMakeSymbol(ctx, buf);
    }
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

static FeObject* Evaluate(FeContext* ctx,
                          FeObject* obj,
                          FeObject* env,
                          FeObject** bind);

static FeObject* EvaluateList(FeContext* ctx, FeObject* lst, FeObject* env) {
  FeObject* res = &nil;
  FeObject** tail = &res;
  while (!FeIsNil(lst)) {
    EvaluationStep(ctx);
    *tail = FeCons(ctx, Evaluate(ctx, FeGetNextArgument(ctx, &lst), env, NULL),
                   &nil);
    tail = &CDR(*tail);
  }
  return res;
}

static FeObject* DoList(FeContext* ctx, FeObject* lst, FeObject* env) {
  FeObject* res = &nil;
  const size_t save = FeSaveGC(ctx);
  while (!FeIsNil(lst)) {
    EvaluationStep(ctx);
    FeRestoreGC(ctx, save);
    FePushGC(ctx, lst);
    FePushGC(ctx, env);
    res = Evaluate(ctx, FeGetNextArgument(ctx, &lst), env, &env);
  }
  return res;
}

static FeObject* ArgsToEnv(FeContext* ctx,
                           FeObject* prm,
                           FeObject* arg,
                           FeObject* env) {
  while (!FeIsNil(prm)) {
    EvaluationStep(ctx);
    if (FeGetType(prm) != FeTPair) {
      env = FeCons(ctx, FeCons(ctx, prm, arg), env);
      break;
    }
    env = FeCons(ctx, FeCons(ctx, CAR(prm), FeCar(ctx, arg)), env);
    prm = CDR(prm);
    arg = FeCdr(ctx, arg);
  }
  return env;
}

#define EVAL_ARG() Evaluate(ctx, FeGetNextArgument(ctx, &arg), env, NULL)

#define ARITH_OP(op)                          \
  {                                           \
    FeDouble x = FeToDouble(ctx, EVAL_ARG()); \
    while (!FeIsNil(arg)) {                   \
      x = x op FeToDouble(ctx, EVAL_ARG());   \
    }                                         \
    res = FeMakeDouble(ctx, x);               \
  }

#define NUM_CMP_OP(op)                                     \
  {                                                        \
    va = CheckType(ctx, EVAL_ARG(), FeTDouble);            \
    vb = CheckType(ctx, EVAL_ARG(), FeTDouble);            \
    res = FeMakeBool(ctx, GetDouble(va) op GetDouble(vb)); \
  }

static FeObject* EvaluatePrimitive(FeContext* ctx,
                                   FeObject* obj,
                                   FeObject* env,
                                   FeObject** newenv,
                                   const FeObject* fn) {
  FeObject* res = &nil;
  FeObject* arg = CDR(obj);
  FeObject* va;
  const FeObject* vb;
  switch (PRIM(fn)) {
    case PAssert:
      va = EVAL_ARG();
      if (FeIsNil(va)) {
        FeHandleError(ctx, "assertion failure");
      }
      return res;
    case PEnv:
      return ctx->symbol_list;
    case PLet:
      va = CheckType(ctx, FeGetNextArgument(ctx, &arg), FeTSymbol);
      if (newenv) {
        *newenv = FeCons(ctx, FeCons(ctx, va, EVAL_ARG()), env);
      }
      return res;
    case PSet:
      va = CheckType(ctx, FeGetNextArgument(ctx, &arg), FeTSymbol);
      CDR(GetBound(ctx, va, env)) = EVAL_ARG();
      return res;
    case PIf:
      while (!FeIsNil(arg)) {
        va = EVAL_ARG();
        if (!FeIsNil(va)) {
          res = FeIsNil(arg) ? va : EVAL_ARG();
          return res;
        }
        if (FeIsNil(arg)) {
          return res;
        }
        arg = CDR(arg);
      }
      return res;
    case PFn:
    case PMacro:
      va = FeCons(ctx, env, arg);
      (void)FeGetNextArgument(ctx, &arg);
      res = MakeObject(ctx);
      SetType(res, PRIM(fn) == PFn ? FeTFn : FeTMacro);
      CDR(res) = va;
      return res;
    case PWhile: {
      va = FeGetNextArgument(ctx, &arg);
      const size_t n = FeSaveGC(ctx);
      while (!FeIsNil(Evaluate(ctx, va, env, NULL))) {
        EvaluationStep(ctx);
        DoList(ctx, arg, env);
        FeRestoreGC(ctx, n);
      }
      return res;
    }
    case PQuote:
      return FeGetNextArgument(ctx, &arg);
    case PAnd:
      while (!FeIsNil(arg) && !FeIsNil(res = EVAL_ARG()))
        ;
      return res;
    case POr:
      while (!FeIsNil(arg) && FeIsNil(res = EVAL_ARG()))
        ;
      return res;
    case PDo:
      return DoList(ctx, arg, env);
    case PCons:
      va = EVAL_ARG();
      return FeCons(ctx, va, EVAL_ARG());
    case PCar:
      return FeCar(ctx, EVAL_ARG());
    case PCdr:
      return FeCdr(ctx, EVAL_ARG());
    case PSetCar:
      va = CheckType(ctx, EVAL_ARG(), FeTPair);
      CAR(va) = EVAL_ARG();
      return res;
    case PSetCdr:
      va = CheckType(ctx, EVAL_ARG(), FeTPair);
      CDR(va) = EVAL_ARG();
      return res;
    case PList:
      return EvaluateList(ctx, arg, env);
    case PNot:
      return FeMakeBool(ctx, FeIsNil(EVAL_ARG()));
    case PIs:
      va = EVAL_ARG();
      return FeMakeBool(ctx, Equal(va, EVAL_ARG()));
    case PAtom:
      return FeMakeBool(ctx, FeGetType(EVAL_ARG()) != FeTPair);
    case PPrint:
      while (!FeIsNil(arg)) {
        FeWriteFile(ctx, EVAL_ARG(), stdout);
        if (!FeIsNil(arg)) {
          printf(" ");
        }
      }
      printf("\n");
      return res;
    case PLess:
      NUM_CMP_OP(<)
      return res;
    case PLessEqual:
      NUM_CMP_OP(<=)
      return res;
    case PAdd:
      ARITH_OP(+)
      return res;
    case PSub:
      ARITH_OP(-)
      return res;
    case PMul:
      ARITH_OP(*)
      return res;
    case PDiv:
      ARITH_OP(/)
      return res;
  }
  abort();
}

static FeObject* Evaluate(FeContext* ctx,
                          FeObject* obj,
                          FeObject* env,
                          FeObject** newenv) {
  EvaluationStep(ctx);
  if (FeGetType(obj) == FeTSymbol) {
    return CDR(GetBound(ctx, obj, env));
  }
  if (FeGetType(obj) != FeTPair) {
    return obj;
  }

  FeObject cl;
  CAR(&cl) = obj;
  CDR(&cl) = ctx->call_list;
  // This stack link is restored below or reset by FeHandleError before longjmp.
  // cppcheck-suppress autoVariables
  ctx->call_list = &cl;

  const size_t gc = FeSaveGC(ctx);
  FeObject* fn = Evaluate(ctx, CAR(obj), env, NULL);
  FeObject* arg = CDR(obj);
  FeObject* res = &nil;
  FeObject* va;
  FeObject* vb;

  switch (FeGetType(fn)) {
    case FeTPrimitive:
      res = EvaluatePrimitive(ctx, obj, env, newenv, fn);
      break;

    case FeTNativeFn:
      res = GetNativeFn(fn)(ctx, EvaluateList(ctx, arg, env));
      break;

    case FeTFn:
      arg = EvaluateList(ctx, arg, env);
      va = CDR(fn);  // (env params ...)
      vb = CDR(va);  // (params ...)
      res = DoList(ctx, CDR(vb), ArgsToEnv(ctx, CAR(vb), arg, CAR(va)));
      break;

    case FeTMacro:
      va = CDR(fn);  // (env params ...)
      vb = CDR(va);  // (params ...)
      // Replace caller object with code generated by macro and re-eval:
      *obj = *DoList(ctx, CDR(vb), ArgsToEnv(ctx, CAR(vb), arg, CAR(va)));
      FeRestoreGC(ctx, gc);
      ctx->call_list = CDR(&cl);
      return Evaluate(ctx, obj, env, NULL);

    case FeTPair:
    case FeTFree:
    case FeTNil:
    case FeTDouble:
    case FeTSymbol:
    case FeTString:
    case FeTPtr:
    case FeTFex0:
    case FeTFex1:
    case FeTFex2:
      FeHandleError(ctx, "tried to call non-callable value");

    case FeTSentinel:
      abort();
  }

  FeRestoreGC(ctx, gc);
  FePushGC(ctx, res);
  ctx->call_list = CDR(&cl);
  return res;
}

FeObject* FeEvaluate(FeContext* ctx, FeObject* obj) {
  return Evaluate(ctx, obj, &nil, NULL);
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

FeObject* FeCall(FeContext* ctx,
                 FeObject* callable,
                 FeObject* const* arguments,
                 size_t count) {
  if (FeGetType(callable) != FeTFn && FeGetType(callable) != FeTNativeFn) {
    FeHandleError(ctx, "tried to call non-callable value");
  }
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, callable);
  for (size_t i = 0; i < count; i++) {
    FePushGC(ctx, arguments[i]);
  }

  FeObject* quote = FeMakeSymbol(ctx, "quote");
  FeObject* forms = &nil;
  for (size_t i = count; i > 0; i--) {
    FeObject* value = FeCons(ctx, arguments[i - 1], &nil);
    value = FeCons(ctx, quote, value);
    forms = FeCons(ctx, value, forms);
  }
  ctx->call_result = Evaluate(ctx, FeCons(ctx, callable, forms), &nil, nullptr);
  FeRestoreGC(ctx, gc);
  return ctx->call_result;
}

FeObject* FeEvaluateWithOptions(FeContext* ctx,
                                FeObject* obj,
                                const FeEvalOptions* options) {
  const bool owns_control = BeginEvaluationControl(ctx, options);
  FeObject* result = FeEvaluate(ctx, obj);
  EndEvaluationControl(ctx, owns_control);
  return result;
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
  const size_t length = strlen(name);
  assert(length > 0);
  return 4 + (length - 1) / StringBufferSize;
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

static FeObject* native_expt(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  double y = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, pow(x, y));
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

static FeObject* native_floor(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeDouble(ctx, floor(x));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, floor(x / d));
}

static FeObject* native_ceiling(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeDouble(ctx, ceil(x));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, ceil(x / d));
}

static FeObject* native_round(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeDouble(ctx, nearbyint(x));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, nearbyint(x / d));
}

static FeObject* native_truncate(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeDouble(ctx, trunc(x));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, trunc(x / d));
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
  for (size_t i = 0; i < COUNT(math_names); i++) {
    count += 1 + GetSymbolObjectCount(math_names[i]);
  }
  return count;
}

size_t FeMinimumArenaSize(void) {
  return sizeof(FeArena) + GetCoreObjectCount() * sizeof(FeObject);
}

size_t FeArenaAlignment(void) {
  return alignof(FeArena);
}

FeContext* FeOpenContext(void* arena, size_t size) {
  uintptr_t arena_end;
  const uintptr_t arena_address = (uintptr_t)arena;
  bool invalid = arena == nullptr;
  invalid |= size < FeMinimumArenaSize();
  invalid |= arena_address % FeArenaAlignment() != 0;
  invalid |= ckd_add(&arena_end, arena_address, size);
  if (invalid) {
    return nullptr;
  }

  // Initialize the context:
  FeArena* storage = arena;
  FeContext* ctx = &storage->context;
  memset(ctx, 0, sizeof(FeContext));

  // Initialize the objects memory region:
  ctx->objects = storage->objects;
  ctx->object_count = (size - sizeof(FeArena)) / sizeof(FeObject);

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

  // Register the built-in primitives:
  const size_t save = FeSaveGC(ctx);
  for (Primitive i = PAssert; i < PSentinel; i++) {
    FeObject* v = MakeObject(ctx);
    SetType(v, FeTPrimitive);
    PRIM(v) = (char)i;
    FeSet(ctx, FeMakeSymbol(ctx, primitive_names[i]), v);
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

void FeCloseContext(FeContext* ctx) {
  // Clear the GC stack and symbol list: this makes all objects unreachable:
  ctx->gc_stack_index = 0;
  ctx->symbol_list = &nil;
  ctx->evaluation_result = &nil;
  ctx->call_result = &nil;
  ctx->root_list = &nil;
  CollectGarbage(ctx);
}
