# nx::simd

Data-parallel types and kernels; the [foundation
README](../../../../README.md) covers the rest of the library.

`simd.h` is C++26's `<simd>` (`std::simd`, as in the current working draft)
for C++20, in `nx::simd`: `basic_vec`/`vec` and `basic_mask`/`mask` with every
operator, conversion and constructor (broadcast, converting, generator, range,
complex parts); the flags, `alignment`, `rebind` and `resize` traits; checked
and partial loads and stores, gathers and scatters; `reduce` and the mask
reductions; `select`, `min`, `max`, `minmax`, `clamp`; static and dynamic
`permute`, `compress`, `expand`, `chunk`, `cat`, `iota`; the `<cmath>`,
`<bit>` and `<complex>` functions. Names and semantics follow the standard,
so code moves to `std::simd` by changing the namespace. `interleave.h`,
`pipeline.h`, `linalg.h` and `nn.h` are extensions with no `std::simd`
counterpart.

```cpp
#include "core/foundation/simd/simd.h"

namespace sd = nx::simd;

void scale(std::span<float> values, float factor) {
  using V = sd::vec<float>;
  size_t i = 0;
  for (; i + V::size() <= values.size(); i += V::size())
    sd::unchecked_store(sd::unchecked_load<V>(values.subspan(i)) * factor,
                        values.subspan(i));
  sd::partial_store(sd::partial_load<V>(values.subspan(i)) * factor,
                    values.subspan(i));
}
```

## Backends

| Backend | Used for |
| --- | --- |
| GCC and Clang vector extensions | Every target those compilers build: SSE to AVX-512 on x86, NEON and SVE on AArch64. Mask movemasks use SSE2/AVX2 or NEON directly. |
| NEON and SVE intrinsics | AArch64 under GCC and Clang, over the vector extensions: across-vector reductions, rounding, bit counting and reversal with NEON; with SVE, predicated partial and masked loads and stores, compress, same-width gather and scatter, and SVE2 `bit_compress`/`bit_expand`. |
| SSE2, SSE4.1 and AVX2 intrinsics | MSVC on x64. `/arch:AVX` enables the SSE4.1 paths; `/arch:AVX2` makes 32-byte registers native and adds FMA. |
| Arrays of registers | Widths that fill several registers (`vec<float, 12>` is three SSE registers). |
| Plain arrays | Widths that fill no register exactly, other compilers (MSVC on ARM64 among them), and every type under `NX_SIMD_FORCE_FIXED`, which the tests use as the reference. |

The tests build once per backend (`tests/CMakeLists.txt`). `nx.natvis` shows
vector lanes and masks in the Visual Studio debugger.

## Semantics that bite

- `vec<T>` has the native width of the compile flags: 4 floats with SSE or
  NEON, 8 with AVX2, and `__ARM_FEATURE_SVE_BITS / 32` with a fixed SVE
  vector length (`-msve-vector-bits=256`). Without a fixed length SVE works
  on 16-byte vectors. Translation units built with different flags see
  different types, so keep `vec<T>` out of interfaces shared between them.
- Implicit broadcasts follow the standard: only value-preserving ones, so
  `v * 2.f` compiles and `v * 2` does not for `vec<float>`.
  `std::integral_constant<int, 2>{}` stands in for C++26's `std::cw<2>`.
- Vectors, masks and the algorithms work in constant expressions; the
  `<cmath>` and `<complex>` functions do not, as C++20's `<cmath>` is not
  `constexpr`.
- Widths go up to 64 lanes. `flag_aligned` and `flag_overaligned<N>` are
  checked in debug builds only: unaligned loads cost the same on every
  supported CPU.
- Under MSVC, vectors narrower than a register (`vec<u8, 4>`) are plain
  arrays, so a widening conversion from them runs element by element.
- `vec<std::complex<T>>` keeps real and imaginary parts in separate
  registers; multiplication is the plain formula, division is
  `std::complex`'s.

## Determinism and accuracy

Except for the `fast_` functions, `precision::fast` and `gemm`'s `fused`,
results are the same bits on every backend and compiler.

- Exact operations (arithmetic, comparisons, `sqrt`, rounding, `fma`,
  `fmin`, `fmax`, classification) run on whole registers.
