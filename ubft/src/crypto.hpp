#pragma once

#include <cstdint>
#include <stdexcept>
#include <unordered_map>

#include <fmt/core.h>

#include <dory/memstore/store.hpp>

#include "types.hpp"

// Use Dalek or Sodium
#include <dory/crypto/asymmetric/dalek.hpp>
#define crypto_impl dory::crypto::asymmetric::dalek

// #include <dory/crypto/asymmetric/sodium.hpp>
// #define crypto_impl dory::crypto::asymmetric::sodium

namespace dory::ubft {
class Crypto {
 public:
  using Signature = std::array<uint8_t, crypto_impl::SignatureLength>;

  Crypto(ProcId local_id, std::vector<ProcId> const &all_ids)
      : my_id{local_id} {
    auto &store = dory::memstore::MemoryStore::getInstance();
    crypto_impl::init();
    crypto_impl::publish_pub_key(fmt::format("{}-pubkey", local_id));
    store.barrier("public_keys_announced", all_ids.size());

    for (auto id : all_ids) {
      public_keys.emplace(
          id, crypto_impl::get_public_key(fmt::format("{}-pubkey", id)));
    }
  }

  // WARNING: THIS IS NOT THREAD SAFE
  void fetchPublicKey(ProcId const id) {
    public_keys.emplace(
        id, crypto_impl::get_public_key(fmt::format("{}-pubkey", id)));
  }

  inline Signature sign(uint8_t const *msg,      // NOLINT
                        size_t const msg_len) {  // NOLINT
    Signature sig;
    crypto_impl::sign(sig.data(), msg, msg_len);
    return sig;
  }

  inline bool verify(Signature const &sig, uint8_t const *msg,
                     size_t const msg_len, int const node_id) {
    auto pk_it = public_keys.find(node_id);
    if (pk_it == public_keys.end()) {
      throw std::runtime_error(
          fmt::format("Missing public key for {}!", node_id));
    }

    return crypto_impl::verify(sig.data(), msg, msg_len, pk_it->second);
  }

  inline ProcId myId() const { return my_id; }

 private:
  ProcId const my_id;
  // Map: NodeId (ProcId) -> Node's Public Key
  std::unordered_map<ProcId, crypto_impl::pub_key> public_keys;
};
}  // namespace dory::ubft
