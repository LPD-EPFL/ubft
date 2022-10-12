#pragma once

#include <stdexcept>
#include <string_view>

#include <fmt/core.h>

#include "../../certifier/certificate.hpp"
#include "../types.hpp"
#include "packing.hpp"

namespace dory::ubft::consensus::internal {

/**
 * @brief Stores all the information about a Commit message that was broadcast
 *        by some replica.
 *
 */
struct BroadcastCommit {
  BroadcastCommit(certifier::Certificate& prepare_certificate, Buffer&& buffer)
      : buffer(std::move(buffer)) {
    if (unlikely(this->buffer.size() <
                 size(prepare_certificate.messageSize()))) {
      throw std::logic_error(
          fmt::format("Insufficient buffer size: {} vs {}.", buffer.size(),
                      size(prepare_certificate.messageSize())));
    }
    this->buffer.resize(size(prepare_certificate.messageSize()));
    auto const [v, i] = unpack(prepare_certificate.index());
    view() = v;
    instance() = i;
    proposalSize() = prepare_certificate.messageSize();
    std::copy(prepare_certificate.message(),
              prepare_certificate.message() + prepare_certificate.messageSize(),
              proposal());
  }

  struct Layout {
    View view;
    Instance instance;
    size_t proposal_size;
    uint8_t proposal;  // Fake field, start of the proposal.

    std::string_view stringView() const {
      return std::string_view(reinterpret_cast<char const*>(&proposal),
                              proposal_size);
    }
  };

  Buffer buffer;

  View const& view() const {
    return reinterpret_cast<Layout const*>(buffer.data())->view;
  }

  View& view() { return const_cast<View&>(std::as_const(*this).view()); }

  Instance const& instance() const {
    return reinterpret_cast<Layout const*>(buffer.data())->instance;
  }

  Instance& instance() {
    return const_cast<Instance&>(std::as_const(*this).instance());
  }

  size_t const& proposalSize() const {
    return reinterpret_cast<Layout const*>(buffer.data())->proposal_size;
  }

  size_t& proposalSize() {
    return const_cast<Instance&>(std::as_const(*this).proposalSize());
  }

  uint8_t const* proposal() const {
    return &reinterpret_cast<Layout const*>(buffer.data())->proposal;
  }

  uint8_t* proposal() {
    return const_cast<uint8_t*>(std::as_const(*this).proposal());
  }

  size_t static constexpr size(size_t const max_proposal_size) {
    return offsetof(Layout, proposal) + max_proposal_size;
  }
};

}  // namespace dory::ubft::consensus::internal
