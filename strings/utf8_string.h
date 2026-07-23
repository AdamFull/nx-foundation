#pragma once

/**
 * @file utf8_string.h
 * @brief UTF-8 string with small-string optimization (nx::string).
 */

#include "foundation/core/foundation.h"
#include "foundation/strings/utf8_string_view.h"

#include <cassert>
#include <format>
#include <string>

namespace nx {
namespace utf8 {

constexpr bool is_continuation_byte(const char c) { return (c & 0xC0) == 0x80; }

constexpr usize sequence_length(const char first_byte) {
  if ((first_byte & 0x80) == 0x00)
    return 1; // 0xxxxxxx
  if ((first_byte & 0xE0) == 0xC0)
    return 2; // 110xxxxx
  if ((first_byte & 0xF0) == 0xE0)
    return 3; // 1110xxxx
  if ((first_byte & 0xF8) == 0xF0)
    return 4; // 11110xxx
  return 0;
}

constexpr bool is_valid_codepoint(const char32_t cp) {
  return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

constexpr bool is_surrogate(const char32_t cp) {
  return cp >= 0xD800u && cp <= 0xDFFFu;
}

constexpr bool is_high_surrogate(const char16_t cp) {
  return cp >= 0xD800u && cp <= 0xDBFFu;
}

constexpr bool is_high_surrogate_invalid(const char16_t cp) {
  return cp < 0xD800u || cp > 0xDBFFu;
}

constexpr bool is_low_surrogate(const char16_t cp) {
  return cp >= 0xDC00u && cp <= 0xDFFFu;
}

constexpr bool is_low_surrogate_invalid(const char16_t cp) {
  return cp < 0xDC00u || cp > 0xDFFF;
}

constexpr usize char_byte_count(const char fb) {
  const auto uc = nx::cast<u8>(fb);
  if ((uc & 0x80) == 0)
    return 1;
  if ((uc & 0xE0) == 0xC0)
    return 2;
  if ((uc & 0xF0) == 0xE0)
    return 3;
  if ((uc & 0xF8) == 0xF0)
    return 4;
  return 0;
}

inline bool decode_char(const char *utf8, const usize remaining, char32_t &cp,
                        usize &readed) {
  if (remaining == 0)
    return false;

  const char uc0 = utf8[0];

  switch (char_byte_count(uc0)) {
  case 1: {
    readed = 1;
    cp = nx::cast<char32_t>(uc0);
    return true;
  }
  case 2: {
    const char uc1 = utf8[1];
    if (!is_continuation_byte(uc1))
      return false;
    readed = 2;
    cp = nx::cast<char32_t>(((uc0 & 0x1F) << 6) | (uc1 & 0x3F));
    return true;
  }
  case 3: {
    const char uc1 = utf8[1];
    const char uc2 = utf8[2];
    if (!is_continuation_byte(uc1) || !is_continuation_byte(uc2))
      return false;
    readed = 3;
    cp = nx::cast<char32_t>(((uc0 & 0x0F) << 12) | ((uc1 & 0x3F) << 6) |
                            (uc2 & 0x3F));
    return true;
  }
  case 4: {
    const char uc1 = utf8[1];
    const char uc2 = utf8[2];
    const char uc3 = utf8[3];
    if (!is_continuation_byte(uc1) || !is_continuation_byte(uc2) ||
        !is_continuation_byte(uc3))
      return false;
    readed = 4;
    cp = nx::cast<char32_t>(((uc0 & 0x07) << 18) | ((uc1 & 0x3F) << 12) |
                            ((uc2 & 0x3F) << 6) | (uc3 & 0x3F));
    return true;
  }
  default: {
    readed = 1;
    cp = 0xFFFD;
    return false;
  }
  }
}

inline bool encode_char(const char32_t cp, char *out, usize &len) {
  if (!is_valid_codepoint(cp))
    return false;

  if (cp <= 0x7F) {
    // 1-byte sequence: 0xxxxxxx
    out[0] = nx::cast<char>(cp);
    len = 1;
  } else if (cp <= 0x7FF) {
    // 2-byte sequence: 110xxxxx 10xxxxxx
    out[0] = nx::cast<char>(0xC0 | (cp >> 6));
    out[1] = nx::cast<char>(0x80 | (cp & 0x3F));
    len = 2;
  } else if (cp <= 0xFFFF) {
    // 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
    out[0] = nx::cast<char>(0xE0 | (cp >> 12));
    out[1] = nx::cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = nx::cast<char>(0x80 | (cp & 0x3F));
    len = 3;
  } else if (cp <= 0x10FFFF) {
    // 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    out[0] = nx::cast<char>(0xF0 | (cp >> 18));
    out[1] = nx::cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = nx::cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = nx::cast<char>(0x80 | (cp & 0x3F));
    len = 4;
  } else {
    len = 0;
    return false;
  }

  return true;
}

inline bool encode_char(const char16_t cp, char *out, usize &len) {
  if (is_high_surrogate(cp) || is_low_surrogate(cp))
    return false;

  return encode_char(nx::cast<char32_t>(cp), out, len);
}

inline bool encode_char(const char16_t cp_high, const char16_t cp_low,
                        char *out, usize &len) {
  if (is_high_surrogate_invalid(cp_high) || is_low_surrogate_invalid(cp_low))
    return false;

  const auto cp = nx::cast<char32_t>(0x10000u + ((cp_high - 0xD800u) << 10) +
                                     (cp_low - 0xDC00u));
  return encode_char(cp, out, len);
}

inline bool validate_utf8(const char *data, const usize length) {
  usize pos = 0;
  while (pos < length) {
    usize cp_size = 0;
    if (char32_t cp = 0; !decode_char(data + pos, length - pos, cp, cp_size))
      return false;
    pos += cp_size;
  }
  return true;
}

// Decodes a UTF-8 buffer and calls fn(char32_t) once per code point. Malformed
// bytes yield U+FFFD and advance by one byte.
template <class Fn>
void for_each_codepoint(const char *data, const usize length, Fn &&fn) {
  const char *p = data;
  usize remaining = length;
  while (remaining) {
    const usize need = char_byte_count(*p);
    char32_t cp = 0xFFFD;
    usize r = 1;
    if (need == 0 || need > remaining || !decode_char(p, remaining, cp, r)) {
      cp = 0xFFFD;
      r = 1;
    }
    fn(cp);
    p += r;
    remaining -= r;
  }
}
} // namespace utf8

namespace detail {

// The character types we can transcode from. UTF-8 storage means char and
// char are raw bytes, char16_t (and 2-byte wchar_t) are UTF-16, char32_t
// (and 4-byte wchar_t) are UTF-32; the dispatch keys off sizeof(C).
template <class C>
concept char_like = std::is_same_v<C, char> || std::is_same_v<C, char8_t> ||
                    std::is_same_v<C, char16_t> ||
                    std::is_same_v<C, char32_t> || std::is_same_v<C, wchar_t>;

template <class S>
concept stringish = requires(const S &s) {
  typename std::remove_cvref_t<S>::value_type;
  requires char_like<typename std::remove_cvref_t<S>::value_type>;
  std::basic_string_view<typename std::remove_cvref_t<S>::value_type>(s);
};

} // namespace detail

/**
 * @brief Owning UTF-8 string with small-string optimization.
 *
 * Stores text as UTF-8 bytes and keeps short strings inline (no allocation).
 * Construction and assignment transcode from char, char8_t, char16_t, char32_t
 * and wchar_t sources. Offers a std::string-like interface (append, insert,
 * erase, find, substr, c_str, iterators) and converts implicitly to
 * nx::string_view. Use the nx::string alias for the default allocator.
 *
 * @tparam Alloc Allocator over char8_t.
 */
template <class Alloc = Allocator<char8_t>> class basic_string {
  using ATraits = std::allocator_traits<Alloc>;
  static_assert(std::is_same_v<typename ATraits::value_type, char8_t>,
                "nx::basic_string stores char8_t");

public:
  using value_type = char;
  using size_type = usize;
  using difference_type = isize;
  using reference = char &;
  using const_reference = const char &;
  using pointer = char *;
  using const_pointer = const char *;
  using iterator = char *;
  using const_iterator = const char *;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  static constexpr usize npos = nx::cast<usize>(-1);

  basic_string() noexcept(std::is_nothrow_default_constructible_v<Alloc>)
      : alloc_() {
    init_empty();
  }
  explicit basic_string(const Alloc &a) noexcept : alloc_(a) { init_empty(); }

  template <detail::char_like C>
  basic_string(const C *s, const Alloc &a = Alloc()) : alloc_(a) {
    init_empty();
    append(s);
  }
  template <detail::char_like C>
  basic_string(const C *s, usize n, const Alloc &a = Alloc()) : alloc_(a) {
    init_empty();
    append(s, n);
  }
  template <detail::char_like C>
  basic_string(usize count, C c, const Alloc &a = Alloc()) : alloc_(a) {
    init_empty();
    append(count, c);
  }
  template <detail::char_like C>
  basic_string(C c, const Alloc &a = Alloc()) : alloc_(a) {
    init_empty();
    append_char(c);
  }

  template <detail::stringish S>
  explicit basic_string(const S &s, const Alloc &a = Alloc()) : alloc_(a) {
    init_empty();
    append(s);
  }

  basic_string(const basic_string &o)
      : alloc_(ATraits::select_on_container_copy_construction(o.alloc_)) {
    init_empty();
    append_bytes(o.data(), o.size());
  }
  basic_string(const basic_string &o, const Alloc &a) : alloc_(a) {
    init_empty();
    append_bytes(o.data(), o.size());
  }

  basic_string(basic_string &&o) noexcept : alloc_(std::move(o.alloc_)) {
    steal(o);
  }
  basic_string(basic_string &&o, const Alloc &a) : alloc_(a) {
    if (alloc_ == o.alloc_)
      steal(o);
    else {
      init_empty();
      append_bytes(o.data(), o.size());
    }
  }

  ~basic_string() { free_heap(); }

  basic_string &operator=(const basic_string &o) {
    if (NX_LIKELY(this != &o)) {
      if constexpr (ATraits::propagate_on_container_copy_assignment::value) {
        if (alloc_ != o.alloc_) {
          free_heap();
          init_empty();
        }
        alloc_ = o.alloc_;
      }
      assign(string_view(o.data(), o.size()));
    }
    return *this;
  }

  basic_string &operator=(basic_string &&o) noexcept(
      ATraits::propagate_on_container_move_assignment::value ||
      ATraits::is_always_equal::value) {
    if (NX_LIKELY(this != &o)) {
      if constexpr (ATraits::propagate_on_container_move_assignment::value) {
        free_heap();
        alloc_ = std::move(o.alloc_);
        steal(o);
      } else if (alloc_ == o.alloc_) {
        free_heap();
        steal(o);
      } else
        assign(string_view(o.data(), o.size()));
    }
    return *this;
  }

  template <detail::char_like C> basic_string &operator=(const C *s) {
    return assign(s, std::char_traits<C>::length(s));
  }
  template <detail::stringish S> basic_string &operator=(const S &s) {
    using C = typename std::remove_cvref_t<S>::value_type;
    std::basic_string_view<C> sv(s);
    return assign(sv.data(), sv.size());
  }
  template <detail::char_like C> basic_string &operator=(C c) {
    clear();
    append_char(c);
    return *this;
  }

  [[nodiscard]] Alloc get_allocator() const noexcept { return alloc_; }

  template <detail::char_like C> basic_string &assign(const C *s, usize n) {
    if constexpr (sizeof(C) == 1) {
      const auto *b = reinterpret_cast<const char *>(s);
      if (NX_UNLIKELY(aliases(b))) {
        basic_string tmp(s, n, alloc_);
        return *this = std::move(tmp);
      }
      clear();
      append_bytes(b, n);
    } else {
      clear();
      append_chars(s, n);
    }
    return *this;
  }
  template <detail::char_like C> basic_string &assign(const C *s) {
    return assign(s, std::char_traits<C>::length(s));
  }
  template <detail::stringish S> basic_string &assign(const S &s) {
    using C = std::remove_cvref_t<S>::value_type;
    std::basic_string_view<C> sv(s);
    return assign(sv.data(), sv.size());
  }

  // Element access. Indices are byte offsets, not code points.
  [[nodiscard]] reference operator[](const usize i) noexcept {
    assert(i < size());
    return data()[i];
  }
  [[nodiscard]] const_reference operator[](const usize i) const noexcept {
    assert(i < size());
    return data()[i];
  }
  [[nodiscard]] reference at(const usize i) {
    if (i >= size())
      throw std::out_of_range("nx::basic_string::at");
    return data()[i];
  }
  [[nodiscard]] const_reference at(const usize i) const {
    if (i >= size())
      throw std::out_of_range("nx::basic_string::at");
    return data()[i];
  }
  [[nodiscard]] reference front() noexcept {
    assert(!empty());
    return data()[0];
  }
  [[nodiscard]] const_reference front() const noexcept {
    assert(!empty());
    return data()[0];
  }
  [[nodiscard]] reference back() noexcept {
    assert(!empty());
    return data()[size() - 1];
  }
  [[nodiscard]] const_reference back() const noexcept {
    assert(!empty());
    return data()[size() - 1];
  }

  [[nodiscard]] NX_FORCE_INLINE char *data() noexcept {
    return reinterpret_cast<char *>(is_heap() ? heap_.data : sso_.data);
  }
  [[nodiscard]] NX_FORCE_INLINE const char *data() const noexcept {
    return reinterpret_cast<const char *>(is_heap() ? heap_.data : sso_.data);
  }
  [[nodiscard]] const char *c_str() const noexcept { return data(); }

  [[nodiscard]] iterator begin() noexcept { return data(); }
  [[nodiscard]] iterator end() noexcept { return data() + size(); }
  [[nodiscard]] const_iterator begin() const noexcept { return data(); }
  [[nodiscard]] const_iterator end() const noexcept { return data() + size(); }
  [[nodiscard]] const_iterator cbegin() const noexcept { return data(); }
  [[nodiscard]] const_iterator cend() const noexcept { return data() + size(); }
  [[nodiscard]] reverse_iterator rbegin() noexcept {
    return reverse_iterator{end()};
  }
  [[nodiscard]] reverse_iterator rend() noexcept {
    return reverse_iterator{begin()};
  }
  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator{end()};
  }
  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator{begin()};
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] NX_FORCE_INLINE usize size() const noexcept {
    return is_heap() ? heap_.size : sso_.size;
  }
  [[nodiscard]] usize length() const noexcept { return size(); }
  [[nodiscard]] NX_FORCE_INLINE usize capacity() const noexcept {
    return is_heap() ? heap_capacity() : SSO_CAPACITY;
  }
  [[nodiscard]] usize max_size() const noexcept { return HEAP_MASK - 1; }

