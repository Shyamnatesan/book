# Cancel

A consumer removes a resting order with `CancelOrder(id)`. It returns `true` and the order's quantity leaves its level (the level disappears when it was the last order); it returns `false` for an id that is not resting — never added, already cancelled, or fully filled. A cancelled id may be used again.

## Sub-features

- `cancel-position` cancelling the head, middle or tail of a level keeps the others in FIFO order.
- `cancel-aggregate` the level's total quantity drops by the cancelled quantity.
- `cancel-level-removal` cancelling the last order at the best price moves the top of book to the next level.
- `cancel-refused` unknown, repeated, and filled ids return `false` and change nothing.
- `cancel-id-reuse` a cancelled id can be added again.

## How to get to it (user POV)

- `book.CancelOrder(id)` from a consumer program.
- Indirectly through `book_test` (`CancelHeadMiddleTail`, `CancelUnknownAndRepeated`, `IdReuseAndLevelRemoval`).

## Driving it with vfb

Preconditions: none.

- **Scenario.** Run `$H/scripts/vfb scenario $H/scenarios/cancel.cpp`. Exit `0`, last line `cancel: PASSED (0 failed)`, empty `scenario-cancel.err`.
- **FIFO after middle cancel.** In `scenario-cancel.out`, after `CancelOrder(11) -> true` the top reads `ask 50 x 5`, and `AddOrder(Buy id=20 price=50 qty=2)` trades `maker=10` then `maker=12` (11 is skipped).
- **Level removal.** `CancelOrder(12) -> true` moves the top to `ask 51 x 8`.
- **Refused.** `CancelOrder(999)`, `CancelOrder(12)` (again) and `CancelOrder(10)` (filled) all print `-> false` with the top unchanged.
- **Reuse.** `AddOrder(Buy id=12 price=40 qty=3) -> Ok` and the final top is `bid 0 x 0 | ask 0 x 0`.

## Gotchas

- Proving a cancel by the top of book alone misses FIFO corruption deeper in a level; follow every cancel with a match that walks the level, as the scenario does.
- A fully filled order is no longer resting; cancelling it must return `false`.
