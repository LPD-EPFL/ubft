#pragma once

#include <fmt/core.h>

#include <stdexcept>
#include <string_view>
#include <variant>

#include "../../certifier/certificate.hpp"
#include "../../tail-cb/receiver.hpp"
#include "../types.hpp"
#include "serialized-state.hpp"

/**
 * @brief Cast a variant of a subset to a variant of a superset
 *
 * @tparam SuperV
 * @tparam SubV
 * @param v
 * @return SuperV
 */
template <typename SuperV, typename SubV>
SuperV variant_cast(SubV &&v) {
  return std::visit(
      [](auto &&arg) noexcept -> SuperV {
        return std::forward<decltype(arg)>(arg);
      },
      std::forward<SubV>(v));
}

namespace dory::ubft::consensus::internal {

enum MessageKind : uint8_t {
  Prepare = 1,
  Commit,
  Checkpoint,
  SealView,
  ViewChange,
  NewView
};

/**
 * @brief Base class for all messages received from CB.
 *
 */
struct CbMessage {
 public:
  tail_cb::Message::Index index() const { return msg.index(); }

 protected:
  CbMessage(tail_cb::Message &&msg) : msg{std::move(msg)} {}

  tail_cb::Message msg;
};

struct PrepareMessage : public CbMessage {
  struct Layout {
    MessageKind kind = Prepare;  // Must always be the first field
    View view;
    Instance instance;

    uint8_t *data() { return &data_addr; }
    uint8_t const *data() const { return &data_addr; }

    uint8_t data_addr;
  };

 private:
  PrepareMessage(tail_cb::Message &&msg) : CbMessage{std::move(msg)} {}

 public:
  size_t static bufferSize(size_t const proposal_size) {
    return offsetof(Layout, data_addr) + proposal_size;
  }

  static std::variant<std::invalid_argument, PrepareMessage> tryFrom(
      tail_cb::Message &&msg) {
    if (msg.size() < bufferSize(0)) {
      return std::invalid_argument(
          "Message smaller than minimum prepare size.");
    }
    return PrepareMessage(std::move(msg));
  }

  View view() const {
    return reinterpret_cast<Layout const *>(msg.data())->view;
  }

  Instance instance() const {
    return reinterpret_cast<Layout const *>(msg.data())->instance;
  }

  uint8_t const *data() const {
    return reinterpret_cast<Layout const *>(msg.data())->data();
  }

  size_t size() const { return msg.size() - bufferSize(0); }

  std::string_view stringView() const {
    return std::string_view(reinterpret_cast<char const *>(data()), size());
  }

  // TODO (Antoine): this is broken as the returned batch can modify the data.
  Batch const asBatch() const {
    return Batch(*const_cast<Batch::Layout *>(
                     reinterpret_cast<Batch::Layout const *>(data())),
                 size());
  }
};

/**
 * @brief Abstract class that holds a message that consists of a certificate.
 *        Can be either a CommitMessage (with a prepare certificate) or a
 *        CheckpointMessage (with a checkpoint certificate).
 *
 */
struct CertificateMessage : public CbMessage {
  using Certificate = certifier::Certificate;

  struct Layout {
    MessageKind kind;  // Commit or Checkpoint, Must always be the first field

    uint8_t *certificate() { return &certificate_addr; }
    uint8_t const *certificate() const { return &certificate_addr; }

    uint8_t certificate_addr;
  };

 protected:
  CertificateMessage(tail_cb::Message &&msg) : CbMessage{std::move(msg)} {}

 public:
  size_t static constexpr CertificateOffset =
      offsetof(Layout, certificate_addr);

  std::variant<std::invalid_argument, certifier::Certificate>
  tryIntoCertificate(
      std::optional<std::pair<size_t, size_t>> exp_size_quorum = std::nullopt) {
    auto buffer = msg.takeBuffer();
    auto const cb_msg_data_offset = tail_cb::Message::bufferSize(0);
    buffer.trimLeft(cb_msg_data_offset + CertificateOffset);
    if (exp_size_quorum) {
      if (buffer.size() != Certificate::bufferSize(exp_size_quorum->first,
                                                   exp_size_quorum->second)) {
        return std::invalid_argument(
            fmt::format("Message too small for a certificate with an object of "
                        "size {} and a quorum of {}.",
                        exp_size_quorum->first, exp_size_quorum->second));
      }
    }
    return Certificate::tryFrom(std::move(buffer));
  }

 private:
  /**
   * @brief Return a pointer to the start of the inner certificate.
   *
   * @return uint8_t const*
   */
  uint8_t const *certificate() const {
    return reinterpret_cast<Layout const *>(msg.data())->certificate();
  }

