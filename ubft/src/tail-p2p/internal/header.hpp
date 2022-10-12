#pragma once

#include <cstdint>

#include "../types.hpp"

namespace dory::ubft::tail_p2p::internal {

struct Header {
  using Hash = uint64_t;
  using Incarnation = uint32_t;
  using Size = tail_p2p::Size;

  Hash hash;
  Incarnation incarnation;
  Size size;
};

static_assert(sizeof(Header) == (sizeof(Header::Hash) +
                                 sizeof(Header::Incarnation) + sizeof(Size)),
              "The Header structed is not packed. Use "
              "`__attribute__((__packed__))` to pack it");

}  // namespace dory::ubft::tail_p2p::internal
