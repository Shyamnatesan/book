// Orderbook benchmarks. Hot path only: AddOrder / CancelOrder / matching.
//
// Shape of every benchmark (see per-benchmark comments):
//   * Book is built ONCE before the `for (auto _ : state)` loop and destroyed
//     after it (unique_ptr goes out of scope at function return).
//   * All inputs (orders, ids, cancel lists) are precomputed in a ring of
//     kRing cycles before the loop. Nothing allocates in the loop.
//   * Each iteration is one "cycle" that returns the book to EMPTY, so state is
//     identical at the start of every iteration. The timed phase is bracketed
//     with steady_clock and reported via UseManualTime()/SetIterationTime();
//     the untimed seed/cleanup phases run inside the iteration but are not
//     counted (no PauseTiming/ResumeTiming, which costs ~100s of ns per call).
//     READ THE "Time" COLUMN (manual time), NOT "CPU" (CPU covers the untimed
//     phases too).
//   * Iterations(N) is fixed, so the op floor holds regardless of
//     --benchmark_min_time. Verify with the `ops_total` counter (must be
//     >= 1,000,000) or JSON output.
#include "fastbook.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

using namespace shyamnatesan;

namespace {

constexpr size_t kMaxOrders = 8192;  // pool size; hash table = 16384 slots
constexpr size_t kMaxLevels = 512;   // prices live in [9900,10100] -> <=201/side
constexpr size_t kRing = 16;         // distinct precomputed cycles
constexpr Price kBidTop = 9999;
constexpr Price kAskTop = 10001;

using Alloc = std::allocator<Order>;
using Book = FastBook<Alloc, kMaxLevels>;

struct Rng {
  uint64_t s;
  uint64_t Next() {  // xorshift64*
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
  }
  uint32_t Next32() { return static_cast<uint32_t>(Next() >> 32); }
};

struct IdGen {  // unique, non-sequential, random-low-bits ids
  Rng rng{0x9E3779B97F4A7C15ULL};
  std::unordered_set<OrderId> used;
  OrderId Next() {
    for (;;) {
      OrderId id = rng.Next32();
      if (used.insert(id).second) return id;
    }
  }
};

// Distance from top of book in ticks, 0..99, quadratically skewed toward 0.
Price SkewDepth(Rng &r) {
  uint32_t u = r.Next32() & 1023;
  return static_cast<Price>((u * u * 100u) >> 20);
}

Order MakeResting(Rng &r, IdGen &ids, Quantity qmax) {
  const Side side = (r.Next32() & 1) ? Side::Buy : Side::Sell;
  const Price d = SkewDepth(r);
  const Price p = side == Side::Buy ? kBidTop - d : kAskTop + d;
  return Order{p, ids.Next(), 1 + r.Next32() % qmax, side};
}

// Taker: limit price = top + skewed offset on the crossing side (aggressive
// limit) when far==false, or the far edge of the domain when far==true
// (market-like; sweeps as deep as qty allows).
Order MakeTaker(Rng &r, IdGen &ids, Quantity qmin, Quantity qmax, bool far) {
  const Side side = (r.Next32() & 1) ? Side::Buy : Side::Sell;
  const Price off = far ? 99 : SkewDepth(r);
  const Price p = side == Side::Buy ? kAskTop + off : kBidTop - off;
  return Order{p, ids.Next(), qmin + r.Next32() % (qmax - qmin + 1), side};
}

template <class T> void Shuffle(std::vector<T> &v, Rng &r) {
  for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[r.Next32() % i]);
}

size_t DistinctPrices(const std::vector<Order> &a, const std::vector<Order> &b,
                      Side s) {
  std::unordered_set<Price> p;
  for (auto &o : a) if (o.side == s) p.insert(o.price);
  for (auto &o : b) if (o.side == s) p.insert(o.price);
  return p.size();
}

struct Cycle {
  std::vector<Order> seeds;        // untimed: rest on book before timed phase
  std::vector<Order> takers;       // timed (match) / timed adds (add bench)
  std::vector<OrderId> cancel;     // timed cancel list (cancel bench)
  std::vector<OrderId> cleanup;    // untimed: cancel everything we ever added
  size_t bid_levels_ub{}, ask_levels_ub{};  // distinct prices => level bound
};


constexpr size_t kAddN = 2048;          // timed AddOrder per iteration
constexpr size_t kCancelN = 2048;       // real cancels per iteration
constexpr size_t kCancelMiss = 128;     // + unknown ids (1/16 misses)
constexpr size_t kFullSeed = 2048, kFullTakers = 2048;
constexpr size_t kSweepSeed = 4096, kSweepTakers = 128;

