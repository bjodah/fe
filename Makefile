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
	-Wno-extra-semi-stmt
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
LDLIBS ?= -lm

PROG = fe
TARGET = $(PROG)
SRCS = main.c auto.c fe.c fex.c fex_io.c fex_math.c fex_process.c fex_re.c \
	fex_time.c
HDRS = $(wildcard *.h)
OBJS = $(SRCS:.c=.o)
SOURCES = $(SRCS) $(HDRS)
TEST_API = test_api
TEST_SRCS = test_api.c test_header.c

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
FUZZ_SRCS = $(FUZZ_SUPPORT) $(FUZZ_DIR)/fuzz_reader.c \
	$(FUZZ_DIR)/fuzz_eval.c

# Project metrics
SCC ?= scc
SCC_PATHS ?= $(SOURCES) $(FUZZ_SRCS)
SCC_COMPLEXITY_PATHS ?= $(SOURCES)
SCC_COMPLEXITY_MAX ?= 172
SCC_FILE_COMPLEXITY_MAX ?= 98
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(SRCS)
PMCCABE_FUNCTION_COMPLEXITY_MAX ?= 22
COVERAGE_DIR ?= coverage
COVERAGE_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x -O0 -g --coverage
COVERAGE_LCOV_ARGS ?= --quiet --branch-coverage --ignore-errors inconsistent,gcov
COVERAGE_GENHTML_ARGS ?= --quiet
COVERAGE_MIN_LINES ?= 80
CLANG_FORMAT ?= clang-format
FORMAT_FILES = $(SOURCES) $(TEST_SRCS) $(FUZZ_SRCS) $(FUZZ_DIR)/fuzz_support.h
BEAR ?= bear
CLANG_CC ?= clang
COMPILE_DB_FILE ?= compile_commands.json
IWYU ?= /opt-3/iwyu-21/bin/include-what-you-use
IWYU_TOOL ?= /opt-3/iwyu-21/bin/iwyu_tool.py
IWYU_ARGS ?= -Xiwyu --error=1
IWYU_FILES = $(addprefix $(CURDIR)/,$(SRCS))

all: $(TARGET)

check: test

test: test-header $(TEST_API) $(TARGET)
	./$(TEST_API)
	./test.sh

run: fe
	./fe

bench: clean
	./bench.sh

fuzz: fuzz-reader fuzz-eval

fuzz-reader: $(FUZZ_READER_BIN)

fuzz-eval: $(FUZZ_EVAL_BIN)

fuzz-smoke: fuzz-reader-smoke fuzz-eval-smoke

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

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_API): test_api.o fe.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-header: test_header.c fe.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only test_header.c

%.o: %.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(FUZZ_READER_BIN): $(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe.h
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_reader.c $(FUZZ_SUPPORT) fe.c $(LDLIBS)

$(FUZZ_EVAL_BIN): $(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) \
		$(FUZZ_DIR)/fuzz_support.h fe.c fe.h
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) -I. -o $@ \
		$(FUZZ_DIR)/fuzz_eval.c $(FUZZ_SUPPORT) fe.c $(LDLIBS)

sizes:
	wc *.[ch]
	wc *.md doc/*.md
	wc scripts/*.fe

clean:
	-rm -rf fe $(TEST_API) *.o *.dSYM $(FUZZ_READER_BIN) $(FUZZ_EVAL_BIN)
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
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX)

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
	PATH="$$(dirname "$(IWYU)"):$${PATH}" \
		$(IWYU_TOOL) -p . $(IWYU_FILES) -- $(IWYU_ARGS)

.PHONY: all check test test-header sizes clean fuzz fuzz-reader fuzz-eval fuzz-smoke fuzz-clean \
	fuzz-reader-smoke fuzz-eval-smoke complexity complexity-check pmccabe \
	pmccabe-check coverage coverage-clean format format-check compile-db iwyu
