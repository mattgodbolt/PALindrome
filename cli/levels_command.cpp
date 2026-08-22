#include "levels_command.hpp"

#include "cli_util.hpp"
#include "palindrome/video_types.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <format>
#include <iostream>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace palindrome::cli {

namespace {

// One line's sync-referenced levels, in normalised source units (full scale is
// [-1, 1), so a u8 capture's count 46 reads as 46/128 = 0.359).
struct LineLevels {
  std::size_t leading; // sample index of the sync leading edge
  double tip;
  double blank;
  double burst_amp;
  double active_mean;
  double active_peak;
};

// Percentile over a copy: nth_element mutates, and the line buffer has to
// survive for the later windows. `scratch` is reused across calls.
double percentile_of(std::span<const float> xs, double p, std::vector<double> &scratch) {
  assert(!xs.empty());
  scratch.assign(xs.begin(), xs.end());
  const auto k = static_cast<std::ptrdiff_t>(p * static_cast<double>(scratch.size() - 1) + 0.5);
  std::nth_element(scratch.begin(), scratch.begin() + k, scratch.end());
  return scratch[static_cast<std::size_t>(k)];
}

// Interquartile mean - the mean of the middle half - reordering `v`. As
// outlier-robust as a median, but it resolves below one quantisation step: a
// u8 capture's samples are exact integer counts, so a window MEDIAN is one
// too, and a real 0.1-count difference between segments then prints as a
// full-count cliff (the 46-vs-47 sync reading this replaced).
double iq_mean(std::span<double> v) {
  assert(!v.empty());
  double sum = 0.0;
  if (v.size() < 4) {
    for (const double x: v)
      sum += x;
    return sum / static_cast<double>(v.size());
  }
  const auto q1 = static_cast<std::ptrdiff_t>(v.size() / 4);
  const auto q3 = static_cast<std::ptrdiff_t>(3 * v.size() / 4);
  std::nth_element(v.begin(), v.begin() + q1, v.end());
  std::nth_element(v.begin() + q1, v.begin() + q3, v.end());
  for (auto it = v.begin() + q1; it != v.begin() + q3; ++it)
    sum += *it;
  return sum / static_cast<double>(q3 - q1);
}

double iq_mean_of(std::span<const float> xs, std::vector<double> &scratch) {
  scratch.assign(xs.begin(), xs.end());
  return iq_mean(scratch);
}

// A slice level that lands between the sync tip and blanking whatever the
// screen shows. The tip is the low percentile — sync occupies ~7% of every
// field's samples, so p1 always sits inside it — and a quarter of the way up
// to the bright percentile stays below blanking at both extremes: an all-white
// field puts p99 about 2.4 sync amplitudes above the tip (a quarter of that is
// 0.6 of the way up the sync edge), an all-black field puts it just above
// blanking at the burst peaks (a quarter is ~0.35 of the way up). Derived per
// field, not once from the head: an AC-coupled capture's DC rides the APL, so
// a screen change moves tip and blanking together by a large fraction of the
// sync amplitude and a single global slice can sit above another screen's
// blanking - measured on the cycling test capture, which loses every white
// field to exactly that.
float pick_threshold(std::span<const float> chunk, std::vector<double> &scratch) {
  const auto tip = percentile_of(chunk, 0.01, scratch);
  const auto top = percentile_of(chunk, 0.99, scratch);
  return static_cast<float>(tip + 0.25 * (top - tip));
}

// Slice line syncs off the raw samples at a fixed threshold and measure each
// line's levels in windows referenced to the sync leading edge. Streaming: fed
// spans, state carried across, like the library stages — but this is a
// diagnostic, so it accumulates every accepted line for the report.
class LineAnalyser {
public:
  explicit LineAnalyser(double rate_hz) {
    const auto at = [&](double us) { return static_cast<std::size_t>(us * 1e-6 * rate_hz); };
    tip_lo_ = at(1.2);
    tip_hi_ = at(3.8);
    burst_lo_ = at(5.8);
    burst_hi_ = at(7.2);
    // The textbook back porch starts right after the sync pulse, but the burst
    // rides it — at ~5.6-7.4 us from the leading edge by the book, and real
    // sources put it outside that (a bench capture ran ~4.0-8.6 us) — so
    // blanking is measured late on the porch, after even an early-starting,
    // late-ending burst has died, yet still ahead of nominal active at 10.5 us.
    blank_lo_ = at(8.8);
    blank_hi_ = at(10.2);
    active_lo_ = at(12.0);
    active_hi_ = at(62.0);
    // Accept only line-sync-width pulses (~4.7 us): equalising (~2.35) and
    // broad (~27) pulses start collections too, and this is where they fail.
    width_lo_ = at(3.5);
    width_hi_ = at(6.0);
    // A sync edge stays below the slice for microseconds; a saturated chroma
    // trough dips under it for under half a subcarrier cycle. Requiring a
    // sustained run tells them apart - without it, a colour-bars screen whose
    // troughs reach the sync tip loses every line to false edges.
    qualify_ = at(1.0);
  }

