#pragma once

#define CB_CHECKPOINTS true

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <xxhash.h>
#include <hipony/enumerate.hpp>

#include <dory/conn/rc.hpp>
#include <dory/crypto/hash/blake3.hpp>
#include <dory/shared/branching.hpp>
#include <dory/shared/dynamic-bitset.hpp>
#include <dory/shared/logger.hpp>
#include <dory/shared/match.hpp>
#include <dory/shared/unused-suppressor.hpp>
#include <dory/third-party/sync/mpmc.hpp>

#include "../buffer.hpp"
#include "../certifier/certifier.hpp"
#include "../crypto.hpp"
#include "../tail-cb/broadcaster.hpp"
#include "../tail-cb/receiver.hpp"
#include "../tail-map/tail-map.hpp"
#include "../tail-p2p/receiver.hpp"
#include "../tail-p2p/sender.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "../unsafe-at.hpp"
#include "app.hpp"
#include "internal/instance-state.hpp"
#include "internal/messages.hpp"
#include "internal/packing.hpp"
#include "internal/replica-state.hpp"
#include "internal/requests.hpp"
#include "internal/view-change.hpp"
#include "types.hpp"

#include "../latency.hpp"
#include "../latency-hooks.hpp"

namespace dory::ubft::consensus {

class Consensus {
 public:
  using Size = tail_p2p::Size;

  struct ProposalResult {
    enum ErrorCode {
      NoError,
      NotLeader,
      OngoingViewChange,
      NothingToPropose,
      WaitCheckpoint,
    };

    ProposalResult() = default;

    ProposalResult(ErrorCode error) : error{error} {}

    bool ok() const { return error == NoError; }

    std::string toString() const {
      switch (error) {
        case NoError:
          return "NoError";
        case NotLeader:
          return "NotLeader";
        case OngoingViewChange:
          return "OngoingViewChange";
        case NothingToPropose:
          return "NothingToPropose";
        case WaitCheckpoint:
          return "WaitCheckpoint";
        default:
          return "UnknownError";
      }
    }

    ErrorCode error{NoError};
  };

 private:
  using MessageKind = internal::MessageKind;
  using Message = internal::Message;
  using PrepareMessage = internal::PrepareMessage;
  using CommitMessage = internal::CommitMessage;
  using CheckpointMessage = internal::CheckpointMessage;
  using SealViewMessage = internal::SealViewMessage;
  using NewViewMessage = internal::NewViewMessage;
  using FastCommitMessage = internal::FastCommitMessage;
  using Certificate = certifier::Certificate;

  struct VerifiedCommit {
    size_t from;
    Certificate prepare_certificate;
    bool valid;
  };

 public:
  Consensus(TailThreadPool &thread_pool, tail_cb::Broadcaster &&cb_broadcaster,
            std::vector<tail_cb::Receiver> &&cb_receivers,
            certifier::Certifier &&prepare_certifier,
            std::vector<tail_p2p::AsyncSender> &&fast_commit_senders,
            std::vector<tail_p2p::Receiver> &&fast_commit_receivers,
            std::vector<certifier::Certifier> &&vc_state_certifiers,
            certifier::Certifier &&checkpoint_certifier,
            std::vector<certifier::Certifier> &&cb_checkpoint_certifiers,
            std::vector<tail_p2p::AsyncSender> &&cb_checkpoint_senders,
            std::vector<tail_p2p::Receiver> &&cb_checkpoint_receivers,
            ProcId const local_id, size_t const window,
            size_t const max_request_size, size_t const max_batch_size,
            size_t const client_window)
      : cb_broadcaster{std::move(cb_broadcaster)},
        cb_receivers{std::move(cb_receivers)},
        prepare_certifier{std::move(prepare_certifier)},
        fast_commit_senders{std::move(fast_commit_senders)},
        fast_commit_receivers{std::move(fast_commit_receivers)},
        vc_state_certifiers{std::move(vc_state_certifiers)},
        checkpoint_certifier{std::move(checkpoint_certifier)},
        cb_checkpoint_certifiers{std::move(cb_checkpoint_certifiers)},
        cb_checkpoint_senders{std::move(cb_checkpoint_senders)},
        cb_checkpoint_receivers{std::move(cb_checkpoint_receivers)},
        local_id{local_id},
        local_index{this->cb_receivers.size()},  // We're last.
        quorum{(this->cb_receivers.size() + 1) / 2 + 1},
        local_checkpoint{0, window, {}},
        checkpoint_certificate{this->checkpoint_certifier.genesisCertificate(
            Checkpoint(0, window, {}))},
        window{window},
        can_cb_until{this->cb_broadcaster.getTail() - 1},
        max_proposal_size{Batch::bufferSize(max_batch_size, max_request_size)},
        proposal_buffer_pool{window,
                             PrepareMessage::bufferSize(max_proposal_size)},
        commit_buffer_pool{
            1, CommitMessage::bufferSize(max_proposal_size, quorum)},
        checkpoint_buffer_pool{1, CheckpointMessage::bufferSize(quorum)},
        instance_states{window},
        request_log{client_window, max_request_size} {
    // We don't care about promises for checkpoints, we want certificates.
    this->checkpoint_certifier.toggleFastPath(false);
    this->checkpoint_certifier.toggleSlowPath(true);

    for (auto &vs_state_certifier : this->vc_state_certifiers) {
      vs_state_certifier.toggleFastPath(false);
      vs_state_certifier.toggleSlowPath(true);
    }

    for (auto &cb_checkpoint_certifier : this->cb_checkpoint_certifiers) {
      cb_checkpoint_certifier.toggleFastPath(false);
      cb_checkpoint_certifier.toggleSlowPath(true);
    }

    for (auto const &_ : this->cb_receivers) {
      commit_verification_task_queues.emplace_back(thread_pool, window);
      buffered_commits.emplace_back(window);
    }
    // For simplicity, we also "buffer" our own commits. This happens if the
    // prepared message was decided/checkpointed before using the commit.
    // It could be handled differently, but this way keeps things simple.
    buffered_commits.emplace_back(window);

    // We build the list of ids, and also the inverse.
    for (auto &receiver : this->cb_receivers) {
      indices.emplace(receiver.procId(), ids.size());
      ids.push_back(receiver.procId());
      states.emplace_back(window, max_proposal_size);
    }
    indices.emplace(local_id, ids.size());
    ids.push_back(local_id);
    states.emplace_back(window, max_proposal_size);
    // We sort ids for leader election.
    sorted_ids = ids;
    std::sort(sorted_ids.begin(), sorted_ids.end());
  }

