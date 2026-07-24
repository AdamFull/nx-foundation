#pragma once

/**
 * @file json.h
 * @brief JSON document model, parser and writer (namespace nx::json).
 *
 * A DOM-style JSON implementation built entirely on the foundation: nx::string
 * for text, nx::vector for arrays and object members, nx::result for errors. It
 * does not allocate beyond the document itself - parsing is a single pass over
 * the input with no intermediate token stream, and writing appends straight
 * into the destination buffer.
 *
 * @code
 * auto doc = nx::json::parse(text);
 * if (!doc) return doc.error();
 * const i64 id = (*doc)["id"].as_i64();
 * const nx::string out = nx::json::write(*doc, {.indent = 2});
 * @endcode
 *
 * @section json_numbers Numbers
 * Integers are kept exact. The parser records a number as signed, unsigned or
 * double depending on what the literal actually is, so a u64 identifier past
 * 2^53 survives a parse/write round trip unchanged - which a double-only model
 * cannot do.
 *
 * @section json_custom Custom types
 * Conversion for user types goes through nx::json::serializer<T>. Specialize it
 * (or provide to_json/from_json findable by ADL next to your type) and the whole
 * API - to_value, from_value, parse_as, write_as, and every container built on
 * them - works for that type. See "Custom type registration" below.
 *
 * @section json_threading Thread safety
 * A value is a plain object: concurrent reads are safe, concurrent mutation is
 * not. The parser and writer hold no shared state.
 */

#include "foundation/containers/flat_map.h"
#include "foundation/containers/hash_map.h"
#include "foundation/containers/small_vector.h"
#include "foundation/core/foundation.h"
#include "foundation/core/result.h"
#include "foundation/strings/utf8_string.h"

#include <limits>
#include <optional>
#include <type_traits>

namespace nx::json {

/// What went wrong while parsing, or while converting to a user type.
enum class error_code : u32 {
  none = 0,
  unexpected_end,     ///< Input ended in the middle of a value.
  unexpected_token,   ///< A character that cannot start or continue a value.
  invalid_number,     ///< Malformed numeric literal.
  invalid_string,     ///< Unterminated string or a raw control character.
  invalid_escape,     ///< Unknown \x escape, or a malformed \u sequence.
  invalid_utf8,       ///< Input was not well-formed UTF-8.
  invalid_surrogate,  ///< Unpaired or misordered UTF-16 surrogate.
  depth_exceeded,     ///< Nesting deeper than parse_options::max_depth.
  trailing_content,   ///< Non-whitespace bytes after the top-level value.
  duplicate_key,      ///< Repeated object key (only when rejected by options).
  number_out_of_range,///< Literal does not fit the requested arithmetic type.
  type_mismatch,      ///< Value is not the type a conversion required.
  missing_field,      ///< Required object member was absent.
  count,
};

/// Human-readable description of @p e; never empty.
[[nodiscard]] string_view to_string(error_code e) noexcept;

/**
 * @brief A parse or conversion failure, with the position that caused it.
 *
 * `offset` is a byte index into the input. `line` and `column` are 1-based and
 * measured in bytes; they are computed only when an error is produced, so the
 * success path pays nothing for them.
 */
struct error {
  error_code code = error_code::none;
  usize offset = 0;
  u32 line = 0;
  u32 column = 0;

