// Copyright 2020 rxi, https://github.com/rxi/fe
// Copyright 2024 Chris Palmer, https://noncombatant.org/
// SPDX-License-Identifier: MIT

#ifndef FE_H
#define FE_H

#include <stddef.h>  // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>

// The embedding contract: the C functions, types, and callback signatures
// below. A Lisp-only change such as FE_LANGUAGE_VERSION 3's
// call-position/reader cut does not move this. Version 2 (sub-plan
// 03F of kg's Emacs-subset program) was the frame machine's bound rename:
// `FeEvalOptions.max_depth` split into `max_frames` and
// `max_native_reentry`, and `FeArenaStats.peak_evaluation_depth` split into
// `frame_capacity`, `peak_frame_depth` and `peak_native_reentry`. Every host
// that set the old fields gets a compile error, which was the point -- their
// *meaning* changed, not just their name, so silently keeping the old
// spelling would have been the wrong kind of compatibility.
//
// Version 3 (sub-plan 04D of kg's Emacs-subset program) is the Lisp-2
// namespace cut: `FeDefineNative` now registers into a symbol's function
// cell instead of its value cell, and `FeSetFunction`/`FeGetFunction`/
// `FeIsFBound` (added in 04C under version 2) are part of the same visible
// contract. A host that called `FeDefineNative` for names it then called
// from Lisp must recompile anyway -- the tripwire is kg's own
// `static_assert(FE_API_VERSION == 2)`, which this bump fires.
//
// Version 4 (sub-plan 05D of kg's Emacs-subset program) is the numeric cut:
// 05A's placement (a) Decision inserted `FeTInteger` into the public `FeType`
// enum immediately after `FeTDouble`, renumbering every later constant
// including `FeTPtr`; `FeMakeInteger`/`FeToInteger` (05B) joined the
// constructor/accessor pair; and the reader now produces integers from
// source text, so a host-made and a read number share one meaning. The bump
// was deliberately deferred to the cut so the whole numeric contract moves as
// one visible break -- a "compatible" break that silently reinterprets `42`
// is the worst kind (05D).
//
// Version 5 (sub-plan 06D of kg's Emacs-subset program) is the condition
// cut: `FeCompletion`, `FeGetCompletion`, `FeGetCondition`,
// `FeRaiseCompletion` and `FeResignal` join the surface, and an error a host
// used to see only as text now also carries an `(error-symbol . data)`
// object. Existing calls keep compiling; the bump is the two-axes rule 06D
// set, where the language version moves for programs and this one for hosts,
// and both moved.
//
// Version 6 (sub-plan 07B) removes `FeSetStrictArity` and
// `FeGetStrictArity` outright -- no deprecated no-op, no lax mode -- because
// arity is now unconditional. A host that called either gets a compile
// error, which is the whole notification: there is no runtime answer that
// would still be true.
//
// Version 7 (sub-plan 11C) adds one entry point,
// `FeTryEvaluateStringWithOptions`: the protected *string* evaluation, the
// sibling `FeTryCallWithOptions` (version 5) has needed since a host that
// loads a file from inside an evaluation turned out to have the same problem
// a host that calls a callback has. Nothing is removed and nothing changes
// meaning, so every existing call keeps compiling; the bump exists because a
// version that does not move cannot tell kg whether the fe it is linking
// against has the entry point at all -- the same reasoning version 3's
// language bump used, and the same `static_assert` tripwire.
//
// Version 8 (Phase 12's fe fix cycle) adds the input-unit trio --
// `FeEnterInputUnit`, `FeReadInputForm`, `FeLeaveInputUnit`, with the
// `FeInputUnit` token they pass between them -- and nothing is removed or
// changed in meaning, so every existing call keeps compiling. It exists
// because no composition of the surface before it let a host run its own
// read-eval loop inside ONE input unit in the CURRENT run, which is what a
// `load` written in Lisp needs: `FeEvaluateString*` drives the reader
// itself and starts a nested run per form, `FeTryEvaluateString*` adds a
// throw wall, and neither `ctx->input_scope` nor the diagnostic label had
// any public setter at all. A host looping over `FeReadString` plus `eval`
// got one shared scope for every form it evaluated -- re-opening the
// divergence sub-plan 12C Part 2 had just closed -- and reported every
// error at the position of its own `eval` call rather than at the form's.
// The bump is the same `static_assert` tripwire the earlier ones are.
//
// Version 9 (Phase 18 of kg's Emacs-subset program) adds the two halves the
// value namespace was missing from C: `FeGetValue`, the read half of
// `FeSet`, and `FeMakeUnbound`, the C spelling of what `makunbound` does to
// a global binding. Nothing is removed or changed in meaning. It exists
// because a host that keeps a variable's value SOMEWHERE ELSE for a while
// -- kg's buffer-local bindings stash the displaced value beside the symbol
// and swap it back when the buffer comes round again -- has to be able to
// read the cell it is about to overwrite and to put unboundness back into
// it. Before this the value namespace could be written from C and asked
// whether it was bound, but not read, so the only way to take a value out
// was to evaluate `(symbol-value 'x)` -- a nested run, a step budget and a
// raise, for a two-word load. Both are cell accessors in the shape
// `FeSet`/`FeIsBound` already have: the GLOBAL binding, never an
// environment entry.
//
// Version 10 (Phase 19) adds one declaration, `FeErrorMessageString`: Emacs'
// `error-message-string` rendering of a condition object, written into the
// caller's buffer. It exists for the one caller the Lisp primitive of the
// same name cannot serve -- a host's `FeSetErrorFn` callback, which is
// handed fe's own bare text and wants the sentence. It allocates nothing,
// raises nothing, and suspends the step budget across the render, because
// everything it might otherwise charge for happens while an error is
// already being reported.
//
// Version 11 (Phase 18's follow-up) adds the dynamic-binding location seam:
// `FeBindingSaveFn`, `FeBindingTargetFn` and `FeSetBindingFns`. A shallow
// dynamic binding saves a symbol's value cell and puts it back into that
// same cell; a host that keeps a variable's value somewhere else *some of
// the time* -- kg's buffer-local bindings, which stash the displaced value
// beside the symbol and swap it back when the buffer comes round again --
// needs the restore to name the storage the binding actually displaced,
// which may be a different place by the time the form exits, or gone. The
// two callbacks are how it says so; with neither set (the default) fe binds
// and restores exactly as version 10 did, so no existing host changes
// behaviour and none has to recompile for meaning. The bump is the same
// `static_assert` tripwire every earlier one is.
//
// Version 12 (kg's embedded-prelude program, the post-prelude collect) adds
// one declaration, `FeCollectGarbage`: an immediate, forced mark-and-sweep,
// the same one `ArenaCanAllocate` and `MakeObject`'s exhaustion path already
// run on their own schedule, now reachable from outside fe.c. Before this a
// host had no way to ask for a collection -- `CollectGarbage` stays
// `static`, unreachable from any other translation unit including fe's own
// test suite, which is why `ForceCollection` in test_api.c allocates
// disposable objects in a loop until natural exhaustion triggers one. kg's
// motivating case is a startup-only host, evaluating a large, fixed set of
// definitions once and wanting the transient reader/macro-expansion garbage
// that loading them produced back before a session's own work starts,
// without waiting for that session to allocate enough to trigger a
// collection on its own. Nothing is removed and no existing declaration
// changes meaning, so every existing call keeps compiling; the bump exists
// because a version that does not move cannot tell kg whether the fe it is
// linking against has the entry point at all -- the same reasoning
// versions 7, 8 and 11 (the input-unit trio, the value-cell readers, the
// binding-location seam) already used for an addition with no removal.
//
// Version 13 (Phase 23.2 of kg's Elisp data-model program) is the payload
// substrate's host surface: `FeOpenContextWithOptions` with the
// `FeOpenOptions` record it takes, and five payload fields on
// `FeArenaStats`. Phase 23.1 built the substrate itself -- a bump-allocated,
// compactable region carved out of the caller's arena, which the Phase 22
// ADR selected -- entirely inside fe, with nothing in this header at all.
// What this version adds is the two things a host cannot do without: ASK for
// a region (the arena is the host's memory, so how it is divided is the
// host's decision, not a compiled-in constant), and SEE what the region is
// doing (capacity, live bytes, high-water mark, compactions, and requests
// that could not be met). Nothing is removed and no existing declaration
// changes meaning -- `FeOpenContext` opens the same partition, byte for
// byte, that it did under version 12 -- so every existing call keeps
// compiling; the bump exists because a version that does not move cannot
// tell a host whether the fe it is linking against has the entry point and
// the fields at all, the same reasoning versions 7, 8, 11 and 12 used.
// `FE_LANGUAGE_VERSION` does not move: no Lisp program can reach any of
// this, since no release type owns a payload until Phase 25.
//
// Version 14 (Phase 24 of the same program) is the vector cut, and it is a
// real ABI break rather than an addition: `FeTVector` is inserted into the
// `FeType` enumeration immediately after `FeTString`, renumbering every later
// constant including `FeTPtr` and the three `FeTFex*` extension slots, so a
// host that stored an `FeType` value across the boundary, or that spells one
// of those slots in a `switch`, must recompile. It is 05A's placement
// Decision again and for the same reason: the vector sits beside the string,
// the other sequence and the other type whose contents live outside its own
// cell. What the version adds beside the enumerator is the vector's own
// surface -- `FeMakeVector`, `FeVectorLength`, `FeVectorRef` and
// `FeVectorSet`, the construction / length / checked-ref / checked-set
// quartet -- and, for the first time, an `FeArenaStats` whose payload fields
// move in an ordinary run: a vector's elements ARE its payload block, so a
// host that carves no region cannot make one.
//
// Version 15 (Phase 25 of the same program) is the string cut, and four
// things in this header follow from it. A string is a header plus a payload
// block of bytes now, with its LENGTH in the header, where it was a chain of
// seven-byte cells terminated by a NUL: so `FeMakeStringBytes` builds one
// from a buffer and a length, and a host can hold a string with a NUL in it,
// while `FeMakeString` is that same function called with `strlen`.
// `FeStringBytes` asks for the length and the bytes in ONE call, which is
// what a host copying into a fixed buffer wants; `FeStringByteLength` and
// `FeCopyStringBytes` keep their exact meaning and were already binary-safe,
// neither having ever handed out an interior pointer. `FeWriteFn`'s
// allocation contract is stated for the first time -- a write callback MAY
// allocate, and the printer re-derives its payload addresses after every
// callback so that it can. And `FeOpenContext` now carves the payload region
// it used to leave at zero, because a symbol's name is a string and a context
// without a region cannot finish opening: `FePayloadPercentNone` is REMOVED
// rather than kept as a partition no program can run in, which is the ABI
// break in this version.
#define FE_API_VERSION 15

