#include "core/foundation/serialization/json.h"

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/hash.h"

#include <charconv>
#include <cmath>
#include <cstring>

#if !defined(__cpp_lib_to_chars)
// libc++ (so every Apple toolchain up to at least Xcode 16) still deletes the
// floating-point overloads of std::from_chars, and only defines this macro
// once they exist. The integer overloads are unaffected.
#include <cerrno>
#include <cstdlib>
#include <xlocale.h>
#endif

namespace nx::json {
namespace {

#if defined(__cpp_lib_to_chars)
using std::from_chars;
#else
/// std::from_chars for f64, on toolchains that do not have it yet.
///
/// strtod_l with a null locale is the C locale on Darwin, which is what JSON
/// needs: a user running under de_DE must not have "1.5" parsed as 1.
std::from_chars_result from_chars(const char *first, const char *last,
                                  f64 &out) noexcept {
  // strtod needs a terminator and the parser hands us a view. A number literal
  // this long is malformed in every practical sense, so rejecting is fine.
  char buf[512];
  const usize len = nx::cast<usize>(last - first);
  if (len == 0 || len >= sizeof(buf))
    return {first, std::errc::invalid_argument};
  std::memcpy(buf, first, len);
  buf[len] = '\0';

  errno = 0;
  char *end = nullptr;
  const double value = ::strtod_l(buf, &end, nullptr);
  if (end == buf)
    return {first, std::errc::invalid_argument};

  // On overflow/underflow strtod stores the saturated value (+/-inf or 0),
  // exactly as from_chars does alongside result_out_of_range.
  out = nx::cast<f64>(value);
  return {first + (end - buf),
          errno == ERANGE ? std::errc::result_out_of_range : std::errc{}};
}
#endif

// Character classification
//
// Table lookups rather than comparison chains: the scanner touches every byte
// of the input at least once, so a single indexed load beats four branches.

/// Bit 0: JSON whitespace. Bit 1: terminates a bare number literal.
alignas(64) constexpr u8 kCharClass[256] = {
    // 0x00-0x1F: only the four whitespace characters are legal in a document,
    // and none of them may appear unescaped inside a string.
    /*00*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0,
    /*10*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*20*/ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0,
    /*30*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*40*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*50*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 0,
    /*60*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*70*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 0,
    /*80*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*90*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*A0*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*B0*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*C0*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*D0*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*E0*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /*F0*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

NX_FORCE_INLINE bool is_space(u8 c) noexcept {
  return (kCharClass[c] & 1u) != 0;
}
NX_FORCE_INLINE bool is_digit(u8 c) noexcept { return c >= '0' && c <= '9'; }

/// -1 when @p c is not a hex digit.
NX_FORCE_INLINE i32 hex_value(u8 c) noexcept {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// Parser

class parser {
public:
  parser(const string_view text, const parse_options &opts) noexcept
      : m_begin(text.data()), m_cur(text.data()),
        m_end(text.data() + text.size()), m_opts(opts) {}

  result<value> run() {
    if (m_opts.validate_utf8 &&
        !utf8::validate_utf8(m_begin, nx::cast<usize>(m_end - m_begin)))
      return fail(error_code::invalid_utf8, m_begin);

    skip_space();
    value root;
    if (const error_code e = parse_value(root, 0); e != error_code::none)
      return fail(e, m_error_pos);

    skip_space();
    if (m_cur != m_end)
      return fail(error_code::trailing_content, m_cur);
    return root;
  }

private:
  const char *m_begin;
  const char *m_cur;
  const char *m_end;
  parse_options m_opts;
  const char *m_error_pos = nullptr;

  [[nodiscard]] NX_FORCE_INLINE bool at_end() const noexcept {
    return m_cur >= m_end;
  }
  [[nodiscard]] NX_FORCE_INLINE u8 peek() const noexcept {
    return nx::cast<u8>(*m_cur);
  }

  /// Records where an error happened and returns the code, so callers can
  /// `return set(code)` in one statement.
  NX_FORCE_INLINE error_code set(error_code e, const char *at) noexcept {
    m_error_pos = at;
    return e;
  }

  result<value> fail(const error_code e, const char *at) const {
    const auto offset = nx::cast<usize>((at ? at : m_cur) - m_begin);
    u32 line = 1, column = 1;
    for (usize i = 0; i < offset; ++i) {
      if (m_begin[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    return nx::err(error{e, offset, line, column});
  }

  void skip_space() noexcept {
    for (;;) {
      while (m_cur < m_end && is_space(peek()))
        ++m_cur;
      if (!m_opts.allow_comments || m_cur + 1 >= m_end || *m_cur != '/')
        return;
      if (m_cur[1] == '/') {
        m_cur += 2;
        while (m_cur < m_end && *m_cur != '\n')
          ++m_cur;
      } else if (m_cur[1] == '*') {
        m_cur += 2;
        while (m_cur + 1 < m_end && !(m_cur[0] == '*' && m_cur[1] == '/'))
          ++m_cur;
        m_cur = (m_cur + 1 < m_end) ? m_cur + 2 : m_end;
      } else {
        return; // a lone '/' is not a comment; let the value parser reject it
      }
    }
  }

  /// Matches a bare literal such as `true`; m_cur is left past it on success.
  NX_FORCE_INLINE bool match(const char *lit, usize n) noexcept {
    if (nx::cast<usize>(m_end - m_cur) < n || std::memcmp(m_cur, lit, n) != 0)
      return false;
    m_cur += n;
    return true;
  }

  error_code parse_value(value &out, u32 depth) {
    if (at_end())
      return set(error_code::unexpected_end, m_cur);

    switch (peek()) {
    case '{':
      return parse_object(out, depth);
    case '[':
      return parse_array(out, depth);
    case '"': {
      nx::string s;
      if (const error_code e = parse_string(s); e != error_code::none)
        return e;
      out = std::move(s);
      return error_code::none;
    }
    case 't':
      if (!match("true", 4))
        return set(error_code::unexpected_token, m_cur);
      out = true;
      return error_code::none;
    case 'f':
      if (!match("false", 5))
        return set(error_code::unexpected_token, m_cur);
      out = false;
      return error_code::none;
    case 'n':
      if (!match("null", 4))
        return set(error_code::unexpected_token, m_cur);
      out = value{};
      return error_code::none;
    default:
      return parse_number(out);
    }
  }

  error_code parse_array(value &out, u32 depth) {
    if (depth >= m_opts.max_depth)
      return set(error_code::depth_exceeded, m_cur);
    ++m_cur; // '['
    out = value::make_array();

    skip_space();
    if (at_end())
      return set(error_code::unexpected_end, m_cur);
    if (peek() == ']') {
      ++m_cur;
      return error_code::none;
    }

    for (;;) {
      value element;
      skip_space();
      if (const error_code e = parse_value(element, depth + 1);
          e != error_code::none)
        return e;
      out.push_back(std::move(element));

      skip_space();
      if (at_end())
        return set(error_code::unexpected_end, m_cur);
      const u8 c = peek();
      if (c == ',') {
        ++m_cur;
        skip_space();
        if (!at_end() && peek() == ']') {
          if (!m_opts.allow_trailing_commas)
            return set(error_code::unexpected_token, m_cur);
          ++m_cur;
          return error_code::none;
        }
        continue;
      }
      if (c == ']') {
        ++m_cur;
        return error_code::none;
      }
      return set(error_code::unexpected_token, m_cur);
    }
  }

  /// Member count past which an object switches from a linear duplicate scan
  /// to a hash index. Below it the scan is cheaper than hashing; above it the
  /// quadratic cost starts to dominate.
  static constexpr usize kIndexThreshold = 32;
  using key_index = hash_map<u64, u32>;

  /// Existing member with this key, or null. Uses @p index once @p indexed is
  /// set, otherwise a linear scan.
  static value *find_member(object &members, string_view key, key_index &index,
                            bool indexed) {
    if (!indexed) {
      for (member &m : members)
        if (m.key.view() == key)
          return &m.val;
      return nullptr;
    }
    const auto it = index.find(hash_fnv1a64(key.data(), key.size()));
    if (it == index.end())
      return nullptr;
    member &m = members[it->second];
    if (m.key.view() == key)
      return &m.val;
    // Two distinct keys hashed the same. Astronomically rare for a 64-bit
    // hash, but it must not silently report "absent" and duplicate the member,
    // so fall back to an exact scan.
    for (member &candidate : members)
      if (candidate.key.view() == key)
        return &candidate.val;
    return nullptr;
  }

  /// Records the member just appended, building the index on the way past the
  /// threshold.
  static void note_member(object &members, key_index &index, bool &indexed) {
    if (indexed) {
      const member &m = members.back();
      index.insert_or_assign(hash_fnv1a64(m.key.data(), m.key.size()),
                             nx::cast<u32>(members.size() - 1));
      return;
    }
    if (members.size() < kIndexThreshold)
      return;
    index.reserve(members.size() * 2);
    for (usize i = 0; i < members.size(); ++i)
      index.insert_or_assign(
          hash_fnv1a64(members[i].key.data(), members[i].key.size()),
          nx::cast<u32>(i));
    indexed = true;
  }

  error_code parse_object(value &out, u32 depth) {
    if (depth >= m_opts.max_depth)
      return set(error_code::depth_exceeded, m_cur);
    ++m_cur; // '{'
    out = value::make_object();

    skip_space();
    if (at_end())
      return set(error_code::unexpected_end, m_cur);
    if (peek() == '}') {
      ++m_cur;
      return error_code::none;
    }

    // Deduplicating each member against the ones already parsed is a linear
    // scan, which makes an object quadratic in its member count. That is fine
    // for the handful of keys a normal object has, but it is a denial-of-
    // service vector on untrusted input: a single 100k-key object would cost
    // billions of comparisons. Past kIndexThreshold members we switch to a
    // hash index and the cost becomes linear. The map allocates nothing until
    // that point, so small objects pay only for an empty container.
    key_index index;
    bool indexed = false;

    for (;;) {
      skip_space();
      if (at_end())
        return set(error_code::unexpected_end, m_cur);
      if (peek() != '"')
        return set(error_code::unexpected_token, m_cur);

      const char *key_at = m_cur;
      nx::string key;
      if (const error_code e = parse_string(key); e != error_code::none)
        return e;

      skip_space();
      if (at_end())
        return set(error_code::unexpected_end, m_cur);
      if (peek() != ':')
        return set(error_code::unexpected_token, m_cur);
      ++m_cur;

      value element;
      skip_space();
      if (const error_code e = parse_value(element, depth + 1);
          e != error_code::none)
        return e;

      object &members = out.as_object();
      value *existing = find_member(members, key.view(), index, indexed);
      if (existing != nullptr) {
        if (m_opts.reject_duplicate_keys)
          return set(error_code::duplicate_key, key_at);
        *existing = std::move(element); // last occurrence wins
      } else {
        members.push_back(member{std::move(key), std::move(element)});
        note_member(members, index, indexed);
      }

      skip_space();
      if (at_end())
        return set(error_code::unexpected_end, m_cur);
      const u8 c = peek();
      if (c == ',') {
        ++m_cur;
        skip_space();
        if (!at_end() && peek() == '}') {
          if (!m_opts.allow_trailing_commas)
            return set(error_code::unexpected_token, m_cur);
          ++m_cur;
          return error_code::none;
        }
        continue;
      }
      if (c == '}') {
        ++m_cur;
        return error_code::none;
      }
      return set(error_code::unexpected_token, m_cur);
    }
  }

  /// Appends the UTF-8 encoding of @p cp to @p out. @p cp is already validated.
  static void append_codepoint(nx::string &out, char32_t cp) {
    char buf[4];
    usize len = 0;
    utf8::encode_char(cp, buf, len);
    out.append(buf, len);
  }

  error_code parse_escape(nx::string &out) {
    // m_cur points at the character after the backslash.
    if (at_end())
      return set(error_code::unexpected_end, m_cur);
    const char *esc_at = m_cur - 1;
    const u8 c = peek();
    ++m_cur;
    switch (c) {
    case '"':
      out.push_back('"');
      return error_code::none;
    case '\\':
      out.push_back('\\');
      return error_code::none;
    case '/':
      out.push_back('/');
      return error_code::none;
    case 'b':
      out.push_back('\b');
      return error_code::none;
    case 'f':
      out.push_back('\f');
      return error_code::none;
    case 'n':
      out.push_back('\n');
      return error_code::none;
    case 'r':
      out.push_back('\r');
      return error_code::none;
    case 't':
      out.push_back('\t');
      return error_code::none;
    case 'u':
      break;
    default:
      return set(error_code::invalid_escape, esc_at);
    }

    u32 unit = 0;
    if (const error_code e = read_hex4(unit, esc_at); e != error_code::none)
      return e;

    if (utf8::is_high_surrogate(nx::cast<char16_t>(unit))) {
      // Must be followed by \uDC00-\uDFFF to form a supplementary code point.
      if (m_end - m_cur < 2 || m_cur[0] != '\\' || m_cur[1] != 'u')
        return set(error_code::invalid_surrogate, esc_at);
      m_cur += 2;
      u32 low = 0;
      if (const error_code e = read_hex4(low, esc_at); e != error_code::none)
        return e;
      if (!utf8::is_low_surrogate(nx::cast<char16_t>(low)))
        return set(error_code::invalid_surrogate, esc_at);
      const auto cp = nx::cast<char32_t>(0x10000u + ((unit - 0xD800u) << 10) +
                                         (low - 0xDC00u));
      append_codepoint(out, cp);
      return error_code::none;
    }
    if (utf8::is_low_surrogate(nx::cast<char16_t>(unit)))
      return set(error_code::invalid_surrogate, esc_at); // unpaired low half

    append_codepoint(out, nx::cast<char32_t>(unit));
    return error_code::none;
  }

  error_code read_hex4(u32 &out, const char *esc_at) {
    // Checked one character at a time so the two failure modes stay distinct:
    // running out of input is a truncated document, whereas a character that
    // is present but not a hex digit is a malformed escape.
    u32 v = 0;
    for (int i = 0; i < 4; ++i) {
      if (m_cur + i >= m_end)
        return set(error_code::unexpected_end, m_cur);
      const i32 d = hex_value(nx::cast<u8>(m_cur[i]));
      if (d < 0)
        return set(error_code::invalid_escape, esc_at);
      v = (v << 4) | nx::cast<u32>(d);
    }
    m_cur += 4;
    out = v;
    return error_code::none;
  }

  error_code parse_string(nx::string &out) {
    ++m_cur; // opening quote
    out.clear();

    // Copy in runs: scan for the next byte that needs attention and append
    // everything before it in one go, so unescaped text costs one memcpy.
    for (;;) {
      const char *run = m_cur;
      while (m_cur < m_end) {
        const u8 c = nx::cast<u8>(*m_cur);
        if (c == '"' || c == '\\' || c < 0x20)
          break;
        ++m_cur;
      }
      if (m_cur > run)
        out.append(run, nx::cast<usize>(m_cur - run));

      if (at_end())
        return set(error_code::invalid_string, m_cur);

      const u8 c = peek();
      if (c == '"') {
        ++m_cur;
        return error_code::none;
      }
      if (c < 0x20) // raw control characters must be escaped
        return set(error_code::invalid_string, m_cur);

      ++m_cur; // the backslash
      if (const error_code e = parse_escape(out); e != error_code::none)
        return e;
    }
  }

  error_code parse_number(value &out) {
    const char *start = m_cur;

    if (m_cur < m_end && *m_cur == '-')
      ++m_cur;
    if (at_end())
      return set(error_code::invalid_number, start);

    // Integer part: either a lone 0 or a digit sequence with no leading zero.
    if (peek() == '0') {
      ++m_cur;
    } else if (is_digit(peek())) {
      while (m_cur < m_end && is_digit(peek()))
        ++m_cur;
    } else {
      return set(error_code::unexpected_token, start);
    }

    bool integral = true;
    if (m_cur < m_end && *m_cur == '.') {
      ++m_cur;
      if (at_end() || !is_digit(peek()))
        return set(error_code::invalid_number, start);
      while (m_cur < m_end && is_digit(peek()))
        ++m_cur;
      integral = false;
    }
    if (m_cur < m_end && (*m_cur == 'e' || *m_cur == 'E')) {
      ++m_cur;
      if (m_cur < m_end && (*m_cur == '+' || *m_cur == '-'))
        ++m_cur;
      if (at_end() || !is_digit(peek()))
        return set(error_code::invalid_number, start);
      while (m_cur < m_end && is_digit(peek()))
        ++m_cur;
      integral = false;
    }

    // A number must be followed by a delimiter; `1x` and `01` are not numbers.
    // With comments enabled a '/' also ends one, as in `[1 /* note */]`.
    if (m_cur < m_end) {
      const u8 c = peek();
      const bool delimits = is_space(c) || (kCharClass[c] & 2u) != 0 ||
                            (m_opts.allow_comments && c == '/');
      if (!delimits)
        return set(error_code::invalid_number, start);
    }

    const auto len = nx::cast<usize>(m_cur - start);
    if (integral) {
      // Exact integer path. from_chars reports out_of_range rather than
      // silently wrapping, and we then fall back to a double so that huge
      // literals still parse instead of failing the whole document.
      if (*start == '-') {
        i64 v = 0;
        const auto r = std::from_chars(start, m_cur, v);
        if (r.ec == std::errc{} && r.ptr == m_cur) {
          out = v;
          return error_code::none;
        }
      } else {
        u64 v = 0;
        const auto r = std::from_chars(start, m_cur, v);
        if (r.ec == std::errc{} && r.ptr == m_cur) {
          // Prefer the signed representation when it fits, so that small
          // positive integers compare equal to their signed counterparts
          // without needing cross-representation logic everywhere.
          if (v <= nx::cast<u64>(9223372036854775807LL))
            out = nx::cast<i64>(v);
          else
            out = v;
          return error_code::none;
        }
      }
    }

    f64 d = 0.0;
    const auto r = from_chars(start, m_cur, d); // unqualified: see the top of
                                                // this file for the fallback
    if (r.ptr != m_cur ||
        (r.ec != std::errc{} && r.ec != std::errc::result_out_of_range))
      return set(error_code::invalid_number, start);
    // result_out_of_range means the literal overflowed to +/-inf or underflowed
    // to zero; from_chars has already stored the saturated value.
    (void)len;
    out = d;
    return error_code::none;
  }
};

// Writer

class writer {
public:
  writer(nx::string &out, const write_options &opts) noexcept
      : m_out(out), m_opts(opts) {}

  void write_value(const value &v, u32 depth) {
    switch (v.kind()) {
    case type::null:
      m_out.append("null", nx::cast<usize>(4));
      return;
    case type::boolean:
      if (v.as_bool())
        m_out.append("true", nx::cast<usize>(4));
      else
        m_out.append("false", nx::cast<usize>(5));
      return;
    case type::signed_integer:
      write_int(v.as_i64());
      return;
    case type::unsigned_integer:
      write_int(v.as_u64());
      return;
    case type::real:
      write_real(v.as_double());
      return;
    case type::string:
      write_string(v.as_string().view());
      return;
    case type::array:
      write_array(v, depth);
      return;
    case type::object:
      write_object(v, depth);
      return;
    }
  }

private:
  nx::string &m_out;
  write_options m_opts;

  template <typename T> void write_int(T v) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    m_out.append(buf, nx::cast<usize>(r.ptr - buf));
  }

  /// Largest fraction width honoured from write_options::real_precision. The
  /// buffer below is sized for it, so the conversion cannot fail for space.
  static constexpr u32 kMaxPrecision = 64;

  void write_real(f64 v) {
    // JSON has no way to spell a non-finite number; null is what every other
    // implementation emits and what round-trips without producing invalid text.
    //
    // std::isfinite rather than a hand-rolled `v != v || v == infinity()`: on a
    // target whose numeric_limits reports no infinity, infinity() returns 0.0
    // and that form would quietly turn every zero into null. It is also the
    // form that survives a compiler being told it may assume finite math.
    if (!std::isfinite(v)) {
      m_out.append("null", nx::cast<usize>(4));
      return;
    }

    // Worst case in fixed notation: sign, 309 integer digits, the point and
    // kMaxPrecision fraction digits.
    char buf[400];
    std::to_chars_result r{};
    if (m_opts.real_precision > 0)
      r = std::to_chars(
          buf, buf + sizeof(buf), v, std::chars_format::fixed,
          nx::cast<int>(nx::min(m_opts.real_precision, kMaxPrecision)));
    else
      r = std::to_chars(buf, buf + sizeof(buf), v);

    if (r.ec != std::errc{}) {
      // Unreachable at this buffer size, but a finite number must never be
      // written as something that is not that number: fall back to the shortest
      // round-trip form rather than emitting null.
      r = std::to_chars(buf, buf + sizeof(buf), v);
      if (r.ec != std::errc{}) {
        m_out.append("null", nx::cast<usize>(4));
        return;
      }
    }
    const auto n = nx::cast<usize>(r.ptr - buf);
    m_out.append(buf, n);

    // to_chars produces the shortest round-trip form, which for a whole number
    // is "1" - valid JSON, but it loses the fact that the value was a real. Add
    // a fractional part so a double stays a double across a round trip.
    if (m_opts.real_precision == 0 && std::memchr(buf, '.', n) == nullptr &&
        std::memchr(buf, 'e', n) == nullptr)
      m_out.append(".0", nx::cast<usize>(2));
  }

  void write_string(string_view s) {
    m_out.reserve(m_out.size() + s.size() + 2);
    m_out.push_back('"');

    const char *const begin = s.data();
    const char *const end = begin + s.size();
    const char *run = begin;

    for (const char *p = begin; p < end;) {
      const u8 c = nx::cast<u8>(*p);
      const bool needs_escape = c < 0x20 || c == '"' || c == '\\' ||
                                (m_opts.escape_forward_slash && c == '/') ||
                                (m_opts.escape_non_ascii && c >= 0x80);
      if (!needs_escape) {
        ++p;
        continue;
      }

      if (p > run)
        m_out.append(run, nx::cast<usize>(p - run));

      if (c >= 0x80) {
        // Re-encode the whole code point as one or two \u escapes.
        char32_t cp = 0;
        usize read = 0;
        if (!utf8::decode_char(p, nx::cast<usize>(end - p), cp, read)) {
          // Not decodable: emit the replacement character rather than
          // producing invalid output.
          append_u_escape(0xFFFD);
          ++p;
        } else {
          if (cp <= 0xFFFF) {
            append_u_escape(nx::cast<u32>(cp));
          } else {
            const char32_t v = cp - 0x10000u;
            append_u_escape(0xD800u + nx::cast<u32>(v >> 10));
            append_u_escape(0xDC00u + nx::cast<u32>(v & 0x3FFu));
          }
          p += read;
        }
      } else {
        append_short_escape(c);
        ++p;
      }
      run = p;
    }
    if (end > run)
      m_out.append(run, nx::cast<usize>(end - run));
    m_out.push_back('"');
  }

  void append_short_escape(u8 c) {
    switch (c) {
    case '"':
      m_out.append("\\\"", nx::cast<usize>(2));
      return;
    case '\\':
      m_out.append("\\\\", nx::cast<usize>(2));
      return;
    case '/':
      m_out.append("\\/", nx::cast<usize>(2));
      return;
    case '\b':
      m_out.append("\\b", nx::cast<usize>(2));
      return;
    case '\f':
      m_out.append("\\f", nx::cast<usize>(2));
      return;
    case '\n':
      m_out.append("\\n", nx::cast<usize>(2));
      return;
    case '\r':
      m_out.append("\\r", nx::cast<usize>(2));
      return;
    case '\t':
      m_out.append("\\t", nx::cast<usize>(2));
      return;
    default:
      append_u_escape(c); // any other control character
      return;
    }
  }

  void append_u_escape(u32 v) const {
    static constexpr char kHex[] = "0123456789abcdef";
    char buf[6] = {'\\', 'u', 0, 0, 0, 0};
    buf[2] = kHex[(v >> 12) & 0xF];
    buf[3] = kHex[(v >> 8) & 0xF];
    buf[4] = kHex[(v >> 4) & 0xF];
    buf[5] = kHex[v & 0xF];
    m_out.append(buf, nx::cast<usize>(6));
  }

  void newline_indent(u32 depth) const {
    if (m_opts.indent == 0)
      return;
    m_out.push_back('\n');
    m_out.append(nx::cast<usize>(depth) * m_opts.indent, ' ');
  }

  void write_array(const value &v, u32 depth) {
    const array &a = v.as_array();
    if (a.empty()) {
      m_out.append("[]", nx::cast<usize>(2));
      return;
    }
    m_out.push_back('[');
    for (usize i = 0; i < a.size(); ++i) {
      if (i != 0)
        m_out.push_back(',');
      newline_indent(depth + 1);
      write_value(a[i], depth + 1);
    }
    newline_indent(depth);
    m_out.push_back(']');
  }

  void write_object(const value &v, u32 depth) {
    const object &o = v.as_object();
    if (o.empty()) {
      m_out.append("{}", nx::cast<usize>(2));
      return;
    }
    m_out.push_back('{');
    for (usize i = 0; i < o.size(); ++i) {
      if (i != 0)
        m_out.push_back(',');
      newline_indent(depth + 1);
      write_string(o[i].key.view());
      m_out.push_back(':');
      if (m_opts.indent != 0)
        m_out.push_back(' ');
      write_value(o[i].val, depth + 1);
    }
    newline_indent(depth);
    m_out.push_back('}');
  }
};

} // namespace

// error

string_view to_string(error_code e) noexcept {
  switch (e) {
  case error_code::none:
    return "no error";
  case error_code::unexpected_end:
    return "unexpected end of input";
  case error_code::unexpected_token:
    return "unexpected token";
  case error_code::invalid_number:
    return "invalid number literal";
  case error_code::invalid_string:
    return "invalid string";
  case error_code::invalid_escape:
    return "invalid escape sequence";
  case error_code::invalid_utf8:
    return "invalid UTF-8";
  case error_code::invalid_surrogate:
    return "unpaired UTF-16 surrogate";
  case error_code::depth_exceeded:
    return "maximum nesting depth exceeded";
  case error_code::trailing_content:
    return "trailing content after the document";
  case error_code::duplicate_key:
    return "duplicate object key";
  case error_code::number_out_of_range:
    return "number out of range for the target type";
  case error_code::type_mismatch:
    return "type mismatch";
  case error_code::missing_field:
    return "missing required field";
  default:
    return "unknown error";
  }
}

nx::string error::message() const {
  nx::string out;
  char buf[48];
  const auto r = std::to_chars(buf, buf + sizeof(buf), line);
  out.append(buf, nx::cast<usize>(r.ptr - buf));
  out.push_back(':');
  const auto r2 = std::to_chars(buf, buf + sizeof(buf), column);
  out.append(buf, nx::cast<usize>(r2.ptr - buf));
  out.append(": ", nx::cast<usize>(2));
  out.append(json::to_string(code));
  return out;
}

// value

value::value(string_view s) : m_type(type::string) {
  std::construct_at(&m_u.string, s.data(), s.size());
}
value::value(const nx::string &s) : m_type(type::string) {
  std::construct_at(&m_u.string, s);
}
value::value(nx::string &&s) noexcept : m_type(type::string) {
  std::construct_at(&m_u.string, std::move(s));
}

value value::make_array() {
  value v;
  v.m_type = type::array;
  std::construct_at(&v.m_u.m_array);
  return v;
}
value value::make_object() {
  value v;
  v.m_type = type::object;
  std::construct_at(&v.m_u.m_object);
  return v;
}
value value::make_array(array a) noexcept {
  value v;
  v.m_type = type::array;
  std::construct_at(&v.m_u.m_array, std::move(a));
  return v;
}
value value::make_object(object o) noexcept {
  value v;
  v.m_type = type::object;
  std::construct_at(&v.m_u.m_object, std::move(o));
  return v;
}

// Listed exhaustively rather than with a default label so that adding an
// alternative to `type` is a compiler warning here instead of a silent leak.
void value::destroy() noexcept {
  switch (m_type) {
  case type::null:
  case type::boolean:
  case type::signed_integer:
  case type::unsigned_integer:
  case type::real:
    break; // trivially destructible
  case type::string:
    std::destroy_at(&m_u.string);
    break;
  case type::array:
    std::destroy_at(&m_u.m_array);
    break;
  case type::object:
    std::destroy_at(&m_u.m_object);
    break;
  }
  m_type = type::null;
}

// Every alternative is activated explicitly, including the scalar ones.
//
// memcpy-ing the union would be undefined twice over: `storage` holds a string
// and two vectors, so it is not trivially copyable and its bytes may not be
// copied that way; and copying bytes into it never begins the lifetime of any
// member, leaving the union with no active alternative to read back.
// construct_at both copies the value and makes that member active.
void value::construct_from(const value &o) {
  m_type = o.m_type;
  switch (o.m_type) {
  case type::null:
    break; // no active member
  case type::boolean:
    std::construct_at(&m_u.boolean, o.m_u.boolean);
    break;
  case type::signed_integer:
    std::construct_at(&m_u.signed_integer, o.m_u.signed_integer);
    break;
  case type::unsigned_integer:
    std::construct_at(&m_u.unsigned_integer, o.m_u.unsigned_integer);
    break;
  case type::real:
    std::construct_at(&m_u.real, o.m_u.real);
    break;
  case type::string:
    std::construct_at(&m_u.string, o.m_u.string);
    break;
  case type::array:
    std::construct_at(&m_u.m_array, o.m_u.m_array);
    break;
  case type::object:
    std::construct_at(&m_u.m_object, o.m_u.m_object);
    break;
  }
}

void value::construct_from(value &&o) noexcept {
  m_type = o.m_type;
  switch (o.m_type) {
  case type::null:
    break;
  case type::boolean:
    std::construct_at(&m_u.boolean, o.m_u.boolean);
    break;
  case type::signed_integer:
    std::construct_at(&m_u.signed_integer, o.m_u.signed_integer);
    break;
  case type::unsigned_integer:
    std::construct_at(&m_u.unsigned_integer, o.m_u.unsigned_integer);
    break;
  case type::real:
    std::construct_at(&m_u.real, o.m_u.real);
    break;
  case type::string:
    std::construct_at(&m_u.string, std::move(o.m_u.string));
    break;
  case type::array:
    std::construct_at(&m_u.m_array, std::move(o.m_u.m_array));
    break;
  case type::object:
    std::construct_at(&m_u.m_object, std::move(o.m_u.m_object));
    break;
  }
  o.destroy();
}

value::value(const value &o) : m_type(type::null) { construct_from(o); }
value::value(value &&o) noexcept : m_type(type::null) {
  construct_from(std::move(o));
}

value &value::operator=(const value &o) {
  if (this != &o) {
    value tmp(o); // build first, so a throwing copy leaves *this intact
    destroy();
    construct_from(std::move(tmp));
  }
  return *this;
}

value &value::operator=(value &&o) noexcept {
  if (this != &o) {
    destroy();
    construct_from(std::move(o));
  }
  return *this;
}

value::~value() { destroy(); }

void value::swap(value &o) noexcept {
  value tmp(std::move(*this));
  *this = std::move(o);
  o = std::move(tmp);
}

const value &value::null_value() noexcept {
  static const value shared;
  return shared;
}

bool value::as_bool() const noexcept {
  assert(is_bool() && "json::value::as_bool on a non-boolean");
  return m_u.boolean;
}
i64 value::as_i64() const noexcept {
  assert(is_i64() && "json::value::as_i64 on a non-signed-integer");
  return m_u.signed_integer;
}
u64 value::as_u64() const noexcept {
  assert(is_u64() && "json::value::as_u64 on a non-unsigned-integer");
  return m_u.unsigned_integer;
}
f64 value::as_double() const noexcept {
  assert(is_double() && "json::value::as_double on a non-real");
  return m_u.real;
}
const nx::string &value::as_string() const noexcept {
  assert(is_string() && "json::value::as_string on a non-string");
  return m_u.string;
}
nx::string &value::as_string() noexcept {
  assert(is_string() && "json::value::as_string on a non-string");
  return m_u.string;
}
const array &value::as_array() const noexcept {
  assert(is_array() && "json::value::as_array on a non-array");
  return m_u.m_array;
}
array &value::as_array() noexcept {
  assert(is_array() && "json::value::as_array on a non-array");
  return m_u.m_array;
}
const object &value::as_object() const noexcept {
  assert(is_object() && "json::value::as_object on a non-object");
  return m_u.m_object;
}
object &value::as_object() noexcept {
  assert(is_object() && "json::value::as_object on a non-object");
  return m_u.m_object;
}

bool value::get_bool(bool fallback) const noexcept {
  return is_bool() ? m_u.boolean : fallback;
}

i64 value::get_i64(i64 fallback) const noexcept {
  switch (m_type) {
  case type::signed_integer:
    return m_u.signed_integer;
  case type::unsigned_integer:
    return m_u.unsigned_integer <= nx::cast<u64>(9223372036854775807LL)
               ? nx::cast<i64>(m_u.unsigned_integer)
               : fallback;
  case type::real: {
    const f64 d = m_u.real;
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0 &&
        d == nx::cast<f64>(nx::cast<i64>(d)))
      return nx::cast<i64>(d);
    return fallback;
  }
  default:
    return fallback;
  }
}

u64 value::get_u64(u64 fallback) const noexcept {
  switch (m_type) {
  case type::unsigned_integer:
    return m_u.unsigned_integer;
  case type::signed_integer:
    return m_u.signed_integer >= 0 ? nx::cast<u64>(m_u.signed_integer)
                                   : fallback;
  case type::real: {
    const f64 d = m_u.real;
    if (d >= 0.0 && d < 18446744073709551616.0 &&
        d == nx::cast<f64>(nx::cast<u64>(d)))
      return nx::cast<u64>(d);
    return fallback;
  }
  default:
    return fallback;
  }
}

f64 value::get_double(f64 fallback) const noexcept {
  switch (m_type) {
  case type::real:
    return m_u.real;
  case type::signed_integer:
    return nx::cast<f64>(m_u.signed_integer);
  case type::unsigned_integer:
    return nx::cast<f64>(m_u.unsigned_integer);
  default:
    return fallback;
  }
}

string_view value::get_string(string_view fallback) const noexcept {
  return is_string() ? m_u.string.view() : fallback;
}

const nx::string *value::try_string() const noexcept {
  return is_string() ? &m_u.string : nullptr;
}
const array *value::try_array() const noexcept {
  return is_array() ? &m_u.m_array : nullptr;
}
const object *value::try_object() const noexcept {
  return is_object() ? &m_u.m_object : nullptr;
}
nx::string *value::try_string() noexcept {
  return is_string() ? &m_u.string : nullptr;
}
array *value::try_array() noexcept {
  return is_array() ? &m_u.m_array : nullptr;
}
object *value::try_object() noexcept {
  return is_object() ? &m_u.m_object : nullptr;
}

usize value::size() const noexcept {
  switch (m_type) {
  case type::array:
    return m_u.m_array.size();
  case type::object:
    return m_u.m_object.size();
  default:
    return 0;
  }
}

const value &value::operator[](usize i) const noexcept {
  assert(is_array() && i < m_u.m_array.size() &&
         "json::value: index out of range");
  return m_u.m_array[i];
}
value &value::operator[](usize i) noexcept {
  assert(is_array() && i < m_u.m_array.size() &&
         "json::value: index out of range");
  return m_u.m_array[i];
}

const value *value::find(string_view key) const noexcept {
  if (!is_object())
    return nullptr;
  for (const member &m : m_u.m_object)
    if (m.key.view() == key)
      return &m.val;
  return nullptr;
}

value *value::find(string_view key) noexcept {
  if (!is_object())
    return nullptr;
  for (member &m : m_u.m_object)
    if (m.key.view() == key)
      return &m.val;
  return nullptr;
}

bool value::contains(string_view key) const noexcept {
  return find(key) != nullptr;
}

const value &value::operator[](string_view key) const noexcept {
  const value *m = find(key);
  return m ? *m : null_value();
}

value &value::operator[](string_view key) {
  if (is_null())
    *this = make_object();
  assert(is_object() && "json::value: keyed access on a non-object");
  if (value *m = find(key))
    return *m;
  m_u.m_object.push_back(member{nx::string(key), value{}});
  return m_u.m_object.back().val;
}

void value::push_back(value v) {
  if (is_null())
    *this = make_array();
  assert(is_array() && "json::value::push_back on a non-array");
  m_u.m_array.push_back(std::move(v));
}

value &value::emplace_back(value v) {
  push_back(std::move(v));
  return m_u.m_array.back();
}

value &value::insert(string_view key, value v) {
  if (is_null())
    *this = make_object();
  assert(is_object() && "json::value::insert on a non-object");
  if (value *m = find(key)) {
    *m = std::move(v);
    return *m;
  }
  m_u.m_object.push_back(member{nx::string(key), std::move(v)});
  return m_u.m_object.back().val;
}

bool value::erase(string_view key) {
  if (!is_object())
    return false;
  object &o = m_u.m_object;
  for (usize i = 0; i < o.size(); ++i) {
    if (o[i].key.view() == key) {
      o.erase(o.begin() + nx::cast<isize>(i));
      return true;
    }
  }
  return false;
}

void value::reserve(usize n) {
  switch (m_type) {
  case type::array:
    m_u.m_array.reserve(n);
    break;
  case type::object:
    m_u.m_object.reserve(n);
    break;
  default:
    break;
  }
}

void value::clear() noexcept {
  switch (m_type) {
  case type::array:
    m_u.m_array.clear();
    break;
  case type::object:
    m_u.m_object.clear();
    break;
  case type::string:
    m_u.string.clear();
    break;
  default:
    destroy();
    break;
  }
}

bool operator==(const value &a, const value &b) noexcept {
  // Numbers compare by mathematical value so that 1, 1u and 1.0 are equal.
  if (a.is_number() && b.is_number()) {
    if (a.is_double() || b.is_double())
      return a.get_double() == b.get_double();
    if (a.is_u64() || b.is_u64()) {
      // One of them may be a negative signed value, which no u64 can equal.
      if ((a.is_i64() && a.as_i64() < 0) || (b.is_i64() && b.as_i64() < 0))
        return false;
      return a.get_u64() == b.get_u64();
    }
    return a.as_i64() == b.as_i64();
  }
  if (a.kind() != b.kind())
    return false;

  switch (a.kind()) {
  case type::null:
    return true;
  case type::boolean:
    return a.as_bool() == b.as_bool();
  case type::string:
    return a.as_string().view() == b.as_string().view();
  case type::array: {
    const array &x = a.as_array();
    const array &y = b.as_array();
    if (x.size() != y.size())
      return false;
    for (usize i = 0; i < x.size(); ++i)
      if (!(x[i] == y[i]))
        return false;
    return true;
  }
  case type::object: {
    const object &x = a.as_object();
    const object &y = b.as_object();
    if (x.size() != y.size())
      return false;
    // Member order is not significant, so match by key.
    //
    // Spelled !(a == b) rather than a != b: this *is* operator==, and in C++20
    // `!=` on a type with only operator== rewrites back to it, which MSVC
    // rightly flags (C5232) as a call to the function we are inside. The
    // recursion is intended - a value is a tree - but the explicit form says so
    // without going through the rewrite.
    for (const member &m : x) {
      const value *other = b.find(m.key.view());
      if (!other || !(m.val == *other))
        return false;
    }
    return true;
  }
  default:
    return false;
  }
}

// Entry points

result<value> parse(string_view text, const parse_options &options) {
  parser p(text, options);
  return p.run();
}

void write_to(nx::string &out, const value &v, const write_options &options) {
  writer w(out, options);
  w.write_value(v, 0);
}

nx::string write(const value &v, const write_options &options) {
  nx::string out;
  write_to(out, v, options);
  return out;
}

} // namespace nx::json
