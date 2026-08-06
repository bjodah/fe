# Implementation

## Memory

The implementation uses a fixed-size region of memory supplied by the caller
when creating the `FeContext`. The implementation stores the context at the
start of this memory region, then an evaluator-frame region, then the
`FeObject` region. The frame region has a 64-frame floor, a 32-frame cleanup
reserve, and receives 10% of bytes beyond the minimum; the remaining bytes
become object slots. The arena must satisfy `alignof(FeContext)`; static
assertions ensure that both following regions are aligned. Fe neither
reallocates nor frees this storage; its address, size, and exclusive lifetime
are controlled by the caller through `FeCloseContext()`.

`FeMinimumArenaSize()` derives its result from the private context and object
layouts and from the objects required to intern and bind every core primitive.
`FeOpenContext()` validates null pointers, alignment, size, and address-space
boundaries with checked arithmetic before writing to the arena. Invalid arenas
return `nullptr` without invoking the error machinery.

The context also stores one opaque host userdata pointer and its error, mark,
and collection callback pointers. These details remain private to `fe.c`.

## Objects

All data is stored in fixed-sized `FeObject`s. Each object consists of a `car`
and `cdr`. The lowest bit of an object’s `car` stores type information — if the
object is a `FeTPair` (cons cell) the lowest bit is `0`, otherwise it is `1`.
The second-lowest bit is used by the garbage collector to mark the object and is
always `0` outside of the `CollectGarbage` function.

Pairs use the `car` and `cdr` as pointers to other objects. As all objects are
at least 4 byte-aligned we can always assume the lower two bits on a pointer
referencing an object are `0`.

Non-pair objects store their full type in the first byte of `car`.

### Strings

Strings are stored using multiple objects of type `STRING_BUFFER` linked
together — each string object stores a part of the string in the bytes of `car`
not used by the type and GC mark. The `cdr` stores the object with the next part
of the string, or `nil` if this was the last part of the string.

### Symbols

Symbols store a pair object in the `cdr`; the `car` of that pair is a second
pair holding the symbol's name string and its function cell, and the `cdr`
part of the outer pair contains the globally bound value for the symbol:
`CDR(sym) = ((name . function) . value)` (sub-plan 04B of kg's Emacs-subset
program). The function cell starts out holding `unbound`; sub-plan 04C made
it live, not dormant: call position, `funcall`/`apply`, and `FeGetFunction`
all read it through one shared resolver (`ResolveFunctionCallable`, below),
and only the `fe_internal.h` accessors `SymbolFunction`/`SetSymbolFunction`
spell the cell. Every reader of this private
layout goes through the named accessors (`SymbolName`, `SymbolBindingCell`,
`SymbolFunction`) rather than spelling the pair walk itself, so the later
Phase-4 lookup slices can change resolution without touching the
representation readers. Symbols are interned.

### Numbers

Numbers store an `FeDouble` in the `cdr` part of the `object`. By default
`FeDouble` is a `double`, but any value can be used so long as it is equal to or
smaller in size than an `FeObject` pointer. If a different type of value is
used, `FeRead` and `FeWrite` must also be updated to handle the new type
correctly.

Sub-plan 05B of kg's Emacs-subset program added the integer object:
`FeTInteger`, an `int64_t` payload in the `cdr` (the `Value` union, which
`static_assert`s pointer-size on both CI compilers), constructed and read
through the host API (`FeMakeInteger`/`FeToInteger`). 05C's numeric tower made
it live rather than dormant: arithmetic, the chained comparators, `=`, `/=`,
`integerp`/`floatp`, `Equal()`/`is`, and the math natives all dispatch on both
numeric tags, and the arithmetic primitives and the rounding family return
integers. 05D's cut made it reachable from source: `ReadAtom`'s bare `strtod`
became a classify-then-convert lexer (integer = sign + digits + optional
trailing dot; float = fraction and/or exponent; the printer's nonfinite
spellings read back; everything else -- `0x10`, `inf`, `nan`, `1e` -- is a
symbol), integer literals overflow int64 by falling back to a double (the
recorded pre-bignum divergence), and the writer's `EmitDouble` integral
shortcut died: floats print Emacs' shortest-round-trip spelling with an
explicit `.0` or exponent, and `eq`/`eql` landed beside `is`. A written
`(+ 2 3)` now computes the integer `5` end to end. An integer marks and
collects exactly as a double does: a leaf.

### The numeric tower

Sub-plan 05C extended every numeric path to both tags behind the unchanged
reader. The promotion rule lives once, in `GetNumericPair` (fe_eval.c): two
numeric operands become two `int64_t`s when both are integers, two `double`s
otherwise — integer arithmetic stays integer, and any double promotes the
rest of the reduction. Every binary arithmetic combine, chained-comparison
step and `=`/`/=` comparison goes through it, and a non-number operand is
`wrong-type-argument` there, the numeric family's name (05A Decision 5)
replacing the old "expected double" texts. `ResumeArith`'s accumulator is
either-type: the first delivered operand seeds it (`SeedArith`, giving unary
`-` its negation and unary `/` its truncated reciprocal), and every later
delivery combines through `GetNumericPair`. Integer overflow on
`+`/`-`/`*` (via `ckd_add`/`ckd_sub`/`ckd_mul`), integer division by zero,
and `INT64_MIN / -1` are all `arith-error` — never UB and never a promotion
to float — matching 05A's Decision 5 and the recorded divergence from Emacs'
bignums (row A8). The chained comparators `<`/`<=`/`>`/`>=` and `=` are
variadic in the `FeFrameEvalList` frame, sharing one comparator loop whose
direction is the primitive (`NumericSatisfies` over adjacent pairs, no
short-circuiting); `/=` is strictly binary, its arity rejected at dispatch
before the frame exists. `integerp`/`floatp` are `FeFrameUnary` leaves
answering from the tag alone. `Equal()`/`is` gained Decision 2's integer arm:
exact within integers, mathematical value across int/float (the integer
converts to double), the epsilon comparison retained for double/double. The
math natives return per-function types: `floor`/`ceiling`/`round`/`truncate`
return integers (two-argument forms divide first; an out-of-range result, NaN
or ±Infinity included, is `arith-error`), `expt` stays integer for an integer
base and non-negative integer exponent under the same overflow policy, and
the transcendentals stay floats. In the standalone `fe` binary the Fex
extensions (`fex_math.c`) still shadow `floor`/`ceiling`/`log`/`round`/
`truncate` with their one-argument versions; that is a recorded fact 05C's
tests work around (`scripts/math.fe` pins the Fex side, `TestMathNatives` the
core build), not something this slice changed.

