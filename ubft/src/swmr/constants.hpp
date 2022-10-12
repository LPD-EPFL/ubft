#pragma once

#include <chrono>

namespace dory::ubft::swmr::constants {

static auto constexpr WriteCooldown = std::chrono::milliseconds(1);

}  // namespace dory::ubft::swmr::constants
