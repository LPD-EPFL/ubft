#pragma once

#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include "../builder.hpp"
#include "../certifier/certifier-builder.hpp"
#include "../helpers.hpp"
#include "../replicated-swmr/host-builder.hpp"
#include "../tail-cb/broadcaster-builder.hpp"
#include "../tail-cb/broadcaster.hpp"
#include "../tail-cb/receiver-builder.hpp"
#include "../tail-cb/receiver.hpp"
#include "../tail-p2p/receiver-builder.hpp"
#include "../tail-p2p/sender-builder.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "consensus.hpp"
#include "internal/cb-checkpoint.hpp"
#include "internal/messages.hpp"
#include "internal/replica-state.hpp"
#include "types.hpp"

namespace dory::ubft::consensus {

class ConsensusBuilder : Builder<Consensus> {
 public:
  ConsensusBuilder(ctrl::ControlBlock &cb, ProcId const local_id,
                   std::vector<ProcId> const &replicas,
                   std::string const &identifier,
                   // Consensus constructor param
                   Crypto &crypto, TailThreadPool &thread_pool,
                   size_t const window, size_t const cb_tail,
                   size_t const max_request_size, size_t const max_batch_size,
                   size_t const client_window)
      : local_id{local_id},
        replicas{replicas},
        crypto{crypto},
        thread_pool{thread_pool},
        window{window},
        max_request_size{max_request_size},
        max_batch_size{max_batch_size},
        client_window{client_window},
        max_proposal_size{Batch::bufferSize(max_batch_size, max_request_size)},
        max_cb_message_size{internal::Message::maxBufferSize(
            window, max_proposal_size, replicas.size() / 2 + 1)},
        max_borrowed_cb_messages{
            1 +       // To explain, 1 is probably slack.
            1 +       // Checkpoint certificate
            1 +       // Held in view change
            window +  // Prepare messages held in InstanceStates
            TailThreadPool::TaskQueue::maxOutstanding(
                window, thread_pool) +  // Commits in verification worker
            0 +  // No need to account for certificates in verified_commits as
                 // they are polled in the same loop as the messages are pushed
            window  // Buffered (async) commits
        },
        cb_broadcaster_builder{
            cb,
            local_id,
            without(replicas, local_id),
            fmt::format("consensus-{}-cb-{}", identifier, local_id),
            crypto,
            thread_pool,
            max_borrowed_cb_messages,
            cb_tail,
            max_cb_message_size},
        prepare_certifier_builder{
            cb,       local_id,
            replicas, fmt::format("consensus-{}-prepares", identifier),
            crypto,   thread_pool,
            window,   max_proposal_size},
        checkpoint_certifier_builder{
            cb,       local_id,
            replicas, fmt::format("consensus-{}-checkpoint", identifier),
            crypto,   thread_pool,
            1,        sizeof(ubft::consensus::Checkpoint)} {
    // We need one certifier per replica for its state.
    size_t const max_state_size =
        internal::SerializedState::bufferSize(window, max_proposal_size);
    size_t const max_cb_checkpoint_size = internal::CbCheckpoint::bufferSize(
        window, max_proposal_size, window, max_proposal_size);
    for (auto const replica : move_back(replicas, local_id)) {
      vc_state_certifier_builders.emplace_back(
          cb, local_id, replicas,
          fmt::format("consensus-{}-vc-state-{}", identifier, replica), crypto,
          thread_pool, 1, max_state_size);
      cb_checkpoint_certifier_builders.emplace_back(
          cb, local_id, replicas,
          fmt::format("consensus-{}-cb-checkpoint-{}", identifier, replica),
          crypto, thread_pool, 1, max_cb_checkpoint_size);
    }

    // tail_cb
    // In this version, each replica hosts SWMRs.
    auto hosts = replicas;
    // If we should host SWMRs:
    if (std::find(hosts.begin(), hosts.end(), local_id) != hosts.end()) {
      for (auto const broadcaster : replicas) {
        // We must host SWMR registers for each cb_receiver for each
        // cb_broadcaster
        auto const ns =
            fmt::format("consensus-{}-cb-{}", identifier, broadcaster);
        auto receivers = without(replicas, broadcaster);
        for (auto const writer : receivers) {
          host_builders.emplace_back(cb, local_id, writer, receivers, ns,
                                     cb_tail,
                                     tail_cb::Receiver::RegisterValueSize);
        }
      }
    }
    auto const max_cb_message_size =
        ubft::consensus::internal::Message::maxBufferSize(
            window, max_proposal_size, replicas.size() / 2 + 1);
    for (auto const replica : without(replicas, local_id)) {
      auto const ns = fmt::format("consensus-{}-cb-{}", identifier, replica);
      auto const receivers = without(replicas, replica);
      cb_receiver_builders.emplace_back(
          cb, local_id, replica, receivers, hosts, ns, crypto, thread_pool,
          max_borrowed_cb_messages, cb_tail, max_cb_message_size);
      fast_commit_senders_builders.emplace_back(
          cb, local_id, replica,
          fmt::format("consensus-{}-fast-commit", identifier), window,
          sizeof(internal::FastCommitMessage));
      fast_commit_receivers_builders.emplace_back(
          cb, local_id, replica,
          fmt::format("consensus-{}-fast-commit", identifier), window,
          sizeof(internal::FastCommitMessage));
      cb_checkpoint_senders_builders.emplace_back(
          cb, local_id, replica,
          fmt::format("consensus-{}-cb-checkpoint", identifier), window,
          certifier::Certificate::bufferSize(max_cb_checkpoint_size,
                                             replicas.size() / 2 + 1));
      cb_checkpoint_receivers_builders.emplace_back(
          cb, local_id, replica,
          fmt::format("consensus-{}-cb-checkpoint", identifier), window,
          certifier::Certificate::bufferSize(max_cb_checkpoint_size,
                                             replicas.size() / 2 + 1));
    }
  }

