// Explicit-state model check of FastBook against a reference price-time book.
//
// Breadth-first search over every internal state FastBook can reach in a small
// universe (few ids that collide in the order table, few prices, quantities
// 1 and UINT32_MAX), until no new state appears. At every transition it checks:
//   * observables equal the reference book: AddResult, unfilled remainder,
//     the exact trade sequence, CancelOrder's result, BestBid/Ask/Qty;
//   * internal invariants: level order, per-level FIFO contents equal to the
//     reference, linked-list links, aggregates, order table <-> pool agreement,
//     linear-probing reachability, free-list integrity.
// Build with sanitizers and without NDEBUG so asserts and UB also fail the run.
//
// Private state is read through `#define private public`. Every standard header
// the library uses is included first so the macro only touches project headers.
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#define private public
#include "fastbook.hpp"
#undef private

using namespace shyamnatesan;

namespace {

constexpr Quantity kMaxQty = std::numeric_limits<Quantity>::max();

struct Op {
  bool cancel;
  OrderId id;
  Side side;
  Price price;
  Quantity qty;
};

std::string OpStr(const Op &op) {
  char buf[128];
  if (op.cancel) {
    std::snprintf(buf, sizeof buf, "CancelOrder(%u)", op.id);
  } else {
    std::snprintf(buf, sizeof buf, "AddOrder{%s id=%u price=%lld qty=%s}",
                  op.side == Side::Buy ? "Buy" : "Sell", op.id,
                  static_cast<long long>(op.price),
                  op.qty == kMaxQty ? "UINT32_MAX" : std::to_string(op.qty).c_str());
  }
  return buf;
}

const char *ResultStr(AddResult r) {
  switch (r) {
  case AddResult::Ok: return "Ok";
  case AddResult::Duplicate: return "Duplicate";
  case AddResult::LevelsFull: return "LevelsFull";
  case AddResult::PoolFull: return "PoolFull";
  case AddResult::QuantityOverflow: return "QuantityOverflow";
  }
  return "?";
}

// ------------------------------------------------------------ reference book
struct Model {
  using Queue = std::deque<std::pair<OrderId, Quantity>>;
  std::map<Price, Queue> bids, asks; // best bid = rbegin, best ask = begin
  size_t max_orders, max_levels;

  size_t Resting() const {
    size_t n = 0;
    for (auto *side : {&bids, &asks})
      for (auto &[p, q] : *side) n += q.size();
    return n;
  }
  bool Has(OrderId id) const {
    for (auto *side : {&bids, &asks})
      for (auto &[p, q] : *side)
        for (auto &[i, qty] : q)
          if (i == id) return true;
    return false;
  }

  AddResult Add(Order &o, std::vector<Trade> &trades) {
    if (Has(o.order_id)) return AddResult::Duplicate;
    auto &opp = o.side == Side::Buy ? asks : bids;
    while (o.quantity > 0 && !opp.empty()) {
      auto it = o.side == Side::Buy ? opp.begin() : std::prev(opp.end());
      if (o.side == Side::Buy ? o.price < it->first : o.price > it->first) break;
      auto &q = it->second;
      while (o.quantity > 0 && !q.empty()) {
        Quantity fill = std::min(o.quantity, q.front().second);
        o.quantity -= fill;
        q.front().second -= fill;
        trades.push_back(Trade{it->first, fill, o.order_id, q.front().first});
        if (q.front().second != 0) break;
        q.pop_front();
      }
      if (q.empty()) opp.erase(it);
    }
    if (o.quantity == 0) return AddResult::Ok;
    auto &same = o.side == Side::Buy ? bids : asks;
    auto lvl = same.find(o.price);
    if (lvl == same.end() && same.size() == max_levels) return AddResult::LevelsFull;
    if (lvl != same.end()) {
      uint64_t total = 0;
      for (auto &[i, qty] : lvl->second) total += qty;
      if (total + o.quantity > kMaxQty) return AddResult::QuantityOverflow;
    }
    if (Resting() == max_orders) return AddResult::PoolFull;
    same[o.price].push_back({o.order_id, o.quantity});
    return AddResult::Ok;
  }

