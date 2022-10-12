#pragma once

#include "internal/async-sender.hpp"
#include "internal/sync-sender.hpp"

namespace dory::ubft::tail_p2p {
// Re-exports
using dory::ubft::tail_p2p::internal::AsyncSender;
using dory::ubft::tail_p2p::internal::SyncSender;
// Default is the async one.
using Sender = AsyncSender;
}  // namespace dory::ubft::tail_p2p
