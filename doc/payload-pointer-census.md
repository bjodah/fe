# The interior-pointer census

Taken at Phase 23.0 of kg's Elisp data-model program, as the entry gate for
the payload substrate the Phase 22 ADR selected (Design B: stable
`FeObject *` headers over a bump-allocated, compactable payload region).  It
is the checklist the one publish protocol in `fe_internal.h` is enforced
against.  It was written while every row could still be settled by reading;
since Phase 24 gave the region a vector to hold and Phase 25 gave it every
string in the language, a row is settled by reading the rule AND by the
poison lane failing when the rule is broken.

The failure class is nelisp's, documented at
`/opt/nelisp/docs/design/147-box-layout-container-shrink.org`.  Its Phase 1.5
found that the expensive surface was not the ~302 *readers* -- those collapse
behind one accessor -- but the sites that hold a raw interior pointer across
an operation that moves the storage under it, and especially the ones that
WRITE through it.  Its summary states the lesson directly: "The hidden cost
was the interior-pointer WRITE surface, not the 302 accessors."  This census
is that inventory.

## What counts as a payload here

A payload is storage an object OWNS but does not contain.  It lives in the
region, it MOVES when the compactor runs, and the `FeObject *` header that
names it does not.  Two release types own one:

* a VECTOR's elements are its block's traced children (Phase 24);
* a STRING's bytes are its block's byte tail (Phase 25), and a symbol's name
  is a string, so every context that opens at all has blocks in its region.

A payload pointer is exactly an address derived from an object that names its
stored elements or its stored bytes.  A pointer to an object (`FeObject *`),
to a pair's `car`/`cdr` link, to a `FeContext` field, or to host memory is
NOT a payload pointer and is not censused as one -- Design B's whole premise
is that headers are stable.  A string's LENGTH is not a payload either: it
lives in the bytes of the header's own `car` word that the type tag does not
use, which is why `StringLength` needs no context and cannot go stale.

Nothing else in fe migrates in this program's current scope.

## How to read a row

Every row names a file, a function and the line the pattern is on, so a
reader can open it and see the pattern.  Line numbers are as of the commit
that last rewrote this file (Phase 25.1); the function name is the durable
half.

* **holds across alloc?** -- can an fe allocation (anything reaching
  `MakeObject`, `fe.c:1305`, or `PublishPayload`, `fe.c:1252`, directly or
  through a callback) happen between the moment this site derives a payload
  address and the moment it last reads or writes through it?  `no` means the
  address is derived and spent with no allocation in between.  `YES` is the
  row that needs the protocol.
* **the rule that makes it safe** -- what a reader must not break.

## A. fe core: the direct payload dereferences, string side

Every read or write of a string's stored bytes in fe.c, fe_eval.c, fe_run.c
and fe_unwind.c goes through `StringBytes` (A1), and these are its callers.
The write surface is four rows -- A4, A5, A6, A15 -- plus the allocator's
zero fill.

