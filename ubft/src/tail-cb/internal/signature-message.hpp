#pragma once

#include "../../buffer.hpp"
#include "../../crypto.hpp"
#include "../../message.hpp"

namespace dory::ubft::tail_cb::internal {

class SignatureMessage : public ubft::Message {
  using Message::Message;
  using Index = size_t;

 public:
  struct BufferLayout {
    Index index;
    Crypto::Signature signature;
  };

  static_assert(sizeof(BufferLayout) ==
                    sizeof(Index) + sizeof(Crypto::Signature),
                "The BufferLayout struct is not packed. Use "
                "`__attribute__((__packed__))` to pack it");

  auto static constexpr BufferSize = sizeof(BufferLayout);

  static std::variant<std::invalid_argument, SignatureMessage> tryFrom(
      Buffer &&buffer) {
    if (buffer.size() != BufferSize) {
      return std::invalid_argument("Buffer is not of size BufferSize.");
    }
    return SignatureMessage(std::move(buffer));
  }

  Index index() const {
    return *reinterpret_cast<Index const *>(rawBuffer().data());
  }

  Crypto::Signature const &signature() const {
    return *reinterpret_cast<Crypto::Signature const *>(
        rawBuffer().data() + offsetof(BufferLayout, signature));
  }
};

}  // namespace dory::ubft::tail_cb::internal
