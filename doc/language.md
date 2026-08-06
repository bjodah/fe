# Language

## Syntax

A list is written `(` element … `)`. The final `cdr` may be given explicitly
with a dot:

```clojure
fe > '(a b c)
(a b c)
fe > '(a . b)
(a . b)
fe > '(a . (b c))
(a b c)
```

The dot is the pair marker only in that position, and the reader accepts
exactly

```text
list := '(' element* [ '.' element ] ')'
```

with at least one element before the dot. Everything else is a syntax error,
because the alternative is a list that quietly means something other than what
was written:

| Written | Result |
| --- | --- |
| `(. a)` | error: `'.' at start of list` |
| `(a .)` | error: `missing value after '.'` |
| `(a . b c)` | error: `extra value after dotted tail` |
| `(a . b . c)` | error: `extra value after dotted tail` |

(Earlier versions of Fe read `(a . b c)` as `(a c)`, `(a . b . c)` as `(a . c)`
and `(. a)` as `a`.)

Outside a list, `.` is an ordinary symbol, and a leading dot still starts a
float: `'.` is the symbol `.` and `.5` is `0.5`.

Number literals follow Emacs' grammar (05A Decision 3, 05D's cut). The reader
classifies a token first, then converts only the classified text, so what a
literal *is* is decided by shape, not by whatever `strtod` would accept:

| Written | Reads as | Note |
| --- | --- | --- |
| `42`, `+5`, `-5` | integer | optional sign, digits |
| `1.` | integer `1` | a trailing dot is still an integer (R2) |
| `.5` | float `0.5` | a leading dot is a float (R3) |
| `1e3`, `1.e3`, `1.5e+3` | float | digits with a fraction and/or exponent (R5) |
| `1.0e+INF`, `0.0e+NaN` | nonfinite float | the exact spellings the printer emits, read back |
| `0x10`, `inf`, `nan`, `1e`, `1.0e+` | symbol | everything else is un-numbered (R6-R8) |

An integer literal that overflows `int64_t` reads as a double -- the
pre-bignum Emacs behaviour -- recorded as a divergence row against modern
Emacs' bignums (05A Decision 3).

## Forms

### Special Forms

#### `(let symbol value)`

Creates a new binding of `symbol` to the value `value` in the current environment.

#### `(setq symbol value ...)`

The Emacs Lisp assignment special form, and the one assignment spelling Fe
has. Takes any number of `symbol value` pairs and processes them left to
right: each `value` form is evaluated -- already seeing every earlier pair's
new value, since they run in order -- and then bound to the innermost
lexical binding of `symbol` if one exists in the current environment, else
to the global value cell. Assignment is the only thing that creates a global
binding: naming a symbol that has never been assigned is `void-variable`,
not `nil` (`nil` is a value like any other, so a variable assigned `nil` is
bound). `(setq)`, with no pairs, is `nil`; otherwise `setq` returns the
value of the last pair.

Each `symbol` is checked to be a symbol before its paired `value` form is
evaluated: a non-symbol target is `wrong-type-argument`. A dangling final
`symbol` with no paired value is `wrong-number-of-arguments`, but only
diagnosed after every earlier complete pair has already run -- those
assignments stand even though the call as a whole signals an error.

```clojure
fe > (setq a 1 b a)
1
fe > (list a b)
(1 1)
fe > ((lambda () (setq x 9) (list ((lambda (x) (setq x 2) x) 1) x)))
(2 9)
fe > (setq a 1 b)
error: wrong-number-of-arguments
fe > a
1
```

The nested-lambda example shows both halves of the lexical/global rule in
one call: the outer `setq x 9` runs in a scope with no lexical `x`, so it
writes the global cell, while the inner lambda's `setq x 2` finds its own
parameter and updates that -- leaving the global `x` at 9, which is why the
result is `(2 9)` and not `(2 2)`.  The last two lines show the other rule:
`a` keeps the 1 that the complete first pair assigned, even though the call
went on to signal.

See `(set symbol value)` below for the other assignment primitive -- an
ordinary function that always writes the global cell -- and `(= number ...)`
for the numeric comparator sub-plan 02C repurposed `=` as.

#### `(if condition then else ...)`

If `condition` is true, evaluates `then`; otherwise, evaluates every remaining
form as an implicit `do` and returns the last one, exactly as Emacs Lisp's `if`
does. `(if condition)` and `(if condition then)` with a false condition are
`nil`.

