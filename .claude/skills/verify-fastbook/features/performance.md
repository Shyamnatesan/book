# Performance

FastBook exists to be fast on the hot path: resting adds, cancels, top-of-book matches and multi-level sweeps, with no allocation after construction. The `book_bench` Google Benchmark binary measures each path over ≥ 1,000,000 operations per benchmark.

## Sub-features

- `perf-resting-add` `BM_RestingAdd`: adds that rest without matching.
- `perf-cancel` `BM_Cancel`: cancels of resting orders.
- `perf-match-top` `BM_Match_TopOfBook`: takers that fill at the best level.
- `perf-match-sweep` `BM_Match_Sweep`: takers that walk several levels.

## How to get to it (user POV)

- `book_bench` built in Release (`-O3 -march=native`), run with an optional `--benchmark_filter`.

## Driving it with vfb

Preconditions: `$H/scripts/vfb build` then `$H/scripts/vfb doctor` prints `doctor: OK` with `build type: Release`.

- **One path.** Run `$H/scripts/vfb bench BM_Cancel`. Exit `0`; `bench.out` has a `BM_Cancel/iterations:1024/manual_time` row with `ops_total` ≥ 1M (≈ 2.2M at the time of writing); `bench.json` saved.
- **All paths.** Run `$H/scripts/vfb bench` (no filter).
- **Before/after.** Use two `VFB_RUN` names (e.g. `perf-before`, `perf-after`), same filter, compare `real_time` per benchmark in the two `bench.json` files.

## Gotchas

- Read the `Time` (manual time) column, not `CPU`; CPU includes untimed setup phases inside each iteration.
- `***WARNING*** CPU scaling is enabled` means results are noisy; compare medians of several runs, or report the warning alongside numbers.
- A Debug or stale build produces meaningless numbers; doctor catches both.
