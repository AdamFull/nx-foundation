#pragma once

#include "core/foundation/simd/backend_complex.h"
#include "core/foundation/simd/backend_generic.h"
#include "core/foundation/simd/backend_vector.h"
#include "core/foundation/simd/backend_x86.h"

#include <bitset>
#include <cassert>
#include <compare>
#include <iterator>
#include <ranges>
#include <span>

namespace nx::simd {

namespace detail {

struct convert_flag {};
struct aligned_flag {};
template <size_t N> struct overaligned_flag {
  static constexpr size_t value = N;
};

template <class F> struct overaligned_of : std::integral_constant<size_t, 0> {};
template <size_t N>
struct overaligned_of<overaligned_flag<N>> : std::integral_constant<size_t, N> {
};

template <class Merged, class... Rest> struct merge_flags {
  using type = Merged;
};
template <class... Have, class Next, class... Rest>
struct merge_flags<std::tuple<Have...>, Next, Rest...> {
  using type = typename merge_flags<
      std::conditional_t<(std::is_same_v<Have, Next> || ...),
                         std::tuple<Have...>, std::tuple<Have..., Next>>,
      Rest...>::type;
};

} // namespace detail

template <class... Flags> struct flags {
  template <class... Other>
  friend consteval auto operator|(flags, flags<Other...>) {
    return []<class... F>(std::tuple<F...> *) {
      return flags<F...>{};
    }(static_cast<typename detail::merge_flags<std::tuple<>, Flags...,
                                               Other...>::type *>(nullptr));
  }
};

inline constexpr flags<> flag_default{};
inline constexpr flags<detail::convert_flag> flag_convert{};
inline constexpr flags<detail::aligned_flag> flag_aligned{};
template <size_t N>
  requires(std::has_single_bit(N))
inline constexpr flags<detail::overaligned_flag<N>> flag_overaligned{};

namespace detail {

template <class Flag, class... Flags>
inline constexpr bool has_flag = (std::is_same_v<Flag, Flags> || ...);

template <class... Flags>
inline constexpr bool has_convert_flag = has_flag<convert_flag, Flags...>;

template <class... Flags>
inline constexpr size_t overaligned_value =
    (size_t{0} | ... | overaligned_of<Flags>::value);

template <class T> struct is_basic_vec : std::false_type {};
template <class T, class Abi>
struct is_basic_vec<basic_vec<T, Abi>> : std::true_type {};
template <class T> struct is_basic_mask : std::false_type {};
template <size_t B, class Abi>
struct is_basic_mask<basic_mask<B, Abi>> : std::true_type {};

template <class V>
concept simd_vec_type =
    is_basic_vec<V>::value && std::is_default_constructible_v<V>;
template <class V>
concept simd_mask_type =
    is_basic_mask<V>::value && std::is_default_constructible_v<V>;
template <class V>
concept simd_integral =
    simd_vec_type<V> && std::integral<typename V::value_type>;
template <class V>
concept simd_floating_point =
    simd_vec_type<V> && std::floating_point<typename V::value_type>;
template <class V>
concept simd_complex = simd_vec_type<V> && is_complex_v<typename V::value_type>;

template <size_t Bytes> struct mask_element;
template <> struct mask_element<1> {
  using type = i8;
};
template <> struct mask_element<2> {
  using type = i16;
};
template <> struct mask_element<4> {
  using type = i32;
};
template <> struct mask_element<8> {
  using type = i64;
};
template <> struct mask_element<16> {
  using type = std::complex<double>;
};
template <size_t Bytes>
using mask_element_t = typename mask_element<Bytes>::type;

inline constexpr size_t dynamic_size = static_cast<size_t>(-1);

template <class R> consteval size_t static_range_size() {
  using U = std::remove_cvref_t<R>;
  if constexpr (std::is_bounded_array_v<U>)
    return std::extent_v<U>;
  else if constexpr (requires { std::tuple_size<U>::value; })
    return std::tuple_size_v<U>;
  else if constexpr (requires { U::extent; })
    return U::extent == std::dynamic_extent ? dynamic_size : U::extent;
  else
    return dynamic_size;
}

template <class V> class simd_iterator;

struct access {
  template <class V> static constexpr auto &data(V &v) noexcept {
    return v.m_data;
  }
  template <class V> static constexpr const auto &data(const V &v) noexcept {
    return v.m_data;
  }
  template <class V, class S> static constexpr V make(const S &s) noexcept {
    return V(private_init, s);
  }
};

template <class R, class T> consteval bool generated_value_fits() {
  if constexpr (!std::convertible_to<R, T>)
    return false;
  else if constexpr (std::is_arithmetic_v<std::remove_cvref_t<R>>)
    return value_preserving<std::remove_cvref_t<R>, T>();
  else
    return true;
}

template <class G, class T, int... I>
consteval bool is_generator(std::integer_sequence<int, I...>) {
  if constexpr (!(std::invocable<G &, std::integral_constant<size_type, I>> &&
                  ...))
    return false;
  else
    return (generated_value_fits<
                std::invoke_result_t<G &, std::integral_constant<size_type, I>>,
                T>() &&
            ...);
}

template <class G, class T, int N>
concept generator_for =
    is_generator<G, T>(std::make_integer_sequence<int, N>{});

template <class T, int N, class G> constexpr auto generate_values(G &gen) {
  struct values {
    T v[N];
  } r{};
  [&]<int... I>(std::integer_sequence<int, I...>) {
    ((r.v[I] = static_cast<T>(gen(std::integral_constant<size_type, I>{}))),
     ...);
  }(std::make_integer_sequence<int, N>{});
  return r;
}

template <class A> struct is_abi_vector : std::false_type {};
#if defined(NX_SIMD_VECTOR_EXT)
template <int Bytes>
struct is_abi_vector<abi_vector<Bytes>> : std::true_type {};
#endif

template <class T, int N> struct real_vec {
  struct type {};
};
template <class F, int N> struct real_vec<std::complex<F>, N> {
  using type = basic_vec<F, deduce_abi_t<F, N>>;
};

template <size_t Bytes, class Abi> consteval bool mask_lanes_are_integers() {
  if constexpr (Bytes > 8)
    return false;
  else if constexpr (simd_size<integer_from<Bytes>, Abi>() !=
                     mask_size<Bytes, Abi>())
    return false;
  else
    return std::is_same_v<mask_storage<Bytes, Abi>,
                          vec_storage<integer_from<Bytes>, Abi>>;
}

template <size_t B, class A, size_t UB, class UA>
constexpr mask_storage<B, A> convert_mask(const mask_storage<UB, UA> &s) {
  using To = mask_ops<B, A>;
  using From = mask_ops<UB, UA>;
  if constexpr (std::is_same_v<To, From>)
    return s;
#if defined(NX_SIMD_VECTOR_EXT)
  else if constexpr (is_abi_vector<A>::value && is_abi_vector<UA>::value &&
                     B <= 8 && UB <= 8)
    return __builtin_convertvector(s, typename To::storage);
#endif
  else if (std::is_constant_evaluated())
    return To::generate([&](int i) { return From::get(s, i); });
  else
    return To::from_bits(From::to_bits(s));
}

} // namespace detail

template <class T, class U = typename T::value_type> struct alignment {};
template <class T, class Abi, class U>
  requires detail::vectorizable<U> && (detail::simd_size<T, Abi>() > 0)
struct alignment<basic_vec<T, Abi>, U>
    : std::integral_constant<
          size_t,
          std::bit_ceil(sizeof(U) * size_t(detail::simd_size<T, Abi>())) < 64
              ? std::bit_ceil(sizeof(U) * size_t(detail::simd_size<T, Abi>()))
              : 64> {};
template <class T, class U = typename T::value_type>
inline constexpr size_t alignment_v = alignment<T, U>::value;

template <class T, class V> struct rebind {};
template <class T, class U, class Abi>
  requires detail::vectorizable<T> && requires {
    typename detail::deduce_abi_t<T, basic_vec<U, Abi>::size()>;
  }
struct rebind<T, basic_vec<U, Abi>> {
  using type = basic_vec<T, detail::deduce_abi_t<T, basic_vec<U, Abi>::size()>>;
};
template <class T, size_t B, class Abi>
  requires detail::vectorizable<T> && requires {
    typename detail::deduce_abi_t<T, basic_mask<B, Abi>::size()>;
  }
struct rebind<T, basic_mask<B, Abi>> {
  using type = basic_mask<sizeof(T),
                          detail::deduce_abi_t<T, basic_mask<B, Abi>::size()>>;
};
template <class T, class V> using rebind_t = typename rebind<T, V>::type;

template <detail::size_type N, class V> struct resize {};
template <detail::size_type N, class T, class Abi>
  requires requires { typename detail::deduce_abi_t<T, N>; }
struct resize<N, basic_vec<T, Abi>> {
  using type = basic_vec<T, detail::deduce_abi_t<T, N>>;
};
template <detail::size_type N, size_t B, class Abi>
  requires requires {
    typename detail::deduce_abi_t<detail::mask_element_t<B>, N>;
  }
struct resize<N, basic_mask<B, Abi>> {
  using type =
      basic_mask<B, detail::deduce_abi_t<detail::mask_element_t<B>, N>>;
};
template <detail::size_type N, class V>
using resize_t = typename resize<N, V>::type;

template <class T, int N = detail::simd_size<T, detail::native_abi<T>>()>
using vec = basic_vec<T, detail::deduce_abi_t<T, N>>;

template <class T, int N = detail::simd_size<T, detail::native_abi<T>>()>
using mask = basic_mask<sizeof(T), detail::deduce_abi_t<T, N>>;

namespace detail {

template <class V> class simd_iterator {
  template <class> friend class simd_iterator;

  V *m_data = nullptr;
  size_type m_offset = 0;

public:
  using value_type = typename V::value_type;
  using iterator_category = std::input_iterator_tag;
  using iterator_concept = std::random_access_iterator_tag;
  using difference_type = size_type;

  constexpr simd_iterator() = default;
  constexpr simd_iterator(V &data, size_type offset) noexcept
      : m_data(&data), m_offset(offset) {}
  template <class U>
    requires std::is_const_v<V> && std::is_same_v<U, std::remove_const_t<V>>
  constexpr simd_iterator(const simd_iterator<U> &i)
      : m_data(i.m_data), m_offset(i.m_offset) {}

  constexpr value_type operator*() const { return (*m_data)[m_offset]; }
  constexpr value_type operator[](difference_type n) const {
    return (*m_data)[m_offset + n];
  }

  constexpr simd_iterator &operator++() {
    ++m_offset;
    return *this;
  }
  constexpr simd_iterator operator++(int) {
    simd_iterator r = *this;
    ++m_offset;
    return r;
  }
  constexpr simd_iterator &operator--() {
    --m_offset;
    return *this;
  }
  constexpr simd_iterator operator--(int) {
    simd_iterator r = *this;
    --m_offset;
    return r;
  }
  constexpr simd_iterator &operator+=(difference_type n) {
    m_offset += n;
    return *this;
  }
  constexpr simd_iterator &operator-=(difference_type n) {
    m_offset -= n;
    return *this;
  }

  friend constexpr simd_iterator operator+(simd_iterator i, difference_type n) {
    return i += n;
  }
  friend constexpr simd_iterator operator+(difference_type n, simd_iterator i) {
    return i += n;
  }
  friend constexpr simd_iterator operator-(simd_iterator i, difference_type n) {
    return i -= n;
  }
  friend constexpr difference_type operator-(const simd_iterator &a,
                                             const simd_iterator &b) {
    return a.m_offset - b.m_offset;
  }

  friend constexpr bool operator==(const simd_iterator &a,
                                   const simd_iterator &b) {
    return a.m_offset == b.m_offset;
  }
  friend constexpr auto operator<=>(const simd_iterator &a,
                                    const simd_iterator &b) {
    return a.m_offset <=> b.m_offset;
  }
  friend constexpr bool operator==(const simd_iterator &i,
                                   std::default_sentinel_t) noexcept {
    return i.m_offset == V::size();
  }
  friend constexpr difference_type operator-(const simd_iterator &i,
                                             std::default_sentinel_t) noexcept {
    return i.m_offset - V::size();
  }
  friend constexpr difference_type operator-(std::default_sentinel_t,
                                             const simd_iterator &i) noexcept {
    return V::size() - i.m_offset;
  }
};

} // namespace detail

template <size_t Bytes, class Abi> class basic_mask {
public:
  using value_type = bool;
  using abi_type = Abi;

  basic_mask() = delete;
  ~basic_mask() = delete;
  basic_mask(const basic_mask &) = delete;
  basic_mask &operator=(const basic_mask &) = delete;
};

// The explicit alignment survives the #pragma pack(8) that MSVC's standard
// headers apply, which would otherwise under-align a vector inside std::pair,
// std::array or std::tuple under clang-cl.
template <size_t Bytes, class Abi>
  requires(detail::mask_size<Bytes, Abi>() > 0)
class alignas(alignof(detail::mask_storage<Bytes, Abi>))
    basic_mask<Bytes, Abi> {
  using ops = detail::mask_ops<Bytes, Abi>;
  using storage = typename ops::storage;
  static constexpr int N = detail::mask_size<Bytes, Abi>();

  friend struct detail::access;
  template <size_t, class> friend class basic_mask;
  template <class, class> friend class basic_vec;

  storage m_data;

  constexpr basic_mask(detail::private_init_t, const storage &s) noexcept
      : m_data(s) {}

  using unary_vec =
      basic_vec<detail::integer_from<(Bytes <= 8 ? Bytes : 8)>, Abi>;

  template <class U>
  constexpr basic_vec<U, Abi> to_vec(U t, U f) const noexcept {
    using V = basic_vec<U, Abi>;
    using VO = typename V::ops;
    return detail::access::make<V>(
        VO::select(m_data, VO::broadcast(t), VO::broadcast(f)));
  }

public:
  using value_type = bool;
  using abi_type = Abi;
  using iterator = detail::simd_iterator<basic_mask>;
  using const_iterator = detail::simd_iterator<const basic_mask>;

  static constexpr std::integral_constant<detail::size_type, N> size{};

  constexpr basic_mask() noexcept = default;

  constexpr explicit basic_mask(std::same_as<value_type> auto b) noexcept
      : m_data(ops::broadcast(b)) {}

  template <size_t UBytes, class UAbi>
    requires(basic_mask<UBytes, UAbi>::size() == N)
  constexpr explicit basic_mask(const basic_mask<UBytes, UAbi> &x) noexcept
      : m_data(detail::convert_mask<Bytes, Abi, UBytes, UAbi>(x.m_data)) {}

  template <class G>
    requires detail::generator_for<G, bool, N>
  constexpr explicit basic_mask(G &&gen)
      : m_data([&] {
          const auto values = detail::generate_values<bool, N>(gen);
          return ops::generate([&](int i) { return values.v[i]; });
        }()) {}

  template <std::same_as<std::bitset<N>> T>
  constexpr basic_mask(const T &b) noexcept
      : m_data(ops::generate([&](int i) { return b[i]; })) {}

  template <std::unsigned_integral T>
    requires(!std::same_as<T, value_type>)
  constexpr explicit basic_mask(T val) noexcept
      : m_data(ops::from_bits(static_cast<u64>(val))) {}

  constexpr iterator begin() noexcept { return iterator(*this, 0); }
  constexpr const_iterator begin() const noexcept {
    return const_iterator(*this, 0);
  }
  constexpr const_iterator cbegin() const noexcept { return begin(); }
  constexpr std::default_sentinel_t end() const noexcept { return {}; }
  constexpr std::default_sentinel_t cend() const noexcept { return {}; }

  constexpr value_type operator[](detail::size_type i) const {
    assert(i >= 0 && i < N);
    return ops::get(m_data, i);
  }
  template <detail::simd_integral I>
  constexpr resize_t<I::size(), basic_mask> operator[](const I &indices) const {
    return permute(*this, indices);
  }

  constexpr basic_mask operator!() const noexcept {
    return basic_mask(detail::private_init, ops::lnot(m_data));
  }
  // A register mask already is the vector of -1 and 0 lanes.
  static constexpr bool lanes_are_integers =
      detail::mask_lanes_are_integers<Bytes, Abi>();

  constexpr unary_vec operator+() const noexcept
    requires(Bytes <= 8)
  {
    using I = detail::integer_from<Bytes>;
    if constexpr (lanes_are_integers)
      return -detail::access::make<unary_vec>(m_data);
    else
      return to_vec<I>(I(1), I(0));
  }
  constexpr unary_vec operator-() const noexcept
    requires(Bytes <= 8)
  {
    using I = detail::integer_from<Bytes>;
    if constexpr (lanes_are_integers)
      return detail::access::make<unary_vec>(m_data);
    else
      return to_vec<I>(I(-1), I(0));
  }
  constexpr unary_vec operator~() const noexcept
    requires(Bytes <= 8)
  {
    using I = detail::integer_from<Bytes>;
    return to_vec<I>(I(-2), I(-1));
  }

  template <class U, class A>
    requires(detail::simd_size<U, A>() == N)
  constexpr explicit(sizeof(U) != Bytes)
  operator basic_vec<U, A>() const noexcept {
    const basic_mask<sizeof(U), A> k(*this);
    return k.template to_vec<U>(U(1), U(0));
  }

  constexpr std::bitset<N> to_bitset() const noexcept {
    return std::bitset<N>(
        static_cast<unsigned long long>(ops::to_bits(m_data)));
  }
  constexpr unsigned long long to_ullong() const {
    return static_cast<unsigned long long>(ops::to_bits(m_data));
  }

  friend constexpr basic_mask operator&&(const basic_mask &a,
                                         const basic_mask &b) noexcept {
    return basic_mask(detail::private_init, ops::land(a.m_data, b.m_data));
  }
  friend constexpr basic_mask operator||(const basic_mask &a,
                                         const basic_mask &b) noexcept {
    return basic_mask(detail::private_init, ops::lor(a.m_data, b.m_data));
  }
  friend constexpr basic_mask operator&(const basic_mask &a,
                                        const basic_mask &b) noexcept {
    return basic_mask(detail::private_init, ops::land(a.m_data, b.m_data));
  }
  friend constexpr basic_mask operator|(const basic_mask &a,
                                        const basic_mask &b) noexcept {
    return basic_mask(detail::private_init, ops::lor(a.m_data, b.m_data));
  }
  friend constexpr basic_mask operator^(const basic_mask &a,
                                        const basic_mask &b) noexcept {
    return basic_mask(detail::private_init, ops::lxor(a.m_data, b.m_data));
  }
  friend constexpr basic_mask &operator&=(basic_mask &a,
                                          const basic_mask &b) noexcept {
    a.m_data = ops::land(a.m_data, b.m_data);
    return a;
  }
  friend constexpr basic_mask &operator|=(basic_mask &a,
                                          const basic_mask &b) noexcept {
    a.m_data = ops::lor(a.m_data, b.m_data);
    return a;
  }
  friend constexpr basic_mask &operator^=(basic_mask &a,
                                          const basic_mask &b) noexcept {
    a.m_data = ops::lxor(a.m_data, b.m_data);
    return a;
  }

  friend constexpr basic_mask simd_select_impl(const basic_mask &k,
                                               const basic_mask &a,
                                               const basic_mask &b) noexcept {
    return basic_mask(detail::private_init,
                      ops::select(k.m_data, a.m_data, b.m_data));
  }
  friend constexpr basic_mask
  simd_select_impl(const basic_mask &k, std::same_as<bool> auto a,
                   std::same_as<bool> auto b) noexcept {
    return basic_mask(
        detail::private_init,
        ops::select(k.m_data, ops::broadcast(a), ops::broadcast(b)));
  }
  template <class T0, class T1>
    requires std::same_as<T0, T1> && detail::vectorizable<T0> &&
             (sizeof(T0) == Bytes)
  friend constexpr vec<T0, N> simd_select_impl(const basic_mask &k, const T0 &a,
                                               const T1 &b) noexcept {
    using R = vec<T0, N>;
    using RO = detail::vec_ops<T0, typename R::abi_type>;
    const typename R::mask_type rk(k);
    return detail::access::make<R>(RO::select(
        detail::access::data(rk), RO::broadcast(a), RO::broadcast(b)));
  }

  friend constexpr basic_mask operator==(const basic_mask &a,
                                         const basic_mask &b) noexcept {
    return !(a ^ b);
  }
  friend constexpr basic_mask operator!=(const basic_mask &a,
                                         const basic_mask &b) noexcept {
    return a ^ b;
  }
  friend constexpr basic_mask operator>=(const basic_mask &a,
                                         const basic_mask &b) noexcept {
    return a || !b;
  }
  friend constexpr basic_mask operator<=(const basic_mask &a,
                                         const basic_mask &b) noexcept {
    return !a || b;
  }
  friend constexpr basic_mask operator>(const basic_mask &a,
                                        const basic_mask &b) noexcept {
    return a && !b;
  }
  friend constexpr basic_mask operator<(const basic_mask &a,
                                        const basic_mask &b) noexcept {
    return !a && b;
  }
};

template <class T, class Abi = detail::native_abi<T>> class basic_vec {
public:
  using value_type = T;
  using abi_type = Abi;

  basic_vec() = delete;
  ~basic_vec() = delete;
  basic_vec(const basic_vec &) = delete;
  basic_vec &operator=(const basic_vec &) = delete;
};

template <class T, class Abi>
  requires(detail::simd_size<T, Abi>() > 0)
class alignas(alignof(detail::vec_storage<T, Abi>)) basic_vec<T, Abi> {
  using ops = detail::vec_ops<T, Abi>;
  using storage = typename ops::storage;
  static constexpr int N = detail::simd_size<T, Abi>();