  void reserve(const usize new_cap) {
    if (new_cap > capacity())
      reallocate(new_cap);
  }

  void shrink_to_fit() {
    if (!is_heap())
      return;
    const usize sz = heap_.size;
    if (sz <= SSO_CAPACITY) {
      char8_t *old = heap_.data;
      const usize old_alloc = heap_capacity() + 1;
      std::memcpy(sso_.data, old, sz + 1);
      sso_.size = nx::cast<u8>(sz);
      ATraits::deallocate(alloc_, old, old_alloc);
    } else if (heap_capacity() > sz)
      reallocate(sz);
  }

  NX_FORCE_INLINE void clear() noexcept { set_size(0); }

  NX_FORCE_INLINE void push_back(const char c) {
    const usize sz = size();
    ensure_capacity(sz + 1);
    char *d = data();
    d[sz] = c;
    set_size(sz + 1);
  }
  NX_FORCE_INLINE void pop_back() noexcept {
    assert(!empty());
    set_size(size() - 1);
  }

  template <detail::char_like C> basic_string &append(const C *s) {
    return append(s, std::char_traits<C>::length(s));
  }
  template <detail::char_like C> basic_string &append(const C *s, usize n) {
    append_chars(s, n);
    return *this;
  }
  template <detail::char_like C> basic_string &append(const usize count, C c) {
    if constexpr (sizeof(C) == 1)
      fill_append(count, nx::cast<char>(c));
    else {
      for (usize i = 0; i < count; ++i)
        append_char(c);
    }
    return *this;
  }
  template <detail::stringish S> basic_string &append(const S &s) {
    using C = std::remove_cvref_t<S>::value_type;
    std::basic_string_view<C> sv(s);
    append_chars(sv.data(), sv.size());
    return *this;
  }
  basic_string &append(const basic_string &o) {
    append_bytes(o.data(), o.size());
    return *this;
  }