  void set_threshold(float threshold) { threshold_ = threshold; }

  void process(std::span<const float> x) {
    for (const float s: x)
      step(s);
  }

  [[nodiscard]] std::span<const LineLevels> lines() const { return lines_; }
  [[nodiscard]] std::size_t rejected() const { return rejected_; }
  [[nodiscard]] std::size_t samples() const { return index_; }

private:
  void step(float x) {
    const bool below = x < threshold_;
    if (collecting_) {
      line_.push_back(x);
      if (width_ == 0 && !below) {
        width_ = line_.size() - 1; // samples spent below the slice: the pulse width
        if (width_ < qualify_) { // a chroma trough or noise blip, not a sync edge
          collecting_ = false;
          line_.clear();
        }
      }
      if (collecting_ && line_.size() > active_hi_) {
        finalize();
        collecting_ = false;
        line_.clear();
      }
    }
    if (!collecting_ && below && !prev_below_) {
      collecting_ = true;
      width_ = 0;
      leading_ = index_;
      line_.push_back(x);
    }
    prev_below_ = below;
    ++index_;
  }

  void finalize() {
    if (width_ < width_lo_ || width_ > width_hi_) {
      ++rejected_;
      return;
    }
    const auto window = [&](std::size_t lo, std::size_t hi) {
      return std::span<const float>{line_}.subspan(lo, hi - lo);
    };
    LineLevels m{};
    m.leading = leading_;
    m.tip = iq_mean_of(window(tip_lo_, tip_hi_), scratch_);
    // The burst amplitude as RMS x sqrt(2) of the window about its own mean:
    // at ~4.5 samples per subcarrier cycle a half peak-to-peak is biased by
    // where the samples land on the sine, and a single spike owns it outright.
    const auto burst = window(burst_lo_, burst_hi_);
    double bsum = 0.0;
    for (const float v: burst)
      bsum += static_cast<double>(v);
    const double bmean = bsum / static_cast<double>(burst.size());
    double bsq = 0.0;
    for (const float v: burst) {
      const double d = static_cast<double>(v) - bmean;
      bsq += d * d;
    }
    m.burst_amp = std::numbers::sqrt2 * std::sqrt(bsq / static_cast<double>(burst.size()));
    m.blank = iq_mean_of(window(blank_lo_, blank_hi_), scratch_);
    const auto active = window(active_lo_, active_hi_);
    double sum = 0.0;
    for (const float v: active)
      sum += static_cast<double>(v);
    m.active_mean = sum / static_cast<double>(active.size());
    // Near-peak, not max: a percentile shrugs off isolated noise spikes but
    // still lands in the brightest bar of a test pattern. Saturated chroma is
    // wider than a spike, so its peaks DO reach this - which is measurement,
    // not error: those excursions are what the source really swings to.
    m.active_peak = percentile_of(active, 0.95, scratch_);
    lines_.push_back(m);
  }