  friend struct detail::access;
  template <size_t, class> friend class basic_mask;
  template <class, class> friend class basic_vec;

  storage m_data;

  constexpr basic_vec(detail::private_init_t, const storage &s) noexcept
      : m_data(s) {}

  static constexpr basic_vec make(const storage &s) noexcept {
    return basic_vec(detail::private_init, s);
  }

  using real_type = typename detail::real_vec<T, N>::type;

public:
  using value_type = T;
  using mask_type = basic_mask<sizeof(T), Abi>;
  using abi_type = Abi;
  using iterator = detail::simd_iterator<basic_vec>;
  using const_iterator = detail::simd_iterator<const basic_vec>;

  static constexpr std::integral_constant<detail::size_type, N> size{};

  constexpr basic_vec() noexcept = default;

  template <class U>
    requires detail::broadcast_from<std::remove_cvref_t<U>, T>
  constexpr basic_vec(U &&x) noexcept
      : m_data(ops::broadcast(static_cast<T>(x))) {}

  template <class U, class UAbi>
    requires(detail::simd_size<U, UAbi>() == N &&
             detail::explicitly_convertible_to<U, T>)
  constexpr explicit(!detail::value_preserving<U, T>() ||
                     detail::higher_rank<U, T>())
      basic_vec(const basic_vec<U, UAbi> &x) noexcept
      : m_data(ops::template convert<U, UAbi>(x.m_data)) {}