std::vector<Cycle> BuildAdd() {
  std::vector<Cycle> ring;
  IdGen ids;
  Rng r{0x1234567887654321ULL};
  for (size_t c = 0; c < kRing; ++c) {  // resting add
    Cycle cy;
    for (size_t i = 0; i < kAddN; ++i) cy.takers.push_back(MakeResting(r, ids, 100));
    for (auto &o : cy.takers) cy.cleanup.push_back(o.order_id);
    Shuffle(cy.cleanup, r);
    cy.bid_levels_ub = DistinctPrices(cy.takers, {}, Side::Buy);
    cy.ask_levels_ub = DistinctPrices(cy.takers, {}, Side::Sell);
    ring.push_back(std::move(cy));
  }
  return ring;
}

std::vector<Cycle> BuildCancel() {
  std::vector<Cycle> ring;
  IdGen ids;
  Rng r{0x1234567887654321ULL};
  for (size_t c = 0; c < kRing; ++c) {  // cancel
    Cycle cy;
    for (size_t i = 0; i < kCancelN; ++i) cy.seeds.push_back(MakeResting(r, ids, 100));
    for (auto &o : cy.seeds) cy.cancel.push_back(o.order_id);
    for (size_t i = 0; i < kCancelMiss; ++i) cy.cancel.push_back(ids.Next());
    Shuffle(cy.cancel, r);
    cy.bid_levels_ub = DistinctPrices(cy.seeds, {}, Side::Buy);
    cy.ask_levels_ub = DistinctPrices(cy.seeds, {}, Side::Sell);
    ring.push_back(std::move(cy));
  }
  return ring;
}

std::vector<Cycle> BuildMatch(size_t seeds, size_t takers, Quantity qmin,
                              Quantity qmax, bool far) {
  std::vector<Cycle> out;
  IdGen ids;
  Rng r{0x1234567887654321ULL};
  {
    for (size_t c = 0; c < kRing; ++c) {
      Cycle cy;
      for (size_t i = 0; i < seeds; ++i) cy.seeds.push_back(MakeResting(r, ids, 100));
      for (size_t i = 0; i < takers; ++i) cy.takers.push_back(MakeTaker(r, ids, qmin, qmax, far));
      for (auto &o : cy.seeds) cy.cleanup.push_back(o.order_id);
      for (auto &o : cy.takers) cy.cleanup.push_back(o.order_id);
      Shuffle(cy.cleanup, r);
      cy.bid_levels_ub = DistinctPrices(cy.seeds, cy.takers, Side::Buy);
      cy.ask_levels_ub = DistinctPrices(cy.seeds, cy.takers, Side::Sell);
      out.push_back(std::move(cy));
    }
  };
  return out;
}

using Clock = std::chrono::steady_clock;

struct MatchStats {
  uint64_t trades{0};
  uint64_t volume{0};
  uint64_t rested{0};  // takers that left a remainder on the book
};

// ---- hot-path phases (noinline so they are separable in perf) -------------
[[gnu::noinline]] void SeedPhase(Book &b, const std::vector<Order> &orders) {
  uint64_t trades = 0;
  for (const Order &src : orders) {
    Order o = src;
    b.AddOrder(o, [&](const Trade &) { ++trades; });
  }
  benchmark::DoNotOptimize(trades);
}

[[gnu::noinline]] void AddPhase(Book &b, const std::vector<Order> &orders,
                                MatchStats &st) {
  uint64_t trades = 0;
  for (const Order &src : orders) {
    Order o = src;
    b.AddOrder(o, [&](const Trade &) { ++trades; });
  }
  st.trades += trades;
}

[[gnu::noinline]] uint64_t CancelPhase(Book &b, const std::vector<OrderId> &ids) {
  uint64_t hits = 0;
  for (OrderId id : ids) hits += b.CancelOrder(id);
  return hits;
}

[[gnu::noinline]] void MatchPhase(Book &b, const std::vector<Order> &orders,
                                  MatchStats &st) {
  uint64_t trades = 0, vol = 0, rested = 0;
  for (const Order &src : orders) {
    Order o = src;
    b.AddOrder(o, [&](const Trade &t) { ++trades; vol += t.quantity; });
    rested += (o.quantity != 0);
  }
  st.trades += trades;
  st.volume += vol;
  st.rested += rested;
}

[[gnu::noinline]] void CleanupPhase(Book &b, const std::vector<OrderId> &ids) {
  uint64_t hits = 0;
  for (OrderId id : ids) hits += b.CancelOrder(id);
  benchmark::DoNotOptimize(hits);
}

double Seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

void Finish(benchmark::State &state, const Book &book, uint64_t ops_per_iter,
            const std::vector<Cycle> &ring) {
  const uint64_t total = ops_per_iter * state.iterations();
  state.SetItemsProcessed(total);
  state.counters["ops_total"] = static_cast<double>(total);
  size_t bl = 0, al = 0;
  for (auto &c : ring) { bl = std::max(bl, c.bid_levels_ub); al = std::max(al, c.ask_levels_ub); }
  state.counters["peak_bid_lvls_ub"] = static_cast<double>(bl);
  state.counters["peak_ask_lvls_ub"] = static_cast<double>(al);
  if (book.BestBid() != 0 || book.BestAsk() != 0)
    state.SkipWithError("book not empty at end: cycle did not restore state");
}