  bool Cancel(OrderId id) {
    for (auto *side : {&bids, &asks})
      for (auto it = side->begin(); it != side->end(); ++it)
        for (auto q = it->second.begin(); q != it->second.end(); ++q)
          if (q->first == id) {
            it->second.erase(q);
            if (it->second.empty()) side->erase(it);
            return true;
          }
    return false;
  }

  Price BestBid() const { return bids.empty() ? 0 : bids.rbegin()->first; }
  Price BestAsk() const { return asks.empty() ? 0 : asks.begin()->first; }
  static Quantity Total(const Queue &q) {
    Quantity t = 0;
    for (auto &[i, qty] : q) t += qty;
    return t;
  }
  Quantity BestBidQty() const { return bids.empty() ? 0 : Total(bids.rbegin()->second); }
  Quantity BestAskQty() const { return asks.empty() ? 0 : Total(asks.begin()->second); }
};

// ------------------------------------------------------------ the checker
template <size_t MaxLevels> struct Checker {
  using Book = FastBook<std::allocator<Order>, MaxLevels>;

  const char *name;
  size_t max_orders;
  size_t depth_bound; // 0: search until no new state appears
  std::vector<Op> alphabet;

  struct Node {
    int64_t parent;
    Op op;
  };
  std::vector<Node> nodes;

  std::vector<Op> PathTo(int64_t idx) const {
    std::vector<Op> path;
    for (; idx > 0; idx = nodes[idx].parent) path.push_back(nodes[idx].op);
    std::reverse(path.begin(), path.end());
    return path;
  }

  // Applies op to both, returns "" or a description of the first mismatch.
  std::string Step(Book &book, Model &model, const Op &op) {
    char buf[512];
    if (op.cancel) {
      bool got = book.CancelOrder(op.id);
      bool want = model.Cancel(op.id);
      if (got != want) {
        std::snprintf(buf, sizeof buf, "CancelOrder returned %d, reference %d", got, want);
        return buf;
      }
    } else {
      Order bo{op.price, op.id, op.qty, op.side}, mo = bo;
      std::vector<Trade> bt, mt;
      AddResult got = book.AddOrder(bo, [&](const Trade &t) { bt.push_back(t); });
      AddResult want = model.Add(mo, mt);
      if (got != want) {
        std::snprintf(buf, sizeof buf, "AddOrder returned %s, reference %s",
                      ResultStr(got), ResultStr(want));
        return buf;
      }
      if (bo.quantity != mo.quantity) {
        std::snprintf(buf, sizeof buf, "unfilled remainder %u, reference %u",
                      bo.quantity, mo.quantity);
        return buf;
      }
      if (bt.size() != mt.size()) {
        std::snprintf(buf, sizeof buf, "%zu trades, reference %zu", bt.size(), mt.size());
        return buf;
      }
      for (size_t i = 0; i < bt.size(); ++i) {
        const Trade &a = bt[i], &b = mt[i];
        if (a.price != b.price || a.quantity != b.quantity || a.taker != b.taker ||
            a.maker != b.maker) {
          std::snprintf(buf, sizeof buf,
                        "trade %zu is {price=%lld qty=%u taker=%u maker=%u}, "
                        "reference {price=%lld qty=%u taker=%u maker=%u}",
                        i, (long long)a.price, a.quantity, a.taker, a.maker,
                        (long long)b.price, b.quantity, b.taker, b.maker);
          return buf;
        }
      }
    }
    if (book.BestBid() != model.BestBid() || book.BestAsk() != model.BestAsk() ||
        book.BestBidQty() != model.BestBidQty() || book.BestAskQty() != model.BestAskQty()) {
      std::snprintf(buf, sizeof buf,
                    "top of book bid %lld x %u / ask %lld x %u, reference "
                    "bid %lld x %u / ask %lld x %u",
                    (long long)book.BestBid(), book.BestBidQty(), (long long)book.BestAsk(),
                    book.BestAskQty(), (long long)model.BestBid(), model.BestBidQty(),
                    (long long)model.BestAsk(), model.BestAskQty());
      return buf;
    }
    return Invariants(book, model);
  }

  std::string Invariants(Book &book, const Model &model) {
    auto &pool = book.orders_;
    auto &table = book.order_table_;
    size_t listed = 0;

    for (int s = 0; s < 2; ++s) {
      const Side side = s == 0 ? Side::Buy : Side::Sell;
      auto &levels = s == 0 ? book.bids_ : book.asks_;
      const auto &ref = s == 0 ? model.bids : model.asks;
      const std::string sname = s == 0 ? "bids_" : "asks_";
      if (levels.size() != ref.size())
        return sname + " has " + std::to_string(levels.size()) + " levels, reference " +
               std::to_string(ref.size());
      // bids_ ascend and asks_ descend, so back() is the best price.
      auto rit = ref.begin();
      for (size_t i = 0; i < levels.size(); ++i, ++rit) {
        const PriceLevel &lvl = levels[side == Side::Buy ? i : levels.size() - 1 - i];
        const std::string where = sname + " level " + std::to_string(lvl.price);
        if (lvl.price != rit->first) return sname + " out of price order";
        const auto &q = rit->second;
        if (lvl.order_count != q.size() || lvl.total_quantity != Model::Total(q))
          return where + " order_count/total_quantity disagree with its orders";
        uint32_t h = lvl.orders_head, prev = kInvalidPoolIndex;
        for (auto &[id, qty] : q) {
          auto *n = pool.Get(h);
          if (n == nullptr) return where + " links to a free pool slot";
          if (n->order.order_id != id || n->order.quantity != qty ||
              n->order.price != lvl.price || n->order.side != side)
            return where + " FIFO differs from reference at order " + std::to_string(id);
          if (n->prev != prev) return where + " broken prev link at order " + std::to_string(id);
          auto *slot = table.Find(id);
          if (slot == nullptr || *slot != h)
            return "order table does not map id " + std::to_string(id) + " to its pool handle";
          prev = h;
          h = static_cast<uint32_t>(n->next);
          ++listed;
        }
        if (h != kInvalidPoolIndex || lvl.orders_tail != prev)
          return where + " list does not end at orders_tail";
      }
    }

    size_t live = 0;
    for (size_t i = 0; i < pool.capacity_; ++i) live += !pool.pool_[i].is_free;
    if (live != listed)
      return "pool has " + std::to_string(live) + " live slots but levels list " +
             std::to_string(listed);
    size_t free_walk = 0;
    for (size_t f = pool.free_list_; f != kInvalidPoolIndex; f = pool.pool_[f].payload.next_free)
      if (f >= pool.capacity_ || !pool.pool_[f].is_free || ++free_walk > pool.capacity_)
        return "pool free list is corrupt";
    if (free_walk + live != pool.capacity_) return "pool free list misses free slots";

    if (table.size_ != listed)
      return "order table size_ " + std::to_string(table.size_) + " but " +
             std::to_string(listed) + " orders rest";
    using SlotState = std::remove_cvref_t<decltype(table.slots_[0].state)>;
    const size_t cap = table.capacity_, mask = cap - 1;
    size_t table_live = 0;
    for (size_t i = 0; i < cap; ++i) {
      auto &sl = table.slots_[i];
      if (sl.state != SlotState::Live) continue;
      ++table_live;
      // Linear probing: every slot from the key's home up to i must be Live.
      for (size_t j = sl.k & mask; j != i; j = (j + 1) & mask)
        if (table.slots_[j].state != SlotState::Live)
          return "order table key " + std::to_string(sl.k) +
                 " is unreachable: Empty slot between its home and its slot";
      auto *n = pool.Get(sl.v);
      if (n == nullptr || n->order.order_id != sl.k)
        return "order table maps id " + std::to_string(sl.k) + " to the wrong pool slot";
    }
    if (table_live != listed) return "order table holds stale ids";
    return "";
  }

  // Byte image of every field that affects future behavior, plus the model.
  std::string Snapshot(Book &book, const Model &model) {
    std::string s;
    auto put = [&](const auto &v) { s.append(reinterpret_cast<const char *>(&v), sizeof v); };
    for (auto *levels : {&book.bids_, &book.asks_}) {
      put(levels->size());
      for (size_t i = 0; i < levels->size(); ++i) {
        const PriceLevel &l = (*levels)[i];
        put(l.price); put(l.total_quantity); put(l.order_count);
        put(l.orders_head); put(l.orders_tail);
      }
    }
    auto &pool = book.orders_;
    put(pool.free_list_);
    for (size_t i = 0; i < pool.capacity_; ++i) {
      auto &sl = pool.pool_[i];
      put(sl.is_free);
      if (sl.is_free) {
        put(sl.payload.next_free);
      } else {
        auto &n = sl.payload.data;
        put(n.order.price); put(n.order.order_id); put(n.order.quantity);
        put(n.order.side); put(n.next); put(n.prev);
      }
    }
    auto &table = book.order_table_;
    using SlotState = std::remove_cvref_t<decltype(table.slots_[0].state)>;
    put(table.size_);
    for (size_t i = 0; i < table.capacity_; ++i) {
      auto &sl = table.slots_[i];
      put(sl.state);
      if (sl.state == SlotState::Live) { put(sl.k); put(sl.v); }
    }
    for (auto *side : {&model.bids, &model.asks}) {
      put(side->size());
      for (auto &[p, q] : *side) {
        put(p); put(q.size());
        for (auto &[id, qty] : q) { put(id); put(qty); }
      }
    }
    return s;
  }

  void Report(int64_t parent, const Op &op, const std::string &why) {
    auto path = PathTo(parent);
    path.push_back(op);
    std::printf("FAIL [%s] after %zu steps: %s\n", name, path.size(), why.c_str());
    std::printf("  shortest counterexample (fresh book, max_orders=%zu, MaxLevels=%zu):\n",
                max_orders, MaxLevels);
    for (size_t i = 0; i < path.size(); ++i)
      std::printf("  %2zu. %s\n", i + 1, OpStr(path[i]).c_str());
  }

  // FastBook is not copyable; copy every field Snapshot covers. Pool slots hold
  // a union with a user-declared destructor, so they are copied bytewise.
  static void CopyInto(Book &dst, Book &src) {
    dst.bids_ = src.bids_;
    dst.asks_ = src.asks_;
    std::memcpy(static_cast<void *>(dst.orders_.pool_), src.orders_.pool_,
                src.orders_.capacity_ * sizeof(*src.orders_.pool_));
    dst.orders_.free_list_ = src.orders_.free_list_;
    std::memcpy(static_cast<void *>(dst.order_table_.slots_), src.order_table_.slots_,
                src.order_table_.capacity_ * sizeof(*src.order_table_.slots_));
    dst.order_table_.size_ = src.order_table_.size_;
  }

  bool Run(size_t state_limit) {
    std::unordered_map<std::string, int64_t> seen;
    nodes.push_back({-1, {}});
    {
      Book book(max_orders, std::allocator<Order>());
      Model model{{}, {}, max_orders, MaxLevels};
      seen.emplace(Snapshot(book, model), 0);
    }
    size_t transitions = 0, max_depth = 0;
    Book base(max_orders, std::allocator<Order>());
    Book book(max_orders, std::allocator<Order>());
    for (size_t head = 0; head < nodes.size(); ++head) {
      // Replay once per state; every step on the path was checked when first reached.
      const auto path = PathTo(static_cast<int64_t>(head));
      if (depth_bound != 0 && path.size() == depth_bound) continue;
      if (path.size() > max_depth) {
        max_depth = path.size();
        std::fprintf(stderr, "  [%s] depth %zu: %zu states found, %zu expanded\n", name,
                     max_depth, nodes.size(), head);
      }
      Book fresh_base(max_orders, std::allocator<Order>());
      CopyInto(base, fresh_base);
      Model base_model{{}, {}, max_orders, MaxLevels};
      for (const Op &p : path) Step(base, base_model, p);
      for (const Op &op : alphabet) {
        CopyInto(book, base);
        Model model = base_model;
        ++transitions;
        std::string why = Step(book, model, op);
        if (!why.empty()) {
          Report(static_cast<int64_t>(head), op, why);
          return false;
        }
        if (seen.emplace(Snapshot(book, model), static_cast<int64_t>(nodes.size())).second) {
          nodes.push_back({static_cast<int64_t>(head), op});
          if (nodes.size() > state_limit) {
            std::printf("INCOMPLETE [%s] state limit %zu hit; raise --state-limit\n",
                        name, state_limit);
            return false;
          }
        }
      }
    }
    if (depth_bound == 0)
      std::printf("PASS [%s] fixpoint: %zu reachable states, %zu transitions, "
                  "deepest shortest path %zu\n",
                  name, nodes.size(), transitions, max_depth);
    else
      std::printf("PASS [%s] bounded: every sequence of <= %zu ops (%zu states, "
                  "%zu transitions)\n",
                  name, depth_bound, nodes.size(), transitions);
    return true;
  }
};

std::vector<Op> Alphabet(std::initializer_list<OrderId> ids, std::initializer_list<Price> prices,
                         std::initializer_list<Quantity> qtys) {
  std::vector<Op> ops;
  for (OrderId id : ids) {
    for (Side side : {Side::Buy, Side::Sell})
      for (Price p : prices)
        for (Quantity q : qtys) ops.push_back({false, id, side, p, q});
    ops.push_back({true, id, Side::Buy, 0, 0});
  }
  return ops;
}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  size_t state_limit = 5'000'000;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--state-limit") == 0)
      state_limit = std::strtoull(argv[i + 1], nullptr, 10);

