#pragma once

#include <dory/crypto/hash/blake3.hpp>

#include "internal/requests.hpp"

namespace dory::ubft::consensus {

using Instance = uint64_t;
using View = uint64_t;
using Batch = internal::Batch;
using Request = Batch::Request;

// Exposed as the SMR needs to know about it.
#pragma pack(push, 1)  // Packed as this must be certified.
struct Checkpoint {
  Checkpoint(Checkpoint const&) = default;
  Checkpoint& operator=(Checkpoint const&) = default;

  bool operator==(Checkpoint const& o) const {
    return propose_range.low == o.propose_range.low &&
           propose_range.high == o.propose_range.high &&
           app_digest == o.app_digest;
  }
  bool operator!=(Checkpoint const& o) const { return !(o == *this); }

  bool operator<(Checkpoint const& o) const {
    return propose_range.low < o.propose_range.low;
  }
  bool operator<=(Checkpoint const& o) const {
    return propose_range.low <= o.propose_range.low;
  }
  bool operator>=(Checkpoint const& o) const {
    return propose_range.low >= o.propose_range.low;
  }
  bool operator>(Checkpoint const& o) const {
    return propose_range.low > o.propose_range.low;
  }

  struct ProposeRange {
    Instance low;   // included in the range
    Instance high;  // excluded from the range

    inline bool contains(Instance const i) const {
      return i >= low && i < high;
    }
  };
  // The range of slot this checkpoint opens
  ProposeRange propose_range = {0, 1};
  // The digest of the application so far
  crypto::hash::Blake3Hash app_digest = {};

  Checkpoint(Instance const next, size_t const window,
             crypto::hash::Blake3Hash const& digest)
      : propose_range{next, next + window}, app_digest{digest} {}
};
#pragma pack(pop)

}  // namespace dory::ubft::consensus
