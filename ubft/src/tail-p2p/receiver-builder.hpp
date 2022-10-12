#pragma once

#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include <dory/conn/rc-exchanger.hpp>

#include <dory/memstore/store.hpp>

#include "../builder.hpp"
#include "../types.hpp"
#include "receiver.hpp"

namespace dory::ubft::tail_p2p {

class ReceiverBuilder : private Builder<Receiver> {
 public:
  ReceiverBuilder(dory::ctrl::ControlBlock &cb, ProcId const local_id,
                  ProcId const sender_id, std::string const &identifier,
                  size_t const tail, size_t const max_msg_size)
      : sender_id{sender_id},
        qp_ns{fmt::format("p2p-{}-S{}-R{}", identifier, sender_id, local_id)},
        store{dory::memstore::MemoryStore::getInstance()},
        exchanger{local_id, {sender_id}, cb},
        // Receiver params
        tail{tail},
        max_msg_size{max_msg_size} {
    std::string const uuid =
        fmt::format("p2p-receiver-{}-S{}-R{}", identifier, sender_id, local_id);
    // Initialize Memory
    cb.allocateBuffer(uuid, Receiver::bufferSize(tail, max_msg_size), 64);
    cb.registerMr(uuid, "standard", uuid, WriteMemoryRights);
    // Initialize QPs
    exchanger.configure(sender_id, "standard", uuid, "unused", "unused");
  }

  void announceQps() override {
    announcing();
    exchanger.announceAll(store, qp_ns);
  }

  void connectQps() override {
    connecting();
    exchanger.connectAll(store, qp_ns, WriteMemoryRights);
  }

  Receiver build() override {
    building();
    return Receiver(tail, max_msg_size, exchanger.extract(sender_id));
  }

 private:
  ProcId const sender_id;
  std::string const qp_ns;

  dory::memstore::MemoryStore &store;
  dory::conn::RcConnectionExchanger<ProcId> exchanger;

  size_t const tail;
  size_t const max_msg_size;

  static auto constexpr WriteMemoryRights =
      dory::ctrl::ControlBlock::LOCAL_READ |
      dory::ctrl::ControlBlock::LOCAL_WRITE |
      dory::ctrl::ControlBlock::REMOTE_READ |
      dory::ctrl::ControlBlock::REMOTE_WRITE;
};

}  // namespace dory::ubft::tail_p2p