| # | site | pattern | holds across alloc? | the rule that makes it safe |
| --- | --- | --- | --- | --- |
| A1 | `fe.c:510` `StringBytes` | `PayloadBytes(ctx, PAYLOAD(string))` -- THE accessor, one derivation per call | no | The whole function is the derivation; the result lives in its caller's statement and dies there. Never returned to a host and never stored in a struct. Never null, because every string publishes a block -- the empty string's is a header with no bytes after it, which is why `PayloadBytes` accepts a one-past-the-end offset. |
| A2 | `fe.c:492` `StringLength`, `fe.c:501` `SetStringLength` | the bytes of the owner's own `car` word | -- | NOT a payload, and listed so that a reader stops looking for one. The header does not move, so a length read is a field read: no context, no walk, no staleness. It is also why the block can be capacity rather than length. |
| A3 | `fe.c:517` `StringCapacity` | `OwnedBlock(ctx, string)->bytes` -- the block HEADER, not its bytes | no | Reads the block's own size word, which the compactor moves with the block. Only the two constructors ask; every other caller wants `StringLength`. |
| A4 | `fe.c:1582` `FeMakeStringBytes` | `memcpy(StringBytes(ctx, obj), bytes, length)` -- a payload WRITE | no | Derived after the last allocation the constructor makes (the publish inside `MakeStringObject`) and spent in the same statement. `bytes` is host memory the caller owns; a zero length skips the `memcpy` entirely rather than passing a possibly-null source. |
| A5 | `fe.c:1610` `AppendStringByte` | `StringBytes(ctx, string)[length] = chr` -- the reader's per-byte WRITE | **YES** | The `PublishPayload` above it, on the iteration where the capacity runs out, is the allocation. The address is derived after it, in the statement that spends it. A hoisted base here is the deliberately plantable bug for a string, exactly as a hoisted destination in `AppendSequence` is for a vector. |
| A6 | `fe.c:1235` `CopyReplacedPayload` | `memcpy(PayloadBytes(handle), PayloadBytes(previous), ...)` -- TWO payload pointers in one statement, one of them a WRITE | no | Called from inside `PublishPayload` after its allocation and before the store that makes the old block dead, which is the only window in which both blocks exist and neither can move. It is what makes a replacement a copy of what it replaces -- the reason a growing string is the same string. |
| A7 | `fe.c:1848` `EmitSymbolName` | `(char)StringBytes(w->ctx, name)[i]` inside the emit loop | **YES** | `Emit` reaches the host's `FeWriteFn`, which fe.h now says MAY allocate (finding 2, decided). The address is derived per byte and the BYTE is copied to a local before the callback runs. Never hoist the base out of this loop. |
| A8 | `fe.c:1892` `EmitStoredString` | as A7, plus the quote/backslash/NUL tests on the copied byte | **YES**, see A7 | The byte is read into `chr` and every test is on `chr`, so the payload window is one statement wide however many branches follow it. |
| A9 | `fe.c:1798` `CopyStringBytes` | `memcpy(dst, StringBytes(ctx, string), length)` -- the one copy OUT | no | `dst` is the caller's buffer, always host memory. Every byte copy in fe and behind `fe.h` funnels through here, which is what keeps the number of derivations countable. A length-only call (`dst` null) derives no address at all. |
| A10 | `fe.c:1180` `IsStringEqual` | `memcmp(StringBytes(ctx, obj), str, length)` | no | `str` is a host C string. The length test above it decides most calls without deriving an address, which is what made the obarray scan cheaper as well as safer. |
| A11 | `fe.c:1056` `Equal` | `memcmp(StringBytes(ctx, a), StringBytes(ctx, b), length)` -- TWO payload pointers in one expression | no | Both are derived inside the expression that spends them, and nothing in `Equal` allocates. This is the row Phase 23.0's finding 1 predicted would have to be REWRITTEN rather than re-derived: it used to compare whole `car` words. |
| A12 | `fe.c:1129` `StringOperandLess` | `memcmp(StringBytes(ctx, left), StringBytes(ctx, right), shared)` -- two more | no | Both coercions that can allocate (`StringOperand`, which may build the string `"nil"`) happen above, and `left` is rooted across the second one. Both addresses are derived after the last allocation. |
| A13 | `fe.c:3055` `IsKeywordSymbol` | `StringBytes(ctx, name)[0] == ':'` | no | One derivation, one read, no allocation, guarded by a nonzero length. |
| A14 | `fe.c:3860` `StringByteAt` | `StringBytes(ctx, string)[index]` | no | The caller has bounds-checked against `StringLength`. Since Phase 25 this is the same arithmetic a vector's element already was, rather than a walk. |
| A15 | `fe.c:3935` `SetStringByte` | `StringBytes(ctx, string)[index] = value` -- `aset`'s WRITE | no | Both refusals (`characterp`, and the above-a-byte one) raise before the address is derived, so the derivation and the store are one statement with nothing between them. |

## B. fe core: the payload readers that reach A1-A15 through a helper

Exhaustive for fe.c, fe_eval.c, fe_run.c and fe_unwind.c.  None of these
derives a payload address itself; each one's safety is the helper's.

