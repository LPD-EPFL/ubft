#pragma once

#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include "../replicated-swmr/reader-builder.hpp"
#include "../replicated-swmr/writer-builder.hpp"
#include "../tail-p2p/receiver-builder.hpp"
#include "../tail-p2p/sender-builder.hpp"

#include "../builder.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "internal/signature-message.hpp"
#include "message.hpp"
#include "receiver.hpp"

namespace dory::ubft::tail_cb {

class ReceiverBuilder : Builder<Receiver> {
 public:
  ReceiverBuilder(dory::ctrl::ControlBlock &cb, ProcId const local_id,
                  ProcId const broadcaster_id,
                  const std::vector<ProcId> &receivers_ids,
                  const std::vector<ProcId> &hosts_ids,
                  std::string const &identifier,
                  // Receiver constructor params
                  Crypto &crypto, TailThreadPool &thread_pool,
                  size_t const borrowed_messages, size_t const tail,
                  size_t const max_message_size)
      : message_recv_builder{cb,
                             local_id,
                             broadcaster_id,
                             fmt::format("cb-broadcaster-messages-{}",
                                         identifier, tail,
                                         Message::bufferSize(max_message_size)),
                             tail,
                             Message::bufferSize(max_message_size)},
        signature_recv_builder{
            cb,
            local_id,
            broadcaster_id,
            fmt::format("cb-broadcaster-signatures-{}", identifier),
            tail,
            internal::SignatureMessage::BufferSize},
        writer_builder{cb,         local_id, hosts_ids,
                       identifier, tail,     Receiver::RegisterValueSize,
                       true},
        broadcaster_id{broadcaster_id},
        crypto{crypto},
        thread_pool{thread_pool},
        borrowed_messages{borrowed_messages},
        tail{tail},
        max_message_size{max_message_size} {
    for (auto const receiver_id : receivers_ids) {
      if (local_id == receiver_id) {
        continue;
      }

      echo_send_builders.emplace_back(
          cb, local_id, receiver_id, fmt::format("cb-echoes-{}", identifier),
          tail, Receiver::maxEchoSize(max_message_size));
      echo_recv_builders.emplace_back(
          cb, local_id, receiver_id, fmt::format("cb-echoes-{}", identifier),
          tail, Receiver::maxEchoSize(max_message_size));
      reader_builders.emplace_back(cb, local_id, receiver_id, hosts_ids,
                                   identifier, tail,
                                   Receiver::RegisterValueSize);
    }
  }

  void announceQps() override {
    announcing();
    message_recv_builder.announceQps();
    signature_recv_builder.announceQps();
    for (auto &builder : echo_send_builders) {
      builder.announceQps();
    }
    for (auto &builder : echo_recv_builders) {
      builder.announceQps();
    }
    for (auto &builder : reader_builders) {
      builder.announceQps();
    }
    writer_builder.announceQps();
  }

  void connectQps() override {
    connecting();
    message_recv_builder.connectQps();
    signature_recv_builder.connectQps();
    for (auto &builder : echo_send_builders) {
      builder.connectQps();
    }
    for (auto &builder : echo_recv_builders) {
      builder.connectQps();
    }
    for (auto &builder : reader_builders) {
      builder.connectQps();
    }
    writer_builder.connectQps();
  }

  Receiver build() override {
    building();
    std::vector<tail_p2p::Receiver> echo_receivers;
    for (auto &builder : echo_recv_builders) {
      echo_receivers.emplace_back(builder.build());
    }

    std::vector<tail_p2p::Sender> echo_senders;
    for (auto &builder : echo_send_builders) {
      echo_senders.push_back(builder.build());
    }

    std::vector<replicated_swmr::Reader> readers;
    for (auto &builder : reader_builders) {
      readers.push_back(builder.build());
    }

    return Receiver(crypto, thread_pool, broadcaster_id, borrowed_messages,
                    tail, max_message_size, message_recv_builder.build(),
                    signature_recv_builder.build(), std::move(echo_receivers),
                    std::move(echo_senders), std::move(readers),
                    writer_builder.build());
  }

 private:
  tail_p2p::ReceiverBuilder message_recv_builder;
  tail_p2p::ReceiverBuilder signature_recv_builder;
  std::vector<tail_p2p::AsyncSenderBuilder> echo_send_builders;
  std::vector<tail_p2p::ReceiverBuilder> echo_recv_builders;
  std::vector<replicated_swmr::ReaderBuilder> reader_builders;
  replicated_swmr::WriterBuilder writer_builder;
  ProcId const broadcaster_id;
  Crypto &crypto;
  TailThreadPool &thread_pool;
  size_t const borrowed_messages;
  size_t const tail;
  size_t const max_message_size;
};

}  // namespace dory::ubft::tail_cb
