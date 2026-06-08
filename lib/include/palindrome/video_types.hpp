#pragma once

// The wire types of the video graph: the small POD samples that flow stage to
// stage, plus the nominal PAL timing. Kept in one place so each stage depends only
// on this shared vocabulary, not on its sibling stages.
namespace palindrome::video {

// PAL-B/G nominal timing (625 lines, 2:1 interlace at 50 fields/s). These are
// fixed facts of the standard, not tuning knobs — every config that needs a
// nominal line or field rate defaults from these.
inline constexpr double kNominalLineHz = 15625.0; // 625 lines × 25 frames/s
inline constexpr double kNominalFieldHz = 50.0;

// Output of the sync separator: just the sliced one-bit sync signal — true
// while the envelope sits in the sync region (above the slice level, since
// vision is negatively modulated so sync tips are the envelope peaks). True
// during EVERY sync pulse (line sync and the vertical-interval broad /
// equalising pulses alike); telling them apart is downstream's job. It is a
// struct, not a bare bool, so the sandcastle-style gating levels (burst gate,
// clamp, blanking) can join it later without changing the stage signature.
struct SyncSample {
  bool sync = false;
};

// Output of the horizontal sweep: the beam's horizontal position as a phase in
// [0, 1) (0 at the locked line start), and line_start true on the single sample
// where the sweep locks a new line. The envelope is NOT here — it rides the
// picture branch; the renderer joins the two rails by index.
struct BeamSample {
  float h_phase = 0.0f;
  bool line_start = false;
};

// Output of the vertical sync: the beam's vertical position as a phase in
// [0, 1) (0 at the locked field start), and field_start true on the single
// sample where a new field is locked. A second timing rail off the same sync
// bit the separator produces — it joins the horizontal rail at the renderer.
struct VSample {
  float v_phase = 0.0f;
  bool field_start = false;
};

// One decoded sample on the colour rail: the chroma-notched luma (still in
// envelope units — negatively modulated, so a *lower* value is whiter) plus the
// two colour-difference components recovered by synchronous demodulation. u and
// v are zero on a monochrome line (no burst lock) and on the grey rail.
struct ChromaSample {
  float luma = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
};

} // namespace palindrome::video
