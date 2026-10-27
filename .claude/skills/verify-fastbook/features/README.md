# FastBook verification map

This directory is the maintained source for verifying FastBook's user-facing behaviour. A "user" is a C++ program calling the public API. Read this index, then use the matching feature file as the recipe.

## Baseline preconditions

- Run from the repo root. `H=.claude/skills/verify-fastbook`.
- `export VFB_RUN=<task>` so evidence from one task lands in one folder.
- For `test`/`bench`: `$H/scripts/vfb build`, then `$H/scripts/vfb doctor` must print `doctor: OK`.
- Scenarios need no build; they compile straight from `include/`.
- Never drive the repo's own `build/` binaries; they may be stale or built with other flags.

## Driving conventions

- Every scenario starts from a freshly constructed book; there is no shared state between runs.
- Assert on observable results only: `AddResult`, `order.quantity` after `AddOrder` (the remainder), the trades passed to `on_trade`, `CancelOrder`'s bool, `BestBid/BestAsk/BestBidQty/BestAskQty`.
- `0` from `BestBid`/`BestAsk` means "side empty"; never use price 0 in a scenario.

## Proof and skip reporting

- Cite `$VFB_ROOT/evidence/$VFB_RUN/<name>.out` and `.exit`. The `.out` transcript shows each call, its result, trades and the top of book after it.
- A non-empty `.err` (sanitizer or assert) is a failure even when `.exit` is 0.
- If an entry point could not be exercised, report the command tried and why; do not report it as verified through another entry point.

## Feature entry contract

Each feature file has an H1, one paragraph of user-visible behaviour, then `Sub-features`, `How to get to it (user POV)`, `Driving it with vfb`, `Gotchas`.

## Features

- [Add and match](./add-match.md): resting orders, top of book, price-time priority, partial fills, multi-level sweeps.
- [Cancel](./cancel.md): head/middle/tail cancels, level removal, refused cancels, id reuse.
- [Rejections](./rejections.md): Duplicate, LevelsFull, PoolFull, QuantityOverflow, and rejection after a partial match.
- [Performance](./performance.md): latency/throughput of add, cancel and matching hot paths.
