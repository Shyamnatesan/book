# Add and match

A consumer submits a limit order with `AddOrder(order, on_trade)`. If it crosses the opposite side it trades against resting orders best price first and, within a price, oldest first, at the resting (maker) price; `on_trade` receives one `Trade{price, quantity, taker, maker}` per fill, and `order.quantity` is left holding the unfilled remainder. Whatever does not trade rests, and `BestBid/BestAsk/BestBidQty/BestAskQty` show the top of each side.

## Sub-features

- `add-rest` a non-crossing order rests and becomes the top of book if it is the best price.
- `add-top` `Best*Qty` is the sum of all orders at the best price.
- `match-time-priority` within one price, earlier orders fill first.
- `match-partial` partial fills of the taker and of the maker.
- `match-sweep` a taker walks several levels, each fill at the maker's price, remainder rests at the taker's limit.

## How to get to it (user POV)

- `book.AddOrder(order, callback)` from a consumer program.
- Indirectly through `book_test` (`PriceTimePriority`, `MultiLevelSweepAndRemainder`, `PartialFills`, `RandomizedVsModel`, ...).

## Driving it with vfb

Preconditions: none (scenario compiles from `include/`).

- **Scenario.** Run `$H/scripts/vfb scenario $H/scenarios/match.cpp`. Exit `0`, last line `match: PASSED (0 failed)`, empty `scenario-match.err`.
- **Time priority.** In `scenario-match.out` under `-- time priority inside a level`: `AddOrder(Buy id=6 price=101 qty=3)` produces `trade price=101 qty=2 taker=6 maker=4` then `trade price=101 qty=1 taker=6 maker=5`, and top becomes `ask 101 x 5`.
- **Sweep.** Under `-- sweep across levels`: `AddOrder(Buy id=7 price=102 qty=12)` trades `101 x 5` then `102 x 4`, `remainder=3`, top becomes `bid 102 x 3 | ask 0 x 0`.
- **Suite entry.** Run `$H/scripts/vfb test`. `book_test.out` ends with `22/22 tests passed` (count changes as tests are added; any `FAILED` line is a failure).

## Gotchas

- The remainder is reported by mutating the caller's `Order`; read `order.quantity` after the call, not the return value.
- `AddResult::Ok` with `quantity == 0` means fully filled, nothing rested.
- Trade price is always the maker's level price, never the taker's limit.
