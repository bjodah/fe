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
  // Sub-plan 10B of kg's Emacs-subset program (compatibility proofs): the
  // reflective half of the macro surface. `macroexpand-1` performs one
  // expansion step and `macroexpand` repeats it to a fixpoint, both
  // function-shaped (their FORM operand is evaluated like any argument) and
  // both reusing the evaluator's own `FeFrameMacro` body machinery rather
  // than a second transformer-application path -- see `EnterMacroBody` and
  // `MacroexpandStep` in fe_eval.c. `macroexpand-all` needs a code walker
  // and does not exist; it is a primitive only so that calling it says so by
  // name instead of answering `void-function` like a typo (10A Decision 2).
  PMacroexpand1,
  PMacroexpand,
  PMacroexpandAll,
  // Sub-plan 11B of kg's Emacs-subset program (special variables and shallow
  // dynamic binding). `internal--mark-special` is arity 2 -- SYMBOL and a
  // FULL-P flag -- and is the only way a symbol acquires either flag: FULL-P
  // non-nil sets both (Emacs' two-arg `defvar` and `defconst`), nil sets the
  // let-dynamic flag alone (Emacs' one-arg `defvar`, whose symbol binds
  // dynamically while `special-variable-p` still answers nil -- measured on
  // 31.0.90). Marking is idempotent and one-way; Emacs has no unmarking
  // either. `special-variable-p` answers the *special* flag only. Fe has no
  // `defvar` of its own: kg's prelude macros call these two.
  PMarkSpecial,
  PSpecialVariableP,
  // Sub-plan 12B Part 2 of kg's Emacs-subset program: `eval`, Emacs'
  // `(eval FORM &optional LEXICAL)`. Function-shaped -- FORM is an evaluated
  // operand, which is why `(eval '(+ 1 2))` needs the quote, and
  // `(special-form-p 'eval)` is nil on the pinned Emacs too. It evaluates
  // the resulting form in the CURRENT run, by relaying through
  // `FeFrameRelay` exactly as `funcall`/`apply` do, so a condition, throw or
  // quit out of the evaluated form propagates to handlers and catches
  // established outside the `eval` call. A non-nil LEXICAL is rejected by
  // name, the `macroexpand` ENVIRONMENT convention; the environment used is
  // the global one, which is what Emacs' LEXICAL=nil means -- measured:
  // `(let ((qq 1)) (eval 'qq))` is `(void-variable qq)` on 31.0.90 under
  // `lexical-binding: t`, while a `let` over a dynamic name IS visible.
  PEval,
  // Phase 14 of kg's Emacs-subset program: the symbol surface. Eight
  // ordinary functions -- every operand is evaluated, none of them touches
  // the evaluator's state -- so the evaluator only routes them: the whole
  // family sets up one `FeFrameEvalList` and finishes in fe.c's
  // `EvaluateSymbolPrimitive`, beside the symbol accessors and the obarray
  // the four interning ones read. They are CONTIGUOUS on purpose, and
  // `IsSymbolPrimitive` is the range test both routing sites use: eight
  // `case` labels in each of `DispatchPrimitive` and `ResumeEvalList` would
  // be sixteen cyclomatic points spent saying "these eight are one family",
  // which is what the predicate says in two. Keep `PIntern` first and
  // `PSymbolPlist` last if this block ever grows.
  PIntern,
  PInternSoft,
  PSymbolName,
  PMakeSymbol,
  PGensym,
  PPut,
  PGet,
  PSymbolPlist,
  // Phase 19 of kg's Emacs-subset program: `error-message-string`, Emacs'
  // rendering of an ERROR object `(SYMBOL . DATA)` into the sentence a
  // handler prints. An ordinary unary function -- its operand is evaluated
  // and it touches no evaluator state -- so it rides the unary frame beside
  // `keywordp`. The text it starts from is the condition symbol's
  // `error-message` PROPERTY, seeded on the hierarchy's symbols when the
  // context opens, which is why a program can `put` its own over one; the
  // rendering rule is `ConditionMessageText`'s, in fe.c beside the writer it
  // spends.
  PErrorMessageString,
  // Phase 20 of kg's Emacs-subset program: `string<` and `string>`, Emacs'
  // lexicographic string order. Two ordinary binary functions that touch no
  // evaluator state, so they ride the binary frame beside `eq`; the order
  // itself is `StringOperandLess` in fe.c, beside the string model it walks,
  // and `string>` is it with the operands swapped -- which is how Emacs'
  // own `string-greaterp` is defined.
  PStringLess,
  PStringGreater,
  // Phase 24 of kg's Elisp data-model program: the vector family, and the
  // sequence contract that ships with it rather than a phase later. Eight
  // ordinary functions -- every operand is evaluated, none of them touches
  // the evaluator's state -- so the evaluator only routes them, exactly as
  // it routes Phase 14's symbol family: one `FeFrameEvalList` set up by
  // `DispatchPrimitive` and one arm in `ResumeEvalList`, both selected by
  // the range test `IsVectorPrimitive`, both finishing in fe.c's
  // `EvaluateVectorPrimitive` beside the payload storage the family reads.
  // Two range tests cost this evaluator two decision points where sixteen
  // `case` labels would cost sixteen.
  //
  // `length` and `elt` are here rather than in a family of their own
  // because Emacs' contract for them is the SEQUENCE contract -- they answer
  // for lists, strings and vectors alike -- and a `length` that knew about
  // two of the three would be the deliberately broken interval the master
  // plan's Phase 24 exists to avoid. Keep `PVector` first and `PElt` last if
  // this block ever grows.
  PVector,
  PMakeVector,
  PVectorp,
  PAref,
  PAset,
  PVconcat,
  PLength,
  PElt,
  PSentinel
} Primitive;

// A block in the payload region, named by its offset within the live extent
// plus one so that zero can mean "no block". Offsets rather than addresses
// because the storage a handle names MOVES and the handle does not; see the
// publish protocol below.
typedef size_t FePayloadHandle;

