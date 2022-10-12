#pragma once

#include <cstddef>
#include <map>

#include <hipony/enumerate.hpp>

#include "../types.hpp"
#include "messages.hpp"

namespace dory::ubft::consensus::internal {

/**
 * @brief Holds information about the ongoing view chage required by the next
 *        leader.
 *
 */
class ViewChangeState {
 public:
  ViewChangeState(View const view) : view{view} {}
  View view;  // View transiting from
  std::map<ProcId, certifier::Certificate> vc_state_certificates;

  Buffer buildNewView(size_t const window, size_t const max_proposal_size,
                      size_t const quorum) {
    Buffer buffer(
        NewViewMessage::bufferSize(window, max_proposal_size, quorum));
    auto &nv = *reinterpret_cast<NewViewMessage::Layout *>(buffer.data());
    nv.kind = MessageKind::NewView;
    nv.new_view = view + 1;
    for (auto &&[index, proc_certificate] :
         hipony::enumerate(vc_state_certificates)) {
      auto const &[proc_id, certificate] = proc_certificate;
      auto &ce = *reinterpret_cast<NewViewMessage::VcCertificateEntry *>(
          buffer.data() +
          NewViewMessage::bufferSize(window, max_proposal_size, quorum, index));
      ce.replica_id = proc_id;
      ce.certificate_size = certificate.rawBuffer().size();
      std::copy(certificate.rawBuffer().data(),
                certificate.rawBuffer().data() + certificate.rawBuffer().size(),
                &ce.certificate);
    }
    return buffer;
  }
};

}  // namespace dory::ubft::consensus::internal
