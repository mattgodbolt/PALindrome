#include "palindrome/sigmf.hpp"

#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

#include <lyra/lyra.hpp>

namespace {

namespace sigmf = palindrome::sigmf;

// `info` accepts either a .sigmf-meta path or just the recording's basename
// (e.g. "corpus/alex_kidd"); resolve both to the metadata file.
std::filesystem::path resolve_meta(std::filesystem::path path) {
  if (path.extension() != ".sigmf-meta")
    path.replace_extension(".sigmf-meta");
  return path;
}

void print_field(std::string_view label, const std::optional<std::string> &value) {
  if (value)
    std::println("  {:<12}: {}", label, *value);
}

int run_info(const std::filesystem::path &input) {
  const auto meta_path = resolve_meta(input);
  sigmf::Metadata meta;
  try {
    meta = sigmf::load(meta_path);
  }
  catch (const sigmf::ParseError &e) {
    std::println(std::cerr, "info: {}", e.what());
    return 1;
  }

  const auto &g = meta.global;
  std::println("{}", meta_path.string());
  print_field("description", g.description);
  std::println("  {:<12}: {} ({})", "datatype", g.datatype, g.parsed_datatype);
  if (g.sample_rate)
    std::println("  {:<12}: {:g} MS/s", "sample rate", *g.sample_rate / 1e6);
  std::println("  {:<12}: {}", "version", g.version);
  print_field("author", g.author);
  print_field("recorder", g.recorder);
  print_field("hardware", g.hardware);

  // Sample count / duration come from the paired data file when it's present
  // (it may be a git-LFS pointer that hasn't been pulled).
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(sigmf::data_path_for(meta_path), ec);
  if (const auto bps = g.parsed_datatype.bytes_per_sample(); !ec && bps != 0) {
    const auto samples = bytes / bps;
    if (g.sample_rate)
      std::println("  {:<12}: {} ({:.3f} s)", "samples", samples, static_cast<double>(samples) / *g.sample_rate);
    else
      std::println("  {:<12}: {}", "samples", samples);
  }

  std::println("  {:<12}: {}", "captures", meta.captures.size());
  for (const auto &cap: meta.captures) {
    std::string detail;
    if (cap.frequency)
      detail += std::format(" {:g} MHz", *cap.frequency / 1e6);
    if (cap.datetime)
      detail += "  " + *cap.datetime;
    std::println("    @{}{}", cap.sample_start, detail);
  }

  std::println("  {:<12}: {}", "annotations", meta.annotations.size());
  for (const auto &ann: meta.annotations) {
    const auto end = ann.sample_count ? std::format("{}", ann.sample_start + *ann.sample_count) : "end";
    std::println("    [{} .. {}) {}", ann.sample_start, end, ann.label.value_or(""));
  }

  if (!g.extensions.empty()) {
    std::string names;
    for (const auto &ext: g.extensions)
      names += std::format("{}{} v{}", names.empty() ? "" : ", ", ext.name, ext.version);
    std::println("  {:<12}: {}", "extensions", names);
  }

  // The rx888 extension records the measured IF carriers; show them if present.
  const auto vision = meta.field<std::int64_t>("rx888:vision_if_hz");
  const auto chroma = meta.field<std::int64_t>("rx888:chroma_if_hz");
  const auto sound = meta.field<std::int64_t>("rx888:sound_if_hz");
  if (vision && chroma && sound)
    std::println("  {:<12}: vision {:.3f} / chroma {:.3f} / sound {:.3f} MHz", "carriers (IF)",
        static_cast<double>(*vision) / 1e6, static_cast<double>(*chroma) / 1e6, static_cast<double>(*sound) / 1e6);

  return 0;
}

int run_convert(std::string_view direction, const std::filesystem::path &input, const std::filesystem::path &output) {
  // The real PAL encode/decode surface isn't implemented yet; report intent.
  std::println("{}: not yet implemented", direction);
  std::println("  {} -> {}", input.empty() ? "<stdin>" : input.string(), output.empty() ? "<stdout>" : output.string());
  return 0;
}

} // namespace

int main(int argc, const char **argv) {
  bool show_help{};
  std::function<int()> action;

  std::filesystem::path encode_in, encode_out;
  std::filesystem::path decode_in, decode_out;
  std::filesystem::path info_in;

  auto cli = lyra::cli();
  cli.add_argument(lyra::help(show_help));

  cli.add_argument(lyra::command(
      "encode", [&](const lyra::group &) { action = [&] { return run_convert("encode", encode_in, encode_out); }; })
          .help("Encode an input into a PAL signal")
          .add_argument(lyra::opt(encode_out, "file")["-o"]["--output"]("Output file (defaults to stdout)"))
          .add_argument(lyra::arg(encode_in, "input")("Input to encode (defaults to stdin)")));

  cli.add_argument(lyra::command(
      "decode", [&](const lyra::group &) { action = [&] { return run_convert("decode", decode_in, decode_out); }; })
          .help("Decode a PAL signal back to an image")
          .add_argument(lyra::opt(decode_out, "file")["-o"]["--output"]("Output file (defaults to stdout)"))
          .add_argument(lyra::arg(decode_in, "input")("Input to decode (defaults to stdin)")));

  cli.add_argument(lyra::command("info", [&](const lyra::group &) { action = [&] { return run_info(info_in); }; })
          .help("Summarise a SigMF recording")
          .add_argument(
              lyra::arg(info_in, "recording")("Path to a .sigmf-meta file, or its basename (e.g. corpus/alex_kidd)")));

  const auto result = cli.parse({argc, argv});
  if (show_help) {
    std::cout << cli << '\n';
    return 0;
  }
  if (!result) {
    std::println(std::cerr, "Error in command line: {}", result.message());
    return 1;
  }
  if (!action) {
    // No subcommand given: show usage rather than doing nothing.
    std::cout << cli << '\n';
    return 1;
  }
  return action();
}
