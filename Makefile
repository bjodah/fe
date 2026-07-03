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

# Project metrics
SCC ?= scc
SCC_PATHS ?= $(SOURCES)
SCC_COMPLEXITY_PATHS ?= $(SOURCES)
SCC_COMPLEXITY_MAX ?= 147
SCC_FILE_COMPLEXITY_MAX ?= 75
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= $(SRCS)
PMCCABE_FUNCTION_COMPLEXITY_MAX ?= 22
COVERAGE_DIR ?= coverage
COVERAGE_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x -O0 -g --coverage
COVERAGE_LCOV_ARGS ?= --quiet --branch-coverage --ignore-errors inconsistent,gcov
COVERAGE_GENHTML_ARGS ?= --quiet
COVERAGE_MIN_LINES ?= 80
CLANG_FORMAT ?= clang-format
FORMAT_FILES = $(SOURCES)
BEAR ?= bear
CLANG_CC ?= clang
COMPILE_DB_FILE ?= compile_commands.json
IWYU ?= /opt-3/iwyu-21/bin/include-what-you-use
IWYU_TOOL ?= /opt-3/iwyu-21/bin/iwyu_tool.py
IWYU_ARGS ?= -Xiwyu --error=1
IWYU_FILES = $(addprefix $(CURDIR)/,$(SRCS))

all: $(TARGET)

check: test

test:
	./test.sh

run: fe
	./fe

bench: clean
	./bench.sh

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

sizes:
	wc *.[ch]
	wc *.md doc/*.md
	wc scripts/*.fe

clean:
	-rm -rf fe *.o *.dSYM
	-rm -f scripts/*.csv scripts/*.times


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

.PHONY: all check test sizes clean complexity complexity-check pmccabe pmccabe-check \
	coverage coverage-clean format format-check compile-db iwyu
