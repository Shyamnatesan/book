// Orderbook test suite. Uses only the public API of FastBook (AddOrder,
// CancelOrder, BestBid/Ask, BestBid/AskQty) plus OpenAddressingHashMap.
// Every test runs; every failure is reported with the test name.
#include "fastbook.hpp"

#include <initializer_list>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>

using namespace shyamnatesan;

// ---------------------------------------------------------------- harness
static const char *g_test = "";
static int g_test_failures = 0;

#define CHECK(cond)                                                            \
  if (!(cond)) {                                                               \
    std::printf("  FAIL [%s] line %d: %s\n", g_test, __LINE__, #cond);         \
    ++g_test_failures;                                                         \
  }

// ---------------------------------------------------------------- helpers
using Book = FastBook<std::allocator<Order>, 64>;

struct Fixture {
  Book book;
  std::vector<Trade> trades;

  explicit Fixture(size_t max_orders = 64)
      : book(max_orders, std::allocator<Order>()) {}

  // Returns the quantity left unfilled (what would rest on the book).
  Quantity Add(OrderId id, Side side, Price price, Quantity qty) {
    Order o{price, id, qty, side};
    book.AddOrder(o, [&](const Trade &t) { trades.push_back(t); });
    return o.quantity;
  }
  bool Buy(OrderId id, Price p, Quantity q) { return Add(id, Side::Buy, p, q) == 0; }
  Quantity Sell(OrderId id, Price p, Quantity q) { return Add(id, Side::Sell, p, q); }
  bool Cancel(OrderId id) { return book.CancelOrder(id); }

  bool Crossed() const {
    return book.BestBid() != 0 && book.BestAsk() != 0 &&
           book.BestBid() >= book.BestAsk();
  }
  bool IsEmpty() const {
    return book.BestBid() == 0 && book.BestAsk() == 0 &&
           book.BestBidQty() == 0 && book.BestAskQty() == 0;
  }
};

static bool TradeIs(const Trade &t, Price price, Quantity qty, OrderId taker,
                    OrderId maker) {
  return t.price == price && t.quantity == qty && t.taker == taker &&
         t.maker == maker;
}

// ------------------------------------------------------------------ tests

// Empty-book queries and cancel on an empty book.
static void EmptyBook() {
  Fixture f;
  CHECK(f.book.BestBid() == 0);
  CHECK(f.book.BestAsk() == 0);
  CHECK(f.book.BestBidQty() == 0);
  CHECK(f.book.BestAskQty() == 0);
  CHECK(!f.Cancel(1));
  CHECK(f.IsEmpty());
}

// Resting adds on both sides, no trades, not crossed.
static void RestingAddsBothSides() {
  Fixture f;
  CHECK(f.Add(1, Side::Buy, 100, 5) == 5);
  CHECK(f.Add(2, Side::Sell, 105, 7) == 7);
  CHECK(f.trades.empty());
  CHECK(f.book.BestBid() == 100);
  CHECK(f.book.BestBidQty() == 5);
  CHECK(f.book.BestAsk() == 105);
  CHECK(f.book.BestAskQty() == 7);
  CHECK(!f.Crossed());
}

// Invariants 1 and 2: bids ascending / asks descending, best at the end.
// Insert in scrambled order, then peel the best level off by cancelling and
// check the best price walks strictly in the right direction.
static void SortedBidsAndAsks() {
  Fixture f;
  const Price prices[] = {100, 97, 103, 99, 101, 95, 102, 98, 96, 104};
  OrderId id = 1;
  for (Price p : prices) {
    f.Add(id, Side::Buy, p - 50, 1);        // bids 45..54, below all asks
    f.Add(id + 100, Side::Sell, p + 50, 1); // asks 145..154
    ++id;
  }
  // Bids: best bid must be the max of what remains.
  for (Price expect = 54; expect >= 45; --expect) {
    CHECK(f.book.BestBid() == expect);
    // the order for price `expect` is the one with prices[i] == expect + 50
    OrderId victim = 0;
    for (size_t i = 0; i < 10; ++i)
      if (prices[i] - 50 == expect) victim = static_cast<OrderId>(i + 1);
    CHECK(f.Cancel(victim));
  }
  CHECK(f.book.BestBid() == 0);
  // Asks: best ask must be the min of what remains.
  for (Price expect = 145; expect <= 154; ++expect) {
    CHECK(f.book.BestAsk() == expect);
    OrderId victim = 0;
    for (size_t i = 0; i < 10; ++i)
      if (prices[i] + 50 == expect) victim = static_cast<OrderId>(i + 101);
    CHECK(f.Cancel(victim));
  }
  CHECK(f.book.BestAsk() == 0);
}

// Full fill on both sides, price improvement (trade at resting price).
static void FullFillBothSides() {
  Fixture f;
  f.Add(1, Side::Sell, 100, 5);
  CHECK(f.Add(2, Side::Buy, 110, 5) == 0); // buy taker, fully filled
  CHECK(f.trades.size() == 1);
  CHECK(f.trades.size() == 1 && TradeIs(f.trades[0], 100, 5, 2, 1));
  CHECK(f.IsEmpty());

  f.trades.clear();
  f.Add(3, Side::Buy, 90, 4);
  CHECK(f.Add(4, Side::Sell, 80, 4) == 0); // sell taker, fully filled
  CHECK(f.trades.size() == 1 && TradeIs(f.trades[0], 90, 4, 4, 3));
  CHECK(f.IsEmpty());
}

// Partial fill: taker smaller than maker, and taker larger than maker.
static void PartialFills() {
  Fixture f;
  f.Add(1, Side::Sell, 100, 10);
  CHECK(f.Add(2, Side::Buy, 100, 4) == 0);
  CHECK(f.trades.size() == 1 && TradeIs(f.trades[0], 100, 4, 2, 1));
  CHECK(f.book.BestAsk() == 100);
  CHECK(f.book.BestAskQty() == 6); // maker partially filled, still resting
  CHECK(f.book.BestBid() == 0);    // taker fully consumed, nothing rests

  f.trades.clear();
  f.Add(3, Side::Buy, 90, 3);
  CHECK(f.Add(4, Side::Sell, 90, 8) == 5); // sell taker, 5 left over
  CHECK(f.trades.size() == 1 && TradeIs(f.trades[0], 90, 3, 4, 3));
  CHECK(f.book.BestBid() == 0);
  CHECK(f.book.BestAsk() == 90); // remainder rests at its limit, below 100
  CHECK(f.book.BestAskQty() == 5);
}

// Multi-level sweep on both sides, with the remainder resting at its limit.
static void MultiLevelSweepAndRemainder() {
  Fixture f;
  f.Add(1, Side::Sell, 101, 5);
  f.Add(2, Side::Sell, 102, 5);
  f.Add(3, Side::Sell, 103, 5);
  // Buy limit 103 for 12: takes 101, 102 fully and 2 of 103.
  CHECK(f.Add(10, Side::Buy, 103, 12) == 0);
  CHECK(f.trades.size() == 3);
  if (f.trades.size() == 3) {
    CHECK(TradeIs(f.trades[0], 101, 5, 10, 1));
    CHECK(TradeIs(f.trades[1], 102, 5, 10, 2));
    CHECK(TradeIs(f.trades[2], 103, 2, 10, 3));
  }
  CHECK(f.book.BestAsk() == 103);
  CHECK(f.book.BestAskQty() == 3);

  // Buy limit 103 for 10: 3 trade, remaining 7 rests as a bid at 103.
  f.trades.clear();
  CHECK(f.Add(11, Side::Buy, 103, 10) == 7);
  CHECK(f.trades.size() == 1 && TradeIs(f.trades[0], 103, 3, 11, 3));
  CHECK(f.book.BestAsk() == 0);
  CHECK(f.book.BestBid() == 103);
  CHECK(f.book.BestBidQty() == 7);
  CHECK(!f.Crossed());

  // Sell sweep, limit stops the sweep: bids 103(7), 102, 101.
  f.Add(12, Side::Buy, 102, 4);
  f.Add(13, Side::Buy, 101, 4);
  f.trades.clear();
  CHECK(f.Add(14, Side::Sell, 102, 20) == 9); // 7 @103 + 4 @102 = 11 traded
  CHECK(f.trades.size() == 2);
  if (f.trades.size() == 2) {
    CHECK(TradeIs(f.trades[0], 103, 7, 14, 11));
    CHECK(TradeIs(f.trades[1], 102, 4, 14, 12));
  }
  CHECK(f.book.BestAsk() == 102); // remainder rests at its own limit
  CHECK(f.book.BestAskQty() == 9);
  CHECK(f.book.BestBid() == 101); // 101 not touched (limit 102)
  CHECK(f.book.BestBidQty() == 4);
  CHECK(!f.Crossed());
}

// Invariant 6: price-time priority. FIFO within a level, partial fill of the
// head keeps its place, better price first across levels.
static void PriceTimePriority() {
  Fixture f;
  f.Add(1, Side::Sell, 100, 5);
  f.Add(2, Side::Sell, 100, 5);
  f.Add(3, Side::Sell, 100, 5);
  f.Add(4, Side::Sell, 99, 2); // better price, later in time
  f.Buy(10, 100, 3);           // 2 @99 from order 4, 1 @100 from order 1
  CHECK(f.trades.size() == 2);
  if (f.trades.size() == 2) {
    CHECK(TradeIs(f.trades[0], 99, 2, 10, 4));
    CHECK(TradeIs(f.trades[1], 100, 1, 10, 1));
  }
  f.trades.clear();
  f.Buy(11, 100, 4); // 4 left of order 1 -> head keeps priority
  CHECK(f.trades.size() == 1 && TradeIs(f.trades[0], 100, 4, 11, 1));
  f.trades.clear();
  f.Buy(12, 100, 7); // order 2 (5) then order 3 (2)
  CHECK(f.trades.size() == 2);
  if (f.trades.size() == 2) {
    CHECK(TradeIs(f.trades[0], 100, 5, 12, 2));
    CHECK(TradeIs(f.trades[1], 100, 2, 12, 3));
  }
  CHECK(f.book.BestAskQty() == 3);

  // Same on the bid side: the highest bid goes first, then oldest.
  Fixture g;
  g.Add(1, Side::Buy, 50, 1);
  g.Add(2, Side::Buy, 51, 1);
  g.Add(3, Side::Buy, 51, 1);
  g.Sell(9, 50, 3);
  CHECK(g.trades.size() == 3);
  if (g.trades.size() == 3) {
    CHECK(TradeIs(g.trades[0], 51, 1, 9, 2));
    CHECK(TradeIs(g.trades[1], 51, 1, 9, 3));
    CHECK(TradeIs(g.trades[2], 50, 1, 9, 1));
  }
}

// Invariant 4: level quantity and order count match the list. Observable via
// BestXxxQty while mutating the top level by add / partial fill / cancel.
static void LevelAggregates() {
  Fixture f;
  f.Add(1, Side::Buy, 100, 3);
  f.Add(2, Side::Buy, 100, 4);
  f.Add(3, Side::Buy, 100, 5);
  f.Add(4, Side::Buy, 99, 9);
  CHECK(f.book.BestBid() == 100);
  CHECK(f.book.BestBidQty() == 12);
  f.Sell(20, 100, 2); // partial fill of head
  CHECK(f.book.BestBidQty() == 10);
  CHECK(f.Cancel(2));
  CHECK(f.book.BestBidQty() == 6); // 1 (3-2) + 5
  CHECK(f.Cancel(3));
  CHECK(f.book.BestBidQty() == 1);
  CHECK(f.Cancel(1));
  CHECK(f.book.BestBid() == 99); // level gone, next level exposed
  CHECK(f.book.BestBidQty() == 9);

  // Asks too.
  f.Add(5, Side::Sell, 120, 2);
  f.Add(6, Side::Sell, 120, 3);
  CHECK(f.book.BestAskQty() == 5);
  CHECK(f.Cancel(5));
  CHECK(f.book.BestAskQty() == 3);
}

// Invariant 5: cancel head / middle / tail removes exactly one order; the
// survivors keep their FIFO order and quantities.
static void CancelHeadMiddleTail() {
  Fixture f;
  for (OrderId id = 1; id <= 5; ++id) f.Add(id, Side::Sell, 100, id); // qty=id
  CHECK(f.book.BestAskQty() == 15);
  CHECK(f.Cancel(3)); // middle
  CHECK(f.book.BestAskQty() == 12);
  CHECK(f.Cancel(1)); // head
  CHECK(f.book.BestAskQty() == 11);
  CHECK(f.Cancel(5)); // tail
  CHECK(f.book.BestAskQty() == 6);
  CHECK(!f.Cancel(3)); // already gone, nothing else disturbed
  CHECK(f.book.BestAskQty() == 6);
  // Remaining are 2 then 4 in that order.
  f.Buy(10, 100, 6);
  CHECK(f.trades.size() == 2);
  if (f.trades.size() == 2) {
    CHECK(TradeIs(f.trades[0], 100, 2, 10, 2));
    CHECK(TradeIs(f.trades[1], 100, 4, 10, 4));
  }
  CHECK(f.IsEmpty());
  // A new order appended after cancelling the tail goes behind the survivors.
  Fixture g;
  g.Add(1, Side::Buy, 10, 1);
  g.Add(2, Side::Buy, 10, 1);
  CHECK(g.Cancel(2));
  g.Add(3, Side::Buy, 10, 1);
  g.Sell(9, 10, 2);
  CHECK(g.trades.size() == 2);
  if (g.trades.size() == 2) {
    CHECK(g.trades[0].maker == 1);
    CHECK(g.trades[1].maker == 3);
  }
}

// Cancel of unknown / already cancelled / already filled ids returns false
// and leaves the book untouched.
static void CancelUnknownAndRepeated() {
  Fixture f;
  f.Add(1, Side::Buy, 100, 5);
  f.Add(2, Side::Buy, 100, 6);
  CHECK(!f.Cancel(999));
  CHECK(f.book.BestBidQty() == 11);
  CHECK(f.Cancel(1));
  CHECK(!f.Cancel(1));
  CHECK(f.book.BestBidQty() == 6);
  f.Sell(3, 100, 6); // fills order 2 completely
  CHECK(f.IsEmpty());
  CHECK(!f.Cancel(2));
  CHECK(!f.Cancel(3)); // fully filled taker never rested
  CHECK(f.IsEmpty());
}

// Duplicate id of a live resting order is ignored entirely.
static void DuplicateIdIgnored() {
  Fixture f;
  f.Add(1, Side::Buy, 100, 5);
  CHECK(f.Add(1, Side::Buy, 100, 7) == 7); // same level: not added
  CHECK(f.book.BestBidQty() == 5);
  f.Add(1, Side::Sell, 90, 5); // would cross if it were processed
  CHECK(f.trades.empty());
  CHECK(f.book.BestAsk() == 0);
  CHECK(f.book.BestBid() == 100);
  CHECK(f.book.BestBidQty() == 5);
  CHECK(f.Cancel(1));
  CHECK(!f.Cancel(1)); // exactly one order existed
  CHECK(f.IsEmpty());
}

// Id reuse after cancel and after a full fill; level removal when the last
// order goes (cancel and fill) and a fresh level afterwards.
static void IdReuseAndLevelRemoval() {
  Fixture f;
  f.Add(1, Side::Buy, 100, 5);
  f.Add(2, Side::Buy, 99, 5);
  CHECK(f.Cancel(1));
  CHECK(f.book.BestBid() == 99); // level 100 removed with last order
  f.Add(1, Side::Buy, 100, 2);   // reuse id after cancel; fresh level
  CHECK(f.book.BestBid() == 100);
  CHECK(f.book.BestBidQty() == 2);

  f.Sell(3, 100, 2); // fill order 1 fully
  CHECK(f.book.BestBid() == 99);
  CHECK(f.book.BestBidQty() == 5);
  f.Add(1, Side::Buy, 101, 8); // reuse id after fill
  CHECK(f.book.BestBid() == 101);
  CHECK(f.book.BestBidQty() == 8);
  CHECK(f.Cancel(1));
  CHECK(f.book.BestBid() == 99);

  // Taker id reuse: id 3 was a fully filled taker above.
  f.Add(3, Side::Sell, 150, 1);
  CHECK(f.book.BestAsk() == 150);
  CHECK(f.Cancel(3));
  CHECK(f.Cancel(2));
  CHECK(f.IsEmpty());
}

// Ids equal modulo the table capacity share a probe chain. FastBook(8) gives
// a table of 16 slots, so 1, 17, 33, 49 collide.
static void CollidingIdsInBook() {
  Fixture f(8);
  f.Add(1, Side::Buy, 10, 1);
  f.Add(17, Side::Buy, 10, 2);
  f.Add(33, Side::Buy, 10, 3);
  CHECK(f.book.BestBidQty() == 6);
  CHECK(f.Cancel(17)); // middle of the probe chain
  CHECK(f.book.BestBidQty() == 4);
  CHECK(f.Add(33, Side::Buy, 10, 9) == 9); // still found: duplicate ignored
  CHECK(f.book.BestBidQty() == 4);
  CHECK(!f.Cancel(17));
  f.Add(49, Side::Buy, 10, 5); // reuses the dead slot
  CHECK(f.book.BestBidQty() == 9);
  CHECK(f.Cancel(33));
  CHECK(f.Cancel(1));
  CHECK(f.Cancel(49));
  CHECK(f.IsEmpty());
}

// OpenAddressingHashMap directly: insert / find / erase / update on a chain.
static void HashMapCollisions() {
  OpenAddressingHashMap<uint32_t, size_t> m(8);
  CHECK(m.Find(1) == nullptr);
  CHECK(!m.Erase(1));
  CHECK(m.Insert(1, 100));
  CHECK(m.Insert(9, 900));  // same home slot as 1
  CHECK(m.Insert(17, 1700)); // and again
  CHECK(m.Find(1) && *m.Find(1) == 100);
  CHECK(m.Find(9) && *m.Find(9) == 900);
  CHECK(m.Find(17) && *m.Find(17) == 1700);
  CHECK(m.Erase(9)); // erase the middle of the chain
  CHECK(m.Find(9) == nullptr);
  CHECK(m.Find(17) && *m.Find(17) == 1700); // still reachable past the hole
  CHECK(!m.Erase(9));
  CHECK(m.Insert(17, 1701)); // update existing key, no duplicate
  CHECK(*m.Find(17) == 1701);
  CHECK(m.Erase(17));
  CHECK(m.Find(17) == nullptr);
  CHECK(m.Find(1) && *m.Find(1) == 100);
  CHECK(m.Insert(9, 901)); // reinsert after erase
  CHECK(*m.Find(9) == 901);
  CHECK(m.Find(25) == nullptr); // same chain, never inserted
  CHECK(m.Erase(1));
  CHECK(m.Erase(9));
  CHECK(m.Find(1) == nullptr && m.Find(9) == nullptr);
}

// Hash map churn: many insert / erase cycles and a
// full table, with every key on one chain.
static void HashMapChurnAndFull() {
  OpenAddressingHashMap<uint32_t, size_t> m(8);
  for (uint32_t round = 0; round < 50; ++round) {
    const uint32_t a = 8 * round + 3, b = 8 * round + 3 + 8 * 1000;
    CHECK(m.Insert(a, a));
    CHECK(m.Insert(b, b));
    CHECK(m.Find(a) && *m.Find(a) == a);
    CHECK(m.Find(b) && *m.Find(b) == b);
    CHECK(m.Erase(a));
    CHECK(m.Find(a) == nullptr);
    CHECK(m.Find(b) && *m.Find(b) == b);
    CHECK(m.Erase(b));
    CHECK(m.Find(b) == nullptr);
  }
  for (uint32_t k = 0; k < 8; ++k) CHECK(m.Insert(k * 8, k));
  CHECK(!m.Insert(64, 1)); // full
  for (uint32_t k = 0; k < 8; ++k) CHECK(m.Find(k * 8) && *m.Find(k * 8) == k);
}

// Backward-shift deletion: wrapped collision chain, full table, churn.
static void HashMapBackwardShift() {
  OpenAddressingHashMap<uint32_t, size_t> m(8);
  const uint32_t keys[] = {7, 15, 23, 31, 0, 1}; // 7,15,23,31 home to 7 and wrap
  auto all_but = [&](std::initializer_list<uint32_t> gone) {
    for (uint32_t k : keys) {
      bool erased = false;
      for (uint32_t g : gone) erased |= (g == k);
      if (erased) { CHECK(m.Find(k) == nullptr); }
      else { CHECK(m.Find(k) && *m.Find(k) == k * 10); }
    }
  };
  for (uint32_t k : keys) CHECK(m.Insert(k, k * 10));
  all_but({});
  CHECK(m.Find(39) == nullptr); // same home, never inserted
  CHECK(!m.Erase(39));
  CHECK(m.Erase(7)); // chain head
  all_but({7});
  CHECK(m.Erase(23)); // middle of the wrapped run
  all_but({7, 23});
  for (uint32_t k : {7u, 23u}) CHECK(m.Insert(k, k * 10));
  all_but({});
  CHECK(!m.Erase(39));

  // Full table: every slot Live, no Empty slot anywhere.
  OpenAddressingHashMap<uint32_t, size_t> f(8);
  for (uint32_t k = 0; k < 8; ++k) CHECK(f.Insert(k * 8, k)); // all home to 0
  CHECK(!f.Insert(64, 1));
  CHECK(f.Erase(24)); // must terminate
  for (uint32_t k = 0; k < 8; ++k) {
    if (k == 3) { CHECK(f.Find(k * 8) == nullptr); }
    else { CHECK(f.Find(k * 8) && *f.Find(k * 8) == k); }
  }
  CHECK(f.Insert(64, 8));
  CHECK(f.Find(64) && *f.Find(64) == 8);
  CHECK(!f.Insert(72, 9)); // full again

  // Full table, wrapped homes.
  OpenAddressingHashMap<uint32_t, size_t> w(8);
  for (uint32_t k = 0; k < 8; ++k) CHECK(w.Insert(7 + 8 * k, k));
  CHECK(w.Erase(7));
  for (uint32_t k = 1; k < 8; ++k) CHECK(w.Find(7 + 8 * k) && *w.Find(7 + 8 * k) == k);
  CHECK(w.Insert(7, 0));

  // Entry that wrapped past the end must not be pulled back over a hole that
  // lies inside its own probe run (home 7 at slot 0, hole at slot 5).
  OpenAddressingHashMap<uint32_t, size_t> v(8);
  for (uint32_t k : {5u, 6u, 7u, 15u, 23u}) CHECK(v.Insert(k, k));
  CHECK(v.Erase(5));
  for (uint32_t k : {6u, 7u, 15u, 23u}) CHECK(v.Find(k) && *v.Find(k) == k);
  CHECK(v.Find(5) == nullptr);

  // Churn: 3 keys sharing a home, 100k insert/erase rounds.
  OpenAddressingHashMap<uint32_t, size_t> c(8);
  const uint32_t ck[3] = {5, 13, 21};
  for (uint32_t r = 0; r < 100000; ++r) {
    const uint32_t a = ck[r % 3], b = ck[(r + 1) % 3], d = ck[(r + 2) % 3];
    CHECK(c.Insert(a, r)); CHECK(c.Insert(b, r + 1)); CHECK(c.Insert(d, r + 2));
    CHECK(*c.Find(a) == r && *c.Find(b) == r + 1 && *c.Find(d) == r + 2);
    CHECK(c.Erase(a));
    CHECK(c.Find(a) == nullptr && *c.Find(b) == r + 1 && *c.Find(d) == r + 2);
    CHECK(c.Erase(d));
    CHECK(c.Find(a) == nullptr && c.Find(d) == nullptr && *c.Find(b) == r + 1);
    CHECK(c.Erase(b));
    CHECK(c.Find(b) == nullptr && !c.Erase(b));
  }
}

// Randomized differential test against a tiny reference book. Checks after
// every operation: trades, leftover quantity, best price/qty, no crossed book;
// then drains level by level so every level's aggregate is observed at top.
namespace {
struct ModelOrder { OrderId id; Quantity qty; };
struct Model {
  std::map<Price, std::deque<ModelOrder>> bids, asks;
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
  Price BestBid() const { return bids.empty() ? 0 : bids.rbegin()->first; }
  Price BestAsk() const { return asks.empty() ? 0 : asks.begin()->first; }
  static Quantity Sum(const std::deque<ModelOrder> &q) {
    Quantity s = 0;
    for (auto &o : q) s += o.qty;
    return s;
  }
  Quantity BestBidQty() const { return bids.empty() ? 0 : Sum(bids.rbegin()->second); }
  Quantity BestAskQty() const { return asks.empty() ? 0 : Sum(asks.begin()->second); }
};
} // namespace

static void RandomizedVsModel() {
  Fixture f(64);
  Model m;
  uint32_t s = 12345;
  auto rnd = [&](uint32_t n) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s % n;
  };
  bool ok = true;
  for (int step = 0; step < 4000 && ok; ++step) {
    const OrderId id = 1 + rnd(300); // heavy id reuse and duplicates
    if (rnd(10) < 3) {
      ok = f.Cancel(id) == m.Cancel(id);
      CHECK(ok);
    } else if (m.live.size() < 60) {
      const Side side = rnd(2) ? Side::Buy : Side::Sell;
      const Price price = 90 + rnd(20);
      const Quantity qty = 1 + rnd(9);
      f.trades.clear();
      m.trades.clear();
      const Quantity r1 = f.Add(id, side, price, qty);
      const Quantity r2 = m.Add(id, side, price, qty);
      ok = r1 == r2 && f.trades.size() == m.trades.size();
      for (size_t i = 0; ok && i < m.trades.size(); ++i)
        ok = TradeIs(f.trades[i], m.trades[i].price, m.trades[i].quantity,
                     m.trades[i].taker, m.trades[i].maker);
      CHECK(ok);
    }
    const bool same = f.book.BestBid() == m.BestBid() &&
                      f.book.BestAsk() == m.BestAsk() &&
                      f.book.BestBidQty() == m.BestBidQty() &&
                      f.book.BestAskQty() == m.BestAskQty();
    CHECK(same);
    CHECK(!f.Crossed());
    ok = ok && same;
  }
  // Drain best-first, checking the aggregate of every level on the way.
  while (ok && !m.bids.empty()) {
    ok = f.book.BestBid() == m.BestBid() && f.book.BestBidQty() == m.BestBidQty();
    CHECK(ok);
    CHECK(f.Cancel(m.bids.rbegin()->second.front().id));
    m.Cancel(m.bids.rbegin()->second.front().id);
  }
  while (ok && !m.asks.empty()) {
    ok = f.book.BestAsk() == m.BestAsk() && f.book.BestAskQty() == m.BestAskQty();
    CHECK(ok);
    CHECK(f.Cancel(m.asks.begin()->second.front().id));
    m.Cancel(m.asks.begin()->second.front().id);
  }
  if (ok) CHECK(f.IsEmpty());
}

