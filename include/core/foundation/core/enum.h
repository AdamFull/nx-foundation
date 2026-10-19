#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <array>
#include <optional>

namespace nx::enums {
template <typename E>
concept has_count = std::is_enum_v<E> && requires {
  { E::Count };
};

template <typename E>
/// Wraps the enum's underlying integer and offers the usual bit operations (set, clear, toggle,
/// has, has_any, has_all) plus the |, &, ^, ~ operators. @tparam E Scoped enum type whose
/// enumerators are individual bits.
  requires std::is_enum_v<E>
class flags {
public:
  using underlying_type = std::underlying_type_t<E>;

  constexpr flags() noexcept : m_value(0) {}
  constexpr flags(E bit) noexcept : m_value(nx::cast<underlying_type>(bit)) {}
  explicit constexpr flags(underlying_type flags) noexcept : m_value(flags) {}

  constexpr underlying_type value() const noexcept { return m_value; }

  constexpr bool has(E flag) const noexcept {
    return (m_value & nx::cast<underlying_type>(flag)) != 0;
  }

  constexpr bool has_any(flags f) const noexcept {
    return (m_value & f.m_value) != 0;
  }

  constexpr bool has_all(flags f) const noexcept {
    return (m_value & f.m_value) == f.m_value;
  }

  constexpr flags &set(E flag) noexcept {
    m_value |= nx::cast<underlying_type>(flag);
    return *this;
  }

  constexpr flags &clear(E flag) noexcept {
    m_value &= ~nx::cast<underlying_type>(flag);
    return *this;
  }

  constexpr flags &toggle(E flag) noexcept {
    m_value ^= nx::cast<underlying_type>(flag);
    return *this;
  }

  constexpr flags &reset() noexcept {
    m_value = 0;
    return *this;
  }

  constexpr bool any() const noexcept { return m_value != 0; }
  constexpr bool none() const noexcept { return m_value == 0; }

  constexpr flags operator|(E flag) const noexcept {
    return flags(m_value | nx::cast<underlying_type>(flag));
  }

  constexpr flags operator|(flags other) const noexcept {
    return flags(m_value | other.m_value);
  }

  constexpr flags operator&(E flag) const noexcept {
    return flags(m_value & nx::cast<underlying_type>(flag));
  }

  constexpr flags operator&(flags other) const noexcept {
    return flags(m_value & other.m_value);
  }

  constexpr flags operator^(E flag) const noexcept {
    return flags(m_value ^ nx::cast<underlying_type>(flag));
  }

  constexpr flags operator^(flags other) const noexcept {
    return flags(m_value ^ other.m_value);
  }

  constexpr flags operator~() const noexcept { return flags(~m_value); }

  constexpr flags &operator|=(E flag) noexcept {
    m_value |= nx::cast<underlying_type>(flag);
    return *this;
  }

  constexpr flags &operator|=(flags other) noexcept {
    m_value |= other.m_value;
    return *this;
  }

  constexpr flags &operator&=(E flag) noexcept {
    m_value &= nx::cast<underlying_type>(flag);
    return *this;
  }

  constexpr flags &operator&=(flags other) noexcept {
    m_value &= other.m_value;
    return *this;
  }

  constexpr flags &operator^=(E flag) noexcept {
    m_value ^= nx::cast<underlying_type>(flag);
    return *this;
  }

  constexpr flags &operator^=(flags other) noexcept {
    m_value ^= other.m_value;
    return *this;
  }

  constexpr bool operator==(flags other) const noexcept {
    return m_value == other.m_value;
  }

  constexpr bool operator!=(flags other) const noexcept {
    return m_value != other.m_value;
  }

  constexpr bool operator==(E flag) const noexcept {
    return m_value == nx::cast<underlying_type>(flag);
  }

  explicit constexpr operator bool() const noexcept { return m_value != 0; }

private:
  underlying_type m_value;
};

template <typename E, typename T, usize N = nx::cast<usize>(E::Count)>
  requires std::is_enum_v<E>
class map {
public:
  constexpr T &operator[](E e) noexcept { return m_data[nx::cast<usize>(e)]; }
  constexpr const T &operator[](E e) const noexcept {
    return m_data[nx::cast<usize>(e)];
  }

  constexpr usize size() const noexcept { return N; }

