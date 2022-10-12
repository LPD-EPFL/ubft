#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>
#include <xxhash.h>

#include <dory/conn/rc.hpp>
#include <dory/ctrl/block.hpp>
#include <dory/shared/branching.hpp>

#include "../unsafe-at.hpp"
#include "constants.hpp"
#include "header.hpp"
#include "host.hpp"

namespace dory::ubft::swmr {

class Writer {
  struct Register {
    Register(uintptr_t const buffer) noexcept
        : raw_buffer{reinterpret_cast<void *>(buffer)},
          content{reinterpret_cast<void *>(buffer + sizeof(Header))},
          header{*reinterpret_cast<Header *>(buffer)} {
      header.incarnation = 0;
    }

    void incrementIncarnation() { ++header.incarnation; }

    void setIncarnation(Header::Incarnation const custom_incarnation) {
      // The writer initializes the remote memory by writing twice to it, hence
      // introducing an offset of 2 between the expected incarnation number and
      // the underlying one.
      auto const real_ci = custom_incarnation + 2;
      if (unlikely(real_ci <= header.incarnation)) {
        throw std::runtime_error(fmt::format(
            "Incarnation numbers must be monotonic; new: {}, previous: {}.",
            custom_incarnation, header.incarnation - 2));
      }
      header.incarnation = real_ci;
    }

    void changeRemoteSubslot() { remote_subslot = (remote_subslot + 1) % 2; }

    void *const raw_buffer;
    void *const content;
    bool scheduled = false;
    std::optional<std::chrono::steady_clock::time_point> last_write;
    std::size_t remote_subslot = 0;
    Header &header;
  };

 public:
  using Index = size_t;
  using Incarnation = Header::Incarnation;
  static size_t constexpr MaxOutstandingWrites =
      conn::ReliableConnection::WrDepth;
  static_assert(MaxOutstandingWrites <= ctrl::ControlBlock::CqDepth);

  /**
   * How to use:
   * optional<void*> opt_slot = Writer.getSlot(Register Index);
   * if (!opt_slot) => There is a scheduled (non-completed) WRITE for Index.
   * memcpy(*opt_slot, data, data_size);
   * JobId const job_id = Writer.write(Register Index);
   * Writer.completed(job_id) -> bool
   */

  Writer(size_t const nb_registers, size_t const value_size,
         conn::ReliableConnection &&rc,
         bool const allow_custom_incarnation = false)
      : nb_registers{nb_registers},
        value_size{value_size},
        register_size{Host::subslotSize(value_size)},
        remote_register_size{Host::registerSize(value_size)},
        rc{std::move(rc)},
        allow_custom_incarnation{allow_custom_incarnation} {
    if (this->rc.remoteSize() < Host::bufferSize(nb_registers, value_size)) {
      throw std::runtime_error(fmt::format(
          "Remote MR too small to host {} registers: {} given, {} required",
          nb_registers, this->rc.remoteSize(),
          Host::bufferSize(nb_registers, value_size)));
    }

    if (this->rc.getMr().size < Host::bufferSize(nb_registers, value_size)) {
      throw std::runtime_error(fmt::format(
          "Local MR too small to host {} register buffers: {} given, {} "
          "required",
          nb_registers, this->rc.getMr().size,
          Host::bufferSize(nb_registers, value_size)));
    }

    // Store the pointers to register starts in my local memory.
    // Use these pointers as data source for RDMA WRITEs.
    for (size_t i = 0; i < nb_registers; i++) {
      registers.emplace_back(this->rc.getMr().addr + i * register_size);
    }

    // Note: The available space for WCs may be less than WrDepth if
    // the CQ has many users (i.e., is shared among many QPs).
    wcs.reserve(MaxOutstandingWrites);

    initializeRemoteRegisters();
  }

  std::optional<void *> getSlot(Index const index) {
    auto &reg = uat(registers, index);
    if (reg.scheduled) {
      return std::nullopt;
    }
    return reg.content;
  }

