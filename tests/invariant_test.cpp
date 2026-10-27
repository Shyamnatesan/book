// Invariant tests for FastBook, through the public API only. Expected values
// come from a reference price-time book (Ref), never from FastBook itself.
//
// Invariants covered:
//   I1 bids sorted, best bid last      -> Depth(Buy) strictly descending best-first
//   I2 asks sorted, best ask last      -> Depth(Sell) strictly ascending best-first
//   I3 never crossed                   -> BestBid < BestAsk whenever both exist
//   I4 level aggregates match orders   -> each level's quantity/order_count equals
//                                         the sum over its resting orders
//   I5 cancel removes exactly one order
//   I6 price-time priority             -> trades equal the reference book's
// plus CheckInvariants() == "" after every operation of every randomized run.
#include "fastbook.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace shyamnatesan;

static const char *g_test = "";
static int g_test_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("  FAIL [%s] line %d: %s\n", g_test, __LINE__, #cond);       \
      ++g_test_failures;                                                       \
    }                                                                          \
  } while (0)

namespace {

constexpr size_t kLevels = 64;
using Book = FastBook<std::allocator<Order>, kLevels>;

struct Ref {
  struct Resting { OrderId id; Quantity qty; };
  std::map<Price, std::deque<Resting>> bids, asks;
  std::map<OrderId, std::pair<Side, Price>> live;
  std::vector<Trade> trades;

  Quantity Add(OrderId id, Side side, Price price, Quantity qty) {
    if (live.count(id)) return qty;
    const bool buy = side == Side::Buy;
    auto &opp = buy ? asks : bids;
    while (qty > 0 && !opp.empty()) {
      auto it = buy ? opp.begin() : std::prev(opp.end());
      if (buy ? it->first > price : it->first < price) break;
      auto &q = it->second;
      while (qty > 0 && !q.empty()) {
        const Quantity fill = std::min(qty, q.front().qty);
        qty -= fill;
        q.front().qty -= fill;
        trades.push_back(Trade{it->first, fill, id, q.front().id});
        if (q.front().qty == 0) {
          live.erase(q.front().id);
          q.pop_front();
        }
      }
      if (q.empty()) opp.erase(it);
    }
    if (qty > 0) {
      (buy ? bids : asks)[price].push_back({id, qty});
      live[id] = {side, price};
    }
    return qty;
  }

  bool Cancel(OrderId id) {
    auto it = live.find(id);
    if (it == live.end()) return false;
    auto &side = it->second.first == Side::Buy ? bids : asks;
    auto &q = side[it->second.second];
    for (auto o = q.begin(); o != q.end(); ++o)
      if (o->id == id) { q.erase(o); break; }
    if (q.empty()) side.erase(it->second.second);
    live.erase(it);
    return true;
  }
};

std::vector<LevelView> DepthOf(const Book &b, Side s) {
  std::vector<LevelView> out(kLevels);
  out.resize(b.Depth(s, std::span<LevelView>(out)));
  return out;
}

// Every observable invariant of `b`, with expected values taken from `ref`.
void CheckAll(const Book &b, const Ref &ref) {
  CHECK(b.CheckInvariants().empty());
  if (!b.CheckInvariants().empty())
    std::printf("    CheckInvariants: %.*s\n", (int)b.CheckInvariants().size(),
                b.CheckInvariants().data());

  const auto bids = DepthOf(b, Side::Buy), asks = DepthOf(b, Side::Sell);
  for (size_t i = 1; i < bids.size(); ++i) CHECK(bids[i - 1].price > bids[i].price); // I1
  for (size_t i = 1; i < asks.size(); ++i) CHECK(asks[i - 1].price < asks[i].price); // I2
  if (!bids.empty() && !asks.empty()) CHECK(bids[0].price < asks[0].price);          // I3
  CHECK(b.BestBid() == (bids.empty() ? 0 : bids[0].price));
  CHECK(b.BestAsk() == (asks.empty() ? 0 : asks[0].price));

  for (int s = 0; s < 2; ++s) {
    const auto &got = s == 0 ? bids : asks;
    const auto &want = s == 0 ? ref.bids : ref.asks;
    CHECK(got.size() == want.size());
    CHECK(b.LevelCount(s == 0 ? Side::Buy : Side::Sell) == want.size());
    for (const LevelView &lv : got) {
      auto it = want.find(lv.price);
      CHECK(it != want.end());
      if (it == want.end()) continue;
      Quantity sum = 0, sum_seen = 0;
      for (const auto &r : it->second) {
        sum += r.qty;
        auto v = b.FindOrder(r.id);
        CHECK(v && v->price == lv.price && v->quantity == r.qty &&
              v->side == (s == 0 ? Side::Buy : Side::Sell));
        if (v) sum_seen += v->quantity;
      }
      CHECK(lv.quantity == sum);                    // I4 against the reference
      CHECK(lv.quantity == sum_seen);               // I4 against the book's own orders
      CHECK(lv.order_count == it->second.size());
    }
  }
  CHECK(b.OrderCount() == ref.live.size());
}

struct Rng {
  uint32_t s;
  uint32_t Below(uint32_t n) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s % n;
  }
};

