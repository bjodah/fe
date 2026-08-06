// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FE_INTERNAL_H
#define FE_INTERNAL_H

// The private shared surface between fe.c (object model, GC, reader,
// writer) and fe_eval.c (the evaluator). Only those two translation units
// may include this header; everything here is implementation, and nothing
// of it leaks into the public fe.h. It is one of the headers
// `make test-header` compiles on its own (test_internal_header.c), so it
// includes fe.h and the standard headers it needs and nothing more.

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "fe.h"

typedef enum Primitive {
  PAssert,
  PEnv,
  PLet,
  // Numeric equality, chained left to right over Fe's existing doubles.
  // `=` was Fe's historical, non-Emacs assignment primitive until sub-plan
  // 02C of the Emacs-subset hard cut repurposed it as numeric equality and
  // moved assignment to `setq`/`set` below; see doc/language.md.
  PNumericEqual,
  PSetq,
  PSet,
  PIf,
  PFn,
  PMacro,
  PWhile,
  PQuote,
  PBoundp,
  PMakeUnbound,
  PAnd,
  POr,
  PDo,
  PUnwindProtect,
  PCons,
  PCar,
  PCdr,
  PSetCar,
  PSetCdr,
  PList,
  PNot,
  PIs,
  // Sub-plan 05D of kg's Emacs-subset program (the numeric cut): `eq` and
  // `eql`, Emacs' identity operators, land with the cut because they are the
  // names kg's prelude still aliases to `is` until 05E deletes the alias.
  // `eq` is pointer identity or both-integers-equal; `eql` adds same-type
  // float equality by bits.
  PEq,
  PEql,
  PAtom,
  PPrint,
  PLess,
  PLessEqual,
  // Sub-plan 05C of kg's Emacs-subset program (the numeric tower): `>`/`>=`
  // join `<`/`<=` as chained comparators, `/=` is the exact binary
  // inequality, and `integerp`/`floatp` are the numeric predicates. The old
  // "Add > and >=" TODO above was resolved here.
  PGreater,
  PGreaterEqual,
  PNotEqual,
  PIntegerp,
  PFloatp,
  PKeywordp,
  PAdd,
  PSub,
  PMul,
  PDiv,
  // Sub-plan 04C of kg's Emacs-subset program (Lisp-2 namespaces, additive):
  // the function namespace's primitives. `function` is a special form
  // (its argument stays raw); `funcall`/`apply` are function-shaped special
  // forms -- they evaluate their operands like an ordinary call's argument
  // list and then dispatch, but run on the frame stack rather than re-entering
  // evaluation (see fe_eval.c). The rest are ordinary functions. The
  // function-cell accessors (`SymbolFunction`/`SetSymbolFunction`) are 04B's;
  // this slice is the first to read the cell.
  PFunction,
  PFset,
  PDefalias,
  PSymbolFunction,
  PSymbolValue,
  PFboundp,
  PFmakunbound,
  PFuncall,
  PApply,
  // Sub-plan 06C of kg's Emacs-subset program (catch and throw): the first
  // non-local exit that stops partway down the frame stack. `catch` is a
  // special form (its body region is evaluated as an implicit body); `throw`
  // is a function-shaped primitive whose two operands evaluate normally
  // before it unwinds to the innermost matching catch frame (see
  // `FeFrameCatch` and `PerformThrow`, fe_eval.c).
  PCatch,
  PThrow,
  PConditionCase,
  PSignal,
  PError,
  PSentinel
} Primitive;

typedef union {
  FeObject* o;
  FeNativeFn* f;
  FeDouble n;
  // Sub-plan 05B of kg's Emacs-subset program: the integer payload. 05A's
  // spike confirmed the union stays pointer-sized with it; the assert below
  // makes that permanent.
  int64_t i;
  // TODO: Might need/want to make this `uintptr_t` someday.
  char c;
} Value;

// The representation Decision the integer object stands on (05A's spike,
// confirmed on both CI compilers): every object is exactly one pointer.
static_assert(sizeof(Value) == sizeof(FeObject*));