  void write(Index const index,
             std::optional<Header::Incarnation> opt_incarnation = {}) {
    auto &reg = uat(registers, index);
    reg.scheduled = true;
    if (likely(opt_incarnation)) {
      if (unlikely(!allow_custom_incarnation)) {
        throw std::runtime_error(
            "Custom incarnation numbers were disabled in the constructor.");
      }
      reg.setIncarnation(*opt_incarnation);
    } else {
      reg.incrementIncarnation();
    }
    reg.changeRemoteSubslot();
    reg.header.hash = XXH3_64bits(&reg.header.incarnation,
                                  sizeof(Header::incarnation) + value_size);
    queued_writes.emplace_back(index);
    pushToQp();
  }

  bool completed(Index const index) {
    // Note: a nullopt is smaller than any other value.
    return !uat(registers, index).scheduled;
  }

  void tick() {
    if (outstanding_writes != 0) {
      pollCompletion();
    }
    pushToQp();
  }

  size_t nbRegisters() const { return nb_registers; }

  size_t valueSize() const { return value_size; }

  bool customIncarnationAllowed() const { return allow_custom_incarnation; }

 private:
  void initializeRemoteRegisters() {
    for (int subslot = 0; subslot < 2; ++subslot) {
      for (Index i = 0; i < registers.size(); ++i) {
        auto slot = getSlot(i);
        if (!slot) {
          throw std::runtime_error("Failed to initialize remote registers.");
        }
        memset(*slot, 0, value_size);
        write(i);
      }

      // Wait for the last register write to complete
      while (!completed(registers.size() - 1)) {
        pollCompletion(true);
        pushToQp();
      }
    }
  }

  void pollCompletion(bool const bypass_cooldown = false) {
    wcs.resize(outstanding_writes);
    if (!rc.pollCqIsOk(conn::ReliableConnection::SendCq, wcs)) {
      throw std::runtime_error("Error while polling CQ.");
    }
    for (auto const &wc : wcs) {
      if (wc.status != IBV_WC_SUCCESS) {
        // TODO(Antoine): consider the guy as being dead instead?
        throw std::runtime_error(
            fmt::format("Error in RDMA WRITE: {}", wc.status));
      }

      auto const index = wc.wr_id;
      auto &reg = uat(registers, index);

      if (!reg.scheduled) {
        throw std::runtime_error("WRITE completed without being scheduled.");
      }

      if (!bypass_cooldown) {
        reg.last_write = std::chrono::steady_clock::now();
      }
      reg.scheduled = false;
      --outstanding_writes;
    }
  }

  void pushToQp() {
    for (size_t i = 0;
         i < queued_writes.size() && outstanding_writes < MaxOutstandingWrites;
         ++i) {
      auto const index = queued_writes.front();
      auto const &reg = uat(registers, index);

      if (reg.last_write && *reg.last_write + constants::WriteCooldown >
                                std::chrono::steady_clock::now()) {
        queued_writes.pop_front();
        queued_writes.push_back(index);
        continue;
      }

      auto const posted =
          rc.postSendSingle(conn::ReliableConnection::RdmaReq::RdmaWrite,
                            reinterpret_cast<uint64_t>(index), reg.raw_buffer,
                            static_cast<uint32_t>(register_size),
                            rc.remoteBuf() + index * remote_register_size +
                                reg.remote_subslot * register_size);
      if (!posted) {
        // TODO(Antoine): consider as having failed.
        throw std::runtime_error("Failed to post WRITE");
      }

      queued_writes.pop_front();
      outstanding_writes++;
    }
  }

  size_t const nb_registers;
  size_t const value_size;
  size_t const register_size;
  size_t const remote_register_size;
  conn::ReliableConnection rc;
  bool const allow_custom_incarnation;

  std::vector<Register> registers;
  std::deque<Index> queued_writes;
  size_t outstanding_writes = 0;

  std::vector<struct ibv_wc> wcs;
};

}  // namespace dory::ubft::swmr
