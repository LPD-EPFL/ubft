#pragma once

#include <cstddef>
#include <string>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <dory/ctrl/block.hpp>

#include <dory/conn/rc-exchanger.hpp>

#include <dory/memstore/store.hpp>

#include "../builder.hpp"
#include "../types.hpp"
#include "host.hpp"
#include "internal/exchanger-role.hpp"

namespace dory::ubft::swmr {

class HostBuilder : Builder<Host> {
 public:
  HostBuilder(dory::ctrl::ControlBlock &cb, ProcId const host_id,
              ProcId const owner_id, std::vector<ProcId> const &remote_ids,
              std::string const &identifier, size_t const nb_registers,
              size_t const value_size)
      : owner_id{owner_id},
        remote_ids{remote_ids},
        uuid{
            fmt::format("swmr-host-{}-H{}-O{}", identifier, host_id, owner_id)},
        qp_ns{fmt::format("swmr-{}-H{}-O{}", identifier, host_id, owner_id)},
        store{dory::memstore::MemoryStore::getInstance()},
        exchanger{host_id, remote_ids, cb, internal::HOST,
                  internal::READER_WRITER},
        nb_registers(nb_registers),
        value_size(value_size) {
    // initialize memory
    fmt::print("[DISAG. MEMORY ALLOCATED]: {}B\n", Host::bufferSize(nb_registers, value_size));
    cb.allocateBuffer(uuid, Host::bufferSize(nb_registers, value_size), 64);
    cb.registerMr(uuid + "-read", "standard", uuid, ReadMemoryRights);
    cb.registerMr(uuid + "-write", "standard", uuid, WriteMemoryRights);

    // Single CQ, we do not use it anyway
    cb.registerCq(uuid);

    // initialize qps
    initializeQps();
  }

  void initializeQps() {
    for (auto const id : remote_ids) {
      std::string mr = uuid + (id == owner_id ? "-write" : "-read");
      exchanger.configure(id, "standard", mr, uuid, uuid);
    }
  }

  void announceQps() override {
    announcing();
    exchanger.announceAll(store, qp_ns);
  }

  void connectQps() override {
    connecting();
    for (auto const id : remote_ids) {
      exchanger.connect(id, store, qp_ns,
                        id == owner_id ? WriteMemoryRights : ReadMemoryRights);
    }
  }

  /**
   * @brief Useless for now as Hosts are totally passive. We could make them
   *        initialize memory in the future.
   *
   * @return Host
   */
  Host build() override {
    building();
    return Host();
  }

 private:
  ProcId const owner_id;
  std::vector<ProcId> const remote_ids;
  std::string const uuid;
  std::string const qp_ns;

  dory::memstore::MemoryStore &store;
  dory::conn::RcConnectionExchanger<ProcId, internal::Role> exchanger;

  size_t const nb_registers;
  size_t const value_size;

  auto static constexpr ReadMemoryRights =
      dory::ctrl::ControlBlock::LOCAL_READ |
      dory::ctrl::ControlBlock::LOCAL_WRITE |
      dory::ctrl::ControlBlock::REMOTE_READ;
  auto static constexpr WriteMemoryRights =
      ReadMemoryRights | dory::ctrl::ControlBlock::REMOTE_WRITE;
};
}  // namespace dory::ubft::swmr