  void testApp(size_t const nb_proposals, size_t const request_size,
               size_t const batch_size, bool const fast_path,
               size_t credits = 1,
               std::optional<size_t> const crash_at = std::nullopt) {
    // We will change to slow path upon failure.
    toggleSlowPath(!fast_path);

    app::Application app;
    size_t proposed = 0;
    size_t accepted = 0;
    size_t executed = 0;
    bool triggered_view_change = false;
    LatencyProfiler latency_profiler(fast_path ? 5000 : 100);
    Buffer received_request(request_size);

    // If we are going through the slow path but no failure is planned, the last
    // replica does not participate.
    if (!fast_path && !crash_at) {
      while (sorted_ids.back() == local_id) {
      };
    }

    std::deque<std::chrono::steady_clock::time_point> proposal_times;
    auto const begin = std::chrono::steady_clock::now();
    while (true) {
      if (unlikely(crash_at)) {
        // After some proposals, the first leader stops working.
        while (proposed == *crash_at && local_id == leader(0)) {
        }

        // Let's say that the other trigger view change at about the same time.
        if (!triggered_view_change && executed >= *crash_at - 1) {
          toggleSlowPath(true);
          changeView();
          if (leader(uat(states, local_index).at_view) == local_id) {
            while (!canPropose()) {
              tick();
            }
            proposed = this->proposed;  // So that we continue the sequence.
          }
          triggered_view_change = true;
        }
      }

      // Leader changes now
      auto const is_leader =
          leader(uat(states, local_index).at_view) == local_id;

      // We simulate receiving requests form clients: every time we execute a
      // request, we accept some more.
      while (accepted <
             std::min({nb_proposals, executed + request_log.window()})) {
        fmt::format_to(received_request.data(), "{0:0{1}}", accepted,
                       request_size);
        if (!acceptRequest(0, accepted, received_request.data(),
                           request_size)) {
          throw std::logic_error("Should be able to accept the request.");
        }
        accepted++;
      }

      tick();
      if (auto opt_decision = pollDecision()) {
        auto const &batch = std::get<1>(*opt_decision);
        LOGGER_DEBUG(logger,
                     "[Test] Decided on a batch of size {} for instance {}!",
                     batch.size, std::get<0>(*opt_decision));
        for (auto it = batch.requests(); !it.done(); ++it) {
          auto const request = *it;
          LOGGER_DEBUG(
              logger,
              "[Test] Executing <client_id: {}, id: {}, request: '{}'>!",
              request.clientId(), request.id(), request.stringView());
          app.execute(request.begin(), request.size());
          executed++;
        }
        if (is_leader) {
          latency_profiler.addMeasurement(std::chrono::steady_clock::now() -
                                          proposal_times.front());
          proposal_times.pop_front();
        }
        // If we should trigger a checkpoint
        if (std::get<2>(*opt_decision)) {
          auto const app_state = app.hash();
          auto const *const app_state_begin =
              reinterpret_cast<uint8_t const *>(&app_state);
          auto const *const app_state_end = app_state_begin + sizeof(app_state);
          triggerCheckpoint(std::get<0>(*opt_decision), app_state_begin,
                            app_state_end);
        }
        if (executed == nb_proposals) {
          // Latency report
          if (is_leader) {
            auto const duration = std::chrono::steady_clock::now() - begin;
            latency_profiler.report();
            LOGGER_INFO(logger, "Decision latency percentiles:");
            for (int i = 1; i < 100; i++) {
              fmt::print("{}\n",
                         latency_profiler.percentile(static_cast<double>(i)));
            }
            fmt::print("Final state of the app after {} proposals: {}\n",
                       nb_proposals, app.hash());
            auto const decided = request_size * batch_size * nb_proposals;
            auto const nb_micros = static_cast<size_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(duration)
                    .count());
            LOGGER_INFO(
                logger, "Duration: {}, Decided: {}KB, Throughput: {}KBps",
                duration, decided / 1024, decided * 1000000 / nb_micros / 1024);
          }

          // Reference.
          app::Application ref_app;
          for (size_t p = 0; p < nb_proposals; p++) {
            auto const data = fmt::format("{0:0{1}}", p, request_size);
            ref_app.execute(reinterpret_cast<uint8_t const *>(data.data()),
                            data.size());
          }
          if (app.hash() == ref_app.hash()) {
            LOGGER_INFO(logger, "TEST PASSED!");
          } else {
            LOGGER_CRITICAL(logger, "TEST FAILED!");
          }
        }
        if (is_leader) {
          credits++;
        }
      }

      if (canPropose() && credits > 0 && proposed < accepted) {
        auto const batched_requests =
            std::min({accepted - proposed, batch_size});
        auto opt_batch = getSlot(static_cast<Size>(
            Batch::bufferSize(batched_requests, request_size)));
        if (!opt_batch) {
          throw std::runtime_error("Ran out of slots!\n");
        }
        auto &batch = *opt_batch;
        for (auto it = batch.requests(); !it.done(); ++it) {
          auto request = *it;
          request.clientId() = 0;
          request.id() = proposed++;
          request.size() = request_size;
          fmt::format_to(request.begin(), "{0:0{1}}", request.id(),
                         request_size);
          LOGGER_DEBUG(logger,
                       "[Test] Batched <client_id: {}, id: {}, request: '{}'>.",
                       request.clientId(), request.id(), request.stringView());
        }
        proposal_times.emplace_back(std::chrono::steady_clock::now());
        while (propose().error == ProposalResult::WaitCheckpoint) {
          // We give the opportunity to the consensus to fetch the next
          // checkpoint.
          tick();
        }
        credits--;
      }
    }
  }

  bool slotAvailable() { return proposal_buffer_pool.borrowNext().has_value(); }

  /**
   * @brief Get a batch where to write the requests.
   *
   * @param proposal_size
   * @return std::optional<Batch>
   */
  std::optional<Batch> getSlot(Size const batch_size) {
    if (unlikely(batch_size > max_proposal_size)) {
      throw std::invalid_argument(
          fmt::format("Requested size {} > max proposal size {}.", batch_size,
                      max_proposal_size));
    }
    auto const prepare_size = PrepareMessage::bufferSize(batch_size);
    auto opt_buffer = proposal_buffer_pool.take(prepare_size);
    if (unlikely(!opt_buffer)) {
      return std::nullopt;
    }
    to_propose.emplace_back(std::move(*opt_buffer));
    auto &prepare_buffer =
        *reinterpret_cast<PrepareMessage::Layout *>(to_propose.back().data());
    prepare_buffer.kind = MessageKind::Prepare;
    prepare_buffer.view = uat(states, local_index).at_view;
    prepare_buffer.instance = next_proposal++;
    return Batch(*reinterpret_cast<Batch::Layout *>(prepare_buffer.data()),
                 batch_size);
  }