| # | site | reaches payload via | holds across alloc? | the rule that makes it safe |
| --- | --- | --- | --- | --- |
| B1 | `fe.c:2300` `FeStringByteLength`, `fe.c:2304` `FeCopyStringBytes`, `fe.c:2317` `FeStringBytes` | A9 | no | The public API's whole string surface. `FeCopyStringBytes` calls A9 twice with nothing between; the object it re-reads is a header, so the second call is valid whatever moved. fe hands a host no interior pointer at all, and there is no plan to. |
| B2 | `fe.c:2270` `GetStringObject` | -- | no | Returns a header, or raises. The type gate every public reader passes through; a symbol answers its name string. |
| B3 | `fe.c:2419` `SymbolName` | -- | no | Returns the name STRING's header. The one function whose result looks like a payload handle and is not. |
| B4 | `fe.c:1848` `EmitSymbolName` number-lookalike test | A9 into a 64-byte stack buffer | no | The test runs on the COPY, not on the payload. Correct shape; keep it. |
| B5 | `fe.c:3646` `CopyNameArgument`, `fe.c:3668` `InternSoft`, `fe.c:3706` `SymbolNameString` | A9 into a `char[SymbolNameLimit + 1]` | no | Length pass, bound check, copy pass. `SymbolNameString` then builds a NEW string from the stack copy rather than from the payload, which is what keeps a payload address from crossing the allocation that makes the result. |
| B6 | `fe.c:1208` `FindInternedSymbol` | A10 per candidate | no | The obarray scan allocates nothing, so no candidate's name can move mid-scan, and each candidate's address is derived inside `IsStringEqual`. `FeMakeSymbol` calls it BEFORE the allocations that mint a new symbol, never between them. Phase 25's version rejects most candidates on their LENGTH, which touches no payload at all. |
| B7 | `fe.c:3043` `IsNamedSymbol` | A10 | no | Also `fe.c:3064` `IsConstantSymbol`, `fe.c:3068` `IsDot`, `fe.c:4185`/`fe.c:4197` `SelectErrorMessage`, `fe.c:2076`/`fe.c:2078` `EmitAbbreviation`. |
| B8 | `fe.c:4202` `RenderErrorMessage` | A9 for the "is the message empty" test | no | Renders into the caller's buffer through `AppendMessageObject`/`RenderObject`, which allocate nothing -- they run on the host error path where there may be no arena left. |
| B9 | `fe.c:746` `FeMark`, payload arm | -- | -- | A string's block has no traced child, so `DescendIntoPayload` finds it a leaf; a vector's children are walked by the same pointer reversal. This is where a mark/rewrite asymmetry would live -- nelisp's recurring bug, its Phase-3 learning 4. |
| B10 | `fe.c:902` `CollectGarbage`, `fe.c:586` `CompactPayloads` | -- | -- | The compactor runs after the mark phase has restored the graph and BEFORE the sweep clears the mark bits, which is the ordering the liveness rule depends on. Its return-to-base move pattern-fills what it vacates under the poison knob; without that, a stale pointer into the TAIL of the old extent read valid data, because a backward `memmove` does not overwrite its own tail (found by the poison lane in Phase 25). |
| B11 | `fe.c:1305` `MakeObject`, `fe.c:1252` `PublishPayload` | -- | -- | THE two allocation points: every constructor reaches one, so they are where the poison step hooks and where "any allocation" is defined for this census. |
| B12 | `fe.c:1565` `MakeStringObject`, `fe.c:1652` `MakeSymbolObject` | A4 | no pointer held | Retype, zero the length, clear the handle, publish -- the order `MakeVector` and `MakeAggregate` established, so that a collection landing between the retype and the publish finds an owner that coherently owns nothing. `MakeSymbolObject` holds only `FeObject *` across its four allocations. |
| B13 | `fe.c:3119` `ReadStringLiteral` | A5 through `AppendStringByte`, A3 through `FinishString` | no pointer held | Holds `res` as an `FeObject *` across the host `FeReadFn` and across its own growth, both of which allocate. The string it hands back is the object it started with. |
| B14 | `fe.c:4293` `GetStringPayloadBytes`, `fe.c:4547` `GetCorePayloadBytes` | -- | no | Arithmetic on `sizeof(FePayloadBlock)` and a name's length; no object is dereferenced. They must agree with `AllocatePayloadBlock` exactly, because `FeMinimumArenaSize` is exact and funds the core names' blocks. |
| B15 | `fe_eval.c:2061` `ResumeBinary` | A11 | no | `equal`. |
| B16 | `fe_eval.c:2093`, `fe_eval.c:2096` `ResumeBinary` | A12 | no | `string<` / `string>`. |
| B17 | `fe_eval.c:1930` `ResumeUnary` | A13 | no | `keywordp`. |
| B18 | `fe_eval.c:2878`/`fe_eval.c:2883` and `fe_eval.c:2914`/`fe_eval.c:2918` `FormatErrorMessage`, `fe_eval.c:3034`/`fe_eval.c:3038` `ResumeEvalList` | B1 into stack buffers | no | Length pass then copy pass; nothing allocates between a pass pair. |
| B19 | `fe_eval.c:513`, `fe_eval.c:1944`, `fe_eval.c:2837`, `fe_eval.c:2921` | the printer (A7-A8) through `FeToString`/`RenderObject`/`RenderErrorMessage` | no | Rendering into a caller buffer; the payload window stays inside the printer. |
| B20 | `fe_eval.c` `IsNamedSymbol` -- lines 61, 85, 162, 163, 371, 382, 386, 426, 430, 1098, 1099, 3049 | B7 | no | Parameter-list keywords (`&optional`, `&rest`), `lambda`/`fn` heads, `t`, `quit`. Phase 21 recorded this as a LOOKUP cost no representation change fixes; it is also twelve payload reads per shape that a payload move must keep correct. |
| B21 | `fe_eval.c` `IsConstantSymbol` -- lines 48, 60, 84, 96, 107, 114, 1958 -- and `IsConditionSymbol` at `fe_eval.c:3040` | B7, B22 | no | |
| B22 | `fe_unwind.c` `IsNamedSymbol` -- lines 195, 206, 244, 256, 263 | B7 | no | The condition hierarchy's name comparisons and `condition-case`'s spec test. fe_unwind.c has no other payload contact of any kind. |
| B23 | `fe_run.c` | -- | -- | NO payload contact at all. Recorded so the sweep is exhaustive over all four core translation units. |