  /// "line:column: description" - suitable for logging directly.
  [[nodiscard]] string message() const;
};

template <typename T> using result = nx::result<T, error>;

/// Discriminator for nx::json::value. Numbers keep their literal form so that
/// integer precision survives a round trip.
enum class type : u8 {
  null = 0,
  boolean,
  signed_integer,
  unsigned_integer,
  real,
  string,
  array,
  object,
};

/// True when @p t is one of the three numeric representations.
[[nodiscard]] constexpr bool is_number(type t) noexcept {
  return t == type::signed_integer || t == type::unsigned_integer ||
         t == type::real;
}

class value;

/// One key/value pair of an object. Members keep insertion order.
struct member;

using array = vector<value>;
using object = vector<member>;

/**
 * @brief A JSON value: null, boolean, number, string, array or object.
 *
 * A tagged union the size of a string plus a tag. Copyable (deep) and cheaply
 * movable. Accessors come in three flavours:
 *
 *  - `as_*()`   - precondition-checked; asserts in debug, undefined otherwise.
 *  - `get_*()`  - returns a fallback when the type does not match.
 *  - `try_*()`  - returns a pointer, null when the type does not match.
 *
 * Subscripting an object with a missing key returns a shared immutable null
 * rather than inserting, so `doc["a"]["b"].get_i64(0)` is safe on any document.
 * Use `insert()` or the non-const `at()` to mutate.
 */
class value {
public:
  value() noexcept : type_(type::null), u_{} {}
  value(std::nullptr_t) noexcept : value() {}

  value(bool b) noexcept : type_(type::boolean) { u_.boolean = b; }

  // Every integral type funnels into the signed or unsigned representation,
  // and every floating-point type into the real one. Templates rather than
  // overloads so that `value(0)`, `value(0u)` and `value(0.0)` are not
  // ambiguous with the bool and pointer constructors.
  template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
  value(T v) noexcept {
    if constexpr (std::is_signed_v<T>) {
      type_ = type::signed_integer;
      u_.signed_integer = static_cast<i64>(v);
    } else {
      type_ = type::unsigned_integer;
      u_.unsigned_integer = static_cast<u64>(v);
    }
  }

  template <typename T>
    requires std::is_floating_point_v<T>
  value(T v) noexcept : type_(type::real) {
    u_.real = static_cast<f64>(v);
  }

  value(const char *s) : value(string_view(s)) {}
  value(string_view s);
  value(const nx::string &s);
  value(nx::string &&s) noexcept;

  /// Tagged constructors for the container kinds.
  static value make_array();
  static value make_object();
  static value make_array(array a) noexcept;
  static value make_object(object o) noexcept;

  value(const value &o);
  value(value &&o) noexcept;
  value &operator=(const value &o);
  value &operator=(value &&o) noexcept;
  ~value();

  void swap(value &o) noexcept;

  // Type inspection

  [[nodiscard]] type kind() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == type::null; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == type::boolean; }
  [[nodiscard]] bool is_number() const noexcept { return json::is_number(type_); }
  [[nodiscard]] bool is_i64() const noexcept {
    return type_ == type::signed_integer;
  }
  [[nodiscard]] bool is_u64() const noexcept {
    return type_ == type::unsigned_integer;
  }
  [[nodiscard]] bool is_double() const noexcept { return type_ == type::real; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == type::string; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == type::array; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == type::object; }

  /// True when the value holds an integer exactly representable as T.
  template <typename T>
    requires std::is_integral_v<T>
  [[nodiscard]] bool fits() const noexcept;

  // Checked access. Preconditions are asserted in debug builds.

  [[nodiscard]] bool as_bool() const noexcept;
  [[nodiscard]] i64 as_i64() const noexcept;
  [[nodiscard]] u64 as_u64() const noexcept;
  [[nodiscard]] f64 as_double() const noexcept;
  [[nodiscard]] const nx::string &as_string() const noexcept;
  [[nodiscard]] nx::string &as_string() noexcept;
  [[nodiscard]] const array &as_array() const noexcept;
  [[nodiscard]] array &as_array() noexcept;
  [[nodiscard]] const object &as_object() const noexcept;
  [[nodiscard]] object &as_object() noexcept;

  // Forgiving access. Numbers convert between representations when the value
  // is exactly representable; anything else yields the fallback.

  [[nodiscard]] bool get_bool(bool fallback = false) const noexcept;
  [[nodiscard]] i64 get_i64(i64 fallback = 0) const noexcept;
  [[nodiscard]] u64 get_u64(u64 fallback = 0) const noexcept;
  [[nodiscard]] f64 get_double(f64 fallback = 0.0) const noexcept;

