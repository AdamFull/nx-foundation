#pragma once

#include "core/foundation/simd/detail.h"

#include <cmath>

namespace nx::simd::detail {

template <class T, class Abi> struct vec_ops;
template <size_t Bytes, class Abi> struct mask_ops;

template <class T, class Abi>
using vec_storage = typename vec_ops<T, Abi>::storage;
template <size_t Bytes, class Abi>
using mask_storage = typename mask_ops<Bytes, Abi>::storage;

struct op_add {
  template <class T> constexpr T operator()(T a, T b) const {
    return static_cast<T>(a + b);
  }
};
struct op_mul {
  template <class T> constexpr T operator()(T a, T b) const {
    return static_cast<T>(a * b);
  }
};
struct op_and {
  template <class T> constexpr T operator()(T a, T b) const {
    return static_cast<T>(a & b);
  }
};
struct op_or {
  template <class T> constexpr T operator()(T a, T b) const {
    return static_cast<T>(a | b);
  }
};
struct op_xor {
  template <class T> constexpr T operator()(T a, T b) const {
    return static_cast<T>(a ^ b);
  }
};
struct op_min {
  template <class T> constexpr T operator()(T a, T b) const {
    return b < a ? b : a;
  }
};
struct op_max {
  template <class T> constexpr T operator()(T a, T b) const {
    return a < b ? b : a;
  }
};

template <class T> constexpr T shift_left(T a, int n) {
  if constexpr (sizeof(T) < sizeof(int))
    return static_cast<T>(a << n);
  else
    return static_cast<T>(static_cast<std::make_unsigned_t<T>>(a) << n);
}

template <class T> constexpr T shift_right(T a, int n) {
  return static_cast<T>(a >> n);
}

// Every element-wise operation, written on get() and generate() alone. A
// backend inherits these and replaces the ones its registers do directly.
template <class D, class T, int N, class S, class M> struct vec_ops_base {
  using value_type = T;
  using storage = S;
  using mops = M;
  using mstorage = typename M::storage;
  static constexpr int size = N;

  template <class F> static constexpr S map(const S &a, F f) {
    return D::generate([&](int i) { return static_cast<T>(f(D::get(a, i))); });
  }
  template <class F> static constexpr S zip(const S &a, const S &b, F f) {
    return D::generate(
        [&](int i) { return static_cast<T>(f(D::get(a, i), D::get(b, i))); });
  }
  template <class F>
  static constexpr mstorage compare(const S &a, const S &b, F f) {
    return M::generate([&](int i) { return f(D::get(a, i), D::get(b, i)); });
  }

  template <class U> static constexpr S load(const U *p) {
    return D::generate([&](int i) { return static_cast<T>(p[i]); });
  }
  template <class U> static constexpr S load_n(const U *p, int n) {
    return D::generate(
        [&](int i) { return i < n ? static_cast<T>(p[i]) : T(); });
  }
  template <class U>
  static constexpr S load_masked(const U *p, const mstorage &k, int n) {
    return D::generate([&](int i) {
      return i < n && M::get(k, i) ? static_cast<T>(p[i]) : T();
    });
  }
  template <class U> static constexpr void store(const S &v, U *p) {
    for (int i = 0; i < N; ++i)
      p[i] = static_cast<U>(D::get(v, i));
  }
  template <class U> static constexpr void store_n(const S &v, U *p, int n) {
    for (int i = 0; i < N && i < n; ++i)
      p[i] = static_cast<U>(D::get(v, i));
  }
  template <class U>
  static constexpr void store_masked(const S &v, U *p, const mstorage &k,
                                     int n) {
    for (int i = 0; i < N && i < n; ++i)
      if (M::get(k, i))
        p[i] = static_cast<U>(D::get(v, i));
  }

  template <class U, class UAbi>
  static constexpr S convert(const vec_storage<U, UAbi> &u) {
    return D::generate(
        [&](int i) { return static_cast<T>(vec_ops<U, UAbi>::get(u, i)); });
  }

  static constexpr S neg(const S &a) {
    return map(a, [](T x) {
      if constexpr (std::is_unsigned_v<T>)
        return T(0) - x;
      else
        return -x;
    });
  }
  static constexpr S bit_not(const S &a) {
    return map(a, [](T x) { return ~x; });
  }
  static constexpr S add(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x + y; });
  }
  static constexpr S sub(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x - y; });
  }
  static constexpr S mul(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x * y; });
  }
  static constexpr S div(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x / y; });
  }
  static constexpr S mod(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x % y; });
  }
  static constexpr S bit_and(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x & y; });
  }
  static constexpr S bit_or(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x | y; });
  }
  static constexpr S bit_xor(const S &a, const S &b) {
    return zip(a, b, [](T x, T y) { return x ^ y; });
  }
  static constexpr S shl(const S &a, const S &b) {
    return zip(a, b,
               [](T x, T y) { return shift_left(x, static_cast<int>(y)); });
  }
  static constexpr S shr(const S &a, const S &b) {
    return zip(a, b,
               [](T x, T y) { return shift_right(x, static_cast<int>(y)); });
  }
  static constexpr S shl_n(const S &a, int n) {
    return map(a, [n](T x) { return shift_left(x, n); });
  }
  static constexpr S shr_n(const S &a, int n) {
    return map(a, [n](T x) { return shift_right(x, n); });
  }

  static constexpr mstorage eq(const S &a, const S &b) {
    return compare(a, b, [](T x, T y) { return x == y; });
  }
  static constexpr mstorage ne(const S &a, const S &b) {
    return compare(a, b, [](T x, T y) { return x != y; });
  }
  static constexpr mstorage lt(const S &a, const S &b) {
    return compare(a, b, [](T x, T y) { return x < y; });
  }
  static constexpr mstorage le(const S &a, const S &b) {
    return compare(a, b, [](T x, T y) { return x <= y; });
  }

  static constexpr S select(const mstorage &k, const S &a, const S &b) {
    return D::generate(
        [&](int i) { return M::get(k, i) ? D::get(a, i) : D::get(b, i); });
  }
  static constexpr S min(const S &a, const S &b) { return zip(a, b, op_min{}); }
  static constexpr S max(const S &a, const S &b) { return zip(a, b, op_max{}); }

  template <class Op> static constexpr T reduce(const S &v, Op op) {
    T r = D::get(v, 0);
    for (int i = 1; i < N; ++i)
      r = op(r, D::get(v, i));
    return r;
  }

  static S sqrt(const S &a) {
    return map(a, [](T x) { return std::sqrt(x); });
  }
  static S abs(const S &a) {
    return map(a, [](T x) {
      if constexpr (std::is_floating_point_v<T>)
        return std::fabs(x);
      else
        return x < 0 ? -x : x;
    });
  }
  static S floor(const S &a) {
    return map(a, [](T x) { return std::floor(x); });
  }
  static S ceil(const S &a) {
    return map(a, [](T x) { return std::ceil(x); });
  }
  static S trunc(const S &a) {
    return map(a, [](T x) { return std::trunc(x); });
  }
  static S round(const S &a) {
    return map(a, [](T x) { return std::round(x); });
  }
  static S rint(const S &a) {
    return map(a, [](T x) { return std::rint(x); });
  }
  static S fma(const S &a, const S &b, const S &c) {
    return D::generate([&](int i) {
      return std::fma(D::get(a, i), D::get(b, i), D::get(c, i));
    });
  }
};