Every function in B7, B15-B22 takes a `const FeContext *` for exactly one
reason: reading a name's bytes needs the context that owns the region.  That
parameter is the census made structural -- a helper that reads a payload
cannot be called from somewhere that has no context to read it from.

## C. fe harnesses

| # | site | what it touches | verdict |
| --- | --- | --- | --- |
| C1 | `test_api.c` `CAR`/`CDR` writes building self-referential and ring structures, and its cdr-chain walks | pair LINK words | Header words, not payload. Design B does not move them. No change owed. |
| C2 | `test_api.c` everywhere else, `TestStringRepresentation` and `TestStringMutation` included | the public API | `FeMakeStringBytes`/`FeStringBytes`/`FeStringByteLength`/`FeCopyStringBytes`/`FeToString` only, which is B1's copy-out contract. No interior pointer crosses the API. |
| C3 | `payload_tests.c` string and vector groups | `PAYLOAD(obj)` (a HANDLE), `AggregateBytes`, and the public string API | A handle is not an address: the string cases assert that a handle CHANGED across a collection and then read the bytes back through B1, which is the only shape a test of a moving payload can have. `AggregateBytes` is derived and spent inside the statement, per clause 1. |
| C4 | `perf_workloads.c` | `sizeof(FePayloadBlock)`, `FePayloadAlignment`, the version asserts | Reads no object field and no `ctx->` field. No payload contact. |
| C5 | `gc_stress.c`, `example_host.c`, `main.c`, `fex.c`, `fex_*.c`, `fuzz/*.c` | the public API only | None includes `fe_internal.h`. No payload contact. |

## D. kg's `src/lisp_*.c`

kg reaches fe only through `fe.h`, and `fe.h` returns no pointer into object
storage: `FeStringByteLength` + `FeCopyStringBytes`, `FeStringBytes` and
`FeToString` all copy bytes into a buffer the CALLER owns.  So kg holds no
payload pointer and cannot begin to without a new public symbol.  That was
the finding at Phase 23.0 and it is why Phase 25 needed NO change to kg's C
boundary: the copy-out contract was already binary-safe, and the two-call
pair still means exactly what it did.

