#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <lyra/lyra.hpp>

#include <dory/ctrl/block.hpp>
#include <dory/ctrl/device.hpp>

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

  int const measurer_id = 1;
  int const responder_id = 2;
  int const host_id = 3;

  cli.add_argument(lyra::help(get_help))
      .add_argument(lyra::opt(local_id, "id")
                        .required()
                        .name("-l")
                        .name("--local-id")
                        .choices(measurer_id, responder_id, host_id)
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

  size_t const pings = 1024;
  size_t const experiments = 32;
  size_t const nb_registers = pings * experiments;
  size_t const register_size = dory::units::kibibytes(1);

  if (local_id == host_id) {
    dory::ubft::swmr::HostBuilder ping_builder(
        cb, host_id, measurer_id, {measurer_id, responder_id}, "ping",
        nb_registers, register_size);
    dory::ubft::swmr::HostBuilder pong_builder(
        cb, host_id, responder_id, {measurer_id, responder_id}, "pong",
        nb_registers, register_size);

    ping_builder.announceQps();
    pong_builder.announceQps();
    store.barrier("qp_announced", 3);
    ping_builder.connectQps();
    pong_builder.connectQps();
    store.barrier("qp_connected", 3);

    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(100));
    }
  } else if (local_id == measurer_id) {
    dory::ubft::swmr::WriterBuilder ping_builder(
        cb, local_id, host_id, "ping", nb_registers, register_size, true);
    dory::ubft::swmr::ReaderBuilder pong_builder(cb, local_id, responder_id,
                                                 host_id, "pong", nb_registers,
                                                 register_size);

    ping_builder.announceQps();
    pong_builder.announceQps();
    store.barrier("qp_announced", 3);
    ping_builder.connectQps();
    pong_builder.connectQps();
    store.barrier("qp_connected", 3);

    auto ping_writer = ping_builder.build();
    auto pong_reader = pong_builder.build();

    store.barrier("abstractions_initialized", 2);

    using Clock = std::chrono::steady_clock;
    for (size_t e = 0; e < experiments; e++) {
      Clock::time_point start = Clock::now();
      for (size_t p = 0; p < pings; p++) {
        auto const write_nb = p + e * pings;
        auto const reg = write_nb % nb_registers;
        ping_writer.getSlot(reg);
        ping_writer.write(reg, write_nb + 1);

        auto ponged = false;
        while (!ponged) {
          auto const read_handle = *pong_reader.read(reg);
          dory::ubft::swmr::Reader::PollResult opt_polled;
          while (!(opt_polled = pong_reader.poll(read_handle))) {
            ping_writer.tick();
            pong_reader.tick();
          }
          pong_reader.release(read_handle);
          if (opt_polled->second == reg + 1) {
            ponged = true;
          }
        }
      }
      std::chrono::nanoseconds duration(Clock::now() - start);
      fmt::print("[Size={}] {} pings in {}, measured one-way latency: {}\n",
                 register_size, pings, duration, duration / pings / 2);
    }
  } else if (local_id == responder_id) {
    dory::ubft::swmr::ReaderBuilder ping_builder(cb, local_id, measurer_id,
                                                 host_id, "ping", nb_registers,
                                                 register_size);
    dory::ubft::swmr::WriterBuilder pong_builder(
        cb, local_id, host_id, "pong", nb_registers, register_size, true);

    ping_builder.announceQps();
    pong_builder.announceQps();
    store.barrier("qp_announced", 3);
    ping_builder.connectQps();
    pong_builder.connectQps();
    store.barrier("qp_connected", 3);

    auto ping_reader = ping_builder.build();
    auto pong_writer = pong_builder.build();

    store.barrier("abstractions_initialized", 2);

    for (size_t write_nb = 0; write_nb < experiments * pings; write_nb++) {
      auto const reg = write_nb % nb_registers;

      auto pinged = false;
      while (!pinged) {
        auto const read_handle = *ping_reader.read(reg);
        dory::ubft::swmr::Reader::PollResult polled;
        while (!(polled = ping_reader.poll(read_handle))) {
          pong_writer.tick();
          ping_reader.tick();
        }
        ping_reader.release(read_handle);
        if (polled->second == reg + 1) {
          pinged = true;
        }
      }

      pong_writer.getSlot(reg);
      pong_writer.write(reg, write_nb + 1);
    }
  }

  return 0;
}
