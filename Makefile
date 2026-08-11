ifeq ($(wildcard tiny-regex-c/re.c),)
$(error tiny-regex-c/re.c is missing; run 'git submodule update --init --recursive')
endif

ifeq ($(origin CC),default)
CC = clang
endif
CFLAGS ?= -Weverything -Werror -std=c2x \
	-Wno-poison-system-directories \
	-Wno-declaration-after-statement \
	-Wno-padded \
	-Wno-switch-default \
	-Wno-pre-c23-compat \
	-Wno-pre-c11-compat \
	-Wno-c++-compat \
	-Wno-unsafe-buffer-usage \
	-Wno-implicit-fallthrough \
	-Wno-unused-command-line-argument \
	-Wno-unknown-warning-option \
	-Wno-reserved-macro-identifier \
	-Wno-reserved-identifier \
	-Wno-extra-semi-stmt
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -Itiny-regex-c
LDLIBS ?= -lm

# tiny-regex-c is third-party and is not held to Fe's -Weverything build, but it
# must carry whatever sanitizer flags CI puts in CFLAGS: otherwise the ASan and
# MSan lanes cover only the Fe side of fex_re.c.
RE_SANITIZE = $(filter -fsanitize%,$(CFLAGS)) \
	$(filter -fno-sanitize%,$(CFLAGS)) \
	$(filter -fno-omit-frame-pointer,$(CFLAGS))
RE_CFLAGS ?= -O3 -Wall -Wextra -std=c2x $(RE_SANITIZE)

# Nothing below makes an object depend on the flags it was compiled with,
# and every CI lane here changes them: the sanitizer stages get away with
# it only because they pass -B.  A lane that forgets to (or a developer
# switching between two flag sets by hand) links objects from the previous
# one, and an MSan stage once passed against a re.o that MSan had never
# seen.  This stamp holds the current flag set, and every object depends
# on it, so a changed flag set is a changed prerequisite.
BUILD_STAMP := .build-flags
BUILD_ID := $(CC)|$(CPPFLAGS)|$(CFLAGS)|$(RE_CFLAGS)|$(LDFLAGS)|$(LDLIBS)
$(shell [ "$$(cat $(BUILD_STAMP) 2>/dev/null)" = '$(BUILD_ID)' ] || \
	printf '%s' '$(BUILD_ID)' >$(BUILD_STAMP))

PROG = fe
TARGET = $(PROG)
SRCS = main.c auto.c fe.c fe_eval.c fe_run.c fe_unwind.c fex.c fex_io.c \
	fex_math.c fex_process.c fex_re.c fex_time.c
# The evaluator's own object list, shared by every link rule that used to
# name `fe.o` alone (sub-plan 03B's fe.c -> fe.c + fe_eval.c split, sub-plan
# 11B's fe_eval.c -> fe_eval.c + fe_run.c one, and Phase 20's fe_eval.c ->
# fe_eval.c + fe_unwind.c one): a list so every consumer below stays a
# one-line change.
FE_CORE_OBJS = fe.o fe_eval.o fe_run.o fe_unwind.o
HDRS = $(wildcard *.h)
OBJS = $(SRCS:.c=.o) tiny-regex-c/re.o
SOURCES = $(SRCS) $(HDRS)
TEST_API = test_api
TEST_SRCS = test_api.c test_header.c test_internal_header.c gc_stress.c
EXAMPLE_HOST = example_host
EXAMPLE_SRCS = example_host.c
EXAMPLE_RUNNER ?=

# Standalone core checks use flags supported by both kg's C23 compilers.
CORE_GCC ?= gcc
CORE_CLANG ?= clang
CORE_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x
CORE_OBJS = fe-core-gcc.o fe-core-clang.o fe-eval-core-gcc.o \
	fe-eval-core-clang.o fe-run-core-gcc.o fe-run-core-clang.o \
	fe-unwind-core-gcc.o fe-unwind-core-clang.o

# Fuzzing
FUZZ_DIR ?= fuzz
FUZZ_CC ?= clang
FUZZ_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x -O1 -g \
	-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer
FUZZ_RUNS ?= 1000
FUZZ_MAX_LEN ?= 4096
FUZZ_TIMEOUT ?= 2
FUZZ_RSS_LIMIT_MB ?= 512
FUZZ_VERBOSITY ?= 0
FUZZ_CORPUS_DIR ?= $(FUZZ_DIR)/corpus
FUZZ_ARTIFACT_DIR ?= $(FUZZ_DIR)/artifacts
FUZZ_SUPPORT = $(FUZZ_DIR)/fuzz_support.c
FUZZ_READER_BIN = $(FUZZ_DIR)/fuzz_reader
FUZZ_EVAL_BIN = $(FUZZ_DIR)/fuzz_eval
FUZZ_WRITE_BIN = $(FUZZ_DIR)/fuzz_write
FUZZ_SRCS = $(FUZZ_SUPPORT) $(FUZZ_DIR)/fuzz_reader.c \
	$(FUZZ_DIR)/fuzz_eval.c $(FUZZ_DIR)/fuzz_write.c

