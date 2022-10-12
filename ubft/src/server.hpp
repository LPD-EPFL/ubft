#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>

#include <dory/shared/branching.hpp>
#include <dory/shared/logger.hpp>

#include "consensus/consensus.hpp"
#include "rpc/server.hpp"

#include "latency-hooks.hpp"

namespace dory::ubft {

class Server {
  template <typename T>
  using Ref = std::reference_wrapper<T>;
  template <typename T>
  using ConstRef = Ref<const T>;

 public:
  using Request = consensus::Request;

  Server(ProcId const local_id, std::vector<ProcId> const& server_ids,
         rpc::Server&& rpc_server, consensus::Consensus&& consensus,
         size_t const max_batch_size)
      : local_id{local_id},
        server_ids{server_ids},
        leader_id{*std::min_element(server_ids.begin(), server_ids.end())},
        rpc_server{std::move(rpc_server)},
        consensus{std::move(consensus)} {
    to_propose.reserve(max_batch_size);
  }

  void tick() {
    if (unlikely(waiting_for_checkpoint_after)) {
      throw std::runtime_error(
          "Cannot tick before having checkpointed the app state.");
    }
    if (unlikely(batch)) {
      throw std::runtime_error(
          "Cannot tick before having fully consummed last batch.");
    }
    rpc_server.tick();
    consensus.tick();
    pollClientRequests();
    if (leader_id == local_id) {
      if (unlikely(should_repropose)) {
        repropose();
      } else {
        pollProposable();
      }
    }
    // TODO: if no progress is made... change leader!
  }

  /**
   * @brief Optionally return a request to execute.
   *
   * IMPORTANT: Must be called until it returns std::nullopt. Otherwise,
   * some requests may be lost upon the next tick. Indeed, as batches are only
   * valid as long as consensus is not ticked, the next tick will invalidate the
   * current batch. As a protection, tick will not run if a batch was not fully
   * polled.
   *
   * @return std::optional<std::pair<Request, bool>>
   *         first: The request to execute.
   *         second: Whether tha app state should be checkpointed.
   */
  std::optional<std::pair<Request, bool>> pollToExecute() {
    // If we are don't have a batch, we try to fetch a new one.
    if (!batch) {
      if (auto const opt_decision = consensus.pollDecision()) {
        #ifdef LATENCY_HOOKS
          if (leader_id == local_id) {
            hooks::smr_latency.addMeasurement(hooks::Clock::now() - hooks::smr_start);
            if (hooks::smr_latency.measured() == 30000) {
              fmt::print("SMR LATENCY REPORT\n");
              hooks::smr_latency.reportOnce();
              fmt::print("SWMR READ REPORT\n");
              hooks::swmr_read_latency.reportOnce();
              fmt::print("SWMR WRITE REPORT\n");
              hooks::swmr_write_latency.reportOnce();
              fmt::print("SIG COMPUTATION REPORT\n");
              hooks::sig_computation_latency.reportOnce();
              fmt::print("SIG CHECK REPORT\n");
              hooks::sig_check_latency.reportOnce();
            }
          }
        #endif
        auto [instance, new_batch, checkpoint] = *opt_decision;
        if (unlikely(next_expected_batch != instance)) {
          throw std::logic_error(
              "Missed a decision and state transfer not implemented.");
        }
        next_expected_batch = instance + 1;
        if (unlikely(checkpoint)) {
          waiting_for_checkpoint_after = instance;
        }
        batch.emplace(new_batch, std::nullopt);
        batch->second.emplace(batch->first.requests());
      } else {
        // If no batch could be fetched...
        return std::nullopt;
      }
    }
    // There is a current batch, let's return a request.
    auto& request_it = *batch->second;
    auto request = *request_it;
    ++request_it;
    if (request_it.done()) {
      batch.reset();
    }
    LOGGER_DEBUG(logger, "Polled request {} from {} to execute.", request.id(),
                 request.clientId());
    return std::make_pair(request, waiting_for_checkpoint_after.has_value());
  }

  /**
   * @brief Respond to the client.
   *
   * @param request
   * @param response
   * @param response_size
   */
  inline void executed(Request const& request, uint8_t const* const response,
                       size_t const response_size) {
    rpc_server.executed(request.clientId(), request.id(), response,
                        response_size);
  }