  ProposalResult propose() {
    if (unlikely(leader(uat(states, local_index).at_view) != local_id)) {
      return ProposalResult::NotLeader;
    }

    if (unlikely(ongoing_view_change)) {
      return ProposalResult::OngoingViewChange;
    }

    if (unlikely(to_propose.empty())) {
      return ProposalResult::NothingToPropose;
    }

    while (!to_propose.empty()) {
      auto &buffer = *to_propose.begin();
      if (unlikely(proposed >=
                   uat(states, local_index).checkpoint.propose_range.high)) {
        return ProposalResult::WaitCheckpoint;
      }
      proposed++;
      waitForCbSlack();
      auto cb_msg = cb_broadcaster.broadcast(
          buffer.data(),
          static_cast<tail_cb::Broadcaster::Size>(buffer.size()));
      to_propose.pop_front();
      handleCbMessage(local_index, std::move(cb_msg));
    }

    return ProposalResult();
  }

  void tick() {
    // 1. Base Abstractions
    cb_broadcaster.tick();
    for (auto &receiver : cb_receivers) {
      receiver.tick();
    }
    prepare_certifier.tick();
    for (auto &sender : fast_commit_senders) {
      sender.tickForCorrectness();
    }
    for (auto &certifier : cb_checkpoint_certifiers) {
      certifier.tick();
    }
    for (auto &sender : cb_checkpoint_senders) {
      sender.tickForCorrectness();
    }
    checkpoint_certifier.tick();

    // 2. Consensus logic
    pollCheckpointCertificate();
    broadcastCheckpointCertificate();
    pollCbs();
    pollPrepareCertificatePromises();
    if (unlikely(slow_path_enabled)) {
      tryCertifyPrepares();
      pollPrepareCertificates();
      pollVerifiedCommits();
      for (auto &certifier : vc_state_certifiers) {
        certifier.tick();
      }
      pollVcStateCertificates();
    }
    pollFastCommits();
    pollCbCheckpointCertificate();
  }

  /**
   * @brief Poll for any decided value.
   *
   * @return std::optional<std::tuple<Instance, Batch, bool>>
   *         0. The decided instance.
   *         1. A Batch view of the decided requests, safe until the next tick.
   *         2. Whether a new checkpoint should be triggered.
   */
  std::optional<std::tuple<Instance, Batch, bool>> pollDecision() {
    if (unlikely(instance_states.empty())) {
      return std::nullopt;
    }
    auto next_it = instance_states.find(next_to_decide);
    if (unlikely(next_it == instance_states.end())) {
      return std::nullopt;
    }
    auto &data = next_it->second;
    if (likely(!data.decidable())) {
      return std::nullopt;
    }
    data.decided = true;
    auto const decided_instance = next_to_decide++;
    auto const should_checkpoint =
        (decided_instance % (window / 2)) == (window / 2 - 1);
    auto const batch = data.prepare_message.asBatch();
    request_log.decided(batch);
    return std::make_tuple(decided_instance, batch, should_checkpoint);
  }

  void triggerCheckpoint(Instance const last_applied,
                         uint8_t const *const state_begin,
                         uint8_t const *const state_end) {
    auto const next_instance = last_applied + 1;
    if (unlikely(next_instance <= local_checkpoint.propose_range.low)) {
      throw std::logic_error("App digests went backwards.");
    }
    // The previous is useless, we can remove it.
    // TODO(Ant.): we might improve performance by keeping 2 checkpoints in the
    // pipeline.
    checkpoint_certifier.forgetMessages(next_instance);

    // TODO(Antoine): remove from the critical path
    local_checkpoint = {next_instance, window,
                        crypto::hash::blake3(state_begin, state_end)};
    auto const *const pbegin =
        reinterpret_cast<uint8_t const *>(&local_checkpoint);
    auto const *const pend = pbegin + sizeof(Checkpoint);
    checkpoint_certifier.acknowledge(next_instance, pbegin, pend);
    LOGGER_DEBUG(logger,
                 "[Checkpoint] Acknowledged the checkpoint that opens [{}, {})",
                 local_checkpoint.propose_range.low,
                 local_checkpoint.propose_range.high);
  }

  void toggleSlowPath(bool const enable) {
    slow_path_enabled = enable;
    cb_broadcaster.toggleSlowPath(enable);
    for (auto &receiver : cb_receivers) {
      receiver.toggleSlowPath(enable);
    }
    prepare_certifier.toggleSlowPath(enable);
    for (auto &certifier : vc_state_certifiers) {
      certifier.toggleSlowPath(enable);
    }
  }

  void changeView() {
    // We need to cb-broadcast Commit messages for each FastCommit message we
    // broadcast. We can do it in a sloppy fashion: tick the certifier and
    // commit while blocking the rest of the protocol.
    auto committed_all = false;
    while (!committed_all) {
      // Let's check that everything was committed, and if not we loop.
      // 1. Let's tick what lets us fast commit. We shouldn't tick everything as
      // we don't want to process new cb-messages.
      // TODO: also get checkpoints to not get stuck.
      prepare_certifier.tick();
      pollPrepareCertificates();
      pollVerifiedCommits();
      // 2. Let's check.
      committed_all = true;
      for (auto &[instance, data] : instance_states) {
        if (data.fastCommitted(local_index) &&
            !data.slowCommitted(local_index)) {
          committed_all = false;
          break;
        }
      }
    }
    LOGGER_DEBUG(logger,
                 "[ChangingView] Slow-committed all fast committed proposals.");

    SealViewMessage::Layout seal_view;
    seal_view.kind = MessageKind::SealView;
    waitForCbSlack();
    auto cb_msg = cb_broadcaster.broadcast(
        reinterpret_cast<uint8_t *>(&seal_view),
        static_cast<tail_cb::Broadcaster::Size>(SealViewMessage::BufferSize));
    handleCbMessage(local_index, std::move(cb_msg));
    // We can forget about all prepare messages from older views.
    prepare_certifier.forgetMessages(
        internal::pack(uat(states, local_index).at_view, 0));
    // And we can also forget all the state about instances from older
    // instances.
    instance_states.clear();
  }

  bool inline canPropose() const {
    return leader(uat(states, local_index).at_view) == local_id &&
           !ongoing_view_change;
  }

  bool acceptRequest(ProcId const client_id, RequestId const request_id,
                     uint8_t const *const begin, size_t const size) {
    return request_log.addRequest(client_id, request_id, begin, size);
  }

 private:
  ProcId leader(View view) const { return uat(sorted_ids, view % ids.size()); }