// The Lisp language Fe evaluates. Version 1 was implicit -- Fe's historical,
// non-Emacs dialect, where `=` assigned and returned nil. Version 2 (sub-plan
// 02C of the Emacs-subset hard cut) was the first explicit contract: `setq`
// and `set` are assignment, and `=` is chained numeric equality, matching
// Emacs Lisp. Version 3 (sub-plan 04D) is the Lisp-2 namespace cut: a symbol
// in call position resolves through its function cell only -- the
// value-cell fallback is deleted -- `#'x` reads as `(function x)`, and
// `boundp`-of-a-callable changed meaning (the bootstrap callables now live
// in function cells, so `(boundp 'car)` is nil while `(fboundp 'car)` is t).
// Version 4 (sub-plan 05D) is the numeric cut: integer literals (`42`, `+5`,
// `1.`) and the Emacs float spellings (`.5`, `1e3`, the nonfinite
// `1.0e+INF`/`0.0e+NaN` family) read as their own types instead of every
// number reading as a double, floats print shortest-round-trip with an
// explicit `.0`, integer division truncates, `(/ 1 0)` is `arith-error`, and
// `eq`/`eql` are core primitives with Emacs' identity semantics.
//
// Version 5 (sub-plan 06D) is the condition cut: `condition-case`, `signal`
// and a condition hierarchy exist, and every raise the evaluator makes now
// carries a symbol and a data list. A program that only ever printed the
// message text still runs; one that branched on that text can now branch on
// the symbol instead.
//
// Version 6 (sub-plan 07B) is the strict-arity cut, and it changes what
// existing programs *mean*. Calls that used to answer are now errors:
// `((lambda (x) x))` bound `x` to nil and is `wrong-number-of-arguments`;
// `((lambda () 1) 2)` dropped the extra argument and is the same; `(car 1 2)`
// and `(quote 1 2)` ignored their surplus operands and now reject them
// before evaluating any of them. Two answers changed rather than
// disappearing: `(and)` was nil and is `t`, and `(signal 'error)` was an
// arity error and is now accepted with nil data. `print` gains a
// one-argument minimum, so the zero-argument blank line is gone. Malformed
// parameter lists raise `invalid-function` instead of prose errors. Fe's
// dotted-tail and bare-symbol rest spellings are unaffected.
// See doc/language.md and doc/c-api.md.
// Version 7 is the whole of Phase 8's language contract, both of its slices
// under one bump. It protects `t`, `nil` and keyword symbols (including `:`
// itself) from value or function assignment and makes keywords
// self-evaluating: programs that assigned `t` now signal `setting-constant`,
// and `:foo` no longer needs quoting. It also makes the reader strict, and
// that half breaks programs too -- a previously readable one may no longer
// read at all. A bare `#` and every `#`-initial symbol are gone (Fe's own
// scripts/concatenate.fe named a function `#`); `[...]`, `#:`, `#s(...)` and
// symbol escapes such as `a\ b` are named read errors rather than symbols;
// an unknown string escape errors where the backslash used to be dropped;
// and a string escape must land in one byte, so `"\0"` and `"\400"` error
// where they used to truncate the string silently. What it adds is the
// measured Emacs subset: UTF-8 character literals with the `\C-`/`\M-`
// modifiers, greedy `\x`, radix integers, and one-based source lines on
// evaluated-file and evaluated-string diagnostics. One language bump for the
// phase, covering the reader break as well as the constants: 8.0's release
// version moves, both compatibility macros stay where 08B put them.
//
// Version 8 (sub-plan 10B) is the reflective-expansion cut: `macroexpand-1`,
// `macroexpand` and `macroexpand-all` are names the language answers.  The
// first two expand a macro call without evaluating it -- one step, and
// Emacs' fixpoint -- and the third names itself as unimplemented instead of
// answering `void-function`.  Nothing an existing program could write breaks;
// what moves is what those three names *answer*, which used to be
// `void-function` for all of them, and `(fboundp 'macroexpand)`, which was
// nil.  The bump is deliberate under this file's own "compatible additions do
// not require a bump" rule, and the reasoning is recorded with the commit:
// under the program's §0.4 (no external constituency) the only consumer of
// this macro is kg's `static_assert`, and a version that does not move is a
// version that cannot tell kg whether the fe it is linking against has these
// names.  `FE_API_VERSION` does not move: no declaration in this header
// changed.
//
// Version 9 (sub-plan 11B) is the special-variable cut, and it is the first
// language bump in this series that changes what an *existing* program
// answers rather than only adding names.  A symbol marked by
// `internal--mark-special` binds dynamically: `let` and `let*` over it swap
// the global value cell instead of extending the lexical environment, a
// function that reads the name free sees the bound value, `setq` inside the
// binding writes the binding, and the previous value -- or the symbol's
// unboundness -- is restored on every completion kind.  Nothing that never
// calls `internal--mark-special` can tell the difference, and closure and
// defun *parameters* stay lexical unconditionally, which is Emacs' own
// answer under `lexical-binding: t`.  `special-variable-p` is the reader for
// the flag.  `FE_API_VERSION` does not move here either -- no declaration in
// this header changed -- though sub-plan 11C moves it for the protected
// string entry, and the two land under one `FeVersion` "10.0".
//
// Language version 10 (sub-plan 12B) is two changes to control flow, both of
// which change what an existing program answers.  A condition handler
// established *inside* an `unwind-protect` cleanup is now honored by a raise
// from that cleanup, where before every raise inside a running cleanup
// behaved as unhandled -- 06A Decision 4's replace-the-completion rule is
// unchanged and now applies only where nothing in the cleanup can handle it.
// And `eval` exists: Emacs' `(eval FORM &optional LEXICAL)`, evaluating FORM
// in the caller's own run so conditions, throws and quits out of it reach
// enclosing handlers and catches, with a non-nil LEXICAL rejected by name.
// `FE_API_VERSION` stays at 7 -- no declaration in this header changed.
//
// Language version 11 (Phase 13.1) is a classification repair: `signal`,
// `error` and `keywordp` are function-shaped primitives -- their arms
// evaluate every operand, and `(special-form-p ...)` is nil for all three on
// Emacs -- but had no row in the evaluator's `primitive_is_function[]`, so
// `funcall` and `apply` rejected them as special forms.  `(funcall 'signal
// 'error '("x"))`, `(apply 'error '("boom"))` and `(mapcar 'keywordp '(:a
// 1))` answered `invalid-function` and now do what Emacs does; `functionp`,
// and the host's `FeIsFunction` under it, answer t for all three where they
// answered nil.  It is the same kind of bump as version 8's: no program that
// ran under 10 answers differently under 11, because every affected program
// raised.  It is a bump all the same, for version 8's reason -- kg's
// compile-time `static_assert` is this macro's only consumer, and a macro
// that does not move cannot tell kg whether the fe it links against can
// reach `signal` through the two entry points a prelude is built on.
// `FE_API_VERSION` stays at 8: no declaration in this header changed, and
// the GC stress knob that lands in the same slice is a build-time
// `FE_GC_STRESS` inside fe.c, not a C contract.
// Version 12 (Phase 14 of kg's Emacs-subset program) is the symbol surface
// and the reader escapes that go with it, and unlike versions 8 and 11 it IS
// a break: a program that read before may read differently now, and one that
// printed before may print differently.  `intern`, `intern-soft`,
// `symbol-name`, `make-symbol`, `gensym`, `put`, `get` and `symbol-plist`
// are new names that answered `void-function`.  A backslash in a token was a
// named read error and is now Emacs' symbol escape, so `(a\ b)` is a
// one-element list where it used to be a diagnostic and `\1` is the symbol
// `1` where it used to be one too; `\.` in a list is an ordinary element
// rather than a dotted-tail marker; `##` reads as the symbol with the empty
// name.  The printer escapes a symbol name that would otherwise read back as
// something else, so the symbol `.` now prints `\.` and `(intern "a b")`
// prints `a\ b`.  And an uninterned symbol named like a keyword is no longer
// a keyword: `keywordp` now asks whether the interner self-bound it, which
// is what makes `(keywordp (make-symbol ":a"))` nil as it is on Emacs.
// `FE_API_VERSION` stays at 8: no declaration in this header changed.
// Version 13 (Phase 19) is Emacs' error RENDERING, and it is a break in two
// directions.  `error-message-string` is a new name that answered
// `void-function`; every condition symbol in the standard hierarchy now
// carries the `error-message` property Emacs gives it, so `(get
// 'wrong-type-argument 'error-message)` answers "Wrong type argument" where
// it answered nil, and a `signal` of that condition reports `Wrong type
// argument: listp, 6` instead of the bare symbol.  And the writer now
// escapes a backslash inside a printed string, so `(format "%S" "a\\b")` is
// `"a\\b"` where it was `"a\b"` -- the last printed form that did not read
// back.  `FE_API_VERSION` moves to 10 in the same slice for
// `FeErrorMessageString`, the C half of the first of those.
// Version 14 (Phase 20) is two additions, landed under one bump.  `string<`
// and `string>` are new names that answered `void-function`: Emacs'
// lexicographic string order, taking a string or a symbol on either side.
// And `end-of-buffer` and `beginning-of-buffer` join the condition
// hierarchy, so `(signal 'end-of-buffer nil)` is legal where it was
// `(error "unknown condition ...")`, `(get 'end-of-buffer 'error-message)`
// answers "End of buffer" where it answered nil, and an `error` handler now
// catches both.  Neither is a break -- no program that ran under 13 answers
// differently under 14 -- and both bump the macro for Phase 10's recorded
// reason: kg's compile-time `static_assert` is the only consumer, and a
// macro that does not move cannot tell kg whether the fe it links against
// has the names it reflects with.  `FE_API_VERSION` stays at 10: no
// declaration in this header changed.
// Version 15 (Phase C2 of kg's fe-simplification plan) is one byte in the
// reader: the form feed (`\f`, 0x0C) is whitespace, which it was not.  Emacs'
// `read1` retries on exactly space, form feed, newline, tab and carriage
// return; fe had the other four, so a page separator -- the conventional
// Elisp section break, `s.el:770` and `f.el:39` -- was a symbol constituent,
// and `nil\f\nnil` read as one symbol named "nil\f" and answered
// `void-variable` where Emacs reads two `nil`s.  It IS a break, in the
// narrow direction a program can notice: a symbol whose name contained a
// literal form feed no longer reads back as itself unless the byte is
// escaped, which is what `(intern "a\fb")` has printed as `a\<FF>b` since
// version 12's printer escapes.  A form feed inside a string body is
// unaffected -- it never was reader syntax there.  `FE_API_VERSION` stays at
// 12: no declaration in this header changed.
// Version 16 (Phase 24 of kg's Elisp data-model program) is the VECTOR cut:
// the first Lisp-visible aggregate fe has ever had, its reader and writer
// syntax, and the sequence contract that comes with it.  `[1 2 3]` reads as
// a vector where it was the named read error `unsupported read syntax:
// vector brackets`, and prints back in the same syntax; `vector`,
// `make-vector`, `vectorp`, `aref`, `aset`, `vconcat`, `length` and `elt`
// are eight names that answered `void-function`.  `length` and `elt` are
// generic over lists, strings and vectors -- Emacs' own contract, including
// its asymmetry, where `(elt LIST 9)` past the end is nil (it routes to
// `nth`) and `(elt VECTOR 9)` is `args-out-of-range` (it routes to `aref`).
// `end-of-file` joins the condition hierarchy, which is what `[1 2` raises,
// measured on the pinned Emacs rather than invented; the same condition now
// names an unclosed LIST, whose message text is unchanged but which used to
// be a bare `error`.  A program that never writes a bracket and never calls
// one of the eight names cannot tell the difference, except for that one
// condition symbol.  `FE_API_VERSION` moves to 14 in the same slice for the
// C quartet and the `FeType` enumerator, and the two land under one
// `FeVersion` "20.0".
//
// Version 17 (Phase 25 of the same program) is what a length-bearing string
// makes reachable from Lisp.  `"a\0b"` is a three-byte string where that
// escape used to be the named read error `unsupported read syntax: NUL
// character in string`, and it prints as `"a\000b"` -- three octal digits, so
// that a digit after it stays a digit -- which reads back to the same three
// bytes.  `(aset STRING INDEX BYTE)` writes the byte in place and answers it,
// where every `aset` on a string was `unsupported: aset on a string`; a value
// above 255 is refused with Emacs' own sentence, since Emacs will not widen a
// string either, and `aset` therefore never changes a string's length in
// either dialect.  `length`, `aref` and `elt` still answer BYTES for a string
// -- fe's strings are byte strings and that contract is unchanged.
// `FE_API_VERSION` moves to 15 in the same slice, and the two land under one
// `FeVersion` "21.0".
//
// Version 18 (the frontier demand phase of kg's Elisp campaign) is one row
// in the condition hierarchy: `search-failed`, a child of `error` with
// Emacs' own message text.  `(signal 'search-failed '("z"))` is legal where
// it was `Invalid error symbol`, `(get 'search-failed 'error-message)`
// answers "Search failed" where it answered nil, and an `error` handler
// catches it.  It is not a break -- no program that ran under 17 answers
// differently under 18 -- and it bumps the macro for Phase 10's recorded
// reason: kg's compile-time `static_assert` is the only consumer, and a
// macro that does not move cannot tell kg whether the fe it links against
// has the condition its search family raises.  `FE_API_VERSION` stays at
// 15: no declaration in this header changed.
//
// Version 19 (the external-review correctness tranche of the same campaign)
// is the Fex boundary cut, and it changes what two extensions answer.  It
// lands under `FeVersion` "23.0" (which moves 22.0 -> 23.0); `FE_API_VERSION`
// stays at 15: no declaration in this header changed.  A
// length-bearing string has been readable since version 17, but the I/O and
// regex extensions still crossed their C-string boundaries through one
// helper: `read-file` built its record with `strlen`'s length -- reading
// /proc/self/cmdline answered the first argv element where the whole
// NUL-separated record was read -- and pattern/subject/path/mode strings
// were silently truncated at an embedded NUL, so a pattern "a\0b" behaved
// as "a".  `read-file` now builds its record from getdelim's byte count,
// and every boundary that cannot carry a NUL (open-file's path and mode,
// remove-file, execute's argv, compile-re's pattern, match-re's subject)
// refuses an embedded NUL by name instead of truncating; `write-file`
// keeps writing exact bytes, NULs included.  A program that never puts a
// NUL in one of those strings answers exactly what it answered under 18.
#define FE_LANGUAGE_VERSION 19

