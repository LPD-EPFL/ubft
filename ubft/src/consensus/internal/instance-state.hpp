#pragma once

#include <dory/shared/dynamic-bitset.hpp>

#include "../types.hpp"
#include "messages.hpp"

namespace dory::ubft::consensus::internal {

class InstanceState {
 public:
  InstanceState(PrepareMessage &&prepare_message, size_t const replicas)
      : prepare_message{std::move(prepare_message)},
        fast_committed{replicas},
        committed{replicas} {}

  /**
   * @brief Mark a replica as having fast committed the instance.
   *
   * @param from the index of the committer
   * @return whether it was the first time.
   */
  bool receivedFastCommit(size_t from) { return fast_committed.set(from); }

  /**
   * @brief Mark a replica as having committed the instance.
   *
   * @param from the index of the committer
   * @return whether it was the first time.
   */
  bool receivedCommit(size_t from) { return committed.set(from); }

  bool decidable() const {
    return !decided && (fast_committed.full() || committed.majority());
  }

  bool fastCommitted(size_t const index) const {
    return fast_committed.get(index);
  }

  bool slowCommitted(size_t const index) const { return committed.get(index); }

  PrepareMessage prepare_message;  // The prepare message received.
  DynamicBitset fast_committed;    // Who fast committed.
  DynamicBitset committed;         // Who committed.
  bool decided = false;
  bool certified_prepare = false;
};

}  // namespace dory::ubft::consensus::internal
