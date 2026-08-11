# C API

For the full details of the core Fe API, refer to `fe.h`. The runnable
[`example_host.c`](../example_host.c) demonstrates the complete lifecycle
described below.

## API Compatibility

Fe has two independent version numbers, because a change to the C embedding
contract and a change to the Lisp language it evaluates are different kinds of
break; a host should assert both.

The two numbers are counted separately and are currently close enough to be
confused for each other, so every mention below names its unit.

`FE_API_VERSION` identifies the public embedding interface -- the C functions,
types, and callback signatures declared in `fe.h`; API version 8 adds the
input-unit trio `FeEnterInputUnit`/`FeReadInputForm`/`FeLeaveInputUnit`, and
API version 7 added the
protected string evaluation `FeTryEvaluateStringWithOptions`. `FE_LANGUAGE_VERSION`
identifies the Lisp language `FeEvaluateString()` and friends evaluate --
language version 11 is the funcall-classification repair (`signal`, `error`
and `keywordp` are reachable through `funcall`/`apply`, and `FeIsFunction`
answers true for all three);
language version 10 is the cleanup-handler, `eval` and input-unit contract
(a handler
established inside an `unwind-protect` cleanup is honored by that cleanup's
own raise, `eval` evaluates its form in the caller's run, and a
one-argument-`defvar` mark is scoped to the input unit that made it);
language version 9 is the special-variable contract (`internal--mark-special`,
`special-variable-p`, and shallow dynamic binding at `let`'s two binding
paths); language version 8 is the reflective-expansion contract
(`macroexpand-1`, `macroexpand`, and `macroexpand-all` naming itself as
unimplemented); language version 7 -- a different 7 from the API version
above -- is the protected-constants and self-evaluating-keywords contract;
language version 6 is the strict-arity contract described below; language
version 4 was the numeric contract of the Emacs-subset cut: the reader
classifies `42` as an integer and `42.0` as a float (05A Decision 3), floats
print shortest-round-trip with a `.` or an exponent always present (`42.0`,
`1e+20` -- an appended `.0` only when neither is there already), integer
division truncates
with `arith-error` on overflow and divide-by-zero, and `eq`/`eql` are core
primitives with Emacs' identity semantics. A host that vendors or pins Fe
should assert both versions it was written against at compile time:

```c
static_assert(FE_API_VERSION == 8);
static_assert(FE_LANGUAGE_VERSION == 11);
```

Fe 12.0 moves `FE_LANGUAGE_VERSION` 10 -> 11 and leaves `FE_API_VERSION` at
8; the reasoning is below, under the version history.

Fe 11.0 moves `FE_LANGUAGE_VERSION` 9 -> 10 and, in Phase 12's fe fix cycle,
`FE_API_VERSION` 7 -> 8; the reasoning for both is below, under the version
history. The release string does not move again for the fix cycle: the cycle
is the same phase, reopened, and 11.0 covers what the phase shipped.

Fe 10.0 moved `FE_LANGUAGE_VERSION` 8 -> 9 and `FE_API_VERSION` 6 -> 7;
the reasoning is below, under the version history.

Fe 9.0 moved `FE_LANGUAGE_VERSION` 7 -> 8 and left `FE_API_VERSION` at 6;
the reasoning is below, under the version history.

Fe 8.0 kept `FE_API_VERSION` unchanged, and that was a deliberate
choice rather than a claim that nothing moved. `FE_LANGUAGE_VERSION` went 6 ->
7 for Phase 8, and 7 covers the whole phase: the protected constants and
self-evaluating keywords *and* the strict reader, which is one bump for one
phase rather than two for two slices of it.

The reader half does change what an existing program means, so a host pinning
version 7 should read it as a break and not only as an addition. A bare `#`
and every `#`-initial symbol no longer read -- Fe's own
`scripts/concatenate.fe` named a function `#` and had to be renamed --
`[...]`, `#:`, `#s(...)` and symbol escapes such as `a\ b` are named read
errors instead of symbols, an unknown string escape errors where the
backslash used to be dropped silently, and a string escape must land in one
byte, so `"\0"` and `"\400"` error where they used to truncate the string.
What the release adds is the measured Emacs subset: UTF-8 character literals
with the `\C-`/`\M-` modifiers, greedy variable-width `\x`, radix integers,
and a one-based line number on evaluated-file and evaluated-string
diagnostics. `doc/language.md` states the accepted and rejected syntax in
full, and `compat/features.json` records every deliberate divergence from
Emacs with the oracle's own answer beside it.

Both macros moved 3 -> 4 together in sub-plan 05D of kg's Emacs-subset
program, the numeric cut: 05A's placement (a) Decision had inserted
`FeTInteger` into the public `FeType` enum immediately after `FeTDouble`,
renumbering every later constant, so a host built against the pre-cut header
was already ABI-incompatible; `FeMakeInteger`/`FeToInteger` (05B) joined the
constructor/accessor pair; and the reader now produces integers from source
text, so a host-made and a read number share one meaning. The bump was
deliberately deferred to the cut so the whole numeric contract moves as one
visible break, and kg's existing `static_assert(FE_API_VERSION == 3)` fails
at the 05E pin as the designed tripwire. `FeVersion` moves "4.0" -> "5.0"
with the same break.

Both macros moved 4 -> 5 together in sub-plan 06D, the condition cut, for
different reasons on each axis -- which is the case the two-axes rule exists
for. On the C side `FeCompletion`, `FeGetCompletion`, `FeGetCondition`,
`FeRaiseCompletion` and `FeResignal` are new surface and an error a host used
to see only as text now also carries an `(error-symbol . data)` object. On
the Lisp side `condition-case`, `signal` and a condition hierarchy exist, and
every evaluator raise carries a symbol and a data list. `FeVersion` moves
"5.0" -> "6.0".

Both moved 5 -> 6 in sub-plan 07B, the strict-arity cut, with `FeVersion`
"6.0" -> "7.0". The C break is a removal: `FeSetStrictArity()` and
`FeGetStrictArity()` are gone outright -- no deprecated no-op, no lax mode --
so a host that called either gets a compile error, which is the only honest
notification, since there is no runtime answer that would still be true. The
language break is that existing programs change meaning rather than failing
to compile: `((lambda (x) x))`, `((lambda () 1) 2)`, `(car 1 2)` and
`(quote 1 2)` all used to answer and are now `wrong-number-of-arguments`;
`(and)` was nil and is `t`; `(signal 'error)` was an arity error and is now
accepted with nil data; `print` gains a one-argument minimum; and malformed
parameter lists raise `invalid-function` rather than prose errors. Fe's
dotted-tail and bare-symbol rest spellings are unaffected.

`FE_LANGUAGE_VERSION` moved 6 -> 7 for protected constants and
self-evaluating keywords. Assigning `t`, `nil`, or a keyword now signals
`setting-constant`, and a keyword such as `:foo` no longer needs quoting.

`FE_LANGUAGE_VERSION` moved 9 -> 10 in sub-plan 12B, with `FeVersion` "10.0"
-> "11.0" and `FE_API_VERSION` left at 7 in that commit -- no declaration in
`fe.h` changed there, though Phase 12's fix cycle moves it to 8 for the
input-unit trio, and the two land under one `FeVersion` "11.0". THREE
language changes land under language version 10, and all three change what
an existing program answers.

The first is a fix. A `condition-case` written inside an `unwind-protect`
*cleanup* now handles what that cleanup raises. Before, it handled nothing at
all, drain or no drain: the raise path tested its cleanup bounce before it
ever searched for a handler, so `(unwind-protect 'body (condition-case nil
(car 6) (error nil)))` reached the host as an unhandled error where Emacs
31.0.90 answers `body`. 06A Decision 4 is unchanged and is now the rule for
the remaining case: a cleanup raise that *nothing in the cleanup* can handle
still replaces the completion being unwound and is still catchable by an
enclosing `condition-case`. A host's `FeProtectWithCleanup` cleanup
establishes no handlers of its own, so nothing changes for a native cleanup
that runs no Lisp.

The second is an addition: `eval`, Emacs' `(eval FORM &optional LEXICAL)`.
FORM is an evaluated operand and its value is evaluated in the *caller's own
run*, not in a nested one, so a condition, a `throw` or a quit out of the
evaluated form propagates to handlers, catches and cleanups established
around the `eval` call, and the steps it spends come out of the caller's
budget. LEXICAL must be nil or absent; a non-nil value is rejected by name
(`unsupported feature: eval lexical argument`), the same convention
`macroexpand`'s ENVIRONMENT uses. The environment is the global one, which is
what Emacs' LEXICAL=nil means: the caller's lexical bindings are not visible
to the evaluated form, while dynamically bound names are.

