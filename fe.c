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
#include "fe_perf.h"

const char* FeVersion = "21.0";

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
    [PSymbolPlist] = "symbol-plist",
    [PErrorMessageString] = "error-message-string",
    [PStringLess] = "string<",
    [PStringGreater] = "string>",
    [PVector] = "vector",
    [PMakeVector] = "make-vector",
    [PVectorp] = "vectorp",
    [PAref] = "aref",
    [PAset] = "aset",
    [PVconcat] = "vconcat",
    [PLength] = "length",
    [PElt] = "elt"};

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
    [FeTVector] = "vector",
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
  // The by-type allocation charge. Every cell that gets a type gets it here,
  // once, from the constructor that made it -- until Phase 25 a string cell
  // arrived here as a PAIR, because `BuildString` took it through `FeCons`,
  // and the charge had to be moved rather than made.
  FE_PERF_TYPED(type);
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

void FeSetBindingFns(FeContext* ctx,
                     FeBindingSaveFn* save,
                     FeBindingTargetFn* target) {
  ctx->binding_save_fn = save;
  ctx->binding_target_fn = target;
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
    case FeTVector:
      return "vectorp";
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

// ---------------------------------------------------------------------------
// The payload region (Phase 23.1; the Phase 22 ADR's Design B). What a block
// is, and the publish protocol these functions implement, is stated once in
// fe_internal.h beside the block layout they share.
// ---------------------------------------------------------------------------

// The types whose objects keep a payload handle: a vector, whose elements ARE
// its block's traced children (Phase 24), and a string, whose bytes are its
// block's byte tail (Phase 25). `FE_PAYLOAD_TEST_OBJECT`'s aggregate is still
// here because the substrate's own harness needs an owner whose width and
// byte tail it can choose independently, which neither release type is.
static bool OwnsPayload(const FeObject* obj) {
  const FeType type = FeGetType(obj);
#if FE_PAYLOAD_TEST_OBJECT
  if (type == FeTFex2) {
    return true;
  }
#endif
  return type == FeTVector || type == FeTString;
}

// Where an object of a payload-owning type keeps its handle. ONE function, so
// that a new owning type lands here rather than teaching the collector a
// second place to look.
static FePayloadHandle* PayloadSlot(FeObject* obj) {
  assert(OwnsPayload(obj));
  return &PAYLOAD(obj);
}

// The region's own address arithmetic, and the one place the const rule below
// is worth stating. These read the CONTEXT and never write it: what a caller
// goes on to write is arena storage, whose mutability travels with the
// returned pointer rather than with the context that located it. So they take
// a `const FeContext*`, and the functions that really do change the region --
// `PublishPayload`, `CompactPayloads`, `MovePayloadRegion` -- are exactly the
// ones that do not.
static unsigned char* PayloadAt(const FeContext* ctx, size_t offset) {
  assert(ctx->payload_start + offset <= ctx->payload_capacity);
  return ctx->payload_base + ctx->payload_start + offset;
}

static FePayloadBlock* PayloadBlockAt(const FeContext* ctx, size_t offset) {
  void* const at = PayloadAt(ctx, offset);
  return at;
}

// A block's payload starts one header past the block, and a handle is that
// offset plus one -- the plus one being what reserves zero for "no block".
static FePayloadHandle HandleOfBlockAt(size_t offset) {
  return offset + sizeof(FePayloadBlock) + 1;
}

// `<=`, not `<`: a block with no payload at all -- the empty string's -- has
// its bytes one past its own header, which is the end of the live extent
// when it is the last block published. That address is never dereferenced,
// and answering it rather than asserting is what keeps the empty string an
// ordinary string instead of a case every reader has to test for.
unsigned char* PayloadBytes(const FeContext* ctx, FePayloadHandle handle) {
  assert(handle != FePayloadNone && handle - 1 <= ctx->payload_used);
  return PayloadAt(ctx, handle - 1);
}

// The block a handle names. The handle is one past the block's own header, so
// this is the arithmetic that undoes `HandleOfBlockAt`.
static FePayloadBlock* BlockAtHandle(const FeContext* ctx,
                                     FePayloadHandle handle) {
  return PayloadBlockAt(ctx, handle - 1 - sizeof(FePayloadBlock));
}

// The owner's block, or null when it has not published one yet: a constructor
// is in that state between retyping the cell and publishing, and a collection
// can land in that window.
static FePayloadBlock* OwnedBlock(const FeContext* ctx, FeObject* owner) {
  const FePayloadHandle handle = *PayloadSlot(owner);
  return handle == FePayloadNone ? nullptr : BlockAtHandle(ctx, handle);
}

// The block's `index`th traced word. Clause 1 of the protocol applies to the
// result and to `block` itself: both are derived where they are spent.
static FeObject** PayloadChildSlot(FePayloadBlock* block, size_t index) {
  assert(index < block->children);
  void* const at = (unsigned char*)block + sizeof(FePayloadBlock) +
                   index * sizeof(FeObject*);
  return at;
}

// ---------------------------------------------------------------------------
// A STRING's length and bytes (Phase 25). A string is a header plus a payload
// block of bytes: the header keeps the byte LENGTH in the bytes of its `car`
// word that are not the type tag -- where the seven-byte cell chain this
// replaced kept text -- and the block keeps the bytes, which may contain NUL
// and are not terminated. The constructors are further down, beside the other
// constructors; these three are the whole read surface, and every string
// reader in fe and behind `fe.h` goes through them.
//
// Which half holds what is the point. A LENGTH read is a field read: it needs
// no context, costs no walk, and cannot go stale. A BYTE address is payload:
// it is derived immediately before the read or write that spends it (clause 1
// of the publish protocol) and re-derived after anything that can allocate
// (clause 2). The block is CAPACITY rather than length -- the allocator
// rounds up to `FePayloadAlignment` and the reader grows a literal
// geometrically -- so the length in the header is the only thing that knows
// how much of the block is text.
// ---------------------------------------------------------------------------

// Endian-neutral, and deliberately not an integer aliased onto the word:
// `car.c` is byte 0 of that word on a little-endian host and byte 7 on a
// big-endian one, so the bytes after the tag are written and read as
// base-256 digits, least significant first.
static size_t StringLength(const FeObject* string) {
  const unsigned char* const digits = (const unsigned char*)&string->car.c + 1;
  size_t length = 0;
  for (size_t i = StringLengthBytes; i-- > 0;) {
    length = (length << 8) | digits[i];
  }
  return length;
}

static void SetStringLength(FeObject* string, size_t length) {
  unsigned char* const digits = (unsigned char*)&string->car.c + 1;
  for (size_t i = 0; i < StringLengthBytes; i++) {
    digits[i] = (unsigned char)(length >> (8 * i));
  }
}

// The bytes, live for exactly as long as the statement that derives them.
// Never null: every string publishes a block, the empty string included.
static unsigned char* StringBytes(const FeContext* ctx,
                                  const FeObject* string) {
  return PayloadBytes(ctx, PAYLOAD(string));
}

// How many bytes the block holds, which is at least the length. Only the
// constructors ask; every other caller wants `StringLength`.
static size_t StringCapacity(const FeContext* ctx, FeObject* string) {
  return OwnedBlock(ctx, string)->bytes;
}

// The block size a request of BYTES really takes, since the allocator rounds
// up to `FePayloadAlignment`. Two callers need that without allocating: the
// reader's trim, which asks whether a smaller block would be a different
// block at all, and the arena census, which has to agree with the allocator
// exactly because `FeMinimumArenaSize` is exact rather than an upper bound.
static size_t RoundUpToAlignment(size_t bytes) {
  return (bytes + FePayloadAlignment - 1) / FePayloadAlignment *
         FePayloadAlignment;
}

// Bump-allocate one block, or answer `FePayloadNone` when the region cannot
// hold it. It publishes nothing: `PublishPayload` is the one function allowed
// to hand a block to an owner, and it is this function's only caller.
static FePayloadHandle AllocatePayloadBlock(FeContext* ctx,
                                            size_t children,
                                            size_t bytes) {
  size_t want = 0;
  size_t total = 0;
  size_t reach = 0;
  if (ckd_mul(&want, children, sizeof(FeObject*)) ||
      ckd_add(&want, want, bytes) ||
      ckd_add(&want, want, FePayloadAlignment - 1)) {
    return FePayloadNone;
  }
  want -= want % FePayloadAlignment;
  if (ckd_add(&total, want, sizeof(FePayloadBlock)) ||
      ckd_add(&reach, ctx->payload_used, total) ||
      ckd_add(&reach, reach, ctx->payload_start) ||
      reach > ctx->payload_capacity) {
    return FePayloadNone;
  }
  const size_t offset = ctx->payload_used;
  // Zero-filled, so a caller that publishes object slots has null-not-garbage
  // in them until it fills them in, and a byte payload starts NUL-terminated.
  memset(PayloadAt(ctx, offset), 0, total);
  FePayloadBlock* const block = PayloadBlockAt(ctx, offset);
  block->children = children;
  block->bytes = want;
  FE_PERF_INC(FePerfPayloadAlloc);
  FE_PERF_ADD(FePerfPayloadByte, total);
  ctx->payload_used += total;
  if (ctx->payload_used > ctx->payload_peak_used) {
    ctx->payload_peak_used = ctx->payload_used;
  }
  return HandleOfBlockAt(offset);
}

// Whether the block at OFFSET survives this collection. Two halves: its owner
// survived the mark phase, AND the owner still names THIS block -- which is
// what makes a REPLACED block dead though its owner lives. Both are read
// before the sweep clears the mark bits, which is why `CompactPayloads` runs
// where it does.
static bool PayloadBlockIsLive(const FeContext* ctx, size_t offset) {
  FeObject* const owner = PayloadBlockAt(ctx, offset)->owner;
  assert(owner != nullptr);
  if (~TAG(owner) & GcMarkBit) {
    return false;
  }
  return *PayloadSlot(owner) == HandleOfBlockAt(offset);
}

// Compaction preserves ALLOCATION ORDER: the region is scanned from its base
// and survivors slide down in the order they were published, so a given
// history produces the same handles every time. That determinism is what lets
// the tests assert addresses rather than relationships between them.
void CompactPayloads(FeContext* ctx) {
  size_t source = 0;
  size_t target = 0;
  FE_PERF_INC(FePerfPayloadCompact);
  while (source < ctx->payload_used) {
    const size_t total =
        sizeof(FePayloadBlock) + PayloadBlockAt(ctx, source)->bytes;
    if (PayloadBlockIsLive(ctx, source)) {
      if (target != source) {
        // Overlapping by construction -- a survivor slides down over the gap
        // a shorter dead block left -- so this is `memmove` and can never be
        // `memcpy`.
        memmove(PayloadAt(ctx, target), PayloadAt(ctx, source), total);
        FE_PERF_INC(FePerfPayloadCompactMoved);
      }
      *PayloadSlot(PayloadBlockAt(ctx, target)->owner) =
          HandleOfBlockAt(target);
      target += total;
    }
    source += total;
  }
  if (target != ctx->payload_used) {
    ctx->payload_compaction_count++;
    ctx->payload_used = target;
  }
  // ...and the survivors slide down to the REGION's base, not merely to the
  // extent's. `payload_start` is zero in every ordinary build -- only
  // `MovePayloadRegion`, the poison knob, ever moves it -- so this is one
  // comparison there and nothing else. In the poison lane it is what keeps
  // the knob a diagnostic rather than a second, smaller region: without it
  // the extent drifts forward one unit per allocation and the bytes behind
  // it are unreachable, so a large publish that the region can hold fails
  // with `(payload-exhaustion)` after a few thousand allocations. Handles
  // are offsets INTO the extent, so moving the extent does not change one.
  if (ctx->payload_start != 0) {
    memmove(ctx->payload_base, PayloadAt(ctx, 0), ctx->payload_used);
#if FE_DEBUG_PAYLOAD_MOVE
    // The bytes the extent vacated read as poison, exactly as the ones
    // `MovePayloadRegion` vacates do. Without this a pointer into the TAIL of
    // the old extent reads valid old data after this move: the extent slides
    // DOWN over itself, and a backward `memmove` does not overwrite its own
    // tail. Found by the poison lane in Phase 25 -- before strings owned
    // payloads the region was empty at this point in the substrate's own
    // test, so the case could not arise.
    memset(ctx->payload_base + ctx->payload_used, FePayloadPoisonByte,
           ctx->payload_start);
#endif
    ctx->payload_start = 0;
  }
}

#if FE_DEBUG_PAYLOAD_MOVE

void MovePayloadRegion(FeContext* ctx) {
  if (ctx->payload_used == 0) {
    // Nothing live: park the extent at the region's base, so a build with no
    // payloads in flight -- which is every shipped one -- pays one comparison
    // per allocation and moves nothing.
    ctx->payload_start = 0;
    return;
  }
  unsigned char* const live = ctx->payload_base + ctx->payload_start;
  // Forward by one unit, vacating the unit at the extent's old START, which is
  // the address a caller that ignored clause 1 is holding.
  if (ctx->payload_start + ctx->payload_used + FePayloadAlignment <=
      ctx->payload_capacity) {
    memmove(live + FePayloadAlignment, live, ctx->payload_used);
    memset(live, FePayloadPoisonByte, FePayloadAlignment);
    ctx->payload_start += FePayloadAlignment;
    return;
  }
  // No room ahead: back to the base instead, vacating the extent's old TAIL.
  // Still a move, so a held pointer is still stale.
  memmove(ctx->payload_base, live, ctx->payload_used);
  memset(ctx->payload_base + ctx->payload_used, FePayloadPoisonByte,
         ctx->payload_start);
  ctx->payload_start = 0;
}

#endif  // FE_DEBUG_PAYLOAD_MOVE

// The mark phase's payload arm, descending half: the first traced child of
// OWNER's block, with the walk's return link left in its place. Null when the
// owner has published no block or its block has no children, which makes it a
// leaf like any other cell.
static FeObject* DescendIntoPayload(const FeContext* ctx,
                                    FeObject* owner,
                                    FeObject* parent) {
  FePayloadBlock* const block = OwnedBlock(ctx, owner);
  if (block == nullptr || block->children == 0) {
    return nullptr;
  }
  block->mark_cursor = 0;
  FeObject** const slot = PayloadChildSlot(block, 0);
  FeObject* const child = *slot;
  *slot = parent;
  return child;
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
// An object with exactly one pointer child -- `fn`, `macro`, `symbol`, whose
// `car` word is a type tag and never a pointer -- needs no state bit: its
// link is always in `cdr`, and the ascent tells it from a pair by its type.
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
  FE_PERF_INC(FePerfGcMarkVisit);
  if (~TAG(current) & GcMarkBit) {
    FE_PERF_INC(FePerfGcMarkNew);
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
    if (OwnsPayload(current)) {
      // Phase 23.1's payload arm. An owner's children live in its block, not
      // in `car`/`cdr`, so the walk descends into the block instead of into
      // the switch below -- by the same pointer reversal, which is what keeps
      // an aggregate's WIDTH off the C stack however many children it has.
      FeObject* const child = DescendIntoPayload(ctx, current, parent);
      if (child == nullptr) {
        goto ascend;
      }
      parent = current;
      current = child;
      goto descend;
    }
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
      case FeTSymbol: {
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
      // Neither a vector nor a string reaches this switch: `OwnsPayload` took
      // both above, because what they hold is in their payload block and not
      // in `car`/`cdr` -- and a string's block holds no traced child at all,
      // so the payload arm finds it a leaf. Named here so `-Wswitch-enum`
      // keeps this switch exhaustive and a future type that stops owning a
      // payload cannot fall through it silently.
      case FeTString:
      case FeTVector:
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
  if (OwnsPayload(parent)) {
    // The payload arm's ascent: put the child just finished back, then take
    // the next one. The block's cursor is the walk's position in it -- which
    // is what `GcMarkCdrBit` is for a pair, one bit being enough for two
    // halves where n children need a word.
    // Non-null by construction: the walk is here only because it descended
    // into this owner's block, and nothing between the descent and this
    // ascent can allocate, publish or compact.
    FePayloadBlock* const block = OwnedBlock(ctx, parent);
    assert(block != nullptr);
    FeObject** const done = PayloadChildSlot(block, block->mark_cursor);
    FeObject* const grandparent = *done;
    *done = current;
    if (++block->mark_cursor < block->children) {
      FeObject** const next = PayloadChildSlot(block, block->mark_cursor);
      current = *next;
      *next = grandparent;
      goto descend;
    }
    current = parent;
    parent = grandparent;
    goto ascend;
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
  FE_PERF_INC(FePerfGcCollection);
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

  // Compact: AFTER the mark phase has restored the graph it reversed, and
  // BEFORE the sweep clears the mark bits. That order is the payload
  // substrate's correctness argument, not an accident of layout -- the
  // liveness rule reads a mark bit the sweep is about to clear -- so
  // `FE_PAYLOAD_COMPACT_ORDER_BUG` exists to break it on purpose and watch
  // the substrate's tests fail. It is 0 in every configuration and no target
  // sets it; an ordering that is merely currently right is not evidence.
#if !FE_PAYLOAD_COMPACT_ORDER_BUG
  CompactPayloads(ctx);
#endif

  // Sweep and unmark:
  for (size_t i = 0; i < ctx->object_count; i++) {
    FeObject* obj = &ctx->objects[i];
    // Counted before the free-cell skip: the sweep's cost is proportional to
    // the arena, not to the live set, and that is the property this number
    // exists to show.
    FE_PERF_INC(FePerfGcSweepExamined);
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
      FE_PERF_INC(FePerfGcReclaimed);
    } else {
      TAG(obj) &= ~GcMarkBit;
    }
  }
#if FE_PAYLOAD_COMPACT_ORDER_BUG
  CompactPayloads(ctx);
#endif
  ctx->collecting = false;
}

// The public collect-now entry point (FE_API_VERSION 12): a thin wrapper so
// the internal name stays internal and a host gets exactly the same
// mark-and-sweep `ArenaCanAllocate`/`MakeObject` already run on their own
// schedule. `CollectGarbage` itself already guards re-entrancy (the
// `ctx->collecting` check above), so this adds no policy of its own.
void FeCollectGarbage(FeContext* ctx) {
  CollectGarbage(ctx);
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

bool Equal(const FeContext* ctx, FeObject* a, FeObject* b) {
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
    // Length, then bytes. Both addresses are derived inside the expression
    // that spends them and nothing here allocates, so neither can go stale
    // (clause 1). A length test first is not an optimisation but the whole
    // comparison for the common case: two strings of different lengths are
    // unequal without either one's bytes being touched.
    const size_t length = StringLength(a);
    return length == StringLength(b) &&
           memcmp(StringBytes(ctx, a), StringBytes(ctx, b), length) == 0;
  }
  return false;
}

// The string a `string<`/`string>` operand designates. Emacs takes
// a string or a SYMBOL on either side, measured on 31.0.90: `(string< 'abc
// "abd")` and `(string< "abc" 'abd)` are both t, and `(string< "n" nil)` is t
// because nil's name is "nil" -- fe's nil owns no name chain, so it is the
// one operand that has to be built. Anything else is `(wrong-type-argument
// stringp X)` naming the operand, which is Emacs' own answer for `(string<
// "a" 1)` and for `(string< 1 "a")` alike.
static FeObject* StringOperand(FeContext* ctx, FeObject* obj) {
  if (FeIsNil(obj)) {
    return FeMakeString(ctx, "nil");
  }
  const FeType type = FeGetType(obj);
  if (type == FeTSymbol) {
    return SymbolName(obj);
  }
  if (type != FeTString) {
    RaiseWrongType(ctx, "stringp", obj);
  }
  return obj;
}

// Phase 20's `string<`, and `string>` with the operands swapped. Emacs
// compares by CHARACTER; this compares by BYTE, and the two agree for every
// string either dialect can hold, because UTF-8 preserves codepoint order
// under byte-lexicographic comparison -- measured against the oracle at the
// boundary that would show it, `(string< "é" "z")` being nil in both.
//
// One `memcmp` over the bytes the two share, then the lengths: a prefix is
// less than what extends it, and equal lengths that compare equal are not
// less. That is exactly Emacs' rule, and it is also why the comparison could
// not be written this way before Phase 25 -- a string had no length to stop
// at, so the old chain walk leaned on the NUL padding of a short last cell to
// order a prefix first. An embedded NUL is byte 0 here and sorts before every
// other byte, which is what Emacs' character comparison says too.
//
// `left` is rooted across the second coercion because that one can allocate
// (the nil operand above): the operands themselves are frame fields and
// rooted by the caller, but a freshly built "nil" is not. Both byte addresses
// are derived after the last allocation and spent in the same statement.
bool StringOperandLess(FeContext* ctx, FeObject* a, FeObject* b) {
  const size_t gc = FeSaveGC(ctx);
  FeObject* left = StringOperand(ctx, a);
  FePushGC(ctx, left);
  FeObject* right = StringOperand(ctx, b);
  FeRestoreGC(ctx, gc);
  const size_t left_length = StringLength(left);
  const size_t right_length = StringLength(right);
  const size_t shared = left_length < right_length ? left_length : right_length;
  const int order =
      memcmp(StringBytes(ctx, left), StringBytes(ctx, right), shared);
  return order != 0 ? order < 0 : left_length < right_length;
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

// Does this string hold exactly the bytes of the C string STR? The length
// decides most calls on its own -- the obarray scan asks this once per
// interned symbol -- and only a length match reaches the bytes, which is why
// `FePerfNameByte` is charged there and not before. A string with an embedded
// NUL is never equal to a C string, because its length says so.
static bool IsStringEqual(const FeContext* ctx,
                          const FeObject* obj,
                          const char* str) {
  FE_PERF_INC(FePerfNameCompare);
  const size_t length = StringLength(obj);
  if (strlen(str) != length) {
    return false;
  }
  FE_PERF_ADD(FePerfNameByte, length);
  return memcmp(StringBytes(ctx, obj), str, length) == 0;
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
  FE_PERF_INC(FePerfInternLookup);
  for (FeObject* rest = ctx->symbol_list; !FeIsNil(rest); rest = CDR(rest)) {
    FE_PERF_INC(FePerfInternCandidate);
    if (IsStringEqual(ctx, SymbolName(CAR(rest)), name)) {
      return CAR(rest);
    }
  }
  FE_PERF_INC(FePerfInternMiss);
  return nullptr;
}

static void InitializeKeywordValue(FeObject* symbol, const char* name) {
  if (IsKeywordName(name)) {
    CDR(SymbolBindingCell(symbol)) = symbol;
  }
}

// A REPLACEMENT block starts with the contents the old one held, up to what
// it can take. This is how a string grows -- the region is a bump allocator,
// so a block cannot be extended in place -- and it is what makes the growth
// invisible from outside: the header is the same object and the handle inside
// it is the only thing that changed. Both addresses are derived HERE, after
// the allocation that may have compacted the old block, and spent before the
// store that makes the old block dead (clause 2). A replacement keeps its
// owner's child count, so the copy cannot land a traced word in byte
// territory or the reverse.
static void CopyReplacedPayload(const FeContext* ctx,
                                FePayloadHandle previous,
                                FePayloadHandle handle) {
  if (previous == FePayloadNone) {
    return;
  }
  const FePayloadBlock* const from = BlockAtHandle(ctx, previous);
  const FePayloadBlock* const to = BlockAtHandle(ctx, handle);
  assert(from->children == to->children);
  memcpy(PayloadBytes(ctx, handle), PayloadBytes(ctx, previous),
         from->bytes < to->bytes ? from->bytes : to->bytes);
}

// Clause 3 of the publish protocol: the ONE way a block reaches its owner.
// Allocation and publication are one function on purpose -- an allocated
// block that no owner names yet is exactly the thing the compactor reclaims,
// so there is no safe moment between them for a caller to stand in.
void PublishPayload(FeContext* ctx,
                    FeObject* owner,
                    size_t children,
                    size_t bytes) {
  FePayloadHandle* const slot = PayloadSlot(owner);
  // A payload allocation is an allocation, so it invalidates outstanding
  // payload pointers exactly as a cell allocation does (clause 2).
  FE_POISON_PAYLOAD_POINTERS(ctx);
  // Rooted because the collection below may run, and a collection with the
  // owner reachable from nowhere else sweeps it -- after which this function
  // would publish a handle into a free cell. Clause 3 is this
  // Save/Push/Restore, not the store at the end.
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, owner);
  FePayloadHandle handle = AllocatePayloadBlock(ctx, children, bytes);
  if (handle == FePayloadNone) {
    // The region is full of blocks nothing has been asked about yet: collect,
    // which marks, compacts and sweeps, and try once more. A second failure
    // is a region that genuinely cannot hold the request.
    CollectGarbage(ctx);
    handle = AllocatePayloadBlock(ctx, children, bytes);
  }
  if (handle == FePayloadNone) {
    ctx->payload_allocation_failures++;
    FeRestoreGC(ctx, gc);
    RaisePayloadExhaustion(ctx);
  }
  BlockAtHandle(ctx, handle)->owner = owner;
  CopyReplacedPayload(ctx, *slot, handle);
  // Last, and only now: any block this owner named before this line is dead
  // from here on, though the owner itself is very much alive. That is the
  // replacement half of the liveness rule.
  *slot = handle;
  FeRestoreGC(ctx, gc);
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
  // Clause 2 of the publish protocol, made to happen at every allocation
  // rather than at the rare one that would naturally compact. Compiles to
  // nothing unless the poison lane armed it.
  FE_POISON_PAYLOAD_POINTERS(ctx);
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
  FE_PERF_INC(FePerfAllocObject);
  if (ctx->arena_live_count > ctx->arena_peak_live_count) {
    ctx->arena_peak_live_count = ctx->arena_live_count;
  }
  FePushGC(ctx, obj);
  return obj;
}

#if FE_PAYLOAD_TEST_OBJECT

// The substrate's test object (Phase 23.1). See its declaration in
// fe_internal.h for why it exists only under this knob and why it borrows the
// extension type slot.
//
// The order here is the one every Phase 24/25 constructor will copy: retype
// the cell, clear its handle, THEN publish. The window between the retype and
// the publish is one a collection can land in -- `PublishPayload` allocates --
// so the cleared handle is what tells the collector this owner has no block
// yet.
FeObject* MakeAggregate(FeContext* ctx, size_t children, size_t bytes) {
  FeObject* const obj = MakeObject(ctx);
  SetType(obj, FeTFex2);
  PAYLOAD(obj) = FePayloadNone;
  PublishPayload(ctx, obj, children, bytes);
  for (size_t i = 0; i < children; i++) {
    // A zero-filled slot is a null pointer, which is not an object; nothing
    // may mark this block until every child is nil. No allocation runs in
    // this loop, which is what makes that true.
    *PayloadChildSlot(OwnedBlock(ctx, obj), i) = &nil;
  }
  return obj;
}

FeObject* AggregateChild(const FeContext* ctx,
                         FeObject* aggregate,
                         size_t index) {
  return *PayloadChildSlot(OwnedBlock(ctx, aggregate), index);
}

void SetAggregateChild(const FeContext* ctx,
                       FeObject* aggregate,
                       size_t index,
                       FeObject* value) {
  *PayloadChildSlot(OwnedBlock(ctx, aggregate), index) = value;
}

unsigned char* AggregateBytes(const FeContext* ctx, FeObject* aggregate) {
  FePayloadBlock* const block = OwnedBlock(ctx, aggregate);
  return (unsigned char*)block + sizeof(FePayloadBlock) +
         block->children * sizeof(FeObject*);
}

FePayloadHandle AggregateHandle(const FeObject* aggregate) {
  return PAYLOAD(aggregate);
}

#endif  // FE_PAYLOAD_TEST_OBJECT

// ---------------------------------------------------------------------------
// Vectors (Phase 24 of kg's Elisp data-model program): the payload
// substrate's first release consumer.
//
// A vector IS a header plus a payload block whose traced children are its
// elements. Nothing else: no length word, no capacity, no separate element
// array. Three consequences worth stating once, because every function below
// is one of them:
//
//   * `length` is the block's `children` count, so it is a field read and not
//     a walk -- which is what makes `aref`, `aset` and `length` O(1) in the
//     vector's size, the property Phase 24's counter gate exists to show;
//   * the collector reaches the elements through the payload arm the ADR's
//     Design B already built (`DescendIntoPayload`, `FeMark`), so a vector of
//     any width costs the mark phase no C stack and needs no new arm;
//   * the elements MOVE when the compactor runs. Every read and write below
//     therefore derives the block address immediately before spending it,
//     which is clause 1 of the publish protocol in fe_internal.h, and none of
//     them hands an interior pointer to anyone -- `fe.h` has no vector
//     accessor that could.
// ---------------------------------------------------------------------------

// The block, or null for a vector whose constructor has retyped its cell and
// not yet published. That window is one a collection can land in, and a
// vector inside it is a zero-length one -- which is the honest answer, since
// it has no elements yet.
size_t VectorLength(const FeContext* ctx, FeObject* vector) {
  const FePayloadBlock* const block = OwnedBlock(ctx, vector);
  return block == nullptr ? 0 : block->children;
}

FeObject* VectorElement(const FeContext* ctx, FeObject* vector, size_t index) {
  FE_PERF_INC(FePerfVectorRef);
  return *PayloadChildSlot(OwnedBlock(ctx, vector), index);
}

void SetVectorElement(const FeContext* ctx,
                      FeObject* vector,
                      size_t index,
                      FeObject* value) {
  FE_PERF_INC(FePerfVectorSet);
  *PayloadChildSlot(OwnedBlock(ctx, vector), index) = value;
}

// The constructor, in the order `MakeAggregate` established and every payload
// owner copies: retype the cell, clear its handle, THEN publish. `init` fills
// every slot with ONE object rather than a copy per slot, which is Emacs'
// `make-vector` contract and is why `(eq (aref v 0) (aref v 1))` is t.
//
// `init` survives the publish because `PublishPayload` may collect: it is
// rooted here, and the vector is rooted by `MakeObject` having pushed it.
// The fill loop allocates nothing, which is what lets it derive the block
// once per write and never park it.
static FeObject* MakeVector(FeContext* ctx, size_t length, FeObject* init) {
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, init);
  FeObject* const vector = MakeObject(ctx);
  SetType(vector, FeTVector);
  PAYLOAD(vector) = FePayloadNone;
  PublishPayload(ctx, vector, length, 0);
  FE_PERF_ADD(FePerfVectorElement, length);
  for (size_t i = 0; i < length; i++) {
    SetVectorElement(ctx, vector, i, init);
  }
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, vector);
  return vector;
}

// A vector of the `length` elements a proper list holds, which is what the
// reader's `[...]`, `vector` and `vconcat` all end in. The list is rooted
// across the construction, and nothing between two element writes allocates.
static FeObject* MakeVectorFromList(FeContext* ctx,
                                    FeObject* list,
                                    size_t length) {
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, list);
  FeObject* const vector = MakeVector(ctx, length, &nil);
  for (size_t i = 0; i < length; i++) {
    SetVectorElement(ctx, vector, i, CAR(list));
    list = CDR(list);
  }
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, vector);
  return vector;
}

