#pragma once

// nx::simd::pipeline: element-wise stages composed once and run over ranges
// as one fused loop. An extension; std::simd has no counterpart.

#include "core/foundation/simd/interleave.h"
#include "core/foundation/simd/simd.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace nx::simd {

template <int N> struct lanes_t {
  static_assert(N >= 1 && N <= detail::max_lanes);
  static constexpr int value = N;
};
template <int N> inline constexpr lanes_t<N> lanes{};

template <int N> struct unroll_t {
  static_assert(N >= 1 && N <= 8);
  static constexpr int value = N;
};
template <int N> inline constexpr unroll_t<N> unroll{};

template <class Pred> struct keep_if_t {
  Pred pred;
};
template <class Pred> constexpr keep_if_t<Pred> keep_if(Pred pred) {
  return {std::move(pred)};
}

namespace detail {

template <class Op> struct standard_reducer {
  template <class T> constexpr T identity() const {
    if constexpr (std::is_same_v<Op, std::multiplies<>>)
      return T(1);
    else if constexpr (std::is_same_v<Op, std::bit_and<>>)
      return static_cast<T>(~T(0));
    else
      return T();
  }
  template <class V> constexpr V operator()(const V &a, const V &b) const {
    return Op{}(a, b);
  }
  template <class V> constexpr auto horizontal(const V &v) const {
    return simd::reduce(v, Op{});
  }
};

struct minimum_reducer {
  template <class T> constexpr T identity() const {
    if constexpr (std::numeric_limits<T>::has_infinity)
      return std::numeric_limits<T>::infinity();
    else
      return std::numeric_limits<T>::max();
  }
  template <class V> constexpr V operator()(const V &a, const V &b) const {
    return simd::min(a, b);
  }
  template <class V> constexpr auto horizontal(const V &v) const {
    return simd::reduce_min(v);
  }
};

struct maximum_reducer {
  template <class T> constexpr T identity() const {
    if constexpr (std::numeric_limits<T>::has_infinity)
      return -std::numeric_limits<T>::infinity();
    else
      return std::numeric_limits<T>::lowest();
  }
  template <class V> constexpr V operator()(const V &a, const V &b) const {
    return simd::max(a, b);
  }
  template <class V> constexpr auto horizontal(const V &v) const {
    return simd::reduce_max(v);
  }
};

} // namespace detail

inline constexpr detail::standard_reducer<std::plus<>> sum{};
inline constexpr detail::standard_reducer<std::multiplies<>> product{};
inline constexpr detail::minimum_reducer minimum{};
inline constexpr detail::maximum_reducer maximum{};

template <class Op, class T> struct reducer_t {
  Op op;
  T identity_value;

  template <class U> constexpr U identity() const {
    return static_cast<U>(identity_value);
  }
  template <class V> constexpr V operator()(const V &a, const V &b) const {
    return op(a, b);
  }
  template <class V> constexpr auto horizontal(const V &v) const {
    return simd::reduce(v, op);
  }
};
template <class Op, class T>
constexpr reducer_t<Op, T> reducer(Op op, T identity) {
  return {std::move(op), identity};
}

namespace detail {

template <class R> constexpr auto as_reducer(R r) {
  if constexpr (std::is_same_v<R, std::plus<>> ||
                std::is_same_v<R, std::multiplies<>> ||
                std::is_same_v<R, std::bit_and<>> ||
                std::is_same_v<R, std::bit_or<>> ||
                std::is_same_v<R, std::bit_xor<>>)
    return standard_reducer<R>{};
  else
    return r;
}

} // namespace detail

template <class... Reducers> struct fold_t {
  std::tuple<Reducers...> reducers;
};
template <class... Reducers> constexpr auto fold(Reducers... reducers) {
  static_assert(sizeof...(Reducers) >= 1, "fold takes a reducer per value");
  return fold_t<decltype(detail::as_reducer(reducers))...>{
      {detail::as_reducer(reducers)...}};
}

