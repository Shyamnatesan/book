#pragma once

#include "hashmap.hpp"
#include "order.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace shyamnatesan {

enum class AddResult { Ok, Duplicate, LevelsFull, PoolFull, QuantityOverflow };

constexpr std::string_view ToString(AddResult r) {
  switch (r) {
  case AddResult::Ok: return "Ok";
  case AddResult::Duplicate: return "Duplicate";
  case AddResult::LevelsFull: return "LevelsFull";
  case AddResult::PoolFull: return "PoolFull";
  case AddResult::QuantityOverflow: return "QuantityOverflow";
  }
  std::unreachable();
}

struct LevelView {
  Price price;
  Quantity quantity;
  uint32_t order_count;
};

struct OrderView {
  Price price;
  Quantity quantity;
  Side side;
};

struct BookStats {
  std::array<uint64_t, 5> add_results{}; // AddOrder calls, indexed by AddResult
  uint64_t cancels{};
  uint64_t trades{};
  uint64_t traded_quantity{};
};

// Hot-path helpers carry [[gnu::always_inline]] so their code does not depend on
// the caller's inlining budget. Left to GCC, the same benchmarks ran up to 26%
// slower in a translation unit with more instantiations of the book.
template <typename Allocator, size_t MaxLevels> class FastBook {
private:
  // level_of_ packs the side into bit 15 and the level slot into the rest.
  static_assert(MaxLevels > 0 && MaxLevels <= 0x7FFF);

  static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

  struct Node {
    uint32_t next; // FIFO successor; free-list link while the node is free
    uint32_t prev;
    OrderId id;
    Quantity qty;
  };
  static_assert(sizeof(Node) == 16);

  // One side of the book. Level records stay in their slot while live; the
  // ladder orders the live slots by key, best last. Keys are price for bids
  // and ~price for asks, so both sides ascend toward the best and share code.
  struct Ladder {
    int64_t key[MaxLevels];
    uint16_t slot[MaxLevels];
    PriceLevel levels[MaxLevels];
    uint32_t size;
    uint32_t free_level; // free slots chain through PriceLevel::orders_head

    // First position whose key is >= k.
    [[gnu::always_inline]] uint32_t LowerBound(int64_t k) const {
      if (size == 0 || k > key[size - 1]) {
        return size;
      }
      if (k == key[size - 1]) {
        return size - 1;
      }
      uint32_t n = size - 1;
      if (n == 0) {
        return 0;
      }
      const int64_t *base = key;
      while (n > 1) {
        const uint32_t half = n / 2;
        base = base[half] < k ? base + half : base;
        n -= half;
      }
      return static_cast<uint32_t>(base - key) + (*base < k);
    }

    [[gnu::always_inline]] uint16_t Insert(uint32_t pos, int64_t k, Price price) {
      assert(size < MaxLevels && free_level != kNone && pos <= size);
      const auto s = static_cast<uint16_t>(free_level);
      free_level = levels[s].orders_head;
      // A new best level appends; skipping the empty copies avoids two memmove calls.
      if (pos != size) {
        std::copy_backward(key + pos, key + size, key + size + 1);
        std::copy_backward(slot + pos, slot + size, slot + size + 1);
      }
      key[pos] = k;
      slot[pos] = s;
      ++size;
      levels[s] = PriceLevel{price, 0, 0, kNone, kNone};
      return s;
    }

    [[gnu::always_inline]] void Erase(uint32_t pos) {
      assert(pos < size);
      const uint16_t s = slot[pos];
      if (pos + 1 != size) {
        std::copy(key + pos + 1, key + size, key + pos);
        std::copy(slot + pos + 1, slot + size, slot + pos);
      }
      --size;
      levels[s].orders_head = free_level;
      free_level = s;
    }

    [[gnu::always_inline]] const PriceLevel *Best() const {
      return size == 0 ? nullptr : &levels[slot[size - 1]];
    }
  };

  template <typename T>
  using Rebind = typename std::allocator_traits<Allocator>::template rebind_alloc<T>;
  using OrderIndex = OpenAddressingHashMap<OrderId, uint32_t, Allocator, FibonacciHash>;

  // Slot is {u32 key, u32 handle, u8 state} = 12 B; keeps the table compact.
  static_assert(OrderIndex::kSlotSize == 12, "hash map Slot must be 12 bytes");

  [[gnu::always_inline]] static int64_t Key(Side side, Price price) {
    return side == Side::Buy ? price : ~price;
  }

  // A ternary over constant indices keeps one ladder pointer live; indexing by a
  // runtime side made GCC rebuild side * sizeof(Ladder) at each use and spill.
  [[gnu::always_inline]] Ladder &LadderOf(Side side) { return side == Side::Buy ? ladders_[0] : ladders_[1]; }

  static Side Opposite(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
  }

  static uint16_t Tag(Side side, uint16_t slot) {
    return static_cast<uint16_t>(static_cast<unsigned>(side) << 15 | slot);
  }

  static size_t CheckedCapacity(size_t max_orders) {
    if (max_orders == 0 || max_orders >= kNone) {
      throw std::invalid_argument("max_orders must be in [1, UINT32_MAX)");
    }
    return max_orders;
  }

  std::array<Ladder, 2> ladders_{}; // indexed by Side
  std::vector<Node, Rebind<Node>> nodes_;
  std::vector<uint16_t, Rebind<uint16_t>> level_of_; // Tag of each live node's level
  uint32_t free_node_{0};
  OrderIndex index_;
  BookStats stats_{};

public:
  explicit FastBook(size_t max_orders, Allocator alloc)
      : nodes_(CheckedCapacity(max_orders), Rebind<Node>(alloc)),
        level_of_(max_orders, Rebind<uint16_t>(alloc)),
        index_(std::bit_ceil(2 * max_orders), alloc) {
    for (size_t i = 0; i < max_orders; ++i) {
      nodes_[i].next = i + 1 < max_orders ? static_cast<uint32_t>(i + 1) : kNone;
    }
    for (Ladder &ladder : ladders_) {
      for (size_t i = 0; i < MaxLevels; ++i) {
        ladder.levels[i].orders_head = i + 1 < MaxLevels ? static_cast<uint32_t>(i + 1) : kNone;
      }
    }
  }

  // On LevelsFull, PoolFull or QuantityOverflow the order may already have been partially
  // matched: order.quantity holds the unfilled remainder, which was not rested.
  template <typename F> AddResult AddOrder(Order &order, F &&on_trade) {
    AddResult result = AddResult::Duplicate;
    if (index_.Find(order.order_id) == nullptr) [[likely]] {
      if (Crosses(order)) [[unlikely]] {
        Match(order, on_trade);
      }
      result = order.quantity == 0 ? AddResult::Ok : Rest(order);
    }
    if (result == AddResult::Ok) [[likely]] {
      ++stats_.add_results[0];
    } else {
      ++stats_.add_results[static_cast<size_t>(result)];
    }
    return result;
  }

  [[gnu::always_inline]] bool CancelOrder(OrderId order_id) {
    const uint32_t *found = index_.Find(order_id);
    if (found == nullptr) {
      return false;
    }
    const uint32_t h = *found;
    const uint16_t tag = level_of_[h];
    const Side side = static_cast<Side>(tag >> 15);
    const uint16_t slot = tag & 0x7FFF;
    Ladder &ladder = LadderOf(side);
    PriceLevel &level = ladder.levels[slot];

    level.total_quantity -= nodes_[h].qty;
    Unlink(level, h);
    if (level.order_count == 0) [[unlikely]] {
      const uint32_t pos = ladder.LowerBound(Key(side, level.price));
      assert(pos < ladder.size && ladder.slot[pos] == slot);
      ladder.Erase(pos);
    }
    index_.Erase(order_id);
    FreeNode(h);
    ++stats_.cancels;
    return true;
  }

  Price BestBid() const { return BestPrice(Side::Buy); }
  Price BestAsk() const { return BestPrice(Side::Sell); }
  Quantity BestBidQty() const { return BestQty(Side::Buy); }
  Quantity BestAskQty() const { return BestQty(Side::Sell); }

  // Best-first copy of up to out.size() levels; returns the number written.
  size_t Depth(Side side, std::span<LevelView> out) const {
    const Ladder &ladder = ladders_[static_cast<size_t>(side)];
    const size_t n = std::min<size_t>(ladder.size, out.size());
    for (size_t i = 0; i < n; ++i) {
      const PriceLevel &level = ladder.levels[ladder.slot[ladder.size - 1 - i]];
      out[i] = LevelView{level.price, level.total_quantity, level.order_count};
    }
    return n;
  }

  std::optional<OrderView> FindOrder(OrderId order_id) const {
    const uint32_t *found = index_.Find(order_id);
    if (found == nullptr) {
      return std::nullopt;
    }
    const uint16_t tag = level_of_[*found];
    const Side side = static_cast<Side>(tag >> 15);
    const PriceLevel &level = ladders_[static_cast<size_t>(side)].levels[tag & 0x7FFF];
    return OrderView{level.price, nodes_[*found].qty, side};
  }

  size_t OrderCount() const { return index_.Size(); }
  size_t LevelCount(Side side) const { return ladders_[static_cast<size_t>(side)].size; }
  const BookStats &Stats() const { return stats_; }

  // O(orders + levels). Returns "" when every invariant holds, else the first
  // one found broken.
  std::string_view CheckInvariants() const {
    const size_t capacity = nodes_.size();
    std::vector<bool> node_seen(capacity);
    size_t live_nodes = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
      const Ladder &ladder = ladders_[static_cast<size_t>(side)];
      if (ladder.size > MaxLevels) {
        return "ladder size exceeds MaxLevels";
      }
      std::bitset<MaxLevels> slot_seen;
      for (uint32_t pos = 0; pos < ladder.size; ++pos) {
        if (pos > 0 && ladder.key[pos - 1] >= ladder.key[pos]) {
          return "ladder keys are not strictly ascending";
        }
        const uint16_t s = ladder.slot[pos];
        if (s >= MaxLevels || slot_seen[s]) {
          return "ladder slot out of range or listed twice";
        }
        slot_seen[s] = true;
        const PriceLevel &level = ladder.levels[s];
        if (ladder.key[pos] != Key(side, level.price)) {
          return "ladder key disagrees with its level's price";
        }
        uint64_t total = 0;
        uint32_t count = 0;
        uint32_t prev = kNone;
        for (uint32_t h = level.orders_head; h != kNone; h = nodes_[h].next) {
          if (h >= capacity || node_seen[h]) {
            return "level list leaves the node array or revisits a node";
          }
          node_seen[h] = true;
          const Node &node = nodes_[h];
          if (node.prev != prev) {
            return "prev link disagrees with the list order";
          }
          if (level_of_[h] != Tag(side, s)) {
            return "level_of does not point at the node's level";
          }
          if (node.qty == 0) {
            return "resting order has zero quantity";
          }
          const uint32_t *indexed = index_.Find(node.id);
          if (indexed == nullptr || *indexed != h) {
            return "index does not map a resting order's id to its node";
          }
          total += node.qty;
          ++count;
          prev = h;
        }
        if (level.orders_tail != prev) {
          return "orders_tail is not the last node of the list";
        }
        if (count == 0) {
          return "a live level has no orders";
        }
        if (count != level.order_count || total != level.total_quantity) {
          return "level order_count/total_quantity disagree with its orders";
        }
        live_nodes += count;
      }
      size_t free_levels = 0;
      for (uint32_t s = ladder.free_level; s != kNone; s = ladder.levels[s].orders_head) {
        if (s >= MaxLevels || slot_seen[s]) {
          return "level free list holds a live, repeated or out-of-range slot";
        }
        slot_seen[s] = true;
        ++free_levels;
      }
      if (ladder.size + free_levels != MaxLevels) {
        return "level free list does not cover exactly the free slots";
      }
    }
    const PriceLevel *bid = ladders_[static_cast<size_t>(Side::Buy)].Best();
    const PriceLevel *ask = ladders_[static_cast<size_t>(Side::Sell)].Best();
    if (bid != nullptr && ask != nullptr && bid->price >= ask->price) {
      return "book is crossed";
    }
    if (index_.Size() != live_nodes) {
      return "index size differs from the number of resting orders";
    }
    size_t free_nodes = 0;
    for (uint32_t h = free_node_; h != kNone; h = nodes_[h].next) {
      if (h >= capacity || node_seen[h]) {
        return "node free list holds a live, repeated or out-of-range node";
      }
      node_seen[h] = true;
      ++free_nodes;
    }
    if (live_nodes + free_nodes != capacity) {
      return "node free list does not cover exactly the free nodes";
    }
    return "";
  }

