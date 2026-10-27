#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>

namespace shyamnatesan {

constexpr uint32_t kInvalidPoolIndex = std::numeric_limits<uint32_t>::max();

template <typename T, typename Allocator = std::allocator<T>> class MemPool {
public:
  union Payload {
    T data;
    size_t next_free;
    Payload() : next_free(static_cast<size_t>(kInvalidPoolIndex)) {}
    ~Payload() noexcept {}
  };

  struct Slot {
    Payload payload;
    bool is_free{true};
  };

  using SlotAlloc =
      typename std::allocator_traits<Allocator>::template rebind_alloc<Slot>;

  using SlotTraits = std::allocator_traits<SlotAlloc>;
  using ptr = typename SlotTraits::pointer;

  explicit MemPool(size_t capacity, const Allocator &alloc)
      : alloc_(alloc), capacity_(check_capacity(capacity)) {
    pool_ = SlotTraits::allocate(alloc_, capacity_);
    for (size_t i = 0; i < capacity_; ++i)
      SlotTraits::construct(alloc_, pool_ + i);
    link_free();
  }

  ~MemPool() {
    if (pool_ == nullptr) {
      return;
    }

    destroy_live();

    SlotTraits::deallocate(alloc_, pool_, capacity_);
  }

  MemPool(const MemPool &) = delete;
  MemPool &operator=(const MemPool &) = delete;

  MemPool(MemPool &&) = delete;
  MemPool &operator=(MemPool &&) = delete;

private:
  [[no_unique_address]] SlotAlloc alloc_{};
  size_t capacity_{};

  ptr pool_{nullptr};
  size_t free_list_{};

public:
  size_t Allocate(T t) {
    if (free_list_ == kInvalidPoolIndex) {
      return static_cast<size_t>(kInvalidPoolIndex);
    }

    auto current_free_handle = free_list_;
    auto *slot = pool_ + current_free_handle;

    auto next = slot->payload.next_free;
    std::construct_at(&slot->payload.data, std::move(t));
    slot->is_free = false;
    free_list_ = next;

    return current_free_handle;
  }

  bool Release(size_t handle) {
    if (handle >= capacity_) {
      return false;
    }

    auto *slot = (pool_ + handle);

    if (slot->is_free) {
      return false;
    }
    std::destroy_at(&slot->payload.data);
    slot->is_free = true;
    slot->payload.next_free = free_list_;
    free_list_ = handle;
    return true;
  }

  T *Get(size_t handle) {
    if (handle >= capacity_ || (pool_ + handle)->is_free) {
      return nullptr;
    }
    return &(pool_ + handle)->payload.data;
  }

  void Reset() {
    destroy_live();
    link_free();
  }

private:
  void link_free() {
    free_list_ = 0;
    for (size_t i = 0; i < capacity_; ++i) {
      auto *slot = pool_ + i;
      slot->is_free = true;
      slot->payload.next_free = i + 1 == capacity_ ? kInvalidPoolIndex : i + 1;
    }
  }

  void destroy_live() noexcept {
    for (size_t i{}; i < capacity_; ++i) {
      if (!(pool_ + i)->is_free) {
        std::destroy_at(&(pool_ + i)->payload.data);
      }
    }
  }

  size_t check_capacity(size_t capacity) {
    if (capacity > 0 && capacity < std::numeric_limits<size_t>::max()) {
      return capacity;
    }

    throw std::invalid_argument("capacity exceeds max size_t value or is zero");
  }
};

} // namespace shyamnatesan
