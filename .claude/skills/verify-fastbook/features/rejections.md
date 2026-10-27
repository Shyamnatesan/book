# Rejections

`AddOrder` refuses work it cannot do safely and says why via `AddResult`: `Duplicate` (the id is already resting; nothing happens), and — for the part of the order that would rest — `LevelsFull` (a new price on a side that already has `MaxLevels` prices), `PoolFull` (`max_orders` orders already rest), `QuantityOverflow` (the level total would exceed `UINT32_MAX`). These three can follow a partial match: the fills stand and `order.quantity` holds the remainder that was *not* rested.

## Sub-features

- `reject-duplicate` adding a resting id again returns `Duplicate`, no trades, book unchanged.
- `reject-levels-full` a new price on a full side is refused; existing levels and the other side still accept.
- `reject-pool-full` an order that would rest when the pool is full is refused; one that fully matches still succeeds.
- `reject-overflow` adding to a level whose total would wrap is refused.
- `reject-after-match` the fills before a rejection are kept and reported via `on_trade`.

## How to get to it (user POV)

- `book.AddOrder(order, callback)` with the book constructed at small `max_orders` / `MaxLevels`.
- Indirectly through `book_test` (`DuplicateIdIgnored`, `LevelsFullRejected`, `PoolFullRejected`, `LevelQuantityOverflowRejected`).

## Driving it with vfb

Preconditions: none.

- **Scenario.** Run `$H/scripts/vfb scenario $H/scenarios/rejections.cpp`. Exit `0`, last line `rejections: PASSED (0 failed)`, empty `scenario-rejections.err`.
- **Each code.** `scenario-rejections.out` shows `-> Duplicate`, `-> LevelsFull`, `-> PoolFull`, `-> QuantityOverflow`, each followed by an unchanged top of book.
- **After a match.** Under `-- rejection after a partial match`: `AddOrder(Buy id=3 price=10 qty=5) -> LevelsFull, remainder=3, trades=1` with `trade price=10 qty=2 taker=3 maker=1`, and top `bid 5 x 1 | ask 0 x 0`.

## Gotchas

- `MaxLevels` is a template parameter and `max_orders` a constructor argument; scenarios need small values to reach the limits.
- `max_orders` × 2 must be a power of two (the order table's capacity) or the constructor throws `std::invalid_argument`.
- Rejection is not rollback: after `LevelsFull`/`PoolFull`/`QuantityOverflow` the trades already reported have happened.