// interleaved<K>(range) reads or writes a range of K-element tuples, such as
// stereo frames or RGBA pixels, as K values per element.
template <int K, class T> struct interleaved_t {
  static_assert(K >= 1);
  std::span<T> span;
};
template <int K, std::ranges::contiguous_range R>
  requires std::ranges::sized_range<R>
constexpr auto interleaved(R &&range) {
  using T = std::remove_reference_t<std::ranges::range_reference_t<R>>;
  return interleaved_t<K, T>{
      std::span<T>(std::ranges::data(range), std::ranges::size(range))};
}

// The element index as an input: index has int lanes, index_as<T> T lanes.
template <class T> struct index_t {
  static_assert(detail::vectorizable_arithmetic<T>,
                "an index has an arithmetic type");
};
inline constexpr index_t<int> index{};
template <class T> inline constexpr index_t<T> index_as{};

namespace detail {

template <class T> struct is_interleaved : std::false_type {};
template <int K, class T>
struct is_interleaved<interleaved_t<K, T>> : std::true_type {};

template <class T> struct span_target {
  using element = T;
  static constexpr int width = 1;
  T *data;
  size_t size;
};

template <int K, class T> struct interleaved_target {
  using element = T;
  static constexpr int width = K;
  T *data;
  size_t size;
};

template <int K, class T>
constexpr auto as_target(const interleaved_t<K, T> &out) {
  static_assert(!std::is_const_v<T>, "an output is writable");
  static_assert(vectorizable_arithmetic<T>,
                "interleaved ranges hold arithmetic elements");
  return interleaved_target<K, T>{out.span.data(), out.span.size()};
}
template <std::ranges::contiguous_range R>
  requires std::ranges::sized_range<R>
constexpr auto as_target(R &&r) {
  using T = std::remove_reference_t<std::ranges::range_reference_t<R>>;
  static_assert(!std::is_const_v<T>, "an output is writable");
  return span_target<T>{std::ranges::data(r), std::ranges::size(r)};
}

template <class A>
concept target_argument =
    is_interleaved<std::remove_cvref_t<A>>::value ||
    (std::ranges::contiguous_range<A> && std::ranges::sized_range<A>);

} // namespace detail

template <class Flags, class... Targets> struct out_t {
  std::tuple<Targets...> targets;
};
template <class... Flags, detail::target_argument... R>
constexpr auto out(flags<Flags...>, R &&...ranges) {
  return out_t<flags<Flags...>,
               decltype(detail::as_target(std::forward<R>(ranges)))...>{
      {detail::as_target(std::forward<R>(ranges))...}};
}
template <detail::target_argument... R> constexpr auto out(R &&...ranges) {
  return out(flag_default, std::forward<R>(ranges)...);
}

template <class... Stages> class pipeline_t;

