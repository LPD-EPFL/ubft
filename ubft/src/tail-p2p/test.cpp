#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

// #include <dory/conn/rc-exchanger.hpp>
// #include <dory/conn/rc.hpp>

#include <dory/memstore/store.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>
#include <utility>

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
  int local_id;
  std::vector<int> remote_ids;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .help("ID of the present process"))
      .add_argument(lyra::opt(remote_ids, "remotes")
                        .required()
                        .name("-r")
                        .name("--remote-id")
                        .help("ID of remote process"));

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

  size_t const tail = 512;
  auto constexpr MaxMessageSize = dory::units::kibibytes(1);

  std::vector<SenderBuilder> sender_builders;
  std::vector<dory::ubft::tail_p2p::ReceiverBuilder> receiver_builders;
  for (auto const remote_id : remote_ids) {
    sender_builders.emplace_back(cb, local_id, remote_id, "main", tail,
                                 MaxMessageSize);
    sender_builders.back().announceQps();
    receiver_builders.emplace_back(cb, local_id, remote_id, "main", tail,
                                   MaxMessageSize);
    receiver_builders.back().announceQps();
  }

  store.barrier("qp_announced", 1 + remote_ids.size());

  for (auto &builder : sender_builders) {
    builder.connectQps();
  }
  for (auto &builder : receiver_builders) {
    builder.connectQps();
  }

  store.barrier("qp_connected", 1 + remote_ids.size());

  std::vector<Sender> senders;
  std::vector<dory::ubft::tail_p2p::Receiver> receivers;

  senders.reserve(sender_builders.size());
  for (auto &builder : sender_builders) {
    senders.emplace_back(builder.build());
  }

  receivers.reserve(receiver_builders.size());
  for (auto &builder : receiver_builders) {
    receivers.emplace_back(builder.build());
  }

  store.barrier("abstractions_initialized", 1 + remote_ids.size());

  // Application logic
  size_t const messages_to_send = tail << 10;

  std::vector<size_t> sent(senders.size(), 0);
  while (true) {
    for (size_t i = 0; i < senders.size(); i++) {
      auto &sender = senders.at(i);
      sender.tick();
      if (sent[i] < messages_to_send) {
#ifdef ASYNC_SENDER
        auto *const slot = sender.getSlot(sizeof(uint64_t));
        *reinterpret_cast<uint64_t *>(slot) = sent[i];
#else
        auto const slot = sender.getSlot(sizeof(uint64_t));
        if (!slot) {
          continue;
        }
        *reinterpret_cast<uint64_t *>(*slot) = sent[i];
#endif
        sent[i]++;
        sender.send();
      }
    }
    for (size_t i = 0; i < senders.size(); i++) {
      auto &receiver = receivers.at(i);
      uint64_t received_val;
      auto const polled = receiver.poll(&received_val);
      if (polled) {
        fmt::print("polled {}/{} from {}\n", received_val + 1, messages_to_send,
                   remote_ids[i]);
      }
    }
  }

  return 0;
}
