#pragma once

#include <array>
#include <deque>
#include <map>
#include <vector>

namespace dory::ubft {

/**
 * Vector accessor that check boundings in debug builds but not in release.
 *
 */
#ifdef NDEBUG

template <typename T, size_t N>
inline T& uat(std::array<T, N>& v, size_t const index) {
  return v[index];
}

template <typename T, size_t N>
inline T const& uat(std::array<T, N> const& v, size_t const index) {
  return v[index];
}

template <typename T>
inline T& uat(std::deque<T>& v, size_t const index) {
  return v[index];
}

template <typename T>
inline T const& uat(std::deque<T> const& v, size_t const index) {
  return v[index];
}

template <typename T>
inline T& uat(std::vector<T>& v, size_t const index) {
  return v[index];
}

template <typename T>
inline T const& uat(std::vector<T> const& v, size_t const index) {
  return v[index];
}

template <typename K, typename V>
inline V& uat(std::map<K, V>& m, K const& key) {
  return m[key];
}

template <typename K, typename V>
inline V const& uat(std::map<K, V> const& m, K const& key) {
  return m[key];
}

#else

template <typename T, size_t N>
inline T& uat(std::array<T, N>& v, size_t const index) {
  return v.at(index);
}

template <typename T, size_t N>
inline T const& uat(std::array<T, N> const& v, size_t const index) {
  return v.at(index);
}

template <typename T>
inline T& uat(std::deque<T>& v, size_t const index) {
  return v.at(index);
}

template <typename T>
inline T const& uat(std::deque<T> const& v, size_t const index) {
  return v.at(index);
}

template <typename T>
inline T& uat(std::vector<T>& v, size_t const index) {
  return v.at(index);
}

template <typename T>
inline T const& uat(std::vector<T> const& v, size_t const index) {
  return v.at(index);
}

template <typename K, typename V>
inline V& uat(std::map<K, V>& m, K const& key) {
  return m.at(key);
}

template <typename K, typename V>
inline V const& uat(std::map<K, V> const& m, K const& key) {
  return m.at(key);
}
#endif

}  // namespace dory::ubft