namespace detail {

template <class T> struct is_lanes : std::false_type {};
template <int N> struct is_lanes<lanes_t<N>> : std::true_type {};
template <class T> struct is_unroll : std::false_type {};
template <int N> struct is_unroll<unroll_t<N>> : std::true_type {};
template <class T> struct is_keep_if : std::false_type {};
template <class P> struct is_keep_if<keep_if_t<P>> : std::true_type {};
template <class T> struct is_fold : std::false_type {};
template <class... R> struct is_fold<fold_t<R...>> : std::true_type {};
template <class T> struct is_out : std::false_type {};
template <class F, class... S>
struct is_out<out_t<F, S...>> : std::true_type {};
template <class T> struct is_pipeline : std::false_type {};
template <class... S> struct is_pipeline<pipeline_t<S...>> : std::true_type {};

template <class T>
inline constexpr bool is_option = is_lanes<T>::value || is_unroll<T>::value;

template <template <class> class Is, class... S>
consteval int option_value(int fallback) {
  int n = fallback;
  (
      [&] {
        if constexpr (Is<S>::value)
          n = S::value;
      }(),
      ...);
  return n;
}

struct all_lanes {};

template <class Values, class Mask> struct flow {
  Values values;
  Mask mask;
};

template <class M, class K> constexpr M mask_as(const K &k) {
  if constexpr (std::is_same_v<M, K>)
    return k;
  else
    return M(k);
}

template <class M, class K> constexpr auto and_mask(const M &m, const K &k) {
  if constexpr (std::is_same_v<M, all_lanes>)
    return k;
  else
    return m && mask_as<M>(k);
}

template <class R> constexpr auto as_values(R &&r) {
  using T = std::remove_cvref_t<R>;
  if constexpr (simd_vec_type<T>)
    return std::tuple<T>(std::forward<R>(r));
  else
    return std::tuple_cat(std::forward<R>(r));
}

template <class Values> struct values_info;
template <class... V> struct values_info<std::tuple<V...>> {
  static_assert(sizeof...(V) >= 1 && (simd_vec_type<V> && ...),
                "a pipeline stage returns a basic_vec or a std::tuple of them");
  static constexpr int lanes =
      std::tuple_element_t<0, std::tuple<V...>>::size();
  static_assert(((V::size() == lanes) && ...),
                "the values in a pipeline share one lane count");
  static constexpr size_t widest = [] {
    size_t w = 0;
    ((w = sizeof(typename V::value_type) > w ? sizeof(typename V::value_type)
                                             : w),
     ...);
    return w;
  }();
  static constexpr int register_bytes = [] {
    int r = std::numeric_limits<int>::max();
    ((r = std::min(r, int(vec<typename V::value_type>::size() *
                          sizeof(typename V::value_type)))),
     ...);
    return r;
  }();
};

template <size_t I, class Stages, class Values, class Mask>
NX_FORCE_INLINE constexpr auto run_stages(const Stages &stages, Values values,
                                          Mask mask) {
  if constexpr (I == std::tuple_size_v<Stages>) {
    (void)values_info<Values>::lanes;
    return flow<Values, Mask>{std::move(values), std::move(mask)};
  } else {
    using S = std::tuple_element_t<I, Stages>;
    const S &stage = std::get<I>(stages);
    if constexpr (is_option<S> || is_fold<S>::value) {
      return run_stages<I + 1>(stages, std::move(values), std::move(mask));
    } else if constexpr (is_keep_if<S>::value) {
      const auto keep = std::apply(stage.pred, values);
      static_assert(simd_mask_type<std::remove_cvref_t<decltype(keep)>>,
                    "a keep_if predicate returns a basic_mask");
      return run_stages<I + 1>(stages, std::move(values), and_mask(mask, keep));
    } else if constexpr (is_pipeline<S>::value) {
      static_assert(!S::ends_in_fold,
                    "a pipeline used as a stage cannot end in a fold");
      auto inner =
          run_stages<0>(stage.stages(), std::move(values), std::move(mask));
      return run_stages<I + 1>(stages, std::move(inner.values),
                               std::move(inner.mask));
    } else {
      return run_stages<I + 1>(stages, as_values(std::apply(stage, values)),
                               std::move(mask));
    }
  }
}

struct flow_shape {
  size_t widest = 0;
  int register_bytes = std::numeric_limits<int>::max();

