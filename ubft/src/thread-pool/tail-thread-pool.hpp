#pragma once

#include "lock-free.hpp"
#include "locking.hpp"

namespace dory::ubft {

using TailThreadPool = LockingThreadPool;

}
