# Fe ↔ Emacs compatibility corpus

This is the **mechanism** for comparing Fe against the pinned Emacs oracle,
built by the kg repository's sub-plan 00B
(`doc/plans/2026-08-03-elisp-subset-and-fe-evaluator-subplans/00b-oracle-and-differential-corpus.md`
in the parent kg checkout -- not a path inside this submodule, since Fe is
usable standalone). It is deliberately not the corpus itself: this directory holds only the
handful of cases needed to prove the mechanism works, in all four record
kinds and both failure modes (a version mismatch, a missing Emacs).
Populating it broadly is sub-plan 00C's job.

No language behavior changes here. Every case below already agrees with
Emacs except the two marked `planned`/`unsupported` in `features.json`,
which are recorded as **known, expected gaps**, not failures.

## Why this exists, and why it is not the human-readable printer

Every later phase of the parent plan is gated on "matches the pinned Emacs
oracle." Without a mechanism that phrase means "somebody ran Emacs once and
wrote the answer in a test comment" -- which is what `test/test_lisp.c` and
kg's Lisp PTY cases had before this. Fe's writer is also the wrong
comparison key on its own: it is deliberately bounded (`#<cycle>`,
`#<deep>`, `#<truncated>`) so it stays stable under a fixed-arena budget,
which is exactly what makes it useless for asking "does this equal what
Emacs produced." The record protocol below is the layer underneath that
question.

## Layout

```text
compat/
  README.md          this file
  features.json       the manifest: which construct, whose it is, how it
                       compares, which cases and (if owned by kg) which kg
                       test cover it
  cases/*.json         one file per case: setup forms, expression, notes
  oracle/*.json         one checked-in Emacs snapshot per case, version-
                        stamped
  oracle/emacs-shim.el   what runs under `emacs -Q --batch` to produce a
                          canonical record for one case
../utils/
  run-emacs-oracle.py   regenerates/verifies oracle/*.json against a
                         resolved Emacs; the only thing that may write
                         those files
  run-fe-compat.py       replays cases/*.json against the standalone `fe`
                          binary and the checked-in snapshots; no Emacs
                          needed
  check_compat_manifest.py   structural checks on features.json and the
                              corpus it claims to describe
```

`run-emacs-oracle.py` takes the corpus root as its first argument (not a
constant), so sub-plan 00C can point it at kg's own
`test/lisp-compat/` -- which needs the identical `cases/`, `oracle/`, and
`oracle/emacs-shim.el` shape -- without copying the runner. `fe`'s
convention is `utils/` for Python tooling
(`utils/check_scc_complexity.py`, `utils/check_pmccabe_complexity.py`);
these three scripts follow that rather than the parent plan's illustrative
`tools/` path.

## JSON, not TOML

Every ratchet and machine-readable artifact in both the kg and fe trees is
already JSON (`.ci/coverage-baseline.json`, `.ci/mutation-gateway.json`,
`.ci/pmccabe-baseline.json`, `test/.results/*.json`), and every checker is
a Python script under `utils/`. A second serialization format buys nothing
here and costs a parser in every consumer.

## The record protocol

One JSON record per case, per side, on stdout, one line, nothing else.
Anything a runner prints incidentally (Emacs' batch startup noise, GC
messages) goes to stderr and is never parsed.

```json
{"kind": "value", "type": "integer", "printed": "3"}
{"kind": "condition", "condition": "wrong-type-argument", "data": "...", "condition_source": "structured"}
{"kind": "quit"}
{"kind": "unsupported", "feature": "hash-tables"}
{"kind": "timeout", "seconds": 5.0}
```

`kind` is the classifier and **is never derived by parsing a message
string** -- it is decided by which code path produced the record:

* `value` -- normal completion. `printed` is the comparison key; `type`
  is informational only (Fe has no integer/float split yet, so it is
  omitted on Fe's side rather than forced to agree with something Phase 5
  has not built).