The third is sub-plan 12C Part 2's input-unit scoping, and it changes what an
existing *embedder* answers, not only what a program does. A one-argument
`internal--mark-special` mark -- Emacs' one-argument `(defvar v)`, which
makes `let` over the name dynamic while `special-variable-p` still answers
nil -- now belongs to the input unit that made it. Every
`FeEvaluateString()`/`FeEvaluateFile()` call is one such unit, and they
nest, so two calls on one context no longer share those marks: a host that
made the mark in one call and relied on it in the next now gets the lexical
answer there, which is what Emacs gives for two separate files. Full marks
(the two-argument form, and `defconst`) are global and unaffected, and so is
`special-variable-p`. Outside any input unit -- a `FeCall()` straight from C,
a host-driven `let` -- every mark is visible, because there is no unit there
for one to be foreign to. See `doc/language.md` for the model and its two
recorded residuals.

`FE_LANGUAGE_VERSION` moved 11 -> 12 in Phase 14, with `FeVersion` "12.0" ->
"13.0" and `FE_API_VERSION` left at 8 -- no declaration in `fe.h` changed.
Unlike versions 8 and 11, this one IS a break, in three directions at once.

New names: `intern`, `intern-soft`, `symbol-name`, `make-symbol`, `gensym`,
`put`, `get` and `symbol-plist`, all ordinary functions, all answering
`void-function` before. That much alone would be an addition, and would still
be a bump for version 8's reason.

The reader: a backslash in a token was a named read error and is now Emacs'
symbol escape, so `(a\ b)` is a one-element list where it used to be a
diagnostic, `\1` is the symbol `1`, an escaped `\.` inside a list is an
ordinary element rather than the dotted-tail marker, and `##` is the symbol
with the empty name. A program that did not read now reads.

The writer: a symbol whose name would otherwise read back as something else
prints with escapes, so the symbol `.` prints `\.` and `(intern "a b")`
prints `a\ b`. A program that printed now prints differently -- which is what
makes the reader change safe, the two being inverses.

One behaviour outside those three moved with them: `keywordp` now asks
whether the interner self-bound the name, not only whether it starts with a
colon, so `(keywordp (make-symbol ":a"))` is nil as it is on Emacs 31.0.90.

The object layout moved too, though no host can see it: a symbol's `cdr`
chain widened from `((name . function) . value)` to
`(((name . plist) . function) . value)`, one cons per symbol, which raises
`FeMinimumArenaSize()` accordingly. The property list lives in the symbol
rather than in a context-side registry because an uninterned symbol is the
first symbol Fe has that the collector may reclaim, and a registry keyed by
symbol would pin every symbol that ever carried a property.

`FE_LANGUAGE_VERSION` moved 10 -> 11 in Phase 13.1, with `FeVersion` "11.0"
-> "12.0" and `FE_API_VERSION` left at 8 -- no declaration in `fe.h` changed.
The language change is one repair: `signal`, `error` and `keywordp` had no
row in the evaluator's `primitive_is_function[]`, so `funcall` and `apply`
rejected them as special forms even though every one of their arms evaluates
its operands and `(special-form-p ...)` is nil for all three on Emacs
31.0.90. `(funcall 'signal 'error '("x"))`, `(apply 'error '("boom"))` and
`(mapcar 'keywordp '(:a 1))` raised `invalid-function` and now behave as
Emacs does, and `FeIsFunction` -- `functionp`'s question, which reads the
same table -- answers true for all three where it answered false.

Like language version 8's, this bump is not a break: no program that ran
under 10 answers differently under 11, because every affected program raised.
It is a bump for version 8's stated reason. This macro's only consumer is a
host's compile-time `static_assert`, and `signal` reachable through
`funcall`/`apply` is precisely what a prelude's higher-order functions are
built on, so a macro that did not move could not tell an embedder whether the
fe it links against has it. The `FE_GC_STRESS` build knob lands in the same
slice and moves neither macro: it is a compile-time define inside `fe.c` with
no declaration in `fe.h` and no effect on any program's answer.

`FE_API_VERSION` moved 7 -> 8 in Phase 12's fe fix cycle, for the input-unit
trio: `FeEnterInputUnit`, `FeReadInputForm` and `FeLeaveInputUnit`, with the
`FeInputUnit` token they pass between them. Nothing was removed and nothing
changed meaning. It exists because no composition of the surface before it
let a host run its own read-eval loop inside ONE input unit in the *current*
run -- see "Driving An Input Unit Yourself" below for what that means and why
a `load` written in Lisp needs it. `FeVersion` stays "11.0": the fix cycle is
the same phase, and the release string covers what the phase shipped.

`FE_API_VERSION` moved 6 -> 7 in sub-plan 11C, for one added declaration:
`FeTryEvaluateStringWithOptions`. Nothing was removed and nothing changed
meaning, so every existing call keeps compiling; the bump exists because a
version that does not move cannot tell a host whether the fe it is linking
against has the entry point at all.

`FE_LANGUAGE_VERSION` moved 8 -> 9 in sub-plan 11B, with `FeVersion` "9.0" ->
"10.0" and `FE_API_VERSION` left at 6 in that commit -- no declaration in
`fe.h` changed there. This is the first bump in the series that changes what
an *existing* program answers rather than only adding names: a symbol marked
by `internal--mark-special` binds dynamically, so `let` and `let*` over it
swap the global value cell instead of extending the lexical environment, a
function that reads the name free sees the bound value, and the previous
value -- or the symbol's unboundness -- comes back on every completion kind.
A program that never marks anything cannot tell the difference, and closure
and defun *parameters* stay lexical unconditionally, which is Emacs' own
answer under `lexical-binding: t`. See `doc/language.md` for the model.