enum {
  // Stored in the lowest-order bit of `Value.c`:
  ConsCell = 0,
  OtherCell = 1,
  // The 2nd-lowest-order bit of `Value.c` is the mark bit:
  GcMarkBit = 2,
  // TODO: This should scale with arena size?
  // A self-recursive Fe call no longer costs any GC-stack slots at all
  // (every live pair form roots its own operands as frame fields --
  // `FeMarkEvaluatorRoots`, fe_eval.c); nothing pushes here per level of
  // ordinary Lisp nesting any more, so this is no longer a recursion bound
  // of any kind, designed or accidental. It still bounds the reader's own
  // native recursion (`Read`/`ReadList`) and the writer's, neither of which
  // go through the frame machine.
  GcStackSize = 4096,
  // The tail of `gc_stack` an ordinary push may not reach, the direct
  // analogue of `AllocateFrame`'s `CleanupFrameReserve`: reporting an
  // overflow is itself a rooting operation (`RaiseCompletionCore` protects
  // the condition object across the cleanup drain, and a cleanup entry
  // evaluates Lisp of its own), so a raise that starts from a completely
  // full stack re-enters `FePushGC` and recurses. Only an in-flight
  // completion (`ctx->completion != FeCompletionNormal`) may use these
  // slots; see `FePushGC`.
  GcStackReserve = 64,
  StringBufferSize = (sizeof(FeObject*) - 1),
  DefaultEvalPollInterval = 1024,
  // Cleanup entries are pushed only by `unwind-protect` and
  // `FeProtectWithCleanup`, not by every object creation the way the GC
  // stack is, so nesting this deep is not a realistic program; it is sized
  // generously rather than to match `GcStackSize` slot for slot.
  CleanupStackSize = 256,
  // The per-entry step budget a cleanup gets when `FeEvalOptions`'s
  // `cleanup_step_limit` is 0 (the common case: most callers never think
  // about this at all). Generous enough that the `test_api.c` regression
  // for "a body that exhausted a tiny budget still gets a working cleanup"
  // needs no per-test override -- a cleanup doing realistic restore-state
  // work is at most a few hundred evaluator steps -- and small enough that
  // a runaway one is a bounded pause, not a hang: at Fe's own evaluator
  // rate this is well under a second even in a debug build. A host that
  // wants a different bound, tighter or looser, sets `cleanup_step_limit`
  // explicitly instead of tuning this constant.
  DefaultCleanupStepLimit = 4096,
  // The default `native_reentry_limit` ceiling (see `struct FeContext`) when
  // `FeEvalOptions.max_native_reentry` is left 0. Unlike `max_frames`, this
  // bound is a real C-stack bound: each level is a live C activation chain
  // (the native's own frame, `FeCall`, `Evaluate`, `RunEvaluation`, its
  // `jmp_buf`, `RunEvaluationLoop`) that the frame machine cannot move off
  // the C stack, so it has to stay small.
  //
  // Derived from kg's own corpus, sub-plan 03F of kg's Emacs-subset program:
  // kg's only synchronously re-entering natives are
  // `internal--save-excursion` and `internal--with-current-buffer` (each one
  // `FeCall`s a body thunk it was handed), plus hook dispatch and process
  // filter/sentinel callbacks (each one `FeCallWithOptions`s a resolved
  // handler once). The deepest nesting actually found by grepping kg's Lisp
  // prelude and PTY corpus is 2 -- `with-current-buffer` wrapping
  // `save-excursion` (e.g. `test/pty/lisp-auto-fill-mode-undo.yaml`) -- and a
  // hook or process callback invoking that pattern adds one more live level
  // on top, for 3 in the deepest real construct found. Static grep is only a
  // lower bound on what a host program might legitimately nest, so the
  // default is not that number: it is a **10x** margin over it, chosen small
  // enough to stay a "small number" (the parent plan's own words, contrasted
  // with the legacy 1000) rather than a second `DefaultEvaluationDepth`.
  //
  // Measured (`test_api.c`'s `TestNativeReentry`, and a throwaway host-side
  // probe run for this derivation, not checked in) against a native that
  // mimics `internal--with-current-buffer`'s shape -- call `FeCall` on a
  // thunk, synchronously, once per level -- built with the same flags
  // `.ci/ci-05` (MSan) uses: roughly 1000-1100 bytes of C stack per level of
  // `native_reentry_depth`. `DefaultNativeReentry` levels therefore costs on
  // the order of 30-35 KiB against a default 8 MiB C stack (`ulimit -s`) --
  // under 0.5% of it -- even before accounting for the fact that a
  // synthetic minimal native almost certainly *understates* what a real one
  // (kg's, with its own locals, mallocs and wrapper C frames around the fe
  // call) costs per level. There is no measured crash boundary this default
  // is being tuned against, unlike the deleted `DefaultEvaluationDepth`: the
  // margin above is wide enough on both sides (10x the deepest real corpus
  // use, well under 1% of a default C stack) that no binary search was
  // needed to place it safely between them.
  DefaultNativeReentry = 32,
};

struct FeObject {
  Value car, cdr;
};