### Primitives

Primitives (built-ins) store a `Primtive` `enum` value in the `cdr` part of the
object.

### Native Functions

`FeNativeFn`s store a function pointer in the `cdr` part of the object.

### `FePtr`

`FePtr`s store a `void*` in the `cdr` part of the object. The `FeHandler`
functions `gc` and `mark` are called whenever an `FePtr` is collected or marked
by the garbage collector — the set `FeNativeFn` is passed the object itself in
place of an arguments list.

## Environments

Environments are stored as association lists; for example, an environment with
the symbol `x` bound to `10` and `y` bound to `20` would be `((x . 10) (y .
20))`. Globally bound values are stored directly in the symbol object: a symbol's
`cdr` is the pair `((name . function) . value)`, whose `cdr` -- the value cell
-- is the same shape as a lexical binding, so `GetBound`'s global path returns
that cell (`SymbolBindingCell`) and one lookup returns the cell either way. The
`(name . function)` inner pair is private to the symbol accessors; the value
path's unit of currency is the binding cell, so there is deliberately no
value-shaped accessor.

A symbol exists as soon as it is read, which is not the same as having a value.
A fresh symbol's value cell and function cell both hold `unbound`, a private
static object outside the arena — like `nil`, so the collector neither sweeps
nor has to mark it, and `FeMark` treats it as a leaf. Nothing returns it: Lisp
cannot reach a value cell (`(cdr sym)` is a type error, and `(env)` yields
symbols whose printed form is their name), and the value cell's reader —
symbol evaluation — turns it into `void-variable NAME`. Since sub-plan 04C,
call position is not a second reader of the value cell — it resolves the
function cell — and 04D's namespace cut deleted the transitional value-cell
fallback, so the value cell has exactly one reader. It is
tagged `FeTFree` so that an escape aborts in
the writer rather than impersonating a value.

## Sub-plan 04C: the function namespace

The namespace split reached its final form with sub-plan 04D's cut. The
evaluator's one shared function-designator resolver, `ResolveFunctionCallable`,
walks a function cell's symbol indirection chain iteratively — charging one
`EvaluationStep` per hop, and finding a cycle with two-pointer detection
rather than leaving it to exhaust the step budget —
and reports `void-function NAME` when the chain dies in an empty function cell.
A cycle is `cyclic-function-indirection` for every reader that has a
catchable evaluation around it, and `nil` for the one that may not — the
host-facing `FeGetFunction`, whose caller is a C frame the longjmp would
skip past; the resolver's `cycle` out-parameter is which of the two the
caller asked for.
Sub-plan 04C shipped the same resolver with a value-cell fallback after the
last link, because every bootstrap callable still lived in a value cell; 04D
moved the bootstrap — the primitives, the `fn` alias, and the math natives
registered through `FeDefineNative` — into function cells (leaving `t`, `pi`
and `e` as values) and deleted the fallback, so `car`, `+`, and every native
resolve through the function cell like any other callable, and
`FeDefineNative` itself writes the same cell. The one upfront `EvaluationStep`
in `ResolveCallHead` is the charge the pre-04C head resolution made, so an
unbound cell costs exactly what `CDR(GetBound(head, env))` used to and the
step pins hold. That upfront charge is call position's alone, and the step
accounting is deliberately non-uniform because of it: `funcall`/`apply` and
`FeGetFunction` reach the resolver directly and pay only the per-hop charges,
so resolving the same designator chain costs one step less through them than
through a symbol head.

An empty function cell is reported as plain `&unbound`, and each caller raises
`void-function` at the name *it* was given rather than at the last link the
chain reached -- `(fset 'a 'b) (a)` and `(funcall 'a)` are both
`void-function a`, which is what Emacs reports.

`funcall`/`apply` are implemented with the evaluate-then-redispatch shape,
chosen over a dedicated apply frame kind: both are function-shaped special
forms that run their complete raw argument list through the existing
`FeFrameEvalList` machinery, then resolve the first result through the same
designator resolver and hand the remaining *already evaluated* values to the
ordinary call path. `MakeCallForm` rebuilds them as a `(callable (quote v)
...)` form — the same quoted-argument construction `FeCall`'s host path
uses — so the call is pushed as an ordinary sub-expression frame with no new
frame kind, no new GC-per-state row, and no new arm in the exhaustive
`FeFrameKind` switches; the cost is the few conses the wrap and reorder
build per call, which the dedicated frame shape would have saved at the price
of a new frame kind touched in every switch. `apply`'s spread is a separate
helper, `SpreadApplyArgs`, which rebuilds the fixed operands plus the spread
list's elements into a fresh argument list — never mutating the caller's —
after validating that the final operand is a proper list. The 03F rooting
lesson applies to both boundaries: the evaluated operand buffer is rooted in
the EvalList frame's `accumulator` and, across the redispatch, the relay
frame's fields, so a collection forced by the *called body* finds every value
still live (`test_api.c`'s resumable-frame GC table drives collections across
`funcall` and `apply` calls). Both helpers cost a fixed number of GC-stack
slots rather than one per element: because the frame field is already a
mark-phase root, each pass restores the checkpoint the helper itself took and
`MakeCallForm` re-pushes only its chain's head, the idiom `ReadList` uses.
Leaving every cons `MakeObject` pushes on the GC stack until the caller's own
restore made a wide `apply` die on "GC stack overflow" where the same call
written out directly ran.

