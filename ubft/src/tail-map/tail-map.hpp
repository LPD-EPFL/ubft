#pragma once

#include "tree.hpp"
#include "vector.hpp"

#define TAIL_MAP_VECTOR_IMPL true

namespace dory::ubft {

#if TAIL_MAP_VECTOR_IMPL
template <typename K, typename V>
using TailMap = VectorTailMap<K, V>;
#else
template <typename K, typename V>
using TailMap = TreeTailMap<K, V>;
#endif

}  // namespace dory::ubft