  void announceQps() override {
    announcing();

    for (auto &builder : host_builders) {
      builder.announceQps();
    }

    cb_broadcaster_builder.announceQps();

    for (auto &builder : cb_receiver_builders) {
      builder.announceQps();
    }

    prepare_certifier_builder.announceQps();

    for (auto &builder : fast_commit_senders_builders) {
      builder.announceQps();
    }

    for (auto &builder : fast_commit_receivers_builders) {
      builder.announceQps();
    }

    for (auto &builder : vc_state_certifier_builders) {
      builder.announceQps();
    }

    checkpoint_certifier_builder.announceQps();

    for (auto &builder : cb_checkpoint_certifier_builders) {
      builder.announceQps();
    }

    for (auto &builder : cb_checkpoint_senders_builders) {
      builder.announceQps();
    }

    for (auto &builder : cb_checkpoint_receivers_builders) {
      builder.announceQps();
    }
  }

  void connectQps() override {
    connecting();

    for (auto &builder : host_builders) {
      builder.connectQps();
    }

    cb_broadcaster_builder.connectQps();

    for (auto &builder : cb_receiver_builders) {
      builder.connectQps();
    }

    prepare_certifier_builder.connectQps();

    for (auto &builder : fast_commit_senders_builders) {
      builder.connectQps();
    }

    for (auto &builder : fast_commit_receivers_builders) {
      builder.connectQps();
    }

    for (auto &builder : vc_state_certifier_builders) {
      builder.connectQps();
    }

    checkpoint_certifier_builder.connectQps();

    for (auto &builder : cb_checkpoint_certifier_builders) {
      builder.connectQps();
    }

    for (auto &builder : cb_checkpoint_senders_builders) {
      builder.connectQps();
    }

    for (auto &builder : cb_checkpoint_receivers_builders) {
      builder.connectQps();
    }
  }

  ubft::consensus::Consensus build() override {
    building();

    // Building the CB Receivers
    std::vector<tail_cb::Receiver> cb_receivers;
    for (auto &builder : cb_receiver_builders) {
      cb_receivers.emplace_back(builder.build());
    }

    // Building fast commit senders and receivers
    std::vector<tail_p2p::AsyncSender> fast_commit_senders;
    for (auto &builder : fast_commit_senders_builders) {
      fast_commit_senders.emplace_back(builder.build());
    }
    std::vector<tail_p2p::Receiver> fast_commit_receivers;
    for (auto &builder : fast_commit_receivers_builders) {
      fast_commit_receivers.emplace_back(builder.build());
    }

    // Building state certifiers
    std::vector<ubft::certifier::Certifier> vc_state_certifiers;
    for (auto &builder : vc_state_certifier_builders) {
      vc_state_certifiers.emplace_back(builder.build());
    }

    // Building cb checkpoint certifiers
    std::vector<ubft::certifier::Certifier> cb_checkpoint_certifiers;
    for (auto &builder : cb_checkpoint_certifier_builders) {
      cb_checkpoint_certifiers.emplace_back(builder.build());
    }

    // Building cb checkpoint senders and receivers
    std::vector<tail_p2p::AsyncSender> cb_checkpoint_senders;
    for (auto &builder : cb_checkpoint_senders_builders) {
      cb_checkpoint_senders.emplace_back(builder.build());
    }
    std::vector<tail_p2p::Receiver> cb_checkpoint_receivers;
    for (auto &builder : cb_checkpoint_receivers_builders) {
      cb_checkpoint_receivers.emplace_back(builder.build());
    }

    return Consensus(
        thread_pool, cb_broadcaster_builder.build(), std::move(cb_receivers),
        prepare_certifier_builder.build(), std::move(fast_commit_senders),
        std::move(fast_commit_receivers), std::move(vc_state_certifiers),
        checkpoint_certifier_builder.build(),
        std::move(cb_checkpoint_certifiers), std::move(cb_checkpoint_senders),
        std::move(cb_checkpoint_receivers), crypto.myId(), window,
        max_request_size, max_batch_size, client_window);
  }

 private:
  ProcId const local_id;
  std::vector<ProcId> const replicas;
  Crypto &crypto;
  TailThreadPool &thread_pool;
  size_t const window;
  size_t const max_request_size;
  size_t const max_batch_size;
  size_t const client_window;
  size_t const max_proposal_size;
  size_t const max_cb_message_size;
  size_t const max_borrowed_cb_messages;

  std::vector<replicated_swmr::HostBuilder> host_builders;
  tail_cb::BroadcasterBuilder cb_broadcaster_builder;
  std::vector<tail_cb::ReceiverBuilder> cb_receiver_builders;
  // We need a certifier for prepare messages.
  certifier::CertifierBuilder prepare_certifier_builder;
  // We need on fast commit sender and receiver per replica.
  std::vector<tail_p2p::AsyncSenderBuilder> fast_commit_senders_builders;
  std::vector<tail_p2p::ReceiverBuilder> fast_commit_receivers_builders;
  // We need one certifier for each replica for its vc state.
  std::vector<certifier::CertifierBuilder> vc_state_certifier_builders;
  // We need to certify checkpoints.
  certifier::CertifierBuilder checkpoint_certifier_builder;
  // We need to certify all cb checkpoints.
  std::vector<certifier::CertifierBuilder> cb_checkpoint_certifier_builders;
  // We need to broadcast our cb checkpoint to all.
  std::vector<tail_p2p::AsyncSenderBuilder> cb_checkpoint_senders_builders;
  std::vector<tail_p2p::ReceiverBuilder> cb_checkpoint_receivers_builders;
};

}  // namespace dory::ubft::consensus
