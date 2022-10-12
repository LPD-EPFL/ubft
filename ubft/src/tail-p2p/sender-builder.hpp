#pragma once

#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include <dory/conn/rc-exchanger.hpp>

#include <dory/memstore/store.hpp>

#include "../builder.hpp"
#include "../types.hpp"
#include "sender.hpp"

namespace dory::ubft::tail_p2p {

/**
 * @brief Builder for a Sync/AsyncSender. Defaults to the Sender alias.
 *
 * @tparam SenderVariant SyncSender or AsyncSender
 */
template <typename SenderVariant = Sender>
class SenderBuilder : private Builder<SenderVariant> {
 public:
  SenderBuilder(dory::ctrl::ControlBlock &cb, ProcId const local_id,
                ProcId const receiver_id, std::string const &identifier,
                size_t const tail, size_t const max_msg_size)
      : receiver_id{receiver_id},
        qp_ns{fmt::format("p2p-{}-S{}-R{}", identifier, local_id, receiver_id)},
        store{dory::memstore::MemoryStore::getInstance()},
        exchanger{local_id, {receiver_id}, cb},
        // Receiver params
        tail{tail},
        max_msg_size{max_msg_size} {
    std::string const uuid =
        fmt::format("p2p-sender-{}-S{}-R{}", identifier, local_id, receiver_id);
    // Initialize Memory
    cb.allocateBuffer(uuid, Sender::bufferSize(tail, max_msg_size), 64);
    cb.registerMr(uuid, "standard", uuid, dory::ctrl::ControlBlock::LOCAL_READ);
    cb.registerCq(uuid);
    // Initialize QP
    exchanger.configure(receiver_id, "standard", uuid, uuid, uuid);
  }

  void announceQps() override {
    Builder<SenderVariant>::announcing();
    exchanger.announceAll(store, qp_ns);
  }

  void connectQps() override {
    Builder<SenderVariant>::connecting();
    exchanger.connectAll(store, qp_ns);
  }

  SenderVariant build() override {
    Builder<SenderVariant>::building();
    return SenderVariant(tail, max_msg_size, exchanger.extract(receiver_id));
  }

 private:
  ProcId const receiver_id;
  std::string const qp_ns;

  dory::memstore::MemoryStore &store;
  dory::conn::RcConnectionExchanger<ProcId> exchanger;

  size_t const tail;
  size_t const max_msg_size;
};

using SyncSenderBuilder = SenderBuilder<SyncSender>;
using AsyncSenderBuilder = SenderBuilder<AsyncSender>;

}  // namespace dory::ubft::tail_p2p
