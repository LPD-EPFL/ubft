#include <array>
#include <chrono>
#include <deque>
#include <string>
#include <memory>
#include <queue>

#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/shared/dynamic-bitset.hpp>
#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include <dory/ubft/types.hpp>
#include <dory/ubft/rpc/client.hpp>
#include <dory/shared/latency.hpp>

#include "app/flip.hpp"
#include "app/memc.hpp"
#include "app/redis.hpp"
#include "app/liquibook.hpp"

static auto main_logger = dory::std_out_logger("Main");

int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;
#ifdef UBFT
  std::vector<dory::ubft::ProcId> server_ids;
#else
  dory::ubft::ProcId server_id;
#endif
  size_t window = 16;
  size_t requests_to_send = 96000;
  std::string app;
  std::string app_config;
  bool check_flip = false;
  bool fast_path = false;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .help("ID of the present process"))
#ifdef UBFT
      .add_argument(lyra::opt(server_ids, "servers")
                        .required()
                        .name("-s")
                        .name("--server-id")
                        .help("IDs of servers"))
#else
      .add_argument(lyra::opt(server_id, "server")
                        .required()
                        .name("-s")
                        .name("--server-id")
                        .help("ID of server"))
#endif
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
                        .help("Clients' window"))
      .add_argument(lyra::opt(requests_to_send, "requests_to_send")
                        .name("-r")
                        .name("--requests_to_send")
                        .help("Requests to send"))
      .add_argument(lyra::opt(fast_path)
                        .name("-f")
                        .name("--fast-path")
                        .help("Do not send signed messages"))
      .add_argument(lyra::opt(check_flip)
                      .name("--check")
                      .help("Check that the responses in the flip application are the inverse of the requests"));

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
  LOGGER_INFO(main_logger, "Running `{}`", app);
  std::unique_ptr<Application> chosen_app;
  if (app == "flip") {
    chosen_app = std::make_unique<Flip>(false, app_config);
  } else if (app == "memc") {
    chosen_app = std::make_unique<Memc>(false, app_config);
  } else if (app == "redis") {
    chosen_app = std::make_unique<Redis>(false, app_config);
  } else if (app == "liquibook") {
    auto liquibook_app = std::make_unique<Liquibook>(false, app_config);
    liquibook_app->setClientId(local_id);
    chosen_app = std::move(liquibook_app);
  } else {
    throw std::runtime_error("Unknown application");
  }

  dory::ubft::rpc::Client rpc_client(crypto, thread_pool, cb, local_id, 
#ifdef UBFT
    server_ids,
#else
    {server_id}, 
#endif
  "app", window, chosen_app->maxRequestSize(), chosen_app->maxResponseSize());

  rpc_client.toggleSlowPath(!fast_path);
  
  dory::ubft::Buffer response(chosen_app->maxResponseSize());

  dory::LatencyProfiler latency_profiler(0);
  std::deque<std::chrono::steady_clock::time_point> request_posted_at;
  std::chrono::steady_clock::time_point proposal_time;

  size_t fulfilled_requests = 0;
  size_t outstanding_requests = 0;

  // Used with the flip application to check the results
  std::queue<std::vector<uint8_t>> check;

  while (fulfilled_requests < requests_to_send) {
    rpc_client.tick();
    while (auto const polled = rpc_client.poll(response.data())) {
      latency_profiler.addMeasurement(std::chrono::steady_clock::now() -
                                      request_posted_at.front());
      request_posted_at.pop_front();
      response.resize(*polled);

      // std::cout << "Response: " << kvstores::buff_repr(response.cbegin(), response.cend()) << std::endl;
      // std::cout << "Response: " << Liquibook::resp_buff_repr(response.cbegin(), response.cend()) << std::endl;

      if (check_flip) {
        auto & original_request = check.front();

        if (*polled != original_request.size()) {
          throw std::runtime_error("Response size was not the expected one!");
        }

        size_t i = original_request.size() - 1;
        for (auto c = response.cbegin(); c != response.cend(); i--, c++) {
          if (original_request[i] != *c) {
            throw std::runtime_error("Response was not the expected one!");
          }
        }
        check.pop();
      }

      fulfilled_requests++;
      outstanding_requests--;
    }
    while (outstanding_requests < window &&
           fulfilled_requests + outstanding_requests < requests_to_send) {
      auto &request = chosen_app->randomRequest();

      // std::cout << "Request: " << kvstores::buff_repr(request.begin(), request.end()) << std::endl;

      if (check_flip) {
        check.push(request);
      }

      auto slot = rpc_client.getSlot(request.size());
      std::copy(request.begin(), request.end(), *slot);
      outstanding_requests++;
      request_posted_at.push_back(std::chrono::steady_clock::now());
      rpc_client.post();
    }
  }
  latency_profiler.report();

  return 0;
}