// One randomized run: aggressive and passive orders around a mid price,
// cancels of live, dead and unknown ids; full check after every operation.
void RandomRun(uint32_t seed, size_t max_orders, OrderId id_stride, OrderId id_base) {
  Book book(max_orders, std::allocator<Order>());
  Ref ref;
  Rng r{seed};
  std::vector<Trade> trades;
  for (int step = 0; step < 3000; ++step) {
    const OrderId id = id_base + id_stride * (1 + r.Below(400));
    if (r.Below(10) < 3) {
      CHECK(book.CancelOrder(id) == ref.Cancel(id));
    } else if (ref.live.size() + 1 < max_orders) {
      const Side side = r.Below(2) ? Side::Buy : Side::Sell;
      const Price price = 1000 + static_cast<Price>(r.Below(40));
      const Quantity qty = 1 + r.Below(20);
      Order o{price, id, qty, side};
      trades.clear();
      ref.trades.clear();
      book.AddOrder(o, [&](const Trade &t) { trades.push_back(t); });
      CHECK(o.quantity == ref.Add(id, side, price, qty));
      CHECK(trades.size() == ref.trades.size());                         // I6
      for (size_t i = 0; i < trades.size() && i < ref.trades.size(); ++i)
        CHECK(trades[i].price == ref.trades[i].price && trades[i].quantity == ref.trades[i].quantity &&
              trades[i].taker == ref.trades[i].taker && trades[i].maker == ref.trades[i].maker);
    }
    CheckAll(book, ref);
    if (g_test_failures) {
      std::printf("    seed %u step %d\n", seed, step);
      return;
    }
  }
}

void RandomizedInvariants() {
  for (uint32_t seed = 1; seed <= 20; ++seed) RandomRun(seed, 300, 1, 0);
}