  template <class G>
    requires detail::generator_for<G, T, N>
  constexpr explicit basic_vec(G &&gen)
      : m_data([&] {
          const auto values = detail::generate_values<T, N>(gen);
          return ops::generate([&](int i) { return values.v[i]; });
        }()) {}

  template <std::ranges::contiguous_range R, class... Flags>
    requires std::ranges::sized_range<R> &&
             (detail::static_range_size<R>() == size_t(N)) &&
             detail::vectorizable<std::ranges::range_value_t<R>> &&
             detail::explicitly_convertible_to<std::ranges::range_value_t<R>, T>
  constexpr basic_vec(R &&r, flags<Flags...> = {})
      : m_data(ops::load(std::ranges::data(r))) {
    static_assert(
        detail::has_convert_flag<Flags...> ||
            detail::value_preserving<std::ranges::range_value_t<R>, T>(),
        "a conversion that is not value-preserving needs "
        "flag_convert");
  }

  template <std::ranges::contiguous_range R, class... Flags>
    requires std::ranges::sized_range<R> &&
             (detail::static_range_size<R>() == size_t(N)) &&
             detail::vectorizable<std::ranges::range_value_t<R>> &&
             detail::explicitly_convertible_to<std::ranges::range_value_t<R>, T>
  constexpr basic_vec(R &&r, const mask_type &k, flags<Flags...> = {})
      : m_data(ops::load_masked(std::ranges::data(r), detail::access::data(k),
                                N)) {
    static_assert(
        detail::has_convert_flag<Flags...> ||
            detail::value_preserving<std::ranges::range_value_t<R>, T>(),
        "a conversion that is not value-preserving needs "
        "flag_convert");
  }