// The language version spelled as a string literal, so a banner that reports
// it composes the two and they cannot drift apart.
#define FE_STRINGIZE_(x) #x
#define FE_STRINGIZE(x) FE_STRINGIZE_(x)
#define FE_LANGUAGE_VERSION_STRING FE_STRINGIZE(FE_LANGUAGE_VERSION)

extern const char* FeVersion;

typedef double FeDouble;
typedef struct FeObject FeObject;
typedef struct FeContext FeContext;
typedef struct FeRoot FeRoot;
typedef FeObject* FeNativeFn(FeContext* ctx, FeObject* args);
typedef bool FeInterruptFn(FeContext* ctx, void* userdata);
typedef void FeErrorFn(FeContext* ctx, const char* err, FeObject* cl);
// A host cleanup registered with `FeProtectWithCleanup`. It runs at most once,
// must not raise, must not call back into the evaluator, and must not create
// Fe objects -- see `FeProtectWithCleanup`'s comment for the full contract.
typedef void FeCleanupFn(FeContext* ctx, void* data);
// The writer's byte sink, one byte per call. Unlike a mark callback
// (`FeSetMarkFn`), it MAY allocate Fe objects and it MAY raise: the printer
// holds no address into object storage across it, deriving a string's bytes
// or a vector's elements again after every call, precisely because a host's
// sink is where a host does host things -- kg's grows a buffer with
// `realloc` and can run out of memory. A raise from inside it leaves the
// printer by `longjmp`, which is an exit and not a half-written object.
typedef void FeWriteFn(FeContext* ctx, void* udata, char chr);
typedef char FeReadFn(FeContext* ctx, void* udata);
// The two halves of the dynamic-binding location seam (FE_API_VERSION 11),
// installed together by `FeSetBindingFns`. See its comment for the whole
// contract; in one line each: `FeBindingSaveFn` is asked, as a shallow
// dynamic binding is pushed, for an opaque token naming the storage the
// value cell it is about to shadow belongs to, and `FeBindingTargetFn` is
// asked, as that binding is undone, which symbol's value cell the saved
// value goes back into -- `nullptr` to drop it.
//
// The token is a `uintptr_t` and not a `void*` because that is what it is:
// a number fe does not interpret, which a host is free to make an index, a
// generation stamp, or (`(uintptr_t)p`) a pointer of its own. Zero is the
// value a binding carries when no save callback is installed, so a host
// that means something by zero must not mean something else by it.
typedef uintptr_t FeBindingSaveFn(FeContext* ctx, FeObject* symbol);
typedef FeObject* FeBindingTargetFn(FeContext* ctx,
                                    FeObject* symbol,
                                    uintptr_t tag);

