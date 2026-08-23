#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <tuple>
#include <type_traits>
#include <utility>

namespace nx::reflect {

inline constexpr usize max_fields = 64;

template <class T>
concept plain_aggregate =
    std::is_aggregate_v<std::remove_cvref_t<T>> && !std::is_union_v<T>;

namespace detail {

struct any_ref {
  template <class T> constexpr operator T &() const noexcept;
};

template <usize> using slot = any_ref;

/// An array cannot be initialised from a reference, so the compiler elides its braces and `i32
/// slots[4]` reads as four slots - under clang and gcc, at least;
template <class T, class... Probe> consteval usize count_slots() {
  if constexpr (requires { T{Probe{}..., any_ref{}}; })
    return count_slots<T, Probe..., any_ref>();
  else
    return sizeof...(Probe);
}

template <class T, usize... Head, usize... Tail>
consteval bool fits_around(std::index_sequence<Head...>,
                           std::index_sequence<Tail...>) {
  return requires { T{slot<Head>{}..., {}, slot<Tail>{}...}; };
}

template <class T, usize Total, usize Head, usize Tail>
consteval usize slots_at() {
  if constexpr (fits_around<T>(std::make_index_sequence<Head>{},
                               std::make_index_sequence<Tail>{}))
    return Total - Head - Tail;
  else if constexpr (Tail == 0)
    return 1;
  else
    return slots_at<T, Total, Head, Tail - 1>();
}

template <class T, usize Total, usize Head> consteval usize count_fields() {
  if constexpr (Head >= Total)
    return 0;
  else
    return 1 +
           count_fields<T, Total,
                        Head + slots_at<T, Total, Head, Total - Head - 1>()>();
}

template <class T> extern const T fake;

template <class T, usize I, auto P> consteval string_view signature() {
#if defined(_MSC_VER) && !defined(__clang__)
  return __FUNCSIG__;
#else
  return __PRETTY_FUNCTION__;
#endif
}

[[nodiscard]] consteval string_view name_from(const string_view sig) {
#if defined(_MSC_VER) && !defined(__clang__)
  const usize arrow = sig.rfind("->");
  if (arrow == string_view::npos)
    return {};
  const usize first = arrow + 2;
  const usize last = sig.find('>', first);
  return last == string_view::npos ? string_view{}
                                   : sig.substr(first, last - first);
#else
  const usize end = sig.rfind(']');
  if (end == string_view::npos)
    return {};
  const usize dot = sig.rfind('.', end);
  return dot == string_view::npos ? string_view{}
                                  : sig.substr(dot + 1, end - dot - 1);
#endif
}

template <usize N> struct tie_n;

template <> struct tie_n<0> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &, Fn &&fn) {
    return fn();
  }
};
template <> struct tie_n<1> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0] = o;
    return fn(f0);
  }
};
template <> struct tie_n<2> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1] = o;
    return fn(f0, f1);
  }
};
template <> struct tie_n<3> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2] = o;
    return fn(f0, f1, f2);
  }
};
template <> struct tie_n<4> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3] = o;
    return fn(f0, f1, f2, f3);
  }
};
template <> struct tie_n<5> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4] = o;
    return fn(f0, f1, f2, f3, f4);
  }
};
template <> struct tie_n<6> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5] = o;
    return fn(f0, f1, f2, f3, f4, f5);
  }
};
template <> struct tie_n<7> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6);
  }
};
template <> struct tie_n<8> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7);
  }
};
template <> struct tie_n<9> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8);
  }
};
template <> struct tie_n<10> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9);
  }
};
template <> struct tie_n<11> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10);
  }
};
template <> struct tie_n<12> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11);
  }
};
template <> struct tie_n<13> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12);
  }
};
template <> struct tie_n<14> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13);
  }
};
template <> struct tie_n<15> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14);
  }
};
template <> struct tie_n<16> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
           f15] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15);
  }
};
template <> struct tie_n<17> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16);
  }
};
template <> struct tie_n<18> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17);
  }
};
template <> struct tie_n<19> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18);
  }
};
template <> struct tie_n<20> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19);
  }
};
template <> struct tie_n<21> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20);
  }
};
template <> struct tie_n<22> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21);
  }
};
template <> struct tie_n<23> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22);
  }
};
template <> struct tie_n<24> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23);
  }
};
template <> struct tie_n<25> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24);
  }
};
template <> struct tie_n<26> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25);
  }
};
template <> struct tie_n<27> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26);
  }
};
template <> struct tie_n<28> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27);
  }
};
template <> struct tie_n<29> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28);
  }
};
template <> struct tie_n<30> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28,
           f29] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29);
  }
};
template <> struct tie_n<31> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30);
  }
};
template <> struct tie_n<32> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31);
  }
};
template <> struct tie_n<33> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32);
  }
};
template <> struct tie_n<34> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33);
  }
};
template <> struct tie_n<35> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34);
  }
};
template <> struct tie_n<36> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35);
  }
};
template <> struct tie_n<37> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36);
  }
};
template <> struct tie_n<38> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37);
  }
};
template <> struct tie_n<39> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38);
  }
};
template <> struct tie_n<40> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39);
  }
};
template <> struct tie_n<41> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40);
  }
};
template <> struct tie_n<42> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41);
  }
};
template <> struct tie_n<43> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42);
  }
};
template <> struct tie_n<44> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42,
           f43] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43);
  }
};
template <> struct tie_n<45> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44);
  }
};
template <> struct tie_n<46> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45);
  }
};
template <> struct tie_n<47> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46);
  }
};
template <> struct tie_n<48> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47);
  }
};
template <> struct tie_n<49> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48);
  }
};
template <> struct tie_n<50> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49);
  }
};
template <> struct tie_n<51> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50);
  }
};
template <> struct tie_n<52> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51);
  }
};
template <> struct tie_n<53> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52);
  }
};
template <> struct tie_n<54> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53);
  }
};
template <> struct tie_n<55> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54);
  }
};
template <> struct tie_n<56> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55);
  }
};
template <> struct tie_n<57> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56);
  }
};
template <> struct tie_n<58> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56,
           f57] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56, f57);
  }
};
template <> struct tie_n<59> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57,
           f58] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56, f57, f58);
  }
};
template <> struct tie_n<60> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57,
           f58, f59] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56, f57, f58, f59);
  }
};
template <> struct tie_n<61> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57,
           f58, f59, f60] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56, f57, f58, f59, f60);
  }
};
template <> struct tie_n<62> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57,
           f58, f59, f60, f61] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56, f57, f58, f59, f60, f61);
  }
};
template <> struct tie_n<63> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57,
           f58, f59, f60, f61, f62] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56, f57, f58, f59, f60, f61, f62);
  }
};
template <> struct tie_n<64> {
  template <class T, class Fn>
  static constexpr decltype(auto) apply(T &o, Fn &&fn) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15,
           f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
           f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43,
           f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57,
           f58, f59, f60, f61, f62, f63] = o;
    return fn(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14,
              f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
              f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40,
              f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53,
              f54, f55, f56, f57, f58, f59, f60, f61, f62, f63);
  }
};

}

