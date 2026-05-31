#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

// A small streaming stage-pipeline runner. Describe a pipeline as a push source
// plus a chain of stages — N transforms then a sink — and run() executes them
// either as overlapped threads or inline, from one description:
//
//   pipe::run(threaded, depth,
//       [&](auto emit){ stream_envelope(rec, opts, emit, block); },     // source
//       pipe::transform<DecodedBlock>(depth, [&](auto env, auto& out){   // stage
//           decoder.decode_into(out, env); }),
//       pipe::sink([&](const DecodedBlock& b){ decoder.deposit(b); }));  // sink
//
// Threaded, each stage is pinned to one in-order (FIFO) worker thread a block
// apart, with owned buffers passed between stages through bounded pools
// (backpressure -> bounded memory). The source's blocks (std::span<const float>)
// are copied once into the first pool; each stage transforms into the next pool.
// Serial runs the very same stage functions back-to-back on the calling thread
// with reused buffers and no copies. Both are in-order per stage, so the result
// is identical — the block-invariance guarantee. A worker exception is captured,
// winds the pipeline down, and is rethrown on the calling thread after a clean
// drain (it must not escape into async_scope, which would std::terminate).
namespace palindrome::pipe {

// Bounded pool of reusable buffers. acquire() blocks when every buffer is in
// flight (backpressure), and returns a move-only Handle that returns the buffer
// to the pool on destruction — so a buffer is released down every path,
// including a worker exception or the handle being dropped mid-pipeline.
template<class T>
class Pool {
public:
  class Handle {
  public:
    Handle() = default;
    Handle(Pool *pool, T *buf) : pool_{pool}, buf_{buf} {}
    Handle(Handle &&o) noexcept : pool_{o.pool_}, buf_{std::exchange(o.buf_, nullptr)} {}
    Handle &operator=(Handle &&o) noexcept {
      if (this != &o) {
        reset();
        pool_ = o.pool_;
        buf_ = std::exchange(o.buf_, nullptr);
      }
      return *this;
    }
    ~Handle() { reset(); }
    [[nodiscard]] T &operator*() const noexcept { return *buf_; }
    [[nodiscard]] T *operator->() const noexcept { return buf_; }

  private:
    void reset() noexcept {
      if (buf_)
        pool_->release(std::exchange(buf_, nullptr));
    }
    Pool *pool_ = nullptr;
    T *buf_ = nullptr;
  };

  explicit Pool(std::ptrdiff_t n) : avail_{require_positive(n)}, buffers_(static_cast<std::size_t>(n)) {
    for (auto &b: buffers_)
      free_.push_back(&b);
  }
  [[nodiscard]] Handle acquire() {
    avail_.acquire();
    const std::lock_guard lk{mtx_};
    T *b = free_.back();
    free_.pop_back();
    return Handle{this, b};
  }

private:
  // Validate before the semaphore/buffer members initialise — a non-positive
  // count would be a deadlocked semaphore and (via the signed->unsigned cast) a
  // gigantic buffer allocation.
  static std::ptrdiff_t require_positive(std::ptrdiff_t n) {
    if (n <= 0)
      throw std::invalid_argument{"pipe::Pool: in_flight must be positive"};
    return n;
  }
  void release(T *b) {
    {
      const std::lock_guard lk{mtx_};
      free_.push_back(b);
    }
    avail_.release();
  }
  std::counting_semaphore<> avail_;
  std::mutex mtx_;
  std::deque<T> buffers_;
  std::vector<T *> free_;
};

// One in-order stage: a stdexec run_loop drained by a single jthread, so work
// scheduled on it runs FIFO. Non-movable (owns the loop and thread).
class Worker {
public:
  Worker() = default;
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  ~Worker() { loop_.finish(); } // run() then returns; the jthread member joins
  [[nodiscard]] auto scheduler() noexcept { return loop_.get_scheduler(); }

private:
  stdexec::run_loop loop_;
  std::jthread thread_{[this] { loop_.run(); }};
};

// First worker exception, captured under a mutex; the relaxed flag lets stages
// cheaply skip their work once something has failed, so the pipeline drains fast.
class ErrorFunnel {
public:
  void capture() {
    const std::lock_guard lk{mtx_};
    if (!error_)
      error_ = std::current_exception();
    stopped_.store(true, std::memory_order_relaxed);
  }
  [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_relaxed); }
  void rethrow_if_failed() const {
    const std::lock_guard lk{mtx_};
    if (error_)
      std::rethrow_exception(error_);
  }

private:
  mutable std::mutex mtx_;
  std::exception_ptr error_;
  std::atomic<bool> stopped_{false};
};

// A transform stage: fills an owned Out from its input. in_flight sizes its
// output pool (threaded). The sink is the terminal stage and produces nothing.
template<class Out, class Fn>
struct Transform {
  using out_type = Out;
  std::ptrdiff_t in_flight;
  Fn fn; // void(const In&, Out&)  (In is std::span<const float> for the first stage)
};
template<class Out, class Fn>
[[nodiscard]] Transform<Out, std::decay_t<Fn>> transform(std::ptrdiff_t in_flight, Fn &&fn) {
  return {in_flight, std::forward<Fn>(fn)};
}

template<class Fn>
struct Sink {
  Fn fn; // void(const In&)
};
template<class Fn>
[[nodiscard]] Sink<std::decay_t<Fn>> sink(Fn &&fn) {
  return {std::forward<Fn>(fn)};
}

