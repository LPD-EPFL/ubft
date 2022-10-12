#pragma once

#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include "../tail-p2p/receiver-builder.hpp"
#include "../tail-p2p/sender-builder.hpp"

#include "../builder.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "certifier.hpp"
#include "internal/share-message.hpp"
#include "types.hpp"

namespace dory::ubft::certifier {

class CertifierBuilder : public Builder<Certifier> {
 public:
  CertifierBuilder(dory::ctrl::ControlBlock &cb, ProcId const local_id,
                   std::vector<ProcId> const &replicas,
                   std::string const &identifier,
                   // Certifier constructor params
                   Crypto &crypto, TailThreadPool &thread_pool,
                   size_t const tail, size_t const max_message_size)
      : crypto{crypto},
        thread_pool{thread_pool},
        tail{tail},
        max_message_size{max_message_size},
        identifier{identifier} {
    for (auto const replica : replicas) {
      if (replica == local_id) {
        continue;
      }
      promise_send_builders.emplace_back(
          cb, local_id, replica,
          fmt::format("certifier-promise-{}", identifier), tail, sizeof(Index));
      promise_recv_builders.emplace_back(
          cb, local_id, replica,
          fmt::format("certifier-promise-{}", identifier), tail, sizeof(Index));
      share_send_builders.emplace_back(
          cb, local_id, replica, fmt::format("certifier-share-{}", identifier),
          tail, internal::ShareMessage::BufferSize);
      share_recv_builders.emplace_back(
          cb, local_id, replica, fmt::format("certifier-share-{}", identifier),
          tail, internal::ShareMessage::BufferSize);
    }
  }

  void announceQps() override {
    announcing();
    for (auto &builder : promise_send_builders) {
      builder.announceQps();
    }
    for (auto &builder : promise_recv_builders) {
      builder.announceQps();
    }
    for (auto &builder : share_send_builders) {
      builder.announceQps();
    }
    for (auto &builder : share_recv_builders) {
      builder.announceQps();
    }
  }

  void connectQps() override {
    connecting();
    for (auto &builder : promise_send_builders) {
      builder.connectQps();
    }
    for (auto &builder : promise_recv_builders) {
      builder.connectQps();
    }
    for (auto &builder : share_send_builders) {
      builder.connectQps();
    }
    for (auto &builder : share_recv_builders) {
      builder.connectQps();
    }
  }

  ubft::certifier::Certifier build() override {
    std::vector<tail_p2p::AsyncSender> promise_senders;
    for (auto &builder : promise_send_builders) {
      promise_senders.emplace_back(builder.build());
    }
    std::vector<tail_p2p::Receiver> promise_receivers;
    for (auto &builder : promise_recv_builders) {
      promise_receivers.emplace_back(builder.build());
    }
    std::vector<tail_p2p::AsyncSender> share_senders;
    for (auto &builder : share_send_builders) {
      share_senders.emplace_back(builder.build());
    }
    std::vector<tail_p2p::Receiver> share_receivers;
    for (auto &builder : share_recv_builders) {
      share_receivers.emplace_back(builder.build());
    }
    return Certifier(crypto, thread_pool, tail, max_message_size, identifier,
                     std::move(promise_senders), std::move(promise_receivers),
                     std::move(share_senders), std::move(share_receivers));
  }

 private:
  Crypto &crypto;
  TailThreadPool &thread_pool;
  size_t const tail;
  size_t const max_message_size;
  std::string identifier;
  std::vector<tail_p2p::AsyncSenderBuilder> promise_send_builders;
  std::vector<tail_p2p::ReceiverBuilder> promise_recv_builders;
  std::vector<tail_p2p::AsyncSenderBuilder> share_send_builders;
  std::vector<tail_p2p::ReceiverBuilder> share_recv_builders;
};

}  // namespace dory::ubft::certifier
