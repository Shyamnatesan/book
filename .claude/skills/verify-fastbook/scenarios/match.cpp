// Feature: add-match. Resting orders, top of book, price-time priority,
// partial fills, multi-level sweep with remainder resting.
#include "scenario.hpp"

int main() {
  Session<16> s(64);

  std::puts("-- resting orders build both sides");
  EXPECT(s.Add(1, Side::Buy, 99, 5) == AddResult::Ok && s.trades.empty());
  EXPECT(s.Add(2, Side::Buy, 100, 3) == AddResult::Ok);
  EXPECT(s.Add(3, Side::Sell, 102, 4) == AddResult::Ok);
  EXPECT(s.Add(4, Side::Sell, 101, 2) == AddResult::Ok);
  EXPECT(s.book.BestBid() == 100 && s.book.BestBidQty() == 3);
  EXPECT(s.book.BestAsk() == 101 && s.book.BestAskQty() == 2);

  std::puts("-- time priority inside a level");
  EXPECT(s.Add(5, Side::Sell, 101, 6) == AddResult::Ok); // behind id 4 at 101
  EXPECT(s.book.BestAskQty() == 8);
  EXPECT(s.Add(6, Side::Buy, 101, 3) == AddResult::Ok);
  EXPECT(s.trades.size() == 2);
  EXPECT(s.TradeIs(0, 101, 2, 6, 4)); // earlier order fills first
  EXPECT(s.TradeIs(1, 101, 1, 6, 5)); // then a partial fill of the next
  EXPECT(s.remainder == 0 && s.book.BestAsk() == 101 && s.book.BestAskQty() == 5);

  std::puts("-- sweep across levels at maker prices, remainder rests");
  EXPECT(s.Add(7, Side::Buy, 102, 12) == AddResult::Ok);
  EXPECT(s.trades.size() == 2);
  EXPECT(s.TradeIs(0, 101, 5, 7, 5));
  EXPECT(s.TradeIs(1, 102, 4, 7, 3));
  EXPECT(s.remainder == 3);
  EXPECT(s.book.BestBid() == 102 && s.book.BestBidQty() == 3 && s.book.BestAsk() == 0);

  std::puts("-- sell that does not cross rests");
  EXPECT(s.Add(8, Side::Sell, 103, 1) == AddResult::Ok && s.trades.empty());
  EXPECT(s.book.BestBid() < s.book.BestAsk());
  return Finish("match");
}