```clojure
fe > (setq x 2)
2
fe > (if (is x 1) "one"
         "bleep")
bleep
fe > (if (is x 1) "one"
         (print "not one")
         "bleep")
not one
bleep
```

Chain nested `if` forms to replicate the functionality of C's `else if` and
`switch`/`case` statements. (Earlier versions of Fe read the trailing forms as
an `else if` chain of alternating conditions and consequents instead.)

```clojure
fe > (if (is x 1) "one"
       (if (is x 2) "two"
         (if (is x 3) "three"
           "?")))
two
```

#### `(lambda arguments ...)`

Creates a new function. `fn` is a synonym, kept because it is Fe's historical
spelling; both names are bound to the same primitive, and functions print as
`(lambda ...)`.

```clojure
fe > (fset 'square (lambda (n) (* n n)))
(lambda (n) (* n n))
fe > (square 4)
16
```

A lambda in head position is legal and stays as-is: `((lambda (n) (* n n)) 4)`
is `16`. To name a function, install it in the name's function cell with
`(fset 'name (lambda ...))` or `(defalias 'name ...)`; a `setq`'d lambda is a
value and is `void-function` in call position, since call position resolves
only the function cell.

##### Parameter lists

A parameter list is a list of symbols. A parameter with no corresponding
argument is `nil`, and `&optional` may be written before such parameters to say
so:

```clojure
fe > ((lambda (a &optional b) (list a b)) 1)
(1 nil)
```

Three spellings collect the remaining arguments into a list. Fe's dotted tail
and bare symbol are the historical ones; `&rest` is Emacs Lisp's, and nothing
may follow its parameter:

```clojure
fe > ((lambda (a . r) (list a r)) 1 2 3)
(1 (2 3))
fe > ((lambda r r) 1 2 3)
(1 2 3)
fe > ((lambda (a &rest r) (list a r)) 1 2 3)
(1 (2 3))
```

`(a &rest)` and `(a &rest r x)` are errors. `&optional` and `&rest` are
recognized by name, so they cannot also be used as parameter names.

Macros take their parameters the same way, over the caller's unevaluated forms.

By default Fe does not check argument counts at all: `((lambda (x) x))` is
`nil`, `((lambda () 1) 2)` is `1`, and `((lambda (1) 5) 2)` is `5`. A host can
ask for the checks with `FeSetStrictArity()`, and `fe -a` turns them on for the
interpreter. Then a parameter before `&optional` must have an argument, an
argument must have a parameter or a rest parameter to go to, and a parameter
must be a symbol; the failures are `wrong-number-of-arguments` and `parameter
is not a symbol`. Optional and rest parameters are unaffected, and passing
`nil` explicitly is still passing an argument.

#### `(macro arguments ...)`

Creates a new macro.

Macros work similar to functions, but receive their arguments unevaluated and
return code which is evaluated in the scope of the caller. The macro runs on
every call: the expansion is evaluated as it stands and the call site keeps the
macro call.

For example, we could define a macro named `++` to increment a numeric value by
1:

```clojure
(fset '++
  (macro (sym)
  (list 'setq sym (list '+ sym 1))))
```

Then we could use it in the following `while` loop:

```clojure
(setq i 0)
(while (< i 10)
  (print i)
  (++ i))
```

Each iteration expands `(++ i)` afresh, so the loop body behaves as if it had
been written

```clojure
(setq i 0)
(while (< i 10)
  (print i)
  (setq i (+ i 1)))
```

Earlier versions of Fe expanded once and then overwrote the call site with the
generated code. That made a macro's own definition unreachable after the first
call, and it was wrong for expansions that are not lists: overwriting the call
site copied the returned object, and `nil` and interned symbols are compared by
address, so a macro expanding to `nil` produced a `nil` that was true, and one
expanding to a symbol produced a symbol that missed every lexical binding of
that name. Expanding each time costs one expansion per call -- charged against
the host's evaluation-step budget -- and buys back a macro that means what it
says.

For more examples, see [macros.fe](../scripts/macros.fe).

#### `(while condition ...)`

If `condition` evaluates to true, evaluates the rest of its arguments. Repeats
this process until `condition` evaluates to `nil`.

```clojure
fe > (setq i 0)
0
fe > (while (< i 3)
       (print i)
       (setq i (+ i 1)))
0
1
2
nil
```

#### `(quote expression)`

Returns `expression` unevaluated.

