#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/hash.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <cassert>
#include <cstring>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>

namespace nx {
namespace utf8 {

constexpr bool is_continuation_byte(const char c) { return (c & 0xC0) == 0x80; }

constexpr usize sequence_length(const char first_byte) {
  if ((first_byte & 0x80) == 0x00)
    return 1;
  if ((first_byte & 0xE0) == 0xC0)
    return 2;
  if ((first_byte & 0xF0) == 0xE0)
    return 3;
  if ((first_byte & 0xF8) == 0xF0)
    return 4;
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

/// Decodes one well-formed sequence (Unicode table 3-7): no overlong forms, no
/// surrogates, nothing past U+10FFFF. On failure @p cp is U+FFFD and @p readed
/// is 1, so a caller that skips the bad byte resynchronises.
inline bool decode_char(const char *utf8, const usize remaining, char32_t &cp,
                        usize &readed) {
  if (remaining == 0)
    return false;

  const auto uc0 = nx::cast<u8>(utf8[0]);
  const usize n = char_byte_count(utf8[0]);
  const auto fail = [&] {
    cp = 0xFFFD;
    readed = 1;
    return false;
  };
  if (n == 0 || n > remaining)
    return fail();
  if (n == 1) {
    cp = uc0;
    readed = 1;
    return true;
  }

  constexpr u8 lead_bits[] = {0, 0, 0x1F, 0x0F, 0x07};
  constexpr char32_t shortest[] = {0, 0, 0x80, 0x800, 0x10000};
  auto value = nx::cast<char32_t>(uc0 & lead_bits[n]);
  for (usize i = 1; i < n; ++i) {
    if (!is_continuation_byte(utf8[i]))
      return fail();
    value = nx::cast<char32_t>((value << 6) | (nx::cast<u8>(utf8[i]) & 0x3Fu));
  }
  if (value < shortest[n] || !is_valid_codepoint(value))
    return fail();
  cp = value;
  readed = n;
  return true;
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

// Byte offset of the character after the one @p at is in. A caret steps by
// these: one byte at a time would land inside a character and cut it in half.
[[nodiscard]] constexpr usize next_char(const string_view text,
                                        usize at) noexcept {
  if (at >= text.size())
    return text.size();
  ++at;
  while (at < text.size() && is_continuation_byte(text[at]))
    ++at;
  return at;
}

[[nodiscard]] constexpr usize prev_char(const string_view text,
                                        usize at) noexcept {
  if (at > text.size())
    at = text.size();
  if (at == 0)
    return 0;
  --at;
  while (at > 0 && is_continuation_byte(text[at]))
    --at;
  return at;
}

/// The length of @p text without a sequence cut short at its end, as text
/// truncated at a byte limit leaves one.
[[nodiscard]] constexpr usize complete_prefix(const string_view text) noexcept {
  const usize last = prev_char(text, text.size());
  if (last == text.size())
    return last;
  const usize need = sequence_length(text[last]);
  return need != 0 && text.size() - last < need ? last : text.size();
}

// Places a caret can stand, which is what next_char steps through. basic_string::code_point_count
// answers the decoder's question instead, and the two differ on a stray continuation byte:
[[nodiscard]] constexpr usize count_chars(const string_view text) noexcept {
  usize n = 0;
  for (usize at = 0; at < text.size(); at = next_char(text, at))
    ++n;
  return n;
}

// The code point starting at @p at, or 0 for bytes that do not decode. No
// caller can confuse that with a real answer: U+0000 does not appear in text.
[[nodiscard]] inline char32_t char_at(const string_view text,
                                      const usize at) noexcept {
  char32_t cp = 0;
  usize read = 0;
  if (at >= text.size() ||
      !decode_char(text.data() + at, text.size() - at, cp, read))
    return 0;
  return cp;
}
}

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

}

/// Owning UTF-8 string with small-string optimization. Stores text as UTF-8 bytes and keeps short
/// strings inline (no allocation).
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
      : m_alloc() {
    init_empty();
  }
  explicit basic_string(const Alloc &a) noexcept : m_alloc(a) { init_empty(); }