`FE_LANGUAGE_VERSION` moved 7 -> 8 in sub-plan 10B, with `FeVersion` "8.0" ->
"9.0" and `FE_API_VERSION` deliberately left at 6 -- no declaration in `fe.h`
changed. The language gains `macroexpand-1` and `macroexpand`, which apply a
macro's transformer without evaluating what it produces (one step, and Emacs'
fixpoint), and `macroexpand-all`, which exists only to reject itself by name.
This is the one bump in the series that is *not* a break: no program that ran
under version 7 answers differently under 8. It moves anyway, and the reason
is recorded rather than assumed. The rule two paragraphs below -- compatible
additions do not require a bump -- is written for downstream embedders who
pin a commit; this fork has none (the parent program's §0.4), and its single
consumer is kg's compile-time `static_assert`. A macro that does not move
cannot tell kg whether the fe it links against has these names, and the
alternative -- discovering it at run time as `void-function` -- is exactly the
failure the two-macro scheme exists to prevent. The counter-argument is real
and is recorded here beside the decision: sub-plan 04C added seven primitives
(`funcall`, `apply`, `fboundp`, `symbol-function`, `fset`, `fmakunbound`,
`defalias`) under the additive rule without a bump, and Phase 9 made
exhaustion catchable -- a genuine change to an existing program's answer --
without one either. Read together, the precedent says a bump is optional
here; the two-repository contract says it is useful. It moves.

`FE_API_VERSION` moved 1 -> 2 (`FeVersion` "2.0" -> "3.0") in sub-plan 03F of
kg's Emacs-subset program: the frame machine's Lisp-nesting and native
re-entry bounds are now two separate `FeEvalOptions` fields
(`max_frames`/`max_native_reentry`) and three separate `FeArenaStats` fields
(`frame_capacity`/`peak_frame_depth`/`peak_native_reentry`), replacing the
single `max_depth`/`peak_evaluation_depth` pair whose *meaning* changed when
the frame machine replaced the recursive evaluator. `FE_LANGUAGE_VERSION`
moved 1 -> 2 earlier, in sub-plan 02C (the `setq`/`set` assignment cut and `=`
as chained numeric equality), and stayed at 2 through Phase 3 and 04C --
Phase 3 is behaviour-neutral by design, and 04C added the namespace machinery
additively, so no Lisp-visible evaluation result, side effect, ordering, or
diagnostic changed and no call-position answer moved until 04D's cut.

Each macro is bumped for every breaking change in its own axis: a C ABI/API
break bumps `FE_API_VERSION` without necessarily touching the language, and a
language break such as the `=` cut bumps `FE_LANGUAGE_VERSION` without
advertising a C embedding break it did not make. Compatible additions do not
require a bump, so downstreams should still pin an exact released commit or
tag rather than using either macro as a substitute for source control.

## Initializing A Context

`FeMinimumArenaSize()` reports the smallest arena that can hold the context and
all objects created while installing the core primitives. It does not leave
space for application objects, so practical arenas should be larger.
`FeArenaAlignment()` reports the required alignment. Both values follow the
private object layout and may differ between platforms or Fe versions.

The caller owns the arena. Its address and size must remain unchanged and its
storage must remain valid and exclusively available to Fe until
`FeCloseContext()` returns. Fe never frees the arena. An arena returned by
`malloc()` has sufficient alignment; other storage must be aligned to at least
`FeArenaAlignment()`.

`FeOpenContext()` returns `nullptr`, without printing or terminating the
process, when the arena is null, misaligned, too small, or crosses the address
space boundary. Failure does not initialize the arena. A successfully opened
context must be closed before its arena is released or reused. Closing runs the
GC callback for unreachable pointer-backed objects. The same arena can then be
opened again.

```c
size_t size = 1024 * 1024;
void* arena = malloc(size);
FeContext* ctx = FeOpenContext(arena, size);
if (ctx == nullptr) {
  free(arena);
  return false;
}

// ...

FeCloseContext(ctx);
free(arena);
```

Each context is single-threaded: do not call into one context concurrently or
move an active call chain between threads. Callbacks run synchronously on the
calling thread. Distinct contexts have distinct core interpreter state. The
legacy Fex custom-type names are process-global and are not suitable for
independent kg-style contexts; see "Legacy Fex Custom Types" below.

## Arena Statistics

`FeGetArenaStats(ctx)` returns an `FeArenaStats` snapshot: total and
currently-free object slots, the peak live-object count, the collection
count, the peak GC-stack (root) depth, the arena's host-usable evaluator
frame capacity (`frame_capacity`, excluding the private cleanup reserve --
the same ceiling `FeEvalOptions.max_frames` of 0 selects) and the peak
number of simultaneously live evaluator frames against it
(`peak_frame_depth`), the peak cleanup-stack depth, the peak number of
nested evaluator runs started synchronously from a native
(`peak_native_reentry`, zero for a program that never re-enters through a
native no matter how deep its Lisp nesting), and the count of allocation
failures (a `MakeObject()` call that still found no free slot after a
collection). Every field is a counter Fe already maintains at the site that
changes it; the call itself allocates no object, walks no list, and mutates
nothing, so it is safe to call at any time, including from an error handler
or between collections.

This is a read-only accessor for baselining and margin questions -- "how
close is the fixed arena to full" -- not a live diagnostic surface: there is
no Lisp-visible primitive that exposes it, and a host that wants to surface
it to users owns that decision and its own presentation.

## Context Userdata And Callbacks

`FeSetUserData()` stores one host pointer in a context, and `FeGetUserData()`
retrieves it. Fe does not dereference, retain, or release the pointed-to data.
It must remain valid whenever host code or a callback accesses it. This is the
usual way for native and error callbacks to find per-context host state.

Install callbacks with `FeSetErrorFn()`, `FeSetMarkFn()`, and `FeSetGCFn()`.
Passing `nullptr` disables a callback. Callback functions and any state they
use must remain valid until replaced or until `FeCloseContext()` returns. The
mark and GC callbacks can run during any operation that allocates an Fe object,
including context close.

The mark callback runs *inside* the mark phase, which since sub-plan 09C walks
the object graph by pointer reversal: while the walk is in progress the
`car`/`cdr` fields of the objects between the roots and the one being marked
hold the collector's return path, not their own values. A mark callback may
call `FeMark()` on anything (an object the walk is inside is already marked, so
a nested walk stops at it immediately), and may read the object it was handed;
it must not read `car`/`cdr` of anything else, and must not allocate. The
collector puts every field back before it returns, so nothing outside the
callback can observe any of it.

A mark or GC callback must also **return normally**. It may not `longjmp` out,
and it may not raise: no `FeHandleError()`, no `FeRaiseCompletion()`, and
nothing that raises on its behalf, `FeCar()`/`FeCdr()` on a non-pair included.
The walk keeps its return path in the reversed graph rather than on the C
stack, so a non-local exit has nothing to unwind it with and abandons the arena
half-reversed; the next reader of one of those chains dereferences a tagged
parent pointer. Under the recursive walk this rule did not exist, because that
walk only ever set mark bits and a `longjmp` past it left a valid heap. Fe
detects a raise from inside collection and calls `abort()` after printing a
line naming this contract -- a loud stop rather than a silent corruption that
faults later somewhere unrelated. `error_fn` is deliberately not consulted on
that path: leaving non-locally is exactly what an `error_fn` does.

Printing from a callback is safe. `FeToString()` on the object it was handed
does not charge the evaluation step budget and does not poll the interrupt
while a collection is running, precisely so that the obvious diagnostic
callback -- `main.c`'s own `mark`/`gc` tracers are this -- cannot trip the rule
above through a step-limit or interrupt raise.

Lambda and macro arity is always strict. Missing required arguments and
leftover arguments raise `wrong-number-of-arguments`; missing `&optional`
parameters bind `nil`, and `&rest` receives a fresh list. Fe's dotted-tail and
bare-symbol parameter spellings remain variadic. A malformed proper parameter
list raises `invalid-function` when the callable is invoked.

Every *core primitive* reports a wrong argument count the same way, with no
exceptions: `wrong-number-of-arguments` carrying `(FUNCTION NARGS)`, checked
from the raw argument list before any operand is evaluated.

*Native helpers* are the separate contract, because a native declares no
arity: their checks necessarily happen after operands have been evaluated,
and they retain their historical `too few arguments` and `too many arguments`
messages while raising the same condition. During a native callback,
`FeGetNextArgument()` and `FeRequireNoArguments()` read a per-call record --
the callable and the original argument count -- and put it in the condition
data. That record is saved and restored around nested native re-entry, is
restored when a `condition-case` inside a run the native started catches, and
is cleared before a non-local raise abandons the activation that published
it.

An Fe native callback may re-enter the reader or evaluator on the same context.
This nesting is supported on the same thread. The callback must observe the GC
protection rules below, and an error escaping nested evaluation must unwind all
the way to the host's recovery point.

## Object Lifetime And GC Protection

Object-producing functions return an object that is either on the context's GC
protection stack or reachable from an interpreter root. Newly allocated
objects are pushed automatically; reader and evaluator results retain the
protection or rooted reachability established while producing them. The
context has separate internal roots for the latest multi-form evaluation
result, the latest `FeCall()` result, and the persistent-root list. Save an index
before temporary work and restore it afterward:

```c
size_t gc = FeSaveGC(ctx);
FeObject* result = FeEvaluate(ctx, expression);
// Use result while it is protected.
FeRestoreGC(ctx, gc);
```

Accessors such as `FeCar()`, `FeCdr()`, and `FeGetNextArgument()` return
existing objects and do not add another protection entry. Such a result is safe
while reachable from a protected object or another interpreter root. Call
`FePushGC()` when it must survive allocations after that reachability can be
lost. Never retain an `FeObject*` after removing its last protection or root;
any object creation can trigger collection.

For values that must outlive a temporary GC frame, `FeCreateRoot()` creates an
opaque persistent root in the context's arena. `FeGetRoot()` retrieves its
value, and `FeReleaseRoot()` releases it. Roots are independent and survive
collections until released or until `FeCloseContext()`. Root bookkeeping is
bounded by the caller-provided arena; creating a root can therefore raise the
normal `out of memory` error. A released root handle is invalid. Passing a
handle that is not active in that context to `FeReleaseRoot()` raises `root is
not active`; this includes immediate double release and handles from another
context.

`FeNil()` returns Fe's nil object without requiring an embedder to reference
the legacy public `nil` global.

`FeMakeInteger(ctx, value)` creates an `FeTInteger` with an exact `int64_t`
payload, and `FeToInteger(ctx, object)` returns that payload or raises an
`expected integer` type error. Integers are an ordinary Lisp value since
05D's cut -- the reader classifies `42` as one and the arithmetic tower
returns them -- so a host-made integer and a read one are the same thing;
the sentence that once stood here, that no Lisp program could create one,
described 05B's dormant object and stopped being true two sub-plans later.
`FeToDouble()` also accepts an integer and converts it to an
`FeDouble`; as with any integer-to-floating conversion, large values may not
be represented exactly.

`FeIsBound()` reports whether a symbol has a global value. A symbol exists as
soon as it is read or interned, but its value cell starts out holding a private
sentinel that no API returns and Lisp cannot reach; evaluating such a symbol
raises `void-variable NAME`, and in head position `void-function NAME`.
`FeSet()` and Lisp's `=` create or update the binding, and `nil` is an ordinary
bound value. The Lisp-level spellings are `(boundp 'name)` and
`(makunbound 'name)`, which also see lexical bindings; `FeIsBound()` has no
environment to consult and answers about the global one.

`FeGetValue()` (FE_API_VERSION 9) is `FeSet()`'s inverse: the symbol's global
value, or `nullptr` for an unbound name. It answers a pointer rather than
`nil` precisely so a caller that takes a value out and puts it back later can
tell "no value" from "the value nil" -- the sentinel that distinguishes them
inside Fe is private and no API returns it. `FeMakeUnbound()` is the other
direction, `(makunbound 'name)`'s global arm, and after it `FeIsBound()` is
false and a reference raises `void-variable` again. Neither evaluates
anything, so neither charges the step budget or can be caught by a Lisp
handler; both type-check their symbol, and `FeMakeUnbound()` refuses `t`,
`nil` and keywords with `setting-constant` exactly as `FeSet()` does. Both
address the *global* binding and never an environment entry, which is the
same rule `FeSet()`/`FeIsBound()` already live under: a host calling them is
talking about the cell `setq` writes when nothing shadows the name.

### The function namespace (sub-plan 04C)

`FeSetFunction()` writes `sym`'s *function* cell, storing the object or symbol
designator as-is, so a `defalias`-style indirection stays a symbol. `FeSet`,
`FeSetFunction`, and `FeDefineNative` reject `t`, `nil`, and keyword targets
with `setting-constant`; the condition data is a one-element list containing
the target.
`FeIsFBound()` reports whether that cell holds anything. They are the
function-cell twins of `FeSet()`/`FeIsBound()`, which keep their Emacs
meaning and address the value cell. `FeGetFunction()` resolves a name the way
call position does: the function cell first, symbol indirection followed
iteratively (one step charged per hop), and -- since sub-plan 04D's namespace
cut deleted the value-cell fallback -- nothing else. It returns `nil` when the
name is unbound, and `nil` for a self-referential chain (`(fset 'x 'x)`) as
well: it is the one reader of the chain that does not raise
`cyclic-function-indirection`. That is a host-API design choice, not a copy of
Emacs -- Emacs has no cyclic chain to resolve, because `fset` itself signals
`cyclic-function-indirection` and leaves the cell untouched. A C host cannot
catch an Fe error: `FeHandleError` longjmps into whatever evaluation is
running, which for a host resolving a callback name is an *outer* run or none
at all, so the raise would land in a frame that has already returned. Call
position, `funcall`/`apply` and `FeIsFunction` are all inside a catchable
evaluation and keep raising. `FeIsFBound()` tells the two `nil`s apart: an
empty cell is not f-bound, a cycle is. Outside an active evaluation the
per-hop step charges are no-ops.
The Lisp-level spellings are `(fboundp 'name)`, `(fset 'name ...)`,
`(symbol-function 'name)` and `funcall`/`apply`. `FeDefineNative` now shares
the same cell (see below), which is the meaning change 04D's `FE_API_VERSION`
3 bump names.

`FeIsFunction()` answers Emacs' `functionp` question about the resolved
callable: true for a lambda, a host native, and a *function-shaped* primitive
-- one whose operands the evaluator evaluates before the primitive itself acts
(`car`, `+`, `set`, `funcall`, `apply`, ...) -- and false for a macro, for a
special form whose operands stay raw (`if`, `quote`, `let`, `lambda`,
`function`, ...), and for a value that is not callable at all. A symbol
argument is resolved through the same designator chain `FeGetFunction()`
follows, so this asks about the symbol's binding rather than about the symbol:
an unbound name is false, and a self-referential chain raises
`cyclic-function-indirection` -- where `FeGetFunction()` answers `nil`, so a
host that must not be raised at resolves with that first and asks this about
the result. The predicate is the same
classification `funcall`/`apply` reject an `invalid-function` operand by, so a
host `functionp` built on it and the interpreter cannot disagree. It is an
additive C entry point: no version bump, per the policy above.

## Reading And Running Source

`FeReadString()` reads one form from an explicitly sized byte sequence. The
input need not be NUL-terminated and Fe never reads at or beyond `length`.
When `offset` is non-null, it supplies the zero-based starting position and is
updated to the first byte not belonging to the returned form; pass the same
value back to read subsequent forms. A null `offset` starts at byte zero.
The function returns `nullptr` when no form remains. `source` may be `nullptr`
only when `length` is zero.

Fe source is textual rather than binary. An embedded NUL within the supplied
length is an error, including a NUL between otherwise valid forms; it is never
treated as end-of-input. Reader diagnostics from `FeReadString()` use the form
`byte <offset>: <message>` with zero-based byte offsets.

`FeEvaluateString()` evaluates every top-level form and returns the last
value, or nil when the input contains no forms. It accepts the same explicitly
sized, non-NUL-terminated input and rejects embedded NUL bytes.
`FeEvaluateFile()` does the same from the `FILE` object's current position
through end-of-file. It neither closes nor rewinds the stream, so ownership
remains with the caller. File read errors and embedded NUL bytes are reported
through the normal Fe error callback.

Both evaluation helpers restore the GC stack to its entry index between forms
and before returning. Their returned value is held by a context-owned root,
which is replaced by the next call to either helper. The result therefore
survives allocations without growing the GC stack until another string or file
evaluation begins, or until the context is closed.

During either evaluation helper, both reader and evaluator errors are
reported as `<label>:<line>: <message>`, with one-based lines counted from
the start of the input; an evaluator error names the line the failing
top-level form *starts* on, which is what the reader latches after each
form. Outside an input unit, where there is no label, a reader diagnostic
falls back to `byte <offset>: <message>`. File offsets start at zero at the
stream position passed to `FeEvaluateFile()`. Passing `nullptr` as the label
disables the label prefix. Fe borrows the label only for the duration of the
call and does not retain its pointer -- unlike `FeEnterInputUnit()`, which
borrows it for the unit's whole lifetime. Before invoking the error callback,
Fe copies the composed diagnostic into its temporary error-message buffer; as
with other error messages, that borrowed message is valid only during the
callback.

### Input units, and what they scope

Each `FeEvaluateString()`/`FeEvaluateFile()` call is an INPUT UNIT: a source
label its diagnostics carry, a position within that source, and -- since
`FE_LANGUAGE_VERSION` 10 -- the scope a one-argument `internal--mark-special`
mark made inside it belongs to. Units nest, so a load reached from inside a
load gets a unit of its own and the outer one comes back when it returns.

The consequence a host has to know about is the scoping one. Two
`FeEvaluateString()` calls on one context are two units, so a
let-dynamic-only mark made in the first does not make `let` dynamic in the
second; it is a *unit*-scoped mark, not a context-scoped one. Full marks are
global and unaffected. Outside any unit -- `FeCall()` into a callable, a
host-driven `let`, `SymbolIsLetDynamic` asked from C -- every mark is
visible, so a host's own hooks and callbacks see the names a loaded file
declared.

The unit is entered and left as a whole, and it is put back on every exit:

* a NORMAL return restores the enclosing unit;
* a CONTAINED abnormal exit -- `FeTryCallWithOptions()`,
  `FeTryEvaluateStringWithOptions()` -- restores the enclosing unit at the
  barrier, which is how an outer load keeps its own marks and its own file
  name after an inner one raised;
* an UNCONTAINED abnormal exit, one that reaches the host `error_fn`, leaves
  for the HOST CONTEXT rather than restoring anything: no unit, no label.
  Every unit between the raise and the host is being abandoned at once, so
  there is nothing to go back to. Cleanups registered inside the abandoned
  units run first, during the drain, so a host that keeps bookkeeping of its
  own still gets to unwind it.

### Driving An Input Unit Yourself

`FeEnterInputUnit()`, `FeReadInputForm()` and `FeLeaveInputUnit()`
(`FE_API_VERSION` 8) hand the unit to a host that wants to run the read-eval
loop itself. The shape they exist for is a `load` written in Lisp:

```lisp
(defun load (path)
  (let ((h (host-open path)))          ; FeEnterInputUnit
    (unwind-protect
        (let ((cell (host-read h)))    ; FeReadInputForm
          (while cell
            (eval (car cell))          ; the CURRENT run, not a nested one
            (setq cell (host-read h))))
      (host-close h))))                ; FeLeaveInputUnit
```

`FeEnterInputUnit()` takes the next scope number, publishes `label`, and sets
the position to line 1. It starts no run and drives no reader: it is a state
change on the context and nothing else, which is exactly what lets the forms
be evaluated by `eval` -- in the run the loop is already inside. So a
`condition-case` or a `catch` established *outside* the loop receives a
condition, a `throw` or a quit raised by a loaded form, which is the property
no earlier composition could give. `FeEvaluateString()` starts a nested run
per form, so a raise from a loaded form transfers to the outermost barrier,
past every handler between the load and the raise;
`FeTryEvaluateStringWithOptions()` contains that raise but is deliberately a
throw wall. Measured, with the same loop driven both ways:

| the loop evaluates with | `(condition-case e (load ...) (error ...))` | `(catch 'tag (load ...))` |
| --- | --- | --- |
| `eval` (this API) | receives | receives |
| a per-form nested run | escapes to the host | `no-catch tag` to the host |

`label` is borrowed for the unit's whole lifetime, so it must outlive the
matching `FeLeaveInputUnit()`; storage the host owns per nesting level is the
natural place for it. `FeInputUnit` is the enclosing unit, written into
caller-owned storage by the enter and handed back to the leave; its fields
are declared only so a caller can allocate one, and nothing outside Fe should
read or write them. Nesting works because the host keeps one token per level.

`FeReadInputForm()` reads one form and publishes the line that form *starts*
on as the position an error raised while evaluating it will report.
`FeReadString()` cannot be used for this: it saves and restores the label and
position around itself, so a host looping over it publishes neither, and its
line counter restarts at 1 on every call. `offset` and `line` are the
caller's cursor over one source -- both in/out, both required -- initialised
to 0 and 1 and handed back unchanged for each successive form; `offset` ends
at the first byte not belonging to the returned form. It returns `nullptr`
when no form remains. `source` may be `nullptr` only when `length` is zero,
Fe never reads at or beyond `length`, and an embedded NUL inside that length
is an error rather than end-of-input, exactly as for `FeReadString()`. The
returned form has no root of its own, as `FeRead()`'s and
`FeReadString()`'s do not: push it before allocating anything else.

`FeLeaveInputUnit()` restores the enclosing unit from the token. The unwind
guarantee above applies unchanged, which is why the sketch leaves from an
`unwind-protect` cleanup: a cleanup runs during the drain, before either the
containment barrier's restore or the host exit, so the host's own per-level
bookkeeping unwinds with it. A host that skips the leave on an abnormal path
is not left with a stale unit -- fe puts the unit back or drops it either way
-- but it is left with its own state to reconcile.

The lower-level `FeRead()` and `FeReadFile()` APIs remain available for
streaming embedders and fuzzing. To evaluate such a stream manually, read and
evaluate one form at a time:

```c
FILE* file = fopen("test.fe", "rb");
size_t gc = FeSaveGC(ctx);
while (true) {
  FeObject* obj = FeReadFile(ctx, file);
  // Break if there's nothing left to read.
  if (obj == nullptr) {
    break;
  }
  // Evaluate read object.
  FeObject* result = FeEvaluate(ctx, obj);
  (void)result;

  // Restore GC stack which would now contain both the read object and
  // the result from evaluation.
  FeRestoreGC(ctx, gc);
}
fclose(file);
```

## Bounding And Cancelling Evaluation

`FeEvaluateWithOptions()`, `FeEvaluateStringWithOptions()`, and
`FeEvaluateFileWithOptions()` are the controlled counterparts of the plain
evaluation functions. The string and file variants retain the same input,
label, ownership, final-result rooting, and multi-form behavior described
above.

```c
FeEvalOptions options = {
  .step_limit = 100000,
  .poll_interval = 256,
  .interrupt = ShouldCancel,
  .userdata = host,
};
FeObject* result = FeEvaluateStringWithOptions(
    ctx, "init.fe", source, source_length, &options);
```

`step_limit` is the maximum number of evaluation steps; zero means unlimited.
The first `step_limit` steps are allowed, and attempting another raises
`evaluation step limit exceeded`. Fe counts every internal evaluator entry,
including atoms and callable forms, every element processed by evaluator list
loops, every lexical environment and parameter-binding entry traversed, and
every successful `while` iteration. Function calls therefore count their
call-form entry, evaluated arguments, parameter binding, and body forms. Macro
expansion counts the macro call, body forms, and the generated form's re-entry
into the evaluator. This accounting is deterministic for identical input and
Fe version, but hosts should treat limits as work bounds rather than portable
instruction counts because the granularity may change in a later version.

When `interrupt` is non-null, Fe calls it after each `poll_interval` evaluation
steps. A zero interval selects the default of 1024 steps. Returning `true`
raises the distinct `evaluation cancelled` error. Polling uses a decrementing
counter and performs no clock calls. Fe borrows the callback and its `userdata`
for the controlled call; both must remain valid until it returns or transfers
control through the error callback. Cancellation is cooperative at evaluator
step boundaries: a native callback that does not return cannot be preempted by
Fe.

### Bounding Recursion: Two Bounds, Not One

Sub-plan 03F of kg's Emacs-subset program split the single, historical
`max_depth`/`evaluation_depth` pair into two independent bounds, because they
protect two different things:

- **`max_frames`** bounds Lisp nesting: the number of simultaneously live
  ordinary evaluator frames on the context-owned frame stack -- nested calls,
  nested special forms, self-expanding macros, deep argument lists. The frame
  machine (sub-plan 03C-03E) roots every one of those in the arena, not in C
  recursion, so **this costs no C stack no matter how large it is**. Zero
  selects the arena's own physical frame capacity
  (`FeGetArenaStats().frame_capacity`); a nonzero value only ever *lowers*
  that ceiling, never raises it past what the arena partition actually holds.
  A push that would exceed the effective limit fails before writing, raising
  `evaluation frame limit exceeded`.
- **`max_native_reentry`** bounds native re-entry: the number of nested
  evaluator runs a native may start synchronously, one below another --
  e.g. a native that calls `FeCall`/`FeCallWithOptions`/`FeEvaluate*` on a
  callback or body it was handed, the way `unwind-protect`'s own cleanup
  mechanism, or a host's `with-current-buffer`-shaped native, does. Unlike
  `max_frames`, this **is** a real C-stack bound: each level is a live native
  C activation, `FeCall`, `Evaluate`, and the nested run's own barrier, none
  of which the frame machine can move off the C stack. Zero selects the
  built-in default (`DefaultNativeReentry`, a small number, derived from the
  deepest synchronous re-entry any known embedding actually nests, times a
  comfortable safety margin). Exceeding it raises `native evaluation
  re-entry limit exceeded` before the nested run starts.

Calling a native from Lisp is **not**, by itself, re-entry -- only that
native *synchronously starting another evaluation* is. An ordinary top-level
host call therefore never counts against `max_native_reentry`, no matter how
deep the Lisp nesting it drives; only a native, invoked during that
evaluation, that turns around and starts a nested run moves the counter.

```c
FeEvalOptions options = {
  .step_limit = 100000,
  .poll_interval = 256,
  .interrupt = ShouldCancel,
  .userdata = host,
  .max_frames = 0,             // this run's own physical capacity
  .max_native_reentry = 4,     // tighter than the built-in default
};
FeObject* result = FeEvaluateStringWithOptions(
    ctx, "init.fe", source, source_length, &options);
```

Both bounds are ambient for the complete outermost `*WithOptions()` call, the
same way `step_limit` and `interrupt` are. Evaluation re-entered from a
native callback through either a plain evaluator or another controlled
evaluator is bound by the same outer `max_frames`/`max_native_reentry`
values; options supplied to a nested controlled call are ignored -- nested
code cannot reset or extend either active ceiling. This includes all forms
in nested string and file evaluation. Passing `nullptr` as the outermost
options pointer is equivalent to zero-initialized options and still
establishes an ambient control scope (both bounds at their built-in
defaults) whose nested options are ignored.

The plain `FeEvaluate()`, `FeEvaluateString()`, and `FeEvaluateFile()`
functions remain unlimited on `step_limit` and `interrupt` when no ambient
control is active, but `max_frames` and `max_native_reentry` are not opt-in
the way those are: every frame push and every nested run is checked against
its effective ceiling (the arena's own physical capacity, or
`DefaultNativeReentry`, when nothing more specific was configured)
regardless of whether a host ever calls a `*WithOptions()` function at all --
the point being that native re-entry's C-stack bound in particular protects
the process whether or not a host ever thinks about evaluation control.
Successful return from the outermost controlled call clears the *ambient
limits* (`max_frames`/`max_native_reentry` revert to their built-in
defaults for the next call), but **not** `native_reentry_depth` itself:
that counter is a census of live C activations, and while an error is
unwinding -- before any `longjmp` has actually popped a single one of those
C frames -- every native activation the abandoned computation was inside is
still real and still live on the C stack, so a cleanup native that itself
re-enters evaluation is stacking a fresh C frame on top of all of them and
the check needs the true count to stay meaningful. It falls back to the
correct value only as the unwind actually happens, one nested run's own
barrier at a time -- see "Unwinding And Cleanup" below. The exhaustion and
cancellation messages pass through normal label handling; for example, a
controlled string call labelled `init.fe` reports `init.fe: evaluation
cancelled`, `init.fe: evaluation frame limit exceeded`, or `init.fe: native
evaluation re-entry limit exceeded`.

## Calling A Function

`FeCall()` invokes an `FeTFn` or `FeTNativeFn` with already-evaluated argument
values. It protects the callable and every argument while constructing its
internal call form, so a list argument is passed as the list itself and is not
evaluated as code. Macros and primitives are not supported by this API; passing
one, or any other non-callable value, raises `tried to call non-callable value`.

```c
size_t gc = FeSaveGC(ctx);
FeObject* function = FeEvaluateString(
    ctx, "host", "(lambda (x y) (+ x y))",
    sizeof("(lambda (x y) (+ x y))") - 1);
FeRoot* root = FeCreateRoot(ctx, function);
FeObject* arguments[] = {
    FeMakeDouble(ctx, 10),
    FeMakeDouble(ctx, 20),
};
FeObject* result = FeCall(ctx, FeGetRoot(root), arguments, 2);
printf("result: %g\n", FeToDouble(ctx, result));
FeReleaseRoot(ctx, root);
FeRestoreGC(ctx, gc);
```

On normal return `FeCall()` restores its internal GC-stack frame, so repeated
calls do not grow the stack. Its result is held by a context-owned root until
the next `FeCall()` or context close. Create a persistent root before the next
call if the result must live longer. When called during an ambient controlled
evaluation, the internal invocation consumes the existing step budget and
uses its interrupt settings. Normal Fe errors and nonlocal recovery rules are
unchanged.

`FeCallWithOptions()` is the controlled counterpart: it takes the same
callable, argument array, and count as `FeCall()`, plus a `FeEvalOptions*`.
Its semantics are exactly those of the other `*WithOptions()` entry points —
when no evaluation is active it establishes a fresh step budget and interrupt
scope, and when called during an active evaluation it is ambient: the outer
budget and interrupt settings apply and its own options are ignored. The
callable is invoked through `FeCall()`, so GC protection, argument handling,
error signalling and the internal GC-stack frame are identical. A host that
invokes a rooted callable under a budget should prefer this to evaluating a
source-string trampoline, which only exists to reach the same accounting.

## Unwinding And Cleanup

`FeProtectWithCleanup()` registers a C cleanup that runs exactly once,
whatever way the call form currently being evaluated finishes: an ordinary
return, a Lisp error, a host interrupt, or step-budget exhaustion. It shares
one registry, and one last-in-first-out order, with Lisp `unwind-protect`
(`doc/language.md`) -- the two interleave correctly when a host cleanup
wraps a Lisp body that itself uses `unwind-protect`.

```c
typedef void FeCleanupFn(FeContext* ctx, void* data);
void FeProtectWithCleanup(FeContext* ctx, FeCleanupFn* fn, void* data);
```

Call it from within an active evaluation -- concretely, from a native
function that is about to hand a body it was given to `FeCall()` or
`FeEvaluate()`, the same shape `unwind-protect` itself uses internally.
`fn` runs when the nearest enclosing call form finishes: normally that is
the native's own call, since that is the form still being evaluated while
the native is running.

```c
static void CloseFileCleanup(FeContext* ctx, void* data) {
  (void)ctx;
  fclose(data);
}

// (with-open-file THUNK): opens a fixed path, protects it, calls THUNK.
static FeObject* WithOpenFile(FeContext* ctx, FeObject* args) {
  FeObject* thunk = FeGetNextArgument(ctx, &args);
  FeRequireNoArguments(ctx, args);
  FILE* file = fopen("state.dat", "r");
  if (file == NULL) {
    FeHandleError(ctx, "could not open state.dat");
  }
  FeProtectWithCleanup(ctx, CloseFileCleanup, file);
  return FeCall(ctx, thunk, nullptr, 0);
}
```

`fn` must not fail, must not call back into the evaluator, and must not
create Fe objects; it may free non-Fe resources and call plain C or
extension-internal functions. A cleanup pushed with `FeProtectWithCleanup()`
but never reached by a normal return -- for example, one registered directly
from host code outside of any active evaluation, rather than from within a
native -- has no enclosing call form to drain it, so only a later error
still runs it. There is no cancellation: call it only once ownership of the
resource is final. The registry is a fixed-size array sized like the GC
stack; exceeding it raises `"cleanup stack overflow"` before `fn` or `data`
are recorded, so nothing has been allocated through this call that the
caller must now release itself.

### The cleanup budget

Cleanup does not run under the budget the body was using: by the time a
cleanup runs, that budget may be exactly what ran out. Nor does it run
unbounded, which would leave a host with no way to escape a runaway
cleanup. Each entry gets a **fresh** budget of
`FeEvalOptions.cleanup_step_limit` evaluator steps, or 4096 when that field
is left 0.

`interrupt`, `userdata` and `poll_interval` stay live for the drain and are
re-armed per entry, so the interrupt that caused the unwind does not
immediately abort its own cleanup, and a *second* interrupt during a
runaway cleanup aborts that one entry while the remaining cleanups still
run.

Both recursion bounds get a fresh *ceiling* the same way, but the two differ
in what "fresh" means for their live counters. `FeHandleError()` resets the
ambient `max_frames`/`max_native_reentry` *limits* to 0 (their built-in
defaults) before it drains the cleanup registry, so a cleanup that itself
nests Lisp forms or re-enters through a native is checked against the full
default ceiling, not whatever tighter body limit the abandoned computation
was configured with. For frames this is the whole story: a cleanup's own
frame pushes get `frame_stack_capacity` slots (plus the private cleanup
reserve) to work with, exactly as a top-level call would. For native
re-entry it is not: `native_reentry_depth` -- the live count of C
activations, not the configured ceiling -- is **not** reset by
`FeHandleError()`. The abandoned computation's own native activations are
still real and still live on the C stack while cleanups run (nothing has
`longjmp`ed yet), so a cleanup native that itself re-enters evaluation is
stacking a fresh C frame on top of all of them, and resetting the count to 0
would let it re-enter far deeper than the C stack actually has room for. The
count falls back to its correct, lower value only as the unwind actually
happens afterward, one nested run's own barrier at a time -- see "Bounding
Recursion" above.

The worst case a host must be able to tolerate is therefore

    CleanupStackSize (256) x cleanup_step_limit

evaluator steps between the moment an error is raised and the moment
`error_fn` sees it -- 1,048,576 steps at the default, and bounded rather
than open-ended even if every cleanup on a full registry misbehaves. A host
that cannot afford that pause sets `cleanup_step_limit` lower; one running
cleanups that legitimately do more work sets it higher.

A cleanup that itself raises replaces the completion being unwound. Remaining
cleanups still run inner to outer, and `error_fn` receives the replacement
condition rather than the abandoned completion.

### Completion kinds

Every abnormal way out of a form is a **completion**, and the context
remembers which kind it was. `FeCompletion` has five values:

| Kind | Producer |
| --- | --- |
| `FeCompletionNormal` | a form produced a value; the default, and the value after any normal top-level return |
| `FeCompletionError` | every ordinary `FeHandleError()` call |
| `FeCompletionQuit` | the interrupt callback returned true ("evaluation cancelled"), or `(signal 'quit nil)` |
| `FeCompletionBudget` | the step limit, the frame wall, or the native re-entry wall was hit |
| `FeCompletionThrow` | a Lisp `throw` that has found its catch, for the duration of the drain to it |

Four of the five can reach a host. `FeCompletionThrow` is the exception: it
is assigned only between a matching `catch` frame being found and the value
being delivered to it, so a host observes it just once -- from inside a
cleanup that runs on the throw's own unwind path -- and never at a barrier
or in `error_fn`. A throw with no matching catch is not this kind at all; it
raises the condition `(no-catch TAG VALUE)` and arrives as
`FeCompletionError`.

```c
FeCompletion FeGetCompletion(const FeContext* ctx);
FeObject* FeGetCondition(const FeContext* ctx);
const char* FeGetCompletionMessage(const FeContext* ctx);
```

`FeGetCompletion()` is Decision 5's additive migration path: a host telling
quit from a genuine error reads the kind instead of comparing message
strings, and `FeErrorFn`'s signature is unchanged, so every existing host
compiles and behaves as before without edits. The kind is always valid --
it is assigned before the cleanup drain and before `error_fn` runs, and it
stays readable after the host's recovery `longjmp` until the next run's
outermost barrier or a normal top-level return resets it to
`FeCompletionNormal`.

`FeGetCondition()` returns the completion's `(SYMBOL . DATA)` object, and it
is deliberate for every kind rather than left over from an earlier one. An
error carries the condition that was signalled (`(wrong-type-argument listp
5)`, `(error "boom")`); a quit -- including a real host interrupt, not only
`(signal 'quit nil)` -- carries `(quit)`; a budget completion carries `nil`,
because fe's own ceilings are not Emacs conditions and have nothing to
construct.

An exhausted arena cannot build a condition object, and used to answer `nil`
for that reason -- which meant `condition-case` could match it with `(t ...)`
and nothing else. Since sub-plan 09B it carries one of two objects
`FeOpenContext()` interns and conses once, before any host code runs, and
roots for the context's whole life:

- `(arena-exhaustion)` -- the object list is full and a collection cannot
  refill it. Every `out of memory` raise carries it, and so does any *named*
  condition that could not be built because the arena was full: the handler
  is told truthfully that the raise became an exhaustion rather than being
  handed nothing. The raise's own message is unchanged.
- `(evaluation-stack-exhaustion)` -- the GC root stack overflowed.

Both are `error`'s children in the condition hierarchy, so
`(condition-case e BIG (error ...))` catches either, and each is also
catchable by its own name. A quit raised while the arena is full still
carries `nil`: `condition-case` decides a quit by completion *kind* before it
looks at the object, so `(quit ...)` catches it either way, and an interrupt
is not an exhaustion.

Both objects are shared, and a handler is handed the object itself, not a
copy. Every raise therefore re-stamps the condition -- symbol back in the
`car`, `nil` back in the `cdr` -- before publishing it. Two stores and no
allocation, so it is still safe on the path where allocation has already
failed, and it is what keeps a `setcar`/`setcdr` from a handler out of the
*next* exhaustion: without it, `(condition-case e BIG (error (setcar e
'poisoned)))` permanently disabled the mechanism, since `ConditionMatches()`
reads the condition's `car` to place it in the hierarchy, and
`(setcdr e (list 9 9 9))` made the next unrelated exhaustion signal
`(arena-exhaustion 9 9 9)` with that list rooted for the life of the context.
A caught condition is still yours to read; if you want to keep or mutate one,
copy what you need out of it, exactly as with any other condition whose
lifetime ends at the next completion.

The object lives in the context (`ctx->condition`) and is a GC root for as
long as it is there, so it stays valid across the cleanup drain, inside
`error_fn`, and after the host's recovery `longjmp` -- until the next
completion replaces it or the next outermost run barrier resets it to `nil`.
A host that needs it beyond that must copy what it wants out of it, or root
it with `FeCreateRoot()`.

`FeGetCompletionMessage()` returns the completion's fully formatted message
-- source label and all, the same string `error_fn` is handed. It is valid
until the next completion in this context.

### Raising a completion from the host

```c
[[noreturn]] void FeHandleError(FeContext* ctx, const char* msg);
[[noreturn]] void FeRaiseCompletion(FeContext* ctx, FeCompletion kind,
                                    const char* msg);
```

`FeHandleError()` is the ordinary raise and what nearly every host site
wants: an `FeCompletionError` whose condition object is `(error "msg")`.

`FeRaiseCompletion()` exists for the two kinds `FeHandleError()` cannot
spell. A host raises `FeCompletionQuit` when its own C-g arrived somewhere
fe cannot poll -- the condition object is `(quit)`, so an enclosing
`(quit ...)` handler catches it exactly as it catches the interrupt
callback's -- and `FeCompletionBudget` when its own ceiling tripped, which
no `condition-case` may catch.

The other two kinds are not a host's to raise. `FeCompletionNormal` is not a
raise, and `FeCompletionThrow` is the evaluator-internal state described
above: raising it from outside would claim a delivery to a catch frame that
never happens. Both are asserted against, and treated as an ordinary error
when assertions are compiled out, because a `[[noreturn]]` function has no
way to return and report a bad argument.

### Containing a completion: the protected call

```c
bool FeTryCallWithOptions(FeContext* ctx, FeObject* callable,
                          FeObject* const* arguments, size_t count,
                          const FeEvalOptions* options, FeObject** result);
bool FeTryEvaluateStringWithOptions(FeContext* ctx, const char* label,
                                    const char* source, size_t length,
                                    const FeEvalOptions* options,
                                    FeObject** result);
[[noreturn]] void FeResignal(FeContext* ctx);
```

`FeCall()` and `FeCallWithOptions()` transfer a completion by `longjmp`, and
the destination is the *enclosing* run's barrier -- past the C frame of any
native that started the call. A host that puts its own `setjmp` inside such
a native is therefore longjmping back into a frame that has already been
unwound, which is undefined. `FeTryCallWithOptions()` is the entry point for
that case: its `setjmp` lives inside fe, in a frame that is live for exactly
as long as the call.

It returns `true` when the call completed normally, with the value in
`*result`. It returns `false` on any other completion, and then:

- `error_fn` was **not** called and the host's frame was **not** unwound;
- `FeGetCompletion()`, `FeGetCondition()` and `FeGetCompletionMessage()`
  describe what happened;
- the callee's `unwind-protect` and `FeProtectWithCleanup()` cleanups have
  run, down to the depth this call started at and no further, so the
  caller's own pending cleanups are untouched;
- the callee's evaluator frames and GC-stack entries are gone, and the
  caller's ambient evaluation-control record -- remaining steps included --
  is exactly as it was;
- a `throw` cannot escape: the barrier is also a catch wall, so an unmatched
  throw inside the call becomes `no-catch` there;
- a completion already unwinding *outside* the call is untouched. Containment
  is reachable from inside an enclosing completion's cleanup drain -- an
  `unwind-protect` form, or a `FeProtectWithCleanup()` entry, that calls a
  host native which contains -- and the enclosing completion's kind and
  condition object survive it, so an enclosing `condition-case` still binds
  the object that was in flight and a host that the enclosing completion
  reaches still reads it from `FeGetCompletion()` and `FeGetCondition()`.
  What a contained completion replaces is what a *cleanup's own* raise
  replaces, and it is not one: an uncontained raise inside a cleanup does
  still replace the completion being unwound, which is the long-standing
  rule for cleanups and is unchanged.

`options` bounds this call rather than being ignored in favour of an ambient
record, which is the difference from `FeCallWithOptions()` when called from
inside a native; a null `options` leaves the caller's ambient limits in
place.

The host then chooses. **Containment** -- a hook, a process filter, a
sentinel -- swallows it and carries on; that is the whole point, since a
broken hook in a user's init file must not take the editor's own evaluation
down with it. **Wrapping** -- a native that runs a body between a save and a
restore -- does its restoring work and then calls `FeResignal()`, which puts
the completion back in flight in the enclosing run with its kind, its
condition object and its already-formatted message intact, so an enclosing
Lisp `condition-case` matches on the *original* condition symbol.

```c
static FeObject* CallHook(FeContext* ctx, FeObject* arguments) {
  FeObject* hook = FeGetNextArgument(ctx, &arguments);
  FeObject* value = FeNil(ctx);
  const FeEvalOptions options = {.step_limit = 100000};
  if (FeTryCallWithOptions(ctx, hook, nullptr, 0, &options, &value)) {
    return value;
  }
  fprintf(stderr, "hook failed: %s\n", FeGetCompletionMessage(ctx));
  return FeNil(ctx);  // or FeResignal(ctx) to re-raise it
}
```

`FeTryEvaluateStringWithOptions()` (`FE_API_VERSION` 7, sub-plan 11C) is the
same contract over *text* instead of a callable: every bullet above applies
to it word for word, with `*result` holding the value of the last form on the
`true` path. That value is rooted exactly as `FeEvaluateString()`'s is --
by the context-owned root described under "Evaluating input", replaced by
the next string or file evaluation -- and `FeTryCallWithOptions()`'s is
rooted the same way by its own. Neither result needs a GC-stack slot, and
neither is freed by a collection the host causes between the call returning
and the value being used, which is what makes the `Load` shape below (unwind
first, then hand the value back) safe. It exists because a host that **loads** Lisp from inside an
evaluation has the problem the paragraph above describes, and could not fix
it the same way: `FeEvaluateString()` is a nested run dressed as a top-level
call, so a completion raised by the loaded text transfers to the outermost
barrier -- past every `condition-case` between the load and the raise.

```c
static FeObject* Load(FeContext* ctx, const char* path,
                      const char* text, size_t length) {
  FeObject* value = FeNil(ctx);
  if (FeTryEvaluateStringWithOptions(ctx, path, text, length,
                                     nullptr, &value)) {
    return value;
  }
  UnwindLoaderBookkeeping(ctx);  // while this frame is still live
  FeResignal(ctx);               // now an enclosing condition-case sees it
}
```

Two things the string variant does not change. Forms *before* the raising one
have already run and their side effects stand -- containment is not a
transaction. And a `throw` out of the loaded text is contained as the
barrier-wall `no-catch` error it already is, so a `catch` established outside
the loaded text is still not reached; that is a recorded divergence from
Emacs rather than a property of this entry point, and closing it would need
`load` to be an fe primitive with a frame kind of its own.

`example_host.c` exercises both the accessors and the protected call.

### The standalone interpreter's structured channel

The `fe` binary is a host like any other, and its error callback prints the
message to `stderr`. Setting `FE_STRUCTURED_ERRORS` to any value makes it
print the completion's condition object first, so a machine reader (the
compat runner, `utils/run-fe-compat.py`) does not have to grep diagnostic
text:

