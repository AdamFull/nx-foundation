#pragma once

// nx::simd: the C++26 <simd> library (std::simd) for C++20. Include this
// header; the others are its parts.

#include "core/foundation/simd/algorithm.h"
#include "core/foundation/simd/bit.h"
#include "core/foundation/simd/complex.h"
#include "core/foundation/simd/math.h"
#include "core/foundation/simd/vec.h"

namespace nx {
using simd::clamp;
using simd::max;
using simd::min;
} // namespace nx
