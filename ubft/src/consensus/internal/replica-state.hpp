#pragma once

#include <map>

#include <dory/shared/branching.hpp>

#include "../../buffer.hpp"
#include "../../certifier/certificate.hpp"
#include "../../tail-cb/message.hpp"
#include "../../tail-map/tail-map.hpp"
#include "../types.hpp"
#include "broadcast-commit.hpp"
#include "cb-checkpoint.hpp"
#include "packing.hpp"
#include "serialized-state.hpp"

namespace dory::ubft::consensus::internal {

/**
 * @brief Stores all the data deduced from what a replica cb-broadcast.
 *        Can serialize a state so that it can be agreed upon.
 *        Pre-allocates buffers so that committing is "free".
 *
 */
class ReplicaState {
 public:
  ReplicaState(size_t const window, size_t const max_proposal_size)
      : checkpoint{0, window, {}},
        pool{window + 1, BroadcastCommit::size(max_proposal_size)} {}

  // The view the replica is in, increasing upon SealView message.
  View at_view = 0;

  // The map of all Commit messages received from the replica.
  std::map<Instance, BroadcastCommit> commits;

  // The next prepare message we expect to see (to detect equivocation).
  Instance next_prepare = 0;

  // What proposals should be considered valid, updated upon NewView.
  std::optional<std::pair<View, TailMap<Instance, Buffer>>> valid_values;

  // The latest checkpoint received.
  consensus::Checkpoint checkpoint;

  // The next CB message we expect to deliver (to detect gaps).
  tail_cb::Message::Index next_cb = 0;

  // How many commits (i.e., prepare certificates) are under verification.
  size_t outstanding_commit_verifications = 0;

  /**
   * @brief Store the value committed by a replica.
   *
   * @param prepare_certificate
   * @return whether it was the first time it committed this instance.
   */
  bool committed(certifier::Certificate& prepare_certificate) {
    auto const [view, instance] = unpack(prepare_certificate.index());

    auto opt_buffer = pool.take();
    if (unlikely(!opt_buffer)) {
      throw std::logic_error(
          "Ran out of buffers to store committed proposals.");
    }

    auto const prev_it = commits.find(instance);
    if (unlikely(prev_it != commits.end())) {
      if (unlikely(prev_it->second.view() >= view)) {
        return false;
      } else {
        commits.erase(prev_it);
      }
    }
    commits.try_emplace(instance, prepare_certificate, std::move(*opt_buffer));
    return true;
  }

  internal::SerializedState const& serializeState() {
    serialized_state.emplace(at_view, commits);
    return *serialized_state;
  }

  // The serialized state for the last view change is held.
  std::optional<internal::SerializedState> serialized_state;

  internal::CbCheckpoint const& checkpointCb() {
    cb_checkpoint.emplace(next_cb, at_view, checkpoint, next_prepare,
                          valid_values, commits);
    return *cb_checkpoint;
  }

  // Last cb_checkpoint generated.
  std::optional<internal::CbCheckpoint> cb_checkpoint;

 private:
  Pool pool;
};

}  // namespace dory::ubft::consensus::internal