* `condition` -- an error was signaled. `condition_source` says how firm
  the `condition` field's claim is:
  * `"structured"` -- from Emacs' own `condition-case`, which hands back
    the real `(error-symbol . data)` pair. This is what the oracle always
    emits.
  * `"message"` -- Fe has no condition system before Phase 6
    (parent plan §6): `FeHandleError()` only carries a free-text message,
    surfaced here as `message` rather than `condition`/`data`. The
    comparator (`run-fe-compat.py`'s `records_agree()`) treats this as a
    **weaker claim**: it checks whether the oracle's condition name
    appears in Fe's message, not structural equality. This field exists
    now specifically so Phase 6 does not have to rewrite every stored
    record to add it retroactively.
* `quit` -- Emacs' batch `condition-case` reached `quit`, which is not a
  subtype of `error` and would not be caught by an `error` handler alone
  (`oracle/emacs-shim.el` handles both explicitly, and
  `cases/planned-quit-signal.json` exercises the distinction). Fe has no
  interactive interrupt reachable from the standalone binary and does not
  produce this kind.
* `unsupported` -- the **manifest**, not a runtime error, says Fe cannot
  do this at all (`features.json` status `unsupported`). `run-fe-compat.py`
  short-circuits on this before invoking `fe`: an unbound-symbol error
  from a missing primitive and a deliberate "this does not exist" answer
  can look identical on stderr, and only one of them is a structural
  claim rather than a guess from a message.
* `timeout` -- not one of the four kinds the parent plan named, added here
  because the protocol has to survive a case that does not terminate
  without hanging every future run of the corpus. Both runners impose a
  wall-clock bound (`--timeout`, default 5s) and report this kind
  structurally, from the subprocess timeout itself, when a case does not
  return in time; nothing about it comes from message text.

Each case file (`cases/<id>.json`) records:

```json
{
	"id": "value-arith-add",
	"setup": [],
	"expr": "(+ 1 2)",
	"note": "human-readable rationale"
}
```

`setup` is a list of Lisp source strings evaluated (for side effects only)
before `expr`; both runners read `expr` as one Lisp form and evaluate it.
Setup forms must not write to stdout -- `run-fe-compat.py` takes the
entirety of `fe`'s stdout as the printed value, by convention rather than
enforcement, the same way `fe`'s own `print` primitive is the only thing
in `scripts/*.fe` that writes to stdout in a script under test.

Each oracle snapshot (`oracle/<id>.json`) wraps one record with the exact
Emacs version that produced it:

```json
{
	"schema": "fe-compat-oracle/1",
	"case": "value-arith-add",
	"emacs_version": "GNU Emacs 31.0.90 (build 1, ...) of 2026-07-09",
	"record": {"kind": "value", "type": "integer", "printed": "3"}
}
```

## The Emacs runner

`oracle/emacs-shim.el`, loaded via `emacs -Q --batch -l
oracle/emacs-shim.el CASE.json`. Lexical binding is turned on with the
optional `LEXICAL` argument to `eval` (`(eval form t)`) rather than a
per-case `-*- lexical-binding: t -*-` header or a separate `-l` shim that
sets the buffer-local variable: case bodies are strings read at run time,
not files Emacs loads on its own, so `eval`'s own argument is the direct
mechanism rather than something layered on top of it. One Emacs process
per case: simpler isolation, and it means a non-terminating case only
costs the run its own timeout, not the whole corpus's.

`condition-case` names `:success`, `quit`, and `error` as three separate
clauses (see `cases/planned-quit-signal.json`, which proves the middle one
is reachable and distinct from the third).

## The Fe runner

`run-fe-compat.py` drives the **standalone `fe` binary**, never kg, one
fresh process per case: `fe`'s non-interactive error handler
(`main.c`'s `HandleFatalError`) calls `exit(EXIT_FAILURE)` on the first
error rather than returning, so process-per-case is the natural unit, the
same shape `test.sh` already uses for `scripts/*.fe`. This is the
structured sibling of that suite, not a replacement -- `make check` keeps
running both.

Classification is structural:

* feature status `unsupported` → `{"kind": "unsupported", ...}`, `fe`
  never invoked;
* `fe`'s exit code `0` → `kind: value`, `printed` is `fe`'s stdout
  (produced by wrapping the case's `expr` in `(print expr)`, since `fe`
  only echoes a result automatically when reading from an interactive
  stdin REPL, which per-case isolation does not use);
* nonzero exit → `kind: condition`, `condition_source: message`, the
  first `error: ...` line of stderr;
* no return within `--timeout` → `kind: timeout`.

## What decides pass vs. fail: `features.json`'s `status`

* `status: supported` -- the oracle and Fe **must** agree; a mismatch is a
  regression and fails `make compat`.
* `status: planned` / `unsupported` / `divergent` -- a mismatch is a
  **known, expected gap**, printed for visibility but not a failure. This
  is the whole point of the manifest: "compatibility work still pending"
  and "a regression" are different findings, and only a structured status
  field makes that mechanically decidable instead of a matter of whoever
  is reading the CI log that day.

A `planned` entry whose case has a checked-in Emacs snapshot is a **phase
contract**, 02A's precedent: the snapshot freezes the target semantics
before the implementation slice that will land them (Phase 4's Lisp-2
namespaces are the current one, recorded ahead by sub-plan 04A), and the
landing slice flips the entry to `supported` with the behaviour change as
its evidence. The snapshot is the oracle's answer, not a promise about Fe
-- Fe keeps reporting its current known gap until the flip.

## Running it

```sh
make -C fe compat          # fe + checked-in snapshots; no Emacs needed
make -C fe compat-oracle   # regenerate/verify snapshots against the
                            # resolved Emacs; fails on a version mismatch
```

Emacs resolution reuses `utils/pty_accept.py`'s `resolve_emacs()` order
exactly, on purpose: `--emacs` (here, `COMPAT_EMACS=` on `make`), then
`$KG_PTY_EMACS`, then `emacs` on `PATH`, then the `/opt-3` developer-box
pin. Missing Emacs `SKIP`s `compat-oracle`; `--require-tools` (passed
through `COMPAT_ORACLE_ARGS`) turns that into a named failure instead --
`compat` itself never needs Emacs at all.

A version mismatch between a checked-in snapshot and the running Emacs
**fails** `compat-oracle` and leaves the snapshot untouched; regenerating
under a genuinely different Emacs is `--allow-version-change`
(`COMPAT_ORACLE_ARGS=--allow-version-change`), a deliberate action, never
the default. "Emacs 31" is not a pin -- the box runs a development build
off the `emacs-31` branch, dated by build, not by release -- so silently
re-recording under a moved version would turn the oracle into an echo.

## What this does not do

* It does not populate the corpus broadly. Sub-plan 00C does that,
  deliberately kept separate so the schema was not designed around
  whichever twenty cases got written first.
* It does not execute kg. Sub-plan 00C links kg's existing native/PTY
  assertions to these oracle snapshots for `comparison: emacs` entries,
  and to kg-specific expectations for `comparison: kg-policy` ones.
* It does not compare backward regex matching, cyclic-structure printing,
  or anything else where Fe/kg deliberately encode a policy of their own
  rather than Emacs'. The parent kg repository's top-level `AGENTS.md`
  (`CLAUDE.md`) already states the precedent for `kg_regex_match_backward()`:
  where kg's behavior is a deliberate local policy, an oracle for it would
  have to encode that policy rather than Emacs', which makes it a test,
  not an oracle.
