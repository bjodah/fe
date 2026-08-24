ifeq ($(wildcard tiny-regex-c/re.c),)
$(error tiny-regex-c/re.c is missing; run 'git submodule update --init --recursive')
endif

CFLAGS ?= -Wall -Wextra -pedantic -std=c2x \
        -Wredundant-decls \
        -Wcast-align \
        -Wmissing-include-dirs \
        -Wswitch-enum \
        -Wswitch-default \
        -Winvalid-pch \
        -Wredundant-decls \
        -Wformat=2 \
        -Wmissing-format-attribute \
        -Wformat-nonliteral \
        -Wodr \
	$(CFLAGS_EXTRA)
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -Itiny-regex-c
# Phase 23.0's payload poison knob (see the publish protocol in
# fe_internal.h).  0 in every configuration but the lane that arms it, where
# every allocation slides the payload region and pattern-fills what it
# vacates.  Appended to CPPFLAGS rather than named in each rule so that the
# standalone header checks compile the armed header too, and so that
# .build-flags -- which hashes CPPFLAGS -- rebuilds when the knob moves.
FE_DEBUG_PAYLOAD_MOVE ?= 0
CPPFLAGS += -DFE_DEBUG_PAYLOAD_MOVE=$(FE_DEBUG_PAYLOAD_MOVE)
# Phase 23.1's payload TEST OBJECT knob.  A shipped interpreter has no type
# that owns a payload -- strings migrate in Phase 25 -- so the substrate's own
# tests need a build that has one.  Unlike the poison knob above it is NOT
# appended to CPPFLAGS: it is set per object, by the `%-payload.o` rules
# below, so that the ordinary binaries in the same tree never carry it.
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
SRCS = main.c auto.c fe.c fe_eval.c fe_run.c fe_unwind.c fe_perf.c fex.c \
	fex_io.c fex_math.c fex_process.c fex_re.c fex_time.c
# The evaluator's own object list, shared by every link rule that used to
# name `fe.o` alone (sub-plan 03B's fe.c -> fe.c + fe_eval.c split, sub-plan
# 11B's fe_eval.c -> fe_eval.c + fe_run.c one, and Phase 20's fe_eval.c ->
# fe_eval.c + fe_unwind.c one): a list so every consumer below stays a
# one-line change.
# `fe_perf.o` is one of them: an ordinary build compiles it to nothing (see
# fe_perf.h), and a counting build needs it wherever the instrumented core
# is linked.
FE_CORE_OBJS = fe.o fe_eval.o fe_run.o fe_unwind.o fe_perf.o
HDRS = $(wildcard *.h)
OBJS = $(SRCS:.c=.o) tiny-regex-c/re.o
SOURCES = $(SRCS) $(HDRS)
TEST_API = test_api
TEST_SRCS = test_api.c test_header.c test_internal_header.c gc_stress.c \
	payload_tests.c perf_workloads.c
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
SCC_COMPLEXITY_MAX ?= 1096
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
# WHAT THIS NUMBER IS TODAY, the rule the scc knobs above now carry too: the
# measured actual with no slack, 1504 across 487 symbols, the worst single
# function being `ResumeEvalList` at 16 against the 22 cap. Any one raise is
# derived in `git log`, with its per-symbol deltas; the blocks above are the
# record of the raises that predate that rule.
PMCCABE_TOTAL_MAX ?= 1512
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

# Every leaf below is a *run* of one already-built binary, and they are
# prerequisites rather than recipe lines because a recipe's lines are a
# sequence make may not reorder or overlap: `make -j` runs prerequisites
# concurrently and recipe lines one after another.  The suite is dominated
# by a single case -- the payload harness's GC-stress build is 23 s of a
# 24 s `make check` here -- so what the other runs cost is whether they
# overlap it or queue behind it.
test: core test-header run-test-api run-example-host run-debug-host \
	run-scripts check-gc-stress check-payload

run-test-api: $(TEST_API)
	./$(TEST_API)

run-example-host: $(EXAMPLE_HOST)
	$(EXAMPLE_RUNNER) ./$(EXAMPLE_HOST)

