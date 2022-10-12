#pragma once

#include <dory/ctrl/block.hpp>

#include <dory/conn/rc.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/units.hpp>

#include <dory/rpc/conn/universal-connection.hpp>

#include "common.hpp"
#include "dynamic-connections.hpp"
#include "request.hpp"

#include "../../crypto.hpp"
#include "../../tail-p2p/receiver.hpp"
#include "../../tail-p2p/sender.hpp"
#include "../../types.hpp"

namespace dory::ubft::rpc::internal {
struct ConnectionData {
  ConnectionData(std::string &&memory_region, Receiver &&receiver,
                 Receiver &&sig_receiver, Sender &&sender)
      : memory_region{std::move(memory_region)},
        receiver{std::move(receiver)},
        sig_receiver{std::move(sig_receiver)},
        sender{std::move(sender)} {}

  std::string memory_region;
  Receiver receiver;
  Receiver sig_receiver;
  Sender sender;
};

struct Connection {
  // Use a shared pointer, otherwise the value is not preserved when altering
  // connections. Because the DynamicConnections have two ping-pong set of
  // connections, if the data of this struct is not a pointer, during copying
  // (from ping to pong set of connections) the latest value may be lost.
  std::shared_ptr<bool> active{std::make_shared<bool>(true)};

  std::shared_ptr<ConnectionData> data;
};

using Handler = dory::rpc::conn::UniversalConnectionRpcHandler<ProcId, RpcKind>;

class Manager : public Handler::AbstractManager {
 private:
  using ProcIdType = Handler::AbstractManager::ProcIdType;
  using ConnMap = std::unordered_map<ProcIdType, Connection>;
  using ConnIterator = std::unordered_map<ProcIdType, Connection>::iterator;

 public:
  using DynamicConnections = internal::DynamicConnections<ConnIterator>;

  Manager(ctrl::ControlBlock &cb, ProcId const local_id, size_t const tail,
          size_t const max_send_size, size_t const max_recv_size,
          size_t const max_connections)
      : cb{cb},
        tail{tail},
        max_send_size{max_send_size},
        max_recv_size{max_recv_size},
        max_sig_recv_size{max_recv_size + sizeof(Crypto::Signature)} {
    LOGGER_DEBUG(logger, "Preallocating memory for connections");
    for (size_t i = 0; i < max_connections; i++) {
      std::string const uuid =
          fmt::format("rpc-mngr-p2p-receiver-{}-seq-{}", local_id, i);

      available_memory.push_back(uuid);

      auto uuid_recv = fmt::format("{}-recv", uuid);
      cb.allocateBuffer(uuid_recv, Receiver::bufferSize(tail, max_recv_size),
                        64);
      cb.registerMr(uuid_recv, "standard", uuid_recv, WriteMemoryRights);

      auto uuid_sig_recv = fmt::format("{}-sig-recv", uuid);
      cb.allocateBuffer(uuid_sig_recv,
                        Receiver::bufferSize(tail, max_sig_recv_size), 64);
      cb.registerMr(uuid_sig_recv, "standard", uuid_sig_recv,
                    WriteMemoryRights);

      auto uuid_send = fmt::format("{}-send", uuid);
      cb.allocateBuffer(uuid_send, Sender::bufferSize(tail, max_send_size), 64);
      cb.registerMr(uuid_send, "standard", uuid_send, NoMemoryRights);
      cb.registerCq(uuid_send);
    }
  }