// One pending `unwind-protect`/`FeProtectWithCleanup` registration. Lisp and
// C cleanups interleave in one registry so they share a single ordering, per
// `doc/unwind-design.md`: unwinding always drains the most recently pushed
// entry first, whichever kind it is.
typedef enum FeCleanupKind {
  FeCleanupNative,
  FeCleanupLisp,
} FeCleanupKind;

typedef struct FeCleanupEntry {
  FeCleanupKind kind;
  union {
    struct {
      FeCleanupFn* fn;
      void* data;
    } native;
    struct {
      FeObject* forms;  // The unwind forms, evaluated as an implicit `do`.
      FeObject* env;    // The environment `unwind-protect` was entered with.
    } lisp;
  } as;
} FeCleanupEntry;

// The evaluator owns this stack, but the storage is carved out of the host
// arena by fe.c.  Keep the frame deliberately plain: every pointer is an
// explicit GC-rooting decision in FeMarkEvaluatorRoots().
typedef enum FeFrameKind {
  FeFrameExpression,
  // A pair form whose head is not a symbol: the frame has switched from
  // `FeFrameExpression`, pushed the head expression on the frame stack, and
  // is waiting for that expression's value (delivered into `callee`).
  FeFrameCallHead,
  // An ordinary callable's (native function or lambda) argument list: the
  // frame switches from `FeFrameCallHead`/`FeFrameExpression`, stores the
  // callable in `fn` and the remaining raw arguments in `rest`, and pushes
  // each argument as a sub-expression frame. Each argument's value is
  // delivered into `callee` (prepended onto `accumulator`, so the list is
  // built reversed and reordered when complete); `callee` is `&unbound`
  // between deliveries, marking a frame no argument has completed in yet.
  // When `rest` is empty the callable is dispatched on the evaluated list.
  FeFrameCallArguments,
  // A lambda application: the frame has switched from `FeFrameCallArguments`
  // once the argument list is complete and the callable is a lambda.
  // `ArgsToEnv` (allocating, never evaluating, so it stays an ordinary
  // helper rather than a frame of its own) has already produced the callee
  // environment into `env`, and `rest` holds the raw body forms. The resume
  // case switches the frame to `FeFrameBody` and starts the first body form.
  FeFrameLambda,
  // A sequential body (a lambda's body forms): `env` is the callee
  // environment, `rest` the remaining raw forms, `callee` the just-delivered
  // form's value (`&unbound` marks a freshly set up frame), and `accumulator`
  // the last completed value, which becomes the result when `rest` is empty.
  // Each form is pushed as a sub-expression frame whose `bind` points at this
  // frame's `env`, so a `let` in the body extends the environment the
  // following forms see, exactly as `DoList`'s `&env` out-parameter did.
  FeFrameBody,
  // A macro call: the frame has switched from `FeFrameCallHead`/
  // `FeFrameExpression` when the resolved head is a `FeTMacro`. `ArgsToEnv`
  // (allocating, never evaluating, like the lambda frame's) has already
  // bound the raw, unevaluated arguments into the macro's closure
  // environment (into `env`), `rest` holds the raw body forms, and `fn`
  // holds the caller environment -- the environment the expansion must be
  // evaluated in, which the body otherwise overwrites. The resume case
  // evaluates the body forms one at a time as sub-expression frames whose
  // `bind` points at this frame's `env`, exactly as `DoList`'s `&env` out-
  // parameter did; once `rest` is empty the last completed value is the
  // expansion, the call's trace/GC/cleanup checkpoints are restored, and the
  // frame switches to `FeFrameMacroExpansion`.
  FeFrameMacro,
  // A macro call whose body has produced its expansion: the frame switched
  // from `FeFrameMacro`, restored the macro call's trace, GC and cleanup
  // checkpoints so the expansion is evaluated as if written at the call
  // site, and pushed the raw expansion as a sub-expression frame in the
  // caller environment (`fn`). The value that frame delivers into `callee`
  // is the macro call's result.
  FeFrameMacroExpansion,
  // A native function whose evaluated argument list is ready: the frame has
  // switched from `FeFrameCallArguments` once the callable resolved to a
  // `FeTNativeFn` and the reordered argument list was moved into
  // `accumulator`. The loop's resume case invokes the native synchronously
  // -- no per-native setjmp, the enclosing `RunEvaluation` barrier is the
  // only one in effect -- while bounding the live native C activations with
  // `native_reentry_depth`, so a native that calls back into
  // `FeCall`/`FeCallWithOptions` starts a nested run on a fresh C frame and
  // counts as another active level until it returns.
  FeFrameNative,
  // A sequential body pushed directly, with no preceding pair-form dispatch
  // of its own: `if`'s false branch, `while`'s body each iteration, `do`,
  // and a cleanup's unwind forms (`RunEvaluationBody`). Reuses `FeFrameBody`'s
  // own resume logic (`env`/`rest`/`accumulator`/`callee` mean exactly the
  // same thing) but completes through a lighter path that does not touch
  // `call_list`: unlike a lambda body, whose wrapping frame *is* the call
  // form's own frame (one trace-cell link at entry, one matching unlink at
  // completion), this frame is an extra child with no entry of its own to
  // balance, and its `trace_cell` is never linked into `call_list` at all.
  // Folding it into `FeFrameBody` and telling the two apart some other way
  // was tried and rejected: the two need different completions, not just
  // different data, and a frame-kind switch already exists to carry that.
  FeFrameImplicitBody,
  // `if`: after the condition (`accumulator` holds it, `&unbound` marking
  // "not yet known"), evaluates the chosen branch -- the single then-form for
  // a truthy condition (`bind=NULL`, exactly as the recursive arm's
  // `EVAL_ARG()` did) or the remaining forms as an implicit body
  // (`FeFrameBody`, pushed directly) for a falsy one. The second delivery,
  // from either branch, is the form's result.
  FeFrameIf,
  // `while`: `fn` holds the fixed, never-consumed condition form and `rest`
  // the fixed body forms; `accumulator` distinguishes "awaiting the
  // condition" (`&unbound`) from "awaiting the body" (anything else). Each
  // pass pushes the condition as an ordinary sub-expression, then -- if
  // truthy -- an implicit-body sub-frame (`FeFrameBody`) restored to this
  // frame's own `gc_checkpoint` between passes, exactly as the recursive
  // arm's per-call `FeRestoreGC(ctx, n)` was.
  FeFrameWhile,
  // `and`/`or`: `fn` holds the resolved primitive object (to tell the two
  // apart) and `rest` the remaining raw forms. Each delivered value decides
  // whether to stop (short-circuit) or continue; the last delivered value is
  // always the result, matching the recursive loop's shared `res` variable.
  FeFrameAndOr,
  // `let`: the frame's own `bind` is the `newenv` target the recursive arm
  // received as an out-parameter (nullptr when `let` is not extending an
  // enclosing body/base frame's environment, in which case the value form is
  // never even evaluated -- see the primitive dispatch, which never creates
  // this frame kind for a nullptr `bind`). `accumulator` holds the raw,
  // already-checked target symbol; the delivered value form's value extends
  // `*bind`.
  FeFrameLet,
  // `setq`: `rest` holds the remaining raw SYMBOL VALUE pairs, evaluated and
  // assigned left to right; `accumulator` holds the pending pair's raw target
  // symbol between validating it and the value form's delivery. Each
  // assignment goes through `GetBound`, exactly as the recursive arm's did.
  FeFrameSetq,
  // A single delivered sub-expression's value is this frame's own result,
  // with no other bookkeeping: `do`'s implicit body (`FeFrameBody`, pushed
  // directly) and `unwind-protect`'s body (an ordinary sub-expression,
  // pushed after the cleanup entry is registered). The two are unrelated in
  // Lisp but share this one continuation shape.
  FeFrameRelay,
  // The primitives that evaluate exactly one operand and finish from it:
  // `assert`, `not`, `atom`, `car`, `cdr`, `boundp`, `makunbound`, and --
  // since 05C -- the numeric predicates `integerp`/`floatp`. `fn` holds
  // the resolved primitive object and `rest` the remaining raw forms (still
  // needed by `boundp`/`makunbound`/`integerp`/`floatp`'s extra-argument
  // check after the operand is consumed).
  FeFrameUnary,
  // The primitives that evaluate exactly two operands in sequence, with a
  // per-primitive check or side effect at each delivery: `cons`, `setcar`,
  // `setcdr`, `is`. `fn` holds the resolved primitive object,
  // `rest` the remaining raw forms, and `accumulator` the checked first
  // operand (`&unbound` marks "not yet delivered") -- `setcar`/`setcdr`
  // validate it as a pair immediately on delivery, before the second operand
  // is even evaluated, exactly as the recursive arm's ordering required.
  // The comparisons `<`/`<=` shared this kind until 05C made them chained
  // and variadic; they now live in `FeFrameEvalList` beside `=`.
  FeFrameBinary,
  // `+`, `-`, `*`, `/`: streams every operand, validating and combining each
  // as it arrives -- never batching the whole list first, since the
  // recursive arm's `ARITH_OP` validated the same way. `fn` holds the
  // resolved primitive object, `rest` the remaining raw forms, and
  // `accumulator` the running boxed total, which since 05C is either a
  // `FeTInteger` or a `FeTDouble` (`&unbound` marks "no operand combined
  // yet").
  FeFrameArith,
  // `print`: streams every operand, writing each as it arrives and printing
  // a separating space only when another operand remains, exactly
  // interleaved with evaluation as the recursive arm's loop was. `rest`
  // holds the remaining raw forms.
  FeFramePrint,
  // `list`, `=`, the chained comparators `<`/`<=`/`>`/`>=`, the binary `/=`,
  // `set`: evaluates the complete raw argument list first --
  // unlike every other kind above, whose ordering is what makes them not
  // this -- then finishes per primitive: `list` returns it, `=`/`<`/`<=`/
  // `>`/`>=` compare adjacent pairs left to right and stop at the first
  // false one (`/=` is the same shape over its arity-checked two), and
  // `set` checks
  // the (already arity-validated) two-element list and assigns through
  // `FeSet`. `fn` holds the resolved primitive object, `rest` the remaining
  // raw forms, and `accumulator` the list built so far (reversed into order
  // once `rest` is exhausted, exactly as `FeFrameCallArguments` reorders its
  // own).
  FeFrameEvalList,
  // `(catch TAG BODY...)` (sub-plan 06C): the frame switched from
  // `FeFrameExpression` when the resolved head is the `catch` primitive.
  // `rest` holds the raw TAG and BODY forms, `accumulator` the evaluated tag
  // (`&unbound` marks "awaiting the tag"), and `callee` the delivered body
  // value -- or, after a `throw` unwinds here, the delivered thrown value
  // (`PerformThrow` sets `callee` and marks the tag phase complete before the
  // loop resumes this frame). This is the first frame kind that resumes from
  // a mid-stack unwind rather than from a normal sub-expression delivery:
  // the frame's `gc_checkpoint`/`cleanup_checkpoint` are the throw's
  // destination (`RunCleanupsDownTo(ctx, cleanup_checkpoint)` and
  // `FeRestoreGC(ctx, gc_checkpoint)`), and its `trace_cell` link is where
  // the discarded frames' `call_list` chain is restored to.
  FeFrameCatch,
  FeFrameConditionCase,
} FeFrameKind;

