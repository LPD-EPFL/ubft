#include <chrono>
#include <csignal>
#include <unistd.h>

#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>
#include <dory/special/proc-mem.hpp>

#include <dory/ubft/server-builder.hpp>

#include "app/flip.hpp"
#include "app/memc.hpp"
#include "app/redis.hpp"
#include "app/liquibook.hpp"


using dory::ubft::rpc::internal::RequestIngress;
using dory::ubft::rpc::internal::RequestStateMachine;

static auto main_logger = dory::std_out_logger("Init");

static void signalHandler(int signum) {
  std::cout << "Process signal (" << signum << ") received." << std::endl;
  auto consumption = dory::special::process_memory_consumption();
  std::cout << "Process memory consumption (in bytes):\n" << consumption.toString() << std::endl;
}

int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;
  std::vector<dory::ubft::ProcId> server_ids;
  size_t client_window = 16;
  bool optimistic_rpc = false;
  bool fast_path = false;
  bool dump_vm_consumption = false;
  size_t consensus_window = 256;
  size_t consensus_cb_tail = 128;
  size_t consensus_batch_size = 16;
  std::string app;
  std::string app_config;

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
      .add_argument(lyra::opt(app, "application")
                        .required()
                        .name("-a")
                        .name("--application")
                        .choices("flip", "memc", "redis", "herd", "liquibook")("Which application to run"))
      .add_argument(lyra::opt(app_config, "app_config")
                        .name("-c")
                        .name("--app-config")
                        .help("App specific config"))
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
                        .name("--consensus-fast-path")
                        .help("Enable consensus' fast path"))
      .add_argument(lyra::opt(dump_vm_consumption)
                        .name("--dump-vm-consumption")
                        .help("Dump the memory consumption"))
      .add_argument(lyra::opt(consensus_window, "consensus_window")
                        .name("-W")
                        .name("--consensus-window")
                        .help("Consensus' window"))
      .add_argument(lyra::opt(consensus_cb_tail, "consensus_cb_tail")
                        .name("-t")
                        .name("--consensus-cb-tail")
                        .help("Consensus' cb tail"))
      .add_argument(lyra::opt(consensus_batch_size, "consensus_batch_size")
                        .name("-b")
                        .name("--consensus-batch-size")
                        .help("Consensus' batch size"));

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

  if (dump_vm_consumption) {
    signal(SIGUSR1, signalHandler);
    std::cout << "PID" << getpid() << "PID" <<std::endl;
  }

  //// Initialize the crypto library ////
  dory::ubft::Crypto crypto(local_id, server_ids);

  //// Initialize the thread pool ////
  dory::ubft::TailThreadPool thread_pool("ubft-pool", 1);

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
  dory::ubft::ProcId const min_client_id = 64;
  dory::ubft::ProcId const max_client_id = 128;
  auto const max_connections =
      static_cast<size_t>(max_client_id - min_client_id + 1);
  size_t const rpc_server_window = 16;
  auto &store = dory::memstore::MemoryStore::getInstance();

  LOGGER_INFO(main_logger, "Running `{}`", app);
  std::unique_ptr<Application> chosen_app;
  if (app == "flip") {
    chosen_app = std::make_unique<Flip>(true, app_config);
  } else if (app == "memc") {
    chosen_app = std::make_unique<Memc>(true, app_config);
  } else if (app == "redis") {
    chosen_app = std::make_unique<Redis>(true, app_config);
  } else if (app == "liquibook") {
    chosen_app = std::make_unique<Liquibook>(true, app_config);
  } else {
    throw std::runtime_error("Unknown application");
  }

  dory::ubft::ServerBuilder server_builder(
      cb, local_id, server_ids, "app", crypto, thread_pool, chosen_app->maxRequestSize(),
      chosen_app->maxResponseSize(), min_client_id, max_client_id, client_window,
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

  std::array<uint8_t, 1> empty_app_state;
  std::vector<uint8_t> response;

  auto const idle = *std::max_element(server_ids.begin(), server_ids.end());

  response.reserve(chosen_app->maxResponseSize());
  while (true) {
    server.tick();
    while (auto polled = server.pollToExecute()) {
      while (unlikely(!fast_path && local_id == idle)) {
        // In case of slow path, the last server doesn't react.
        // We wait here so that the client could connect.
        continue;
      }

      auto &[request, should_checkpoint] = *polled;

      chosen_app->execute(request.payload(), request.size(), response);
      server.executed(request, response.data(), response.size());

      if (should_checkpoint) {
        server.checkpointAppState(empty_app_state.begin(), empty_app_state.end());
      }
    }
  }
}
