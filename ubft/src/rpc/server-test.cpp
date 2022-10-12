#include <chrono>

#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include "kvstores.hpp"
#include "server.hpp"

using dory::ubft::rpc::internal::RequestIngress;
using dory::ubft::rpc::internal::RequestStateMachine;

static auto main_logger = dory::std_out_logger("Init");

int main(int argc, char* argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;
  std::vector<dory::ubft::ProcId> server_ids;
  size_t window = 16;
  bool optimistic = false;
  bool fast_path = false;

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
      .add_argument(lyra::opt(window, "window")
                        .name("-w")
                        .name("--window")
                        .help("Clients' window"))
      .add_argument(lyra::opt(optimistic)
                        .name("-o")
                        .name("--optimistic")
                        .help("Reply to the client without echoes."))
      .add_argument(lyra::opt(fast_path)
                        .name("-f")
                        .name("--fast-path")
                        .help("Do not send signed messages"));

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
  size_t const server_window = 16;
  auto const max_request_size =
      dory::ubft::kvstores::memcached::put_max_buffer_size(key_size,
                                                           value_size);
  size_t const max_response_size = 1024;  // TODO: FIX

  dory::ubft::rpc::Server rpc_server(
      crypto, thread_pool, cb, local_id, "app", min_client_id, max_client_id,
      window, max_request_size, max_response_size, max_connections,
      server_window, server_ids);
  rpc_server.toggleSlowPath(!fast_path);
  rpc_server.toggleOptimism(optimistic);

  std::array<uint8_t, 4> response = {'a', 'b', 'c', 'd'};
  auto const leader = *std::min_element(server_ids.begin(), server_ids.end());
  auto const idle = *std::max_element(server_ids.begin(), server_ids.end());
  while (true) {
    rpc_server.tick();
    // We fake consensus:
    //  - followers reply as soon as they receive the request,
    //  - the leader replies when the request is proposable to consensus.
    if (leader != local_id) {
      if (auto polled = rpc_server.pollReceived()) {
        auto& request = polled->get();
        rpc_server.executed(request.clientId(), request.id(), response.data(),
                            response.size());
        // fmt::print("Executed #{}\n", request.id());
      }
    } else {
      rpc_server.pollReceived();  // pollProposable requires first pollReceived.
      if (auto polled = rpc_server.pollProposable()) {
        auto& request = polled->get();
        rpc_server.executed(request.clientId(), request.id(), response.data(),
                            response.size());
        // fmt::print("Executed #{} (leader)\n", request.id());
      }
    }
  }
}
