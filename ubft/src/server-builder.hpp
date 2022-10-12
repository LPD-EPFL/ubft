#pragma once

#include <string>

#include <fmt/core.h>

#include <dory/ctrl/block.hpp>

#include <dory/conn/rc-exchanger.hpp>

#include <dory/memstore/store.hpp>

#include "builder.hpp"
#include "server.hpp"
#include "types.hpp"

#include "consensus/consensus-builder.hpp"
#include "rpc/server.hpp"

namespace dory::ubft {

class ServerBuilder : private Builder<Server> {
 public:
  ServerBuilder(ctrl::ControlBlock &cb, ProcId const local_id,
                std::vector<ProcId> const &server_ids,
                std::string const &identifier,

                Crypto &crypto, TailThreadPool &thread_pool,
                size_t const max_request_size, size_t const max_response_size,

                // rpc server specific
                ProcId const min_client_id, ProcId const max_client_id,
                size_t const client_window, size_t const max_rpc_connections,
                size_t const rpc_server_window,

                // consensus specific
                size_t const consensus_window, size_t const cb_tail,
                size_t const max_batch_size)
      : rpc_server{crypto,
                   thread_pool,
                   cb,
                   local_id,
                   fmt::format("ubft-{}", identifier),
                   min_client_id,
                   max_client_id,
                   client_window,
                   max_request_size,
                   max_response_size,
                   max_rpc_connections,
                   rpc_server_window,
                   server_ids},
        consensus_builder{cb,
                          local_id,
                          server_ids,
                          fmt::format("ubft-{}", identifier),
                          crypto,
                          thread_pool,
                          consensus_window,
                          cb_tail,
                          max_request_size,
                          max_batch_size,
                          client_window},
        // ubft::Server arguments
        local_id{local_id},
        server_ids{server_ids},
        max_batch_size{max_batch_size} {}

  void announceQps() override {
    announcing();
    consensus_builder.announceQps();
  }

  void connectQps() override {
    connecting();
    consensus_builder.connectQps();
  }

  Server build() override {
    building();
    return Server(local_id, server_ids, std::move(rpc_server),
                  consensus_builder.build(), max_batch_size);
  }

 private:
  rpc::Server rpc_server;
  consensus::ConsensusBuilder consensus_builder;

  // Arguments
  ProcId const local_id;
  std::vector<ProcId> const server_ids;
  size_t const max_batch_size;
};

}  // namespace dory::ubft