  template <class Values> constexpr void add() {
    widest = values_info<Values>::widest > widest ? values_info<Values>::widest
                                                  : widest;
    register_bytes =
        std::min(register_bytes, values_info<Values>::register_bytes);
  }
};

template <size_t I, class Stages, class Values>
consteval void trace_flow(flow_shape &shape) {
  shape.add<Values>();
  if constexpr (I < std::tuple_size_v<Stages>) {
    using S = std::tuple_element_t<I, Stages>;
    if constexpr (is_option<S> || is_fold<S>::value || is_keep_if<S>::value) {
      trace_flow<I + 1, Stages, Values>(shape);
    } else if constexpr (is_pipeline<S>::value) {
      using Inner = typename S::stage_tuple;
      trace_flow<0, Inner, Values>(shape);
      using Next = decltype(run_stages<0>(std::declval<const Inner &>(),
                                          std::declval<Values>(), all_lanes{})
                                .values);
      trace_flow<I + 1, Stages, Next>(shape);
    } else {
      using Next = decltype(as_values(
          std::apply(std::declval<const S &>(), std::declval<Values &>())));
      trace_flow<I + 1, Stages, Next>(shape);
    }
  }
}

// The widest element type in the flow fills one register; narrower ones fill
// part of one.
template <int Forced, class Stages, class Values, class Extra>
consteval int choose_lanes() {
  if constexpr (Forced != 0) {
    return Forced;
  } else {
    flow_shape shape;
    trace_flow<0, Stages, Values>(shape);
    if constexpr (std::tuple_size_v<Extra> != 0)
      shape.add<Extra>();
    return std::max(1, shape.register_bytes / int(shape.widest));
  }
}

template <class V> constexpr V repeat_first(const V &v, int n) {
  return simd::select(typename V::mask_type([n](int i) { return i < n; }), v,
                      V(v[0]));
}

template <class V, size_t K, size_t... k>
NX_FORCE_INLINE constexpr auto tuple_of(const std::array<V, K> &parts,
                                        std::index_sequence<k...>) {
  return std::tuple(parts[k]...);
}

// Each source hands a chunk its values. In the tail, lanes past the end
// repeat the first lane, so a stage only ever sees values from the data: no
// division by a zero that was never there.
template <class T> struct span_source {
  static constexpr bool sized = true;
  const T *data;
  size_t size;

  constexpr size_t count() const { return size; }
  template <int N>
  NX_FORCE_INLINE constexpr std::tuple<vec<T, N>> full(size_t pos) const {
    return {simd::unchecked_load<vec<T, N>>(data + pos, N)};
  }
  template <int N>
  constexpr std::tuple<vec<T, N>> tail(size_t pos, int n) const {
    return {repeat_first(simd::partial_load<vec<T, N>>(data + pos, n), n)};
  }
};

template <int K, class T> struct interleaved_source {
  static constexpr bool sized = true;
  const T *data;
  size_t size;

  constexpr size_t count() const {
    assert(size % K == 0 && "an interleaved range holds whole elements");
    return size / K;
  }
  template <int N> NX_FORCE_INLINE constexpr auto full(size_t pos) const {
    return tuple_of(
        simd::unchecked_load_interleaved<vec<T, N>, K>(data + pos * K, K * N),
        std::make_index_sequence<K>());
  }
  template <int N> constexpr auto tail(size_t pos, int n) const {
    auto parts =
        simd::partial_load_interleaved<vec<T, N>, K>(data + pos * K, K * n);
    for (auto &part : parts)
      part = repeat_first(part, n);
    return tuple_of(parts, std::make_index_sequence<K>());
  }
};

template <class T> struct index_source {
  static constexpr bool sized = false;

  constexpr size_t count() const { return 0; }
  template <int N>
  NX_FORCE_INLINE constexpr std::tuple<vec<T, N>> full(size_t pos) const {
    return {vec<T, N>(static_cast<T>(pos)) + simd::iota<vec<T, N>>};
  }
  template <int N>
  constexpr std::tuple<vec<T, N>> tail(size_t pos, int n) const {
    return {repeat_first(std::get<0>(full<N>(pos)), n)};
  }
};

template <class T> constexpr auto as_source(const index_t<T> &) {
  return index_source<T>{};
}
template <int K, class T>
constexpr auto as_source(const interleaved_t<K, T> &in) {
  using E = std::remove_const_t<T>;
  static_assert(vectorizable_arithmetic<E>,
                "interleaved ranges hold arithmetic elements");
  return interleaved_source<K, E>{in.span.data(), in.span.size()};
}
template <std::ranges::contiguous_range R>
  requires std::ranges::sized_range<R>
constexpr auto as_source(const R &r) {
  using T = std::ranges::range_value_t<R>;
  static_assert(vectorizable<T>,
                "pipeline inputs hold arithmetic or complex elements");
  return span_source<T>{std::ranges::data(r), std::ranges::size(r)};
}

template <int N, class... Src>
using source_values_t = decltype(std::tuple_cat(
    std::declval<const Src &>().template full<N>(0)...));

template <int N, class Stages, class... Src>
NX_FORCE_INLINE constexpr auto full_chunk(const Stages &stages, size_t pos,
                                          const Src &...in) {
  return run_stages<0>(stages, std::tuple_cat(in.template full<N>(pos)...),
                       all_lanes{});
}

template <int N, class Stages, class... Src>
constexpr auto tail_chunk(const Stages &stages, size_t pos, int n,
                          const Src &...in) {
  auto values = std::tuple_cat(in.template tail<N>(pos, n)...);
  using M = typename std::tuple_element_t<0, decltype(values)>::mask_type;
  return run_stages<0>(stages, std::move(values),
                       M([n](int i) { return i < n; }));
}

// MSVC, without type-based alias analysis, reloads anything reached through
// memory after every store, and does not always inline lambdas. So the loop
// keeps the sources, the count and small stages in locals, and its hot path
// has no lambdas.
template <class S>
using local_t =
    std::conditional_t<std::is_trivially_copyable_v<S> && sizeof(S) <= 256,
                       const S, const S &>;

template <int N, class Stages, class Sink, class... Src, size_t... u>
NX_FORCE_INLINE constexpr size_t
run_block(const Stages &stages, Sink &sink, size_t pos, size_t count,
          std::index_sequence<u...>, const Src &...in) {
  ((count =
        sink.template put<u>(full_chunk<N>(stages, pos + u * N, in...), count)),
   ...);
  return count;
}

// Returns how many elements the sink wrote.
template <int N, int U, class Stages, class Sink, class... Src>
constexpr size_t drive(const Stages &shared, Sink &sink, size_t n,
                       const Src... in) {
  local_t<Stages> stages = shared;
  size_t i = 0, count = 0;
  if constexpr (U > 1) {
    for (; i + size_t(U * N) <= n; i += size_t(U * N))
      count = run_block<N>(stages, sink, i, count,
                           std::make_index_sequence<U>(), in...);
  }
  for (; i + size_t(N) <= n; i += size_t(N))
    count = sink.template put<0>(full_chunk<N>(stages, i, in...), count);
  if (i < n)
    count = sink.template put<0>(tail_chunk<N>(stages, i, int(n - i), in...),
                                 count);
  return count;
}

template <class E, class T, class... F>
consteval void check_target(flags<F...>) {
  check_store<E, T, F...>();
}

// Each target takes the next `width` values of the flow.
template <class Flags, class... Targets> struct out_sink {
  std::tuple<Targets...> targets;

  static constexpr size_t value_count = (size_t(Targets::width) + ... + 0);
  template <size_t t> static consteval size_t first_value() {
    constexpr size_t widths[] = {size_t(Targets::width)...};
    size_t first = 0;
    for (size_t i = 0; i < t; ++i)
      first += widths[i];
    return first;
  }

  template <size_t u, class Flow>
  NX_FORCE_INLINE constexpr size_t put(const Flow &f, size_t count) const {
    static_assert(std::tuple_size_v<decltype(f.values)> == value_count,
                  "out takes a range per value the pipeline produces, and an "
                  "interleaved<K> range for K of them");
    return put_values(f, count, std::index_sequence_for<Targets...>());
  }

  template <class Flow, size_t... t>
  NX_FORCE_INLINE constexpr size_t put_values(const Flow &f, size_t count,
                                              std::index_sequence<t...>) const {
    using Values = decltype(f.values);
    constexpr int N = values_info<Values>::lanes;
    if constexpr (std::is_same_v<decltype(f.mask), all_lanes>) {
      (store_all<t, N>(f, count), ...);
      return count + size_t(N);
    } else if constexpr (all_fast_compress<Values>(
                             std::make_index_sequence<value_count>())) {
      return compress_all(f, count, std::index_sequence<t...>());
    } else {
      if (std::is_constant_evaluated())
        return compress_all(f, count, std::index_sequence<t...>());
      // Without a compress instruction, copying the kept lanes one by one
      // beats the generic compress and a partial store.
      const unsigned long long bits = f.mask.to_ullong();
      if (bits == (N == 64 ? ~0ull : (1ull << N) - 1)) {
        (store_all<t, N>(f, count), ...);
        return count + size_t(N);
      }
      size_t kept = 0;
      ((kept = copy_kept<t>(f, count, bits)), ...);
      return count + kept;
    }
  }

  template <class Values, size_t... k>
  static consteval bool all_fast_compress(std::index_sequence<k...>) {
    return (vops<std::tuple_element_t<k, Values>>::fast_compress && ...);
  }

  // The flow's values as the target's element type, under its flags.
  template <class T, class V> static constexpr auto as_element(const V &v) {
    using E = typename V::value_type;
    check_target<E, T>(Flags{});
    if constexpr (std::is_same_v<E, T>)
      return v;
    else
      return rebind_t<T, V>(v);
  }

  template <size_t t, class Flow>
  static constexpr auto parts_of(const Flow &f) {
    using Target = std::tuple_element_t<t, std::tuple<Targets...>>;
    using T = typename Target::element;
    return [&]<size_t... k>(std::index_sequence<k...>) {
      return std::array{
          as_element<T>(std::get<first_value<t>() + k>(f.values))...};
    }(std::make_index_sequence<Target::width>());
  }

  template <size_t t, int N, class Flow>
  NX_FORCE_INLINE constexpr void store_all(const Flow &f, size_t count) const {
    const auto &target = std::get<t>(targets);
    using Target = std::remove_cvref_t<decltype(target)>;
    if constexpr (Target::width == 1)
      simd::unchecked_store(std::get<first_value<t>()>(f.values),
                            target.data + count, N, Flags{});
    else
      simd::unchecked_store_interleaved(parts_of<t>(f),
                                        target.data + count * Target::width,
                                        Target::width * N);
  }

  template <class Flow, size_t... t>
  constexpr size_t compress_all(const Flow &f, size_t count,
                                std::index_sequence<t...>) const {
    const int kept = simd::reduce_count(f.mask);
    (compress_one<t>(f, count, kept), ...);
    return count + size_t(kept);
  }

  template <size_t t, class Flow>
  constexpr void compress_one(const Flow &f, size_t count, int kept) const {
    const auto &target = std::get<t>(targets);
    using Target = std::remove_cvref_t<decltype(target)>;
    auto parts = parts_of<t>(f);
    for (auto &part : parts)
      part = simd::compress(
          part,
          mask_as<typename std::remove_cvref_t<decltype(part)>::mask_type>(
              f.mask));
    if constexpr (Target::width == 1)
      simd::partial_store(parts[0], target.data + count, kept);
    else
      simd::partial_store_interleaved(
          parts, target.data + count * Target::width, Target::width * kept);
  }

  template <size_t t, class Flow>
  NX_FORCE_INLINE constexpr size_t copy_kept(const Flow &f, size_t count,
                                             unsigned long long bits) const {
    const auto &target = std::get<t>(targets);
    using Target = std::remove_cvref_t<decltype(target)>;
    using T = typename Target::element;
    constexpr int K = Target::width;
    const auto parts = parts_of<t>(f);
    constexpr int N = std::remove_cvref_t<decltype(parts[0])>::size();
    T lanes[K][N];
    for (int k = 0; k < K; ++k)
      simd::unchecked_store(parts[size_t(k)], lanes[k], N);
    T *dst = target.data + count * K;
    size_t n = 0;
    for (; bits != 0; bits &= bits - 1, ++n) {
      const int lane = std::countr_zero(bits);
      for (int k = 0; k < K; ++k)
        dst[n * K + size_t(k)] = lanes[k][lane];
    }
    return n;
  }
};

template <int U, class Reducers, class Values> struct fold_sink;
template <int U, class... R, class... V>
struct fold_sink<U, std::tuple<R...>, std::tuple<V...>> {
  static_assert(sizeof...(R) == sizeof...(V),
                "fold takes one reducer per value the pipeline produces");

