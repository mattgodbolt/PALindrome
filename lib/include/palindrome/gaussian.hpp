#pragma once

#include <cstddef>
#include <vector>

// Separable Gaussian beam-spot splat kernels, used by the phosphor screen to
// deposit each sample as a round 2-D spot (one kernel per axis). Pure functions,
// so the "weights normalise to unit sum" charge-conservation contract is testable.
namespace palindrome::dsp {

// Half-width of the splat in cells: covers the Gaussian out to 2.5 sigma. sigma 0
// (a bare point) gives radius 0.
[[nodiscard]] std::size_t splat_radius_for(double sigma);

// One axis of the beam-spot Gaussian, tabulated per sub-pixel-fraction bin: each
// of `bins` rows holds 2*radius+1 weights, normalised to sum 1, centred on that
// row's fractional offset from the splat's base cell. sigma 0 (radius 0)
// degenerates to a single unit weight (a bare point). Built and normalised in
// double for accuracy, then stored float (a normalised weight in [0,1] has float
// precision to spare, and the deposit multiplies in float). The deposit multiplies
// a row weight by a column weight for the separable 2-D spot.
[[nodiscard]] std::vector<float> gaussian_splat_lut(double sigma, std::size_t radius, std::size_t bins);

} // namespace palindrome::dsp
