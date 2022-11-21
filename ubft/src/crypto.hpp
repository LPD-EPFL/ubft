#pragma once

#include <cstdint>
#include <stdexcept>
#include <set>

#include <fmt/core.h>

#include <dory/memstore/store.hpp>
#include <dory/pony/pony.hpp>

#include "types.hpp"

namespace dory::ubft {
class Crypto {
 public:
  using Signature = dory::pony::Signature;

  Crypto(ProcId local_id, std::vector<ProcId> const &all_ids)
      : my_id{local_id}, ponylib{my_id} {
    auto &store = dory::memstore::MemoryStore::getInstance();
    store.barrier("server_public_keys_announced", all_ids.size());

    for (auto id : all_ids) {
      public_keys.insert(id);
    }
  }

  // WARNING: THIS IS NOT THREAD SAFE
  void fetchPublicKey(ProcId const id) {
    throw std::logic_error("Unimplemented `fetchPublicKey`!");
  }

  inline Signature sign(uint8_t const *msg,      // NOLINT
                        size_t const msg_len) {  // NOLINT
    Signature sig;
    ponylib.sign(sig, msg, msg_len);
    return sig;
  }

  inline bool verify(Signature const &sig, uint8_t const *msg,
                     size_t const msg_len, int const node_id) {
    auto pk_it = public_keys.find(node_id);
    if (pk_it == public_keys.end()) {
      throw std::runtime_error(
          fmt::format("Missing public key for {}!", node_id));
    }

    return ponylib.verify(sig.data(), msg, msg_len, node_id);
  }

  inline ProcId myId() const { return my_id; }

 private:
  ProcId const my_id;
  // Map: NodeId (ProcId) -> Node's Public Key
  std::set<ProcId> public_keys;
  pony::PonyLib ponylib;
};
}  // namespace dory::ubft
