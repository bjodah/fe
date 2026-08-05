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
SRCS = main.c auto.c fe.c fe_eval.c fex.c fex_io.c fex_math.c fex_process.c \
	fex_re.c fex_time.c
# The evaluator's own object list, shared by every link rule that used to
# name `fe.o` alone (sub-plan 03B's fe.c -> fe.c + fe_eval.c split): a list
# so every consumer below stays a one-line change.
FE_CORE_OBJS = fe.o fe_eval.o
HDRS = $(wildcard *.h)
OBJS = $(SRCS:.c=.o) tiny-regex-c/re.o
SOURCES = $(SRCS) $(HDRS)
TEST_API = test_api
TEST_SRCS = test_api.c test_header.c test_internal_header.c
EXAMPLE_HOST = example_host
EXAMPLE_SRCS = example_host.c
EXAMPLE_RUNNER ?=

# Standalone core checks use flags supported by both kg's C23 compilers.
CORE_GCC ?= gcc
CORE_CLANG ?= clang
CORE_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x
CORE_OBJS = fe-core-gcc.o fe-core-clang.o fe-eval-core-gcc.o \
	fe-eval-core-clang.o

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
SCC_COMPLEXITY_MAX ?= 480
SCC_FILE_COMPLEXITY_MAX ?= 300
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
PMCCABE_TOTAL_MAX ?= 690
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

test: core test-header $(TEST_API) $(EXAMPLE_HOST) $(TARGET)
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

fuzz-smoke: fuzz-reader-smoke fuzz-eval-smoke fuzz-write-smoke

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

%.o: %.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

tiny-regex-c/re.o: tiny-regex-c/re.c tiny-regex-c/re.h $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) $(RE_CFLAGS) -c $< -o $@

$(FUZZ_READER_BIN): $(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe_eval.c fe.h fe_internal.h \
		$(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) fe.c fe_eval.c $(LDLIBS)

$(FUZZ_EVAL_BIN): $(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe_eval.c fe.h fe_internal.h \
		$(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) fe.c fe_eval.c $(LDLIBS)

$(FUZZ_WRITE_BIN): $(FUZZ_DIR)/fuzz_write.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe_eval.c fe.h fe_internal.h \
		$(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_write.c $(FUZZ_SUPPORT) fe.c fe_eval.c $(LDLIBS)

sizes:
	wc *.[ch]
	wc *.md doc/*.md
	wc scripts/*.fe

clean:
	-rm -f $(BUILD_STAMP)
	-rm -rf fe $(TEST_API) $(EXAMPLE_HOST) *.o *.dSYM $(FUZZ_READER_BIN) $(FUZZ_EVAL_BIN) $(FUZZ_WRITE_BIN) tiny-regex-c/*.o
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

pmccabe-baseline:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		python3 utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX) \
			--max-total $(PMCCABE_TOTAL_MAX) \
			--max-new-function $(PMCCABE_NEW_FUNCTION_MAX) \
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
	find . -maxdepth 1 \( -name '*.gcda' -o -name '*.gcno' \) -delete

# compat/ is the Emacs-oracle differential corpus (00b-oracle-and-differential-
# corpus.md).  `compat` never touches Emacs: it checks the manifest against
# the cases and snapshots on disk, then replays every case against $(TARGET)
# and the checked-in oracle/*.json snapshots.  `compat-oracle` is the only
# target that runs Emacs, and only it may rewrite those snapshots.
compat: $(TARGET)
	python3 utils/check_compat_manifest.py \
		--manifest $(COMPAT_ROOT)/features.json
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

.PHONY: all check test core test-header sizes clean fuzz fuzz-reader fuzz-eval fuzz-write fuzz-smoke fuzz-clean \
	fuzz-reader-smoke fuzz-eval-smoke fuzz-write-smoke complexity complexity-check pmccabe \
	pmccabe-check pmccabe-baseline coverage coverage-clean compat compat-oracle format format-check compile-db iwyu
