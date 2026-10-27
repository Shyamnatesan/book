// Orderbook benchmarks. Hot paths only, public API only, so the same file
// measures any FastBook implementation.
//
// Every benchmark shares one shape:
//   * A background book (kDepth levels per side, kPerLevel orders each) is
//     seeded once before the loop.
//   * Each iteration: optional untimed setup, one timed batch, untimed
//     restore. The book is back to the background at the end of every
//     iteration, so iterations are identical.
//   * Timing is manual (steady_clock around the batch). The `ns_per_op`
//     counter is the number to compare. Read it, not CPU time.
//   * Each benchmark checks its work (trade counts, cancel hits, final top of
//     book) and fails with SkipWithError instead of reporting a bogus number.
//   * ops_total >= 1,000,000 for every benchmark.
#include "fastbook.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace shyamnatesan;

namespace {

constexpr size_t kMaxOrders = 8192;
constexpr size_t kMaxLevels = 2048;
constexpr size_t kDepth = 100;
constexpr size_t kPerLevel = 10;
constexpr Quantity kBgQty = 100;
constexpr size_t kOps = 1024;
constexpr size_t kRing = 16;

using Book = FastBook<std::allocator<Order>, kMaxLevels>;
using Clock = std::chrono::steady_clock;

struct Rng {
  uint64_t s;
  uint64_t Next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
  }
  uint32_t Below(uint32_t n) { return static_cast<uint32_t>((Next() >> 32) % n); }
};

template <class T> void Shuffle(std::vector<T> &v, Rng &r) {
  for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[r.Below(static_cast<uint32_t>(i))]);
}

// Prices of the background book. Depth d = 0 is the best level.
struct Ladder {
  Price bid_top, ask_top;
  Price Bid(size_t d) const { return bid_top - static_cast<Price>(d); }
  Price Ask(size_t d) const { return ask_top + static_cast<Price>(d); }
  Price At(Side s, size_t d) const { return s == Side::Buy ? Bid(d) : Ask(d); }
};

constexpr Ladder kTight{9998, 10002};
constexpr Ladder kWide{9400, 10600}; // room for kOps/2 new best levels per side

constexpr OrderId kBgIdBase = 1;
// Tracked ids continue the background's sequence. Ids from a disjoint range
// are a separate scenario (BM_CancelOffsetIds) because the hash maps them
// onto the background's slots.
constexpr OrderId kTrackedIdBase = kBgIdBase + 2 * kDepth * kPerLevel;

Side SideOf(size_t i) { return (i & 1) ? Side::Sell : Side::Buy; }

struct Fixture {
  std::unique_ptr<Book> book = std::make_unique<Book>(kMaxOrders, std::allocator<Order>());
  Ladder ladder;
  double seconds = 0;
  uint64_t ops = 0;

  explicit Fixture(Ladder l) : ladder(l) {
    // Round-robin across levels so one level's orders are not adjacent in memory.
    OrderId id = kBgIdBase;
    for (size_t k = 0; k < kPerLevel; ++k)
      for (size_t d = 0; d < kDepth; ++d)
        for (Side s : {Side::Buy, Side::Sell}) {
          Order o{ladder.At(s, d), id++, kBgQty, s};
          book->AddOrder(o, [](const Trade &) {});
        }
  }

  template <class F> void Timed(benchmark::State &state, size_t n, F &&batch) {
    auto t0 = Clock::now();
    batch();
    benchmark::ClobberMemory();
    auto t1 = Clock::now();
    const double s = std::chrono::duration<double>(t1 - t0).count();
    state.SetIterationTime(s);
    seconds += s;
    ops += n;
  }

  bool AtBackground() const {
    return book->BestBid() == ladder.bid_top && book->BestAsk() == ladder.ask_top &&
           book->BestBidQty() == kPerLevel * kBgQty && book->BestAskQty() == kPerLevel * kBgQty;
  }

  void Finish(benchmark::State &state, const std::string &err = {}, bool background = true) {
    state.counters["ns_per_op"] = seconds * 1e9 / static_cast<double>(ops);
    state.counters["ops_total"] = static_cast<double>(ops);
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    if (!err.empty()) state.SkipWithError(err.c_str());
    else if (background && !AtBackground())
      state.SkipWithError("book did not return to the background state");
  }
};

