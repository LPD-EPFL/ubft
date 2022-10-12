#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <dory/shared/logger.hpp>

#include "../abstract-handler.hpp"
#include "rpc-parser.hpp"

namespace dory::rpc::conn {

template <typename ProcIdT, typename K>
class UniversalConnectionRpcHandler
    : public AbstractRpcHandler<typename K::Kind> {
 private:
  using EnumKind = typename K::Kind;

 public:
  using Parser = ConnectionRpcHandlerParser<ProcIdT>;

  template <typename I, typename P>
  class AbstractManagerInterface {
   public:
    using ProcIdType = I;
    using Parser = P;

    virtual ~AbstractManagerInterface() = default;

    virtual std::pair<bool, std::string> handleStep1(ProcIdType proc_id,
                                                     Parser const &parser) = 0;

    virtual bool handleStep2(ProcIdType proc_id, Parser const &parser) = 0;

    virtual void remove(ProcIdType proc_id) = 0;

    virtual std::vector<ProcIdType> collectInactive() = 0;

    virtual void markInactive(ProcIdType proc_id) = 0;
  };

  using AbstractManager = AbstractManagerInterface<ProcIdT, Parser>;

  UniversalConnectionRpcHandler(std::unique_ptr<AbstractManager> manager,
                                EnumKind k)
      : manager{std::move(manager)},
        enum_kind{k},
        LOGGER_INIT(logger, K::name(k)) {}

  EnumKind kind() const override { return enum_kind; }

  void feed(uv_stream_t *client, ssize_t nread, char const *buf) {
    // Disconnect connections marked as inactive
    for (auto proc_id : manager->collectInactive()) {
      LOGGER_DEBUG(logger, "Deleting inactive connection for {}", proc_id);
      auto session_inv_it = sessions_inv.find(proc_id);
      if (session_inv_it != sessions_inv.end()) {
        auto ref = session_inv_it->second;
        this->disconnect(reinterpret_cast<uv_stream_t *>(ref));
        LOGGER_DEBUG(logger, "Disconnecting {}", proc_id);
      }
    }

    // Create new parser or get existing one
    auto &connection = sessions[reinterpret_cast<uintptr_t>(client)];
    auto &parser = connection.parser;
    auto &proc_has_id = connection.proc_id;

    if (proc_has_id) {
      LOGGER_DEBUG(logger, "Using parser for client with id: {}",
                   proc_has_id.value());
    } else {
      LOGGER_DEBUG(logger, "Using parser for client with ptr: {}",
                   reinterpret_cast<uintptr_t>(client));
    }

    parser.feed(nread, buf);

    std::optional<typename Parser::Step> has_more;
    while ((has_more = parser.parse())) {
      auto step = has_more.value();
      switch (step) {
        case Parser::Step1: {
          auto proc_id = static_cast<ProcIdT>(parser.clientId());
          proc_has_id = proc_id;
          sessions_inv[proc_id] = reinterpret_cast<uintptr_t>(client);

          LOGGER_DEBUG(logger, "Process {} sent a connection request", proc_id);

          auto [ok, resp] = manager->handleStep1(proc_id, parser);

          if (ok) {
            auto len = static_cast<uint32_t>(resp.size());
            this->write(client, sizeof(len), &len);
            this->write(client, resp.size(), resp.data());
          } else {
            LOGGER_WARN(logger,
                        "Process {} failed at step 1 of the connection request",
                        proc_id);
          }

          break;
        }

        case Parser::Step2: {
          auto proc_id = static_cast<ProcIdT>(parser.clientId());
          LOGGER_DEBUG(logger, "Process {} sent DONE", proc_id);

          bool ok = manager->handleStep2(proc_id, parser);

          std::string reply;
          reply = ok ? "OK" : "NK";
          this->write(client, reply.size(), reply.data());

          break;
        }

        default:
          break;
      }
    }
  }

  void disconnected(uv_stream_t *client) override {
    auto connection_it = sessions.find(reinterpret_cast<uintptr_t>(client));

    if (connection_it != sessions.end()) {
      auto &connection = connection_it->second;
      auto &proc_has_id = connection.proc_id;

      if (proc_has_id) {
        LOGGER_DEBUG(
            logger,
            "Client with id {} disconnected. Destroying its connection data",
            proc_has_id.value());

        sessions_inv.erase(proc_has_id.value());

        manager->markInactive(proc_has_id.value());
        manager->remove(proc_has_id.value());
      } else {
        LOGGER_DEBUG(logger, "Client with ptr {} disconnected",
                     reinterpret_cast<uintptr_t>(client));
      }
    } else {
      LOGGER_DEBUG(logger, "Client with ptr {} disconnected",
                   reinterpret_cast<uintptr_t>(client));
    }

    if (sessions.erase(reinterpret_cast<uintptr_t>(client)) == 0) {
      LOGGER_WARN(logger, "Client {} did not have a session",
                  reinterpret_cast<uintptr_t>(client));
    }
  }

 private:
  struct Connection {
    Parser parser;
    std::optional<ProcIdT> proc_id;
  };

  std::unordered_map<uintptr_t, Connection> sessions;
  std::unordered_map<ProcIdT, uintptr_t> sessions_inv;

  std::unique_ptr<AbstractManager> manager;
  EnumKind enum_kind;

  LOGGER_DECL(logger);
};
}  // namespace dory::rpc::conn
