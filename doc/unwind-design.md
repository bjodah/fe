# Unwinding And Cleanup — Design

Status: **design only.** Nothing described here is implemented. It exists so
that the C-side cleanup stack, `unwind-protect`, `condition-case`,
`catch`/`throw` and quit are not designed four times by four separate patches
that then have to be reconciled.

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

`quit` and `budget` must not be catchable by ordinary Lisp handlers. Emacs Lisp
made `condition-case` unable to catch quit by default for good reasons and
learned them the hard way; Fe should start there.

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

## What reaches the host

The error callback should receive the completion kind, the message, and the
call list, and should keep receiving them only for the duration of the call
(the message is a borrowed buffer today and should stay that way). A cleanup
that itself raises while unwinding is the one genuinely hard case: the options
are to abort, to discard the new error and continue unwinding with the
original, or to replace it. Emacs discards. Discarding is the right default
here too, but it must be a decision, not an accident, and it must be tested.

## Order of work

1. Completion kinds in the error callback. Independent, small, unblocks a host
   telling quit from a genuine error.
2. The C-only cleanup stack, with an error-injection matrix: inject an error at
   every allocation position in `fex_io.c` and `fex_process.c` and assert that
   no descriptor and no allocation survives. Only then migrate Fex to it.
3. Lisp `unwind-protect` on top of the same sequence numbers.
4. `catch`/`throw` and `condition-case`, which need the resumable completion
   kinds and therefore need 1–3 first.

Per-context extension type descriptors (`doc/c-api.md`'s "Phase 8") depend on
step 2, because a descriptor's finalizer is a cleanup with the same rules.

## What this is not for

kg does not need any of this today. kg's cleanup runs after `setjmp()` returns
and consists of restoring the GC checkpoint and releasing frame buffers, none of
which is C-owned resource ownership across a raise. This design serves
standalone Fe and Fex, and it serves the kg that will exist once Fe objects can
own editor buffers and processes.