// Capacity: a new price level beyond MaxLevels is rejected with no mutation;
// an existing level still accepts orders; the other side is independent.
static void LevelsFullRejected() {
  using SmallBook = FastBook<std::allocator<Order>, 4>;
  SmallBook book(16, std::allocator<Order>());
  auto add = [&](OrderId id, Side side, Price price, Quantity qty) {
    Order o{price, id, qty, side};
    return book.AddOrder(o, [](const Trade &) {});
  };
  for (OrderId i = 0; i < 4; ++i)
    CHECK(add(1 + i, Side::Buy, 100 + 10 * i, 5) == AddResult::Ok);
  CHECK(book.BestBid() == 130);
  CHECK(book.BestBidQty() == 5);
  CHECK(add(5, Side::Buy, 95, 7) == AddResult::LevelsFull);
  CHECK(add(6, Side::Buy, 115, 7) == AddResult::LevelsFull); // between levels
  CHECK(book.BestBid() == 130);
  CHECK(book.BestBidQty() == 5);
  CHECK(add(1, Side::Buy, 95, 1) == AddResult::Duplicate);
  // Existing price still accepted while full.
  CHECK(add(7, Side::Buy, 130, 3) == AddResult::Ok);
  CHECK(book.BestBidQty() == 8);
  // Asks are independent of the full bids.
  CHECK(add(8, Side::Sell, 200, 2) == AddResult::Ok);
  CHECK(book.BestAsk() == 200);
  CHECK(book.BestAskQty() == 2);
  // Rejected orders were never stored.
  CHECK(!book.CancelOrder(5));
  CHECK(!book.CancelOrder(6));
  // Peel the bids best-first: still sorted, aggregates intact.
  CHECK(book.CancelOrder(7));
  CHECK(book.BestBid() == 130 && book.BestBidQty() == 5);
  CHECK(book.CancelOrder(4));
  CHECK(book.BestBid() == 120 && book.BestBidQty() == 5);
  CHECK(book.CancelOrder(3));
  CHECK(book.BestBid() == 110 && book.BestBidQty() == 5);
  CHECK(book.CancelOrder(2));
  CHECK(book.BestBid() == 100 && book.BestBidQty() == 5);
  CHECK(book.CancelOrder(1));
  CHECK(book.BestBid() == 0);
  CHECK(book.CancelOrder(8));
  CHECK(book.BestAsk() == 0);
}

