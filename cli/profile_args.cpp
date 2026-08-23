#include "profile_args.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace palindrome::cli {

namespace {

// "--flag value" or "--flag=value" at args[i]; advances i past a consumed
// value token. Missing-value errors are reported by the caller via `error`.
bool match_flag(const std::vector<std::string> &args, std::size_t &i, std::string_view flag, std::string &value,
    std::string &error) {
  const std::string_view a{args[i]};
  if (a == flag) {
    if (i + 1 == args.size()) {
      error = std::format("render: {} needs a value", flag);
      return false;
    }
    value = args[++i];
    return true;
  }
  if (a.size() > flag.size() && a.starts_with(flag) && a[flag.size()] == '=') {
    value = std::string{a.substr(flag.size() + 1)};
    return true;
  }
  return false;
}

// A bare name means <dir>/<name>.json; anything already shaped like a path
// (a slash or a .json suffix) is used as-is - the same rule as the tools.
std::filesystem::path resolve_profile(const std::string &spec, const std::string &dir) {
  if (spec.contains('/') || spec.ends_with(".json"))
    return spec;
  return std::filesystem::path{dir} / std::format("{}.json", spec);
}

} // namespace

ProfileExpansion expand_profiles(std::vector<std::string> args) {
  ProfileExpansion out;
  if (args.size() < 2 || args[1] != "render") {
    out.args = std::move(args);
    return out;
  }
  // Help wins before any profile I/O, as it does everywhere else in lyra; the
  // untouched --profile tokens land in render's help-only bindings.
  if (std::ranges::any_of(args, [](const std::string &a) { return a == "--help" || a == "-h" || a == "-?"; })) {
    out.args = std::move(args);
    return out;
  }

  std::vector<std::string> specs;
  bool profile_seen = false;
  std::string dir{"profiles"};
  std::vector<std::string> rest;
  rest.reserve(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string value;
    if (match_flag(args, i, "--profile", value, out.error)) {
      profile_seen = true;
      // Comma-separable as well as repeatable, exactly as the tools accept.
      for (const auto piece: std::views::split(value, ','))
        if (!piece.empty())
          specs.emplace_back(std::string_view{piece});
    }
    else if (match_flag(args, i, "--profiles-dir", value, out.error))
      dir = value;
    else if (out.error.empty())
      rest.push_back(std::move(args[i]));
    if (!out.error.empty())
      return out;
  }
  if (profile_seen && specs.empty()) {
    out.error = "render: --profile needs a profile name";
    return out;
  }
  if (specs.empty()) {
    out.args = std::move(rest);
    return out;
  }

  // The mismatch check needs the mode render will end up with, so read it the
  // same way lyra will; the default matches render_command's. A trailing
  // valueless --input is left for lyra to diagnose.
  std::string user_input{"rf"};
  for (std::size_t i = 0; i < rest.size(); ++i) {
    std::string value;
    std::string scan_error;
    if (match_flag(rest, i, "--input", value, scan_error))
      user_input = value;
  }

  // Merge in order: later profiles override earlier for the same key. Types
  // are checked per file so the error can name the file that carried the key.
  std::map<std::string, nlohmann::json> merged;
  for (const auto &spec: specs) {
    const auto path = resolve_profile(spec, dir);
    // is_regular_file with an error_code: a directory here would otherwise
    // sail into the parse, whose stream failure is not a json::parse_error.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
      out.error = std::format(
          "render: profile {}: {}", path.string(), std::filesystem::exists(path, ec) ? "not a file" : "no such file");
      return out;
    }
    std::ifstream in{path};
    if (!in.is_open()) {
      out.error = std::format("render: profile {}: cannot open", path.string());
      return out;
    }
    nlohmann::json data;
    try {
      data = nlohmann::json::parse(in);
    }
    catch (const nlohmann::json::parse_error &e) {
      out.error = std::format("render: profile {}: {}", path.string(), e.what());
      return out;
    }
    if (!data.is_object() || (data.contains("values") && !data["values"].is_object())) {
      out.error = std::format("render: profile {}: not a profile ({{description, input, values}})", path.string());
      return out;
    }
    if (data.contains("input")) {
      if (!data["input"].is_string()) {
        out.error = std::format("render: profile {}: 'input' must be a string", path.string());
        return out;
      }
      if (data["input"].get<std::string>() != user_input) {
        out.error = std::format("render: profile {}: a {} profile, but --input is {}", path.string(),
            data["input"].get<std::string>(), user_input);
        return out;
      }
    }
    if (data.contains("values")) {
      for (const auto &[key, v]: data["values"].items()) {
        if (!(v.is_boolean() || v.is_string() || v.is_number())) {
          out.error = std::format("render: profile {}: {} must be a number, string or true/false", path.string(), key);
          return out;
        }
        merged.insert_or_assign(key, v);
      }
    }
    out.profiles.push_back(path.string());
  }

  for (const auto &[key, v]: merged) {
    auto flag = std::format("--{}", key);
    std::ranges::replace(flag, '_', '-');
    // Explicit flags beat profiles - and lyra rejects a duplicated flag, so
    // the profile's copy must be omitted, not merely ordered after.
    const auto user_has = std::ranges::any_of(rest, [&](const std::string &t) {
      const auto matches = [&](std::string_view f) {
        return t == f || (t.size() > f.size() && t.starts_with(f) && t[f.size()] == '=');
      };
      // --color is lyra's alias for --colour (the only aliased knob): either
      // spelling must silence the profile's copy, or lyra sees it twice.
      return matches(flag) || (flag == "--colour" && matches("--color"));
    });
    if (user_has)
      continue;
    // Single --flag=value tokens: immune to value-vs-option ambiguity, and
    // they read as one unit in the parse-failure context line.
    if (v.is_boolean()) {
      if (v.get<bool>())
        out.tokens.push_back(std::move(flag));
    }
    else if (v.is_string())
      out.tokens.push_back(std::format("{}={}", flag, v.get<std::string>()));
    else
      // dump(): ints verbatim, doubles shortest-round-trip - no get<> wrap
      // on a huge unsigned value quietly becoming a negative sentinel.
      out.tokens.push_back(std::format("{}={}", flag, v.dump()));
  }

  out.args.assign(rest.begin(), rest.begin() + 2);
  out.args.insert(out.args.end(), out.tokens.begin(), out.tokens.end());
  out.args.insert(out.args.end(), rest.begin() + 2, rest.end());
  return out;
}

} // namespace palindrome::cli