typedef struct FeEvalFrame {
  FeFrameKind kind;
  FeObject* expr;
  FeObject* env;
  // The `newenv`/`bind` target this frame's own `FeFrameLet` continuation
  // (if this frame's raw form turns out to be a `let`) writes into: NULL for
  // a computed head, an argument sub-frame, or a frame with no surrounding
  // sequence -- in which case the primitive dispatch never even evaluates
  // the value form -- and `&env` of the enclosing `FeFrameBody` for a body
  // form, so a `let` there extends the environment the following body forms
  // see. It points at a frame's `env` field or at a caller's C local, never
  // at an arena object, so it is not a GC root and the collector must not
  // touch it.
  FeObject** bind;
  // The callable an `FeFrameCallArguments` frame is dispatching to, and --
  // after `ArgsToEnv` has captured the closure environment into `env` -- the
  // caller environment a `FeFrameMacro` frame evaluates its expansion in.
  // Reused by several primitive-continuation kinds (see `FeFrameWhile`,
  // `FeFrameAndOr`, `FeFrameUnary`, `FeFrameBinary`, `FeFrameArith`,
  // `FeFrameEvalList` above) to hold the fixed form or resolved primitive
  // object their own resumption needs across pushes. Either value must
  // outlive the argument evaluations or the macro body (a
  // collection may run between them), and the collector marks it with the
  // other fields.
  FeObject* fn;
  FeObject* rest;
  FeObject* accumulator;
  FeObject* callee;
  size_t gc_checkpoint;
  size_t cleanup_checkpoint;
  FeObject trace_cell;
} FeEvalFrame;