// Capacity: when the order pool is exhausted the add is rejected with no
// mutation, and cancelling frees a slot for reuse.
static void PoolFullRejected() {
  Fixture f(2);
  auto add = [&](OrderId id, Side side, Price price, Quantity qty) {
    Order o{price, id, qty, side};
    return f.book.AddOrder(o, [](const Trade &) {});
  };
  CHECK(add(1, Side::Buy, 100, 5) == AddResult::Ok);
  CHECK(add(2, Side::Buy, 100, 6) == AddResult::Ok);
  CHECK(add(3, Side::Buy, 100, 7) == AddResult::PoolFull); // existing level
  CHECK(add(4, Side::Buy, 90, 7) == AddResult::PoolFull);  // new level
  CHECK(f.book.BestBid() == 100);
  CHECK(f.book.BestBidQty() == 11);
  CHECK(add(1, Side::Buy, 100, 1) == AddResult::Duplicate);
  CHECK(!f.Cancel(3));
  CHECK(!f.Cancel(4));
  CHECK(f.Cancel(1));
  CHECK(f.book.BestBidQty() == 6);
  CHECK(add(3, Side::Buy, 100, 7) == AddResult::Ok); // slot reused
  CHECK(f.book.BestBidQty() == 13);
  CHECK(f.Cancel(2));
  CHECK(f.Cancel(3));
  CHECK(f.IsEmpty());
}

