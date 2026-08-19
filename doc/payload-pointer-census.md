# The interior-pointer census

Phase 23.0 of kg's Elisp data-model program, the entry gate for the payload
substrate the Phase 22 ADR selected (Design B: stable `FeObject *` headers
over a bump-allocated, compactable payload region).  It is the checklist the
one publish protocol in `fe_internal.h` is enforced against, and it exists
NOW because fe has no movable payloads yet: today every row can be settled by
reading, and after 23.1 every row has to be settled by debugging.

The failure class is nelisp's, documented at
`/opt/nelisp/docs/design/147-box-layout-container-shrink.org`.  Its Phase 1.5
found that the expensive surface was not the ~302 *readers* -- those collapse
behind one accessor -- but the sites that hold a raw interior pointer across
an operation that moves the storage under it, and especially the ones that
WRITE through it.  Its summary states the lesson directly: "The hidden cost
was the interior-pointer WRITE surface, not the 302 accessors."  This census
is that inventory, taken before the storage can move.

## What counts as a payload here

fe stores a string as a cdr chain of typed cells.  Each cell's `car` word is
one tag byte and `StringBufferSize` = 7 bytes of text; `STRING_BUFFER(x)`
(`fe_internal.h`) is `&(x)->car.c + 1`, the interior pointer to those 7
bytes.  A symbol's name is such a chain.

Once Phase 25 migrates strings onto the substrate, those bytes live in the
payload region and MOVE when the compactor runs; the `FeObject *` header
does not.  So a payload pointer, today and after, is exactly: an address
derived from an object that names its stored TEXT.  A pointer to an object
(`FeObject *`), to a pair's `car`/`cdr` link, to a `FeContext` field, or to
host memory is NOT a payload pointer and is not censused as one -- Design B's
whole premise is that headers are stable.

Nothing else in fe migrates in this program's current scope.  Vectors
(Phase 24) do not exist yet, so they have no sites.

## How to read a row

Every row names a file, a function and the line the pattern is on, so a
reader can open it and see the pattern.  Line numbers are as of the commit
that adds this file; the function name is the durable half.

* **holds across alloc?** -- can an fe allocation (anything reaching
  `MakeObject`, `fe.c:872`, directly or through a callback) happen between
  the moment this site derives a payload address and the moment it last
  reads or writes through it?  `no` means the address is derived and spent
  with no allocation in between.  `YES` is the row that needs the protocol.
* **the rule that makes it safe** -- what a reader must not break.

## A. fe core: the direct payload dereferences

These eleven sites are every occurrence of `STRING_BUFFER` in fe's four core
translation units, plus the one place a string's stored bytes are read as a
whole word rather than through the macro, plus the one place they are
written by initialisation.  Every other payload access in fe -- in fe.c,
fe_eval.c, fe_run.c and fe_unwind.c alike -- goes through one of these.