// The kind of the most recent evaluation completion, read through
// `FeGetCompletion`. Sub-plan 06B (kg's Emacs-subset program) made the
// previously dormant kinds true at their producers: the interrupt path raises
// Quit, the step-limit/frame-wall/native-re-entry-wall ceilings raise Budget,
// and every ordinary `FeHandleError` stays Error. Throw is still unassigned
// until 06C. A kind is assigned before a completion reaches the host and stays
// valid until the next run's outermost barrier resets it, so a host reads the
// failing kind from inside its error callback and after it recovers. `quit`
// and `budget` are not signalable condition symbols (06A Decision 1 records
// that as a deliberate exclusion); they are completion kinds, observable only
// through this accessor until 06C/06D give them catch semantics.
typedef enum FeCompletion {
  FeCompletionNormal,
  FeCompletionError,
  FeCompletionThrow,
  FeCompletionQuit,
  FeCompletionBudget,
} FeCompletion;

typedef struct FeEvalOptions {
  size_t step_limit;
  size_t poll_interval;
  FeInterruptFn* interrupt;
  void* userdata;
  // The per-entry step budget every `unwind-protect`/`FeProtectWithCleanup`
  // cleanup gets while this call is unwinding through an error, an
  // interrupt, or its own `step_limit` running out. Zero selects a built-in
  // default. It is never the exhausted or cancelled budget the body was
  // running under -- a cleanup always gets a fresh one, so a body that ran
  // out of steps still gets a working cleanup -- and it is never unbounded
  // either, so a runaway cleanup terminates instead of hanging with no
  // escape. `interrupt`/`userdata`/`poll_interval` stay live for the same
  // drain, re-armed fresh per entry. See `doc/c-api.md`'s "Unwinding And
  // Cleanup" for the worst-case bound this implies.
  size_t cleanup_step_limit;
  // Lisp nesting: the maximum number of simultaneously live ordinary
  // evaluator frames (nested calls, nested special forms, self-expanding
  // macros, deep argument lists) the context-owned frame stack may hold at
  // once. This is a slot count, not a C-stack bound -- the frame machine
  // roots Lisp nesting in the arena, not in C recursion, so this limit costs
  // no C stack no matter how large it is. Zero selects the arena's own
  // physical capacity (`FeArenaStats.frame_capacity`); a nonzero value only
  // ever *lowers* that ceiling, never raises it past what the arena
  // partition actually holds. A push that would exceed the effective limit
  // fails before writing, with "evaluation frame limit exceeded". See
  // `doc/c-api.md`'s "Bounding And Cancelling Evaluation".
  size_t max_frames;
  // Native re-entry: the maximum number of nested evaluator runs a native
  // may start synchronously, one below another -- e.g. `internal--
  // with-current-buffer` calling `FeCall` on a body that itself calls
  // `internal--save-excursion`. Unlike `max_frames`, each level here *is* a
  // real C-stack bound: a native's own C activation, `FeCall`, `Evaluate`
  // and the nested run's barrier cannot be moved off the C stack, so this
  // has to stay a small number, not a large one. Calling a native from Lisp
  // is not by itself re-entry; only that native synchronously starting
  // another evaluation is, so an ordinary top-level host call is never
  // counted against this limit no matter how deep the Lisp nesting it
  // drives. Zero selects the built-in default (`DefaultNativeReentry`,
  // derived from the deepest synchronous re-entry any known embedding
  // actually nests, times a comfortable safety margin -- see its own
  // comment in fe_internal.h). Exceeding it raises "native evaluation
  // re-entry limit exceeded" before the nested run starts.
  //
  // Both `max_frames` and `max_native_reentry` are owned by the outermost
  // active evaluation: a nested `FeCallWithOptions` reached from a native
  // does not replace either ambient limit merely because it was handed
  // another `FeEvalOptions` pointer (see `BeginEvaluationControl`). While an
  // error is unwinding, every cleanup's own re-entry runs under the default
  // ceiling for both limits, not the abandoned body's -- but
  // `native_reentry_depth` itself (the live count, not the configured
  // limit) is not reset for the duration of that unwind: the real C
  // activations the abandoned computation was inside are still live below
  // the barrier until `longjmp` actually pops them, one run at a time. See
  // `doc/c-api.md`'s "Unwinding And Cleanup".
  size_t max_native_reentry;
} FeEvalOptions;

typedef enum FeType {
  FeTPair,
  FeTFree,
  FeTNil,
  FeTDouble,
  // Sub-plan 05A's placement (a) Decision (kg's Emacs-subset program): an
  // integer sits next to the double it was born from, renumbering every
  // later constant. See the version comment above -- the ABI break rides
  // 05D's FE_API_VERSION bump, not this slice.
  FeTInteger,
  FeTSymbol,
  FeTString,
  // Phase 24 of kg's Elisp data-model program: the vector, placed beside the
  // string it shares a shape with -- both are sequences, and both keep their
  // contents somewhere other than their own cell -- and renumbering every
  // later constant exactly as 05A's `FeTInteger` did. The ABI break rides
  // FE_API_VERSION 14 above. A vector's elements live in the payload region
  // (Phase 23's substrate), which is why a host that opens a context with no
  // payload carve cannot build one.
  FeTVector,
  FeTFn,
  FeTMacro,
  FeTPrimitive,
  FeTNativeFn,
  FeTPtr,

  // This is a disgusting/hilarious way to extend `FeType` in the Fex API: When
  // defining custom types in Fex, use these `FeTFex*` values. Example:
  //
  //   enum {
  //     FexTMyType = FeTFex0,
  //     FexTMyOtherType = FeTFex1,
  //   };
  //
  // This allows us to use `FeType` instead of `int` in the API.
  FeTFex0,
  FeTFex1,
  FeTFex2,
  // Add more as needed:
  //   * add them here
  //   * add cases for them in all relevant `switch`/`case` statements
  //   * add a slot and default name for them in `type_names`
  //   * assign your name for them in `FexInstallNativeFn`
  // TODO: Try to find a way to do this with less toil.

  FeTSentinel,
} FeType;

extern const char* type_names[];

extern FeObject nil;

[[nodiscard]] size_t FeMinimumArenaSize(void);
[[nodiscard]] size_t FeArenaAlignment(void);

