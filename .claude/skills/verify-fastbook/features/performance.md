# Performance

FastBook exists to be fast on the hot path, with no allocation after construction. `book_bench` measures ten named paths plus one integration hazard, each over at least 1,000,000 operations, on a background book of 100 levels per side with 10 orders each.

## Sub-features

- `perf-add-at-best` `BM_AddAtBest`: join the best level.
- `perf-add-new-best` `BM_AddNewBest`: each add opens a new best level.
- `perf-add-deep` `BM_AddDeep`: join a level 75 to 99 levels from the best.
- `perf-cancel-best` / `perf-cancel-middle` / `perf-cancel-deep` `BM_CancelBest`, `BM_CancelMiddle`, `BM_CancelDeep`: cancel at depth 0, 50 and 99.
- `perf-match-top` `BM_MatchTopOfBook`: each taker fully fills exactly one maker at the best price.
- `perf-sweep` `BM_Sweep10Levels`: each taker consumes exactly 10 levels (ns per sweep).
- `perf-random-cancel` `BM_CancelRandomId`: random 32-bit ids at random depths, 1 in 16 cancels misses.
- `perf-clustered` `BM_ClusteredPrices`: mixed adds, takers and cancels within 5 ticks of mid.
- `perf-offset-ids` `BM_CancelOffsetIds`: sequential ids from a second range next to the background's; guards against hash clustering.

## How to get to it (user POV)

- `book_bench` built in Release (`-O3 -march=native`), with an optional `--benchmark_filter`.
- `bench/compare.py OLD NEW` for any before/after claim.

## Driving it with vfb

Preconditions: `$H/scripts/vfb build`, then `$H/scripts/vfb doctor` prints `doctor: OK` with `build type: Release`.

- **One path.** Run `$H/scripts/vfb bench BM_CancelDeep`. Exit `0`; `bench.out` has a `BM_CancelDeep/iterations:1024/manual_time` row with `ns_per_op` and `ops_total` >= 1M; `bench.json` saved.
- **All paths.** Run `$H/scripts/vfb bench` (no filter). No row may show `ERROR`: each benchmark checks its trade and cancel counts and that the book returned to the background.
- **Before/after.** Build both binaries, then `python3 bench/compare.py OLD NEW --runs 7 --json out.json`. Report medians and ranges; `no measurable difference` is the verdict when ranges overlap.
- **A full attempt.** `bench/attempt.sh KEPT_BIN 7` gates the working tree (Release ctest, ASan/UBSan tests, the exhaustive model check) and then runs the A/B.

## Gotchas

- `ns_per_op` is manual time over the timed batch only. The CPU column includes the untimed seed and restore phases.
- `***WARNING*** CPU scaling is enabled` means the governor is `schedutil`. Interleaved A/B with medians is required; a single run can be 15% off.
- A Debug or stale build produces meaningless numbers; doctor catches both.
- Uncommitted edits to `bench/book_bench.cpp` change the harness itself; rebuild both sides from the same bench source before comparing.
