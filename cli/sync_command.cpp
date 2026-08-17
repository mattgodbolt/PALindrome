#include "sync_command.hpp"

#include "cli_util.hpp"
#include "palindrome/demod.hpp"
#include "palindrome/horizontal_sweep.hpp"
#include "palindrome/sync_separator.hpp"
#include "palindrome/vertical_sync.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace palindrome::cli {

namespace {

struct Pulse {
  std::size_t leading; // sample index of the leading edge
  std::size_t width; // samples from leading to trailing edge
};

// Collect every sync pulse (leading->trailing transition of the sliced bit).
std::vector<Pulse> find_pulses(std::span<const video::SyncSample> sliced) {
  std::vector<Pulse> pulses;
  bool prev = false;
  std::size_t leading = 0;
  for (std::size_t i = 0; i < sliced.size(); ++i) {
    const auto s = sliced[i].sync;
    if (s && !prev)
      leading = i;
    else if (!s && prev)
      pulses.push_back({leading, i - leading});
    prev = s;
  }
  return pulses;
}

// Mean and population standard deviation (divides by N) of a set of doubles.
struct Stats {
  double mean;
  double stddev;
};
Stats stats_of(std::span<const double> xs) {
  if (xs.empty())
    return {0.0, 0.0};
  double sum = 0.0;
  for (const double x: xs)
    sum += x;
  const auto mean = sum / static_cast<double>(xs.size());
  double sq = 0.0;
  for (const double x: xs)
    sq += (x - mean) * (x - mean);
  return {mean, std::sqrt(sq / static_cast<double>(xs.size()))};
}

// Text histogram of `values` across `buckets` even bins over [lo, hi].
void print_histogram(std::span<const double> values, double lo, double hi, int buckets, const std::string &unit) {
  if (values.empty() || buckets <= 0 || !(hi > lo)) {
    std::println("  (no data)");
    return;
  }
  std::vector<std::size_t> counts(static_cast<std::size_t>(buckets), 0);
  const double width = (hi - lo) / buckets;
  for (const double v: values) {
    auto b = static_cast<int>((v - lo) / width);
    b = std::clamp(b, 0, buckets - 1);
    ++counts[static_cast<std::size_t>(b)];
  }
  const auto peak = *std::ranges::max_element(counts);
  for (int b = 0; b < buckets; ++b) {
    const double bin_lo = lo + width * b;
    const auto c = counts[static_cast<std::size_t>(b)];
    const int bar = peak ? static_cast<int>(50 * c / peak) : 0;
    std::println("  {:6.2f}-{:6.2f} {} | {:5} {}", bin_lo, bin_lo + width, unit, c,
        std::string(static_cast<std::size_t>(bar), '#'));
  }
}

} // namespace

void SyncCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("sync", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Diagnose the sync chain: slice the composite and report pulse widths, spacing and lock")
          .add_argument(lyra::opt(input_mode_, "mode")["--input"](
              "Signal kind: rf (default) | composite (baseband CVBS, which skips the IF filter and detector). "
              "In composite mode --carrier does not apply: there is no carrier to resolve"))
          .add_argument(lyra::opt(carrier_, "hz")["--carrier"](
              "Carrier Hz (default: the metadata's vision_if_hz, or a signal scan if it has none)"))
          .add_argument(lyra::opt(composite_scale_, "volts")["--composite-scale"](
              "Composite: volts at input full scale. Declaring the source's real value matters here - a "
              "mis-scaled rail moves where the fixed slice cuts the sync edge, which changes the pulse widths "
              "this command reports and can make most of the 'equalising' count an artifact"))
          .add_argument(lyra::opt(composite_sync_v_, "volts")["--composite-sync"](
              "Composite: the source's sync amplitude in volts (default 0.3)"))
          .add_argument(lyra::opt(cutoff_, "hz")["--cutoff"]("Baseband low-pass cutoff Hz"))
          .add_argument(lyra::opt(decimate_, "n")["--decimate"](
              "Keep 1 sample per N inputs (0 = the mode's default: 2 for rf, 1 for composite)"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to inspect (e.g. corpus/alex_kidd)")));
}

int SyncCommand::run() const {
  const auto input = parse_input_mode("sync", input_mode_);
  if (!input)
    return 1;
  const bool composite = *input == InputMode::composite;
  if (composite && carrier_ > 0.0) {
    std::println(std::cerr, "sync: --carrier describes the RF front end; --input composite has no carrier");
    return 1;
  }
  const auto loaded = load_recording(recording_, {.carrier_override = carrier_, .input = *input});
  for (const auto &w: loaded.warnings)
    std::println(std::cerr, "sync: warning: {}", w);

  // To the composite envelope - through the vision front end for RF, or
  // straight onto the same rail by the affine map for baseband. Composite
  // defaults to no decimation, and the reason is stronger than "it does not
  // need it": decimating actively corrupts what this tool reports. The 127-tap
  // anti-alias FIR rings on the sync edges and manufactures roughly one
  // spurious narrow pulse per line, which lands straight in the equalising
  // count and inflates the jitter figure by an order of magnitude. Measured on
  // a 20 MS/s capture, /2 took the equalising count from 2235 to 70549 and the
  // jitter from 0.041 to 0.394 us. Use --decimate here only deliberately.
  std::vector<float> env;
  const std::size_t decimation = decimate_ != 0 ? decimate_ : (composite ? 1 : 2);
  EnvelopeOptions opts{.input = *input, .cutoff_hz = cutoff_, .decimation = decimation};
  if (composite_scale_ > 0.0)
    opts.composite.full_scale_volts = composite_scale_;
  if (composite_sync_v_ > 0.0)
    opts.composite.sync_amplitude_v = composite_sync_v_;
  const auto es =
      stream_envelope(loaded, opts, [&](std::span<const float> e) { env.insert(env.end(), e.begin(), e.end()); });
  for (const auto &w: es.warnings)
    std::println(std::cerr, "sync: warning: {}", w);
  if (env.empty()) {
    std::println(std::cerr, "sync: no samples read from {}", loaded.data_path.string());
    return 1;
  }

  const double rate = es.rate_hz;
  const double us_per_sample = 1e6 / rate;
  const double nominal_line_samples = rate / video::kNominalLineHz;

  video::SyncSeparator separator{video::SyncSeparatorConfig{.sample_rate_hz = rate}};
  separator.prepare(env.size());
  const auto sliced = separator.process(env);

  const auto pulses = find_pulses(sliced);

  std::println("recording: {} samples @ {:g} MS/s ({:.1f} ms), ~{:.1f} samples/line nominal", env.size(), rate / 1e6,
      static_cast<double>(env.size()) * us_per_sample / 1000.0, nominal_line_samples);
  std::println("separator sliced {} sync pulses", pulses.size());
  if (pulses.empty())
    return 0;

  // Width distribution, in microseconds. Line sync ~4.7 us, equalising ~2.35,
  // broad ~27 — three clusters the eye can separate straight off the histogram.
  std::vector<double> widths_us;
  widths_us.reserve(pulses.size());
  for (const auto &p: pulses)
    widths_us.push_back(static_cast<double>(p.width) * us_per_sample);

  std::println("\npulse width distribution:");
  print_histogram(widths_us, 0.0, 30.0, 15, "us");

  // Classify against the SAME width window the sweep accepts, so the diagnostic
  // reports what the sweep actually does rather than a second hard-coded guess.
  // The sweep gates on pulse_fraction/omega samples; at nominal that is
  // pulse_fraction * nominal_line_samples, which we convert to microseconds.
  const video::HorizontalSweepConfig sweep_cfg{.sample_rate_hz = rate};
  const double line_lo_us = sweep_cfg.min_pulse_fraction * nominal_line_samples * us_per_sample;
  const double line_hi_us = sweep_cfg.max_pulse_fraction * nominal_line_samples * us_per_sample;

  // Spacing of line-sync pulses: leading-edge to leading-edge, expressed in
  // lines. A tight cluster at 1.0 line with small spread is a clean lock; broad
  // spread is the wobble we're chasing.
  std::vector<double> line_gaps;
  std::optional<std::size_t> prev_leading;
  std::size_t n_line = 0;
  std::size_t n_eq = 0;
  std::size_t n_broad = 0;
  for (const auto &p: pulses) {
    const auto w = static_cast<double>(p.width) * us_per_sample;
    if (w < line_lo_us) {
      ++n_eq;
      continue;
    }
    if (w > line_hi_us) {
      ++n_broad;
      continue;
    }
    ++n_line;
    if (prev_leading)
      line_gaps.push_back(static_cast<double>(p.leading - *prev_leading) / nominal_line_samples);
    prev_leading = p.leading;
  }

  std::println("\nclassified (sweep accepts {:.2f}-{:.2f} us): {} line-sync, {} equalising (narrower), "
               "{} broad (wider)",
      line_lo_us, line_hi_us, n_line, n_eq, n_broad);

  // Restrict the spacing stats to gaps near one line, so half-line VBI gaps and
  // dropouts don't swamp the jitter measurement we actually care about. A
  // (0.5, 1.5) window is not tight enough on its own: an interlaced source's
  // half-line gap is 0.938 of a NOMINAL line and sits well inside it, so on a
  // 625/50 interlaced signal one gap per frame drags the mean ~0.01% low. That
  // is a fifth of the accuracy the interlace verdict below needs, and it always
  // pulls away from 312.5 - the tool would talk itself out of the very thing it
  // is measuring. So take a second pass over gaps within 2% of the first
  // estimate: relative, not absolute, so an off-nominal source keeps its set.
  std::vector<double> near_one;
  for (const double g: line_gaps)
    if (g > 0.5 && g < 1.5)
      near_one.push_back(g);
  std::vector<double> trimmed;
  if (!near_one.empty()) {
    const double coarse = stats_of(near_one).mean;
    for (const double g: near_one)
      if (std::abs(g - coarse) < 0.02 * coarse)
        trimmed.push_back(g);
  }
  const bool have_line = !trimmed.empty();
  // The same trim fixes the jitter figure, which the half-line gaps inflate by
  // an order of magnitude on an interlaced source.
  if (have_line) {
    const auto s = stats_of(trimmed);
    std::println("\nline-sync spacing (gaps near 1 line, n={} of {} kept):", trimmed.size(), near_one.size());
    std::println("  mean {:.4f} lines = {:.2f} samples = {:.3f} us", s.mean, s.mean * nominal_line_samples,
        s.mean * nominal_line_samples * us_per_sample);
    std::println("  stddev {:.4f} lines = {:.2f} samples = {:.3f} us (the line-to-line jitter)", s.stddev,
        s.stddev * nominal_line_samples, s.stddev * nominal_line_samples * us_per_sample);
  }

  // Vertical structure: the broad pulses cluster into one run per field's
  // vertical sync (5 broad pulses ~half a line apart). Group broad pulses that
  // sit within a few lines of each other; the spacing between runs is the field
  // period. Interlace should show as ~312.5 lines between runs.
  // The source's own line, shared by every "lines" figure below so the page
  // does not mix measured and nominal denominators.
  const double measured_line_samples = have_line ? stats_of(trimmed).mean * nominal_line_samples : nominal_line_samples;

  std::vector<std::size_t> broad_leadings;
  for (const auto &p: pulses)
    if (static_cast<double>(p.width) * us_per_sample > line_hi_us)
      broad_leadings.push_back(p.leading);

  std::vector<std::size_t> run_starts;
  const double run_break_samples = 5.0 * nominal_line_samples; // new run if > 5 lines since the last broad pulse
  std::optional<std::size_t> prev_broad;
  for (const std::size_t s: broad_leadings) {
    if (!prev_broad || static_cast<double>(s - *prev_broad) > run_break_samples)
      run_starts.push_back(s);
    prev_broad = s;
  }

  std::println("\nvertical sync: {} broad-pulse runs (expect one per field, ~{} for {} frames)", run_starts.size(),
      2 * (env.size() / static_cast<std::size_t>(nominal_line_samples * 625.0)),
      env.size() / static_cast<std::size_t>(nominal_line_samples * 625.0));
  if (run_starts.size() >= 2) {
    // In MEASURED lines, not nominal: a source off nominal line rate would
    // otherwise report 312.61 for a 312.5-line field and the reader has to do
    // the correction in their head. `measured_line_samples` comes from the
    // line-sync spacing above, so this is the source's own line.
    std::vector<double> field_lines;
    for (std::size_t i = 1; i < run_starts.size(); ++i)
      field_lines.push_back(static_cast<double>(run_starts[i] - run_starts[i - 1]) / measured_line_samples);
    // Median, not mean, because the first interval is routinely bogus: the
    // front end's cold start pins the rail at the sync tip until the first real
    // tip arrives, which reads as one spurious broad pulse at sample 0 and
    // becomes run_starts[0]. The median is robust to that and to any other
    // single bad run (a dropout merging two fields, a run split in half).
    auto sorted = field_lines;
    std::ranges::sort(sorted);
    const double median = sorted[sorted.size() / 2];
    const auto fs = stats_of(field_lines);
    // 312.5 lines between vertical syncs IS interlace - it measures the
    // half-line offset of vertical against line sync, which is the mechanism
    // rather than a proxy. Whole 312/313 alternation is a 625-line frame with
    // no half-line offset, so it is NOT interlaced, and the median hides the
    // alternation; the spacings printed below are where to see it.
    const std::string_view verdict = std::abs(median - 312.5) < 0.15   ? "INTERLACED (312.5)"
                                     : std::abs(median - 312.0) < 0.15 ? "non-interlaced (312)"
                                     : std::abs(median - 313.0) < 0.15 ? "non-interlaced (313)"
                                                                       : "non-standard";
    // Say which line the verdict was measured against, and admit it when there
    // was none: the fallback is the nominal line, which is exactly the error
    // this report exists to remove, and it would otherwise print as "measured".
    if (have_line)
      std::println(
          "  field period: median {:.3f} measured lines ({:+.3f} from 312.5) -> {}", median, median - 312.5, verdict);
    else
      std::println("  field period: median {:.3f} NOMINAL lines -> {} (UNRELIABLE: no line-sync spacing was "
                   "measurable, so this is not the source's own line)",
          median, verdict);
    std::println("  (mean {:.2f}, stddev {:.2f}; line {:.3f} samples)", fs.mean, fs.stddev, measured_line_samples);
    // A run start that hops half a line between fields fakes both verdicts, and
    // the tell is the broad-pulse count per run changing. Print the range.
    {
      std::vector<std::size_t> per_run(run_starts.size(), 0);
      if (run_starts.size() < 2)
        return 0;
      std::size_t r = 0;
      for (const std::size_t b: broad_leadings) {
        while (r + 1 < run_starts.size() && b >= run_starts[r + 1])
          ++r;
        ++per_run[r];
      }
      // Skip run 0 for the same reason the median does: the cold-start latch
      // makes it a run of one, which would flag every capture.
      const std::span<const std::size_t> settled{per_run.begin() + 1, per_run.end()};
      const auto [lo, hi] = std::ranges::minmax_element(settled);
      if (*lo != *hi)
        std::println("  broad pulses per run vary {}-{} (normal on an interlaced source - the two fields serrate "
                     "differently). Check the spacings below alternate by ~0.5 rather than cluster: if they "
                     "alternate, the run start is hopping half a line and the verdict above is wrong",
            *lo, *hi);
      else
        std::println("  broad pulses per run: {} (consistent, so the run start is a stable feature)", *lo);
    }
    std::println("  first few run spacings (lines): ");
    for (std::size_t i = 0; i < std::min<std::size_t>(field_lines.size(), 8); ++i)
      std::println("    {:.2f}", field_lines[i]);
  }

  // Run the vertical sync and report the field lock it settled on.
  video::VerticalSync vsync{video::VerticalSyncConfig{.sample_rate_hz = rate}};
  vsync.prepare(sliced.size());
  (void)vsync.process(sliced);
  const double field_hz = vsync.omega() * rate;
  std::println("  vertical flywheel: locked {} fields, {:.2f} Hz ({:.1f} lines/field, {:+.2f}% from 50 Hz)",
      vsync.detected_fields(), field_hz, (1.0 / vsync.omega()) / measured_line_samples,
      100.0 * (field_hz - video::kNominalFieldHz) / video::kNominalFieldHz);

  // Now run the sweep and report the lock it settled on.
  video::HorizontalSweep sweep{sweep_cfg};
  sweep.prepare(sliced.size());
  (void)sweep.process(sliced);
  const double locked_hz = sweep.omega() * rate;
  std::println(
      "\nsweep: accepted {} edges, rejected {}, locked to {:.2f} Hz ({:.2f} samples/line, {:+.2f}% from nominal)",
      sweep.accepted_edges(), sweep.rejected_edges(), locked_hz, rate / locked_hz,
      100.0 * (locked_hz - video::kNominalLineHz) / video::kNominalLineHz);
  return 0;
}

} // namespace palindrome::cli
