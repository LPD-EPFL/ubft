#pragma once

#include <cstdint>
#include <stdexcept>
#include <variant>

#include <fmt/core.h>

#include <dory/shared/branching.hpp>

#include "../../crypto.hpp"
#include "../../message.hpp"
#include "../../types.hpp"

#include "../../consensus/types.hpp"

namespace dory::ubft::rpc::internal {

class Request : public Message {
 public:
  using Id = RequestId;

  using Layout = consensus::Request::Layout;

  using Message::Message;

  size_t static constexpr bufferSize(size_t const request_size) {
    return consensus::Request::bufferSize(request_size);
  }

  static std::variant<std::invalid_argument, Request> tryFrom(Buffer&& buffer) {
    if (unlikely(buffer.size() < bufferSize(0))) {
      return std::invalid_argument("Buffer too small for a Request!");
    }
    auto request = Request(std::move(buffer));
    if (unlikely(request.rawBuffer().size() != bufferSize(request.size()))) {
      return std::invalid_argument(
          fmt::format("Buffer size does not match: {} vs {}.",
                      request.rawBuffer().size(), bufferSize(request.size())));
    }
    return request;
  }

  inline ProcId const& clientId() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->client_id;
  }
  inline ProcId& clientId() {
    return const_cast<ProcId&>(std::as_const(*this).clientId());
  }

  inline RequestId const& id() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->id;
  }
  inline RequestId& id() {
    return const_cast<RequestId&>(std::as_const(*this).id());
  }

  inline size_t const& size() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->size;
  }
  inline size_t& size() {
    return const_cast<size_t&>(std::as_const(*this).size());
  }

  inline uint8_t const* payload() const {
    return &reinterpret_cast<Layout const*>(rawBuffer().data())->payload;
  }
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
};

class SignedRequest : public Request {
 public:
  using Request::Request;

  size_t static constexpr bufferSize(size_t const request_size) {
    return Request::bufferSize(request_size) + sizeof(Crypto::Signature);
  }

  static std::variant<std::invalid_argument, SignedRequest> tryFrom(
      Buffer&& buffer) {
    if (unlikely(buffer.size() < bufferSize(0))) {
      return std::invalid_argument("Buffer too small for a SignedRequest!");
    }
    auto request = SignedRequest(std::move(buffer));
    if (unlikely(request.rawBuffer().size() != bufferSize(request.size()))) {
      return std::invalid_argument(
          fmt::format("Buffer size does not match: {} vs {}.",
                      request.rawBuffer().size(), bufferSize(request.size())));
    }
    return request;
  }

  inline Crypto::Signature const& signature() const {
    return *reinterpret_cast<Crypto::Signature const*>(end());
  }
  inline Crypto::Signature& signature() {
    return *const_cast<Crypto::Signature*>(&std::as_const(*this).signature());
  }

  std::pair<Request, Crypto::Signature> split() {
    auto sig = signature();
    auto const req_size = Request::bufferSize(size());
    auto req = takeBuffer();
    req.resize(req_size);
    return {std::move(req), sig};
  }
};

// struct RequestSignature {
//   ProcId client_id;
//   RequestId request_id;
//   Crypto::Signature signature;
// };

}  // namespace dory::ubft::rpc::internal
