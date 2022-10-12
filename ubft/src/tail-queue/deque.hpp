#pragma once

#include <deque>

#include <dory/shared/branching.hpp>

namespace dory::ubft {

/**
 * @brief A queue of bounded size that is allocated on the spot.
 *
 * @tparam T
 */
template <typename T>
class DequeTailQueue {
  using Deque = typename std::deque<T>;
  using iterator = typename Deque::iterator;

 public:
  DequeTailQueue(size_t tail) : tail{tail} {}

  inline size_t size() const { return deque.size(); }

  inline bool empty() const { return deque.empty(); }

  template <typename... Args>
  inline void emplaceBack(Args&&... args) {
    if (deque.size() + 1 > tail) {
      deque.pop_front();
    }
    deque.emplace_back(std::forward<Args>(args)...);
  }

  inline void clear() { deque.clear(); }

  inline T& front() { return deque.front(); }

  inline void popFront() { deque.pop_front(); }

  inline T& back() { return deque.back(); }

  void popBack() { deque.pop_back(); }

  inline iterator begin() { return deque.begin(); }

  inline iterator end() { return deque.end(); }

 private:
  size_t tail;
  Deque deque;
};

}  // namespace dory::ubft