```text
condition: wrong-type-argument
data: (listp 5)
error: expected pair, got integer
```

`condition:` is the condition symbol -- the car of `FeGetCondition()` -- and
`data:` on the next line is the cdr, rendered exactly as Emacs'
`prin1-to-string` renders it, so the two can be compared character for
character. Since sub-plan 11C that is true without qualification: the one
remaining printer difference over data a condition can carry was
`(quote X)`, which Emacs abbreviates to `'X` and fe wrote out in full, and
the writer now abbreviates it too. The compat corpus's
`primitive-quote-too-many` case, which used to opt out of comparing its
data for exactly that reason, compares it. A quit is the one special case: it prints `condition: quit` and
no `data:` line, because a quit is a completion kind a host may need to tell
apart from every condition, not a condition to be inspected. A completion
with no condition object at all (budget exhaustion) prints
neither line, only `error:`; an arena or GC-stack exhaustion prints its own
`condition:`/`data:` lines like any other error. The variable changes nothing
else: the `error:` line and the call trace are printed exactly as they are
without it, and the exit status is unchanged.

The kind also has one internal effect a host can rely on: while a
non-Normal completion is draining, a cleanup's own frame pushes get the
private `CleanupFrameReserve` of extra slots, so a cleanup provoked by
frame exhaustion is not refused by the same wall the body just hit. That
coupling used to be free -- only `FeCompletionError` was ever assigned --
and is now load-bearing, since the frame wall assigns `FeCompletionBudget`.