  std::pair<bool, std::string> handleStep1(
      ProcIdType proc_id,
      Handler::AbstractManager::Parser const &parser) override {
    std::istringstream remote_info(parser.connectionInfo());
    std::string rc_recv_info;
    std::string rc_sig_recv_info;
    std::string rc_send_info;
    remote_info >> rc_recv_info;
    remote_info >> rc_sig_recv_info;
    remote_info >> rc_send_info;

    LOGGER_DEBUG(logger, "Process {} sent ReliableConnection info: {}", proc_id,
                 rc_recv_info);

    if (available_memory.empty()) {
      LOGGER_WARN(logger, "I have run out of memory!");
      return std::make_pair(false, "nothing");
    }

    auto memory_uuid = available_memory.back();
    available_memory.pop_back();
    auto uuid_recv = fmt::format("{}-recv", memory_uuid);
    auto uuid_sig_recv = fmt::format("{}-sig-recv", memory_uuid);
    auto uuid_send = fmt::format("{}-send", memory_uuid);

    conn::ReliableConnection rc_recv(cb);
    rc_recv.bindToPd(pd_standard);
    rc_recv.bindToMr(uuid_recv);
    rc_recv.associateWithCq(cq_unused, cq_unused);
    rc_recv.init(WriteMemoryRights);
    rc_recv.connect(conn::RemoteConnection::fromStr(rc_recv_info), proc_id);

    conn::ReliableConnection rc_sig_recv(cb);
    rc_sig_recv.bindToPd(pd_standard);
    rc_sig_recv.bindToMr(uuid_sig_recv);
    rc_sig_recv.associateWithCq(cq_unused, cq_unused);
    rc_sig_recv.init(WriteMemoryRights);
    rc_sig_recv.connect(conn::RemoteConnection::fromStr(rc_sig_recv_info),
                        proc_id);

    // TODO: Do I need to zero memory again?

    conn::ReliableConnection rc_send(cb);
    rc_send.bindToPd(pd_standard);
    rc_send.bindToMr(uuid_send);
    rc_send.associateWithCq(uuid_send, uuid_send);
    rc_send.init(NoMemoryRights);
    rc_send.connect(conn::RemoteConnection::fromStr(rc_send_info), proc_id);

    // Get the serialization info before moving
    auto local_serialized_info = fmt::format(
        "{} {} {}", rc_recv.remoteInfo().serialize(),
        rc_sig_recv.remoteInfo().serialize(), rc_send.remoteInfo().serialize());

    // Store connection
    Connection conn_data;
    conn_data.data = std::make_shared<ConnectionData>(
        std::move(memory_uuid),
        Receiver(tail, max_recv_size, std::move(rc_recv)),
        Receiver(tail, max_sig_recv_size, std::move(rc_sig_recv)),
        Sender(tail, max_send_size, std::move(rc_send)));

    conns.insert({proc_id, conn_data});

    LOGGER_DEBUG(logger, "Replying to process {}", proc_id);
    return std::make_pair(true, local_serialized_info);
  }

  bool handleStep2(ProcIdType proc_id,
                   Handler::AbstractManager::Parser const &parser) override {
    dory::ignore(proc_id);
    dory::ignore(parser);

    dc.alterConnections(conns.begin(), conns.end());
    return true;
  }

  void remove(ProcIdType proc_id) override {
    auto conn_it = conns.find(proc_id);
    if (conn_it != conns.end()) {
      auto &memory_uuid = conn_it->second.data->memory_region;
      available_memory.push_back(std::move(memory_uuid));

      conns.erase(conn_it);
    }
  }

  DynamicConnections &connections() { return dc; }

  std::vector<ProcIdType> collectInactive() override {
    std::vector<ProcIdType> inactive_vec;
    auto *inactive = dc.alterConnections(conns.begin(), conns.end());

    for (auto &[proc_id, c] : *inactive) {
      if (!*c.active) {
        inactive_vec.push_back(proc_id);
      }
    }

    return inactive_vec;
  }

  void markInactive(ProcIdType proc_id) override {
    *conns[proc_id].active = false;
    dc.alterConnections(conns.begin(), conns.end());
  }

 private:
  dory::ctrl::ControlBlock &cb;
  size_t const tail;
  size_t const max_send_size;
  size_t const max_recv_size;
  size_t const max_sig_recv_size;
  std::vector<std::string> available_memory;

  ConnMap conns;
  DynamicConnections dc;

  LOGGER_DECL_INIT(logger, "ConnectionManager");

  static auto constexpr WriteMemoryRights =
      dory::ctrl::ControlBlock::LOCAL_READ |
      dory::ctrl::ControlBlock::LOCAL_WRITE |
      dory::ctrl::ControlBlock::REMOTE_READ |
      dory::ctrl::ControlBlock::REMOTE_WRITE;

  static auto constexpr NoMemoryRights = dory::ctrl::ControlBlock::LOCAL_READ;

  static auto constexpr pd_standard = "standard";
  static auto constexpr cq_unused = "unused";
};
}  // namespace dory::ubft::rpc::internal