| # | site | pattern | holds across alloc? | the rule that makes it safe |
| --- | --- | --- | --- | --- |
| A1 | `fe.c:764` `StringOperandLess` | `memcmp(STRING_BUFFER(left), STRING_BUFFER(right), StringBufferSize)` -- TWO payload pointers live in one expression | no | Both coercions that can allocate (`StringOperandChain`, which may build the string `"nil"`) happen before the loop, and the loop body allocates nothing. Both addresses are derived inside the expression that spends them. Under payloads: re-derive both per iteration; never hoist either out of the loop. |
| A2 | `fe.c:809` `IsStringEqual` | `STRING_BUFFER(obj)[i] != *str` per byte | no | `str` is a host C string, never a payload. The walk allocates nothing. |
| A3 | `fe.c:932` `BuildString` | `STRING_BUFFER(tail)[StringBufferSize - 1] != '\0'` -- the "is this cell full" guard | **YES** | The guard is read, then `FeCons` at `fe.c:933` allocates (and may collect), then `fe.c:946` re-derives the address. Safe today ONLY because `STRING_BUFFER` is a macro re-evaluated at each use and the address is never parked in a local. This is the constructor row the ADR's "publish protocol" condition is about. |
| A4 | `fe.c:946` `BuildString` | `STRING_BUFFER(tail)[strlen(STRING_BUFFER(tail))] = chr` -- a payload WRITE, and the census's only one outside initialisation | **YES**, see A3 | Two derivations of the same address in one expression with no sequence point that allocates between them. Under payloads this statement is the whole publish protocol in miniature: derive, write, discard. The nelisp lesson says the write surface is the expensive one; in fe it is one line. |
| A5 | `fe.c:933`+`fe.c:176` `BuildString` / `SetType` | `FeCons(ctx, NULL, &nil)` zero-fills the `car` word, then `SetType` writes only its tag byte -- so the 7 payload bytes are zeroed by the `NULL` car | no | The zero fill and the retype are one uninterrupted pair. Under payloads a new cell's block must be zero-filled by the allocator, since A4 finds its write offset with `strlen` and depends on the tail being NUL. |
| A6 | `fe.c:1183` `EmitSymbolName` | `i < StringBufferSize && STRING_BUFFER(name)[i]` -- the loop condition, evaluated after the previous iteration's `Emit` | **YES** | `Emit` (`fe.c:1108`) calls the host's `FeWriteFn`, and fe's contract for that callback does not forbid allocation (contrast `FeSetMarkFn`, `fe.h:528`, which forbids it in as many words). Safe today because the address is re-derived at every use. Under payloads: no caching across `Emit`, ever. |
| A7 | `fe.c:1184` `EmitSymbolName` | `const char chr = STRING_BUFFER(name)[i]` -- one byte copied out before it is used | **YES**, see A6 | The BYTE is copied to a local; the ADDRESS is not. Copying the byte out is the right shape and should stay. |
| A8 | `fe.c:1210` `EmitStoredString` | loop condition, as A6 | **YES**, see A6 | As A6. |
| A9 | `fe.c:1212` `EmitStoredString` | `STRING_BUFFER(obj)[i] == '"' \|\| STRING_BUFFER(obj)[i] == '\\'` -- the quoting test | **YES**, see A6 | Both derivations precede the `Emit` they guard. |
| A10 | `fe.c:1215` `EmitStoredString` | `Emit(w, STRING_BUFFER(obj)[i])` -- the byte is read, then the callback runs | **YES**, see A6 | The argument is evaluated before the call, so the read happens before the allocation the callback might do. Under payloads this ordering must stay explicit, because the obvious "optimisation" (hoisting the base) breaks it silently. |
| A11 | `fe.c:1563` `CopyStoredStringBytes` | `const char* buffer = STRING_BUFFER(string);` -- the census's ONE cached payload pointer, held across `memchr` and `memcpy` | no | Neither `memchr` nor `memcpy` allocates, and the cache dies at the end of the loop iteration. This is the one site where a reviewer must check the rule rather than see it in the syntax, and therefore the one to state the invariant beside under payloads. |
| A12 | `fe.c:2313` `IsKeywordSymbol` | `STRING_BUFFER(SymbolName(v))[0] == ':'` | no | One derivation, one read, no allocation. |
| A13 | `fe.c:708` `Equal` | `CAR(a) != CAR(b)` -- string equality compares the whole `car` WORD, i.e. the tag byte and all 7 stored bytes at once | no | The loop allocates nothing. Not a `STRING_BUFFER` use, and therefore the row a `STRING_BUFFER` grep misses: under payloads `car` no longer holds the text and this arm must be rewritten, not merely re-derived. |

## B. fe core: the payload readers that reach A1-A13 through a helper

Exhaustive for fe.c, fe_eval.c, fe_run.c and fe_unwind.c.  None of these
derives a payload address itself; each one's safety is the helper's, and
each one is listed because Phase 25 changes the helper under it.

