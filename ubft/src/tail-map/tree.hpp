#pragma once

#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <dory/shared/branching.hpp>

namespace dory::ubft {

/**
 * @brief A map of bounded size that is preallocated. Elements must be inserted
 * in order.
 *
 * @tparam K
 * @tparam V
 */
template <typename K, typename V>
class TreeTailMap {
  using Map = std::map<K, V>;

 public:
  using key_type = K;
  using iterator = typename Map::iterator;
  using const_iterator = typename Map::const_iterator;
  using size_type = size_t;

  TreeTailMap(size_t const tail) : tail{tail} {}
  TreeTailMap(TreeTailMap const&) = delete;
  TreeTailMap& operator=(TreeTailMap const&) = delete;
  TreeTailMap(TreeTailMap&&) = default;
  TreeTailMap& operator=(TreeTailMap&&) = default;

  inline bool empty() const { return map.empty(); }

  template <typename... Args>
  inline std::pair<iterator, bool> tryEmplace(K const key, Args&&... args) {
    if (unlikely(key < min)) {
      return {find(key), false};
    }
    min = key;
    while (begin()->first + tail <= min) {
      popFront();
    }
    return map.try_emplace(key, V(std::forward<Args>(args)...));
  }

  inline iterator find(K const key) { return map.find(key); }

  inline const_iterator find(K const key) const { return map.find(key); }

  inline V& front() {
    if (unlikely(empty())) {
      throw std::runtime_error("Empty map while accessing front.");
    }
    return begin()->second;
  }

  inline void popFront() {
    if (unlikely(empty())) {
      throw std::runtime_error("Empty map while accessing front.");
    }
    map.erase(map.begin());
  }

  inline iterator begin() { return map.begin(); }
  inline const_iterator begin() const { return map.begin(); }

  inline iterator rbegin() { return map.rbegin(); }
  inline const_iterator rbegin() const { return map.rbegin(); }

  inline iterator end() { return map.end(); }
  inline const_iterator end() const { return map.end(); }

  void clear() {
    while (begin() != end()) {
      popFront();
    }
    min = 0;
  }

  size_type size() const { return map.size(); }

 private:
  size_t tail;
  Map map;
  K min = 0;
};

}  // namespace dory::ubft