// The public quartet's type gate. A non-vector is `(wrong-type-argument
// vectorp OBJ)`, which is what `CheckType` already spells for every other
// type through `TypePredicate`.
static FeObject* CheckVector(FeContext* ctx, FeObject* vector) {
  return CheckType(ctx, vector, FeTVector);
}

// The bounds gate, shared by the public accessors and by Lisp's `aref`/`aset`
// because it is the same check: Emacs' data is `(SEQUENCE INDEX)` -- the
// offending object FIRST -- and a negative index takes the same route, which
// is why the Lisp side hands the index through as a signed value and this
// side never sees one.
[[noreturn]] static void RaiseOutOfRange(FeContext* ctx,
                                         FeObject* sequence,
                                         FeObject* index) {
  FeObject* items[] = {sequence, index};
  RaiseCondition(ctx, FeCompletionError, "args-out-of-range",
                 FeMakeList(ctx, items, COUNT(items)), "args-out-of-range");
}

FeObject* FeMakeVector(FeContext* ctx, size_t length) {
  return MakeVector(ctx, length, &nil);
}

size_t FeVectorLength(FeContext* ctx, FeObject* vector) {
  return VectorLength(ctx, CheckVector(ctx, vector));
}

FeObject* FeVectorRef(FeContext* ctx, FeObject* vector, size_t index) {
  CheckVector(ctx, vector);
  if (index >= VectorLength(ctx, vector)) {
    RaiseOutOfRange(ctx, vector, FeMakeInteger(ctx, (int64_t)index));
  }
  return VectorElement(ctx, vector, index);
}