# The repository's own documented example, as a test (repair R1 of the Phases
# 23--26 review).  `-d` installs `main.c`'s bundled `mark`/`gc` tracers, which
# print the object they are handed -- exactly what `fe.h` promises a callback
# may do, and what `doc/c-api.md` names as THE example -- and every object a
# context reclaims goes through them: strings, vectors, and a dying symbol's
# internal cells.  It aborted for a whole phase while every other run here
# passed, because nothing here ran the binary the documentation tells a reader
# to run.  Both halves are asserted, because an exit status on its own would
# still pass if `-d` quietly stopped installing anything.  It takes
# `EXAMPLE_RUNNER` for the same reason the example host does: under valgrind
# this is the case that walks a whole arena of doomed objects.
run-debug-host: $(TARGET)
	@output=$$($(EXAMPLE_RUNNER) ./$(TARGET) -d -e '"payload"' 2>&1); \
	status=$$?; \
	if [ $$status -ne 0 ]; then \
		printf '%s\n' "$$output" | tail -n 20; \
		echo "FAIL: fe -d -e '\"payload\"' exited $$status"; \
		exit 1; \
	fi; \
	if ! printf '%s\n' "$$output" | grep -q '^gc: payload$$'; then \
		echo "FAIL: fe -d traced no collection of the string"; \
		exit 1; \
	fi; \
	echo "debug host: fe -d traced $$(printf '%s\n' "$$output" | \
		grep -c '^gc: ') collected objects"

run-scripts: $(TARGET)
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

# The three smoke runs are independent and overlap under `make -j`; the seed
# verification is not, and is a prerequisite of the eval smoke run rather than
# a sibling of it, since siblings have no order.
fuzz-smoke: fuzz-reader-smoke fuzz-eval-smoke fuzz-write-smoke

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

fuzz-eval-smoke: $(FUZZ_EVAL_BIN) fuzz-eval-seed-verify
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
		$(FUZZ_CORPUS_DIR)/write $(FUZZ_DIR)/seeds/write

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_API): test_api.o $(FE_CORE_OBJS) fex.o fex_io.o fex_re.o tiny-regex-c/re.o
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
	fe_run-stress.o fe_unwind-stress.o fe_perf-stress.o

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

# fe_perf.c reads no FE_GC_STRESS of its own, and gets a stress object anyway:
# the whole point of the -stress.o names is that the on-build's link is made
# entirely of objects that cannot be confused with the off-build's, and one
# shared object in the middle of it is how a stale mixed-flag link starts.
fe_perf-stress.o: fe_perf.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_GC_STRESS=1 $(CFLAGS) -c fe_perf.c -o $@

check-gc-stress: run-gc-stress run-gc-stress-on

run-gc-stress: $(GC_STRESS)
	./$(GC_STRESS)

run-gc-stress-on: $(GC_STRESS_ON)
	./$(GC_STRESS_ON)

# The payload substrate's harness (Phase 23.1).  Built against core objects
# that carry `FE_PAYLOAD_TEST_OBJECT=1`, because a shipped interpreter has no
# type that owns a payload and there would otherwise be nothing to allocate a
# block for.  The `-payload.o` names are the `-stress.o` names' argument: the
# link is made entirely of objects that cannot be confused with the ordinary
# ones, so both builds coexist in one tree and neither invalidates the other.
# Pattern rules rather than one rule per translation unit, since every object
# in the set differs from its ordinary twin by exactly the one flag.
# The second build is the compactor's: `FE_GC_STRESS=1` collects before every
# allocation, so every one of these cases runs its own compaction between each
# pair of allocations rather than at the handful of points an ordinary run
# would.  A compactor that left a handle stale for one allocation looks
# correct in the off build and cannot in the on one.
PAYLOAD_TESTS = payload_tests
PAYLOAD_TESTS_ON = payload_tests_stress
PAYLOAD_TESTS_OBJS = payload_tests-payload.o fe-payload.o fe_eval-payload.o \
	fe_run-payload.o fe_unwind-payload.o fe_perf-payload.o
PAYLOAD_TESTS_ON_OBJS = payload_tests-payload-stress.o fe-payload-stress.o \
	fe_eval-payload-stress.o fe_run-payload-stress.o \
	fe_unwind-payload-stress.o fe_perf-payload-stress.o

