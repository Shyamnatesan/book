

#pragma once

#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace shyamnatesan {

// A hash maps a key to its home slot in a table of 2^bits slots.
struct MaskHash {
  template <typename K> size_t operator()(const K &k, unsigned bits) const {
    return static_cast<size_t>(k) & ((size_t{1} << bits) - 1);
  }
};

// Multiplicative hashing keeps the top bits of k * 2^32/phi, so ids that share
// their low bits (strided sequences) still spread across the table.
struct FibonacciHash {
  size_t operator()(uint32_t k, unsigned bits) const {
    return static_cast<size_t>((uint64_t{k * 0x9E3779B9u} << bits) >> 32);
  }
};

template <typename K, typename V, typename Allocator = std::allocator<K>,
          typename Hash = MaskHash>
  requires std::default_initializable<K> && std::copy_constructible<K> &&
           std::assignable_from<K &, const K &> &&
           std::equality_comparable<K> && std::default_initializable<V> &&
           std::assignable_from<V &, const V &>
class OpenAddressingHashMap {

  enum class SlotState : uint8_t {
    Live,
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
  static constexpr size_t kSlotSize = sizeof(Slot);

  explicit OpenAddressingHashMap(size_t capacity,
                                 const Allocator &alloc = Allocator())
      : alloc_(alloc), capacity_(check_capacity(capacity)),
        bits_(static_cast<unsigned>(std::countr_zero(capacity))) {
    slots_ = SlotTraits::allocate(alloc_, capacity_);
    for (size_t i{}; i < capacity_; ++i) {
      SlotTraits::construct(alloc_, slots_ + i);
    }
  }

  ~OpenAddressingHashMap() {
    if (slots_ == nullptr) {
      return;
    }

    for (size_t i{}; i < capacity_; ++i) {
      SlotTraits::destroy(alloc_, slots_ + i);
    }

    SlotTraits::deallocate(alloc_, slots_, capacity_);
  }

  OpenAddressingHashMap(const OpenAddressingHashMap &) = delete;
  OpenAddressingHashMap &operator=(const OpenAddressingHashMap &) = delete;

  OpenAddressingHashMap(OpenAddressingHashMap &&) = delete;
  OpenAddressingHashMap &operator=(OpenAddressingHashMap &&) = delete;

private:
  [[no_unique_address]] SlotAlloc alloc_{};
  size_t capacity_;
  unsigned bits_;
  size_t size_{};

  ptr slots_{nullptr};

public:
  bool Insert(const K &k, V v) {
    if (size_ == capacity_) {
      return false;
    }

    size_t idx = hash(k);

    // size_ < capacity_ here, so an Empty slot always exists on the probe path.
    for (;;) {
      if ((slots_ + idx)->state == SlotState::Empty) {
        break;
      }
      if ((slots_ + idx)->k == k) {
        // key already exists
        (slots_ + idx)->v = v;
        return true;
      }
      idx = (idx + 1) & (capacity_ - 1);
    }
    (slots_ + idx)->k = k;
    (slots_ + idx)->v = v;
    (slots_ + idx)->state = SlotState::Live;
    ++size_;
    return true;
  }

  V *Find(const K &k) {
    return const_cast<V *>(std::as_const(*this).Find(k));
  }

  const V *Find(const K &k) const {
    if (size_ == 0) {
      return nullptr;
    }
    auto idx = Probe(k);
    if (idx == capacity_ || (slots_ + idx)->state == SlotState::Empty) {
      return nullptr;
    }

    return &(slots_ + static_cast<size_t>(idx))->v;
  }

  size_t Size() const { return size_; }

  // Backward-shift deletion: no tombstones. Pointers returned by Find are
  // invalidated by an Erase of a different key (entries may be moved).
  bool Erase(const K &k) {
    if (size_ == 0) {
      return false;
    }
    size_t i = Probe(k);
    if (i == capacity_ || (slots_ + i)->state == SlotState::Empty) {
      return false;
    }
    const size_t mask = capacity_ - 1;
    size_t j = i;
    // The step bound guards a completely full table, which has no Empty slot.
    for (size_t steps = 1; steps < capacity_; ++steps) {
      j = (j + 1) & mask;
      if ((slots_ + j)->state == SlotState::Empty) {
        break;
      }
      const size_t h = hash((slots_ + j)->k);
      // Slot j may fill hole i iff its home h is not cyclically in (i, j].
      if (((j - h) & mask) >= ((j - i) & mask)) {
        (slots_ + i)->k = (slots_ + j)->k;
        (slots_ + i)->v = (slots_ + j)->v;
        i = j;
      }
    }
    (slots_ + i)->state = SlotState::Empty;
    --size_;
    return true;
  }

private:
  static size_t check_capacity(size_t capacity) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
      throw std::invalid_argument("capacity must be a non-zero power of two");
    }

    return capacity;
  }

  size_t hash(const K &k) const { return Hash{}(k, bits_); }

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
      if ((slots_ + idx)->k == k) {
        break;
      }
      idx = (idx + 1) & (capacity_ - 1);
      if (idx == start) {
        return capacity_;
      }
    }
    return idx;
  }
};

} // namespace shyamnatesan
