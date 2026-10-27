#include "fastbook.hpp"
#include "order.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

using namespace shyamnatesan;

namespace {

using Alloc = std::allocator<std::byte>;
using Book = FastBook<Alloc, 256>;

constexpr size_t kMaxOrders = 1 << 21; // power of two, >= largest scenario
constexpr int kNumLevels = 100;
constexpr Price kBasePrice = 10000;
constexpr Quantity kQty = 10;

inline Order MakeOrder(Price price, OrderId id, Quantity qty, Side side) {
  return Order{.price = price, .order_id = id, .quantity = qty, .side = side};
}

inline void NoTrade(const Trade &) {}

// AddOrder takes Order& and may modify it, so each submission needs an lvalue.
template <typename F>
void Submit(Book &book, Price price, OrderId id, Quantity qty, Side side,
            F &&on_trade) {
  Order order = MakeOrder(price, id, qty, side);
  book.AddOrder(order, std::forward<F>(on_trade));
}

} // namespace

// Adds N buy orders spread across 100 existing price levels.
static void BM_AddToExistingLevel(benchmark::State &state) {
  const int N = static_cast<int>(state.range(0));
  Alloc alloc;
  std::unique_ptr<Book> holder;

  for (auto _ : state) {
    state.PauseTiming();
    holder = std::make_unique<Book>(kMaxOrders, alloc); // old book freed here
    Book &book = *holder;
    OrderId next_id = 1;
    for (int i = 0; i < kNumLevels; ++i) {
      Submit(book, kBasePrice + i, next_id++, 1, Side::Buy, NoTrade);
    }
    state.ResumeTiming();

    for (int i = 0; i < N; ++i) {
      Submit(book, kBasePrice + (i % kNumLevels), next_id++, kQty, Side::Buy,
             NoTrade);
    }

    benchmark::DoNotOptimize(book.BestBid());
    benchmark::ClobberMemory();

    if (book.BestAsk() != 0) {
      state.SkipWithError("AddToExistingLevel: unexpected ask");
      return;
    }
  }

  state.SetItemsProcessed(state.iterations() * N);
}

// Cancels N resting orders by ID.
// Args: {N, shuffled}
static void BM_CancelById(benchmark::State &state) {
  const int N = static_cast<int>(state.range(0));
  const bool shuffled = state.range(1) != 0;
  Alloc alloc;
  std::mt19937 rng(12345);
  std::unique_ptr<Book> holder;

  for (auto _ : state) {
    state.PauseTiming();
    holder = std::make_unique<Book>(kMaxOrders, alloc);
    Book &book = *holder;
    std::vector<OrderId> ids(N);
    std::iota(ids.begin(), ids.end(), OrderId{1});
    for (int i = 0; i < N; ++i) {
      Submit(book, kBasePrice + (i % kNumLevels), ids[i], kQty, Side::Buy,
             NoTrade);
    }
    if (shuffled) {
      std::shuffle(ids.begin(), ids.end(), rng);
    }
    state.ResumeTiming();

    size_t cancelled = 0;
    for (OrderId id : ids) {
      cancelled += book.CancelOrder(id) ? 1 : 0;
    }

    benchmark::DoNotOptimize(cancelled);
    benchmark::DoNotOptimize(book.BestBid());
    benchmark::ClobberMemory();

    if (cancelled != static_cast<size_t>(N)) {
      state.SkipWithError("CancelById: not every order was cancelled");
      return;
    }
  }

  state.SetItemsProcessed(state.iterations() * N);
}

// N aggressive buys, each consuming one resting ask at the best price.
static void BM_TakeTopOfBookDeep(benchmark::State &state) {
  const int N = static_cast<int>(state.range(0));
  constexpr Price BestAsk = kBasePrice;
  Alloc alloc;
  std::unique_ptr<Book> holder;

  for (auto _ : state) {
    state.PauseTiming();
    holder = std::make_unique<Book>(kMaxOrders, alloc);
    Book &book = *holder;
    OrderId next_id = 1;
    for (int i = 0; i < N; ++i) {
      Submit(book, BestAsk, next_id++, kQty, Side::Sell, NoTrade);
    }
    Submit(book, BestAsk + 1, next_id++, kQty, Side::Sell, NoTrade);
    state.ResumeTiming();

    uint64_t trades = 0;
    auto on_trade = [&trades](const Trade &) { ++trades; };
    for (int i = 0; i < N; ++i) {
      Submit(book, BestAsk, next_id++, kQty, Side::Buy, on_trade);
    }

    benchmark::DoNotOptimize(trades);
    benchmark::DoNotOptimize(book.BestAsk());
    benchmark::ClobberMemory();

    if (trades != static_cast<uint64_t>(N)) {
      state.SkipWithError("TakeTopOfBookDeep: unexpected trade count");
      return;
    }
  }

  state.SetItemsProcessed(state.iterations() * N);
}

// One aggressive buy that sweeps all 10 ask levels, each with `depth` orders.
// Arg: depth (orders per level).
static void BM_SweepTenLevels(benchmark::State &state) {
  const OrderId depth = static_cast<OrderId>(state.range(0));
  constexpr int kLevels = 10;
  constexpr Price StartAsk = kBasePrice;
  const uint64_t expected_trades = static_cast<uint64_t>(depth) * kLevels;
  const Quantity sweep_qty = static_cast<Quantity>(expected_trades) * kQty;
  Alloc alloc;
  std::unique_ptr<Book> holder;

  for (auto _ : state) {
    state.PauseTiming();
    holder = std::make_unique<Book>(kMaxOrders, alloc);
    Book &book = *holder;
    OrderId next_id = 1;
    for (int lvl = 0; lvl < kLevels; ++lvl) {
      for (OrderId i = 0; i < depth; ++i) {
        Submit(book, StartAsk + lvl, next_id++, kQty, Side::Sell, NoTrade);
      }
    }
    state.ResumeTiming();

    uint64_t trades = 0;
    auto on_trade = [&trades](const Trade &) { ++trades; };
    Submit(book, StartAsk + kLevels - 1, next_id++, sweep_qty, Side::Buy,
           on_trade);

    benchmark::DoNotOptimize(trades);
    benchmark::DoNotOptimize(book.BestAsk());
    benchmark::ClobberMemory();

    if (trades != expected_trades || book.BestAsk() != 0) {
      state.SkipWithError("SweepTenLevels: sweep did not clear all levels");
      return;
    }
  }

  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(expected_trades));
}

BENCHMARK(BM_AddToExistingLevel)
    ->Arg(1'000'000)
    ->Iterations(10)
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_CancelById)
    ->ArgsProduct({{1'000'000}, {0, 1}})
    ->Iterations(10)
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_TakeTopOfBookDeep)
    ->Arg(100'000)
    ->Iterations(20)
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_SweepTenLevels)
    ->Arg(1)
    ->Arg(1'000)
    ->Arg(10'000)
    ->Iterations(50)
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
