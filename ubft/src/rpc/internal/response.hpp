#pragma once

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <variant>

#include <dory/shared/branching.hpp>

#include "../../crypto.hpp"
#include "../../message.hpp"

#include "request.hpp"

namespace dory::ubft::rpc::internal {

class Response : public Message {
  using Message::Message;

 public:
  struct Layout {
    Request::Id request_id;
    uint8_t response;  // Fake field
  };

  size_t static constexpr bufferSize(size_t const response_size) {
    return offsetof(Layout, response) + response_size;
  }

  static std::variant<std::invalid_argument, Response> tryFrom(
      Buffer&& buffer) {
    if (unlikely(buffer.size() < bufferSize(0))) {
      return std::invalid_argument("Buffer too small!");
    }
    return Response(std::move(buffer));
  }

  Request::Id const& requestId() const {
    return reinterpret_cast<Layout const*>(rawBuffer().data())->request_id;
  }

  uint8_t const* begin() const {
    return &reinterpret_cast<Layout const*>(rawBuffer().data())->response;
  }

  uint8_t const* end() const { return begin() + size(); }

  size_t size() const {
    return rawBuffer().size() - offsetof(Layout, response);
  }

  std::string_view stringView() const {
    return std::string_view(reinterpret_cast<char const*>(begin()), size());
  }
};

}  // namespace dory::ubft::rpc::internal
