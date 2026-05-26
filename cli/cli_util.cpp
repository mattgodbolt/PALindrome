#include "cli_util.hpp"

namespace palindrome::cli {

std::filesystem::path resolve_meta(std::filesystem::path path) {
  if (path.extension() != ".sigmf-meta")
    path.replace_extension(".sigmf-meta");
  return path;
}

} // namespace palindrome::cli
