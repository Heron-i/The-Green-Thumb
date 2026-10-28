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

all: $(TEST_BINS) $(BUILD)/light_probe $(BUILD)/soil_probe $(BUILD)/soil_sensor $(BUILD)/hub_listen $(BUILD)/hub_main $(BUILD)/storage_main $(BUILD)/storage_feed $(BUILD)/log_verify
all: $(TEST_BINS) $(BUILD)/light_probe $(BUILD)/light_sensor $(BUILD)/hub_listen $(BUILD)/hub_main $(BUILD)/storage_main $(BUILD)/storage_feed $(BUILD)/log_verify

$(BUILD)/%: tests/%.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

# On-Pi BH1750 bring-up check (see src/light_probe_main.c). Not run by
# `make test`: it needs real I2C hardware.
$(BUILD)/light_probe: src/light_probe_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

light_probe: $(BUILD)/light_probe

# On-Pi STEMMA seesaw bring-up check (see src/soil_probe_main.c). Not run by
# `make test`: it needs real I2C hardware.
$(BUILD)/soil_probe: src/soil_probe_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

soil_probe: $(BUILD)/soil_probe

# The soil sensor process (see src/soil_sensor_main.c): reads the seesaw and
# sends to hub.sock. Not run by `make test`: it runs until SIGTERM.
$(BUILD)/soil_sensor: src/soil_sensor_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

soil_sensor: $(BUILD)/soil_sensor
# The light sensor process (see src/light_sensor_main.c): BH1750 readings
# to hub.sock. Not run by `make test`: it runs until SIGTERM, and reads
# real I2C (without it, it logs i2c_open_error and retries every 5 s).
$(BUILD)/light_sensor: src/light_sensor_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

light_sensor: $(BUILD)/light_sensor

# On-Pi stand-in for the hub on hub.sock (see src/hub_listen_main.c). Not
# run by `make test`: it serves until killed.
$(BUILD)/hub_listen: src/hub_listen_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

hub_listen: $(BUILD)/hub_listen

# The hub process (see src/hub_main.c): sensors on hub.sock, records to
# storage.sock. Not run by `make test`: it serves until SIGTERM.
$(BUILD)/hub_main: src/hub_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

hub_main: $(BUILD)/hub_main

# The storage process (see src/storage_main.c). Not run by `make test`: it
# serves until SIGTERM.
$(BUILD)/storage_main: src/storage_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

storage_main: $(BUILD)/storage_main

# Load generator standing in for the hub on storage.sock (see
# src/storage_feed_main.c). Not run by `make test`: it needs storage_main.
$(BUILD)/storage_feed: src/storage_feed_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

storage_feed: $(BUILD)/storage_feed

# Offline log verifier (see src/log_verify_main.c), used after every fault
# test. Not run by `make test`: it needs a real log directory.
$(BUILD)/log_verify: src/log_verify_main.c $(LIB_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LIB_SRC) $< -o $@

log_verify: $(BUILD)/log_verify

# Every test binary runs even if an earlier one fails; failures are listed
# at the end and the target still exits nonzero for CI.
test: $(TEST_BINS)
	@failed=""; for b in $(TEST_BINS); do echo "== $$b"; ./$$b || failed="$$failed $$b"; done; \
	if [ -n "$$failed" ]; then echo "FAILED:$$failed"; exit 1; fi

# Sanitizers get their own build dirs so they never reuse plain objects.
asan:
	$(MAKE) test BUILD=build/asan \
	  EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

ubsan:
	$(MAKE) test BUILD=build/ubsan \
	  EXTRA_CFLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined"

# valgrind and ASan don't mix, so memcheck runs the plain build.
memcheck: $(TEST_BINS)
	@failed=""; for b in $(TEST_BINS); do echo "== valgrind $$b"; $(VALGRIND) ./$$b || failed="$$failed $$b"; done; \
	if [ -n "$$failed" ]; then echo "FAILED:$$failed"; exit 1; fi

clean:
	rm -rf build

.PHONY: all test asan ubsan memcheck clean light_probe soil_probe soil_sensor hub_listen hub_main storage_main storage_feed log_verify
.PHONY: all test asan ubsan memcheck clean light_probe light_sensor hub_listen hub_main storage_main storage_feed log_verify
