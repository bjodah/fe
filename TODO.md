Replace `car` and `cdr` with `head` and `tail`, but keep `car`/`cdr` for
historical flair.

When available, use `constexpr` instead of `enum`.

Use `readline` in main?

Reduce/eliminate C macros — replace the remaining lvalue accessors with `Set*`?

Use GMP for numbers, and expose relevant parts of libgmp.

Add (at least) `fflush`, `fseeko`, `ftello`, `remove-file` (`remove`).

Consider namespaces, e.g. `math.*`, `io.*`, `sys.*`, et c.

Update docs given new names and types et c.

Turn the examples in the docs into scripts, and test them, and then refer to
them in the docs in place of the code.

Help strings for symbols. E.g. when evaluating a function, if the first element
is a string, just ignore it (so that consing it doesn't waste resources).

Use bitfields for the "is cons cell" and GC mark bits, instead of shifts and ors
and so on.

Ensure that everything declared in fe.h really needs to be public.

Functions add their arguments to the global environment, but they should be
creating their own and destorying it upon return. (Naming a non-existent
variable no longer creates it: an unassigned symbol's value cell holds a
private sentinel and evaluating it raises `void-variable`.)

Add time functions.

Implement `($ ...)`, like sh’s `$(...)`.