# Project metrics
SCC ?= scc
SCC_PATHS ?= $(SOURCES) $(FUZZ_SRCS)
SCC_COMPLEXITY_PATHS ?= $(SOURCES)
# Raised from 172, via 185 and 195, for the Fex file-lifecycle and
# argument-handling work and then `unwind-protect`/`FeProtectWithCleanup`:
# the validation and unwind bookkeeping those needed is branches, and
# refusing to add them is the wrong trade. Note that scc's total is a floor,
# not a measurement: its C string-state machine desynchronizes on fe.c's
# `'"'` character literals, so keywords below them are not counted at all.
# pmccabe below sees the whole file.
#
# Raised again, 210->220 / 105->112, by kg's
# 2026-08-03-elisp-subset-and-fe-evaluator-subplans/00a-budget-and-fe-structure.md:
# the Emacs-subset program's Phase 0 needs a handful of read-only arena
# counters (object/free slot counts, peak live objects, GC count) in fe.c
# before kg's Phase 0 baseline can be taken at all -- see that sub-plan's
# Decision for the reasoning and the measured spike that this budget does
# NOT yet cover (a translation-unit split of fe.c, priced separately when
# Phase 3 lands). This is a small, named, immediate need, not a program-wide
# promise: every later phase of that program prices and requests its own
# raise against this file's measured total when its own sub-plan lands.
#
# Raised a third time, 220->420 / 112->240, by sub-plan 03A of the same
# program's Phase 3 Decision (set README, dated 2026-08-04). This is the
# large one: 03A's throwaway split spike (fe.c -> fe.c + fe_eval.c, exactly
# 03B's cut) measured the total jumping 214->286 from the mechanical move
# alone, with the new fe_eval.c file alone scoring 108 of the *old* 112 file
# cap before a single frame-machine line exists -- confirming scc's own
# comment below: the evaluator was invisible to scc while it lived past
# fe.c's `'"'`-literal desync, and extracting it un-blinds real complexity
# that was always there. 03A's Decision funds two things ahead of when they
# land, per Rule 6: 03B's measured split (an unconditional total floor of
# 286, no substance yet) plus a frame-machine substance estimate obtained by
# roughly doubling fe_eval.c's own *measured* 108, not the stale cross-file
# 00A estimate this comment used to cite. `PMCCABE_TOTAL_MAX` below is the
# authoritative aggregate for the core from this Decision on; scc's total
# and file caps remain secondary ratchets -- they still catch an unbudgeted
# new file or complexity in the `fex_*` files, where no desync applies.
#
# Raised a fourth time, 420->480 / 240->300, by sub-plan 04A of the same
# program's Phase 4 Decision (set README, dated 2026-08-05). This funds
# Lisp-2 namespaces, 04B-04D by name, priced against the shape of the real
# tree rather than the price table's original guess: ~9 new primitives
# (`function`, `fset`, `symbol-function`, `symbol-value`, `fboundp`,
# `fmakunbound`, `defalias`, `funcall`, `apply`) landing in
# `DispatchPrimitive` and the resume arms, the head-resolution fork in
# `RunEvaluationLoop` (already at 14 of the per-function cap; budget for
# extracting a helper rather than growing the switch), designator-chain
# resolution, and 04B's accessor layer, which is near-free in pmccabe but
# not zero. The estimate is +40 to +60; both gates had exactly 29 points of
# measured headroom (391/420 scc, 601/630 pmccabe) against a phase priced
# at +40 to +60, so the caps move by the top of that range, funding the
# whole phase at once per the 00A/03A precedent. pmccabe remains the
# authoritative unit for the core from 03A's Decision; both units are
# priced and reported anyway. kg needs no raise for the same phase -- its
# +15 to +25 estimate sits inside 56 points of measured headroom (5444/5500).
#
# Raised a fifth time, 480->540 / 300->340, by sub-plan 05A of the same
# program's Phase 5 Decision (set README, dated 2026-08-05), funding
# 05B-05D by name ahead of when they land: the integer tower is priced
# +50 to +70 against the real tree (the type in the existing Value union,
# the Emacs number lexer replacing ReadAtom's strtod, shortest-round-trip
# float printing, either-type ResumeArith/ResumeBinary arms, the chained
# comparators, the `>`/`>=`/`/=`/`integerp`/`floatp`/`eq`/`eql` primitives, per-function
# math-native return types), and the measured starting state is 459/480
# scc (fe_eval.c 276/300) with 660/690 pmccabe across 248 symbols -- 21
# and 30 points of headroom against a row priced at +50 to +70, so the
# caps move to the top of that range and fund the whole phase at once.
# This slice adds no code, so the actuals stay put until 05B lands.
#
# Raised a sixth time, 540->680 / 340->420, by sub-plan 06A of the same
# program's Phase 6 Decision (set README, dated 2026-08-05), funding
# 06B-06D by name ahead of when they land: structured errors and non-local
# exits are priced +80 to +120 against Phase 3's *measured* +101 pmccabe
# as the closest control-flow comparable (the condition hierarchy has no
# predecessor), and the measured starting state is 533/540 scc (fe_eval.c
# 326/340) with 751/760 pmccabe -- 7, 14 and 9 points of headroom against
# a row priced +80 to +120, so all three caps move at once by the top of
# that range and fund the whole phase. The scc total is funded explicitly
# even though the price table's own instruction forgot to name it: at 2
# points free it is the *tightest* of the three, and the split tax 06A
# Decision 1 may incur (03B's precedent: +72 scc, pmccabe conserved) is
# absorbed by the scc raise. This slice adds no code, so the actuals stay
# put until 06B lands.
#
# The per-file cap alone is raised a seventh time, 420 -> 460, dated
# 2026-08-06, by the Phase 6 adversarial-review fixes: measured 440 for
# fe_eval.c after them, against 422 before (the tree was already 2 over its
# own cap at that point). The +18 is control-flow the review found missing
# rather than new features -- the evaluation-control record that a caught
# condition restores instead of clearing, the raise split into a formatting
# half and a working half so a cleanup's replayed completion is not labelled
# twice, the completion kind travelling with a cleanup's own failure, the
# quit/budget condition objects, the pending-throw escape that lets a
# cleanup's `throw` reach the catch it names, and `FeTryCallWithOptions`/
# `FeResignal`. The two totals are NOT raised and did not need to be: scc
# 654/680 and pmccabe 863/900 after the same work.
#
# The alternative to this raise is 06A Decision 1's deferred split -- the
# condition hierarchy, the matcher and `error`'s format machinery are a
# natural unit and would move out as ~a third of the file. It is not taken
# here because 03B measured a mechanical split at +72 scc *total*, which
# would put the total at ~726 against a 680 cap and so would have to be
# funded as a phase of its own rather than smuggled in with a review fix.
# fe_eval.c is at 440 of 460; the next phase to touch it should price the
# split rather than ask for 500.
# Raised a seventh time by Phase 7 sub-plan 07A (2026-08-06), funding 07B's
# strict-arity dispatch, ArgsToEnv validation, native-helper classification and
# fuzz builders: the measured starting tree is 654/680 scc, fe_eval.c 440/460,
# and 863/900 pmccabe across 292 symbols. The phase is priced +50..80 in both
# scc and pmccabe; the top of that range funds it before implementation, as in
# 03A/04A/05A/06A. The per-file raise funds fe_eval.c's measured 440 and the
# new validation path; a new function still must stay at or below 15.
# Raised an eighth time by Phase 9 sub-plan 09A's Decision 7 (2026-08-06),
# funding 09B (catchable exhaustion conditions) and 09C (the flat-stack mark
# phase) by name. The measured starting tree is 746/760 scc with fe_eval.c at
# 489 and fe.c at 140 against the 520 file cap; Phase 9 is priced fe +30..50
# scc, which breaches the total at the *bottom* of its band, so the cap moves
# by the top of the band plus the same small margin 03A/04A/05A/06A/07A used.
# The per-file cap is NOT raised and does not need to be: 09B's plumbing and
# 09C's mark rewrite both land in fe.c (140/520) and fe_eval.c (489/520), and
# a file crossing 520 in this phase would be a design signal, not a funding
# one. 09D re-sets both totals to the measured actuals at the phase close.
# Set 820 -> 757 at the Phase 9 fe close (2026-08-07): the measured actual
# after 09B and 09C, not an estimate, and the same convention the Phase 8 fix
# cycle established -- the cap sits *on* the number, proven by temporarily
# lowering it by one and watching the gate fire. At 756 `make complexity-check`
# reports "FAIL: total complexity 757 exceeds limit 756"; at 757 it passes.
# The phase spent 11 of the 74 points the raise funded (746 -> 757 against a
# +30..50 price), so the 63 unspent ones go back rather than sitting as
# unearned headroom. `SCC_FILE_COMPLEXITY_MAX` was never raised for Phase 9
# and stays where it is; the worst file is fe_eval.c at 490.
# Raised a ninth time, 757 -> 775, by the Phase 9 fe fix cycle (2026-08-07),
# funding the adversarial review's F1, F2, F9 and F10 by name:
#   F1  a collector guard (`collecting` in `FeContext`, set across
#       `CollectGarbage`) plus the two places that read it -- `WriteObject`'s
#       step charge and `RaiseCompletionCore`'s fatal arm.
#   F2  re-stamping the two shared exhaustion conditions before they are
#       published, so a handler's `setcar`/`setcdr` cannot disable them.
#   F9  a `static_assert` on the pointer-reversal alignment premise.
#   F10 a sweep-time assert that no `GcMarkCdrBit` leaked.
# The fix cycle is priced +6..14 scc against the real tree; the measured
# starting total is 757/757 -- the cap sits *exactly* on the number, so the
# first of those fixes breaches it. 775 is the top of the band plus the same
# small margin 03A/04A/05A/06A/07A/09A used. Proved live before this raise
# landed by temporarily setting the cap to 756 and watching
# `make complexity-check` report "FAIL: total complexity 757 exceeds limit
# 756". `SCC_FILE_COMPLEXITY_MAX` is not raised and does not need to be: all
# of it lands in fe.c (147/520) and fe_eval.c (490/520). The final commit of
# this cycle re-sets this number to the measured actual.
# Set 775 -> 765 at the Phase 9 fe fix cycle's close (2026-08-07): the measured
# actual after F1, F2, F8, F9 and F10, not an estimate, and proved the way this
# repository's convention requires -- the cap sits *on* the number, checked by
# temporarily lowering it by one and watching the gate fire. At 764
# `make complexity-check` reports "FAIL: total complexity 765 exceeds limit
# 764"; at 765 it passes. The cycle spent 8 of the 18 points the raise funded
# (757 -> 765 against a +6..14 price), so the 10 unspent ones go back rather
# than sitting as unearned headroom. `SCC_FILE_COMPLEXITY_MAX` was not raised
# and stays where it is; the worst file is fe_eval.c at 495.
# Raised a tenth time, 765 -> 795, by Phase 10 sub-plan 10A's Decision 8
# (2026-08-07), funding 10B -- `macroexpand-1` and `macroexpand` as
# primitives, `macroexpand-all` rejected by its own name -- and nothing else.
# The measured starting total is 765/765: the cap sits *exactly* on the
# number, so the phase's first C line breaches it. Phase 10's fe share is
# priced +15..30 scc (exposure of the evaluator's existing expansion path
# plus a fixpoint loop, not new machinery), and 795 is the top of that band
# with no margin at all -- the price table's own instruction, since the
# work is bounded to three primitives. Proved live before this raise landed
# by temporarily setting the cap to 764 and watching `make complexity-check`
# report "FAIL: total complexity 765 exceeds limit 764", then restoring.
# `SCC_FILE_COMPLEXITY_MAX` is not raised and does not need to be: the whole
# slice lands in fe_eval.c (495/520) and fe.c (150/520). The last commit of
# this slice re-sets this number to the measured actual, as 10A Decision 8
# requires and every phase since 08 has done.
# Set 795 -> 787 at the sub-plan 10B close (2026-08-07): the measured actual
# after the `macroexpand-1`/`macroexpand` primitives, the `macroexpand-all`
# rejection, the compat corpus and the alias-rule correction -- not an
# estimate, and proved the way this repository's convention requires, by
# temporarily lowering the cap by one and watching the gate fire. At 786
# `make complexity-check` reports "FAIL: total complexity 787 exceeds limit
# 786"; at 787 it passes. The slice spent 22 of the 30 points the raise
# funded (765 -> 787, against a +15..30 price), so the 8 unspent ones go back
# rather than sitting as unearned headroom.
# `SCC_FILE_COMPLEXITY_MAX` was not raised, and one number here is a warning
# rather than a result: fe_eval.c is at **517 of 520**, three points of
# headroom, up from 495. It crossed 520 once during this slice and was
# brought back by collapsing three functions into one rather than by raising
# this cap. The next change to that file of any size should expect to price a
# file-cap raise or a translation-unit split rather than assume the room is
# there. fe.c is at 150.
# Raised an eleventh time, 787 -> 835, by Phase 11 sub-plan 11A's Decision 8
# (2026-08-07), funding 11B (special variables and shallow dynamic binding),
# 11C (the `(quote X)` -> `'X` writer abbreviation and
# `FeTryEvaluateStringWithOptions`) and nothing else. The measured starting
# total is 787/787 -- the cap sits exactly on the number again, so the phase
# breaches it at its first C line. The priced band is +15..30 for 11B and
# +10..19 for 11C, so 835 is 787 plus the top of both, with no margin.
# Proved live before this raise landed by temporarily setting the cap to 786
# and watching `make complexity-check` report "FAIL: total complexity 787
# exceeds limit 786", then restoring. The last commit of sub-plan 11C re-sets
# this number to the measured actual, pre-pin, as 11A Decision 8 requires.
# `SCC_FILE_COMPLEXITY_MAX` is *not* raised, and 10B's warning above is why:
# fe_eval.c was at 517 of 520 and the whole of 11B's binding work lands in
# its binding section. 11A Decision 8 answers that with a translation-unit
# split rather than a raise, and this commit is it -- fe_eval.c 517 -> 494
# and a new fe_run.c at 23, summing to the same 517, because
# `utils/check_scc_complexity.py` sums per file and a split therefore moves
# no total. The per-file cap goes on binding at full strength; proved live
# the same way, by setting `SCC_FILE_COMPLEXITY_MAX=493` and watching
# "FAIL: 1 file(s) exceed per-file limit 493".
# Set 835 -> 806 at the sub-plan 11C close (2026-08-07), pre-pin: the
# measured actual after the translation-unit split, dynamic binding, the two
# new primitives, the `(quote X)` -> `'X` writer abbreviation and
# `FeTryEvaluateStringWithOptions`. Proved the way this repository's
# convention requires, by temporarily lowering the cap by one and watching
# the gate fire: at 805 `make complexity-check` reports "FAIL: total
# complexity 806 exceeds limit 805"; at 806 it passes. Phase 11 spent 19 of
# the 48 points the raise funded (787 -> 806, against a +25..49 price across
# 11B and 11C), so the 29 unspent ones go back rather than sitting as
# unearned headroom. The split is why the number is that low: 11B's own
# opening commit moved 23 points of fe_eval.c into fe_run.c at zero net
# cost, and everything after it was priced against a file that had room.
# `SCC_FILE_COMPLEXITY_MAX` is deliberately *not* re-set to its actual and
# stays at 520, as it has since 08: the per-file numbers are fe_eval.c 509,
# fe.c 152, fe_run.c 25, main.c 37. 10B's warning -- that fe_eval.c at 517
# of 520 left the next change of any size to price a raise or a split -- was
# answered by the split rather than by a raise, and fe_eval.c came out of
# this phase at 509 with 11 points of headroom, which is *not* comfortable.
# The next slice to touch that file should expect to price a second split
# (the completion/cleanup machinery, lines 230-700, is the coherent seam)
# rather than assume the room is there.
# Re-measured and left at 806 by the Phase 11 fe fix cycle (2026-08-07),
# pre-pin: the cycle fixed two blockers and a major and cost this unit
# nothing at all. The checked binding-list advance is a `CheckType` call
# rather than an `if`, the `for` it replaced and the `while` it uses count
# the same, and the completion-in-flight fix and the result-rooting fix add
# no branch anywhere. Per-file after the cycle: fe_eval.c 509, fe.c 152,
# main.c 37, fex_io.c 28, fe_run.c 25 -- fe_eval.c unmoved, so the warning
# above still stands exactly as written. Proved live at this head by
# temporarily lowering each cap by one and watching the gate fire: at 805
# `make complexity-check` reports "FAIL: total complexity 806 exceeds limit
# 805" and at 806 it passes; at 508 it reports "FAIL: 1 file(s) exceed
# per-file limit 508".
#
# Raised 806 -> 835 by sub-plan 12A Decision 7 (set README, dated
# 2026-08-07), funding Phase 12's fe workstream by name: 12B's two
# evaluator items (honoring a condition handler established inside a
# running cleanup, and the `eval` primitive evaluating in the current run)
# and 12C's two (the `file-missing` hierarchy line, which is pure data, and
# the one-argument-`defvar` scope carrier in `fe.c`'s `EvaluateInput`).
# Priced at +12..22 for 12B and +6..14 for 12C against the real tree; the
# measured starting total is 806/806 -- zero headroom against a +18..36
# phase, so the cap moves by 29, near the top of that range, and funds both
# implementing slices at once. That is the same 00A/03A/04A/05A precedent
# every earlier phase used, and it restores exactly the 29 points 11C's
# close handed back unspent. 12C re-sets this to its measured actual in the
# phase's last fe commit, pre-pin.
#
# `SCC_FILE_COMPLEXITY_MAX` deliberately does NOT move: four Phase 12 items
# were audited into `fe_eval.c`, which has 11 points of headroom, and 12A
# Decision 7 funds the Makefile-named second seam split
# (`fe_eval.c:230-700`, the completion/cleanup machinery) in preference to
# a per-file raise if the file cap threatens.
#
# Proved live at this head, before the raise, the way this repository's
# convention requires -- by temporarily lowering each cap and watching the
# gate fire. At 805 `make complexity-check` reports "FAIL: total complexity
# 806 exceeds limit 805" and exits 2; at 806, and at the 835 below, it
# passes. At 508 it reports "FAIL: 1 file(s) exceed per-file limit 508" and
# exits 2.
#
# Set 835 -> 808 at the sub-plan 12C close (2026-08-07), pre-pin: that IS
# the measured actual after Phase 12's whole fe workstream -- the
# cleanup-handler ordering fix, the `eval` primitive, the `file-missing`
# hierarchy line and the one-argument-`defvar` scope carrier. The phase spent
# 2 of the 29 points the raise funded (806 -> 808), so the 27 unspent ones go
# back rather than sitting as unearned headroom, exactly as 11C handed 29
# back. Two points is the whole of it because scc counts keywords: the
# ordering fix's `&&` and the scope carrier's comparisons are not keywords,
# and the hierarchy line is data. The +2 is `DispatchEval`'s arm.
# `SCC_FILE_COMPLEXITY_MAX` is again deliberately NOT re-set to its actual
# and stays at 520. Per-file at the close: fe_eval.c 511, fe.c 152, main.c
# 37, fex_io.c 28, fe_run.c 25. fe_eval.c moved 509 -> 511 and has 9 points
# left, which is less comfortable than the 11 the last phase warned about --
# so the warning above stands with more force, not less, and the
# Makefile-named second seam split (the completion/cleanup machinery, lines
# 230-700) remains the funded answer for the next slice that needs room
# there. 12C's own work deliberately landed in fe.c for that reason.
# Proved live at this head by temporarily lowering each cap and watching the
# gate fire: at 807 `make complexity-check` reports "FAIL: total complexity
# 808 exceeds limit 807" and exits 2, and at 808 it passes; at 510 it
# reports "FAIL: 1 file(s) exceed per-file limit 510" and exits 2.
#
# Raised 808 -> 835 by Phase 12's fe FIX CYCLE (2026-08-07), which reopens
# the workstream an acceptance review rejected and so forces the phase's
# second pin move. The ceiling is not a new number: it is the 835 12A
# Decision 7 already funded this phase, restored for the remainder of the
# work it was funding. What the cycle has to pay for is one blocker fix
# (`input_scope` was never restored when an input unit exited abnormally
# through an unprotected entry point, so one failed `M-:` inverted
# one-argument-`defvar` visibility for the rest of a kg session), the
# loader entry trio the kg review's negative result sized
# (`FeEnterInputUnit`/`FeReadInputForm`/`FeLeaveInputUnit`, which let a host
# drive its own read-eval loop inside one input unit in the CURRENT run),
# and three review majors that are documentation and test evidence rather
# than code. Priced against the real tree at +0..2 for the blocker (the fix
# is a store at the host exit plus a save/restore struct; scc counts
# keywords and it introduces none) and +6..12 for the trio, so 27 points is
# again well above the top of the range -- deliberately, because this is the
# same funded ceiling and re-pricing it downward mid-phase would only mean
# a third raise. The cycle re-sets this to its measured actual in its own
# last commit, pre-pin, exactly as 12C's close did.
#
# `SCC_FILE_COMPLEXITY_MAX` again does NOT move. The blocker fix and the
# entry trio both land in `fe.c` (152 of 520), for the reason the warning
# above gives: `fe_eval.c` is at 511 with 9 points left, and the
# Makefile-named second seam split remains the funded answer there.
#
# Proved live at this head, before the raise, the way this repository's
# convention requires -- by temporarily lowering each cap and watching the
# gate fire. At 807 `make complexity-check` reports "FAIL: total complexity
# 808 exceeds limit 807" and exits 2; at 808, and at the 835 below, it
# passes. At 510 it reports "FAIL: 1 file(s) exceed per-file limit 510" and
# exits 2.
#
# Set 835 -> 808 at the close of Phase 12's fe FIX CYCLE (2026-08-07),
# pre-pin: that IS the measured actual after the cycle -- the input-unit
# unwind fix, the loader entry trio, and three review majors that were
# documentation, manifest and test evidence. The cycle spent ZERO of the 27
# points the raise funded, and all 27 go back rather than sitting as
# unearned headroom, exactly as 12C's close handed 27 back and 11C handed 29.
#
# Zero is the honest number and it is an artefact, not an achievement: scc's
# string-state machine desynchronizes on fe.c's '"' character literal and
# stops counting keywords below it, and every line this cycle added to fe.c
# -- `SaveInputUnit`, `RestoreInputUnit`, `EnterHostInputContext`,
# `EnterInputUnit`, `FeEnterInputUnit`, `FeLeaveInputUnit` and
# `FeReadInputForm`, three `if`s and eleven `return`s among them -- is below
# it. `PMCCABE_TOTAL_MAX` below is the authoritative core measure and it did
# move (+13); read the two together and believe that one. The Makefile has
# said scc is "a floor, not a measurement" since 03A and this is what that
# means in practice.
#
# `SCC_FILE_COMPLEXITY_MAX` is again deliberately NOT re-set to its actual
# and stays at 520. Per-file at the close: fe_eval.c 511, fe.c 152, main.c
# 37, fex_io.c 28, fe_run.c 25 -- identical to 12C's close, for the same
# undercount reason on fe.c. fe_eval.c is unmoved at 511 with 9 points left,
# so the seam-split warning above stands exactly as written; the cycle's own
# work landed in fe.c deliberately.
#
# Proved live at this head by temporarily lowering each cap and watching the
# gate fire, exit status checked: at 807 `make complexity-check` reports
# "FAIL: total complexity 808 exceeds limit 807" and exits 2, and at 808 it
# passes; at 510 it reports "FAIL: 1 file(s) exceed per-file limit 510" and
# exits 2.
#
# Set 832 -> 840 at Phase 19 (2026-08-11), pre-pin: that IS the measured
# actual after the phase's whole fe workstream -- `error-message-string` and
# its C entry point, the `error-message` properties seeded at context open,
# and the writer's backslash escape. No raise-then-spend cycle this time:
# the phase is small enough that its cost was measured directly and the cap
# set to it, which is what every close in this file does anyway.
#
# The +8, by file: fe_eval.c 511 -> 519 (`ConditionRowAt` and
# `ConditionInheritsFrom` beside the table, the `error-message-string`
# dispatch and resume arms, and the quit row's `if` REMOVED from
# `IsConditionSymbol`), fe.c 176 unchanged as scc counts it -- and that last
# figure is the undercount this file has warned about since 03A, since the
# renderer, the seeding and the arena accounting are all in fe.c and all
# below its '"' character literal. `PMCCABE_TOTAL_MAX` moved +45 and is the
# authoritative measure; read the two together and believe that one.
#
# `SCC_FILE_COMPLEXITY_MAX` does NOT move, and the warning it has carried
# since 12A now has ONE point of slack behind it rather than nine:
# fe_eval.c is at 519 of 520. The Makefile-named second seam split
# (`fe_eval.c`'s completion/cleanup machinery, lines 230-700) is not
# optional headroom any more -- the next slice that adds a branch to that
# file has to do it first.
#
# Proved live at this head by temporarily lowering each cap and watching the
# gate fire, exit status checked: at 839 `make complexity-check` reports
# "FAIL: total complexity 840 exceeds limit 839" and exits 2, and at 840 it
# passes; at 518 it reports "FAIL: 1 file(s) exceed per-file limit 518" and
# exits 2, the one file being fe_eval.c at 519.
#
# Left at 840 by Phase 20's translation-unit split (2026-08-11), which is the
# second seam split this file has named as the funded answer since 12A and
# has warned about with growing force since 10B: fe_eval.c 519 -> 404 and a
# new fe_unwind.c at 115, summing to the same 519, so the total does not
# move at all -- `utils/check_scc_complexity.py` sums per file and a split
# therefore costs nothing, exactly as 11B's fe_run.c split measured. The seam
# is the one this file named: the completion machinery -- the ambient
# evaluation-control record, the condition hierarchy and its handler search,
# the cleanup registry, and every raise. The new file's own boilerplate is
# comments and includes, neither of which scc counts as complexity, which is
# why the split price is zero rather than the few points a new file usually
# costs.
#
# `SCC_FILE_COMPLEXITY_MAX` again does NOT move and stays at 520. Per-file
# after the split: fe_eval.c 404, fe.c 176, fe_unwind.c 115, main.c 37,
# fex_io.c 28, fe_run.c 25. The warning 10B/12A/19 carried -- one point of
# slack on fe_eval.c -- is retired by this split, not by a raise: the
# evaluator now has 116 points of headroom and the completion machinery 405,
# and the per-file cap goes on both at full strength.
#
# Proved live at this head by temporarily lowering each cap and watching the
# gate fire, exit status checked: at 839 `make complexity-check` reports
# "FAIL: total complexity 840 exceeds limit 839" and exits 2, and at 840 it
# passes; at 403 it reports "FAIL: 1 file(s) exceed per-file limit 403" and
# exits 2, the one file being fe_eval.c at 404.
SCC_COMPLEXITY_MAX ?= 840
SCC_FILE_COMPLEXITY_MAX ?= 520
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(SRCS)
PMCCABE_FUNCTION_COMPLEXITY_MAX ?= 22
# The scc total above is a floor, not a measurement (its string-state
# machine desynchronizes on fe.c's '"' character literals and stops
# counting keywords below them).  This manifest is the per-symbol
# no-regression ratchet: pmccabe reads every function, its complexity is
# recorded per symbol, no symbol may exceed its entry, and a function with
# no entry is new and has to arrive at or under PMCCABE_NEW_FUNCTION_MAX.
# `make pmccabe-baseline` is the only thing that rewrites it, and -- since
# sub-plan 03A's Decision -- it may not do so for a tree outside either
# budget below; both are checked first.
PMCCABE_BASELINE ?= .ci/pmccabe-baseline.json
PMCCABE_NEW_FUNCTION_MAX ?= 15
# The funded whole-program pmccabe aggregate (sub-plan 03A, set README
# Decision, 2026-08-04): the authoritative core measure from this Decision
# on, because scc's total is not one (see above). Audited at 500 across 202
# symbols before this Decision; 03A's split spike measured the total
# *conserved exactly* across the mechanical fe.c -> fe.c + fe_eval.c move
# (500 before, 500 after -- pmccabe reads every function regardless of which
# file it is in). This raise funds 03C-03E's frame-machine substance ahead
# of when it lands, per Rule 6: roughly doubling fe_eval.c's own measured
# evaluator weight (104 across 29 symbols in the spike) is +100 to +140,
# landing the total at 600-640; funded at 630, near the top of that range
# with a small margin, not the program's full uncertainty range.
#
# Raised again, 630->690, by sub-plan 04A's Phase 4 Decision (set README,
# dated 2026-08-05), funding Lisp-2 namespaces 04B-04D by name: the phase
# is priced +40 to +60 pmccabe against the real tree (9 new primitives in
# `DispatchPrimitive`/the resume arms, `funcall`/`apply` priced against
# `ResumeEvalList`'s weight roughly doubled, a head-resolution helper
# extracted out of `RunEvaluationLoop`, designator-chain resolution, and
# 04B's accessor layer), and the measured starting total is 601/630 -- 29
# points of headroom against a +40 to +60 phase, so the cap moves by the
# top of that range and funds the whole phase at once, the 00A/03A
# precedent. Both scc gates move with it (420->480 / 240->300, same
# estimate); the per-symbol manifest is unchanged and is re-banked only if
# 04B-04D land improvements.
#
# Raised again, 690->760, by sub-plan 05A's Phase 5 Decision (set README,
# dated 2026-08-05), funding integers 05B-05D by name: the phase is priced
# +50 to +70 pmccabe against the real tree (integer dispatch in
# `ResumeArith`/`ResumeBinary`/the `=` arm -- the `ARITH_OP`/`NUM_CMP_OP`
# macros this row originally named died with the recursive evaluator in
# 03E -- plus seven new primitives `>` `>=` `/=` `integerp` `floatp` 05D's
# `eq`/`eql`, the Emacs number lexer and the shortest-round-trip printer),
# and the measured starting total is 660/690 across 248 symbols -- 30
# points of headroom against a +50 to +70 phase, so the cap moves by the
# top of that range and funds the whole phase at once, the 00A/03A/04A
# precedent. Both scc gates move with it (480->540 / 300->340, same
# estimate); the per-symbol manifest is unchanged and is re-banked only if
# 05B-05D land improvements.
#
# Raised again, 760->900, by sub-plan 06A's Phase 6 Decision (set README,
# dated 2026-08-05), funding 06B-06D by name: the phase is priced +80 to
# +120 pmccabe against Phase 3's measured +101 control-flow weight (the
# closest comparable -- catch/throw, condition-case, signal/error and the
# static hierarchy have no predecessor), and the measured starting total
# is 751/760 across 263 symbols -- 9 points of headroom against a +80 to
# +120 phase, so the cap moves by the top of that range and funds the
# whole phase at once, the 00A/03A/04A/05A precedent. Both scc gates move
# with it (540->680 / 340->420, same estimate); the per-symbol manifest is
# unchanged and is re-banked only if 06B-06D land improvements.
# Raised 900 -> 980 by Phase 7 sub-plan 07A (2026-08-06), on the same 863/900
# baseline, funding 07B's +50..80 pmccabe estimate. This is a phase fund, not
# permission for an over-limit function or an unpriced primitive.
# Raised 980 -> 1090 inside Phase 8's reader commit (c40ca6c), which was
# wrong three ways and is corrected here. It was attributed to sub-plan 08A,
# which had said neither fe cap moves; +110 was 08C's *scc* band applied to
# pmccabe, a different unit; and the tree it funded measured 1034, leaving 56
# unearned points nothing had asked for.
# Set 1090 -> 1056 by the Phase 8 review-cycle repair (2026-08-06), which is
# the measured post-fix actual, not an estimate: kg's convention is that the
# cap sits at the number, proven by temporarily lowering it by one and
# watching the gate fire. At 1055 `make pmccabe-check` reports
# "pmccabe total complexity 1056 exceeds 1055"; at 1056 it passes. The
# per-symbol manifest under .ci/pmccabe-baseline.json is the ratchet that
# stops a symbol growing inside this envelope; this number stops the tree
# growing as a whole, and the next phase to want room prices it explicitly.
# Raised 1056 -> 1120 by Phase 9 sub-plan 09A's Decision 7 (2026-08-06),
# funding 09B and 09C by name. The measured starting total is 1056 across
# 339 symbols -- the cap sits *exactly* on the number, so the phase breaches
# it at its first point; +30..50 is the priced band and 1120 is its top plus
# the usual small margin. Proved live before the raise landed by temporarily
# setting it to 1055 and watching `make pmccabe-check` fail, then restoring.
# 09D re-sets this to the measured actual at the phase close.
# Set 1120 -> 1065 at the Phase 9 fe close (2026-08-07), the measured actual
# across 341 symbols after 09B and 09C. Proven the same way: at 1064
# `make pmccabe-check` reports "FAIL: total complexity 1065 exceeds funded
# budget 1064 (+1)"; at 1065 it passes. The phase spent 9 of the 64 funded
# points. The per-symbol manifest under .ci/pmccabe-baseline.json is unchanged
# by this commit and remains the ratchet that stops a symbol growing inside
# this envelope.
# Raised 1065 -> 1082 by the Phase 9 fe fix cycle (2026-08-07), funding the
# same four review findings the scc row above names (F1, F2, F9, F10). The
# measured starting total is 1065 across 341 symbols -- again exactly on the
# cap -- and the cycle is priced +5..12 pmccabe: one new helper apiece for F1
# and F2, one extra branch in `WriteObject` and one in `RaiseCompletionCore`,
# and asserts (which pmccabe does not count) for F9/F10. 1082 is the top of
# that band plus the usual small margin. Proved live before this raise landed
# by temporarily setting it to 1064 and watching `make pmccabe-check` report
# "FAIL: total complexity 1065 exceeds funded budget 1064 (+1)". The
# per-symbol manifest under .ci/pmccabe-baseline.json is untouched here; every
# per-symbol increase this cycle needs is banked explicitly, with its reason,
# in the commit that causes it. The final commit re-sets this number to the
# measured actual.
# Set 1082 -> 1072 at the Phase 9 fe fix cycle's close (2026-08-07), the
# measured actual across 343 symbols. Proved the same way: at 1071
# `make pmccabe-check` reports "FAIL: total complexity 1072 exceeds funded
# budget 1071 (+1)"; at 1072 it passes. The cycle spent 7 of the 17 funded
# points, across two new symbols (`FatalCollectorViolation` at 2,
# `PublishExhaustion` at 1) and four banked per-symbol increases -- fe.c's
# `CollectGarbage` 6 -> 8 (the collector re-entry guard, then F10's sweep
# assert), `WriteObject` 9 -> 10 (the step charge's `&& !collecting`), and
# fe_eval.c's `RaiseCompletionCore` 3 -> 4 (the raise guard) -- each recorded
# in `.ci/pmccabe-baseline.json` by the commit that caused it, with its reason
# in that commit's body. The manifest remains the ratchet that stops a symbol
# growing inside this envelope; this number stops the tree growing as a whole.
# Raised 1072 -> 1105 by Phase 10 sub-plan 10A's Decision 8 (2026-08-07),
# funding 10B by name and nothing else. The measured starting total is 1072
# across 343 symbols -- exactly on the cap, so the slice breaches it at its
# first point. Phase 10's fe share is priced +15..30 in both units; 1105 is
# the top of that band plus the small margin this file's raises have used
# since 03A. Proved live before this raise landed by temporarily setting it
# to 1071 and watching `make pmccabe-check` report "FAIL: total complexity
# 1072 exceeds funded budget 1071 (+1)", then restoring. The per-symbol
# manifest under .ci/pmccabe-baseline.json is untouched here; every
# per-symbol increase this slice needs is banked explicitly, with its reason,
# in the commit that causes it. The last commit of this slice re-sets this
# number to the measured actual.
# Set 1105 -> 1088 at the sub-plan 10B close (2026-08-07), the measured
# actual across 347 symbols (343 before, four new: `EnterMacroBody` at 1,
# `MacroexpandStep` at 6, `MacroexpandContinue` at 4, `DispatchMacroexpand`
# at 3 -- all inside PMCCABE_NEW_FUNCTION_MAX). Proved the same way: at 1087
# `make pmccabe-check` reports "FAIL: total complexity 1088 exceeds funded
# budget 1087 (+1)"; at 1088 it passes. The slice spent 16 of the 33 funded
# points; the other 17 go back. Three per-symbol increases were banked in the
# commits that caused them, with reasons: `ResumeMacroBody` 3 -> 5 -> 4,
# `RunEvaluationLoop` 14 -> 15, and `MacroexpandStep` 5 -> 6. The worst
# function in the tree is `RunEvaluationLoop` at 15 of the 22 per-function
# cap.
# Raised 1088 -> 1140 by Phase 11 sub-plan 11A's Decision 8 (2026-08-07),
# funding 11B and 11C by name and nothing else. The measured starting total
# is 1088 across 347 symbols -- exactly on the cap, so the phase breaches it
# at its first point. Phase 11's fe share is priced +20..35 (11B) and
# +10..25 (11C) in this unit; 1140 is 1088 plus the middle of the two bands
# summed, not the top, because the shallow-binding design reuses the cleanup
# registry's teardown rather than adding a second unwind mechanism. Proved
# live before this raise landed by temporarily setting it to 1087 and
# watching `make pmccabe-check` report "FAIL: total complexity 1088 exceeds
# funded budget 1087 (+1)", then restoring. The per-symbol manifest under
# .ci/pmccabe-baseline.json is migrated in this commit for the twelve symbols
# the translation-unit split moved from fe_eval.c to fe_run.c -- path keys
# only, every value unchanged -- and every later per-symbol increase is
# banked explicitly, with its reason, in the commit that causes it. The last
# commit of sub-plan 11C re-sets this number to the measured actual, pre-pin.
# Set 1140 -> 1120 at the sub-plan 11C close (2026-08-07), the measured
# actual across 358 symbols (347 before; eleven new, none above 5:
# `FindSpecialEntry` 3, `MarkSpecialSymbol` 4, `SymbolIsSpecial` 2,
# `SymbolIsLetDynamic` 1, `PushDynamicBinding` 1, `RestoreDynamicBinding` 1,
# `BindingsHaveDynamic` 3, `InstallLetBindings` 3, `ResumeDynamicLet` 3,
# `EmitAbbreviation` 5, `FeTryEvaluateStringWithOptions` 2 -- all inside
# PMCCABE_NEW_FUNCTION_MAX). Proved the same way: at 1119 `make
# pmccabe-check` reports "FAIL: total complexity 1120 exceeds funded budget
# 1119 (+1)"; at 1120 it passes. The phase spent 32 of the 52 funded points;
# the other 20 go back. Five per-symbol increases were banked in the commit
# that caused them, with reasons, each one an arm added to an existing
# switch or `if`: `ResumeUnary` 6 -> 8, `MarkCleanupRoots` 3 -> 4,
# `RunCleanups` 2 -> 3, `StartBindingLet` 2 -> 3, `ResumeLet` 2 -> 3; one
# decrease was banked as an improvement, `WriteObject` 10 -> 8, when the two
# writer abbreviations were folded into one helper. The worst function in
# the tree is still `RunEvaluationLoop` at 15 of the 22 per-function cap.
# Raised 1120 -> 1121 by the Phase 11 fe fix cycle (2026-08-07) for the one
# new symbol a rejected blocker's fix needs: `NextLetBinding`, complexity 1,
# the checked list advance `ResumeDynamicLet` and `InstallLetBindings` walk
# their binding list through so a value form that mutates that list cannot
# steer them off it. This is a ratchet raise and not funded headroom -- one
# point, one symbol, named -- and the fix cycle's last commit re-sets this
# number to the measured actual, pre-pin, as the Phase 11 ordering rule
# requires. Proved live before the raise landed by leaving the cap at 1120
# and watching `make pmccabe-check` report "FAIL: total complexity 1121
# exceeds funded budget 1120 (+1)". `SCC_COMPLEXITY_MAX` is *not* raised
# with it: the accessor's type check is a `CheckType` call rather than an
# `if`, so scc's keyword count is unchanged at 806 and the fix pays for
# itself in the branch it removes from the two walks.
# Re-measured at the fix cycle's close (2026-08-07), pre-pin, and left at
# 1121: that IS the measured actual, 1121 across 359 symbols, and the one
# point above 11C's 1120 is the `NextLetBinding` raise above and nothing
# else. Nothing was spent on the other two fixes -- the completion-in-flight
# fix adds locals and stores to `RaiseCompletionCore`, whose per-symbol
# complexity is 5 before and after, and the result-rooting fix deletes two
# lines from `FeTryEvaluateStringWithOptions`, whose 2 is unchanged. No
# per-symbol entry in `.ci/pmccabe-baseline.json` moved; the only diff to
# that file in the whole cycle is the one added key. Proved live at this
# head the same way: at 1120 `make pmccabe-check` reports "FAIL: total
# complexity 1121 exceeds funded budget 1120 (+1)" and at 1121 it passes;
# `PMCCABE_FUNCTION_COMPLEXITY_MAX` stays at 22 with the worst function in
# the tree still `RunEvaluationLoop` at 15, and at 14 the gate reports
# "FAIL: 1 function(s) exceed complexity limit 14".
#
# Raised 1121 -> 1155 by sub-plan 12A Decision 7 (set README, dated
# 2026-08-07), the pmccabe half of the same funding the scc total above
# carries: 12B's cleanup-handler ordering fix and `eval` primitive, and
# 12C's `file-missing` hierarchy line and one-argument-`defvar` scope
# carrier. Priced at +15..25 for 12B and +8..16 for 12C, i.e. +23..41; the
# measured starting total is 1121/1121 across 359 symbols -- zero headroom
# -- so the cap moves by 34, inside that range with the split funded
# separately if `fe_eval.c`'s per-file scc cap threatens. This is the
# authoritative core measure (the set README's rule for anything below
# `fe.c:1010`), so it is the number the slices report against; scc's total
# is the floor beside it. `PMCCABE_FUNCTION_COMPLEXITY_MAX` stays at 22 and
# `PMCCABE_NEW_FUNCTION_MAX` at 15 -- new functions this phase adds must
# arrive at or under 15, unraised. 12C re-sets this to its measured actual
# in the phase's last fe commit, pre-pin.
#
# Proved live at this head, before the raise, by temporarily lowering each
# gate and watching it fire: at 1120 `make pmccabe-check` reports "FAIL:
# total complexity 1121 exceeds funded budget 1120 (+1)" and exits 2, and
# at 1121 -- and at the 1155 below -- it passes; at
# `PMCCABE_FUNCTION_COMPLEXITY_MAX=14` it reports "FAIL: 1 function(s)
# exceed complexity limit 14" and exits 2, the one function being
# `RunEvaluationLoop` at 15.
#
# Set 1155 -> 1133 at the sub-plan 12C close (2026-08-07), pre-pin: that IS
# the measured actual, 1133 across 361 symbols, and this is the
# authoritative core measure, so it is the one that says what Phase 12's fe
# workstream cost. It spent 12 of the funded 34 (1121 -> 1133): +1 for the
# cleanup ordering fix's frame-floor test in `RaiseCompletionCore`, +3 for
# `eval`'s new `DispatchEval` arm, 0 for the `file-missing` hierarchy line,
# which is pure data, and +8 for the scope carrier (`SymbolIsLetDynamic`
# 1 -> 5, `MarkSpecialSymbol` 4 -> 5, and a new `CurrentMarkScope` at 3).
# The 22 unspent points go back. Two symbols were added over the phase
# (359 -> 361) and three per-symbol entries rose, each banked in the commit
# that caused it with the reason in its body.
# `PMCCABE_FUNCTION_COMPLEXITY_MAX` stays at 22 with the worst function in
# the tree still `RunEvaluationLoop` at 15, and `PMCCABE_NEW_FUNCTION_MAX`
# stays at 15 -- both functions the phase added arrived at 3.
# Proved live at this head by temporarily lowering each gate and watching it
# fire: at 1132 `make pmccabe-check` reports "FAIL: total complexity 1133
# exceeds funded budget 1132 (+1)" and exits 2, and at 1133 it passes; at
# `PMCCABE_FUNCTION_COMPLEXITY_MAX=14` it reports "FAIL: 1 function(s)
# exceed complexity limit 14" and exits 2.
#
# Raised 1133 -> 1155 by Phase 12's fe FIX CYCLE (2026-08-07), the sibling
# of the scc raise above and for the same reason: this is the 1155 12A
# Decision 7 already funded for Phase 12, restored for the remainder of the
# work it was funding after an acceptance review reopened the workstream.
# This is the authoritative core measure, so it is the one that will say
# what the fix cycle cost. Priced against the real tree: +4 for the blocker
# fix (`SaveInputUnit`, `RestoreInputUnit`, `EnterHostInputContext` and
# `EnterInputUnit`, four straight-line functions at 1 each -- the fix adds
# no branch anywhere, it moves five scattered save/restore pairs behind one
# named value and adds a store at the host exit) and +6..14 for the loader
# entry trio, whose only branching member is `FeReadInputForm`'s argument
# validation and reader-pushback bookkeeping. 22 points is above the top of
# that range, deliberately: it is the same funded ceiling, and re-pricing it
# downward mid-phase would only mean a third raise.
# `PMCCABE_FUNCTION_COMPLEXITY_MAX` stays at 22 and `PMCCABE_NEW_FUNCTION_MAX`
# at 15 -- every function the cycle adds must arrive at or under 15, which
# the per-symbol ratchet checks independently of this total.
# Proved live at this head, before the raise, by temporarily lowering each
# gate and watching it fire: at 1132 `make pmccabe-check` reports "FAIL:
# total complexity 1133 exceeds funded budget 1132 (+1)" and exits 2, and at
# 1133 -- and at the 1155 below -- it passes; at
# `PMCCABE_FUNCTION_COMPLEXITY_MAX=14` it reports "FAIL: 1 function(s)
# exceed complexity limit 14" and exits 2, the one function being
# `RunEvaluationLoop` at 15.
#
# Set 1155 -> 1146 at the close of Phase 12's fe FIX CYCLE (2026-08-07),
# pre-pin: that IS the measured actual, 1146 across 368 symbols, and this is
# the authoritative core measure, so it is the one that says what the cycle
# cost. It spent 13 of the funded 22 (1133 -> 1146), all of it in new
# straight-line code and none of it in an existing symbol -- not one
# per-symbol baseline entry moved in either direction over the whole cycle:
#
#   +1  fe.c:SaveInputUnit          )  the blocker fix: five scattered
#   +1  fe.c:RestoreInputUnit       )  save/restore pairs behind one named
#   +1  fe.c:EnterHostInputContext  )  value, plus the store at the host
#   +1  fe.c:EnterInputUnit         )  exit. No new branch anywhere.
#   +1  fe.c:FeEnterInputUnit       )  the loader entry trio. The branching
#   +1  fe.c:FeLeaveInputUnit       )  is all in the reader: two argument
#   +7  fe.c:FeReadInputForm        )  checks and the pushback bookkeeping.
#
# The 9 unspent points go back. Seven symbols were added (361 -> 368), each
# banked in the commit that added it, and every one arrived at or under
# `PMCCABE_NEW_FUNCTION_MAX`. That macro stays at 15 and
# `PMCCABE_FUNCTION_COMPLEXITY_MAX` stays at 22, with the worst function in
# the tree still `RunEvaluationLoop` at 15 -- unchanged by this cycle.
# Proved live at this head by temporarily lowering each gate and watching it
# fire, exit status checked: at 1145 `make pmccabe-check` reports "FAIL:
# total complexity 1146 exceeds funded budget 1145 (+1)" and exits 2, and at
# 1146 it passes; at `PMCCABE_FUNCTION_COMPLEXITY_MAX=14` it reports "FAIL: 1
# function(s) exceed complexity limit 14" and exits 2, the one function being
# `RunEvaluationLoop` at 15.
#
# Set 1213 -> 1258 at Phase 19 (2026-08-11), pre-pin: the measured actual,
# 1258 across 399 symbols, and the authoritative core measure, so this is
# the number that says what the phase cost. Eleven symbols added, none over
# `PMCCABE_NEW_FUNCTION_MAX`, and two existing entries moved:
#
#   +10  fe.c:RenderErrorMessage             Emacs' print_error_message rule
#    +9  fe.c:IsCoreSymbolName               exact minimum-arena accounting
#    +7  fe.c:SelectErrorMessage             which object the sentence starts from
#    +3  fe.c:AppendMessageText              bounded append
#    +3  fe.c:GetConditionMessageObjectCount what the seeding allocates
#    +3  fe_eval.c:ConditionInheritsFrom     "is this a file-error?"
#    +2  fe.c:SeedConditionMessages          the seeding loop
#    +2  fe.c:GetStringObjectCount           string cells for a C string
#    +2  fe_eval.c:ConditionRowAt            the table's one accessor
#    +1  fe.c:AppendMessageObject            bounded append of an object
#    +1  fe.c:FeErrorMessageString           the C entry point
#    +1  fe.c:EmitStoredString       8 -> 9  the backslash escape
#    +2  fe_eval.c:ResumeUnary       8 -> 10 the new arm
#    -1  fe_eval.c:IsConditionSymbol 4 -> 3  the quit `if` became a table row
#
# `RenderErrorMessage` arrived at 16, one over `PMCCABE_NEW_FUNCTION_MAX`,
# and `SelectErrorMessage` is the split that fixed it rather than a raise:
# choosing the message is a question with its own answer and the two halves
# read better apart. `PMCCABE_FUNCTION_COMPLEXITY_MAX` stays at 22 and
# `PMCCABE_NEW_FUNCTION_MAX` at 15; the worst function in the tree is still
# `RunEvaluationLoop` at 15 (with `ResumeEvalList` beside it, also 15),
# both unmoved by this phase. The two per-symbol
# regressions are banked with `PMCCABE_BASELINE_ARGS=--allow-regressions`,
# which is the only way to bank one, and are listed above rather than
# absorbed silently.
# Proved live at this head by temporarily lowering each gate and watching it
# fire, exit status checked: at 1257 `make pmccabe-check` reports "FAIL:
# total complexity 1258 exceeds funded budget 1257 (+1)" and exits 2, and at
# 1258 it passes; at `PMCCABE_FUNCTION_COMPLEXITY_MAX=14` it reports "FAIL: 2
# function(s) exceed complexity limit 14" and exits 2, those two being
# `RunEvaluationLoop` and `ResumeEvalList`, both at 15.
PMCCABE_TOTAL_MAX ?= 1258
COMPAT_ROOT ?= compat
COMPAT_EMACS ?=
COMPAT_ORACLE_ARGS ?=
COVERAGE_DIR ?= coverage
COVERAGE_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x -O0 -g --coverage
COVERAGE_LCOV_ARGS ?= --quiet --branch-coverage --ignore-errors inconsistent,gcov
COVERAGE_GENHTML_ARGS ?= --quiet
COVERAGE_MIN_LINES ?= 80
CLANG_FORMAT ?= clang-format
FORMAT_FILES = $(SOURCES) $(TEST_SRCS) $(EXAMPLE_SRCS) $(FUZZ_SRCS) \
	$(FUZZ_DIR)/fuzz_support.h