| # | site | reaches payload via | holds across alloc? | the rule that makes it safe |
| --- | --- | --- | --- | --- |
| B1 | `fe.c:1558` `CopyStoredStringBytes` | A11 | no | The chain walk holds only `FeObject *` (`string = CDR(string)`); `dst` is the caller's buffer. The one function every byte-copy in fe and kg funnels through. |
| B2 | `fe.c:1579` `FeStringByteLength`, `fe.c:1583` `FeCopyStringBytes` | B1 | no | `FeCopyStringBytes` calls B1 twice (`fe.c:1588`, `fe.c:1592`) with nothing between; the object it re-walks is a header, so the second walk is valid whatever moved. This pair is the public API's whole payload surface -- fe hands a host no interior pointer at all. |
| B3 | `fe.c:1166`, `fe.c:1177` `EmitSymbolName` | B1 into a 64-byte stack buffer | no | The number-lookalike test runs on the COPY, not on the chain. Correct shape; keep it. |
| B4 | `fe.c:2839`, `fe.c:2843` `CopyNameArgument` | B1 into the caller's `char[SymbolNameLimit + 1]` | no | Length pass, bound check, copy pass; the `FeHandleError` between them raises rather than allocates. |
| B5 | `fe.c:2862`, `fe.c:2866` `InternSoft` | B1 twice, each re-deriving `SymbolName(argument)` from the header | no | The re-derivation per pass is the pattern to keep: the header is the durable name of the chain. |
| B6 | `fe.c:2904`, `fe.c:2908` `SymbolNameString` | B1 twice off a cached `const FeObject* const stored` | no | `stored` is a HEADER, which Design B does not move; caching it is safe and caching a `STRING_BUFFER` off it would not be. |
| B7 | `fe.c:3142` `RenderErrorMessage` | B1 for the "is the message empty" test | no | Renders into the caller's buffer through `AppendMessageObject`/`RenderObject`, which `fe.c:3095` documents as allocating nothing -- it runs on the host error path where there may be no arena left. |
| B8 | `fe.c:841` `FindInternedSymbol` | A2 per candidate | no | The obarray scan allocates nothing, so no candidate's name can move mid-scan. `FeMakeSymbol` (`fe.c:986`) calls it BEFORE the allocations that mint a new symbol, never between them. This is the plan's "`strcmp` in interning" row: fe spells it `IsStringEqual`, byte-at-a-time over the chain, because a chain is not a C string. |
| B9 | `fe.c:2299` `IsNamedSymbol` | A2 | no | Also `fe.c:1358`, `fe.c:1360`, `fe.c:2317`, `fe.c:2321`, `fe.c:3108`, `fe.c:3120`. |
| B10 | `fe.c:2311` `IsKeywordSymbol`, `fe.c:1631`/`fe.c:2316` `IsConstantSymbol` | A12, B9 | no | |
| B11 | `fe.c:1421` `WriteObject` (symbol arm), `fe.c:1425` (string arm) | A6-A10 | **YES**, inherited | The printer's payload window is exactly A6-A10. Everything below it -- `FeWrite`, `FeWriteWithOptions`, `FeWriteFile`, `RenderObject` (`fe.c:1506`), `FeToString` (`fe.c:1529`) -- inherits that window and adds nothing of its own. |
| B12 | `fe.c:950` `FeMakeString` | A3-A5 through `BuildString` | no pointer held | Holds `obj` and `tail` as `FeObject *` across every `BuildString` call, which is why the constructor is safe even though A3/A4 straddle an allocation. The model for a Phase-25 constructor. |
| B13 | `fe.c:2363` `ReadStringLiteral` | A3-A5 through `BuildString` | no pointer held | Holds `res` and `value` as `FeObject *` across the host `FeReadFn` (`fn(ctx, udata)`), which may allocate. Same shape as B12. |
| B14 | `fe.c:977` `MakeSymbolObject` | B12 for the name | no pointer held | Four allocations with every intermediate on the GC stack; nothing derives a name address at all. |
| B15 | `fe.c:1544` `GetStringObject` | -- | no | Returns a header, or raises. Named here because it is the type gate every public payload reader passes through. |
| B16 | `fe.c:1689` `SymbolName` | -- | no | Returns the name CHAIN's header. The one function whose result looks like a payload handle and is not. |
| B17 | `fe.c:432` `FeMark`, string/symbol arm | -- | -- | The collector walks `CDR` and never touches stored bytes. Under payloads this arm gains the payload edge, per the ADR's "a new arm of the existing Deutsch-Schorr-Waite trampoline". Censused because it is where a mark/rewrite asymmetry would live -- nelisp's recurring bug, its Phase-3 learning 4. |
| B18 | `fe.c:545` `CollectGarbage`, sweep loop | -- | -- | Walks `ctx->objects[i]` linearly. Under payloads the compactor runs between the mark and this sweep, per the ADR's ordering argument. |
| B19 | `fe.c:872` `MakeObject` | -- | -- | THE allocation point: every constructor reaches it, so it is where the poison step hooks and where "any allocation" is defined for this census. |
| B20 | `fe.c:3204` `GetSymbolObjectCount`, `fe.c:3216` `GetStringObjectCount` | -- | no | Arithmetic on `StringBufferSize` alone; no object is dereferenced. Payloads change the ARITHMETIC (a string stops costing one cell per 7 bytes) and `FeMinimumArenaSize` with it. |
| B21 | `fe_eval.c:2074`, `fe_eval.c:2077` `ResumeBinary` | A1 | no | `string<` / `string>`. |
| B22 | `fe_eval.c:2042` `ResumeBinary` | A13 | no | `equal`. |
| B23 | `fe_eval.c:1911` `ResumeUnary` | A12 | no | `keywordp`. |
| B24 | `fe_eval.c:2847`, `fe_eval.c:2852`, `fe_eval.c:2883`, `fe_eval.c:2887` `FormatErrorMessage` | B2 into stack buffers | no | Length pass then copy pass, twice; nothing allocates between a pass pair. |
| B25 | `fe_eval.c:2997`, `fe_eval.c:3001` `ResumeEvalList` | B2 over `SymbolName(name)` | no | As B24. |
| B26 | `fe_eval.c:511`, `fe_eval.c:2806`, `fe_eval.c:2807` | B11 through `FeToString` into stack buffers | no | Rendering into a caller buffer; the payload window stays inside the printer. |
| B27 | `fe_eval.c:2890`, `fe_eval.c:2894` | B11 through `RenderObject` | no | As B26. |
| B28 | `fe_eval.c:1925` | B7 through `RenderErrorMessage` | no | As B26. |
| B29 | `fe_eval.c` `IsNamedSymbol` -- lines 60, 84, 161, 369, 380, 384, 424, 428, 1079, 1080, 3013 | B9 | no | Parameter-list keywords (`&optional`, `&rest`), `lambda`/`fn` heads, `t`, `quit`. Phase 21 recorded this as a LOOKUP cost no representation change fixes; it is also eleven payload reads per shape that a payload move must keep correct. |
| B30 | `fe_eval.c` `IsConstantSymbol` -- lines 48, 60, 83, 95, 106, 113, 1939 | B10 | no | |
| B31 | `fe_unwind.c` `IsNamedSymbol` -- lines 179, 189, 224, 236, 243 | B9 | no | The condition hierarchy's name comparisons and `condition-case`'s spec test. fe_unwind.c has no other payload contact of any kind. |
| B32 | `fe_run.c` | -- | -- | NO payload contact at all: `fe_run.c` touches no string bytes, directly or through a helper. Recorded so the sweep is exhaustive over all four core translation units. |

