# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Green-Thumb: project rules

## Commands

Run `make` from WSL:

- Build & Unit tests: `make test`: build and run every `tests/test_*.c`
- Sanitiziers: `make asan` Valgrind: `make memcheck`
- A change is DONE only when all three pass. Always run them; show output.
- `make light_probe`: on-Pi BH1750 bring-up tool; needs real I2C, so `make test` doesn't run it without established connection.

## Tests

- **One file per driver operation**, each its own binary and the Makefile picks up any new `tests/test_*.c` automatically.
- **Case IDs** are `<prefix>-NN`, where the prefix names the driver and operation. Follow the same pattern for new drivers and name each test function after its ID (`test_lso03_...`), list the IDs in the file's header comment, and head each case with a `/* ---- LSO-03: ... ---- */` comment.
- **Test bodies** are `void` and use `CHECK` / `CHECK_EQ` from `tests/check.h`. Each file ends with a `test_case_t` table and `int main(void) { return RUN_TESTS(tests); }`. Each case prints `[ID] name PASS/FAIL` and the file prints `N/M tests passed`. Keep names under 45 characters so the PASS/FAIL column lines up.
- **`make test` runs every binary** even after a failure, then prints `FAILED: <binaries>` and exits nonzero.
- **Hardware is faked** with `tests/fake_i2c.h`, which scripts read data and injects write/read failures.

## Hard Constraints 
- NEVER weaken, skip, or delete a test to make the suite pass. If a test looks wrong, stop and explain why instead.
- Do not modify or implement code without explicit approval.

## Style
- -Wall -Wextra -Werror must stay clean. No VLAs.
- Prefer the smallest diff that passes. Do not refactor unrelated code.
- Every non-obvious loop gets a one-line invariant comment.

## Workflow
- For any multi-file or algorithmic change: propose a plan and wait for approval before editing.
- build tests first always, then implementation.
- hardware-specific code (GPIO/sensor access) can only be exercised on the Raspberry Pi itself, so keep sensor I/O separable from logic that can be tested on a dev machine once code exists.


