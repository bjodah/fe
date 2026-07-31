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

#### `(= symbol value)`

Sets the existing binding of `symbol` to the value `value` in the current
environment. If there is no such binding in the current environment, creates or
updates a binding in the global environment.

#### `(if condition then else ...)`

If `condition` is true, evaluates `then`; otherwise, evaluates every remaining
form as an implicit `do` and returns the last one, exactly as Emacs Lisp's `if`
does. `(if condition)` and `(if condition then)` with a false condition are
`nil`.

```clojure
fe > (= x 2)
nil
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
fe > (= square (lambda (n) (* n n)))
nil
fe > (square 4)
16
```

#### `(macro arguments ...)`

Creates a new macro.

Macros work similar to functions, but receive their arguments unevaluated and
return code which is evaluated in the scope of the caller. The first time a
macro is called the call site is replaced by the generated code, such that the
macro itself is only run once in each call site.

For example, we could define a macro named `++` to increment a numeric value by
1:

```clojure
(= ++
  (macro (sym)
  (list '= sym (list '+ sym 1))))
```

Then we could use it in the following `while` loop:

```clojure
(= i 0)
(while (< i 10)
  (print i)
  (++ i))
```

Upon the first call to `++`, the program code would be modified in place,
replacing the call to the macro with the code it generated. Thus, the above code
expands to, and is equivalent to, this code:

```clojure
(= i 0)
(while (< i 0)
  (print i)
  (= i (+ i 1)))
```

Subsequent iterations of the loop would run the new code which now exists where
the macro call was originally.

For more examples, see [macros.fe](../scripts/macros.fe).

#### `(while condition ...)`

If `condition` evaluates to true, evaluates the rest of its arguments. Repeats
this process until `condition` evaluates to `nil`.

```clojure
fe > (= i 0)
nil
fe > (while (< i 3)
       (print i)
       (= i (+ i 1)))
0
1
2
nil
```

#### `(quote expression)`

Returns `expression` unevaluated.

```clojure
fe > wow
nil  ;; There is no binding for the name wow, so its value is nil.
fe > (quote wow)
wow
fe > (hello world)
error: void-function hello  ;; There is no binding for the name hello, so its
                            ;; value is nil. And, nil is not callable.
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

### Functions

#### `(cons car cdr)`

Creates a new pair with the given `car` and `cdr` values.

```clojure
fe > (= p (cons 1 2))
nil
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

#### `(setcar pair value)`

Sets the first element of `pair` to `value`.

```clojure
fe > (= p (cons 1 2))
nil
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
fe > (= p (cons 1 2))
nil
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