  // Encodes and appends a single code point. An invalid scalar value appends
  // the U+FFFD replacement character.
  basic_string &append_codepoint(const char32_t cp) {
    char buf[4];
    usize len = 0;
    if (!utf8::encode_char(cp, buf, len))
      utf8::encode_char(nx::cast<char32_t>(0xFFFD), buf, len);
    append_bytes(buf, len);
    return *this;
  }

  template <detail::char_like C> basic_string &operator+=(const C *s) {
    return append(s);
  }
  template <detail::stringish S> basic_string &operator+=(const S &s) {
    return append(s);
  }
  basic_string &operator+=(const basic_string &o) { return append(o); }
  template <detail::char_like C> basic_string &operator+=(C c) {
    append_char(c);
    return *this;
  }

  void resize(const usize count) { resize(count, '\0'); }
  void resize(const usize count, const char c) {
    const usize sz = size();
    if (count <= sz)
      set_size(count);
    else {
      ensure_capacity(count);
      std::memset(data() + sz, c, count - sz);
      set_size(count);
    }
  }

  basic_string &insert(const usize pos, string_view sv) {
    assert(pos <= size());
    const usize n = sv.size();
    if (n == 0)
      return *this;
    if (NX_UNLIKELY(aliases(sv.data()))) {
      basic_string tmp(sv, alloc_);
      return insert(pos, string_view(tmp.data(), tmp.size()));
    }
    const usize sz = size();
    ensure_capacity(sz + n);
    char *d = data();
    std::memmove(d + pos + n, d + pos, sz - pos);
    std::memcpy(d + pos, sv.data(), n);
    set_size(sz + n);
    return *this;
  }