// Compile-time: the book's order table slot is 12 bytes. The static_assert
// lives in fastbook.hpp next to OrderTable (Slot is private); this repeats the
// check via the public kSlotSize for the same key/value types.
static_assert(OpenAddressingHashMap<uint32_t, uint32_t>::kSlotSize == 12);
static void SlotSize12() {
  CHECK((OpenAddressingHashMap<uint32_t, uint32_t>::kSlotSize == 12));
}

// Randomized model test with tiny tables kept at/near full load, small key
// range so collisions and wrap-around are constant.
static void HashMapRandomFullLoad() {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  size_t full_ops = 0, rejects = 0;
  for (size_t cap : {size_t{8}, size_t{16}}) {
    OpenAddressingHashMap<uint32_t, uint32_t> m(cap);
    std::unordered_map<uint32_t, uint32_t> model;
    auto verify = [&]() {
      for (uint32_t k = 0; k < 64; ++k) {
        auto it = model.find(k);
        uint32_t *p = m.Find(k);
        if (it == model.end()) { CHECK(p == nullptr); }
        else { CHECK(p != nullptr && *p == it->second); }
      }
    };
    auto ins = [&](uint32_t k, uint32_t v) {
      // Insert rejects everything when full, even an update of a present key.
      const bool expect = model.size() < cap;
      if (model.size() == cap) { ++full_ops; ++rejects; }
      CHECK(m.Insert(k, v) == expect);
      if (expect) model[k] = v;
    };
    for (size_t op = 0; op < 100000; ++op) {
      if (op % 2000 == 0) { // fill until FULL, then probe the reject path
        for (uint32_t k = 0; k < 64 && model.size() < cap; ++k) {
          ins(static_cast<uint32_t>(rnd() % 64), op);
        }
        for (int i = 0; i < 20; ++i) ins(static_cast<uint32_t>(rnd() % 64), op);
        verify();
      }
      const uint32_t k = static_cast<uint32_t>(rnd() % 64);
      // bias toward insert so the table stays at or near full
      const uint64_t r = rnd() % 10;
      if (r < 5) {
        ins(k, static_cast<uint32_t>(op));
      } else if (r < 8) {
        const bool expect = model.erase(k) != 0;
        CHECK(m.Erase(k) == expect);
      } else {
        auto it = model.find(k);
        uint32_t *p = m.Find(k);
        CHECK((p != nullptr) == (it != model.end()));
      }
      verify();
    }
  }
  CHECK(full_ops > 10000); // substantial time at full load
  CHECK(rejects > 1000);   // reject path exercised
}