- For `float`, `exp`, `exp2`, `expm1`, `log`, `log2`, `log1p`, `tanh` and
  `erf` run on whole registers too (`float_math.h`). They are not `<cmath>`'s
  bits, but they use no FMA, and every special value is as `<cmath>` has it.
  Over all 2^32 inputs the largest errors are 0.96 ulp (`exp`), 1.08
  (`exp2`), 1.45 (`expm1`), 0.87 (`log`), 1.87 (`log2`), 1.44 (`log1p`),
  1.33 (`tanh`) and 1.46 (`erf`). `tools/fit_float_math.py` regenerates their coefficients.
- Extensions for `float`: `sigmoid` (2.4 ulp) and the fast forms `fast_exp`,
  `fast_tanh`, `fast_sigmoid` and `fast_erf`, within a relative 1e-5 (3e-5
  for `fast_erf`) of the exact value for normal results. The fast forms use
  FMA where the build has it, so their last bits vary between targets, and
  may flush a subnormal result to 0.
- Every other transcendental and special function, and every `double` one,
  applies the scalar `<cmath>` function per element, so its results equal
  the scalar ones bit for bit. The special functions exist where the
  standard library has them (`__cpp_lib_math_special_functions`; not
  libc++).

## Interleaved data

`interleave.h` loads K-element tuples, such as stereo frames, RGBA pixels or
xyz positions, as one vector per component, and stores them back:

```cpp
#include "core/foundation/simd/interleave.h"

using V = sd::vec<float>;
const auto [left, right] = sd::unchecked_load_interleaved<V, 2>(frames, 2 * V::size());
sd::unchecked_store_interleaved(std::array{right, left}, swapped, 2 * V::size());
```

- Lane i of part k is `first[i * K + k]`. The unchecked forms touch exactly
  `K * V::size()` elements; `partial_load_interleaved` and
  `partial_store_interleaved` take the first n, loading zeros past them and
  storing nothing past them.
- Two components are one shuffle per pair of registers (`uzp`/`zip` on
  NEON, unpacks and shuffles on x86); four are two rounds of two. Other
  counts, and plain-array vectors, go lane by lane.
- Elements are not converted: the range holds the vector's element type.

## Pipelines

`pipeline.h` builds a loop once from element-wise stages and runs it over
ranges:

```cpp
#include "core/foundation/simd/pipeline.h"

const auto to_unit = sd::pipeline([](auto p) {
  return sd::rebind_t<float, decltype(p)>(p) * (1.f / 255.f);
});
to_unit(pixels, sd::out(unit)); // u8 in, float out

const auto [lo, hi] = sd::pipeline([](auto v) { return std::tuple(v, v); },
                                   sd::fold(sd::minimum, sd::maximum))(values);

const size_t visible = sd::pipeline(sd::keep_if([](auto r) { return r > 0.f; }))(
    radii, sd::out(kept));

// Gain ramp over stereo frames, in place.
sd::pipeline([=](auto l, auto r, auto f) {
  const auto g = start + step * f;
  return std::tuple(l * g, r * g);
})(sd::interleaved<2>(frames), sd::index_as<float>,
   sd::out(sd::interleaved<2>(frames)));
```

- A stage takes the previous values as `vec`s, the inputs first, and returns
  a `vec` or a `std::tuple` of them; generic lambdas let the pipeline choose
  the width. `keep_if(pred)` drops lanes. `fold(reducers...)` ends a
  pipeline with one reducer per value: `sum`, `product`, `minimum`,
  `maximum`, the `std::plus<>`-style operations, or `reducer(op, identity)`.
  A pipeline without a fold takes `out(ranges...)` last and returns how many
  elements it wrote. A pipeline is itself a stage, so parts are built once
  and reused.
