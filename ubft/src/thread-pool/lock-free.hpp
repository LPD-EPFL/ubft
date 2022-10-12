#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

#include <dory/third-party/queues/keyed-priority-queue.hpp>
#include <dory/third-party/sync/mpmc.hpp>

#include <dory/shared/assert.hpp>
#include <dory/shared/branching.hpp>
#include <dory/shared/move-indicator.hpp>
#include <dory/shared/pinning.hpp>

#include <fmt/core.h>
#include <hipony/enumerate.hpp>

#include "../unsafe-at.hpp"

namespace dory::ubft {
class LockFreeTailThreadPool {
  /**
   * @brief A wrapper around std::function to make sure it is not copied.
   *
   */
  class Task {
   public:
    Task(std::function<void()> &&f) : f{std::move(f)} {}

    Task(Task const &) = delete;
    Task &operator=(Task const &) = delete;
    Task(Task &&) = default;
    Task &operator=(Task &&) = default;

    void operator()() { f(); }

   private:
    std::function<void()> f;
  };

  class TailTaskQueues {
    using Mpmc = third_party::sync::MpmcQueue<Task>;
    using ProducerToken = third_party::sync::MpmcProducerToken;

    struct TailTaskQueue {
      TailTaskQueue(size_t const tail)
          : tail{tail},
            mpmcs{{Mpmc(tail), Mpmc(tail)}},
            tokens{{ProducerToken(mpmcs.at(0)), ProducerToken(mpmcs.at(1))}} {}

      TailTaskQueue(TailTaskQueue const &) = delete;
      TailTaskQueue &operator=(TailTaskQueue const &) = delete;
      TailTaskQueue(TailTaskQueue &&) = default;
      TailTaskQueue &operator=(TailTaskQueue &&) = default;

      size_t tail;
      std::array<Mpmc, 2> mpmcs;
      std::array<ProducerToken, 2> tokens;
      size_t dest = 0;
      size_t inserted_in_dest = 0;

      void enqueue(Task &&task) {
        if (!uat(mpmcs, dest).try_enqueue(uat(tokens, dest), std::move(task))) {
          throw std::logic_error("Should always be able to enqueue.");
        }
        if (++inserted_in_dest < tail) {
          return;
        }
        // After tail insertions, we use the next mpmc.
        dest = 1 - dest;
        inserted_in_dest = 0;
        // We first empty it
        std::optional<Task> dropped;
        // try_dequeue does not use the optional: manual reset required.
        while ((dropped.reset(), uat(mpmcs, dest).try_dequeue(dropped))) {
        }
      }

      std::optional<Task> tryPop() {
        std::optional<Task> popped;
        for (auto &mpmc : mpmcs) {
          if (mpmc.try_dequeue(popped)) {
            return popped;
          }
        }
        return std::nullopt;
      }

      void clear() {
        std::optional<Task> dropped;
        for (auto &mpmc : mpmcs) {
          while ((dropped.reset(), mpmc.try_dequeue(dropped))) {
          }
        }
      }
    };

   public:
    using Index = size_t;

    /**
     * @brief Creates a queue with a maximum number of messages and returns its
     * identifier.
     *
     * @param tail
     */
    Index createQueue(size_t const tail) {
      auto const index = queues.size();
      queues.emplace_back(tail);
      return index;
    }

    /**
     * @brief Enqueue a task in the indexed queue. Drops the oldest element if
     * the queue is bigger than its tail.
     *
     * @param index
     * @param task
     */
    void enqueue(Index const index, Task &&task) {
      uat(queues, index).enqueue(std::move(task));
    }

    std::optional<Task> tryPop() {
      // TODO(Antoine): add heuristic
      for (auto &queue : queues) {
        if (auto popped = queue.tryPop()) {
          return popped;
        }
      }
      return std::nullopt;
    }

    void clear(Index const index) { uat(queues, index).clear(); }

   private:
    // We use the deque as a vector that doesn't move its elements.
    std::deque<TailTaskQueue> queues;
  };

 public:
  /**
   * @brief Handle for a task queue within a LockFreeTailThreadPool.
   *
   */
  class TaskQueue {
   public:
    using Id = TailTaskQueues::Index;