enum {
  MinFrameCapacity = 64,
  CleanupFrameReserve = 32,
  // The share of the bytes beyond the minimum that become frame storage.
  // Sub-plan 03F (kg's Emacs-subset program) deleted the transitional
  // logical `evaluation_depth` counter that `DefaultEvaluationDepth` (1000)
  // used to bound: the Lisp-nesting bound is now `frame_stack_capacity`
  // (this partition's own output, exposed as `FeArenaStats.frame_capacity`)
  // and *nothing else* -- there is one number, not two independent ones that
  // happened to be close. Before this slice, "the logical limit fires
  // before the physical frame wall" held only by a ~10% margin (1100 frames
  // against the 1000 default) and broke once already (03D's frame-size
  // retune, 8% -> 10%, recorded in kg's 03D Decision follow-up); that
  // coincidence cannot recur now that the physical wall is the only wall.
  // The 10% split itself is unchanged by this slice -- it was already
  // retuned once for kg's 1 MiB arena's frame/object trade-off, and 03F's
  // own scope is the bound API, not the partition.
  FrameArenaPercent = 10,
};

static_assert(sizeof(FeEvalFrame) == 96);
static_assert(alignof(FeEvalFrame) == alignof(FeObject));

// The value of a symbol that has never been assigned. Like `nil` it is a
// static object outside the arena, so the collector neither sweeps it nor has
// to mark it, and `FeMark` treats it as a leaf. It is never returned to Lisp or
// to a host: the only place it lives is a symbol's value cell, which Lisp
// cannot reach (`(cdr sym)` is a type error) and which every reader of a value
// cell turns into `void-variable`. It is tagged `FeTFree` so that an escape
// aborts in the writer instead of impersonating a value. Defined in fe.c;
// both fe.c (installs it in a fresh symbol's value cell) and fe_eval.c (the
// evaluator compares value cells against it) need it, so it is not `static`.
extern FeObject unbound;

