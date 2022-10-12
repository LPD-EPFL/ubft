#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/core.h>

#include "tail-thread-pool.hpp"

using namespace dory::ubft;

int main() {
  static size_t constexpr Runs = 5;
  static size_t constexpr MaxNbThreads = 8;
  static size_t constexpr NbQueues = 20;
  static size_t constexpr QueueSize = 20;
  static size_t constexpr TasksPerQueue = 100;
  static auto constexpr TaskDuration = std::chrono::microseconds(30);

  for (size_t threads = 1; threads <= MaxNbThreads; threads++) {
    for (size_t r = 0; r < Runs; r++) {
      TailThreadPool thread_pool("main", threads);
      std::vector<TailThreadPool::TaskQueue> task_queues;
      std::vector<std::future<void>> futures;
      for (size_t q = 0; q < NbQueues; q++) {
        task_queues.emplace_back(thread_pool, QueueSize);
      }
      auto start = std::chrono::steady_clock::now();
      for (size_t t = 0; t < TasksPerQueue; t++) {
        for (auto &queue : task_queues) {
          auto f = queue.enqueue([t]() {
            auto wait_start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - wait_start < TaskDuration)
              ;
          });
          if (t == TasksPerQueue - 1) {
            futures.emplace_back(std::move(f));
          }
        }
      }
      for (auto &future : futures) {
        future.wait();
      }
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start);
      auto goal = TaskDuration * std::min(QueueSize, TasksPerQueue) * NbQueues /
                  threads;
      fmt::print("[{} threads] Measured time: {}, Goal: {}, Efficiency: {}%\n",
                 threads, duration, goal, goal * 100 / duration);
    }
  }
  return 0;
}
