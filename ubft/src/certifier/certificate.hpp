#pragma once

#include <unordered_set>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <hipony/enumerate.hpp>

#include "../buffer.hpp"
#include "../crypto.hpp"
#include "../message.hpp"
#include "../types.hpp"
#include "types.hpp"

#include <dory/shared/unused-suppressor.hpp>

namespace dory::ubft::certifier {

class Certificate : public Message {
  using Message::Message;

 public:
  struct Header {
    Identifier identifier;
    Index index;
    size_t nb_shares;
  };
  static_assert(sizeof(Header) ==
                    sizeof(Identifier) + sizeof(Index) + sizeof(size_t),
                "The Header struct is not packed. Use "
                "`__attribute__((__packed__))` to pack it");

 private:
  struct Share {
    ProcId emitter;
    Crypto::Signature signature;
  };
  static_assert(sizeof(Share) == sizeof(ProcId) + sizeof(Crypto::Signature),
                "The Share struct is not packed. Use "
                "`__attribute__((__packed__))` to pack it");

 public:
  size_t static constexpr bufferSize(size_t const msg_size,
                                     size_t const nb_shares) {
    return sizeof(Header) + nb_shares * sizeof(Share) + msg_size;
  }

  static std::variant<std::invalid_argument, Certificate> tryFrom(
      Buffer &&buffer) {
    if (buffer.size() < sizeof(Header)) {
      return std::invalid_argument("Buffer is smaller than Header.");
    }
    auto const &header = *reinterpret_cast<Header const *>(buffer.data());
    if (buffer.size() < bufferSize(0, header.nb_shares)) {
      return std::invalid_argument("Buffer too small to hold shares.");
    }
    return Certificate(std::move(buffer));
  }

  /**
   * @brief Construct a new Certificate Message object from a list of signatures
   * and pointers to the message to copy in the (newly allocated) buffer.
   *
   * @param signatures
   * @param msg_size
   * @param msg
   */
  Certificate(Identifier const id, Index const idx,
              const std::vector<std::pair<ProcId, Crypto::Signature const &>>
                  &signatures,
              uint8_t const *const msg_begin, uint8_t const *const msg_end)
      : Message(bufferSize(static_cast<size_t>(msg_end - msg_begin),
                           signatures.size())) {
    identifier() = id;
    index() = idx;
    nbShares() = signatures.size();
    for (auto &&[index, proc_sig] : hipony::enumerate(signatures)) {
      share(index) = {proc_sig.first, proc_sig.second};
    }
    std::copy(msg_begin, msg_end, message());
  }

  Identifier const &identifier() const {
    return reinterpret_cast<Header const *>(rawBuffer().data())->identifier;
  }

  Identifier &identifier() {
    return const_cast<size_t &>(std::as_const(*this).identifier());
  }

  Index const &index() const {
    return reinterpret_cast<Header const *>(rawBuffer().data())->index;
  }

  Index &index() { return const_cast<size_t &>(std::as_const(*this).index()); }

  size_t const &nbShares() const {
    return reinterpret_cast<Header const *>(rawBuffer().data())->nb_shares;
  }

  size_t &nbShares() {
    return const_cast<size_t &>(std::as_const(*this).nbShares());
  }

  Share const &share(size_t const index) const {
    auto const *const first_share =
        reinterpret_cast<Share const *>(rawBuffer().data() + sizeof(Header));
    return *(first_share + index);
  }

  Share &share(size_t const index) {
    return const_cast<Share &>(std::as_const(*this).share(index));
  }

  uint8_t const *message() const {
    // We return the first byte after the last share.
    return reinterpret_cast<uint8_t const *>(&share(nbShares()));
  }

  uint8_t *message() {
    return const_cast<uint8_t *>(std::as_const(*this).message());
  }

  size_t messageSize() const {
    return rawBuffer().size() - bufferSize(0, nbShares());
  }
};

}  // namespace dory::ubft::certifier