// 1. Resting AddOrder: 2048 non-crossing adds into an EMPTY book, ids random,
//    prices skewed to top, both sides. Timed: the adds. Untimed: cancel all.
void BM_RestingAdd(benchmark::State &state) {
  static const std::vector<Cycle> ring = BuildAdd();
  auto book = std::make_unique<Book>(kMaxOrders, Alloc());
  MatchStats st;
  size_t it = 0;
  for (auto _ : state) {
    const Cycle &c = ring[it++ & (kRing - 1)];
    auto t0 = Clock::now();
    AddPhase(*book, c.takers, st);
    benchmark::DoNotOptimize(book->BestBid());
    benchmark::DoNotOptimize(book->BestAsk());
    benchmark::ClobberMemory();
    auto t1 = Clock::now();
    state.SetIterationTime(Seconds(t0, t1));
    CleanupPhase(*book, c.cleanup);
  }
  if (st.trades != 0) state.SkipWithError("resting add produced trades");
  Finish(state, *book, kAddN, ring);
}

// 2. CancelOrder: untimed seed of 2048 resting orders; timed: 2176 cancels in
//    random order (2048 hits + 128 unknown ids => miss path). Book ends empty.
void BM_Cancel(benchmark::State &state) {
  static const std::vector<Cycle> ring = BuildCancel();
  auto book = std::make_unique<Book>(kMaxOrders, Alloc());
  uint64_t hits = 0;
  size_t it = 0;
  for (auto _ : state) {
    const Cycle &c = ring[it++ & (kRing - 1)];
    SeedPhase(*book, c.seeds);
    auto t0 = Clock::now();
    hits += CancelPhase(*book, c.cancel);
    benchmark::DoNotOptimize(hits);
    benchmark::ClobberMemory();
    auto t1 = Clock::now();
    state.SetIterationTime(Seconds(t0, t1));
  }
  if (hits != kCancelN * state.iterations())
    state.SkipWithError("unexpected cancel hit count");
  Finish(state, *book, kCancelN + kCancelMiss, ring);
}

// 3/4. Matching. Untimed: seed resting liquidity. Timed: taker AddOrders.
//      Untimed: cancel every id ever added (already-filled ids just miss).
void RunMatch(benchmark::State &state, const std::vector<Cycle> &ring,
              uint64_t takers_per_iter) {
  auto book = std::make_unique<Book>(kMaxOrders, Alloc());
  MatchStats st;
  size_t it = 0;
  for (auto _ : state) {
    const Cycle &c = ring[it++ & (kRing - 1)];
    SeedPhase(*book, c.seeds);
    auto t0 = Clock::now();
    MatchPhase(*book, c.takers, st);
    benchmark::DoNotOptimize(st);
    benchmark::DoNotOptimize(book->BestBid());
    benchmark::DoNotOptimize(book->BestAsk());
    benchmark::ClobberMemory();
    auto t1 = Clock::now();
    state.SetIterationTime(Seconds(t0, t1));
    CleanupPhase(*book, c.cleanup);
  }
  const double iters = static_cast<double>(state.iterations());
  state.counters["trades_per_taker"] = st.trades / (iters * takers_per_iter);
  state.counters["rested_frac"] = st.rested / (iters * takers_per_iter);
  Finish(state, *book, takers_per_iter, ring);
}

// 3. Takers qty 1..100 at aggressive limit prices near top: mostly full fills
//    of 1-2 resting orders at the top levels; late takers (book thinned) rest
//    their remainder (partial/no fill). Mix is reported via counters.
void BM_Match_TopOfBook(benchmark::State &state) {
  static const auto ring = BuildMatch(kFullSeed, kFullTakers, 1, 100, false);
  RunMatch(state, ring, kFullTakers);
}

// 4. Takers qty 500..3000 at the far limit: each walks several price levels
//    (~10-60 resting orders), erasing levels from the end of the buffer;
//    final taker remainder rests when the side is exhausted.
void BM_Match_Sweep(benchmark::State &state) {
  static const auto ring = BuildMatch(kSweepSeed, kSweepTakers, 500, 3000, true);
  RunMatch(state, ring, kSweepTakers);
}

}  // namespace

BENCHMARK(BM_RestingAdd)->UseManualTime()->Iterations(1024)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Cancel)->UseManualTime()->Iterations(1024)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Match_TopOfBook)->UseManualTime()->Iterations(1024)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Match_Sweep)->UseManualTime()->Iterations(10240)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