uint64_t AddAll(Book &b, const std::vector<Order> &orders) {
  uint64_t trades = 0;
  for (const Order &src : orders) {
    Order o = src;
    b.AddOrder(o, [&](const Trade &) { ++trades; });
  }
  return trades;
}

uint64_t CancelAll(Book &b, const std::vector<OrderId> &ids) {
  uint64_t hits = 0;
  for (OrderId id : ids) hits += b.CancelOrder(id);
  return hits;
}

// Timed batches go through these noinline copies so perf can tell the timed
// path from the untimed seed and restore phases.
[[gnu::noinline]] uint64_t TimedAdds(Book &b, const std::vector<Order> &orders) {
  return AddAll(b, orders);
}

[[gnu::noinline]] uint64_t TimedCancels(Book &b, const std::vector<OrderId> &ids) {
  return CancelAll(b, ids);
}

std::vector<OrderId> IdsOf(const std::vector<Order> &orders) {
  std::vector<OrderId> ids;
  for (const Order &o : orders) ids.push_back(o.order_id);
  return ids;
}

// Resting adds at depths chosen by `depth(i, rng)`; restore cancels them.
template <class DepthFn> void RunRestingAdd(benchmark::State &state, Ladder ladder, DepthFn depth) {
  Fixture f(ladder);
  Rng r{0x9E3779B97F4A7C15ULL};
  std::vector<std::vector<Order>> ring(kRing);
  for (auto &batch : ring)
    for (size_t i = 0; i < kOps; ++i) {
      Side s = SideOf(i);
      Price p = depth(i, r, s);
      batch.push_back(Order{p, kTrackedIdBase + static_cast<OrderId>(i), 1 + r.Below(100), s});
    }
  const auto ids = IdsOf(ring[0]);
  uint64_t trades = 0, hits = 0;
  size_t it = 0;
  for (auto _ : state) {
    const auto &batch = ring[it++ % kRing];
    f.Timed(state, kOps, [&] { trades += TimedAdds(*f.book, batch); });
    hits += CancelAll(*f.book, ids);
  }
  f.Finish(state, trades != 0 ? "resting add traded"
                  : hits != kOps * state.iterations() ? "restore missed orders" : "");
}

void BM_AddAtBest(benchmark::State &state) {
  RunRestingAdd(state, kTight, [](size_t, Rng &, Side s) { return kTight.At(s, 0); });
}

// Each add is strictly better than the current best on its side.
void BM_AddNewBest(benchmark::State &state) {
  RunRestingAdd(state, kWide, [](size_t i, Rng &, Side s) {
    const Price step = static_cast<Price>(i / 2 + 1);
    return s == Side::Buy ? kWide.bid_top + step : kWide.ask_top - step;
  });
}

void BM_AddDeep(benchmark::State &state) {
  RunRestingAdd(state, kTight, [](size_t, Rng &r, Side s) {
    return kTight.At(s, 75 + r.Below(kDepth - 75));
  });
}

// Untimed: seed kOps orders at the given depths. Timed: cancel them in random
// order (plus `misses` unknown ids).
template <class DepthFn>
void RunCancel(benchmark::State &state, DepthFn depth, bool random_ids, size_t misses,
               OrderId seq_base = kTrackedIdBase) {
  Fixture f(kTight);
  Rng r{0x1234567887654321ULL};
  std::vector<std::vector<Order>> seeds(kRing);
  std::vector<std::vector<OrderId>> cancels(kRing);
  std::unordered_set<OrderId> used;
  auto fresh_id = [&](size_t i) -> OrderId {
    if (!random_ids) return seq_base + static_cast<OrderId>(i);
    for (;;) {
      OrderId id = static_cast<OrderId>(r.Next() >> 32);
      if (id >= kBgIdBase + 2 * kDepth * kPerLevel && used.insert(id).second) return id;
    }
  };
  for (size_t c = 0; c < kRing; ++c) {
    if (random_ids) used.clear();
    for (size_t i = 0; i < kOps; ++i) {
      Side s = SideOf(i);
      seeds[c].push_back(Order{kTight.At(s, depth(r)), fresh_id(i), 1 + r.Below(100), s});
    }
    cancels[c] = IdsOf(seeds[c]);
    for (size_t i = 0; i < misses; ++i) cancels[c].push_back(fresh_id(kOps + i));
    Shuffle(cancels[c], r);
  }
  uint64_t trades = 0, hits = 0;
  size_t it = 0;
  for (auto _ : state) {
    const size_t c = it++ % kRing;
    trades += AddAll(*f.book, seeds[c]);
    f.Timed(state, cancels[c].size(), [&] { hits += TimedCancels(*f.book, cancels[c]); });
  }
  f.Finish(state, trades != 0 ? "seed traded"
                  : hits != kOps * state.iterations() ? "cancel hit count wrong" : "");
}

