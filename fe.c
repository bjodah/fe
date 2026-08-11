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

const char* FeVersion = "13.0";

// Collect before *every* arena allocation, so an object that is live only
// through an unrooted C local is reclaimed at the first opportunity rather
// than at whatever unrelated allocation happens to empty the free list. Off
// unless the build asks for it, the way kg's `KG_FUZZ` and
// `KG_PERF_COUNTERS` knobs are: the whole thing compiles to nothing at 0, so
// the shipped interpreter carries no test of it and no branch for it. It is
// never on by default because the cost is a full mark-and-sweep per `cons`.
//
// What it defends against, both measured elsewhere and neither one visible
// to an ordinary suite: a collector that is never actually invoked (a run
// whose collection counter reads 0 from end to end, with nothing asserting
// otherwise), and a use-after-free whose reproduction is *masked* by
// unrelated work that happens to reduce churn. A build where every
// allocation is a collection turns both into a first-run failure, and the
// arena-stats collection count is what says the knob is on rather than
// silently compiled out.
#ifndef FE_GC_STRESS
#define FE_GC_STRESS 0
#endif

#define COUNT(a) (sizeof((a)) / sizeof((a)[0]))

static const char* primitive_names[] = {
    [PAssert] = "assert",
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
    [PKeywordp] = "keywordp",
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
    [PApply] = "apply",
    [PCatch] = "catch",
    [PThrow] = "throw",
    [PConditionCase] = "condition-case",
    [PSignal] = "signal",
    [PError] = "error",
    [PMacroexpand1] = "macroexpand-1",
    [PMacroexpand] = "macroexpand",
    [PMacroexpandAll] = "macroexpand-all",
    [PMarkSpecial] = "internal--mark-special",
    [PSpecialVariableP] = "special-variable-p",
    [PEval] = "eval",
    [PIntern] = "intern",
    [PInternSoft] = "intern-soft",
    [PSymbolName] = "symbol-name",
    [PMakeSymbol] = "make-symbol",
    [PGensym] = "gensym",
    [PPut] = "put",
    [PGet] = "get",
    [PSymbolPlist] = "symbol-plist"};

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
      if (ctx->native_call_active) {
        RaiseNativeArity(ctx, ctx->native_identity, ctx->native_argc,
                         "too few arguments");
      }
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

static const char* TypePredicate(FeType type) {
  switch (type) {
    case FeTPair:
      return "listp";
    case FeTDouble:
      return "numberp";
    case FeTInteger:
      return "integerp";
    case FeTSymbol:
      return "symbolp";
    case FeTString:
      return "stringp";
    case FeTFn:
    case FeTMacro:
    case FeTPrimitive:
    case FeTNativeFn:
      return "functionp";
    case FeTPtr:
      return "user-ptr-p";
    case FeTFree:
    case FeTNil:
    case FeTFex0:
    case FeTFex1:
    case FeTFex2:
    case FeTSentinel:
      return "objectp";
  }
  return "objectp";
}

FeObject* CheckType(FeContext* ctx, FeObject* obj, FeType type) {
  if (FeGetType(obj) != type) {
    char message[64];
    Format(message, sizeof(message), "expected %s, got %s", GetTypeName(type),
           GetTypeName(FeGetType(obj)));
    FeObject* items[] = {FeMakeSymbol(ctx, TypePredicate(type)), obj};
    FeObject* data = FeMakeList(ctx, items, COUNT(items));
    RaiseCondition(ctx, FeCompletionError, "wrong-type-argument", data,
                   message);
  }
  return obj;
}

