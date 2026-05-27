#pragma once

// PAL_RESTRICT expands to the compiler's non-standard restrict qualifier, or to
// nothing where it isn't available. Use it on *function-local* pointers lifted
// out of a span/Buffer at the top of a hot loop -- a span or container can't
// carry restrict across an API boundary, so this is the escape hatch that lets
// the optimiser assume the input and output don't alias and vectorise the loop.
#if defined(__GNUC__) || defined(__clang__)
#define PAL_RESTRICT __restrict
#else
#define PAL_RESTRICT
#endif
