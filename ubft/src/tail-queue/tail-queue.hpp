#pragma once

#include "deque.hpp"
#include "vector.hpp"

#define TAIL_QUEUE_VECTOR_IMPL true

namespace dory::ubft {

#if TAIL_QUEUE_VECTOR_IMPL
template <typename V>
using TailQueue = VectorTailQueue<V>;
#else
template <typename V>
using TailQueue = DequeTailQueue<V>;
#endif

}  // namespace dory::ubft