#define CAR(x) ((x)->car.o)
#define CDR(x) ((x)->cdr.o)
#define TAG(x) ((x)->car.c)
#define DOUBLE(x) ((x)->cdr.n)
#define INTEGER(x) ((x)->cdr.i)
#define PRIM(x) ((x)->cdr.c)
#define NATIVE_FN(x) ((x)->cdr.f)
#define STRING_BUFFER(x) (&(x)->car.c + 1)

// Small object-layer accessors both translation units use. Defined in fe.c,
// declared here instead of kept `static` because fe_eval.c calls them too.
FeDouble GetDouble(const FeObject* o);
FeNativeFn* GetNativeFn(const FeObject* o);
void SetType(FeObject* o, FeType type);
FeObject* CheckType(FeContext* ctx, FeObject* obj, FeType type);
FeObject* GetBound(FeContext* ctx, FeObject* sym, FeObject* env);
// Symbol accessors (sub-plan 04B of kg's Emacs-subset program): a symbol's
// `cdr` is one cons holding `((name . function) . value)`, and every reader
// of that private layout goes through these instead of spelling the pair walk
// itself. `SymbolBindingCell` is the cell `GetBound`'s global path returns;
// the value path's unit of currency is the cell, not a value-shaped accessor
// -- lexical environment entries and the global cell share the `CDR(cell)`
// read/write contract `GetBound` depends on, so wrapping that in an accessor
// would hide exactly the symmetry. `SymbolFunction`/`SetSymbolFunction` reach
// the independent function cell used by call-position resolution.
FeObject* SymbolName(const FeObject* sym);  // the name string chain
FeObject* SymbolBindingCell(
    FeObject* sym);  // the cell GetBound's global path returns