BEAR ?= bear
CLANG_CC ?= clang
COMPILE_DB_FILE ?= compile_commands.json
# Found on PATH; the absolute path is one developer box's layout, kept
# only as a last resort.  Override with `make IWYU=... IWYU_TOOL=...`.
IWYU_FALLBACK_DIR ?= /opt-3/iwyu-21/bin
IWYU ?= $(shell command -v include-what-you-use 2>/dev/null || \
	echo $(IWYU_FALLBACK_DIR)/include-what-you-use)
IWYU_TOOL ?= $(shell command -v iwyu_tool.py 2>/dev/null || \
	echo $(IWYU_FALLBACK_DIR)/iwyu_tool.py)
IWYU_ARGS ?= -Xiwyu --error=1
IWYU_FILES = $(addprefix $(CURDIR)/,$(SRCS))

all: $(TARGET)

check: test

test: core test-header $(TEST_API) $(EXAMPLE_HOST) $(TARGET) check-gc-stress
	./$(TEST_API)
	$(EXAMPLE_RUNNER) ./$(EXAMPLE_HOST)
	./test.sh

core: $(CORE_OBJS)

run: fe
	./fe

bench: clean
	./bench.sh

fuzz: fuzz-reader fuzz-eval fuzz-write

fuzz-reader: $(FUZZ_READER_BIN)

