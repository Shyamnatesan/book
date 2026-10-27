---
name: verify-fastbook
description: Drive the FastBook order-book library (header-only C++23, include/fastbook.hpp) the way a consumer does — compile a program against the public API, run the test suite and benchmarks from an isolated build — and capture evidence. Use to prove a change to AddOrder/CancelOrder/matching/rejections/top-of-book or to performance actually works, before claiming it does.
---

# Verify FastBook

FastBook is a header-only library: there is no server or UI. Its users are C++ programs that include `fastbook.hpp`, build a `FastBook<Allocator, MaxLevels>(max_orders, alloc)`, call `AddOrder(order, on_trade)` / `CancelOrder(id)`, and read `BestBid/BestAsk/BestBidQty/BestAskQty`. "Driving it like a user" means compiling such a program against `include/` and observing what those calls return. The repo's other entry points are the `book_test` suite, the `book_bench` benchmarks, and an empty `book_demo`.

Everything goes through one helper: `.claude/skills/verify-fastbook/scripts/vfb` (run from the repo root). It never writes to the repo's own `build/`; its state lives under `$VFB_ROOT` (default `${TMPDIR:-/tmp}/verify-fastbook`).

For the exhaustive state-space proof of matching/cancel/rejection correctness, use the sibling skill `verify-fastbook-matching`. This skill proves the specific behaviour you changed, end to end, with a readable transcript.

## Launch

```bash
export VFB_RUN=<short-task-name>     # groups evidence; default is a timestamp
.claude/skills/verify-fastbook/scripts/vfb build
```

Configures a Release build in `$VFB_ROOT/build` and builds `book_test`, `invariant_test`, `book_bench`, `book_demo`. Ready when it prints `[vfb] built: ...`. It reuses `build/_deps/benchmark-src` (google/benchmark v1.9.1) when present, so no network is needed; without it CMake's FetchContent clones from GitHub. Logs: `$VFB_ROOT/configure.log`, `$VFB_ROOT/build.log`. Re-run `build` after any edit to `include/`, `tests/`, `bench/`, `src/` or `CMakeLists.txt`. Scenarios (below) compile directly from `include/` and need no build step.

## Doctor

```bash
.claude/skills/verify-fastbook/scripts/vfb doctor
```

Read-only. Confirms g++ exists, the build dir was configured from *this* repo, the build type, and that each binary is newer than every source file. Prints `doctor: OK` or `doctor: NOT OK` with the reason (`STALE ... (run: vfb build)`). Run it first whenever a result looks surprising.

## Drive

- **Consumer scenario** (primary): `vfb scenario <file.cpp>`. Compiles a program against `include/` with asserts, ASan and UBSan on, runs it, and records the transcript. Ready-made scenarios live in `.claude/skills/verify-fastbook/scenarios/` (one per feature, see `features/`). For a new behaviour, copy the closest one to `$VFB_ROOT/scenarios/` (not into the repo) and edit it; `vfb scenario` puts both `include/` and the scenarios directory on the include path, so `#include "scenario.hpp"` works from anywhere. `Session<MaxLevels>` logs every `AddOrder`/`CancelOrder` call, its result, the remainder, each trade and the resulting top of book; `EXPECT(cond)` prints `ok`/`FAIL`; `Finish(name)` sets the exit code.
- **Test suites**: `vfb test` runs `book_test` (behaviour) and `invariant_test` (the six book invariants after every operation, plus `CheckInvariants()`). Each prints `N/N tests passed` and exits non-zero on any failure.
- **Benchmarks**: `vfb bench [regex]`, e.g. `vfb bench BM_CancelDeep`. Saves `bench.json`. Read the `ns_per_op` counter; `ops_total` must be ≥ 1,000,000 (see the header comment of `bench/book_bench.cpp`). For a before/after claim use `bench/compare.py OLD_BIN NEW_BIN --runs 7`, which interleaves the two binaries on one pinned core and only calls a difference when the ranges do not overlap.

## Evidence

Each command writes `$VFB_ROOT/evidence/$VFB_RUN/<name>.{cmd,out,err,exit}` (plus `bench.json`, and `<scenario>.compile.*`). A proof consists of:

- the scenario transcript showing the **calls made and the resulting state** (trades, remainder, top of book) — not just `PASSED`;
- `exit` = 0 for the scenario, and an empty `.err` (a sanitizer report or `Assertion ... failed` lands there);
- for changes touching existing behaviour, `vfb test` evidence as well;
- for performance claims, `bench.json` from before *and* after the change, same filter, same machine, `doctor: OK` both times.

Proof standards: exercise behaviour only through the public API (`AddOrder`, `CancelOrder`, `Best*`), never by poking private fields; assert on the trade sequence and top of book, not only on return codes; a scenario that was edited until it passed must still state the behaviour price-time priority demands, not what the code happened to do.

## Cleanup

```bash
.claude/skills/verify-fastbook/scripts/vfb cleanup
```

Removes `$VFB_ROOT/build`, `$VFB_ROOT/bin` and the logs. Evidence under `$VFB_ROOT/evidence/` is kept; cite its path in your report. Nothing long-running is started, so there are no processes to kill.

## Isolation

Two runs can share `$VFB_ROOT/build` only sequentially (CMake builds are not concurrent-safe). For parallel agents, give each its own `VFB_ROOT=$TMPDIR/verify-fastbook-<agent>`.

## Feature map

See [`features/README.md`](features/README.md). A proof that drives one entry point is incomplete if the map lists others for the same feature.
