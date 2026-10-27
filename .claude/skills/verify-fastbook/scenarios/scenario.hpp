// Shared helpers for verify-fastbook scenarios: a consumer of the public API
// that logs every call and its observable result, and counts failed EXPECTs.
#pragma once
#include "fastbook.hpp"

#include <cstdio>
#include <memory>
#include <vector>

using namespace shyamnatesan;

inline int g_failures = 0;

#define EXPECT(cond)                                                           \
  do {                                                                         \
    if (cond) {                                                                \
      std::printf("  ok    %s\n", #cond);                                      \
    } else {                                                                   \
      std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                 \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

inline const char *Name(AddResult r) {
  switch (r) {
  case AddResult::Ok: return "Ok";
  case AddResult::Duplicate: return "Duplicate";
  case AddResult::LevelsFull: return "LevelsFull";
  case AddResult::PoolFull: return "PoolFull";
  case AddResult::QuantityOverflow: return "QuantityOverflow";
  }
  return "?";
}

template <size_t MaxLevels> struct Session {
  FastBook<std::allocator<Order>, MaxLevels> book;
  std::vector<Trade> trades; // trades from the most recent Add
  Quantity remainder{};      // unfilled quantity from the most recent Add

  explicit Session(size_t max_orders) : book(max_orders, std::allocator<Order>()) {}

  AddResult Add(OrderId id, Side side, Price price, Quantity qty) {
    Order o{price, id, qty, side};
    trades.clear();
    AddResult r = book.AddOrder(o, [&](const Trade &t) { trades.push_back(t); });
    remainder = o.quantity;
    std::printf("AddOrder(%s id=%u price=%lld qty=%u) -> %s, remainder=%u, trades=%zu\n",
                side == Side::Buy ? "Buy" : "Sell", id, (long long)price, qty, Name(r),
                remainder, trades.size());
    for (const Trade &t : trades)
      std::printf("    trade price=%lld qty=%u taker=%u maker=%u\n", (long long)t.price,
                  t.quantity, t.taker, t.maker);
    Top();
    return r;
  }

  bool Cancel(OrderId id) {
    bool r = book.CancelOrder(id);
    std::printf("CancelOrder(%u) -> %s\n", id, r ? "true" : "false");
    Top();
    return r;
  }

  void Top() const {
    std::printf("    top: bid %lld x %u | ask %lld x %u\n", (long long)book.BestBid(),
                book.BestBidQty(), (long long)book.BestAsk(), book.BestAskQty());
  }

  bool TradeIs(size_t i, Price p, Quantity q, OrderId taker, OrderId maker) const {
    return i < trades.size() && trades[i].price == p && trades[i].quantity == q &&
           trades[i].taker == taker && trades[i].maker == maker;
  }
};

inline int Finish(const char *name) {
  std::printf("%s: %s (%d failed)\n", name, g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