That synthetic form is visible: a backtrace taken inside a `funcall` or an
`apply` shows the rebuilt `(callable (quote v) ...)` frame, not the
`(funcall ...)` form the program wrote, because the trace is a stack of the
pair forms actually being evaluated. This is documented rather than hidden --
suppressing it would mean a second, parallel notion of what a frame is.

Only a *function* may be redispatched. A macro and the special-form primitives
consume their operands raw, so handing them the quote wrappers would execute
something else entirely (`(funcall 'quote 'a)` answering `(quote a)`); both are
`invalid-function`, as in Emacs. The function-shaped/special-form split is a
table read off `DispatchPrimitive`'s own routing, and the C API exposes it as
`FeIsFunction()` so a host's `functionp` and the interpreter's rejection cannot
drift apart.

### The reader and writer at the cut

Two more 04D changes live in fe.c's reader and writer rather than in the
evaluator. The reader's `#` case no longer returns the following form as-is:
when the byte after `#` is `'`, it builds `(function X)` through the same
`ReadWrapped` construction `quote` uses, keeping the `stray '#''` diagnostic
for a bare `#'`; `#` otherwise stays an ordinary symbol character. The writer
prints the exact `(function X)` pair as `#'X` -- the abbreviation Emacs' own
printer uses, and the shape the `function` special form accepts -- while
`(function)` and `(function X . tail)` still print as ordinary pairs, which is
what makes the `reader-sharp-quote-identity` comparison match. Neither touches
the frame machine or the object layout.

## Garbage Collection

Fe uses a simple mark-and-sweep garbage collector in conjunction with a
freelist. `FeOpenContext` initializes the context and creates a freelist
containing all the objects. When an object is required it is popped from the
freelist. If there are no more objects on the freelist, the garbage collector
does a full mark-and-sweep run, pushing unreachable objects back to the
freelist. Thus, garbage collection may occur whenever a new object is created.

The context maintains a `gc_stack` which protects objects that may not be
otherwise reachable. Newly created objects are automatically pushed to this
stack. Reader and evaluator results are either protected there or remain
reachable from an existing interpreter root. Accessors return existing objects
without pushing them; their lifetime therefore depends on an existing root
until the caller explicitly calls `FePushGC()`.

