#pragma once

#include <filesystem>
#include <functional>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome info <recording>` — print a summary of a SigMF recording.
class InfoCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  std::filesystem::path recording_;
};

} // namespace palindrome::cli
