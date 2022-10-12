#pragma once

#include <fmt/core.h>

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <dory/shared/branching.hpp>

#include "../unsafe-at.hpp"

namespace dory::ubft {

/**
 * @brief A map of bounded size that is preallocated. Elements must be inserted
 * in order.
 *
 * @tparam K
 * @tparam V
 */
template <typename K, typename V>
class VectorTailMap {
  template <bool IsConst>
  class GenericIterator {
   public:
    using TM = typename std::conditional_t<IsConst, VectorTailMap const,
                                           VectorTailMap>;
    using value_type =
        typename std::conditional_t<IsConst, std::pair<K, V> const,
                                    std::pair<K, V>>;
    using reference = value_type&;

    GenericIterator(TM& tm, std::optional<size_t> const oi) : tm{tm}, i{oi} {}
    GenericIterator(TM& tm, size_t const i) : tm{tm}, i{i} {}  // Found
    GenericIterator(TM& tm) : tm{tm} {}                        // Not found

    GenericIterator(GenericIterator const&) = default;
    GenericIterator& operator=(GenericIterator const& o) {
      if (unlikely(&tm != &o.tm)) {
        throw std::invalid_argument("Can only assign iterators to the same tm");
      }
      i = o.i;
      return *this;
    }

    // Trivial copyability from non-const iterator.
    template <bool IsConst_ = IsConst, class = std::enable_if_t<IsConst_>>
    GenericIterator(const GenericIterator<false>& o) : tm{o.tm}, i{o.i} {}

    bool operator==(GenericIterator const& o) const { return i == o.i; }

    bool operator!=(GenericIterator const& o) const { return !(*this == o); }

    GenericIterator& operator++() {
      i = uat(tm.dyn_array, *i)->second;
      return *this;
    }

    GenericIterator operator++(int) {
      auto old = *this;
      ++(*this);
      return old;
    }

    GenericIterator operator+(size_t const to_add) const {
      auto copy = *this;
      for (size_t i = 0; i < to_add; i++) {
        ++copy;
      }
      return copy;
    }

    reference operator*() { return uat(tm.dyn_array, *i)->first; }
    value_type* operator->() const { return &uat(tm.dyn_array, *i)->first; }

    void setNext(GenericIterator const& next) {
      uat(tm.dyn_array, *i)->second = next.i;
    }

    void erase() { uat(tm.dyn_array, *i).reset(); }

    std::optional<size_t> const& index() const { return i; }

   private:
    TM& tm;
    std::optional<size_t> i;
    friend GenericIterator<true>;
  };

 public:
  using Iterator = GenericIterator<false>;
  using ConstIterator = GenericIterator<true>;
  using key_type = K;
  using iterator = Iterator;
  using size_type = size_t;

  VectorTailMap(size_t const tail) : tail{tail} { dyn_array.resize(tail); }
  VectorTailMap(VectorTailMap const&) = delete;
  VectorTailMap& operator=(VectorTailMap const&) = delete;
  VectorTailMap(VectorTailMap&&) = default;
  VectorTailMap& operator=(VectorTailMap&&) = default;

  bool empty() const { return begin() == end(); }

  template <typename... Args>
  inline std::pair<Iterator, bool> tryEmplace(K const key, Args&&... args) {
    if (unlikely(key < min)) {
      auto it = find(key);
      if (it == end()) {
        throw std::invalid_argument("Tried to emplace a past element.");
      }
      return {find(key), false};
    }
    min = key;

    // Move/drop the head if is left behind (elements will be skipped).
    while (unlikely(!empty() && key >= begin()->first + tail)) {
      popFront();
    }

    auto const vindex = static_cast<size_t>(key);
    Iterator it = {*this, vindex % tail};
    uat(dyn_array, vindex % tail)
        .emplace(std::make_pair(key, V(std::forward<Args>(args)...)),
                 std::nullopt);
    if (empty()) {
      head = it.index();
    } else {
      rbegin().setNext(it);
    }
    back = it.index();
    return {it, true};
  }

  Iterator find(K const key) {
    auto const vindex = static_cast<size_t>(key);
    auto const index = vindex % tail;
    auto const& opt = uat(dyn_array, index);
    if (opt && opt->first.first == key) {
      return {*this, index};
    }
    return *this;
  }

  ConstIterator find(K const key) const {
    return const_cast<VectorTailMap&>(*this).find(key);
  }

  inline V& front() {
    if (unlikely(empty())) {
      throw std::runtime_error("Empty map while accessing front.");
    }
    return begin()->second;
  }

  void popFront() {
    if (unlikely(empty())) {
      throw std::runtime_error("Empty map while accessing front.");
    }
    if (head == back) {
      back.reset();
    }
    auto new_head = begin() + 1;
    begin().erase();
    head = new_head.index();
  }

  inline Iterator begin() { return {*this, head}; }
  inline ConstIterator begin() const { return {*this, head}; }

  /**
   * @brief Return a FORWARD iterator to the last element. CANNOT be used for
   *        reverse traversal.
   *
   * @return Iterator
   */
  inline Iterator rbegin() { return {*this, back}; }
  inline ConstIterator rbegin() const { return {*this, back}; }

  inline Iterator end() { return *this; }
  inline ConstIterator end() const { return *this; }

  void clear() {
    while (begin() != end()) {
      popFront();
    }
    min = 0;
  }

  /**
   * @brief Compute the size of the tail map.
   *
   * This is O(n).
   *
   * @return size_type
   */
  size_type size() const {
    size_type count = 0;
    for (auto it = begin(); it != end(); it++) {
      count++;
    }
    return count;
  }

 private:
  size_t tail;
  // Each element holds the optional index of its successor.
  std::vector<std::optional<std::pair<std::pair<K, V>, std::optional<size_t>>>>
      dyn_array;
  std::optional<size_t> head;
  std::optional<size_t> back;
  K min = 0;
  friend Iterator;
};

}  // namespace dory::ubft