  std::tuple<R...> reducers;
  std::tuple<std::array<V, U>...> acc;

  constexpr explicit fold_sink(const std::tuple<R...> &r)
      : reducers(r), acc(initial(r, std::index_sequence_for<V...>())) {}

  template <size_t... k>
  static constexpr std::tuple<std::array<V, U>...>
  initial(const std::tuple<R...> &r, std::index_sequence<k...>) {
    return {filled<V>(std::get<k>(r))...};
  }
  template <class W, class Reducer>
  static constexpr std::array<W, U> filled(const Reducer &r) {
    std::array<W, U> a{};
    a.fill(W(r.template identity<typename W::value_type>()));
    return a;
  }

  template <size_t u, class Flow>
  NX_FORCE_INLINE constexpr size_t put(const Flow &f, size_t count) {
    put_values<u>(f, std::index_sequence_for<V...>());
    return count;
  }

  template <size_t u, class Flow, size_t... k>
  NX_FORCE_INLINE constexpr void put_values(const Flow &f,
                                            std::index_sequence<k...>) {
    (step<k, u>(std::get<k>(f.values), f.mask), ...);
  }

  template <size_t k, size_t u, class W, class M>
  NX_FORCE_INLINE constexpr void step(const W &v, const M &mask) {
    const auto &r = std::get<k>(reducers);
    W &a = std::get<k>(acc)[u];
    if constexpr (std::is_same_v<M, all_lanes>)
      a = r(a, v);
    else
      a = r(a, simd::select(mask_as<typename W::mask_type>(mask), v,
                            W(r.template identity<typename W::value_type>())));
  }

