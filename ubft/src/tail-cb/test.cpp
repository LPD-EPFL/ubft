#include <chrono>
#include <string>
#include <utility>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/memstore/store.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include "../crypto.hpp"
#include "../replicated-swmr/host-builder.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "broadcaster-builder.hpp"
#include "broadcaster.hpp"
#include "receiver-builder.hpp"
#include "receiver.hpp"

static auto main_logger = dory::std_out_logger("Init");

int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;
  int broadcaster_id;
  std::vector<int> receiver_ids;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .help("ID of the present process"))
      .add_argument(lyra::opt(receiver_ids, "receivers")
                        .required()
                        .name("-r")
                        .name("--receiver-id")
                        .help("IDs of receiving processes"))
      .add_argument(lyra::opt(broadcaster_id, "broadcaster")
                        .required()
                        .name("-b")
                        .name("--broadcaster-id")
                        .help("ID of the broadcasting process"));

  // Parse the program arguments.
  auto result = cli.parse({argc, argv});

  if (get_help) {
    std::cout << cli;
    return 0;
  }

  if (!result) {
    fmt::print(stderr, "Error in command line: {}\n", result.errorMessage());
    return 1;
  }

  //// Initialize the crypto library ////
  auto const all_ids = [&]() {
    auto all_ids = receiver_ids;
    all_ids.emplace_back(broadcaster_id);
    return all_ids;
  }();
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
  cb.registerPd("standard");
  cb.registerCq("unused");

  auto &store = dory::memstore::MemoryStore::getInstance();

  size_t const tail = 128;
  auto constexpr MaxMessageSize = dory::units::kibibytes(1);
  auto const nb_broadcasts = tail << 4;

  // Everyone is a memory host
  std::vector<dory::ubft::replicated_swmr::HostBuilder> host_builders;
  for (auto const &writer_id : receiver_ids) {
    host_builders.emplace_back(
        cb, local_id, writer_id, receiver_ids, "main", tail,
        dory::ubft::tail_cb::Receiver::RegisterValueSize);
    host_builders.back().announceQps();
  }

  if (local_id == broadcaster_id) {
    dory::ubft::tail_cb::BroadcasterBuilder broadcaster_builder(
        cb, local_id, receiver_ids, "main", crypto, thread_pool, 0, tail,
        MaxMessageSize);
    broadcaster_builder.announceQps();
    store.barrier("qp_announced", all_ids.size());
    for (auto &builder : host_builders) {
      builder.connectQps();
    }
    broadcaster_builder.connectQps();
    store.barrier("qp_connected", all_ids.size());
    auto broadcaster = broadcaster_builder.build();
    store.barrier("abstractions_initialized", all_ids.size());

    std::array<std::array<uint8_t, MaxMessageSize>, 4> msgs{
        {{{1, 2, 3}},
         {{4, 8, 15, 16, 23, 42}},
         {{2, 4, 6, 8, 10, 12}},
         {{100, 1, 99, 2}}}};

    for (size_t i = 0; i < nb_broadcasts; i++) {
      auto &msg = msgs[i % msgs.size()];
      broadcaster.broadcast(
          msg.data(),
          static_cast<dory::ubft::tail_cb::Broadcaster::Size>(msg.size()));
      broadcaster.tick();
      fmt::print("broadcast {}/{}\n", i + 1, nb_broadcasts);
    }

    while (true) {
      broadcaster.tick();
    }

  } else if (std::find(receiver_ids.begin(), receiver_ids.end(), local_id) !=
             receiver_ids.end()) {
    auto hosts_ids = receiver_ids;
    hosts_ids.push_back(broadcaster_id);

    dory::ubft::tail_cb::ReceiverBuilder receiver_builder(
        cb, local_id, broadcaster_id, receiver_ids, hosts_ids, "main", crypto,
        thread_pool, 0, tail, MaxMessageSize);
    receiver_builder.announceQps();
    store.barrier("qp_announced", all_ids.size());
    for (auto &builder : host_builders) {
      builder.connectQps();
    }
    receiver_builder.connectQps();
    store.barrier("qp_connected", all_ids.size());
    auto receiver = receiver_builder.build();
    store.barrier("abstractions_initialized", all_ids.size());

    auto to_poll_in_tail = tail;
    while (true) {
      receiver.tick();
      auto polled = receiver.poll();
      if (polled) {
        std::vector<uint8_t> msg(polled->cbegin(), polled->cbegin() + 10);
        auto const in_tail = polled->index() >= nb_broadcasts - tail;
        fmt::print("{}polled {}/{} `{}...` (size = {}) from {}\n",
                   in_tail ? "[TAIL] " : "", polled->index() + 1, nb_broadcasts,
                   msg, polled->size(), broadcaster_id);
        if (in_tail) {
          if (--to_poll_in_tail == 0) {
            fmt::print("TEST PASSED: Polled all messages in the tail!\n");
            while (true) {
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
          }
          if (polled->index() + 1 == nb_broadcasts && to_poll_in_tail != 0) {
            fmt::print(
                "TEST FAILED: Polled the last message ({}/{}) but missing {} "
                "in the tail!\n",
                polled->index() + 1, nb_broadcasts, to_poll_in_tail);
          }
        }
      }
    }

  } else {
    throw std::runtime_error(fmt::format(
        "Id `{}` is neither the broadcaster nor a receiver.", local_id));
  }

  return 0;
}