$(PAYLOAD_TESTS): $(PAYLOAD_TESTS_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PAYLOAD_TESTS_ON): $(PAYLOAD_TESTS_ON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%-payload.o: %.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_PAYLOAD_TEST_OBJECT=1 $(CFLAGS) -c $< -o $@

%-payload-stress.o: %.c $(HDRS) $(BUILD_STAMP)
	$(CC) $(CPPFLAGS) -DFE_PAYLOAD_TEST_OBJECT=1 -DFE_GC_STRESS=1 $(CFLAGS) \
		-c $< -o $@

check-payload: run-payload-tests run-payload-tests-stress

run-payload-tests: $(PAYLOAD_TESTS)
	./$(PAYLOAD_TESTS)

run-payload-tests-stress: $(PAYLOAD_TESTS_ON)
	./$(PAYLOAD_TESTS_ON)

# The counting build (Phase 21.1 of kg's elisp data-model plan).  `fe_perf.h`
# compiles to nothing unless FE_PERF_COUNTERS is 1, so proving the counters
# do anything needs the same sources built both ways -- the GC stress pair's
# argument above, at a larger scale: every core translation unit is
# instrumented here, not one knob in one file, so the whole interpreter is
# relinked rather than four objects.
#
# The objects go in their own directory instead of taking the `-stress.o`
# suffix the stress pair uses.  There are twelve of them rather than four,
# they are the whole program rather than the collector, and a directory is
# the discipline kg's own counting build (`test/perfobj/`) established: a
# counting object cannot be linked into an ordinary binary, an ordinary
# object cannot be linked into a counting one, and neither build invalidates
# the other's objects.  `tiny-regex-c/re.o` is shared rather than duplicated
# -- it carries no counter code and is compiled from RE_CFLAGS, which
# FE_PERF_COUNTERS does not appear in.
PERF_DIR ?= perfobj
PERF_CPPFLAGS = $(CPPFLAGS) -DFE_PERF_COUNTERS=1
PERF_TARGET = $(PERF_DIR)/$(PROG)
PERF_TEST_API = $(PERF_DIR)/$(TEST_API)
PERF_EXAMPLE_HOST = $(PERF_DIR)/$(EXAMPLE_HOST)
PERF_CORE_OBJS = $(addprefix $(PERF_DIR)/,$(FE_CORE_OBJS))
PERF_OBJS = $(addprefix $(PERF_DIR)/,$(SRCS:.c=.o)) tiny-regex-c/re.o
# Phase 21.2's workload battery: one binary, built from the counting objects,
# that runs each named shape in its own FeContext with the counters reset
# around it.  It lives on the test side (`perf_workloads.c` is in TEST_SRCS,
# not SRCS), so it costs the scc and pmccabe ratchets nothing and
# `format-check` covers it.  It exists only in the counting build: an
# ordinary one has no FePerfRead to call.
PERF_WORKLOADS = $(PERF_DIR)/perf_workloads
# The payload harness, counting: the one build in which fe_perf.h's payload
# counters can be asserted at all.  The shipped counting build above has no
# type that owns a payload -- deliberately, since none exists before Phase 25
# -- so every payload counter in it is zero by construction, and a counter
# nothing exercises is untested code.  Same objects as `$(PAYLOAD_TESTS)`,
# with `FE_PERF_COUNTERS=1` on top, in `$(PERF_DIR)` so a counting object
# still cannot reach an ordinary link.
PAYLOAD_TESTS_PERF = $(PERF_DIR)/payload_tests
PAYLOAD_TESTS_PERF_OBJS = $(PERF_DIR)/payload_tests-payload.o \
	$(PERF_DIR)/fe-payload.o $(PERF_DIR)/fe_eval-payload.o \
	$(PERF_DIR)/fe_run-payload.o $(PERF_DIR)/fe_unwind-payload.o \
	$(PERF_DIR)/fe_perf-payload.o
# Where the battery's machine-readable records go.  Not tracked: the numbers
# a phase argues from belong in a commit message or a checked-in report, not
# in a file a build rewrites.
PERF_WORKLOAD_JSON ?= $(PERF_DIR)/workloads.json
# The artifact line the battery writes into its JSON header: which fe tree,
# and which binary, produced the numbers.  Both are taken here, at
# measurement time, rather than compiled into `perf_workloads.c`: a describe
# baked into an object file names the tree that last triggered a rebuild.  A
# box without `git` or `sha256sum` passes an empty string, which the battery
# reports as null rather than as an answer.
PERF_DESCRIBE = git describe --always --dirty 2>/dev/null
PERF_SHA256 = sha256sum ./$(PERF_WORKLOADS) 2>/dev/null | cut -d" " -f1
# Extra arguments for the battery (`--list`).  There is no "slow" tier and
# no flag to enable one: the whole battery, 8192-symbol interning tier
# included, is 0.75 s here and 0.80 s under ASan+UBSan, against the ~12 s
# `make check` already costs, so a workload the plan names by number is not
# made optional to save a fraction of that.
PERF_WORKLOAD_ARGS ?=

perf: $(PERF_TARGET) $(PERF_TEST_API) $(PERF_EXAMPLE_HOST) $(PERF_WORKLOADS) \
	$(PAYLOAD_TESTS_PERF)

# The counting build's own `check`: the C API suite -- which is where the
# counter relationships are asserted -- the payload harness, which is where
# the payload counters are, the workload battery, the example host, and the
# whole script corpus against the counting interpreter, so every instrumented
# line is executed rather than merely compiled.  `FE_BIN` is what keeps
# test.sh from rebuilding and re-cleaning the ordinary tree underneath it.
perf-check: perf-workloads run-perf-test-api run-perf-payload \
	run-perf-example-host run-perf-scripts

run-perf-test-api: $(PERF_TEST_API)
	./$(PERF_TEST_API)

run-perf-payload: $(PAYLOAD_TESTS_PERF)
	./$(PAYLOAD_TESTS_PERF)

run-perf-example-host: $(PERF_EXAMPLE_HOST)
	$(EXAMPLE_RUNNER) ./$(PERF_EXAMPLE_HOST)

run-perf-scripts: $(PERF_TARGET)
	FE_BIN=./$(PERF_TARGET) ./test.sh

# The battery at its default sizes, which are chosen to stay inside the
# perf-check lane's budget.  Its counter assertions are the gate; the wall
# times it prints are a report and nothing reads them.
perf-workloads: $(PERF_WORKLOADS)
	$(EXAMPLE_RUNNER) ./$(PERF_WORKLOADS) --json $(PERF_WORKLOAD_JSON) \
		--git-describe "$$($(PERF_DESCRIBE))" \
		--binary-sha256 "$$($(PERF_SHA256))" \
		$(PERF_WORKLOAD_ARGS)

$(PERF_DIR):
	mkdir -p $(PERF_DIR)

$(PERF_TARGET): $(PERF_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERF_TEST_API): $(PERF_DIR)/test_api.o $(PERF_CORE_OBJS) \
		$(PERF_DIR)/fex.o $(PERF_DIR)/fex_io.o $(PERF_DIR)/fex_re.o \
		tiny-regex-c/re.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERF_EXAMPLE_HOST): $(PERF_DIR)/example_host.o $(PERF_CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERF_WORKLOADS): $(PERF_DIR)/perf_workloads.o $(PERF_CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PAYLOAD_TESTS_PERF): $(PAYLOAD_TESTS_PERF_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PERF_DIR)/%.o: %.c $(HDRS) $(BUILD_STAMP) | $(PERF_DIR)
	$(CC) $(PERF_CPPFLAGS) $(CFLAGS) -c $< -o $@

