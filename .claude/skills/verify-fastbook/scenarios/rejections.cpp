// Feature: rejections. Duplicate, LevelsFull, PoolFull and QuantityOverflow
// leave the book unchanged; a rejection after a partial match keeps the fills.
#include "scenario.hpp"

#include <limits>

int main() {
  constexpr Quantity kMax = std::numeric_limits<Quantity>::max();

  std::puts("-- Duplicate: a resting id cannot be added again");
  {
    Session<4> s(8);
    s.Add(1, Side::Buy, 10, 5);
    EXPECT(s.Add(1, Side::Sell, 9, 5) == AddResult::Duplicate);
    EXPECT(s.trades.empty() && s.book.BestBidQty() == 5);
  }

  std::puts("-- LevelsFull: MaxLevels=2, a third bid price is refused");
  {
    Session<2> s(8);
    s.Add(1, Side::Buy, 10, 1);
    s.Add(2, Side::Buy, 11, 1);
    EXPECT(s.Add(3, Side::Buy, 12, 1) == AddResult::LevelsFull);
    EXPECT(s.book.BestBid() == 11);
    EXPECT(s.Add(4, Side::Buy, 10, 1) == AddResult::Ok); // existing level still accepts
    EXPECT(s.Add(5, Side::Sell, 20, 1) == AddResult::Ok); // other side independent
  }

  std::puts("-- PoolFull: max_orders=2, a third resting order is refused");
  {
    Session<4> s(2);
    s.Add(1, Side::Buy, 10, 1);
    s.Add(2, Side::Sell, 20, 1);
    EXPECT(s.Add(3, Side::Buy, 11, 1) == AddResult::PoolFull);
    EXPECT(s.book.BestBid() == 10);
    std::puts("   ...but an order that fully matches needs no slot");
    EXPECT(s.Add(3, Side::Buy, 20, 1) == AddResult::Ok && s.trades.size() == 1);
  }

  std::puts("-- QuantityOverflow: level total would exceed UINT32_MAX");
  {
    Session<4> s(8);
    s.Add(1, Side::Buy, 10, kMax);
    EXPECT(s.Add(2, Side::Buy, 10, 1) == AddResult::QuantityOverflow);
    EXPECT(s.book.BestBidQty() == kMax);
  }

  std::puts("-- rejection after a partial match keeps the fills, remainder not rested");
  {
    Session<1> s(8);
    s.Add(1, Side::Sell, 10, 2);
    s.Add(2, Side::Buy, 5, 1); // the only bid level
    EXPECT(s.Add(3, Side::Buy, 10, 5) == AddResult::LevelsFull);
    EXPECT(s.trades.size() == 1 && s.TradeIs(0, 10, 2, 3, 1));
    EXPECT(s.remainder == 3);
    EXPECT(s.book.BestAsk() == 0 && s.book.BestBid() == 5);
  }
  return Finish("rejections");
}
