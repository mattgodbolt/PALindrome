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

  std::vector<std::string> specs;
  std::string dir{"profiles"};
  std::vector<std::string> rest;
  rest.reserve(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string value;
    if (match_flag(args, i, "--profile", value, out.error)) {
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
    if (!std::filesystem::exists(path)) {
      out.error = std::format("render: profile {}: no such file", path.string());
      return out;
    }
    std::ifstream in{path};
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
    if (data.contains("input") && data["input"].is_string() && data["input"].get<std::string>() != user_input) {
      out.error = std::format("render: profile {}: a {} profile, but --input is {}", path.string(),
          data["input"].get<std::string>(), user_input);
      return out;
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

  std::vector<std::string> tokens;
  for (const auto &[key, v]: merged) {
    auto flag = std::format("--{}", key);
    std::ranges::replace(flag, '_', '-');
    // Explicit flags beat profiles - and lyra rejects a duplicated flag, so
    // the profile's copy must be omitted, not merely ordered after.
    const auto user_has = std::ranges::any_of(rest, [&](const std::string &t) {
      return t == flag || (t.size() > flag.size() && t.starts_with(flag) && t[flag.size()] == '=');
    });
    if (user_has)
      continue;
    if (v.is_boolean()) {
      if (v.get<bool>())
        tokens.push_back(std::move(flag));
    }
    else if (v.is_string()) {
      tokens.push_back(std::move(flag));
      tokens.push_back(v.get<std::string>());
    }
    else if (v.is_number_integer()) {
      tokens.push_back(std::move(flag));
      tokens.push_back(std::format("{}", v.get<long long>()));
    }
    else {
      tokens.push_back(std::move(flag));
      tokens.push_back(std::format("{}", v.get<double>()));
    }
  }

  // Splice the profile-derived flags directly after the subcommand token, in
  // front of everything the user typed, so lyra sees one flat render command.
  out.args.assign(rest.begin(), rest.begin() + 2);
  out.args.insert(out.args.end(), tokens.begin(), tokens.end());
  out.args.insert(out.args.end(), rest.begin() + 2, rest.end());
  return out;
}

} // namespace palindrome::cli
