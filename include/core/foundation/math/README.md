# nx::math

Vector math under Slang's names, for C++ that shares data and formulas with
shaders. The [foundation README](../../../../README.md) covers the rest of
the library.

`math.h` holds, in `nx::math`: `float2`-`float4`, `int`, `uint`, `double` and
`bool` vectors (`vec<T, N>`), matrices `float2x2` to `float4x4` and the
non-square shapes (`mat<T, R, C>`), `quat`, HLSL's functions (`dot`,
`cross`, `normalize`, `lerp`, `saturate`, `frac`, `rsqrt`, `select`, `mul`,
...), and transform builders (`translation`, `rotation`, `perspective_rh_zo`,
`look_at_rh`, ...). `names.h` puts the type names in the global namespace,
as `foundation.h` does `f32`, so a struct C++ and Slang share reads the same
in both.

```cpp
#include "core/foundation/math/math.h"

using namespace nx::math;

float4x4 view_proj = mul(perspective_rh_zo(radians(60.f), aspect, 0.1f, 100.f),
                         look_at_rh(eye, target, float3(0.f, 1.f, 0.f)));
float3 p = transform_point(view_proj, float3(1.f, 2.f, 3.f));
```

Code writes the global type names (`float3`) and qualifies functions as
`nx::math::dot`: locals named `length` or `dot` are common, so calls do not
rely on argument-dependent lookup.

## Why not glm

One math library, the one the shaders are named after, so C++ and Slang read
alike. Release builds gain little (glm was 1-2% of a profiled frame); debug
builds and compile times gain more.

## Storage and meaning

- Storage is glm's: `float3` is 12 bytes, every type is 4-aligned, and a
  matrix is its columns in order, so `float4x4` has `glm::mat4`'s bytes and
  `float3x4` (three rows, four columns) `glm::mat4x3`'s.
- Meaning is Slang's: `float3x4` has three rows, products are `mul(m, v)`
  and `mul(a, b)`, `*` is per component. Where glm and Slang read the same
  code differently there is nothing to call, so code ported from either
  fails to compile instead of changing meaning: no `m[i]` (use `m.row(r)` or
  `m.columns[c]`), no matrix `operator*`, no scalar or vector matrix
  constructors (use `from_rows`, `from_columns`, `identity`), no
  four-scalar `quat` constructor (glm's takes w first; use `quat(xyz, w)`).
- `==` compares whole values; `less`, `equal` and the others compare
  components into a `bool2`-`bool4`.

## Results

- Results equal glm's bit for bit where both are built from `+`, `-`, `*`,
  `/` and `sqrt`: the 4x4 products, `inverse`, `transpose`, quaternion
  products and rotations, conversions and the camera builders sum in glm's
  order.
- Results are the same on every compiler and backend: the headers turn off
  Clang's fusing of `a * b + c` (it would fuse wherever FMA exists, AVX2 and
  every AArch64) for their own code only. GCC fuses only outside ISO mode
  (`-std=gnu++20`).

## Differences from glm that compile

- `round` rounds halves to even, as HLSL does; glm rounds them away from
  zero. Use `std::round` for glm's result. An unqualified `round(f)` on a
  scalar finds the C library's.
- `min(a, b)` is `a < b ? a : b` and `max(a, b)` is `a > b ? a : b`, NaN
  included, on every path, so `min` returns `b` when either is NaN.
- `dot` of `float4` sums `(x + y) + (z + w)` everywhere; glm under MSVC sums
  left to right.
- `normalize` of a zero vector gives NaNs, as in glm.

## Implementation

- `f32x4` (`register.h`) is four floats in a register with the operations
  the types are built from: SSE2 on x86-64, NEON on AArch64 with GCC and
  Clang, plain arrays elsewhere and under `NX_MATH_FORCE_SCALAR`. Each
  operation is one intrinsic or a short fixed sequence, so debug builds stay
  fast; the 4x4, affine and quaternion products, `inverse` and `transpose`
  use it, while per-component operations are plain code.
- The headers include only `core/config.h` and small standard headers, not
  `foundation.h`.
- The tests (`tests/math/`) build once per backend; their determinism case
  hashes a fixed workload that every backend must match.
