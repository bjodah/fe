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

Symbols store a pair object in the `cdr`; the `car` of this pair contains a
`string` object, and the `cdr` part contains the globally bound value for the
symbol. Symbols are interned.

### Numbers

Numbers store an `FeDouble` in the `cdr` part of the `object`. By default
`FeDouble` is a `double`, but any value can be used so long as it is equal to or
smaller in size than an `FeObject` pointer. If a different type of value is
used, `FeRead` and `FeWrite` must also be updated to handle the new type
correctly.

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
`cdr` is the pair `(name . value)`, so a global binding and a lexical one are
the same shape and one lookup returns the cell either way.

A symbol exists as soon as it is read, which is not the same as having a value.
A fresh symbol's value cell holds `unbound`, a private static object outside the
arena — like `nil`, so the collector neither sweeps nor has to mark it, and
`FeMark` treats it as a leaf. Nothing returns it: Lisp cannot reach a value cell
(`(cdr sym)` is a type error, and `(env)` yields symbols whose printed form is
their name), and the two readers of a value cell — symbol evaluation and the
head of a call — turn it into `void-variable NAME` and `void-function NAME`. It
is tagged `FeTFree` so that an escape aborts in the writer rather than
impersonating a value.

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
described below (GC-stack depth, evaluation depth, cleanup-stack depth) as a
read-only snapshot. It adds no new counters beyond what the sites already
below track for their own bookkeeping -- `arena_live_count` is the one
exception, a running total `MakeObject()`/`CollectGarbage()` maintain solely
so the accessor can report `free_slots` without walking the freelist.

The `gc_stack` is a fixed 4096-slot array inside `FeContext`, so it also caps
recursion: a self-recursive Fe function costs several slots per frame, which
allows roughly 450 frames before `GC stack overflow`. Because the array lives in
the arena, its size is the dominant term in `FeMinimumArenaSize()`.

That cap is incidental, not designed: it tracks GC-stack slot consumption,
not C-stack usage, so a build with fatter per-call C frames (a sanitizer,
`-O0`, a debug build) can exhaust the real C stack first and crash instead
of raising `GC stack overflow`. `Evaluate()`'s recursion is bounded
separately and explicitly by `evaluation_depth` against
`FeEvalOptions.max_depth` (`DefaultEvaluationDepth` when left 0), checked on
every pair-form evaluation regardless of whether any `*WithOptions()` control
is active. See `doc/c-api.md`'s "Bounding Recursion". `evaluation_depth` is
reset to 0 by `FeHandleError()`, the same way `call_list` is, since a
`longjmp` skips every pending decrement.

Native re-entry is bounded separately at the native-call boundary by a
private `native_reentry_depth` counter on `FeContext`. It counts currently
active native invocations -- an ordinary native is one fixed C activation,
and a native that synchronously calls `FeCall`/`FeCallWithOptions` starts a
nested `RunEvaluation` and then another active native level, which is the
one place a fresh C frame legitimately enters through the evaluator. Until
03F it is capped by the same ambient legacy `evaluation_depth_limit`/
`DefaultEvaluationDepth` and reports the same old
`evaluation depth limit exceeded` text, so no public expectation moves
mid-migration; 03F gives native re-entry its own public option, measured
smaller default, statistic and error. Each `RunEvaluation` barrier saves and
restores the counter beside `evaluator_catch` on both the normal and the
`longjmp` path -- the counter is a plain `size_t`, never a copied `jmp_buf`
-- and `FeHandleError()` resets it to 0 with the other live-depth state, so
a recovered context starts fresh.

For a zero-argument re-entering chain the two counters track the same
nesting: each activation is one pair form and one native level, so with the
shared legacy ceiling they reach the limit together. The legacy pair-depth
check runs earlier in the loop and reports the old message first, at the
same blocked activation; `native_reentry_depth` is live bookkeeping for
03F -- incremented, restored, barrier-saved and error-reset, so its nesting
level is honest even though it is not the independently observable bound
today. 03F gives it its own smaller default and that is when it becomes the
reported bound.