// Ids that share low bits (stride 4096) and ids from a far range must work
// like any others.
void RandomizedInvariantsStridedIds() {
  for (uint32_t seed = 21; seed <= 30; ++seed) RandomRun(seed, 300, 4096, 0);
  for (uint32_t seed = 31; seed <= 40; ++seed) RandomRun(seed, 300, 1, 3'000'000'000u);
}

void DepthIsBestFirst() {
  Book book(64, {});
  Ref ref;
  for (Price p : {100, 103, 101, 102}) {
    Order o{p, static_cast<OrderId>(p), 1, Side::Buy};
    book.AddOrder(o, [](const Trade &) {});
    ref.Add(o.order_id, Side::Buy, p, 1);
  }
  for (Price p : {110, 107, 109, 108}) {
    Order o{p, static_cast<OrderId>(p), 1, Side::Sell};
    book.AddOrder(o, [](const Trade &) {});
    ref.Add(o.order_id, Side::Sell, p, 1);
  }
  const auto bids = DepthOf(book, Side::Buy), asks = DepthOf(book, Side::Sell);
  CHECK(bids.size() == 4 && bids[0].price == 103 && bids[3].price == 100);
  CHECK(asks.size() == 4 && asks[0].price == 107 && asks[3].price == 110);
  std::array<LevelView, 2> top{};
  CHECK(book.Depth(Side::Buy, top) == 2 && top[0].price == 103 && top[1].price == 102);
  CheckAll(book, ref);
}

void CrossingOrderTradesInsteadOfResting() {
  Book book(64, {});
  Ref ref;
  auto add = [&](OrderId id, Side s, Price p, Quantity q) {
    Order o{p, id, q, s};
    book.AddOrder(o, [](const Trade &) {});
    ref.Add(id, s, p, q);
  };
  add(1, Side::Sell, 101, 5);
  add(2, Side::Buy, 99, 5);
  add(3, Side::Buy, 105, 3);  // crosses: takes 3 at 101, nothing rests at 105
  CHECK(book.BestBid() == 99 && book.BestAsk() == 101 && book.BestAskQty() == 2);
  add(4, Side::Sell, 90, 10); // crosses: takes 99 x 5, remainder 5 rests at 90
  CHECK(book.BestAsk() == 90 && book.BestBid() == 0);
  CheckAll(book, ref);
}

void CancelRemovesExactlyOne() {
  Book book(64, {});
  Ref ref;
  auto add = [&](OrderId id, Side s, Price p, Quantity q) {
    Order o{p, id, q, s};
    book.AddOrder(o, [](const Trade &) {});
    ref.Add(id, s, p, q);
  };
  // Ids 8, 16, 24 ... collide in a power-of-two table under a masking hash.
  for (OrderId k = 1; k <= 5; ++k) add(k * 8, Side::Buy, 100, k);
  add(100, Side::Buy, 99, 7);
  add(200, Side::Sell, 101, 9);
  const auto before = DepthOf(book, Side::Buy);
  const size_t orders_before = book.OrderCount();

  CHECK(book.CancelOrder(24));
  ref.Cancel(24);
  CHECK(!book.FindOrder(24));
  CHECK(book.OrderCount() == orders_before - 1);
  const auto after = DepthOf(book, Side::Buy);
  CHECK(after.size() == before.size());
  CHECK(after[0].quantity == before[0].quantity - 3 && after[0].order_count == before[0].order_count - 1);
  CHECK(after[1].quantity == before[1].quantity && after[1].order_count == before[1].order_count);
  for (OrderId id : {8u, 16u, 32u, 40u, 100u, 200u}) CHECK(book.FindOrder(id).has_value());
  CheckAll(book, ref);

  CHECK(!book.CancelOrder(24)); // a second cancel of the same id removes nothing
  CHECK(book.OrderCount() == orders_before - 1);
  CHECK(!book.CancelOrder(12345));
  CheckAll(book, ref);

  // FIFO of the survivors is intact: a taker fills 8, 16, 32 in that order.
  std::vector<OrderId> makers;
  Order t{100, 999, 1 + 2 + 4, Side::Sell};
  book.AddOrder(t, [&](const Trade &tr) { makers.push_back(tr.maker); });
  ref.Add(999, Side::Sell, 100, 7);
  CHECK((makers == std::vector<OrderId>{8, 16, 32}));
  CheckAll(book, ref);
}

void PriceTimePriorityAcrossLevels() {
  Book book(64, {});
  std::vector<Trade> trades;
  auto add = [&](OrderId id, Side s, Price p, Quantity q) {
    Order o{p, id, q, s};
    book.AddOrder(o, [&](const Trade &t) { trades.push_back(t); });
    return o.quantity;
  };
  add(1, Side::Sell, 102, 2);
  add(2, Side::Sell, 101, 2);
  add(3, Side::Sell, 101, 2); // same price, later
  add(4, Side::Sell, 100, 1); // better price, latest
  trades.clear();
  CHECK(add(5, Side::Buy, 102, 6) == 0);
  CHECK(trades.size() == 4);
  if (trades.size() == 4) {
    CHECK(trades[0].maker == 4 && trades[0].price == 100 && trades[0].quantity == 1);
    CHECK(trades[1].maker == 2 && trades[1].price == 101 && trades[1].quantity == 2);
    CHECK(trades[2].maker == 3 && trades[2].price == 101 && trades[2].quantity == 2);
    CHECK(trades[3].maker == 1 && trades[3].price == 102 && trades[3].quantity == 1);
  }
  CHECK(book.BestAsk() == 102 && book.BestAskQty() == 1);
}

// max_orders need not be a power of two: exactly that many orders rest.
void AnyCapacity() {
  Book book(1000, {});
  int ok = 0;
  for (OrderId id = 1; id <= 1001; ++id) {
    Order o{static_cast<Price>(100 + id % 50), id * 4096u, 1, Side::Buy};
    ok += book.AddOrder(o, [](const Trade &) {}) == AddResult::Ok;
  }
  CHECK(ok == 1000);
  CHECK(book.OrderCount() == 1000);
  Order extra{100, 7, 1, Side::Buy};
  CHECK(book.AddOrder(extra, [](const Trade &) {}) == AddResult::PoolFull);
  CHECK(book.CheckInvariants().empty());
}

void StatsCountEvents() {
  Book book(4, {});
  auto add = [&](OrderId id, Side s, Price p, Quantity q) {
    Order o{p, id, q, s};
    return book.AddOrder(o, [](const Trade &) {});
  };
  add(1, Side::Sell, 100, 5);
  add(2, Side::Buy, 100, 2);         // 1 trade of 2
  add(1, Side::Buy, 90, 1);          // Duplicate
  book.CancelOrder(1);
  book.CancelOrder(1);               // miss: not counted as a cancel
  const BookStats &s = book.Stats();
  CHECK(s.trades == 1 && s.traded_quantity == 2);
  CHECK(s.cancels == 1);
  CHECK(s.rejects[static_cast<size_t>(AddResult::Duplicate)] == 1);
  CHECK(std::string(ToString(AddResult::PoolFull)) == "PoolFull");
}

struct TestCase { const char *name; void (*fn)(); };

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const TestCase tests[] = {
      {"DepthIsBestFirst", DepthIsBestFirst},
      {"CrossingOrderTradesInsteadOfResting", CrossingOrderTradesInsteadOfResting},
      {"CancelRemovesExactlyOne", CancelRemovesExactlyOne},
      {"PriceTimePriorityAcrossLevels", PriceTimePriorityAcrossLevels},
      {"AnyCapacity", AnyCapacity},
      {"StatsCountEvents", StatsCountEvents},
      {"RandomizedInvariants", RandomizedInvariants},
      {"RandomizedInvariantsStridedIds", RandomizedInvariantsStridedIds},
  };
  int failed = 0;
  for (const TestCase &t : tests) {
    g_test = t.name;
    g_test_failures = 0;
    std::printf("run    %s\n", t.name);
    t.fn();
    if (g_test_failures) {
      ++failed;
      std::printf("FAILED %s (%d checks)\n", t.name, g_test_failures);
    } else {
      std::printf("ok     %s\n", t.name);
    }
  }
  const int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
  std::printf("%d/%d tests passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
