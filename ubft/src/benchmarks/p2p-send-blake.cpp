#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <lyra/lyra.hpp>

#include <dory/crypto/hash/blake2b.hpp>
#include <dory/crypto/hash/blake3.hpp>
#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>
#include <dory/memstore/store.hpp>
#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include "../tail-p2p/receiver-builder.hpp"
#include "../tail-p2p/receiver.hpp"
#include "../tail-p2p/sender-builder.hpp"
#include "../tail-p2p/sender.hpp"

using Sender = dory::ubft::tail_p2p::AsyncSender;

static auto main_logger = dory::std_out_logger("Init");

/**
 * This benchmark compares the latency of:
 * - Sending a raw message over p2p.
 * - Sending the Blake2 hash of a message over p2p.
 * - Sending the Blake3 hash of a message over p2p.
 * - std::copying a message.
 * - Computing the Blake2 hash of a message.
 * - Computing the Blake3 hash of a message.
 *
 * Conclusion: Up to 2KiB, Blake2 only has a ~80ns penalty. Blake3 scales better
 * after. The Blake3 approach is only definitively faster after 8KiB.
 */
int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .choices(1, 2)
                        .help("ID of the present process"));

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

  // auto const Measurer = 1;
  size_t const tail = 512;
  auto constexpr MaxMessageSize = dory::units::kibibytes(16);

  auto const remote_id = 3 - local_id;  // Ids are 1-based.
  dory::ubft::tail_p2p::AsyncSenderBuilder sender_builder(
      cb, local_id, remote_id, "main", tail, MaxMessageSize);
  dory::ubft::tail_p2p::ReceiverBuilder receiver_builder(
      cb, local_id, remote_id, "main", tail, MaxMessageSize);
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

  std::vector<std::vector<uint8_t>> msgs{
      std::vector<uint8_t>(dory::units::bytes(1), 0),
      std::vector<uint8_t>(dory::units::bytes(8), 0),
      std::vector<uint8_t>(dory::units::bytes(16), 0),
      std::vector<uint8_t>(dory::units::bytes(32), 0),
      std::vector<uint8_t>(dory::units::bytes(64), 0),
  };

  for (auto size = dory::units::bytes(128); size <= MaxMessageSize;
       size += dory::units::bytes(128)) {
    msgs.emplace_back(size, 0);
  }

  std::vector<uint8_t> buffer(MaxMessageSize, 0);

  size_t const pings = 1024;
  size_t const hashes = 1024 * 8;

  using Clock = std::chrono::steady_clock;
  using Blake2Hash = dory::crypto::hash::Blake2Hash;
  auto constexpr Blake2HashLength = dory::crypto::hash::Blake2HashLength;
  using Blake3Hash = dory::crypto::hash::Blake3Hash;
  auto constexpr Blake3HashLength = dory::crypto::hash::Blake3HashLength;
  using Size = dory::ubft::tail_p2p::Size;

  enum Hash { None, Blake2, Blake3 };

  fmt::print("Msg size (B)\n");
  for (auto &msg : msgs) {
    fmt::print("{}\n", msg.size());
  }

  for (auto warmed : {false, true}) {
    for (auto hash : {None, Blake2, Blake3}) {
      if (warmed) {
        switch (hash) {
          case Blake2:
            fmt::print("Send Blake2\n");
            break;
          case Blake3:
            fmt::print("Send Blake3\n");
            break;
          default:
            fmt::print("Send raw\n");
        }
      }
      for (auto &msg : msgs) {
        auto start = Clock::now();
        for (size_t p = 0; p < pings; p++) {
          // The responder poll first.
          if (local_id != 1) {
            while (!receiver.poll(buffer.data())) {
              sender.tick();
            }
          }

          if (hash == Blake2) {
            auto constexpr size = static_cast<Size>(Blake2HashLength);
            auto *slot = reinterpret_cast<Blake2Hash *>(sender.getSlot(size));
            *slot = dory::crypto::hash::blake2b(msg);
            sender.send();
          } else if (hash == Blake3) {
            auto constexpr size = static_cast<Size>(Blake3HashLength);
            auto *slot = reinterpret_cast<Blake3Hash *>(sender.getSlot(size));
            *slot = dory::crypto::hash::blake3(msg);
            sender.send();
          } else {
            auto size = static_cast<Size>(msg.size());
            auto *slot = reinterpret_cast<uint8_t *>(sender.getSlot(size));
            std::copy(msg.begin(), msg.end(), slot);
            sender.send();
          }

          // The leaders polls after.
          if (local_id == 1) {
            while (!receiver.poll(buffer.data())) {
              sender.tick();
            }
          }
        }

        if (warmed) {
          std::chrono::nanoseconds duration(Clock::now() - start);
          fmt::print("{}\n", duration / pings / 2);
        }
      }
    }
  }

  for (auto warmed : {false, true}) {
    for (auto hash : {None, Blake2, Blake3}) {
      if (warmed) {
        switch (hash) {
          case Blake2:
            fmt::print("Blake2\n");
            break;
          case Blake3:
            fmt::print("Blake3\n");
            break;
          default:
            fmt::print("std::copy\n");
        }
      }
      for (auto &msg : msgs) {
        auto start = Clock::now();
        for (size_t h = 0; h < hashes; h++) {
          if (hash == Blake2) {
            auto hash = dory::crypto::hash::blake2b(msg);
          } else if (hash == Blake3) {
            auto hash = dory::crypto::hash::blake3(msg);
          } else {
            std::copy(msg.begin(), msg.end(), buffer.begin());
          }
        }
        if (warmed) {
          std::chrono::nanoseconds duration(Clock::now() - start);
          fmt::print("{}\n", duration / hashes);
        }
      }
    }
  }

  return 0;
}