The pattern kg does hold across allocation is `FeObject *` -- a header, which
Design B pins.

| # | site | pattern | holds a payload pointer? | the rule that makes it safe |
| --- | --- | --- | --- | --- |
| D1 | `src/lisp_core.c:251` `copy_fe_string` | `FeStringByteLength` -> `malloc` -> `FeCopyStringBytes` -> NUL-terminate | no | What survives the `malloc` is `FeObject *object`, a header. The `malloc` is HOST memory and no fe allocation happens between the length pass and the copy pass, so even the length cannot go stale. Twenty call sites inherit this: `lisp_cmd.c:320`, `lisp_cmd.c:994`, `lisp_core.c:296`, `lisp_core.c:1246`, `lisp_hooks.c:449`, `lisp_hooks.c:487`, `lisp_hooks.c:527`, `lisp_io.c:519`, `lisp_io.c:557`, `lisp_io.c:642`, `lisp_io.c:959`, `lisp_io.c:1063`, `lisp_motion.c:317`, `lisp_obj.c:629`, `lisp_process.c:412`, `lisp_require.c:41`, `lisp_require.c:150`, `lisp_search.c:261`, `lisp_search.c:628`, `lisp_string.c:29`. The one thing that changed under them is that the bytes they copy may now contain NUL, which is a kg-side semantic question and not a lifetime one. |
| D2 | `src/lisp_search.c:439`-`453` `lisp_pattern_and_subject` | two `FeStringByteLength`, one `malloc`, two `FeCopyStringBytes` into halves of one block | no | Both objects are held as headers across the `malloc`; the block is host memory parked in `state.scratch` so a raise between here and `release_scratch()` still frees it. |
| D3 | `src/lisp_search.c:572`-`585` `native_regexp_quote` | length, `malloc`, copy, then a byte loop over the HOST copy | no | The quoting loop reads `block[i]`, never fe storage. |
| D4 | `src/lisp_string.c:153` `lisp_concat_bytes`, `src/lisp_string.c:179`-`181` `native_concat` | a length pass over the argument list, then a `malloc`, then a second pass re-reading each length and copying | no | Both passes walk `FeObject *` argument chains. The second pass re-asks `FeStringByteLength` per element rather than caching the first pass's numbers -- the right shape, and the one to keep. |
| D5 | `src/lisp_string.c:204`-`219` `native_string_equal` | two lengths, one `malloc` for both copies, `memcmp` on the copies | no | The comparison is between two HOST copies, so it is unaffected by anything fe does to storage. |
| D6 | `src/lisp_process.c:338`-`344` `start-process` argv build | per argument: length, bound check, copy into a fixed block | no | `argv[argc]` points into the HOST block, not into fe. |
| D7 | `src/lisp_prompt.c:118`-`124` `copy_string_argument` | length, bound check, copy onto the caller's frame | no | A candidate for `FeStringBytes`, which is that shape in one call. |
| D8 | `src/lisp_cmd.c:286`, `src/lisp_core.c:650`, `src/lisp_word.c:223`, `src/lisp_prompt.c:331` | `FeToString` into a stack buffer, then `strcmp` on the buffer | no | The payload window is fe's printer (A7-A8); kg sees only the copy. |
| D9 | `src/lisp_io.c:368`, `src/lisp_io.c:383` `format_object` | `FeWrite` with kg's own `FeWriteFn` (`format_write_text` -> `format_put`) | no -- and it is now GUARANTEED not to matter | THE cross-repository row, and finding 2's. kg's callback grows a HOST buffer with `realloc` and can raise out of memory. fe.h now states that a write callback MAY allocate and MAY raise, and the printer re-derives after every callback so that it can; the gap this row recorded is closed by a promise fe keeps rather than by a restriction kg has to. |
| D10 | `src/lisp_core.c:195`-`214` `render_condition` | `FeToString` and `FeErrorMessageString` into host buffers, from inside kg's `FeErrorFn` | no | Runs on the error path. fe's own renderer allocates nothing there (B8). |
| D11 | `src/lisp_hooks.c:170`, `src/lisp_process.c:132` | `FeGetCompletionMessage` interpolated into a host `snprintf` | no | Returns a pointer into a fixed `FeContext` message buffer, not into object storage; the context does not move. |
| D12 | `src/lisp_obj.c:81`, `src/lisp_obj.c:147` | `FeToPtr` -> `struct kg_lisp_object *rec`, held across further fe calls | no | The one kg pattern that LOOKS like an interior pointer and is not: an `FeTPtr` object stores a HOST pointer and `FeToPtr` hands that value back. |
| D13 | everything else in `src/lisp_*.c` | `FeCons`, `FeCar`, `FeCdr`, `FeMakeString`, `FeMakeSymbol`, `FeGetNextArgument`, roots, `FePushGC`/`FeRestoreGC` | no | Header traffic. |

