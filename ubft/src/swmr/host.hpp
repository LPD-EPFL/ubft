#pragma once

#include <cstddef>

#include "header.hpp"

namespace dory::ubft::swmr {

class Host {
 public:
  static size_t constexpr bufferSize(size_t const nb_registers,
                                     size_t const value_size) {
    return registerSize(value_size) * nb_registers;
  }

  static size_t constexpr registerSize(size_t const value_size) {
    return subslotSize(value_size) * 2;
  }

  static size_t constexpr subslotSize(size_t const value_size) {
    return sizeof(Header) + value_size;
  }

  /**
   * @brief Useless for now as Hosts are totally passive. We could make them
   *        initialize memory in the future.
   *
   */
  Host() {
    // TODO(Antoine): we may want to initialize the memory here.
  }
};

}  // namespace dory::ubft::swmr
