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

Implement `macroexpand-all`. Sub-plan 10B landed `macroexpand-1` and
`macroexpand`; `macroexpand-all` exists only as a name that raises
`unsupported feature: macroexpand-all` (10A Decision 2), because expanding
every sub-form needs a code walker that knows the shape of each special form
— which arms of `if`, `let`, `lambda`, `condition-case`, `unwind-protect`,
`catch` and `quote` are expressions and which are not. Emacs' own
implementation is `macroexp.el`, several hundred lines of exactly that
knowledge. `compat/features.json`'s `macroexpand-all` entry carries the
oracle's answer, so the target is on file.

Implement macro environments, or decide against them. `macroexpand-1` and
`macroexpand` accept an ENVIRONMENT argument and require it to be nil; a
non-nil value raises `unsupported feature: macroexpand environment`. Emacs'
environments are an alist of `(NAME . DEFINITION)` entries that shadow the
function cell for the duration of one expansion — a second name-resolution
path, wanted mainly by a code walker, which is the item above.