  /**
   * Text of a string value, or @p fallback for any other kind.
   *
   * The result is a view, so it borrows: on the matching path it aliases this
   * value's storage and is invalidated by any mutation of it, and on the
   * fallback path it aliases whatever @p fallback pointed at. The parameter is
   * marked lifetimebound so that passing a temporary - `get_string(std::string(
   * "x"))` - is a compile-time dangling diagnostic rather than a silent
   * use-after-free. Use as_string() or to a copy when you need to own the text.
   */
  [[nodiscard]] string_view
  get_string(NX_LIFETIMEBOUND string_view fallback = {}) const noexcept;

  [[nodiscard]] const nx::string *try_string() const noexcept;
  [[nodiscard]] const array *try_array() const noexcept;
  [[nodiscard]] const object *try_object() const noexcept;
  [[nodiscard]] nx::string *try_string() noexcept;
  [[nodiscard]] array *try_array() noexcept;
  [[nodiscard]] object *try_object() noexcept;

  // Array and object shape

  /// Elements of an array, members of an object, 0 for anything else.
  [[nodiscard]] usize size() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  /// Element @p i of an array. Precondition: is_array() and i < size().
  [[nodiscard]] const value &operator[](usize i) const noexcept;
  [[nodiscard]] value &operator[](usize i) noexcept;

  /// Member @p key of an object, or an immutable null when absent or when this
  /// is not an object. Never inserts.
  [[nodiscard]] const value &operator[](string_view key) const noexcept;

  /// Member @p key, inserting a null one if absent. Precondition: is_object()
  /// or is_null() (a null value becomes an empty object).
  [[nodiscard]] value &operator[](string_view key);

  [[nodiscard]] bool contains(string_view key) const noexcept;
  [[nodiscard]] const value *find(string_view key) const noexcept;
  [[nodiscard]] value *find(string_view key) noexcept;

  /// Reads member @p key into @p out through nx::json::serializer<T>.
  /// @return false when the member is absent or has an incompatible type.
  template <typename T> bool get_to(string_view key, T &out) const;

  // Mutation

  void push_back(value v);
  value &emplace_back(value v = {});
  /// Inserts or overwrites member @p key.
  value &insert(string_view key, value v);
  bool erase(string_view key);
  void reserve(usize n);
  void clear() noexcept;

  /// Deep structural and numeric equality. Numbers compare by mathematical
  /// value across representations; object member order is not significant.
  [[nodiscard]] friend bool operator==(const value &a, const value &b) noexcept;

  /// The shared immutable null returned by failed lookups.
  [[nodiscard]] static const value &null_value() noexcept;

private:
  union storage {
    bool boolean;
    i64 signed_integer;
    u64 unsigned_integer;
    f64 real;
    nx::string string;
    array array_;
    object object_;

    storage() noexcept {}
    ~storage() noexcept {}
  };

  void destroy() noexcept;
  void construct_from(const value &o);
  void construct_from(value &&o) noexcept;

  type type_;
  storage u_;
};

struct member {
  nx::string key;
  value val;
};

/**
 * True when the value holds an integer that T can represent exactly.
 *
 * Reals never fit an integral T, even when their value happens to be whole: a
 * document that said 1.0 asked for a real, and silently narrowing it would make
 * 1.5 decode as 1. Use get_i64()/get_double() to convert deliberately.
 */
template <typename T>
  requires std::is_integral_v<T>
bool value::fits() const noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    return type_ == type::boolean;
  } else if constexpr (std::is_signed_v<T>) {
    if (type_ == type::signed_integer) {
      const i64 v = u_.signed_integer;
      return v >= static_cast<i64>(std::numeric_limits<T>::min()) &&
             v <= static_cast<i64>(std::numeric_limits<T>::max());
    }
    if (type_ == type::unsigned_integer)
      return u_.unsigned_integer <=
             static_cast<u64>(std::numeric_limits<T>::max());
    return false;
  } else {
    if (type_ == type::unsigned_integer)
      return u_.unsigned_integer <=
             static_cast<u64>(std::numeric_limits<T>::max());
    if (type_ == type::signed_integer) {
      const i64 v = u_.signed_integer;
      return v >= 0 &&
             static_cast<u64>(v) <=
                 static_cast<u64>(std::numeric_limits<T>::max());
    }
    return false;
  }
}

