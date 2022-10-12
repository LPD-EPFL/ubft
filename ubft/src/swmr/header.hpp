#pragma once

#include <cstdint>

namespace dory::ubft::swmr {

struct Header {
  using Hash = uint64_t;
  using Incarnation = uint32_t;

  Hash hash;
  Incarnation incarnation;
} __attribute__((__packed__));

static_assert(sizeof(Header) ==
                  (sizeof(Header::Hash) + sizeof(Header::Incarnation)),
              "The Header struct is not packed. Use "
              "`__attribute__((__packed__))` to pack it");

}  // namespace dory::ubft::swmr