  template <detail::char_like C>
  basic_string(const C *s, const Alloc &a = Alloc()) : m_alloc(a) {
    init_empty();
    append(s);
  }
  template <detail::char_like C>
  basic_string(const C *s, usize n, const Alloc &a = Alloc()) : m_alloc(a) {
    init_empty();
    append(s, n);
  }
  template <detail::char_like C>
  basic_string(usize count, C c, const Alloc &a = Alloc()) : m_alloc(a) {
    init_empty();
    append(count, c);
  }
  template <detail::char_like C>
  explicit basic_string(C c, const Alloc &a = Alloc()) : m_alloc(a) {
    init_empty();
    append_char(c);
  }

  template <detail::stringish S>
  explicit basic_string(const S &s, const Alloc &a = Alloc()) : m_alloc(a) {
    init_empty();
    append(s);
  }

  basic_string(const basic_string &o)
      : m_alloc(ATraits::select_on_container_copy_construction(o.m_alloc)) {
    init_empty();
    append_bytes(o.data(), o.size());
  }
  basic_string(const basic_string &o, const Alloc &a) : m_alloc(a) {
    init_empty();
    append_bytes(o.data(), o.size());
  }

  basic_string(basic_string &&o) noexcept : m_alloc(std::move(o.m_alloc)) {
    steal(o);
  }
  basic_string(basic_string &&o, const Alloc &a) : m_alloc(a) {
    if (m_alloc == o.m_alloc)
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
        if (m_alloc != o.m_alloc) {
          free_heap();
          init_empty();
        }
        m_alloc = o.m_alloc;
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
        m_alloc = std::move(o.m_alloc);
        steal(o);
      } else if (m_alloc == o.m_alloc) {
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

  [[nodiscard]] Alloc get_allocator() const noexcept { return m_alloc; }

  template <detail::char_like C> basic_string &assign(const C *s, usize n) {
    if constexpr (sizeof(C) == 1) {
      const auto *b = reinterpret_cast<const char *>(s);
      if (NX_UNLIKELY(aliases(b))) {
        basic_string tmp(s, n, m_alloc);
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
    return reinterpret_cast<char *>(is_heap() ? m_heap.data : m_sso.data);
  }
  [[nodiscard]] NX_FORCE_INLINE const char *data() const noexcept {
    return reinterpret_cast<const char *>(is_heap() ? m_heap.data : m_sso.data);
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
    return is_heap() ? m_heap.size : m_sso.size;
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
    const usize sz = m_heap.size;
    if (sz <= SSO_CAPACITY) {
      char8_t *old = m_heap.data;
      const usize old_alloc = heap_capacity() + 1;
      std::memcpy(m_sso.data, old, sz + 1);
      m_sso.size = nx::cast<u8>(sz);
      ATraits::deallocate(m_alloc, old, old_alloc);
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
    if (pos > size())
      throw std::out_of_range("nx::basic_string::insert");
    const usize n = sv.size();
    if (n == 0)
      return *this;
    if (NX_UNLIKELY(aliases(sv.data()))) {
      basic_string tmp(sv, m_alloc);
      return insert(pos, string_view(tmp.data(), tmp.size()));
    }
    const usize sz = size();
    ensure_capacity(grown(sz, n));
    char *d = data();
    std::memmove(d + pos + n, d + pos, sz - pos);
    std::memcpy(d + pos, sv.data(), n);
    set_size(sz + n);
    return *this;
  }

  basic_string &insert(const usize pos, const usize count, const char c) {
    if (pos > size())
      throw std::out_of_range("nx::basic_string::insert");
    if (count == 0)
      return *this;
    const usize sz = size();
    ensure_capacity(grown(sz, count));
    char *d = data();
    std::memmove(d + pos + count, d + pos, sz - pos);
    std::memset(d + pos, nx::cast<int>(c), count);
    set_size(sz + count);
    return *this;
  }

  /// Replaces the bytes [pos, pos + count), clamped to the end, with @p sv.
  basic_string &replace(const usize pos, const usize count, string_view sv) {
    const usize sz = size();
    if (pos > sz)
      throw std::out_of_range("nx::basic_string::replace");
    if (NX_UNLIKELY(aliases(sv.data()))) {
      basic_string tmp(sv, m_alloc);
      return replace(pos, count, string_view(tmp.data(), tmp.size()));
    }
    const usize cut = count < sz - pos ? count : sz - pos;
    const usize n = sv.size();
    if (n > cut)
      ensure_capacity(grown(sz - cut, n));
    char *d = data();
    std::memmove(d + pos + n, d + pos + cut, sz - pos - cut);
    if (n)
      std::memcpy(d + pos, sv.data(), n);
    set_size(sz - cut + n);
    return *this;
  }
  basic_string &replace(const usize pos, const usize count, const usize n,
                        const char c) {
    const usize sz = size();
    if (pos > sz)
      throw std::out_of_range("nx::basic_string::replace");
    const usize cut = count < sz - pos ? count : sz - pos;
    if (n > cut)
      ensure_capacity(grown(sz - cut, n));
    char *d = data();
    std::memmove(d + pos + n, d + pos + cut, sz - pos - cut);
    std::memset(d + pos, nx::cast<int>(c), n);
    set_size(sz - cut + n);
    return *this;
  }

  /// Gives @p op room for @p count bytes, as std::string does: op(data,
  /// count) writes them and returns the size to keep, at most count. Bytes
  /// past the old size are uninitialized until op writes them.
  template <typename Op> void resize_and_overwrite(const usize count, Op op) {
    ensure_capacity(count);
    const usize kept = nx::cast<usize>(std::move(op)(data(), count));
    assert(kept <= count);
    set_size(kept);
  }

  basic_string &erase(const usize pos = 0, const usize count = npos) {
    const usize sz = size();
    if (pos > sz)
      throw std::out_of_range("nx::basic_string::erase");
    const usize n = count < sz - pos ? count : sz - pos;
    char *d = data();
    std::memmove(d + pos, d + pos + n, sz - pos - n);
    set_size(sz - n);
    return *this;
  }

  void swap(basic_string &o) noexcept {
    using std::swap;
    const Raw tmp = m_raw;
    m_raw = o.m_raw;
    o.m_raw = tmp;
    if constexpr (ATraits::propagate_on_container_swap::value)
      swap(m_alloc, o.m_alloc);
  }

  [[nodiscard]] string_view view() const noexcept { return {data(), size()}; }
  NX_FORCE_INLINE operator string_view() const noexcept {
    return {data(), size()};
  }

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
    return basic_string(view().substr(pos, count), m_alloc);
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
  [[nodiscard]] usize rfind(const char c,
                            const usize pos = npos) const noexcept {
    return view().rfind(c, pos);
  }
  [[nodiscard]] usize find_first_of(string_view chars,
                                    const usize pos = 0) const noexcept {
    return view().find_first_of(chars, pos);
  }
  [[nodiscard]] usize find_first_of(const char c,
                                    const usize pos = 0) const noexcept {
    return view().find_first_of(c, pos);
  }
  [[nodiscard]] usize find_last_of(string_view chars,
                                   const usize pos = npos) const noexcept {
    return view().find_last_of(chars, pos);
  }
  [[nodiscard]] usize find_last_of(const char c,
                                   const usize pos = npos) const noexcept {
    return view().find_last_of(c, pos);
  }
  [[nodiscard]] usize find_first_not_of(string_view chars,
                                        const usize pos = 0) const noexcept {
    return view().find_first_not_of(chars, pos);
  }
  [[nodiscard]] usize find_first_not_of(const char c,
                                        const usize pos = 0) const noexcept {
    return view().find_first_not_of(c, pos);
  }
  [[nodiscard]] usize find_last_not_of(string_view chars,
                                       const usize pos = npos) const noexcept {
    return view().find_last_not_of(chars, pos);
  }
  [[nodiscard]] usize find_last_not_of(const char c,
                                       const usize pos = npos) const noexcept {
    return view().find_last_not_of(c, pos);
  }
  [[nodiscard]] bool starts_with(string_view sv) const noexcept {
    return view().starts_with(sv);
  }
  [[nodiscard]] bool starts_with(const char c) const noexcept {
    return view().starts_with(c);
  }
  [[nodiscard]] bool ends_with(string_view sv) const noexcept {
    return view().ends_with(sv);
  }
  [[nodiscard]] bool ends_with(const char c) const noexcept {
    return view().ends_with(c);
  }
  [[nodiscard]] bool contains(string_view sv) const noexcept {
    return view().find(sv) != npos;
  }
  [[nodiscard]] bool contains(const char c) const noexcept {
    return view().find(c) != npos;
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

  [[nodiscard]] bool is_valid_utf8() const noexcept {
    return utf8::validate_utf8(data(), size());
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
    Raw m_raw;
    Heap m_heap;
    Sso m_sso;
  };
  NX_NO_UNIQUE_ADDRESS Alloc m_alloc;

  [[nodiscard]] NX_FORCE_INLINE bool is_heap() const noexcept {
    return (m_sso.size & 0x80u) != 0;
  }
  [[nodiscard]] NX_FORCE_INLINE usize heap_capacity() const noexcept {
    return m_heap.capacity & ~HEAP_MASK;
  }

  NX_FORCE_INLINE void init_empty() noexcept {
    m_sso.size = 0;
    m_sso.data[0] = u8'\0';
  }

  NX_FORCE_INLINE void set_size(usize n) noexcept {
    if (is_heap())
      m_heap.size = n;
    else
      m_sso.size = nx::cast<u8>(n);
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
    usize new_cap = nx::min(cur + cur / 2, max_size());
    if (new_cap < min_cap)
      new_cap = min_cap;
    reallocate(new_cap);
  }

  [[nodiscard]] usize grown(const usize sz, const usize n) const {
    if (n > max_size() - sz)
      throw std::length_error("nx::basic_string: longer than max_size");
    return sz + n;
  }

  void reallocate(const usize new_cap) {
    if (new_cap > max_size())
      throw std::length_error("nx::basic_string: longer than max_size");
    const usize sz = size();
    const char8_t *src = is_heap() ? m_heap.data : m_sso.data;
    char8_t *nb = ATraits::allocate(m_alloc, new_cap + 1);
    std::memcpy(nb, src, sz);
    nb[sz] = u8'\0';

    char8_t *old = nullptr;
    usize old_alloc = 0;
    if (is_heap()) {
      old = m_heap.data;
      old_alloc = heap_capacity() + 1;
    }
    m_heap.data = nb;
    m_heap.size = sz;
    m_heap.capacity = new_cap | HEAP_MASK;
    if (old)
      ATraits::deallocate(m_alloc, old, old_alloc);
  }

  NX_FORCE_INLINE void free_heap() noexcept {
    if (is_heap())
      ATraits::deallocate(m_alloc, m_heap.data, heap_capacity() + 1);
  }

  NX_FORCE_INLINE void steal(basic_string &o) noexcept {
    m_raw = o.m_raw;
    o.init_empty();
  }

  void append_bytes(const char *s, const usize n) {
    if (n == 0)
      return;
    const usize sz = size();
    if (NX_UNLIKELY(aliases(s))) {
      const auto off = nx::cast<usize>(s - data());
      ensure_capacity(grown(sz, n));
      char *d = data();
      std::memmove(d + sz, d + off, n);
      set_size(sz + n);
    } else {
      ensure_capacity(grown(sz, n));
      std::memcpy(data() + sz, s, n);
      set_size(sz + n);
    }
  }

  void fill_append(const usize count, const char c) {
    if (count == 0)
      return;
    const usize sz = size();
    ensure_capacity(grown(sz, count));
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

using string = basic_string<>;

static_assert(sizeof(string) == 3 * sizeof(void *));

namespace utf8 {

/// Appends the UTF-8 encoding of @p cp. False, and @p out untouched, when the
/// code point cannot be encoded.
template <class A> bool append_char(basic_string<A> &out, const char32_t cp) {
  char buf[4];
  usize len = 0;
  if (!encode_char(cp, buf, len))
    return false;
  out.append(buf, len);
  return true;
}

}

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

// A temporary on either side lends its buffer to the result, so a chain such
// as a + b + c allocates once rather than at every +.
template <class A>
basic_string<A> operator+(basic_string<A> &&a, const string_view b) {
  a.append(b);
  return std::move(a);
}
template <class A>
basic_string<A> operator+(basic_string<A> &&a, const basic_string<A> &b) {
  a.append(b);
  return std::move(a);
}
template <class A>
basic_string<A> operator+(basic_string<A> &&a, basic_string<A> &&b) {
  a.append(b);
  return std::move(a);
}
template <class A>
basic_string<A> operator+(basic_string<A> &&a, const char *b) {
  a.append(b);
  return std::move(a);
}
template <class A> basic_string<A> operator+(basic_string<A> &&a, char b) {
  a.push_back(b);
  return std::move(a);
}
template <class A>
basic_string<A> operator+(const string_view a, basic_string<A> &&b) {
  b.insert(0, a);
  return std::move(b);
}
template <class A>
basic_string<A> operator+(const basic_string<A> &a, basic_string<A> &&b) {
  b.insert(0, a.view());
  return std::move(b);
}
template <class A>
basic_string<A> operator+(const char *a, basic_string<A> &&b) {
  b.insert(0, string_view(a));
  return std::move(b);
}

template <class A>
NX_FORCE_INLINE void swap(basic_string<A> &a, basic_string<A> &b) noexcept {
  a.swap(b);
}

template <class... Args>
[[nodiscard]] string format(std::format_string<Args...> fmt, Args &&...args) {
  string out;
  std::vformat_to(std::back_inserter(out), fmt.get(),
                  std::make_format_args(args...));
  return out;
}

} // namespace nx

/// Hashes the raw UTF-8 bytes with the same function nx::id_string uses, so a
/// string and its interned id agree on what identity means. Transparent: a
/// view hashes as the string it names.
template <class A> struct std::hash<nx::basic_string<A>> {
  using is_transparent = void;
  [[nodiscard]] std::size_t operator()(const nx::string_view s) const noexcept {
    return nx::cast<std::size_t>(nx::hash_fnv1a64(s.data(), s.size()));
  }
  [[nodiscard]] std::size_t
  operator()(const nx::basic_string<A> &s) const noexcept {
    return (*this)(s.view());
  }
  [[nodiscard]] std::size_t operator()(const char *s) const noexcept {
    return (*this)(nx::string_view(s));
  }
};

template <class A, class CharT>
struct std::formatter<nx::basic_string<A>, CharT>
    : std::formatter<std::basic_string_view<CharT>, CharT> {
  template <class FormatContext>
  auto format(const nx::basic_string<A> &s, FormatContext &ctx) const {
    using Base = std::formatter<std::basic_string_view<CharT>, CharT>;
    if constexpr (std::is_same_v<CharT, char>)
      return Base::format(std::string_view(s.c_str(), s.size()), ctx);
    else if constexpr (sizeof(CharT) == 2) {
      const std::u16string tmp = s.u16string();
      return Base::format(
          std::basic_string_view<CharT>(
              reinterpret_cast<const CharT *>(tmp.data()), tmp.size()),
          ctx);
    } else {
      const std::u32string tmp = s.u32string();
      return Base::format(
          std::basic_string_view<CharT>(
              reinterpret_cast<const CharT *>(tmp.data()), tmp.size()),
          ctx);
    }
  }
};