## Serializing Objects

`FeWrite()` renders an object as Fe syntax one character at a time through a
caller-supplied `FeWriteFn`; `FeWriteFile()` is the `FILE*` wrapper. `qt`
selects quoted rendering, in which strings are surrounded by `"` and embedded
`"` characters are escaped.

Rendering is bounded, and always terminates:

```c
typedef struct FeWriteOptions {
  size_t max_bytes;
  size_t max_nodes;
  size_t max_depth;
} FeWriteOptions;

bool FeWriteWithOptions(FeContext* ctx, FeObject* obj, FeWriteFn fn,
                        void* udata, int qt, const FeWriteOptions* options);
```

A zero field takes the default, and a null `options` takes all three.
`FeWriteWithOptions()` returns `true` when the whole object was rendered and
`false` when it stopped early; `FeWrite()` is the same call with default
options and the answer discarded.

Three things can stop it, each with a marker in the output so a truncated
rendering is not mistaken for a complete one:

| Marker | Cause |
| --- | --- |
| `#<cycle>` | the list spine returns to a pair already on it |
| `#<deep>` | `max_depth` levels of `car` nesting |
| `#<truncated>` | `max_bytes` or `max_nodes` exhausted |

The spine is walked iteratively with two pointers, so a cycle costs no
allocation, no visited set, and no depth: `(setcdr x x)` renders as
`(1 . #<cycle>)`. Only `car` nesting spends depth, and shared acyclic
structure is therefore rendered in full every time it appears rather than
being reported as a cycle. The byte and node budgets are backstops against
structure that is finite but unreasonable.

