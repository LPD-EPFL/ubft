#include <chrono>

#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include "rpc/kvstores.hpp"
#include "server-builder.hpp"

using dory::ubft::rpc::internal::RequestIngress;
using dory::ubft::rpc::internal::RequestStateMachine;

static auto main_logger = dory::std_out_logger("Init");

int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;
  std::vector<dory::ubft::ProcId> server_ids;
  size_t client_window = 16;
  bool optimistic_rpc = false;
  bool fast_path = false;
  size_t consensus_window = 256;
  size_t consensus_cb_tail = 128;
  size_t consensus_batch_size = 16;
  size_t max_request_size = 8;
  size_t max_response_size = 8;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .help("ID of the present process"))
      .add_argument(lyra::opt(server_ids, "servers")
                        .required()
                        .name("-s")
                        .name("--server-id")
                        .help("IDs of servers"))
      .add_argument(lyra::opt(client_window, "client_window")
                        .name("-w")
                        .name("--client-window")
                        .help("Clients' window"))
      .add_argument(lyra::opt(optimistic_rpc)
                        .name("-o")
                        .name("--optimistic-rpc")
                        .help("Propose requests without waiting for echoes"))
      .add_argument(lyra::opt(fast_path)
                        .name("-f")
                        .name("--fast-path")
                        .help("Enable RPC+consensus's fast path"))
      .add_argument(lyra::opt(consensus_window, "consensus_window")
                        .name("-W")
                        .name("--consensus-window")
                        .help("Consensus' window"))
      .add_argument(lyra::opt(consensus_cb_tail, "consensus_cb_tail")
                        .name("-c")
                        .name("--consensus-cb-tail")
                        .help("Consensus' cb tail"))
      .add_argument(lyra::opt(consensus_batch_size, "consensus_batch_size")
                        .name("-b")
                        .name("--consensus-batch-size")
                        .help("Consensus' batch size"))
      .add_argument(lyra::opt(max_request_size, "max_request_size")
                        .name("-r")
                        .name("--max-request-size")
                        .help("Maximum request size"))
      .add_argument(lyra::opt(max_request_size, "max_response_size")
                        .name("-R")
                        .name("--max-response-size")
                        .help("Maximum response size"));

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
  dory::ubft::Crypto crypto(local_id, server_ids);

  //// Initialize the thread pool ////
  dory::ubft::TailThreadPool thread_pool("ubft-pool", 3);

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

  //// Create Memory Regions and QPs ////
  cb.registerPd("standard");
  cb.registerCq("unused");

  //// Application logic ////
  size_t const key_size = 16;
  size_t const value_size = 32;
  dory::ubft::ProcId const min_client_id = 64;
  dory::ubft::ProcId const max_client_id = 128;
  auto const max_connections =
      static_cast<size_t>(max_client_id - min_client_id + 1);
  size_t const rpc_server_window = 16;

  auto &store = dory::memstore::MemoryStore::getInstance();

  dory::ubft::ServerBuilder server_builder(
      cb, local_id, server_ids, "app", crypto, thread_pool, max_request_size,
      max_response_size, min_client_id, max_client_id, client_window,
      max_connections, rpc_server_window, consensus_window, consensus_cb_tail,
      consensus_batch_size);

  server_builder.announceQps();
  store.barrier("qp_announced", server_ids.size());

  server_builder.connectQps();
  store.barrier("qp_connected", server_ids.size());

  auto server = server_builder.build();
  store.barrier("abstractions_initialized", server_ids.size());

  server.toggleRpcOptimism(optimistic_rpc);
  server.toggleSlowPath(!fast_path);

  std::array<uint8_t, 8> response = {1, 2, 3, 4, 5, 6, 7, 8};
  std::array<uint8_t, 4> app_state = {'a', 'b', 'c', 'd'};

  auto const idle = *std::max_element(server_ids.begin(), server_ids.end());

  while (true) {
    server.tick();
    while (auto polled = server.pollToExecute()) {
      while (unlikely(!fast_path && local_id == idle)) {
        // In case of slow path, the last server doesn't react.
        // We wait here so that the client could connect.
        continue;
      }
      auto &[request, should_checkpoint] = *polled;
      // Let's assume we processed the request...
      server.executed(request, response.begin(), response.size());
      if (should_checkpoint) {
        server.checkpointAppState(app_state.begin(), app_state.end());
      }
    }
  }
}
