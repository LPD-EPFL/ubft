#pragma once

#include <cstddef>

#include <dory/shared/branching.hpp>

namespace dory::ubft::tail_p2p::internal {

/**
 * @brief Adds the tickEvery method to tickable abstractions.
 */
class Lazy {
 public:
  virtual inline void tick() = 0;
  virtual ~Lazy() = default;

  /**
   * @brief Call tick after `frequency` calls.
   *
   * @param frequency
   */
  inline void tickEvery(size_t const frequency) {
    if (unlikely(++calls >= frequency)) {
      tick();
      calls = 0;
    }
  }

  /**
   * @brief Call tick after *many* calls.
   *
   */
  inline void tickForCorrectness() { tickEvery(1ull << 8); }

 private:
  size_t calls = 0;
};

}  // namespace dory::ubft::tail_p2p::internal