  template <size_t k> constexpr auto total() const {
    const auto &r = std::get<k>(reducers);
    const auto &a = std::get<k>(acc);
    auto t = a[0];
    for (int u = 1; u < U; ++u)
      t = r(t, a[u]);
    return r.horizontal(t);
  }

  template <size_t... k>
  constexpr auto totals(std::index_sequence<k...>) const {
    if constexpr (sizeof...(k) == 1)
      return total<0>();
    else
      return std::tuple(total<k>()...);
  }

  constexpr auto result() const {
    return totals(std::index_sequence_for<V...>());
  }
};

} // namespace detail

// Stages run in order on each chunk of lanes. A callable stage takes the
// previous values (the inputs, first) and returns a basic_vec or a std::tuple
// of them; keep_if drops lanes; a fold ends the pipeline. lanes<N> and
// unroll<N> override the lane count and the chunks in flight.
template <class... Stages> class pipeline_t {
public:
  using stage_tuple = std::tuple<Stages...>;

  static constexpr bool ends_in_fold = [] {
    bool last = false;
    ((last = detail::is_option<Stages> ? last : detail::is_fold<Stages>::value),
     ...);
    return last;
  }();
  static_assert((int(detail::is_fold<Stages>::value) + ... + 0) <=
                    int(ends_in_fold),
                "fold is the last stage of a pipeline");

  constexpr explicit pipeline_t(Stages... stages)
      : m_stages(std::move(stages)...) {}

  constexpr const stage_tuple &stages() const noexcept { return m_stages; }

  // Runs over the inputs. A pipeline that ends in a fold returns the folded
  // value, or a std::tuple of them; any other takes out(ranges...) last and
  // returns how many elements it wrote. Inputs other than the index have
  // equal sizes; an output holds at least that many and is either one of the
  // inputs or disjoint from all of them.
  template <class... Args> constexpr auto operator()(Args &&...args) const {
    static_assert(sizeof...(Args) >= 1 + int(!ends_in_fold),
                  "a pipeline runs over at least one input");
    if constexpr (ends_in_fold) {
      return run_fold(detail::as_source(args)...);
    } else {
      static_assert(sizeof...(Args) >= 2 &&
                        detail::is_out<std::remove_cvref_t<std::tuple_element_t<
                            sizeof...(Args) - 1, std::tuple<Args...>>>>::value,
                    "a pipeline without a fold takes out(ranges...) last");
      const auto refs = std::forward_as_tuple(args...);
      return [&]<size_t... I>(std::index_sequence<I...>) {
        return run_out(std::get<sizeof...(Args) - 1>(refs),
                       detail::as_source(std::get<I>(refs))...);
      }(std::make_index_sequence<sizeof...(Args) - 1>());
    }
  }

private:
  template <class... Src>
  static constexpr size_t checked_size([[maybe_unused]] const Src &...in) {
    static_assert((Src::sized || ...),
                  "a pipeline needs an input with a size besides an index");
    size_t n = 0;
    [[maybe_unused]] bool first = true;
    (
        [&] {
          if constexpr (Src::sized) {
            assert((first || in.count() == n) &&
                   "pipeline inputs have equal sizes");
            n = in.count();
            first = false;
          }
        }(),
        ...);
    return n;
  }
  template <class Target>
  static constexpr void check_output(const Target &target, size_t n) {
    assert(target.size % size_t(Target::width) == 0 &&
           "an interleaved range holds whole elements");
    assert(target.size / size_t(Target::width) >= n &&
           "an output holds every element");
    (void)target;
    (void)n;
  }

  template <class Flags, class... Targets, class... Src>
  constexpr size_t run_out(const out_t<Flags, Targets...> &o,
                           const Src &...in) const {
    constexpr int N = detail::choose_lanes<
        detail::option_value<detail::is_lanes, Stages...>(0), stage_tuple,
        detail::source_values_t<1, Src...>,
        std::tuple<vec<typename Targets::element, 1>...>>();
    constexpr int unroll =
        detail::option_value<detail::is_unroll, Stages...>(2);
    const size_t n = checked_size(in...);
    std::apply([n](const auto &...t) { (check_output(t, n), ...); }, o.targets);
    const detail::out_sink<Flags, Targets...> sink{o.targets};
    return detail::drive<N, unroll>(m_stages, sink, n, in...);
  }

  template <class... Src> constexpr auto run_fold(const Src &...in) const {
    constexpr int N = detail::choose_lanes<
        detail::option_value<detail::is_lanes, Stages...>(0), stage_tuple,
        detail::source_values_t<1, Src...>, std::tuple<>>();
    using Values = decltype(detail::full_chunk<N>(m_stages, 0, in...).values);
    // Enough accumulators to hide the add latency, without running out of
    // registers beside the other folded values.
    constexpr int values = int(std::tuple_size_v<Values>);
    constexpr int unroll = detail::option_value<detail::is_unroll, Stages...>(
        values >= 3 ? 2 : 8 / values);
    const size_t n = checked_size(in...);
    const auto &reducers = std::get<fold_index()>(m_stages).reducers;
    using Sink =
        detail::fold_sink<unroll, std::remove_cvref_t<decltype(reducers)>,
                          Values>;
    Sink sink(reducers);
    detail::drive<N, unroll>(m_stages, sink, n, in...);
    return sink.result();
  }

  static consteval size_t fold_index() {
    size_t i = 0, found = 0;
    ((found = detail::is_fold<Stages>::value ? i : found, ++i), ...);
    return found;
  }

  stage_tuple m_stages;
};

// A function, not a class: `pipeline(f)(in, out(o));` as a statement would
// otherwise declare a variable f.
template <class... Stages>
constexpr pipeline_t<Stages...> pipeline(Stages... stages) {
  return pipeline_t<Stages...>(std::move(stages)...);
}

} // namespace nx::simd
