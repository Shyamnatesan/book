// Feature: cancel. Head/middle/tail cancels keep FIFO order, aggregates
// update, empty levels disappear, unknown/repeated cancels are refused, and a
// cancelled id can be reused.
#include "scenario.hpp"

int main() {
  Session<16> s(64);

  std::puts("-- three orders at one level, plus a deeper level");
  s.Add(10, Side::Sell, 50, 1);
  s.Add(11, Side::Sell, 50, 2);
  s.Add(12, Side::Sell, 50, 4);
  s.Add(13, Side::Sell, 51, 8);
  EXPECT(s.book.BestAsk() == 50 && s.book.BestAskQty() == 7);

  std::puts("-- cancel middle, then check FIFO via a match");
  EXPECT(s.Cancel(11));
  EXPECT(s.book.BestAskQty() == 5);
  s.Add(20, Side::Buy, 50, 2);
  EXPECT(s.TradeIs(0, 50, 1, 20, 10) && s.TradeIs(1, 50, 1, 20, 12));

  std::puts("-- cancel the last order at the best level: level disappears");
  EXPECT(s.Cancel(12));
  EXPECT(s.book.BestAsk() == 51 && s.book.BestAskQty() == 8);

  std::puts("-- unknown, repeated and filled ids are refused");
  EXPECT(!s.Cancel(999));
  EXPECT(!s.Cancel(12));
  EXPECT(!s.Cancel(10)); // fully filled above

  std::puts("-- a cancelled id can be reused");
  EXPECT(s.Add(12, Side::Buy, 40, 3) == AddResult::Ok);
  EXPECT(s.book.BestBid() == 40 && s.book.BestBidQty() == 3);
  EXPECT(s.Cancel(12) && s.Cancel(13));
  EXPECT(s.book.BestBid() == 0 && s.book.BestAsk() == 0);
  return Finish("cancel");
}