  void checkpointAppState(uint8_t const* const state_begin,
                          uint8_t const* const state_end) {
    if (unlikely(!waiting_for_checkpoint_after)) {
      throw std::logic_error("No checkpoint expected.");
    }
    consensus.triggerCheckpoint(*waiting_for_checkpoint_after, state_begin,
                                state_end);
    waiting_for_checkpoint_after.reset();
  }

  void toggleSlowPath(bool const enable) {
    // slow_path_enabled = enable;
    // rpc_server.toggleSlowPath(enable);
    consensus.toggleSlowPath(enable);
    rpc_server.toggleSlowPath(enable);
  }

  void toggleRpcOptimism(bool const optimism) {
    optimistic_rpc = optimism;
    rpc_server.toggleOptimism(optimism);
  }

 private:
  /**
   * @brief Poll requests received in RPC to participate on them in consensus.
   *
   */
  void pollClientRequests() {
    while (auto const opt_request = rpc_server.pollReceived()) {
      auto const& request = opt_request->get();
      LOGGER_DEBUG(logger, "Will accept request {} from {}.", request.id(),
                   request.clientId());
      if (!consensus.acceptRequest(request.clientId(), request.id(),
                                   request.begin(), request.size())) {
        LOGGER_WARN(logger,
                    "Won't accept the new request {} from {} as it could drop "
                    "(undecided) promises.",
                    request.id(), request.clientId());
      }
    }
  }

  /**
   * @brief Poll requests that were echoed by everyone and propose them to
   * consensus.
   *
   */
  void pollProposable() {
    if (!consensus.canPropose() || !consensus.slotAvailable()) {
      return;
    }
    if (!to_propose.empty()) {
      to_propose.clear();
    }
    size_t batch_buffer_size = 0;
    while (to_propose.size() < to_propose.capacity()) {
      auto const opt_request = rpc_server.pollProposable();
      if (!opt_request) {
        break;
      }
      LOGGER_DEBUG(logger, "Will propose {}.", opt_request->get().id());
      batch_buffer_size += Request::bufferSize(opt_request->get().size());
      to_propose.push_back(*opt_request);
    }
    if (!to_propose.empty()) {
      #ifdef LATENCY_HOOKS
        hooks::smr_start = hooks::Clock::now();
      #endif
      auto opt_batch =
          consensus.getSlot(consensus::Consensus::Size(batch_buffer_size));
      if (unlikely(!opt_batch)) {
        throw std::logic_error("Was checked just before, should not throw.");
      }
      // We copy each request in the new batch to propose.
      auto& batch = *opt_batch;
      auto batch_it = batch.requests();
      for (auto const& request_ref : to_propose) {
        auto& request = request_ref.get();
        if (unlikely(batch_it.done())) {
          throw std::logic_error("Should be able to copy ALL requests.");
        }
        auto batch_request = *batch_it;
        batch_request.clientId() = request.clientId();
        batch_request.id() = request.id();
        batch_request.size() = request.size();
        std::copy(request.begin(), request.end(), batch_request.begin());
        ++batch_it;
      }
      if (unlikely(!batch_it.done())) {
        throw std::logic_error("The requests should fit perfectly the batch.");
      }

      propose();
    }
  }

  /**
   * @brief Try to propose again consensus slots that have been prepared but
   *        yielded a WaitCheckpoint last time.
   *
   */
  void repropose() { propose(); }

  /**
   * @brief Propose prepared consensus slots and handle errors.
   *
   */
  void propose() {
    auto const propose_res = consensus.propose();
    if (!propose_res.ok()) {
      if (propose_res.error !=
          consensus::Consensus::ProposalResult::WaitCheckpoint) {
        throw std::runtime_error(
            fmt::format("Proposing failed: {}", propose_res.toString()));
      }
      should_repropose = true;
    } else {
      should_repropose = false;
    }
  }

  ProcId const local_id;
  std::vector<ProcId> const server_ids;
  ProcId leader_id;

  rpc::Server rpc_server;
  consensus::Consensus consensus;

  std::vector<ConstRef<rpc::Server::Request>>
      to_propose;  // Defined here to not allocate dynamically

  bool optimistic_rpc = false;

  consensus::Instance next_expected_batch = 0;
  std::optional<consensus::Instance> waiting_for_checkpoint_after;
  bool should_repropose = false;

  std::optional<
      std::pair<consensus::Batch, std::optional<consensus::Batch::Iterator>>>
      batch;
  LOGGER_DECL_INIT(logger, "UbftServer");
};

}  // namespace dory::ubft
