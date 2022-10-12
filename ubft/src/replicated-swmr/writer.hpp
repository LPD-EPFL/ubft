#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>
#include <dory/conn/rc.hpp>

#include "../swmr/writer.hpp"
#include "../unsafe-at.hpp"

namespace dory::ubft::replicated_swmr {

class Writer {
 public:
  using Index = swmr::Writer::Index;
  using Incarnation = swmr::Writer::Incarnation;

 private:
  struct Register {
    Register(size_t const value_size)
        : buffer(std::make_unique<uint8_t[]>(value_size)) {}

    void setIncarnation(Incarnation const custom_incarnation) {
      if (unlikely(custom_incarnation <= incarnation)) {
        throw std::runtime_error(fmt::format(
            "Incarnation numbers must be monotonic; new: {}, previous: {}.",
            custom_incarnation, incarnation));
      }
      incarnation = custom_incarnation;
    }

    std::unique_ptr<uint8_t[]> buffer;
    bool outstanding_write = false;
    Incarnation incarnation = 0;
  };

  class ManagedWriter {
   public:
    ManagedWriter(swmr::Writer &&writer) : writer{std::move(writer)} {}

    void tick() {
      writer.tick();
      pushToWriter();
    }

    void write(Index const index, Incarnation const incarnation,
               void *const buffer) {
      auto find_it = to_write.find(index);
      if (find_it == to_write.end()) {
        to_write.try_emplace(index, incarnation, buffer);
      } else {
        find_it->second = {incarnation, buffer};
      }
      pushToWriter();
    }

    bool completed(Index const index) {
      auto const find_it = to_write.find(index);
      return find_it == to_write.end() && writer.completed(index);
    }

   private:
    void pushToWriter() {
      // We iterate over the map while removing elements from it.
      for (auto it = to_write.cbegin(); it != to_write.cend(); /* in body */) {
        auto const &[index, job] = *it;
        auto const opt_slot = writer.getSlot(index);
        if (!unlikely(opt_slot)) {
          ++it;
          continue;
        }
        auto const &[incarnation, buffer] = job;
        std::memcpy(*opt_slot, buffer, writer.valueSize());
        writer.write(index, incarnation);
        it = to_write.erase(it);
      }
    }

    swmr::Writer writer;
    std::unordered_map<Index, std::pair<Incarnation, void *>> to_write;
  };

 public:
  Writer(std::vector<swmr::Writer> &&writers,
         bool const allow_custom_incarnation = false)
      : allow_custom_incarnation{allow_custom_incarnation} {
    if (writers.empty()) {
      throw std::runtime_error("There should be at least one sub-writer.");
    }
    // TODO(Ant.): check that writers is not empty and all have the same params.
    auto const nb_registers = writers.front().nbRegisters();
    auto const value_size = writers.front().valueSize();
    for (size_t i = 0; i < nb_registers; i++) {
      registers.emplace_back(value_size);
    }
    for (auto &writer : writers) {
      this->writers.emplace_back(std::move(writer));
    }
  }

  std::optional<void *> getSlot(Index const index) {
    auto &reg = uat(registers, index);
    if (reg.outstanding_write) {
      return std::nullopt;
    }
    return reg.buffer.get();
  }

  void write(Index const index,
             std::optional<Incarnation> opt_incarnation = {}) {
    auto &reg = uat(registers, index);
    if (unlikely(reg.outstanding_write)) {
      throw std::runtime_error(
          "Cannot write before the completion of the previous WRITE.");
    }
    if (likely(opt_incarnation)) {
      if (unlikely(!allow_custom_incarnation)) {
        throw std::runtime_error(
            "Custom incarnation numbers were disabled in the constructor.");
      }
      reg.setIncarnation(*opt_incarnation);
    } else {
      reg.incarnation++;
    }
    for (auto &managed_writer : writers) {
      managed_writer.write(index, reg.incarnation, reg.buffer.get());
    }
    reg.outstanding_write = true;
  }

  bool completed(Index const index) {
    auto &outstanding_write = uat(registers, index).outstanding_write;
    if (unlikely(!outstanding_write)) {
      throw std::runtime_error(
          fmt::format("No outstanting write to {}.", index));
    }
    size_t writes = 0;
    for (auto &managed_writer : writers) {
      if (managed_writer.completed(index)) {
        writes++;
      }
    }
    if (writes >= (writers.size() + 1) / 2) {
      outstanding_write = false;
      return true;
    }
    return false;
  }

  void tick() {
    for (auto &managed_writer : writers) {
      managed_writer.tick();
    }
  }

 private:
  bool const allow_custom_incarnation;
  std::vector<Register> registers;
  std::vector<ManagedWriter> writers;
};

}  // namespace dory::ubft::replicated_swmr