  void pollCbs() {
    for (auto &&[replica, receiver] : hipony::enumerate(cb_receivers)) {
      if (auto polled = receiver.poll()) {
        handleCbMessage(replica, std::move(*polled));
      }
    }
  }

  void handleCbMessage(size_t const from, tail_cb::Message &&cb_msg) {
    if (cb_msg.index() != uat(states, from).next_cb++) {
      LOGGER_CRITICAL(logger,
                      "Gap in CB messages, recovery not implemented yet. "
                      "Spinning to preserve RDMA accesses.");
      while (true) {
      }
      // TODO:
      // 1. Buffer the polled message.
      // 2. Mark not to cb-poll this replica anymore.
      // 3. Poll cb_checkpoint_receivers.at(from).
      // 4. If polled a valid certificate:
      //    - adopt the state
      //    - consume the slot buffered in (1)
      //    - undo (2)
    }
    auto pot_msg =
        Message::tryFrom(std::move(cb_msg), window, max_proposal_size, quorum);
    match{pot_msg}(
        [this](std::invalid_argument &err) noexcept {
          LOGGER_ERROR(logger, "Message is malformatted: {}", err.what());
        },
        [this, from](PrepareMessage &prepare) {
          handlePrepare(from, std::move(prepare));
        },
        [this, from](CommitMessage &commit) {
          handleCommit(from, std::move(commit));
        },
        [this, from](CheckpointMessage &checkpoint) {
          handleCheckpoint(from, std::move(checkpoint));
        },
        [this, from](SealViewMessage &seal_view) {
          handleSealView(from, std::move(seal_view));
        },
        [this, from](NewViewMessage &new_view) {
          handleNewView(from, std::move(new_view));
        });
    maybeCertifyCbCheckpoint(from);
  }

  bool isByzantine(size_t from, PrepareMessage &prepare) {
    auto id = uat(ids, from);
    if (leader(prepare.view()) != id) {
      LOGGER_ERROR(
          logger,
          "Received a prepare from {} while he wasn't leader (view's {} leader "
          "is {}).",
          id, prepare.view(), leader(prepare.view()));
      return true;
    }
    auto &state = uat(states, from);
    if (state.at_view != prepare.view()) {
      // TODO(Antoine): this field is actually redundant.
      LOGGER_ERROR(logger, "Received prepare <V: {},...> from a sender in {}.",
                   prepare.view(), state.at_view);
      return true;
    }
    auto const state_it = pessimistic_find(instance_states, prepare.instance());
    if (state_it != instance_states.end()) {
      LOGGER_ERROR(logger, "{} had already prepared {} in view {}.", id,
                   prepare.instance(), prepare.view());
      return true;
    }
    // Test for replicas that experienced a CB hole:
    // Note: without holes, we could expect consecutive prepares.
    if (unlikely(state.next_prepare > prepare.instance())) {
      LOGGER_ERROR(logger, "{} had already prepared {} in view {}.", id,
                   prepare.instance(), prepare.view());
      return true;
    }
    if (!state.checkpoint.propose_range.contains(prepare.instance())) {
      LOGGER_ERROR(logger, "{} hadn't open instance {}.", id,
                   prepare.instance());
      return true;
    }
    // Checking if the proposal is valid
    if (unlikely(prepare.view() != 0)) {
      if (unlikely(!state.valid_values)) {
        LOGGER_ERROR(logger, "Didn't receive a corresponding NewView");
        return true;
      }
      auto const vv_it = state.valid_values->second.find(prepare.instance());
      if (vv_it != state.valid_values->second.end() &&
          vv_it->second.stringView() != prepare.stringView()) {
        LOGGER_ERROR(logger,
                     "Prepare <V:{}, I:{}> didn't follow NewView's values.",
                     prepare.view(), prepare.instance());
        LOGGER_ERROR(logger, "Expected: {}.", vv_it->second.stringView());
        LOGGER_ERROR(logger, "Received: {}.", prepare.stringView());
        return true;
      }
    }
    return false;
  }

  void handlePrepare(size_t from, PrepareMessage &&prepare) {
    auto &local_state = uat(states, local_index);
    auto &replica_state = uat(states, from);
    if (unlikely(isByzantine(from, prepare))) {
      LOGGER_ERROR(logger, "[Bad prepare] View: {}, Instance: {}, Data: `{}`",
                   prepare.view(), prepare.instance(), prepare.stringView());
      throw std::logic_error(
          fmt::format("Byzantine prepare received from {}.", uat(ids, from)));
    }
    if (unlikely(prepare.view() != local_state.at_view)) {
      return;
    }

    if (unlikely(prepare.instance() <
                 certifiedCheckpoint().propose_range.low)) {
      return;
    }

    auto instance = prepare.instance();
    auto view = prepare.view();
    LOGGER_DEBUG(
        logger,
        "[CB:{}][Prepare] <view: {}, instance: {}, H(proposal): {:016x}>",
        uat(ids, from), view, instance,
        XXH3_64bits(prepare.data(), prepare.size()));
    replica_state.next_prepare = instance;
    auto &data =
        instance_states.tryEmplace(instance, std::move(prepare), ids.size())
            .first->second;

    // Replay buffered (verified) commits
    auto const useful = internal::pack(view, instance);
    for (auto &&[c_from, buffer] : hipony::enumerate(buffered_commits)) {
      while (unlikely(!buffer.empty() && buffer.front().index() < useful)) {
        buffer.popFront();
      }
      if (unlikely(!buffer.empty() && buffer.front().index() == useful)) {
        handleVerifiedCommit(c_from, std::move(buffer.front()));
        buffer.popFront();
      }
    }

    // We prune the valid values that we don't need anymore (so that we don't
    // send them in cb-checkpoints).
    if (unlikely(replica_state.valid_values)) {
      // Valid values must be for this view.
      while (!replica_state.valid_values->second.empty() &&
             replica_state.valid_values->second.begin()->first <= instance) {
        replica_state.valid_values->second.popFront();
      }
    }

    tryCertifyPrepare(local_state, data);
  }

  /**
   * @brief Certify consensus instances for which external validity passes. This
   *        is event-based as external validity can *eventually* become valid in
   *        the slow path.
   */
  void tryCertifyPrepares() {
    auto &local_state = uat(states, local_index);
    // As we are in the slow path, iterating over all requests should have
    // virtually no impact.
    for (auto &[id, data] : instance_states) {
      tryCertifyPrepare(local_state, data);
    }
  }