// Read-only counters over state Fe already tracks at the sites that change
// it (`MakeObject`, `CollectGarbage`, `FePushGC`, the evaluation-depth and
// cleanup-stack pushes) -- not a live diagnostic surface, and not one this
// call itself grows: `FeGetArenaStats` allocates no Fe object, walks no
// list, and does not mutate `ctx`. It exists to answer "how close is the
// arena to full" questions before and during embedding changes that add to
// every allocation path.
typedef struct FeArenaStats {
  size_t
      total_slots;    // `ctx`'s fixed object capacity (see FeMinimumArenaSize).
  size_t free_slots;  // total_slots minus objects currently live.
  size_t peak_live_objects;    // high-water mark of live objects since
                               // FeOpenContext.
  size_t collection_count;     // CollectGarbage() calls so far.
  size_t peak_gc_stack_depth;  // high-water mark of the FePushGC root stack
                               // (bound: GcStackSize, 4096).
  // The arena's host-usable evaluator frame capacity -- i.e. NOT counting
  // the private cleanup reserve -- the same ceiling FeEvalOptions.max_frames
  // of 0 selects.
  size_t frame_capacity;
  // High-water mark of simultaneously live ordinary evaluator frames
  // (bound: FeEvalOptions.max_frames, default frame_capacity above); always
  // <= frame_capacity for a computation that never triggers cleanup's
  // private reserve.
  size_t peak_frame_depth;
  size_t peak_cleanup_stack_depth;  // high-water mark of the
                                    // unwind-protect/FeProtectWithCleanup
                                    // registry (bound: CleanupStackSize).
  // High-water mark of nested evaluator runs started synchronously from a
  // native (bound: FeEvalOptions.max_native_reentry, default
  // DefaultNativeReentry); zero for a program that never re-enters through
  // a native, no matter how deep its Lisp nesting.
  size_t peak_native_reentry;
  size_t allocation_failures;  // MakeObject() calls that still found no free
                               // slot after a collection.

  // The payload region (FE_API_VERSION 13), the pool the cells are priced
  // against: `FeOpenOptions.payload_percent` is what divides them, and the
  // frame capacity above is funded before either. Every context has a region
  // and every context has blocks in it, because a symbol's name is a string
  // and a string's bytes live there (FE_API_VERSION 15); the numbers below
  // are never all zero the way they were when only a vector could reach the
  // region.
  size_t payload_capacity_bytes;    // bytes carved for the region; the
                                    // denominator the other four are read
                                    // against.
  size_t payload_live_bytes;        // bytes currently held by published blocks,
                                    // block headers included.
  size_t payload_peak_bytes;        // high-water mark of payload_live_bytes.
  size_t payload_compaction_count;  // collections that moved survivors
                                    // down over reclaimed blocks.
  size_t payload_allocation_failures;  // payload requests the region could
                                       // not meet even after a collection,
                                       // i.e. `(payload-exhaustion)` raises.
} FeArenaStats;

[[nodiscard]] FeArenaStats FeGetArenaStats(const FeContext* ctx);

// Force an immediate collection (FE_API_VERSION 12): the same
// mark-and-sweep `ArenaCanAllocate`/`MakeObject`'s exhaustion path and the
// `FE_GC_STRESS` build already run on their own schedule, now callable on
// demand. `collection_count` in `FeGetArenaStats` moves by exactly one, and
// whatever was unreachable at the call comes back to `free_slots`; a call
// with nothing new to reclaim is still a real, counted collection rather
// than a no-op. Safe to call whenever re-entering the collector would be
// safe at all: not from inside a `mark_fn` or `gc_fn` callback, and not
// while a collection this same call started is still running (both raise
// through `FatalCollectorViolation`, exactly as an `ArenaCanAllocate`- or
// `MakeObject`-triggered collection would). It allocates nothing itself.
void FeCollectGarbage(FeContext* ctx);

// What `FeOpenOptions.payload_percent` means when it is not an ordinary
// percentage (FE_API_VERSION 13).
enum {
  // What a zero-initialized `FeOpenOptions` asks for, and what a null
  // `options` selects: Fe's own split of the arena, which is the Phase 22
  // ADR's selected 25% of what is left once the frame region is funded. Zero
  // means "Fe decides" here for the same reason it does in `FeEvalOptions` --
  // a host that has an opinion about one knob writes that one field and
  // leaves the rest alone.
  //
  // There is no "none" any more (FE_API_VERSION 15). It was a real partition
  // while only a vector could own a payload; now a symbol's name is a string
  // and a string's bytes are payload, so a context with no region cannot
  // finish opening, let alone run a program. The floor the core names need is
  // funded out of `FeMinimumArenaSize` and is not this percentage's to
  // withhold; what this divides is the surplus above that floor.
  FeDefaultPayloadPercent = 25,
};

// The knobs `FeOpenContextWithOptions` takes (FE_API_VERSION 13). Zero-
// initialize it -- `(FeOpenOptions){0}`, or a designated initializer that
// names only the fields it cares about -- and every field it does not name
// takes its documented default, which is what lets a later version add a
// field without touching a host that does not want it.
typedef struct FeOpenOptions {
  // How much of the arena becomes the payload region, as a percentage of the
  // bytes left once the frame region is funded. The cells get the rest, so
  // this is a division between the two pools and never a raid on the third:
  // frame capacity is identical at every value here.
  //
  // `FeDefaultPayloadPercent` (0, the zero-initialized value) selects Fe's
  // own split; 1 to 100 ask for exactly that percentage of the surplus.
  //
  // OUT OF RANGE IS REFUSED, NOT CLAMPED: anything negative or above 100
  // makes `FeOpenContextWithOptions` return null, exactly as a null arena or
  // one below `FeMinimumArenaSize` does. A clamp would hand back a context
  // partitioned to a number the host never asked for and no way to find out,
  // which is a lie about the one budget Fe's product contract is built on.
  int payload_percent;
} FeOpenOptions;

// Open a context with knobs (FE_API_VERSION 13). `ptr` and `size` are
// `FeOpenContext`'s arena, with the same requirements; `options` may be null,
// which selects every default.
//
// A host that calls this with default options is asking Fe to divide its
// arena Fe's way, which is what `FeOpenContext` below now asks for too: the
// two differ only in that this one can say a number.
[[nodiscard]] FeContext* FeOpenContextWithOptions(void* ptr,
                                                  size_t size,
                                                  const FeOpenOptions* options);
// `FeOpenContextWithOptions` with default options, and the entry point every
// host with no opinion about the split should keep calling. Until
// FE_API_VERSION 15 it carved no payload region at all; strings ended that,
// since a context whose region cannot hold the name `car` cannot open.
[[nodiscard]] FeContext* FeOpenContext(void* ptr, size_t size);
void FeCloseContext(FeContext* ctx);
void FeSetUserData(FeContext* ctx, void* userdata);
[[nodiscard]] void* FeGetUserData(const FeContext* ctx);
void FeSetErrorFn(FeContext* ctx, FeErrorFn* fn);
// The two collector callbacks. `mark_fn` is called once per reachable
// pointer-carrying object (`FeTPtr`, `FeTFex0`..`FeTFex2`) during the mark
// phase so the host can `FeMark()` whatever that object refers to; `gc_fn`
// is called once per object about to be freed.
//
// Both run *inside* collection, and both must RETURN NORMALLY. Neither may
// `longjmp` out, and neither may raise -- no `FeHandleError()`, no
// `FeRaiseCompletion()`, and nothing that raises on its behalf, including
// `FeCar`/`FeCdr` on a non-pair. There is no stack to unwind the mark walk
// from: its state *is* the graph it has reversed, so leaving non-locally
// abandons the arena half-reversed. fe detects a raise from inside
// collection and aborts with a message naming this contract rather than
// continuing on a heap that will fault later somewhere unrelated. Neither
// may allocate, for the same reason: an allocation can collect, and a
// nested collection would clear the outer one's marks.
//
// Beyond that the two run in DIFFERENT PHASES and have different rules.
//
//   - `mark_fn` runs inside the walk, which is the one window in which the
//     object graph is not readable: the walk stores its return path in the
//     objects it is passing (Deutsch-Schorr-Waite pointer reversal), so
//     while it is inside a `car` chain those cells hold parent links rather
//     than their own cars. A `mark_fn` may call `FeMark()` -- that is what
//     it is for -- and may read the object it was handed. It must not read
//     `car`/`cdr` of anything else.
//
//   - `gc_fn` runs in a FINALIZATION PASS of its own, after the walk has put
//     every field back and before anything is reclaimed or moved. The whole
//     graph is intact there, so a `gc_fn` may read the object it was handed
//     AND anything reachable from it, whether or not that is doomed too: a
//     dead pair may be printed even though its children are dead, and a dead
//     string or vector still owns its payload bytes at the handle it names.
//     Until FE_API_VERSION 15 this callback ran inline in the sweep, where
//     both of those were false; a host written against that behaviour needs
//     no change, since everything it was allowed to do it may still do.
//
//     `FeMark()` from a `gc_fn` IS A NO-OP, and calling it is not an error.
//     The pass runs on a decided graph and its value is precisely that every
//     doomed object is still readable, which holds only while nothing can
//     still change who is doomed: a mark taken here would make the payload
//     compaction retain a block whose owner the sweep then frees. Marking
//     from a finalizer never resurrected anything -- under the old inline
//     callback it kept an object the sweep had not reached yet and did
//     nothing for one it had already passed, an accident of arena order that
//     was never a contract.
//
// Printing is safe from either callback (`FeToString()` on the object it was
// handed): the writer does not charge the evaluation step budget or poll the
// interrupt while collecting, precisely so that the obvious diagnostic
// callback cannot trip the return-normally rule. `main.c`'s `-d` tracers are
// that callback, and `make check` runs them.
void FeSetMarkFn(FeContext* ctx, FeNativeFn* fn);
void FeSetGCFn(FeContext* ctx, FeNativeFn* fn);
// Where a dynamic binding's saved value came from, and where it goes back
// (FE_API_VERSION 11). Fe's `let` over a special variable is shallow: it
// saves the symbol's one value cell and writes the saved value back into
// that same cell when the form completes, on every completion kind. That is
// the whole truth for a host whose variables live in that cell and nowhere
// else. It is not the truth for a host that MOVES a variable's value --
// kg's buffer-local bindings keep one cell per symbol holding whichever
// per-buffer binding is current and stash the rest beside it -- because
// between the save and the restore the cell can come to hold a different
// buffer's binding, and the storage the `let` displaced can have moved
// aside, or ceased to exist.
//
// `save` is called by every dynamic bind, *before* the cell is read, with
// the symbol being bound. Whatever it answers is stored with the binding and
// handed back later, uninterpreted: fe never reads the number, never
// dereferences anything through it, and never frees anything. `target` is
// called by the matching restore and answers the symbol whose value cell
// receives the saved value -- the bound symbol itself for the ordinary
// case, some other symbol for a host that moved the storage, or `nullptr`
// to drop the saved value entirely, which is the answer for storage that no
// longer exists. Passing `nullptr` for either function (the default state
// of a fresh context) restores version 10's behaviour exactly, and one may
// be set without the other; with `save` unset every tag is zero.
//
// Both run under `FeCleanupFn`'s contract and one rule more. They must not
// raise, must not call back into the evaluator, and must not create Fe
// objects: `target` in particular runs inside the unwind of a completion
// that may already be a quit or an exhausted budget, where fe's own restore
// is two stores that cannot fail and the drain does not give it a barrier.
// The rule more is that they must not change which bindings exist: a
// callback that pushed or popped a dynamic binding would be editing the
// stack that is calling it.
void FeSetBindingFns(FeContext* ctx,
                     FeBindingSaveFn* save,
                     FeBindingTargetFn* target);
