# fe/.meta-docs/plans/101-ADD-REGEX.md

## Plan: Add tiny-regex-c-backed regex support to fe

### Goal

Expose regular-expression capabilities to Fe Lisp using the shared `tiny-regex-c` engine.

This replaces the current POSIX-regex-backed extension with a portable engine that is also used by kg.

---

## Design decisions

### Regex is an extension, not Fe core

Keep regex in the Fex/native-extension layer rather than adding it to the minimal `fe.c` core.

Rationale:

* Fe core stays small.
* Regex objects need native storage and GC handling.
* Fex already has a regex type slot and regex install hook.

### Use tiny-regex-c, not POSIX regex

Replace `<regex.h>` usage with the `tiny-regex-c` API.

Benefits:

* same dialect as kg;
* same tests as kg;
* no platform POSIX-regex dependency;
* controlled Emacs-like dialect.

### Match result must be capture-ready

Even if initial engine support only returns whole-match spans, Fe’s public result shape should not need to change later when captures arrive.

Recommended result shape:

```lisp
((START END) (CAP1-START CAP1-END) ...)
```

Where:

* span 0 is the whole match;
* capture spans follow;
* unmatched captures are represented as `nil`;
* indexes are byte offsets.

---

## Phase 1: Replace backend storage

### Current model

The existing regex extension compiles into a native regex object and frees it via the Fex GC hook.

### New model

Compiled regex object should own:

* compiled-regex byte storage;
* compiled-regex size;
* possibly original pattern for printing/debugging.

Example native structure:

```c
struct FexRegex {
    unsigned char *storage;
    unsigned storage_size;
    re_t regex;
};
```

GC should free `storage` and the wrapper object.

---

## Phase 2: Lisp API

### Required functions

```lisp
(compile-re PATTERN)
(match-re REGEX TEXT)
```

Recommended behavior:

```lisp
(compile-re PATTERN)
```

Returns:

* regex object on success;
* structured error object/list on failure.

```lisp
(match-re REGEX TEXT)
```

Returns:

* `nil` on no match;
* list of spans on match.

Example:

```lisp
(match-re (compile-re "foo") "xxfooyy")
;; => ((2 5))
```

With future captures:

```lisp
(match-re (compile-re "\\(foo\\)bar") "xxfoobaryy")
;; => ((2 8) (2 5))
```

### Optional helper functions

Add only if useful:

```lisp
(re-match PATTERN TEXT)
(re-match? PATTERN TEXT)
```

`re-match` compiles and matches in one call.

`re-match?` returns `t` or `nil`.

If these are added, they should use the same backend as `compile-re` / `match-re`.

---

## Phase 3: Error shape

Choose one structured error representation and use it consistently.

Recommended simple shape:

```lisp
(error CODE MESSAGE)
```

Examples:

```lisp
(error bad-pattern "missing closing \\)")
(error too-complex "match step limit exceeded")
(error buffer-too-small "compiled regex storage too small")
```

If Fe convention prefers `(CODE MESSAGE)`, document that instead and keep it consistent.

---

## Phase 4: Tests

Add tests for:

* successful compile;
* invalid compile;
* no match;
* whole match;
* zero-length match;
* case-insensitive matching once engine flags exist;
* capture-ready result shape;
* GC/free path for compiled regex objects.

Example tests:

```lisp
(match-re (compile-re "foo") "xxfooyy")
;; expected: ((2 5))

(match-re (compile-re "^") "abc")
;; expected: ((0 0))

(match-re (compile-re "zzz") "abc")
;; expected: nil
```

When captures are implemented:

```lisp
(match-re (compile-re "\\(foo\\)bar") "xxfoobaryy")
;; expected: ((2 8) (2 5))
```

---

## Phase 5: Standalone fe integration

Ensure standalone `fe` initializes and installs the regex extension.

Document whether regex functions are always available in standalone `fe`, or only when a specific Fex initialization path is used.

---

## Phase 6: Documentation

Document:

* function names;
* supported regex syntax;
* result shape;
* byte-offset indexing;
* error representation;
* differences from full Emacs regex.

---

## Non-goals

* Full Emacs regex compatibility.
* Multi-line matching.
* Unicode-aware spans.
* Replacement expansion.
* kg editor command implementation.
