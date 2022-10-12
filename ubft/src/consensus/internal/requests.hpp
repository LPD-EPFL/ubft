#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <dory/shared/branching.hpp>
#include <dory/shared/logger.hpp>

#include "../../buffer.hpp"
#include "../../tail-map/tail-map.hpp"
#include "../../types.hpp"

namespace dory::ubft::consensus::internal {

/**
 * @brief Batch of requests received from the leader. *Does NOT own the batch.*
 *
 */
class Batch {
 public:
  /**
   * @brief Individual request inside the batch. *Does NOT own the request.*
   *
   */
  class Request {
   public:
    struct Layout {
      ProcId client_id;
      RequestId id;
      size_t size;
      uint8_t payload; /* Fake field where to store the payload */
    };

    static size_t constexpr bufferSize(size_t const request_size) {
      return offsetof(Layout, payload) + request_size;
    }

    inline Request(Request::Layout& raw_request) : raw_request{raw_request} {}

    inline ProcId const& clientId() const { return raw_request.client_id; }
    inline ProcId& clientId() {
      return const_cast<ProcId&>(std::as_const(*this).clientId());
    }

    inline RequestId const& id() const { return raw_request.id; }
    inline RequestId& id() {
      return const_cast<RequestId&>(std::as_const(*this).id());
    }

    inline size_t const& size() const { return raw_request.size; }
    inline size_t& size() {
      return const_cast<size_t&>(std::as_const(*this).size());
    }

    inline uint8_t const* payload() const { return &raw_request.payload; }
    inline uint8_t* payload() {
      return const_cast<uint8_t*>(std::as_const(*this).payload());
    }

    inline uint8_t const* begin() const { return payload(); }
    inline uint8_t* begin() {
      return const_cast<uint8_t*>(std::as_const(*this).begin());
    }
    inline uint8_t const* end() const { return payload() + size(); }
    inline uint8_t* end() {
      return const_cast<uint8_t*>(std::as_const(*this).end());
    }

    std::string_view stringView() const {
      return std::string_view(reinterpret_cast<char const*>(begin()), size());
    }

   private:
    Request::Layout& raw_request;
  };

  /**
   * @brief Iterator for requests within a batch.
   *
   */
  class Iterator {
   public:
    inline Iterator(Batch const& batch)
        : batch{batch}, end{offsetof(Layout, requests) >= batch.size} {}

    inline Iterator& operator++() {
      offset += Request::bufferSize((**this).size());
      end = offsetof(Layout, requests) + offset >= batch.size;
      return *this;
    }

    inline const Request operator*() const {
      return *reinterpret_cast<Request::Layout*>(&batch.raw_batch.requests +
                                                 offset);
    }

    inline Request operator*() { return *std::as_const(*this); }

    inline bool done() const { return end; }

   private:
    Batch const& batch;
    bool end;
    size_t offset = 0;  // Offset of the current request.
  };

  struct Layout {
    uint8_t requests; /* Fake field where to store the requests */
  };

  static size_t bufferSize(size_t const batch_size, size_t const request_size) {
    return offsetof(Layout, requests) +
           batch_size * Batch::Request::bufferSize(request_size);
  }

  inline Batch(Layout& raw_batch, size_t const size)
      : raw_batch{raw_batch}, size{size} {}

  inline const Iterator requests() const { return Iterator(*this); }

  inline Iterator requests() { return std::as_const(*this).requests(); }

  inline uint8_t* raw() { return reinterpret_cast<uint8_t*>(&raw_batch); }

 private:
  Batch::Layout& raw_batch;

 public:
  size_t const size;
};

/**
 * @brief Store for requests received (potentially indirectly) from one client.
 *
 */
class SingleClientRequests {
 public:
  SingleClientRequests(size_t const window, size_t const max_request_size)
      : window{window}, pool{window + 1, max_request_size}, requests{window} {}

  bool addRequest(RequestId const request_id, uint8_t const* const begin,
                  size_t const size) {
    if (unlikely(!accept_below)) {
      accept_below = request_id + window;
    } else if (unlikely(request_id >= accept_below)) {
      return false;
    }

    accept_below = request_id + window;

    auto opt_buffer = pool.take(size);
    if (unlikely(!opt_buffer)) {
      throw std::logic_error("Should not run out of buffers.");
    }
    std::copy(begin, begin + size, opt_buffer->data());
    return requests.tryEmplace(request_id, std::move(*opt_buffer)).second;
  }

  bool isValid(Batch::Request const& request) const {
    auto const it = requests.find(request.id());
    if (unlikely(it == requests.end())) {
      return false;
    }
    return std::equal(it->second.cbegin(), it->second.cend(), request.begin(),
                      request.end());
  }

  void decided(Batch::Request const& request) {
    accept_below = request.id() + window + 1;
  }

 private:
  size_t window;
  Pool pool;
  TailMap<RequestId, Buffer> requests;
  std::optional<RequestId> accept_below;
};

/**
 * @brief Store for requests received (potentially indirectly) from all clients.
 *
 */
class RequestLog {
 public:
  RequestLog(size_t const client_window, size_t const max_request_size)
      : client_window{client_window}, max_request_size{max_request_size} {}

  /**
   * @brief Control path operation to add a new client.
   *
   * @return true if the client was inserted.
   * @return false if the client already existed.
   */
  bool addClient(ProcId const client_id) {
    if (clientExists(client_id)) {
      return false;
    }
    while (client_requests.size() < static_cast<size_t>(client_id) + 1) {
      client_requests.emplace_back();
    }
    client(client_id).emplace(client_window, max_request_size);
    return true;
  }

  bool addRequest(ProcId const client_id, RequestId const request_id,
                  uint8_t const* const begin, size_t const size) {
    if (unlikely(!clientExists(client_id))) {
      addClient(client_id);
    }
    return client(client_id)->addRequest(request_id, begin, size);
  }

  bool clientExists(ProcId const client_id) const {
    return client_requests.size() >= static_cast<size_t>(client_id) + 1 &&
           client(client_id);
  }

  bool isValid(Batch const& batch) const {
    for (auto it = batch.requests(); !it.done(); ++it) {
      auto const& request = *it;
      if (unlikely(!clientExists(request.clientId()))) {
        LOGGER_WARN(logger, "Client {} does not exist.", request.clientId());
        return false;
      }
      if (unlikely(!client(request.clientId())->isValid(request))) {
        LOGGER_DEBUG(logger, "Request {} not valid for client {}.",
                     request.id(), request.clientId());
        return false;
      }
    }
    return true;
  }

  void decided(Batch const& batch) {
    for (auto it = batch.requests(); !it.done(); ++it) {
      auto const request = *it;
      if (unlikely(!clientExists(request.clientId()))) {
        LOGGER_WARN(logger,
                    "A request was accepted for a client that we didn't know.");
      }
      client(request.clientId())->decided(request);
    }
  }

  inline size_t window() const { return client_window; }

  std::optional<SingleClientRequests>& client(ProcId client_id) {
    return client_requests.at(static_cast<size_t>(client_id));
  }

  std::optional<SingleClientRequests> const& client(ProcId client_id) const {
    return client_requests.at(static_cast<size_t>(client_id));
  }

 private:
  size_t const client_window;
  size_t const max_request_size;
  std::vector<std::optional<SingleClientRequests>>
      client_requests; /* map from clients' ids to clients' requests */
  LOGGER_DECL_INIT(logger, "RequestLog");
};

}  // namespace dory::ubft::consensus::internal
