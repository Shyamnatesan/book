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
//   * BM_Large_* run the same scenarios on a ~500k-order book.
//   * BM_Lat_* run AddAtBest/CancelBest/MatchTopOfBook/Sweep10Levels with every
//     operation timed on its own (core cycles via rdpru) and report percentiles.
#include "fastbook.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <x86intrin.h>

using namespace shyamnatesan;

namespace {

constexpr OrderId kBgIdBase = 1;
constexpr Quantity kBgQty = 100;
constexpr size_t kOps = 1024;
constexpr size_t kRing = 16;

// Background book geometry: `Depth` levels per side, `PerLevel` orders each.
template <size_t MaxOrders_, size_t MaxLevels, size_t Depth, size_t PerLevel_> struct Shape {
  using Book = FastBook<std::allocator<Order>, MaxLevels>;
  static constexpr size_t kMaxOrders = MaxOrders_, kDepth = Depth, kPerLevel = PerLevel_;
  static constexpr size_t kBgOrders = 2 * kDepth * kPerLevel;
  // Tracked ids continue the background's sequence. Ids from a disjoint range
  // are a separate scenario (BM_CancelOffsetIds) because the hash maps them
  // onto the background's slots.
  static constexpr OrderId kTrackedIdBase = kBgIdBase + kBgOrders;
};

using Small = Shape<8192, 2048, 100, 10>;
using Large = Shape<size_t{1} << 20, 4096, 2500, 100>; // 500k resting orders

constexpr size_t kDepth = Small::kDepth;
constexpr size_t kPerLevel = Small::kPerLevel;
constexpr OrderId kTrackedIdBase = Small::kTrackedIdBase;

using Book = Small::Book;
using Clock = std::chrono::steady_clock;

enum class Mode { Throughput, Latency };

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

Side SideOf(size_t i) { return (i & 1) ? Side::Sell : Side::Buy; }

template <class B> [[gnu::always_inline]] inline uint64_t AddAll(B &b, const std::vector<Order> &orders) {
  uint64_t trades = 0;
  for (const Order &src : orders) {
    Order o = src;
    b.AddOrder(o, [&](const Trade &) { ++trades; });
  }
  return trades;
}

template <class B> [[gnu::always_inline]] inline uint64_t CancelAll(B &b, const std::vector<OrderId> &ids) {
  uint64_t hits = 0;
  for (OrderId id : ids) hits += b.CancelOrder(id);
  return hits;
}

// Timed batches go through these noinline copies so perf can tell the timed
// path from the untimed seed and restore phases.
template <class B> [[gnu::noinline]] uint64_t TimedAdds(B &b, const std::vector<Order> &orders) {
  return AddAll(b, orders);
}

template <class B> [[gnu::noinline]] uint64_t TimedCancels(B &b, const std::vector<OrderId> &ids) {
  return CancelAll(b, ids);
}

// Fenced core-cycle read. On Zen 2 the TSC advances in 30-cycle steps, too
// coarse for 10 ns operations, so this reads APERF (actual core cycles, one
// cycle resolution) with rdpru. lfence before keeps earlier work out of the
// sample; lfence after keeps later work from starting before the read.
[[gnu::always_inline]] inline uint64_t FencedCycles() {
  uint32_t lo, hi;
  _mm_lfence();
  asm volatile("rdpru" : "=a"(lo), "=d"(hi) : "c"(1));
  _mm_lfence();
  return (uint64_t{hi} << 32) | lo;
}

struct CycleCalibration {
  uint64_t overhead = 0; // cycles of one empty fenced pair
  double cycles_per_ns = 0;
};

const CycleCalibration &Calibration() {
  static const CycleCalibration cal = [] {
    CycleCalibration c;
    std::vector<uint64_t> pairs(10'001);
    for (uint64_t &d : pairs) {
      const uint64_t t0 = FencedCycles();
      const uint64_t t1 = FencedCycles();
      d = t1 - t0;
    }
    std::nth_element(pairs.begin(), pairs.begin() + pairs.size() / 2, pairs.end());
    c.overhead = pairs[pairs.size() / 2];
    const auto w0 = Clock::now();
    const uint64_t c0 = FencedCycles();
    while (Clock::now() - w0 < std::chrono::milliseconds(50)) {}
    const uint64_t c1 = FencedCycles();
    const auto w1 = Clock::now();
    c.cycles_per_ns = static_cast<double>(c1 - c0) / std::chrono::duration<double, std::nano>(w1 - w0).count();
    return c;
  }();
  return cal;
}

// One cycle count per operation, overhead already subtracted.
struct LatSamples {
  std::vector<uint32_t> cycles;
  uint64_t sum = 0;

  void Add(uint64_t raw) {
    const uint64_t over = Calibration().overhead;
    const uint32_t c = static_cast<uint32_t>(std::min<uint64_t>(raw > over ? raw - over : 0, UINT32_MAX));
    cycles.push_back(c); // capacity reserved before the first timed batch
    sum += c;
  }

  double Ns(uint32_t c) const { return static_cast<double>(c) / Calibration().cycles_per_ns; }

  // Nearest-rank percentile, exact (nth_element), q in (0, 1].
  uint32_t Percentile(double q) {
    const size_t k = static_cast<size_t>(std::ceil(q * static_cast<double>(cycles.size()))) - 1;
    std::nth_element(cycles.begin(), cycles.begin() + static_cast<std::ptrdiff_t>(k), cycles.end());
    return cycles[k];
  }
};

// Per-operation latency of the book call. A sample is the cycle count between
// two fenced cycle reads around exactly one AddOrder (or one CancelOrder),
// minus the cost of an empty fenced pair measured once per process. It
// includes the call and the callback; it excludes the Order copy, the loop
// and the sample store. Samples are per op, not per batch: a batch of kOps
// operations yields kOps samples, so the tail is not averaged away. The
// calibrated overhead is a median, so a sample can come out a few cycles
// off, and it is clamped at zero.
template <class B> [[gnu::noinline]] uint64_t LatAdds(B &b, const std::vector<Order> &orders, LatSamples &out) {
  uint64_t trades = 0;
  for (const Order &src : orders) {
    Order o = src;
    const uint64_t t0 = FencedCycles();
    b.AddOrder(o, [&](const Trade &) { ++trades; });
    const uint64_t t1 = FencedCycles();
    out.Add(t1 - t0);
  }
  return trades;
}

template <class B> [[gnu::noinline]] uint64_t LatCancels(B &b, const std::vector<OrderId> &ids, LatSamples &out) {
  uint64_t hits = 0;
  for (OrderId id : ids) {
    const uint64_t t0 = FencedCycles();
    hits += b.CancelOrder(id);
    const uint64_t t1 = FencedCycles();
    out.Add(t1 - t0);
  }
  return hits;
}

template <class S = Small> struct Fixture {
  using Book = typename S::Book;
  std::unique_ptr<Book> book = std::make_unique<Book>(S::kMaxOrders, std::allocator<Order>());
  Ladder ladder;
  double seconds = 0;
  uint64_t ops = 0;
  double seed_seconds = 0;
  LatSamples lat;
  bool latency;

  explicit Fixture(Ladder l, Mode mode = Mode::Throughput) : ladder(l), latency(mode == Mode::Latency) {
    if (latency) (void)Calibration();
    const auto t0 = Clock::now();
    // Round-robin across levels so one level's orders are not adjacent in memory.
    OrderId id = kBgIdBase;
    for (size_t k = 0; k < S::kPerLevel; ++k)
      for (size_t d = 0; d < S::kDepth; ++d)
        for (Side s : {Side::Buy, Side::Sell}) {
          Order o{ladder.At(s, d), id++, kBgQty, s};
          book->AddOrder(o, [](const Trade &) {});
        }
    seed_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  }

  uint64_t Adds(const std::vector<Order> &orders) {
    return latency ? LatAdds(*book, orders, lat) : TimedAdds(*book, orders);
  }

  uint64_t Cancels(const std::vector<OrderId> &ids) {
    return latency ? LatCancels(*book, ids, lat) : TimedCancels(*book, ids);
  }

  template <class F> void Timed(benchmark::State &state, size_t n, F &&batch) {
    if (latency && lat.cycles.capacity() == 0) lat.cycles.reserve(state.max_iterations * n);
    const uint64_t sum0 = lat.sum;
    auto t0 = Clock::now();
    batch();
    benchmark::ClobberMemory();
    auto t1 = Clock::now();
    const double s = latency ? static_cast<double>(lat.sum - sum0) / Calibration().cycles_per_ns * 1e-9
                             : std::chrono::duration<double>(t1 - t0).count();
    state.SetIterationTime(s);
    seconds += s;
    ops += n;
  }

  bool AtBackground() const {
    return book->BestBid() == ladder.bid_top && book->BestAsk() == ladder.ask_top &&
           book->BestBidQty() == S::kPerLevel * kBgQty && book->BestAskQty() == S::kPerLevel * kBgQty;
  }

  void Finish(benchmark::State &state, std::string err = {}, bool background = true) {
    state.counters["ns_per_op"] = seconds * 1e9 / static_cast<double>(ops);
    state.counters["ops_total"] = static_cast<double>(ops);
    state.counters["seed_ms"] = seed_seconds * 1e3;
    state.SetItemsProcessed(static_cast<int64_t>(ops));
    if (latency && err.empty()) {
      if (lat.cycles.size() != ops) err = "sample count differs from op count";
      else {
        for (const auto &[name, q] : {std::pair{"p50", 0.5}, {"p99", 0.99}, {"p999", 0.999}}) {
          const uint32_t c = lat.Percentile(q);
          state.counters[std::string(name) + "_cyc"] = c;
          state.counters[std::string(name) + "_ns"] = lat.Ns(c);
        }
        state.counters["max_ns"] = lat.Ns(*std::max_element(lat.cycles.begin(), lat.cycles.end()));
        state.counters["ghz"] = Calibration().cycles_per_ns;
      }
    }
    if (!err.empty()) state.SkipWithError(err.c_str());
    else if (background && !AtBackground())
      state.SkipWithError("book did not return to the background state");
  }
};

std::vector<OrderId> IdsOf(const std::vector<Order> &orders) {
  std::vector<OrderId> ids;
  for (const Order &o : orders) ids.push_back(o.order_id);
  return ids;
}

// Resting adds at depths chosen by `depth(i, rng)`; restore cancels them.
template <class S = Small, class DepthFn>
void RunRestingAdd(benchmark::State &state, Ladder ladder, DepthFn depth, Mode mode = Mode::Throughput) {
  Fixture<S> f(ladder, mode);
  Rng r{0x9E3779B97F4A7C15ULL};
  std::vector<std::vector<Order>> ring(kRing);
  for (auto &batch : ring)
    for (size_t i = 0; i < kOps; ++i) {
      Side s = SideOf(i);
      Price p = depth(i, r, s);
      batch.push_back(Order{p, S::kTrackedIdBase + static_cast<OrderId>(i), 1 + r.Below(100), s});
    }
  const auto ids = IdsOf(ring[0]);
  uint64_t trades = 0, hits = 0;
  size_t it = 0;
  for (auto _ : state) {
    const auto &batch = ring[it++ % kRing];
    f.Timed(state, kOps, [&] { trades += f.Adds(batch); });
    hits += CancelAll(*f.book, ids);
  }
  f.Finish(state, trades != 0 ? "resting add traded"
                  : hits != kOps * state.iterations() ? "restore missed orders" : "");
}

template <class S> void AddAtBest(benchmark::State &state, Mode mode) {
  RunRestingAdd<S>(state, kTight, [](size_t, Rng &, Side s) { return kTight.At(s, 0); }, mode);
}
void BM_AddAtBest(benchmark::State &state) { AddAtBest<Small>(state, Mode::Throughput); }

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
template <class S = Small, class DepthFn>
void RunCancel(benchmark::State &state, DepthFn depth, bool random_ids, size_t misses,
               OrderId seq_base = S::kTrackedIdBase, Mode mode = Mode::Throughput) {
  Fixture<S> f(kTight, mode);
  Rng r{0x1234567887654321ULL};
  std::vector<std::vector<Order>> seeds(kRing);
  std::vector<std::vector<OrderId>> cancels(kRing);
  std::unordered_set<OrderId> used;
  auto fresh_id = [&](size_t i) -> OrderId {
    if (!random_ids) return seq_base + static_cast<OrderId>(i);
    for (;;) {
      OrderId id = static_cast<OrderId>(r.Next() >> 32);
      if (id >= S::kTrackedIdBase && used.insert(id).second) return id;
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
    f.Timed(state, cancels[c].size(), [&] { hits += f.Cancels(cancels[c]); });
  }
  f.Finish(state, trades != 0 ? "seed traded"
                  : hits != kOps * state.iterations() ? "cancel hit count wrong" : "");
}

template <class S> void CancelBest(benchmark::State &state, Mode mode) {
  RunCancel<S>(state, [](Rng &) { return size_t{0}; }, false, 0, S::kTrackedIdBase, mode);
}
void BM_CancelBest(benchmark::State &state) { CancelBest<Small>(state, Mode::Throughput); }
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
template <class S> void MatchTopOfBook(benchmark::State &state, Mode mode) {
  Fixture<S> f(kTight, mode);
  const Price bid = kTight.bid_top + 1, ask = kTight.ask_top - 1;
  std::vector<Order> makers, takers;
  Rng r{0xC0FFEEULL};
  for (size_t i = 0; i < kOps; ++i) {
    Side s = SideOf(i);
    Quantity q = 1 + r.Below(100);
    makers.push_back(Order{s == Side::Buy ? bid : ask, S::kTrackedIdBase + static_cast<OrderId>(i), q, s});
    Side t = s == Side::Buy ? Side::Sell : Side::Buy;
    takers.push_back(Order{s == Side::Buy ? bid : ask, S::kTrackedIdBase + static_cast<OrderId>(kOps + i), q, t});
  }
  uint64_t seed_trades = 0, trades = 0;
  for (auto _ : state) {
    seed_trades += AddAll(*f.book, makers);
    f.Timed(state, kOps, [&] { trades += f.Adds(takers); });
  }
  f.Finish(state, seed_trades != 0 ? "seed traded"
                  : trades != kOps * state.iterations() ? "takers did not fill exactly one maker each" : "");
}
void BM_MatchTopOfBook(benchmark::State &state) { MatchTopOfBook<Small>(state, Mode::Throughput); }

// Untimed: kSweeps*10 one-order levels per side in front of the background.
// Timed: kSweeps takers per side, each consuming exactly the next 10 levels.
template <class S> void Sweep10Levels(benchmark::State &state, Mode mode) {
  constexpr size_t kSweeps = 64, kLevels = 10;
  constexpr Ladder ladder{9400 - 1000, 10600 + 1000}; // background behind 640 tracked levels
  Fixture<S> f(ladder, mode);
  std::vector<Order> makers, takers;
  OrderId id = S::kTrackedIdBase;
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
    f.Timed(state, takers.size(), [&] { trades += f.Adds(takers); });
  }
  f.Finish(state, seed_trades != 0 ? "seed traded"
                  : trades != kLevels * takers.size() * state.iterations() ? "sweeps did not take exactly 10 levels" : "");
}
void BM_Sweep10Levels(benchmark::State &state) { Sweep10Levels<Small>(state, Mode::Throughput); }

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

#define FASTBOOK_SCENARIO_BENCH(name, fn, shape, mode, iters) \
  static void name(benchmark::State &state) { fn<shape>(state, mode); } \
  FASTBOOK_BENCH(name, iters)

FASTBOOK_SCENARIO_BENCH(BM_Large_AddAtBest, AddAtBest, Large, Mode::Throughput, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Large_CancelBest, CancelBest, Large, Mode::Throughput, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Large_MatchTopOfBook, MatchTopOfBook, Large, Mode::Throughput, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Large_Sweep10Levels, Sweep10Levels, Large, Mode::Throughput, 8192);

FASTBOOK_SCENARIO_BENCH(BM_Lat_AddAtBest, AddAtBest, Small, Mode::Latency, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Lat_CancelBest, CancelBest, Small, Mode::Latency, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Lat_MatchTopOfBook, MatchTopOfBook, Small, Mode::Latency, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Lat_Sweep10Levels, Sweep10Levels, Small, Mode::Latency, 8192);
FASTBOOK_SCENARIO_BENCH(BM_Lat_Large_AddAtBest, AddAtBest, Large, Mode::Latency, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Lat_Large_CancelBest, CancelBest, Large, Mode::Latency, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Lat_Large_MatchTopOfBook, MatchTopOfBook, Large, Mode::Latency, 1024);
FASTBOOK_SCENARIO_BENCH(BM_Lat_Large_Sweep10Levels, Sweep10Levels, Large, Mode::Latency, 8192);

BENCHMARK_MAIN();