    TaskQueue(LockFreeTailThreadPool &thread_pool, size_t const tail)
        : thread_pool{thread_pool},
          id{thread_pool.initTaskQueue(tail)},
          tail{tail} {}

    TaskQueue(TaskQueue &&) = default;
    TaskQueue &operator=(TaskQueue &&) = delete;

    ~TaskQueue() {
      if (moved) {
        return;
      }
      // We drop all the tasks in the queue (and wait for the outstanding ones)
      // before returning.
      thread_pool.clear(id);
      thread_pool.waitOneIteration();
    }

    /**
     * @brief Enqueue a task. Drop the oldest task if the queue grows beoynd
     * `tail`.
     *
     * @tparam F
     * @param f
     * @return std::future<typename std::result_of<F()>::type>
     */
    template <class F>
    auto enqueue(F &&f) -> std::future<typename std::result_of<F()>::type> {
      return thread_pool.enqueue(id, std::forward<F>(f));
    }

    static size_t maxOutstanding(size_t const tail,
                                 LockFreeTailThreadPool const &thread_pool) {
      return 2 * tail + thread_pool.nbWorkers();
    }

   private:
    LockFreeTailThreadPool &thread_pool;
    Id const id;
    size_t const tail;
    MoveIndicator moved;
  };

  LockFreeTailThreadPool(std::string const &name, size_t const threads,
                         std::vector<int> const &proc_aff = {}) {
    for (size_t i = 0; i < threads; ++i) {
      worker_loops.emplace_back(0);
      workers.emplace_back([&, i] {
        auto &worker_loop = uat(worker_loops, i);
        size_t idle_loops = 0;
        for (;;) {
          if (stop.load(std::memory_order_relaxed)) {
            break;
          }
          worker_loop.fetch_add(1, std::memory_order_relaxed);
          if (auto task = tasks.tryPop()) {
            (*task)();
            idle_loops = 0;
          } else {
            // We go to sleep to prevent busy-waiting.
            if (++idle_loops > 1024) {
              std::this_thread::sleep_for(std::chrono::microseconds(50));
              if (stop) {
                break;
              }
            }
          }
        }
      });
      dory::set_thread_name(workers[i], (name + std::to_string(i)).c_str());
    }

    for (size_t i = 0; i < std::min(threads, proc_aff.size()); ++i) {
      dory::pin_thread_to_core(workers[i], proc_aff.at(i));
    }
  }

  // As TaskQueues hold references to the pool, it shouldn't be moved.
  LockFreeTailThreadPool(LockFreeTailThreadPool &&) = delete;
  LockFreeTailThreadPool &operator=(LockFreeTailThreadPool &&) = delete;

  /**
   * @brief Initialize a task queue with a maximum number of elements.
   *
   * @param tail
   */
  TaskQueue::Id initTaskQueue(size_t const tail) {
    return tasks.createQueue(tail);
  }

  /**
   * @brief Enqueue a task to a queue. Drop its oldest task if it grows beyond
   * `tail`.
   *
   * @tparam F
   * @param tq_id
   * @param f
   * @return std::future<typename std::result_of<F()>::type>
   */
  template <class F>
  auto enqueue(TaskQueue::Id const &tq_id, F &&f)
      -> std::future<typename std::result_of<F()>::type> {
    using return_type = typename std::result_of<F()>::type;

    auto task =
        std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));

    std::future<return_type> res = task->get_future();
    // if (unlikely(stop)) {
    //  throw std::runtime_error("enqueue on stopped ThreadPool");
    // }
    tasks.enqueue(tq_id, std::function<void()>([task]() { (*task)(); }));
    return res;
  }

  void clear(TaskQueue::Id const &tq_id) { tasks.clear(tq_id); }

  void waitOneIteration() {
    for (auto &loop : worker_loops) {
      auto old_loop = loop.load();
      while (old_loop == loop.load()) {
      }
    }
  }

  ~LockFreeTailThreadPool() {
    stop = true;
    for (std::thread &worker : workers) {
      worker.join();
    }
  }

  size_t nbWorkers() const { return workers.size(); }

 private:
  std::deque<std::atomic<uint64_t>> worker_loops;
  std::vector<std::thread> workers;
  TailTaskQueues tasks;
  std::atomic<bool> stop = false;
};

}  // namespace dory::ubft