// A value must cost no more than its largest alternative plus the tag. Stated
// relative to the alternatives rather than as a byte count because the standard
// containers grow under MSVC's debug iterators, and that is expected.
static_assert(sizeof(value) <= sizeof(object) + sizeof(void *),
              "nx::json::value should be no larger than its largest "
              "alternative plus the discriminator");
static_assert(sizeof(object) == sizeof(array),
              "the array and object alternatives should share a layout");

// Parsing

/// Knobs for parse(). The defaults accept exactly RFC 8259 and nothing more.
struct parse_options {
  /// Maximum nesting of arrays and objects. Guards the parser's recursion
  /// against adversarial input; exceeding it is a clean error, not a crash.
  u32 max_depth = 256;
  /// Accept `,` before a closing `]` or `}` (JSON5-style convenience).
  bool allow_trailing_commas = false;
  /// Accept `//` and `/* */` comments, treating them as whitespace.
  bool allow_comments = false;
  /// Report duplicate_key instead of letting the last occurrence win.
  bool reject_duplicate_keys = false;
  /// Reject input that is not well-formed UTF-8. Off by default because the
  /// parser already rejects everything that would corrupt the document.
  bool validate_utf8 = false;
};

/// Parses a complete JSON document. @p text is not retained.
[[nodiscard]] result<value> parse(string_view text,
                                  const parse_options &options = {});

// Writing

/// Knobs for write(). The default is the compact form.
struct write_options {
  /// Spaces of indentation per level; 0 writes the compact single-line form.
  u32 indent = 0;
  /// Escape every non-ASCII code point as \uXXXX, so the output is pure ASCII.
  bool escape_non_ascii = false;
  /// Escape '/' as '\/' (only ever needed when embedding JSON in HTML).
  bool escape_forward_slash = false;
  /// Digits after the decimal point; 0 uses the shortest round-trip form.
  u32 real_precision = 0;
};

/// Serializes @p v and returns the text.
[[nodiscard]] nx::string write(const value &v,
                               const write_options &options = {});

/// Appends the serialization of @p v to @p out, which is not cleared first.
void write_to(nx::string &out, const value &v,
              const write_options &options = {});

// Custom type registration
//
// Every conversion in this file goes through serializer<T>. There are two ways
// to teach it about a type, and they cost the same at run time because both are
// resolved at compile time:
//
//  1. Specialize the trait:
//
//       template <> struct nx::json::serializer<Vec2> {
//         static void to_json(nx::json::value &out, const Vec2 &v) {
//           out = nx::json::value::make_array();
//           out.push_back(v.x);
//           out.push_back(v.y);
//         }
//         static bool from_json(const nx::json::value &in, Vec2 &out) {
//           if (!in.is_array() || in.size() != 2) return false;
//           return in[0].fits<f32>() && in[1].fits<f32>() &&
//                  (out = {in[0].get_double(), in[1].get_double()}, true);
//         }
//       };
//
//  2. Or write the two functions next to your own type, where ADL finds them:
//
//       void to_json(nx::json::value &out, const Vec2 &v);
//       bool from_json(const nx::json::value &in, Vec2 &out);
//
// Either way to_value<T>, from_value<T>, parse_as<T>, write_as<T> and every
// container of T start working. from_json returns false on a type mismatch;
// the top-level entry points turn that into a proper nx::json::error.

/// Primary template. Unspecialized types fall back to ADL to_json/from_json.
template <typename T, typename Enable = void> struct serializer;