namespace detail {

// The first stage reads the source view (span); later stages read the previous
// stage's owned output by const ref. arg_for() picks the right one per index.
template<std::size_t I, class Handle>
[[nodiscard]] decltype(auto) arg_for(Handle &h) {
  if constexpr (I == 0)
    return std::span<const float>{*h}; // h is the owned source buffer (vector<float>)
  else
    return static_cast<const std::remove_reference_t<decltype(*h)> &>(*h);
}

// Tuple of the transform out_types (every stage except the trailing sink).
template<class Tuple, class Seq>
struct outs_of;
template<class Tuple, std::size_t... I>
struct outs_of<Tuple, std::index_sequence<I...>> {
  using type = std::tuple<typename std::tuple_element_t<I, Tuple>::out_type...>;
};
template<class... Stages>
using outs_tuple_t = typename outs_of<std::tuple<Stages...>, std::make_index_sequence<sizeof...(Stages) - 1>>::type;

// Run stages [I..] inline on the current thread: transform fills the next reused
// buffer, the sink consumes. In-order, no copies, no pools.
template<std::size_t I, class In, class Stages, class Outs>
void serial_step(In &&in, Stages &stages, Outs &outs) {
  if constexpr (I + 1 == std::tuple_size_v<Stages>) {
    std::get<I>(stages).fn(std::forward<In>(in));
  }
  else {
    auto &out = std::get<I>(outs);
    std::get<I>(stages).fn(std::forward<In>(in), out);
    serial_step<I + 1>(static_cast<const std::tuple_element_t<I, Outs> &>(out), stages, outs);
  }
}

// Append stages [I..] onto a sender that delivers stage I's input handle, each
// hop transferring onto that stage's FIFO worker with continues_on — one flat
// chain, no nesting. A transform's then returns its owned output handle, which
// flows to the next stage; the sink's then returns void. Each then catches
// everything into the funnel (so it is noexcept and the spawned sender is
// error-free, as async_scope::spawn requires) and skips its body once the funnel
// has tripped, so the chain always completes and releases its buffers.
template<std::size_t I, class Sender, class Stages, class Pools, class Workers>
[[nodiscard]] auto append_stage(Sender upstream, Stages &stages, Pools &pools, Workers &workers, ErrorFunnel &funnel) {
  auto sched = workers[I].scheduler();
  if constexpr (I + 1 == std::tuple_size_v<Stages>) {
    // Sink: stopped() is checked before arg_for derefs `in`, so an empty handle
    // from a stopped upstream is never dereferenced.
    return std::move(upstream) | stdexec::continues_on(sched) |
           stdexec::then([&stages, &funnel](auto in) mutable noexcept {
             if (funnel.stopped())
               return;
             try {
               std::get<I>(stages).fn(arg_for<I>(in));
             }
             catch (...) {
               funnel.capture();
             }
           });
  }
  else {
    using Out = typename std::tuple_element_t<I, Stages>::out_type;
    auto next = std::move(upstream) | stdexec::continues_on(sched) |
                stdexec::then([&stages, &pools, &funnel](auto in) mutable noexcept -> typename Pool<Out>::Handle {
                  // Skip before acquiring once the pipeline has tripped — no point
                  // doing the backpressure wait on work we're abandoning. The whole
                  // body (the acquire's mutex/semaphore included) is inside the try,
                  // so nothing escapes this noexcept lambda. An empty handle is
                  // returned only when stopped, and downstream is stopped too.
                  if (funnel.stopped())
                    return {};
                  try {
                    auto out = std::get<I>(pools).acquire();
                    std::get<I>(stages).fn(arg_for<I>(in), *out);
                    return out;
                  }
                  catch (...) {
                    funnel.capture();
                    return {};
                  }
                });
    return append_stage<I + 1>(std::move(next), stages, pools, workers, funnel);
  }
}

} // namespace detail

template<class Source, class... Stages>
void run(bool threaded, std::ptrdiff_t source_in_flight, Source &&source, Stages... stages) {
  static_assert(sizeof...(Stages) >= 1, "a pipeline needs at least a sink");
  auto stage_tuple = std::tuple<Stages...>{std::move(stages)...};

  if (!threaded) {
    detail::outs_tuple_t<Stages...> outs; // reused across blocks, in-order
    source([&](std::span<const float> block) { detail::serial_step<0>(block, stage_tuple, outs); });
    return;
  }

  // One FIFO worker per stage (the sink included), and one output pool per
  // transform (the sink has none), sized by its in_flight. Worker and Pool are
  // non-movable, so both are constructed in place — the array value-inits its
  // workers, and the pool tuple is a prvalue elided into `pools` (no moves, no
  // heap indirection).
  std::array<Worker, sizeof...(Stages)> workers{};
  auto pools = [&]<std::size_t... I>(std::index_sequence<I...>) {
    return std::tuple<Pool<typename std::tuple_element_t<I, decltype(stage_tuple)>::out_type>...>{
        std::get<I>(stage_tuple).in_flight...};
  }(std::make_index_sequence<sizeof...(Stages) - 1>{});

  Pool<std::vector<float>> source_pool{source_in_flight};
  ErrorFunnel funnel;
  exec::async_scope scope;

  std::exception_ptr source_error;
  try {
    source([&](std::span<const float> block) {
      auto in = source_pool.acquire(); // backpressure if the first stage is behind
      in->assign(block.begin(), block.end()); // own the source block for the worker
      scope.spawn(detail::append_stage<0>(stdexec::just(std::move(in)), stage_tuple, pools, workers, funnel));
    });
  }
  catch (...) {
    source_error = std::current_exception();
  }

  stdexec::sync_wait(scope.on_empty()); // drain every spawned chain; the workers join on destruction below

  if (source_error)
    std::rethrow_exception(source_error);
  funnel.rethrow_if_failed();
}

} // namespace palindrome::pipe