// Masks keep one lane per element, all bits set or clear, so the same bits
// select between values of the element's width.
template <class D, size_t B, int N, class S> struct mask_ops_base {
  using storage = S;
  static constexpr int size = N;
  static constexpr u64 full_bits = ~u64{0} >> (64 - N);

  static constexpr S land(const S &a, const S &b) {
    return D::generate([&](int i) { return D::get(a, i) && D::get(b, i); });
  }
  static constexpr S lor(const S &a, const S &b) {
    return D::generate([&](int i) { return D::get(a, i) || D::get(b, i); });
  }
  static constexpr S lxor(const S &a, const S &b) {
    return D::generate([&](int i) { return D::get(a, i) != D::get(b, i); });
  }
  static constexpr S lnot(const S &a) {
    return D::generate([&](int i) { return !D::get(a, i); });
  }
  static constexpr S select(const S &k, const S &a, const S &b) {
    return D::generate(
        [&](int i) { return D::get(k, i) ? D::get(a, i) : D::get(b, i); });
  }

  static constexpr u64 to_bits(const S &a) {
    u64 r = 0;
    for (int i = 0; i < N; ++i)
      r |= u64{D::get(a, i)} << i;
    return r;
  }
  static constexpr S from_bits(u64 bits) {
    return D::generate([bits](int i) { return ((bits >> i) & 1) != 0; });
  }

  static constexpr bool all_of(const S &a) {
    return D::to_bits(a) == full_bits;
  }
  static constexpr bool any_of(const S &a) { return D::to_bits(a) != 0; }
  static constexpr int count(const S &a) {
    return std::popcount(D::to_bits(a));
  }
  static constexpr int min_index(const S &a) {
    return std::countr_zero(D::to_bits(a));
  }
  static constexpr int max_index(const S &a) {
    return std::bit_width(D::to_bits(a)) - 1;
  }
};

