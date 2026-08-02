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
SRCS = main.c auto.c fe.c fex.c fex_io.c fex_math.c fex_process.c fex_re.c \
	fex_time.c
HDRS = $(wildcard *.h)
OBJS = $(SRCS:.c=.o) tiny-regex-c/re.o
SOURCES = $(SRCS) $(HDRS)
TEST_API = test_api
TEST_SRCS = test_api.c test_header.c
EXAMPLE_HOST = example_host
EXAMPLE_SRCS = example_host.c
EXAMPLE_RUNNER ?=

# Standalone core checks use flags supported by both kg's C23 compilers.
CORE_GCC ?= gcc
CORE_CLANG ?= clang
CORE_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x
CORE_OBJS = fe-core-gcc.o fe-core-clang.o

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
SCC_COMPLEXITY_MAX ?= 210
SCC_FILE_COMPLEXITY_MAX ?= 105
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(SRCS)
PMCCABE_FUNCTION_COMPLEXITY_MAX ?= 22
# The scc total above is a floor, not a measurement (its string-state
# machine desynchronizes on fe.c's '"' character literals and stops
# counting keywords below them).  This manifest is the honest gate:
# pmccabe reads every function, its complexity is recorded per symbol, no
# symbol may exceed its entry, and a function with no entry is new and has
# to arrive at or under PMCCABE_NEW_FUNCTION_MAX.  `make pmccabe-baseline`
# is the only thing that rewrites it.
PMCCABE_BASELINE ?= .ci/pmccabe-baseline.json
PMCCABE_NEW_FUNCTION_MAX ?= 15
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
		$(FUZZ_CORPUS_DIR)/eval scripts

fuzz-write-smoke: $(FUZZ_WRITE_BIN)
	mkdir -p $(FUZZ_CORPUS_DIR)/write $(FUZZ_ARTIFACT_DIR)/write
	./$(FUZZ_WRITE_BIN) -runs=$(FUZZ_RUNS) -max_len=$(FUZZ_MAX_LEN) \
		-timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		-verbosity=$(FUZZ_VERBOSITY) \
		-artifact_prefix=$(FUZZ_ARTIFACT_DIR)/write/ \
		$(FUZZ_CORPUS_DIR)/write

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_API): test_api.o fe.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(EXAMPLE_HOST): example_host.o fe.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-header: test_header.c fe.h
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only test_header.c
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only test_header.c

fe-core-gcc.o: fe.c fe.h $(BUILD_STAMP)
	$(CORE_GCC) $(CPPFLAGS) $(CORE_CFLAGS) -c fe.c -o $@

fe-core-clang.o: fe.c fe.h $(BUILD_STAMP)
	$(CORE_CLANG) $(CPPFLAGS) $(CORE_CFLAGS) -c fe.c -o $@

%.o: %.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

tiny-regex-c/re.o: tiny-regex-c/re.c tiny-regex-c/re.h $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) $(RE_CFLAGS) -c $< -o $@

$(FUZZ_READER_BIN): $(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe.h $(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) fe.c $(LDLIBS)

$(FUZZ_EVAL_BIN): $(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe.h $(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) fe.c $(LDLIBS)

$(FUZZ_WRITE_BIN): $(FUZZ_DIR)/fuzz_write.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe.h $(BUILD_STAMP)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_write.c $(FUZZ_SUPPORT) fe.c $(LDLIBS)

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
			--max-new-function $(PMCCABE_NEW_FUNCTION_MAX) \
			--baseline $(PMCCABE_BASELINE)

pmccabe-baseline:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		python3 utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX) \
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
	pmccabe-check pmccabe-baseline coverage coverage-clean format format-check compile-db iwyu
