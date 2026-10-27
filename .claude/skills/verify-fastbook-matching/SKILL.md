---
name: verify-fastbook-matching
description: Exhaustively model-check FastBook (include/fastbook.hpp and the order index under it) against a reference price-time-priority book. Use after any change to AddOrder, CancelOrder, Match, Rest, the Ladder, the node array or level_of, CheckInvariants, OpenAddressingHashMap Insert/Find/Erase/Probe or its hashes, or the Order/PriceLevel structs, or when asked to prove the book's matching, cancel, or rejection behavior.
---

# Verify FastBook matching

## The property

**Promise:** for every sequence of `AddOrder` and `CancelOrder` calls, FastBook behaves exactly like a textbook price-time-priority book. That means the same `AddResult`, the same unfilled remainder, the same trades in the same order (price, quantity, taker, maker), the same `CancelOrder` result, and the same top of book. Its internals also stay consistent: `FastBook::CheckInvariants()` returns `""`, and the harness's own checks hold. Those are ladder keys sorted and encoding each level's price, FIFO order per level equal to the reference, `prev`/`next` links, `level_of` pointing back at each order's level, `order_count` and `total_quantity`, the order index mapping every resting id to its node (and nothing else), every index key reachable by linear probing from its `FibonacciHash` home, and the node and level free lists covering exactly the free slots.

**Harm if broken:** a trader is filled at the wrong price or out of time priority, a cancel silently misses a live order, or the order table loses an id so a resting order can never be cancelled. Memory errors and failed `assert`s also fail the check (ASan, UBSan, and asserts are on).

## How it works

`check/book_exhaustive.cpp` is an explicit-state model checker that runs on the real headers, not a hand-written model. It does a breadth-first search over every internal state FastBook can reach in a small universe. It reads private fields via `#define private public` and copies a state bytewise instead of replaying it. For each state it tries every operation in the universe's alphabet and compares the result against `Model`, a deliberately naive `std::map<Price, std::deque<...>>` reference book. A state already seen (same internal bytes and same reference contents) is not expanded again. So when the search runs out of new states (a **fixpoint**), sequences of every length in that universe have been covered. A failure prints the **shortest** sequence of calls that reaches it.

Tool choice: no off-the-shelf verifier handles this code. CBMC's and ESBMC's C++ front ends do not cover C++23 concepts and libstdc++, and a TLA+ model would check a re-description rather than these headers. Enumerating the real code's state space covers every case in scope, and needs only `g++`.

