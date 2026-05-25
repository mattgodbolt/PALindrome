#include "palindrome/palindrome.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("greeting mentions PALindrome") { CHECK(palindrome::greeting().contains("PALindrome")); }
