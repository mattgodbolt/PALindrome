#include "palindrome/restrict_ptr.hpp"

#include <array>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

using palindrome::restrict_ptr;

TEST_CASE("restrict_ptr behaves like the pointer it wraps") {
  std::array<float, 4> a{10.0f, 11.0f, 12.0f, 13.0f};
  restrict_ptr<float> p{a.data()};

  CHECK(p.get() == a.data());
  CHECK(static_cast<bool>(p));
  CHECK(*p == 10.0f);
  CHECK(p[2] == 12.0f);

  p[1] = 99.0f; // writes through the wrapper
  CHECK(a[1] == 99.0f);

  ++p;
  CHECK(*p == 99.0f); // now points at a[1]
  CHECK(p[1] == 12.0f);
  CHECK(p - restrict_ptr<float>{a.data()} == 1);

  const restrict_ptr<float> q = p + 2; // a[3]
  CHECK(*q == 13.0f);
  CHECK((q - p) == 2);
}

TEST_CASE("restrict_ptr default-constructs to null") {
  restrict_ptr<int> p;
  CHECK(p.get() == nullptr);
  CHECK_FALSE(static_cast<bool>(p));
  CHECK(p == restrict_ptr<int>{});
}

TEST_CASE("restrict_ptr carries const correctly") {
  constexpr std::array<float, 3> a{1.0f, 2.0f, 3.0f};
  restrict_ptr<const float> p{a.data()};
  CHECK(p[2] == 3.0f);
  static_assert(std::is_same_v<decltype(p[0]), const float &>);
}
