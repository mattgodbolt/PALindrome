#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

// A linear pipeline of streaming DSP stages. Each stage owns its output buffer
// and exposes the same small protocol (process / max_output_for / input_multiple /
// prepare), so a Chain can size every stage's storage once up front from a
// single input-block size, then pipe a block straight through stage to stage
// with no per-call allocation. This is the "set the atom size, then ask each
// node to allocate its storage" pattern, specialised to a straight line.
//
// A stage's process() returns a view owned by that stage and valid only until
// its next call; because the Chain reads each stage's output exactly once (when
// feeding the next stage) before that stage runs again, piping the views
// straight through is safe.
namespace palindrome {

// What every stage must provide to live in a Chain.
template<class S>
concept PipelineStage = requires(S s, std::span<const float> in, std::size_t n) {
  { s.prepare(n) } -> std::same_as<void>;
  { s.process(in) } -> std::convertible_to<std::span<const float>>;
  { std::as_const(s).max_output_for(n) } -> std::convertible_to<std::size_t>;
  { std::as_const(s).input_multiple() } -> std::convertible_to<std::size_t>;
};

class Chain {
public:
  // Append a stage (moved in, owned by the Chain). Returns a reference to the
  // stored stage so callers can keep configuring or inspecting it.
  template<PipelineStage S>
  S &add(S stage) {
    auto holder = std::make_unique<Holder<S>>(std::move(stage));
    S &ref = holder->stage;
    nodes_.push_back(std::move(holder));
    return ref;
  }

  // Size every stage's storage for input blocks of up to `max_in` samples. The
  // budget shrinks down the chain as decimating stages reduce the rate.
  void prepare(std::size_t max_in) {
    std::size_t budget = max_in;
    for (auto &node: nodes_) {
      node->prepare(budget);
      budget = node->max_output_for(budget);
    }
  }

  // Run one block through every stage in turn. Returns the final stage's output
  // view (empty if the chain has no stages), valid until the next process().
  [[nodiscard]] std::span<const float> process(std::span<const float> in) {
    std::span<const float> s = in;
    for (auto &node: nodes_)
      s = node->process(s);
    return s;
  }

  // Upper bound on the final output count for `n_in` inputs, found by walking
  // the chain. Phase-independent, like the per-stage version -- for sizing, not
  // an exact prediction (process() returns the actual count).
  [[nodiscard]] std::size_t max_output_for(std::size_t n_in) const {
    std::size_t n = n_in;
    for (const auto &node: nodes_)
      n = node->max_output_for(n);
    return n;
  }

  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

private:
  // Type-erasure: a stage is reached only once per block, so a virtual call here
  // is negligible against a block of thousands of samples.
  struct Node {
    virtual ~Node() = default;
    virtual void prepare(std::size_t max_in) = 0;
    virtual std::span<const float> process(std::span<const float> in) = 0;
    virtual std::size_t max_output_for(std::size_t n_in) const = 0;
  };

  template<PipelineStage S>
  struct Holder final : Node {
    explicit Holder(S s) : stage{std::move(s)} {}
    void prepare(std::size_t max_in) override { stage.prepare(max_in); }
    std::span<const float> process(std::span<const float> in) override { return stage.process(in); }
    std::size_t max_output_for(std::size_t n_in) const override { return stage.max_output_for(n_in); }
    S stage;
  };

  std::vector<std::unique_ptr<Node>> nodes_;
};

} // namespace palindrome
