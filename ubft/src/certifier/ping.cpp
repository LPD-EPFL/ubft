#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/memstore/store.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>
#include <utility>

#include "../buffer.hpp"
#include "../replicated-swmr/host-builder.hpp"
#include "../tail-cb/broadcaster-builder.hpp"
#include "../tail-cb/broadcaster.hpp"
#include "../tail-cb/message.hpp"
#include "../tail-cb/receiver-builder.hpp"
#include "../tail-cb/receiver.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "certifier-builder.hpp"
#include "certifier.hpp"

static auto main_logger = dory::std_out_logger("Init");

int main(int argc, char *argv[]) {
  using Message = dory::ubft::tail_cb::Message;
  using Certificate = dory::ubft::certifier::Certificate;
  using Size = dory::ubft::tail_cb::Broadcaster::Size;

  int const measurer_id = 1;
  int const responder_id = 2;
  int const witness_id = 3;
  std::vector<int> const hosts_ids = {measurer_id, responder_id, witness_id};
  std::vector<int> const &all_ids = hosts_ids;

  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;
  size_t pings = 1024;
  size_t experiments = 16;
  size_t message_size = dory::units::bytes(1024);
  size_t tail = 200;
  bool fast_path = false;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .choices(measurer_id, responder_id, witness_id)
                        .help("ID of the present process"))
      .add_argument(
          lyra::opt(pings, "pings").name("-p").name("--pings").help("Pings"))
      .add_argument(lyra::opt(experiments, "experiments")
                        .name("-e")
                        .name("--experiments")
                        .help("Experiments"))
      .add_argument(lyra::opt(message_size, "message_size")
                        .name("-s")
                        .name("--message_size")
                        .help("Size of messages"))
      .add_argument(
          lyra::opt(tail, "tail").name("-t").name("--tail").help("Tail window"))
      .add_argument(lyra::opt(fast_path)
                        .name("-f")
                        .name("--fast-path")
                        .choices(measurer_id, responder_id, witness_id)
                        .help("Enable the fast path"));

  // Parse the program arguments.
  auto result = cli.parse({argc, argv});

  if (get_help) {
    std::cout << cli;
    return 0;
  }

  if (!result) {
    std::cerr << "Error in command line: " << result.errorMessage()
              << std::endl;
    return 1;
  }

  //// Initialize the crypto library ////
  dory::ubft::Crypto crypto(local_id, all_ids);

  //// Initialize the thread pool ////
  dory::ubft::TailThreadPool thread_pool("main-pool", 1);

  //// Setup RDMA ////
  LOGGER_INFO(main_logger, "Opening RDMA device ...");
  auto open_device = std::move(dory::ctrl::Devices().list().back());
  LOGGER_INFO(main_logger, "Device: {} / {}, {}, {}", open_device.name(),
              open_device.devName(),
              dory::ctrl::OpenDevice::typeStr(open_device.nodeType()),
              dory::ctrl::OpenDevice::typeStr(open_device.transportType()));

  size_t binding_port = 0;
  LOGGER_INFO(main_logger, "Binding to port {} of opened device {}",
              binding_port, open_device.name());
  dory::ctrl::ResolvedPort resolved_port(open_device);
  auto binded = resolved_port.bindTo(binding_port);
  if (!binded) {
    throw std::runtime_error("Couldn't bind the device.");
  }
  LOGGER_INFO(main_logger, "Binded successfully (port_id, port_lid) = ({}, {})",
              +resolved_port.portId(), +resolved_port.portLid());

  LOGGER_INFO(main_logger, "Configuring the control block");
  dory::ctrl::ControlBlock cb(resolved_port);

  // //// Create Memory Regions and QPs ////
  cb.registerPd("standard");
  cb.registerCq("unused");

  auto &store = dory::memstore::MemoryStore::getInstance();

  dory::ubft::Pool buffer_pool(1, Message::bufferSize(message_size));

  using Clock = std::chrono::steady_clock;

  // Everyone is a memory host
  // For the ping, measurer_id broadcasts while responder_id and witness_id are
  // receivers. The latter must thus have their own replicated SWMR.
  std::vector<dory::ubft::replicated_swmr::HostBuilder> ping_host_builders;
  for (auto const writer_id : {responder_id, witness_id}) {
    std::vector<int> const accessors = {responder_id, witness_id};
    ping_host_builders.emplace_back(
        cb, local_id, writer_id, accessors, "ping", tail,
        dory::ubft::tail_cb::Receiver::RegisterValueSize);
    ping_host_builders.back().announceQps();
  }
  // For the pong, the measurer and the responder swap their roles.
  std::vector<dory::ubft::replicated_swmr::HostBuilder> pong_host_builders;
  for (auto const writer_id : {measurer_id, witness_id}) {
    std::vector<int> const accessors = {measurer_id, witness_id};
    pong_host_builders.emplace_back(
        cb, local_id, writer_id, accessors, "pong", tail,
        dory::ubft::tail_cb::Receiver::RegisterValueSize);
    pong_host_builders.back().announceQps();
  }

  // Everyone acknowledges messages.
  dory::ubft::certifier::CertifierBuilder ping_certifier_builder(
      cb, local_id, all_ids, "ping", crypto, thread_pool, tail, message_size);
  ping_certifier_builder.announceQps();
  dory::ubft::certifier::CertifierBuilder pong_certifier_builder(
      cb, local_id, all_ids, "pong", crypto, thread_pool, tail, message_size);
  pong_certifier_builder.announceQps();

  if (local_id == measurer_id) {
    dory::ubft::tail_cb::BroadcasterBuilder ping_broadcaster_builder(
        cb, local_id, {responder_id, witness_id}, "ping", crypto, thread_pool,
        0, tail, message_size);
    dory::ubft::tail_cb::ReceiverBuilder pong_receiver_builder(
        cb, local_id, responder_id, {local_id, witness_id}, hosts_ids, "pong",
        crypto, thread_pool, 0, tail, message_size);

    ping_broadcaster_builder.announceQps();
    pong_receiver_builder.announceQps();
    store.barrier("qp_announced", 3);
    for (auto &builder : ping_host_builders) {
      builder.connectQps();
    }
    for (auto &builder : pong_host_builders) {
      builder.connectQps();
    }
    ping_broadcaster_builder.connectQps();
    pong_receiver_builder.connectQps();
    ping_certifier_builder.connectQps();
    pong_certifier_builder.connectQps();
    store.barrier("qp_connected", 3);
    auto ping_broadcaster = ping_broadcaster_builder.build();
    auto pong_receiver = pong_receiver_builder.build();
    auto ping_certifier = ping_certifier_builder.build();
    auto pong_certifier = pong_certifier_builder.build();
    ping_certifier.toggleSlowPath(!fast_path);
    pong_certifier.toggleSlowPath(!fast_path);
    store.barrier("abstractions_initialized", 3);

    std::optional<Message> opt_polled_msg;
    for (size_t e = 0; e < experiments; e++) {
      size_t nb_ticks = 0;
#undef BETTER_BENCHMARK
#ifdef BETTER_BENCHMARK
      std::chrono::nanoseconds broadcast{};
      std::chrono::nanoseconds broadcaster_ticks{};
      std::chrono::nanoseconds receiver_ticks{};
      std::chrono::nanoseconds polls{};
      std::chrono::nanoseconds poll{};
#endif
      auto const start = Clock::now();
      for (size_t p = 0; p < pings; p++) {
#ifdef BETTER_BENCHMARK
        auto const broadcast_start = Clock::now();
#endif
        auto index = e * pings + p;
        auto buffer = std::move(*buffer_pool.take());
        *reinterpret_cast<Message::Index *>(buffer.data()) = index;
        auto msg_ok = Message::tryFrom(std::move(buffer));
        auto &msg = std::get<Message>(msg_ok);
        ping_broadcaster.broadcast(msg.data(), static_cast<Size>(msg.size()));
        ping_certifier.acknowledge(msg.index(), msg.data(),
                                   msg.data() + msg.size());
#ifdef BETTER_BENCHMARK
        broadcast += Clock::now() - broadcast_start;
#endif
        if (opt_polled_msg) {
          pong_certifier.forgetMessages(opt_polled_msg->index());
          opt_polled_msg.reset();
        }
        while (!opt_polled_msg) {
#ifdef BETTER_BENCHMARK
          nb_ticks++;
          auto const broadcaster_tick_start = Clock::now();
#endif
          ping_broadcaster.tick();
#ifdef BETTER_BENCHMARK
          auto const receiver_tick_start = Clock::now();
#endif
          // TODO(Antoine): benchmark certifiers
          ping_certifier.tick();
          pong_certifier.tick();
          pong_receiver.tick();
#ifdef BETTER_BENCHMARK
          auto const tick_over = Clock::now();
          broadcaster_ticks += receiver_tick_start - broadcaster_tick_start;
          receiver_ticks += tick_over - receiver_tick_start;
#endif
          opt_polled_msg = pong_receiver.poll();
#ifdef BETTER_BENCHMARK
          auto const poll_over = Clock::now();
          polls += poll_over - tick_over;
          if (opt_polled) {
            poll += poll_over - tick_over;
          }
#endif
        }
        pong_certifier.acknowledge(
            opt_polled_msg->index(), opt_polled_msg->data(),
            opt_polled_msg->data() + opt_polled_msg->size());
        if (fast_path) {
          std::optional<Message::Index> opt_polled_promise;
          while (!opt_polled_promise || *opt_polled_promise != index) {
            ping_certifier.tick();
            pong_certifier.tick();
            opt_polled_promise = pong_certifier.pollPromise();
          }
        } else {
          std::optional<Certificate> opt_polled_certificate;
          while (!opt_polled_certificate) {
            ping_certifier.tick();
            pong_certifier.tick();
            opt_polled_certificate = pong_certifier.pollCertificate();
          }
          // fmt::print("CERT WITH {} SHARES RECEIVED.\n",
          //            opt_polled_certificate->nbShares());
          // fmt::print("VALID CERT: {}\n",
          // pong_certifier.check(*opt_polled_certificate)); std::vector
          // msg(opt_polled_certificate->message(),
          //                 opt_polled_certificate->message() +
          //                     opt_polled_certificate->messageSize());
          // fmt::print("CERT CONTENT: {}\n", msg);
        }

        ping_certifier.forgetMessages(index);
      }
      std::chrono::nanoseconds duration(Clock::now() - start);
      fmt::print("[Size={}] {} pings in {}, measured one-way latency: {}\n",
                 message_size, pings, duration, duration / pings / 2);
#ifdef BETTER_BENCHMARK
      fmt::print(
          "{} ticks per ping, one bcst tick: {}, one recv tick: {}, one poll: "
          "{}\n",
          nb_ticks / pings, broadcaster_ticks / nb_ticks,
          receiver_ticks / nb_ticks, polls / nb_ticks);
      fmt::print("one bcst: {}, one final poll: {}\n", broadcast / pings,
                 poll / pings);
#endif
    }
    if (opt_polled_msg) {
      pong_certifier.forgetMessages(opt_polled_msg->index());
    }
    fmt::print("done.\n");
  } else if (local_id == responder_id) {
    dory::ubft::tail_cb::ReceiverBuilder ping_receiver_builder(
        cb, local_id, measurer_id, {local_id, witness_id}, hosts_ids, "ping",
        crypto, thread_pool, 0, tail, message_size);
    dory::ubft::tail_cb::BroadcasterBuilder pong_broadcaster_builder(
        cb, local_id, {measurer_id, witness_id}, "pong", crypto, thread_pool, 0,
        tail, message_size);

    ping_receiver_builder.announceQps();
    pong_broadcaster_builder.announceQps();
    store.barrier("qp_announced", 3);
    for (auto &builder : ping_host_builders) {
      builder.connectQps();
    }
    for (auto &builder : pong_host_builders) {
      builder.connectQps();
    }
    ping_receiver_builder.connectQps();
    pong_broadcaster_builder.connectQps();
    ping_certifier_builder.connectQps();
    pong_certifier_builder.connectQps();
    store.barrier("qp_connected", 3);
    auto ping_receiver = ping_receiver_builder.build();
    auto pong_broadcaster = pong_broadcaster_builder.build();
    auto ping_certifier = ping_certifier_builder.build();
    auto pong_certifier = pong_certifier_builder.build();
    ping_certifier.toggleSlowPath(!fast_path);
    pong_certifier.toggleSlowPath(!fast_path);
    store.barrier("abstractions_initialized", 3);

    std::optional<Message> outstanding_msg;
    std::optional<Message> opt_polled_msg;
    for (size_t i = 0; i < experiments * pings; i++) {
      while (!opt_polled_msg) {
        ping_receiver.tick();
        pong_broadcaster.tick();
        ping_certifier.tick();
        pong_certifier.tick();
        opt_polled_msg = ping_receiver.poll();
      }

      if (outstanding_msg) {
        pong_certifier.forgetMessages(outstanding_msg->index());
        outstanding_msg.reset();
      }

      ping_certifier.acknowledge(
          opt_polled_msg->index(), opt_polled_msg->data(),
          opt_polled_msg->data() + opt_polled_msg->size());
      if (fast_path) {
        std::optional<Message::Index> opt_polled_promise;
        while (!opt_polled_promise || *opt_polled_promise != i) {
          ping_certifier.tick();
          pong_certifier.tick();
          opt_polled_promise = ping_certifier.pollPromise();
        }
      } else {
        std::optional<Certificate> opt_polled_certificate;
        while (!opt_polled_certificate) {
          ping_certifier.tick();
          pong_certifier.tick();
          opt_polled_certificate = ping_certifier.pollCertificate();
        }
      }

      opt_polled_msg.reset();
      pong_certifier.forgetMessages(i);
      auto buffer = std::move(*buffer_pool.take());
      *reinterpret_cast<Message::Index *>(buffer.data()) = i;
      auto msg_ok = Message::tryFrom(std::move(buffer));
      outstanding_msg.emplace(std::move(std::get<Message>(msg_ok)));
      pong_broadcaster.broadcast(outstanding_msg->data(),
                                 static_cast<Size>(outstanding_msg->size()));
      pong_certifier.acknowledge(
          outstanding_msg->index(), outstanding_msg->data(),
          outstanding_msg->data() + outstanding_msg->size());
    }
    for (auto i = 0; i < 100000; i++) {
      pong_broadcaster.tick();
      ping_certifier.tick();
      pong_certifier.tick();
    }
    if (outstanding_msg) {
      pong_certifier.forgetMessages(outstanding_msg->index());
    }
    if (opt_polled_msg) {
      ping_certifier.forgetMessages(opt_polled_msg->index());
    }
    fmt::print("done.\n");
  } else if (local_id == witness_id) {
    dory::ubft::tail_cb::ReceiverBuilder ping_receiver_builder(
        cb, local_id, measurer_id, {local_id, responder_id}, hosts_ids, "ping",
        crypto, thread_pool, 0, tail, message_size);
    dory::ubft::tail_cb::ReceiverBuilder pong_receiver_builder(
        cb, local_id, responder_id, {local_id, measurer_id}, hosts_ids, "pong",
        crypto, thread_pool, 0, tail, message_size);

    ping_receiver_builder.announceQps();
    pong_receiver_builder.announceQps();
    store.barrier("qp_announced", 3);
    for (auto &builder : ping_host_builders) {
      builder.connectQps();
    }
    for (auto &builder : pong_host_builders) {
      builder.connectQps();
    }
    ping_receiver_builder.connectQps();
    pong_receiver_builder.connectQps();
    ping_certifier_builder.connectQps();
    pong_certifier_builder.connectQps();
    store.barrier("qp_connected", 3);
    auto ping_receiver = ping_receiver_builder.build();
    auto pong_receiver = pong_receiver_builder.build();
    auto ping_certifier = ping_certifier_builder.build();
    auto pong_certifier = pong_certifier_builder.build();
    ping_certifier.toggleSlowPath(!fast_path);
    pong_certifier.toggleSlowPath(!fast_path);
    store.barrier("abstractions_initialized", 3);

    std::optional<Message> ping_polled_msg;
    std::optional<Message> pong_polled_msg;
    while (true) {
      while (!ping_polled_msg) {
        ping_receiver.tick();
        pong_receiver.tick();
        ping_certifier.tick();
        pong_certifier.tick();
        ping_polled_msg = ping_receiver.poll();
      }
      ping_certifier.acknowledge(
          ping_polled_msg->index(), ping_polled_msg->data(),
          ping_polled_msg->data() + ping_polled_msg->size());
      if (pong_polled_msg) {
        pong_certifier.forgetMessages(pong_polled_msg->index());
        pong_polled_msg.reset();
      }
      while (!pong_polled_msg) {
        ping_receiver.tick();
        pong_receiver.tick();
        ping_certifier.tick();
        pong_certifier.tick();
        pong_polled_msg = pong_receiver.poll();
      }
      pong_certifier.acknowledge(
          pong_polled_msg->index(), pong_polled_msg->data(),
          pong_polled_msg->data() + pong_polled_msg->size());
      if (ping_polled_msg) {
        ping_certifier.forgetMessages(ping_polled_msg->index());
        ping_polled_msg.reset();
      }
    }
  }

  return 0;
}
