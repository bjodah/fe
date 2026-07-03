# C API

For the full details of the core Fe API, refer to `fe.h`. Here is an overview.

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
calling thread. Distinct contexts have distinct interpreter state, although a
host must still synchronize any globals it modifies, such as the current
extension type-name table.

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

An Fe native callback may re-enter the reader or evaluator on the same context.
This nesting is supported on the same thread. The callback must observe the GC
protection rules below, and an error escaping nested evaluation must unwind all
the way to the host's recovery point.

## Object Lifetime And GC Protection

Object-producing functions return an object that is either on the context's GC
protection stack or reachable from an interpreter root. Newly allocated
objects are pushed automatically; reader and evaluator results retain the
protection or rooted reachability established while producing them. Save an
index before temporary work and restore it afterward:

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

## Running A Script

To run a script, Fe must first read and then evaluate it. Do this in a loop if
there are several root-level expressions contained in the script. Fe provides
`FeReadFile` as a convenience to read from a file pointer, and you can also use
`FeRead` with a custom `FeReadFn` callback function to read from other sources.

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

## Calling A Function

You can call a function by creating a list and evaulating it; for example, we
could add two numbers using the `+` function:

```c
size_t gc = FeSaveGC(ctx);

FeObject* objs[] = {
  FeMakeSymbol(ctx, "+"),
  FeMakeDouble(ctx, 10),
  FeMakeDouble(ctx, 20),
};
FeObject* result = FeEvaluate(ctx, FeMakeList(ctx, objs, 3));
printf("result: %g\n", FeToNumber(ctx, result));

// Discard all temporary objects pushed to the GC stack.
FeRestoreGC(ctx, gc);
```

## Extending The Core

For examples of using the extension API in full detail, refer to `fex.[ch]` and
`fex_*.[ch]`. Here is an overview.

### Exposing A C Function

You can install a `FeNativeFn` into a `FeContext` by using `FeMakeNativeFn`. The
`FeNativeFn` can be bound to a global variable by using the `FeSet` function.
`FeNativeFn`s take a context and an argument list as arguments, and return an
`FeObject`. The result must never be `nullptr`; if you want to return `nil`, use
the value returned by `FeMakeBool(ctx, false)`.

You could expose the `pow` function from `math.h` like so:

```c
static FeObject* Power(FeContext* ctx, FeObject* arg) {
  double x = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  double y = FeToDouble(ctx, FeGetNextArgument(ctx, &arg));
  return FeMakeDouble(ctx, pow(x, y));
}

FeSet(ctx, FeMakeSymbol(ctx, "pow"), FeMakeNativeFn(ctx, Power));
```

You can then call the `FeNativeFn` from Fe like any other function:

```clojure
(print (pow 2 10))
```

### Creating An `FePtr`

Fe provides the `FePtr` object type to allow for custom objects. For type
safety, you must tag your types by using the `FeTFex*` elements of the `FeType`
`enum` type. You can give them your own `enum` names:

```c
enum {
  MyTypeFoo = FeTFex0,
  MyTypeBar = FeTFex1,
  // ...
};
```

And you should give them string names, too, using the global `type_names` array:

```c
type_names[MyTypeFoo] = "foo";
type_names[MyTypeBar] = "bar";
```

You can create an `FePtr` object by using the `FeMakePtr` function.

Fe provides mark and GC callbacks to customize garbage collection for `FePtr`
types. Whenever the GC marks an `FePtr`, it calls the function installed by
`FeSetMarkFn()`; this callback can mark referenced Fe objects with `FeMark()`.
When a pointer-backed object becomes unreachable, Fe calls the function
installed by `FeSetGCFn()` so it can release external resources.

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
external temporary resources explicitly.
