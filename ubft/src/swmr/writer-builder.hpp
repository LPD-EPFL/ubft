#pragma once

#include <cstdint>
#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include <dory/conn/rc-exchanger.hpp>

#include <dory/memstore/store.hpp>

#include "../builder.hpp"
#include "../types.hpp"
#include "host.hpp"
#include "internal/exchanger-role.hpp"
#include "writer.hpp"

namespace dory::ubft::swmr {

class WriterBuilder : Builder<Writer> {
 public:
  WriterBuilder(dory::ctrl::ControlBlock &cb, ProcId const owner_id,
                ProcId const host_id, std::string const &identifier,
                // params for Writer constructor
                size_t const nb_registers, size_t const value_size,
                bool const allow_custom_incarnation = false)
      : host_id{host_id},
        uuid{fmt::format("swmr-writer-{}-H{}-O{}", identifier, host_id,
                         owner_id)},
        qp_ns{fmt::format("swmr-{}-H{}-O{}", identifier, host_id, owner_id)},
        store{dory::memstore::MemoryStore::getInstance()},
        exchanger{
            owner_id, {host_id}, cb, internal::READER_WRITER, internal::HOST},
        nb_registers(nb_registers),
        value_size(value_size),
        allow_custom_incarnation{allow_custom_incarnation} {
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

  Writer build() override {
    building();
    return Writer(nb_registers, value_size, exchanger.extract(host_id),
                  allow_custom_incarnation);
  }

 private:
  ProcId const host_id;
  std::string const uuid;
  std::string const qp_ns;

  dory::memstore::MemoryStore &store;
  dory::conn::RcConnectionExchanger<ProcId, internal::Role> exchanger;

  size_t const nb_registers;
  size_t const value_size;
  bool const allow_custom_incarnation;

  auto static constexpr LocalMemoryRights =
      dory::ctrl::ControlBlock::LOCAL_READ |
      dory::ctrl::ControlBlock::LOCAL_WRITE;
};
}  // namespace dory::ubft::swmr