template <size_t B, int N> struct lane_array {
  mask_lane_t<B> v[N];
};

template <size_t B, int N>
struct mask_ops_lanes
    : mask_ops_base<mask_ops_lanes<B, N>, B, N, lane_array<B, N>> {
  using S = lane_array<B, N>;
  using L = mask_lane_t<B>;

  static constexpr S broadcast(bool b) {
    S r{};
    for (int i = 0; i < N; ++i)
      r.v[i] = lane_to_value<L>(b);
    return r;
  }
  static constexpr bool get(const S &a, int i) { return lane_to_bool(a.v[i]); }
  template <class G> static constexpr S generate(G &&g) {
    S r{};
    for (int i = 0; i < N; ++i)
      r.v[i] = lane_to_value<L>(static_cast<bool>(g(i)));
    return r;
  }
};

template <class T, int N> struct fixed_storage {
  T v[N];
};

template <size_t B, int N>
struct mask_ops<B, abi_fixed<N>> : mask_ops_lanes<B, N> {};

template <class T, int N>
  requires vectorizable_arithmetic<T>
struct vec_ops<T, abi_fixed<N>>
    : vec_ops_base<vec_ops<T, abi_fixed<N>>, T, N, fixed_storage<T, N>,
                   mask_ops<sizeof(T), abi_fixed<N>>> {
  using S = fixed_storage<T, N>;

  static constexpr S broadcast(T x) {
    S r{};
    for (int i = 0; i < N; ++i)
      r.v[i] = x;
    return r;
  }
  static constexpr T get(const S &a, int i) { return a.v[i]; }
  template <class G> static constexpr S generate(G &&g) {
    S r{};
    for (int i = 0; i < N; ++i)
      r.v[i] = static_cast<T>(g(i));
    return r;
  }
};

template <size_t B, class Part, int C>
struct mask_ops<B, abi_multi<Part, C>>
    : mask_ops_base<mask_ops<B, abi_multi<Part, C>>, B,
                    C * mask_ops<B, Part>::size,
                    fixed_storage<mask_storage<B, Part>, C>> {
  using P = mask_ops<B, Part>;
  using S = fixed_storage<mask_storage<B, Part>, C>;
  static constexpr int W = P::size;

  static constexpr S broadcast(bool b) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::broadcast(b);
    return r;
  }
  static constexpr bool get(const S &a, int i) {
    return P::get(a.v[i / W], i % W);
  }
  template <class G> static constexpr S generate(G &&g) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::generate([&](int i) { return g(j * W + i); });
    return r;
  }
  template <class F> static constexpr S each(const S &a, const S &b, F f) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = f(a.v[j], b.v[j]);
    return r;
  }
  static constexpr S land(const S &a, const S &b) {
    return each(a, b, [](auto x, auto y) { return P::land(x, y); });
  }
  static constexpr S lor(const S &a, const S &b) {
    return each(a, b, [](auto x, auto y) { return P::lor(x, y); });
  }
  static constexpr S lxor(const S &a, const S &b) {
    return each(a, b, [](auto x, auto y) { return P::lxor(x, y); });
  }
  static constexpr S lnot(const S &a) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::lnot(a.v[j]);
    return r;
  }
  static constexpr S select(const S &k, const S &a, const S &b) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::select(k.v[j], a.v[j], b.v[j]);
    return r;
  }
  static constexpr u64 to_bits(const S &a) {
    u64 r = 0;
    for (int j = 0; j < C; ++j)
      r |= P::to_bits(a.v[j]) << (j * W);
    return r;
  }
  static constexpr S from_bits(u64 bits) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::from_bits(bits >> (j * W));
    return r;
  }
};