The `FeFrameNative` resume saves both active-depth values -- this counter
and `evaluation_depth` -- before calling the native and restores both on the
ordinary return, before `CompletePairFrame` runs its own unconditional
decrement for the enclosing pair form. An owning nested `FeCallWithOptions`
-- started when no control record is active, e.g. under a plain
`FeEvaluateString` -- runs `EndEvaluationControl` on its way out, which
clears the whole control record (both counters included) while the enclosing
frames are still live. Restoring both puts the enclosing evaluation's
accounting back: a blind decrement would wrap `native_reentry_depth` to
`SIZE_MAX` and spuriously block the next native, and merely leaving
`evaluation_depth` at 0 would under-count the still-live pair forms for
every later form in the same run, weakening the logical max_depth bound.
Error paths skip the restore entirely: the enclosing barrier restores its
saved counter, and `FeHandleError()` resets both to 0.

Evaluation has one context-owned frame stack. The collector marks every live
frame, including the temporary recursive-dispatch frame used while Phase 3 is
being migrated. Self-evaluating objects, symbols, and primitive `quote` use
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
every body form and every parameter walk `DoList` and `ArgsToEnv` charged,
and pushing each form as a sub-expression frame. The body frame owns its
environment, and a body form's `let` is handed a pointer to it (the `bind`
field of the pushed frame), so a `let` in a lambda body extends the
environment the following body forms see exactly as the recursive `DoList`
`&env` out-parameter did -- the one piece of `let`/`newenv` threading that
works today; `let` itself stays on the temporary recursive path until 03E.
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
Primitives that evaluate their operands (`list`, `set`, `=`, and the
arithmetic/comparison forms) keep using `EvaluateList` for this slice.
`DoList` remains for the primitive/special-form bodies (`do`, `if`,
`while`) and cleanup paths 03E still owns. A native call is the one
remaining ordinary callable: once the argument frame has reordered the
evaluated argument list, the frame switches to `FeFrameNative` and the run
loop invokes the `FeNativeFn` synchronously from that explicit state -- the
existing public signature unchanged, and no per-native `setjmp` (the
enclosing `RunEvaluation` barrier is the only one in effect). The
`native_reentry_depth` check wraps that invocation, so a native calling back
into `FeCall*` is the one C-recursion route this slice deliberately keeps;
every special form still takes the temporary recursive path.
Embedded frame trace cells preserve the host error callback's semantic
call trace without allocating after an error. Frame exhaustion keeps the
existing `evaluation depth limit exceeded` text until the final public-bound
slice.

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
exit policy belong to the host.

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
clears the complete control record before invoking the host error callback.
Consequently a nonlocal transfer cannot leave a stale budget active in a
recovered context.

## Unwinding And Cleanup

`doc/unwind-design.md` is the design this section's implementation follows;
it also records which parts of that design (checkpoints and tokens for a
rollback-on-error registry, distinct completion kinds, `catch`/`throw`,
`condition-case`) are still future work.

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

A cleanup that itself raises is the one case `FeHandleError()` treats
differently: `cleanup_catch`, a `jmp_buf*` naming the local `setjmp()` a
helper (`RunOneCleanupEntry()`) installed around that one entry's execution,
is non-null exactly while that entry is running. `FeHandleError()` checks it
first, before anything else, and when it is set, resumes there directly
instead of reaching `error_fn` -- reaching a callback contracted to never
return would abandon every cleanup entry still below this one. The message
is copied into context-owned storage first, since the frame that formatted
it is what is about to be unwound past, then printed to `stderr` once
control resumes at the `setjmp()`. Whichever error, interrupt, or budget
exhaustion was already unwinding when the cleanup failed is what
`RunCleanupsDownTo()`'s caller still eventually reports: a cleanup failure
never replaces it, and the loop moves on to the next entry.

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