  void tryCertifyPrepare(internal::ReplicaState &local_state,
                         internal::InstanceState &data) {
    if (data.certified_prepare) {
      return;
    }
    auto &pm = data.prepare_message;
    if (!request_log.isValid(pm.asBatch())) {
      LOGGER_DEBUG(logger,
                   "[Prepare] Received some batched requests that I never "
                   "received (indirectly) from their clients.");
      return;
    }
    auto const from_me = leader(pm.view()) == local_id;
    auto const dont_send_promise = from_me;
    auto const index = internal::pack(pm.view(), pm.instance());
    prepare_certifier.acknowledge(index, pm.data(), pm.data() + pm.size(),
                                  dont_send_promise);
    if (!from_me) {
      prepare_certifier.receivedImplicitPromise(leader(local_state.at_view),
                                                index);
    }
    data.certified_prepare = true;
  }

  void pollPrepareCertificatePromises() {
    while (auto opt_promise = prepare_certifier.pollPromise()) {
      auto const [view, instance] = internal::unpack(*opt_promise);
      LOGGER_DEBUG(logger, "[Prepare Certificate Promise] <instance: {}>.",
                   instance);
      auto data_it = instance_states.find(instance);
      if (unlikely(data_it == instance_states.end())) {
        LOGGER_WARN(logger,
                    "Received prepare promise for {} after having dropped it.",
                    instance);
        continue;
      }
      auto &data = data_it->second;
      for (auto &sender : fast_commit_senders) {
        auto *slot = sender.getSlot(sizeof(FastCommitMessage));
        auto &fcm = *reinterpret_cast<FastCommitMessage *>(slot);
        fcm.view = view;
        fcm.instance = instance;
        sender.send();
      }
      data.receivedFastCommit(local_index);
    }
  }

  void pollPrepareCertificates() {
    while (auto opt_certificate = prepare_certifier.pollCertificate()) {
      auto &certificate = *opt_certificate;
      auto const [view, instance] = internal::unpack(certificate.index());
      LOGGER_DEBUG(
          logger,
          "[Certified Prepare] <view: {}, instance: {}, H(proposal): {:016x}>.",
          view, instance,
          XXH3_64bits(certificate.message(), certificate.messageSize()));
      // If we sealed the view in the mean time, we shouldn't commit.
      if (view != uat(states, local_index).at_view) {
        continue;
      }

      // If we haven't already, we should broadcast a checkpoint to allow the
      // commit.
      if (instance >= uat(states, local_index).checkpoint.propose_range.high) {
        broadcastCheckpointCertificate(true);
      }

      // If we have checkpointed this instance, we shouldn't commit.
      if (instance < uat(states, local_index).checkpoint.propose_range.low) {
        continue;
      }

      // We CB-broadcast the certificate as our commit message.
      // TODO(Antoine): remove the copy that is currently needed for CB-message
      // multiplexing.
      auto const commit_size = CommitMessage::bufferSize(
          certificate.messageSize(), certificate.nbShares());
      auto opt_buffer = commit_buffer_pool.take(commit_size);
      if (unlikely(!opt_buffer)) {
        throw std::logic_error(
            "This buffer should be recycled by the end of the call.");
      }
      auto commit_buffer = std::move(*opt_buffer);
      auto *commit =
          reinterpret_cast<CommitMessage::Layout *>(commit_buffer.data());
      commit->kind = MessageKind::Commit;
      std::copy(certificate.rawBuffer().cbegin(),
                certificate.rawBuffer().cend(), commit->certificate());
      waitForCbSlack();
      auto cb_msg = cb_broadcaster.broadcast(
          commit_buffer.data(),
          static_cast<tail_cb::Broadcaster::Size>(commit_buffer.size()));
      // We also deliver the message locally.
      handleCbMessage(local_index, std::move(cb_msg));
    }
  }

  bool isByzantineCommit(size_t from, Certificate &certificate) {
    // Note: does NOT check the validity of the certificate.
    auto &replica_state = uat(states, from);
    auto const [view, instance] = internal::unpack(certificate.index());
    if (replica_state.checkpoint.propose_range.high <= instance) {
      LOGGER_ERROR(
          logger,
          "Byzantine behavior: instance {} out of {}'s range, it should have "
          "sent a checkpoint.",
          instance, uat(ids, from));
      return true;
    }
    if (replica_state.at_view != view) {
      LOGGER_ERROR(
          logger,
          "Byzantine behavior: {} provided a prepare certificate from view {} "
          "while in view {}.",
          uat(ids, from), view, replica_state.at_view);
      return true;
    }
    auto const prev_commit_it = replica_state.commits.find(instance);
    if (prev_commit_it != replica_state.commits.end() &&
        prev_commit_it->second.view() == view) {
      LOGGER_ERROR(logger,
                   "Byzantine behavior: {} committed {} twice in view {}.",
                   uat(ids, from), instance, view);
    }

    return false;
  }

  void handleCommit(size_t from, CommitMessage &&commit) {
    // Extract the prepare certificate from the commit message.
    auto certificate_ok = commit.tryIntoCertificate();
    if (auto *const error =
            std::get_if<std::invalid_argument>(&certificate_ok)) {
      throw std::runtime_error(fmt::format(
          "Byzantine behavior: malformed certificate: {}", error->what()));
    }
    auto certificate = std::move(std::get<Certificate>(certificate_ok));

    if (unlikely(isByzantineCommit(from, certificate))) {
      throw std::logic_error(
          fmt::format("Byzantine commit received from {}.", uat(ids, from)));
    }

    // We take the commit into account for this replica, but we only use it
    // (i.e., certify a state or decide) when it is verified.
    if (unlikely(!uat(states, from).committed(certificate))) {
      throw std::runtime_error(
          fmt::format("Byzantine {} committed twice.", uat(ids, from)));
    }

    // Push the verification of the commits to a thread if we cannot trust it
    // directly.
    uat(states, from).outstanding_commit_verifications++;
    if (from != local_index) {
      uat(commit_verification_task_queues, from)
          .enqueue(
              [this, from, certificate = std::move(certificate)]() mutable {
                auto const valid = prepare_certifier.check(certificate);
                verified_commits.enqueue({from, std::move(certificate), valid});
              });
    } else {
      // As the commit comes from us, we don't need to check it.
      verified_commits.enqueue({from, std::move(certificate), true});
    }
  }

  /**
   * @brief Poll the commits (i.e., prepare certificates) that have been checked
   *        in the thread pool.
   *
   */
  void pollVerifiedCommits() {
    std::optional<VerifiedCommit> opt_verified_commit;
    // Note: try_dequeue doesn't call optional's destructor.
    while ((opt_verified_commit.reset(),
            verified_commits.try_dequeue(opt_verified_commit))) {
      auto &[from, certificate, valid] = *opt_verified_commit;
      if (unlikely(!valid)) {
        throw std::logic_error(fmt::format(
            "Byzantine commit received from {}, invalid prepare certificate.",
            uat(ids, from)));
      }
      handleVerifiedCommit(from, std::move(certificate));
    }
  }