The writer does not allocate. Closures and macros used to be printed by consing
their head onto their body, which meant printing could collect and raise `out of
memory` part way through -- a `longjmp` out of a caller that was holding a
destination buffer. They are now written directly.

While a controlled evaluation is active, rendering spends its step budget and
polls its interrupt callback, so printing a large object answers a host's
cancellation the way evaluating one does. `FeWriteFn` returns `void`, so a
failing write callback cannot report itself; `FeWriteFile()` correspondingly
does not detect `fputc` failures.

`FeToString()` renders into a caller-supplied buffer:

```c
size_t FeToString(FeContext* ctx, FeObject* obj, char* dst, size_t size);
```

- `size == 0` writes nothing at all, renders nothing, and returns 0. `dst` may
  be null in that case, and it is the only case in which it may be.
- `size > 0` requires a writable buffer of at least `size` bytes. At most
  `size - 1` rendered bytes are stored and a NUL terminator always follows
  them.
- The return value is the number of bytes stored, not counting the terminator.
  It never exceeds `size - 1`.

The rendering is therefore truncating, not measuring: a return value equal to
`size - 1` means the output may have been cut short, and there is no
`snprintf`-style "required length". Computing one would mean walking the whole
object graph, which for a cyclic object is work with no useful answer.
`FeToString()` passes its destination size as `max_bytes`, so rendering a
cyclic object into a small buffer costs that buffer's worth of work. Use `FeStringByteLength()` and `FeCopyStringBytes()` when
the goal is to extract a string's bytes rather than to display an object.