## C. fe harnesses

The plan asks for "the harnesses that touch object internals".  Only two of
them include `fe_internal.h`, and neither reads a payload.

| # | site | what it touches | verdict |
| --- | --- | --- | --- |
| C1 | `test_api.c:8623`-`8640` (`CAR`/`CDR` writes building self-referential and ring structures), `test_api.c:3092`-`3095` (a cdr-chain walk and append) | pair LINK words | Header words, not payload. Design B does not move them. No change owed. |
| C2 | `test_api.c` everywhere else | the public API | `FeStringByteLength`/`FeCopyStringBytes`/`FeToString` only, which is B2/B11's copy-out contract. No interior pointer crosses the API. |
| C3 | `perf_workloads.c:62` includes `fe_internal.h` | `FE_API_VERSION`/`FE_LANGUAGE_VERSION` static asserts and the arena-size enums | Reads no object field and no `ctx->` field. No payload contact. |
| C4 | `gc_stress.c`, `example_host.c`, `main.c`, `fex.c`, `fex_io.c`, `fex_math.c`, `fex_process.c`, `fex_re.c`, `fex_time.c`, `fuzz/*.c` | the public API only | None includes `fe_internal.h`; the only `car`/`cdr` text in them is Lisp source inside string literals. No payload contact. |