`FeGetArenaStats()` (`doc/c-api.md`'s "Arena Statistics") exposes this
freelist/live-object bookkeeping, the collection count, and the peaks
described below (GC-stack depth, frame depth, cleanup-stack depth, native
re-entry depth) as a read-only snapshot. It adds no new counters beyond what
the sites already below track for their own bookkeeping -- `arena_live_count`
is the one exception, a running total `MakeObject()`/`CollectGarbage()`
maintain solely so the accessor can report `free_slots` without walking the
freelist.

The `gc_stack` is a fixed 4096-slot array inside `FeContext`. Sub-plan 03E
(the frame machine's own gate) stopped it growing with Lisp nesting at all --
every live frame is a mark-phase root, so an intermediate result needs no
separate `FePushGC()`, only the run's own final result gets one push -- so
today it bounds only the reader's and writer's own native C recursion, not
evaluation. Because the array lives in the arena, its size is still the
dominant term in `FeMinimumArenaSize()`.

Sub-plan 03F replaced the single, transitional `max_depth`/
`evaluation_depth` pair -- itself a stand-in the frame machine's own
migration (03C-03E) kept alive only for API continuity while it moved
Lisp-nesting state off the C stack -- with two independent, permanent
bounds, because they protect two different things:

- **Lisp nesting** (`FeEvalOptions.max_frames`, `FeArenaStats.frame_capacity`
  / `peak_frame_depth`) is a frame-stack slot count, not a C-stack bound: the
  frame machine roots every level of ordinary recursion in the arena, so this
  costs no C stack regardless of its value. `AllocateFrame()` checks it
  before every push -- `min(max_frames, frame_stack_capacity)`, zero meaning
  "use the arena's own physical capacity" -- and raises `evaluation frame
  limit exceeded` before writing the slot that would exceed it. A private
  `CleanupFrameReserve` (32 frames) is available only while a cleanup is
  draining (`ctx->completion != FeCompletionNormal`), so exhaustion during an
  ordinary body does not itself immediately refuse the cleanup it triggers.
- **Native re-entry** (`FeEvalOptions.max_native_reentry`,
  `FeArenaStats.peak_native_reentry`) is the one place a fresh C activation
  still legitimately enters the evaluator: a native that synchronously calls
  `FeCall`/`FeCallWithOptions`/`FeEvaluate*` starts a nested `RunEvaluation`
  on top of its own C frame. `native_reentry_depth` on `FeContext` counts
  exactly those nested runs, not every native invocation -- calling a native
  from Lisp is not itself re-entry, only that native synchronously starting
  *another* evaluation is. `RunEvaluation()` increments it, checked against
  `EnterNativeReentry()`'s effective limit (`native_reentry_limit`, or
  `DefaultNativeReentry` when left 0), exactly when its own frame stack was
  already non-empty at entry -- the one condition that can only be true if a
  native, somewhere below on the C stack, is what started this run. A
  top-level host call always sees an empty frame stack at its own entry and
  is never counted; neither is a cleanup drain (`RunEvaluationBody()`, a
  distinct function that never calls `EnterNativeReentry()`, even though its
  own base is typically deep inside the physical frame reserve). Exceeding
  the limit raises `native evaluation re-entry limit exceeded` before the
  nested run starts.

Every `RunEvaluation()` barrier saves its own pre-entry `native_reentry_depth`
in a local and restores it on both the normal-return and `longjmp` paths, so
nesting unwinds the counter level by level as each C activation actually
returns -- the counter is a plain `size_t`, never a copied `jmp_buf`.
`ClearEvaluationControl()` -- run by `FeHandleError()` before cleanups drain,
and by `EndEvaluationControl()` on an owning call's ordinary return -- resets
the ambient *limits* (`max_frames_limit`, `native_reentry_limit`) to their
built-in defaults, but deliberately leaves `native_reentry_depth` itself
alone: while an error is unwinding, before any `longjmp` has popped a single
C frame, every native activation the abandoned computation was inside is
still real and still live on the C stack, and a cleanup native that itself
re-enters evaluation stacks a fresh C frame on top of all of them, so the
check needs the true count to stay meaningful. It falls back to the correct
value only as the unwind actually happens, one `RunEvaluation()` barrier's
restore at a time. This replaces a genuine, now-closed hazard from the
migration period: an *owning* nested `FeCallWithOptions` (started when no
control record was yet active, e.g. under a plain `FeEvaluateString`) used to
run `EndEvaluationControl()` on its way out, which cleared the whole control
record -- both counters, in the pre-03F design -- while the enclosing frames
were still live, so `FeFrameNative`'s resume had to save and restore both
active-depth values around every native call to put the enclosing
evaluation's accounting back. 03F's redefinition (only nested `RunEvaluation`
calls increment; `ClearEvaluationControl()` never resets the depth) makes
that per-call save/restore unnecessary: there is no longer a live counter for
an owning call's cleanup to corrupt in the first place.

Evaluation has one context-owned frame stack. The collector marks every live
frame. There is no temporary recursive-dispatch frame kind any more: sub-plan
03E converted the last special forms and primitives to frame continuations
and deleted it, along with the recursive `EvaluatePrimitive`, `EvaluatePair`,
`EvaluateList`, `DoList`, `EvaluateSetq`, `EvaluateSet` and
`EvaluateNumericEqual` helpers it was the last caller of. Self-evaluating
objects, symbols, and primitive `quote` use
the frame loop directly, as does computed call-head resolution: a call whose
head is not a symbol switches its frame to a call-head continuation, pushes
the head expression on the frame stack, and resumes when the head's value is
known, so a pure computed-head chain stops consuming C stack. Symbol heads
resolve synchronously in the loop. Argument evaluation for an ordinary
callable -- a native function or a lambda -- is also a frame transition, not
a recursive `EvaluateList`: the call frame stores the callable and the
remaining raw arguments, pushes each argument as a sub-expression frame, and
accumulates the values (prepended, then reordered) as each one completes, so
a call chain nested through argument positions stops consuming C stack too.
The step charge `EvaluateList` made before each argument is preserved, as are
the `FeGetNextArgument` list-shape errors, the call trace, and the GC
checkpointing of the accumulating list. Lambda application is the next frame
transition: once the argument list is complete and the callable is a lambda,
`ArgsToEnv` still binds the arguments into the callee environment as an
ordinary allocating, non-evaluating helper -- it deliberately does not become
a frame kind -- and the frame then becomes a sequential-body frame that
evaluates the body forms one at a time, charging the same one step before
every body form and every parameter walk the old recursive `DoList` and
`ArgsToEnv` charged, and pushing each form as a sub-expression frame. The
body frame owns its environment, and a body form's `let` is handed a
pointer to it (the `bind` field of the pushed frame), so a `let` in a
lambda body extends the environment the following body forms see exactly as
the old recursive `DoList`'s `&env` out-parameter did -- now `FeFrameLet`'s
own job (03E), which writes into `*frame->bind` once its value form's
delivery arrives, or (when `bind` is `NULL` -- a `let` used somewhere with
no enclosing sequence to extend, such as the untaken shape of an `if`'s
`bind=NULL` branch) never evaluates the value form at all, exactly as the
old recursive arm's `if (newenv) { ... }` did.
A pure call chain through lambda bodies therefore stops consuming C stack
too, with each level's nested call a sub-expression frame in the same
evaluator run. A macro call is likewise two frame transitions: `ArgsToEnv`
binds the raw, unevaluated arguments into the macro's closure environment
(charging one step per parameter walk, exactly as it does for a lambda),
the frame becomes a sequential body that evaluates the macro's forms one at
a time with the same `let` threading as a lambda body, and once the last
body form produces the expansion the frame restores the macro call's
cleanup, GC and call-trace checkpoints and pushes the raw expansion as a
sub-expression frame in the caller environment -- a genuine frame-machine
tail call that holds the logical depth across the expansion. That makes the
physical frame wall, not C recursion, what stops a macro whose expansion is
another macro call, and it keeps the expansion off the call site and out of
the macro's own backtrace frame, exactly as the recursive arm did.
A native call is the one remaining ordinary callable that is not a Lisp
special form or primitive: once the argument frame has reordered the
evaluated argument list, the frame switches to `FeFrameNative` and the run
loop invokes the `FeNativeFn` synchronously from that explicit state -- the
existing public signature unchanged, and no per-native `setjmp` (the
enclosing `RunEvaluation` barrier is the only one in effect), and no
per-call bookkeeping either: calling the native is not itself bounded here,
because it is not itself re-entry. If the native synchronously starts a
nested run of its own, that nested `RunEvaluation()` call is what checks and
counts it (see "Native re-entry" above) -- a native calling back into
`FeCall*` is the one C-recursion route the frame machine deliberately keeps,
and it is bounded at its own entry point, not wrapped around every native
call whether or not it re-enters.

Every remaining special form and primitive is a frame continuation, added by
sub-plan 03E. `EvaluatePrimitive`'s old recursive switch is not one frame
kind with a generic "evaluate every operand first" policy: the primitives
disagree, deliberately, about when a raw argument is checked relative to
when the next one is evaluated (`doc/language.md` and
`fe/tests/*.err`/`.out` pin the exact answers), so the frame kinds are
grouped by evaluation *shape*, not by primitive:

Primitive arity is checked in one raw-form preflight table before a
continuation push, so fixed-arity failures do not evaluate an operand. Lambda
and macro operands are evaluated completely before `ArgsToEnv` validates and
binds the parameter list. Both paths use the same condition builder, rooting
the original designator/callable and original argument count while constructing
`(FUNCTION NARGS)` data.

- `FeFrameIf`, `FeFrameWhile`, `FeFrameAndOr`, `FeFrameLet`, `FeFrameSetq`
  and `FeFrameRelay` (`do` and `unwind-protect`'s body) are each their own
  kind, since each threads a distinct small state machine (`if`'s
  condition-then-branch, `while`'s fixed condition/body forms and per-pass
  GC reset, `and`/`or`'s short-circuit, `let`'s `newenv`-or-nothing case,
  `setq`'s pair-at-a-time assignment, a plain relay of whatever a single
  pushed sub-frame delivers).
- `FeFrameUnary`
  (`assert`/`not`/`atom`/`car`/`cdr`/`boundp`/`makunbound`, 04C's
  `symbol-function`/`symbol-value`/`fboundp`/`fmakunbound`, and 05C's
  `integerp`/`floatp`)
  and `FeFrameBinary` (`cons`/`setcar`/`setcdr`/`is` and 04C's
  `fset`/`defalias`) share one kind
  per arity, dispatching the specific check or side effect by the resolved
  primitive object at each delivery -- `setcar`/`setcdr` validate their pair
  operand immediately on the first delivery, before the second is even
  evaluated, and `boundp`/`makunbound`/`integerp`/`floatp` reject a leftover
  argument the other unary primitives silently ignore. The comparisons
  `<`/`<=` shared `FeFrameBinary` until 05C made them chained and variadic;
  they now live in `FeFrameEvalList` beside `=`, and their binary arm died
  with the move.
- `FeFrameArith` (`+`/`-`/`*`//`) streams and validates every operand as it
  arrives, exactly as the old `ARITH_OP` macro's loop did, never batching
  the whole list first; `FeFramePrint` streams too, but interleaves output
  and separators with evaluation instead of combining a running total.
  The arithmetic accumulator starts at the `&unbound` sentinel, so an
  operandless frame has to complete with the operator's identity element
  (`(+)` and `(-)` 0, `(*)` 1, `(/)` `wrong-number-of-arguments`) rather than
  with the accumulator: "no expression evaluates to `&unbound`" is an
  invariant of the whole evaluator, and the resume arms that read a delivery
  of `&unbound` as "nothing delivered yet" break when it is violated. Since
  05C the identities are *integers* (the type Emacs gives them), the
  accumulator is either-type, and each delivery combines through the tower's
  single promotion rule (`GetNumericPair`/`CombineNumeric`, see "The numeric
  tower" above) after the first operand seeds it.
- `FeFrameEvalList` (`list`/`=`, 05C's chained comparators
  `<`/`<=`/`>`/`>=` and the strictly-binary `/=`, `set`, and 04C's
  `funcall`/`apply`) is the
  one kind that *does* evaluate
  its whole raw argument list first, exactly as the old `EvaluateList`-based
  arms did, before validating or dispatching on any of it — which is what
  makes `funcall`/`apply`'s evaluate-then-redispatch shape fit it with no new
  frame kind (see "Sub-plan 04C" above). The chained comparators and `=`
  compare adjacent pairs left to right and stop at the first false pair, and
  a single operand is `t` without any type check;
  `/=`'s binary arity is rejected at dispatch before this frame is even
  created.
- `FeFrameCatch` (`(catch TAG BODY...)`, sub-plan 06C) is the one frame kind
  that does not just *wait* for a delivery -- it is also the destination of
  a mid-stack unwind. Its `accumulator` holds the evaluated tag (`&unbound`
  until the tag sub-expression delivers), `rest` the raw BODY forms, and
  its `gc_checkpoint`/`cleanup_checkpoint` are exactly what a `throw` that
  matches it restores to: cleanups above it drain through the same
  `RunCleanupsDownTo` a completing pair frame uses, the GC stack restores to
  its checkpoint (with the delivered value re-pushed across the restore),
  every frame above it is discarded, and the value lands in its `callee` for
  the loop to resume. The body region is one implicit-body frame
  (`PushBodyFrame`, the sub-plan's "one expression frame" accounting), so
  the kind adds no new resume logic for the body itself. `throw` is a
  function-shaped primitive that reuses `FeFrameEvalList`'s evaluate-all
  machinery for its two operands and then performs the unwind
  (`PerformThrow`); it gets no frame kind of its own, per the sub-plan's
  one-new-kind rule.

`PushBodyFrame` pushes a sequential-body frame directly, bypassing the call
path, for the places a raw form list is evaluated as an implicit body with
no call involved: `if`'s false branch when it has more than one form,
`while`'s body each pass, and `do`. It reuses `FeFrameBody`'s own resume
logic (`ResumeBody`) but is its own kind, `FeFrameImplicitBody`, because it
completes through a lighter path, `CompleteImplicitBodyFrame`, that does not
touch `call_list`: a lambda-body frame's push already went through the
trace-cell link when it was still the call form's own `FeFrameExpression`,
but this frame's push has no such entry to balance.

Conflating the two here was a real bug during 03E's development, and the
lesson outlived the counter it was found on. Back when a transitional
`evaluation_depth` counter still existed beside the frame stack, routing
this frame through `CompletePairFrame` decremented that counter once too
many times per level, unsigned-wrapping it to a huge value and turning the
very next pair-form entry into a false depth error. 03F deleted the
counter, so that exact failure can no longer happen -- but the rule
generalizes to every piece of per-form bookkeeping `CompletePairFrame`
still owns (`call_list`, the GC and cleanup checkpoints): a new frame kind
pushed by something other than `FeFrameExpression`'s own pair-form dispatch
must not route through `CompletePairFrame`, or its unmatched restore
corrupts whatever that path balances.

`if`'s false branch is special-cased when it has exactly one form -- the
common shape, and the one the canonical `(deep N)` chain uses -- to push
that form directly (`bind = &frame->env`, exactly as the recursive
`DoList`'s own `&env` out-parameter threaded even for a lone body form)
rather than through `PushBodyFrame`. This is not an optimization for its
own sake: 03A/03C/03D's frame-storage Decision derives kg's 1 MiB arena's
1100-frame capacity from *3* simultaneously-open frames per `(deep N)`
level (the call, `if`, and the arithmetic form waiting on its second
operand). A whole extra retained `FeFrameImplicitBody` per level for a
single form makes that 4 -- a 33% cut in every host's usable recursion
depth for no behavioural gain. It was found as a regression rather than
reasoned about in advance: without the special case, `(dc 300)` in kg's own
`test/test_perf.c` failed at N ~ 274 on the ordinary 1 MiB arena.

Since 03F this arithmetic is the *whole* bound, not half of it. While the
transitional logical counter still existed, its 1000-unit default fired
first for kg's arena and partly masked the frames-per-level cost; deleting
it (see "two independent, permanent bounds" above) leaves `frame_capacity`
alone deciding how deep a host can recurse, so frames per level now
converts directly and solely into usable depth. Any future change that
retains an extra frame per level of ordinary recursion is a proportional
cut in every embedding's recursion depth, and should be measured as one.

`unwind-protect`'s cleanup forms run through a second entry point,
`RunEvaluationBody`, sharing `RunEvaluation`'s own barrier/`setjmp`
machinery and the same `RunEvaluationLoop` (factored out so there is exactly
one loop implementing evaluation, not two that could drift) but pushing a
body frame instead of an expression frame as its base. `RunOneCleanupEntry`
calls it once per cleanup entry in place of the old recursive `DoList`'s one
nested `Evaluate()` call per form -- fewer nested barriers for a
multi-form cleanup, same `let`-threads-through-forms semantics. It is a
nested run on the unused suffix of the *same* frame stack, above a saved
barrier, per 03C's decision: not a second stack, and not a second
evaluator. A cleanup's own error still bypasses this nested barrier
entirely, via `cleanup_catch`'s direct `longjmp`, exactly as it bypassed the
old recursive `DoList`'s implicit one; `RunOneCleanupEntry`'s own restores
(frame index, `evaluator_catch`, `native_reentry_depth`, `call_list`) are
what put the context back together afterward, unchanged by this slice.

Embedded frame trace cells preserve the host error callback's semantic
call trace without allocating after an error. Frame exhaustion raises the
final, permanent `evaluation frame limit exceeded` text (03F); nothing
transitional remains at this boundary.

The GC-stack retention this section used to describe -- the lambda-body
wrapper's `FePushGC(env)`/`FePushGC(rest)` pair, left live per still-open
level until the whole activation unwound, which made `GcStackSize` (4096) a
second, previously non-binding recursion bound once `max_frames` was raised
far enough past its default (`(deep 1021)` succeeded, `(deep 1022)` raised
`GC stack overflow`) -- is gone. An intermediate frame's result is delivered
straight into the frame below's `callee`, already a mark-phase root
(`FeMarkEvaluatorRoots()`), with no allocation in between; only the run's
own final result needs one `FePushGC()`, at the barrier, not per level. See
`fe/test_api.c`'s `TestGcStackConstantInNesting`, which pins
`peak_gc_stack_depth` as a small *constant* across `(deep 200)` and
`(deep 2000)` on the same context rather than a threshold, and the permanent
flatness gate in `TestEvaluationStackProbe`, which now runs `(deep 100000)`
itself -- on a dynamically sized arena, since neither `./fe -s` nor kg's
1 MiB arena needs to or can hold that many frames -- and asserts a flat
C-stack high-water mark across `(deep 10)`, `(deep 1000)` and
`(deep 100000)`.

The context's three result/retention roots have separate lifetimes.
`evaluation_result` holds the latest string or file evaluation result,
`call_result` holds the latest `FeCall()` result, and `root_list` links explicit
persistent roots. The collector marks all three. Each persistent `FeRoot` is a
pair whose `car` is the retained value and whose `cdr` is the next root, so the
bookkeeping and retained values stay inside the arena. Releasing a root unlinks
its pair; a later collection can reclaim it. Release checks that the handle
names an active node in the same context, so foreign and already-released
handles raise `root is not active`.

Multi-form string and file evaluation uses `evaluation_result` for its final
value. Each helper restores its entry GC-stack index between forms and before
returning, then keeps the returned value alive through this root. The next
multi-form evaluation replaces it. Length-aware input is fed through the
existing callback reader; its adapter treats the supplied length as the only
end-of-input marker and raises an error when a NUL byte occurs inside that
range. The adapter updates a zero-based byte offset before each read, allowing
reader errors to carry `<label>:<offset>` while evaluator errors carry the
source label without a reader offset.

`FeCall()` temporarily protects the callable and all host-supplied arguments,
then builds an ordinary internal call form whose arguments are individually
quoted. The normal evaluator therefore receives the supplied values without
re-evaluating list values as forms, while retaining its function/native
dispatch, call trace, error behavior, and ambient step accounting. The context
has a separate `call_result` GC root. A successful call assigns its result to
that root before restoring the entry GC-stack index, keeping normal returns
stack-balanced. The next call replaces this root.

## Error Handling

If an error occurs, `FeHandleError()` detaches the active call trace and invokes
the configured error callback. The borrowed error message and trace are valid
only for that invocation. A recovering callback must `longjmp` or perform an
equivalent nonlocal transfer; it must not allocate Fe objects. If the callback
is absent or returns, Fe silently calls `abort()`. Error reporting and process
exit policy belong to the host. The callback can distinguish the failure's
kind with `FeGetCompletion()` -- ordinary Error, Quit for the interrupt path,
Budget for the step/frame/re-entry ceilings -- valid for the duration of the
call and after recovery (sub-plan 06B; see "Unwinding And Cleanup").

String and file evaluation temporarily install a borrowed source label and
reader byte offset in the context. `FeHandleError()` copies those values into a
labelled diagnostic and clears the temporary input state before invoking the
callback. Successful nested evaluation restores the enclosing label state;
an escaping error clears it because the active evaluation chain is abandoned.

The host must save and restore its GC stack checkpoint around a recoverable
operation. Error handling resets the evaluator's call-trace link, but cannot
unwind host resources, the GC stack, or input callback state. Once the host has
restored those invariants, the context remains usable.

The default error policy is a silent `abort()`: it is used when no error
callback is installed and when an installed callback returns. The core never
prints or exits on runtime failure. A recovering host callback must copy any
needed diagnostic and perform a nonlocal transfer; the message and trace are
borrowed only for the callback invocation.

Native callbacks may synchronously re-enter evaluation on the same context.
The evaluator's call trace and GC stack support nesting, but a context has no
internal synchronization and must be used by only one thread at a time.

## Evaluation Control

The context stores one ambient evaluation-control record. Only the outermost
controlled API call initializes and owns that record. Plain evaluator calls
and nested controlled calls see it already active, so all re-entry consumes
the outer record and nested options cannot replace its limit, interrupt
callback, polling interval, or userdata. The owning call clears the record on
normal return.

An evaluator step is charged on every `Evaluate()` entry, on each element of
the evaluator's argument and body list loops, on lexical environment and
parameter-binding traversal, and on each successful `while` iteration.
Macro-generated code re-enters `Evaluate()` and is charged there. Finite
budgets store a remaining-step count. Interrupt polling stores a countdown that
is reset before invoking the callback, making the common path a counter
decrement with no clock access and allowing callback re-entry to keep using the
same polling state.

Budget exhaustion and interrupt cancellation both enter `FeHandleError()`.
Along with clearing the call trace and temporary source label, that function
clears the ambient control record -- `step_limit`, `interrupt`,
`max_frames_limit`, `native_reentry_limit`, and the rest -- before invoking
the host error callback. Consequently a nonlocal transfer cannot leave a
stale *budget* active in a recovered context. `native_reentry_depth` is the
one deliberate exception: it is a live count of C activations, not a
configured limit, and those activations are still real while cleanups drain
before the `longjmp` actually happens -- see "Native re-entry" in "Garbage
Collection" above.

## Unwinding And Cleanup

`doc/unwind-design.md` is the design this section's implementation follows;
it also records which parts of that design are shipped and which are not. A
cancellation token for the cleanup registry (so a host can retire an entry
before its form completes) is the remaining piece of future work there.
Conditions are `(SYMBOL . DATA)` objects built at raise time against a
static hierarchy: `condition-case` validates its clauses before its
body runs, then unwinds to the first matching clause and evaluates its body in
an environment optionally binding the condition. `error` is the parent of the
registered ordinary error symbols; `quit` is separate and only matches `quit`
or `t`; budget completion is never catchable.
`catch`/`throw` are no longer future work: since sub-plan 06C the
distinct completion kinds include the interrupt path assigning
`FeCompletionQuit`, the step-limit/frame/re-entry walls assigning
`FeCompletionBudget`, every ordinary `FeHandleError()` assigning
`FeCompletionError`, and a matching `throw` holding `FeCompletionThrow`
for the duration of its checkpointed drain -- all readable through
`FeGetCompletion()`/`FeGetCondition()` (the condition is nil only when it
cannot be constructed). They are a parallel host channel; Lisp observes
catchable errors through `condition-case`, while budget remains uncatchable.

Lisp `unwind-protect` and the host's `FeProtectWithCleanup()` share one
registry, `FeContext.cleanup_stack`: a fixed-size array of entries, each
either a C function pointer and `void*` or a pair of Fe objects (the unwind
forms and the environment to evaluate them in). One registry, rather than
one per kind, is what gives host and Lisp cleanups a single interleaved
last-in-first-out order when they nest.

`Evaluate()` saves `cleanup_stack_index` at entry, the same way it saves the
GC stack index, and on an ordinary return drains the registry back down to
that checkpoint: an entry pushed while evaluating one call form always runs
by the time that call form's own `Evaluate()` frame returns, whether the
push came from the `unwind-protect` primitive registering its own unwind
forms or from a native reaching into the C API. `unwind-protect` itself
does nothing more than push its entry and evaluate its body; it never runs
its own cleanup directly, because the enclosing `Evaluate()` call always
does.

`FeHandleError()` is the other path an entry can be run from, and the only
one for the abnormal exits (error, interrupt, budget exhaustion): since it
does not return to any of the C frames between the raise and itself, no
frame's own "drain to my checkpoint" tail ever executes for those, so
`FeHandleError()` drains the entire registry down to zero, unconditionally,
before invoking `error_fn`. It does this after clearing the ambient
evaluation-control record (see below), so a cleanup's own evaluation is
never charged against a budget or interrupt schedule that already ran out
-- the reason a body that exhausted its step budget still gets a working
cleanup. It also does this before `error_fn` can `longjmp` the host away, so
every cleanup still sees the GC stack exactly as populated as it was when
the error was raised (see "Garbage Collection" above and "Error Handling"
below).

A cleanup that itself raises is the one case the raise path treats
differently: `cleanup_catch`, a `jmp_buf*` naming the local `setjmp()` a
helper (`RunOneCleanupEntry()`) installed around that one entry's execution,
is non-null exactly while that entry is running. `RaiseCompletionCore()`
checks it first, before anything else, and when it is set, resumes there
directly instead of reaching `error_fn` -- reaching a callback contracted to
never return would abandon every cleanup entry still below this one. The
message is copied into context-owned storage first, since the frame that
formatted it is what is about to be unwound past, and the *kind* is copied
with it, so a cleanup that runs out of its own bounded budget or answers a
second host interrupt does not arrive relabelled as an ordinary error.

Once control resumes at that `setjmp()`, `RunOneCleanupEntry()` restores
the enclosing run's frame stack, floor, barriers and evaluation-control
record, and then *replays* the completion there. That is 06A Decision 4 and
Emacs' measured policy: the cleanup's own completion replaces whatever was
already unwinding, and because the replay happens in the enclosing context
it is an ordinary raise -- an enclosing `condition-case` can catch it. The
replay goes to `RaiseCompletionCore()` rather than to `RaiseCompletion()`,
which is the half that applies the `error_label` prefix, so the source label
already in the saved text is not applied a second time.

A cleanup's `throw` takes the same route for the same reason. Its catch
frame is below the cleanup run's floor, so the search inside the cleanup
finds nothing; instead of raising `no-catch` there, `PerformThrow()` parks
the tag and value (both GC roots) and jumps to `cleanup_catch`, and
`RunOneCleanupEntry()` re-issues the throw in the enclosing context, where
the catch frames are. Delivering it then abandons the drain -- and whatever
raise or completing form started it -- with a `longjmp` to that run's
`condition_catch`.

`RaiseCompletionCore()` deliberately does *not* clear the ambient
evaluation-control record on the way in, because its three exits need three
different answers and each one is stated at the exit that takes it. A caught
condition resumes the interrupted program and keeps the record exactly as
the body left it, remaining steps included -- clearing it there is what once
let a single caught condition disarm the step limit, the frame wall, the
re-entry ceiling and the host's interrupt for the rest of the run. A cleanup
drain re-arms its own fresh bounded budget per entry inside
`RunOneCleanupEntry()` and puts the ambient record back on both exits. Only
the host exit clears it, and only the host exit drops `error_label`, so a
stale source name cannot prefix a later unrelated raise.

`catch`/`throw` (sub-plan 06C) are the first non-local exit that stops
*partway* down the frame stack, and they are built on the checkpointed half
of this machinery. `catch` is a special form whose frame (`FeFrameCatch`)
carries the GC and cleanup checkpoints `RunEvaluation()` saves for every
pair form; `throw` is a function-shaped primitive whose two operands
evaluate through `FeFrameEvalList`. When both operands are in, `PerformThrow`
searches the frame stack from the top down to the current run's floor
(`ctx->run_base`, the frame-stack index the innermost
`RunEvaluation`/`RunEvaluationBody` started from) for the innermost
`FeFrameCatch` whose tag is `eq` to the throw's -- a search that deliberately
stops at that floor, so a catch below a nested run's base (the abandoned
body of an outer run, or an outer run itself when a native re-entered) is
never matched: the C activations between the runs are live and cannot be
popped by frame-index assignment. The native re-entry boundary is therefore
a wall, tested and recorded as a divergence. No matching catch raises
`no-catch TAG VALUE` through the ordinary error path; an enclosing
`condition-case` may catch it before cleanups drain to zero. A matching catch
is delivered the value: the completion is
`FeCompletionThrow` for the duration, `RunCleanupsDownTo(ctx,
catch->cleanup_checkpoint)` runs the cleanups the throw passes (innermost
first), the GC stack restores to the catch frame's checkpoint with the
delivered value re-pushed across it, every frame above the catch is
discarded, and the value lands in the catch frame's `callee` for the loop
to resume -- the "drain to the checkpoint of the frame that catches" the
design document names. `FeHandleError()`'s drain-to-zero is unchanged: an
*error* still goes to the host; only the throw path uses the checkpointed
drain.

## Known Issues

The implementation has some known issues. These exist as a side effect of trying
to keep the implementation concise, but should not hinder normal usage.

* The garbage collector recurses on the `car` of objects; thus, deeply nested
  `car`s may overflow the C stack. An object’s `cdr` is looped on and will not
  overflow the stack. `FeMark()` also has no cycle detection of its own; it
  relies on the mark bit, which the writer has no equivalent of. The writer
  used to share the recursion and shares it no longer: `WriteObject()` bounds
  `car` nesting with an explicit depth budget and walks the `cdr` spine with
  two pointers, so the two are no longer the same shape and should not be
  changed as if they were.
* The storage of an object’s type and GC mark assumes a little-endian system and
  will not work correctly on systems of other endianness.
* Proper tailcalls are not implemented — `while` can be used for iterating over
  lists.
* Strings are `NUL`-terminated and therefore not binary-safe.