  constexpr basic_vec(const real_type &reals,
                      const real_type &imags = {}) noexcept
    requires detail::is_complex_v<T>
      : m_data{detail::access::data(reals), detail::access::data(imags)} {}

  constexpr iterator begin() noexcept { return iterator(*this, 0); }
  constexpr const_iterator begin() const noexcept {
    return const_iterator(*this, 0);
  }
  constexpr const_iterator cbegin() const noexcept { return begin(); }
  constexpr std::default_sentinel_t end() const noexcept { return {}; }
  constexpr std::default_sentinel_t cend() const noexcept { return {}; }

  constexpr value_type operator[](detail::size_type i) const {
    assert(i >= 0 && i < N);
    return ops::get(m_data, i);
  }
  template <detail::simd_integral I>
  constexpr resize_t<I::size(), basic_vec> operator[](const I &indices) const {
    return permute(*this, indices);
  }

  constexpr real_type real() const noexcept
    requires detail::is_complex_v<T>
  {
    return detail::access::make<real_type>(m_data.re);
  }
  constexpr real_type imag() const noexcept
    requires detail::is_complex_v<T>
  {
    return detail::access::make<real_type>(m_data.im);
  }
  constexpr void real(const real_type &x) noexcept
    requires detail::is_complex_v<T>
  {
    m_data.re = detail::access::data(x);
  }
  constexpr void imag(const real_type &x) noexcept
    requires detail::is_complex_v<T>
  {
    m_data.im = detail::access::data(x);
  }