void FeVectorSet(FeContext* ctx,
                 FeObject* vector,
                 size_t index,
                 FeObject* value) {
  CheckVector(ctx, vector);
  if (index >= VectorLength(ctx, vector)) {
    RaiseOutOfRange(ctx, vector, FeMakeInteger(ctx, (int64_t)index));
  }
  SetVectorElement(ctx, vector, index, value);
}

FeObject* FeCons(FeContext* ctx, FeObject* car, FeObject* cdr) {
  FeObject* obj = MakeObject(ctx);
  // A pair is the one cell whose type is never spelled: writing `car` with
  // an aligned pointer is what makes it one, so this is the by-type charge's
  // only site outside `SetType`.
  FE_PERF_ALLOC(FeTPair);
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

// ---------------------------------------------------------------------------
// Making a string (Phase 25). `StringLength`/`StringBytes` above are the read
// surface; these four are how bytes get in. Two are constructors, and the
// other two are the reader's, which is the only place a string's length
// changes after it is built.
// ---------------------------------------------------------------------------

// A string owning CAPACITY bytes and holding none of them yet. The order is
// `MakeVector`'s and `MakeAggregate`'s -- retype, clear the handle, publish --
// because `PublishPayload` allocates and a collection landing in that window
// must find an owner that coherently owns nothing. The LENGTH is zeroed in
// the same window, for the reason a fresh block is zero-filled: a recycled
// cell's `car` bytes are the previous object's, and a string whose length word
// said 4000 would be a string the printer reads 4000 bytes of.
static FeObject* MakeStringObject(FeContext* ctx, size_t capacity) {
  // The length word is `StringLengthBytes` bytes wide, so a string longer than
  // it can name is refused rather than truncated. Unreachable on any machine
  // whose arena fits in memory -- a region of 2^56 bytes would have to exist
  // first -- and cheaper than the argument that says so.
  if (capacity >> (StringLengthBytes * 8) != 0) {
    RaisePayloadExhaustion(ctx);
  }
  FeObject* const obj = MakeObject(ctx);
  SetType(obj, FeTString);
  SetStringLength(obj, 0);
  PAYLOAD(obj) = FePayloadNone;
  FE_PERF_INC(FePerfStringObject);
  PublishPayload(ctx, obj, 0, capacity);
  return obj;
}

FeObject* FeMakeStringBytes(FeContext* ctx, const char* bytes, size_t length) {
  FeObject* const obj = MakeStringObject(ctx, length);
  SetStringLength(obj, length);
  FE_PERF_ADD(FePerfStringByte, length);
  if (length != 0) {
    // Derived after the last allocation this constructor makes -- the publish
    // inside `MakeStringObject` -- and spent in the same statement.
    memcpy(StringBytes(ctx, obj), bytes, length);
  }
  return obj;
}

FeObject* FeMakeString(FeContext* ctx, const char* str) {
  return FeMakeStringBytes(ctx, str, strlen(str));
}

// The reader's literal builder, and the only place a string's length changes
// after construction. Growth is geometric because the reader learns a
// literal's length one byte at a time and a block cannot be extended in place
// -- the region is a bump allocator, so the next block begins where this one
// ends. Republishing per byte would copy the whole literal per byte; doubling
// copies it a bounded number of times.
//
// What a replacement does NOT change is the object: the string handed back to
// the reader is the same `FeObject*` before and after, because a new block
// reaches its owner through `PublishPayload` and the owner is a header the
// compactor never moves. That is the property Phase 25's mutation gate names,
// and the reader is where a release build exercises it.
static void AppendStringByte(FeContext* ctx, FeObject* string, char chr) {
  const size_t length = StringLength(string);
  if (length == StringCapacity(ctx, string)) {
    PublishPayload(ctx, string, 0,
                   length == 0 ? FePayloadAlignment : length * 2);
  }
  SetStringLength(string, length + 1);
  FE_PERF_INC(FePerfStringByte);
  // Derived after the publish above, which is the allocation this statement
  // must not be hoisted over.
  StringBytes(ctx, string)[length] = (unsigned char)chr;
}

// ...and the literal is finished: give back whatever the doubling overshot, so
// that every string in the tree has the capacity `FeMakeStringBytes` would
// have given it and the reader's growth policy is nothing the rest of fe has
// to know about.
static void FinishString(FeContext* ctx, FeObject* string) {
  const size_t length = StringLength(string);
  if (StringCapacity(ctx, string) != RoundUpToAlignment(length)) {
    PublishPayload(ctx, string, 0, length);
  }
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

static size_t CopyStringBytes(const FeContext* ctx,
                              const FeObject* string,
                              char* dst);
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
// and reads it. Never recurses.
//
// The whole-name test needs the name in one buffer, and `SymbolNameLimit` is
// the longest name fe itself ever builds; a longer one can only come from an
// embedder's `FeMakeSymbol`, and it gets the per-byte escapes without the
// number-lookalike test, which no name that long can pass here anyway.
//
// The byte address is derived inside the loop, once per byte, and never
// hoisted: `Emit` reaches the host's `FeWriteFn`, which fe.h says may
// allocate, and an allocation may compact this name's bytes out from under an
// address taken before it. That is clause 2 of the publish protocol, and this
// loop plus `EmitStoredString`'s are the two places in fe that pay it.
static void EmitSymbolName(Writer* w, FeObject* name) {
  const size_t length = StringLength(name);
  if (length == 0) {
    EmitString(w, "##");
    return;
  }
  bool escape_first = false;
  if (length <= SymbolNameLimit) {
    // Zero-initialized so the first-byte tests below read a defined byte even
    // to an analyzer that cannot see `CopyStringBytes` filling it: `length` is
    // nonzero here, so it always does.
    char buf[SymbolNameLimit + 1] = "";
    (void)CopyStringBytes(w->ctx, name, buf);
    buf[length] = '\0';
    escape_first = IsConfusingSymbolName(buf);
  }
  for (size_t i = 0; i < length; i++) {
    const char chr = (char)StringBytes(w->ctx, name)[i];
    if ((i == 0 && escape_first) || IsAlwaysEscapedSymbolByte(chr)) {
      Emit(w, '\\');
    }
    Emit(w, chr);
  }
}

// A string or a symbol's name. Never recurses, and derives the byte address
// once per byte for the reason `EmitSymbolName` above states.
//
// THREE bytes `prin1` escapes inside a string. Two are the ones the reader
// would otherwise take for itself: the closing quote, and the backslash that
// introduces an escape. Escaping only the quote (which is what this did until
// Phase 19) left `(prin1 "x\\y")` printing `"x\y"`, which reads back as
// `"xy"` -- the one printed form in fe that was not re-readable once Phase 14
// made backslashes ordinary bytes rather than a read error.
//
// The third is NUL, and it is Phase 25's, in three octal digits so that a
// digit after it stays a digit. Emacs prints an embedded NUL raw and fe
// cannot: a `FeReadFn` answers one `char` and spells end of input as 0, so a
// raw NUL in a literal is where the reader stops. Escaping it is what makes
// the round trip the phase's first gate names -- print, read back, `equal` --
// hold for a string with a NUL in it, and it is a printed-form divergence of
// exactly the class `reader-string-raw-byte-printing` already records in the
// other direction. Nothing else is escaped, Emacs included: a newline inside
// a string prints as a newline there too.
static void EmitStoredString(Writer* w, FeObject* obj, int qt) {
  if (qt) {
    Emit(w, '"');
  }
  const size_t length = StringLength(obj);
  for (size_t i = 0; i < length; i++) {
    const char chr = (char)StringBytes(w->ctx, obj)[i];
    if (qt && chr == '\0') {
      EmitString(w, "\\000");
      continue;
    }
    if (qt && (chr == '"' || chr == '\\')) {
      Emit(w, '\\');
    }
    Emit(w, chr);
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

// A vector's elements, without the surrounding brackets. Deliberately shaped
// like `WriteElements` above and deliberately unlike it in one way: there is
// no cycle detector here, and there is nothing for one to detect on a spine,
// because a vector has none. A vector that contains ITSELF is finite work all
// the same -- every element costs a level of `depth`, so the recursion ends
// in `#<deep>` exactly as a `(setcar x x)` list already does. Emacs prints
// `[0 #0]` for that shape; fe's deliberately bounded writer is the recorded
// `writer-bounded-output` policy and this is it, unchanged, applied to a new
// type. doc/language.md states the decision and why it was not `#0`.
//
// The block address is derived per element, inside `VectorElement`, and never
// hoisted: `WriteObject` below reaches the host's `FeWriteFn`, whose contract
// does not forbid allocation, and an allocation compacts. That is census row
// A6-A10's rule met by a new site rather than inherited from an old one.
static void WriteVectorElements(Writer* w, FeObject* obj, size_t depth) {
  const size_t length = VectorLength(w->ctx, obj);
  for (size_t i = 0; i < length; i++) {
    if (i != 0) {
      Emit(w, ' ');
    }
    if (w->bytes == 0 || w->nodes == 0) {
      EmitString(w, "#<truncated>");
      w->complete = false;
      return;
    }
    WriteObject(w, VectorElement(w->ctx, obj, i), w->nested_qt, depth);
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
  if (IsNamedSymbol(w->ctx, CAR(obj), "function")) {
    prefix = "#'";
  } else if (IsNamedSymbol(w->ctx, CAR(obj), "quote")) {
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

    // Phase 24: the reader's own syntax back out, which is what makes
    // `[1 (2) "x"]` print as Emacs prints it.
    case FeTVector:
      Emit(w, '[');
      WriteVectorElements(w, obj, depth - 1);
      Emit(w, ']');
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

// The bytes into DST, and the length either way -- a null DST asks for the
// length alone, which is a field read now rather than a walk. Every byte copy
// in fe and behind `fe.h` funnels through here, which is what keeps the number
// of places that derive a payload address countable.
static size_t CopyStringBytes(const FeContext* ctx,
                              const FeObject* string,
                              char* dst) {
  const size_t length = StringLength(string);
  FE_PERF_INC(FePerfStringCopy);
  if (dst != nullptr && length != 0) {
    FE_PERF_ADD(FePerfStringByteCopied, length);
    memcpy(dst, StringBytes(ctx, string), length);
  }
  return length;
}

size_t FeStringByteLength(FeContext* ctx, const FeObject* obj) {
  return CopyStringBytes(ctx, GetStringObject(ctx, obj), nullptr);
}

bool FeCopyStringBytes(FeContext* ctx,
                       const FeObject* obj,
                       char* dst,
                       size_t size) {
  const FeObject* string = GetStringObject(ctx, obj);
  const size_t length = CopyStringBytes(ctx, string, nullptr);
  if (size < length || (dst == nullptr && length != 0)) {
    return false;
  }
  (void)CopyStringBytes(ctx, string, dst);
  return true;
}

size_t FeStringBytes(FeContext* ctx,
                     const FeObject* obj,
                     char* dst,
                     size_t size) {
  const FeObject* const string = GetStringObject(ctx, obj);
  return CopyStringBytes(ctx, string,
                         StringLength(string) <= size ? dst : nullptr);
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
  FE_PERF_INC(FePerfEnvLookup);
  // Try to find the symbol in the environment:
  for (; !FeIsNil(env); env = CDR(env)) {
    FE_PERF_INC(FePerfEnvCell);
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
  if (FeIsNil(sym) || IsConstantSymbol(ctx, sym)) {
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
// Phase 24: `]`'s own sentinel. A second one rather than reusing `rparen`,
// because the two are not interchangeable in either direction -- `[1 2)` and
// `(1 2]` are both errors on the pinned Emacs, and one sentinel would make
// each of them close the other's opener silently.
static FeObject rbracket;

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

// The reader's whitespace, byte for byte the set Emacs' own `read1` retries
// on: space, form feed, newline, tab, carriage return. The form feed (0x0C)
// is the one that was missing. Elisp files use it as a page separator
// (`s.el:770`, `f.el:39`), and while it was not in this set it was an
// ordinary symbol constituent, so `nil\f\nnil` read as the single symbol
// named "nil\f" and answered `void-variable` where Emacs reads two `nil`s.
// It is spelled once here and pasted into the delimiter sets below, so the
// three places that ask "does this byte end a token?" cannot drift apart
// from the one that asks "does this byte separate them?". A form feed inside
// a STRING body is not reader syntax at all: `ReadStringLiteral` copies
// every byte up to the closing quote and never consults this.
#define ReaderWhitespace " \f\n\t\r"

// The bytes that END a token, whitespace included: whitespace separates
// tokens, these also terminate one that is already running. Spelled once for
// the reason above -- the three places that ask the question must not drift
// -- and Phase 24 is why it is a macro of its own rather than a string pasted
// three times: `[` and `]` joined it, and a set that had gained them in two
// places out of three would read `[1 2 3]` as `1 2 3]` and then blame the
// closing paren of whatever form it was inside.
#define ReaderTokenEnd ReaderWhitespace "();`,[]"

// A number, `nil`, or a symbol. `chr` is the first character; a character
// already pushed back into `ctx->nextchr` is consumed before the input.
static FeObject* ReadAtom(FeContext* ctx, FeReadFn fn, void* udata, char chr) {
  char buf[SymbolNameLimit + 1];
  char* p = buf;
  const char* delimiter = ReaderTokenEnd;
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
  if (chr != '\0' && strchr(ReaderTokenEnd "\"'", chr) == NULL) {
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
  while (chr && !strchr(ReaderTokenEnd, chr)) {
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

bool IsNamedSymbol(const FeContext* ctx, const FeObject* v, const char* name) {
  return FeGetType(v) == FeTSymbol && IsStringEqual(ctx, SymbolName(v), name);
}

// A keyword is an INTERNED symbol whose name starts with a colon (08B). The
// self-binding `InitializeKeywordValue` gives one at intern time is what says
// "interned" here: Phase 14's `make-symbol` skips it, so an uninterned symbol
// named `:a` is an ordinary unbound symbol, which is what the pinned Emacs
// answers too -- `(keywordp (make-symbol ":a"))` is nil there and
// `(symbol-value (make-symbol ":a"))` is `(void-variable :a)`. Nothing can
// clear an interned keyword's self-binding: `setq`, `set`, `makunbound` and
// `let` all refuse a constant, and a keyword is one.
bool IsKeywordSymbol(const FeContext* ctx, const FeObject* v) {
  if (FeGetType(v) != FeTSymbol) {
    return false;
  }
  const FeObject* const name = SymbolName(v);
  return StringLength(name) != 0 && StringBytes(ctx, name)[0] == ':' &&
         CDR(CDR(v)) == v;
}

bool IsConstantSymbol(const FeContext* ctx, const FeObject* v) {
  return IsNamedSymbol(ctx, v, "t") || IsKeywordSymbol(ctx, v);
}

static bool IsDot(const FeContext* ctx, const FeObject* v) {
  return IsNamedSymbol(ctx, v, ".");
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

// A fe string is a byte string, so a string escape has to land in one byte.
// A decoded NUL is now one of them -- Phase 25's strings carry a length, so a
// stored NUL is a byte like any other and `"a\0b"` is three bytes long, which
// is Emacs' answer too. A value ABOVE 255 is still a named error: `"\400"` is
// the character U+0100 in Emacs, and holding it would mean a multibyte string
// type rather than a length. That half of the divergence is recorded in
// `compat/features.json` and `doc/language.md`.
static int ReadStringEscape(FeContext* ctx, FeReadFn fn, void* udata) {
  const int value = ReadEscape(ctx, fn, udata);
  if (value > 0xff) {
    FeHandleError(ctx,
                  "unsupported read syntax: character above 255 in string");
  }
  return value;
}

// The literal is built into one string object that grows, rather than
// accumulated anywhere first: a `FeReadFn` answers one byte at a time and
// cannot be rewound, so the length is not known until the closing quote.
// `res` is on the GC stack from `MakeStringObject` onwards, which is what
// keeps it alive across the host read callback and across its own growth --
// both of which allocate. No payload address survives either.
//
// It opens at one alignment unit because that is the smallest block the
// allocator hands out anyway, so a literal that fits in it costs one publish
// rather than two. A raw NUL byte in the SOURCE is still the end of input:
// `FeReadFn` answers a `char` and spells exhaustion as 0, which is why `\0`
// (or `\000`) is the way to write one, and why the writer escapes it.
static FeObject* ReadStringLiteral(FeContext* ctx, FeReadFn fn, void* udata) {
  FeObject* const res = MakeStringObject(ctx, FePayloadAlignment);
  char chr = fn(ctx, udata);
  while (chr != '"') {
    if (chr == '\0')
      FeHandleError(ctx, "unclosed string");
    if (chr == '\\') {
      chr = (char)ReadStringEscape(ctx, fn, udata);
    }
    AppendStringByte(ctx, res, chr);
    chr = ctx->nextchr ? ctx->nextchr : fn(ctx, udata);
    ctx->nextchr = '\0';
  }
  FinishString(ctx, res);
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
      RaiseNamedError(ctx, "end-of-file", "end-of-file: unclosed list");
    }
    if (v == &rbracket) {
      FeHandleError(ctx, "stray ']'");
    }
    // An ESCAPED dot is an ordinary symbol, so `(a \. b)` is a three-element
    // list where `(a . b)` is a pair (Phase 14). The two read to the same
    // interned symbol -- `\.` is not a different object -- so the object
    // alone cannot answer this and the reader's own flag has to.
    if (IsDot(ctx, v) && !ctx->reader_atom_escaped) {
      if (FeIsNil(res)) {
        FeHandleError(ctx, "'.' at start of list");
      }
      // Only the internal `Read` reports ')' as `&rparen`; `FeRead` would turn
      // it into the misleading `stray ')'`.
      v = Read(ctx, fn, udata);
      if (v == NULL) {
        RaiseNamedError(ctx, "end-of-file", "end-of-file: unclosed list");
      }
      if (v == &rparen) {
        FeHandleError(ctx, "missing value after '.'");
      }
      *tail = v;
      FeRestoreGC(ctx, gc);
      FePushGC(ctx, res);
      v = Read(ctx, fn, udata);
      if (v == NULL) {
        RaiseNamedError(ctx, "end-of-file", "end-of-file: unclosed list");
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

// Reads the rest of a vector, the opening '[' already consumed. The elements
// go into a list first and the list into a block of exactly that length,
// because the length is not known until `]` arrives and the input is a
// stream: the alternatives are re-publishing a growing block per element,
// which is quadratic copying, or a second pass over input there is no way to
// rewind. The transient list is n pairs of garbage, collected like any other
// reader garbage, and the GC discipline is `ReadList`'s exactly -- one slot,
// restored per element, so a 100 000-element literal costs the root stack
// one entry and not 100 000.
//
// A '.' is a syntax error here rather than the dotted-tail marker it is
// inside a list: `[1 . 2]` is `invalid-read-syntax` on the pinned Emacs, and
// Phase 8's rule for this reader is reject rather than misread -- accepting
// it would silently produce the three-element vector `[1 \. 2]`.
static FeObject* ReadVector(FeContext* ctx, FeReadFn fn, void* udata) {
  FeObject* list = &nil;
  FeObject** tail = &list;
  size_t length = 0;
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, list);  // To cause error on too-deep nesting
  FeObject* v;
  while ((v = Read(ctx, fn, udata)) != &rbracket) {
    if (v == NULL) {
      RaiseNamedError(ctx, "end-of-file", "end-of-file: unclosed vector");
    }
    if (v == &rparen) {
      FeHandleError(ctx, "stray ')'");
    }
    if (IsDot(ctx, v) && !ctx->reader_atom_escaped) {
      FeHandleError(ctx, "'.' inside a vector");
    }
    *tail = FeCons(ctx, v, &nil);
    tail = &CDR(*tail);
    length++;
    FeRestoreGC(ctx, gc);
    FePushGC(ctx, list);
  }
  FeObject* const vector = MakeVectorFromList(ctx, list, length);
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, vector);
  return vector;
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
    while (chr && strchr(ReaderWhitespace, chr)) {
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

  switch (chr) {
    case '\0':
      return NULL;

    case ')':
      return &rparen;

    case ']':
      return &rbracket;

    case '[':
      return ReadVector(ctx, fn, udata);

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
  if (obj == &rbracket) {
    FeHandleError(ctx, "stray ']'");
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
  const size_t length = CopyStringBytes(ctx, string, nullptr);
  if (length > SymbolNameLimit) {
    FeHandleError(ctx, "symbol name too long (63-byte limit)");
  }
  (void)CopyStringBytes(ctx, string, buf);
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
    const size_t length = CopyStringBytes(ctx, SymbolName(argument), nullptr);
    if (length > SymbolNameLimit) {
      return &nil;
    }
    (void)CopyStringBytes(ctx, SymbolName(argument), name);
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
  const size_t length = CopyStringBytes(ctx, stored, nullptr);
  if (length > SymbolNameLimit) {
    FeHandleError(ctx, "symbol name too long (63-byte limit)");
  }
  (void)CopyStringBytes(ctx, stored, name);
  return FeMakeStringBytes(ctx, name, length);
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

bool IsVectorPrimitive(Primitive primitive) {
  return primitive >= PVector && primitive <= PElt;
}

// A LENGTH operand. Emacs answers `(wrong-type-argument wholenump N)` for a
// negative length and for a float -- measured, `(make-vector -1 0)` and
// `(make-vector 2.0 0)` both -- rather than treating either as an arity or a
// range problem.
static size_t LengthArgument(FeContext* ctx, FeObject* obj) {
  if (FeGetType(obj) != FeTInteger || INTEGER(obj) < 0) {
    RaiseWrongType(ctx, "wholenump", obj);
  }
  return (size_t)INTEGER(obj);
}

// An INDEX operand. A non-integer is `(wrong-type-argument fixnump I)`,
// Emacs' own predicate for the position of an array element; a NEGATIVE
// integer is not rejected here, because Emacs reports it as out of RANGE and
// with the index it was given -- `(aref [10 20] -1)` is `(args-out-of-range
// [10 20] -1)` -- which is why this returns a signed value the bounds check
// below tests rather than a `size_t` that would already have wrapped.
static int64_t IndexArgument(FeContext* ctx, FeObject* obj) {
  if (FeGetType(obj) != FeTInteger) {
    RaiseWrongType(ctx, "fixnump", obj);
  }
  return INTEGER(obj);
}

// The `index`th stored byte, which since Phase 25 is the same arithmetic a
// vector's element is: one derivation of the block, one read, no walk. The
// caller has already bounds-checked against `StringLength`.
static unsigned char StringByteAt(const FeContext* ctx,
                                  const FeObject* string,
                                  size_t index) {
  return StringBytes(ctx, string)[index];
}

// How many elements a SEQUENCE has, which is `length`'s answer and the sum
// `vconcat` sizes its result from. The three sequence types and nothing else:
// anything not a list, a string or a vector is `(wrong-type-argument
// sequencep X)`, and a list whose tail is not nil is `(wrong-type-argument
// listp TAIL)` -- about the offending TAIL, which is Emacs' answer for
// `(length '(1 . 2))` too.
//
// A string's answer is its BYTE count. fe strings are byte strings (the
// recorded `reader-string-byte-limit` family), so `(length "é")` is 3
// here and 2 on Emacs, whose strings are sequences of characters; that is the
// existing divergence showing through a new name rather than a new one.
static size_t SequenceCount(FeContext* ctx, FeObject* sequence) {
  const FeType type = FeGetType(sequence);
  if (type == FeTVector) {
    return VectorLength(ctx, sequence);
  }
  if (type == FeTString) {
    return StringLength(sequence);
  }
  if (type != FeTNil && type != FeTPair) {
    RaiseWrongType(ctx, "sequencep", sequence);
  }
  size_t count = 0;
  while (FeGetType(sequence) == FeTPair) {
    count++;
    sequence = CDR(sequence);
  }
  if (!FeIsNil(sequence)) {
    RaiseWrongType(ctx, "listp", sequence);
  }
  return count;
}

// `(aref ARRAY INDEX)`. An ARRAY is a vector or a string -- strings are
// arrays in Emacs, which is why the wrong-type predicate is `arrayp` and not
// `vectorp`, the one detail Phase 24.0 froze against the oracle before there
// was an implementation to get it wrong. A string element is its byte.
static FeObject* Aref(FeContext* ctx, FeObject* array, FeObject* index_obj) {
  const int64_t index = IndexArgument(ctx, index_obj);
  const FeType type = FeGetType(array);
  if (type != FeTVector && type != FeTString) {
    RaiseWrongType(ctx, "arrayp", array);
  }
  const size_t length =
      type == FeTVector ? VectorLength(ctx, array) : StringLength(array);
  if (index < 0 || (uint64_t)index >= length) {
    RaiseOutOfRange(ctx, array, index_obj);
  }
  return type == FeTVector
             ? VectorElement(ctx, array, (size_t)index)
             : FeMakeInteger(ctx, StringByteAt(ctx, array, (size_t)index));
}

// `aset` on a STRING, which Phase 25 made possible and 25.0 measured against
// Emacs first (`string25-aset-*`). Emacs' unibyte rule, both halves: a value
// that is a byte is stored in place and the string keeps its length, and a
// value above 255 is refused -- with Emacs' own sentence, because the refusal
// is the contract and a caller reading the message should find the one it
// would find there. Emacs refuses for the same reason fe does: storing it
// would have to WIDEN the string, and `aset` never changes a string's width
// in either dialect. So a string's length is fixed here, and the phase's
// "mutation preserves the stable header even when the size changes" gate is
// about the reader's growth rather than about this.
//
// Emacs' other refusal -- "Attempt to replace non-ASCII char in multibyte
// string" -- has no analogue here: a fe string is a sequence of BYTES, so
// there is no multibyte character for a write to land in the middle of.
// Writing over one byte of a two-byte UTF-8 character is a recorded
// divergence rather than an error.
static void SetStringByte(FeContext* ctx,
                          FeObject* string,
                          size_t index,
                          FeObject* value) {
  if (FeGetType(value) != FeTInteger || INTEGER(value) < 0) {
    RaiseWrongType(ctx, "characterp", value);
  }
  if (INTEGER(value) > 0xff) {
    FeHandleError(ctx, "Attempt to store non-byte value into unibyte string");
  }
  StringBytes(ctx, string)[index] = (unsigned char)INTEGER(value);
}

// `(aset ARRAY INDEX VALUE)`, answering VALUE as Emacs does. An ARRAY is a
// vector or a string, exactly as it is for `aref` above, and the bounds and
// wrong-type answers are the same two conditions for both.
static FeObject* Aset(FeContext* ctx,
                      FeObject* array,
                      FeObject* index_obj,
                      FeObject* value) {
  const int64_t index = IndexArgument(ctx, index_obj);
  const FeType type = FeGetType(array);
  if (type != FeTVector && type != FeTString) {
    RaiseWrongType(ctx, "arrayp", array);
  }
  const size_t length =
      type == FeTVector ? VectorLength(ctx, array) : StringLength(array);
  if (index < 0 || (uint64_t)index >= length) {
    RaiseOutOfRange(ctx, array, index_obj);
  }
  if (type == FeTVector) {
    SetVectorElement(ctx, array, (size_t)index, value);
  } else {
    SetStringByte(ctx, array, (size_t)index, value);
  }
  return value;
}

// `(elt SEQUENCE N)`, and Emacs' measured ASYMMETRY with it: on a vector or a
// string `elt` routes to `aref` and an index past the end raises, while on a
// list it routes to `nth` and answers nil -- `(elt '(1 2) 9)` is nil and
// `(elt [1 2] 9)` is `args-out-of-range`. A negative index on a list is `nth`'s
// answer too, the first element, measured: `(nth -1 '(1 2))` is 1.
static FeObject* SequenceElement(FeContext* ctx,
                                 FeObject* sequence,
                                 FeObject* index_obj) {
  const FeType type = FeGetType(sequence);
  if (type == FeTVector || type == FeTString) {
    return Aref(ctx, sequence, index_obj);
  }
  if (type != FeTNil && type != FeTPair) {
    RaiseWrongType(ctx, "sequencep", sequence);
  }
  int64_t index = IndexArgument(ctx, index_obj);
  while (index > 0 && FeGetType(sequence) == FeTPair) {
    index--;
    sequence = CDR(sequence);
  }
  return FeCar(ctx, sequence);
}

// One `vconcat` operand's elements written into VECTOR from `at`, answering
// the next free slot. A STRING contributes its bytes as integers, which is
// Emacs' rule and the one an implementation gets wrong by contributing the
// string itself: `(vconcat "ab")` is `[97 98]`.
//
// The GC-stack checkpoint inside the loop is load-bearing rather than tidy.
// `FeMakeInteger` pushes what it allocates, so a string operand pushed one
// root per byte, and the root stack's ordinary ceiling is
// `GcStackSize - GcStackReserve` = 4032 slots: without the restore,
// `(vconcat A-4033-BYTE-STRING)` overflowed the root stack instead of
// answering. Restoring after the store is safe because the element is
// reachable from VECTOR, which is rooted above this checkpoint.
static size_t AppendSequence(FeContext* ctx,
                             FeObject* vector,
                             size_t at,
                             FeObject* sequence) {
  const FeType type = FeGetType(sequence);
  const size_t count = SequenceCount(ctx, sequence);
  const size_t gc = FeSaveGC(ctx);
  for (size_t i = 0; i < count; i++) {
    FeObject* element = nullptr;
    if (type == FeTVector) {
      element = VectorElement(ctx, sequence, i);
    } else if (type == FeTString) {
      element = FeMakeInteger(ctx, StringByteAt(ctx, sequence, i));
    } else {
      element = CAR(sequence);
      sequence = CDR(sequence);
    }
    SetVectorElement(ctx, vector, at + i, element);
    FeRestoreGC(ctx, gc);
  }
  return at + count;
}

// `(vconcat &rest SEQUENCES)`. Two passes over the operands: the first sizes
// the result, the second fills it. One pass over a growing block would mean
// re-publishing per operand, which copies what is already there each time;
// two passes over an argument list that cannot change between them do not.
static FeObject* Vconcat(FeContext* ctx, FeObject* arguments) {
  size_t total = 0;
  for (FeObject* rest = arguments; !FeIsNil(rest); rest = CDR(rest)) {
    total += SequenceCount(ctx, CAR(rest));
  }
  FeObject* const vector = MakeVector(ctx, total, &nil);
  size_t at = 0;
  for (FeObject* rest = arguments; !FeIsNil(rest); rest = CDR(rest)) {
    at = AppendSequence(ctx, vector, at, CAR(rest));
  }
  return vector;
}

// Phase 24's vector family, finished from its evaluated operand list exactly
// as `EvaluateSymbolPrimitive` finishes Phase 14's: `arguments` is a C local
// in `ResumeEvalList` by now, so it is rooted here across the allocations
// below, and the result is pushed after the restore -- the state every other
// primitive leaves the GC stack in.
FeObject* EvaluateVectorPrimitive(FeContext* ctx,
                                  Primitive primitive,
                                  FeObject* arguments) {
  const size_t gc = FeSaveGC(ctx);
  FePushGC(ctx, arguments);
  FeObject* result = &nil;
  // On an `int` for `-Wswitch-enum`'s sake, exactly as
  // `EvaluateSymbolPrimitive` above: this switch deliberately handles the
  // eight its caller filtered for.
  switch ((int)primitive) {
    case PVector:
      result =
          MakeVectorFromList(ctx, arguments, SequenceCount(ctx, arguments));
      break;
    case PMakeVector:
      result = MakeVector(ctx, LengthArgument(ctx, CAR(arguments)),
                          CAR(CDR(arguments)));
      break;
    case PVectorp:
      result = FeMakeBool(ctx, FeGetType(CAR(arguments)) == FeTVector);
      break;
    case PAref:
      result = Aref(ctx, CAR(arguments), CAR(CDR(arguments)));
      break;
    case PAset:
      result = Aset(ctx, CAR(arguments), CAR(CDR(arguments)),
                    CAR(CDR(CDR(arguments))));
      break;
    case PVconcat:
      result = Vconcat(ctx, arguments);
      break;
    case PLength:
      result = FeMakeInteger(ctx, (int64_t)SequenceCount(ctx, CAR(arguments)));
      break;
    case PElt:
      result = SequenceElement(ctx, CAR(arguments), CAR(CDR(arguments)));
      break;
    default:
      abort();
  }
  FeRestoreGC(ctx, gc);
  FePushGC(ctx, result);
  return result;
}

static const char ArenaExhaustionName[] = "arena-exhaustion";
static const char EvaluationStackExhaustionName[] =
    "evaluation-stack-exhaustion";

// The names `OpenContext` interns before Phase 19's seeding runs: every
// primitive, its aliases, and the maths natives. The seeding asks, because
// `error` is both a condition and a primitive and its symbol must be counted
// once -- `FeMinimumArenaSize` is exact, not an upper bound (the exact-fit
// arena in `test_api.c` asserts `free_slots == 0` at it), so an
// over-estimate is as wrong here as an under-estimate.
static const char* const math_names[] = {
    "sin", "cos", "tan",   "asin",    "acos",  "atan",     "expt", "sqrt",
    "exp", "log", "floor", "ceiling", "round", "truncate", "pi",   "e",
};

static bool IsCoreSymbolName(const char* name) {
  for (Primitive i = PAssert; i < PSentinel; i++) {
    if (strcmp(name, primitive_names[i]) == 0) {
      return true;
    }
  }
  for (size_t i = 0; i < COUNT(primitive_aliases); i++) {
    if (strcmp(name, primitive_aliases[i].name) == 0) {
      return true;
    }
  }
  for (size_t i = 0; i < COUNT(math_names); i++) {
    if (strcmp(name, math_names[i]) == 0) {
      return true;
    }
  }
  return strcmp(name, "t") == 0 || strcmp(name, ArenaExhaustionName) == 0 ||
         strcmp(name, EvaluationStackExhaustionName) == 0;
}

// The name of the property Phase 19's messages live under. One spelling,
// used by the seeding at context open and by the lookup in the renderer.
static const char ErrorMessageProperty[] = "error-message";

// Append TEXT, then whatever fits. `used` is always left at a NUL, so the
// caller's buffer is a valid C string after every step.
static void AppendMessageText(char* dst,
                              size_t size,
                              size_t* used,
                              const char* text) {
  for (; *text != '\0' && *used + 1 < size; text++) {
    dst[(*used)++] = *text;
  }
  dst[*used] = '\0';
}

static void AppendMessageObject(FeContext* ctx,
                                FeObject* obj,
                                char* dst,
                                size_t size,
                                size_t* used,
                                int qt) {
  *used += RenderObject(ctx, obj, dst + *used, size - *used, qt);
}

// Emacs' `error-message-string`, byte for byte with `print_error_message`
// (print.c) on 31.0.90, over the ERROR object `(SYMBOL . DATA)`:
//
//   * `error` alone takes its message from the DATA's first item, which is
//     why `(error "boom")` reports `boom` and not the word `error`;
//   * a `file-error` subtype takes its message from the DATA's first item
//     too, and prints the remaining items with `princ`, which is what makes
//     `Cannot open load file: No such file or directory, /nope/x.el` one
//     sentence of three strings rather than three quoted objects;
//   * everything else takes the `error-message` PROPERTY of its symbol and
//     prints its data items with `prin1`;
//   * a message that is not a string at all is `peculiar error`, and an
//     EMPTY one drops the `: ` that would otherwise follow it.
//
// It allocates nothing and raises nothing: `FeErrorMessageString` renders on
// the host's error path, where there may be no arena left to allocate from
// and no frame to raise into. The `*used + 1 < size` bound is also what
// terminates a circular DATA list -- a cycle costs `size` bytes of work, the
// rule the writer already lives by -- since the loop cannot make progress
// once the buffer is full.
// The message half of the rule above: which object the sentence starts
// from, with `*data` left at the items that follow it. Split out of the
// renderer so that neither half is over `PMCCABE_NEW_FUNCTION_MAX`.
static FeObject* SelectErrorMessage(FeContext* ctx,
                                    FeObject* symbol,
                                    FeObject** data,
                                    bool file_error) {
  const bool from_data = IsNamedSymbol(ctx, symbol, "error") || file_error;
  if (from_data && FeGetType(*data) == FeTPair) {
    FeObject* const message = CAR(*data);
    *data = CDR(*data);
    return message;
  }
  FeObject* const property = FeGetType(symbol) == FeTSymbol
                                 ? FindInternedSymbol(ctx, ErrorMessageProperty)
                                 : nullptr;
  // `error` itself never falls back to its property: Emacs reads its
  // message out of the data or reports `peculiar error`, and `(get 'error
  // 'error-message)` -- the string "error" -- is not what it prints.
  return property == nullptr || IsNamedSymbol(ctx, symbol, "error")
             ? &nil
             : PlistGet(SymbolPlist(symbol), property);
}

size_t RenderErrorMessage(FeContext* ctx,
                          FeObject* error,
                          char* dst,
                          size_t size) {
  if (size == 0) {
    return 0;
  }
  size_t used = 0;
  dst[0] = '\0';
  const bool structured = FeGetType(error) == FeTPair;
  FeObject* const symbol = structured ? CAR(error) : &nil;
  FeObject* data = structured ? CDR(error) : &nil;
  const bool file_error = ConditionInheritsFrom(ctx, symbol, "file-error");
  FeObject* const message = SelectErrorMessage(ctx, symbol, &data, file_error);
  const char* separator = ": ";
  if (FeGetType(message) != FeTString) {
    AppendMessageText(dst, size, &used, "peculiar error");
  } else if (CopyStringBytes(ctx, message, nullptr) != 0) {
    AppendMessageObject(ctx, message, dst, size, &used, 0);
  } else {
    separator = nullptr;
  }
  while (FeGetType(data) == FeTPair && used + 1 < size) {
    if (separator != nullptr) {
      AppendMessageText(dst, size, &used, separator);
    }
    separator = ", ";
    AppendMessageObject(ctx, CAR(data), dst, size, &used, file_error ? 0 : 1);
    data = CDR(data);
  }
  return used;
}

// `error-message-string` from C (FE_API_VERSION 10), for the one caller a
// primitive cannot serve: a host's `FeSetErrorFn` callback, which is handed
// fe's own bare message text and wants Emacs' sentence for the condition
// `FeGetCondition` is reporting.
//
// The step budget is suspended across the render, and this is the reason the
// two entry points are not one line apart: printing is work, so `RenderObject`
// charges the budget and polls the host's interrupt, and BOTH of those raise.
// A raise from inside the error callback would abandon the error being
// reported for one about the reporting. Inside the primitive the same charge
// is correct -- that is ordinary evaluation -- so the suspension lives here
// rather than in `RenderErrorMessage`.
size_t FeErrorMessageString(FeContext* ctx,
                            FeObject* error,
                            char* dst,
                            size_t size) {
  const bool active = ctx->evaluation_active;
  ctx->evaluation_active = false;
  const size_t used = RenderErrorMessage(ctx, error, dst, size);
  ctx->evaluation_active = active;
  return used;
}

// The `error-message` property of every condition in the hierarchy, seeded
// once while the arena is still empty. Seeded rather than looked up in the
// C table on demand, because Emacs' properties are real -- `(get
// 'wrong-type-argument 'error-message)` answers there -- and because it
// gives a program one way to override a message (`put`) instead of none.
// The cost is counted in `GetCoreObjectCount` below, so a context opened at
// `FeMinimumArenaSize` can still do this.
//
// `FeMakeSymbol` interns on `ctx->symbol_list`, itself a mark root, so each
// symbol is rooted before the string beside it is allocated; the GC stack
// carries the string and the first pair across the second allocation.
void SeedConditionMessages(FeContext* ctx) {
  FeObject* const property = FeMakeSymbol(ctx, ErrorMessageProperty);
  for (size_t i = 0; ConditionRowAt(i) != nullptr; i++) {
    const ConditionParent* const row = ConditionRowAt(i);
    const size_t gc = FeSaveGC(ctx);
    FeObject* const symbol = FeMakeSymbol(ctx, row->name);
    FeObject* const text = FeMakeString(ctx, row->message);
    SetSymbolPlist(symbol, FeCons(ctx, property, FeCons(ctx, text, &nil)));
    FeRestoreGC(ctx, gc);
  }
}

// What one object of each kind costs in CELLS, which since Phase 25 does not
// depend on how long the name is: a string is one object whatever its length,
// and a symbol is six -- the symbol itself, the three pairs of its
// `((name . plist) . function) . value` chain, the `symbol_list` cell that
// interns it, and the one string object its name is. The length went into the
// region instead, which is what `GetStringPayloadBytes` below counts.
enum { SymbolObjectCount = 6, StringObjectCount = 1 };

// ...and in REGION BYTES: one block per string, its header plus its bytes
// rounded the way `AllocatePayloadBlock` rounds them. Exactly, not
// approximately: `FeMinimumArenaSize` is the size at which the exact-fit
// context in `test_api.c` finds no free cell and no free region byte.
static size_t GetStringPayloadBytes(const char* text) {
  return sizeof(FePayloadBlock) + RoundUpToAlignment(strlen(text));
}

// What `SeedConditionMessages` allocates: the shared property symbol, and
// per row a message string, the two pairs of the plist, and the condition
// symbol unless one of the tables above already interned it.
static size_t GetConditionMessageObjectCount(void) {
  size_t count = SymbolObjectCount;
  for (size_t i = 0; ConditionRowAt(i) != nullptr; i++) {
    const ConditionParent* const row = ConditionRowAt(i);
    count += StringObjectCount + 2;
    if (!IsCoreSymbolName(row->name)) {
      count += SymbolObjectCount;
    }
  }
  return count;
}

// The same seeding, in region bytes: one block per name and one per message.
static size_t GetConditionMessagePayloadBytes(void) {
  size_t bytes = GetStringPayloadBytes(ErrorMessageProperty);
  for (size_t i = 0; ConditionRowAt(i) != nullptr; i++) {
    const ConditionParent* const row = ConditionRowAt(i);
    bytes += GetStringPayloadBytes(row->message);
    if (!IsCoreSymbolName(row->name)) {
      bytes += GetStringPayloadBytes(row->name);
    }
  }
  return bytes;
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

static size_t GetCoreObjectCount(void) {
  size_t count = SymbolObjectCount;
  for (Primitive i = PAssert; i < PSentinel; i++) {
    count += 1 + SymbolObjectCount;
  }
  for (size_t i = 0; i < COUNT(primitive_aliases); i++) {
    count += SymbolObjectCount;
  }
  for (size_t i = 0; i < COUNT(math_names); i++) {
    count += 1 + SymbolObjectCount;
  }
  // The two pre-built exhaustion conditions: each is an interned symbol plus
  // the one pair that makes it a condition object (09B). Counting them here
  // is what keeps `FeMinimumArenaSize()` honest -- a context opened at the
  // minimum must still be able to build them.
  count += 1 + SymbolObjectCount;
  count += 1 + SymbolObjectCount;
  // Phase 19's seeded `error-message` properties, for the same reason.
  count += GetConditionMessageObjectCount();
  return count;
}

// The region bytes those same objects need: every name and every seeded
// message is a string, and a string is a block. This is why an fe context
// cannot open without a region and why the minimum arena funds one -- an
// interpreter that cannot hold the name `car` cannot hold anything.
static size_t GetCorePayloadBytes(void) {
  size_t bytes = GetStringPayloadBytes("t");
  for (Primitive i = PAssert; i < PSentinel; i++) {
    bytes += GetStringPayloadBytes(primitive_names[i]);
  }
  for (size_t i = 0; i < COUNT(primitive_aliases); i++) {
    bytes += GetStringPayloadBytes(primitive_aliases[i].name);
  }
  for (size_t i = 0; i < COUNT(math_names); i++) {
    bytes += GetStringPayloadBytes(math_names[i]);
  }
  bytes += GetStringPayloadBytes(ArenaExhaustionName);
  bytes += GetStringPayloadBytes(EvaluationStackExhaustionName);
  bytes += GetConditionMessagePayloadBytes();
  return bytes;
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
      ckd_add(&minimum, minimum, objects) ||
      ckd_add(&minimum, minimum, GetCorePayloadBytes());
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
      // The region, whose bookkeeping is already tracked at the sites that
      // change it, exactly as the cell gauges above are. `payload_used` is
      // the live extent's length rather than its end address, so it is what
      // a host reads as "bytes in use" whether or not the poison knob has
      // slid the extent forward.
      .payload_capacity_bytes = ctx->payload_capacity,
      .payload_live_bytes = ctx->payload_used,
      .payload_peak_bytes = ctx->payload_peak_used,
      .payload_compaction_count = ctx->payload_compaction_count,
      .payload_allocation_failures = ctx->payload_allocation_failures,
  };
}

// The payload region's DISCRETIONARY share of what is left once the frame
// region is funded, rounded DOWN to a whole alignment unit so that the cells
// laid out after it keep their own alignment. It is a share of the surplus
// only: the region's floor -- the blocks the core names occupy -- is funded
// out of `FeMinimumArenaSize` beside the core cells, so this number is what a
// host's own strings get to use.
static size_t PayloadCarveBytes(size_t bytes, size_t percent) {
  const size_t share = (bytes / 100) * percent + (bytes % 100) * percent / 100;
  return share - share % FePayloadAlignment;
}

static bool InitializeArenaLayout(FeContext* ctx,
                                  void* arena,
                                  size_t size,
                                  size_t payload_percent) {
  const size_t minimum = GetMinimumArenaSize();
  const size_t remainder = size - minimum;
  const size_t frame_bonus_bytes = (remainder / 100) * FrameArenaPercent +
                                   (remainder % 100) * FrameArenaPercent / 100;
  // What frames did not take, which the payload region and the cells then
  // split. The split is priced against the frame region rather than out of
  // it, which is the Phase 22 ADR's "no unpriced split" rule: frames are the
  // pool Phase 21 measured as kg's real scarcity.
  const size_t cell_bytes = remainder - frame_bonus_bytes;
  const size_t payload_share = PayloadCarveBytes(cell_bytes, payload_percent);
  // Both floors plus both shares: the core names' blocks and the core cells
  // come out of the minimum, and what is left over is split. Every term is a
  // multiple of the shared alignment, so no region seam needs padding.
  const size_t payload_bytes = GetCorePayloadBytes() + payload_share;
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
              (cell_bytes - payload_share) / sizeof(FeObject));
  if (layout_overflow) {
    return false;
  }

  // Initialize the context and its three arena-resident regions. Frames, then
  // payload, then objects; every region's size is a multiple of the shared
  // 8-byte alignment, so no implicit padding is needed at either seam.
  memset(ctx, 0, sizeof(FeContext));
  void* const frame_region = (unsigned char*)arena + sizeof(FeArena);
  unsigned char* const payload_region =
      (unsigned char*)frame_region + frame_bytes;
  void* const object_region = payload_region + payload_bytes;
  ctx->frame_stack = frame_region;
  ctx->frame_stack_capacity = frame_capacity;
  ctx->payload_base = payload_region;
  ctx->payload_capacity = payload_bytes;
  ctx->objects = object_region;
  ctx->object_count = object_count;
  return true;
}

FeContext* OpenContextWithPayload(void* arena,
                                  size_t size,
                                  size_t payload_percent) {
  uintptr_t arena_end;
  const uintptr_t arena_address = (uintptr_t)arena;
  bool invalid = arena == nullptr;
  invalid |= size < FeMinimumArenaSize();
  invalid |= arena_address % FeArenaAlignment() != 0;
  invalid |= payload_percent > 100;
  invalid |= ckd_add(&arena_end, arena_address, size);
  if (invalid) {
    return nullptr;
  }

  FeArena* storage = arena;
  FeContext* ctx = &storage->context;
  if (!InitializeArenaLayout(ctx, arena, size, payload_percent)) {
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

  // Phase 19: the condition hierarchy's `error-message` properties. Here,
  // while the arena is still empty, for the same reason the two conditions
  // above are: what a raise renders with cannot depend on there being room
  // to build it at the time of the raise.
  SeedConditionMessages(ctx);

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

// The percentage `options` asks for, or false when it asks for one no
// partition can be made from. Refusal rather than a clamp, per the contract
// in fe.h: a host that asks for 150% has a bug, and a context quietly
// partitioned to 100 would hide it behind a split the host never chose.
static bool SelectedPayloadPercent(const FeOpenOptions* options,
                                   size_t* percent) {
  const int asked = options == nullptr ? 0 : options->payload_percent;
  if (asked < 0 || asked > 100) {
    return false;
  }
  // Zero is "Fe decides", which is what makes a zero-initialized record mean
  // the default.
  *percent = asked == 0 ? (size_t)FeDefaultPayloadPercent : (size_t)asked;
  return true;
}

FeContext* FeOpenContextWithOptions(void* arena,
                                    size_t size,
                                    const FeOpenOptions* options) {
  size_t percent = 0;
  if (!SelectedPayloadPercent(options, &percent)) {
    return nullptr;
  }
  return OpenContextWithPayload(arena, size, percent);
}

// Fe's own split, which is the only thing this entry point can mean now.
// Until Phase 25 it carved nothing, because nothing a Lisp program could
// build lived in the region and a host that had not asked for one should not
// lose a quarter of its cells to it. Strings ended that: a symbol's name is a
// string, so a context with no region cannot even finish opening, and "no
// carve" stopped being a partition a host can be given.
FeContext* FeOpenContext(void* arena, size_t size) {
  return FeOpenContextWithOptions(arena, size, nullptr);
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
