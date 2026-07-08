## Plan: Add `re-match` / `re-match-p` to kg's Lisp layer

### 1. Build system (Makefile)

**Add `re.o` compilation and linking**, guarded by `WITH_LISP=1`:

```makefile
# Next to FE_OBJ definition (line 46):
RE_OBJ = $(OBJDIR)/re.o

# Next to fe.o rule (line 149):
$(OBJDIR)/re.o: fe/tiny-regex-c/re.c fe/tiny-regex-c/re.h
	$(CC) $(FE_CFLAGS) -I fe/tiny-regex-c -c $< -o $@

# Modify TARGET link line (line 140):
$(TARGET): $(OBJS) $(FE_OBJ) $(RE_OBJ)

# Modify clean (line 279):
rm -f ... $(OBJDIR)/re.o ...
```

Also add `$(RE_OBJ)` to `FE_OBJ`-adjacent variables for fuzz and test linking where needed (`EXTRA_lisp`, `FUZZ_SRCS`, etc.).

### 2. Native functions (`src/lisp.c`)

Inside the `#ifdef KG_USE_LISP` block:

**Include** `"../fe/tiny-regex-c/re.h"` alongside the existing fe.h include.

**`native_re_match`** — compile + match in one shot:
- Extract `pattern` (string) and `text` (string) via `FeGetNextArgument`
- Convert both to C strings with `FeToString` (use small fixed buffers, e.g. 4096 for pattern, 65536 for text — see `ArbitraryRELengthLimit` / `ArbitraryDataLengthLimit` in `fex_re.c` for precedent)
- Call `re_match(pattern, text, &matchlen)` — the one-shot convenience function, no static buffer persistence issue
- On success, return `(cons (double start) (double length))` via `FeMakeDouble` + `FeMakeCons`
- On no match (return -1), return `FeNil(context)`

**`native_re_match_p`** — boolean test:
- Same argument extraction
- Returns `t` (the symbol) on match, `nil` otherwise
- Use `FeMakeSymbol(context, "t")` for the true return

**Register** in `register_natives()`:
```c
FeDefineNative(context, "re-match", native_re_match);
FeDefineNative(context, "re-match-p", native_re_match_p);
```

**No `WITH_LISP=0` changes** needed — the existing `#else` stubs cover all `kg_lisp_*` functions already.

### 3. Design rationale

- **Use `re_match()` not `re_compile()`**: tiny-regex-c's `re_compile()` stores the result in a single static buffer (`~180 bytes`). Holding multiple compiled patterns simultaneously would require `re_compile_to()` with kg-managed storage. The convenience function `re_match()` avoids this entirely for the MVP.
- **No global/replace APIs yet**: tiny-regex-c has no capture groups (it's a TODO in `re.c:34`) and no `re_match_all` iterator. Adding `(re-match-all pattern string)` by looping `re_matchp` with offset advance is possible but adds complexity. Start simple.
- **Pattern in kg namespace, not fe proper**: fe's own regex extension (`fex_re.c`) uses POSIX `<regex.h>`. tiny-regex-c is intentionally not part of fe's build — it's for embedders like kg. Following the existing `kg-*` naming convention keeps the division clear.

### 4. Testing

**PTY test** in `test/pty/regex.yaml`:

```yaml
requires_feature: lisp
filename: test.txt
backend: pexpect
steps:
  - key: M-x
  - key: eval-expression
  - key: RET
  # Test match returning start+length
  - input: "(re-match \"[0-9]+\" \"abc123def\")\n"
  - key: RET
  - expected_saved: ...
```

Or add to the existing `test/test_lisp.c` unit test (which already links `lisp.o`, `fe.o`, and stubs). This path exercises `kg_lisp_eval_string()` with `(re-match ...)` forms.

### 5. Documentation

- **`README.md`**: Add `re-match` and `re-match-p` to the Lisp bindings table alongside the existing `kg-*` functions.
- **`doc/kg.1`**: Mention the new functions in the Lisp section.
- **`src/help.c`**: Only if there's a help topic for Lisp API (currently help is keybinding-focused).

### Summary of files touched

| File | Change |
|------|--------|
| `Makefile` | Add `re.o` rule + link + clean |
| `src/lisp.c` | Add `re.h` include, 2 native functions, 2 registrations |
| `test/test_lisp.c` or `test/pty/*.yaml` | Add regex test cases |
| `README.md` | Document `re-match` / `re-match-p` |
| `doc/kg.1` | Document new Lisp functions |

### Open questions

1. **Buffer size for strings**: Should `re-match` accept arbitrary-length strings (copying from Fe via `FeCopyStringBytes` + malloc, like `native_insert` does), or use fixed stack buffers (simpler, but limits text size)? The `copy_fe_string` pattern is already in `lisp.c` and is the right call for correctness.

2. **`re-match-p` naming**: Could also be `re-match?` — Fe supports `?` in symbol names. Following Scheme convention, `re-match?` would be idiomatic Lisp. But Fe's Fex functions (`compile-re`, `match-re`) use the `-p` style from Common Lisp. Either works.
