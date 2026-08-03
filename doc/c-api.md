# C API

For the full details of the core Fe API, refer to `fe.h`. The runnable
[`example_host.c`](../example_host.c) demonstrates the complete lifecycle
described below.

## API Compatibility

`FE_API_VERSION` identifies the public embedding interface. A host that vendors
or pins Fe should assert the version it was written against at compile time:

```c
static_assert(FE_API_VERSION == 1);
```

The macro is bumped for every breaking public API change. Compatible additions
do not require a bump, so downstreams should still pin an exact released commit
or tag rather than using the macro as a substitute for source control.

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

`FeSetStrictArity()` turns on argument-count checking for lambda and macro
calls in that context, and `FeGetStrictArity()` reports it. It is off by
default, which is Fe's historical behaviour: a missing argument binds `nil`, an
extra one is dropped, and a non-symbol parameter binds nothing. With it on, a
parameter before `&optional` must have an argument, an argument must have
somewhere to go, and a parameter must be a symbol. It is a per-context setting
that can be changed at any time and does not affect native functions, which
enforce their own arity with `FeGetNextArgument()` and
`FeRequireNoArguments()`.

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

`FeIsBound()` reports whether a symbol has a global value. A symbol exists as
soon as it is read or interned, but its value cell starts out holding a private
sentinel that no API returns and Lisp cannot reach; evaluating such a symbol
raises `void-variable NAME`, and in head position `void-function NAME`.
`FeSet()` and Lisp's `=` create or update the binding, and `nil` is an ordinary
bound value. The Lisp-level spellings are `(boundp 'name)` and
`(makunbound 'name)`, which also see lexical bindings; `FeIsBound()` has no
environment to consult and answers about the global one.

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

During either evaluation helper, reader errors are reported as
`<label>:<offset>: <message>` and evaluator errors as
`<label>: <message>`. File offsets start at zero at the stream position passed
to `FeEvaluateFile()`. Passing `nullptr` as the label disables the label prefix.
Fe borrows the label only for the duration of the call and does not retain its
pointer. Before invoking the error callback, Fe copies the composed diagnostic
into its temporary error-message buffer; as with other error messages, that
borrowed message is valid only during the callback.

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

### Bounding Recursion

`max_depth` is the maximum live recursion depth `Evaluate()` may reach; zero
selects the built-in default. Attempting to exceed it raises `evaluation
depth limit exceeded`. This is a **C-stack** bound, not an evaluation-step
count: before it existed, recursion was bounded only as a side effect of how
many GC-stack slots a self-recursive call happens to consume (the arena is
fixed-size; see `Bounding Recursion` in `doc/implementation.md` for the slot
mechanics), which tracks C-stack usage only by accident. A build with fatter
per-call C frames than the one the built-in default was measured against --
a sanitizer, `-O0`, a debug build -- could exhaust the real C stack before
the GC stack noticed, crashing the process instead of raising a catchable
error. `max_depth` is the designed bound instead: a host embedding Fe with a
smaller C stack than kg's own sanitizer CI measured against sets it lower
explicitly, the same way `cleanup_step_limit` is set lower or higher than
its own built-in default.

The control state is ambient for the complete outermost `*WithOptions()` call.
Evaluation re-entered from a native callback through either a plain evaluator
or another controlled evaluator consumes the same outer step budget and uses
the outer interrupt settings. Options supplied to a nested controlled call are
ignored; nested code cannot reset or extend the active budget. This includes
all forms in nested string and file evaluation. Passing `nullptr` as the
outermost options pointer is equivalent to zero-initialized options and still
establishes an unlimited ambient control scope whose nested options are
ignored.

The plain `FeEvaluate()`, `FeEvaluateString()`, and `FeEvaluateFile()` functions
remain unlimited when no ambient control is active. When called during a
controlled evaluation, they inherit and consume its controls. Recursion depth
is the one exception: unlike `step_limit` and `interrupt`, it is not opt-in.
`Evaluate()` checks it on every call, controlled or plain, so the plain
functions are bounded by the built-in default even with no `FeEvalOptions` in
sight -- the point being that this bound protects the C stack whether or not
a host ever thinks about evaluation control at all. Successful return from
the outermost controlled call clears the control state, including
`evaluation_depth`. `FeHandleError()` also clears it before calling the host
error callback, so a context recovered with `longjmp` starts its next
top-level evaluation fresh -- a `longjmp` skips every pending decrement, so
without this reset the next legal deep call would see a depth already
exhausted by one that failed. The exhaustion and cancellation messages pass
through normal label handling;
for example, a controlled string call labelled `init.fe` reports
`init.fe: evaluation cancelled`.

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

Recursion depth gets the same fresh treatment, for free: `FeHandleError()`
resets `evaluation_depth` to 0 before it drains the cleanup registry (see
"Bounding Recursion" above), so a cleanup that itself recurses starts from
an empty counter rather than one already at the limit that just fired. A
cleanup body that recurses as deep as `max_depth` allows is therefore
exactly as legal as a top-level call doing the same.

The worst case a host must be able to tolerate is therefore

    CleanupStackSize (256) x cleanup_step_limit

evaluator steps between the moment an error is raised and the moment
`error_fn` sees it -- 1,048,576 steps at the default, and bounded rather
than open-ended even if every cleanup on a full registry misbehaves. A host
that cannot afford that pause sets `cleanup_step_limit` lower; one running
cleanups that legitimately do more work sets it higher.

A cleanup that itself raises -- Lisp or C -- does not reach `error_fn`: that
callback's contract (below) is to transfer control away and never return,
which would abandon every cleanup still pending behind it. Instead its
message is printed to `stderr` and the remaining cleanups, inner to outer,
still run. Whichever error, interrupt, or budget exhaustion was actually
unwinding when the cleanup failed is still what `error_fn` eventually sees.

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
create Fe objects or resume evaluation from inside the callback.

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
