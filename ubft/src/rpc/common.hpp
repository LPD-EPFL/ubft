#pragma once

#include "../tail-p2p/receiver.hpp"
#include "../tail-p2p/sender.hpp"

namespace dory::ubft::rpc {

using Sender = tail_p2p::AsyncSender;
using Receiver = tail_p2p::Receiver;

}  // namespace dory::ubft::rpc
