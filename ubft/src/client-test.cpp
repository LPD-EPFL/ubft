#include <array>
#include <chrono>
#include <deque>

#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/shared/dynamic-bitset.hpp>
#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include "client.hpp"
#include "rpc/kvstores.hpp"

#include "buffer.hpp"
#include "latency.hpp"
#include "types.hpp"

static auto main_logger = dory::std_out_logger("Main");

int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  int local_id;
  std::vector<dory::ubft::ProcId> server_ids;
  size_t window = 16;
  bool fast_path = false;
  size_t requests_to_send = 96000;

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
      .add_argument(lyra::opt(fast_path)
                        .name("-f")
                        .name("--fast-path")
                        .help("Enable RPC's fast path"))
      .add_argument(lyra::opt(requests_to_send, "requests_to_send")
                        .name("-r")
                        .name("--requests_to_send")
                        .help("Requests to send"));

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
  size_t const max_request_size = 8;
  size_t const max_response_size = 8;

  dory::ubft::Client ubft_client(crypto, thread_pool, cb, local_id, server_ids,
                                 "app", window, max_request_size,
                                 max_response_size);
  ubft_client.toggleSlowPath(!fast_path);

  size_t fulfilled_requests = 0;
  size_t outstanding_requests = 0;
  dory::ubft::Buffer response(max_response_size);

  LatencyProfiler latency_profiler;
  std::deque<std::chrono::steady_clock::time_point> request_posted_at;
  std::chrono::steady_clock::time_point proposal_time;

  std::array<uint8_t, 8> const request = {1, 2, 3, 4, 5, 6, 7, 8};

  while (fulfilled_requests < requests_to_send) {
    ubft_client.tick();
    while (auto const polled = ubft_client.poll(response.data())) {
      latency_profiler.addMeasurement(std::chrono::steady_clock::now() -
                                      request_posted_at.front());
      request_posted_at.pop_front();
      response.resize(*polled);
      // Assume we do something with the response...
      fulfilled_requests++;
      outstanding_requests--;
    }
    while (outstanding_requests < window &&
           fulfilled_requests + outstanding_requests < requests_to_send) {
      auto slot = ubft_client.getSlot(request.size());
      std::copy(request.begin(), request.end(), *slot);
      outstanding_requests++;
      request_posted_at.push_back(std::chrono::steady_clock::now());
      ubft_client.post();
    }
  }
  latency_profiler.reportOnce();

  return 0;
}
