# Unwinding And Cleanup — Design

Status: **`unwind-protect` and its C-side counterpart are implemented**
(`FeProtectWithCleanup()`, `doc/c-api.md`'s "Unwinding And Cleanup",
`doc/implementation.md`'s section of the same name, `doc/language.md`'s
`(unwind-protect ...)`). The evaluator has normal/error/throw/quit/budget
completion kinds and a context-owned frame stack. Only normal and
ordinary error are reachable through the Lisp-facing machinery; since
sub-plan 06B the Quit and Budget kinds are also *true at their producers*
and host-readable through Decision 5's additive accessors. Sub-plan 06C
then made `throw` a real completion: `catch`/`throw` are implemented as the
mid-stack unwind this document's "Nested evaluation and native re-entry"
section designs -- a `FeFrameCatch` whose checkpoints are the throw's
destination, a drain to that frame's checkpoint rather than to zero, the
native re-entry boundary as a tested wall (the C activations between runs
are live, recorded as a divergence rather than pretended away), and
`no-catch` as a message through the existing error path until 06D provides
condition objects. An error under an evaluator barrier is
copied into context storage, drains the current cleanup registry, then reaches
the outer public boundary exactly once. `condition-case`,
and the token-based rollback-on-error registry sketched
below for `MakeFile()`-shaped problems are still design only. The rest of
this document is the original design; where the shipped implementation took
a narrower or different shape, a note says so inline rather than rewriting
history. It still exists so the remaining pieces -- `condition-case`,
and quit as a distinct *catchable* kind -- are not designed
four times by four separate patches that then have to be reconciled.

**Reconciled 2026-08-05 against the measured oracle (sub-plan 06A of kg's
`2026-08-03-elisp-subset-and-fe-evaluator` plan set).** One claim below is
wrong, and 06A's Decision 4 settles it: the "What reaches the host" section
says "Emacs discards" a cleanup's own error. Measured against the pinned
Emacs 31.0.90, Emacs lets the **new error replace** the in-flight one --
`(condition-case e (unwind-protect (error "orig") (error "cleanup"))
(error e))` → `(error cleanup)` -- and a cleanup's `throw` likewise wins
over an in-flight throw. Fe's shipped behaviour is a third thing (print the
cleanup failure to `stderr` and keep unwinding with the original), pinned by
`test_api.c`'s `TestUnwindLisp`; Decision 4's policy is **match Emacs**, and
06D implements it, rewriting the three `stderr` assertions with it. The rest
of 06A's reconciliation is that the substrate this document designs is
half-built and documented as such: the five completion kinds already exist
as `FeCompletion` (in `fe.h` since 06B made the enum public; `fe_internal.h`
before it) with only `Normal`/`Error` ever
assigned (Phase 6 makes the other three true; it does not add the enum), and
the checkpointed drain `RunCleanupsDownTo` (`fe_eval.c:132`) is already live
and called by every completing pair frame, so the "Nested evaluation" note's
"this has to change" is already half-built -- only the drain-to-zero
`RunCleanupsAfterError` needs a `catch` frame to displace. Phase 5's residue
is recorded here for the first time: `arith-error` and int64-overflow joined
the message-level condition names (`num-div-zero`, `num-overflow-bignum`),
so "only normal and ordinary error are reachable today" is exact only at the
message level -- the names ride in `FeHandleError()`'s text, exactly as the
parent plan's "Condition names before conditions exist" note requires, until
06D turns the named sites into structured signals.

## The constraint everything follows from

`FeHandleError()` does not return. In the interactive interpreter its handler
`longjmp`s to the REPL; otherwise the process exits. `auto.[ch]`'s
`cleanup`-attribute helpers therefore do **not** run, and neither does any
ordinary C stack unwinding. Anything a C frame owns at the moment an error is
raised is lost.

That is not hypothetical. Two places in this repository own heap memory across
a call that can raise:

- `MakeFile()` in `fex_io.c` allocates the `FexFile` record and then calls
  `FeMakePtr()`, which can collect and raise `out of memory`. On that path the
  record and its descriptor are lost. There is no way to write it the other way
  round: the Fe cell cannot be created first because there is no setter for a
  pointer cell.
