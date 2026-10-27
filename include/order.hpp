#pragma once

#include <cstdint>

namespace shyamnatesan {

enum class Side : char {
  Buy,
  Sell,
};

inline namespace V1 {

using Price = int64_t;
using OrderId = uint32_t;
using Quantity = uint32_t;

struct Order {
  Price price;
  OrderId order_id;
  Quantity quantity;
  Side side;
};

struct Trade {
  Price price;
  Quantity quantity;
  OrderId taker;
  OrderId maker;
};

struct PriceLevel {
  Price price;
  Quantity total_quantity;
  uint32_t order_count;
  uint32_t orders_head;
  uint32_t orders_tail;
};
static_assert(sizeof(PriceLevel) == 24);

} // namespace V1

namespace V2 {} // namespace V2

} // namespace shyamnatesan