typedef union {
  FeObject* o;
  FeNativeFn* f;
  FeDouble n;
  // Sub-plan 05B of kg's Emacs-subset program: the integer payload. 05A's
  // spike confirmed the union stays pointer-sized with it; the assert below
  // makes that permanent.
  int64_t i;
  // Phase 23.1's payload handle. A member of its own rather than a cast of
  // `i`, so that the one place an object names its block is spelled with the
  // handle's own type.
  FePayloadHandle p;
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
  // And the 3rd, on a *pair* only, is which half of that pair the mark phase
  // is currently inside (sub-plan 09C's pointer reversal; `FeMark` documents
  // the encoding). It is meaningless -- and must never be set -- on any other
  // cell, whose `car` word holds `type << GcMarkBit | OtherCell` and so uses
  // this bit as the low bit of the type. No object is less than 8-byte
  // aligned, so a pointer stored in a pair's `car` or `cdr` leaves bits 0-2
  // free for exactly this.
  GcMarkCdrBit = 4,
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
  // The bytes of a non-pair cell's `car` word that are not its type tag.
  // Phase 25 gives them to a string's byte LENGTH, where the cell chain they
  // replaced kept that many bytes of text; see "Strings" in fe.c.
  StringLengthBytes = (sizeof(FeObject*) - 1),
  // The longest symbol name fe builds, in bytes. It has been the reader's
  // token buffer since long before Phase 14; that phase made `intern`,
  // `make-symbol` and `gensym` share it, so a name a program constructs is
  // bounded exactly where a name a program writes is. A symbol's name is a
  // string object and has no structural limit -- this is a policy, and the
  // reason it is one is that a 64-byte stack buffer is also what the
  // printer's number-lookalike test and `signal`'s condition-name copy use.
  SymbolNameLimit = 63,
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

// Sub-plan 09C's pointer reversal stores its links in a pair's `car`/`cdr`
// words with the collector's flags in the low bits, so it needs bits 0, 1 and
// 2 of an object pointer to be free -- i.e. every object at least 8-byte
// aligned. That was prose in `FeMark`'s comment and nowhere else, and
// `FeArenaAlignment()` answers 8, so bit 2 is the *last* free one: a layout
// change that took the alignment to 4 would silently start writing the half
// flag over an address bit. Stated here, beside the `sizeof(Value)` assert
// the integer object stands on, because both are the same kind of claim.
static_assert(alignof(FeObject) > GcMarkCdrBit);

// One pending `unwind-protect`/`FeProtectWithCleanup` registration. Lisp and
// C cleanups interleave in one registry so they share a single ordering, per
// `doc/unwind-design.md`: unwinding always drains the most recently pushed
// entry first, whichever kind it is.
typedef enum FeCleanupKind {
  FeCleanupNative,
  FeCleanupLisp,
  // One shallow dynamic binding owing a restore (sub-plan 11B). It is an
  // entry in this registry rather than a registry of its own because the
  // restore obligation is exactly the one property every cleanup entry
  // already has: it must be honoured on all five completion kinds -- normal,
  // error, throw, quit, budget -- and the drains that do that
  // (`CompletePairFrame`, `CompleteImplicitBodyFrame`,
  // `RaiseCompletionCore`'s handler and host drains, `PerformThrow`'s
  // unwind) are already written against this stack and this stack only. See
  // doc/unwind-design.md's "Two cleanup registries" section.
  //
  // Unlike the other two kinds this one cannot raise and evaluates no Lisp,
  // so `RunCleanups` performs it inline instead of paying
  // `RunOneCleanupEntry`'s barrier and control-record save/restore per
  // binding.
  FeCleanupBinding,
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
    struct {
      FeObject* symbol;
      // The global value cell's contents from before the binding, which is
      // `&unbound` when the symbol had none -- the measured A10a answer, a
      // `let` over an unbound special leaves it unbound again afterwards.
      // `MarkCleanupRoots` marks it: nothing else refers to a shadowed value
      // while the binding is in force.
      FeObject* value;
      // What the host's `FeBindingSaveFn` answered when this binding was
      // pushed, handed back to its `FeBindingTargetFn` at the restore and
      // uninterpreted in between (FE_API_VERSION 11). Zero both when no host
      // callback is installed and when one answered zero; fe never tells the
      // two apart, because it never looks at the number.  NOT an `FeObject*`
      // and deliberately not marked: it is host bookkeeping, and a host that
      // wants an Fe object to survive the binding has to root it itself.
      uintptr_t host_tag;
    } binding;
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
  // A `let` with an Emacs-shaped binding list at least one of whose targets
  // is marked let-dynamic (sub-plan 11B). A binding list with no such target
  // never becomes this kind: it keeps compiling into the lambda application
  // `StartBindingLet` has always built, so nothing about ordinary lexical
  // `let` changes and closure parameter binding never consults the flag --
  // the measured A4 guard, that a defun parameter named after a special is
  // still bound lexically under `lexical-binding: t`.
  //
  // `fn` holds `(BINDINGS . BODY)`, both fixed for the frame's life; `rest`
  // is the remaining raw bindings, `accumulator` the delivered values
  // (reversed while collecting, reordered once), and `callee` the
  // just-delivered value (`&unbound` between deliveries). Every value form
  // is evaluated in the frame's *entry* environment, so this is `let` and
  // not `let*`. When `rest` is empty `InstallLetBindings` binds each target
  // -- dynamic ones by swapping the global cell and pushing an
  // `FeCleanupBinding` entry, lexical ones by extending the environment --
  // and switches the frame to `FeFrameBody`, whose completion through
  // `CompletePairFrame` drains those entries back down to this frame's own
  // `cleanup_checkpoint`.
  FeFrameDynamicLet,
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
// Phase 23.1: where an object names its payload block. Only a type the
// substrate knows about keeps a handle here; see `PayloadSlot` in fe.c.
#define PAYLOAD(x) ((x)->cdr.p)

// THE PAYLOAD SUBSTRATE, AND THE PROTOCOL THAT KEEPS IT SAFE (Phase 23 of
// kg's Elisp data-model program; the Phase 22 ADR's Design B).
//
// A payload is storage an object OWNS but does not contain: it lives in a
// bump-allocated region carved out of the caller's arena, and it MOVES when
// the compactor runs. The `FeObject*` header does not move, which is the
// whole premise -- a host, and every C pointer fe hands out, keeps naming
// the header.
//
// The layout is one block per owner:
//
//     [ FePayloadBlock header ][ `children` FeObject* ][ `bytes` raw bytes ]
//
// The collector traces the leading `children` words and never looks at the
// bytes after them, so one region serves an aggregate (all children) and a
// string (all bytes) without either knowing about the other.
//
// A string's bytes live in the region (Phase 25), so an interior pointer
// into one is a pointer into storage that slides, and the whole class of
// bugs nelisp documented -- a raw pointer held across an allocation that
// compacts under it -- is available to fe.
//
// One protocol keeps it unavailable, and it has three clauses:
//
//   1. A payload pointer is obtained through the one accessor
//      (`PayloadBytes`), IMMEDIATELY before the read or write that spends
//      it. Never parked in a local that outlives the statement, never
//      hoisted out of a loop, never returned to a caller and never handed to
//      a host.
//   2. A payload pointer is INVALID after ANY allocation. "Any allocation"
//      means anything that reaches `MakeObject` or `PublishPayload`,
//      including through a host callback: an `FeWriteFn` running inside the
//      printer, an `FeReadFn` running inside the reader. Re-derive from the
//      handle afterwards; the handle is what stays valid.
//   3. A new or replacement block reaches its owning header through the ONE
//      publish function and no other way. That function roots the owner
//      across its own allocation, because that allocation may collect and an
//      unrooted owner would be swept out from under the handle about to be
//      written into it.
//
// `doc/payload-pointer-census.md` is the checklist: every site in fe and in
// kg that reads through a payload pointer, each with the clause that keeps
// it correct. The printer's byte loops are the ones that straddle an
// allocation and therefore pay clause 2.
//
// WHAT OWNS A PAYLOAD: a vector, whose elements are its block's traced
// children (Phase 24), and a STRING, whose bytes are its block's byte tail
// (Phase 25) -- and a symbol's name is a string, so every context that opens
// at all has a region with blocks in it. That is why `FeOpenContext` carves
// one and why `FeMinimumArenaSize` funds one: an interpreter with no region
// cannot hold its own primitive names.

typedef struct FePayloadBlock {
  // The header this block belongs to. Half of the liveness rule: a block
  // survives only when its owner survived the mark phase.
  FeObject* owner;
  // How many leading words of the block are `FeObject*` the collector traces.
  size_t children;
  // The block's payload bytes, children included, rounded up to
  // `FePayloadAlignment`.
  size_t bytes;
  // Which child the collector is currently inside, while it is inside one.
  // This is what lets the mark phase walk an aggregate of any width without
  // a C frame per element: fe's pointer reversal keeps a pair's position in
  // a tag bit, and a block of n children keeps its position here.
  size_t mark_cursor;
} FePayloadBlock;

enum {
  // No block. A handle is a one-based offset, so zero can never name one.
  FePayloadNone = 0,
  // The granularity a block is rounded to, and the poison step's stride.
  // `alignof(FeObject)` is 8 and a payload word is pointer-sized, so one
  // unit is one word either way.
  FePayloadAlignment = 8,
  // What a vacated byte reads as under the poison knob. Not 0 and not 0xff:
  // both of those are values real data takes, and a NUL in particular is
  // what fe's string walks stop on, so poisoning with one would look like an
  // empty string rather than like a bug.
  FePayloadPoisonByte = 0xa5,
  // The Phase 22 ADR's selected split, as a percentage of the bytes left
  // after the frame region is funded. The number itself lives in fe.h, where
  // a host reads it as the default `FeOpenContextWithOptions` applies; this
  // is the internal name for it, so there is one constant and not two.
  // `FeOpenContext` does NOT apply it: the ADR requires that entry point to
  // keep today's behaviour, and carving a quarter of every host's cells for
  // a region nothing can allocate from until Phase 25 is not today's
  // behaviour.
  PayloadArenaPercent = FeDefaultPayloadPercent,
};

static_assert(sizeof(FePayloadBlock) % FePayloadAlignment == 0);

// Turn a handle into bytes. Clause 1: call it immediately before the access
// and let the result die there.
unsigned char* PayloadBytes(const FeContext* ctx, FePayloadHandle handle);
// Clause 3: the one way a block reaches its owner, for a first block and for
// a replacement alike. Rounds the request up to `FePayloadAlignment`,
// zero-fills the block and stores its handle in the owner. `owner` is rooted
// across the collection this may run, and a request the region cannot hold
// after that collection raises `(payload-exhaustion)` rather than returning
// a half-published block.
void PublishPayload(FeContext* ctx,
                    FeObject* owner,
                    size_t children,
                    size_t bytes);
// Reclaim the blocks whose owners did not survive, and slide the survivors
// down. Called by `CollectGarbage` after the mark phase has restored the
// graph and BEFORE the sweep clears the mark bits, which is the ordering the
// liveness rule depends on.
void CompactPayloads(FeContext* ctx);

// A diagnostic knob that BREAKS the compactor on purpose, by running it after
// the sweep instead of before it. 0 in every configuration and set by no
// target: it exists so that the ordering `CompactPayloads` depends on can be
// shown to be load-bearing -- reversed, the sweep has already cleared every
// mark bit, so the liveness rule reads false for survivors too and the
// substrate's tests fail. An ordering that is merely currently right is not
// evidence.
#ifndef FE_PAYLOAD_COMPACT_ORDER_BUG
#define FE_PAYLOAD_COMPACT_ORDER_BUG 0
#endif

#ifndef FE_DEBUG_PAYLOAD_MOVE
#define FE_DEBUG_PAYLOAD_MOVE 0
#endif

#if FE_DEBUG_PAYLOAD_MOVE

// The poison mode, in the shape kg's `KG_DEBUG_COORDS` and fe's own
// `FE_GC_STRESS` have: the whole facility compiles to nothing at 0, so the
// shipped interpreter carries neither the storage nor a branch for it, and a
// CI lane arms it. At 1, EVERY allocation slides the live payload extent by
// one alignment unit and pattern-fills the bytes it vacates, so a pointer
// held across an allocation reads `FePayloadPoisonByte` instead of data.
// Natural compaction moves only when something died; this moves always,
// which is what turns "corrupts quietly one run in a thousand" into "fails
// on the first run, in the lane".
void MovePayloadRegion(FeContext* ctx);
#define FE_POISON_PAYLOAD_POINTERS(ctx) MovePayloadRegion(ctx)

#else

#define FE_POISON_PAYLOAD_POINTERS(ctx) ((void)(ctx))

#endif  // FE_DEBUG_PAYLOAD_MOVE

#ifndef FE_PAYLOAD_TEST_OBJECT
#define FE_PAYLOAD_TEST_OBJECT 0
#endif

#if FE_PAYLOAD_TEST_OBJECT

// The child-bearing object the substrate's own tests are written against,
// under a knob in the `FE_GC_STRESS` shape: it exists only in the build that
// arms it, so a shipped interpreter has neither the constructor nor a type
// that owns a payload. Phase 24 is where a Lisp-visible aggregate is
// designed, and a substrate test must not pre-empt that decision -- so this
// one borrows `FeTFex2`, the extension slot fe already reserves for a host's
// own types, and the `FeType` enum, the printer and every exhaustive switch
// over a type stay untouched by it.
//
// `children` object slots (initialised to nil) followed by `bytes` raw
// bytes, which is the block layout above with nothing added.
FeObject* MakeAggregate(FeContext* ctx, size_t children, size_t bytes);
FeObject* AggregateChild(const FeContext* ctx,
                         FeObject* aggregate,
                         size_t index);
void SetAggregateChild(const FeContext* ctx,
                       FeObject* aggregate,
                       size_t index,
                       FeObject* value);
// The raw tail, for the allocator's own tests. Clause 1 applies to the
// result exactly as it does to `PayloadBytes`.
unsigned char* AggregateBytes(const FeContext* ctx, FeObject* aggregate);
FePayloadHandle AggregateHandle(const FeObject* aggregate);

#endif  // FE_PAYLOAD_TEST_OBJECT

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
// HASH AND EQUALITY CONTRACTS (Phase 26 of kg's Elisp data-model program,
// doc/plans/2026-08-18-elisp-data-model.md).
//
// Phase 26 gave fe its first hash table -- the symbol index in fe.c, keyed on
// name bytes -- and Phase 27's user tables would be its second, keyed on
// `eq`, `eql` and `equal`. These are the rules a key hash obeys, written
// before anything is built on them, because each one is a rule a plausible
// implementation gets wrong in a way that no test of the index alone would
// show. Nothing here is Lisp-visible; the index's own name hash is the first
// and so far only consumer.
//
// THE ONE-WAY RULE, which everything below is an application of. A hash owes
// its equality predicate exactly one implication:
//
//     EQUAL(a, b)  =>  hash(a) == hash(b)
//
// and never the converse. Two keys that are NOT equal may hash the same; the
// table probes on and compares, which is what a probe is for. So a hash may
// lose information freely -- stop early, ignore part of a structure, fold a
// whole subtree to a constant -- and stay correct, while a hash that
// distinguishes two keys the predicate calls equal is broken however clever
// it is. Every bound below is licensed by this and is not a compromise on it.
//
// `eq` KEYS. fe's `eq` is `IdentityObjects(a, b, false)`: pointer identity,
// or two INTEGERS with equal values, `(eq 3 3)` being t. So an `eq` hash
// hashes the ADDRESS of every object except an integer, and an integer by its
// VALUE. Hashing an integer's address instead is the one way to get this
// wrong, and it puts two `eq` fixnums in different slots. Addresses are safe
// to hash at all because Design B never moves a header: a payload block
// slides under compaction and an `FeObject*` does not, so an `eq` table needs
// no rehash after a collection -- which is the same property the symbol index
// relies on. Doubles take the address route, `(eq 3.0 3.0)` being nil.
//
// `eql` KEYS. `IdentityObjects(a, b, true)` adds same-type doubles equal BY
// BITS, so an `eql` hash reads a double's 64 BITS and nothing derived from
// its value:
//   * `(eql 0.0 -0.0)` is nil -- the sign bit is the one distinction IEEE
//     `==` folds away. A hash that folded -0.0 onto 0.0 would only be
//     colliding, which is free; but a hash computed by floating-point
//     arithmetic can go the other way and give two bit-identical doubles
//     different numbers, and THAT is the broken direction. Hash the bytes.
//   * a NaN is `eql` to itself and to any NaN with the same bits, and a bit
//     hash agrees with that for free. NaNs with different payloads are not
//     `eql` and may hash apart; nothing has to canonicalise a NaN, and
//     nothing may read its payload as a number.
//   * `(eql 1 1.0)` is nil and `(equal 1 1.0)` is nil: an integer and a
//     double are never equal across the type boundary. Mixing the type tag
//     into the hash is therefore a quality choice and not a correctness one.
//
// `equal` KEYS, and the trap in fe's own source. fe has no `equal` primitive.
// It has `eq`, `eql` and `is` -- and `bool Equal(...)` in fe.c is `is`, the
// TOLERANT comparator: it calls `1` and `1.0` equal and compares two doubles
// within an epsilon. Emacs' `equal`, which is what a Phase 27 `:test equal`
// would mean, is the structural one kg defines in `lisp/prelude.el`:
// iterative on the spine, recursive on the car, strings by content, numbers
// by `eql`, vectors element by element, everything else by identity. A hash
// written against fe's `Equal` would fold `1` onto `1.0`, which is harmless,
// and would also be free to call two doubles an epsilon apart equal, which
// the table would then have to make good on. An `equal` hash is written
// against the structural rule and against nothing in fe.c.
//
// CYCLES, and what fe does at its bound. `equal` on circular structure does
// not return. That is Emacs' behaviour and it is kg's: the prelude's spine
// loop keeps no visited set, so it runs until the evaluator's step budget
// trips or `C-g` interrupts it, and what comes back is a raise rather than an
// answer. A HASH has no such liberty, because it is asked BEFORE any
// comparison is possible -- so an `equal` hash carries a bound and folds a
// constant when it reaches it. The one-way rule is what makes that sound: two
// structures that are `equal` agree on every prefix, so they agree on the
// bounded prefix too. The consequence, stated here so that nobody discovers
// it during Phase 27: a cyclic key can be HASHED, and therefore inserted and
// found by identity, while a lookup that has to compare two DISTINCT cyclic
// structures still does not return. A hash table does not make `equal`
// terminate. It makes the HASH terminate.
//
// THE STEP BOUND, which Phase 27 spells as two constants: at most 32 levels
// of car descent and at most 1024 elements consumed in total, whichever comes
// first, after which the hash folds a fixed constant and stops. Depth and
// work rather than a visited set, because a visited set is an allocation
// inside a hash, and a hash that allocates is a hash that can raise, collect,
// and move the very storage the table is about to write into.
//
// MUTABLE KEYS. A hash by content is a hash of the content AT INSERTION TIME,
// and the slot does not move when the content does. fe's strings and vectors
// are mutable (`aset` writes both) and so are pairs (`setcar`), so a key
// mutated after insertion is LOST to any lookup by content while remaining
// reachable by `eq`. fe will not detect that and will not rehash; Emacs
// behaves the same way and documents it. So the rule is a rule for the
// caller: a key in an `equal` table is immutable for as long as it is a key.
// `eq` and `eql` tables are unaffected -- an address does not change, and an
// integer or a double object holds no mutable part.
//
// WHAT THE SYMBOL INDEX IS UNDER THESE RULES: an `equal` hash restricted to
// strings, which is the case with no recursion, no cycles and no bound to
// reach -- `HashNameBytes` over the bytes, and a comparison that is length
// then bytes, embedded NUL included. It is exempt from the mutable-key rule
// for a reason particular to it rather than by luck: a symbol's name string
// is written by the constructor and by nothing afterwards, `symbol-name`
// handing out a fresh copy (`SymbolNameString`), so no Lisp program has a
// reference to mutate. If that ever stopped being true the debug check below
// would say so, a mutated name being a symbol no longer found at its own
// name.
// Phase 26's debug check: does the symbol index still say exactly what
// `symbol_list` says? Every listed symbol reachable in the table from its own
// name's home slot, no slot holding anything else, and the three counts --
// listed, occupied, `symbol_index_count` -- equal. Allocates nothing and
// changes nothing, so it can be called from anywhere a test can stand,
// including between two allocations in the poison lane.
bool SymbolIndexMatchesSymbolList(const FeContext* ctx);
bool StringOperandLess(FeContext* ctx, FeObject* a, FeObject* b);
FeObject* SymbolBindingCell(
    FeObject* sym);  // the cell GetBound's global path returns
FeObject* SymbolFunction(FeObject* sym);  // &unbound when no function binding
void SetSymbolFunction(FeObject* sym, FeObject* fn);
// Phase 14: the property list, nil until a `put` writes one. It lives in the
// symbol object rather than in a context-side registry -- the shape the
// special-variable list below uses -- because an UNINTERNED symbol is the
// first symbol fe has that the collector may reclaim, and a registry keyed
// by symbol would pin every one that ever carried a property.
FeObject* SymbolPlist(FeObject* sym);
void SetSymbolPlist(FeObject* sym, FeObject* plist);
// Phase 14's symbol family (see the `PIntern`..`PSymbolPlist` block above):
// the range test the evaluator routes on, and the one entry point that
// finishes all eight from their evaluated operand list.
bool IsSymbolPrimitive(Primitive primitive);
FeObject* EvaluateSymbolPrimitive(FeContext* ctx,
                                  Primitive primitive,
                                  FeObject* arguments);
// Phase 24's vector family (see the `PVector`..`PElt` block above), in the
// same two-function shape: the range test the evaluator routes on, and the
// one entry point that finishes all eight from their evaluated operand list.
bool IsVectorPrimitive(Primitive primitive);
FeObject* EvaluateVectorPrimitive(FeContext* ctx,
                                  Primitive primitive,
                                  FeObject* arguments);
// A vector's length and its elements, unchecked, for fe's own code: the
// public `FeVectorLength`/`FeVectorRef`/`FeVectorSet` are these three behind
// the type and bounds checks. The length is the payload block's child count,
// which is why it is O(1) and why a vector needs no length word of its own.
//
// `VectorElement` and `SetVectorElement` derive the block address inside
// themselves and spend it there, which is clause 1 of the publish protocol:
// a caller that hoisted the address out of a loop containing an allocation
// -- the printer's element loop is exactly such a loop, because an
// `FeWriteFn` may allocate -- would read storage the compactor had moved.
size_t VectorLength(const FeContext* ctx, FeObject* vector);
FeObject* VectorElement(const FeContext* ctx, FeObject* vector, size_t index);
void SetVectorElement(const FeContext* ctx,
                      FeObject* vector,
                      size_t index,
                      FeObject* value);
// The special-variable registry (sub-plan 11B), defined in fe.c beside the
// symbol accessors because it is symbol metadata; the evaluator's binding
// paths and the two primitives that expose it are in fe_eval.c.
// `MarkSpecialSymbol` is idempotent and one-way: marking full over
// let-dynamic-only upgrades, and nothing ever clears either flag.
void MarkSpecialSymbol(FeContext* ctx, FeObject* sym, bool full);
bool SymbolIsSpecial(FeContext* ctx, const FeObject* sym);
bool SymbolIsLetDynamic(FeContext* ctx, const FeObject* sym);
FeObject* MakeObject(FeContext* ctx);
// Emacs' `equal`: structural for strings, tolerant for numbers, identity
// otherwise. It takes a CONTEXT because a string's bytes are payload and a
// payload address is only reachable from the context that owns the region
// -- the shape every predicate below that reads a name shares.
bool Equal(const FeContext* ctx, FeObject* a, FeObject* b);
// `eq`/`eql`'s shared answer (05D): pointer identity, both-integers-equal, or
// -- for `eql` (`compare_floats`) -- same-type floats equal by bits. Defined
// in fe.c beside `Equal`; the evaluator's `eq`/`eql` primitives call it.
bool IdentityObjects(FeObject* a, FeObject* b, bool compare_floats);
bool IsNamedSymbol(const FeContext* ctx, const FeObject* v, const char* name);
bool IsKeywordSymbol(const FeContext* ctx, const FeObject* v);
bool IsConstantSymbol(const FeContext* ctx, const FeObject* v);
void __attribute((format(printf, 3, 4))) Format(char* result,
                                                size_t size,
                                                const char* format,
                                                ...);

struct FeContext {
  FeErrorFn* error_fn;
  FeNativeFn* mark_fn;
  FeNativeFn* gc_fn;
  // The dynamic-binding location seam (FE_API_VERSION 11), both null unless
  // the host installed them with `FeSetBindingFns`. See fe.h for the
  // contract; the two call sites are `PushDynamicBinding` and
  // `RestoreDynamicBinding`, and null at either of them means fe's own
  // answer -- the tag is null, and the saved value goes back into the bound
  // symbol's own cell.
  FeBindingSaveFn* binding_save_fn;
  FeBindingTargetFn* binding_target_fn;
  // True for exactly as long as `CollectGarbage` is running, which is the
  // one window in which the object graph is not in a state anything else may
  // read: since 09C the mark phase reverses the pointers it walks, so a
  // `car` chain the walk is inside holds parent links, not its own cars,
  // until the walk climbs back out. Two places read this flag.
  //
  // `RaiseCompletionCore` treats a raise from here as fatal. A raise
  // `longjmp`s past the walk's ascent, and there is no stack to unwind it
  // from -- the walk's return path *is* the scrambled graph -- so the arena
  // stays reversed and the next reader dereferences a tagged parent
  // pointer. The recursive walk this replaced only set mark bits, so a
  // `longjmp` out of it left a valid heap; the contract had never had to be
  // written down. It is written down now (`fe.h`, `doc/c-api.md`), and this
  // is what makes a violation a loud abort instead of a silent corruption
  // that surfaces as a SIGSEGV somewhere else entirely.
  //
  // `WriteObject` reads it so fe's own `mark_fn`/`gc_fn` callbacks cannot
  // trip that abort: printing charges the step budget and polls the
  // interrupt, both of which raise, and printing the object it was handed is
  // the obvious thing for such a callback to do (`main.c` does exactly
  // that). Collection is not evaluation, so it does not charge.
  bool collecting;
  void* userdata;
  FeObject* gc_stack[GcStackSize];
  size_t gc_stack_index;
  FeObject* objects;
  size_t object_count;
  FeObject* call_list;
  FeObject* free_list;
  FeObject* symbol_list;
  // Phase 26's symbol index: the private owner of the open-addressed table
  // that answers `FindInternedSymbol`, and how many of its slots are taken.
  // `symbol_list` above is still the authority and still the enumeration
  // order; this is a cache over it, and `SymbolIndexMatchesSymbolList` is
  // what says so. `symbol_index_count` is the length of `symbol_list` --
  // every interned symbol is in exactly one slot -- so a disagreement
  // between the two numbers is the whole of "one of them has a symbol the
  // other does not". See "THE SYMBOL INDEX" in fe.c for the table itself.
  FeObject* symbol_index;
  size_t symbol_index_count;
  FeObject* evaluation_result;
  FeObject* call_result;
  FeObject* root_list;
  FeObject* t;
  // The special-variable registry (sub-plan 11B): a list of `(SYMBOL .
  // FULL-P)` pairs, one per marked symbol, `FULL-P` being `t` for a full
  // special and nil for let-dynamic-only. A list rather than a bit in the
  // symbol object because a symbol's `car` word is a tag whose spare bits
  // the collector's pointer reversal already owns (see `GcMarkCdrBit`), and
  // a list costs two cells per *marked symbol* rather than anything at all
  // per binding -- which is the cost that would be on the hot path. Marked
  // by `CollectGarbage` as a root in its own right; membership is one linear
  // scan, the same shape `FeMakeSymbol`'s interning and `GetBound`'s
  // environment walk already are.
  //
  // Sub-plan 12C Part 2 gave each entry a third slot: `(SYMBOL FULL-P .
  // SCOPE)`, where SCOPE is nil for a mark that is global and an integer
  // naming an input unit for a let-dynamic-only mark made inside one. Still
  // ordinary conses and an ordinary integer, so `CollectGarbage`'s existing
  // walk needs no new shape. A full mark is always global, which is Emacs'
  // rule for the two-argument `defvar` and `defconst`.
  FeObject* special_list;
  // The input unit currently being evaluated, and the monotone source of
  // those numbers (sub-plan 12C Part 2). `EvaluateInput` -- entered exactly
  // once per `FeEvaluateString`/`FeEvaluateFile`, i.e. once per kg `load`,
  // `require`, batch file or prelude install -- takes the next number on the
  // way in and puts the enclosing one back on the way out, so nested loads
  // stack. Zero means "no input unit", which is a *host* context, and a mark
  // made there is global.
  //
  // This is what scopes a one-argument `(defvar v)` to the unit it appears
  // in, which is Emacs' rule as measured on 31.0.90 -- in both directions
  // across a nested load, and in neither of them by half. See
  // `SymbolIsLetDynamic` for the comparison and for the residual this model
  // does not capture.
  size_t input_scope;
  size_t input_scope_next;
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
  // The frame-stack index the currently running cleanup entry started at,
  // and zero when no entry is running. It is the floor that decides which
  // condition handlers a raise inside a cleanup may see: a handler at or
  // above it was established by this cleanup entry itself and is honored;
  // one below it belongs to the computation the drain is abandoning, and
  // the raise takes `cleanup_catch` instead so the rest of the registry
  // still unwinds (06A Decision 4).
  //
  // `run_base` cannot serve as that floor. A *Lisp* cleanup's forms run
  // through `RunEvaluationBody`, whose loop republishes `run_base` to the
  // cleanup's own base, so the handler search is already confined; a
  // *native* cleanup does not, and neither does the window inside
  // `RunEvaluationBody` before the loop starts (where a frame-limit raise
  // lands). In both of those `run_base` is still the enclosing run's, and
  // without this floor the search would reach outside the `unwind-protect`
  // and transfer to a handler with the frame stack and the remaining drain
  // in the wrong state.
  //
  // For a native cleanup the floor equals the frame index the entry started
  // at. What is above it depends on what the cleanup does, and 12B Part 1
  // stated only half of that; the other half is Phase 12's fix cycle's
  // correction. A cleanup that honours `FeCleanupFn`'s contract -- "must not
  // call back into the evaluator" -- pushes no frames at all, so nothing is
  // ever above the floor, no handler is ever accepted, and the arm is
  // bit-identical to the pre-existing behaviour; that is what
  // `TestNativeCleanupHandlerFloor`'s first three cases assert. A cleanup
  // that VIOLATES that contract and re-enters the evaluator does put frames
  // above the floor, and a `condition-case` among them is honored where
  // before the fix it was not. That is not a native special case: it is this
  // floor's own rule -- a handler established by the cleanup's own work
  // belongs to the cleanup, not to the computation the drain is abandoning
  // -- and it is what the pure-Lisp `unwind-protect` form and Emacs 31.0.90
  // both answer. Measured, `(condition-case o (with-lisp-cleanup (fn ()
  // 'body) (fn () (condition-case e (car 6) (error 'inner)))) (error (list
  // 'outer o)))`: `(outer (wrong-type-argument listp 6))` before the fix,
  // `body` after, and `body` in Emacs for the pure-Lisp analogue. The last
  // three cases of `TestNativeCleanupHandlerFloor` pin it.
  size_t cleanup_frame_floor;
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
  // The two conditions a raise must be able to signal when there is no
  // memory left to build one (sub-plan 09B). Both are ordinary
  // `(NAME . nil)` pairs, interned and consed once by `FeOpenContext` and
  // rooted for the context's whole life by `CollectGarbage`, so raising
  // either allocates nothing at all. Before they existed the raise paths set
  // `condition` to nil instead, and `ConditionMatches` -- which has to walk a
  // pair to reach the hierarchy -- then answered false for every *named*
  // handler, so `(condition-case e BIG (error ...))` could not catch an
  // out-of-memory and only `(t ...)` could. `GetCoreObjectCount` counts both
  // names and both pairs, so the minimum arena still holds them.
  //
  // They are shared objects, and a handler is handed the object itself, so a
  // `setcar`/`setcdr` on a caught one used to change what every *later*
  // exhaustion signalled -- for the life of the context. Measured before the
  // fix: `(condition-case e BIG (error (setcar e 'poisoned)))` left the next
  // out-of-memory escaping `(error ...)` and `(arena-exhaustion ...)` alike,
  // and `(setcdr e (list 9 9 9))` made the next one signal
  // `(arena-exhaustion 9 9 9)` with that list permanently rooted through a
  // context-lifetime root. Carrying nil data was never a defence, only a
  // reason there is nothing worth mutating.
  //
  // So every raise re-stamps the object before publishing it
  // (`PublishExhaustion`, fe_eval.c), which is why the two names are kept
  // here as well as inside the pairs: the pair's own `car` is exactly what a
  // poisoning handler overwrote, so it cannot be its own source of truth.
  // Re-stamping costs two stores and no allocation, which is the whole point
  // of these objects.
  FeObject* arena_exhaustion_condition;
  FeObject* evaluation_stack_exhaustion_condition;
  // The interned names of the two conditions above. Reachable anyway through
  // `symbol_list`, which is a root, so these need no marking of their own.
  FeObject* arena_exhaustion_name;
  FeObject* evaluation_stack_exhaustion_name;
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
  // Phase 14's reader flag: set by `ReadAtom` on every call, true when the
  // token it just produced contained a backslash escape. `ReadList` reads
  // it, and only when the object it holds IS the symbol `.` -- which nothing
  // but `ReadAtom` can produce -- so a stale value from an earlier atom
  // cannot be consulted. It is what keeps `(a \. b)` a three-element list
  // where `(a . b)` is a pair: the two produce the same interned symbol, so
  // the dotted-tail test cannot tell them apart from the object alone.
  bool reader_atom_escaped;
  // Phase 14's `gensym` sequence number, Emacs' `gensym-counter` without the
  // Lisp variable: fe exposes no way to read or set it, so the names are
  // unique within a context and nothing more is promised of them.
  uint64_t gensym_counter;

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

  // The payload region (Phase 23.1). Seven fields, and every one of them is
  // zero in a context opened by `FeOpenContext`: that entry point carves no
  // payload bytes, so a host that has not asked for a region has one of
  // length zero and every path below is a comparison against it. See the
  // substrate's comment above for what a block is.
  //
  // `payload_base` is the region's own first byte and never moves.
  // `payload_start` is where the LIVE EXTENT begins inside it, and is zero
  // except under the poison knob, whose whole job is to move the extent at
  // every allocation. A handle is an offset within the extent, so it
  // survives that slide; an address does not, which is the point.
  unsigned char* payload_base;
  size_t payload_capacity;
  size_t payload_start;
  size_t payload_used;
  // High-water mark of `payload_start + payload_used`: the margin this
  // phase's results are read against, and what Phase 23.2 reports through
  // `FeArenaStats`.
  size_t payload_peak_used;
  size_t payload_compaction_count;
  size_t payload_allocation_failures;
};

// The context open the payload region needs, and what
// `FeOpenContextWithOptions` resolves its options to: `payload_percent` of
// the bytes left after the frame region is funded becomes the payload
// region, and the rest stays cells. `FeOpenContext` passes 0, which is
// byte-for-byte today's partition; `PayloadArenaPercent` is the ADR's
// selected split, which is what default options ask for. A percentage above
// 100 is refused here as it is there, by returning null.
FeContext* OpenContextWithPayload(void* arena,
                                  size_t size,
                                  size_t payload_percent);

// The whole ambient evaluation-control record, as one value: everything
// `ClearEvaluationControl` clears, so a raise that ends up *resuming* the
// interrupted program (a `condition-case` handler, a cleanup that runs and
// returns) can put back exactly what it found -- including the steps still
// remaining, not a fresh budget. Deliberately does not carry
// `native_reentry_depth`, which is a census of live C activations rather
// than a configured ceiling; see its own comment on `struct FeContext`.
typedef struct FeEvaluationControl {
  FeInterruptFn* interrupt;
  void* userdata;
  size_t steps;
  size_t poll_interval;
  size_t poll_countdown;
  size_t cleanup_step_limit;
  size_t max_frames_limit;
  size_t native_reentry_limit;
  bool active;
  bool limited;
} FeEvaluationControl;

// The input unit an evaluation is inside is `FeInputUnit` (fe.h, public
// since FE_API_VERSION 8): the scope number a one-argument-`defvar` mark
// made now carries (sub-plan 12C Part 2) together with the source label and
// reader position a diagnostic raised now is prefixed with. They are one
// value because a unit is entered and left as a whole -- `EvaluateInput` on
// its normal return, the two containment barriers in fe_run.c on a contained
// abnormal exit, and `FeEnterInputUnit`/`FeLeaveInputUnit` for a host that
// drives its own read-eval loop -- and because the Phase 12 fix cycle found
// both halves leaking on paths where only one of them was being put back: an
// UNCONTAINED raise left the abandoned unit's scope number in the context for
// the life of the context (so a legitimately-loaded one-argument `defvar`
// mark became invisible to the host, and the abandoned unit's became
// visible), and a CONTAINED one put the scope back but not the label (so
// every later diagnostic in the enclosing file lost its file name).
// `EnterHostInputContext` is the third case: leaving for the host, where
// there is no enclosing unit to go back to.
//
// Defined in fe.c beside `EvaluateInput`, the unit machinery's own site.
FeInputUnit SaveInputUnit(const FeContext* ctx);
void RestoreInputUnit(FeContext* ctx, const FeInputUnit* saved);
void EnterHostInputContext(FeContext* ctx);

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

// The standard condition hierarchy: one row per condition symbol, holding
// the name, the name of the condition it is a subtype of (null for a root)
// and the text of its `error-message` property. The table itself stays in
// fe_eval.c, where the handler search walks it; what fe.c needs from it is
// the ability to walk every row once at context open -- to seed the
// properties, and to count what that seeding allocates for
// `FeMinimumArenaSize` -- and to ask whether a condition is a `file-error`,
// which is the one class Emacs' rendering treats differently.
// `ConditionRowAt` answers null past the last row, so a caller loops without
// knowing the count.
typedef struct ConditionParent {
  const char* name;
  const char* parent;
  const char* message;
} ConditionParent;
const ConditionParent* ConditionRowAt(size_t index);
bool ConditionInheritsFrom(const FeContext* ctx,
                           const FeObject* symbol,
                           const char* ancestor);
// Emacs' `error-message-string` rendering of the ERROR object `error`,
// written into `dst` (always NUL-terminated) and never allocating: both its
// callers -- the primitive, and `FeErrorMessageString` on the host's error
// path -- need a rendering that cannot itself fail for want of arena.
// Defined in fe.c beside the writer it spends.
size_t RenderErrorMessage(FeContext* ctx,
                          FeObject* error,
                          char* dst,
                          size_t size);
// The `error-message` property text seeded on every hierarchy symbol at
// context open. Defined in fe.c; called from `OpenContext`.
void SeedConditionMessages(FeContext* ctx);
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
// The collector's contract, broken (sub-plan 09C; see `FeContext::collecting`
// and `FeSetMarkFn` in fe.h). `what` names the violation and `detail`, when
// non-null, is whatever text goes with it. Prints both and aborts: there is
// nothing to recover to, because the mark phase's return path is the
// half-reversed graph itself. Deliberately does not call `error_fn` -- an
// `error_fn` leaves non-locally, which is the failure being reported.
[[noreturn]] void FatalCollectorViolation(const char* what, const char* detail);
bool ArenaCanAllocate(FeContext* ctx);

// The run driver's seam (sub-plan 11B of kg's Emacs-subset program). fe_run.c
// holds the two places an evaluator run's error barrier is installed
// (`RunEvaluation`, `RunEvaluationBody`), the `Evaluate` seam every
// evaluator-internal caller reaches them through, and the public
// `FeEvaluate*`/`FeCall*` surface; fe_eval.c keeps the loop those drive, the
// frame pushes, the barrier helper and the raise core. Nothing below is new
// or moved code -- each was `static` in fe_eval.c until the split, and the
// split exists so the 520-per-file complexity cap keeps binding on the
// evaluator at full strength. Both directions are listed here because the
// seam has two of them, and it is the only cross-TU coupling the split
// created.
//
// fe_run.c -> fe_eval.c:
FeObject* RunEvaluationLoop(FeContext* ctx, size_t base);
jmp_buf* BeginRunBarrier(FeContext* ctx, jmp_buf* jump);
[[noreturn]] void TransferRunError(FeContext* ctx, jmp_buf* saved_catch);
void EnterNativeReentry(FeContext* ctx);
void PushEvaluationFrame(FeContext* ctx,
                         FeObject* obj,
                         FeObject* env,
                         FeObject** bind);
void PushBodyFrame(FeContext* ctx, FeObject* env, FeObject* forms);
FeObject* ResolveFunctionCallable(FeContext* ctx, FeObject* fn, bool* cycle);
bool IsRawFormCallable(const FeObject* fn);
FeEvaluationControl SaveEvaluationControl(const FeContext* ctx);
void RestoreEvaluationControl(FeContext* ctx,
                              const FeEvaluationControl* control);
[[noreturn]] void RaiseCompletionCore(FeContext* ctx,
                                      FeCompletion kind,
                                      const char* msg);
//
// fe_eval.c -> fe_run.c:
FeObject* Evaluate(FeContext* ctx,
                   FeObject* obj,
                   FeObject* env,
                   FeObject** bind);
FeObject* RunEvaluationBody(FeContext* ctx, FeObject* forms, FeObject* env);

// The completion seam (Phase 20 of the same program). fe_unwind.c holds the
// ambient evaluation-control record, the condition hierarchy and its handler
// search, the cleanup registry and every raise; fe_eval.c keeps the
// frame-driven evaluator. As with the run seam above, nothing here is new or
// moved *code* -- each was `static` in fe_eval.c until the split, which
// exists so the 520-per-file complexity cap keeps binding on the evaluator
// at full strength.
//
// fe_eval.c -> fe_unwind.c (the raises, the two cleanup pushes, the drain,
// and the `signal` name gate; `SaveEvaluationControl`,
// `RestoreEvaluationControl`, `EnterNativeReentry` and `RaiseCompletionCore`
// are declared above because fe_run.c reaches them too):
[[noreturn]] void RaiseWrongType(FeContext* ctx,
                                 const char* predicate,
                                 FeObject* value);
[[noreturn]] void RaiseNamedError(FeContext* ctx,
                                  const char* name,
                                  const char* message);
[[noreturn]] void RaiseBudget(FeContext* ctx, const char* msg);
// The payload region is full and a compacting collection did not free enough
// of it. `(payload-exhaustion)` names that, and degrades to the pre-built
// `(arena-exhaustion)` when the cell pool is spent too -- building a
// condition object needs cells, and the pre-built one is the only condition
// a state with none can signal. Both are catchable, and `error` catches
// either.
[[noreturn]] void RaisePayloadExhaustion(FeContext* ctx);
void PushCleanup(FeContext* ctx, FeCleanupEntry entry);
void PushDynamicBinding(FeContext* ctx, FeObject* symbol, FeObject* value);
void RunCleanupsDownTo(FeContext* ctx, size_t target);
void ValidateConditionHandlers(FeContext* ctx, FeObject* handlers);
bool IsConditionSymbol(const FeContext* ctx, const FeObject* symbol);
//
// fe_unwind.c -> fe_eval.c: one edge, the throw a cleanup re-issues.
bool PerformThrow(FeContext* ctx, FeObject* tag, FeObject* value);

#endif