- `FexCopyStringZ()` takes a `cleanup` pointer purely so that a caller holding
  one allocation can hand it over before raising. That parameter is a hand-rolled
  one-slot cleanup stack, and it is already used in three places.

Both are small. They are also exactly the shape that stops being small as soon
as kg owns C resources through Fe objects — buffers, processes, markers.

## Completion kinds

A single mechanism has to serve five ways out of a form, and the design should
name them before it names any function:

| Kind | Source | Resumable |
| --- | --- | --- |
| normal | the form produced a value | — |
| error | `FeHandleError()` | at a handler |
| throw | Lisp `throw` to a `catch` tag | at the matching `catch` |
| quit | the interrupt callback returned true | at the host's recovery point only |
| budget | the evaluation step limit was reached | as quit |

Today `error`, `quit` and `budget` are the same thing: all three go through
`FeHandleError()` with a different message, and a host cannot tell them apart
except by string comparison. That is the first thing to fix, and it is fixable
independently of everything else: a completion kind alongside the message in
the error callback.

**Fixed 2026-08-05 (sub-plan 06B).** `FeCompletion` is now a public enum
(`fe.h`), the interrupt path assigns Quit and the step-limit/frame/re-entry
walls assign Budget (all four through a private `RaiseCompletion` sibling of
`FeHandleError`, whose signature is unchanged), every ordinary raise stays
Error, and a host reads the kind with `FeGetCompletion(ctx)` and the (nil,
until 06D) condition with `FeGetCondition(ctx)` -- Decision 5's additive
accessors. The kinds are a parallel channel only: no Lisp program can observe
them (`catch` and `condition-case` do not exist yet), quit and budget remain
ordinary `longjmp`-to-the-host completions, and the completion resets to
Normal at the outermost barrier and after a normal top-level return exactly as
before.

`quit` and `budget` must not be catchable by ordinary Lisp handlers. Emacs Lisp
made `condition-case` unable to catch quit by default for good reasons and
learned them the hard way; Fe should start there.