## E. fe core: the vector's payload sites (Phase 24)

A vector's elements ARE its payload block's children.  Every one of these
derives the block address inside the statement that spends it, which is
clause 1 read as a rule rather than as an accident.

| # | site | pattern | holds across alloc? | the rule that makes it safe |
| --- | --- | --- | --- | --- |
| E1 | `fe.c:1419` `VectorElement` | `*PayloadChildSlot(OwnedBlock(ctx, vector), index)` -- one derivation, one read | no | The whole function is the derivation and the read. Every element READ in fe and in `fe.h` goes through it, which is what makes this row the one to keep true rather than twenty. |
| E2 | `fe.c:1424` `SetVectorElement` | the same address, written | no | Every element WRITE goes through it -- `aset`, `vconcat`'s fill, `make-vector`'s fill, the reader's fill, `FeVectorSet`. |
| E3 | `fe.c:1414` `VectorLength` | `OwnedBlock(ctx, vector)->children` -- the BLOCK HEADER, not its payload | no | Null when the owner has published no block yet, which a constructor between its retype and its publish really is; answering 0 there is why a collection landing in that window finds a coherent object. |
| E4 | `fe.c:1441` `MakeVector` fill loop | E2 per slot | no | Nothing in the loop allocates: `init` is one already-built object stored `length` times, which is `make-vector`'s Emacs contract. `init` is rooted across `PublishPayload`, which allocates. |
| E5 | `fe.c:1460` `MakeVectorFromList` fill loop | E2 per slot | no | Nothing in the loop allocates; the source list is rooted across `MakeVector`, which does. |
| E6 | `fe.c:4008` `AppendSequence` | E1 and E2 per element, A14 for a string operand | **YES** | `FeMakeInteger` runs between two writes for a STRING operand, so the destination address is re-derived per element -- a hoisted one is the deliberately planted bug the poison lane was proved to catch. The GC-stack checkpoint inside the loop is the other half: without it a string operand pushes one root per byte and overflows at 4032. |
| E7 | `fe.c:2020` `WriteVectorElements` | E1 per element, `VectorLength` once | **YES** | `WriteObject` reaches the host's `FeWriteFn`, which fe.h now says may allocate, so this loop is inside the same window A7-A8 are and re-derives for the same reason. The LENGTH is read once because a vector cannot change length; the ADDRESSES are read per element because it can move. |
| E8 | `fe.c:3903` `Aref`, `fe.c:3951` `Aset`, `fe.c:3978` `SequenceElement`, `fe.c:3877` `SequenceCount`, `fe.c:4035` `Vconcat` | E1-E3, A14, A15 | no | The Lisp surface, now generic over both owning types. Each validates, then spends one derivation; `Vconcat` holds only `FeObject *` across its two passes. |
| E9 | `fe.c:3223` `ReadVector` | -- | no | Holds `list` and the finished `vector` as `FeObject *` across `FeCons` and `MakeVectorFromList`. Derives no payload address of its own. |
| E10 | `fe.c:1441` `MakeVector` | -- | -- | The constructor row clause 3 is about: retype the cell, clear its handle, THEN `PublishPayload`, which roots the owner across the collection it may run. `MakeStringObject` copies the order. |
| E11 | `fe.h` `FeMakeVector`, `FeVectorLength`, `FeVectorRef`, `FeVectorSet` | E1-E3 behind the type and bounds checks | no | The public surface hands a host no interior pointer at all, which is B1's property extended to a second type. There is deliberately no borrowed-elements accessor; `doc/c-api.md` says why. |
| E12 | `fe.c:746` `FeMark` payload arm, `fe.c:671` `DescendIntoPayload` | -- | -- | B9's row, reached by both release types. A vector's WIDTH costs the mark phase no C stack, and a string is a leaf there. |
| E13 | `fe.c:586` `CompactPayloads` | -- | -- | B10's row. |