```clojure
fe > wow
error: void-variable wow  ;; There is no binding for the name wow.
fe > (quote wow)
wow
fe > (hello world)
error: void-function hello  ;; A name in head position is resolved through
                            ;; its function cell and reported as a missing
                            ;; function when that cell is empty.
fe > (quote (hello world))
(hello world)
```

As a bit of syntactic sugar, you can also use the single quote, `'expression`,
without parentheses:

```clojure
fe > 'wow
wow
fe > '(hello world)
(hello world)
```

#### Reader macros

Five more prefixes are syntactic sugar for ordinary forms. Only the reader
knows about them; the host or a prelude has to supply `quasiquote`, `unquote`
and `unquote-splicing`, which the core does not define.

| Written | Read as |
| --- | --- |
| `'x` | `(quote x)` |
| `` `x `` | `(quasiquote x)` |
| `,x` | `(unquote x)` |
| `,@x` | `(unquote-splicing x)` |
| `#'x` | `(function x)` |

`` ` `` and `,` are symbol delimiters, so no symbol may contain them. `#` is
not: it is an ordinary symbol character, and only the two-character sequence
`#'` is a reader macro. Emacs Lisp's function quote `#'x` reads as
`(function x)` (sub-plan 04D); a bare `#'` with nothing following is the
error `stray '#''`.

```clojure
fe > (quote #'car)
#'car
fe > (funcall #'car (list 1 2))
1
```

#### The two namespaces

Since sub-plan 04C, Fe has Emacs Lisp's namespace split: a symbol carries two
cells, a *value cell* and a *function cell*. Bare-symbol evaluation reads the
value cell, and `setq`, `let`, `makunbound` and `symbol-value` address it.
Call position resolves the function cell, and `fset`, `defalias`,
`fmakunbound` and `symbol-function` address it. A name can therefore hold a
value and a function at once:

```clojure
fe > (setq f 7)
7
fe > (fset 'f (lambda () 9))
(lambda nil 9)
fe > (list f (f))
(7 9)
```

A name in call position resolves through its function cell *only*: since the
namespace cut (sub-plan 04D), the bootstrap's callables live in function cells
and the transitional value-cell fallback is gone, so an empty function cell is
`void-function NAME` even when the value cell is full. The two namespaces are
independently visible: `(boundp 'car)` is `nil` (the primitive's value cell is
empty) while `(fboundp 'car)` is `t`, and a lexical value binding of a
primitive's name does not shadow it in call position:

```clojure
fe > (boundp 'car)
nil
fe > (fboundp 'car)
t
fe > (let car 5)
nil
fe > (car (list 1 2))
1
```

#### `(boundp symbol)`

Evaluates `symbol` and returns `t` if it has a binding in the current
environment or the global one, otherwise `nil`. A variable whose value is `nil`
is bound. `boundp` asks the *value* namespace; `(fboundp symbol)` below is
its function-cell twin.

```clojure
fe > (boundp 'typo)
nil
fe > (setq typo nil)
nil
fe > (boundp 'typo)
t
```

#### `(makunbound symbol)`

Evaluates `symbol`, removes the value from its innermost binding, and returns
the symbol. Naming it afterwards is `void-variable` again. `makunbound` acts
on the value cell only; the function cell survives, so an `fset`'d name stays
callable (`(fmakunbound symbol)` below is the mirror operation on the other
cell).

#### `(function form)`

