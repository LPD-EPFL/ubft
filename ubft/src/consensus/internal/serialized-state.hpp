#pragma once

#include "../types.hpp"
#include "broadcast-commit.hpp"
#include "packing.hpp"

namespace dory::ubft::consensus::internal {

/**
 * @brief Serialized state of a replica that can be acknowledged/forwarded upon
 *        ViewSeal.
 *
 */
struct SerializedState : public dory::ubft::Message {
  using Message::Message;

#pragma pack(push, 1)
  struct Layout {
    View view;
    size_t nb_commits;
    size_t max_proposal_size;
    uint8_t commits;  // Fake field, start of the commit entries.
  };
#pragma pack(pop)

  // Note: We do not try to compress the commits, we waste space.
  size_t static constexpr bufferSize(size_t const nb_commits,
                                     size_t const max_proposal_size) {
    return offsetof(Layout, commits) +
           nb_commits * BroadcastCommit::size(max_proposal_size);
  }

  // Note: allocates a buffer.
  SerializedState(View const v,
                  std::map<Instance, BroadcastCommit> const& commits)
      : dory::ubft::Message(bufferSize(commits.size(), maxSize(commits))) {
    view() = v;
    nbBroadcastCommits() = commits.size();
    maxProposalSize() = maxSize(commits);
    for (auto const& [index, c] : hipony::enumerate(commits)) {
      std::copy(c.second.buffer.cbegin(), c.second.buffer.cend(),
                reinterpret_cast<uint8_t*>(&commit(index)));
    }
  }

  View const& view() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->view;
  }

  View& view() { return const_cast<View&>(std::as_const(*this).view()); }

  size_t const& nbBroadcastCommits() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->nb_commits;
  }

  size_t& nbBroadcastCommits() {
    return const_cast<size_t&>(std::as_const(*this).nbBroadcastCommits());
  }

  size_t const& maxProposalSize() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())
        ->max_proposal_size;
  }

  size_t& maxProposalSize() {
    return const_cast<size_t&>(std::as_const(*this).maxProposalSize());
  }

  BroadcastCommit::Layout const& commit(size_t const index) const {
    auto const offset = offsetof(Layout, commits) +
                        index * BroadcastCommit::size(maxProposalSize());
    return *reinterpret_cast<BroadcastCommit::Layout const*>(
        rawBuffer().data() + offset);
  }

  BroadcastCommit::Layout& commit(size_t const index) {
    return const_cast<BroadcastCommit::Layout&>(
        std::as_const(*this).commit(index));
  }

  size_t size() const { return rawBuffer().size(); }

 private:
  static size_t maxSize(std::map<Instance, BroadcastCommit> const& commits) {
    size_t max = 0;
    for (auto const& [_, commit] : commits) {
      if (max < commit.proposalSize()) {
        max = commit.proposalSize();
      }
    }
    return max;
  }
};

}  // namespace dory::ubft::consensus::internal