  constexpr basic_vec &operator++() noexcept
    requires requires(value_type a) { ++a; }
  {
    m_data = ops::add(m_data, ops::broadcast(T(1)));
    return *this;
  }
  constexpr basic_vec operator++(int) noexcept
    requires requires(value_type a) { a++; }
  {
    basic_vec r = *this;
    ++*this;
    return r;
  }
  constexpr basic_vec &operator--() noexcept
    requires requires(value_type a) { --a; }
  {
    m_data = ops::sub(m_data, ops::broadcast(T(1)));
    return *this;
  }
  constexpr basic_vec operator--(int) noexcept
    requires requires(value_type a) { a--; }
  {
    basic_vec r = *this;
    --*this;
    return r;
  }

  constexpr mask_type operator!() const noexcept
    requires requires(const value_type a) { !a; }
  {
    return detail::access::make<mask_type>(
        ops::eq(m_data, ops::broadcast(T())));
  }
  constexpr basic_vec operator~() const noexcept
    requires requires(const value_type a) { ~a; }
  {
    return make(ops::bit_not(m_data));
  }
  constexpr basic_vec operator+() const noexcept
    requires requires(const value_type a) { +a; }
  {
    return *this;
  }
  constexpr basic_vec operator-() const noexcept
    requires requires(const value_type a) { -a; }
  {
    return make(ops::neg(m_data));
  }

#define NX_SIMD_BINARY(op, fn)                                                 \
  friend constexpr basic_vec operator op(const basic_vec &a,                   \
                                         const basic_vec &b) noexcept          \
    requires requires(value_type x, value_type y) { x op y; }                  \
  {                                                                            \
    return make(ops::fn(a.m_data, b.m_data));                                  \
  }                                                                            \
  friend constexpr basic_vec &operator op## =                                  \
      (basic_vec & a, const basic_vec &b) noexcept                             \
    requires requires(value_type x, value_type y) { x op y; }                  \
  {                                                                            \
    a.m_data = ops::fn(a.m_data, b.m_data);                                    \
    return a;                                                                  \
  }
  NX_SIMD_BINARY(+, add)
  NX_SIMD_BINARY(-, sub)
  NX_SIMD_BINARY(*, mul)
  NX_SIMD_BINARY(/, div)
  NX_SIMD_BINARY(%, mod)
  NX_SIMD_BINARY(&, bit_and)
  NX_SIMD_BINARY(|, bit_or)
  NX_SIMD_BINARY(^, bit_xor)
  NX_SIMD_BINARY(<<, shl)
  NX_SIMD_BINARY(>>, shr)
#undef NX_SIMD_BINARY