private:
  Price BestPrice(Side side) const {
    const PriceLevel *best = ladders_[static_cast<size_t>(side)].Best();
    return best == nullptr ? 0 : best->price;
  }

  Quantity BestQty(Side side) const {
    const PriceLevel *best = ladders_[static_cast<size_t>(side)].Best();
    return best == nullptr ? 0 : best->total_quantity;
  }

  [[gnu::always_inline]] void Unlink(PriceLevel &level, uint32_t h) {
    const Node &node = nodes_[h];
    (node.prev == kNone ? level.orders_head : nodes_[node.prev].next) = node.next;
    (node.next == kNone ? level.orders_tail : nodes_[node.next].prev) = node.prev;
    --level.order_count;
  }

  [[gnu::always_inline]] void FreeNode(uint32_t h) {
    nodes_[h].next = free_node_;
    free_node_ = h;
  }

  [[gnu::always_inline]] bool Crosses(const Order &taker) {
    const Side maker_side = Opposite(taker.side);
    const Ladder &opposite = LadderOf(maker_side);
    return opposite.size > 0 && Key(maker_side, taker.price) <= opposite.key[opposite.size - 1];
  }

  // Out of line, so a resting add does not carry the matching loop's register
  // state; AddOrder calls it only when Crosses says the order trades.
  template <typename F> [[gnu::noinline]] void Match(Order &taker, F &on_trade) {
    const Side maker_side = Opposite(taker.side);
    Ladder &opposite = LadderOf(maker_side);
    const int64_t limit = Key(maker_side, taker.price);
    while (taker.quantity > 0 && opposite.size > 0 &&
           limit <= opposite.key[opposite.size - 1]) {
      PriceLevel &level = opposite.levels[opposite.slot[opposite.size - 1]];

      while (taker.quantity > 0 && level.orders_head != kNone) {
        const uint32_t h = level.orders_head;
        Node &maker = nodes_[h];
        const Quantity fill = std::min(taker.quantity, maker.qty);

        taker.quantity -= fill;
        maker.qty -= fill;
        level.total_quantity -= fill;
        ++stats_.trades;
        stats_.traded_quantity += fill;
        on_trade(Trade{level.price, fill, taker.order_id, maker.id});

        if (maker.qty != 0) {
          break;
        }
        Unlink(level, h);
        index_.Erase(maker.id);
        FreeNode(h);
      }
      if (level.orders_head == kNone) {
        opposite.Erase(opposite.size - 1);
      }
    }
  }

  [[gnu::always_inline]] AddResult Rest(Order &order) {
    Ladder &ladder = LadderOf(order.side);
    const int64_t key = Key(order.side, order.price);
    const uint32_t pos = ladder.LowerBound(key);
    const bool found = pos < ladder.size && ladder.key[pos] == key;

    // Reject before mutating anything. A new level starts from a single
    // Quantity and cannot overflow.
    if (!found && ladder.size == MaxLevels) [[unlikely]] {
      return AddResult::LevelsFull;
    }
    if (found && ladder.levels[ladder.slot[pos]].total_quantity >
                     std::numeric_limits<Quantity>::max() - order.quantity) [[unlikely]] {
      return AddResult::QuantityOverflow;
    }
    if (free_node_ == kNone) [[unlikely]] {
      return AddResult::PoolFull;
    }

    const uint16_t s = found ? ladder.slot[pos] : ladder.Insert(pos, key, order.price);
    PriceLevel &level = ladder.levels[s];
    const uint32_t h = free_node_;
    Node &node = nodes_[h];
    free_node_ = node.next;
    node = Node{kNone, level.orders_tail, order.order_id, order.quantity};
    (level.orders_tail == kNone ? level.orders_head : nodes_[level.orders_tail].next) = h;
    level.orders_tail = h;
    level.total_quantity += order.quantity;
    ++level.order_count;
    level_of_[h] = Tag(order.side, s);

    [[maybe_unused]] const bool inserted = index_.Insert(order.order_id, h);
    assert(inserted);
    return AddResult::Ok;
  }
};

} // namespace shyamnatesan