fuzz-eval: $(FUZZ_EVAL_BIN)

fuzz-write: $(FUZZ_WRITE_BIN)

fuzz-smoke: fuzz-eval-seed-verify fuzz-reader-smoke fuzz-eval-smoke fuzz-write-smoke

# A tracked seed under fuzz/seeds/ steers the grammar by its bytes, so any
# change to the grammar re-steers every one of them at once and nothing says
# so. Phase 9 proved that: MaxDepth 4 -> 6 plus two new BuildExpression arms
# left six of the fourteen eval seeds reaching none of the constructs they
# exist to force, silently, for a whole phase. This replays each one with
# FE_FUZZ_DUMP=1 and checks the forms it actually builds against
# fuzz/seeds/reachability.json. Runs before the smoke targets, since a seed
# that no longer steers anywhere is not something more fuzzing will reveal.
fuzz-eval-seed-verify: $(FUZZ_EVAL_BIN)
	python3 utils/verify_fuzz_seeds.py \
		--fuzzer ./$(FUZZ_EVAL_BIN) \
		--manifest $(FUZZ_DIR)/seeds/reachability.json \
		--seed-dir $(FUZZ_DIR)/seeds/eval \
		--target eval

fuzz-reader-smoke: $(FUZZ_READER_BIN)
	mkdir -p $(FUZZ_CORPUS_DIR)/reader $(FUZZ_ARTIFACT_DIR)/reader
	./$(FUZZ_READER_BIN) -runs=$(FUZZ_RUNS) -max_len=$(FUZZ_MAX_LEN) \
		-timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		-verbosity=$(FUZZ_VERBOSITY) \
		-dict=$(FUZZ_DIR)/fe.dict \
		-artifact_prefix=$(FUZZ_ARTIFACT_DIR)/reader/ \
		$(FUZZ_CORPUS_DIR)/reader scripts