- An input is a range, `interleaved<K>(range)` for K values per element, or
  `index` (the element's index as `int`; `index_as<T>` for another type),
  which takes no data and needs a sized input beside it. `out` takes ranges
  and `interleaved<K>(range)` the same way.
- The stages run as one loop: nothing is stored between stages, and several
  outputs or folds take one pass over the data.
- The widest element type in the flow fills one register, so `u8` in and
  `float` out run `vec<float>::size()` lanes. `lanes<N>` overrides that and
  `unroll<N>` sets the chunks in flight: 2 by default, and 8, 4 or 2
  accumulators for folds of one, two or more values.
- The last partial chunk is padded with its own first element, and the
  index with its own first index: stages never see a padding zero to divide
  by.
- Element types convert at the edges by the `std::simd` rules; a narrowing
  output takes `out(flag_convert, ranges...)`.
- Inputs have equal sizes. An output holds at least that many elements and
  is one of the inputs or disjoint from all of them.
- A fold's order depends on the lane count and unroll: the same on every run
  of a build, but different in the last bits from a scalar loop and between
  targets.
- Pipelines run in constant expressions. MSVC keeps fold accumulators in
  memory, so folds of several values run slower there than under Clang.
- `sd::pipeline` is a function: as a class, `sd::pipeline(f)(in, sd::out(o));`
  would declare a variable `f`.

## Linear algebra

`linalg.h` has kernels over spans of `float` or `double`, in
`nx::simd::linalg`: `dot`, `sum`, `sum_squares`, `max`, `axpy` (`y += a x`),
`scale`, `add`, `mul`, `gemv` (`y = W x + b`, `W` row-major), `gemv_t`
(`y = W^T x`) and `gemm`.

```cpp
#include "core/foundation/simd/linalg.h"

namespace la = nx::simd::linalg;

la::gemv(weights, input, bias, output); // output.size() rows of weights
const float norm = std::sqrt(la::sum_squares(output));
```

- A sum keeps 16 lanes: lane k adds elements k, k + 16, k + 32, ... in turn,
  and the lanes then fold as (k, k + 8), (k, k + 4), (k, k + 2), (k, k + 1).
  `gemv`'s row i is `dot(row i, x) + b[i]` bit for bit; `gemv_t` adds the
  scaled rows in turn. The element-wise kernels round once per operation.
- `max` returns NaN if any element is NaN, and -inf for an empty span.
- The element-wise kernels allow their output to be one of their inputs.
  `gemv_t`'s output must not overlap its inputs.
- `gemm(m, n, k, a, lda, b, ldb, c, ldc, options)` multiplies row-major
  matrices: `C = A B`, or `C += A B` with `options.accumulate`. Element
  (i, j) starts at 0 (or C) and adds `a[i][p] b[p][j]` for p = 0, 1, ... in
  turn, so every kernel, CPU and thread count gives the same bits.
  `options.fused` fuses each multiply-add where the kernel's CPU can:
  faster, and then the bits depend on the kernel.
- `gemm` packs blocks of A and B (GotoBLAS-style) and runs a register-tile
  micro-kernel: 4 x 8 on SSE2, 6 x 8 on NEON, and 6 x 16 with AVX2 and FMA,
  which `gemm_avx2.cpp` compiles for those alone and which runs when CPUID
  and the OS allow it. That file uses raw intrinsics only, so no inline
  function compiled for AVX2 can replace a baseline one at link time.
  `options.kernel = gemm_kernel::baseline` forces the baseline kernel.
- `options.pool` splits C's rows across a thread pool for products of about
  2^26 multiply-adds or more.

## Neural-network kernels

`nn.h` has float kernels over spans, in `nx::simd::nn`: `softmax`,
`log_softmax`, `logsumexp`, `rmsnorm`, `layernorm`, `gelu` (the erf form),
`gelu_tanh` (GPT-2's), `silu` and `swiglu` (`silu(gate) up`).

```cpp
#include "core/foundation/simd/nn.h"

namespace nn = nx::simd::nn;

nn::rmsnorm(hidden, weight, 1e-5f, hidden);
nn::softmax(scores, scores);
nn::gelu(mlp, mlp, nn::precision::fast);
```

- Sums run in linalg's 16-lane order. `precision::exact` (the default)
  computes exp, tanh, erf and sigmoid with the register-wide float math;
  `precision::fast` uses their `fast_` forms, whose last bits depend on the
  target's FMA.
- `softmax` and `log_softmax` subtract the maximum first, so large inputs do
  not overflow. A -inf element (a masked score) gets 0, or -inf from
  `log_softmax`; a NaN, a +inf or a row of -inf alone makes the whole row
  NaN. `logsumexp` returns -inf for an empty row or one of -inf alone and
  +inf with any +inf.
- `layernorm` sums the variance as (x - mean)^2 after the mean, not as
  E[x^2] - mean^2, and returns the mean and 1 / sqrt(var + eps) it
  normalized with. Its gamma and beta, and `rmsnorm`'s gamma, may be empty.
- Every kernel allows its output to be one of its inputs.