  /**
   * @brief Handle a commit (i.e., the certificate it contained) of which the
   *        certificate was verified to be valid.
   *
   * @param from
   * @param certificate
   */
  void handleVerifiedCommit(size_t from, Certificate &&certificate) {
    auto &replica_state = uat(states, from);
    replica_state.outstanding_commit_verifications--;

    auto const [view, instance] = internal::unpack(certificate.index());

    LOGGER_DEBUG(
        logger,
        "[VerifiedCommit:{}] Prepare Certificate: <view: {}, instance {}, "
        "H(proposal): {:016x}>.",
        uat(ids, from), view, instance,
        XXH3_64bits(certificate.message(), certificate.messageSize()));

    // The implementation could not decide locally on what was already
    // checkpointed.
    bool constexpr DontDecideOnCheckpointedInstances = false;
    if constexpr (DontDecideOnCheckpointedInstances) {
      if (unlikely(view < uat(states, local_index).at_view)) {
        return;
      }
    }

    auto data_it = instance_states.find(instance);
    if (unlikely(data_it == instance_states.end())) {
      uat(buffered_commits, from).emplaceBack(std::move(certificate));
      return;
    }

    auto &data = data_it->second;
    if (unlikely(!data.receivedCommit(from))) {
      throw std::logic_error(fmt::format("Should have been detected before."));
    }
  }

  void pollFastCommits() {
    for (auto &&[from, receiver] : hipony::enumerate(fast_commit_receivers)) {
      FastCommitMessage fcm;
      if (auto opt_polled = receiver.poll(&fcm)) {
        LOGGER_DEBUG(logger, "[P2P:{}][Fast Commit] <view: {}, instance: {}>",
                     uat(ids, from), fcm.view, fcm.instance);
        if (unlikely(*opt_polled != sizeof(FastCommitMessage))) {
          throw std::runtime_error("Faulty Fast Commit received.");
        }
        if (unlikely(uat(states, local_index).at_view != fcm.view)) {
          LOGGER_WARN(logger, "Fast Commit received in wrong view.");
        }
        auto data_it = instance_states.find(fcm.instance);
        if (unlikely(data_it == instance_states.end())) {
          LOGGER_WARN(logger,
                      "Fast Commit received before prepare/after decision.");
          // TODO(Antoine): buffer fast commit.
          continue;
        }
        auto &data = data_it->second;
        if (unlikely(!data.receivedFastCommit(from))) {
          throw std::runtime_error("Byzantine behavior, received twice.");
        }
      }
    }
  }

  bool isByzantineCheckpoint(size_t from, Certificate &certificate) {
    // Note: does NOT check the validity of the certificate.
    auto &replica_state = uat(states, from);
    auto const next_instance = static_cast<Instance>(certificate.index());
    if (next_instance <= replica_state.checkpoint.propose_range.low) {
      LOGGER_ERROR(logger, "Byzantine behavior: checkpoints went backwards.");
      return true;
    }

    return false;
  }

  void handleCheckpoint(size_t from, CheckpointMessage &&checkpoint) {
    // Extract the prepare certificate from the commit message.
    auto certificate_ok = checkpoint.tryIntoCertificate(
        std::make_pair(sizeof(Checkpoint), quorum));
    if (auto *const error =
            std::get_if<std::invalid_argument>(&certificate_ok)) {
      throw std::runtime_error(
          fmt::format("Byzantine behavior by {}: malformed certificate: {}",
                      uat(ids, from), error->what()));
    }
    auto certificate = std::move(std::get<Certificate>(certificate_ok));

    if (unlikely(isByzantineCheckpoint(from, certificate))) {
      throw std::logic_error(fmt::format(
          "Byzantine checkpoint received from {}.", uat(ids, from)));
    }

    // Fast Checkpoint verification
    // We don't want to spend time verifying checkpoints on the critical path.
    // We'll thus accept a checkpoint if it matches a checpoint certificate we
    // received. If the signatures are incorrect, others are free to consider
    // him as Byzantine and stop listening to him. We cannot harm by accepting a
    // bad certificate if we built an equivalent good one ourselves.
    // Additionally: we trust ourselves.
    auto const &raw_checkpoint =
        *reinterpret_cast<Checkpoint *>(certificate.message());
    LOGGER_DEBUG(logger,
                 "[CB:{}][Certified Checkpoint] <propose_range: [{}:{}[, ...>",
                 uat(ids, from), raw_checkpoint.propose_range.low,
                 raw_checkpoint.propose_range.high);
    if (from != local_index && certifiedCheckpoint() != raw_checkpoint) {
      LOGGER_WARN(logger,
                  "Didn't have >= checkpoint ([{}, ..) vs [{}, ..)), verifying "
                  "the certificate.",
                  certifiedCheckpoint().propose_range.low,
                  raw_checkpoint.propose_range.low);
      if (!checkpoint_certifier.check(certificate)) {
        throw std::logic_error(fmt::format(
            "Byzantine checkpoint received from {}, invalid certificate.",
            uat(ids, from)));
      }
    }
    auto &replica_state = uat(states, from);
    replica_state.checkpoint = raw_checkpoint;
    auto &commits = replica_state.commits;
    auto const prune_below = raw_checkpoint.propose_range.low;
    while (!commits.empty() && commits.begin()->first < prune_below) {
      commits.erase(commits.begin());
    }
    handleCheckpointCertificate(std::move(certificate));
  }

  void pollCheckpointCertificate() {
    if (auto opt_cert = checkpoint_certifier.pollCertificate()) {
      handleCheckpointCertificate(std::move(*opt_cert));
    }
  }

  /**
   * @brief Handles a CORRECT checkpoint certificate.
   *
   */
  void handleCheckpointCertificate(Certificate &&certificate) {
    // We only care about the certificate if it is the highest we have ever
    // seen.
    auto const &raw_checkpoint =
        *reinterpret_cast<Checkpoint *>(certificate.message());
    if (raw_checkpoint <= certifiedCheckpoint()) {
      // fmt::print("Discarding certificate as we had already a certified
      // checkpoint with <= low: {} vs {}\n", raw_checkpoint.propose_range.low,
      // certifiedCheckpoint().propose_range.low);
      return;
    }

    // We adopt the checkpoint certificate
    checkpoint_certificate = std::move(certificate);

    // No need to manually prune instance_states as it is a tail-map.

    // The broadcast itself is delayed a bit.
  }