fuzz-eval-smoke: $(FUZZ_EVAL_BIN)
	mkdir -p $(FUZZ_CORPUS_DIR)/eval $(FUZZ_ARTIFACT_DIR)/eval
	./$(FUZZ_EVAL_BIN) -runs=$(FUZZ_RUNS) -max_len=$(FUZZ_MAX_LEN) \
		-timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		-verbosity=$(FUZZ_VERBOSITY) \
		-artifact_prefix=$(FUZZ_ARTIFACT_DIR)/eval/ \
		$(FUZZ_CORPUS_DIR)/eval $(FUZZ_DIR)/seeds/eval scripts

fuzz-write-smoke: $(FUZZ_WRITE_BIN)
	mkdir -p $(FUZZ_CORPUS_DIR)/write $(FUZZ_ARTIFACT_DIR)/write
	./$(FUZZ_WRITE_BIN) -runs=$(FUZZ_RUNS) -max_len=$(FUZZ_MAX_LEN) \
		-timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		-verbosity=$(FUZZ_VERBOSITY) \
		-artifact_prefix=$(FUZZ_ARTIFACT_DIR)/write/ \
		$(FUZZ_CORPUS_DIR)/write

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_API): test_api.o $(FE_CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(EXAMPLE_HOST): example_host.o $(FE_CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The GC stress pair.  `fe.c`'s FE_GC_STRESS knob compiles to nothing at 0,
# so proving it does anything needs the same harness built both ways: the
# off build is the standing assertion that the collector is invoked at all
# (a collector that never runs looks like a working one to every other test
# here), the on build is the assertion that the knob really collects per
# allocation and that nothing the churn script keeps is taken when it does.
# The stress objects get their own names rather than -B or a clean, so both
# builds coexist in one tree and neither invalidates the ordinary ones.
GC_STRESS = gc_stress
GC_STRESS_ON = gc_stress_on
GC_STRESS_ON_OBJS = gc_stress-stress.o fe-stress.o fe_eval-stress.o \
	fe_run-stress.o fe_unwind-stress.o

$(GC_STRESS): gc_stress.o $(FE_CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(GC_STRESS_ON): $(GC_STRESS_ON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

gc_stress-stress.o: gc_stress.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_GC_STRESS=1 $(CFLAGS) -c gc_stress.c -o $@

fe-stress.o: fe.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_GC_STRESS=1 $(CFLAGS) -c fe.c -o $@

fe_eval-stress.o: fe_eval.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_GC_STRESS=1 $(CFLAGS) -c fe_eval.c -o $@

fe_run-stress.o: fe_run.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_GC_STRESS=1 $(CFLAGS) -c fe_run.c -o $@

fe_unwind-stress.o: fe_unwind.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_GC_STRESS=1 $(CFLAGS) -c fe_unwind.c -o $@

check-gc-stress: $(GC_STRESS) $(GC_STRESS_ON)
	./$(GC_STRESS)
	./$(GC_STRESS_ON)

test-header: test_header.c test_internal_header.c fe.h fe_internal.h
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only test_header.c
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only test_header.c
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only test_internal_header.c
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only test_internal_header.c

fe-core-gcc.o: fe.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -c fe.c -o $@

fe-core-clang.o: fe.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -c fe.c -o $@

fe-eval-core-gcc.o: fe_eval.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -c fe_eval.c -o $@

fe-eval-core-clang.o: fe_eval.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -c fe_eval.c -o $@

fe-run-core-gcc.o: fe_run.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -c fe_run.c -o $@

fe-run-core-clang.o: fe_run.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -c fe_run.c -o $@

fe-unwind-core-gcc.o: fe_unwind.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -c fe_unwind.c -o $@

fe-unwind-core-clang.o: fe_unwind.c fe.h fe_internal.h $(BUILD_STAMP)
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -c fe_unwind.c -o $@

%.o: %.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

tiny-regex-c/re.o: tiny-regex-c/re.c tiny-regex-c/re.h $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) $(RE_CFLAGS) -c $< -o $@

$(FUZZ_READER_BIN): $(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe_eval.c fe_run.c fe_unwind.c fe.h fe_internal.h \
		$(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) fe.c fe_eval.c fe_run.c fe_unwind.c $(LDLIBS)

$(FUZZ_EVAL_BIN): $(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe_eval.c fe_run.c fe_unwind.c fe.h fe_internal.h \
		$(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) fe.c fe_eval.c fe_run.c fe_unwind.c $(LDLIBS)

$(FUZZ_WRITE_BIN): $(FUZZ_DIR)/fuzz_write.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe_eval.c fe_run.c fe_unwind.c fe.h fe_internal.h \
		$(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_write.c $(FUZZ_SUPPORT) fe.c fe_eval.c fe_run.c fe_unwind.c $(LDLIBS)

sizes:
	wc *.[ch]
	wc *.md doc/*.md
	wc scripts/*.fe

clean:
	-rm -f $(BUILD_STAMP)
	-rm -rf fe $(TEST_API) $(EXAMPLE_HOST) $(GC_STRESS) $(GC_STRESS_ON) *.o *.dSYM $(FUZZ_READER_BIN) $(FUZZ_EVAL_BIN) $(FUZZ_WRITE_BIN) tiny-regex-c/*.o
	-rm -f scripts/*.csv scripts/*.times

fuzz-clean:
	rm -rf $(FUZZ_CORPUS_DIR) $(FUZZ_ARTIFACT_DIR)


complexity:
	$(SCC) --ci --by-file --sort complexity $(SCC_PATHS)

complexity-check:
	$(SCC) --ci --by-file --format json $(SCC_COMPLEXITY_PATHS) | \
		python3 utils/check_scc_complexity.py \
			--max-total $(SCC_COMPLEXITY_MAX) \
			--max-file $(SCC_FILE_COMPLEXITY_MAX)

pmccabe:
	$(PMCCABE) $(PMCCABE_PATHS) | sort -nr

pmccabe-check:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		python3 utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX) \
			--max-total $(PMCCABE_TOTAL_MAX) \
			--max-new-function $(PMCCABE_NEW_FUNCTION_MAX) \
			--baseline $(PMCCABE_BASELINE)

# Records improvements.  A rewrite that would raise an individual symbol is
# refused unless PMCCABE_BASELINE_ARGS=--allow-regressions says so
# deliberately, because "the funded envelopes still hold" is not the same
# claim as "no symbol got worse" and the second one used to be banked in
# silence.
pmccabe-baseline:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		python3 utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX) \
			--max-total $(PMCCABE_TOTAL_MAX) \
			--max-new-function $(PMCCABE_NEW_FUNCTION_MAX) \
			$(PMCCABE_BASELINE_ARGS) \
			--write-baseline $(PMCCABE_BASELINE)

coverage: coverage-clean
	$(MAKE) clean
	mkdir -p $(COVERAGE_DIR)
	$(MAKE) check CFLAGS="$(COVERAGE_CFLAGS)"
	lcov $(COVERAGE_LCOV_ARGS) --capture --directory . \
		--output-file $(COVERAGE_DIR)/run.info
	lcov $(COVERAGE_LCOV_ARGS) --extract $(COVERAGE_DIR)/run.info \
		'$(CURDIR)/*.c' --output-file $(COVERAGE_DIR)/fe.info
	genhtml $(COVERAGE_GENHTML_ARGS) $(COVERAGE_DIR)/fe.info \
		--output-directory $(COVERAGE_DIR)/html
	lcov --branch-coverage --summary $(COVERAGE_DIR)/fe.info \
		--fail-under-lines $(COVERAGE_MIN_LINES)

coverage-clean:
	rm -rf $(COVERAGE_DIR)
	find . \( -name '*.gcda' -o -name '*.gcno' \) -delete

# compat/ is the Emacs-oracle differential corpus (00b-oracle-and-differential-
# corpus.md).  `compat` never touches Emacs: it checks the manifest against
# the cases and snapshots on disk, then replays every case against $(TARGET)
# and the checked-in oracle/*.json snapshots.  `compat-oracle` is the only
# target that runs Emacs, and only it may rewrite those snapshots.
compat: $(TARGET)
	python3 utils/check_compat_manifest.py \
		--manifest $(COMPAT_ROOT)/features.json \
		--primitive-source fe.c
	python3 utils/run-fe-compat.py --fe ./$(TARGET) \
		--corpus-root $(COMPAT_ROOT)

compat-oracle:
	python3 utils/run-emacs-oracle.py $(COMPAT_ROOT) \
		--emacs '$(COMPAT_EMACS)' $(COMPAT_ORACLE_ARGS)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

compile-db:
	$(BEAR) -- $(MAKE) CC="$(CLANG_CC)" -B

iwyu:
	@test -f $(COMPILE_DB_FILE) || { \
		echo "$(COMPILE_DB_FILE) missing; run 'make compile-db' first"; \
		exit 2; \
	}
	@command -v "$(IWYU)" >/dev/null 2>&1 || { \
		echo "include-what-you-use not found (tried '$(IWYU)');" \
		     "install it, or set IWYU=/path/to/include-what-you-use" >&2; \
		exit 2; \
	}
	@command -v "$(IWYU_TOOL)" >/dev/null 2>&1 || { \
		echo "iwyu_tool.py not found (tried '$(IWYU_TOOL)');" \
		     "install it, or set IWYU_TOOL=/path/to/iwyu_tool.py" >&2; \
		exit 2; \
	}
	PATH="$$(dirname "$(IWYU)"):$${PATH}" \
		$(IWYU_TOOL) -p . $(IWYU_FILES) -- $(IWYU_ARGS)

.PHONY: all check test core test-header check-gc-stress sizes clean fuzz fuzz-reader fuzz-eval fuzz-write fuzz-smoke fuzz-clean \
	fuzz-reader-smoke fuzz-eval-smoke fuzz-write-smoke fuzz-eval-seed-verify \
	complexity complexity-check pmccabe \
	pmccabe-check pmccabe-baseline coverage coverage-clean compat compat-oracle format format-check compile-db iwyu