  /**
   * @brief Return the number of shares within the inner certificate.
   *
   * @return size_t
   */
  size_t nbShares() const {
    return reinterpret_cast<Certificate::Header const *>(certificate())
        ->nb_shares;
  }
};

struct CommitMessage : public CertificateMessage {
 private:
  CommitMessage(tail_cb::Message &&msg) : CertificateMessage{std::move(msg)} {}

 public:
  size_t static bufferSize(size_t const proposal_size, size_t const nb_shares) {
    return CertificateOffset +
           Certificate::bufferSize(proposal_size, nb_shares);
  }

  static std::variant<std::invalid_argument, CommitMessage> tryFrom(
      tail_cb::Message &&msg, size_t const quorum) {
    if (msg.size() < bufferSize(0, quorum)) {
      return std::invalid_argument("Message smaller than minimum commit size.");
    }
    return CommitMessage(std::move(msg));
  }
};

struct CheckpointMessage : public CertificateMessage {
 private:
  CheckpointMessage(tail_cb::Message &&msg)
      : CertificateMessage{std::move(msg)} {}

 public:
  size_t static bufferSize(size_t const nb_shares) {
    return CertificateOffset +
           Certificate::bufferSize(sizeof(consensus::Checkpoint), nb_shares);
  }

  static std::variant<std::invalid_argument, CheckpointMessage> tryFrom(
      tail_cb::Message &&msg, size_t const quorum) {
    if (msg.size() != bufferSize(quorum)) {
      return std::invalid_argument("Checkpoint message size doesn't match.");
    }
    return CheckpointMessage(std::move(msg));
  }
};

struct SealViewMessage : public CbMessage {
  struct Layout {
    MessageKind kind;  // Must always be the first field
  };

 private:
  SealViewMessage(tail_cb::Message &&msg) : CbMessage{std::move(msg)} {}

 public:
  size_t static constexpr BufferSize = sizeof(Layout);

  static std::variant<std::invalid_argument, SealViewMessage> tryFrom(
      tail_cb::Message &&msg) {
    if (msg.size() != BufferSize) {
      return std::invalid_argument("Seal view size doesn't match.");
    }
    return SealViewMessage(std::move(msg));
  }
};

struct NewViewMessage : public CbMessage {
  struct Layout {
    MessageKind kind;  // Must always be the first field
    View new_view;
    uint8_t vc_certificates;  // Fake field
  };

  struct VcCertificateEntry {
    ProcId replica_id;
    size_t certificate_size;
    uint8_t certificate;  // Fake field
  };

 private:
  NewViewMessage(tail_cb::Message &&msg) : CbMessage{std::move(msg)} {}

 public:
  size_t static constexpr bufferSize(
      size_t const window, size_t const max_proposal_size, size_t const quorum,
      std::optional<size_t> nb_certificates = std::nullopt) {
    return offsetof(Layout, vc_certificates) +
           nb_certificates.value_or(quorum) *
               (offsetof(VcCertificateEntry, certificate) +
                certifier::Certificate::bufferSize(
                    internal::SerializedState::bufferSize(window,
                                                          max_proposal_size),
                    quorum));
  }

  static std::variant<std::invalid_argument, NewViewMessage> tryFrom(
      tail_cb::Message &&msg, size_t const window,
      size_t const max_proposal_size, size_t const quorum) {
    if (msg.size() != bufferSize(window, max_proposal_size, quorum)) {
      return std::invalid_argument("New view size doesn't match.");
    }
    return NewViewMessage(std::move(msg));
  }

  View view() const {
    return reinterpret_cast<Layout const *>(msg.data())->new_view;
  }

  /**
   * @brief Clone a buffer containing the certificate at `index` (so that a
   *        certificate can be built from it).
   *
   * Allocates the buffer on the heap.
   *
   * @param index of the certificate to clone.
   * @param tail of Consensus.
   * @param max_proposal_size of Consensus.
   * @param quorum of Consensus
   * @return a pair composed of the id of the process the certificate is about
   *         and the certificate buffer.
   */
  std::pair<ProcId, Buffer> cloneCertificateBuffer(
      size_t const index, size_t const window, size_t const max_proposal_size,
      size_t const quorum) const {
    auto const &ce = *reinterpret_cast<VcCertificateEntry const *>(
        msg.data() + bufferSize(window, max_proposal_size, quorum, index));
    Buffer buffer(ce.certificate_size);
    std::copy(&ce.certificate, &ce.certificate + ce.certificate_size,
              buffer.data());
    return {ce.replica_id, std::move(buffer)};
  }

