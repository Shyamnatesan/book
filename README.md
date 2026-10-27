# book

Experimenting with the [pstack](https://github.com/cursor/plugins/tree/main/pstack) Claude Code plugin by Lauren Tan.

The point of this repo is the experiment, not the order book. I wanted a small, measurable C++ project to see how pstack's workflows hold up on real engineering work: rewriting a hot data structure, proving it correct, and chasing performance with evidence instead of vibes. The order book is the test subject. It is a header-only C++23 limit order book with price-time priority matching.

## What pstack was used for

All of the work below was driven through Claude Code with pstack installed.

| pstack piece | What it did here |
|---|---|
| `/setup-pstack` | Picked models per role (Opus for judgment and prose, Sonnet for code and review) at a "medium" reasoning budget. |
| `create-verification-skill` | Generated `.claude/skills/verify-fastbook`, which drives the library like a consumer: builds it, runs tests, scenarios and benchmarks, and saves evidence. |
| `create-formal-verification-skill` | Generated `.claude/skills/verify-fastbook-matching`, an exhaustive model check of the book against a reference price-time-priority book. |
| `/poteto-mode` (twice) | Ran the rewrite and then the performance investigation: architect runners for the design, a hillclimb loop with one commit per accepted win, a decision trail, and a cross-model review before reporting. |

Things the workflow caught that I would likely have shipped otherwise:

- **A wrong root cause.** The first explanation for three regressions ("cache misses from the hash") came from perf counters that were silently multiplexed. Exact counters showed the rewrite was executing more instructions instead.
- **A rigged comparison.** A cross-model reviewer noticed that the final A/B compared against a baseline the bench source had handicapped by about 27% through lost inlining. The numbers below use a baseline with its inlining restored.
- **Weak tests.** Mutation testing (`tests/mutate_invariants.py`) proved every invariant test fails when its invariant is broken, even with the book's own self-check disabled.

## The library

`FastBook<Allocator, MaxLevels>` in `include/fastbook.hpp`.

- Price-time priority matching. An incoming order trades against the best opposite levels, oldest order first, and the remainder rests.
- No allocation after construction. Levels, order nodes and the order index are sized once.
- O(1) cancel by order id, at any depth.
- Built-in observability: `Depth`, `FindOrder`, `Stats`, `OrderCount`, `LevelCount` and `CheckInvariants`.

### Quick start

```cpp
#include "fastbook.hpp"

#include <array>
#include <cstdio>
#include <memory>

using namespace shyamnatesan;

int main() {
  using Book = FastBook<std::allocator<Order>, 1024>; // up to 1024 price levels per side
  auto book = std::make_unique<Book>(100'000, std::allocator<Order>{}); // up to 100k resting orders

  auto print_trade = [](const Trade &t) {
    std::printf("trade %u @ %lld (taker %u, maker %u)\n", t.quantity, static_cast<long long>(t.price), t.taker, t.maker);
  };

  Order ask{101, 1, 50, Side::Sell};
  Order bid{99, 2, 30, Side::Buy};
  book->AddOrder(ask, print_trade);
  book->AddOrder(bid, print_trade);

  Order taker{101, 3, 20, Side::Buy}; // crosses the best ask
  AddResult r = book->AddOrder(taker, print_trade);
  std::printf("taker: %s, unfilled %u\n", ToString(r).data(), taker.quantity);

  std::printf("best bid %lld x %u, best ask %lld x %u\n", static_cast<long long>(book->BestBid()), book->BestBidQty(),
              static_cast<long long>(book->BestAsk()), book->BestAskQty());

  std::array<LevelView, 5> asks{};
  for (size_t i = 0, n = book->Depth(Side::Sell, asks); i < n; ++i)
    std::printf("ask level %zu: %lld x %u (%u orders)\n", i, static_cast<long long>(asks[i].price), asks[i].quantity, asks[i].order_count);

  book->CancelOrder(2);
  std::printf("orders %zu, trades %llu, invariants %s\n", book->OrderCount(),
              static_cast<unsigned long long>(book->Stats().trades), book->CheckInvariants().empty() ? "ok" : "BROKEN");
}
```

Output:

```
trade 20 @ 101 (taker 3, maker 1)
taker: Ok, unfilled 0
best bid 99 x 30, best ask 101 x 30
ask level 0: 101 x 30 (1 orders)
orders 1, trades 1, invariants ok
```

### API

| Call | Returns |
|---|---|
| `FastBook(size_t max_orders, Allocator)` | Throws `std::invalid_argument` unless `1 <= max_orders < UINT32_MAX`. |
| `AddOrder(Order&, on_trade)` | `AddResult`: `Ok`, `Duplicate`, `LevelsFull`, `PoolFull` or `QuantityOverflow`. Calls `on_trade(const Trade&)` per fill. `order.quantity` holds any unfilled remainder. |
| `CancelOrder(OrderId)` | `true` if exactly that order was removed. |
| `BestBid()`, `BestAsk()`, `BestBidQty()`, `BestAskQty()` | Top of book, or 0 when a side is empty. |
| `Depth(Side, span<LevelView>)` | Copies up to `span.size()` levels, best first. Returns the count written. |
| `FindOrder(OrderId)` | `optional<OrderView{price, quantity, side}>`. |
| `Stats()` | Counts of each `AddResult`, cancels, trades and traded quantity. |
| `CheckInvariants()` | Empty string when every invariant holds, else the first one broken. O(orders + levels), for tests and debugging. |

`on_trade` runs in the middle of matching and must not call back into the book.

### Invariants

These hold after every call, and the tests and model check enforce them:

1. Bids are sorted ascending with the best bid last.
2. Asks are sorted descending with the best ask last.
3. The book is never crossed.
4. Each level's total quantity and order count match its orders.
5. Cancel by id removes exactly one order.
6. Matching is price-time priority.

### Design

- **Levels.** Each side is a `Ladder` with fixed level slots and a sorted array of keys pointing at them, best last. Keys are the price for bids and `~price` for asks, so both sides share one code path.
- **Orders.** Orders are 16-byte nodes in a fixed array, linked into a FIFO per level. A side table maps each node to its level, which makes cancel O(1).
- **Order index.** Ids map to nodes through an open-addressing hash map with a Fibonacci hash and backward-shift deletion. The hash keeps strided or offset id ranges from forming long probe chains.
- **Inlining.** Hot-path helpers are `[[gnu::always_inline]]` and `Match` is out of line, so performance does not depend on how much else the caller's translation unit instantiates. Without this, the same benchmark moved 26% with identical headers.

## Build and test

Requires a C++23 compiler (tested with GCC 15.2) and CMake 3.16 or newer. Google Benchmark is fetched by CMake.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest            # book_test (22 tests) and invariant_test (8 tests)
./build/book_bench           # all benchmarks
```

More checks:

```sh
python3 tests/mutate_invariants.py                          # 12 mutants, all must be caught
.claude/skills/verify-fastbook-matching/check/run.sh        # exhaustive model check, about 90 s
python3 bench/compare.py OLD_BIN NEW_BIN --runs 7 --cpu 3   # interleaved A/B with a verdict per benchmark
```

## Benchmarks

### Summary

The rewrite is faster on 12 of the 15 throughput benchmarks, ties on 1, and is slower on 2.

The large wins all come from removing work that grew with the size of the book. The original found a price level by scanning, and it found an order by walking a cluster of neighbouring ids. Both costs grew as the book filled up. The rewrite reaches levels and orders directly, so most operations now cost about the same at any depth:

- Cancels take about 10 ns whether the order is at the best price or 100 levels deep. The original needed 32 to 71 ns.
- Matching an order against the top of the book drops from about 740 ns to about 14 ns.
- The pathological id patterns drop from hundreds or thousands of nanoseconds to 13 to 16 ns. These are cancelling random ids, and ids that share their low bits.

The rewrite also cuts tail latency. The slowest 1 in 1,000 cancels used to take about 900 ns, and now take 22 to 30 ns. For a trading system the tail matters as much as the average, because one slow cancel is a stale quote left in the market.

The rewrite is slower in two places, and both are small and explained:

- Sweeping 10 price levels in one order is 3% slower on the small book and 8% slower on the 500k-order book.
- On the 500k-order book, the slowest 1 in 1,000 adds takes 84 ns instead of 22 ns. The typical add is still faster, at 11 ns versus 15 ns.

Both come from the same tradeoff, described under "Where the rewrite is slower, and why" below.

### How it was measured

Every number compares the original implementation with the rewrite. Both binaries are built from the same benchmark source with the same flags (`-O3 -march=native`, GCC 15.2).

The original also gets a raised GCC inline budget. Without it, the larger benchmark file makes GCC stop inlining the original's hot path, which slows it by about 27%. That would flatter the rewrite, so the raised budget gives the original its normal speed back.

The machine is an AMD Ryzen 5 4600H laptop on Linux 7.0. Each benchmark runs 7 times, alternating between the two binaries, pinned to one CPU core. Each figure is the median of the 7 runs. A result counts as faster or slower only when the slowest run of one binary beats the fastest run of the other. Anything closer is reported as "no measurable difference".

Two book sizes are used:

- **Small.** 100 price levels per side with 10 orders each, so 2,000 resting orders. Everything fits in the CPU cache.
- **Large.** 2,500 price levels per side with 100 orders each, so 500,000 resting orders. This is far bigger than the cache, so it shows the cost of memory access.

What each benchmark does:

| Benchmark | Operation measured |
|---|---|
| AddAtBest | Add an order at the current best price, behind the orders already there. |
| AddNewBest | Add an order that creates a new best price. |
| AddDeep | Add an order at a new price far from the best. |
| CancelBest, CancelMiddle, CancelDeep | Cancel an order at the best level, a middle level, or the worst level. |
| MatchTopOfBook | An incoming order fully fills against one resting order at the best price. |
| Sweep10Levels | One incoming order consumes every order on the 10 best levels. Reported per sweep. |
| CancelRandomId | Cancel orders in random id order. |
| ClusteredPrices | Add and cancel orders within a narrow band of prices. |
| CancelOffsetIds | Cancel orders whose ids share their low bits, which the original's index handled worst. |

### Throughput

Average time per operation in nanoseconds. Lower is better.

| Benchmark | Original | Rewrite | Change |
|---|---:|---:|---|
| AddAtBest | 10.67 | 8.40 | 21% faster |
| AddNewBest | 9.33 | 9.33 | no measurable difference |
| AddDeep | 49.80 | 19.80 | 60% faster |
| CancelBest | 31.83 | 9.90 | 69% faster |
| CancelMiddle | 46.04 | 9.88 | 79% faster |
| CancelDeep | 70.99 | 9.87 | 86% faster |
| MatchTopOfBook | 743.40 | 13.82 | 98% faster |
| Sweep10Levels | 102.70 | 105.66 | 2.9% slower |
| CancelRandomId | 261.71 | 16.43 | 94% faster |
| ClusteredPrices | 26.85 | 19.31 | 28% faster |
| CancelOffsetIds | 2329.03 | 13.04 | 99% faster |
| Large AddAtBest | 10.62 | 8.81 | 17% faster |
| Large CancelBest | 32.01 | 10.67 | 67% faster |
| Large MatchTopOfBook | 742.88 | 16.55 | 98% faster |
| Large Sweep10Levels | 103.28 | 111.12 | 7.6% slower |

How to read this table:

- **Adds.** Adding at the best price is about 20% faster on both book sizes. Adding far from the best is 60% faster, because the original scanned levels to find where the new price goes.
- **Cancels.** The rewrite's cancel time is flat at about 10 ns. The original got slower the deeper the order sat, from 32 ns at the best level to 71 ns at the worst. Depth no longer matters.
- **Matching.** This is the biggest change. The original spent about 740 ns per fill walking its order index. The rewrite fills in 14 ns on the small book and 17 ns on the large one.
- **Bad id patterns.** CancelRandomId and CancelOffsetIds show what happens when ids don't arrive in a neat sequence. The original slowed to 262 ns and 2.3 µs. The rewrite stays at 13 to 16 ns.
- **Sweeps.** A sweep is the one case where the rewrite loses. The gap is small on the small book and grows to 8% on the large book.

### Latency per operation

The throughput table gives averages. This table times each operation on its own, to show how slow the slow cases get. Each cell is original → rewrite in nanoseconds:

- **p50** is the typical operation.
- **p99** is the slowest 1 in 100.
- **p99.9** is the slowest 1 in 1,000.

A dash means no measurable difference.

Timing uses the CPU's core-cycle counter (`rdpru`), because this CPU's timestamp counter only ticks every 10 ns. That is too coarse for operations that take 10 ns.

| Benchmark | p50 | p99 | p99.9 |
|---|---|---|---|
| AddAtBest | 15.3 → 10.8 | 17.3 → 12.8 | – |
| CancelBest | 25.1 → 12.8 | 142.3 → 18.6 | 901 → 22 |
| MatchTopOfBook | 732 → 19.3 | 1426 → 22.9 | 1632 → 30.9 |
| Sweep10Levels | 95.4 → 99.4 | – | – |
| Large AddAtBest | 15.1 → 11.0 | – | 22.3 → 84.1 |
| Large CancelBest | 25.1 → 14.6 | 143.8 → 21.8 | 900 → 29.6 |
| Large MatchTopOfBook | 730 → 20.6 | 1424 → 34.9 | 1656 → 87.9 |
| Large Sweep10Levels | 96.7 → 104.9 | – | – |

How to read this table:

- **Cancels have a much shorter tail.** In the original, 1 cancel in 100 took about 140 ns and 1 in 1,000 took about 900 ns. Those were the cancels that hit a long run of neighbouring ids in the index. In the rewrite, even the slowest 1 in 1,000 takes 22 to 30 ns.
- **Matching is faster at every percentile.** Even the rewrite's slowest 1 in 1,000 fills is 8 to 24 times faster than the original's typical fill. The tail is higher on the large book (88 ns) because some fills touch memory that is not in the cache.
- **Adds are faster in the typical case.** On the large book, though, the slowest 1 in 1,000 add takes 84 ns instead of 22 ns. That is one cache miss out to main memory, roughly once per 1,000 adds.
- **Sweeps are slightly slower in the typical case.** The tail shows no measurable difference.

### Where the rewrite is slower, and why

The original stored orders at position `id & mask`. Orders with consecutive ids therefore sat next to each other in memory. That helps a sweep, which fills many orders in a row, because the next order is usually already in the cache. It also caused the original's worst numbers. Each fill or cancel had to walk the run of neighbouring ids, which is the 743 ns MatchTopOfBook and the 2.3 µs CancelOffsetIds above.

The rewrite scatters ids across its index with a hash. That removes the long walks, which is where the 16× to 179× wins on matching and id-pattern cancels come from. The cost is that consecutive orders are no longer neighbours in memory. A sweep, or an add on a book too big for the cache, sometimes has to fetch from main memory.

I confirmed this is the cause rather than guessing. I put the original's `id & mask` placement into the new book as a test. The large-book sweep then matched the original (103.2 vs 103.1 ns), and the slow-add tail disappeared. With huge pages enabled for both builds (`GLIBC_TUNABLES=glibc.malloc.hugetlb=1`), the large-book sweep gap also falls to no measurable difference. That points at memory translation cost rather than the code itself.

Two index designs that try to keep both properties were built and measured: Robin Hood probing, and a hash that keeps blocks of consecutive ids together. Both were slower overall than the current design, so they were dropped. The current design trades a few percent on sweeps for large wins on every other operation. I think that trade is right.

## Layout

| Path | What it is |
|---|---|
| `include/fastbook.hpp` | The book. |
| `include/hashmap.hpp` | The open-addressing order index. |
| `include/order.hpp` | `Order`, `Trade`, `PriceLevel` and the id, price and quantity types. |
| `tests/book_test.cpp` | Behaviour tests for matching, cancels, rejections and the hash map. |
| `tests/invariant_test.cpp` | The six invariants, including randomized runs against a reference book. |
| `tests/mutate_invariants.py` | Breaks each invariant on purpose and requires a test to fail. |
| `bench/book_bench.cpp` | Throughput, large-book and latency benchmarks. |
| `bench/compare.py` | Interleaved A/B of two benchmark binaries with medians, ranges and verdicts. |
| `bench/attempt.sh` | One optimization attempt: every gate, then the A/B. |
| `.claude/skills/` | The two pstack-generated verification skills. |
