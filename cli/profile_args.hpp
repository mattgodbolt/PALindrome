#pragma once

#include <string>
#include <vector>

namespace palindrome::cli {

// Pre-parse expansion of `render`'s --profile/--profiles-dir: the outcome is
// either a rewritten argument list or an error to print. Profiles are applied
// before lyra ever parses, because lyra errors on a duplicated flag - so
// "explicit flags win" is implemented by omitting a profile key whose flag the
// user already typed, and the parser only ever sees one flat argument list.
struct ProfileExpansion {
  std::vector<std::string> args; // argv[0] first, as lyra::args expects
  std::vector<std::string> profiles; // resolved paths, for parse-error context
  std::string error; // non-empty: print it and exit 1
};

// A no-op (args returned untouched) unless the command is `render` - profiles
// map knob names to render flags, so no other subcommand can consume them.
// Knob keys invert mechanically to flags (underscores to dashes, "--"
// prefixed); a number becomes "--flag value", a string likewise, true a bare
// "--flag", false nothing. Later profiles override earlier ones for the same
// key. An unknown key becomes an unknown flag for lyra to reject.
[[nodiscard]] ProfileExpansion expand_profiles(std::vector<std::string> args);

} // namespace palindrome::cli