[[noreturn]] void FeHandleError(FeContext* ctx, const char* msg);
[[noreturn]] void FeRaiseCompletion(FeContext* ctx,
                                    FeCompletion kind,
                                    const char* msg);

// The completion kind of the last completion that reached a host boundary:
// `FeCompletionError` for an ordinary error, `FeCompletionQuit` for the
// interrupt path, `FeCompletionBudget` for the step-limit/frame/re-entry
// walls, and `FeCompletionNormal` after any normal top-level return. Always
// valid -- inside the error callback, and until the next run's outermost
// barrier resets it after recovery. This is Decision 5's (sub-plan 06A)
// additive migration path: a host telling quit from a genuine error reads
// this instead of comparing message strings, and the `FeErrorFn` signature
// itself is unchanged, so every existing host compiles and behaves as before
// without edits.
[[nodiscard]] FeCompletion FeGetCompletion(const FeContext* ctx);
// The completion's `(SYMBOL . DATA)` condition object.
//
// Every `FeCompletionError` has one, arena exhaustion included: since
// sub-plan 09B the two raises that cannot allocate signal pre-built
// `(arena-exhaustion)` and `(evaluation-stack-exhaustion)` conditions
// interned once by `FeOpenContext`, both children of `error` in the
// hierarchy, rather than the nil this comment used to promise. (They are
// shared objects re-stamped before each raise; a caught one is yours to read,
// not to keep or mutate. See `doc/c-api.md`.)
//
// It is nil for a completion that has no condition to describe: any
// `FeCompletionBudget` (the step, frame and native-re-entry walls, which
// `condition-case` deliberately cannot catch and which have no Emacs
// counterpart to name), a `FeCompletionQuit` raised while the arena is
// exhausted (`condition-case` decides a quit by completion *kind* before it
// looks at the object, and calling an interrupt an out-of-memory would be
// worse than saying nothing), and `FeCompletionNormal`.
[[nodiscard]] FeObject* FeGetCondition(const FeContext* ctx);
// The fully formatted text of that completion -- source label included --
// the same string `FeErrorFn` is handed. Valid until the next completion in
// this context.
[[nodiscard]] const char* FeGetCompletionMessage(const FeContext* ctx);
// Puts a completion contained by `FeTryCallWithOptions` back in flight in
// the enclosing run, with its kind, condition object and message intact, so
// an enclosing Lisp `condition-case` matches on the original condition
// symbol. Call it from the frame that made the protected call, after it
// returned false.
[[noreturn]] void FeResignal(FeContext* ctx);

[[nodiscard]] FeType FeGetType(const FeObject* obj);
[[nodiscard]] bool FeIsNil(const FeObject* obj);
[[nodiscard]] FeObject* FeNil(FeContext* ctx);

void FePushGC(FeContext* ctx, FeObject* obj);
void FeRestoreGC(FeContext* ctx, size_t idx);
[[nodiscard]] size_t FeSaveGC(const FeContext* ctx);
// Marks `obj` and everything reachable from it. The only thing a host calls
// it for is a `mark_fn` callback reporting what its pointer object refers
// to; see `FeSetMarkFn` above for the two rules such a callback lives under.
// It uses no C stack proportional to the graph and allocates nothing, but it
// does temporarily reverse the pointers of the objects it is walking, which
// is why those rules exist.
void FeMark(FeContext* ctx, FeObject* obj);

// Registers a C cleanup that runs exactly once, in the same last-in-first-out
// order as Lisp `unwind-protect` cleanups (both share one registry): on an
// ordinary return, a Lisp error, a host interrupt, or step-budget exhaustion.
// Call it from within an active evaluation -- typically a native function
// that is about to call `FeCall`/`FeEvaluate*` on a body it was handed, the
// way `unwind-protect` itself does. It runs when the nearest enclosing call
// form finishes evaluating (normally the native's own call, since that is
// the form still being evaluated when the native runs), or earlier still if
// some form enclosing that one raises first. Calling it outside any active
// evaluation registers a cleanup with no enclosing form to attach to, so
// nothing drains it on an ordinary return; only a later error will.
//
// `fn` is called with `data` and must not fail, must not call back into the
// evaluator, and must not create Fe objects; it may free non-Fe resources and
// call plain C or extension-internal functions. There is no cancellation:
// register only once ownership of the cleanup's resource is final. The
// registry is a fixed-size array sized like the GC stack; exceeding it
// raises "cleanup stack overflow" before `fn` or `data` are recorded, so the
// caller has allocated nothing through this call that it must now release
// itself.
void FeProtectWithCleanup(FeContext* ctx, FeCleanupFn* fn, void* data);

[[nodiscard]] FeObject* FeCons(FeContext* ctx, FeObject* car, FeObject* cdr);
[[nodiscard]] FeObject* FeMakeBool(FeContext* ctx, bool b);
[[nodiscard]] FeObject* FeMakeDouble(FeContext* ctx, FeDouble n);
// Sub-plan 05B of kg's Emacs-subset program: an `int64_t` number, dormant
// -- constructible and readable from the host API but producible by no Lisp
// program yet. `FeToDouble` accepts it, so a host-made integer already flows
// through every double-taking host read; `FeToInteger` is its mirror.
[[nodiscard]] FeObject* FeMakeInteger(FeContext* ctx, int64_t n);
// A string of exactly LENGTH bytes, copied from a buffer the caller owns and
// keeps (FE_API_VERSION 15). The bytes may contain NUL and are not
// terminated: a fe string carries its length, so it is whatever bytes it was
// given. `FeMakeString` is this with `strlen`, and remains the spelling for
// the ordinary case of a C string.
[[nodiscard]] FeObject* FeMakeStringBytes(FeContext* ctx,
                                          const char* bytes,
                                          size_t length);
[[nodiscard]] FeObject* FeMakeString(FeContext* ctx, const char* str);
[[nodiscard]] FeObject* FeMakeSymbol(FeContext* ctx, const char* name);
[[nodiscard]] FeObject* FeMakeNativeFn(FeContext* ctx, FeNativeFn fn);
[[nodiscard]] FeObject* FeMakePtr(FeContext* ctx, FeType type, void* ptr);
[[nodiscard]] FeObject* FeMakeList(FeContext* ctx, FeObject** objs, size_t n);

[[nodiscard]] FeObject* FeCar(FeContext* ctx, FeObject* obj);
[[nodiscard]] FeObject* FeCdr(FeContext* ctx, FeObject* obj);