void BM_CancelBest(benchmark::State &state) {
  RunCancel(state, [](Rng &) { return size_t{0}; }, false, 0);
}
void BM_CancelMiddle(benchmark::State &state) {
  RunCancel(state, [](Rng &) { return kDepth / 2; }, false, 0);
}
void BM_CancelDeep(benchmark::State &state) {
  RunCancel(state, [](Rng &) { return kDepth - 1; }, false, 0);
}
// Random 32-bit ids at random depths, 1 in 16 cancels misses.
void BM_CancelRandomId(benchmark::State &state) {
  RunCancel(state, [](Rng &r) { return size_t{r.Below(kDepth)}; }, true, kOps / 16);
}
// Extra scenario: sequential ids from a second range (1,000,000+) at random
// depths, next to the background's 1..2000.
void BM_CancelOffsetIds(benchmark::State &state) {
  RunCancel(state, [](Rng &r) { return size_t{r.Below(kDepth)}; }, false, 0, 1'000'000);
}

// Untimed: kOps makers on a new best level per side (in front of the
// background). Timed: kOps takers, each fully filling exactly one maker.
void BM_MatchTopOfBook(benchmark::State &state) {
  Fixture f(kTight);
  const Price bid = kTight.bid_top + 1, ask = kTight.ask_top - 1;
  std::vector<Order> makers, takers;
  Rng r{0xC0FFEEULL};
  for (size_t i = 0; i < kOps; ++i) {
    Side s = SideOf(i);
    Quantity q = 1 + r.Below(100);
    makers.push_back(Order{s == Side::Buy ? bid : ask, kTrackedIdBase + static_cast<OrderId>(i), q, s});
    Side t = s == Side::Buy ? Side::Sell : Side::Buy;
    takers.push_back(Order{s == Side::Buy ? bid : ask, kTrackedIdBase + static_cast<OrderId>(kOps + i), q, t});
  }
  uint64_t seed_trades = 0, trades = 0;
  for (auto _ : state) {
    seed_trades += AddAll(*f.book, makers);
    f.Timed(state, kOps, [&] { trades += TimedAdds(*f.book, takers); });
  }
  f.Finish(state, seed_trades != 0 ? "seed traded"
                  : trades != kOps * state.iterations() ? "takers did not fill exactly one maker each" : "");
}

// Untimed: kSweeps*10 one-order levels per side in front of the background.
// Timed: kSweeps takers per side, each consuming exactly the next 10 levels.
void BM_Sweep10Levels(benchmark::State &state) {
  constexpr size_t kSweeps = 64, kLevels = 10;
  constexpr Ladder ladder{9400 - 1000, 10600 + 1000}; // background behind 640 tracked levels
  Fixture f(ladder);
  std::vector<Order> makers, takers;
  OrderId id = kTrackedIdBase;
  // Far levels first, so each seed add is a new best (cheap) and seeding stays untimed noise.
  for (size_t lvl = kSweeps * kLevels; lvl-- > 0;) {
    makers.push_back(Order{ladder.bid_top + 1 + static_cast<Price>(kSweeps * kLevels - 1 - lvl), id++, 1, Side::Buy});
    makers.push_back(Order{ladder.ask_top - 1 - static_cast<Price>(kSweeps * kLevels - 1 - lvl), id++, 1, Side::Sell});
  }
  // Best tracked bid is bid_top + 640, best tracked ask is ask_top - 640.
  const Price best_bid = ladder.bid_top + static_cast<Price>(kSweeps * kLevels);
  const Price best_ask = ladder.ask_top - static_cast<Price>(kSweeps * kLevels);
  for (size_t j = 0; j < kSweeps; ++j) {
    const Price reach = static_cast<Price>((j + 1) * kLevels - 1);
    takers.push_back(Order{best_ask + reach, id++, kLevels, Side::Buy});
    takers.push_back(Order{best_bid - reach, id++, kLevels, Side::Sell});
  }
  uint64_t seed_trades = 0, trades = 0;
  for (auto _ : state) {
    seed_trades += AddAll(*f.book, makers);
    f.Timed(state, takers.size(), [&] { trades += TimedAdds(*f.book, takers); });
  }
  f.Finish(state, seed_trades != 0 ? "seed traded"
                  : trades != kLevels * takers.size() * state.iterations() ? "sweeps did not take exactly 10 levels" : "");
}

