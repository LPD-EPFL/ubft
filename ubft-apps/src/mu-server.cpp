#include <chrono>

#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include <dory/ubft/types.hpp>
#include <dory/ubft/rpc/server.hpp>

#include <dory/crash-consensus.hpp>

#include "app/flip.hpp"
#include "app/memc.hpp"
#include "app/redis.hpp"
#include "app/liquibook.hpp"

static auto main_logger = dory::std_out_logger("Init");

int main(int argc, char* argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  dory::ubft::ProcId local_id;
  std::vector<dory::ubft::ProcId> all_ids;
  size_t window = 16;
  std::string app;
  std::string app_config;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .help("ID of the present process"))
      .add_argument(lyra::opt(all_ids, "all_ids")
                        .required()
                        .name("-r")
                        .name("--replica")
                        .help("IDs of all replicas"))
      .add_argument(lyra::opt(app, "application")
                        .required()
                        .name("-a")
                        .name("--application")
                        .choices("flip", "memc", "redis", "herd", "liquibook")("Which application to run"))
      .add_argument(lyra::opt(app_config, "app_config")
                        .name("-c")
                        .name("--app-config")
                        .help("App specific config"))
      .add_argument(lyra::opt(window, "window")
                        .name("-w")
                        .name("--window")
                        .help("Clients' window"));

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
  dory::ubft::Crypto crypto(local_id, {});

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
  dory::ubft::ProcId const min_client_id = 64;
  dory::ubft::ProcId const max_client_id = 128;
  size_t const server_window = window;
  auto const max_connections =
      static_cast<size_t>(max_client_id - min_client_id + 1);
  bool const optimistic = true;

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

  dory::ubft::rpc::Server rpc_server(crypto, thread_pool, cb, local_id, "app", min_client_id,
                                     max_client_id, window, chosen_app->maxRequestSize(), 
                                     chosen_app->maxResponseSize(), max_connections,
                                     server_window, {local_id});
  rpc_server.toggleOptimism(optimistic);


  std::vector<int> remote_ids;
  for (auto id : all_ids) {
    if (local_id == id) {
      continue;
    }

    remote_ids.push_back(id);
  }

  if (remote_ids.size() == 0) {
    LOGGER_INFO(main_logger, "Running without replication");

    std::vector<uint8_t> response;
    response.reserve(chosen_app->maxResponseSize());
    while (true) {
      rpc_server.tick();
      if (auto polled_received = rpc_server.pollReceived()) {
          auto& request = polled_received->get();
          chosen_app->execute(request.payload(), request.size(), response);
          rpc_server.executed(request.clientId(), request.id(), response.data(),
                              response.size());
      }
    }

    return 0;
  }

  LOGGER_INFO(main_logger, "Running with replication");
  // Zero outstanding requests means no speculation.
  // For every post the consensus engine waits for a majority of replies.
  int outstanding_req = 0;

  dory::Consensus consensus(local_id, remote_ids, outstanding_req);
  consensus.commitHandler([&chosen_app]([[maybe_unused]] bool leader,
                                      [[maybe_unused]] uint8_t const* const buf,
                                      [[maybe_unused]] size_t len) {
    if (!leader) {
      std::vector<uint8_t> response;
      response.reserve(chosen_app->maxResponseSize());
      chosen_app->execute(buf, len, response);
    }
  });

  LOGGER_INFO(main_logger,
              "Waiting some time to make the consensus engine ready");
  std::this_thread::sleep_for(std::chrono::seconds(5));

  std::vector<uint8_t> response;
  response.reserve(chosen_app->maxResponseSize());
  while (true) {
    rpc_server.tick();
    if (auto polled_received = rpc_server.pollReceived()) {
        auto& request = polled_received->get();

        dory::ProposeError err = consensus.propose(request.payload(), request.size());
        // dory::ProposeError err = dory::ProposeError::NoError;

        if (err != dory::ProposeError::NoError) {
          switch (err) {
            case dory::ProposeError::FastPath:
            case dory::ProposeError::FastPathRecyclingTriggered:
            case dory::ProposeError::SlowPathCatchFUO:
            case dory::ProposeError::SlowPathUpdateFollowers:
            case dory::ProposeError::SlowPathCatchProposal:
            case dory::ProposeError::SlowPathUpdateProposal:
            case dory::ProposeError::SlowPathReadRemoteLogs:
            case dory::ProposeError::SlowPathWriteAdoptedValue:
            case dory::ProposeError::SlowPathWriteNewValue:
              fmt::print("Error: in leader mode. Code: {}\n",
                          static_cast<int>(err));
              break;

            case dory::ProposeError::SlowPathLogRecycled:
              fmt::print("Log recycled, waiting a bit...\n");
              std::this_thread::sleep_for(std::chrono::seconds(1));
              break;

            case dory::ProposeError::MutexUnavailable:
            case dory::ProposeError::FollowerMode:
              fmt::print(
                  "Error! I am in follower mode. Potential leader: {}\n",
                  consensus.potentialLeader());
              break;

            default:
              fmt::print("Bug in code. You should only handle errors here\n");
          }
        } else {
          chosen_app->execute(request.payload(), request.size(), response);
          rpc_server.executed(request.clientId(), request.id(), response.data(),
                              response.size());
        }
    }
  }

  return 0;
}
