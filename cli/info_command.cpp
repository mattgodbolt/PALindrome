#include "info_command.hpp"

#include "cli_util.hpp"
#include "palindrome/sigmf.hpp"

#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

namespace palindrome::cli {

namespace {
namespace sigmf = palindrome::sigmf;

void print_field(std::string_view label, const std::optional<std::string> &value) {
  if (value)
    std::println("  {:<12}: {}", label, *value);
}
} // namespace

void InfoCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("info", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Summarise a SigMF recording")
          .add_argument(lyra::arg(recording_, "recording")(
              "Path to a .sigmf-meta file, or its basename (e.g. corpus/alex_kidd)")));
}

int InfoCommand::run() const {
  const auto meta_path = resolve_meta(recording_);
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

} // namespace palindrome::cli