// Level aggregate is 32-bit: an add that would wrap it is rejected with no
// mutation (no pool leak, no aggregate change).
static void LevelQuantityOverflowRejected() {
  constexpr Quantity kQ = 2'000'000'000u;
  constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
  for (const Side maker_side : {Side::Buy, Side::Sell}) {
    const Side taker_side = maker_side == Side::Buy ? Side::Sell : Side::Buy;
    const bool bids = maker_side == Side::Buy;
    const Price far = bids ? 50 : 150;     // different price, same side
    const Price far2 = bids ? 40 : 160;
    Fixture f(16);
    auto qty = [&] { return bids ? f.book.BestBidQty() : f.book.BestAskQty(); };
    auto add = [&](OrderId id, Side side, Price p, Quantity q) {
      Order o{p, id, q, side};
      return f.book.AddOrder(o, [&](const Trade &t) { f.trades.push_back(t); });
    };

    CHECK(add(1, maker_side, 100, kQ) == AddResult::Ok);
    CHECK(add(2, maker_side, 100, kQ) == AddResult::Ok);
    CHECK(qty() == 4'000'000'000u);

    CHECK(add(3, maker_side, 100, kQ) == AddResult::QuantityOverflow);
    CHECK(qty() == 4'000'000'000u);
    CHECK(!f.Cancel(3));
    CHECK(!f.Crossed());

    // no pool leak: other price still accepted
    CHECK(add(4, maker_side, far, 7) == AddResult::Ok);
    CHECK(!f.Crossed());

    // cancel frees room
    CHECK(f.Cancel(1));
    CHECK(qty() == kQ);
    CHECK(add(5, maker_side, 100, kQ) == AddResult::Ok);
    CHECK(qty() == 4'000'000'000u);
    CHECK(!f.Crossed());

    // FIFO match: oldest (2) gives 2e9, then 5 gives 1e9
    f.trades.clear();
    CHECK(add(21, taker_side, 100, 3'000'000'000u) == AddResult::Ok);
    CHECK(f.trades.size() == 2);
    if (f.trades.size() == 2) {
      CHECK(TradeIs(f.trades[0], 100, kQ, 21, 2));
      CHECK(TradeIs(f.trades[1], 100, 1'000'000'000u, 21, 5));
    }
    CHECK(qty() == 1'000'000'000u);
    CHECK(!f.Crossed());

    // later cancels all succeed
    CHECK(f.Cancel(5));
    CHECK(f.Cancel(4));
    CHECK(f.IsEmpty());

    // boundary: sum exactly UINT32_MAX is fine, one more is rejected
    CHECK(add(31, maker_side, far2, kMax - 5) == AddResult::Ok);
    CHECK(add(32, maker_side, far2, 5) == AddResult::Ok);
    CHECK(qty() == kMax);
    CHECK(add(33, maker_side, far2, 1) == AddResult::QuantityOverflow);
    CHECK(qty() == kMax);
    CHECK(!f.Cancel(33));
    CHECK(!f.Crossed());
    CHECK(f.Cancel(31));
    CHECK(qty() == 5);
    CHECK(f.Cancel(32));
    CHECK(f.IsEmpty());
  }
}

// ------------------------------------------------------------------- main
struct TestCase { const char *name; void (*fn)(); };

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); // keep output if a test crashes
  const TestCase tests[] = {
      {"EmptyBook", EmptyBook},
      {"RestingAddsBothSides", RestingAddsBothSides},
      {"SortedBidsAndAsks", SortedBidsAndAsks},
      {"FullFillBothSides", FullFillBothSides},
      {"PartialFills", PartialFills},
      {"MultiLevelSweepAndRemainder", MultiLevelSweepAndRemainder},
      {"PriceTimePriority", PriceTimePriority},
      {"LevelAggregates", LevelAggregates},
      {"CancelHeadMiddleTail", CancelHeadMiddleTail},
      {"CancelUnknownAndRepeated", CancelUnknownAndRepeated},
      {"DuplicateIdIgnored", DuplicateIdIgnored},
      {"IdReuseAndLevelRemoval", IdReuseAndLevelRemoval},
      {"CollidingIdsInBook", CollidingIdsInBook},
      {"HashMapCollisions", HashMapCollisions},
      {"HashMapChurnAndFull", HashMapChurnAndFull},
      {"HashMapBackwardShift", HashMapBackwardShift},
      {"RandomizedVsModel", RandomizedVsModel},
      {"SlotSize12", SlotSize12},
      {"HashMapRandomFullLoad", HashMapRandomFullLoad},
      {"LevelsFullRejected", LevelsFullRejected},
      {"PoolFullRejected", PoolFullRejected},
      {"LevelQuantityOverflowRejected", LevelQuantityOverflowRejected},
  };
  int failed_tests = 0;
  for (const TestCase &t : tests) {
    g_test = t.name;
    g_test_failures = 0;
    std::printf("run    %s\n", t.name); // last line seen if a test crashes
    t.fn();
    if (g_test_failures != 0) {
      ++failed_tests;
      std::printf("FAILED %s (%d checks)\n", t.name, g_test_failures);
    } else {
      std::printf("ok     %s\n", t.name);
    }
  }
  const int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
  std::printf("%d/%d tests passed\n", total - failed_tests, total);
  return failed_tests == 0 ? 0 : 1;
}