template <plain_aggregate T>
inline constexpr usize field_count =
    detail::count_fields<std::remove_cvref_t<T>,
                         detail::count_slots<std::remove_cvref_t<T>>(), 0>();

template <plain_aggregate T, class Fn>
constexpr decltype(auto) tie(T &object, Fn &&fn) {
  static_assert(field_count<T> <= max_fields,
                "nx::reflect: more fields than the tie arms cover");
  return detail::tie_n<field_count<T>>::apply(object, std::forward<Fn>(fn));
}

namespace detail {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-var-template"
#endif
template <class T, usize I> consteval auto field_pointer() {
  return tie(fake<T>, [](const auto &...fields) {
    return std::get<I>(std::make_tuple(&fields...));
  });
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

}

namespace detail {

[[nodiscard]] consteval string_view type_name_from(const string_view sig) {
#if defined(_MSC_VER) && !defined(__clang__)
  constexpr string_view marker = "type_signature<";
  const usize first = sig.find(marker);
  const usize last = sig.rfind(">(");
  if (first == string_view::npos || last == string_view::npos)
    return {};
  string_view name =
      sig.substr(first + marker.size(), last - first - marker.size());
  for (const string_view prefix : {"struct ", "class ", "enum ", "union "})
    if (name.starts_with(prefix))
      name.remove_prefix(prefix.size());
  return name;
#else
  const usize first = sig.find("[T = ");
  const usize last = sig.rfind(']');
  if (first == string_view::npos || last == string_view::npos)
    return {};
  return sig.substr(first + 5, last - first - 5);
#endif
}

template <class T> consteval string_view type_signature() {
#if defined(_MSC_VER) && !defined(__clang__)
  return __FUNCSIG__;
#else
  return __PRETTY_FUNCTION__;
#endif
}

}

template <class T> [[nodiscard]] consteval string_view type_name() {
  return detail::type_name_from(detail::type_signature<T>());
}

template <plain_aggregate T, usize I>
[[nodiscard]] consteval string_view field_name() {
  static_assert(I < field_count<T>, "nx::reflect: no such field");
  using U = std::remove_cvref_t<T>;
  return detail::name_from(
      detail::signature<U, I, detail::field_pointer<U, I>()>());
}

}