  basic_string &erase(const usize pos = 0, const usize count = npos) {
    const usize sz = size();
    assert(pos <= sz);
    const usize n = count < sz - pos ? count : sz - pos;
    char *d = data();
    std::memmove(d + pos, d + pos + n, sz - pos - n);
    set_size(sz - n);
    return *this;
  }

  void swap(basic_string &o) noexcept {
    using std::swap;
    const Raw tmp = raw_;
    raw_ = o.raw_;
    o.raw_ = tmp;
    if constexpr (ATraits::propagate_on_container_swap::value)
      swap(alloc_, o.alloc_);
  }

  [[nodiscard]] string_view view() const noexcept { return {data(), size()}; }
  NX_FORCE_INLINE operator string_view() const noexcept {
    return {data(), size()};
  }

  // Owning conversions. The view operators above share the UTF-8 bytes; these
  // copy, and the u16/u32 forms transcode out of UTF-8.
  [[nodiscard]] std::string string() const { return {data(), size()}; }
  [[nodiscard]] std::u8string u8string() const {
    return {reinterpret_cast<const char8_t *>(data()), size()};
  }
  [[nodiscard]] std::u16string u16string() const {
    std::u16string out;
    out.reserve(size());
    utf8::for_each_codepoint(data(), size(), [&](const char32_t cp) {
      if (cp <= 0xFFFF)
        out.push_back(nx::cast<char16_t>(cp));
      else {
        const char32_t v = cp - 0x10000u;
        out.push_back(nx::cast<char16_t>(0xD800u + (v >> 10)));
        out.push_back(nx::cast<char16_t>(0xDC00u + (v & 0x3FFu)));
      }
    });
    return out;
  }
  [[nodiscard]] std::u32string u32string() const {
    std::u32string out;
    out.reserve(size());
    utf8::for_each_codepoint(data(), size(),
                             [&](const char32_t cp) { out.push_back(cp); });
    return out;
  }