  void broadcastCheckpointCertificate(bool const force = false) {
    // We don't want to send certificates too fast.
    // Indeed, we want to give slack for the other side to also build an
    // equivalent certificate so that it doesn't have to check the signatures.

    const auto &to_broadcast = certifiedCheckpoint();

    // We only broadcast new checkpoints.
    if (to_broadcast.propose_range.low <= send_checkpoint_above) {
      return;
    }

    // We give some slack.
    // E.g.,: if window == 200, we broadcast the checkpoint opening [100, 300)
    // when we a decided on 190 (prev.high - 10).
    if (!force && states[local_index].checkpoint.propose_range.high >
                      next_to_decide + 10) {
      // fmt::print("Not gonna broadcast...: {} vs {}\n",
      // states[local_index].checkpoint.propose_range.high, next_to_decide +
      // 10); fmt::print("Will not broadcast checkpoint certificate [{},
      // +window) having to decide on {}\n", to_broadcast.propose_range.low,
      // next_to_decide);
      return;
    }

    // We do broadcast.
    send_checkpoint_above = to_broadcast.propose_range.low;

    auto const checkpoint_size =
        CheckpointMessage::bufferSize(checkpoint_certificate.nbShares());
    auto opt_buffer = checkpoint_buffer_pool.take(checkpoint_size);
    if (unlikely(!opt_buffer)) {
      throw std::logic_error(
          "This buffer should be recycled by the end of the call.");
    }
    auto checkpoint_buffer = std::move(*opt_buffer);
    auto *checkpoint =
        reinterpret_cast<CheckpointMessage::Layout *>(checkpoint_buffer.data());
    checkpoint->kind = MessageKind::Checkpoint;
    std::copy(checkpoint_certificate.rawBuffer().cbegin(),
              checkpoint_certificate.rawBuffer().cend(),
              checkpoint->certificate());
    waitForCbSlack();
    auto cb_msg = cb_broadcaster.broadcast(
        checkpoint_buffer.data(),
        static_cast<tail_cb::Broadcaster::Size>(checkpoint_buffer.size()));
    // We also deliver the message locally.
    handleCbMessage(local_index, std::move(cb_msg));

    // Having sent a new checkpoint, we forget about decided instances.
    // This is required for not running out of memory.
    // As some instances may be under certification, we first drop them.
    prepare_certifier.forgetMessages(internal::pack(
        uat(states, local_index).at_view, to_broadcast.propose_range.low - 1));
    while (!instance_states.empty() &&
           instance_states.begin()->first < to_broadcast.propose_range.low) {
      instance_states.popFront();
    }
  }

  void handleSealView(size_t from, SealViewMessage &&) {
    auto &replica_state = uat(states, from);
    LOGGER_DEBUG(logger, "[CB:{}][SealView]", uat(ids, from));
    LOGGER_DEBUG(logger, "[SealView] Waiting for all commits to be verified...",
                 uat(ids, from));
    while (replica_state.outstanding_commit_verifications != 0) {
      pollVerifiedCommits();
    }
    auto const sealed_view = replica_state.at_view;
    auto &state_certifier = uat(vc_state_certifiers, from);
    state_certifier.forgetMessages(sealed_view);
    auto &vc_state = replica_state.serializeState();
    LOGGER_DEBUG(logger, "[SealView] Serialized view {}: {} commits.",
                 vc_state.view(), vc_state.nbBroadcastCommits());
    auto const next_view = ++replica_state.at_view;
    state_certifier.acknowledge(
        sealed_view, vc_state.rawBuffer().data(),
        vc_state.rawBuffer().data() + vc_state.rawBuffer().size());
    if (local_id == leader(next_view)) {
      // TODO: does it work if there are multiple ongoing view changes?
      if (!ongoing_view_change || ongoing_view_change->view < sealed_view) {
        ongoing_view_change.emplace(sealed_view);
      }
    }
  }

  void pollVcStateCertificates() {
    if (likely(!ongoing_view_change)) {
      return;
    }
    for (auto &&[from, certifier] : hipony::enumerate(vc_state_certifiers)) {
      auto cert = certifier.pollCertificate();
      if (!cert || cert->index() != ongoing_view_change->view) {
        continue;
      }
      ongoing_view_change->vc_state_certificates.try_emplace(uat(ids, from),
                                                             std::move(*cert));
      if (ongoing_view_change->vc_state_certificates.size() != quorum) {
        continue;
      }
      auto const new_view =
          ongoing_view_change->buildNewView(window, max_proposal_size, quorum);
      ongoing_view_change.reset();
      waitForCbSlack();
      auto cb_msg = cb_broadcaster.broadcast(
          new_view.data(),
          static_cast<dory::ubft::tail_cb::Broadcaster::Size>(new_view.size()));
      handleCbMessage(local_index, std::move(cb_msg));

      // Let's iterate over valid values and propose them.
      // We fill gaps with empty proposals.
      to_propose.clear();
      auto &vv = uat(states, local_index).valid_values->second;
      auto const first_instance = certifiedCheckpoint().propose_range.low;
      next_proposal = first_instance;
      proposed = first_instance;  // TODO: semantically, doesn't make sense.
      auto const last_instance =
          vv.empty() ? first_instance : vv.rbegin()->first;
      for (auto i = first_instance; i <= last_instance; i++) {
        auto const vv_it = vv.find(i);
        if (vv_it != vv.end()) {
          auto opt_batch = getSlot(static_cast<Size>(vv_it->second.size()));
          if (!opt_batch) {
            throw std::logic_error("Proposal slots should have been recycled.");
          }
          auto &batch = *opt_batch;
          std::copy(vv_it->second.data(),
                    vv_it->second.data() + vv_it->second.size(), batch.raw());
        } else {
          auto opt_slot = getSlot(0);
        }
      }
      if (first_instance != last_instance) {
        propose();
      }
      break;
    }
  }

  bool isByzantine(size_t from, NewViewMessage &new_view) {
    // Let's first check if the certificates make sense.
    // We can assume that the message is well formed and that each certificate,
    // if correct, has a content that makes sense.
    for (size_t i = 0; i < quorum; i++) {
      auto [proc_id, cert_buffer] =
          new_view.cloneCertificateBuffer(i, window, max_proposal_size, quorum);
      auto certificate_ok = Certificate::tryFrom(std::move(cert_buffer));
      if (auto *error = std::get_if<std::invalid_argument>(&certificate_ok)) {
        LOGGER_ERROR(logger,
                     "Received an invalid NewView from {}: vc state "
                     "certificate #{} is malformed.",
                     uat(ids, from), i);
        return true;
      }
      auto const certificate =
          std::get<certifier::Certificate>(std::move(certificate_ok));
      auto const valid_vc_certificate =
          uat(vc_state_certifiers, uat(indices, proc_id)).check(certificate);
      if (!valid_vc_certificate) {
        LOGGER_ERROR(logger,
                     "Received an invalid NewView from {}: vc state "
                     "certificate #{} is invalid.",
                     uat(ids, from), i);
        return true;
      }
    }
    return false;
  }

