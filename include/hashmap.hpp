

#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace shyamnatesan {

template <typename K, typename V, typename Allocator = std::allocator<K>>
  requires std::default_initializable<K> && std::copy_constructible<K> &&
           std::assignable_from<K &, const K &> &&
           std::equality_comparable<K> && std::default_initializable<V> &&
           std::assignable_from<V &, const V &>
class OpenAddressingHashMap {

  enum class SlotState {
    Live,
    Dead,
    Empty,
  };

  struct Slot {
    K k{};
    V v{};
    SlotState state{SlotState::Empty};
  };

  using SlotAlloc =
      typename std::allocator_traits<Allocator>::template rebind_alloc<Slot>;

  using SlotTraits = std::allocator_traits<SlotAlloc>;

  using ptr = typename SlotTraits::pointer;

public:
  explicit OpenAddressingHashMap(size_t capacity,
                                 const Allocator &alloc = Allocator())
      : alloc_(alloc), capacity_(check_capacity(capacity)) {
    slots_ = SlotTraits::allocate(alloc_, capacity_);
    other_ = SlotTraits::allocate(alloc_, capacity_);
    for (size_t i{}; i < capacity_; ++i) {
      SlotTraits::construct(alloc_, slots_ + i);
      SlotTraits::construct(alloc_, other_ + i);
    }
  }

  ~OpenAddressingHashMap() {
    if (slots_ == nullptr) {
      return;
    }

    for (size_t i{}; i < capacity_; ++i) {
      SlotTraits::destroy(alloc_, slots_ + i);
      SlotTraits::destroy(alloc_, other_ + i);
    }

    SlotTraits::deallocate(alloc_, slots_, capacity_);
    SlotTraits::deallocate(alloc_, other_, capacity_);
  }

  OpenAddressingHashMap(const OpenAddressingHashMap &) = delete;
  OpenAddressingHashMap &operator=(const OpenAddressingHashMap &) = delete;

  OpenAddressingHashMap(OpenAddressingHashMap &&) = delete;
  OpenAddressingHashMap &operator=(OpenAddressingHashMap &&) = delete;

private:
  [[no_unique_address]] SlotAlloc alloc_{};
  size_t capacity_;
  size_t size_{};
  size_t dead_{};

  ptr slots_{nullptr};
  ptr other_{nullptr};

public:
  bool Insert(const K &k, V v) {
    if (size_ == capacity_) {
      return false;
    }

    if (dead_ > size_) {
      rehash();
    }

    size_t idx = hash(k);
    size_t start = idx;
    size_t first_dead_idx = capacity_;

    for (;;) {
      if ((slots_ + idx)->state == SlotState::Empty) {
        break;
      }
      if ((slots_ + idx)->state == SlotState::Live && (slots_ + idx)->k == k) {
        // key already exists
        (slots_ + idx)->v = v;
        return true;
      }
      if ((slots_ + idx)->state == SlotState::Dead &&
          first_dead_idx == capacity_) {
        first_dead_idx = idx;
      }
      idx = (idx + 1) & (capacity_ - 1);
      if (idx == start) {
        if (first_dead_idx == capacity_) {
          return false;
        }
        break;
      }
    }
    // now we have found a slot to insert, this is either a dead slot or an
    // empty slot.
    //
    // if we have moved past a dead slot, we use that.
    if (first_dead_idx != capacity_) {
      idx = first_dead_idx;
      --dead_;
    }
    (slots_ + idx)->k = k;
    (slots_ + idx)->v = v;
    (slots_ + idx)->state = SlotState::Live;
    ++size_;
    return true;
  }

  V *Find(const K &k) {
    if (size_ == 0) {
      return nullptr;
    }
    auto idx = Probe(k);
    if (idx == capacity_ || (slots_ + idx)->state == SlotState::Empty) {
      return nullptr;
    }

    return &(slots_ + static_cast<size_t>(idx))->v;
  }

  bool Erase(const K &k) {
    if (size_ == 0) {
      return false;
    }
    auto idx = Probe(k);
    if (idx == capacity_ || (slots_ + idx)->state == SlotState::Empty) {
      return false;
    }

    (slots_ + static_cast<size_t>(idx))->state = SlotState::Dead;
    --size_;
    ++dead_;
    return true;
  }

private:
  static size_t check_capacity(size_t capacity) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
      throw std::invalid_argument("capacity must be a non-zero power of two");
    }

    return capacity;
  }

  size_t hash(const K &k) const {
    return static_cast<size_t>(k) & (capacity_ - 1);
  }

  size_t Probe(const K &k) const {
    size_t idx = hash(k);
    size_t start = idx;
    for (;;) {
      // we have reached an empty slot, starting from the initial probe index
      // this means, this key/value that may have been inserted, must have been
      // inserted within this boundary (initial_probe_idx.. an_empty_slot)
      // so, this means, the key does not exist in this buffer.
      if ((slots_ + idx)->state == SlotState::Empty) {
        break;
      }
      // break early if we find a live slot + matching key
      if ((slots_ + idx)->state == SlotState::Live && (slots_ + idx)->k == k) {
        break;
      }
      idx = (idx + 1) & (capacity_ - 1);
      if (idx == start) {
        return capacity_;
      }
    }
    return idx;
  }

  void rehash() {
    // other_ must start fully Empty. Clear it first.
    for (size_t i = 0; i < capacity_; ++i) {
      (other_ + i)->state = SlotState::Empty;
    }

    size_t moved = 0;
    for (size_t i = 0; i < capacity_; ++i) {
      if ((slots_ + i)->state != SlotState::Live) {
        continue;
      }
      size_t idx = hash((slots_ + i)->k);
      size_t probes = 0;
      while ((other_ + idx)->state == SlotState::Live) {
        idx = (idx + 1) & (capacity_ - 1);
        assert(++probes < capacity_ && "rehash: other_ overfull");
      }
      (other_ + idx)->k = (slots_ + i)->k;
      (other_ + idx)->v = (slots_ + i)->v;
      (other_ + idx)->state = SlotState::Live;
      ++moved;
    }

    std::swap(slots_, other_);
    dead_ = 0;
    assert(moved == size_);
  }
};

} // namespace shyamnatesan