## Extending The Core

For examples of using the extension API in full detail, refer to `fex.[ch]` and
`fex_*.[ch]`. Here is an overview.

kg-style embedders should use the context-based lifecycle, userdata, error,
evaluation-control, native-binding, extraction, root, and call APIs described
above. In particular, use `FeNil()` rather than `nil`, `FeDefineNative()` rather
than assembling a binding manually, and `FeStringByteLength()` plus
`FeCopyStringBytes()` rather than treating serialized output as host data.

### Exposing A C Function

`FeDefineNative()` creates a `FeNativeFn` and binds it to a global symbol while
balancing all temporary GC protection. Native callbacks take a context and a
list of already-evaluated arguments and return an `FeObject*`. The result must
never be `nullptr`; use `FeNil(ctx)` to return nil.

Since sub-plan 04D's namespace cut, the native binding lands in the symbol's
*function* cell -- the bootstrap lives there, so call position reaches it
directly, and `(symbol-function 'name)` observes it. Before the cut the
binding went to the value cell and call position reached it through the
transitional fallback; `FeDefineNative`'s new home is the meaning change the
`FE_API_VERSION` 3 bump carried (05D's numeric cut then moved both versions
to 4, 06D's condition cut to 5, and 07B's strict-arity cut to 6).

Consume required arguments with `FeGetNextArgument()`, which raises `too few
arguments` for a missing value. After consuming the supported arguments, call
`FeRequireNoArguments()`; it raises `too many arguments` if anything remains.
Together these helpers enforce exact arity.