  float threshold_ = 0.0f;
  std::size_t qualify_{};
  std::size_t tip_lo_{};
  std::size_t tip_hi_{};
  std::size_t burst_lo_{};
  std::size_t burst_hi_{};
  std::size_t blank_lo_{};
  std::size_t blank_hi_{};
  std::size_t active_lo_{};
  std::size_t active_hi_{};
  std::size_t width_lo_{};
  std::size_t width_hi_{};
  std::size_t index_ = 0; // global sample counter
  bool prev_below_ = false;
  bool collecting_ = false;
  std::size_t leading_ = 0; // global index of the current collection's leading edge
  std::size_t width_ = 0; // 0 until the signal first rises back above the slice
  std::vector<float> line_; // samples since the leading edge
  std::vector<double> scratch_;
  std::vector<LineLevels> lines_;
  std::size_t rejected_ = 0;
};

// Lines grouped into one nominal field period, carrying the field's mean
// active level above blanking — the APL trace the segmentation walks.
// Blanking-referenced, not absolute: an AC-coupled capture's DC slides
// against the APL, which compresses the absolute step between screens to a
// fraction of the picture-level step and merges adjacent segments.
struct FieldApl {
  std::size_t index; // field number since the start of the input
  std::size_t first_line; // index into the lines vector
  std::size_t n_lines = 0;
  double mean = 0.0;
};

// Indices into the FieldApl vector, inclusive.
struct Segment {
  std::size_t first;
  std::size_t last;
};

// Interquartile means across a segment's lines - robust like a median, so the
// vertical interval's blanked-but-line-synced lines and any transition junk
// cost nothing, but with sub-quantisation resolution (see iq_mean).
struct SegmentStats {
  double tip;
  double blank;
  double sync;
  double burst;
  double mean_above;
  double peak_above;
};

double iq_mean_over(std::span<const LineLevels> lines, double (*proj)(const LineLevels &)) {
  std::vector<double> v;
  v.reserve(lines.size());
  for (const auto &m: lines)
    v.push_back(proj(m));
  return iq_mean(v);
}

SegmentStats stats_over(std::span<const LineLevels> lines) {
  return {
      .tip = iq_mean_over(lines, [](const LineLevels &m) { return m.tip; }),
      .blank = iq_mean_over(lines, [](const LineLevels &m) { return m.blank; }),
      .sync = iq_mean_over(lines, [](const LineLevels &m) { return m.blank - m.tip; }),
      .burst = iq_mean_over(lines, [](const LineLevels &m) { return m.burst_amp; }),
      .mean_above = iq_mean_over(lines, [](const LineLevels &m) { return m.active_mean - m.blank; }),
      .peak_above = iq_mean_over(lines, [](const LineLevels &m) { return m.active_peak - m.blank; }),
  };
}

// A new segment starts on a per-field APL jump this big (normalised units).
// Big enough that noise and interlace never split a static screen, small
// enough that black-to-dark-colour steps still register.
constexpr double kAplJump = 0.05;
// A real screen holds for many fields; anything shorter is the transition
// itself (a screen switch mid-field leaves one field at an in-between APL).
constexpr std::size_t kMinSegmentFields = 8;

} // namespace

void LevelsCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  cli.add_argument(lyra::command("levels", [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help("Measure signal amplitudes - sync tip, blanking, burst, picture levels per APL segment - and "
                "suggest flag values; the amplitude counterpart of sync, which measures timing")
          .add_argument(lyra::opt(input_mode_, "mode")["--input"](
              "Signal kind: composite (default - baseband CVBS, measured raw) | rf (not yet supported)"))
          .add_argument(lyra::opt(sample_format_, "fmt")["--sample-format"](
              "Raw input: s16 (default, signed 16-bit) | u8 (unsigned 8-bit, what a CX2388x card hands over "
              "through cxadc). A recording carries its own format in the metadata"))
          .add_argument(lyra::opt(sample_rate_, "hz")["--sample-rate"](
              "Raw input sample rate Hz. Giving it treats the argument as headerless raw samples (or stdin when "
              "the argument is - or absent) instead of a recording"))
          .add_argument(lyra::arg(recording_, "recording")("Recording to measure (e.g. corpus/wb3), or a raw "
                                                           "file with --sample-rate")));
}

int LevelsCommand::run() const {
  const auto input = parse_input_mode("levels", input_mode_);
  if (!input)
    return 1;
  if (*input == InputMode::rf) {
    std::println(std::cerr,
        "levels: --input rf is not yet supported - demodulate to baseband first and measure with --input composite");
    return 1;
  }

  std::optional<SampleFormat> format;
  if (sample_format_ == "s16")
    format = SampleFormat::s16;
  else if (sample_format_ == "u8")
    format = SampleFormat::u8;
  else {
    std::println(std::cerr, "levels: --sample-format must be one of: s16, u8");
    return 1;
  }

  const bool raw = sample_rate_ > 0.0;
  const bool from_stdin = recording_.empty() || recording_ == "-";
  std::optional<LoadedRecording> loaded;
  if (!raw) {
    if (from_stdin) {
      std::println(std::cerr, "levels: reading raw samples from stdin needs --sample-rate (and --sample-format)");
      return 1;
    }
    loaded = load_recording(recording_, {.input = *input});
    for (const auto &w: loaded->warnings)
      std::println(std::cerr, "levels: warning: {}", w);
  }
  const double rate = loaded ? loaded->sample_rate_hz : sample_rate_;
  // The burst estimator wants a couple of samples per subcarrier cycle (and
  // below ~9 MS/s the subcarrier does not even clear Nyquist), the porch
  // windows a handful of samples to trim; below ~10 MS/s the microscope has
  // no lens.
  if (rate < 1e7) {
    std::println(
        std::cerr, "levels: sample rate {:g} Hz is too low to resolve the measurement windows (wants 10 MS/s+)", rate);
    return 1;
  }

  // Buffer one field period at a time and derive the slice threshold from
  // that chunk alone before analysing it (see pick_threshold for why per
  // field), still a single pass so stdin works the same as a file.
  LineAnalyser analyser{rate};
  std::vector<double> scratch;
  std::vector<float> chunk;
  const auto chunk_samples = static_cast<std::size_t>(rate / video::kNominalFieldHz);
  chunk.reserve(chunk_samples);
  bool got_samples = false;
  // A line in flight across a chunk boundary is judged by two thresholds -
  // harmless: one line per 20 ms, and they only differ when the screen just
  // changed, whose transition field the segmentation drops anyway.
  const auto flush = [&] {
    if (chunk.empty())
      return;
    got_samples = true;
    analyser.set_threshold(pick_threshold(chunk, scratch));
    analyser.process(chunk);
    chunk.clear();
  };
  const auto feed = [&](std::span<const float> x) {
    while (!x.empty()) {
      const auto take = std::min(x.size(), chunk_samples - chunk.size());
      chunk.insert(chunk.end(), x.begin(), x.begin() + static_cast<std::ptrdiff_t>(take));
      x = x.subspan(take);
      if (chunk.size() == chunk_samples)
        flush();
    }
  };
  if (loaded)
    stream_real_blocks(loaded->data_path, loaded->format, feed);
  else if (from_stdin)
    stream_real_stdin(*format, feed);
  else
    stream_real_blocks(recording_, *format, feed);
  flush();
  if (!got_samples) {
    std::println(std::cerr, "levels: no samples read");
    return 1;
  }

  const auto lines = analyser.lines();
  const double total_s = static_cast<double>(analyser.samples()) / rate;
  std::println("input: {:g} MS/s, {} line syncs measured, {} non-line pulses rejected", rate / 1e6, lines.size(),
      analyser.rejected());
  if (lines.empty()) {
    std::println(std::cerr, "levels: no line syncs found - is this really baseband composite?");
    return 1;
  }
  std::println("units: normalised full scale ([-1, 1); a u8 capture's count/128, an s16 capture's value/32768)");

  // The APL trace: per-field mean of the per-line active means, bucketed by
  // nominal field period (no vertical lock needed for a level average).
  const double samples_per_field = rate / video::kNominalFieldHz;
  std::vector<FieldApl> fields;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto f = static_cast<std::size_t>(static_cast<double>(lines[i].leading) / samples_per_field);
    if (fields.empty() || fields.back().index != f)
      fields.push_back({.index = f, .first_line = i});
    fields.back().mean += lines[i].active_mean - lines[i].blank;
    ++fields.back().n_lines;
  }
  for (auto &f: fields)
    f.mean /= static_cast<double>(f.n_lines);

  // Each field is compared against the CURRENT SEGMENT's running mean, not
  // just the previous field: a screen switch that lands mid-field leaves one
  // field at an in-between APL, and consecutive-field diffs then see two
  // half-jumps that can each duck the threshold - halving the effective
  // sensitivity on a coin flip and merging distinct screens.
  std::vector<Segment> segments{{0, 0}};
  double seg_sum = fields[0].mean;
  std::size_t seg_n = 1;
  for (std::size_t k = 1; k < fields.size(); ++k) {
    if (std::abs(fields[k].mean - seg_sum / static_cast<double>(seg_n)) > kAplJump) {
      segments.push_back({k, k});
      seg_sum = fields[k].mean;
      seg_n = 1;
    }
    else {
      segments.back().last = k;
      seg_sum += fields[k].mean;
      ++seg_n;
    }
  }
  const auto is_transition = [](const Segment &s) { return s.last - s.first + 1 < kMinSegmentFields; };
  const auto transitions = static_cast<std::size_t>(std::ranges::count_if(segments, is_transition));
  std::erase_if(segments, is_transition);
  if (segments.empty()) {
    std::println(std::cerr, "levels: no segment held for {} fields - the APL never settles", kMinSegmentFields);
    return 1;
  }
  std::println("segments: {} APL step{} over {:.1f} s{}", segments.size(), segments.size() == 1 ? "" : "s", total_s,
      transitions > 0 ? std::format(" ({} short transition{} skipped)", transitions, transitions == 1 ? "" : "s")
                      : std::string{});

  const auto lines_of = [&](const Segment &s) {
    const auto begin = fields[s.first].first_line;
    const auto end = s.last + 1 < fields.size() ? fields[s.last + 1].first_line : lines.size();
    return lines.subspan(begin, end - begin);
  };

  // Brightest by MEAN, not by near-peak: a saturated colour-bars screen's
  // chroma peaks overshoot true peak white and would win the peak contest,
  // then masquerade as "white" in the contrast suggestion below.
  std::optional<std::size_t> brightest;
  std::vector<SegmentStats> stats;
  stats.reserve(segments.size());
  for (std::size_t i = 0; i < segments.size(); ++i) {
    stats.push_back(stats_over(lines_of(segments[i])));
    if (!brightest || stats[i].mean_above > stats[*brightest].mean_above)
      brightest = i;
  }

  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto &seg = segments[i];
    const auto &st = stats[i];
    const auto seg_lines = lines_of(seg);
    const double t0 = static_cast<double>(fields[seg.first].index) / video::kNominalFieldHz;
    const double t1 = static_cast<double>(fields[seg.last].index + 1) / video::kNominalFieldHz;
    std::println("\nsegment {}: {:.2f}-{:.2f} s ({} fields, {} lines){}", i + 1, t0, t1, seg.last - seg.first + 1,
        seg_lines.size(), brightest && *brightest == i && segments.size() > 1 ? " - brightest" : "");
    std::println("  sync tip {:+.3f}, blanking {:+.3f}, sync amplitude {:.3f}", st.tip, st.blank, st.sync);
    std::println("  burst amplitude {:.3f} ({:.2f}x sync)", st.burst, st.burst / st.sync);
    std::println("  active mean {:+.3f} above blanking ({:.2f}x sync), near-peak {:+.3f} ({:.2f}x sync)", st.mean_above,
        st.mean_above / st.sync, st.peak_above, st.peak_above / st.sync);
  }

  // Sync amplitude over EVERY measured line, not per segment: it is the one
  // level the picture content cannot move, which is what makes it the anchor
  // the suggestions divide by.
  const double sync_all = iq_mean_over(lines, [](const LineLevels &m) { return m.blank - m.tip; });
  const double burst_all = iq_mean_over(lines, [](const LineLevels &m) { return m.burst_amp; });
  if (!(sync_all > 0.0)) {
    std::println(std::cerr, "levels: measured sync amplitude is not positive - no suggestions");
    return 1;
  }

  std::println("\nsuggestions (measured starting points, not gospel):");
  std::println("  --composite-sync {:.3f} (the measured sync amplitude; in volts when full scale is the default "
               "--composite-scale 1.0)",
      sync_all);
  const double white_over_sync = stats[*brightest].peak_above / sync_all;
  // A capture of nothing but blanking still has a brightest segment; dividing
  // by its near-zero (under noise, even negative) picture level would print
  // an infinite or negative contrast with a straight face.
  constexpr double kMinPictureOverSync = 0.1;
  if (white_over_sync < kMinPictureOverSync)
    std::println("  brightest segment shows no picture content - no contrast suggestion");
  else {
    const double nominal_white_over_sync =
        (video::kNominalFullScaleVolts - video::kNominalSyncVolts) / video::kNominalSyncVolts;
    const double white_ratio = white_over_sync / nominal_white_over_sync;
    std::println("  white/sync {:.2f} vs the standard's {:.2f} (brightest segment): white reaches {:.2f}x nominal, "
                 "so the contrast pot wants ~{:.2f} (1/{:.2f}) if that segment really shows peak white",
        white_over_sync, nominal_white_over_sync, white_ratio, 1.0 / white_ratio, white_ratio);
    if (const double peak_over_mean = stats[*brightest].peak_above / stats[*brightest].mean_above; peak_over_mean > 2.5)
      std::println("  note: that near-peak is {:.1f}x the segment's active mean - the peak may be a chroma "
                   "excursion, not white",
          peak_over_mean);
  }
  // Nominal burst is +-half a sync amplitude about blanking, so burst
  // amplitude over sync should read 0.5. The ACC divides chroma by the
  // measured burst, so an oversize burst UNDERSATURATES the decode when the
  // picture's chroma does not share the excess.
  const double burst_over_sync = burst_all / sync_all;
  if (burst_over_sync < 0.2)
    std::println("  burst/sync {:.2f}: effectively monochrome (that \"burst\" is mostly noise) - no saturation "
                 "suggestion",
        burst_over_sync);
  else {
    // 0.085 is render's --agc sync-tip saturation default (see
    // render_command::decoder_config; --agc adaptive uses 0.17 and would
    // scale that instead).
    const double burst_ratio = burst_over_sync / 0.5;
    std::println("  burst/sync {:.2f} vs the standard's 0.50: burst is {:.2f}x nominal. If the picture chroma is "
                 "nominal, saturation likely wants roughly render's default scaled by that ({:.3f})",
        burst_over_sync, burst_ratio, 0.085 * burst_ratio);
  }
  return 0;
}

} // namespace palindrome::cli