// The vector surface (FE_API_VERSION 14): construction, length, checked ref,
// checked set. Four functions and no fifth, because everything else a host
// might want -- copying, concatenating, converting a list -- is those four in
// a loop, and a vector's elements live in the payload region, which no host
// may hold a pointer into. There is no borrowed-elements accessor and there
// will not be one: the storage MOVES when the collector compacts, and the
// only address that survives that is the `FeObject*` header these all take.
//
// `FeMakeVector` fills every slot with nil and needs a context whose arena
// was opened with a payload carve (`FeOpenContextWithOptions`); a context
// without one raises `(payload-exhaustion)` for any length, zero included.
// `FeVectorLength` is O(1) -- a vector's length is its payload block's child
// count, not a walk -- and so are the two accessors.
//
// All four raise rather than return an error code, which is fe's convention
// for a checked accessor (`FeCar`, `FeToInteger`): a non-vector is
// `(wrong-type-argument vectorp OBJ)` and an index at or past the length is
// `(args-out-of-range VECTOR INDEX)`, the same two conditions the Lisp
// `aref`/`aset` raise, because they are the same checks.
[[nodiscard]] FeObject* FeMakeVector(FeContext* ctx, size_t length);
[[nodiscard]] size_t FeVectorLength(FeContext* ctx, FeObject* vector);
[[nodiscard]] FeObject* FeVectorRef(FeContext* ctx,
                                    FeObject* vector,
                                    size_t index);
void FeVectorSet(FeContext* ctx,
                 FeObject* vector,
                 size_t index,
                 FeObject* value);

// The writer's default `car`-nesting bound: how deep `FeWrite()` and
// `FeToString()` descend into one object before emitting `#<truncated>`. It
// is public because it is the only sensible answer to "how much of a chain
// is a reader ever shown", and a host printing a bounded *sequence* of
// objects -- `main.c`'s escaping-raise trace is the one in this repository --
// should apply the printer's own number rather than restate it and let the
// two drift.
enum { FeWriteDefaultMaxDepth = 256 };

// Zero in any field selects that field's default; `max_depth`'s is
// `FeWriteDefaultMaxDepth` above.
typedef struct FeWriteOptions {
  size_t max_bytes;
  size_t max_nodes;
  size_t max_depth;
} FeWriteOptions;

void FeWrite(FeContext* ctx, FeObject* obj, FeWriteFn fn, void* udata, int qt);
[[nodiscard]] bool FeWriteWithOptions(FeContext* ctx,
                                      FeObject* obj,
                                      FeWriteFn fn,
                                      void* udata,
                                      int qt,
                                      const FeWriteOptions* options);
void FeWriteFile(FeContext* ctx, FeObject* obj, FILE* fp);

[[nodiscard]] FeObject* FeRead(FeContext* ctx, FeReadFn fn, void* udata);
[[nodiscard]] FeObject* FeReadFile(FeContext* ctx, FILE* fp);
[[nodiscard]] FeObject* FeReadString(FeContext* ctx,
                                     const char* source,
                                     size_t length,
                                     size_t* offset);

// The INPUT UNIT a host drives itself (FE_API_VERSION 8). An input unit is
// what `FeEvaluateString`/`FeEvaluateFile` each are: a source label errors
// raised inside it are prefixed with, a position within that source, and --
// since FE_LANGUAGE_VERSION 10 -- the scope a one-argument `defvar` mark
// made inside it belongs to. These three entry points hand that concept to a
// host that wants to run the read-eval loop itself, which is the shape a
// `load` written in Lisp has:
//
//   (let ((h (host-open path)))          ; FeEnterInputUnit
//     (unwind-protect
//         (let ((cell (host-read h)))    ; FeReadInputForm
//           (while cell
//             (eval (car cell))          ; the CURRENT run, not a nested one
//             (setq cell (host-read h))))
//       (host-close h)))                 ; FeLeaveInputUnit
//
// What this buys, and what nothing before it could give: the forms are
// evaluated by `eval`, in the run the loop is already inside, so a
// `condition-case` or a `catch` established OUTSIDE the loop receives a
// condition, throw or quit raised by a loaded form. `FeEvaluateString*`
// cannot do that -- it starts a nested run per form -- and
// `FeTryEvaluateString*` deliberately walls throws off. Meanwhile the label
// and the scope are the unit's, so errors report `path:LINE` of the form and
// a one-argument `defvar` in a loaded file does not leak into the next one.
//
// `FeInputUnit` is the ENCLOSING unit, saved by `FeEnterInputUnit` into
// storage the caller owns and handed back to `FeLeaveInputUnit`. Its fields
// are exposed only so the caller can allocate one; nothing outside fe should
// read or write them. Units nest, and the token is what makes them nest: a
// host that opens a unit inside a unit keeps one token per level.
//
// The unwind guarantee, which is the same one `FeEvaluateString` has and is
// stated in full in doc/c-api.md: a NORMAL exit needs the matching
// `FeLeaveInputUnit`; a CONTAINED abnormal exit (`FeTryCallWithOptions`,
// `FeTryEvaluateStringWithOptions`) restores the enclosing unit at the
// barrier whether or not the host got to leave; and an UNCONTAINED abnormal
// exit leaves for the host context -- scope 0, no label -- because every
// unit between the raise and the host is abandoned at once. A host that
// wants its own bookkeeping unwound too should leave from an
// `unwind-protect` cleanup or an `FeProtectWithCleanup` handler, which run
// during the drain, before either of those.
typedef struct FeInputUnit {
  const char* label;
  size_t scope;
  size_t offset;
  size_t line;
  bool has_offset;
  bool has_line;
} FeInputUnit;

// Enters a new input unit: takes the next scope number, publishes `label`
// as the source label for diagnostics raised inside it, and sets the
// position to line 1. Starts no run and drives no reader -- it is a state
// change on the context and nothing else, which is exactly why the loop
// above stays in the run it was already in. `label` is borrowed for the
// unit's lifetime, so it must outlive the matching `FeLeaveInputUnit` (this
// differs from `FeEvaluateString`, which borrows only for its own call).
// Writes the enclosing unit into `*enclosing`.
void FeEnterInputUnit(FeContext* ctx,
                      const char* label,
                      FeInputUnit* enclosing);

// Reads one form for the current input unit, and publishes the line that
// form STARTS on as the position an error raised while evaluating it will
// report. `FeReadString` cannot be used for this: it saves and restores the
// label and position around itself, so a host looping over it can publish
// neither, and it restarts line counting at 1 on every call.
//
// `offset` and `line` are the caller's cursor over one source, both
// in/out and both required: initialise them to 0 and 1 and hand the same
// pair back for each successive form. `offset` ends at the first byte not
// belonging to the returned form. Returns `nullptr` when no form remains,
// leaving the position where the last form left it. `source` may be
// `nullptr` only when `length` is zero, and Fe never reads at or beyond
// `length`. An embedded NUL inside that length is an error, not
// end-of-input, exactly as for `FeReadString`.
//
// The returned form has no root of its own, as `FeRead`'s and
// `FeReadString`'s do not: push it before allocating anything else.
[[nodiscard]] FeObject* FeReadInputForm(FeContext* ctx,
                                        const char* source,
                                        size_t length,
                                        size_t* offset,
                                        size_t* line);

// Leaves the unit `FeEnterInputUnit` entered, restoring the enclosing one's
// scope, label and position from the token it wrote.
void FeLeaveInputUnit(FeContext* ctx, const FeInputUnit* enclosing);

[[nodiscard]] size_t FeToString(FeContext* ctx,
                                FeObject* obj,
                                char* dst,
                                size_t size);
// Emacs' `error-message-string` of an ERROR object `(SYMBOL . DATA)` -- the
// same rendering the Lisp primitive of that name performs -- written into
// `dst` and always NUL-terminated, with the number of bytes written
// returned. `FeGetCondition`'s object is what a host passes: inside an
// `FeSetErrorFn` callback this turns fe's bare `wrong-type-argument` text
// into `Wrong type argument: listp, 6`.
//
// It allocates nothing, raises nothing, and does not charge the step budget
// or poll the interrupt, so it is safe on the error path where a raise
// cannot be delivered and the arena may be exhausted. Output beyond `size`
// is dropped; a circular DATA list costs `size` bytes of work, the bound the
// writer already lives by. A zero `size` renders nothing and writes nothing,
// so `dst` may be null only then.
[[nodiscard]] size_t FeErrorMessageString(FeContext* ctx,
                                          FeObject* error,
                                          char* dst,
                                          size_t size);
