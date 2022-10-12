#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

#include <dory/conn/rc-exchanger.hpp>
#include <dory/conn/rc.hpp>

#include <dory/memstore/store.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include "host-builder.hpp"
#include "reader-builder.hpp"
#include "reader.hpp"
#include "writer-builder.hpp"
#include "writer.hpp"

static auto main_logger = dory::std_out_logger("Init");

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
                        .help("ID of the present process"));

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

  //// Create Memory Regions and QPs ////
  auto &store = dory::memstore::MemoryStore::getInstance();

  int const reader_id = 1;
  int const writer_id = 2;
  int const host_id = 3;

  size_t const nb_registers = 2048;
  size_t const nb_writes = nb_registers << 5;
  size_t const nb_reads = nb_writes << 2;
  size_t const register_size = dory::units::kibibytes(1);

  if (local_id == host_id) {
    dory::ubft::swmr::HostBuilder builder(cb, host_id, writer_id,
                                          {reader_id, writer_id}, "main",
                                          nb_registers, register_size);

    builder.announceQps();
    store.barrier("qp_announced", 3);
    builder.connectQps();
    store.barrier("qp_connected", 3);

    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(100));
    }
  } else if (local_id == writer_id) {
    dory::ubft::swmr::WriterBuilder builder(cb, writer_id, host_id, "main",
                                            nb_registers, register_size, true);

    builder.announceQps();
    store.barrier("qp_announced", 3);
    builder.connectQps();
    store.barrier("qp_connected", 3);

    auto writer = builder.build();

    store.barrier("abstractions_initialized", 2);

    for (size_t i = 0; i < nb_writes; i++) {
      auto reg = i % nb_registers;
      void *buffer = *writer.getSlot(reg);
      std::memset(buffer, static_cast<uint8_t>(i), register_size);
      writer.write(reg, i + 1);
      while (!writer.completed(reg)) {
        writer.tick();
      }
      fmt::print("WRITE {}/{} @{} completed.\n", i + 1, nb_writes, reg);
    }

  } else if (local_id == reader_id) {
    dory::ubft::swmr::ReaderBuilder builder(
        cb, local_id, writer_id, host_id, "main", nb_registers, register_size);

    builder.announceQps();
    store.barrier("qp_announced", 3);
    builder.connectQps();
    store.barrier("qp_connected", 3);

    auto reader = builder.build();
    store.barrier("abstractions_initialized", 2);

    for (size_t i = 0; i < nb_reads; i++) {
      auto reg = i % nb_registers;
      auto const handle = *reader.read(reg);
      auto opt_completion = reader.poll(handle);
      while (!opt_completion) {
        reader.tick();
        opt_completion = reader.poll(handle);
        // fmt::print("Polling for READ completion...\n");
      }
      std::vector<uint8_t> words{
          reinterpret_cast<uint8_t *>(opt_completion->first),
          reinterpret_cast<uint8_t *>(opt_completion->first) +
              std::min(register_size, 10UL)};
      auto const incarnation = opt_completion->second;
      fmt::print("READ {}/{} @{} completed: Incarnation {}, `{}...`\n", i + 1,
                 nb_reads, reg, incarnation, words);
      reader.release(handle);
      // std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(100));
    }
  } else {
    throw std::runtime_error(fmt::format("Unknown id {}.", local_id));
  }

  return 0;
}