## D. kg's `src/lisp_*.c`

kg reaches fe only through `fe.h`, and `fe.h` returns no pointer into object
storage: `FeStringByteLength` + `FeCopyStringBytes` (`fe.h:821`-`822`) and
`FeToString` (`fe.h:800`) copy bytes into a buffer the CALLER owns.  So kg
holds no payload pointer today and cannot begin to without a new public
symbol.  That is the finding, and the rows below are its proof rather than a
list of hazards.

The pattern kg does hold across allocation is `FeObject *` -- a header, which
Design B pins.

| # | site | pattern | holds a payload pointer? | the rule that makes it safe |
| --- | --- | --- | --- | --- |
| D1 | `src/lisp_core.c:251` `copy_fe_string` | `FeStringByteLength` -> `malloc` -> `FeCopyStringBytes` -> NUL-terminate | no | The plan's `copy_fe_string()` row. What survives the `malloc` is `FeObject *object`, a header. The `malloc` is HOST memory and no fe allocation happens between the length pass and the copy pass, so even the length cannot go stale. Twenty call sites inherit this: `lisp_cmd.c:320`, `lisp_cmd.c:994`, `lisp_core.c:296`, `lisp_core.c:1246`, `lisp_hooks.c:449`, `lisp_hooks.c:487`, `lisp_hooks.c:527`, `lisp_io.c:519`, `lisp_io.c:557`, `lisp_io.c:642`, `lisp_io.c:959`, `lisp_io.c:1063`, `lisp_motion.c:317`, `lisp_obj.c:629`, `lisp_process.c:412`, `lisp_require.c:41`, `lisp_require.c:150`, `lisp_search.c:261`, `lisp_search.c:628`, `lisp_string.c:29`. |
| D2 | `src/lisp_search.c:439`-`453` `lisp_pattern_and_subject` | two `FeStringByteLength`, one `malloc`, two `FeCopyStringBytes` into halves of one block | no | Both objects are held as headers across the `malloc`; the block is host memory parked in `state.scratch` so a raise between here and `release_scratch()` still frees it. |
| D3 | `src/lisp_search.c:572`-`585` `native_regexp_quote` | length, `malloc`, copy, then a byte loop over the HOST copy | no | The quoting loop reads `block[i]`, never fe storage. |
| D4 | `src/lisp_string.c:153` `lisp_concat_bytes` and `src/lisp_string.c:179`-`181` `native_concat` | a length pass over the argument list, then a `malloc`, then a second pass re-reading each length and copying | no | Both passes walk `FeObject *` argument chains. The second pass re-asks `FeStringByteLength` per element rather than caching the first pass's per-element numbers -- the right shape, and the one to keep when strings move. |
| D5 | `src/lisp_string.c:204`-`219` `native_string_equal` | two lengths, one `malloc` for both copies, `memcmp` on the copies | no | The comparison is between two HOST copies, so it is unaffected by anything fe does to storage. |
| D6 | `src/lisp_process.c:338`-`344` `start-process` argv build | per argument: length, bound check, copy into a fixed `LISP_PROCESS_ARGV_BYTES` block | no | `argv[argc]` points into the HOST block, not into fe. |
| D7 | `src/lisp_prompt.c:118`-`124` `copy_string_argument` | length, bound check, copy onto the caller's frame | no | |
| D8 | `src/lisp_cmd.c:286`, `src/lisp_core.c:650`, `src/lisp_word.c:223`, `src/lisp_prompt.c:331` | `FeToString` into a stack buffer, then `strcmp` on the buffer | no | The payload window is fe's printer (B11); kg sees only the copy. |
| D9 | `src/lisp_io.c:368`, `src/lisp_io.c:383` `format_object` | `FeWrite` with kg's own `FeWriteFn` (`format_write_text`, `src/lisp_io.c:307`, -> `format_put`, `src/lisp_io.c:49`) | no -- and it must stay that way | THE cross-repository row. fe's printer is inside A6-A10's payload window while it calls this callback. kg's callback grows a HOST buffer with `realloc` (`format_grow`, `src/lisp_io.c:31`) and never allocates an fe object, so fe's window is never crossed by an fe allocation on kg's path. It CAN raise (out of memory), which longjmps out of the printer -- a raise, not a move, and therefore not a stale-pointer hazard. A future kg write callback that allocated an fe object would break A6-A10 from outside fe, and nothing in `fe.h` forbids it; that gap is recorded in "Findings" below. |
| D10 | `src/lisp_core.c:195`-`214` `render_condition` | `FeToString` and `FeErrorMessageString` into host buffers, from inside kg's `FeErrorFn` | no | Runs on the error path. fe's own renderer allocates nothing there (B7). |
| D11 | `src/lisp_hooks.c:170`, `src/lisp_process.c:132` | `FeGetCompletionMessage` interpolated into a host `snprintf` | no | Returns a pointer into a fixed `FeContext` message buffer, not into object storage; the context does not move. |
| D12 | `src/lisp_obj.c:81`, `src/lisp_obj.c:147` | `FeToPtr` -> `struct kg_lisp_object *rec`, held across further fe calls | no | The one kg pattern that LOOKS like an interior pointer and is not: an `FeTPtr` object stores a HOST pointer, and `FeToPtr` hands that value back. kg's own generation/`wrapper` checks are what keep it honest, and payloads change none of it. |
| D13 | everything else in `src/lisp_*.c` | `FeCons`, `FeCar`, `FeCdr`, `FeMakeString`, `FeMakeSymbol`, `FeGetNextArgument`, roots, `FePushGC`/`FeRestoreGC` | no | Header traffic. |