  /**
   * @brief Clone the serialized state contained within the index-th
   * certificate. Assume that the certificates are valid.
   *
   * Allocates the buffer on the heap.
   *
   * @param index
   * @param tail
   * @param max_proposal_size
   * @param quorum
   * @return SerializedState
   */
  SerializedState cloneSerializedState(size_t const index, size_t const window,
                                       size_t const max_proposal_size,
                                       size_t const quorum) const {
    auto [_, cert_buffer] =
        cloneCertificateBuffer(index, window, max_proposal_size, quorum);
    auto certificate = std::get<certifier::Certificate>(
        certifier::Certificate::tryFrom(std::move(cert_buffer)));
    Buffer ss_buffer(certificate.messageSize());
    std::copy(certificate.message(),
              certificate.message() + certificate.messageSize(),
              ss_buffer.data());
    return ss_buffer;
  }

  /**
   * @brief Build a map of all values that MUST be proposed by the new leader.
   *
   */
  TailMap<Instance, Buffer> validValues(size_t const window,
                                        size_t const max_proposal_size,
                                        size_t const quorum) const {
    // We first build the map in a convenient manner.
    std::map<Instance, std::pair<View, Buffer>> best_proposals;
    for (size_t i = 0; i < quorum; i++) {
      auto const ss =
          cloneSerializedState(i, window, max_proposal_size, quorum);
      for (size_t j = 0; j < ss.nbBroadcastCommits(); j++) {
        auto const &commit = ss.commit(j);
        // fmt::print("Scanning serialized state {}, commit.instance {}...\n",
        //            i, commit.instance);
        auto bp_it = best_proposals.find(commit.instance);
        if (bp_it == best_proposals.end() ||
            bp_it->second.first < commit.view) {
          if (bp_it != best_proposals.end()) {
            best_proposals.erase(bp_it);
          }
          Buffer proposal(commit.proposal_size);
          std::copy(&commit.proposal, &commit.proposal + commit.proposal_size,
                    proposal.data());
          best_proposals.try_emplace(
              commit.instance,
              std::make_pair(commit.view, std::move(proposal)));
        }
      }
    }
    // And now we make it efficient as it will be used on the critical path.
    TailMap<Instance, Buffer> valid_values(window);
    for (auto &[instance, view_buffer] : best_proposals) {
      std::string_view value(
          reinterpret_cast<char const *>(&*view_buffer.second.cbegin()),
          static_cast<size_t>(view_buffer.second.cend() -
                              view_buffer.second.cbegin()));
      valid_values.tryEmplace(instance, std::move(view_buffer.second));
    }
    return valid_values;
  }
};

struct Message {
  static size_t maxBufferSize(size_t const window,
                              size_t const max_proposal_size,
                              size_t const quorum) {
    return std::max(
        {PrepareMessage::bufferSize(max_proposal_size),
         CommitMessage::bufferSize(max_proposal_size, quorum),
         CheckpointMessage::bufferSize(quorum), SealViewMessage::BufferSize,
         NewViewMessage::bufferSize(window, max_proposal_size, quorum)});
  }

  using Variant =
      std::variant<std::invalid_argument, PrepareMessage, CommitMessage,
                   CheckpointMessage, SealViewMessage, NewViewMessage>;

  static Variant tryFrom(tail_cb::Message &&msg, size_t const window,
                         size_t const max_proposal_size, size_t const quorum) {
    if (msg.size() < sizeof(MessageKind)) {
      return std::invalid_argument("Message smaller than Kind.");
    }
    auto const &kind = *reinterpret_cast<MessageKind const *>(msg.data());
    switch (kind) {
      case Prepare:
        return variant_cast<Variant>(PrepareMessage::tryFrom(std::move(msg)));
      case Commit:
        return variant_cast<Variant>(
            CommitMessage::tryFrom(std::move(msg), quorum));
      case Checkpoint:
        return variant_cast<Variant>(
            CheckpointMessage::tryFrom(std::move(msg), quorum));
      case SealView:
        return variant_cast<Variant>(SealViewMessage::tryFrom(std::move(msg)));
      case NewView:
        return variant_cast<Variant>(NewViewMessage::tryFrom(
            std::move(msg), window, max_proposal_size, quorum));
      default:
        return std::invalid_argument(fmt::format("Unknown kind {}", kind));
    }
  }
};

// Small messages that can be moved

// TODO(Antoine): should we pack?
struct FastCommitMessage {
  View view;
  Instance instance;
};

}  // namespace dory::ubft::consensus::internal
