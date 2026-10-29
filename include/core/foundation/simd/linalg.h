#pragma once

#include "core/foundation/simd/algorithm.h"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

// Vector and matrix-vector kernels over spans of float or double. Every sum
// runs in one order on every backend, so results are the same bits
// everywhere: lane k of a 16-lane accumulator sums elements k, k + 16, k + 32,
// ... in turn, and the 16 lanes fold as (l + l+8), then (l + l+4), (l + l+2),
// (l + l+1).
namespace nx::simd::linalg {

namespace impl {

inline constexpr int lanes = 16;

// The 16 lanes live in C native registers: MSVC spills a vec<T, 16> to the
// stack on every iteration.
template <class T> struct shape {
  using V = vec<T>;
  static constexpr int W = V::size();
  static constexpr int C = lanes / W;
  static_assert(W <= lanes && lanes % W == 0);
};

template <class T> NX_FORCE_INLINE vec<T> load(const T *p) {
  return unchecked_load<vec<T>>(p, vec<T>::size());
}
/// The native vector at p with only the first `left` (possibly <= 0) lanes
/// read, the rest 0.
template <class T>
NX_FORCE_INLINE vec<T> load_part(const T *p, ptrdiff_t left) {
  if (left >= ptrdiff_t(vec<T>::size()))
    return load(p);
  return partial_load<vec<T>>(p, std::max<ptrdiff_t>(left, 0));
}

template <class T, int N, class Op> T fold_lanes(const vec<T, N> &v, Op op) {
  if constexpr (N == 1) {
    return v[0];
  } else {
    const auto h = chunk<N / 2>(v);
    return fold_lanes<T, N / 2>(op(h[0], h[1]), op);
  }
}

/// The 16-lane tree over s[0..C).
template <class T, class Op> T fold(vec<T> (&s)[shape<T>::C], Op op) {
  for (int h = shape<T>::C / 2; h >= 1; h /= 2)
    for (int c = 0; c < h; ++c)
      s[c] = op(s[c], s[c + h]);
  return fold_lanes<T, shape<T>::W>(s[0], op);
}

inline constexpr auto plus = [](const auto &a, const auto &b) { return a + b; };
inline constexpr auto larger = [](const auto &a, const auto &b) {
  return simd::max(a, b);
};

template <class T> using registers = vec<T>[shape<T>::C];

/// term at the native vector c of the 16-lane block at i.
template <int c, class T, class Term, class... P>
NX_FORCE_INLINE vec<T> term_at(Term term, size_t i, const P *...x) {
  return term(load(x + i + c * shape<T>::W)...);
}
template <int c, class T, class Term, class... P>
NX_FORCE_INLINE vec<T> term_tail(Term term, size_t i, ptrdiff_t left,
                                 const P *...x) {
  return term(load_part(x + i + c * shape<T>::W, left - c * shape<T>::W)...);
}

// The steps over registers unroll in folds: through a loop over its index
// MSVC keeps a register array in memory.
template <class T, class Term, int... c, class... P>
NX_FORCE_INLINE void accumulate_block(registers<T> &s, Term term, size_t i,
                                      std::integer_sequence<int, c...>,
                                      const P *...x) {
  ((s[c] += term_at<c, T>(term, i, x...)), ...);
}
template <class T, class Term, int... c, class... P>
NX_FORCE_INLINE void
accumulate_tail(registers<T> &s, Term term, size_t i, ptrdiff_t left,
                std::integer_sequence<int, c...>, const P *...x) {
  ((s[c] += term_tail<c, T>(term, i, left, x...)), ...);
}

/// The sum of term(x0[i], x1[i]...) over i < n in the 16-lane order. term
/// captures nothing and the pointers are plain locals: MSVC reloads anything
/// reached through a reference after every store.
template <class T, class Term, class... P>
NX_FORCE_INLINE T accumulate(size_t n, Term term, const P *...x) {
  constexpr int C = shape<T>::C;
  registers<T> s;
  size_t i = 0;
  if constexpr (C <= 4) {
    // Named registers: MSVC reloads even an unrolled array's elements from
    // the stack after storing them.
    vec<T> s0(T(0)), s1(T(0)), s2(T(0)), s3(T(0));
    for (; i + lanes <= n; i += lanes) {
      s0 += term_at<0, T>(term, i, x...);
      if constexpr (C > 1)
        s1 += term_at<1, T>(term, i, x...);
      if constexpr (C > 2) {
        s2 += term_at<2, T>(term, i, x...);
        s3 += term_at<3, T>(term, i, x...);
      }
    }
    if (i < n) {
      const ptrdiff_t left = ptrdiff_t(n - i);
      s0 += term_tail<0, T>(term, i, left, x...);
      if constexpr (C > 1)
        s1 += term_tail<1, T>(term, i, left, x...);
      if constexpr (C > 2) {
        s2 += term_tail<2, T>(term, i, left, x...);
        s3 += term_tail<3, T>(term, i, left, x...);
      }
    }
    const vec<T> named[] = {s0, s1, s2, s3};
    for (int c = 0; c < C; ++c)
      s[c] = named[c];
  } else {
    constexpr auto each = std::make_integer_sequence<int, C>{};
    for (auto &v : s)
      v = vec<T>(T(0));
    for (; i + lanes <= n; i += lanes)
      accumulate_block<T>(s, term, i, each, x...);
    if (i < n)
      accumulate_tail<T>(s, term, i, ptrdiff_t(n - i), each, x...);
  }
  return fold<T>(s, plus);
}

template <class T> T dot(std::span<const T> a, std::span<const T> b) {
  NX_ASSERT(a.size() == b.size(), "dot of spans of different sizes");
  return accumulate<T>(
      a.size(), [](const vec<T> &p, const vec<T> &q) { return p * q; },
      a.data(), b.data());
}

template <class T> T sum(std::span<const T> a) {
  return accumulate<T>(a.size(), [](const vec<T> &p) { return p; }, a.data());
}

template <class T> T sum_squares(std::span<const T> a) {
  return accumulate<T>(
      a.size(), [](const vec<T> &p) { return p * p; }, a.data());
}

template <class T> T max(std::span<const T> a) {
  using S = shape<T>;
  constexpr T lowest = -std::numeric_limits<T>::infinity();
  vec<T> s[S::C];
  for (auto &v : s)
    v = vec<T>(lowest);
  typename vec<T>::mask_type nan(false);
  const auto take = [&](int c, size_t p, ptrdiff_t left) {
    // The lanes past the end load 0; they must not count.
    const vec<T> v =
        left >= ptrdiff_t(S::W)
            ? load(a.data() + p)
            : select((iota<vec<T>>) < T(left), load_part(a.data() + p, left),
                     vec<T>(lowest));
    nan = nan || v != v;
    s[c] = simd::max(s[c], v);
  };
  size_t i = 0;
  for (; i + lanes <= a.size(); i += lanes)
    for (int c = 0; c < S::C; ++c)
      take(c, i + c * S::W, ptrdiff_t(S::W));
  if (i < a.size())
    for (int c = 0; c < S::C; ++c)
      take(c, i + c * S::W, ptrdiff_t(a.size() - i) - c * S::W);
  if (any_of(nan))
    return std::numeric_limits<T>::quiet_NaN();
  return fold<T>(s, larger);
}

/// out[i] = f(x[i]...), native width; out may be one of the inputs.
template <class T, class F, class... In>
void map(std::span<T> out, F f, std::span<const In>... in) {
  NX_ASSERT(((in.size() == out.size()) && ...), "spans of different sizes");
  using V = vec<T>;
  const size_t n = out.size();
  size_t i = 0;
  for (; i + V::size() <= n; i += V::size())
    unchecked_store(f(unchecked_load<V>(in.data() + i, V::size())...),
                    out.data() + i, V::size());
  if (i < n) {
    const auto r = std::iter_difference_t<T *>(n - i);
    partial_store(f(partial_load<V>(in.data() + i, r)...), out.data() + i, r);
  }
}

template <int c, class T, int R, int... r>
NX_FORCE_INLINE void rows_step(registers<T> (&s)[R], const T *w, const T *x,
                               size_t cols, size_t i, ptrdiff_t left,
                               std::integer_sequence<int, r...>) {
  const size_t p = i + c * shape<T>::W;
  const ptrdiff_t l = left - c * shape<T>::W;
  const vec<T> v = load_part(x + p, l);
  ((s[r][c] += load_part(w + r * cols + p, l) * v), ...);
}
template <class T, int R, int... c>
NX_FORCE_INLINE void rows_block(registers<T> (&s)[R], const T *w, const T *x,
                                size_t cols, size_t i, ptrdiff_t left,
                                std::integer_sequence<int, c...>) {
  (rows_step<c, T, R>(s, w, x, cols, i, left,
                      std::make_integer_sequence<int, R>{}),
   ...);
}

/// R rows of W x at once, each in dot's order: x is loaded once for all R.
template <class T, int R>
NX_FORCE_INLINE void rows_dot(const T *w, const T *x, size_t cols, T *y) {
  constexpr auto each = std::make_integer_sequence<int, shape<T>::C>{};
  registers<T> s[R];
  for (auto &row : s)
    for (auto &v : row)
      v = vec<T>(T(0));
  size_t i = 0;
  for (; i + lanes <= cols; i += lanes)
    rows_block<T, R>(s, w, x, cols, i, lanes, each);
  if (i < cols)
    rows_block<T, R>(s, w, x, cols, i, ptrdiff_t(cols - i), each);
  for (int r = 0; r < R; ++r)
    y[r] = fold<T>(s[r], plus);
}

template <class T>
void gemv(std::span<const T> w, std::span<const T> x, std::span<const T> b,
          std::span<T> y) {
  const size_t rows = y.size(), cols = x.size();
  NX_ASSERT(w.size() == rows * cols, "gemv: w is not y.size() x x.size()");
  NX_ASSERT(b.empty() || b.size() == rows, "gemv: b is neither empty nor y");
  // Eight accumulators: four rows on AVX2, two on SSE and NEON.
  constexpr int R = std::clamp(8 / shape<T>::C, 1, 4);
  size_t r = 0;
  for (; r + R <= rows; r += R)
    rows_dot<T, R>(w.data() + r * cols, x.data(), cols, y.data() + r);
  for (; r < rows; ++r)
    rows_dot<T, 1>(w.data() + r * cols, x.data(), cols, y.data() + r);
  if (!b.empty())
    map<T>(y, plus, std::span<const T>(y), b);
}

/// y += x[r] * row r of W for R rows, in row order: y is loaded and stored
/// once for all R.
template <class T, int R, int... r>
NX_FORCE_INLINE void rows_axpy(const T *w, const T *x, size_t cols, T *y,
                               std::integer_sequence<int, r...>) {
  using V = vec<T>;
  constexpr int W = shape<T>::W;
  const V s[R] = {V(x[r])...};
  size_t i = 0;
  for (; i + W <= cols; i += W) {
    V v = load(y + i);
    ((v = v + s[r] * load(w + r * cols + i)), ...);
    unchecked_store(v, y + i, W);
  }
  if (i < cols) {
    const auto left = ptrdiff_t(cols - i);
    V v = load_part(y + i, left);
    ((v = v + s[r] * load_part(w + r * cols + i, left)), ...);
    partial_store(v, y + i, left);
  }
}

template <class T>
void gemv_t(std::span<const T> w, std::span<const T> x, std::span<T> y) {
  const size_t rows = x.size(), cols = y.size();
  NX_ASSERT(w.size() == rows * cols, "gemv_t: w is not x.size() x y.size()");
  std::fill(y.begin(), y.end(), T(0));
  size_t r = 0;
  for (; r + 4 <= rows; r += 4)
    rows_axpy<T, 4>(w.data() + r * cols, x.data() + r, cols, y.data(),
                    std::make_integer_sequence<int, 4>{});
  for (; r < rows; ++r)
    rows_axpy<T, 1>(w.data() + r * cols, x.data() + r, cols, y.data(),
                    std::make_integer_sequence<int, 1>{});
}

} // namespace impl

/// a . b; the spans have equal sizes.
inline float dot(std::span<const float> a, std::span<const float> b) {
  return impl::dot(a, b);
}
inline double dot(std::span<const double> a, std::span<const double> b) {
  return impl::dot(a, b);
}
inline float sum(std::span<const float> a) { return impl::sum(a); }
inline double sum(std::span<const double> a) { return impl::sum(a); }
inline float sum_squares(std::span<const float> a) {
  return impl::sum_squares(a);
}
inline double sum_squares(std::span<const double> a) {
  return impl::sum_squares(a);
}
/// The largest element; NaN if any is NaN, -inf for an empty span.
inline float max(std::span<const float> a) { return impl::max(a); }
inline double max(std::span<const double> a) { return impl::max(a); }

// Element-wise; out may be the same span as an input, or disjoint from it.
#define NX_SIMD_LINALG_ELEMENTWISE(T)                                          \
  /** y += a x. */                                                             \
  inline void axpy(T a, std::span<const T> x, std::span<T> y) {                \
    const vec<T> s(a);                                                         \
    impl::map<T>(                                                              \
        y, [&](const auto &p, const auto &q) { return p + s * q; },            \
        std::span<const T>(y), x);                                             \
  }                                                                            \
  /** out = a x. */                                                            \
  inline void scale(T a, std::span<const T> x, std::span<T> out) {             \
    const vec<T> s(a);                                                         \
    impl::map<T>(out, [&](const auto &p) { return s * p; }, x);                \
  }                                                                            \
  inline void add(std::span<const T> a, std::span<const T> b,                  \
                  std::span<T> out) {                                          \
    impl::map<T>(out, impl::plus, a, b);                                       \
  }                                                                            \
  inline void mul(std::span<const T> a, std::span<const T> b,                  \
                  std::span<T> out) {                                          \
    impl::map<T>(                                                              \
        out, [](const auto &p, const auto &q) { return p * q; }, a, b);        \
  }                                                                            \
  /** y = W x + b for a row-major W of y.size() rows; b may be empty. Row i    \
      of y is dot(row i of W, x) + b[i], bit for bit. */                       \
  inline void gemv(std::span<const T> w, std::span<const T> x,                 \
                   std::span<const T> b, std::span<T> y) {                     \
    impl::gemv(w, x, b, y);                                                    \
  }                                                                            \
  /** y = W^T x for a row-major W of x.size() rows: the rows of W scaled and   \
      added in turn. y must not overlap W or x. */                             \
  inline void gemv_t(std::span<const T> w, std::span<const T> x,               \
                     std::span<T> y) {                                         \
    impl::gemv_t(w, x, y);                                                     \
  }
NX_SIMD_LINALG_ELEMENTWISE(float)
NX_SIMD_LINALG_ELEMENTWISE(double)
#undef NX_SIMD_LINALG_ELEMENTWISE

} // namespace nx::simd::linalg