## Findings

The three things the Phase 23.0 sweep found that the ADR did not state, and
where each of them stands.

1. **`Equal`'s string arm was not an accessor site -- CLOSED in Phase 25.**
   It compared whole `car` WORDS, so a grep for the accessor missed it, and it
   was the one payload reader a re-derive-after-allocation rule would not have
   fixed.  It is A11 now: length, then bytes, through `StringBytes`.  This
   file existing as a document rather than a grep is what caught it.

2. **`FeWriteFn`'s allocation contract -- DECIDED in Phase 25, in favour of
   allowing it.**  The choice was between forbidding allocation from a write
   callback (`FeSetMarkFn`'s shape) and keeping the printer's re-derivation
   deliberate.  Re-derivation won on two grounds.  The printer already pays an
   indirect call per emitted byte, so three loads to re-derive a block address
   is noise against a cost it already has; and the restriction would have been
   a new promise every host has to keep, where this is a promise fe keeps.
   `fe.h` says so at the `FeWriteFn` typedef, `doc/c-api.md` says so under
   "Serializing Objects", and A7, A8 and E7 are the three loops that pay it.
   D9 -- kg's one write callback, which grows a host buffer and can raise --
   is the row that motivated the question and is now safe by contract rather
   than by coincidence.

3. **The write surface is still small.**  nelisp's Phase 1.5 had to decouple a
   parse pool and a per-op scratch layer because ~257 sites wrote 32-byte
   slots through raw interior pointers.  fe writes stored bytes in four
   statements (A4, A5, A6, A15) and stores elements in one (E2), plus the
   allocator's zero fill.  That is the measured reason Phase 25 was a small
   change and not nelisp's Phase 1.5.

## The publish protocol

Stated in `fe_internal.h` beside the accessors it governs, and enforced by the
debug assertions and the poison mode described there.  In one sentence: a
payload pointer is obtained immediately before use through the one accessor,
is invalid after ANY allocation, and a new or replacement block reaches its
owning header only through the one publish function, which roots the owner
across its own allocation and copies the old block's contents into the new
one.

Rows A5, A7, A8, E6 and E7 are the ones that pay clause 2.  Every other row
is safe by holding a header, a handle, or a copied byte instead.

## Status after Phase 25

The substrate landed in Phase 23.1, got its first consumer in Phase 24 and its
universal one in Phase 25: a string's bytes are payload, a symbol's name is a
string, and there is therefore no such thing as an fe context whose region is
empty.  `FeOpenContext` carves one and `FeMinimumArenaSize` funds the floor,
which is what retired `FePayloadPercentNone`.

Three rows have now been settled by DEBUGGING rather than by reading, which is
what this file's introduction predicted would start happening.

* **E6**, in Phase 24: a hoisted destination address there passes an ordinary
  `make check` and fails `.ci/ci-04-clang-asan-ubsan.sh` with the poison knob
  armed, measured by planting exactly that bug and watching the two runs
  disagree (`make check` exit 0, the poison lane exit 2 on `vconcat`'s own
  case).
* **E7**, in Phase 24: the first site inside finding 2's window on purpose
  rather than by inheritance.
* **B10**, in Phase 25: `CompactPayloads`' return-to-base move is BACKWARD,
  and a backward `memmove` does not overwrite its own tail, so a stale pointer
  into the tail of the old extent read valid data.  The poison lane found it
  the first time a context opened with blocks already in its region -- which
  is to say, the first time strings owned payloads.  The vacated bytes are
  pattern-filled now.

What is left for a later phase is section D's own migration, which Phase 25
found to be nothing: kg's boundary copies through an API that never handed out
an interior pointer, so the only kg-side question this phase raises is
semantic (its copies may now contain NUL) rather than about lifetimes.
