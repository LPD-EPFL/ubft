#pragma once

#include <cstddef>
#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include <dory/conn/rc-exchanger.hpp>

#include <dory/memstore/store.hpp>

#include "../builder.hpp"
#include "../types.hpp"
#include "host.hpp"
#include "internal/exchanger-role.hpp"
#include "reader.hpp"

namespace dory::ubft::swmr {

class ReaderBuilder : Builder<Reader> {
 public:
  ReaderBuilder(dory::ctrl::ControlBlock &cb, ProcId const local_id,
                ProcId const owner_id, ProcId const host_id,
                std::string const &identifier,
                // params for Reader constructor
                size_t const nb_registers, size_t const value_size)
      : host_id{host_id},
        uuid{fmt::format("swmr-reader-{}-H{}-O{}", identifier, host_id,
                         owner_id)},
        qp_ns{fmt::format("swmr-{}-H{}-O{}", identifier, host_id, owner_id)},
        store{dory::memstore::MemoryStore::getInstance()},
        exchanger{
            local_id, {host_id}, cb, internal::READER_WRITER, internal::HOST},
        nb_registers(nb_registers),
        value_size(value_size) {
    // initialize memory
    cb.allocateBuffer(uuid, Host::bufferSize(nb_registers, value_size), 64);
    cb.registerMr(uuid, "standard", uuid, LocalMemoryRights);
    cb.registerCq(uuid);
    // initialize qp
    exchanger.configure(host_id, "standard", uuid, uuid, uuid);
  }

  void announceQps() override {
    announcing();
    exchanger.announceAll(store, qp_ns);
  }

  void connectQps() override {
    connecting();
    exchanger.connectAll(store, qp_ns);
  }

  Reader build() override {
    building();
    return Reader(nb_registers, value_size, exchanger.extract(host_id));
  }

 private:
  ProcId const host_id;
  std::vector<ProcId> const remote_ids;
  std::string const uuid;
  std::string const qp_ns;

  dory::memstore::MemoryStore &store;
  dory::conn::RcConnectionExchanger<ProcId, internal::Role> exchanger;

  size_t const nb_registers;
  size_t const value_size;

  auto static constexpr LocalMemoryRights =
      dory::ctrl::ControlBlock::LOCAL_READ |
      dory::ctrl::ControlBlock::LOCAL_WRITE;
};
}  // namespace dory::ubft::swmr