void FeRequireNoArguments(FeContext* ctx, const FeObject* args) {
  if (!FeIsNil(args)) {
    if (ctx->native_call_active) {
      RaiseNativeArity(ctx, ctx->native_identity, ctx->native_argc,
                       "too many arguments");
    }
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

// The ordinary ceiling stops `GcStackReserve` slots short of the array, and
// only a completion already in flight may spend the rest -- `AllocateFrame`'s
// reserve, applied to the other bounded stack. Reporting an overflow roots
// objects of its own, so a raise that started from a genuinely full stack
// came straight back here: `FeHandleError` built its condition string,
// `FeMakeString` called `MakeObject`, `MakeObject` called `FePushGC`, and the
// C stack died before the Lisp error ever surfaced.
void FePushGC(FeContext* ctx, FeObject* obj) {
  const size_t limit = ctx->completion == FeCompletionNormal
                           ? GcStackSize - GcStackReserve
                           : GcStackSize;
  if (ctx->gc_stack_index >= limit) {
    RaiseGcStackOverflow(ctx);
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

// The reversed link a pair carries while the mark phase is inside one of its
// children shares a word with the cell's tag, so taking it back means clearing
// the collector's two flag bits before reading the pointer. This clears them
// in place rather than returning a masked copy because every caller overwrites
// the field immediately afterwards -- and it stays a bit operation on the tag
// byte, the way the rest of fe manipulates these bits and the way the sweep's
// own `TAG(obj) &= ~GcMarkBit` does, rather than a round trip through
// `uintptr_t`.
static FeObject* TakeMarkLink(FeObject* obj) {
  TAG(obj) &= ~(GcMarkBit | GcMarkCdrBit);
  return CAR(obj);
}

// The mark phase, with the C stack out of it (sub-plan 09C).
//
// The old walk recursed once per `car` level, so a chain of `car`s cost C
// stack in proportion to its depth and a deep enough one took the process
// down inside the collector. Every figure below carries the build it was
// measured in, because the frame size differs by nearly a factor of three
// across them: 48 bytes a frame in this tree's default build (clang, `-O1`),
// 32 at `-Os`, 64 under ASan, 80 under MSan.
//
// In fe's own default build, bisection on an 8 MiB stack put the SIGSEGV
// between 130 000 and 150 000 levels, which is reachable from pure Lisp in
// any arena of about 4.9 MiB. (A figure of ~262 000 levels appears in older
// notes; that is the same 8 MiB stack divided by the `-Os` frame, an
// extrapolation rather than a measurement, and it is not this build's
// answer.)
//
// In kg the failure was measured directly rather than derived, on two kg
// binaries differing only in their fe (kg's `-Os` gcc build, 1 MiB arena):
// at `ulimit -s 1280` *neither* crashed, and the old walk's crash reproduces
// at 512 and at 256. An earlier note here claimed 1280, which the
// measurement falsifies -- the point stands with the correct number, since a
// 256 KiB thread stack is an ordinary thing for an embedder to have.
//
// The `cdr` spine was already a loop; this makes the `car` edge one too, for
// every shape, with no bound to tune.
//
// The mechanism is Deutsch-Schorr-Waite pointer reversal: the walk stores its
// own return path in the objects it is walking, so it needs no stack and
// allocates nothing -- which matters here more than anywhere else in fe,
// because the collector is the one thing that runs *after* allocation has
// already failed. The alternative, an explicit worklist, is recorded in the
// commit that landed this; the short version is that bounding one for any data
// shape costs a slot per object, which for kg's 1 MiB arena is 56 224 pointers
// (439 KiB, 43% of the whole arena), and growing one instead puts a failable
// allocation inside collection.
//
// The encoding is two spare low bits of a *pair's* `car` word. Every object is
// at least 8-byte aligned, so bits 0-2 of a pointer stored there are free: bit
// 0 stays clear so `FeGetType` keeps answering `FeTPair`, bit 1 is `GcMarkBit`
// (which the recursive walk already stored inside a pair's car pointer -- that
// is what the sweep's `TAG(obj) &= ~GcMarkBit` puts back), and bit 2 is
// `GcMarkCdrBit`, which says which half of the pair the walk is in:
//
//   car half   car = parent   | mark            cdr = the pair's cdr
//   cdr half   car = own car  | mark | cdr bit  cdr = parent
//   finished   car = own car  | mark            cdr = the pair's cdr
//
// An object with exactly one pointer child -- `fn`, `macro`, `symbol`,
// `string`, whose `car` word is a type tag plus, for a string, seven bytes of
// text, and never a pointer -- needs no state bit: its link is always in
// `cdr`, and the ascent tells it from a pair by its type.
//
// Two invariants make this safe. Every intermediate state keeps `GcMarkBit`
// set and bit 0 clear, so `FeGetType` never lies about a cell and a cycle that
// comes back around stops at the mark check exactly as it did before; and
// every path out restores the cell it leaves, so the sweep sees the tags the
// recursive walk would have left. The graph really is scrambled while the walk
// is inside it, but collection is stop-the-world, so the only code that can
// observe that is `mark_fn` -- which may call `FeMark` (an object the walk is
// inside is already marked, so a nested walk stops at it immediately) but must
// not read `car`/`cdr` of anything but the object it was handed. `doc/c-api.md`
// says so.
void FeMark(FeContext* ctx, FeObject* obj) {
  FeObject* parent = nullptr;
  FeObject* current = obj;

descend:
  if (~TAG(current) & GcMarkBit) {
    // Read `car` before the mark bit goes in: on a pair, the word the mark
    // bit lives in *is* the car pointer. (Meaningless on any other cell,
    // where `car` is a tag -- and never dereferenced there, exactly as in the
    // recursive walk this replaces.)
    FeObject* const car = CAR(current);
    // The `FeTPair` arm below overwrites this whole word with the reversed
    // link and puts the bit back itself, so for a pair this store is dead. It
    // stays because the alternative is the same store repeated in each of the
    // three arms that do keep it, and because "mark, then decide what kind of
    // cell this is" is the order the recursive walk had and the order the
    // invariant is stated in: every intermediate state has `GcMarkBit` set.
    TAG(current) |= GcMarkBit;
    switch (FeGetType(current)) {
      case FeTPair:
        // The reversed link goes in `car`, which is also where the mark bit
        // lives, so the mark has to go back in after the pointer.
        CAR(current) = parent;
        TAG(current) |= GcMarkBit;
        parent = current;
        current = car;
        goto descend;

      case FeTFn:
      case FeTMacro:
      case FeTSymbol:
      case FeTString: {
        FeObject* const child = CDR(current);
        CDR(current) = parent;
        parent = current;
        current = child;
        goto descend;
      }

      case FeTPtr:
      case FeTFex0:
      case FeTFex1:
      case FeTFex2:
        if (ctx->mark_fn) {
          ctx->mark_fn(ctx, current);
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

ascend:
  if (parent == nullptr) {
    return;
  }
  if (FeGetType(parent) == FeTPair && (~TAG(parent) & GcMarkCdrBit)) {
    // The car half is finished: put the car child back with the half flag
    // set, hand the reversed link to `cdr`, and go down the cdr.
    FeObject* const grandparent = TakeMarkLink(parent);
    CAR(parent) = current;
    TAG(parent) |= GcMarkBit | GcMarkCdrBit;
    current = CDR(parent);
    CDR(parent) = grandparent;
    goto descend;
  }
  {
    // The cdr half of a pair, or the only child of a one-child object, is
    // finished: put the child back, clear the half flag, step up.
    FeObject* const grandparent = CDR(parent);
    CDR(parent) = current;
    if (FeGetType(parent) == FeTPair) {
      TAG(parent) &= ~GcMarkCdrBit;
    }
    current = parent;
    parent = grandparent;
    goto ascend;
  }
}

static void MarkCleanupRoots(FeContext* ctx) {
  for (size_t i = 0; i < ctx->cleanup_stack_index; i++) {
    const FeCleanupEntry* entry = &ctx->cleanup_stack[i];
    if (entry->kind == FeCleanupLisp) {
      FeMark(ctx, entry->as.lisp.forms);
      FeMark(ctx, entry->as.lisp.env);
    }
    // A shadowed global value (sub-plan 11B) is reachable from nowhere else
    // for as long as the binding is in force -- that is what shallow binding
    // means -- so the entry holding it is its only root. The symbol itself
    // is on `symbol_list` and would survive anyway; marking it here keeps
    // the entry's rooting one statement rather than an argument about
    // another structure's lifetime, exactly as `native_identity` is marked
    // in `CollectGarbage` for the same reason.
    if (entry->kind == FeCleanupBinding) {
      FeMark(ctx, entry->as.binding.symbol);
      FeMark(ctx, entry->as.binding.value);
    }
  }
}

static void CollectGarbage(FeContext* ctx) {
  // Re-entering here means a `mark_fn` or `gc_fn` allocated, which the
  // contract forbids. It is not survivable: this call's sweep would clear the
  // *outer* walk's mark bits, and the outer walk would then descend back into
  // cells whose `car` holds a tagged parent pointer. Measured: the route is
  // `mark_fn` -> `FeHandleError` -> `ArenaCanAllocate`, which collects
  // precisely because the free list is what ran out and started the outer
  // collection in the first place.
  if (ctx->collecting) {
    FatalCollectorViolation("a callback allocated", nullptr);
  }
  ctx->arena_collection_count++;
  // Stop-the-world means exactly this flag: from here to the last line of
  // the sweep the graph is the mark phase's own working state, and a raise
  // out of `mark_fn` or `gc_fn` would abandon it half-reversed. See
  // `collecting`'s comment on `struct FeContext`.
  ctx->collecting = true;
  // Mark:
  for (size_t i = 0; i < ctx->gc_stack_index; i++) {
    FeMark(ctx, ctx->gc_stack[i]);
  }
  FeMark(ctx, ctx->symbol_list);
  FeMark(ctx, ctx->special_list);
  FeMark(ctx, ctx->evaluation_result);
  FeMark(ctx, ctx->call_result);
  FeMark(ctx, ctx->root_list);
  FeMark(ctx, ctx->condition);
  // The running native's callable. While the native runs this is also
  // reachable through the frame that invoked it, but relying on that made
  // the record's lifetime depend on a second structure's; marking it here
  // makes "native_identity is a live object whenever it is not nil" a local
  // invariant instead of a cross-module argument.
  FeMark(ctx, ctx->native_identity);
  FeMark(ctx, ctx->pending_throw_tag);
  FeMark(ctx, ctx->pending_throw_value);
  // The pre-built exhaustion conditions (09B). They are reachable from
  // `condition` only while one of them is the completion in flight, and the
  // whole point of them is to be raiseable from a state where nothing can be
  // allocated -- so they are roots in their own right, for the life of the
  // context.
  FeMark(ctx, ctx->arena_exhaustion_condition);
  FeMark(ctx, ctx->evaluation_stack_exhaustion_condition);
  FeMarkEvaluatorRoots(ctx);
  MarkCleanupRoots(ctx);

  // Sweep and unmark:
  for (size_t i = 0; i < ctx->object_count; i++) {
    FeObject* obj = &ctx->objects[i];
    if (FeGetType(obj) == FeTFree) {
      continue;
    }
    // Nothing else in the tree notices a leaked half flag: the sweep below
    // clears `GcMarkBit` and never touches bit 2, so a `FeMark` path that
    // forgot to restore one would leave a pair permanently claiming the walk
    // is inside its cdr -- and the *next* collection would take the wrong
    // branch on the ascent. Every path out of `FeMark` restores it; this is
    // the check that says so. It costs one comparison per live object in a
    // loop that already does several, and it is compiled out by `NDEBUG` for
    // an embedder that wants even that back -- nothing in this repository
    // defines it, so every fe build and every CI lane runs the check. The
    // test is restricted to pairs because on any other cell that bit is the
    // low bit of the type (`type << GcMarkBit | OtherCell`), not a flag.
    assert(FeGetType(obj) != FeTPair || (~TAG(obj) & GcMarkCdrBit));
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
  ctx->collecting = false;
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
// across an integer and a double, preserving `(is 1 1.0)` -> t. The integer
// converts to double and the pair then goes through the *same*
// `IsNearlyEqual` tolerance a double/double pair gets, which is the whole of
// Decision 2 ("mathematical value across int/float, epsilon retained"): a
// mixed pair may not be stricter than the same two values both spelled as
// doubles, or `(is 3 (cube-root 27))` and `(is 3.0 (cube-root 27))` answer
// differently for no reason a caller can see. `eq` and `eql` are the exact
// comparisons and never come through here -- they are Emacs semantics, where
// `is` is fe's own broad comparator and its documented contract is the
// tolerant one. The int64 converts exactly to double up to 2^53; beyond that
// the comparison is the value the two numbers share in doubles, the same
// approximation the arithmetic tower's mixed promotion uses.
static bool IntegerAndDoubleEqual(int64_t i, double d) {
  return IsNearlyEqual((FeDouble)i, d, DBL_EPSILON);
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

// Measured, GNU Emacs 31.0.90: `(keywordp :)` is t, `:` self-evaluates to
// `:`, and `(setq : 1)` raises `(setting-constant :)`. 08B guessed a length
// of at least 2 instead of measuring, which left the one-character keyword an
// ordinary, unbound symbol.
static bool IsKeywordName(const char* name) {
  return name[0] == ':';
}

static FeObject* CheckWritableSymbol(FeContext* ctx, FeObject* sym);

// The obarray lookup, shared by `FeMakeSymbol` and Phase 14's `intern-soft`.
// `nullptr` for a miss is what makes `intern-soft` a probe: it is the ONE
// contract this family has that a plausible implementation gets wrong, and
// getting it wrong is unbounded allocation rather than a wrong answer --
// `(while (setq x (intern-soft (format ...))) ...)` is a real idiom, and an
// intern-on-miss makes it never terminate.
static FeObject* FindInternedSymbol(FeContext* ctx, const char* name) {
  for (FeObject* rest = ctx->symbol_list; !FeIsNil(rest); rest = CDR(rest)) {
    if (IsStringEqual(SymbolName(CAR(rest)), name)) {
      return CAR(rest);
    }
  }
  return nullptr;
}

static void InitializeKeywordValue(FeObject* symbol, const char* name) {
  if (IsKeywordName(name)) {
    CDR(SymbolBindingCell(symbol)) = symbol;
  }
}

// "Could an allocation still succeed?", which is what the raise paths mean
// when they ask whether they can build a condition object. `FeIsNil(free_list)`
// on its own answers a narrower question -- whether the free list happens to
// be empty at this instant -- and that is true after any allocation that took
// the last cell, with a whole arena of collectable garbage behind it. Reading
// that as exhaustion silently downgraded a structured condition to a bare,
// uncatchable one whenever a raise landed on that boundary: under memory
// pressure `(condition-case e (car 1 2) (wrong-number-of-arguments e))`
// escaped its own handler.
bool ArenaCanAllocate(FeContext* ctx) {
  if (!FeIsNil(ctx->free_list)) {
    return true;
  }
  CollectGarbage(ctx);
  return !FeIsNil(ctx->free_list);
}

FeObject* MakeObject(FeContext* ctx) {
#if FE_GC_STRESS
  // Every allocation is a collection point under the stress knob. This runs
  // before the free-list test rather than instead of it: the test below is
  // still the one that decides exhaustion, so an arena that genuinely cannot
  // satisfy the request raises `out of memory` here exactly as it does in an
  // ordinary build.
  CollectGarbage(ctx);
#endif
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

// A symbol object with nothing bound and nothing interned. A symbol's cdr is
// one cons: `(((name . plist) . function) . value)` (sub-plan 04B, widened by
// Phase 14). The function cell is initialized to `&unbound` and written only
// through `SetSymbolFunction`; the name moved one pair down in 04B and a
// second one in Phase 14, both times so the binding cell -- the cdr of the
// outer pair -- is unchanged, which is what keeps the whole value path
// (`GetBound`, `FeSet`, `FeIsBound`, `ResumeSetq`) untouched.
//
// Every intermediate here is on the GC stack the moment `MakeObject` returns
// it, so the four allocations may collect between one another without losing
// a half-built symbol.
//
// This is also Phase 14's `make-symbol`, called directly: a symbol object
// that is on no list at all. Such a symbol is the first one fe has that the
// collector may reclaim -- `symbol_list` is a root, so an interned symbol
// lives as long as its context -- which is why the plist lives in the object
// and not in a context-side registry. `InitializeKeywordValue` is NOT part of
// it and is applied by `FeMakeSymbol` alone; see `IsKeywordSymbol`.
static FeObject* MakeSymbolObject(FeContext* ctx, const char* name) {
  FeObject* const obj = MakeObject(ctx);
  SetType(obj, FeTSymbol);
  CDR(obj) = FeCons(
      ctx, FeCons(ctx, FeCons(ctx, FeMakeString(ctx, name), &nil), &unbound),
      &unbound);
  return obj;
}

FeObject* FeMakeSymbol(FeContext* ctx, const char* name) {
  FeObject* obj = FindInternedSymbol(ctx, name);
  if (obj != nullptr) {
    return obj;
  }
  // Create new object, push to symbol_list and return.
  obj = MakeSymbolObject(ctx, name);
  InitializeKeywordValue(obj, name);
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
  CheckWritableSymbol(ctx, symbol);
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
  // Public, because `main.c` bounds its escaping-raise trace by the same
  // number and used to say "256" a second time to do it (09A Decision 5).
  DefaultWriteMaxDepth = FeWriteDefaultMaxDepth,
};

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
// The classification lives above the writer because Phase 14's symbol
// printer asks the same question of a symbol's name: a name Emacs would
// read back as a number is printed with its first byte escaped.
typedef enum NumberKind {
  NumberSymbol,
  NumberInteger,
  NumberFloat,
  NumberInf,
  NumberNan,
} NumberKind;

typedef struct Writer {
  FeContext* ctx;
  FeWriteFn* fn;
  void* udata;
  size_t bytes;
  size_t nodes;
  // The `qt` every *nested* object is written with. The public writers pin
  // it at 1, which is Fe's historical shape: the top-level `qt` argument
  // decides whether a bare string is quoted, and everything inside a list is
  // quoted regardless. `RenderObject` is the one entry point that propagates
  // its own `qt` all the way down instead, which is what Emacs' `princ`
  // (`qt` 0 everywhere) and `prin1` (`qt` 1 everywhere) are, and what
  // `error`'s `%s` and `%S` directives need.
  int nested_qt;
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

static size_t CopyStoredStringBytes(const FeObject* string, char* dst);
static NumberKind ClassifyNumber(const char* buf);

// The bytes Emacs escapes wherever they occur in a symbol name, measured
// byte for byte on 31.0.90 by printing `(intern (string C ?a ?b))` and
// `(intern (string ?a C ?b))` for every C from 1 to 127: everything at or
// below the space, and `"#'(),;[]\` and the backquote. Each one is reader
// syntax somewhere, in Emacs or here or both.
static bool IsAlwaysEscapedSymbolByte(char chr) {
  const unsigned char byte = (unsigned char)chr;
  return byte <= ' ' || strchr("\"#'(),;[]\\`", byte) != nullptr;
}

static bool IsAsciiLetter(char chr) {
  return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z');
}

// Does the FIRST byte need a backslash so the reader does not take the whole
// name for something else? The same measurement says three things do: a name
// that reads as a number (`1`, `+1`, `.5`, `1e5`, `1.0e+INF`), a name
// starting with `?` (a character literal), and a name starting with `.`
// whose second byte is not an ASCII letter -- `.`, `..` and `.5x` are
// escaped, `.emacs` and `.x-` are not. Emacs escapes only that first byte,
// not the rest of the token.
static bool IsConfusingSymbolName(const char* name) {
  if (name[0] == '?') {
    return true;
  }
  if (name[0] == '.') {
    return !IsAsciiLetter(name[1]);
  }
  return ClassifyNumber(name) != NumberSymbol;
}

// A symbol's name, escaped so that reading the output back gives this symbol
// again (Phase 14). The empty name is `##`, which is how Emacs both writes
// and reads it. Never recurses: the cells are a cdr chain.
//
// The whole-name test needs the name in one buffer, and `SymbolNameLimit` is
// the longest name fe itself ever builds; a longer one can only come from an
// embedder's `FeMakeSymbol`, and it gets the per-byte escapes without the
// number-lookalike test, which no name that long can pass here anyway.
static void EmitSymbolName(Writer* w, FeObject* name) {
  const size_t length = CopyStoredStringBytes(name, nullptr);
  if (length == 0) {
    EmitString(w, "##");
    return;
  }
  bool escape_first = false;
  if (length <= SymbolNameLimit) {
    char buf[SymbolNameLimit + 1];
    (void)CopyStoredStringBytes(name, buf);
    buf[length] = '\0';
    escape_first = IsConfusingSymbolName(buf);
  }
  bool first = true;
  while (!FeIsNil(name)) {
    for (size_t i = 0; i < StringBufferSize && STRING_BUFFER(name)[i]; i++) {
      const char chr = STRING_BUFFER(name)[i];
      if ((first && escape_first) || IsAlwaysEscapedSymbolByte(chr)) {
        Emit(w, '\\');
      }
      Emit(w, chr);
      first = false;
    }
    name = CDR(name);
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
// point or an exponent, so a bare integer text like `100` gets `.0`
// appended, while `0.1` already carries a point and `1e+20` already carries
// an exponent and neither is touched. That single appended `.0` is the whole
// fixup -- `%g` never emits a trailing `.` with nothing after it, so there
// is no `100.` case to repair. The old integral-double shortcut (a double
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
    WriteObject(w, CAR(obj), w->nested_qt, depth);
    FeObject* next = CDR(obj);
    if (FeGetType(next) != FeTPair) {
      if (!FeIsNil(next)) {
        EmitString(w, " . ");
        WriteObject(w, next, w->nested_qt, depth);
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
    WriteObject(w, obj, w->nested_qt, depth);
  }
  Emit(w, ')');
}

// The two reader-macro abbreviations the writer produces: `(function X)` as
// `#'X` (the writer half of sub-plan 04D's `#'` change, and the exact shape
// the pinned `reader-sharp-quote-identity` snapshot compares against) and
// `(quote X)` as `'X` (sub-plan 11C, 11A Decision 4 -- what Emacs' printer
// has always done, and the divergence the compat manifest recorded as
// `writer-quote-abbreviation`).
//
// Both fire only for the single-element *proper* form, which is the one the
// special form itself accepts. `(quote x y)`, `(quote)` and `(quote . x)`
// print as the ordinary pairs they are -- measured on Emacs 31.0.90, which
// prints all three that way too. The recursion through `WriteObject` is what
// makes `''x` and `(a 'b c)` come out as Emacs prints them, and it spends
// `depth` exactly as the pair arm below would, so an abbreviation cannot buy
// a level of depth the bounded writer would otherwise have refused.
//
// Backquote is deliberately not here (11A Decision 4): kg's reader expands
// to the ordinary symbols `quasiquote`/`unquote`/`unquote-splicing` where
// Emacs uses `` \` ``/`\,`/`\,@`, and Emacs' comma abbreviation is
// context-sensitive, so closing that half means changing what the *reader*
// produces. It stays the recorded `phase8-reader-backquote-symbol-names`
// divergence.
static bool EmitAbbreviation(Writer* w, FeObject* obj, size_t depth) {
  const char* prefix;
  if (IsNamedSymbol(CAR(obj), "function")) {
    prefix = "#'";
  } else if (IsNamedSymbol(CAR(obj), "quote")) {
    prefix = "'";
  } else {
    return false;
  }
  FeObject* const form = CDR(obj);
  if (FeGetType(form) != FeTPair || !FeIsNil(CDR(form))) {
    return false;
  }
  EmitString(w, prefix);
  WriteObject(w, CAR(form), w->nested_qt, depth - 1);
  return true;
}

static void WriteObject(Writer* w, FeObject* obj, int qt, size_t depth) {
  char buf[32];
  // Printing is work: during a controlled evaluation it spends the step budget
  // and polls the interrupt, so `(print cyclic-thing)` answers C-g. Not while
  // collecting, though: `mark_fn`/`gc_fn` may print the object they were
  // handed (`main.c` does), a step charge or an interrupt poll raises, and a
  // raise from inside the mark phase is fatal by contract. Collection is not
  // evaluation and does not charge for one.
  if (w->ctx->evaluation_active && !w->ctx->collecting) {
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
      if (EmitAbbreviation(w, obj, depth)) {
        break;
      }
      Emit(w, '(');
      WriteElements(w, obj, depth - 1);
      Emit(w, ')');
      break;

    case FeTSymbol:
      EmitSymbolName(w, SymbolName(obj));
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
      .nested_qt = 1,
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

// `FeToString`, but applying `qt` at every level instead of only the top:
// `qt` 0 is Emacs' `princ` (no string quoting anywhere) and 1 its `prin1`
// (quoting everywhere), the two renderings `error`'s `%s` and `%S`
// directives are defined as. Internal, because the public `FeToString` and
// `FeWrite` keep the shape their callers already depend on.
size_t RenderObject(FeContext* ctx,
                    FeObject* obj,
                    char* dst,
                    size_t size,
                    int qt) {
  if (size == 0) {
    return 0;
  }
  SizedString s = {.string = dst, .size = size - 1};
  Writer w = {
      .ctx = ctx,
      .fn = WriteBuffer,
      .udata = &s,
      .bytes = size,
      .nodes = DefaultWriteMaxNodes,
      .nested_qt = qt,
      .complete = true,
  };
  WriteObject(&w, obj, qt, DefaultWriteMaxDepth);
  *s.string = '\0';
  return size - s.size - 1;
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

static FeObject* CheckWritableSymbol(FeContext* ctx, FeObject* sym) {
  if (FeIsNil(sym) || IsConstantSymbol(sym)) {
    RaiseCondition(ctx, FeCompletionError, "setting-constant",
                   FeMakeList(ctx, (FeObject*[]){sym}, 1), "setting-constant");
  }
  return CheckType(ctx, sym, FeTSymbol);
}

void FeSet(FeContext* ctx, FeObject* sym, FeObject* v) {
  CDR(GetBound(ctx, CheckWritableSymbol(ctx, sym), &nil)) = v;
}

bool FeIsBound(FeContext* ctx, FeObject* sym) {
  return CDR(GetBound(ctx, CheckType(ctx, sym, FeTSymbol), &nil)) != &unbound;
}

// The read half of `FeSet` (FE_API_VERSION 9). `&unbound` is fe's own
// sentinel and never escapes into a host's hands, so it becomes `nullptr`
// here: a caller that stashes a value away and puts it back later needs to
// tell "no value" from "the value nil", and a sentinel it cannot name is no
// use to it.
FeObject* FeGetValue(FeContext* ctx, FeObject* sym) {
  FeObject* const value =
      CDR(GetBound(ctx, CheckType(ctx, sym, FeTSymbol), &nil));
  return value == &unbound ? nullptr : value;
}

// `makunbound`'s global arm, spelled for a host. The constant check is
// `FeSet`'s, not a second policy: `(makunbound t)` is `setting-constant`
// from Lisp too.
void FeMakeUnbound(FeContext* ctx, FeObject* sym) {
  CDR(GetBound(ctx, CheckWritableSymbol(ctx, sym), &nil)) = &unbound;
}

// Sub-plan 04C/04D: the function namespace's public surface. `FeSetFunction`
// and `FeIsFBound` are the cell accessors (`FeGetFunction` lives in fe_eval.c
// with the evaluator, because following the designator chain charges the
// step budget and can raise `cyclic-function-indirection`). Since 04D's cut
// the bootstrap lives in function cells too, so these reach what call
// position resolves.
void FeSetFunction(FeContext* ctx, FeObject* sym, FeObject* fn) {
  SetSymbolFunction(CheckWritableSymbol(ctx, sym), fn);
}

bool FeIsFBound(FeContext* ctx, FeObject* sym) {
  return SymbolFunction(CheckType(ctx, sym, FeTSymbol)) != &unbound;
}

// Symbol accessors (sub-plan 04B of kg's Emacs-subset program): a symbol's
// `cdr` is one cons, `(((name . plist) . function) . value)` since Phase 14
// widened the name slot into a pair, and every reader of that
// private layout goes through these. The value path deliberately has no
// accessor of its own: its unit of currency is the binding cell, and lexical
// environment entries and the global cell share the `CDR(cell)` read/write
// contract `GetBound` depends on. The function cell is the Lisp-2 callable
// home (written by `FeSetFunction`/`FeDefineNative`, read by call position,
// `funcall`/`apply`, `symbol-function` and `FeGetFunction`), and
// `SymbolName`/`SymbolBindingCell` read through a `const FeObject*` because
// `GetStringObject`/`IsNamedSymbol` do.
FeObject* SymbolName(const FeObject* sym) {
  return CAR(CAR(CAR(CDR(sym))));
}

FeObject* SymbolPlist(FeObject* sym) {
  return CDR(CAR(CAR(SymbolBindingCell(sym))));
}

void SetSymbolPlist(FeObject* sym, FeObject* plist) {
  CDR(CAR(CAR(SymbolBindingCell(sym)))) = plist;
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

// The special-variable registry (sub-plan 11B of kg's Emacs-subset program).
// `ctx->special_list` is a list of `(SYMBOL FULL-P . SCOPE)` triples; see its
// comment on `struct FeContext` for why it is a list and not a bit in the
// symbol, and for what SCOPE is.
static FeObject* FindSpecialEntry(FeContext* ctx, const FeObject* sym) {
  for (FeObject* rest = ctx->special_list; !FeIsNil(rest); rest = CDR(rest)) {
    if (CAR(CAR(rest)) == sym) {
      return CAR(rest);
    }
  }
  return nullptr;
}

// The scope an entry made *now* should carry: nil -- global -- for a full
// mark and for one made outside any input unit (a host context, where there
// is no unit to scope it to), and this unit's number otherwise.
static FeObject* CurrentMarkScope(FeContext* ctx, bool full) {
  if (full || ctx->input_scope == 0) {
    return &nil;
  }
  return FeMakeInteger(ctx, (int64_t)ctx->input_scope);
}

void MarkSpecialSymbol(FeContext* ctx, FeObject* sym, bool full) {
  FeObject* entry = FindSpecialEntry(ctx, sym);
  if (entry != nullptr) {
    // Idempotent, and one-way in the direction the flags can go: marking
    // full over let-dynamic-only upgrades (a one-arg `defvar` followed by a
    // two-arg one) and takes the global scope with it, while marking
    // let-dynamic-only over full does nothing at all. Emacs has no
    // unmarking, so neither does this.
    //
    // The SCOPE of a still-let-dynamic-only entry is *re-stamped* rather
    // than added to: one entry per symbol keeps the registry bounded by the
    // number of marked symbols, as it has always been, instead of growing by
    // one cell pair every time a file that declares a name is loaded again.
    // The cost is the last-mark-wins edge, recorded in the manifest.
    if (full) {
      CAR(CDR(entry)) = ctx->t;
      CDR(CDR(entry)) = &nil;
    } else if (FeIsNil(CAR(CDR(entry)))) {
      CDR(CDR(entry)) = CurrentMarkScope(ctx, false);
    }
    return;
  }
  const size_t gc = FeSaveGC(ctx);
  FeObject* const scope = CurrentMarkScope(ctx, full);
  FePushGC(ctx, scope);
  entry = FeCons(ctx, sym, FeCons(ctx, full ? ctx->t : &nil, scope));
  ctx->special_list = FeCons(ctx, entry, ctx->special_list);
  FeRestoreGC(ctx, gc);
}

bool SymbolIsSpecial(FeContext* ctx, const FeObject* sym) {
  const FeObject* const entry = FindSpecialEntry(ctx, sym);
  return entry != nullptr && !FeIsNil(CAR(CDR(entry)));
}

// Whether `let` over `sym` binds dynamically. A full mark (Emacs' two-arg
// `defvar`/`defconst`) is global and always answers yes; a let-dynamic-only
// mark (Emacs' one-arg `(defvar v)`) answers yes only while the input unit
// that made it is the one being evaluated -- sub-plan 12C Part 2.
//
// Measured on Emacs 31.0.90, `lexical-binding: t`, real files, 2026-08-07:
// a one-argument `(defvar v)` in file A makes `let` over v dynamic in A and
// leaves it lexical in a file B loaded afterwards; a file C that A loads
// does NOT see A's mark, and A does not see C's after C returns; and each
// `eval` is its own scope. All four are what a save-and-restore of one
// number gives, with an equality test rather than a `>=` one -- a `>=` test
// would let an outer unit's mark reach a nested load, which is the direction
// the B-loads-D probe measured false.
//
// THE RESIDUAL, measured in the same session and recorded rather than
// defended (see the manifest row `one-arg-defvar-scope-carrier`): Emacs'
// mechanism is not a per-unit registry at all but an entry in the *lexical
// environment*, which a `lambda` captures. So in Emacs a function defined in
// A after the `defvar` still binds v dynamically when it is called from B,
// and a function defined in A *before* the `defvar` does not. Fe consults
// this at `let` execution time and has nowhere to record a closure's unit,
// so it answers those two by where the `let` runs rather than by where it
// was written. Closing that means the marking becoming a lexical-environment
// entry, which is a different and larger design.
//
// Outside any input unit -- `ctx->input_scope` 0, which is a host context: a
// `FeCall` into a callable, a host-driven `let`, or this predicate asked
// straight from C -- every mark is visible. There is no unit there for a
// mark to be foreign to, and answering no would narrow the host's view of
// names a file legitimately declared. That keeps the scoping aimed at the
// divergence that was measured, one file's mark reaching another file's
// `let`, and off the paths that never had it.
bool SymbolIsLetDynamic(FeContext* ctx, const FeObject* sym) {
  const FeObject* const entry = FindSpecialEntry(ctx, sym);
  if (entry == nullptr) {
    return false;
  }
  FeObject* const flags = CDR(entry);
  if (!FeIsNil(CAR(flags)) || ctx->input_scope == 0) {
    return true;
  }
  FeObject* const scope = CDR(flags);
  return FeIsNil(scope) || (size_t)FeToInteger(ctx, scope) == ctx->input_scope;
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

// `p` points at the `e`/`E` of an exponent; the significand has been
// consumed. A `+`/`-` sign, digits, or the exact `INF`/`NaN` spellings decide
// a float from a symbol -- `1e3`, `1e+5`, `1e-7` are floats, `1e`, `1e+`,
// `1e+Inf`, `1.0e+NAN` are symbols, and the nonfinite spellings need an
// *explicit* `+`: `1e+INF`/`1E+INF`/`1e+NaN` are nonfinite, while `1e-INF`,
// `1eINF` and `1eNaN` are symbols, exactly as the pinned Emacs answers
// (`(read "1eINF")` is the symbol `1eINF`). A missing sign is not a positive
// sign here, which is why the flag records that a `+` was seen rather than
// that a `-` was not. The sign of a nonfinite spelling is read from the
// token's leading `+`/`-` by `ReadAtom`, not here. Digit runs use `strspn`,
// which the analyzer models as reading a NUL-terminated string (a bare
// `while (digit(*p))` loop it cannot bound).
static NumberKind ClassifyExponent(const char* p) {
  p++;
  bool exponent_plus = false;
  if (*p == '+' || *p == '-') {
    exponent_plus = *p == '+';
    p++;
  }
  if (exponent_plus) {
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
  char buf[SymbolNameLimit + 1];
  char* p = buf;
  const char* delimiter = " \n\t\r();`,";
  bool escaped = false;
  do {
    // Emacs' symbol escapes (Phase 14): a backslash takes the next byte into
    // the name literally, whatever it is -- a delimiter, a digit, another
    // backslash -- so `a\ b` is the one symbol whose name is "a b", and `\1`
    // is the symbol named "1" rather than the integer. It also suppresses
    // the number classification below for the whole token, which is Emacs'
    // rule: one escape anywhere means the token is a symbol.
    if (chr == '\\') {
      chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
      ctx->nextchr = '\0';
      if (chr == '\0') {
        FeHandleError(ctx, "unterminated symbol escape");
      }
      escaped = true;
    }
    if (p == buf + sizeof(buf) - 1) {
      FeHandleError(ctx, "symbol too long (63-byte limit)");
    }
    *p++ = chr;
    chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
    ctx->nextchr = '\0';
  } while (chr && !strchr(delimiter, chr));
  *p = '\0';
  ctx->nextchr = chr;
  ctx->reader_atom_escaped = escaped;
  if (escaped) {
    // `\nil` is the symbol `nil`, which in fe is the nil object -- the same
    // answer `(intern "nil")` gives, and Emacs' too, since its reader is
    // `intern` of the accumulated name.
    return strcmp(buf, "nil") == 0 ? &nil : FeMakeSymbol(ctx, buf);
  }
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

// The two modifier bits Emacs sets on a character that has no plain encoding,
// and the largest character Fe reads. Both were measured against GNU Emacs
// 31.0.90: `?\C-%` is 2^26 + 37 and `?\M-a` is 2^27 + 97.
enum {
  CharControlBit = 1 << 26,
  CharMetaBit = 1 << 27,
  MaxCharacter = 0x10ffff,
};

static int HexDigit(char chr) {
  if (chr >= '0' && chr <= '9')
    return chr - '0';
  if (chr >= 'a' && chr <= 'f')
    return chr - 'a' + 10;
  if (chr >= 'A' && chr <= 'F')
    return chr - 'A' + 10;
  return -1;
}

// Emacs reads `\x` greedily and to any width: `"\x41f"` is U+041F and
// `"\x0041"` is "A". Reading exactly two digits answered "Af" and "41" -- two
// silent misreads of a perfectly ordinary Emacs spelling. The bound is Fe's
// own largest character rather than Emacs' internal one, so the band above
// U+10FFFF that Emacs still accepts is a named error here, not a truncation.
static int ReadHexEscape(FeContext* ctx, FeReadFn fn, void* udata) {
  int value = 0;
  size_t digits = 0;
  char chr = fn(ctx, udata);
  for (int digit = HexDigit(chr); digit >= 0; digit = HexDigit(chr)) {
    if (value > (MaxCharacter - digit) / 16) {
      FeHandleError(ctx, "unsupported read syntax: \\x character out of range");
    }
    value = value * 16 + digit;
    digits++;
    chr = fn(ctx, udata);
  }
  if (digits == 0)
    FeHandleError(ctx, "unsupported read syntax: \\x");
  ctx->nextchr = chr;
  return value;
}

// One escape, shared by string bodies and `?` literals, returning the
// character's value rather than the bytes it will occupy. The caller decides
// what a value means: a `?` literal takes any of them, a string body is a
// byte string and rejects the ones that do not fit in a byte.
static int ReadEscape(FeContext* ctx, FeReadFn fn, void* udata) {
  const char chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
  ctx->nextchr = '\0';
  if (chr == '\0')
    FeHandleError(ctx, "unclosed string");
  switch (chr) {
    case 'a':
      return '\a';
    case 'b':
      return '\b';
    case 't':
      return '\t';
    case 'n':
      return '\n';
    case 'v':
      return '\v';
    case 'f':
      return '\f';
    case 'r':
      return '\r';
    case 'e':
      return 27;
    case 'd':
      return 127;
    case 's':
      return ' ';
    case '\\':
      return '\\';
    case '"':
      return '"';
    default:
      break;
  }
  if (chr == 'x') {
    return ReadHexEscape(ctx, fn, udata);
  }
  if (chr >= '0' && chr <= '7') {
    int value = chr - '0';
    for (size_t i = 1; i < 3; i++) {
      const char next = fn(ctx, udata);
      if (next < '0' || next > '7') {
        ctx->nextchr = next;
        break;
      }
      value = value * 8 + next - '0';
    }
    return value;
  }
  FeHandleError(ctx, "unsupported read syntax: unknown escape");
}

static bool IsValidUtf8Codepoint(int value, size_t count) {
  const int minimum = count == 1 ? 0x80 : count == 2 ? 0x800 : 0x10000;
  return value >= minimum && value <= 0x10ffff &&
         (value < 0xd800 || value > 0xdfff);
}

static int ReadUtf8(FeContext* ctx, FeReadFn fn, void* udata, char lead) {
  const unsigned char first = (unsigned char)lead;
  if (first >= 0x80 && (first < 0xc2 || first > 0xf4)) {
    FeHandleError(ctx, "unsupported read syntax: invalid UTF-8 character");
  }
  size_t count = first < 0x80 ? 0 : first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
  if (count == 0)
    return first;
  int value = first & ((1 << (6 - count)) - 1);
  for (size_t i = 0; i < count; i++) {
    const unsigned char byte = (unsigned char)fn(ctx, udata);
    if ((byte & 0xc0) != 0x80) {
      FeHandleError(ctx, "unsupported read syntax: invalid UTF-8 character");
    }
    value = (value << 6) | (byte & 0x3f);
  }
  if (!IsValidUtf8Codepoint(value, count)) {
    FeHandleError(ctx, "unsupported read syntax: invalid UTF-8 character");
  }
  return value;
}

// Emacs' control-modifier rule, enumerated against GNU Emacs 31.0.90 over
// every printable ASCII character and over `é`: `?` is DEL, `@`..`_` and
// `a`..`z` fold to their ASCII control code, and everything else -- space,
// backquote, digits, punctuation, non-ASCII, and the value of a nested escape
// such as `?\C-\n` -- keeps its own value with the 2^26 control bit set.
// `value &= 0x1f` answered 31 for `?\C-?` and 5 for `?\C-%`.
static int ApplyControlModifier(int value) {
  if (value == '?') {
    return 127;
  }
  if ((value >= '@' && value <= '_') || (value >= 'a' && value <= 'z')) {
    return value & 0x1f;
  }
  return value | CharControlBit;
}

// `\S-`, `\s-`, `\A-` and `\H-` are Emacs character modifiers Fe does not
// implement, and `\^` is Emacs' second spelling of `\C-`. Each is named in
// its own rejection rather than misread.
[[noreturn]] static void RejectCharacterModifier(FeContext* ctx, char letter) {
  char message[64];
  Format(message, sizeof(message),
         "unsupported read syntax: \\%c character modifier", letter);
  FeHandleError(ctx, message);
}

static int ReadEscapedCharacter(FeContext* ctx,
                                FeReadFn fn,
                                void* udata,
                                bool* control,
                                bool* meta);

// The character a modifier applies to: either a nested escape (`?\C-\n`) or
// one UTF-8 sequence (`?\C-a`, `?\C-é`).
static int ReadModifiedCharacter(FeContext* ctx,
                                 FeReadFn fn,
                                 void* udata,
                                 bool* control,
                                 bool* meta) {
  const char chr = fn(ctx, udata);
  if (chr == '\\') {
    return ReadEscapedCharacter(ctx, fn, udata, control, meta);
  }
  return ReadUtf8(ctx, fn, udata, chr);
}

// Escape position inside a `?` literal, the `\` already consumed. `\C-` and
// `\M-` are the two modifiers Fe implements and recurse back into character
// position; the modifier letters it does not implement are rejected by name.
// `\s` is the ordinary space escape unless a `-` follows it, which is the one
// place a second character of lookahead is needed -- reading `?\s-a` as `\s`
// plus a leftover `-a` made one character literal read as two forms.
static int ReadEscapedCharacter(FeContext* ctx,
                                FeReadFn fn,
                                void* udata,
                                bool* control,
                                bool* meta) {
  const char chr = fn(ctx, udata);
  if (chr == 'C' || chr == 'M') {
    const bool is_control = chr == 'C';
    if (is_control ? *control : *meta) {
      FeHandleError(ctx,
                    "unsupported read syntax: duplicate character modifier");
    }
    if (fn(ctx, udata) != '-') {
      FeHandleError(ctx,
                    "unsupported read syntax: malformed character modifier");
    }
    *control |= is_control;
    *meta |= !is_control;
    return ReadModifiedCharacter(ctx, fn, udata, control, meta);
  }
  if (chr == '^') {
    RejectCharacterModifier(ctx, chr);
  }
  if (chr == 'S' || chr == 's' || chr == 'A' || chr == 'H') {
    const char peek = fn(ctx, udata);
    if (peek == '-') {
      RejectCharacterModifier(ctx, chr);
    }
    if (chr != 's') {
      FeHandleError(ctx, "unsupported read syntax: unknown escape");
    }
    ctx->nextchr = peek;
    return ' ';
  }
  ctx->nextchr = chr;
  return ReadEscape(ctx, fn, udata);
}

// A `?` literal ends at a delimiter, as it does in Emacs, which reads `?ab`,
// `?\1a` and `?\s-a` as `invalid-read-syntax`. Without this, each of them
// yielded a character plus a leftover token: one form silently read as two.
static void RequireCharacterDelimiter(FeContext* ctx,
                                      FeReadFn fn,
                                      void* udata) {
  const char chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
  ctx->nextchr = chr;
  if (chr != '\0' && strchr(" \n\t\r();`,\"'", chr) == NULL) {
    FeHandleError(ctx, "unsupported read syntax: ? literal without delimiter");
  }
}

static FeObject* ReadCharacter(FeContext* ctx, FeReadFn fn, void* udata) {
  const char chr = fn(ctx, udata);
  if (chr == '\0') {
    FeHandleError(ctx, "unsupported read syntax: ? at end of input");
  }
  int value;
  if (chr == '\\') {
    bool control = false;
    bool meta = false;
    value = ReadEscapedCharacter(ctx, fn, udata, &control, &meta);
    if (control) {
      value = ApplyControlModifier(value);
    }
    if (meta) {
      value |= CharMetaBit;
    }
  } else {
    value = ReadUtf8(ctx, fn, udata, chr);
  }
  RequireCharacterDelimiter(ctx, fn, udata);
  return FeMakeInteger(ctx, value);
}

typedef struct RadixDigits {
  uint64_t magnitude;
  double fallback;
  size_t digits;
  bool overflow;
} RadixDigits;

static RadixDigits ReadRadixDigits(FeContext* ctx,
                                   FeReadFn fn,
                                   void* udata,
                                   int base,
                                   char chr) {
  RadixDigits result = {0};
  // No digit cap: the digits are accumulated into a `uint64_t` with an
  // overflow flag and a `double` fallback, never into a buffer, so a long
  // literal follows the recorded radix-overflow policy instead of reporting
  // the 63-byte *symbol* limit for something that is not a symbol.
  while (chr && !strchr(" \n\t\r();`,", chr)) {
    const int digit = HexDigit(chr);
    if (digit < 0 || digit >= base) {
      FeHandleError(ctx, "unsupported read syntax: malformed radix integer");
    }
    result.digits++;
    if (!result.overflow) {
      if (result.magnitude > (UINT64_MAX - (unsigned)digit) / (unsigned)base) {
        result.overflow = true;
      } else {
        result.magnitude = result.magnitude * (unsigned)base + (unsigned)digit;
      }
    }
    result.fallback = result.fallback * base + digit;
    chr = fn(ctx, udata);
  }
  if (result.digits == 0)
    FeHandleError(ctx, "unsupported read syntax: malformed radix integer");
  ctx->nextchr = chr;
  return result;
}

static FeObject* ReadRadix(FeContext* ctx, FeReadFn fn, void* udata, int base) {
  char chr = fn(ctx, udata);
  char sign = '+';
  if (chr == '+' || chr == '-') {
    sign = chr;
    chr = fn(ctx, udata);
  }
  const RadixDigits digits = ReadRadixDigits(ctx, fn, udata, base, chr);
  const bool negative = sign == '-';
  const uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : INT64_MAX;
  if (!digits.overflow && digits.magnitude <= limit) {
    if (negative && digits.magnitude == (uint64_t)INT64_MAX + 1)
      return FeMakeInteger(ctx, INT64_MIN);
    return FeMakeInteger(
        ctx, negative ? -(int64_t)digits.magnitude : (int64_t)digits.magnitude);
  }
  return FeMakeDouble(ctx, negative ? -digits.fallback : digits.fallback);
}

static FeObject* Read(FeContext* ctx, FeReadFn fn, void* udata);

bool IsNamedSymbol(const FeObject* v, const char* name) {
  return FeGetType(v) == FeTSymbol && IsStringEqual(SymbolName(v), name);
}

// A keyword is an INTERNED symbol whose name starts with a colon (08B). The
// self-binding `InitializeKeywordValue` gives one at intern time is what says
// "interned" here: Phase 14's `make-symbol` skips it, so an uninterned symbol
// named `:a` is an ordinary unbound symbol, which is what the pinned Emacs
// answers too -- `(keywordp (make-symbol ":a"))` is nil there and
// `(symbol-value (make-symbol ":a"))` is `(void-variable :a)`. Nothing can
// clear an interned keyword's self-binding: `setq`, `set`, `makunbound` and
// `let` all refuse a constant, and a keyword is one.
bool IsKeywordSymbol(const FeObject* v) {
  return FeGetType(v) == FeTSymbol && SymbolName(v) != NULL &&
         STRING_BUFFER(SymbolName(v))[0] == ':' && CDR(CDR(v)) == v;
}

bool IsConstantSymbol(const FeObject* v) {
  return IsNamedSymbol(v, "t") || IsKeywordSymbol(v);
}

static bool IsDot(const FeObject* v) {
  return IsNamedSymbol(v, ".");
}

static FeObject* ReadHash(FeContext* ctx, FeReadFn fn, void* udata) {
  const char next = fn(ctx, udata);
  if (next == '\'')
    return ReadWrapped(ctx, fn, udata, "function", "stray '#''");
  if (next == 'x' || next == 'X')
    return ReadRadix(ctx, fn, udata, 16);
  if (next == 'o' || next == 'O')
    return ReadRadix(ctx, fn, udata, 8);
  if (next == 'b' || next == 'B')
    return ReadRadix(ctx, fn, udata, 2);
  // `##` is the symbol with the empty name, which is how Emacs both prints
  // and reads it. Phase 14 needs the read half so that every symbol the
  // writer produces reads back: `(intern "")` is a legal symbol and `""` is
  // not a token any other spelling can carry.
  if (next == '#')
    return FeMakeSymbol(ctx, "");
  FeHandleError(ctx, "unsupported read syntax: #");
}

// A Fe string is a byte string, so a string escape has to land in one byte.
// `BuildString` writes at `strlen`, which made a decoded NUL a silent no-op
// that shifted every later character down -- `"\0a"` read as `"a"` and
// `"a\0b"` as `"ab"` -- and a value above 255 was truncated the same way,
// which is what `"\400"` did. Both are named errors now. Emacs stores a NUL
// (and reads `"\400"` as the character U+0100); that divergence is recorded
// in `compat/features.json` and `doc/language.md`, and it is the same rule
// the reader already applied to a literal NUL byte in the source text.
static int ReadStringEscape(FeContext* ctx, FeReadFn fn, void* udata) {
  const int value = ReadEscape(ctx, fn, udata);
  if (value == 0) {
    FeHandleError(ctx, "unsupported read syntax: NUL character in string");
  }
  if (value > 0xff) {
    FeHandleError(ctx,
                  "unsupported read syntax: character above 255 in string");
  }
  return value;
}

static FeObject* ReadStringLiteral(FeContext* ctx, FeReadFn fn, void* udata) {
  FeObject* res = BuildString(ctx, NULL, '\0');
  FeObject* value = res;
  char chr = fn(ctx, udata);
  while (chr != '"') {
    if (chr == '\0')
      FeHandleError(ctx, "unclosed string");
    if (chr == '\\') {
      chr = (char)ReadStringEscape(ctx, fn, udata);
    }
    value = BuildString(ctx, value, chr);
    chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
    ctx->nextchr = '\0';
  }
  return res;
}

static void RecordInputLine(FeContext* ctx, size_t* line, char chr) {
  ctx->error_line = *line;
  if (chr == '\n')
    (*line)++;
}

static void RecordTopFormLine(FeContext* ctx) {
  if (ctx->top_form_line == 0)
    ctx->top_form_line = ctx->error_line;
}

static size_t TopFormLine(const FeContext* ctx) {
  return ctx->top_form_line == 0 ? 1 : ctx->top_form_line;
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
    // An ESCAPED dot is an ordinary symbol, so `(a \. b)` is a three-element
    // list where `(a . b)` is a pair (Phase 14). The two read to the same
    // interned symbol -- `\.` is not a different object -- so the object
    // alone cannot answer this and the reader's own flag has to.
    if (IsDot(v) && !ctx->reader_atom_escaped) {
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

  // Skip whitespace and comments. Both have to be out of the way *before* the
  // top-level form's line is latched: `RecordTopFormLine` keeps the first
  // value it is given, so recording ahead of the comment arm reported the line
  // of the leading `;` for every form a comment block precedes -- which is how
  // every real init file begins.
  while (true) {
    while (chr && strchr(" \n\t\r", chr)) {
      chr = fn(ctx, udata);
    }
    if (chr != ';') {
      break;
    }
    while (chr && chr != '\n') {
      chr = fn(ctx, udata);
    }
  }
  RecordTopFormLine(ctx);
  if (chr == '?')
    return ReadCharacter(ctx, fn, udata);
  if (chr == '[' || chr == ']')
    FeHandleError(ctx, "unsupported read syntax: vector brackets");

  switch (chr) {
    case '\0':
      return NULL;

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

    case '#':
      return ReadHash(ctx, fn, udata);
    case '"':
      return ReadStringLiteral(ctx, fn, udata);

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
  size_t line;
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
  RecordInputLine(ctx, &input->line, chr);
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
  const size_t saved_line = ctx->error_line;
  const bool saved_has_line = ctx->error_has_line;
  ctx->error_label = nullptr;
  ctx->error_offset = *position;
  ctx->error_has_offset = true;
  ctx->error_has_line = false;
  ctx->nextchr = '\0';
  if (source == nullptr && length != 0) {
    FeHandleError(ctx, "null source");
  }
  if (*position > length) {
    FeHandleError(ctx, "offset exceeds source length");
  }

  StringInput input = {
      .source = source, .length = length, .offset = position, .line = 1};
  FeObject* result = FeRead(ctx, ReadString, &input);
  if (ctx->nextchr != '\0') {
    (*position)--;
    ctx->nextchr = '\0';
  }
  ctx->error_label = saved_label;
  ctx->error_offset = saved_offset;
  ctx->error_has_offset = saved_has_offset;
  ctx->error_line = saved_line;
  ctx->error_has_line = saved_has_line;
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

// Saves the enclosing input unit, and installs the one an evaluation is
// about to run inside. `SaveInputUnit`/`RestoreInputUnit` are the pair every
// site that owns a unit uses; see `FeInputUnit` in fe_internal.h for why the
// scope number and the diagnostic label travel together.
FeInputUnit SaveInputUnit(const FeContext* ctx) {
  return (FeInputUnit){
      .label = ctx->error_label,
      .scope = ctx->input_scope,
      .offset = ctx->error_offset,
      .line = ctx->error_line,
      .has_offset = ctx->error_has_offset,
      .has_line = ctx->error_has_line,
  };
}

void RestoreInputUnit(FeContext* ctx, const FeInputUnit* saved) {
  ctx->error_label = saved->label;
  ctx->input_scope = saved->scope;
  ctx->error_offset = saved->offset;
  ctx->error_line = saved->line;
  ctx->error_has_offset = saved->has_offset;
  ctx->error_has_line = saved->has_line;
}

// Leaves for the host: no input unit at all. Called from the one place a
// completion stops being any unit's business -- `RaiseCompletionCore`'s host
// exit, after the drain -- where there is no enclosing unit to restore,
// every unit between the raise and the host having been abandoned wholesale.
// Scope 0 is the host context the manifest's `one-arg-defvar-scope-carrier`
// row describes, where every mark is visible; dropping the label keeps a
// later, unrelated raise from being prefixed with a stale file name.
void EnterHostInputContext(FeContext* ctx) {
  ctx->input_scope = 0;
  ctx->error_label = nullptr;
  ctx->error_has_offset = false;
}

// Enters the input unit this evaluation is (sub-plan 12C Part 2). The scope
// number is taken on the way in and handed back on the way out, so nested
// loads stack: a file C that A loads gets a number of its own, and A's comes
// back when C returns. An *abnormal* exit past the restore below is covered
// by the containment barriers in fe_run.c when the completion is contained
// -- the only way an outer unit keeps evaluating after an inner one failed
// -- and by `EnterHostInputContext` when it is not.
static void EnterInputUnit(FeContext* ctx, const char* label) {
  ctx->input_scope = ++ctx->input_scope_next;
  ctx->error_label = label;
  ctx->error_offset = 0;
  ctx->error_has_offset = true;
  ctx->error_has_line = true;
  ctx->error_line = 1;
  ctx->top_form_line = 0;
}

// The host's half of the same machinery (FE_API_VERSION 8): a unit entered
// and left around a read-eval loop the host drives itself, so that the
// forms can be evaluated by `eval` -- in the run the loop is already inside
// -- instead of by an entry point that starts one. See `FeInputUnit` in
// fe.h for what that buys and for the unwind guarantee.
void FeEnterInputUnit(FeContext* ctx,
                      const char* label,
                      FeInputUnit* enclosing) {
  *enclosing = SaveInputUnit(ctx);
  EnterInputUnit(ctx, label);
}

void FeLeaveInputUnit(FeContext* ctx, const FeInputUnit* enclosing) {
  RestoreInputUnit(ctx, enclosing);
}

// One form for the current unit, with the line that form STARTS on left in
// `ctx->error_line` for whoever evaluates it -- which is the whole reason
// this is not `FeReadString`. That entry point saves and restores the label
// and position around itself (a host that reads a form through it cannot
// leave a label behind, which is right for a reader and wrong for a loader),
// and its line counter restarts at 1 per call, so a caller reading form
// after form out of one buffer has no way to learn any form's line at all.
// This is the same latch `EvaluateInput`'s loop makes from `TopFormLine`
// after each `FeRead`, published for a caller instead of consumed in place.
FeObject* FeReadInputForm(FeContext* ctx,
                          const char* source,
                          size_t length,
                          size_t* offset,
                          size_t* line) {
  if (source == nullptr && length != 0) {
    FeHandleError(ctx, "null source");
  }
  if (*offset > length) {
    FeHandleError(ctx, "offset exceeds source length");
  }
  ctx->error_has_offset = false;
  ctx->error_has_line = true;
  ctx->top_form_line = 0;
  ctx->nextchr = '\0';
  StringInput input = {
      .source = source, .length = length, .offset = offset, .line = *line};
  FeObject* const form = FeRead(ctx, ReadString, &input);
  if (ctx->nextchr != '\0') {
    // The reader's one-character pushback: an atom stops at the delimiter
    // that ended it, having already consumed it. `FeReadString` hands that
    // byte back through the caller's offset; this hands back the LINE too,
    // because a pushed-back newline has already been counted and the next
    // call would otherwise count it twice.
    (*offset)--;
    if (ctx->nextchr == '\n') {
      input.line--;
    }
    ctx->nextchr = '\0';
  }
  *line = input.line;
  if (form != nullptr) {
    ctx->error_line = TopFormLine(ctx);
  }
  ctx->top_form_line = 0;
  return form;
}

static FeObject* EvaluateInput(FeContext* ctx,
                               const char* label,
                               FeReadFn* read,
                               void* input) {
  const FeInputUnit enclosing = SaveInputUnit(ctx);
  EnterInputUnit(ctx, label);
  const size_t gc = FeSaveGC(ctx);
  ctx->nextchr = '\0';
  ctx->evaluation_result = &nil;

  while (true) {
    FeRestoreGC(ctx, gc);
    ctx->error_has_offset = false;
    ctx->error_has_line = true;
    FeObject* object = FeRead(ctx, read, input);
    if (object == nullptr) {
      break;
    }
    ctx->error_line = TopFormLine(ctx);
    ctx->evaluation_result = FeEvaluate(ctx, object);
    ctx->top_form_line = 0;
  }

  FeRestoreGC(ctx, gc);
  RestoreInputUnit(ctx, &enclosing);
  return ctx->evaluation_result;
}

FeObject* FeEvaluateString(FeContext* ctx,
                           const char* label,
                           const char* source,
                           size_t length) {
  size_t offset = 0;
  StringInput input = {
      .source = source, .length = length, .offset = &offset, .line = 1};
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
  size_t line;
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
  RecordInputLine(ctx, &input->line, (char)chr);
  return (char)chr;
}

FeObject* FeEvaluateFile(FeContext* ctx, const char* label, FILE* file) {
  FileInput input = {.file = file, .offset = 0, .line = 1};
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

// Phase 14's symbol surface. Eight ordinary functions whose operands the
// evaluator has already evaluated into `arguments`; the arity table has
// already refused a wrong count, so every `CAR`/`CDR` below has something to
// read. See the `PIntern`..`PSymbolPlist` block in fe_internal.h for why the
// family is contiguous and why the routing is a range test.

bool IsSymbolPrimitive(Primitive primitive) {
  return primitive >= PIntern && primitive <= PSymbolPlist;
}

// A name operand as a C string. `SymbolNameLimit` is the reader's own token
// bound, so a name a program constructs is bounded exactly where a name a
// program writes is; a longer one is a named error rather than a truncation.
static void CopyNameArgument(FeContext* ctx, FeObject* obj, char* buf) {
  const FeObject* const string = CheckType(ctx, obj, FeTString);
  const size_t length = CopyStoredStringBytes(string, nullptr);
  if (length > SymbolNameLimit) {
    FeHandleError(ctx, "symbol name too long (63-byte limit)");
  }
  (void)CopyStoredStringBytes(string, buf);
  buf[length] = '\0';
}

// `nil` is not a symbol object in fe, so the name "nil" has to answer the nil
// object -- which is what Emacs answers too, its `nil` being the symbol of
// that name. Without this, `(intern "nil")` would mint a second thing that
// prints `nil` and is not it.
static FeObject* InternName(FeContext* ctx, const char* name) {
  return strcmp(name, "nil") == 0 ? &nil : FeMakeSymbol(ctx, name);
}

// `(intern-soft NAME-OR-SYMBOL)`: nil on a miss, and NO interning. Emacs
// takes a symbol as well as a string, and answers nil for an uninterned one,
// which is identity and not name equality -- `(intern-soft (make-symbol
// "x"))` is nil even when `x` is interned.
static FeObject* InternSoft(FeContext* ctx, FeObject* argument) {
  char name[SymbolNameLimit + 1];
  if (FeGetType(argument) == FeTSymbol) {
    const size_t length = CopyStoredStringBytes(SymbolName(argument), nullptr);
    if (length > SymbolNameLimit) {
      return &nil;
    }
    (void)CopyStoredStringBytes(SymbolName(argument), name);
    name[length] = '\0';
  } else if (FeIsNil(argument)) {
    return &nil;  // Emacs' `(intern-soft "nil")` is nil either way.
  } else {
    CopyNameArgument(ctx, argument, name);
  }
  FeObject* const found = FindInternedSymbol(ctx, name);
  return found == nullptr ||
                 (FeGetType(argument) == FeTSymbol && found != argument)
             ? &nil
             : found;
}

// `(gensym &optional PREFIX)`: an uninterned symbol named PREFIX followed by
// a per-context sequence number. Emacs keeps the counter in the Lisp variable
// `gensym-counter`; fe exposes no way to read or set it, so the only promise
// is uniqueness within a context, which is all a macro's temporary needs.
static FeObject* Gensym(FeContext* ctx, FeObject* arguments) {
  char name[SymbolNameLimit + 1] = "g";
  if (!FeIsNil(arguments)) {
    CopyNameArgument(ctx, CAR(arguments), name);
  }
  const size_t used = strlen(name);
  Format(name + used, sizeof(name) - used, "%" PRIu64, ctx->gensym_counter);
  ctx->gensym_counter++;
  return MakeSymbolObject(ctx, name);
}

// A symbol's name as a string, with fe's nil -- which is not a symbol object
// -- answering "nil" as Emacs' symbol nil does.
static FeObject* SymbolNameString(FeContext* ctx, FeObject* argument) {
  if (FeIsNil(argument)) {
    return FeMakeString(ctx, "nil");
  }
  char name[SymbolNameLimit + 1];
  const FeObject* const stored =
      SymbolName(CheckType(ctx, argument, FeTSymbol));
  const size_t length = CopyStoredStringBytes(stored, nullptr);
  if (length > SymbolNameLimit) {
    FeHandleError(ctx, "symbol name too long (63-byte limit)");
  }
  (void)CopyStoredStringBytes(stored, name);
  name[length] = '\0';
  return FeMakeString(ctx, name);
}

// The property list is `(PROP VALUE PROP VALUE ...)` and properties compare
// by `eq`, exactly as in Emacs -- so an integer property works, `eq` being
// value equality for integers here. `put` appends a new property at the tail
// rather than pushing it at the head, which is measured Emacs behaviour:
// `(put 'p 'a 1)` then `(put 'p 'b 2)` leaves `(a 1 b 2)`.
// The pair whose `car` holds PROPERTY's value, or null when the plist does
// not carry it. A trailing odd element is not a property, which is why the
// walk also requires a `cdr`.
static FeObject* FindPlistCell(FeObject* plist, FeObject* property) {
  while (!FeIsNil(plist) && !FeIsNil(CDR(plist))) {
    if (IdentityObjects(CAR(plist), property, false)) {
      return CDR(plist);
    }
    plist = CDR(CDR(plist));
  }
  return nullptr;
}

static FeObject* PlistGet(FeObject* plist, FeObject* property) {
  FeObject* const cell = FindPlistCell(plist, property);
  return cell == nullptr ? &nil : CAR(cell);
}

static FeObject* PlistPut(FeContext* ctx, FeObject* arguments) {
  FeObject* const symbol = CheckType(ctx, CAR(arguments), FeTSymbol);
  FeObject* const property = CAR(CDR(arguments));
  FeObject* const value = CAR(CDR(CDR(arguments)));
  FeObject* const cell = FindPlistCell(SymbolPlist(symbol), property);
  if (cell != nullptr) {
    CAR(cell) = value;
    return value;
  }
  // Two fresh pairs appended at the tail. `symbol` and `value` are reachable
  // from the caller's rooted operand list across both allocations.
  FeObject* const tail = FeCons(ctx, property, FeCons(ctx, value, &nil));
  FeObject* plist = SymbolPlist(symbol);
  if (FeIsNil(plist)) {
    SetSymbolPlist(symbol, tail);
    return value;
  }
  while (!FeIsNil(CDR(plist))) {
    plist = CDR(plist);
  }
  CDR(plist) = tail;
  return value;
}

FeObject* EvaluateSymbolPrimitive(FeContext* ctx,
                                  Primitive primitive,
                                  FeObject* arguments) {
  // `arguments` is a C local in `ResumeEvalList` by the time it gets here --
  // the accumulator that held it has been emptied by the reversal -- so it is
  // rooted explicitly across the allocations below. The result is pushed back
  // after the restore, which is the state every other primitive leaves the GC
  // stack in: `MakeObject` pushes whatever it hands out.
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, arguments);
  char name[SymbolNameLimit + 1];
  FeObject* result = &nil;
  // On an `int`, not on `Primitive`: `-Wswitch-enum` asks a switch over an
  // enumeration to name every value, and this one deliberately handles the
  // eight its caller filtered for. `ResumeEvalList` switches on `PRIM(...)`
  // for the same reason.
  switch ((int)primitive) {
    case PIntern:
      CopyNameArgument(ctx, CAR(arguments), name);
      result = InternName(ctx, name);
      break;
    case PInternSoft:
      result = InternSoft(ctx, CAR(arguments));
      break;
    case PSymbolName:
      result = SymbolNameString(ctx, CAR(arguments));
      break;
    case PMakeSymbol:
      CopyNameArgument(ctx, CAR(arguments), name);
      result = MakeSymbolObject(ctx, name);
      break;
    case PGensym:
      result = Gensym(ctx, arguments);
      break;
    case PPut:
      result = PlistPut(ctx, arguments);
      break;
    // fe's nil owns no storage of its own, so a property cannot be stored on
    // it. Reading answers nil, which is what Emacs answers for a property
    // nobody set; `put` says so by name -- `(wrong-type-argument symbolp
    // nil)` -- rather than dropping the value on the floor.
    case PGet:
      result =
          FeIsNil(CAR(arguments))
              ? &nil
              : PlistGet(SymbolPlist(CheckType(ctx, CAR(arguments), FeTSymbol)),
                         CAR(CDR(arguments)));
      break;
    case PSymbolPlist:
      result = FeIsNil(CAR(arguments))
                   ? &nil
                   : SymbolPlist(CheckType(ctx, CAR(arguments), FeTSymbol));
      break;
    default:
      abort();
  }
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, result);
  return result;
}

static size_t GetSymbolObjectCount(const char* name) {
  // A symbol object plus its one `((name . plist) . function) . value` cons
  // chain: the symbol, the name string cells, the extra pair the 04B
  // function cell added and the extra pair Phase 14's plist added.
  // `StringBufferSize` name characters fit in one string cell.
  const size_t length = strlen(name);
  assert(length > 0);
  return 6 + (length - 1) / StringBufferSize;
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
        RaiseCondition(ctx, FeCompletionError, "arith-error", &nil,
                       "arith-error");
      }
    }
    e >>= 1;
    if (e > 0 && ckd_mul(&factor, factor, factor)) {
      RaiseCondition(ctx, FeCompletionError, "arith-error", &nil,
                     "arith-error");
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
// integer return type for `floor`/`ceiling`/`round`/`truncate`. A value with
// no integer answer in a no-bignums program -- outside int64's range, either
// infinity, or NaN -- is the same refusal as int64 overflow (05A Decision
// 5), an `arith-error` rather than a cast whose behaviour is undefined.
// Emacs' own name for the nonfinite and out-of-range cases is
// `overflow-error`, not `arith-error`; `overflow-error`'s condition chain
// includes `arith-error`, so at fe's message level -- one free-text name
// until Phase 6's condition system -- `arith-error` is the honest single
// name to carry, and it is also what Emacs raises for this family's
// zero-divisor form below.
static int64_t IntegerRound(FeContext* ctx, double x) {
  // A negated in-range test, not `x < MIN || x >= MAX`: every comparison
  // against NaN is false, so the positive spelling lets NaN through to a
  // conversion C leaves undefined (UBSan reports it, and the value produced
  // was INT64_MIN). `(double)INT64_MAX` rounds up to exactly 2^63, so
  // `x < (double)INT64_MAX` is `x < 2^63` -- the same bounds as the
  // hex-float `0x1p63` spelling, in a form the static analyzer does not
  // mis-solve.
  if (!(x >= (double)INT64_MIN && x < (double)INT64_MAX)) {
    RaiseCondition(ctx, FeCompletionError, "arith-error", &nil, "arith-error");
  }
  return (int64_t)x;
}

// The rounding family's two-argument divisor: `(floor x d)` is Emacs'
// `arith-error` when `d` is zero, of either numeric type and of either sign,
// and the refusal has to come *before* the division rather than out of
// `IntegerRound`. `0 / 0.0` is NaN, and while `IntegerRound` now refuses
// that too, it would be refusing the wrong thing -- a divide by zero, not an
// unrepresentable rounded value -- and `5 / 0.0` is an infinity, which the
// pre-check keeps from being reported as a range failure. `fpclassify`
// rather than `d == 0` so no `-Wfloat-equal` suppression is needed for what
// is a classification, not a comparison.
static double RoundingQuotient(FeContext* ctx, double x, double d) {
  if (fpclassify(d) == FP_ZERO) {
    RaiseCondition(ctx, FeCompletionError, "arith-error", &nil, "arith-error");
  }
  return x / d;
}

static FeObject* native_floor(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, floor(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(ctx,
                       IntegerRound(ctx, floor(RoundingQuotient(ctx, x, d))));
}

static FeObject* native_ceiling(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, ceil(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(ctx,
                       IntegerRound(ctx, ceil(RoundingQuotient(ctx, x, d))));
}

static FeObject* native_round(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, nearbyint(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(
      ctx, IntegerRound(ctx, nearbyint(RoundingQuotient(ctx, x, d))));
}

static FeObject* native_truncate(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  if (FeIsNil(arg)) {
    return FeMakeInteger(ctx, IntegerRound(ctx, trunc(x)));
  }
  double d = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeInteger(ctx,
                       IntegerRound(ctx, trunc(RoundingQuotient(ctx, x, d))));
}

// The names of the two conditions `FeOpenContext` pre-builds so that arena
// exhaustion and GC-root-stack overflow can be *signalled* from a state that
// cannot allocate (sub-plan 09B). Both are already rows of `fe_eval.c`'s
// static hierarchy with `error` as their parent, which is what makes
// `(condition-case e BIG (error ...))` catch them.
static const char ArenaExhaustionName[] = "arena-exhaustion";
static const char EvaluationStackExhaustionName[] =
    "evaluation-stack-exhaustion";

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
  // The two pre-built exhaustion conditions: each is an interned symbol plus
  // the one pair that makes it a condition object (09B). Counting them here
  // is what keeps `FeMinimumArenaSize()` honest -- a context opened at the
  // minimum must still be able to build them.
  count += 1 + GetSymbolObjectCount(ArenaExhaustionName);
  count += 1 + GetSymbolObjectCount(EvaluationStackExhaustionName);
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
  ctx->condition = &nil;
  // Nil, not left zeroed: `CollectGarbage` marks it unconditionally, so the
  // invariant is that it always holds a real object -- the callable of the
  // native currently running, or nil when none is.
  ctx->native_identity = &nil;
  ctx->pending_throw_tag = &nil;
  ctx->pending_throw_value = &nil;
  ctx->free_list = &nil;
  ctx->symbol_list = &nil;
  ctx->special_list = &nil;
  ctx->evaluation_result = &nil;
  ctx->call_result = &nil;
  ctx->root_list = &nil;
  ctx->arena_exhaustion_condition = &nil;
  ctx->evaluation_stack_exhaustion_condition = &nil;
  ctx->arena_exhaustion_name = &nil;
  ctx->evaluation_stack_exhaustion_name = &nil;

  // Populate the free_list:
  for (size_t i = 0; i < ctx->object_count; i++) {
    FeObject* obj = &ctx->objects[i];
    SetType(obj, FeTFree);
    CDR(obj) = ctx->free_list;
    ctx->free_list = obj;
  }

  // Initialize the objects:
  ctx->t = FeMakeSymbol(ctx, "t");
  CDR(SymbolBindingCell(ctx->t)) = ctx->t;

  // The two conditions the raise paths signal when they cannot allocate
  // (09B). Built here, once, while the arena is still empty, and reachable
  // from `CollectGarbage` for the rest of the context's life. `FeMakeSymbol`
  // roots its result on the symbol list before `FeCons` can collect, so no
  // explicit GC-stack save is needed around either pair.
  // The names are kept as well as the pairs because a handler may `setcar`
  // the object it caught, so the pair's own `car` cannot be what a later
  // raise restores it from; `PublishExhaustion` re-stamps both halves from
  // here. Symbols are interned on `symbol_list`, itself a root, so these two
  // fields need no marking.
  ctx->arena_exhaustion_name = FeMakeSymbol(ctx, ArenaExhaustionName);
  ctx->arena_exhaustion_condition =
      FeCons(ctx, ctx->arena_exhaustion_name, &nil);
  ctx->evaluation_stack_exhaustion_name =
      FeMakeSymbol(ctx, EvaluationStackExhaustionName);
  ctx->evaluation_stack_exhaustion_condition =
      FeCons(ctx, ctx->evaluation_stack_exhaustion_name, &nil);

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
  ctx->special_list = &nil;
  ctx->evaluation_result = &nil;
  ctx->call_result = &nil;
  ctx->root_list = &nil;
  ctx->arena_exhaustion_condition = &nil;
  ctx->evaluation_stack_exhaustion_condition = &nil;
  ctx->arena_exhaustion_name = &nil;
  ctx->evaluation_stack_exhaustion_name = &nil;
  ctx->frame_stack_index = 0;
  CollectGarbage(ctx);
}