namespace detail {

template <typename T>
concept has_trait_to = requires(value &v, const T &t) {
  serializer<T>::to_json(v, t);
};
template <typename T>
concept has_trait_from = requires(const value &v, T &t) {
  { serializer<T>::from_json(v, t) } -> std::same_as<bool>;
};
template <typename T>
concept has_adl_to = requires(value &v, const T &t) { to_json(v, t); };
template <typename T>
concept has_adl_from = requires(const value &v, T &t) {
  { from_json(v, t) } -> std::same_as<bool>;
};

} // namespace detail

/// True when T can be converted in both directions.
template <typename T>
concept convertible =
    (detail::has_trait_to<T> || detail::has_adl_to<T>)&&(
        detail::has_trait_from<T> || detail::has_adl_from<T>);

/// Encodes @p in into @p out.
template <typename T> void assign(value &out, const T &in) {
  if constexpr (detail::has_trait_to<T>)
    serializer<T>::to_json(out, in);
  else {
    static_assert(detail::has_adl_to<T>,
                  "nx::json: no serializer<T>::to_json and no to_json(value&, "
                  "const T&) findable by ADL for this type");
    to_json(out, in);
  }
}

/// Decodes @p in into @p out. @return false on a type mismatch.
template <typename T> [[nodiscard]] bool extract(const value &in, T &out) {
  if constexpr (detail::has_trait_from<T>)
    return serializer<T>::from_json(in, out);
  else {
    static_assert(detail::has_adl_from<T>,
                  "nx::json: no serializer<T>::from_json and no from_json(const "
                  "value&, T&) findable by ADL for this type");
    return from_json(in, out);
  }
}

/// Builds a value from any registered type.
template <typename T> [[nodiscard]] value to_value(const T &in) {
  value v;
  assign(v, in);
  return v;
}

/// Reads any registered type out of a value.
template <typename T> [[nodiscard]] result<T> from_value(const value &in) {
  T out{};
  if (!extract(in, out))
    return nx::err(error{error_code::type_mismatch, 0, 0, 0});
  return out;
}

/// parse() followed by from_value<T>().
template <typename T>
[[nodiscard]] result<T> parse_as(string_view text,
                                 const parse_options &options = {}) {
  result<value> doc = parse(text, options);
  if (!doc)
    return nx::err(doc.error());
  return from_value<T>(*doc);
}

/// to_value<T>() followed by write().
template <typename T>
[[nodiscard]] nx::string write_as(const T &in,
                                  const write_options &options = {}) {
  return write(to_value(in), options);
}

template <typename T> bool value::get_to(string_view key, T &out) const {
  const value *m = find(key);
  return m != nullptr && extract(*m, out);
}

// Built-in serializers

template <> struct serializer<value> {
  static void to_json(value &out, const value &in) { out = in; }
  static bool from_json(const value &in, value &out) {
    out = in;
    return true;
  }
};

template <> struct serializer<bool> {
  static void to_json(value &out, bool in) { out = in; }
  static bool from_json(const value &in, bool &out) {
    if (!in.is_bool())
      return false;
    out = in.as_bool();
    return true;
  }
};

/// Integral types. Decoding rejects anything that does not fit exactly, so a
/// negative literal never silently becomes a huge unsigned value and a 64-bit
/// identifier never silently truncates to 32 bits.
template <typename T>
struct serializer<T, std::enable_if_t<std::is_integral_v<T> &&
                                      !std::is_same_v<T, bool>>> {
  static void to_json(value &out, T in) { out = in; }
  static bool from_json(const value &in, T &out) {
    if (!in.fits<T>())
      return false;
    if constexpr (std::is_signed_v<T>)
      out = static_cast<T>(in.get_i64());
    else
      out = static_cast<T>(in.get_u64());
    return true;
  }
};

/// Floating-point types. Any JSON number decodes, including integers.
template <typename T>
struct serializer<T, std::enable_if_t<std::is_floating_point_v<T>>> {
  static void to_json(value &out, T in) { out = in; }
  static bool from_json(const value &in, T &out) {
    if (!in.is_number())
      return false;
    out = static_cast<T>(in.get_double());
    return true;
  }
};