A raw-form special form like `quote`, restricted to what Emacs Lisp's `#'`
abbreviation means. `(function SYMBOL)` is the symbol designator itself;
`(function (lambda ...))` and `(function (fn ...))` are the closure, built by
the same construction arm `lambda`/`fn` use and so capturing the lexical
environment; anything else is `unsupported-function-form`.

```clojure
fe > (function c2)
c2
fe > (funcall (function (lambda (x) (+ x 1))) 2)
3
fe > (funcall ((lambda (z) (function (lambda () z))) 7))
7
```

#### `(funcall function args...)`

A function-shaped special form: every operand is evaluated like an ordinary
call's argument list, the first result is resolved through the
function-designator chain (a symbol is followed through its function cell),
and the call is dispatched with the remaining values. The callable may be a
closure value, a native, or a symbol designator. A resolved value that is not
callable is `tried to call non-callable value`, and `(funcall)` with no
operands is `wrong-number-of-arguments`.

What it may *not* be is a macro or a special form. `funcall` hands the callable
already-evaluated values, and a callable whose operands stay raw would receive
the wrappers rather than the values, so `(funcall 'quote 'a)` and
`(funcall 'if 1 2 3)` are `invalid-function`, named after the operand the
program wrote, as Emacs names it. `funcall` and `apply` are themselves
function-shaped, so `(funcall 'funcall '+ 1 2)` is an ordinary 3. A designator
chain that dies in an empty function cell is `void-function` at the name the
program wrote too, not at the last link the chain reached.

```clojure
fe > (funcall (lambda (x) (+ x 1)) 2)
3
fe > (setq g 7)
7
fe > (fset 'g (lambda () 9))
(lambda nil 9)
fe > (funcall 'g)
9
```

A callable held as a *value* -- a lambda parameter or a `setq`'d variable, for
instance -- is called with `funcall`, since call position resolves the
function cell, not the value cell:

```clojure
fe > (fset 'apply-to-3 (fn (f) (funcall f 3)))
(lambda (f) (funcall f 3))
fe > (apply-to-3 (lambda (x) (* x x)))
9
```

#### `(apply function args... list)`

Like `funcall`, but the final operand must be a list whose elements are
spread into the call as extra arguments:

```clojure
fe > (apply '+ 1 2 (list 3 4))
10
fe > (setq lst '(9 8))
(9 8)
fe > (apply 'cons lst '(7 6))
((9 8) . 7)
fe > lst
(9 8)
```

The spread does not mutate the caller's list -- `lst` still holds `(9 8)`
after `apply` rebuilt it -- and a final operand that is not a proper list is
`apply: last argument must be a proper list`, raised only after every operand
form has run. A callable-only `(apply 'f)` has no final operand at all and is
that same error. `(apply)` with no operands is `wrong-number-of-arguments`,
and `apply` rejects a macro or a special form exactly as `funcall` does.

#### `(fset symbol function)`

Both arguments are evaluated -- the first is a *value* naming the target
symbol, not a quote -- and `function` is stored in `symbol`'s function cell.
Returns `function`. Because the function cell is distinct from the value
cell, a name can be both a variable and a function at once.

```clojure
fe > (fset 'square (lambda (n) (* n n)))
(lambda (n) (* n n))
fe > (square 4)
16
```

#### `(defalias alias definition)`

Both arguments are evaluated; `definition` -- an object or a symbol
designator -- is stored in `alias`'s function cell, and `alias` is returned.
Resolution at call time follows the designator chain, so an alias to a name
that is later given a function picks it up:

```clojure
fe > (defalias 'first 'car)
first
fe > (first (list 1 2))
1
fe > (defalias 'a 'b)
a
fe > (fset 'b (lambda () 1))
(lambda nil 1)
fe > (a)
1
```

#### `(symbol-function symbol)`

Returns the raw contents of `symbol`'s function cell -- a designator is
returned as the symbol, not followed. An empty cell is `void-function NAME`.

```clojure
fe > (fset 'h (lambda (x) x))
(lambda (x) x)
fe > (funcall (symbol-function 'h) 5)
5
```

#### `(symbol-value symbol)`

Reads `symbol`'s global value cell directly. An empty cell is
`void-variable NAME`.

#### `(fboundp symbol)`

`t` if `symbol`'s function cell holds anything, else `nil`. Unlike `boundp`,
it never consults the value cell and never errors: an unbound name is `nil`.
Since the namespace cut moved the bootstrap into function cells,
`(fboundp 'car)` is `t`; the mirror question on the value side,
`(boundp 'car)`, is `nil`.

#### `(fmakunbound symbol)`

Empties `symbol`'s function cell and returns the symbol. The value cell is
untouched, so the pair with `makunbound` keeps the two namespaces disjoint:
`makunbound` empties only the value cell (leaving the function callable), and
`fmakunbound` empties only the function cell (leaving the variable readable).

#### `(and ...)`

Evaluates each argument until one results in `nil` — the last argument’s value
is returned if all the arguments are true.

```clojure
fe > (and 1 2 3)
3
fe > (and 1 nil 3)
nil
```

#### `(or ...)`

Evaluates each argument until one results in true, in which case that argument’s
value is returned. Returns `nil` if no arguments are true.

```clojure
fe > (or 1 2 3)
1
fe > (or nil 2 3)
2
```

#### `(do ...)`

Evaluates each of its arguments and returns the value of the last one.

```clojure
fe > (do
       (print "wow")
       nil
       (print 2 nil)
       (+ 3 4))
wow
2 nil
7
```

#### `(unwind-protect body cleanup...)`

Evaluates `body`, then evaluates the `cleanup` forms as an implicit `do`.
The cleanup forms run exactly once on every way out of `body`: an ordinary
return, a Lisp error, a host interrupt, or evaluation step-budget
exhaustion. The value of the whole form is `body`'s value; the cleanup
forms' values are discarded.

```clojure
fe > (unwind-protect 42 (print "cleanup"))
cleanup
42
fe > (unwind-protect (car 1) (print "cleanup ran anyway"))
cleanup ran anyway
error: expected pair, got integer
```

Nested `unwind-protect` forms run their cleanups innermost first (LIFO),
whether or not `body` errors:

```clojure
fe > (unwind-protect
       (unwind-protect 1 (print "inner"))
       (print "outer"))
inner
outer
1
```

If a cleanup form itself raises, that failure is reported directly (it does
not become a normal, catchable Fe error) and cleanup evaluation moves on to
the next entry -- an enclosing `unwind-protect`'s own cleanup, or, for the
outermost one, the host. Whichever error was already unwinding when the
cleanup failed is still what ultimately reaches the caller or the host; the
cleanup's own failure does not replace it and does not stop any other
pending cleanup from running.

Cleanup forms see the lexical environment `unwind-protect` was entered
with, not any bindings `body` introduced, and objects that environment
reaches remain valid for the cleanup to use even if `body` triggers
garbage collection before it exits.

#### `(catch tag body...)`

Emacs Lisp's non-local exit handler (sub-plan 06C): evaluates `tag`, then
evaluates the `body` forms as an implicit `do`. If a `(throw tag value)`
anywhere in the body delivers to a matching tag, the catch stops the body
and returns `value` immediately; otherwise the whole form returns the last
body form's value. `(catch 'a)` with an empty body is `nil`. A bare
`(catch)` with no tag at all is `wrong-number-of-arguments`.

```clojure
fe > (catch 'tag (throw 'tag 7) 99)
7
fe > (catch 'a (catch 'a (throw 'a 1)) 2)
2
fe > (catch 'a)
nil
```

Tags match by `eq`: a catch and a throw agree when the two tags are the
same object, or both the same integer. Floats, strings and freshly built
lists are distinct objects, so they do not match by content, and `nil`
never matches as a tag at all. The innermost catch whose tag matches wins.
A `throw` that finds no matching catch raises `no-catch`, reported through
the ordinary error path as the message `no-catch TAG VALUE` until a real
condition object replaces it (sub-plan 06D).

Nested `catch` forms with distinct tags let a program choose which level a
throw unwinds to:

```clojure
fe > (catch 'outer (catch 'inner (throw 'inner 1) 2) 3)
1
fe > (catch 'outer (catch 'inner (throw 'outer 1) 2) 3)
1
```

`unwind-protect` cleanups registered between a catch and a throw run as the
throw unwinds past them, innermost first, exactly as they do on an ordinary
error; the delivered value is the catch's result either way.

```clojure
fe > (setq log '())
()
fe > (catch 'tg
       (unwind-protect (throw 'tg 'done)
         (setq log (cons 'inner log))))
done
fe > log
(inner)
```

#### `(throw tag value)`

Emacs Lisp's non-local exit (sub-plan 06C): an ordinary function, not a
special form, whose two arguments evaluate normally. It searches the
innermost live `catch` whose tag is `eq` to `tag` and delivers `value`
there, abandoning the evaluation between the throw and that catch. A throw
that finds no matching catch raises `no-catch TAG VALUE` through the
ordinary error path. Exactly two arguments are required; `(throw)`,
`(throw 'x)` and `(throw 'x 1 2)` are `wrong-number-of-arguments`.

The throw search does not cross a native re-entry boundary: a native that
re-enters evaluation from inside a catch and throws is contained to its own
nested run, so a catch outside that run is not honoured and the throw
raises `no-catch` inside it. This is a recorded divergence from Emacs,
which unwinds C frames of its own in the same situation; fe's C activations
between the runs are live and cannot be abandoned.

### Functions

#### `(cons car cdr)`

Creates a new pair with the given `car` and `cdr` values.

```clojure
fe > (setq p (cons 1 2))
(1 . 2)
fe > p
(1 . 2)
```

#### `(car pair)`

Returns the first element of the `pair`, or `nil` if `pair` is `nil`.

```clojure
fe > p
(3 . 4)
fe > (car p)
3
```

#### `(cdr pair)`

Returns the second element of the `pair`, or `nil` if `pair` is `nil`.

```clojure
fe > p
(3 . 4)
fe > (car p)
3
fe > (cdr p)
4
```

#### `(env)`

Returns a list of all symbols in the current environment.

#### `(set symbol value)`

An ordinary function, unlike `setq` above: both `symbol` and `value` are
evaluated, so the target is usually quoted. `set` always writes `symbol`'s
*global* value cell, even when a lexical binding of the same name is in
scope in the calling environment -- that lexical binding is neither read
nor written. Returns `value`.

Requires exactly two arguments; a wrong argument count is
`wrong-number-of-arguments`, checked before either argument form is
evaluated. Once the count is right, both forms are evaluated left to right,
and only then is the resulting first value checked to be a symbol
(`wrong-type-argument` if not) -- so a type error in that check never
erases a side effect the second form already had.

```clojure
fe > (set 'fresh 7)
7
fe > fresh
7
fe > ((lambda () (setq x 9)
        (list ((lambda (x) (list (set 'x 2) x)) 1) x)))
((2 1) 2)
```

In the last example, `(setq x 9)` runs where no lexical `x` is in scope, so
it writes the global cell; the inner lambda's parameter `x` then shadows
that global binding.  `set` ignores the shadowing parameter entirely --
`(set 'x 2)` returns 2 and the parameter is still 1, so the inner list is
`(2 1)` -- and writes straight through to the global cell, which is why the
outer `x` reads back as 2.  Compare the `setq` example above, where the
same shape leaves the global at 9.

#### `(setcar pair value)`

Sets the first element of `pair` to `value`.

```clojure
fe > (setq p (cons 1 2))
(1 . 2)
fe > p
(1 . 2)
fe > (setcar p 3)
nil
fe > p
(3 . 2)
```

#### `(setcdr pair value)`

Sets the second element of `pair` to `value`.

```clojure
fe > (setq p (cons 1 2))
(1 . 2)
fe > p
(1 . 2)
fe > (setcdr p 4)
nil
fe > p
(1 . 4)
```

#### `(list ...)`

Returns all its arguments as a list.

```clojure
fe > (list 1 2 3)
(1 2 3)
```

#### `(not value)`

Returns true if `value` is `nil`, else returns `nil`.

```clojure
fe > (not 1)
nil
fe > (not nil)
t
```

#### `(is a b)`

Returns true if the values `a` and `b` are equal in value. Numbers compare
by mathematical value across the two numeric types, so `(is 3 3.0)` is `t`:
integers compare exactly, float-vs-float pairs keep Fe's historical epsilon
tolerance (`IsNearlyEqual`), and a mixed pair converts the integer to double
and then takes **the same epsilon tolerance** — a mixed pair is never
stricter than the same two values both spelled as floats, so
`(is 3 (cube-root 27))` and `(is 3.0 (cube-root 27))` are both `t` even
though `(cube-root 27)` is `3.0000000000000004`. Strings are equal if
equivalent, and all other values are equal only if they are the same
underlying object. `is` is Fe's own broad comparator — recorded as such by
05A's Decision 2, whose contract is the tolerant one — not an Emacs Lisp
form; `eq` and `eql` below are the exact comparisons, and they are Emacs
semantics.

#### `(eq a b)`

Emacs Lisp's identity operator (05A Decision 2, rows E1-E3, landed 05D):
`t` if `a` and `b` are the same object, or if both are integers with the same
value — the fixnum rule Emacs' `(eq 3 3)` depends on. Everything else is
identity: two separately-read `3.0` floats are two boxed objects, so
`(eq 3.0 3.0)` is `nil`, and `(eq "a" "a")` is `nil` for the same reason.

```clojure
fe > (eq 3 3)
t
fe > (eq 3.0 3.0)
nil
fe > (eq "a" "a")
nil
```

#### `(eql a b)`

`eq`, or two same-type numbers equal by value — integers by value, floats by
their exact bits. `(eql 3 3.0)` is `nil` because the types differ, and
`(eql 0.0 -0.0)` is `nil` because the sign bit differs; `(eql 1.5 1.5)` is
`t` (rows E4-E5).

#### `(atom x)`

Returns true if `x` is not a pair, otherwise `nil`.

#### `(print ...)`

Prints all its arguments to `stdout`, each separated by a space and followed by
a newline.

### Numbers

Fe has two numeric types, like Emacs Lisp: **integers** (signed 64-bit,
printed bare) and **floats** (IEEE 754 doubles). The numeric functions below
are the full tower: both types flow through every arithmetic, comparison,
equality and math-native path, and each operator preserves integers where
Emacs preserves them and promotes to float where Emacs promotes.

The reader produces both types since 05D's cut, and the two spellings
round-trip: `42` reads as an integer and prints `42`; `42.0` reads as a float
and prints `42.0`. A bare integer spelling never becomes a float, so the
tower's integer paths are reachable from ordinary source text -- `(/ 7 2)` is
the integer `3`, `(/ 1 0)` is `arith-error` -- and a float keeps its `.0`, so
it never impersonates an integer on output. Floats print shortest-round-trip
(the shortest decimal that reads back to the same double: `0.1` prints
`0.1`, `(log 8)` prints all its digits) with a decimal point or exponent
always present, and the nonfinite spellings `1.0e+INF`/`-1.0e+INF`/
`0.0e+NaN`/`-0.0e+NaN` (05A Decision 4). Host-made integers flow through the
tower exactly as read ones do.

Integer overflow (in any direction, on any operator) and integer division by
zero are `arith-error`, never a silent wrap and never a promotion to float.
The numeric family's type errors are `wrong-type-argument`, the Emacs
condition name.

#### `(= number ...)`

Numeric equality, matching Emacs Lisp: `(= a b c ...)` is true only if every
argument is numerically equal to every other, by *mathematical value across
types* — `(= 3 3.0)` is `t`. Until sub-plan 02C of the Emacs-subset hard cut,
`=` was instead Fe's historical, non-Emacs assignment primitive -- see
`(setq symbol value ...)` above for its replacement.

At least one argument is required: `(=)` is `wrong-number-of-arguments`.
One argument is `t` without comparing *or type-checking* anything — a chain
of one has no pair to run, so `(= "a")` and `(= t)` are `t`, matching Emacs.
Two or more are compared as one chain, left to right.

Ordinary-function semantics, like every other function here and unlike
`setq`/`set` above: every argument form is evaluated, left to right, before
any value is type-checked, so a type error in an early operand never erases
a side effect a later operand's form already had. The adjacent pairs are then
compared left to right and the loop stops at the first false pair, which is
Emacs' rule: `(= 1 2 "a")` is `nil`, the answer having been settled before
the string was reached, while `(= 1 1 "a")` does reach it and is
`wrong-type-argument`. Only the type *check* is short-circuited; the
operand's form has already run either way.

```clojure
fe > (= 1 1 1)
t
fe > (= 1 1 2)
nil
fe > (= 1)
t
fe > (= "a")
t
fe > (=)
error: wrong-number-of-arguments
fe > (= 1 "1")
error: wrong-type-argument
fe > (= 1 2 "a")
nil
fe > (= 1 1 "a")
error: wrong-type-argument
```

Integers compare exactly, by value. Floats compare with ordinary IEEE 754
`==`, and a mixed pair compares by the mathematical value of both after the
integer converts exactly to double (exact up to 2^53, like any 64-bit to
double conversion). So `0.0` and `-0.0` compare equal, and `NaN` never
compares equal to itself:

```clojure
fe > (= 0.0 -0.0)
t
fe > (= (sqrt -1) (sqrt -1))
nil
```

#### `(< a b ...)`

Returns true if its arguments are in strictly increasing numerical order:
`(< a b c)` is `t` if and only if `a < b` and `b < c`. One argument is `t`,
with no type check (`(< "a")` is `t`); no arguments is
`wrong-number-of-arguments`. Comparisons use the same
mathematical-value-across-types rule as `=`, and the same first-false-pair
short-circuit: `(< 2 1 "a")` is `nil` because the chain already failed, and
`(< 1 2 "a")` is `wrong-type-argument` because it did not.

#### `(<= a b ...)`

Like `<`, with non-strict comparisons: `(<= a b c)` is `a <= b` and
`b <= c`. Same arity rules.

#### `(> a b ...)`

Like `<`, in strictly decreasing order: `(> a b c)` is `t` if and only if
`a > b` and `b > c`.

#### `(>= a b ...)`

Like `>`, with non-strict comparisons: `(>= a b c)` is `a >= b` and
`b >= c`. Same arity rules.

#### `(/= a b)`

Numerical inequality: true if `a` and `b` are *not* numerically equal, by
the same mathematical-value rule as `=`. Unlike the chained comparisons,
`/=` is strictly binary -- a third argument is `wrong-number-of-arguments`,
exactly as in Emacs Lisp, not a chain.

#### `(+ ...)`

Adds all its arguments, left to right, integer-preserving: all-integer
operands stay integer, and any float operand promotes the whole addition to
float. Integer overflow is `arith-error`. With no arguments the answer is
the identity element, the integer `0`, as in Emacs.

#### `(- ...)`

Subtracts all its arguments, left to right, with the same preservation and
promotion as `+`. With no arguments the answer is the integer `0`. With one
argument the answer is its negation: `(- 5)` is `-5` (negating `INT64_MIN`
is `arith-error`).

#### `(* ...)`

Multiplies all its arguments, with the same preservation and promotion.
Integer overflow is `arith-error`. With no arguments the answer is the
identity element, the integer `1`.

#### `(/ ...)`

Divides all its arguments, left to right. Unlike the three above it has no
identity element to return, so no arguments at all is
`wrong-number-of-arguments`. All-integer division stays integer and
truncates toward zero -- integer `7` by integer `2` is `3`, `-7` by `2` is
`-3` -- and a float operand promotes to float (`(/ 7 2.0)` is `3.5`). With
one integer argument the answer is the reciprocal, truncated: `(/ 5)` on an
integer operand is the integer `0`. Integer division by zero is
`arith-error`. Since 05D's cut the literals read as integers, so `(/ 7 2)` is
`3` and `(/ 1 0)` is `arith-error` from source text.

#### `(integerp x)`

Returns `t` if `x` is an integer, else `nil` -- a float, a string, a list,
anything else is `nil`. Exactly one argument; a leftover argument is
`wrong-number-of-arguments`. Since 05D's cut every integer literal is one.

#### `(floatp x)`

Returns `t` if `x` is a float, else `nil`. Exactly one argument; a leftover
argument is `wrong-number-of-arguments`.

#### `(condition-case variable body handler...)`, `(signal condition data)`, and `(error format ...)`

`condition-case` returns `body`'s normal value or runs the first matching
handler. A handler receives the condition object, a cons whose car is the
condition symbol and whose cdr is its data, bound to `variable` (or no binding
for `nil`), and its forms are an implicit `do`. `error` catches ordinary
conditions including `wrong-type-argument` and `arith-error`; hierarchy
matching is transitive (`overflow-error` is a `range-error`, an
`arith-error`, and an `error`). `quit` is separate and requires a `quit` or
`t` handler. An error raised by a handler bypasses that active handler and
searches an enclosing one. `signal` evaluates its two operands and raises
`(condition . data)`. Unknown condition symbols raise `(error "Invalid error
symbol" SYMBOL)`. `error` formats its message at signal time and raises an
`error` condition. Its supported directives are `%%`, `%d` (integer), `%s`
(a string without quotes, any other object printed), and `%S` (an object
printed with Fe's normal object writer); width, precision, flags, and every
other directive are rejected with an `error` condition.

```clojure
fe > (condition-case e (signal 'arith-error '(7)) (error e))
(arith-error 7)
fe > (condition-case nil (signal 'quit nil) (quit 'stopped))
stopped
```

#### Math functions

The math natives follow per-function return types, as in Emacs Lisp:
`floor`, `ceiling`, `round` (round-half-even) and `truncate` return
integers, either one-argument (`(floor 7.5)` is `7`) or two-argument (divide
first, then round: `(ceiling -7 2)` is `-3`); a result outside int64's range
is `arith-error`. `expt` returns an integer for two integer arguments with a
non-negative exponent (`(expt 2 8)` is `256`), and promotes to float for a
negative exponent or any float argument (`(expt 2 -1)` is `0.5`,
`(expt 2.0 8)` is `256.0`). The transcendentals — `sqrt`, `sin`, `cos`,
`tan`, `asin`, `acos`, `atan`, `exp`, `log` — return floats always
(`(sqrt 16)` is the float `4.0`).

In the standalone `fe` binary the Fex extensions shadow `floor`, `ceiling`,
`log`, `round` and `truncate` with their own one-argument versions (see
`doc/implementation.md`); the core natives described here are what a host
embedding of `fe.c` gets, and what `scripts/math.fe`'s golden exercises is
the Fex side of that split.