  constexpr auto begin() noexcept { return m_data.begin(); }
  constexpr auto end() noexcept { return m_data.end(); }
  constexpr auto begin() const noexcept { return m_data.begin(); }
  constexpr auto end() const noexcept { return m_data.end(); }

private:
  std::array<T, N> m_data{};
};

template <typename E>
concept has_flags = std::is_enum_v<E> && requires(E a, E b) {
  { a | b } -> std::same_as<flags<E>>;
};

/// The enumerators from first to last inclusive. Empty when first is past last
/// or Count is 0; an underlying type's maximum is a valid last.
template <typename E>
  requires std::is_enum_v<E>
struct range {
  using underlying_t = std::underlying_type_t<E>;

  struct iterator {
    using value_type = E;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::bidirectional_iterator_tag;

    underlying_t val{};
    underlying_t last{};
    bool past = true;

    E operator*() const { return nx::cast<E>(val); }

    iterator &operator++() {
      if (val == last)
        past = true;
      else
        ++val;
      return *this;
    }
    iterator operator++(int) {
      auto t = *this;
      ++*this;
      return t;
    }
    iterator &operator--() {
      if (past)
        past = false;
      else
        --val;
      return *this;
    }
    iterator operator--(int) {
      auto t = *this;
      --*this;
      return t;
    }

    bool operator==(const iterator &other) const {
      return past == other.past && (past || val == other.val);
    }
  };

  E first, last;

  constexpr range(E first, E last) : first(first), last(last) {}
  constexpr range(E first)
    requires has_count<E>
      : first(first), last(last_counted(first)),
        m_empty(!(nx::cast<underlying_t>(first) <
                  nx::cast<underlying_t>(E::Count))) {}
  constexpr range()
    requires has_count<E>
      : range(nx::cast<E>(0)) {}

  iterator begin() const {
    if (m_empty || nx::cast<underlying_t>(last) < nx::cast<underlying_t>(first))
      return end();
    return {nx::cast<underlying_t>(first), nx::cast<underlying_t>(last), false};
  }
  iterator end() const {
    return {nx::cast<underlying_t>(last), nx::cast<underlying_t>(last), true};
  }

  std::reverse_iterator<iterator> rbegin() const {
    return std::reverse_iterator<iterator>(end());
  }
  std::reverse_iterator<iterator> rend() const {
    return std::reverse_iterator<iterator>(begin());
  }

private:
  static constexpr E last_counted(const E fallback) {
    const auto count = nx::cast<underlying_t>(E::Count);
    return count == 0 ? fallback : nx::cast<E>(count - 1);
  }

  bool m_empty = false;
};

template <typename E>
  requires std::is_enum_v<E>
struct meta {
  static constexpr std::array<std::pair<E, string_view>, 0> entries{};
};

template <typename E>
concept has_meta = std::is_enum_v<E> && (meta<E>::entries.size() > 0);

}

namespace nx {
template <enums::has_meta E>
constexpr string_view to_string(E value,
                                const string_view fallback = {}) noexcept {
  for (const auto &[key, name] : enums::meta<E>::entries)
    if (key == value)
      return name;
  return fallback;
}

template <enums::has_meta E>
constexpr std::optional<E> from_string(string_view name) noexcept {
  for (const auto &[key, str] : enums::meta<E>::entries)
    if (str == name)
      return key;
  return std::nullopt;
}
}

#define NX_ENUM_FLAGS_DEFINE(E, Name)                                          \
  using Name = ::nx::enums::flags<E>;                                          \
  constexpr ::nx::enums::flags<E> operator|(E lhs, E rhs) noexcept {           \
    return ::nx::enums::flags<E>(lhs) | rhs;                                   \
  }                                                                            \
  constexpr ::nx::enums::flags<E> operator&(E lhs, E rhs) noexcept {           \
    return ::nx::enums::flags<E>(lhs) & rhs;                                   \
  }                                                                            \
  constexpr ::nx::enums::flags<E> operator^(E lhs, E rhs) noexcept {           \
    return ::nx::enums::flags<E>(lhs) ^ rhs;                                   \
  }                                                                            \
  constexpr ::nx::enums::flags<E> operator~(E flag) noexcept {                 \
    return ~::nx::enums::flags<E>(flag);                                       \
  }

#define NX_ENUM_META(Enumerator, Str)                                          \
  ::std::pair<decltype(Enumerator), string_view> { Enumerator, Str }

#define NX_ENUM_REG_META(E, ...)                                               \
  template <> struct nx::enums::meta<E> {                                      \
    static constexpr ::std::array entries = {__VA_ARGS__};                     \
  };