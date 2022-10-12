#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/memstore/store.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>
#include <utility>

#include "../types.hpp"
#include "receiver-builder.hpp"
#include "receiver.hpp"
#include "sender-builder.hpp"
#include "sender.hpp"

#define ASYNC_SENDER

#ifdef ASYNC_SENDER
using Sender = dory::ubft::tail_p2p::AsyncSender;
using SenderBuilder = dory::ubft::tail_p2p::AsyncSenderBuilder;
#else
using Sender = dory::ubft::tail_p2p::SyncSender;
using SenderBuilder = dory::ubft::tail_p2p::SyncSenderBuilder;
#endif

static auto main_logger = dory::std_out_logger("Init");

int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  dory::ubft::ProcId local_id;
  size_t pings = 1024;
  size_t experiments = 1024;
  dory::ubft::tail_p2p::Size message_size = dory::units::bytes(1024);
  size_t tail = 200;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
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
      .add_argument(lyra::opt(tail, "tail")
                        .name("-t")
                        .name("--tail")
                        .help("Tail window"));

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

  auto const remote_id = 3 - local_id;  // Ids are 1-based.
  SenderBuilder sender_builder(cb, local_id, remote_id, "main", tail,
                               message_size);
  dory::ubft::tail_p2p::ReceiverBuilder receiver_builder(
      cb, local_id, remote_id, "main", tail, message_size);
  sender_builder.announceQps();
  receiver_builder.announceQps();

  store.barrier("qp_announced", 2);

  sender_builder.connectQps();
  receiver_builder.connectQps();

  store.barrier("qp_connected", 2);

  auto sender = sender_builder.build();
  auto receiver = receiver_builder.build();

  store.barrier("abstractions_initialized", 2);

  // Application logic
  std::vector<uint8_t> receive_buffer(message_size, 0);

  if (local_id == 1) {
    using Clock = std::chrono::steady_clock;
    for (size_t e = 0; e < experiments; e++) {
      Clock::time_point start = Clock::now();
      for (size_t p = 0; p < pings; p++) {
        auto slot = reinterpret_cast<uint8_t *>(sender.getSlot(message_size));
        *slot = 0;
        sender.send();
        while (!receiver.poll(receive_buffer.data())) {
          sender.tickForCorrectness();
        }
      }
      std::chrono::nanoseconds duration(Clock::now() - start);
      fmt::print("[Size={}] {} pings in {}, measured one-way latency: {}\n",
                 message_size, pings, duration, duration / pings / 2);
    }
    return 0;
  }

  while (true) {
    while (!receiver.poll(receive_buffer.data())) {
      sender.tickForCorrectness();
    }
    auto slot = reinterpret_cast<uint8_t *>(sender.getSlot(message_size));
    *slot = 0;
    sender.send();
  }

  return 0;
}
