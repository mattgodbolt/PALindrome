#include "palindrome/pipeline_run.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <stdexcept>
#include <vector>

namespace {

// A push source over a fixed list of float blocks.
auto block_source(const std::vector<std::vector<float>> &blocks) {
  return [&blocks](const auto &emit) {
    for (const auto &b: blocks)
      emit(std::span<const float>{b});
  };
}

} // namespace

TEST_CASE("pipe::run threaded matches serial and keeps block order") {
  const std::vector<std::vector<float>> blocks{{1, 2, 3}, {4, 5}, {6, 7, 8, 9}, {10}, {}, {11, 12}};

  // transform: copy the block into an owned vector and double it.
  const auto doubler = [] {
    return palindrome::pipe::transform<std::vector<float>>(3, [](std::span<const float> in, std::vector<float> &out) {
      out.assign(in.begin(), in.end());
      for (auto &x: out)
        x *= 2.0f;
    });
  };

  const auto collect = [&](bool threaded) {
    std::vector<float> result;
    palindrome::pipe::run(threaded, 3, block_source(blocks), doubler(),
        palindrome::pipe::sink([&](const std::vector<float> &v) { result.insert(result.end(), v.begin(), v.end()); }));
    return result;
  };

  std::vector<float> expected;
  for (const auto &b: blocks)
    for (float x: b)
      expected.push_back(x * 2.0f);

  const auto serial = collect(false);
  const auto threaded = collect(true);
  CHECK(serial == expected); // serial is correct...
  CHECK(threaded == serial); // ...and threaded is identical and in order
}

TEST_CASE("pipe::run propagates a stage exception in both modes") {
  const std::vector<std::vector<float>> blocks(20, std::vector<float>{1.0f});

  const auto run_throwing = [&](bool threaded) {
    int seen = 0;
    palindrome::pipe::run(threaded, 2, block_source(blocks),
        palindrome::pipe::transform<std::vector<float>>(
            2, [](std::span<const float> in, std::vector<float> &out) { out.assign(in.begin(), in.end()); }),
        palindrome::pipe::sink([&](const std::vector<float> &) {
          if (++seen == 5)
            throw std::runtime_error{"boom"};
        }));
  };

  CHECK_THROWS_AS(run_throwing(false), std::runtime_error); // serial: the throw unwinds out of run()
  CHECK_THROWS_AS(run_throwing(true), std::runtime_error); // threaded: captured on the worker, rethrown after the drain
}

TEST_CASE("pipe::Pool rejects a non-positive in-flight count") {
  CHECK_THROWS_AS((palindrome::pipe::Pool<int>{0}), std::invalid_argument);
  CHECK_THROWS_AS((palindrome::pipe::Pool<int>{-1}), std::invalid_argument);
  CHECK_NOTHROW((palindrome::pipe::Pool<int>{1}));
}
