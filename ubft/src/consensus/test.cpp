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
#include <dory/shared/pinning.hpp>
#include <dory/shared/units.hpp>

#include "../tail-cb/broadcaster.hpp"
#include "../tail-cb/receiver.hpp"

#include "../crypto.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"

#include "../consensus/consensus-builder.hpp"
#include "../consensus/consensus.hpp"

static auto main_logger = dory::std_out_logger("Init");

int main(int argc, char *argv[]) {
  //// Parse Arguments ////
  lyra::cli cli;
  bool get_help = false;
  dory::ubft::ProcId local_id;
  std::vector<dory::ubft::ProcId> all_ids;
  size_t nb_proposals = 16;
  size_t request_size = dory::units::bytes(128);
  size_t batch_size = 1;
  size_t client_window = 10;
  size_t window = 200;
  size_t cb_tail = 128;
  std::optional<int> pinned_core_id;
  size_t tp_threads = 1;
  std::vector<int> pinned_tp_core_ids;
  bool fast_path = false;
  size_t credits = 1;
  std::optional<size_t> crash_at;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .help("ID of the present process"))
      .add_argument(lyra::opt(all_ids, "all_ids")
                        .required()
                        .name("-a")
                        .name("--all-ids")
                        .help("IDs of all processes"))
      .add_argument(lyra::opt(nb_proposals, "proposals")
                        .name("-p")
                        .name("--nb-proposals")
                        .help("Number of proposals to issue"))
      .add_argument(lyra::opt(request_size, "request_size")
                        .name("-s")
                        .name("--request_size")
                        .help("Size of requests"))
      .add_argument(lyra::opt(batch_size, "batch_size")
                        .name("-b")
                        .name("--batch_size")
                        .help("Number of requests in a batch"))
      .add_argument(lyra::opt(client_window, "client_window")
                        .name("-W")
                        .name("--client_window")
                        .help("Number of outstanding requests per client"))
      .add_argument(lyra::opt(window, "window")
                        .name("-w")
                        .name("--window")
                        .help("Window of instances between each checkpoint"))
      .add_argument(lyra::opt(cb_tail, "tail")
                        .name("-t")
                        .name("--cb-tail")
                        .help("Consistent Broadcast tail"))
      .add_argument(lyra::opt(pinned_core_id, "pinned_core_id")
                        .name("-c")
                        .name("--core")
                        .help("Id of the core to pin the application to"))
      .add_argument(lyra::opt(tp_threads, "tp_threads")
                        .name("-x")
                        .name("--tp-threads")
                        .help("Nb of thread pool threads"))
      .add_argument(lyra::opt(pinned_tp_core_ids, "pinned_tp_core_ids")
                        .name("-X")
                        .name("--tp-core")
                        .help("Ids of the cores to pin the thread pool to"))
      .add_argument(lyra::opt(fast_path)
                        .name("-f")
                        .name("--fast_path")
                        .help("Enable the fast path"))
      .add_argument(lyra::opt(credits, "credits")
                        .name("-C")
                        .name("--credits")
                        .help("Number of oustanding requests"))
      .add_argument(lyra::opt(crash_at, "crash_at")
                        .name("-F")
                        .name("--crash-at")
                        .help("Number of decisions before leader crash"));

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

  //// Pinning to the isolated core ////
  if (pinned_core_id) {
    LOGGER_INFO(main_logger, "Pinning the main thread to core {}",
                *pinned_core_id);
    dory::pin_main_to_core(*pinned_core_id);
  }

  //// Initialize the crypto library ////
  dory::ubft::Crypto crypto(local_id, all_ids);

  //// Initialize the thread pool ////
  dory::ubft::TailThreadPool thread_pool("consensus-pool", tp_threads,
                                         pinned_tp_core_ids);

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

  dory::ubft::consensus::ConsensusBuilder consensus_builder(
      cb, local_id, all_ids, "main", crypto, thread_pool, window, cb_tail,
      request_size, batch_size, client_window);

  consensus_builder.announceQps();
  store.barrier("qp_announced", all_ids.size());

  consensus_builder.connectQps();
  store.barrier("qp_connected", all_ids.size());

  auto consensus = consensus_builder.build();
  store.barrier("abstractions_initialized", all_ids.size());

  consensus.testApp(nb_proposals, request_size, batch_size, fast_path, credits,
                    crash_at);
  return 0;
}
