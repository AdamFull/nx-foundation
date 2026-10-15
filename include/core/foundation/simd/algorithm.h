#pragma once

#include "core/foundation/simd/vec.h"

#include <array>
#include <functional>
#include <tuple>

namespace nx::simd {

namespace detail {

template <class V, class G> constexpr V vec_generate(G &&g) {
  return access::make<V>(
      vec_ops<typename V::value_type, typename V::abi_type>::generate(g));
}

template <class M> struct mask_traits;
template <size_t B, class Abi> struct mask_traits<basic_mask<B, Abi>> {
  static constexpr size_t bytes = B;
  using ops = mask_ops<B, Abi>;
};
template <class M> using mask_ops_of = typename mask_traits<M>::ops;

template <class M, class G> constexpr M make_mask(G &&g) {
  return access::make<M>(mask_ops_of<M>::generate(g));
}

template <class V>
using vops = vec_ops<typename V::value_type, typename V::abi_type>;

template <class Op> struct reduce_tag {
  using type = void;
};
template <> struct reduce_tag<std::plus<>> {
  using type = op_add;
};
template <> struct reduce_tag<std::multiplies<>> {
  using type = op_mul;
};
template <> struct reduce_tag<std::bit_and<>> {
  using type = op_and;
};
template <> struct reduce_tag<std::bit_or<>> {
  using type = op_or;
};
template <> struct reduce_tag<std::bit_xor<>> {
  using type = op_xor;
};

template <class Op, class T>
concept reduction_binary_operation = requires(const Op op, const vec<T, 1> v) {
  { op(v, v) } -> std::same_as<vec<T, 1>>;
};

template <class Op>
inline constexpr bool has_default_identity =
    !std::is_void_v<typename reduce_tag<Op>::type>;

template <class T, class Op> constexpr T default_identity() {
  if constexpr (std::is_same_v<Op, std::multiplies<>>)
    return T(1);
  else if constexpr (std::is_same_v<Op, std::bit_and<>>)
    return T(~T());
  else
    return T();
}

template <class T, class Op>
constexpr T generalized_sum(const T *values, int n, Op &op) {
  if (n == 1)
    return values[0];
  const int half = n / 2;
  const T lo = generalized_sum(values, half, op);
  const T hi = generalized_sum(values + half, n - half, op);
  return op(vec<T, 1>(lo), vec<T, 1>(hi))[0];
}

template <class... Flags, class U> constexpr void check_alignment(const U *p) {
  if (std::is_constant_evaluated())
    return;
  (void)p;
  constexpr size_t over = overaligned_value<Flags...>;
  if constexpr (over != 0)
    assert(reinterpret_cast<uintptr_t>(p) % over == 0);
}

template <class V, class U, class... Flags> consteval void check_load() {
  static_assert(vectorizable<U>, "the range's elements must be vectorizable");
  static_assert(simd_vec_type<V>, "V must be an enabled basic_vec");
  static_assert(has_convert_flag<Flags...> ||
                    value_preserving<U, typename V::value_type>(),
                "a conversion that is not value-preserving needs flag_convert");
}

template <class T, class U, class... Flags> consteval void check_store() {
  static_assert(vectorizable<U>, "the range's elements must be vectorizable");
  static_assert(has_convert_flag<Flags...> || value_preserving<T, U>(),
                "a conversion that is not value-preserving needs flag_convert");
}

template <class V, class R>
using load_vec_t =
    std::conditional_t<std::is_void_v<V>,
                       basic_vec<std::ranges::range_value_t<R>>, V>;

template <class V, class R>
constexpr void
check_aligned_flag(const typename std::ranges::range_value_t<R> *p,
                   bool aligned) {
  if (std::is_constant_evaluated() || !aligned)
    return;
  assert((reinterpret_cast<uintptr_t>(p) %
              alignment_v<V, std::ranges::range_value_t<R>> ==
          0));
}

template <bool Partial, class V, class R, class... Flags, class K>
constexpr V load_range(R &&r, K k) {
  using U = std::ranges::range_value_t<R>;
  check_load<V, U, Flags...>();
  const auto *p = std::ranges::data(r);
  const int n = static_cast<int>(std::ranges::size(r));
  check_aligned_flag<V, R>(p, has_flag<aligned_flag, Flags...>);
  check_alignment<Flags...>(p);
  (void)k;
  using O = vops<V>;
  if constexpr (!std::is_null_pointer_v<K>) {
    return access::make<V>(O::load_masked(p, access::data(*k), n));
  } else if constexpr (!Partial) {
    assert(n >= V::size());
    return access::make<V>(O::load(p));
  } else {
    if (n >= V::size())
      return access::make<V>(O::load(p));
    return access::make<V>(O::load_n(p, n));
  }
}

template <bool Partial, class T, class Abi, class R, class... Flags, class K>
constexpr void store_range(const basic_vec<T, Abi> &v, R &&r, K k) {
  using V = basic_vec<T, Abi>;
  using U = std::ranges::range_value_t<R>;
  check_store<T, U, Flags...>();
  auto *p = std::ranges::data(r);
  const int n = static_cast<int>(std::ranges::size(r));
  check_aligned_flag<V, R>(p, has_flag<aligned_flag, Flags...>);
  check_alignment<Flags...>(p);
  (void)k;
  using O = vops<V>;
  if constexpr (!std::is_null_pointer_v<K>) {
    O::store_masked(access::data(v), p, access::data(*k), n);
  } else if constexpr (!Partial) {
    assert(n >= V::size());
    O::store(access::data(v), p);
  } else if (n >= V::size()) {
    O::store(access::data(v), p);
  } else {
    O::store_n(access::data(v), p, n);
  }
}

} // namespace detail

inline constexpr detail::size_type zero_element =
    std::numeric_limits<detail::size_type>::min();
inline constexpr detail::size_type uninit_element = zero_element + 1;

template <size_t B, class Abi>
constexpr bool all_of(const basic_mask<B, Abi> &k) noexcept {
  return detail::mask_ops<B, Abi>::all_of(detail::access::data(k));
}
template <size_t B, class Abi>
constexpr bool any_of(const basic_mask<B, Abi> &k) noexcept {
  return detail::mask_ops<B, Abi>::any_of(detail::access::data(k));
}
template <size_t B, class Abi>
constexpr bool none_of(const basic_mask<B, Abi> &k) noexcept {
  return !any_of(k);
}
template <size_t B, class Abi>
constexpr detail::size_type reduce_count(const basic_mask<B, Abi> &k) noexcept {
  return detail::mask_ops<B, Abi>::count(detail::access::data(k));
}
template <size_t B, class Abi>
constexpr detail::size_type reduce_min_index(const basic_mask<B, Abi> &k) {
  assert(any_of(k));
  return detail::mask_ops<B, Abi>::min_index(detail::access::data(k));
}
template <size_t B, class Abi>
constexpr detail::size_type reduce_max_index(const basic_mask<B, Abi> &k) {
  assert(any_of(k));
  return detail::mask_ops<B, Abi>::max_index(detail::access::data(k));
}

constexpr bool all_of(std::same_as<bool> auto x) noexcept { return x; }
constexpr bool any_of(std::same_as<bool> auto x) noexcept { return x; }
constexpr bool none_of(std::same_as<bool> auto x) noexcept { return !x; }
constexpr detail::size_type reduce_count(std::same_as<bool> auto x) noexcept {
  return x;
}
constexpr detail::size_type reduce_min_index(std::same_as<bool> auto x) {
  assert(x);
  (void)x;
  return 0;
}
constexpr detail::size_type reduce_max_index(std::same_as<bool> auto x) {
  assert(x);
  (void)x;
  return 0;
}

template <class T, class Abi, class BinaryOperation = std::plus<>>
  requires detail::reduction_binary_operation<BinaryOperation, T>
constexpr T reduce(const basic_vec<T, Abi> &x, BinaryOperation binary_op = {}) {
  using Tag = typename detail::reduce_tag<BinaryOperation>::type;
  if constexpr (!std::is_void_v<Tag>) {
    return detail::vops<basic_vec<T, Abi>>::reduce(detail::access::data(x),
                                                   Tag{});
  } else {
    T values[basic_vec<T, Abi>::size()];
    for (int i = 0; i < basic_vec<T, Abi>::size(); ++i)
      values[i] = x[i];
    return detail::generalized_sum(values, basic_vec<T, Abi>::size(),
                                   binary_op);
  }
}

template <class T, class Abi, class BinaryOperation = std::plus<>>
  requires detail::reduction_binary_operation<BinaryOperation, T> &&
           detail::has_default_identity<BinaryOperation>
constexpr T reduce(const basic_vec<T, Abi> &x,
                   const typename basic_vec<T, Abi>::mask_type &mask,
                   BinaryOperation binary_op = {}) {
  return reduce(x, mask, binary_op,
                detail::default_identity<T, BinaryOperation>());
}

template <class T, class Abi, class BinaryOperation>
  requires detail::reduction_binary_operation<BinaryOperation, T>
constexpr T reduce(const basic_vec<T, Abi> &x,
                   const typename basic_vec<T, Abi>::mask_type &mask,
                   BinaryOperation binary_op,
                   std::type_identity_t<T> identity_element) {
  using V = basic_vec<T, Abi>;
  using Tag = typename detail::reduce_tag<BinaryOperation>::type;
  if (none_of(mask))
    return identity_element;
  if constexpr (!std::is_void_v<Tag>) {
    using O = detail::vops<V>;
    return O::reduce(O::select(detail::access::data(mask),
                               detail::access::data(x),
                               O::broadcast(identity_element)),
                     Tag{});
  } else {
    T values[V::size()];
    int n = 0;
    for (int i = 0; i < V::size(); ++i)
      if (mask[i])
        values[n++] = x[i];
    if (n == 0)
      return identity_element;
    return detail::generalized_sum(values, n, binary_op);
  }
}

template <class T, class BinaryOperation = std::plus<>>
  requires detail::vectorizable<T> &&
           detail::reduction_binary_operation<BinaryOperation, T>
constexpr T reduce(const T &x, BinaryOperation = {}) {
  return x;
}

template <class T, class BinaryOperation = std::plus<>>
  requires detail::vectorizable<T> &&
           detail::reduction_binary_operation<BinaryOperation, T> &&
           detail::has_default_identity<BinaryOperation>
constexpr T reduce(const T &x, std::same_as<bool> auto mask,
                   BinaryOperation = {}) {
  return mask ? x : detail::default_identity<T, BinaryOperation>();
}

template <class T, class BinaryOperation>
  requires detail::vectorizable<T> &&
           detail::reduction_binary_operation<BinaryOperation, T>
constexpr T reduce(const T &x, std::same_as<bool> auto mask, BinaryOperation,
                   std::type_identity_t<T> identity_element) {
  return mask ? x : identity_element;
}

template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr T reduce_min(const basic_vec<T, Abi> &x) noexcept {
  return detail::vops<basic_vec<T, Abi>>::reduce(detail::access::data(x),
                                                 detail::op_min{});
}
template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr T
reduce_min(const basic_vec<T, Abi> &x,
           const typename basic_vec<T, Abi>::mask_type &mask) noexcept {
  using O = detail::vops<basic_vec<T, Abi>>;
  return O::reduce(O::select(detail::access::data(mask),
                             detail::access::data(x),
                             O::broadcast(std::numeric_limits<T>::max())),
                   detail::op_min{});
}
template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr T reduce_max(const basic_vec<T, Abi> &x) noexcept {
  return detail::vops<basic_vec<T, Abi>>::reduce(detail::access::data(x),
                                                 detail::op_max{});
}
template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr T
reduce_max(const basic_vec<T, Abi> &x,
           const typename basic_vec<T, Abi>::mask_type &mask) noexcept {
  using O = detail::vops<basic_vec<T, Abi>>;
  return O::reduce(O::select(detail::access::data(mask),
                             detail::access::data(x),
                             O::broadcast(std::numeric_limits<T>::lowest())),
                   detail::op_max{});
}

template <class T>
  requires detail::vectorizable<T> && std::totally_ordered<T>
constexpr T reduce_min(const T &x) noexcept {
  return x;
}
template <class T>
  requires detail::vectorizable<T> && std::totally_ordered<T>
constexpr T reduce_min(const T &x, std::same_as<bool> auto mask) noexcept {
  return mask ? x : std::numeric_limits<T>::max();
}
template <class T>
  requires detail::vectorizable<T> && std::totally_ordered<T>
constexpr T reduce_max(const T &x) noexcept {
  return x;
}
template <class T>
  requires detail::vectorizable<T> && std::totally_ordered<T>
constexpr T reduce_max(const T &x, std::same_as<bool> auto mask) noexcept {
  return mask ? x : std::numeric_limits<T>::lowest();
}

// Unaligned instructions are as fast as aligned ones on every supported
// target, so the alignment flags only add debug checks.

template <class V = void, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto partial_load(R &&r, flags<Flags...> = {})
    -> detail::load_vec_t<V, R> {
  return detail::load_range<true, detail::load_vec_t<V, R>, R &, Flags...>(
      r, nullptr);
}
template <class V = void, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto
partial_load(R &&r, const typename detail::load_vec_t<V, R>::mask_type &mask,
             flags<Flags...> = {}) -> detail::load_vec_t<V, R> {
  return detail::load_range<true, detail::load_vec_t<V, R>, R &, Flags...>(
      r, &mask);
}
template <class V = void, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto unchecked_load(R &&r, flags<Flags...> = {})
    -> detail::load_vec_t<V, R> {
  using W = detail::load_vec_t<V, R>;
  static_assert(detail::static_range_size<R>() == detail::dynamic_size ||
                    detail::static_range_size<R>() >= size_t(W::size()),
                "the range is shorter than the vector");
  return detail::load_range<false, W, R &, Flags...>(r, nullptr);
}
template <class V = void, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto
unchecked_load(R &&r, const typename detail::load_vec_t<V, R>::mask_type &mask,
               flags<Flags...> = {}) -> detail::load_vec_t<V, R> {
  using W = detail::load_vec_t<V, R>;
  static_assert(detail::static_range_size<R>() == detail::dynamic_size ||
                    detail::static_range_size<R>() >= size_t(W::size()),
                "the range is shorter than the vector");
  return detail::load_range<false, W, R &, Flags...>(r, &mask);
}

template <class V = void, std::contiguous_iterator I, class... Flags>
constexpr auto partial_load(I first, std::iter_difference_t<I> n,
                            flags<Flags...> f = {}) {
  return partial_load<V>(std::span<const std::iter_value_t<I>>(first, n), f);
}
template <class V = void, std::contiguous_iterator I, class... Flags>
constexpr auto
partial_load(I first, std::iter_difference_t<I> n,
             const typename detail::load_vec_t<
                 V, std::span<const std::iter_value_t<I>>>::mask_type &mask,
             flags<Flags...> f = {}) {
  return partial_load<V>(std::span<const std::iter_value_t<I>>(first, n), mask,
                         f);
}
template <class V = void, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
constexpr auto partial_load(I first, S last, flags<Flags...> f = {}) {
  return partial_load<V>(std::span<const std::iter_value_t<I>>(first, last), f);
}
template <class V = void, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
constexpr auto
partial_load(I first, S last,
             const typename detail::load_vec_t<
                 V, std::span<const std::iter_value_t<I>>>::mask_type &mask,
             flags<Flags...> f = {}) {
  return partial_load<V>(std::span<const std::iter_value_t<I>>(first, last),
                         mask, f);
}
template <class V = void, std::contiguous_iterator I, class... Flags>
constexpr auto unchecked_load(I first, std::iter_difference_t<I> n,
                              flags<Flags...> f = {}) {
  return unchecked_load<V>(std::span<const std::iter_value_t<I>>(first, n), f);
}
template <class V = void, std::contiguous_iterator I, class... Flags>
constexpr auto
unchecked_load(I first, std::iter_difference_t<I> n,
               const typename detail::load_vec_t<
                   V, std::span<const std::iter_value_t<I>>>::mask_type &mask,
               flags<Flags...> f = {}) {
  return unchecked_load<V>(std::span<const std::iter_value_t<I>>(first, n),
                           mask, f);
}
template <class V = void, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
constexpr auto unchecked_load(I first, S last, flags<Flags...> f = {}) {
  return unchecked_load<V>(std::span<const std::iter_value_t<I>>(first, last),
                           f);
}
template <class V = void, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
constexpr auto
unchecked_load(I first, S last,
               const typename detail::load_vec_t<
                   V, std::span<const std::iter_value_t<I>>>::mask_type &mask,
               flags<Flags...> f = {}) {
  return unchecked_load<V>(std::span<const std::iter_value_t<I>>(first, last),
                           mask, f);
}

template <class T, class Abi, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R> &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<T, std::ranges::range_value_t<R>>
constexpr void partial_store(const basic_vec<T, Abi> &v, R &&r,
                             flags<Flags...> = {}) {
  detail::store_range<true, T, Abi, R &, Flags...>(v, r, nullptr);
}
template <class T, class Abi, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R> &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<T, std::ranges::range_value_t<R>>
constexpr void partial_store(const basic_vec<T, Abi> &v, R &&r,
                             const typename basic_vec<T, Abi>::mask_type &mask,
                             flags<Flags...> = {}) {
  detail::store_range<true, T, Abi, R &, Flags...>(v, r, &mask);
}
template <class T, class Abi, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R> &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<T, std::ranges::range_value_t<R>>
constexpr void unchecked_store(const basic_vec<T, Abi> &v, R &&r,
                               flags<Flags...> = {}) {
  static_assert(detail::static_range_size<R>() == detail::dynamic_size ||
                    detail::static_range_size<R>() >=
                        size_t(basic_vec<T, Abi>::size()),
                "the range is shorter than the vector");
  detail::store_range<false, T, Abi, R &, Flags...>(v, r, nullptr);
}
template <class T, class Abi, std::ranges::contiguous_range R, class... Flags>
  requires std::ranges::sized_range<R> &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<T, std::ranges::range_value_t<R>>
constexpr void
unchecked_store(const basic_vec<T, Abi> &v, R &&r,
                const typename basic_vec<T, Abi>::mask_type &mask,
                flags<Flags...> = {}) {
  static_assert(detail::static_range_size<R>() == detail::dynamic_size ||
                    detail::static_range_size<R>() >=
                        size_t(basic_vec<T, Abi>::size()),
                "the range is shorter than the vector");
  detail::store_range<false, T, Abi, R &, Flags...>(v, r, &mask);
}

template <class T, class Abi, std::contiguous_iterator I, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void partial_store(const basic_vec<T, Abi> &v, I first,
                             std::iter_difference_t<I> n,
                             flags<Flags...> f = {}) {
  partial_store(v, std::span<std::iter_value_t<I>>(first, n), f);
}
template <class T, class Abi, std::contiguous_iterator I, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void partial_store(const basic_vec<T, Abi> &v, I first,
                             std::iter_difference_t<I> n,
                             const typename basic_vec<T, Abi>::mask_type &mask,
                             flags<Flags...> f = {}) {
  partial_store(v, std::span<std::iter_value_t<I>>(first, n), mask, f);
}
template <class T, class Abi, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void partial_store(const basic_vec<T, Abi> &v, I first, S last,
                             flags<Flags...> f = {}) {
  partial_store(v, std::span<std::iter_value_t<I>>(first, last), f);
}
template <class T, class Abi, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void partial_store(const basic_vec<T, Abi> &v, I first, S last,
                             const typename basic_vec<T, Abi>::mask_type &mask,
                             flags<Flags...> f = {}) {
  partial_store(v, std::span<std::iter_value_t<I>>(first, last), mask, f);
}
template <class T, class Abi, std::contiguous_iterator I, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void unchecked_store(const basic_vec<T, Abi> &v, I first,
                               std::iter_difference_t<I> n,
                               flags<Flags...> f = {}) {
  unchecked_store(v, std::span<std::iter_value_t<I>>(first, n), f);
}
template <class T, class Abi, std::contiguous_iterator I, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void unchecked_store(
    const basic_vec<T, Abi> &v, I first, std::iter_difference_t<I> n,
    const typename basic_vec<T, Abi>::mask_type &mask, flags<Flags...> f = {}) {
  unchecked_store(v, std::span<std::iter_value_t<I>>(first, n), mask, f);
}
template <class T, class Abi, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void unchecked_store(const basic_vec<T, Abi> &v, I first, S last,
                               flags<Flags...> f = {}) {
  unchecked_store(v, std::span<std::iter_value_t<I>>(first, last), f);
}
template <class T, class Abi, std::contiguous_iterator I,
          std::sized_sentinel_for<I> S, class... Flags>
  requires std::indirectly_writable<I, std::iter_value_t<I>>
constexpr void
unchecked_store(const basic_vec<T, Abi> &v, I first, S last,
                const typename basic_vec<T, Abi>::mask_type &mask,
                flags<Flags...> f = {}) {
  unchecked_store(v, std::span<std::iter_value_t<I>>(first, last), mask, f);
}

template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr basic_vec<T, Abi> min(const basic_vec<T, Abi> &a,
                                const basic_vec<T, Abi> &b) noexcept {
  return detail::access::make<basic_vec<T, Abi>>(
      detail::vops<basic_vec<T, Abi>>::min(detail::access::data(a),
                                           detail::access::data(b)));
}
template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr basic_vec<T, Abi> max(const basic_vec<T, Abi> &a,
                                const basic_vec<T, Abi> &b) noexcept {
  return detail::access::make<basic_vec<T, Abi>>(
      detail::vops<basic_vec<T, Abi>>::max(detail::access::data(a),
                                           detail::access::data(b)));
}
template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr std::pair<basic_vec<T, Abi>, basic_vec<T, Abi>>
minmax(const basic_vec<T, Abi> &a, const basic_vec<T, Abi> &b) noexcept {
  return {min(a, b), max(a, b)};
}
template <class T, class Abi>
  requires std::totally_ordered<T>
constexpr basic_vec<T, Abi> clamp(const basic_vec<T, Abi> &v,
                                  const basic_vec<T, Abi> &lo,
                                  const basic_vec<T, Abi> &hi) {
  assert(none_of(hi < lo));
  using O = detail::vops<basic_vec<T, Abi>>;
  const auto &x = detail::access::data(v);
  const auto &l = detail::access::data(lo);
  const auto &h = detail::access::data(hi);
  return detail::access::make<basic_vec<T, Abi>>(O::min(O::max(x, l), h));
}

template <class T, class U>
constexpr auto select(bool c, const T &a, const U &b)
    -> std::remove_cvref_t<decltype(c ? a : b)> {
  return c ? a : b;
}
template <size_t Bytes, class Abi, class T, class U>
constexpr auto select(const basic_mask<Bytes, Abi> &c, const T &a,
                      const U &b) noexcept
    -> decltype(simd_select_impl(c, a, b)) {
  return simd_select_impl(c, a, b);
}

namespace detail {

template <class V, class IdxMap>
constexpr size_type permute_index(IdxMap &idxmap, size_type i) {
  if constexpr (std::is_invocable_v<IdxMap &, size_type, size_type>)
    return static_cast<size_type>(idxmap(i, V::size()));
  else
    return static_cast<size_type>(idxmap(i));
}

template <class V> constexpr auto element_or_zero(const V &v, size_type i) {
  using T = typename V::value_type;
  return i < 0 ? T() : v[i];
}

template <class R, class V, class G> constexpr R permute_with(const V &v, G g) {
  if constexpr (simd_mask_type<V>)
    return make_mask<R>([&](int i) { return element_or_zero(v, g(i)); });
  else
    return vec_generate<R>([&](int i) { return element_or_zero(v, g(i)); });
}

template <class V, class IdxMap, int M> consteval bool constant_index_map() {
  using Map = std::remove_cvref_t<IdxMap>;
  if constexpr (std::is_empty_v<Map> && std::is_default_constructible_v<Map>)
    return requires {
      typename std::integral_constant<int, [] {
        Map m{};
        return permute_index<V>(m, 0);
      }()>;
    };
  else
    return false;
}

} // namespace detail

template <detail::size_type N = 0, detail::simd_vec_type V, class IdxMap>
  requires std::integral<std::invoke_result_t<IdxMap &, detail::size_type>> ||
           std::integral<std::invoke_result_t<IdxMap &, detail::size_type,
                                              detail::size_type>>
constexpr resize_t<(N == 0 ? V::size() : N), V> permute(const V &v,
                                                        IdxMap &&idxmap) {
  using R = resize_t<(N == 0 ? V::size() : N), V>;
#if defined(NX_SIMD_VECTOR_EXT) && defined(__clang__)
  using Map = std::remove_cvref_t<IdxMap>;
  if constexpr (detail::constant_index_map<V, IdxMap, R::size()>() &&
                detail::is_abi_vector<typename V::abi_type>::value &&
                detail::is_abi_vector<typename R::abi_type>::value) {
    using SO = detail::vops<V>;
    using RO = detail::vops<R>;
    return [&]<int... I>(std::integer_sequence<int, I...>) {
      constexpr auto at = [](int i) {
        Map m{};
        const int k = detail::permute_index<V>(m, i);
        return k < 0 ? V::size() : k;
      };
      using S = typename SO::storage;
      return detail::access::make<R>(
          (typename RO::storage)__builtin_shufflevector(detail::access::data(v),
                                                        S{}, at(I)...));
    }(std::make_integer_sequence<int, R::size()>{});
  }
#endif
  return detail::permute_with<R>(v, [&](int i) {
    const detail::size_type k = detail::permute_index<V>(idxmap, i);
    assert(k == zero_element || k == uninit_element ||
           (k >= 0 && k < V::size()));
    return k;
  });
}

template <detail::size_type N = 0, detail::simd_mask_type V, class IdxMap>
  requires std::integral<std::invoke_result_t<IdxMap &, detail::size_type>> ||
           std::integral<std::invoke_result_t<IdxMap &, detail::size_type,
                                              detail::size_type>>
constexpr resize_t<(N == 0 ? V::size() : N), V> permute(const V &v,
                                                        IdxMap &&idxmap) {
  using R = resize_t<(N == 0 ? V::size() : N), V>;
  return detail::permute_with<R>(v, [&](int i) {
    const detail::size_type k = detail::permute_index<V>(idxmap, i);
    assert(k == zero_element || k == uninit_element ||
           (k >= 0 && k < V::size()));
    return k;
  });
}

template <detail::simd_vec_type V, detail::simd_integral I>
constexpr resize_t<I::size(), V> permute(const V &v, const I &indices) {
  using R = resize_t<I::size(), V>;
  return detail::permute_with<R>(v, [&](int i) {
    const auto k = static_cast<detail::size_type>(indices[i]);
    assert(k >= 0 && k < V::size());
    return k;
  });
}
template <detail::simd_mask_type V, detail::simd_integral I>
constexpr resize_t<I::size(), V> permute(const V &v, const I &indices) {
  using R = resize_t<I::size(), V>;
  return detail::permute_with<R>(v, [&](int i) {
    const auto k = static_cast<detail::size_type>(indices[i]);
    assert(k >= 0 && k < V::size());
    return k;
  });
}

namespace detail {

template <class V, class K, class Fill>
constexpr V compress_impl(const V &v, const K &selector, Fill fill) {
  typename V::value_type values[V::size()];
  int n = 0;
  for (int i = 0; i < V::size(); ++i)
    if (selector[i])
      values[n++] = v[i];
  for (int i = n; i < V::size(); ++i)
    values[i] = fill(i);
  if constexpr (simd_mask_type<V>)
    return make_mask<V>([&](int i) { return values[i]; });
  else
    return vec_generate<V>([&](int i) { return values[i]; });
}

template <class V, class K>
constexpr V expand_impl(const V &v, const K &selector, const V &original) {
  typename V::value_type values[V::size()];
  int n = 0;
  for (int i = 0; i < V::size(); ++i)
    values[i] = selector[i] ? v[n++] : original[i];
  if constexpr (simd_mask_type<V>)
    return make_mask<V>([&](int i) { return values[i]; });
  else
    return vec_generate<V>([&](int i) { return values[i]; });
}

} // namespace detail

template <detail::simd_vec_type V>
constexpr V compress(const V &v, const typename V::mask_type &selector) {
  return detail::compress_impl(v, selector, [&](int i) { return v[i]; });
}
template <detail::simd_mask_type V>
constexpr V compress(const V &v, const std::type_identity_t<V> &selector) {
  return detail::compress_impl(v, selector, [&](int i) { return v[i]; });
}
template <detail::simd_vec_type V>
constexpr V compress(const V &v, const typename V::mask_type &selector,
                     const typename V::value_type &fill_value) {
  return detail::compress_impl(v, selector, [&](int) { return fill_value; });
}
template <detail::simd_mask_type V>
constexpr V compress(const V &v, const std::type_identity_t<V> &selector,
                     const typename V::value_type &fill_value) {
  return detail::compress_impl(v, selector, [&](int) { return fill_value; });
}

template <detail::simd_vec_type V>
constexpr V expand(const V &v, const typename V::mask_type &selector,
                   const V &original = {}) {
  return detail::expand_impl(v, selector, original);
}
template <detail::simd_mask_type V>
constexpr V expand(const V &v, const std::type_identity_t<V> &selector,
                   const V &original = {}) {
  return detail::expand_impl(v, selector, original);
}

namespace detail {

template <class V, class R, class I>
using gather_vec_t =
    std::conditional_t<std::is_void_v<V>,
                       vec<std::ranges::range_value_t<R>, I::size()>, V>;

template <class V, class R, class I, class... Flags>
constexpr V gather(R &&in, const typename I::mask_type *mask, const I &indices,
                   bool partial) {
  using T = typename V::value_type;
  using U = std::ranges::range_value_t<R>;
  static_assert(V::size() == I::size(), "one index per element");
  check_load<V, U, Flags...>();
  const auto *p = std::ranges::data(in);
  const auto n = std::ranges::size(in);
  check_aligned_flag<V, R>(p, has_flag<aligned_flag, Flags...>);
  check_alignment<Flags...>(p);
  return vec_generate<V>([&](int i) {
    if (mask && !(*mask)[i])
      return T();
    const auto k = indices[i];
    const bool in_range = std::cmp_greater_equal(k, 0) && std::cmp_less(k, n);
    if (!partial)
      assert(in_range);
    return in_range ? static_cast<T>(p[k]) : T();
  });
}

template <class V, class R, class I, class... Flags>
constexpr void scatter(const V &v, R &&out, const typename I::mask_type *mask,
                       const I &indices, bool partial) {
  using U = std::ranges::range_value_t<R>;
  check_store<typename V::value_type, U, Flags...>();
  auto *p = std::ranges::data(out);
  const auto n = std::ranges::size(out);
  check_aligned_flag<V, R>(p, has_flag<aligned_flag, Flags...>);
  check_alignment<Flags...>(p);
  for (int i = 0; i < V::size(); ++i) {
    if (mask && !(*mask)[i])
      continue;
    const auto k = indices[i];
    const bool in_range = std::cmp_greater_equal(k, 0) && std::cmp_less(k, n);
    if (!partial)
      assert(in_range);
    if (in_range)
      p[k] = static_cast<U>(v[i]);
  }
}

} // namespace detail

template <class V = void, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto unchecked_gather_from(R &&in, const I &indices,
                                     flags<Flags...> = {})
    -> detail::gather_vec_t<V, R, I> {
  return detail::gather<detail::gather_vec_t<V, R, I>, R &, I, Flags...>(
      in, nullptr, indices, false);
}
template <class V = void, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto unchecked_gather_from(R &&in, const typename I::mask_type &mask,
                                     const I &indices, flags<Flags...> = {})
    -> detail::gather_vec_t<V, R, I> {
  return detail::gather<detail::gather_vec_t<V, R, I>, R &, I, Flags...>(
      in, &mask, indices, false);
}
template <class V = void, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto partial_gather_from(R &&in, const I &indices,
                                   flags<Flags...> = {})
    -> detail::gather_vec_t<V, R, I> {
  return detail::gather<detail::gather_vec_t<V, R, I>, R &, I, Flags...>(
      in, nullptr, indices, true);
}
template <class V = void, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R>
constexpr auto partial_gather_from(R &&in, const typename I::mask_type &mask,
                                   const I &indices, flags<Flags...> = {})
    -> detail::gather_vec_t<V, R, I> {
  return detail::gather<detail::gather_vec_t<V, R, I>, R &, I, Flags...>(
      in, &mask, indices, true);
}

template <detail::simd_vec_type V, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R> && (V::size() == I::size()) &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<typename V::value_type,
                                             std::ranges::range_value_t<R>>
constexpr void unchecked_scatter_to(const V &v, R &&out, const I &indices,
                                    flags<Flags...> = {}) {
  detail::scatter<V, R &, I, Flags...>(v, out, nullptr, indices, false);
}
template <detail::simd_vec_type V, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R> && (V::size() == I::size()) &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<typename V::value_type,
                                             std::ranges::range_value_t<R>>
constexpr void unchecked_scatter_to(const V &v, R &&out,
                                    const typename I::mask_type &mask,
                                    const I &indices, flags<Flags...> = {}) {
  detail::scatter<V, R &, I, Flags...>(v, out, &mask, indices, false);
}
template <detail::simd_vec_type V, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R> && (V::size() == I::size()) &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<typename V::value_type,
                                             std::ranges::range_value_t<R>>
constexpr void partial_scatter_to(const V &v, R &&out, const I &indices,
                                  flags<Flags...> = {}) {
  detail::scatter<V, R &, I, Flags...>(v, out, nullptr, indices, true);
}
template <detail::simd_vec_type V, std::ranges::contiguous_range R,
          detail::simd_integral I, class... Flags>
  requires std::ranges::sized_range<R> && (V::size() == I::size()) &&
           std::indirectly_writable<std::ranges::iterator_t<R>,
                                    std::ranges::range_value_t<R>> &&
           detail::explicitly_convertible_to<typename V::value_type,
                                             std::ranges::range_value_t<R>>
constexpr void partial_scatter_to(const V &v, R &&out,
                                  const typename I::mask_type &mask,
                                  const I &indices, flags<Flags...> = {}) {
  detail::scatter<V, R &, I, Flags...>(v, out, &mask, indices, true);
}

namespace detail {

template <class T, class X> constexpr T slice(const X &x, int offset) {
  if constexpr (simd_mask_type<T>) {
    return make_mask<T>([&](int i) { return x[offset + i]; });
  } else {
    using TO = vops<T>;
    if constexpr (requires {
                    typename X::abi_type::part;
                    requires std::is_same_v<typename X::abi_type::part,
                                            typename T::abi_type>;
                  }) {
      if (!std::is_constant_evaluated() && offset % T::size() == 0)
        return access::make<T>(access::data(x).v[offset / T::size()]);
    }
    return access::make<T>(TO::generate([&](int i) { return x[offset + i]; }));
  }
}

template <class T, class X> constexpr auto chunk_impl(const X &x) {
  constexpr int W = T::size();
  constexpr int count = X::size() / W;
  constexpr int rest = X::size() % W;
  if constexpr (rest == 0) {
    return [&]<int... K>(std::integer_sequence<int, K...>) {
      return std::array<T, count>{slice<T>(x, K * W)...};
    }(std::make_integer_sequence<int, count>{});
  } else {
    return [&]<int... K>(std::integer_sequence<int, K...>) {
      return std::tuple<std::conditional_t<(K >= 0), T, void>...,
                        resize_t<rest, T>>{
          slice<T>(x, K * W)..., slice<resize_t<rest, T>>(x, count * W)};
    }(std::make_integer_sequence<int, count>{});
  }
}

} // namespace detail

template <class T, class Abi>
  requires detail::simd_vec_type<T>
constexpr auto chunk(const basic_vec<typename T::value_type, Abi> &x) noexcept {
  return detail::chunk_impl<T>(x);
}
template <class T, class Abi>
  requires detail::simd_mask_type<T>
constexpr auto
chunk(const basic_mask<detail::mask_traits<T>::bytes, Abi> &x) noexcept {
  return detail::chunk_impl<T>(x);
}
template <detail::size_type N, class T, class Abi>
constexpr auto chunk(const basic_vec<T, Abi> &x) noexcept {
  return chunk<resize_t<N, basic_vec<T, Abi>>>(x);
}
template <detail::size_type N, size_t Bytes, class Abi>
constexpr auto chunk(const basic_mask<Bytes, Abi> &x) noexcept {
  return chunk<resize_t<N, basic_mask<Bytes, Abi>>>(x);
}

template <class T, class... Abis>
constexpr resize_t<(basic_vec<T, Abis>::size() + ...),
                   basic_vec<T, std::tuple_element_t<0, std::tuple<Abis...>>>>
cat(const basic_vec<T, Abis> &...xs) noexcept {
  using R =
      resize_t<(basic_vec<T, Abis>::size() + ...),
               basic_vec<T, std::tuple_element_t<0, std::tuple<Abis...>>>>;
  T values[R::size()];
  int offset = 0;
  ((detail::vops<basic_vec<T, Abis>>::store(detail::access::data(xs),
                                            values + offset),
    offset += basic_vec<T, Abis>::size()),
   ...);
  return detail::access::make<R>(detail::vops<R>::load(values));
}
template <size_t Bytes, class... Abis>
constexpr resize_t<
    (basic_mask<Bytes, Abis>::size() + ...),
    basic_mask<Bytes, std::tuple_element_t<0, std::tuple<Abis...>>>>
cat(const basic_mask<Bytes, Abis> &...xs) noexcept {
  using R =
      resize_t<(basic_mask<Bytes, Abis>::size() + ...),
               basic_mask<Bytes, std::tuple_element_t<0, std::tuple<Abis...>>>>;
  bool values[R::size()] = {};
  int offset = 0;
  (([&] {
     for (int i = 0; i < basic_mask<Bytes, Abis>::size(); ++i)
       values[offset + i] = xs[i];
     offset += basic_mask<Bytes, Abis>::size();
   }()),
   ...);
  return detail::make_mask<R>([&](int i) { return values[i]; });
}

template <class T>
  requires(detail::vectorizable<T> && std::is_arithmetic_v<T>) ||
              (detail::simd_vec_type<T> &&
               std::is_arithmetic_v<typename T::value_type> &&
               T::size() - 1 <=
                   std::numeric_limits<typename T::value_type>::max())
inline constexpr T iota = [] {
  if constexpr (std::is_arithmetic_v<T>)
    return T();
  else
    return detail::vec_generate<T>(
        [](int i) { return static_cast<typename T::value_type>(i); });
}();

} // namespace nx::simd