  bool ok = true;
  // Quantities {1, 2} cover taker and maker partial fills and multi-order
  // sweeps while keeping the state space finite, so these two run to fixpoint.
  //
  // pool-2: order table has 4 slots (mask 3): ids 3 and 7 share home slot 3, so
  // 7 wraps to slot 0, which is id 0's home. The pool holds 2 orders, so a
  // third resting order is PoolFull; 3 prices on one side hit LevelsFull.
  {
    Checker<2> c{"pool-2", 2, 0, Alphabet({3, 7, 0}, {1, 2, 3}, {1, 2}), {}};
    ok &= c.Run(state_limit);
  }
  // probe-4: order table has 8 slots (mask 7): 7 and 15 share home 7, 0 and 8
  // share home 0, so probe chains cross the wrap point and backward-shift
  // deletion moves entries across it. Partial fills and LevelsFull are left to
  // pool-2 to keep this space small.
  {
    Checker<2> c{"probe-4", 4, 0, Alphabet({7, 15, 0, 8}, {1, 2}, {1}), {}};
    ok &= c.Run(state_limit);
  }
  // overflow: UINT32_MAX makes level totals wrap-adjacent (QuantityOverflow),
  // but each partial fill of a MAX order is a new state, so no fixpoint
  // exists. This one is bounded by sequence length.
  {
    Checker<2> c{"overflow", 2, 7, Alphabet({3, 7, 0}, {1, 2}, {1, kMaxQty}), {}};
    ok &= c.Run(state_limit);
  }
  return ok ? 0 : 1;
}
