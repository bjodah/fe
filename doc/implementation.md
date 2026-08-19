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
pair holding the symbol's name-and-plist pair and its function cell, and the
`cdr` part of the outer pair contains the globally bound value for the symbol:
`CDR(sym) = (((name . plist) . function) . value)` (sub-plan 04B of kg's
Emacs-subset program, whose name slot Phase 14 widened into a pair). The
property list is nil until a `put` writes one; it lives in the symbol object
rather than in a context-side registry -- the shape the special-variable list
uses -- because Phase 14's uninterned symbols are the first symbols the
collector may reclaim, and a registry keyed by symbol would pin every symbol
that ever carried a property. The function cell starts out holding `unbound`; sub-plan 04C made
it live, not dormant: call position, `funcall`/`apply`, and `FeGetFunction`
all read it through one shared resolver (`ResolveFunctionCallable`, below),
and only the `fe_internal.h` accessors `SymbolFunction`/`SetSymbolFunction`
spell the cell. Every reader of this private
layout goes through the named accessors (`SymbolName`, `SymbolBindingCell`,
`SymbolFunction`) rather than spelling the pair walk itself, so the later
Phase-4 lookup slices can change resolution without touching the
representation readers.

Symbols are interned by default; `make-symbol` and `gensym` (Phase 14) build
one that is on no list at all, which is what makes it collectable and what
`intern-soft` answers nil for.

The value cell of a newly interned symbol whose name begins with `:` points
back to the symbol itself, making keywords self-evaluating without an
evaluator special case. That includes `:` on its own, which Emacs also treats
as a keyword. `t` is initialized the same way. `IsConstantSymbol` is the
single name-property test used by value and function mutation paths;
`setting-constant` carries a one-element data list with the rejected object.
Lambda parameter construction is the deliberate exception for `t` alone.
Ordinary `let` binding-list construction validates every target before
evaluating any initializer, and because it compiles the list into a lambda
application, it also refuses `&optional` and `&rest` as binding names -- the
parameter decoder would otherwise read them as lambda-list keywords.

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
`cdr` is the pair `(((name . plist) . function) . value)`, whose `cdr` -- the
value cell -- is the same shape as a lexical binding, so `GetBound`'s global
path returns that cell (`SymbolBindingCell`) and one lookup returns the cell
either way. The `((name . plist) . function)` inner structure is private to
the symbol accessors; the value path's unit of currency is the binding cell,
so there is deliberately no value-shaped accessor.

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

### Special variables and shallow dynamic binding

A symbol's *metadata* -- as opposed to its name, function cell and value
cell, all of which live in the symbol object above -- is one registry on the
context: `ctx->special_list`, a list of `(SYMBOL FULL-P . SCOPE)` triples
with one entry per marked symbol, marked by `CollectGarbage` as a root in
its own right. `MarkSpecialSymbol`, `SymbolIsSpecial` and
`SymbolIsLetDynamic` (fe.c, beside the symbol accessors) are its whole
interface.

`SCOPE` is sub-plan 12C's addition and is what scopes a let-dynamic-only
mark -- Emacs' one-argument `(defvar v)` -- to the file it appears in. It is
nil for a mark that is global (every full mark, and any mark made outside an
input unit) and otherwise an integer naming an input unit: `ctx->input_scope`,
which `EvaluateInput` takes from a monotone counter on the way in and hands
back on the way out, so nested units stack and an outer unit's marks come
back when a nested one returns. `SymbolIsLetDynamic` compares it for
equality, not for ordering -- an ordering test would let an outer unit's
mark reach a unit it loads, which Emacs measurably does not. The abnormal
exit is `FeTryEvaluateStringWithOptions`/`FeTryCallWithOptions`' job: they
save and restore `input_scope` with the rest of the contained state, because
`EvaluateInput` restores it only on its normal return, and a contained
failure is exactly how an outer unit keeps evaluating after an inner one
raised.

A triple, not a longer list or a second registry, because the slot is two
ordinary conses and an ordinary integer: `CollectGarbage`'s existing walk
needs no new shape for it, which was the constraint. One entry per symbol is
kept, so re-marking a symbol that is already let-dynamic-only re-stamps its
scope rather than adding an entry -- last mark wins, and the registry stays
bounded by the number of marked symbols instead of growing every time a file
that declares a name is loaded again.

It is a list, and not a bit in the symbol object, for two reasons. A
symbol's `car` word is a tag whose spare bits the collector's pointer
reversal already owns (see `GcMarkCdrBit`), so there is no free bit to take;
and a list costs two cells per *marked symbol* rather than anything at all
per binding, which is the cost that would be on the hot path. Membership is
one linear scan, the same shape `FeMakeSymbol`'s interning and `GetBound`'s
environment walk already are.

Binding a marked symbol is *shallow*: `PushDynamicBinding` (fe_unwind.c) saves
the global value cell's current contents -- which may be the `unbound`
object itself, the whole of row A10a -- as an `FeCleanupBinding` entry on the
cleanup registry, then writes the new value into that same cell. The restore
is `RestoreDynamicBinding`, two stores that cannot raise and evaluate
nothing, which is why `RunCleanups` performs it inline rather than through
`RunOneCleanupEntry`'s barrier and control-record save/restore.

