#pragma once

#include "core/foundation/containers/flat_map.h"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

#include <limits>
#include <optional>
#include <type_traits>

namespace nx::json {

enum class error_code : u32 {
  none = 0,
  unexpected_end,
  unexpected_token,    ///< A character that cannot start or continue a value.
  invalid_number,
  invalid_string,
  invalid_escape,
  invalid_utf8,        ///< Input was not well-formed UTF-8.
  invalid_surrogate,   ///< Unpaired or misordered UTF-16 surrogate.
  depth_exceeded,
  trailing_content,    ///< Non-whitespace bytes after the top-level value.
  duplicate_key,
  number_out_of_range,
  type_mismatch,
  missing_field,
  count,
};

[[nodiscard]] string_view to_string(error_code e) noexcept;

/// A parse or conversion failure. `offset` is a byte index into the input;
/// `line` and `column` are 1-based and in bytes, computed only when an error
/// is produced, so the success path pays nothing for them.
struct error {
  error_code code = error_code::none;
  usize offset = 0;
  u32 line = 0;
  u32 column = 0;

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

[[nodiscard]] constexpr bool is_number(type t) noexcept {
  return t == type::signed_integer || t == type::unsigned_integer ||
         t == type::real;
}

class value;

struct member;

using array = vector<value>;
using object = vector<member>;

/// Accessors come in three flavours: - `as_*()` - precondition-checked; asserts in debug, undefined
/// otherwise. - `get_*()` - returns a fallback when the type does not match. - `try_*()` - returns
/// a pointer, null when the type does not match.
class value {
public:
  value() noexcept : m_type(type::null), m_u{} {}
  value(std::nullptr_t) noexcept : value() {}

  value(bool b) noexcept : m_type(type::boolean) { m_u.boolean = b; }

  template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
  value(T v) noexcept {
    if constexpr (std::is_signed_v<T>) {
      m_type = type::signed_integer;
      m_u.signed_integer = static_cast<i64>(v);
    } else {
      m_type = type::unsigned_integer;
      m_u.unsigned_integer = static_cast<u64>(v);
    }
  }

  template <typename T>
    requires std::is_floating_point_v<T>
  value(T v) noexcept : m_type(type::real) {
    m_u.real = static_cast<f64>(v);
  }

  value(const char *s) : value(string_view(s)) {}
  value(string_view s);
  value(const nx::string &s);
  value(nx::string &&s) noexcept;

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

  [[nodiscard]] type kind() const noexcept { return m_type; }
  [[nodiscard]] bool is_null() const noexcept { return m_type == type::null; }
  [[nodiscard]] bool is_bool() const noexcept {
    return m_type == type::boolean;
  }
  [[nodiscard]] bool is_number() const noexcept {
    return json::is_number(m_type);
  }
  [[nodiscard]] bool is_i64() const noexcept {
    return m_type == type::signed_integer;
  }
  [[nodiscard]] bool is_u64() const noexcept {
    return m_type == type::unsigned_integer;
  }
  [[nodiscard]] bool is_double() const noexcept { return m_type == type::real; }
  [[nodiscard]] bool is_string() const noexcept {
    return m_type == type::string;
  }
  [[nodiscard]] bool is_array() const noexcept { return m_type == type::array; }
  [[nodiscard]] bool is_object() const noexcept {
    return m_type == type::object;
  }

  template <typename T>
    requires std::is_integral_v<T>
  [[nodiscard]] bool fits() const noexcept;

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

  [[nodiscard]] bool get_bool(bool fallback = false) const noexcept;
  [[nodiscard]] i64 get_i64(i64 fallback = 0) const noexcept;
  [[nodiscard]] u64 get_u64(u64 fallback = 0) const noexcept;
  [[nodiscard]] f64 get_double(f64 fallback = 0.0) const noexcept;

  /// The result borrows - from this value's storage on the matching path, from @p fallback
  /// otherwise - so any mutation invalidates it. @p fallback is lifetimebound, making a temporary a
  /// compile-time diagnostic rather than a use-after-free.
  [[nodiscard]] string_view
  get_string(NX_LIFETIMEBOUND string_view fallback = {}) const noexcept;

  [[nodiscard]] const nx::string *try_string() const noexcept;
  [[nodiscard]] const array *try_array() const noexcept;
  [[nodiscard]] const object *try_object() const noexcept;
  [[nodiscard]] nx::string *try_string() noexcept;
  [[nodiscard]] array *try_array() noexcept;
  [[nodiscard]] object *try_object() noexcept;

  [[nodiscard]] usize size() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  [[nodiscard]] const value &operator[](usize i) const noexcept;
  [[nodiscard]] value &operator[](usize i) noexcept;

  [[nodiscard]] const value &operator[](string_view key) const noexcept;

  [[nodiscard]] value &operator[](string_view key);

  [[nodiscard]] bool contains(string_view key) const noexcept;
  [[nodiscard]] const value *find(string_view key) const noexcept;
  [[nodiscard]] value *find(string_view key) noexcept;

