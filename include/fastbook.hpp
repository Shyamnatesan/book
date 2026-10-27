#pragma once

#include "hashmap.hpp"
#include "inlinebuffer.hpp"
#include "mempool.hpp"
#include "order.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>

#include <cstring>

namespace shyamnatesan {

enum class AddResult { Ok, Duplicate, LevelsFull, PoolFull, QuantityOverflow };

template <typename Allocator, size_t MaxLevels> class FastBook {
public:
  explicit FastBook(size_t max_orders, Allocator alloc)
      : order_table_(max_orders * 2, alloc), orders_(max_orders, alloc) {
    // Every pool handle must fit in OrderHandle without colliding with the
    // kInvalidPoolIndex sentinel.
    if (max_orders >= kInvalidPoolIndex) {
      throw std::invalid_argument("max_orders must be < kInvalidPoolIndex");
    }
  }

private:
  struct OrderNode {
    Order order;
    size_t next{kInvalidPoolIndex};
    size_t prev{kInvalidPoolIndex};

    OrderNode(const Order &o) : order(o) {}
  };

  using OrderHandle = uint32_t;

  using Levels = InlineBuffer<PriceLevel, MaxLevels>;
  using OrderTable = OpenAddressingHashMap<OrderId, OrderHandle, Allocator>;
  using OrderPool = MemPool<OrderNode, Allocator>;

  // Slot is {u32 key, u32 handle, u8 state} = 12 B; keeps the table compact.
  static_assert(OrderTable::kSlotSize == 12, "hash map Slot must be 12 bytes");

  Levels bids_{};
  Levels asks_{};

  OrderTable order_table_;
  OrderPool orders_;

public:
  // On LevelsFull, PoolFull or QuantityOverflow the order may already have been partially
  // matched: order.quantity holds the unfilled remainder, which was not rested.
  template <typename F> AddResult AddOrder(Order &order, F &&on_trade) {
    if (order_table_.Find(order.order_id) != nullptr) [[unlikely]] {
      return AddResult::Duplicate;
    }

    Match(order, on_trade);
    if (order.quantity == 0) [[unlikely]] {
      return AddResult::Ok;
    }

    return AddInBook(order);
  }

  Price BestBid() const { return bids_.empty() ? 0 : bids_.back().price; }

  Price BestAsk() const { return asks_.empty() ? 0 : asks_.back().price; }

  Quantity BestBidQty() const {
    return bids_.empty() ? 0 : bids_.back().total_quantity;
  }

  Quantity BestAskQty() const {
    return asks_.empty() ? 0 : asks_.back().total_quantity;
  }

  bool CancelOrder(OrderId order_id) {
    const auto *order_handle = order_table_.Find(order_id);
    if (order_handle == nullptr) {
      return false;
    }
    OrderNode *node = orders_.Get(*order_handle);

    Levels &levels = node->order.side == Side::Buy ? bids_ : asks_;
    const size_t idx = levels.lower_bound(
        node->order.price, [&](const PriceLevel &level, Price price) {
          return node->order.side == Side::Buy ? level.price < price
                                               : level.price > price;
        });

    assert(idx < levels.size() && levels[idx].price == node->order.price);
    PriceLevel &level = levels[idx];
    level.total_quantity -= node->order.quantity;
    --level.order_count;

    if (node->prev != kInvalidPoolIndex) {
      orders_.Get(node->prev)->next = node->next;
    } else {
      level.orders_head = node->next;
    }
    if (node->next != kInvalidPoolIndex) {
      orders_.Get(node->next)->prev = node->prev;
    } else {
      level.orders_tail = node->prev;
    }
    if (level.order_count == 0) [[unlikely]] {
      levels.erase(idx);
    }

    orders_.Release(*order_handle);
    order_table_.Erase(order_id);
    return true;
  }

private:
  bool Crosses(const Order &incoming, const PriceLevel &best) const {
    return incoming.side == Side::Buy ? incoming.price >= best.price
                                      : incoming.price <= best.price;
  }

  template <typename F> void Match(Order &incoming, F &&on_trade) {
    Levels &opposite = incoming.side == Side::Buy ? asks_ : bids_;
    while (incoming.quantity > 0 && !opposite.empty() &&
           Crosses(incoming, opposite.back())) {
      PriceLevel &level = opposite.back();

      while (incoming.quantity > 0 && level.orders_head != kInvalidPoolIndex) {
        const OrderHandle head = level.orders_head;
        OrderNode *resting = orders_.Get(head);

        const Quantity fill =
            std::min(incoming.quantity, resting->order.quantity);

        incoming.quantity -= fill;
        resting->order.quantity -= fill;
        level.total_quantity -= fill;
        on_trade(Trade{level.price, fill, incoming.order_id,
                       resting->order.order_id});

        if (resting->order.quantity != 0) {
          break;
        }
        level.orders_head = resting->next;
        if (level.orders_head == kInvalidPoolIndex) {
          level.orders_tail = kInvalidPoolIndex;
        } else {
          orders_.Get(level.orders_head)->prev = kInvalidPoolIndex;
        }

        --level.order_count;
        order_table_.Erase(resting->order.order_id);
        orders_.Release(head);
      }
      if (level.orders_head == kInvalidPoolIndex) {
        opposite.erase(opposite.size() - 1);
      }
    }
  }

  AddResult AddInBook(Order &order) {
    // get the appropriate level
    Levels &levels = (order.side == Side::Buy) ? bids_ : asks_;

    // find the insertion point, and check if price-level already exists
    const size_t level_idx = levels.lower_bound(
        order.price, [&](const PriceLevel &level, Price price) {
          return order.side == Side::Buy ? level.price < price
                                         : level.price > price;
        });

    const bool found =
        level_idx < levels.size() && levels[level_idx].price == order.price;

    // reject before allocating or mutating anything
    if (!found && levels.size() == MaxLevels) [[unlikely]] {
      return AddResult::LevelsFull;
    }

    // reject if the level aggregate would wrap; the new-level path starts
    // from a single Quantity and cannot overflow
    if (found && levels[level_idx].total_quantity >
                     std::numeric_limits<Quantity>::max() - order.quantity)
        [[unlikely]] {
      return AddResult::QuantityOverflow;
    }

    // create a new order in the pool
    const OrderHandle new_order_handle =
        static_cast<OrderHandle>(orders_.Allocate(order));
    if (new_order_handle == kInvalidPoolIndex) [[unlikely]] {
      return AddResult::PoolFull;
    }
    OrderNode *new_order = orders_.Get(new_order_handle);

    if (!found) [[unlikely]] {
      levels.emplace_at(level_idx, order.price, order.quantity, 1,
                        new_order_handle, new_order_handle);

      [[maybe_unused]] const bool inserted =
          order_table_.Insert(new_order->order.order_id, new_order_handle);
      assert(inserted);
      return AddResult::Ok;
    }

    const auto curr_tail_handle = levels[level_idx].orders_tail;
    OrderNode *current_tail = orders_.Get(curr_tail_handle);
    new_order->prev = curr_tail_handle;
    current_tail->next = new_order_handle;
    levels[level_idx].orders_tail = new_order_handle;
    levels[level_idx].total_quantity += order.quantity;
    ++levels[level_idx].order_count;

    [[maybe_unused]] const bool inserted =
        order_table_.Insert(new_order->order.order_id, new_order_handle);
    assert(inserted);
    return AddResult::Ok;
  }
};

} // namespace shyamnatesan