template <> struct serializer<nx::string> {
  static void to_json(value &out, const nx::string &in) { out = in; }
  static bool from_json(const value &in, nx::string &out) {
    if (!in.is_string())
      return false;
    out = in.as_string();
    return true;
  }
};

template <> struct serializer<std::string> {
  static void to_json(value &out, const std::string &in) {
    out = string_view(in);
  }
  static bool from_json(const value &in, std::string &out) {
    if (!in.is_string())
      return false;
    const string_view sv = in.get_string();
    out.assign(sv.data(), sv.size());
    return true;
  }
};

/// Write-only: a view cannot own the decoded bytes.
template <> struct serializer<string_view> {
  static void to_json(value &out, string_view in) { out = in; }
};

template <typename T> struct serializer<std::optional<T>> {
  static void to_json(value &out, const std::optional<T> &in) {
    if (in.has_value())
      assign(out, *in);
    else
      out = value{};
  }
  static bool from_json(const value &in, std::optional<T> &out) {
    if (in.is_null()) {
      out.reset();
      return true;
    }
    T tmp{};
    if (!extract(in, tmp))
      return false;
    out = std::move(tmp);
    return true;
  }
};

namespace detail {

/// Shared implementation for any sequence that supports clear/push_back.
template <typename Container, typename T> struct sequence_serializer {
  static void to_json(value &out, const Container &in) {
    out = value::make_array();
    out.reserve(in.size());
    for (const T &e : in)
      out.push_back(to_value(e));
  }
  static bool from_json(const value &in, Container &out) {
    const array *a = in.try_array();
    if (!a)
      return false;
    out.clear();
    for (const value &e : *a) {
      T tmp{};
      if (!extract(e, tmp))
        return false;
      out.push_back(std::move(tmp));
    }
    return true;
  }
};

/// Shared implementation for any string-keyed map.
template <typename Map, typename T> struct map_serializer {
  static void to_json(value &out, const Map &in) {
    out = value::make_object();
    out.reserve(in.size());
    for (const auto &kv : in)
      out.insert(string_view(kv.first), to_value(kv.second));
  }
  static bool from_json(const value &in, Map &out) {
    const object *o = in.try_object();
    if (!o)
      return false;
    out.clear();
    for (const member &m : *o) {
      T tmp{};
      if (!extract(m.val, tmp))
        return false;
      out.insert_or_assign(typename Map::key_type(m.key.view()),
                           std::move(tmp));
    }
    return true;
  }
};

} // namespace detail

template <typename T, typename A>
struct serializer<vector<T, A>> : detail::sequence_serializer<vector<T, A>, T> {
};

template <typename T, usize N, typename A>
struct serializer<small_vector<T, N, A>>
    : detail::sequence_serializer<small_vector<T, N, A>, T> {};

/// String-keyed maps become JSON objects. Both of the engine's map types are
/// supported; any other map works the same way through a one-line derivation
/// from detail::map_serializer.
template <typename T, typename C, typename A, typename KC, typename MC>
struct serializer<flat_map<nx::string, T, C, A, KC, MC>>
    : detail::map_serializer<flat_map<nx::string, T, C, A, KC, MC>, T> {};

template <typename T, typename H, typename E, typename A>
struct serializer<flat_hash_map<nx::string, T, H, E, A>>
    : detail::map_serializer<flat_hash_map<nx::string, T, H, E, A>, T> {};

/// Fixed-size arrays map to JSON arrays of exactly that length.
template <typename T, usize N> struct serializer<T[N]> {
  static void to_json(value &out, const T (&in)[N]) {
    out = value::make_array();
    out.reserve(N);
    for (usize i = 0; i < N; ++i)
      out.push_back(to_value(in[i]));
  }
  static bool from_json(const value &in, T (&out)[N]) {
    const array *a = in.try_array();
    if (!a || a->size() != N)
      return false;
    for (usize i = 0; i < N; ++i)
      if (!extract((*a)[i], out[i]))
        return false;
    return true;
  }
};

} // namespace nx::json
