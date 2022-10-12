#pragma once

#include <atomic>
#include <mutex>
#include <vector>

namespace dory::ubft::rpc::internal {
template <typename Iterator>
class DynamicConnections {
 public:
  using ValueType = typename Iterator::value_type;
  using ConnectionList = typename std::vector<ValueType>;

  DynamicConnections() {
    connections_[0] = &exported_connections;
    connections_[1] = &pending_connections;

    in_use = 0;

    returned_connections = connections_[in_use];

    select.store(in_use);
    switched.store(false);
  }

  std::vector<ValueType> *alterConnections(Iterator first, Iterator last) {
    int index;
    {  // Lock scope
      std::scoped_lock<std::mutex> lock(pending_connections_mtx);

      // I have the mutex, so the other thread cannot change the select.
      // `index` indicates the buffer is currently exported.
      index = select.load();

      // Get the pending buffer
      index = (index + 1) % 2;

      // Empty the pending buffer and write the new connections
      connections_[index]->clear();
      for (auto it = first; it != last; it++) {
        connections_[index]->push_back(*it);
      }

      // Now tell the other thread that the connections changed
      select.store(index);
    }

    // Now wait for the other thread to switch
    while (!switched.load()) {
    }
    switched.store(false);

    // Now return the inactive connections
    return connections_[(index + 1) % 2];
  }

  std::vector<ValueType> &connections() {
    checkPending();
    return *returned_connections;
  }

 private:
  void checkPending() {
    if (select.load() != in_use) {
      // Connections have changed

      // Ensure that the connections are not altered concurrently
      std::scoped_lock<std::mutex> lock(pending_connections_mtx);

      in_use = (in_use + 1) % 2;
      returned_connections = connections_[in_use];

      select.store(in_use);
      switched.store(true);
    }
  }

  std::vector<ValueType> *connections_[2];
  int in_use;
  int for_removal;

  std::vector<ValueType> pending_connections;
  std::vector<ValueType> exported_connections;

  std::mutex pending_connections_mtx;

  // alignas(64)
  std::atomic<int> select;

  // alignas(64)
  std::atomic<bool> switched;

  std::vector<ValueType> *returned_connections;
};
}  // namespace dory::ubft::rpc::internal