  [[nodiscard]] basic_string substr(const usize pos = 0,
                                    const usize count = npos) const {
    return basic_string(view().substr(pos, count), alloc_);
  }

  [[nodiscard]] int compare(string_view sv) const noexcept {
    return view().compare(sv);
  }
  [[nodiscard]] usize find(string_view sv, const usize pos = 0) const noexcept {
    return view().find(sv, pos);
  }
  [[nodiscard]] usize find(const char c, const usize pos = 0) const noexcept {
    return view().find(c, pos);
  }
  [[nodiscard]] usize rfind(string_view sv,
                            const usize pos = npos) const noexcept {
    return view().rfind(sv, pos);
  }
  [[nodiscard]] bool starts_with(string_view sv) const noexcept {
    return view().starts_with(sv);
  }
  [[nodiscard]] bool ends_with(string_view sv) const noexcept {
    return view().ends_with(sv);
  }
  [[nodiscard]] bool contains(string_view sv) const noexcept {
    return view().find(sv) != npos;
  }

  // Number of decoded code points. Differs from size(), which counts bytes.
  [[nodiscard]] usize code_point_count() const noexcept {
    const char *p = data();
    usize remaining = size();
    usize count = 0;
    while (remaining) {
      const usize need = utf8::char_byte_count(*p);
      usize step;
      if (need == 0 || need > remaining)
        step = 1;
      else {
        char32_t cp = 0;
        usize r = 1;
        utf8::decode_char(p, remaining, cp, r);
        step = r ? r : 1;
      }
      p += step;
      remaining -= step;
      ++count;
    }
    return count;
  }