Where the restore *goes* is the host's to override since FE_API_VERSION 11.
`FeSetBindingFns` installs two callbacks: one asked for an opaque
`uintptr_t` token as the binding is pushed (before the cell is read), one
asked at the restore for the symbol whose value cell receives the saved
value, or `nullptr` to drop it. Fe stores the token in the
`FeCleanupBinding` entry beside the saved value and never interprets it --
it is a number, not an `FeObject*`, and is not marked. With
neither callback installed, which is every host but kg, both call sites
answer exactly what they answered before: no tag, and the bound symbol's own
cell. The seam exists because kg's buffer-local bindings *move* a variable's
value between cells as the current buffer changes, so the cell a `let`
displaced may not be the cell that is there when the form exits, and may
have been destroyed with its buffer -- the case in which the saved value is
dropped, which is Emacs' own answer for it.

Putting the obligation on the cleanup registry rather than in a registry of
its own is the design decision worth stating: the property a dynamic binding
needs is exactly the one every cleanup entry already has -- it must be
honoured on all five completion kinds -- and the drains that do that
(`CompletePairFrame`, `CompleteImplicitBodyFrame`, `RaiseCompletionCore`'s
handler and host drains, `PerformThrow`'s unwind) are already written
against this stack and this stack only. `MarkCleanupRoots` marks the saved
value, which while the binding is in force is reachable from nowhere else.

Two frame kinds consult the flag and nothing else does. `StartBindingLet`
asks `BindingsHaveDynamic` once, of a whole binding list: with no marked
target the list compiles into the lambda application it always did, and with
one the frame becomes `FeFrameDynamicLet`, which evaluates every value form
in the entry environment and then binds each target by its own kind
(`InstallLetBindings`) before becoming an ordinary body frame. `ResumeLet`,
the two-argument `(let SYM VALUE)` path, binds shallowly for a marked target
and raises its own frame's `cleanup_checkpoint` past the new entry, because
that binding's scope is the enclosing body rather than its own little form.

`ArgsToEnv` -- every closure, `fn` and macro parameter binding -- never asks.
That is not an oversight but the guard: a defun parameter named after a
special variable is bound *lexically* in Emacs 31.0.90 under
`lexical-binding: t` (measured interpreted, byte-compiled, with a user
`defvar` and with a core variable), and an implementation that bound
specials dynamically at every parameter-binding site would close one
divergence and open another.

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

Sub-plan 11C added the sibling that had been missing since: `(quote X)`
prints as `'X`, under the same single-element-proper-form rule, and the two
share one helper (`EmitAbbreviation`, fe.c) rather than two copies of the
same shape test. Both recurse through `WriteObject`, which is what makes
`''x` and `(a 'b c)` come out as Emacs prints them, and both spend `depth`
exactly as the pair arm they replace would, so an abbreviation cannot buy a
level of nesting the bounded writer would otherwise have refused.
Backquote is deliberately not abbreviated: kg's reader expands to the
ordinary symbols `quasiquote`/`unquote`/`unquote-splicing` where Emacs uses
`` \` ``/`\,`/`\,@`, and Emacs' comma abbreviation is context-sensitive,
so closing that half means changing what the reader produces and breaking
any Lisp that pattern-matches on those names.

## Garbage Collection

Fe uses a simple mark-and-sweep garbage collector in conjunction with a
freelist. `FeOpenContext` initializes the context and creates a freelist
containing all the objects. When an object is required it is popped from the
freelist. If there are no more objects on the freelist, the garbage collector
does a full mark-and-sweep run, pushing unreachable objects back to the
freelist. Thus, garbage collection may occur whenever a new object is created.

`FE_GC_STRESS`, a build-time knob in `fe.c` that defaults to 0 and compiles
to nothing there, makes `MakeObject()` collect before *every* allocation
instead of only when the freelist is empty. It exists because "collection may
occur whenever a new object is created" is a contract the ordinary suite
never tests at its worst case: an object live only through an unrooted C
local survives an ordinary run whenever nothing happens to collect between
its creation and its last use, and the same luck hides a use-after-free until
some unrelated change reduces churn. A stress build turns both into a
first-run failure. The knob does not replace the freelist-empty test -- that
is still what decides exhaustion, so `out of memory` is raised at the same
point in both builds -- and the cost is a full mark-and-sweep per `cons`,
which is why it is never on by default. `gc_stress.c` is the two-build
harness: `make check-gc-stress` builds it with the knob off and on, and both
runs assert that `FeGetArenaStats().collection_count` moved off zero over a
churning script whose answer is still correct.

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

It does not grow with a call's *argument count* either, which is the same
property one width down: an argument frame's accumulator, an EvalList frame's
accumulator, and `apply`'s rebuilt operand list are all frame fields and
therefore mark-phase roots, so each pass restores the frame's own checkpoint
instead of leaving a slot per operand behind. `TestLongArgumentLists` pins
that as a constant across 1500- and 3000-argument calls through `&rest`, the
dotted tail and `apply`.

Overflowing it is reported without allocating. An ordinary push stops
`GcStackReserve` (64) slots short of the array and only a completion already
in flight may spend the rest -- `AllocateFrame`'s `CleanupFrameReserve`,
applied to the other bounded stack -- because reporting the overflow is
itself a rooting operation: the raise protects the condition object across
the cleanup drain, and a cleanup entry evaluates Lisp of its own. The raise
carries `(evaluation-stack-exhaustion)`, one of the two condition objects
`FeOpenContext()` builds once and roots for the context's life (below),
rather than building one here: building one would allocate, which would
push, which is what overflowed.

"Arena exhausted", where a raise cannot build a condition object, means
`ArenaCanAllocate()`: the free list is empty *and* a collection cannot refill
it. A bare free-list test answers a narrower question -- whether the list
happens to be empty at this instant, which is true after any allocation that
took the last cell -- and reading that as exhaustion silently replaced the
condition object with nil, which matches no handler, so a `condition-case`
could be escaped by its own body under memory pressure.

That last sentence was the *whole* story until sub-plan 09B, for genuine
exhaustion as well as for the boundary case: a raise that could not allocate
set `ctx->condition` to nil, and `ConditionMatches()` -- which has to walk a
pair to reach the hierarchy -- then answered false for every named handler,
so `(condition-case e BIG (error ...))` did not catch an out-of-memory and
only `(t ...)` did. `FeOpenContext()` now interns and conses two condition
objects before any host code runs, and `CollectGarbage()` marks both as roots
in their own right for the context's whole life, so signalling either
allocates nothing:

- `ctx->arena_exhaustion_condition` is `(arena-exhaustion)`. `FeHandleError()`
  signals it when `ArenaCanAllocate()` is false, and `RaiseCondition()` falls
  back to it when a *named* Error condition cannot be built for the same
  reason -- the handler learns that the raise became an exhaustion rather
  than learning nothing. The message text is the raise's own either way.
- `ctx->evaluation_stack_exhaustion_condition` is
  `(evaluation-stack-exhaustion)`, signalled by `RaiseGcStackOverflow()`.

Both names are already rows of the static hierarchy with `error` as their
parent, so `(error ...)` catches either and each is catchable by name.
`RaiseCondition()`'s fallback is deliberately restricted to
`FeCompletionError`: a quit raised from an exhausted arena keeps the nil
object, because `ConditionMatches()` decides a quit by completion kind before
it looks at the object's shape and an interrupt is not an exhaustion.
`GetCoreObjectCount()` counts both names and both pairs, so
`FeMinimumArenaSize()` still describes an arena that can open.

`FeOpenContext()` builds one more thing before any host code runs, for the
same reason: `SeedConditionMessages()` writes Emacs' `error-message` property
onto every symbol in the static hierarchy, so the sentence
`error-message-string` renders for a condition does not depend on there being
arena left at the time of the raise. The property lives on the symbol rather
than in the C table the renderer could have read directly because it is data
a program can replace with `put`, and because `(get 'wrong-type-argument
'error-message)` answers on Emacs. `GetConditionMessageObjectCount()` counts
what the seeding allocates -- the shared property symbol, and per row a
message string, two plist pairs, and the condition symbol unless the
primitive/alias/maths tables already interned it. That last exclusion is
`IsCoreSymbolName()`, and it exists because `FeMinimumArenaSize()` is EXACT
rather than an upper bound: `test_api.c` opens a context at exactly that size
and asserts `free_slots == 0`, so counting `error` twice would fail the
assertion as surely as not counting it at all.

Both objects are shared, and a handler receives the object itself, so
`setcar`/`setcdr` on a caught condition would otherwise change what every
later exhaustion signals -- for the rest of the context's life, since these
are context-lifetime roots. `PublishExhaustion()` re-stamps the pair from
`arena_exhaustion_name` / `evaluation_stack_exhaustion_name` before every
publish: `car` back to the symbol, `cdr` back to nil. The names are held
separately for the obvious reason that the pair's own `car` is exactly what a
poisoning handler overwrote. Two stores, no allocation, so the property these
objects exist for -- raiseable from a state where nothing can be allocated --
is unchanged. Measured before this: `(setcar e 'poisoned)` in one handler
left the next out-of-memory escaping `(error ...)` and `(arena-exhaustion
...)` alike, and `(setcdr e (list 9 9 9))` made the next one signal
`(arena-exhaustion 9 9 9)`. `scripts/exhaustion.fe` pins both.

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

`macroexpand-1` and `macroexpand` reuse exactly that machinery rather than
a second transformer-application path: `EnterMacroBody` is the only place a
transformer is ever applied, and it takes the macro call form and the
identity a `wrong-number-of-arguments` should name as parameters, so an
ordinary call passes `frame->expr` and a reflective expansion passes its
evaluated FORM operand. The one field that differs is `frame->fn`, which an
ordinary macro call fills with the caller environment (the environment the
expansion is evaluated in) and a reflective expansion fills with the
resolved `macroexpand-1`/`macroexpand` primitive. `ResumeMacroBody` reads
its type: a primitive there means "the expansion is this frame's value,
never evaluate it", and the `macroexpand` tag additionally means "and take
another step if it is still a macro call". A `defalias` link is a step of
its own -- Emacs stops at each indirection -- taken in a loop rather than
through the frame stack, since substituting a head symbol evaluates nothing.
The link is only taken when the target resolves to a macro, which is Emacs'
rule and also what bounds the loop: resolving the chain raises
`cyclic-function-indirection` on a ring, so the only shape that can run
without a fixpoint is a transformer expanding to a call to itself, and that
charges its body's evaluation steps.
The fixpoint reuses the one frame, so its cost in frames is constant no
matter how many expansions it takes, and the step budget is what bounds it.
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
continuation push, so fixed-arity failures do not evaluate an operand. That
preflight runs exactly once per call, at `DispatchResolvedCall` -- it has to
be there, because `quote`'s fast path is taken before `DispatchPrimitive` is
reached -- and the table has no exception rows: every primitive raises
`wrong-number-of-arguments` with `(FUNCTION NARGS)`, which is also what Emacs
answers for each of them. Host natives are the deliberate exception class,
and for a structural reason rather than a historical one: a native declares
no arity, so `FeGetNextArgument`/`FeRequireNoArguments` can only check after
operands have been evaluated, and they keep their own message text.

Lambda and macro operands are evaluated completely before `ArgsToEnv`
validates and binds the parameter list. Both paths use the same condition
builder, rooting the original designator/callable and original argument count
while constructing `(FUNCTION NARGS)` data. An improper argument list is not
routed through any of this: it has no argument count, so `(car 1 . 2)` is
`wrong-type-argument listp` naming the tail, as in Emacs.

A rest parameter binds the argument list itself, never a copy. For an
ordinary call that list is the evaluated-operand list the argument frame just
consed, so it is fresh per call without copying; for a macro it is the
caller's raw source tail, which is what Emacs binds too and what makes a
macro able to rewrite the form it was given.

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
evaluator. A cleanup's own error bypasses this nested barrier via
`cleanup_catch`'s direct `longjmp`, exactly as it bypassed the old recursive
`DoList`'s implicit one -- but only when nothing the cleanup itself
established can handle it, which is sub-plan 12B Part 1's qualification and
is stated in full further down, at `cleanup_frame_floor`. A handler the
cleanup's own forms established is found first and takes the raise, and no
`longjmp` past this barrier happens at all; `RunOneCleanupEntry`'s own restores
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
multi-form evaluation replaces it. `FeTryEvaluateStringWithOptions()` runs a
string evaluation inside a containment barrier and hands its value back
through an out-parameter, so it depends on the same root and must leave the
field alone: it restores the enclosing run's frame stack, floors, barriers
and GC stack, but restoring this field too would leave the value it just
returned with no root at all. Length-aware input is fed through the
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

The reader has one strict escape decoder shared by string bodies and character
literals. It returns a character value, and the caller decides what a value
means: a `?` literal takes any of them, a string body is a byte string and
rejects 0 and anything above 255 rather than writing it into a
NUL-terminated buffer. `\x` consumes hex digits greedily to a U+10FFFF bound,
as Emacs does.

Character literals decode UTF-8 directly from the byte callback; the lead byte
determines the fixed number of continuation bytes, so no public callback or
pushback API is needed. The `?` grammar is two mutually recursive positions --
escape position after a `\`, character position after a modifier -- which is
what lets `?\C-\n` apply the modifier to a nested escape's value while
`?\C-s` applies it to the letter `s`. Recursion depth is bounded at three by
the duplicate-modifier rejection. A literal must end at a delimiter, so
`?ab` and `?\s-a` are read errors rather than a character followed by a
leftover token; that check is the one place the reader looks one byte ahead
and pushes it back.

The reader's whitespace is one definition, `ReaderWhitespace`, pasted into
the three delimiter sets beside the skip loop that spells it: space, form
feed, newline, tab and carriage return, which is byte for byte what Emacs'
`read1` retries on. One definition rather than four literals because the
question is asked four times -- skip a byte, end an atom, end a `?` literal,
end a radix literal's digits -- and a byte added to one of them and forgotten
in the others is exactly the bug the form feed was: whitespace that did not
terminate a symbol is not whitespace. Comments are not part of it and end at
a newline only, as they do in Emacs.

Evaluated input maintains a one-based line counter in its string/file adapter
and records the line before each top-level form is evaluated. `Read` skips
whitespace and comments in one loop *before* latching that line: the latch
keeps the first value it is given, so recording it ahead of the comment arm
reported the leading `;`'s line for every form a comment block preceded. The
source label and that line survive into both read and runtime diagnostics;
standalone byte-oriented reads keep their byte offset.

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
is non-null exactly while that entry is running. When it is set and nothing
the entry itself established can handle the raise, `RaiseCompletionCore()`
resumes there directly instead of reaching `error_fn` -- reaching a callback
contracted to never return would abandon every cleanup entry still below
this one. The message is copied into context-owned storage first, since the
frame that formatted it is what is about to be unwound past, and the *kind*
is copied with it, so a cleanup that runs out of its own bounded budget or
answers a second host interrupt does not arrive relabelled as an ordinary
error.

"Nothing the entry itself established" is the qualification sub-plan 12B
Part 1 added, and it is decided by a second field, `cleanup_frame_floor`:
the frame-stack index the running entry started at, zero when no entry is
running. The handler search runs *before* the `cleanup_catch` bounce and a
handler is accepted only at or above that floor. Below it are the frames of
the computation the drain is abandoning, and taking one of those directly
would skip the rest of the registry.

The two used to be the other way round -- `cleanup_catch` first,
unconditionally -- so while any cleanup entry ran, *every* raise bounced and
the cleanup's own handler frames were never examined. The visible effect was
not limited to a drain: `(unwind-protect 'body (condition-case nil (car 6)
(error nil)))` escaped to the host, where Emacs 31.0.90 answers `body`.

`run_base` cannot serve as that floor even though it looks like it should.
A *Lisp* cleanup's forms run through `RunEvaluationBody()`, whose loop
republishes `run_base` to the cleanup's own base, so for those the search is
already confined and the floor is redundant. A *native* cleanup runs its
`FeCleanupFn` directly and republishes nothing, and neither does the window
inside `RunEvaluationBody()` before its loop starts, where a frame-limit
raise lands. In both of those `run_base` is still the enclosing run's.

For a native cleanup the floor equals the frame index the entry started at,
and what sits above it depends on what the cleanup does. A cleanup that
honours `FeCleanupFn`'s contract -- `fe.h`: "must not call back into the
evaluator" -- pushes no frames, so nothing is ever above the floor, no
handler is ever accepted, and that arm is bit-identical to what it was
before the fix. A cleanup that violates the contract and re-enters the
evaluator does push frames above the floor, and a `condition-case`
established in that Lisp is honored where before the fix it was not.

That second half is a correction Phase 12's fix cycle made: 12B Part 1
stated the bit-identity unconditionally and offered
`test_api.c:TestNativeCleanupHandlerFloor` as its proof, and that test
covered only cleanups that run no Lisp. The behaviour is not a native
special case and is not a defect. It is this floor's own rule -- a handler
established by the cleanup's OWN work belongs to the cleanup, not to the
computation the drain is abandoning -- and it agrees with both the
pure-Lisp `unwind-protect` form and Emacs 31.0.90. Measured:

```lisp
(condition-case o
  (with-lisp-cleanup (fn () 'body)
    (fn () (condition-case e (car 6) (error 'inner))))
  (error (list 'outer o)))
;; before 95965f0  (outer (wrong-type-argument listp 6))
;; after           body
(condition-case o
  (unwind-protect 'body (condition-case e (car 6) (error 'inner)))
  (error (list 'outer o)))
;; fe and Emacs 31.0.90 alike   body
```

The last three cases of `TestNativeCleanupHandlerFloor` pin all of it,
including the control: with no handler inside the cleanup's Lisp, 06A
Decision 4 is unchanged and the cleanup's raise still replaces the
completion and reaches the enclosing handler.

`RunOneCleanupEntry()` saves and restores `ctx->completion` with the rest of
the ambient state for the same reason. An entry whose own handler catches
its raise leaves the completion `Normal` -- the handler transfer sets it --
and the drain that entry belongs to still needs its own kind afterwards,
because `AllocateFrame()`'s `CleanupFrameReserve` gate and `FePushGC()`'s
reserve both read that field as "a completion is in flight".

Once control resumes at that `setjmp()`, `RunOneCleanupEntry()` restores
the enclosing run's frame stack, floor, barriers and evaluation-control
record, and then *replays* the completion there. That is 06A Decision 4 and
Emacs' measured policy: the cleanup's own completion replaces whatever was
already unwinding, and because the replay happens in the enclosing context
it is an ordinary raise -- an enclosing `condition-case` can catch it. The
replay goes to `RaiseCompletionCore()` rather than to `RaiseCompletion()`,
which is the half that applies the `error_label` prefix, so the source label
already in the saved text is not applied a second time.

Where the replay lands is *not* byte-identical to Emacs, in two shapes, and
the qualification belongs here rather than in the rule above because it is a
consequence of this mechanism. The restore puts back the frame stack the
drain started from, which still holds the abandoned computation's own
handler and catch frames; Emacs, unwinding as it goes, no longer has them.
So a `condition-case` or a `catch` the in-flight completion had already left
can take the cleanup's replacement, where Emacs gives it to the enclosing
handler or answers `no-catch`. Both divergences are pre-existing, were found
by a post-close review rather than by the phase that wrote this, and are
recorded in `doc/language.md`, pinned by
`scripts/unwind-cleanup-handler.fe`, and carried in the manifest as
`unwind-protect-cleanup-raise-residuals`. Closing them means discarding
frames as the drain descends instead of at the replay, which is a different
unwind model, not a bug fix.

A completion a cleanup entry *contains* rather than raises is the opposite
case, and does not replace anything. `FeTryCallWithOptions()` and
`FeTryEvaluateStringWithOptions()` publish the contained completion's kind
and condition object for the host to read, in the same two context fields
the completion being unwound is using, so `RaiseCompletionCore()` holds both
in locals across each of its two drains and puts them back afterwards --
with the condition object on the GC stack for the duration, since the field
that normally roots it is exactly what a containment overwrites. The
handler a `condition-case` selects was never at risk (selection happens
before the drain starts); the object it binds, and the pair a host reads
through `FeGetCompletion()` and `FeGetCondition()`, were.

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

## The Mark Phase

The collector's mark phase walks the object graph without recursion and
without allocating: it is Deutsch-Schorr-Waite pointer reversal, and it stores
its own return path inside the objects it is walking. Until sub-plan 09C it
recursed once per `car` level -- the `cdr` spine was already a loop -- so the
depth at which it took the process down is a function of the frame size, and
the frame size differs by nearly a factor of three across the builds this
project runs: 48 bytes per level in fe's default build (clang, `-O1`), 32 at
`-Os`, 64 under ASan, 80 under MSan. Every figure here therefore names its
build.

In fe's default build, bisection on an 8 MiB stack put the SIGSEGV between
130 000 and 150 000 levels, which is reachable from pure Lisp in any arena of
about 4.9 MiB. A figure of about 262 000 levels appears in older notes: that
is the same 8 MiB stack divided by the `-Os` frame -- an extrapolation, not a
measurement, and not this build's answer.

In kg the failure was measured rather than derived, on two kg binaries
differing only in their fe (kg's `-Os` gcc build, 1 MiB arena): at
`ulimit -s 1280` *neither* crashed, and the old walk's crash reproduces at
`ulimit -s 512` and at 256. An earlier version of this paragraph said the
crash arrived "as soon as the process stack limit dropped to 1280 KiB", which
the measurement falsifies. The claim it was making survives the correction:
a 256 KiB thread stack is an ordinary thing for an embedder to hand a worker,
and a garbage collector that dies on one is a defect wherever the exact
threshold sits.
`test_api.c`'s `TestMarkStackProbe` is the gate: the collector's C-stack
high-water mark, read from inside a `mark_fn` at the bottom of a `car` chain
10, 1 000 and 100 000 levels deep, must stay within 2 KiB of the same probe
fired with no chain at all.

The encoding is two spare low bits of a **pair's** `car` word. Every object is
at least 8-byte aligned, so bits 0-2 of a pointer stored there are free: bit 0
stays clear so `FeGetType()` keeps answering `FeTPair`, bit 1 is `GcMarkBit`
-- which the recursive walk already stored inside a pair's car pointer, and
which the sweep's `TAG(obj) &= ~GcMarkBit` puts back -- and bit 2 is
`GcMarkCdrBit`, which says which half of the pair the walk is in:

| state | `car` | `cdr` |
|---|---|---|
| car half | parent \| mark | the pair's own cdr |
| cdr half | the pair's own car \| mark \| cdr bit | parent |
| finished | the pair's own car \| mark | the pair's own cdr |

An object with exactly one pointer child -- `fn`, `macro`, `symbol`, `string`,
whose `car` word is a type tag (plus, for a string, seven bytes of text) and
never a pointer -- needs no state bit: its link is always in `cdr`, and the
ascent tells it from a pair by its type. Everything else is a leaf.

Two invariants make this safe. Every intermediate state keeps `GcMarkBit` set
and bit 0 clear, so `FeGetType()` never lies about a cell and a cycle that
comes back around stops at the mark check exactly as it did before; and every
path out of the walk restores the cell it leaves, so the sweep sees the tags
the recursive walk would have left it. The graph *is* scrambled while the walk
is inside it, but collection is stop-the-world, so the only code that can
observe that is the host's `mark_fn`. A `mark_fn` may call `FeMark()` -- an
object the walk is inside is already marked, so a nested walk stops at it
immediately -- but must not read `car`/`cdr` of anything but the object it was
handed; `doc/c-api.md` says so.

It also must not leave non-locally. A `longjmp` or a raise out of `mark_fn`
(or out of `gc_fn`, during the sweep) jumps past the ascent that would put the
graph back, and the walk has no state anywhere else to restore it from -- the
reversed graph *is* the state. The recursive walk had no such rule because it
only set mark bits. `FeContext::collecting` is true for exactly the duration of
`CollectGarbage()`, and `RaiseCompletionCore()` treats a raise while it is set
as fatal: it prints the contract and aborts, without calling `error_fn` (whose
whole job is to leave non-locally). `WriteObject()` reads the same flag and
skips its `EvaluationStep()` charge while it is set, which is what keeps the
in-tree route closed -- `main.c`'s `mark`/`gc` tracers print the object they
are handed, and a step budget or an interrupt would otherwise raise from
inside the walk. `test_api.c`'s `TestMarkAbortsOnRaise` forks and pins both
halves: the abort, and the fact that a printing callback does not reach it.

The alternative considered and rejected was an explicit worklist. Bounding one
for *any* data shape costs one slot per object, which for kg's 1 MiB arena is
56 224 pointers -- 439 KiB, 43% of the whole arena -- and growing one on demand
puts a failable allocation inside the one routine that runs after allocation
has already failed. Pointer reversal costs neither.

## Performance Counters

`fe_perf.h` declares 48 compile-time performance counters -- 32 named ones
plus one slot per `FeType` -- and `fe_perf.c` holds their storage, their names
and their JSON report. They exist
because Phase 21 of kg's data-model plan has to *measure* fe's engine -- what
allocates, what is looked up linearly, what the collector walks -- before a
later phase changes any representation, and because a counter is a
deterministic number a unit test can assert, while a wall-clock reading is not.

### The compile-to-nothing contract

Everything is behind `FE_PERF_COUNTERS`, which defaults to 0. In that build:

* every `FE_PERF_*` macro expands to `((void)0)`;
* `fe_perf.c` compiles to a single typedef and defines no symbol at all, so a
  host that links the core without it (kg does) still links;
* no declaration in `fe.h` changes, so `FE_API_VERSION` does not move; and
* the generated code is identical at every instrumented site. The proof is
  mechanical rather than asserted: build `fe.o fe_eval.o fe_run.o fe_unwind.o`
  from the instrumented sources with counters off, build them again from a
  control copy in which every instrumentation line has been replaced by a bare
  `//` comment -- same line count, so `__LINE__` and therefore `assert`'s
  arguments are unchanged -- and the object files compare byte for byte
  identical at `-O0` and at `-O2`. Comparing against the pre-instrumentation
  revision instead leaves exactly one class of difference, the `__LINE__`
  immediates that `assert` passes, which is what the comment control removes.

Counter storage is one process-wide static array and deliberately *not* a
field of `FeContext`: the context lives inside the caller's arena, so widening
it in a counting build would move the object/frame partition and measure a
different arena from the one being described. Measured both ways, a 1 MiB
arena partitions into 56 147 object slots in the counting build and in the
ordinary one. The price of a process-wide array is that the counters are
totals across every context a process opens, while `FeArenaStats` is per
context; a measurement that reads both uses one context.

### What is counted, and where

| Group | Counters | Site |
| --- | --- | --- |
| allocation | `alloc_object`, `alloc_pair` … `alloc_fex2`, `alloc_retyped` | `MakeObject`, `FeCons`, `SetType` |
| collector | `gc_collection`, `gc_mark_visit`, `gc_mark_new`, `gc_sweep_examined`, `gc_reclaimed` | `CollectGarbage`, `FeMark` |
| strings | `string_cell`, `string_byte`, `string_walk`, `string_walk_cell`, `string_walk_byte`, `string_byte_copied` | `BuildString`, `CopyStoredStringBytes` |
| interning | `intern_lookup`, `intern_miss`, `intern_candidate` | `FindInternedSymbol` |
| symbol names | `name_compare`, `name_byte` | `IsStringEqual` |
| environments | `env_lookup`, `env_cell`, `env_bind` | `GetBound`, `HasLexicalBinding`, `Bind` |
| function cells | `function_resolve`, `function_hop` | `ResolveFunctionCallable` |
| evaluator | `eval_step`, `eval_dispatch`, `frame_push`, `dispatch_primitive`, `dispatch_callable`, `dispatch_native`, `dispatch_lambda`, `dispatch_macro`, `macro_expansion` | `EvaluationStep`, `RunEvaluationLoop`, `AllocateFrame`, `DispatchResolvedCall`, `ResumeArguments`, `EnterMacroBody` |

The by-final-type block is one slot per `FeType`, indexed by the type itself
(`FE_PERF_ALLOC_SLOT`), so a new type gets a slot by existing rather than by
being added to a switch. A cell is charged where its type is settled: a pair in
`FeCons`, which is the only place a cell becomes one, and everything else in
`SetType`, which is the only place a type is spelled. `BuildString` is the one
constructor that does both -- it takes its cell through `FeCons` and then
retypes it -- so `FePerfCountRetype` moves that charge and counts the move in
`alloc_retyped`. The invariant this buys, and the first thing `test_api.c`'s
`TestPerfCounters` asserts, is that `alloc_object` equals the sum of the
by-type block.

Peak live cells, peak GC-root depth and peak frame depth are *not* counters:
`FeArenaStats` already tracks them in the shipped build, and `FePerfWriteJson`
reports them in an `"arena"` object beside the counter totals rather than
tracking the same thing twice.

Nothing here reads a clock, allocates, or touches the GC root stack. Every
instrumented site is a bare macro call with no branch of its own, which is also
why the counters cost the complexity ratchets nothing outside `fe_perf.c` and
`main.c` -- `scc` and `pmccabe` both read source text and do not evaluate
`#if`, so code that never compiles is still measured by them.

### Building and reading a counting fe

`make perf` builds the counting interpreter, the counting `test_api` and the
counting example host into `perfobj/`. The objects live in their own directory
so a counting object can never be linked into an ordinary binary and an
ordinary object can never be linked into a counting one; `tiny-regex-c/re.o` is
shared, since `FE_PERF_COUNTERS` does not appear in `RE_CFLAGS`.

`make perf-check` is the counting build's own `check`: it runs the C API suite
-- where the counter relationships are asserted -- the workload battery below,
the example host, and the whole `scripts/` corpus against the counting
interpreter, so every instrumented line is executed and not merely compiled.
`.ci/ci-10-perf-counters.sh` is that target as a CI stage, which is what keeps
a facility nobody compiles by default from rotting.

There are two ways to read the counters:

* in process, with `FePerfRead(counter)` (and `FePerfReset()` to scope a
  measurement to one workload). This is what a test and Phase 21.2's workload
  runner use.
* as JSON, with `FePerfWriteJson(out, &stats)`. The counting `fe` writes it to
  `$FE_PERF_OUT` when a run completes:

```
$ make perf
$ FE_PERF_OUT=/tmp/fe.json ./perfobj/fe -e '(print (+ 1 2))'
```

A run that ends through an escaping error exits before that report by design:
the counters describe a completed run.

### The workload battery

`perf_workloads.c` is Phase 21.2's in-process runner: one binary, built from
the counting objects into `perfobj/perf_workloads`, that runs each named
workload in its own `FeContext` with the counters reset around it, checks the
workload's own answer, and emits a record per workload. `make perf-workloads`
runs it and writes `perfobj/workloads.json`; `make perf-check` runs it too, so
`.ci/ci-10-perf-counters.sh` is its CI home. It lives on the test side --
`perf_workloads.c` is in `TEST_SRCS`, not `SRCS` -- so it costs the `scc` and
`pmccabe` ratchets nothing while `format-check` still covers it.

Twenty workloads in six families: `context` (a bare open, and a bare open
together with its close), `eval` (the four shapes kg's `utils/bench.py`
benchmarks, respelled for a Lisp-2 without kg's prelude), `intern` (128, 1024
and 8192 distinct symbols, then a miss and two hits), `env` (lexical lookup by
environment width and by depth, separately), `string` (0, 7, 8, 256 and 8192
bytes -- 7 and 8 straddle the `StringBufferSize` cell boundary) and `gc`
(sparse-garbage and dense-live collections). `./perfobj/perf_workloads --list`
prints them with the arena each one uses and why.

Three properties are what make the numbers usable.

* **Every workload checks its own answer.** A workload whose result is not
  asserted can silently stop doing its work while its counters still look
  plausible.
* **The counter assertions are the gate; wall time is a report.** `seconds`
  travels with every record and nothing reads it, because a sanitizer lane or
  a loaded box must not be able to fail this. The assertions prefer
  relationships to golden constants, and are chosen so that the phases after
  this one make them fail *loudly*: the `string` checks pin the seven-byte cell
  chain, the `intern` checks pin the linear `symbol_list` scan, the `env`
  checks pin the single flat alist, and the `gc` checks pin the
  arena-proportional sweep.
* **A counter read from a differently sized arena is a different
  measurement.** Each workload names its own arena (96 KiB, 256 KiB, kg's
  1 MiB, or 4 MiB for the intern tiers), and its cell capacity travels with
  its counters. The fixed arena is exercised where it collects often as well
  as where it does not.

The record schema is `fe-perf-workloads/2`: a top-level object carrying the
schema name, an `artifact` header, `StringBufferSize`, then one object per
workload with its name, family, note, `param`, `arena_bytes`,
`cell_capacity`, `context_open_cells`, `includes_context_open`, `answer`,
`seconds`, an `extra` object of workload-specific probes, and
`counters`/`arena` objects whose keys are exactly the ones `FePerfWriteJson`
writes. The counters are a delta over the workload's own measured region --
the context open is excluded from every workload except `context-open`, whose
measured region *is* the open, which is what `includes_context_open` reports
-- so a consumer never has to subtract a baseline itself.

The `artifact` header names what produced the numbers, because a number whose
artifact line is not the tree under discussion is not evidence about it. It
carries `fe_version` (`FeVersion`), `fe_api_version` (`FE_API_VERSION`) and
`fe_language_version` (`FE_LANGUAGE_VERSION`), which the binary knows about
itself and which schema `/1` wrote at the top level, plus `fe_git_describe`
and `binary_sha256`, which it cannot: a describe compiled into an object file
names the tree that last triggered a rebuild, not the tree that ran. Those
two arrive as `--git-describe` and `--binary-sha256`, which `make
perf-workloads` fills in from `git describe --always --dirty` and
`sha256sum` at measurement time. A value the driver did not supply -- a run
of the binary by hand, a box with no `git` -- is `null`, never a guess.

The `context` pair is two workloads because `FeCloseContext` is not a small
destructor: it clears every root and runs a full `CollectGarbage` over the
whole arena. The harness closes an ordinary workload's context *after* the
counters are snapshotted, so `context-open` cannot measure the close even by
accident; `context-open-close` therefore opens and closes a context of its own,
in a second arena, inside its measured region -- one collection, every cell the
open took reclaimed, and a sweep that examined every slot in the arena. Its
`arena` object and the arena table's `capacity`/`end-live` columns describe the
harness context it ran under, which is the one still open when the snapshot is
taken; its `counters` describe the scratch pair.

The GC-root stack decides how a workload is written. `MakeObject` pushes every
new object onto a fixed root stack (`GcStackSize` less `GcStackReserve`, so
4032 in practice), so a C loop that allocates a caller-controlled number of
times overflows it. The `intern` and `string` workloads are C loops that take
one `FeSaveGC` checkpoint and restore it every pass -- safe for interning
because `symbol_list` is a permanent root -- and they measure the cost of an
*operation*. Everything whose size is the point of the workload is a Lisp loop
instead, whose accumulator lives in a value cell the collector marks directly
and which therefore has no such ceiling; those measure the cost of a *shape*.

## Known Issues

The implementation has some known issues. These exist as a side effect of trying
to keep the implementation concise, but should not hinder normal usage.

* `FeMark()` has no cycle detection of its own; it relies on the mark bit,
  which the writer has no equivalent of. The writer used to share the
  collector's recursion and shares it no longer: `WriteObject()` bounds `car`
  nesting with an explicit depth budget and walks the `cdr` spine with two
  pointers, so the two are not the same shape and should not be changed as if
  they were. (The collector's own `car` recursion, which this entry used to
  describe as a known issue, is gone -- see "The Mark Phase" above.)
* The storage of an object’s type and GC mark assumes a little-endian system and
  will not work correctly on systems of other endianness.
* Proper tailcalls are not implemented — `while` can be used for iterating over
  lists.
* Strings are `NUL`-terminated and therefore not binary-safe.
