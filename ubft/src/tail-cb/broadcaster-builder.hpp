#pragma once

#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include "../tail-p2p/sender-builder.hpp"

#include "../builder.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "broadcaster.hpp"
#include "internal/signature-message.hpp"
#include "message.hpp"

namespace dory::ubft::tail_cb {

class BroadcasterBuilder : Builder<Broadcaster> {
 public:
  BroadcasterBuilder(dory::ctrl::ControlBlock &cb, ProcId const local_id,
                     std::vector<ProcId> const &receivers_ids,
                     std::string const &identifier,
                     // Broadcaster constructor params
                     Crypto &crypto, TailThreadPool &thread_pool,
                     size_t const borrowed_messages, size_t const tail,
                     size_t const max_message_size)
      : crypto{crypto},
        thread_pool{thread_pool},
        borrowed_messages{borrowed_messages},
        tail{tail},
        max_message_size{max_message_size} {
    for (auto const receiver_id : receivers_ids) {
      message_sender_builders.emplace_back(
          cb, local_id, receiver_id,
          fmt::format("cb-broadcaster-messages-{}", identifier), tail,
          Message::bufferSize(max_message_size));
      signature_sender_builders.emplace_back(
          cb, local_id, receiver_id,
          fmt::format("cb-broadcaster-signatures-{}", identifier), tail,
          internal::SignatureMessage::BufferSize);
    }
  }

  void announceQps() override {
    announcing();
    for (auto &builder : message_sender_builders) {
      builder.announceQps();
    }
    for (auto &builder : signature_sender_builders) {
      builder.announceQps();
    }
  }

  void connectQps() override {
    connecting();
    for (auto &builder : message_sender_builders) {
      builder.connectQps();
    }
    for (auto &builder : signature_sender_builders) {
      builder.connectQps();
    }
  }

  Broadcaster build() override {
    building();
    std::vector<tail_p2p::Sender> message_senders;
    for (auto &builder : message_sender_builders) {
      message_senders.emplace_back(builder.build());
    }
    std::vector<tail_p2p::AsyncSender> signature_senders;
    for (auto &builder : signature_sender_builders) {
      signature_senders.emplace_back(builder.build());
    }
    return Broadcaster(crypto, thread_pool, borrowed_messages, tail,
                       max_message_size, std::move(message_senders),
                       std::move(signature_senders));
  }

 private:
  Crypto &crypto;
  TailThreadPool &thread_pool;
  size_t const borrowed_messages;
  size_t const tail;
  size_t const max_message_size;
  std::vector<tail_p2p::AsyncSenderBuilder> message_sender_builders;
  std::vector<tail_p2p::AsyncSenderBuilder> signature_sender_builders;
};

}  // namespace dory::ubft::tail_cb
