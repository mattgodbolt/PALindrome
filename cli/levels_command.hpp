#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <lyra/lyra.hpp>

namespace palindrome::cli {

// `palindrome levels <recording-or-raw>` — the amplitude counterpart of `sync`
// (which measures timing). It slices line syncs out of the raw normalised
// samples and reports the sync-referenced levels — tip, blanking, sync
// amplitude, burst, active picture — segmented by APL step so a cycling test
// capture (white/black/colour screens) reports each screen separately, then
// derives suggested flag values. Deliberately measured on the samples BEFORE
// the affine map: the report feeds the very flags (--composite-sync,
// --contrast, --saturation) that the map would otherwise bake in, so it has
// to speak the source's units.
class LevelsCommand {
public:
  void add_to(lyra::cli &cli, std::function<int()> &action);

private:
  int run() const;
  std::filesystem::path recording_;
  std::string input_mode_{"composite"}; // only composite measures yet; rf reports "not yet supported"
  std::string sample_format_{"s16"}; // raw input only; a recording's metadata wins
  double sample_rate_{0.0}; // >0 = the input is headerless raw samples at this rate
};

} // namespace palindrome::cli