// The three ways to read a string's (or a symbol's name's) bytes out. None
// of them hands back a pointer into object storage and none ever will: the
// bytes live in the payload region and MOVE when the collector compacts, so
// the only address that survives that is the `FeObject*` these all take.
//
// `FeStringBytes` (FE_API_VERSION 15) is the one-call form, `snprintf`'s
// contract without the terminator: it answers the string's full byte length,
// and it copies the bytes into DST when they fit in SIZE. A caller compares
// the answer against SIZE to learn whether the copy happened; a caller that
// only wants the length passes `(nullptr, 0)`. DST may be null only when SIZE
// is zero. The older pair below says the same thing in two calls -- a length
// pass, then a copy that refuses rather than truncates -- and is what a
// caller sizing a heap buffer wants, since it has to ask twice anyway.
[[nodiscard]] size_t FeStringBytes(FeContext* ctx,
                                   const FeObject* obj,
                                   char* dst,
                                   size_t size);
[[nodiscard]] size_t FeStringByteLength(FeContext* ctx, const FeObject* obj);
[[nodiscard]] bool FeCopyStringBytes(FeContext* ctx,
                                     const FeObject* obj,
                                     char* dst,
                                     size_t size);
[[nodiscard]] FeDouble FeToDouble(FeContext* ctx, FeObject* obj);
[[nodiscard]] int64_t FeToInteger(FeContext* ctx, FeObject* obj);
[[nodiscard]] void* FeToPtr(FeContext* ctx, FeObject* obj);
void FeSet(FeContext* ctx, FeObject* sym, FeObject* v);
[[nodiscard]] bool FeIsBound(FeContext* ctx, FeObject* sym);
// The other two thirds of the value namespace from C (FE_API_VERSION 9).
// All four functions address the symbol's GLOBAL binding and never a
// lexical environment entry, which is the whole of their contract: a host
// calling them is talking about the cell `setq` writes when nothing shadows
// the name, not about whatever binding some frame currently has in force.
//
// `FeGetValue` is `FeSet`'s inverse and answers `nullptr` -- not `nil` --
// for an unbound name, so the caller does not have to ask `FeIsBound`
// first and cannot confuse "no value" with "the value nil". `FeMakeUnbound`
// is `makunbound`'s global arm: after it `FeIsBound` is false and a
// reference raises `void-variable`. Neither evaluates anything, so neither
// charges the step budget; both type-check their symbol, and
// `FeMakeUnbound` refuses a constant (`nil`, `t`, a keyword) with
// `setting-constant`, exactly as `FeSet` does.
[[nodiscard]] FeObject* FeGetValue(FeContext* ctx, FeObject* sym);
void FeMakeUnbound(FeContext* ctx, FeObject* sym);
// The Lisp-2 function namespace (sub-plans 04C/04D of kg's Emacs-subset
// program). `FeSet`/`FeIsBound` above keep their Emacs meaning -- the value
// namespace; these three address the function cell instead. `FeSetFunction`
// writes the cell (the object or symbol designator is stored as-is, so a
// `defalias`-style indirection stays a symbol). `FeIsFBound` asks whether the
// cell holds anything. `FeGetFunction` resolves the cell the way call-position
// lookup does -- following defalias symbol indirection iteratively; it returns
// `nil` when the name has no function binding (04D deleted the transitional
// value-cell fallback, so a value-cell callable is *not* resolvable through
// it). A self-referential chain (`(fset 'x 'x)`) is `nil` here too, and does
// *not* raise: this is the resolver a host calls, possibly with no evaluation
// running, and a C host cannot catch an Fe error, so it must answer rather
// than longjmp into a frame that has already returned. Every other reader of
// the chain -- call position, `funcall`, `apply`, `FeIsFunction` -- still
// raises `cyclic-function-indirection`. Use `FeIsFBound` to tell an empty cell
// (`nil`, not f-bound) from a cycle (`nil`, f-bound).
void FeSetFunction(FeContext* ctx, FeObject* sym, FeObject* fn);
[[nodiscard]] FeObject* FeGetFunction(FeContext* ctx, FeObject* sym);
[[nodiscard]] bool FeIsFBound(FeContext* ctx, FeObject* sym);
// `functionp`'s question: is `obj` something the evaluator will call as an
// ordinary function -- a lambda, a host native, or a function-shaped
// primitive? A symbol is resolved through the same function-cell designator
// chain `FeGetFunction` follows (so this asks about the symbol's binding, not
// about the symbol), an unbound name is false, and a cycle raises
// `cyclic-function-indirection` -- unlike `FeGetFunction`, which answers `nil`;
// resolve with that first if you need the non-raising answer for a name that
// may be cyclic. A macro, a special form (`if`, `quote`,
// `lambda`, ...) and any non-callable value are false, which is what Emacs'
// `functionp` answers for them and what `funcall`/`apply` reject as
// `invalid-function`.
[[nodiscard]] bool FeIsFunction(FeContext* ctx, FeObject* obj);
// Registers `fn` under `name` so that call position resolves it. Since
// sub-plan 04D's cut (FE_API_VERSION 3) this writes the symbol's *function*
// cell, the same cell `FeSetFunction`/`FeGetFunction` address and the one
// Lisp call-position resolution reads; before the cut it wrote the value
// cell. A name registered here is `(fboundp 'name)` t and `(boundp 'name)`
// nil.
void FeDefineNative(FeContext* ctx, const char* name, FeNativeFn* fn);

[[nodiscard]] FeObject* FeGetNextArgument(FeContext* ctx, FeObject** arg);
void FeRequireNoArguments(FeContext* ctx, const FeObject* args);
[[nodiscard]] FeRoot* FeCreateRoot(FeContext* ctx, FeObject* object);
[[nodiscard]] FeObject* FeGetRoot(const FeRoot* root);
void FeReleaseRoot(FeContext* ctx, FeRoot* root);
[[nodiscard]] FeObject* FeCall(FeContext* ctx,
                               FeObject* callable,
                               FeObject* const* arguments,
                               size_t count);
[[nodiscard]] FeObject* FeCallWithOptions(FeContext* ctx,
                                          FeObject* callable,
                                          FeObject* const* arguments,
                                          size_t count,
                                          const FeEvalOptions* options);
// The protected call: like `FeCallWithOptions`, but a non-normal completion
// is *returned*, not thrown past this frame. On true, `*result` holds the
// value. On false, nothing was written to `*result`, `error_fn` was not
// called, the host's own frame was not unwound, and `FeGetCompletion`,
// `FeGetCondition` and `FeGetCompletionMessage` describe what happened; the
// host either swallows it or `FeResignal`s it. The callee's cleanups run,
// its frames and GC-stack entries are discarded, and the caller's ambient
// evaluation-control record -- remaining steps included -- is restored.
// This is the entry point a native that re-enters evaluation should use:
// `FeCall`/`FeCallWithOptions` transfer a nested run's completion to the
// *enclosing* run's barrier, past the native's own C frame.
[[nodiscard]] bool FeTryCallWithOptions(FeContext* ctx,
                                        FeObject* callable,
                                        FeObject* const* arguments,
                                        size_t count,
                                        const FeEvalOptions* options,
                                        FeObject** result);
[[nodiscard]] FeObject* FeEvaluate(FeContext* ctx, FeObject* obj);
[[nodiscard]] FeObject* FeEvaluateWithOptions(FeContext* ctx,
                                              FeObject* obj,
                                              const FeEvalOptions* options);
[[nodiscard]] FeObject* FeEvaluateString(FeContext* ctx,
                                         const char* label,
                                         const char* source,
                                         size_t length);
[[nodiscard]] FeObject* FeEvaluateStringWithOptions(
    FeContext* ctx,
    const char* label,
    const char* source,
    size_t length,
    const FeEvalOptions* options);
// The protected string evaluation: `FeEvaluateStringWithOptions` under
// exactly the containment `FeTryCallWithOptions` gives a call. On true,
// `*result` holds the value of the last form evaluated. On false, nothing
// was written to `*result`, `error_fn` was not called, the host's own frame
// was not unwound, and `FeGetCompletion`/`FeGetCondition`/
// `FeGetCompletionMessage` describe what happened; the host either swallows
// it or `FeResignal`s it into the enclosing run. Forms before the raising
// one have already run and their side effects stand.
//
// This is the entry point a host that *loads* Lisp from inside an evaluation
// should use. `FeEvaluateString` is a nested run dressed as a top-level
// call, so a completion raised by the loaded text transfers to the outermost
// barrier -- past every `condition-case` between the load and the raise.
// A `throw` out of the loaded text is contained as the barrier-wall error it
// is; the containment barrier is a throw wall, as the protected call's is.
[[nodiscard]] bool FeTryEvaluateStringWithOptions(FeContext* ctx,
                                                  const char* label,
                                                  const char* source,
                                                  size_t length,
                                                  const FeEvalOptions* options,
                                                  FeObject** result);
[[nodiscard]] FeObject* FeEvaluateFile(FeContext* ctx,
                                       const char* label,
                                       FILE* file);
[[nodiscard]] FeObject* FeEvaluateFileWithOptions(FeContext* ctx,
                                                  const char* label,
                                                  FILE* file,
                                                  const FeEvalOptions* options);

#endif