You could expose the `pow` function from `math.h` like so:

```c
static FeObject* Power(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  double y = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  FeRequireNoArguments(ctx, arg);
  return FeMakeDouble(ctx, pow(x, y));
}

FeDefineNative(ctx, "pow", Power);
```

You can then call the `FeNativeFn` from Fe like any other function:

```clojure
(print (pow 2 10))
```

### Extracting String And Symbol Bytes

`FeStringByteLength()` accepts a string or symbol and returns the exact number
of stored text bytes. `FeCopyStringBytes()` accepts the same types and copies
those bytes without quoting, escaping, serialization, or a trailing NUL. A
buffer whose size is exactly the reported length succeeds. If the buffer is
short, or is null for a non-empty value, the function returns `false` without
writing anything. Other object types raise an `expected string or symbol`
type error.

The extraction APIs are byte-counted and walk every chained string cell; they
do not have `FeToString()`'s fixed-buffer serialization semantics.
`FeToString()` renders any object as Fe syntax, including string quoting and
escaping, and its destination may truncate; it is for display, not extracting
host data. Fe source still rejects embedded NUL bytes, and `FeMakeString()`
accepts a NUL-terminated C string, so the current public construction paths
cannot create a string containing an embedded NUL. Extraction nevertheless
reports and copies the exact stored payload rather than treating its
destination as a C string.

### Legacy Fex Custom Types

The public `FeTFex0` through `FeTFex2` tags, mutable `type_names` array, and
`nil` object remain only for compatibility with the in-tree Fex extensions.
They are process-global legacy interfaces, are not safe as per-context type
registrations, and are slated for removal when Phase 8 introduces per-context
custom types. New kg-style embedders must not use them.

The in-tree file extension shows what such a type has to do. An `FexTFile` cell
does not hold a bare `FILE*`; it holds a heap record with the stream, whether
Fex owns it, and whether it has been closed. Every native validates the type and
the live state before touching the stream, `close-file` marks the record closed
before calling `fclose` so a failed close cannot hand the stream back to stdio,
closing twice and reading after close are errors rather than undefined
behaviour, and `stdin`/`stdout`/`stderr` are marked unowned so Lisp cannot close
the host's streams. The GC callback closes an owned, unclosed file exactly once
and frees the record, which is what stops an unclosed file from leaking its
descriptor for the life of the process; because it runs on every swept object it
allocates nothing and is not reentrant.

One hazard has no clean answer under the current API: the record must be
allocated before `FeMakePtr()`, and `FeMakePtr()` can collect and raise, so an
out-of-memory error at exactly that point loses the record. Fixing it needs a
C-side cleanup stack that survives the `longjmp`.

`FeMakePtr()`, `FeSetMarkFn()`, `FeSetGCFn()`, and `FeMark()` support that legacy
Fex model: the mark callback marks Fe objects reachable through an external
pointer, and the GC callback releases external resources. Until the deferred
Phase 8 work provides a replacement, embedders that require their own custom
pointer-backed types must account for the global tag/name limitation rather
than treating this interface as context-local.

### Error Handling

Runtime errors call the function installed by `FeSetErrorFn()`. The callback
receives the context, an error message, and the active Fe call trace. The
message and trace are borrowed and valid only during the callback. Do not
create Fe objects or resume evaluation from inside the callback. The
callback can additionally distinguish the kind of failure -- an ordinary
error, a quit, or budget exhaustion -- through `FeGetCompletion()`, valid
for the duration of the callback and after the host's recovery (see
"Unwinding And Cleanup" above).

Returning from the error callback is not recovery. A recovering callback must
perform a nonlocal transfer, normally `longjmp`, to a host-owned recovery point.
If no callback is installed, or if it returns, Fe follows its panic policy:
silent `abort()`. The core library never prints an error or calls `exit()`.

Before entering an operation that may fail, save the GC stack index somewhere
that remains valid across `longjmp`. `FeHandleError()` clears the internal call
trace, but does not restore the host's GC checkpoint or callback input state.
After the nonlocal transfer, restore that checkpoint before continuing. The
context can then be used again. C cleanup attributes and ordinary stack
unwinding do not run across `longjmp`, so the host must also release any
external temporary resources explicitly, unless they were registered with
`FeProtectWithCleanup()`.

Before `error_fn` runs, `FeHandleError()` runs every pending `unwind-protect`
and `FeProtectWithCleanup()` cleanup, most recently registered first, while
the GC stack is still exactly as populated as it was when the error was
raised. This is why a cleanup can safely reference an object the failing call
created: the host has not yet had a chance to reset its GC checkpoint out
from under it. `error_fn`, once it does run, sees an empty cleanup registry.