**The enum is already there, three-fifths dead (06A's audit).**
`FeCompletion` (in `fe.h`; `fe_internal.h` before 06B moved it to the public
header) declares all five kinds, but only
`Normal` and `Error` are ever assigned today, and the enum currently serves
as a one-bit "draining?" flag read exactly once: `AllocateFrame`'s
`CleanupFrameReserve` gate (`fe_eval.c:711`) tests
`completion != FeCompletionNormal`. Phase 6 does not add the enum -- it makes
the other three values true at their producers, and the reserve gate silently
widens to them the moment they are assigned. That is a live coupling nobody
has yet had to consider; 06B turns it from an accident into an asserted
behaviour (a cleanup provoked by frame exhaustion must be pushable regardless
of which wall tripped), and 06C/06D keep treating it as a decision, not a
side effect.

## Two cleanup registries, one ordering

There are two kinds of cleanup and they cannot be the same list, because one is
Lisp and one is C:

- **Lisp cleanups** are forms to evaluate: `unwind-protect`'s body. They
  allocate, they can themselves fail, and they must run in the environment they
  were registered in. They belong in the arena, as evaluator frames.
- **C cleanups** are a function pointer and a `void*`. They must not allocate Fe
  objects, must not re-enter the evaluator, and must not fail. They belong in a
  fixed-size context array, sized like the GC stack.

They interleave, so they need one ordering, not two. The cheapest way to get it
is a single monotonically increasing sequence number stamped on every
registration in either registry; unwinding walks both in descending sequence.

```c
typedef void FeCleanupFn(FeContext* ctx, void* userdata);

FeCleanupCheckpoint FeSaveCleanups(FeContext* ctx);
FeCleanupToken FeDeferCleanup(FeContext* ctx, FeCleanupFn* fn, void* userdata);
void FeCancelCleanup(FeContext* ctx, FeCleanupToken token);
void FeRunCleanups(FeContext* ctx, FeCleanupCheckpoint checkpoint);
```

`FeDeferCleanup()` must not itself be able to fail after the resource exists,
which means the array is preallocated and the failure mode is "cleanup stack
overflow" raised *before* the caller allocates anything. The registration order
in `MakeFile()` then becomes: defer a cleanup that frees a `FexFile*` slot,
allocate into it, call `FeMakePtr()`, cancel the cleanup on success. That is
four lines and it closes the hole.

`FeCancelCleanup()` takes a token rather than popping, because ownership
transfer is not always LIFO: `FexExecute()` builds an argv array and hands the
whole thing to one owner.

**What shipped instead, for now:** one array of tagged entries (`FeCleanupEntry`
in `fe.c`, a C function-pointer-and-`void*` or a pair of Fe objects), popped
strictly LIFO -- a stack rather than a sequence-numbered set two structures
draw from. That is enough for `unwind-protect` and for
`FeProtectWithCleanup()`, both of which are "run this on the way out,
whichever way that is," and neither of which needs to cancel a registration:
`unwind-protect` always wants its unwind forms to run, and a host cleanup
registered right before an `FeCall()`/`FeEvaluate()` it wraps is exactly the
same shape. `FeSaveCleanups()`/`FeCancelCleanup()`/`FeRunCleanups(checkpoint)`
as a *public* API, and the checkpoint type itself, were not built --
`Evaluate()` and `FeHandleError()` are the only two callers that ever drain
the registry, using an index they keep internally. The `MakeFile()` problem
this section opens with is consequently still open: that needs
run-only-on-error-with-cancel semantics, which a plain "always runs" stack
cannot express, so migrating Fex to this still needs the token/cancel half of
this section built out. `FeCleanupFn`'s second parameter is named `data`, not
`userdata`, to match `FeMakeNativeFn`'s and `FeSetGCFn`'s callbacks rather
than `FeInterruptFn`'s -- there was no strong reason to prefer one existing
name over the other, so this just picked one.

## Interaction with the GC stack

The GC stack and the cleanup stack are separate and must stay separate: the GC
stack is restored by *assignment* (`FeRestoreGC()` sets an index), which is
exactly what a cleanup stack must not do, because dropping a cleanup record
without running it is the bug. A checkpoint mismatch between the two is a
programming error worth asserting on in a debug build.

Note that the host's recovery path already restores the GC stack after
`setjmp()` returns (kg does this in `kg_lisp_eval_string`). Cleanups must run
*before* control reaches the host, or the host will have released its
protection while C cleanup records still reference Fe objects.

## Nested evaluation and native re-entry

A native callback may re-enter the evaluator. Each entry takes a checkpoint;
the error path unwinds to the checkpoint of the frame that catches, not to zero.
This is the reason for checkpoints rather than a single global "run everything"
call, and it is also why the interim C-only stack has to be designed against the
eventual engine rather than bolted on: a `catch` that resumes in the middle of
the stack cannot be modelled by a stack that only knows how to be emptied.

**What shipped instead, for now:** `FeHandleError()` drains the whole registry
down to zero unconditionally, on every error, including one raised from
inside a re-entrant `FeCall()`/`FeEvaluate()` a native started. That is
correct today only because there is exactly one thing an error can unwind
to: the single host-installed recovery point every existing host
(`main.c`, `test_api.c`, `example_host.c`) sets up once, outside any native
re-entry, and `longjmp`s to unconditionally -- there is no `catch` for an
error to stop at partway. The moment `catch`/`throw` exist, this has to
change to "drain to the checkpoint of the frame that catches," exactly as
this section already says, or a `catch` inside a re-entrant native call
would incorrectly run cleanups that belong to a scope outside the `catch`.

**The checkpointed half already exists (06A's audit).**
`RunCleanupsDownTo(ctx, target)` (`fe_eval.c:132`) is live and is what every
completing pair frame calls (`fe_eval.c:2273`) to drop cleanups registered
above it; only the drain-to-zero `RunCleanupsAfterError` is the thing a
`catch` frame has to displace. 06C's throw unwinds on exactly this
function -- search down the frame stack for the innermost matching catch,
then `RunCleanupsDownTo(ctx, catch->cleanup_checkpoint)`, restore the GC
checkpoint, and discard the frames above -- so this section's "this has to
change" is half-built, not open design.

**Implemented 2026-08-05 (sub-plan 06C).** The "this has to change" sentence
above is now true: `catch`/`throw` exist, and the throw path drains to the
checkpoint of the frame that catches (`FeFrameCatch`, `PerformThrow`, both
in `fe_eval.c`; `doc/language.md`'s `(catch ...)`/`(throw ...)`,
`doc/implementation.md`'s "Unwinding And Cleanup"). The search stops at the
current run's frame-stack floor (`ctx->run_base`), which is the wall this
section's "Nested evaluation and native re-entry" rules require: a catch
below a nested run's base is not matchable from inside it, because the C
activations between the runs are live and cannot be popped by frame-index
assignment. A throw that finds no catch raises `no-catch TAG VALUE` through
the ordinary error path -- draining to zero like any error, since until 06D
an error has nowhere nearer to stop -- and `FeHandleError`'s own
drain-to-zero is unchanged. 06D's `condition-case` reuses the same catch
machinery and makes quit a catchable kind; the token-based
run-only-on-error registry stays open (Fex's resource problem, Phase 9).

**A Lisp cleanup's own forms are themselves a nested evaluator run,** since
sub-plan 03E's frame machine: `RunOneCleanupEntry` starts one via
`RunEvaluationBody` in place of the old recursive `DoList`'s one nested
`Evaluate()` call per unwind form. It is a nested run on the *unused suffix
of the same frame stack*, above a saved barrier -- not a second stack, and
not the "single global run everything" call this section argues against;
it is exactly the checkpoint-scoped nesting described above, applied to the
one case (a cleanup) that already existed before `catch` does. A cleanup's
own error still bypasses this nested barrier directly via `cleanup_catch`'s
`longjmp`, precisely as it bypassed the old recursive `DoList`'s implicit
one, so `RunOneCleanupEntry`'s own manual restores (frame index, saved
`evaluator_catch`, `native_reentry_depth`, `call_list`) are what an eventual
`catch` inside a cleanup would also have to reconcile with.

**A cleanup's own resource limits are fresh, but its live counts are not.**
Sub-plan 03F split the evaluator's single depth bound into two
(`FeEvalOptions.max_frames` for Lisp nesting, `max_native_reentry` for
nested runs a native starts), and unwinding treats the two halves of that
state differently on purpose:

- The *configured limits* are cleared. `FeHandleError` calls
  `ClearEvaluationControl` before draining, so a cleanup runs under the
  built-in ceilings rather than whatever tight `max_frames` the abandoned
  body was configured with. This is the same fresh-re-arm the step budget
  already gets via `cleanup_step_limit`, and for the same reason: a body
  that ran out of a resource must still get a working cleanup.
- `CleanupFrameReserve` frames beyond the physical capacity become
  pushable, but only while `ctx->completion != FeCompletionNormal`. A
  cleanup provoked by frame exhaustion is therefore not immediately refused
  by the same wall the body just hit.
- `native_reentry_depth` is **not** cleared, because it is a census of live
  C activations rather than a ceiling. While the drain runs, no `longjmp`
  has popped a single one of the native C frames the abandoned computation
  was inside; they are still on the real stack, and a cleanup native that
  re-enters evaluation stacks a fresh activation on top of all of them. The
  counter falls back to the truth one `RunEvaluation` barrier restore at a
  time, as the unwind actually happens.

The asymmetry is the point: forgiving a cleanup its predecessor's *budget*
is safe, whereas pretending its predecessor's *C stack* went away is not.

## What reaches the host

The error callback should receive the completion kind, the message, and the
call list, and should keep receiving them only for the duration of the call
(the message is a borrowed buffer today and should stay that way). A cleanup
that itself raises while unwinding is the one genuinely hard case: the options
are to abort, to discard the new error and continue unwinding with the
original, or to replace it. **The claim that "Emacs discards" is false,
measured 2026-08-05 against the pinned Emacs 31.0.90:** a cleanup's error
*replaces* the in-flight one --
`(condition-case e (unwind-protect (error "orig") (error "cleanup"))
(error e))` → `(error cleanup)` -- and a cleanup's `throw` likewise wins over
an in-flight throw. 06A's Decision 4 settles the policy as **match Emacs --
the new error replaces the in-flight completion**, because the phase's whole
point is that handlers can rely on Emacs semantics and the divergence is
observable from Lisp (`condition-case` around a failing cleanup); 06D
implements it.

**What shipped instead, for now (superseded by Decision 4 at 06D):**
discarding was this section's original reading, but the callback that reaches
is not `error_fn` -- a cleanup's own failure is printed to `stderr` directly
from inside `FeHandleError()` (`RunOneCleanupEntry()` in `fe.c`), and
`error_fn` never learns a cleanup failed at all. Completion kinds now have a
distinct value for the drain that is in flight (`FeGetCompletion()`, 06B), but
"a cleanup failed" still is not one -- the in-flight kind is preserved by
design, so a cleanup that runs out of its own steps mid-drain does not
overwrite the Error/Quit/Budget the drain is for. Tested per this section's
requirement: `test_api.c`'s `TestUnwindLisp()`
captures `stderr` around a failing nested cleanup and asserts both the
diagnostic and that the outer cleanup still ran. The three assertions pinning
the `stderr` text (the `CHECK(strstr(captured, "cleanup error") ...)` family,
`test_api.c:3086, :3155, :3204`) and `error_fn`'s never-sees-cleanup-failures
contract are rewritten by 06D when the measured replace-policy lands.

## Order of work

1. Completion kinds in the error callback. Independent, small, unblocks a host
   telling quit from a genuine error. **Landed 2026-08-05 (sub-plan 06B),**
   per 06A's Decision 5: `FeErrorFn` is unchanged, `FeGetCompletion(ctx)` +
   `FeGetCondition(ctx)` are the additive accessors, the dormant kinds are
   true at their producers (`EvaluationStep`'s step-limit → Budget, interrupt
   → Quit; the frame/re-entry walls → Budget), and `TestCompletionKinds` pins
   each kind read from inside the callback and after recovery. The standalone
   `fe` binary's structured error
   channel for the compat runner (`condition_source` "message" → "structured")
   is 06D's half of the same item.
2. The C-only cleanup stack, with an error-injection matrix: inject an error at
   every allocation position in `fex_io.c` and `fex_process.c` and assert that
   no descriptor and no allocation survives. Only then migrate Fex to it.
   **Still open** -- what shipped is the always-runs registry two sections
   up, not the run-only-on-error-with-cancel one this step and `MakeFile()`
   need; Fex has not been migrated. **06A records this as staying open with a
   pointer: it is Fex's resource problem, not Phase 6's control-flow problem,
   and Phase 9's robustness scope (kg plan set) is its natural home.**
3. Lisp `unwind-protect` on top of the same sequence numbers. **Shipped**,
   on top of one shared LIFO stack rather than sequence numbers -- see "Two
   cleanup registries, one ordering" above for why that was enough.
4. `catch`/`throw` and `condition-case`, which need the resumable completion
   kinds and therefore need 1–3 first. **`catch`/`throw` landed 2026-08-05
   (sub-plan 06C)** -- the catch frame and the throw unwind on the
   checkpointed drain this document names, with the no-catch message and the
   native-reentry wall tested and recorded; 06D lands `condition-case` on the
   same machinery, the static hierarchy, and Decision 4's cleanup-raise
   policy, closing the fe workstream.

Per-context extension type descriptors (`doc/c-api.md`'s "Phase 8") depend on
step 2, because a descriptor's finalizer is a cleanup with the same rules.

## What this is not for

kg does not need any of this today. kg's cleanup runs after `setjmp()` returns
and consists of restoring the GC checkpoint and releasing frame buffers, none of
which is C-owned resource ownership across a raise. This design serves
standalone Fe and Fex, and it serves the kg that will exist once Fe objects can
own editor buffers and processes.
