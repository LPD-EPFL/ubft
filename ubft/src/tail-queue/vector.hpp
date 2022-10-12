#pragma once

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <dory/shared/branching.hpp>

#include "../unsafe-at.hpp"

namespace dory::ubft {

/**
 * @brief A queue of bounded size that is preallocated.
 *
 * IT DOES NOT ALWAYS PERFORM BETTER THAN THE STL VERSION, TRY BOTH.
 *
 * @tparam T
 */
template <typename T>
class VectorTailQueue {
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = T;
    using reference = T&;

    Iterator(VectorTailQueue& tq, size_t const p) : tq{tq}, p{p} {}

    bool operator==(Iterator const& o) const { return p == o.p; }

    bool operator!=(Iterator const& o) const { return !(*this == o); }

    Iterator& operator++() {
      p++;
      return *this;
    }

    reference operator*() { return tq.at(p); }
    value_type* operator->() const { return &tq.at(p); }

   private:
    VectorTailQueue& tq;
    size_t p;
  };

 public:
  VectorTailQueue(size_t tail) : tail{tail} { dyn_array.resize(tail); }

  inline size_t size() const { return next - head; }

  inline bool empty() const { return size() == 0; }

  template <typename... Args>
  inline void emplaceBack(Args&&... args) {
    auto const dest = next++;
    if (unlikely(head + tail == dest)) {
      head++;
    }
    uat(dyn_array, dest % tail).emplace(std::forward<Args>(args)...);
  }

  inline void clear() {
    for (size_t p = head; p != next; p++) {
      uat(dyn_array, p % tail).reset();
    }
    head = next;
  }

  inline T& at(size_t const p) {
    if (p < head || p >= next) {
      throw std::invalid_argument("Out of bounds");
    }
    return *uat(dyn_array, p % tail);
  }

  inline T& front() { return at(head); }

  void popFront() {
    uat(dyn_array, head % tail).reset();
    head++;
  }

  inline T& back() { return *uat(dyn_array, next - 1); }

  inline void popBack() {
    uat(dyn_array, (next - 1) % tail).reset();
    next--;
  }

  inline Iterator begin() { return {*this, head}; }

  inline Iterator end() { return {*this, next}; }

 private:
  size_t tail;
  std::vector<std::optional<T>> dyn_array;
  size_t head = 0;
  size_t next = 0;
};

}  // namespace dory::ubft