## Findings

Three things this sweep found that the ADR did not state.

1. **`Equal`'s string arm is not a `STRING_BUFFER` site (A13).**  It compares
   whole `car` WORDS, so a grep for the accessor misses it, and it is the one
   payload reader that a re-derive-after-allocation rule does not fix: once
   the bytes are not in `car`, the arm has to be rewritten.  It is the row
   most likely to be missed in Phase 25 and the reason this census is a
   document rather than a grep.

2. **fe's `FeWriteFn` contract does not forbid allocation, and the printer
   holds a payload pointer across it (A6-A10, D9).**  `FeSetMarkFn`'s
   contract (`fe.h:528`) forbids allocation from a mark callback in as many
   words; the writer callback's has no such sentence, because until now
   there was nothing for it to break.  kg's one write callback happens not to
   allocate, so nothing is broken today.  Phase 25 must either state the
   restriction in `fe.h` or make the printer re-derive after every `Emit`;
   the second is what A6-A10 already do, so the cheap answer is to keep that
   property deliberately rather than accidentally.

3. **The census's whole write surface is one line (A4).**  nelisp's Phase 1.5
   had to decouple a parse pool and a per-op scratch layer because ~257 sites
   wrote 32-byte slots through raw interior pointers.  fe writes stored bytes
   in exactly one statement, inside one 16-line constructor, plus the zero
   fill in A5.  That is the measured reason Phase 25 is a small change and not
   nelisp's Phase 1.5.

## The publish protocol

Stated in `fe_internal.h` beside `STRING_BUFFER`, which is the accessor it
will replace, and enforced by the debug assertions and the poison mode
described there.  In one sentence: a payload pointer is obtained immediately
before use through the one accessor, is invalid after ANY allocation, and a
new or replacement block reaches its owning header only through the one
publish function, which roots the owner across its own allocation.

Rows A3, A4, A6, A7, A8, A9, A10 and B11 are the ones that pay it.  Every
other row is safe by holding a header instead.