template <class T, class Part, int C>
  requires vectorizable_arithmetic<T>
struct vec_ops<T, abi_multi<Part, C>>
    : vec_ops_base<vec_ops<T, abi_multi<Part, C>>, T,
                   C * vec_ops<T, Part>::size,
                   fixed_storage<vec_storage<T, Part>, C>,
                   mask_ops<sizeof(T), abi_multi<Part, C>>> {
  using P = vec_ops<T, Part>;
  using M = mask_ops<sizeof(T), abi_multi<Part, C>>;
  using S = fixed_storage<vec_storage<T, Part>, C>;
  using MS = typename M::storage;
  static constexpr int W = P::size;
  static constexpr int N = C * W;

  static constexpr S broadcast(T x) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::broadcast(x);
    return r;
  }
  static constexpr T get(const S &a, int i) {
    return P::get(a.v[i / W], i % W);
  }
  template <class G> static constexpr S generate(G &&g) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::generate([&](int i) { return g(j * W + i); });
    return r;
  }

  template <class F> static constexpr S each(const S &a, F f) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = f(a.v[j]);
    return r;
  }
  template <class F> static constexpr S each(const S &a, const S &b, F f) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = f(a.v[j], b.v[j]);
    return r;
  }
  template <class F>
  static constexpr MS each_mask(const S &a, const S &b, F f) {
    MS r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = f(a.v[j], b.v[j]);
    return r;
  }

  template <class U> static constexpr S load(const U *p) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::load(p + j * W);
    return r;
  }
  template <class U> static constexpr void store(const S &v, U *p) {
    for (int j = 0; j < C; ++j)
      P::store(v.v[j], p + j * W);
  }

  template <class U> static constexpr S load_n(const U *p, int n) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = j * W < n ? P::load_n(p + j * W, n - j * W) : P::broadcast(T());
    return r;
  }
  template <class U>
  static constexpr S load_masked(const U *p, const MS &k, int n) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = j * W < n ? P::load_masked(p + j * W, k.v[j], n - j * W)
                         : P::broadcast(T());
    return r;
  }
  template <class U> static constexpr void store_n(const S &v, U *p, int n) {
    for (int j = 0; j < C && j * W < n; ++j)
      P::store_n(v.v[j], p + j * W, n - j * W);
  }
  template <class U>
  static constexpr void store_masked(const S &v, U *p, const MS &k, int n) {
    for (int j = 0; j < C && j * W < n; ++j)
      P::store_masked(v.v[j], p + j * W, k.v[j], n - j * W);
  }

  template <class U, class UAbi>
  static constexpr S convert(const vec_storage<U, UAbi> &u) {
    if constexpr (requires {
                    typename UAbi::part;
                    requires UAbi::count == C;
                    requires vec_ops<U, typename UAbi::part>::size == W;
                  }) {
      S r{};
      for (int j = 0; j < C; ++j)
        r.v[j] = P::template convert<U, typename UAbi::part>(u.v[j]);
      return r;
    } else {
      return vec_ops_base<vec_ops, T, N, S, M>::template convert<U, UAbi>(u);
    }
  }

  static constexpr S neg(const S &a) {
    return each(a, [](const auto &x) { return P::neg(x); });
  }
  static constexpr S bit_not(const S &a) {
    return each(a, [](const auto &x) { return P::bit_not(x); });
  }