FeObject* SymbolFunction(FeObject* sym);  // &unbound when no function binding
void SetSymbolFunction(FeObject* sym, FeObject* fn);
FeObject* MakeObject(FeContext* ctx);
bool Equal(FeObject* a, FeObject* b);
// `eq`/`eql`'s shared answer (05D): pointer identity, both-integers-equal, or
// -- for `eql` (`compare_floats`) -- same-type floats equal by bits. Defined
// in fe.c beside `Equal`; the evaluator's `eq`/`eql` primitives call it.
bool IdentityObjects(FeObject* a, FeObject* b, bool compare_floats);
bool IsNamedSymbol(const FeObject* v, const char* name);
bool IsKeywordSymbol(const FeObject* v);
bool IsConstantSymbol(const FeObject* v);
void __attribute((format(printf, 3, 4))) Format(char* result,
                                                size_t size,
                                                const char* format,
                                                ...);

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
  FeEvalFrame* frame_stack;
  size_t frame_stack_capacity;
  size_t frame_stack_index;
  // The frame-stack floor of the evaluator run currently being driven by
  // `RunEvaluationLoop`: the index `RunEvaluation`/`RunEvaluationBody` saved
  // as `base` when their own run started. Sub-plan 06C's throw search stops
  // here -- a catch frame below a nested run's floor belongs to an outer run
  // (or to an abandoned body), and the C activations between them are live,
  // so a throw must not match one (the native re-entry wall, recorded as a
  // divergence). Saved and restored by `RunEvaluationLoop` around the loop,
  // so a nested run's own base is visible while it runs and the outer run's
  // base is back in force the moment the nested run returns.
  size_t run_base;
  FeInterruptFn* evaluation_interrupt;
  void* evaluation_userdata;
  size_t evaluation_steps;
  size_t evaluation_poll_interval;
  size_t evaluation_poll_countdown;
  // The ambient call's configured `FeEvalOptions.cleanup_step_limit` (0
  // until set, meaning "use `DefaultCleanupStepLimit`"). Captured by
  // `FeHandleError` before it clears the rest of the control record, so
  // the fresh per-entry budget every cleanup runs under while unwinding
  // still reflects what the host asked for.
  size_t cleanup_step_limit;
  // The ambient call's configured `FeEvalOptions.max_frames` (0 until set,
  // meaning "use `frame_stack_capacity`"), resolved where it is checked
  // (`AllocateFrame`), not here, for the same reason `cleanup_step_limit`
  // is not. Cleared to 0 by `ClearEvaluationControl` before cleanups run, so
  // a cleanup pushes against the full physical capacity (plus
  // `CleanupFrameReserve`) rather than whatever tight body limit the
  // abandoned computation was configured with.
  size_t max_frames_limit;
  // The ambient call's configured `FeEvalOptions.max_native_reentry` (0
  // until set, meaning "use `DefaultNativeReentry`"), resolved where it is
  // checked (`EnterNativeReentry`). Cleared to 0 by `ClearEvaluationControl`
  // for the same reason `max_frames_limit` is: a cleanup that itself
  // re-enters through a native runs under the default ceiling, not the
  // body's.
  size_t native_reentry_limit;
  // The number of nested evaluator runs currently active below the
  // outermost one -- i.e. live `RunEvaluation` C activations reached only
  // by a native synchronously starting `FeCall`/`FeCallWithOptions`/
  // `FeEvaluate*` while another run is already in progress underneath it.
  // Calling a native from Lisp is not itself re-entry; only that native
  // synchronously entering evaluation again is, so a top-level host call
  // (an empty frame stack at its own `RunEvaluation` entry) is never
  // counted, and neither is a cleanup drain (`RunEvaluationBody`, a
  // different function that never touches this counter). `RunEvaluation`
  // increments this once, checked against the ambient limit, exactly when
  // its own frame stack was already non-empty at entry -- see
  // `EnterNativeReentry`'s comment -- and every `RunEvaluation` barrier
  // saves and restores its own pre-entry value on both the normal and the
  // `longjmp` error path, so nesting unwinds it level by level as each C
  // activation actually returns.
  //
  // Deliberately **not** reset by `ClearEvaluationControl`: unlike the
  // ambient *limits* above, which a cleanup should run under fresh
  // defaults, this counter is a census of live C activations. While
  // `FeHandleError` is draining cleanups -- before any `longjmp` has
  // actually popped a single one of those C frames -- every native
  // activation the abandoned computation was inside is still live on the
  // real C stack; a cleanup native that itself re-enters evaluation is
  // stacking a fresh C frame on top of all of them, and the check needs the
  // true count to stay meaningful. It falls back to the correct value only
  // as the `longjmp` unwind actually happens, one `RunEvaluation` barrier's
  // restore at a time.
  size_t native_reentry_depth;
  const char* error_label;
  size_t error_offset;
  size_t error_line;
  bool error_has_line;
  size_t top_form_line;
  FeCleanupEntry cleanup_stack[CleanupStackSize];
  size_t cleanup_stack_index;
  // Non-null while a cleanup entry's own `fn`/unwind-forms are running: the
  // `jmp_buf` of the `RunOneCleanupEntry` frame currently waiting for it.
  // `FeHandleError` checks this first, so a cleanup's own error resumes
  // there instead of reaching the host and abandoning the rest of the
  // cleanup stack.
  jmp_buf* cleanup_catch;
  char cleanup_error_message[256];
  // The kind that goes with `cleanup_error_message`. A cleanup that runs out
  // of its own bounded budget, or that answers a second host interrupt, must
  // reach the enclosing handler or the host as Budget or Quit; before this
  // was recorded the replay hard-coded Error and both were mislabelled.
  FeCompletion cleanup_error_kind;
  // The floor an abnormal drain stops at. Zero -- the whole registry -- for
  // an ordinary run, since every pending cleanup belongs to the computation
  // being abandoned. `FeTryCallWithOptions` raises it to its own entry depth
  // for the duration of the protected call, so a contained completion
  // unwinds the callee's cleanups and not the host's.
  size_t cleanup_floor;
  // An evaluator barrier owns the automatic jmp_buf it points at. It is
  // installed only for the duration of RunEvaluation(), and errors copy
  // their text and trace below before jumping to it.
  jmp_buf* evaluator_catch;
  jmp_buf* condition_catch;
  FeObject* evaluator_error_trace;
  FeObject* condition;
  // The native currently being invoked.  This is published only for the
  // duration of the callback so the generic argument helpers can construct
  // the same wrong-number condition as Lisp calls.
  FeObject* native_identity;
  size_t native_argc;
  bool native_call_active;
  // A `throw` raised inside a cleanup entry whose matching catch frame is
  // not in the cleanup's own nested run: the tag and value are parked here
  // (both GC roots, since the nested run's frames are discarded before they
  // are read again) and `RunOneCleanupEntry` re-issues the throw in the
  // enclosing context, where the catch frames of the run being unwound are
  // still on the frame stack. Emacs' rule, measured: a cleanup's throw wins
  // over whatever completion was already unwinding.
  FeObject* pending_throw_tag;
  FeObject* pending_throw_value;
  // The kind of the completion currently being drained -- Normal otherwise.
  // Assigned by `RaiseCompletion` (fe_eval.c) for every barrier-backed raise,
  // reset by the outermost run's barrier and by a normal top-level return.
  // Read by `FeGetCompletion` for the host and, as a "draining?" flag, by
  // `AllocateFrame`'s `CleanupFrameReserve` gate: a non-Normal kind grants
  // the reserve, so a cleanup provoked by frame exhaustion is pushable
  // regardless of which wall (step/frame/re-entry/interrupt) tripped.
  FeCompletion completion;
  char evaluator_error_message[1024];
  bool pending_throw;
  bool evaluation_active;
  bool evaluation_limited;
  bool error_has_offset;
  char nextchr;

  // Read-only arena/evaluator statistics, exposed by `FeGetArenaStats`.
  // Every field here is maintained at the one or two existing sites that
  // already change the value it tracks (`MakeObject`, `CollectGarbage`,
  // `FePushGC`, `AllocateFrame`, `EnterNativeReentry`, `PushCleanup`);
  // nothing here reads back its own state to compute anything, so querying
  // it does not walk the arena or any list. `arena_live_count` is the only
  // running total; the rest are high-water marks or event counts, and
  // `free_slots` in `FeArenaStats` is `object_count - arena_live_count`
  // computed at query time rather than stored.
  size_t arena_live_count;
  size_t arena_peak_live_count;
  size_t arena_collection_count;
  size_t arena_peak_gc_stack_depth;
  // High-water mark of `frame_stack_index`: actual simultaneously live
  // ordinary evaluator frames, updated in `AllocateFrame` right after a
  // push succeeds. Unlike the deleted transitional `evaluation_depth`, this
  // counts frames, not pair-form re-entries, so it is directly comparable
  // to `frame_capacity` below.
  size_t arena_peak_frame_depth;
  size_t arena_peak_cleanup_stack_depth;
  // High-water mark of `native_reentry_depth`, the same zero-at-top-level
  // convention: a program that never re-enters through a native never
  // moves this off zero, however deep its ordinary Lisp nesting.
  size_t arena_peak_native_reentry;
  size_t arena_allocation_failures;
};

