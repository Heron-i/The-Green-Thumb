# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

Run `make` from WSL:

- `make test`: build and run every `tests/test_*.c`
- `make asan` / `make ubsan`: the same under sanitizers (separate build dirs)
- `make memcheck`: the plain build under Valgrind
- `make light_probe`: on-Pi BH1750 bring-up tool; needs real I2C, so `make test` doesn't run it

## Tests

- **One file per driver operation**, each its own binary: e.g. `tests/test_light_open.c`, `test_light_read.c`, `test_light_close.c`. The Makefile picks up any new `tests/test_*.c` automatically.
- **Case IDs** are `<prefix>-NN`, where the prefix names the driver and operation: `LSO` = Light Sensor Open, `LSR` = Read, `LSC` = Close. Follow the same pattern for new drivers (e.g. `SSO`/`SSR` for the soil sensor). Name each test function after its ID (`test_lso03_...`), list the IDs in the file's header comment, and head each case with a `/* ---- LSO-03: ... ---- */` comment.
- **Test bodies** are `void` and use `CHECK` / `CHECK_EQ` from `tests/check.h`. Each file ends with a `test_case_t` table and `int main(void) { return RUN_TESTS(tests); }`. Each case prints `[ID] name PASS/FAIL` and the file prints `N/M tests passed`. Keep names under 45 characters so the PASS/FAIL column lines up.
- **`make test` runs every binary** even after a failure, then prints `FAILED: <binaries>` and exits nonzero.
- **Hardware is faked** with `tests/fake_i2c.h`, which scripts read data and injects write/read failures.

## Project overview (from README)

CS370 (Operating Systems, Colorado State University) term project: "The Plant Owner's Capable Assistant" — a mini houseplant assistant that measures **soil moisture** and **light exposure** so a user can give their plants better care.

Course constraints stated in the README:
- Must address a real-world problem with a solution that isn't easily handled by a plain app.
- Must run on a low-cost single-board computer (Raspberry Pi) with a **minimum of two sensors** (soil moisture and light are the planned pair).

## Working in this repo

- Development happens under WSL2 on a Windows-mounted path (`/mnt/c/...`); hardware-specific code (GPIO/sensor access) can only be exercised on the Raspberry Pi itself, so keep sensor I/O separable from logic that can be tested on a dev machine once code exists.
- Default branch is `main`.