# Counting AND payload-owning.  Make prefers the shorter stem, so this rule
# wins over `%-payload.o` for a target inside $(PERF_DIR).
$(PERF_DIR)/%-payload.o: %.c $(HDRS) $(BUILD_STAMP) | $(PERF_DIR)
	$(CC) $(PERF_CPPFLAGS) -DFE_PAYLOAD_TEST_OBJECT=1 $(CFLAGS) -c $< -o $@

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
	-rm -rf fe $(TEST_API) $(EXAMPLE_HOST) $(GC_STRESS) $(GC_STRESS_ON) $(PAYLOAD_TESTS) $(PAYLOAD_TESTS_ON) *.o *.dSYM $(FUZZ_READER_BIN) $(FUZZ_EVAL_BIN) $(FUZZ_WRITE_BIN) tiny-regex-c/*.o $(PERF_DIR)
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

.PHONY: all check test core test-header check-gc-stress check-payload \
	run-test-api run-example-host run-debug-host run-scripts \
	run-gc-stress run-gc-stress-on \
	run-payload-tests run-payload-tests-stress \
	run-perf-test-api run-perf-payload run-perf-example-host run-perf-scripts \
	perf perf-check \
	perf-workloads sizes clean fuzz fuzz-reader fuzz-eval fuzz-write fuzz-smoke fuzz-clean \
	fuzz-reader-smoke fuzz-eval-smoke fuzz-write-smoke fuzz-eval-seed-verify \
	complexity complexity-check pmccabe \
	pmccabe-check pmccabe-baseline coverage coverage-clean compat compat-oracle format format-check compile-db iwyu