// Mixed flow on an empty book with every price within 5 ticks of mid: 60%
// resting adds, 15% marketable takers, 25% cancels of earlier ids. Untimed:
// cancel every id the stream used.
void BM_ClusteredPrices(benchmark::State &state) {
  constexpr Ladder ladder{9998, 10002};
  Fixture f(ladder);
  // Clear the background: this workload runs on its own ids around mid 10000.
  for (OrderId id = kBgIdBase; id < kBgIdBase + 2 * kDepth * kPerLevel; ++id) f.book->CancelOrder(id);
  struct Op { bool cancel; Order o; };
  std::vector<std::vector<Op>> ring(kRing);
  std::vector<std::vector<OrderId>> cleanup(kRing);
  std::vector<uint64_t> expected_trades(kRing, UINT64_MAX);
  Rng r{0xBADC0DEULL};
  constexpr Price kMid = 10000;
  for (size_t c = 0; c < kRing; ++c) {
    std::vector<OrderId> added;
    OrderId id = kTrackedIdBase;
    for (size_t i = 0; i < kOps; ++i) {
      uint32_t roll = r.Below(100);
      Side s = r.Below(2) ? Side::Buy : Side::Sell;
      if (roll < 25 && !added.empty()) {
        ring[c].push_back({true, Order{0, added[r.Below(static_cast<uint32_t>(added.size()))], 0, s}});
      } else if (roll < 40) {
        Price p = s == Side::Buy ? kMid + 5 : kMid - 5;
        ring[c].push_back({false, Order{p, id, 1 + r.Below(20), s}});
        added.push_back(id++);
      } else {
        Price off = 1 + r.Below(5);
        ring[c].push_back({false, Order{s == Side::Buy ? kMid - off : kMid + off, id, 1 + r.Below(20), s}});
        added.push_back(id++);
      }
    }
    cleanup[c] = added;
  }
  uint64_t trades_all = 0;
  std::string err;
  size_t it = 0;
  for (auto _ : state) {
    const size_t c = it++ % kRing;
    uint64_t trades = 0;
    f.Timed(state, kOps, [&] {
      for (const Op &op : ring[c]) {
        if (op.cancel) {
          f.book->CancelOrder(op.o.order_id);
        } else {
          Order o = op.o;
          f.book->AddOrder(o, [&](const Trade &) { ++trades; });
        }
      }
    });
    CancelAll(*f.book, cleanup[c]);
    if (expected_trades[c] == UINT64_MAX) expected_trades[c] = trades;
    else if (expected_trades[c] != trades) err = "trade count differs between runs of one stream";
    if (f.book->BestBid() != 0 || f.book->BestAsk() != 0) err = "book not empty after cleanup";
    trades_all += trades;
  }
  state.counters["trades_per_op"] = static_cast<double>(trades_all) / static_cast<double>(f.ops);
  if (trades_all == 0) err = "clustered flow never traded";
  f.Finish(state, err, /*background=*/false);
}

} // namespace

#define FASTBOOK_BENCH(fn, iters) \
  BENCHMARK(fn)->UseManualTime()->Iterations(iters)->Unit(benchmark::kMicrosecond)

FASTBOOK_BENCH(BM_AddAtBest, 1024);
FASTBOOK_BENCH(BM_AddNewBest, 1024);
FASTBOOK_BENCH(BM_AddDeep, 1024);
FASTBOOK_BENCH(BM_CancelBest, 1024);
FASTBOOK_BENCH(BM_CancelMiddle, 1024);
FASTBOOK_BENCH(BM_CancelDeep, 1024);
FASTBOOK_BENCH(BM_MatchTopOfBook, 1024);
FASTBOOK_BENCH(BM_Sweep10Levels, 8192);
FASTBOOK_BENCH(BM_CancelRandomId, 1024);
FASTBOOK_BENCH(BM_ClusteredPrices, 1024);
FASTBOOK_BENCH(BM_CancelOffsetIds, 1024);

BENCHMARK_MAIN();
