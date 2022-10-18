#pragma once

#define CHECK_BUFFER_THREAD false

#if CHECK_BUFFER_THREAD
#include <fmt/core.h>
#include <thread>
#endif

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <dory/crypto/hash/blake3.hpp>
#include <dory/shared/branching.hpp>
#include <dory/shared/move-indicator.hpp>

namespace dory::ubft {

class Pool;

/**
 * @brief Allocator adaptor that interposes construct() calls to convert value
 *        initialization into default initialization.
 *
 * We use this adaptor so that buffers are not initialized upon resize().
 *
 * https://stackoverflow.com/a/21028912
 *
 * @tparam T the type to allocate
 * @tparam A the underlying
 */
template <typename T, typename A = std::allocator<T>>
class DefaultInitAllocator : public A {
  typedef std::allocator_traits<A> a_t;

 public:
  template <typename U>
  struct rebind {
    using other =
        DefaultInitAllocator<U, typename a_t::template rebind_alloc<U>>;
  };

  using A::A;

  template <typename U>
  void construct(U *ptr) noexcept(
      std::is_nothrow_default_constructible<U>::value) {
    ::new (static_cast<void *>(ptr)) U;
  }
  template <typename U, typename... Args>
  void construct(U *ptr, Args &&... args) noexcept {
    a_t::construct(static_cast<A &>(*this), ptr, std::forward<Args>(args)...);
  }
};

/**
 * @brief A Buffer encapsulates an std::vector and prevents any allocation and
 * copy.
 *
 * A Buffer can only be moved. It cannot be resized beyond its original size.
 * A Buffer holds an optional reference to the vector where it needs to be
 * returned upon destruction.
 *
 * Warning, if using it, make sure that the origin vector outlives the buffer.
 *
 * The buffer can be offset to the left.
 */
class Buffer {
  using DynArray = std::vector<uint8_t, DefaultInitAllocator<uint8_t>>;
  using HomeVectorRef = std::reference_wrapper<std::vector<Buffer>>;

 public:
  using ConstIterator = DynArray::const_iterator;

  Buffer(size_t const size) : max_size{size}, dynarray(size) {}
  // Buffer(DynArray &&DynArray) : max_size{DynArray.capacity()},
  // DynArray{std::move(DynArray)} {}

  ~Buffer() {
    if (moved || !home_vector) {
      return;
    }
    left_offset = 0;
    resize(max_size);
    auto &hv = home_vector->get();
    home_vector.reset();
#if CHECK_BUFFER_THREAD
    if (thread_id != std::this_thread::get_id()) {
      fmt::print(
          "Pools are thread-unsafe: returning buffer from another thread.\n");
      std::terminate();
    }
#endif
    hv.push_back(std::move(*this));
  }

  Buffer(Buffer const &) = delete;
  Buffer &operator=(Buffer const &) = delete;
  Buffer(Buffer &&) = default;
  Buffer &operator=(Buffer &&o) = default;

  bool operator==(Buffer const &o) const {
    return std::equal(cbegin(), cend(), o.cbegin());
  }

  bool operator!=(Buffer const &o) const { return !(*this == o); }

  ConstIterator cbegin() const {
    return dynarray.cbegin() +
           static_cast<ConstIterator::difference_type>(left_offset);
  }
  ConstIterator cend() const { return dynarray.cend(); }

  uint8_t *data() { return dynarray.data() + left_offset; }
  uint8_t const *data() const { return dynarray.data() + left_offset; }

  size_t size() const { return dynarray.size() - left_offset; }

  void resize(size_t const size) {
    if (unlikely(size + left_offset > max_size)) {
      throw std::runtime_error(
          "Resize tried to reallocate beyond the initial size.");
    }
    dynarray.resize(size + left_offset);
  }

  void trimLeft(size_t const offset) {
    if (size() < offset) {
      throw std::runtime_error("Trimming too much.");
    }
    left_offset += offset;
  }

  std::string_view stringView() const {
    return std::string_view(reinterpret_cast<char const *>(data()), size());
  }

 private:
  size_t max_size;
  DynArray dynarray;
#if CHECK_BUFFER_THREAD
  std::thread::id thread_id = std::this_thread::get_id();
#endif
  size_t left_offset = 0;
  std::optional<std::reference_wrapper<std::vector<Buffer>>> home_vector;
  MoveIndicator moved;
  friend Pool;
};

/**
 * @brief A thread-UNSAFE pool of buffers.
 *
 */
class Pool {
  static bool constexpr AllowDelayedBufferAlloc = true;

 public:
  Pool(size_t const nb_buffers, size_t const buffer_size)
      : buffer_size{buffer_size} {
    buffers->reserve(nb_buffers);
    for (size_t i = 0; i < nb_buffers; i++) {
      buffers->emplace_back(buffer_size);
    }
  }

  std::optional<Buffer> take(
      std::optional<size_t> const opt_size = std::nullopt) {
    if (unlikely(buffers->empty())) {
      if constexpr (!AllowDelayedBufferAlloc) {
        return std::nullopt;
      }
      buffers->emplace_back(buffer_size);
    }
    auto buffer = std::move(buffers->back());
    buffers->pop_back();
    buffer.home_vector = *buffers;
    if (opt_size) {
      buffer.resize(*opt_size);
    }
    return buffer;
  }

  std::optional<std::reference_wrapper<Buffer>> borrowNext() {
    if (unlikely(buffers->empty())) {
      if constexpr (!AllowDelayedBufferAlloc) {
        return std::nullopt;
      }
      buffers->emplace_back(buffer_size);
    }
    return buffers->back();
  }

 private:
  // We use a unique_ptr to make sure that references to the vector remain valid
  // even upon move.
  std::unique_ptr<std::vector<Buffer>> buffers =
      std::make_unique<std::vector<Buffer>>();
  size_t buffer_size;
};

}  // namespace dory::ubft
