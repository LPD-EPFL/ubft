#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/core.h>

#include <dory/conn/rc.hpp>
#include <dory/shared/branching.hpp>

#include "../swmr/reader.hpp"

namespace dory::ubft::replicated_swmr {

class Reader {
 public:
  using JobHandle = uintptr_t;
  using Index = swmr::Reader::Index;
  using Incarnation = swmr::Reader::Incarnation;
  using PollResult =
      std::optional<std::pair<std::unique_ptr<uint8_t[]>, Incarnation>>;

 private:
  class ManagedReader {
   public:
    using PollResult = swmr::Reader::PollResult;

    ManagedReader(swmr::Reader &&reader) : reader{std::move(reader)} {}

    void tick() {
      reader.tick();
      tryRelease();
      pushToReader();
    }

    void read(JobHandle handle, Index index) {
      queued_reads.emplace_back(handle, index);
      pushToReader();
    }

    PollResult poll(JobHandle handle) {
      // We only poll the underlying reader if a read was scheduled.
      auto const scheduled_it = scheduled_reads.find(handle);
      if (scheduled_it == scheduled_reads.end()) {
        return std::nullopt;
      }
      return reader.poll(scheduled_it->second);
    }

    void release(JobHandle handle) {
      // If the read was already scheduled, we will need to wait for its
      // completion before releasing it.
      auto const scheduled_it = scheduled_reads.find(handle);
      if (likely(scheduled_it != scheduled_reads.end())) {
        scheduled_reads.erase(scheduled_it);
        to_release.insert(scheduled_it->second);
        return;
      }
      // Otherwise, as it hasn't been scheduled yet, we can remove it from the
      // queue.
      auto const find_it =
          std::find_if(queued_reads.begin(), queued_reads.end(),
                       [&handle](auto const &handle_index) {
                         return handle_index.first == handle;
                       });
      queued_reads.erase(find_it);
    }

   private:
    void tryRelease() {
      for (auto it = to_release.cbegin(); it != to_release.cend();
           /* in body */) {
        auto const jh = *it;
        if (reader.poll(jh)) {
          it = to_release.erase(it);
          reader.release(jh);
        } else {
          ++it;
        }
      }
    }

    void pushToReader() {
      while (!queued_reads.empty()) {
        auto const &[replicated_handler, index] = queued_reads.front();
        auto const opt_handle = reader.read(index);
        if (!opt_handle) {
          break;
        }
        queued_reads.pop_front();
        scheduled_reads.try_emplace(replicated_handler, *opt_handle);
      }
    }

    std::deque<std::pair<JobHandle, Index>> queued_reads;
    std::unordered_map<JobHandle, swmr::Reader::JobHandle> scheduled_reads;
    std::unordered_set<swmr::Reader::JobHandle> to_release;
    swmr::Reader reader;
  };

 public:
  Reader(std::vector<swmr::Reader> &&readers)
      : value_size{readers.front().valueSize()} {
    if (readers.empty()) {
      throw std::runtime_error("There should be at least one sub-reader.");
    }
    // TODO(Antoine): check that all abstractions have the same number of
    // registers, etc. for extra safety.
    for (auto &reader : readers) {
      this->readers.emplace_back(std::move(reader));
    }
  }

  /**
   * @brief Schedule a register READ
   *
   * @param index of the register in the register array
   * @return JobHandle where the READ will place the data
   */
  JobHandle read(Index const index) {
    auto const handle = next_handle++;
    for (auto &managed_reader : readers) {
      managed_reader.read(handle, index);
    }
    return handle;
  }

  PollResult poll(JobHandle const handle) {
    size_t reads = 0;
    ManagedReader::PollResult highest_polled;
    for (auto &managed_reader : readers) {
      auto polled = managed_reader.poll(handle);
      if (polled) {
        reads++;
        if (!highest_polled || highest_polled->second < polled->second) {
          highest_polled = polled;
        }
      }
    }
    if (reads < (readers.size() + 1) / 2) {
      return std::nullopt;
    }
    auto buffer = std::make_unique<uint8_t[]>(value_size);
    memcpy(buffer.get(), highest_polled->first, value_size);

    for (auto &managed_reader : readers) {
      managed_reader.release(handle);
    }

    return {{std::move(buffer), highest_polled->second}};
  }

  void tick() {
    for (auto &managed_reader : readers) {
      managed_reader.tick();
    }
  }

 private:
  size_t const value_size;
  std::vector<ManagedReader> readers;

  JobHandle next_handle = 0;
};

}  // namespace dory::ubft::replicated_swmr
