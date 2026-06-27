#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

// A persistent pool of worker threads draining a shared work queue. The threads
// are created once and park between batches (no spinning). run(tasks) submits a
// batch, has the CALLING thread help drain it too, and returns only once every
// task has finished - the per-batch barrier. Pull-based: an idle thread claims
// the next pending task, so an uneven task mix balances across threads instead of
// stalling behind a fixed assignment.
//
// Built for the per-field splat apply (one batch of band tasks per field), so the
// batch rate is low and the queue mutex is nowhere near a hot path; the work is
// the tasks themselves, run lock-free.
namespace palindrome {

class WorkQueue {
public:
  // `threads` total participants: threads - 1 persistent workers plus the caller,
  // which also drains in run(). threads <= 1 makes run() purely inline (no pool).
  explicit WorkQueue(unsigned threads) : threads_{threads < 1 ? 1u : threads} {
    for (unsigned w = 1; w < threads_; ++w)
      workers_.emplace_back([this] { worker(); });
  }
  WorkQueue(const WorkQueue &) = delete;
  WorkQueue &operator=(const WorkQueue &) = delete;
  ~WorkQueue() {
    {
      const std::lock_guard lk{mtx_};
      stop_ = true;
    }
    work_cv_.notify_all();
    // The jthread members join on destruction once worker() returns.
  }

  [[nodiscard]] unsigned threads() const noexcept { return threads_; }

  // Run every task, returning only once all have completed. `tasks` outlives the
  // call (run blocks until the batch drains), so the workers reference it.
  void run(std::span<const std::function<void()>> tasks) {
    if (threads_ == 1) {
      for (const auto &t: tasks)
        t();
      return;
    }
    std::unique_lock lk{mtx_};
    tasks_ = tasks;
    next_ = 0;
    remaining_ = tasks.size();
    work_cv_.notify_all();
    drain(lk); // the caller is a participant, not just a waiter
    done_cv_.wait(lk, [this] { return remaining_ == 0; });
    tasks_ = {}; // idle workers' predicate is now false until the next batch
  }

private:
  // Claim and run tasks until the batch is exhausted, holding `lk` except while a
  // task runs. Shared by the workers and the calling thread.
  void drain(std::unique_lock<std::mutex> &lk) {
    while (next_ < tasks_.size()) {
      const auto i = next_++;
      lk.unlock();
      tasks_[i]();
      lk.lock();
      if (--remaining_ == 0)
        done_cv_.notify_one();
    }
  }

  void worker() {
    std::unique_lock lk{mtx_};
    for (;;) {
      work_cv_.wait(lk, [this] { return stop_ || next_ < tasks_.size(); });
      if (stop_)
        return;
      drain(lk);
    }
  }

  unsigned threads_;
  std::vector<std::jthread> workers_; // the threads_ - 1 persistent helpers
  std::mutex mtx_;
  std::condition_variable work_cv_; // wakes workers when a batch arrives
  std::condition_variable done_cv_; // wakes the caller when the batch drains
  std::span<const std::function<void()>> tasks_{}; // the current batch (valid during run)
  std::size_t next_ = 0; // next task index to claim
  std::size_t remaining_ = 0; // tasks not yet completed
  bool stop_ = false;
};

} // namespace palindrome
