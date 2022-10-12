#pragma once

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <string_view>

#include "../../tail-cb/message.hpp"
#include "../../tail-map/tail-map.hpp"
#include "../types.hpp"
#include "broadcast-commit.hpp"
#include "packing.hpp"

namespace dory::ubft::consensus::internal {

/**
 * @brief Serialized state of a replica that can be certified and then leveraged
 *        to fill CB gaps.
 *
 */
struct CbCheckpoint : public dory::ubft::Message {
  using Message::Message;

#pragma pack(push, 1)
  struct ValidValue {
    Instance instance;
    size_t size;
    uint8_t value;  // Fake field, start of the valid value.

    size_t static constexpr bufferSize(size_t const max_valid_value_size) {
      return offsetof(ValidValue, value) + max_valid_value_size;
    }

    std::string_view stringView() const {
      return std::string_view(reinterpret_cast<char const*>(&value), size);
    }
  };

  struct Layout {
    tail_cb::Message::Index next_cb;
    View view;
    consensus::Checkpoint checkpoint;
    Instance next_prepare;
    size_t nb_valid_values;
    size_t max_valid_value_size;
    size_t nb_commits;
    size_t max_proposal_size;
    uint8_t valid_values;  // Fake field, start of the valid values entries.
  };
#pragma pack(pop)

  // Note: We do not try to compress the commits, we waste space.
  size_t static constexpr bufferSize(size_t const nb_valid_values,
                                     size_t const max_valid_value_size,
                                     size_t const nb_commits,
                                     size_t const max_proposal_size) {
    return offsetof(Layout, valid_values) +
           nb_valid_values * ValidValue::bufferSize(max_valid_value_size) +
           nb_commits * BroadcastCommit::size(max_proposal_size);
  }

  // Note: allocates a buffer.
  CbCheckpoint(tail_cb::Message::Index const next_cb, View const v,
               consensus::Checkpoint const& c, Instance const next_prepare,
               std::optional<std::pair<View, TailMap<Instance, Buffer>>> const&
                   valid_values,
               std::map<Instance, BroadcastCommit> const& commits)
      : dory::ubft::Message(
            bufferSize(valid_values ? valid_values->second.size() : 0,
                       valid_values ? maxSize(valid_values->second) : 0,
                       commits.size(), maxSize(commits))) {
    nextCb() = next_cb;
    view() = v;
    checkpoint() = c;
    nextPrepare() = next_prepare;
    nbValidValues() = valid_values ? valid_values->second.size() : 0;
    maxValidValueSize() = valid_values ? maxSize(valid_values->second) : 0;
    if (valid_values) {
      for (auto [it, i] = std::make_pair(valid_values->second.begin(), 0ul);
           it != valid_values->second.end(); it++, i++) {
        auto& vv = validValue(i);
        vv.instance = it->first;
        vv.size = it->second.size();
        auto* const end = std::copy(it->second.cbegin(), it->second.cend(),
                                    reinterpret_cast<uint8_t*>(&(vv.value)));
        // Fill the rest of the slot with 0s.
        std::fill(end, reinterpret_cast<uint8_t*>(&validValue(i + 1)), 0);
      }
    }
    nbBroadcastCommits() = commits.size();
    maxProposalSize() = maxSize(commits);
    for (auto const& [index, c] : hipony::enumerate(commits)) {
      // WARNING: this assumes that BroadcastCommit buffers are trimmed.
      // This is enforced in BroadcastCommit::BroadcastCommit.
      auto* const end =
          std::copy(c.second.buffer.cbegin(), c.second.buffer.cend(),
                    reinterpret_cast<uint8_t*>(&commit(index)));
      // Fill the rest of the slot with 0s.
      std::fill(end, reinterpret_cast<uint8_t*>(&commit(index + 1)), 0);
    }

    bool constexpr FullTrace = false;
    if constexpr (FullTrace) {
      fmt::print("Creating a CB checkpoint of size {} with:\n",
                 rawBuffer().size());
      fmt::print("- next_cb: {},\n", nextCb());
      fmt::print("- view: {},\n", view());
      fmt::print("- checkpoint: [{}, {}),\n", checkpoint().propose_range.low,
                 checkpoint().propose_range.high);
      fmt::print("- next_prepare: {}\n", nextPrepare());
      fmt::print("- valid values: {},\n", nbValidValues());
      fmt::print("- max valid value size: {},\n", maxValidValueSize());
      for (size_t i = 0; i < nbValidValues(); i++) {
        fmt::print("- valid value #{}: instance={}, value={},\n", i,
                   validValue(i).instance, validValue(i).stringView());
      }
      fmt::print("- commits: {},\n", nbBroadcastCommits());
      fmt::print("- max proposal size: {},\n", maxProposalSize());
      for (size_t i = 0; i < nbBroadcastCommits(); i++) {
        fmt::print("- commit #{}: value={},\n", i, commit(i).stringView());
      }
    }
  }

  tail_cb::Message::Index const& nextCb() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->next_cb;
  }

  tail_cb::Message::Index& nextCb() {
    return const_cast<tail_cb::Message::Index&>(std::as_const(*this).nextCb());
  }

  View const& view() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->view;
  }

  View& view() { return const_cast<View&>(std::as_const(*this).view()); }

  consensus::Checkpoint const& checkpoint() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->checkpoint;
  }

  consensus::Checkpoint& checkpoint() {
    return const_cast<consensus::Checkpoint&>(
        std::as_const(*this).checkpoint());
  }

  Instance const& nextPrepare() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->next_prepare;
  }

  Instance& nextPrepare() {
    return const_cast<Instance&>(std::as_const(*this).nextPrepare());
  }

  size_t const& nbValidValues() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->nb_valid_values;
  }

  size_t& nbValidValues() {
    return const_cast<size_t&>(std::as_const(*this).nbValidValues());
  }

  size_t const& maxValidValueSize() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())
        ->max_valid_value_size;
  }

  size_t& maxValidValueSize() {
    return const_cast<size_t&>(std::as_const(*this).maxValidValueSize());
  }

  ValidValue const& validValue(size_t const index) const {
    auto const offset = offsetof(Layout, valid_values) +
                        index * ValidValue::bufferSize(maxValidValueSize());
    return *reinterpret_cast<ValidValue const*>(rawBuffer().data() + offset);
  }

  ValidValue& validValue(size_t const index) {
    return const_cast<ValidValue&>(std::as_const(*this).validValue(index));
  }

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
    auto const offset =
        offsetof(Layout, valid_values) +
        nbValidValues() * ValidValue::bufferSize(maxValidValueSize()) +
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

  static size_t maxSize(TailMap<Instance, Buffer> const& valid_values) {
    size_t max = 0;
    for (auto const& [_, vv] : valid_values) {
      if (max < vv.size()) {
        max = vv.size();
      }
    }
    return max;
  }
};

}  // namespace dory::ubft::consensus::internal
