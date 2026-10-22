# make predefines CC=cc, so `?=` never fires; only replace make's default.
ifeq ($(origin CC),default)
CC := gcc
endif
BUILD ?= build
BASE_CFLAGS := -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -g -O1 -Iinclude -Itests -pthread
CFLAGS := $(BASE_CFLAGS) $(EXTRA_CFLAGS)

# Library sources are everything in src/ (process main()s should live in
# src/<name>_main.c and be listed in MAIN_SRC so tests don't link them).
MAIN_SRC := $(wildcard src/*_main.c)
LIB_SRC := $(filter-out $(MAIN_SRC),$(wildcard src/*.c))
HEADERS := $(wildcard include/*.h tests/*.h)

# Every tests/test_*.c becomes $(BUILD)/test_* linked against all of LIB_SRC.
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

VALGRIND := valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1

all: $(TEST_BINS) $(BUILD)/light_probe

$(BUILD)/%: tests/%.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

# On-Pi BH1750 bring-up check (see src/light_probe_main.c). Not run by
# `make test`: it needs real I2C hardware.
$(BUILD)/light_probe: src/light_probe_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

light_probe: $(BUILD)/light_probe

test: $(TEST_BINS)
	@for b in $(TEST_BINS); do echo "== $$b"; ./$$b || exit 1; done

# Sanitizers get their own build dirs so they never reuse plain objects.
asan:
	$(MAKE) test BUILD=build/asan \
	  EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

ubsan:
	$(MAKE) test BUILD=build/ubsan \
	  EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined"

# valgrind and ASan don't mix, so memcheck runs the plain build.
memcheck: $(TEST_BINS)
	@for b in $(TEST_BINS); do echo "== valgrind $$b"; $(VALGRIND) ./$$b || exit 1; done

clean:
	rm -rf build

.PHONY: all test asan ubsan memcheck clean light_probe