// Evaluation-control helpers, defined with the rest of the evaluator in
// fe_eval.c. Declared here instead of kept `static` because `GetBound`
// (above, defined in fe.c) charges its environment walk against the same
// step budget, and fe.c's `FeEvaluateStringWithOptions`/
// `FeEvaluateFileWithOptions` wrappers open and close a control record
// around the reader/evaluator loop they keep.
bool BeginEvaluationControl(FeContext* ctx, const FeEvalOptions* options);
void EndEvaluationControl(FeContext* ctx, bool owns_control);
void EvaluationStep(FeContext* ctx);
void FeMarkEvaluatorRoots(FeContext* ctx);
// Defined in fe.c beside `FeToString`; see its comment there.
size_t RenderObject(FeContext* ctx,
                    FeObject* obj,
                    char* dst,
                    size_t size,
                    int qt);
[[noreturn]] void RaiseCondition(FeContext* ctx,
                                 FeCompletion kind,
                                 const char* name,
                                 FeObject* data,
                                 const char* message);
[[noreturn]] void RaiseWrongNumber(FeContext* ctx,
                                   FeObject* function,
                                   size_t argc);
[[noreturn]] void RaiseNativeArity(FeContext* ctx,
                                   FeObject* function,
                                   size_t argc,
                                   const char* message);
[[noreturn]] void RaiseGcStackOverflow(FeContext* ctx);
bool ArenaCanAllocate(FeContext* ctx);

#endif
