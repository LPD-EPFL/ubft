#pragma once

#include <cstdint>

#include "../types.hpp"

namespace dory::ubft::consensus::internal {

inline static uint64_t pack(View const view, Instance const instance) {
  return (view << 48) | (instance & ~(0xFFFFull << 48));
}

inline static std::pair<View, Instance> unpack(uint64_t const packed) {
  return {packed >> 48, packed & ~(0xFFFFull << 48)};
}

}  // namespace dory::ubft::consensus::internal