  void handleNewView(size_t from, NewViewMessage &&new_view) {
    auto &replica_state = uat(states, from);
    LOGGER_DEBUG(logger, "[CB:{}][NewView]", uat(ids, from));
    if (unlikely(isByzantine(from, new_view))) {
      throw std::logic_error(
          fmt::format("Byzantine new view received from {}.", uat(ids, from)));
    }
    while (uat(states, local_index).at_view < new_view.view()) {
      changeView();
    }

    // We will accept prepare messages starting at the checkpoint.
    replica_state.next_prepare = replica_state.checkpoint.propose_range.low;

    replica_state.valid_values = {
        new_view.view(),
        new_view.validValues(window, max_proposal_size, quorum)};

    for (auto const &[instance, vv] : replica_state.valid_values->second) {
      LOGGER_DEBUG(logger, "Will have to propose {} on instance {}.",
                   vv.stringView(), instance);
    }

    LOGGER_DEBUG(logger, "[CB:{}][NewView] Finished handling", uat(ids, from));
  }

  /**
   * @brief Getter for the checkpoint inside our certificate.
   *
   * @return Checkpoint const&
   */
  Checkpoint const &certifiedCheckpoint() {
    return *reinterpret_cast<Checkpoint const *>(
        checkpoint_certificate.message());
  }

  inline void maybeCertifyCbCheckpoint(size_t const from) {
#if CB_CHECKPOINTS
    if (uat(states, from).next_cb % (cb_broadcaster.getTail() / 2) == 0) {
      certifyCbCheckpoint(from);
    }
#endif
  }

  void certifyCbCheckpoint(size_t const from) {
    auto &replica_state = uat(states, from);
    LOGGER_DEBUG(logger, "[CB-CERTIFIER] Waiting for {}'s commit validation...",
                 uat(ids, from));
    // Note: this is going to cause a hiccup, but only on the slow path.
    while (replica_state.outstanding_commit_verifications != 0) {
      pollVerifiedCommits();
    }
    LOGGER_DEBUG(logger, "[CB-CERTIFIER] Certifying {}'s CBs.", uat(ids, from));
    auto &certifier = uat(cb_checkpoint_certifiers, from);
    certifier.forgetMessages();
    auto const &cb_checkpoint = replica_state.checkpointCb();
    certifier.acknowledge(
        replica_state.next_cb, cb_checkpoint.rawBuffer().data(),
        cb_checkpoint.rawBuffer().data() + cb_checkpoint.rawBuffer().size());
  }

  void pollCbCheckpointCertificate() {
#if CB_CHECKPOINTS
    if (auto certificate =
            uat(cb_checkpoint_certifiers, local_index).pollCertificate()) {
      can_cb_until = certificate->index() + cb_broadcaster.getTail() - 1;
      LOGGER_DEBUG(logger, "[CB-CERTIFIER] Unlocked CB up to {}.",
                   can_cb_until);
      for (auto &sender : cb_checkpoint_senders) {
        auto *const slot = sender.getSlot(
            static_cast<tail_p2p::Size>(certificate->rawBuffer().size()));
        std::copy(certificate->rawBuffer().cbegin(),
                  certificate->rawBuffer().cend(),
                  reinterpret_cast<uint8_t *>(slot));
        sender.send();
      }
    }
#endif
  }

  inline void waitForCbSlack() {
#if CB_CHECKPOINTS
    while (cb_broadcaster.nextIndex() > can_cb_until) {
      // We help others and also ourselves.
      for (auto &certifier : cb_checkpoint_certifiers) {
        certifier.tick();
      }
      pollCbCheckpointCertificate();
      cb_broadcaster.tick();
      for (auto &receiver : cb_receivers) {
        receiver.tick();
      }
      pollCbs();
    }
#endif
  }

  tail_cb::Broadcaster cb_broadcaster;
  std::vector<tail_cb::Receiver> cb_receivers;
  certifier::Certifier prepare_certifier;
  std::vector<tail_p2p::AsyncSender> fast_commit_senders;
  std::vector<tail_p2p::Receiver> fast_commit_receivers;
  std::vector<certifier::Certifier> vc_state_certifiers;
  certifier::Certifier checkpoint_certifier;
  std::vector<certifier::Certifier> cb_checkpoint_certifiers;
  std::vector<tail_p2p::AsyncSender> cb_checkpoint_senders;
  std::vector<tail_p2p::Receiver> cb_checkpoint_receivers;

  ProcId local_id;
  size_t local_index;
  size_t const quorum;

  // Checkpoint updated by the upper level app.
  Checkpoint local_checkpoint;
  // Checkpoint updated when receiving new certificates.
  Certificate checkpoint_certificate;
  Instance send_checkpoint_above = 0;

  size_t const window;
  // Used to make sure we do not cb-broadcast more than the cb window.
  tail_cb::Message::Index can_cb_until;
  size_t const max_proposal_size;

  // Used to convert an index to a printable id.
  std::vector<ProcId> ids;
  // Used for leader election.
  std::vector<ProcId> sorted_ids;
  // Used to convert a portable id to a local index.
  std::map<ProcId, size_t> indices;

  Instance next_proposal = 0;
  Instance proposed = 0;
  Instance next_to_decide = 0;

  Pool proposal_buffer_pool;
  std::deque<Buffer> to_propose;

  bool slow_path_enabled = false;

  Pool commit_buffer_pool;
  Pool checkpoint_buffer_pool;

  // The state of each replica according to what we received so far.
  std::vector<internal::ReplicaState> states;

  // Commits that were received and verified before the prepare message.
  std::vector<TailQueue<Certificate>> buffered_commits;

  // The state of instances received in the current view.
  TailMap<Instance, internal::InstanceState> instance_states;

  // Ongoing view change as a leader
  std::optional<internal::ViewChangeState> ongoing_view_change;

  third_party::sync::MpmcQueue<VerifiedCommit> verified_commits;
  std::vector<TailThreadPool::TaskQueue> commit_verification_task_queues;

  internal::RequestLog request_log;
  LOGGER_DECL_INIT(logger, "Consensus");
};

}  // namespace dory::ubft::consensus
