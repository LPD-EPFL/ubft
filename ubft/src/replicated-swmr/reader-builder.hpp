#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <dory/ctrl/block.hpp>

#include "../builder.hpp"
#include "../swmr/reader-builder.hpp"
#include "../swmr/reader.hpp"
#include "reader.hpp"

#include "../types.hpp"

namespace dory::ubft::replicated_swmr {

class ReaderBuilder : Builder<Reader> {
 public:
  ReaderBuilder(dory::ctrl::ControlBlock &cb, ProcId const local_id,
                ProcId const writer_id, std::vector<ProcId> const &hosts_ids,
                std::string const &identifier, size_t const nb_registers,
                size_t const register_size) {
    for (auto const host_id : hosts_ids) {
      builders.emplace_back(cb, local_id, writer_id, host_id, identifier,
                            nb_registers, register_size);
    }
  }

  void announceQps() override {
    announcing();
    for (auto &builder : builders) {
      builder.announceQps();
    }
  }

  void connectQps() override {
    connecting();
    for (auto &builder : builders) {
      builder.connectQps();
    }
  }

  Reader build() override {
    building();
    std::vector<swmr::Reader> readers;
    for (auto &builder : builders) {
      readers.emplace_back(builder.build());
    }
    return Reader(std::move(readers));
  }

 private:
  std::vector<swmr::ReaderBuilder> builders;
};
}  // namespace dory::ubft::replicated_swmr
