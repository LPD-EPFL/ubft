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

#include "constants.hpp"
#include "header.hpp"
#include "host.hpp"

namespace dory::ubft::swmr {

class Reader {
  static size_t constexpr MaxOutstandingReads =
      conn::ReliableConnection::WrDepth;
  static_assert(MaxOutstandingReads <= ctrl::ControlBlock::CqDepth);

 public:
  using JobHandle = uintptr_t;
  using Index = size_t;
  using Incarnation = Header::Incarnation;
  using PollResult = std::optional<std::pair<void *, Incarnation>>;

  Reader(size_t const nb_registers, size_t const value_size,
         conn::ReliableConnection &&rc)
      : nb_registers{nb_registers},
        value_size{value_size},
        subslot_size{Host::subslotSize(value_size)},
        register_size{Host::registerSize(value_size)},
        rc{std::move(rc)} {
    if (this->rc.remoteSize() < Host::bufferSize(nb_registers, value_size)) {
      throw std::runtime_error(fmt::format(
          "Remote MR too small to host {} registers: {} given, {} required.",
          nb_registers, this->rc.remoteSize(),
          Host::bufferSize(nb_registers, value_size)));
    }

    // Store the pointers to register starts in my local memory.
    // Use these pointers as data destination for RDMA READs.
    auto const nb_buffers = this->rc.getMr().size / register_size;
    for (size_t i = 0; i < nb_buffers; i++) {
      buffer_pool.emplace_back(this->rc.getMr().addr + i * register_size);
    }

    // Note: The available space for WCs may be less than WrDepth if
    // the CQ has many users (i.e., is shared among many QPs).
    wcs.reserve(MaxOutstandingReads);
  }

  /**
   * @brief Schedule a register READ if possible
   *
   * @param index of the register in the register array
   * @return std::optional<JobHandle> nullopt if was not possible to schedule
   * the READ (due to lack local storage buffer for READs) or a handle of where
   * the READ will place the data
   */
  std::optional<JobHandle> read(Index const index) {
    if (buffer_pool.empty()) {
      return std::nullopt;
    }
    auto const buffer = buffer_pool.back();
    buffer_pool.pop_back();
    queued_reads.emplace_back(buffer, index);
    pushToQp();
    return buffer;
  }

  PollResult poll(JobHandle const job_handle) {
    auto const find_it = completed_reads.find(job_handle);
    if (find_it == completed_reads.end()) {
      return std::nullopt;
    }
    return find_it->second;
  }

  void release(JobHandle job_handle) {
    auto const find_it = completed_reads.find(job_handle);
    if (find_it == completed_reads.end()) {
      throw std::runtime_error("Job not found in completed set.");
    }
    buffer_pool.push_back(job_handle);
    completed_reads.erase(find_it);
  }

  void tick() {
    if (!outstanding_reads.empty()) {
      pollCompletion();
      pushToQp();
    }
  }

  size_t nbRegisters() const { return nb_registers; }

  size_t valueSize() const { return value_size; }

 private:
  void pollCompletion() {
    wcs.resize(outstanding_reads.size());
    if (!rc.pollCqIsOk(conn::ReliableConnection::SendCq, wcs)) {
      throw std::runtime_error("Error while polling CQ.");
    }
    for (auto const &wc : wcs) {
      if (wc.status != IBV_WC_SUCCESS) {
        // TODO(Antoine): consider the guy as being dead instead?
        throw std::runtime_error(
            fmt::format("Error in RDMA READ: {}", wc.status));
      }

      auto const job_handle = wc.wr_id;

      auto const [expected_job_handle, index, start] =
          outstanding_reads.front();
      outstanding_reads.pop_front();

      if (job_handle != expected_job_handle) {
        throw std::runtime_error(fmt::format(
            "Polling returned job_handle: {}, I was expecting job_handle:  {}",
            job_handle, expected_job_handle));
      }

      // Check if at least one subslot is good.
      // Return the most up-to-date subslot
      // If both subslots are bad, we need to check the timestamp before
      // declaring the writer as Byzantine.

      std::optional<std::pair<Incarnation, size_t>> best_subslot;
      for (size_t subslot = 0; subslot < 2; subslot++) {
        auto const base_ptr = job_handle + subslot_size * subslot;
        auto *const header = reinterpret_cast<Header *>(base_ptr);
        // It's important that the data follows right after the Header, i.e.,
        // the Header is packed.
        if (header->hash ==
            XXH3_64bits(&header->incarnation,
                        sizeof(Header::incarnation) + value_size)) {
          if (!best_subslot || best_subslot->first < header->incarnation) {
            best_subslot.emplace(static_cast<Incarnation>(header->incarnation),
                                 subslot);
          }
        }
      }

      if (best_subslot) {
        completed_reads.try_emplace(
            job_handle,
            reinterpret_cast<void *>(job_handle +
                                     subslot_size * best_subslot->second +
                                     sizeof(Header)),
            best_subslot->first -
                2);  // -2 is because of initialization in which we write twice
        continue;
      }

      if (start + constants::WriteCooldown < std::chrono::steady_clock::now()) {
        // The read took too long, we need to reschedule it.
        queued_reads.emplace_back(job_handle, index);
      } else {
        // TODO(Antoine): the guy is Byzantine.
        throw std::runtime_error(
            "Byzantine behavior detected, we should handle it.");
      }
    }
  }

  void pushToQp() {
    while (outstanding_reads.size() < MaxOutstandingReads &&
           !queued_reads.empty()) {
      auto const [job_handle, index] = queued_reads.front();
      queued_reads.pop_front();
      auto *const local_buffer = reinterpret_cast<void *>(job_handle);
      auto const before_post = std::chrono::steady_clock::now();
      auto const posted =
          rc.postSendSingle(conn::ReliableConnection::RdmaReq::RdmaRead,
                            reinterpret_cast<uint64_t>(job_handle),
                            local_buffer, static_cast<uint32_t>(register_size),
                            rc.remoteBuf() + index * register_size);
      if (!posted) {
        throw std::runtime_error(
            "Failed to post read");  // Todo: consider as having failed.
      }
      outstanding_reads.emplace_back(job_handle, index, before_post);
    }
  }

  size_t const nb_registers;
  size_t const value_size;
  size_t const subslot_size;
  size_t const register_size;
  conn::ReliableConnection rc;

  std::vector<JobHandle> buffer_pool;
  std::deque<std::pair<JobHandle, Index>> queued_reads;
  std::deque<
      std::tuple<JobHandle, Index, std::chrono::steady_clock::time_point>>
      outstanding_reads;
  std::unordered_map<JobHandle, std::pair<void *, Incarnation>> completed_reads;

  std::vector<struct ibv_wc> wcs;
};

}  // namespace dory::ubft::swmr
