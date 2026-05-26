#pragma once

#include <filesystem>
#include <functional>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome encode|decode <input>` — placeholder for the PAL conversion that
// isn't implemented yet. One class serves both directions.
class ConvertCommand {
public:
  enum class Direction { Encode, Decode };
  explicit ConvertCommand(Direction direction) : direction_{direction} {}

  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  Direction direction_;
  std::filesystem::path input_;
  std::filesystem::path output_;
};

} // namespace palindrome::cli