  friend constexpr basic_vec operator<<(const basic_vec &a,
                                        detail::size_type n) noexcept
    requires requires(value_type x, detail::size_type y) { x << y; }
  {
    return make(ops::shl_n(a.m_data, n));
  }
  friend constexpr basic_vec operator>>(const basic_vec &a,
                                        detail::size_type n) noexcept
    requires requires(value_type x, detail::size_type y) { x >> y; }
  {
    return make(ops::shr_n(a.m_data, n));
  }
  friend constexpr basic_vec &operator<<=(basic_vec &a,
                                          detail::size_type n) noexcept
    requires requires(value_type x, detail::size_type y) { x << y; }
  {
    a.m_data = ops::shl_n(a.m_data, n);
    return a;
  }
  friend constexpr basic_vec &operator>>=(basic_vec &a,
                                          detail::size_type n) noexcept
    requires requires(value_type x, detail::size_type y) { x >> y; }
  {
    a.m_data = ops::shr_n(a.m_data, n);
    return a;
  }

  friend constexpr basic_vec simd_select_impl(const mask_type &k,
                                              const basic_vec &a,
                                              const basic_vec &b) noexcept {
    return make(ops::select(detail::access::data(k), a.m_data, b.m_data));
  }

  friend constexpr mask_type operator==(const basic_vec &a,
                                        const basic_vec &b) noexcept {
    return detail::access::make<mask_type>(ops::eq(a.m_data, b.m_data));
  }
  friend constexpr mask_type operator!=(const basic_vec &a,
                                        const basic_vec &b) noexcept {
    return detail::access::make<mask_type>(ops::ne(a.m_data, b.m_data));
  }
  friend constexpr mask_type operator<(const basic_vec &a,
                                       const basic_vec &b) noexcept
    requires requires(value_type x, value_type y) { x < y; }
  {
    return detail::access::make<mask_type>(ops::lt(a.m_data, b.m_data));
  }
  friend constexpr mask_type operator<=(const basic_vec &a,
                                        const basic_vec &b) noexcept
    requires requires(value_type x, value_type y) { x <= y; }
  {
    return detail::access::make<mask_type>(ops::le(a.m_data, b.m_data));
  }
  friend constexpr mask_type operator>(const basic_vec &a,
                                       const basic_vec &b) noexcept
    requires requires(value_type x, value_type y) { x > y; }
  {
    return detail::access::make<mask_type>(ops::lt(b.m_data, a.m_data));
  }
  friend constexpr mask_type operator>=(const basic_vec &a,
                                        const basic_vec &b) noexcept
    requires requires(value_type x, value_type y) { x >= y; }
  {
    return detail::access::make<mask_type>(ops::le(b.m_data, a.m_data));
  }
};

template <class R, class... Ts>
  requires std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
           (detail::static_range_size<R>() != detail::dynamic_size)
basic_vec(R &&r, Ts...)
    -> basic_vec<std::ranges::range_value_t<R>,
                 detail::deduce_abi_t<std::ranges::range_value_t<R>,
                                      int(detail::static_range_size<R>())>>;

template <size_t Bytes, class Abi>
basic_vec(basic_mask<Bytes, Abi>)
    -> basic_vec<detail::integer_from<Bytes>, Abi>;

} // namespace nx::simd