| Config | max_orders / MaxLevels | ids (table slots) | prices | qty | Scope |
| --- | --- | --- | --- | --- | --- |
| `pool-2` | 2 / 2 | 3, 8, 0 (4 slots; FibonacciHash homes 3, 3, 0: 8 wraps into 0's home) | 1, 2, 3 | 1, 2 | **fixpoint**, 3,752 states. Covers PoolFull, LevelsFull, partial fills on both sides, multi-order sweeps, id reuse, duplicates. |
| `probe-4` | 4 / 2 | 8, 16, 0, 5 (8 slots; FibonacciHash homes 7, 7, 0, 0: two chains meeting across the wrap) | 1, 2 | 1 | **fixpoint**, 126,048 states. Covers backward-shift deletion across the wrap point with up to 4 live keys. |
| `overflow` | 2 / 2 | 3, 8, 0 | 1, 2 | 1, `UINT32_MAX` | **bounded**: every sequence of ≤ 7 ops (7,840 states). A MAX-sized order partially filled by 1s yields ~4 billion distinct states, so no fixpoint exists. Covers QuantityOverflow. |

## Not covered

- Larger sizes. The universe uses ≤ 4 resting orders, ≤ 2 levels per side, and ≤ 3 prices. Bugs that need, for example, 3 levels on one side plus a sweep across all of them, a ladder binary search over more than one key, or a probe chain longer than 4, are outside it.
- The order table when it is full or nearly full. FastBook sizes it at 2 × `max_orders`, so the book never fills it. `OpenAddressingHashMap` used on its own at full load is not checked here.
- Quantities other than 1, 2, and `UINT32_MAX`. Order quantity 0. Price 0 or negative prices (0 is the "empty side" sentinel of `BestBid`/`BestAsk`).
- Custom allocators (only `std::allocator`), the constructor's `max_orders` validation, `Depth`, `FindOrder`, `Stats`.
- Concurrency. FastBook is single-threaded and this check is too.
- The reference model is trusted. If the intended semantics change (for example, rejections should leave a partial match unapplied), change `Model` first.

## Run it

```bash
.claude/skills/verify-fastbook-matching/check/run.sh
```

It takes about 60 s and exits 0 only if every config prints `PASS`. Progress lines (`[config] depth N: ...`) go to stderr. To check a modified copy of the headers without touching `include/`, run `run.sh /path/to/include-copy`. Use `-- --state-limit N` to raise the 5,000,000-state safety cap. Hitting the cap prints `INCOMPLETE` and exits 1.

## Reading a result

- `PASS [x] fixpoint` means every reachable state in that universe was checked. `PASS [x] bounded` means every sequence up to that length was checked.
- `FAIL [config] after N steps: <what differed>` followed by numbered calls is a counterexample. Replay those calls on a fresh `FastBook<std::allocator<Order>, 2>(max_orders, {})` to reproduce it. The "what differed" text names the field, for example `order table does not map id 7 to its pool handle` or `top of book bid 1 x 2 ... reference bid 1 x 1`.
- **Real bug or harness error?** Replay the counterexample in a scratch program against the public API only. If the public results (return values, trades, `BestBid`/`BestBidQty`...) differ from what price-time priority demands, it is a product bug. If only an internal invariant fails, read the field. A layout or field change you intended, such as a renamed member, a new sentinel, or a different level ordering, means the harness's `Invariants`, `Snapshot` or `CopyInto` needs updating. A compile error in the harness almost always means a private member was renamed.
- Do not "fix" a failure by weakening `Model`, removing an invariant, or shrinking the universe. Report the counterexample in the code's terms.
- A sanitizer report or `Assertion ... failed` is a failure in the product code, unless the stack is inside `book_exhaustive.cpp`.

## Coverage record

| Check | Covers |
| --- | --- |
| Observables vs `Model` | `FastBook::AddOrder`, `Match`, `Rest`, `CancelOrder`, `BestBid/BestAsk/BestBidQty/BestAskQty` (`include/fastbook.hpp`) |
| Level invariants | `FastBook::Ladder::LowerBound`, `Insert`, `Erase`, `Best`, `Unlink`, `PriceLevel` (`include/order.hpp`) |
| Node invariants | the node array, `level_of_`, the node free list, `FreeNode` (`include/fastbook.hpp`) |
| Index invariants | `OpenAddressingHashMap::Insert`, `Find`, `Erase` (backward shift), `Probe`, `FibonacciHash` (`include/hashmap.hpp`) |
| Book self-check | `FastBook::CheckInvariants` is called after every transition and its message reported |

The harness reads these private members by name: `FastBook::{kNone, ladders_, nodes_, level_of_, free_node_, index_, stats_}`, `Ladder::{key, slot, levels, size, free_level}`, `Node::{next, prev, id, qty}`, and `OpenAddressingHashMap::{slots_, capacity_, size_, hash(), Slot::{k, v, state}}`. `FastBook` opens with an explicit `private:` so `#define private public` reaches its members.

**Sync point:** confirmed against commit `1e1b85e`, with these header contents (first 12 hex digits of sha256):

```
0059c59d8ae8 include/fastbook.hpp
0d75095500a5 include/hashmap.hpp
dd2e966c3bf2 include/order.hpp
```

Before trusting a PASS, check whether a covered file changed since then (`sha256sum include/*.hpp`, or `git diff <sync commit> -- include/` once the sync point is a clean commit). If one did, read the change and confirm that `Invariants`, `Snapshot`, and `CopyInto` still describe every field that affects behavior. A new member that the snapshot ignores makes the fixpoint unsound, because two different states would look equal. Then run the check. Only after that, update the sync point above to the new commit (or new hashes).

## Proving the check still bites

After editing the harness, break the real code in a copy and confirm the check fails:

```bash
rm -rf /tmp/fb-mut && cp -r include /tmp/fb-mut
sed -i 's/((j - h) \& mask) >= ((j - i) \& mask)/((j - h) \& mask) > ((j - i) \& mask)/' /tmp/fb-mut/hashmap.hpp
.claude/skills/verify-fastbook-matching/check/run.sh /tmp/fb-mut   # must FAIL after 3 steps
```

Mutants confirmed caught at the sync point, with the shortest counterexample length:

- `Match` leaving an emptied level on the ladder (`break` instead of the pop): 2 steps, every config.
- `Match` popping the ladder without freeing the level slot: 2 steps, every config.
- `CancelOrder` skipping `level.total_quantity -=`: 3 steps, every config.
- `CancelOrder` leaving an emptied level on the ladder: 2 steps, every config.
- `Rest` appending new orders at the head (FIFO broken): 2 steps, every config.
- removing `- order.quantity` from the overflow guard: 2 steps, `overflow` only.
- the backward-shift condition `>=` → `>`: 3 steps, all three configs.