  // Structurally well-formed UTF-8 (matches the bundled decoder, which does
  // not reject overlong forms or surrogate-range code points).
  [[nodiscard]] bool is_valid_utf8() const noexcept {
    const char *p = data();
    usize remaining = size();
    while (remaining) {
      const usize need = utf8::char_byte_count(*p);
      if (need == 0 || need > remaining)
        return false;
      char32_t cp = 0;
      usize r = 1;
      if (!utf8::decode_char(p, remaining, cp, r))
        return false;
      p += r;
      remaining -= r;
    }
    return true;
  }

  friend bool operator==(const basic_string &a,
                         const basic_string &b) noexcept {
    return a.view() == b.view();
  }
  friend bool operator==(const basic_string &a, string_view b) noexcept {
    return a.view() == b;
  }
  friend bool operator==(const basic_string &a, const char *b) noexcept {
    return a.view() == string_view(b);
  }
  friend std::strong_ordering operator<=>(const basic_string &a,
                                          const basic_string &b) noexcept {
    return a.view() <=> b.view();
  }
  friend std::strong_ordering operator<=>(const basic_string &a,
                                          string_view b) noexcept {
    return a.view() <=> b;
  }

private:
  static_assert(std::endian::native == std::endian::little,
                "nx::basic_string SSO layout assumes a little-endian target");

  struct Heap {
    char8_t *data;
    usize size;
    usize capacity;
  };

  static constexpr usize SSO_BUF = sizeof(Heap) - 1;
  static constexpr usize SSO_CAPACITY = SSO_BUF - 1;
  static constexpr usize HEAP_MASK = nx::cast<usize>(1)
                                     << (sizeof(usize) * 8 - 1);

  struct Sso {
    char8_t data[SSO_BUF];
    u8 size;
  };

  union Raw {
    Heap heap;
    Sso sso;
  };

  union {
    Raw raw_;
    Heap heap_;
    Sso sso_;
  };
  NX_NO_UNIQUE_ADDRESS Alloc alloc_;

  [[nodiscard]] NX_FORCE_INLINE bool is_heap() const noexcept {
    return (sso_.size & 0x80u) != 0;
  }
  [[nodiscard]] NX_FORCE_INLINE usize heap_capacity() const noexcept {
    return heap_.capacity & ~HEAP_MASK;
  }

  NX_FORCE_INLINE void init_empty() noexcept {
    sso_.size = 0;
    sso_.data[0] = u8'\0';
  }

  NX_FORCE_INLINE void set_size(usize n) noexcept {
    if (is_heap())
      heap_.size = n;
    else
      sso_.size = nx::cast<u8>(n);
    data()[n] = '\0';
  }

  NX_FORCE_INLINE bool aliases(const char *s) const noexcept {
    const char *d = data();
    return s >= d && s <= d + size();
  }

  NX_FORCE_INLINE void ensure_capacity(const usize min_cap) {
    if (NX_UNLIKELY(min_cap > capacity()))
      grow(min_cap);
  }

  void grow(const usize min_cap) {
    const usize cur = capacity();
    usize new_cap = cur + cur / 2;
    if (new_cap < min_cap)
      new_cap = min_cap;
    reallocate(new_cap);
  }

  // Allocates a buffer of new_cap + 1 bytes and migrates the contents.
  void reallocate(const usize new_cap) {
    const usize sz = size();
    const char8_t *src = is_heap() ? heap_.data : sso_.data;
    char8_t *nb = ATraits::allocate(alloc_, new_cap + 1);
    std::memcpy(nb, src, sz);
    nb[sz] = u8'\0';

    char8_t *old = nullptr;
    usize old_alloc = 0;
    if (is_heap()) {
      old = heap_.data;
      old_alloc = heap_capacity() + 1;
    }
    heap_.data = nb;
    heap_.size = sz;
    heap_.capacity = new_cap | HEAP_MASK;
    if (old)
      ATraits::deallocate(alloc_, old, old_alloc);
  }

