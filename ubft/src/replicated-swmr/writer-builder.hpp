#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <dory/ctrl/block.hpp>

#include "../builder.hpp"
#include "../swmr/writer-builder.hpp"
#include "../swmr/writer.hpp"
#include "writer.hpp"

#include "../types.hpp"

namespace dory::ubft::replicated_swmr {

class WriterBuilder : Builder<Writer> {
 public:
  WriterBuilder(dory::ctrl::ControlBlock &cb, ProcId const owner_id,
                std::vector<ProcId> const &hosts_ids,
                std::string const &identifier, size_t const nb_registers,
                size_t const register_size,
                bool const allow_custom_incarnation = false)
      : allow_custom_incarnation{allow_custom_incarnation} {
    for (auto const host_id : hosts_ids) {
      builders.emplace_back(cb, owner_id, host_id, identifier, nb_registers,
                            register_size, allow_custom_incarnation);
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

  Writer build() override {
    building();
    std::vector<swmr::Writer> writers;
    for (auto &builder : builders) {
      writers.emplace_back(builder.build());
    }
    return Writer(std::move(writers), allow_custom_incarnation);
  }

 private:
  bool const allow_custom_incarnation;
  std::vector<swmr::WriterBuilder> builders;
};
}  // namespace dory::ubft::replicated_swmr