#define NX_SIMD_MULTI_BINARY(name)                                             \
  static constexpr S name(const S &a, const S &b) {                            \
    return each(a, b,                                                          \
                [](const auto &x, const auto &y) { return P::name(x, y); });   \
  }
  NX_SIMD_MULTI_BINARY(add)
  NX_SIMD_MULTI_BINARY(sub)
  NX_SIMD_MULTI_BINARY(mul)
  NX_SIMD_MULTI_BINARY(div)
  NX_SIMD_MULTI_BINARY(mod)
  NX_SIMD_MULTI_BINARY(bit_and)
  NX_SIMD_MULTI_BINARY(bit_or)
  NX_SIMD_MULTI_BINARY(bit_xor)
  NX_SIMD_MULTI_BINARY(shl)
  NX_SIMD_MULTI_BINARY(shr)
  NX_SIMD_MULTI_BINARY(min)
  NX_SIMD_MULTI_BINARY(max)
#undef NX_SIMD_MULTI_BINARY
  static constexpr S shl_n(const S &a, int n) {
    return each(a, [n](const auto &x) { return P::shl_n(x, n); });
  }
  static constexpr S shr_n(const S &a, int n) {
    return each(a, [n](const auto &x) { return P::shr_n(x, n); });
  }
  static constexpr MS eq(const S &a, const S &b) {
    return each_mask(a, b,
                     [](const auto &x, const auto &y) { return P::eq(x, y); });
  }
  static constexpr MS ne(const S &a, const S &b) {
    return each_mask(a, b,
                     [](const auto &x, const auto &y) { return P::ne(x, y); });
  }
  static constexpr MS lt(const S &a, const S &b) {
    return each_mask(a, b,
                     [](const auto &x, const auto &y) { return P::lt(x, y); });
  }
  static constexpr MS le(const S &a, const S &b) {
    return each_mask(a, b,
                     [](const auto &x, const auto &y) { return P::le(x, y); });
  }
  static constexpr S select(const MS &k, const S &a, const S &b) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::select(k.v[j], a.v[j], b.v[j]);
    return r;
  }

  template <class Op> static constexpr T reduce(const S &v, Op op) {
    auto acc = v.v[0];
    for (int j = 1; j < C; ++j)
      acc = combine(acc, v.v[j], op);
    return P::reduce(acc, op);
  }
  template <class Op>
  static constexpr vec_storage<T, Part>
  combine(const vec_storage<T, Part> &a, const vec_storage<T, Part> &b, Op) {
    if constexpr (std::is_same_v<Op, op_add>)
      return P::add(a, b);
    else if constexpr (std::is_same_v<Op, op_mul>)
      return P::mul(a, b);
    else if constexpr (std::is_same_v<Op, op_and>)
      return P::bit_and(a, b);
    else if constexpr (std::is_same_v<Op, op_or>)
      return P::bit_or(a, b);
    else if constexpr (std::is_same_v<Op, op_xor>)
      return P::bit_xor(a, b);
    else if constexpr (std::is_same_v<Op, op_min>)
      return P::min(a, b);
    else if constexpr (std::is_same_v<Op, op_max>)
      return P::max(a, b);
    else
      return P::zip(a, b, Op{});
  }

  static S sqrt(const S &a) {
    return each(a, [](const auto &x) { return P::sqrt(x); });
  }
  static S abs(const S &a) {
    return each(a, [](const auto &x) { return P::abs(x); });
  }
  static S floor(const S &a) {
    return each(a, [](const auto &x) { return P::floor(x); });
  }
  static S ceil(const S &a) {
    return each(a, [](const auto &x) { return P::ceil(x); });
  }
  static S trunc(const S &a) {
    return each(a, [](const auto &x) { return P::trunc(x); });
  }
  static S round(const S &a) {
    return each(a, [](const auto &x) { return P::round(x); });
  }
  static S rint(const S &a) {
    return each(a, [](const auto &x) { return P::rint(x); });
  }
  static S fma(const S &a, const S &b, const S &c) {
    S r{};
    for (int j = 0; j < C; ++j)
      r.v[j] = P::fma(a.v[j], b.v[j], c.v[j]);
    return r;
  }
};

} // namespace nx::simd::detail