  NX_FORCE_INLINE void free_heap() noexcept {
    if (is_heap())
      ATraits::deallocate(alloc_, heap_.data, heap_capacity() + 1);
  }

  // Takes ownership of o's buffer and leaves o empty. Allocators untouched.
  NX_FORCE_INLINE void steal(basic_string &o) noexcept {
    raw_ = o.raw_;
    o.init_empty();
  }

  void append_bytes(const char *s, const usize n) {
    if (n == 0)
      return;
    const usize sz = size();
    if (NX_UNLIKELY(aliases(s))) {
      const auto off = nx::cast<usize>(s - data());
      ensure_capacity(sz + n);
      char *d = data();
      std::memmove(d + sz, d + off, n);
      set_size(sz + n);
    } else {
      ensure_capacity(sz + n);
      std::memcpy(data() + sz, s, n);
      set_size(sz + n);
    }
  }

  void fill_append(const usize count, const char c) {
    if (count == 0)
      return;
    const usize sz = size();
    ensure_capacity(sz + count);
    std::memset(data() + sz, nx::cast<int>(c), count);
    set_size(sz + count);
  }

  // The single transcoding core. char/char8_t are raw bytes; 2-byte units are
  // UTF-16 (with surrogate pairing), 4-byte units are UTF-32 scalar values.
  template <detail::char_like C> void append_chars(const C *s, usize n) {
    if constexpr (sizeof(C) == 1) {
      append_bytes(reinterpret_cast<const char *>(s), n);
    } else if constexpr (sizeof(C) == 2) {
      char buf[4];
      usize len = 0;
      for (usize i = 0; i < n;) {
        const auto hi = nx::cast<char16_t>(s[i]);
        const bool pair = utf8::is_high_surrogate(hi) && i + 1 < n &&
                          utf8::is_low_surrogate(nx::cast<char16_t>(s[i + 1]));
        if (pair) {
          if (!utf8::encode_char(hi, nx::cast<char16_t>(s[i + 1]), buf, len))
            utf8::encode_char(nx::cast<char32_t>(0xFFFD), buf, len);
          i += 2;
        } else {
          if (!utf8::encode_char(hi, buf, len))
            utf8::encode_char(nx::cast<char32_t>(0xFFFD), buf, len);
          i += 1;
        }
        append_bytes(buf, len);
      }
    } else {
      for (usize i = 0; i < n; ++i)
        append_codepoint(nx::cast<char32_t>(s[i]));
    }
  }

  template <detail::char_like C> void append_char(C c) {
    if constexpr (sizeof(C) == 1)
      push_back(nx::cast<char>(c));
    else
      append_chars(&c, nx::cast<usize>(1));
  }

  template <class> friend class basic_string;
};

/// Default UTF-8 string type; see nx::basic_string.
using string = basic_string<>;

static_assert(sizeof(string) == 3 * sizeof(void *));

template <class A>
basic_string<A> operator+(const basic_string<A> &a, string_view b) {
  basic_string<A> r(a, a.get_allocator());
  r.reserve(a.size() + b.size());
  r.append(b);
  return r;
}
template <class A>
basic_string<A> operator+(string_view a, const basic_string<A> &b) {
  basic_string<A> r(a, b.get_allocator());
  r.reserve(a.size() + b.size());
  r.append(b);
  return r;
}
template <class A>
basic_string<A> operator+(const basic_string<A> &a, const basic_string<A> &b) {
  basic_string<A> r(a, a.get_allocator());
  r.reserve(a.size() + b.size());
  r.append(b);
  return r;
}
template <class A>
basic_string<A> operator+(const basic_string<A> &a, const char *b) {
  return a + string_view(b);
}
template <class A>
basic_string<A> operator+(const char *a, const basic_string<A> &b) {
  return string_view(a) + b;
}
template <class A> basic_string<A> operator+(const basic_string<A> &a, char b) {
  basic_string<A> r(a, a.get_allocator());
  r.push_back(b);
  return r;
}

template <class A>
NX_FORCE_INLINE void swap(basic_string<A> &a, basic_string<A> &b) noexcept {
  a.swap(b);
}

} // namespace nx