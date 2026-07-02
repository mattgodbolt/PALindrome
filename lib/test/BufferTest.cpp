#include "palindrome/buffer.hpp"

#include <cstdint>
#include <span>
#include <utility>

#include <catch2/catch_test_macros.hpp>

namespace {
using palindrome::Buffer;

bool aligned_64(const void *p) { return reinterpret_cast<std::uintptr_t>(p) % 64 == 0; }
} // namespace

TEST_CASE("Buffer starts empty") {
  Buffer<float> b;
  CHECK(b.size() == 0);
  CHECK(b.capacity() == 0);
  CHECK(b.empty());
}

TEST_CASE("a never-reserved Buffer exposes null storage without tripping assume_aligned") {
  Buffer<float> b;
  CHECK(b.data() == nullptr);
  CHECK(b.view().data() == nullptr);
  CHECK(b.view().empty());
  CHECK(b.write_n(0).empty()); // zero-length write on null storage is fine
}

TEST_CASE("reserve allocates 64-byte-aligned storage") {
  Buffer<float> b;
  b.reserve(100);
  CHECK(b.capacity() >= 100);
  CHECK(b.size() == 0); // reserve doesn't change the logical size
  CHECK(aligned_64(b.data()));
}

TEST_CASE("write_n sets the size and returns a writable aligned span") {
  Buffer<float> b{128};
  const std::span<float> w = b.write_n(64);
  REQUIRE(w.size() == 64);
  CHECK(aligned_64(w.data()));
  CHECK(b.size() == 64);

  for (std::size_t k = 0; k < w.size(); ++k)
    w[k] = static_cast<float>(k);
  const std::span<const float> v = b.view();
  REQUIRE(v.size() == 64);
  CHECK(v[0] == 0.0f);
  CHECK(v[63] == 63.0f);
}

TEST_CASE("write_n throws when it exceeds reserved capacity") {
  Buffer<float> b{16};
  CHECK_THROWS_AS(b.write_n(17), std::length_error);
  CHECK(b.write_n(16).size() == 16); // exactly at capacity is fine
}

TEST_CASE("push extends by one, preserving prior elements, and throws when full") {
  Buffer<float> b{3};
  b.push() = 1.0f;
  b.push() = 2.0f;
  b.push() = 3.0f;
  REQUIRE(b.size() == 3);
  CHECK(b.view()[0] == 1.0f);
  CHECK(b.view()[1] == 2.0f);
  CHECK(b.view()[2] == 3.0f);
  CHECK_THROWS_AS(b.push(), std::length_error);
  CHECK(b.size() == 3); // a failed push leaves the buffer untouched
}

TEST_CASE("clear resets the size but keeps the capacity") {
  Buffer<float> b{32};
  (void)b.write_n(20);
  b.clear();
  CHECK(b.size() == 0);
  CHECK(b.capacity() >= 32); // storage retained for reuse
}

TEST_CASE("reserve preserves contents when growing") {
  Buffer<float> b{4};
  const std::span<float> w = b.write_n(4);
  for (std::size_t k = 0; k < 4; ++k)
    w[k] = static_cast<float>(k + 1);

  b.reserve(1000);
  CHECK(b.capacity() >= 1000);
  CHECK(b.size() == 4); // size unchanged by the grow
  CHECK(aligned_64(b.data()));
  const std::span<const float> v = b.view();
  CHECK(v[0] == 1.0f);
  CHECK(v[3] == 4.0f);
}

TEST_CASE("reserve to a smaller capacity is a no-op") {
  Buffer<float> b{256};
  const std::size_t cap = b.capacity();
  b.reserve(8);
  CHECK(b.capacity() == cap);
}

TEST_CASE("Buffer is movable and leaves the source empty") {
  Buffer<float> a{64};
  (void)a.write_n(10);
  const float *data = a.data();

  Buffer<float> b{std::move(a)};
  CHECK(b.size() == 10);
  CHECK(b.data() == data); // storage transferred, not copied
  CHECK(a.capacity() == 0); // NOLINT(bugprone-use-after-move) -- checking moved-from state
  CHECK(a.size() == 0);

  Buffer<float> c;
  c = std::move(b);
  CHECK(c.size() == 10);
  CHECK(c.data() == data);
}
