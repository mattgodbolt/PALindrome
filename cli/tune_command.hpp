#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome tune <recording>` — a spike of an interactive knob tuner. It
// serves a tiny web page on localhost with a slider per decode/CRT parameter;
// changing one re-runs the whole decode (no caching — the live path will be
// fast anyway) and hands the page the full per-field frame sequence to scrub
// and play back. Browse to http://localhost:<port>/.
class TuneCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  std::filesystem::path recording_;
  double carrier_{0.0};
  std::uint16_t port_{8080};
};

} // namespace palindrome::cli
