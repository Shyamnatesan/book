

#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

namespace shyamnatesan {

template <typename T, size_t Cap>
  requires std::is_trivially_copyable_v<T>
class InlineBuffer {
public:
  template <typename... Args> T &emplace_back(Args &&...args) {
    assert(size_ < Cap);

    T &slot = data_[size_];
    std::construct_at(&slot, std::forward<Args>(args)...);
    ++size_;
    return slot;
  }

  template <typename... Args> T &emplace_at(size_t idx, Args &&...args) {
    assert(size_ < Cap && idx <= size_);
    if (idx < size_) {
      std::memmove(&data_[idx + 1], &data_[idx], (size_ - idx) * sizeof(T));
    }
    T &slot = data_[idx];
    std::construct_at(&slot, std::forward<Args>(args)...);
    ++size_;
    return slot;
  }

  template <typename K, typename Less>
  size_t lower_bound(const K &key, Less less) const {
    size_t i{size_};
    while (i > 0) {
      --i;
      if (less(data_[i], key)) {
        return i + 1;
      }
    }
    return 0;
  }

  void erase(size_t idx) {
    assert(idx < size_);
    std::destroy_at(&data_[idx]);
    if (idx + 1 < size_) {
      std::memmove(&data_[idx], &data_[idx + 1], (size_ - idx - 1) * sizeof(T));
    }
    --size_;
  }

  T &back() {
    assert(size_ > 0);
    return data_[size_ - 1];
  }

  const T &back() const {
    assert(size_ > 0);
    return data_[size_ - 1];
  }

  T &operator[](size_t idx) {
    assert(idx < size_);
    return data_[idx];
  }

  const T &operator[](size_t idx) const {
    assert(idx < size_);
    return data_[idx];
  }

  bool empty() const { return size_ == 0; }

  size_t size() const { return size_; }

private:
  T data_[Cap];
  size_t size_{};
};

} // namespace shyamnatesan