  template <typename T> bool get_to(string_view key, T &out) const;

  void push_back(value v);
  value &emplace_back(value v = {});
  value &insert(string_view key, value v);
  bool erase(string_view key);
  void reserve(usize n);
  void clear() noexcept;

  friend bool operator==(const value &a, const value &b) noexcept;

  [[nodiscard]] static const value &null_value() noexcept;

private:
  union storage {
    bool boolean;
    i64 signed_integer;
    u64 unsigned_integer;
    f64 real;
    nx::string string;
    array m_array;
    object m_object;

    storage() noexcept {}
    ~storage() noexcept {}
  };

  void destroy() noexcept;
  void construct_from(const value &o);
  void construct_from(value &&o) noexcept;

  type m_type;
  storage m_u;
};

struct member {
  nx::string key;
  value val;
};

template <typename T>
  requires std::is_integral_v<T>
bool value::fits() const noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    return m_type == type::boolean;
  } else if constexpr (std::is_signed_v<T>) {
    if (m_type == type::signed_integer) {
      const i64 v = m_u.signed_integer;
      return v >= static_cast<i64>(std::numeric_limits<T>::min()) &&
             v <= static_cast<i64>(std::numeric_limits<T>::max());
    }
    if (m_type == type::unsigned_integer)
      return m_u.unsigned_integer <=
             static_cast<u64>(std::numeric_limits<T>::max());
    return false;
  } else {
    if (m_type == type::unsigned_integer)
      return m_u.unsigned_integer <=
             static_cast<u64>(std::numeric_limits<T>::max());
    if (m_type == type::signed_integer) {
      const i64 v = m_u.signed_integer;
      return v >= 0 && static_cast<u64>(v) <=
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

struct parse_options {
  u32 max_depth = 256;
  bool allow_trailing_commas = false;
  bool allow_comments = false;
  bool reject_duplicate_keys = false;
  /// Reject input that is not well-formed UTF-8. Off by default because the
  /// parser already rejects everything that would corrupt the document.
  bool validate_utf8 = false;
};

[[nodiscard]] result<value> parse(string_view text,
                                  const parse_options &options = {});

struct write_options {
  u32 indent = 0;
  bool escape_non_ascii = false;
  bool escape_forward_slash = false;
  /// Digits after the decimal point; 0 uses the shortest round-trip form.
  u32 real_precision = 0;
};

[[nodiscard]] nx::string write(const value &v,
                               const write_options &options = {});

void write_to(nx::string &out, const value &v,
              const write_options &options = {});

template <typename T, typename Enable = void> struct serializer;

namespace detail {

template <typename T>
concept has_trait_to =
    requires(value &v, const T &t) { serializer<T>::to_json(v, t); };
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

}

template <typename T>
concept convertible = (detail::has_trait_to<T> || detail::has_adl_to<T>) &&
                      (detail::has_trait_from<T> || detail::has_adl_from<T>);

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

template <typename T> [[nodiscard]] bool extract(const value &in, T &out) {
  if constexpr (detail::has_trait_from<T>)
    return serializer<T>::from_json(in, out);
  else {
    static_assert(
        detail::has_adl_from<T>,
        "nx::json: no serializer<T>::from_json and no from_json(const "
        "value&, T&) findable by ADL for this type");
    return from_json(in, out);
  }
}

template <typename T> [[nodiscard]] value to_value(const T &in) {
  value v;
  assign(v, in);
  return v;
}

template <typename T> [[nodiscard]] result<T> from_value(const value &in) {
  T out{};
  if (!extract(in, out))
    return nx::err(error{error_code::type_mismatch, 0, 0, 0});
  return out;
}

template <typename T>
[[nodiscard]] result<T> parse_as(string_view text,
                                 const parse_options &options = {}) {
  result<value> doc = parse(text, options);
  if (!doc)
    return nx::err(doc.error());
  return from_value<T>(*doc);
}

template <typename T>
[[nodiscard]] nx::string write_as(const T &in,
                                  const write_options &options = {}) {
  return write(to_value(in), options);
}

template <typename T> bool value::get_to(string_view key, T &out) const {
  const value *m = find(key);
  return m != nullptr && extract(*m, out);
}

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
struct serializer<
    T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
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

}

template <typename T, typename A>
struct serializer<vector<T, A>> : detail::sequence_serializer<vector<T, A>, T> {
};

template <typename T, usize N, typename A>
struct serializer<small_vector<T, N, A>>
    : detail::sequence_serializer<small_vector<T, N, A>, T> {};

template <typename T, typename C, typename A, typename KC, typename MC>
struct serializer<flat_map<nx::string, T, C, A, KC, MC>>
    : detail::map_serializer<flat_map<nx::string, T, C, A, KC, MC>, T> {};

template <typename T, typename H, typename E, typename A>
struct serializer<flat_hash_map<nx::string, T, H, E, A>>
    : detail::map_serializer<flat_hash_map<nx::string, T, H, E, A>, T> {};

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

}
