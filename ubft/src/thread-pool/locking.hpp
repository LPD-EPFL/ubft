#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

#include <dory/third-party/queues/keyed-priority-queue.hpp>

#include <dory/shared/assert.hpp>
#include <dory/shared/branching.hpp>
#include <dory/shared/move-indicator.hpp>
#include <dory/shared/pinning.hpp>

#include <fmt/core.h>

#include "../unsafe-at.hpp"

namespace dory::ubft {
/**
 * A thread pool using stl queues and a condition variable to notify workers
 * upon a new task. While waiting, the worker thread is suspended. Tasks are
 * tagged with an identifier. Tasks with the same identifier are queued
 * together. Queues are of bounded size. If a queue grows larger than `tail`,
 * its oldest element is dropped.
 * */
class LockingThreadPool {
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
    struct QueueTailPair {
      QueueTailPair(size_t const tail) : tail{tail} {}

      QueueTailPair(QueueTailPair const &) = delete;
      QueueTailPair &operator=(QueueTailPair const &) = delete;
      QueueTailPair(QueueTailPair &&) = default;
      QueueTailPair &operator=(QueueTailPair &&) = default;

      std::deque<Task> queue;
      size_t tail;
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
      queues_sizes.set(index, 0);
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
      auto &[queue, tail] = uat(queues, index);
      queue.emplace_back(std::move(task));
      if (unlikely(queue.size() > tail)) {
        queue.pop_front();
      } else {
        tasks++;
        queues_sizes.increment(index);
      }
    }

    /**
     * @brief Pop the queue with the most messages and returns an std::pair with
     * the index of the poped queue and the task.
     *
     * @return std::pair<Index const&, Task>
     */
    std::pair<Index, Task> pop() {
      if (unlikely(tasks == 0)) {
        throw std::runtime_error("Empty queues.");
      }
      tasks--;
      auto const top_index = queues_sizes.top();
      queues_sizes.decrement(top_index);
      auto &queue = uat(queues, top_index).queue;
      std::pair<Index, Task> ret = {top_index, std::move(queue.front())};
      queue.pop_front();
      return ret;
    }

    void clear(Index const index) {
      auto &queue = uat(queues, index).queue;
      tasks -= queue.size();
      std::deque<Task>().swap(queue);
      queues_sizes.set(index, 0);
    }

    bool empty() const { return tasks == 0; }

   private:
    std::vector<QueueTailPair> queues;
    // A priority queue that gives the index of the queue with the most tasks.
    third_party::queues::KeyedPriorityQueue<Index, size_t> queues_sizes;
    // Total number of tasks in the queues.
    size_t tasks = 0;
  };

 public:
  /**
   * @brief Handle for a task queue within a LockingThreadPool.
   *
   */
  class TaskQueue {
   public:
    using Id = TailTaskQueues::Index;

    TaskQueue(LockingThreadPool &thread_pool, size_t const tail)
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
                                 LockingThreadPool const &thread_pool) {
      return tail + thread_pool.nbWorkers();
    }

   private:
    LockingThreadPool &thread_pool;
    Id const id;
    size_t const tail;
    MoveIndicator moved;
  };

  LockingThreadPool(std::string const &name, size_t const threads,
                    std::vector<int> const &proc_aff = {}) {
    for (size_t i = 0; i < threads; ++i) {
      workers.emplace_back([&] {
        for (;;) {
          auto id_task =
              [&]() -> std::optional<std::pair<TaskQueue::Id, Task>> {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock,
                           [&] { return stop || (!frozen && !tasks.empty()); });
            if (unlikely(stop)) {
              return std::nullopt;
            }
            auto id_task = tasks.pop();
            uat(running, id_task.first)++;
            return id_task;
          }();
          if (!id_task) {
            return;
          }
          id_task->second();
          {
            std::unique_lock<std::mutex> lock(mutex);
            uat(running, id_task->first)--;
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
  LockingThreadPool(LockingThreadPool &&) = delete;
  LockingThreadPool &operator=(LockingThreadPool &&) = delete;

  /**
   * @brief Initialize a task queue with a maximum number of elements.
   *
   * @param tail
   */
  TaskQueue::Id initTaskQueue(size_t const tail) {
    auto id = tasks.createQueue(tail);
    running.emplace_back(0);
    return id;
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
    {
      std::unique_lock<std::mutex> lock(mutex);
      if (unlikely(stop)) {
        throw std::runtime_error("enqueue on stopped ThreadPool");
      }
      tasks.enqueue(tq_id, std::function<void()>([task]() { (*task)(); }));
      if (!frozen) {
        condition.notify_one();
      }
    }
    return res;
  }

  void clear(TaskQueue::Id const &tq_id) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      // Remove all queued tasks.
      tasks.clear(tq_id);
    }
    // Wait until all ongoing tasks are computed.
    while (uat(running, tq_id) != 0) {
    }
  }

  ~LockingThreadPool() {
    {
      std::unique_lock<std::mutex> lock(mutex);
      stop = true;
      condition.notify_all();
    }
    for (std::thread &worker : workers) {
      worker.join();
    }
  }

  void freeze() {
    std::unique_lock<std::mutex> lock(mutex);
    frozen = true;
    condition.notify_all();
  }

  void unfreeze() {
    std::unique_lock<std::mutex> lock(mutex);
    frozen = false;
    condition.notify_all();
  }

  size_t nbWorkers() const { return workers.size(); }

 private:
  std::vector<std::thread> workers;
  TailTaskQueues tasks;
  // Deque ~ Vector but doesn't move. Indexed by queue id.
  std::deque<std::atomic<size_t>> running;
  std::mutex mutex;
  std::condition_variable condition;
  bool stop = false;
  bool frozen = false;
};

}  // namespace dory::ubft
