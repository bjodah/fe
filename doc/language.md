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
number: `'.` is the symbol `.` and `.5` is `0.5`.

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
fe > (setq square (lambda (n) (* n n)))
(lambda (n) (* n n))
fe > (square 4)
16
```

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
(setq ++
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
error: void-function hello  ;; Fe has one namespace, but a name in head
                            ;; position is reported as a missing function.
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

Four more prefixes are syntactic sugar for ordinary forms. Only the reader
knows about them; the host or a prelude has to supply `quasiquote`, `unquote`
and `unquote-splicing`, which the core does not define.

| Written | Read as |
| --- | --- |
| `'x` | `(quote x)` |
| `` `x `` | `(quasiquote x)` |
| `,x` | `(unquote x)` |
| `,@x` | `(unquote-splicing x)` |
| `#'x` | `x` |

`` ` `` and `,` are symbol delimiters, so no symbol may contain them. `#` is
not: it is an ordinary symbol character, and only the two-character sequence
`#'` is a reader macro. Fe has one namespace, so Emacs Lisp's function quote
`#'x` reads as plain `x`.

#### `(boundp symbol)`

Evaluates `symbol` and returns `t` if it has a binding in the current
environment or the global one, otherwise `nil`. A variable whose value is `nil`
is bound.

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
the symbol. Naming it afterwards is `void-variable` again.

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
error: expected pair, got double
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

Returns true if the values `a` and `b` are equal in value. Numbers and strings
are equal if equivalent, all other values are equal only if they are the same
underlying object.

#### `(atom x)`

Returns true if `x` is not a pair, otherwise `nil`.

#### `(print ...)`

Prints all its arguments to `stdout`, each separated by a space and followed by
a newline.

#### `(= number ...)`

Numeric equality, matching Emacs Lisp: `(= a b c ...)` is true only if every
argument is numerically equal to every other. Until sub-plan 02C of the
Emacs-subset hard cut, `=` was instead Fe's historical, non-Emacs assignment
primitive -- see `(setq symbol value ...)` above for its replacement.

At least one argument is required: `(=)` is `wrong-number-of-arguments`.
One argument is `t` without comparing anything; two or more are compared as
one chain, left to right.

Ordinary-function semantics, like every other function here and unlike
`setq`/`set` above: every argument form is evaluated, left to right, before
any value is type-checked, so a type error in an early operand never erases
a side effect a later operand's form already had. Every operand is then
checked to be a number and compared without short-circuiting, even once the
chain is already known unequal -- a later operand's form has always run and
been checked by the time `=` returns. A non-number argument is
`wrong-type-argument`.

```clojure
fe > (= 1 1 1)
t
fe > (= 1 1 2)
nil
fe > (= 1)
t
fe > (=)
error: wrong-number-of-arguments
fe > (= 1 "1")
error: wrong-type-argument
```

Fe has only doubles today, so equality is ordinary IEEE 754 `==`, not
special-cased: `0.0` and `-0.0` compare equal, and `NaN` never compares
equal to itself.

```clojure
fe > (= 0.0 -0.0)
t
fe > (= (sqrt -1) (sqrt -1))
nil
```

#### `(< a b)`

Returns true if the numerical value `a` is less than `b`.

#### `(<= a b)`

Returns true if the numerical value `a` is less than or equal to `b`.

#### `(+ ...)`

Adds all its arguments together.

#### `(- ...)`

Subtracts all its arguments, left to right.

#### `(* ...)`

Multiplies all its arguments.

#### `(/ ...)`

Divides all its arguments, left to right.
