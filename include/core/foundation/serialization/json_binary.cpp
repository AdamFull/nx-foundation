#include "core/foundation/serialization/json_binary.h"

#include "core/foundation/serialization/json_member_index.h"

#include <cstring>

namespace nx::json {
namespace {

// What the text parser accepts by default, so a cooked document reads back.
inline constexpr u32 MAX_READ_DEPTH = parse_options{}.max_depth;

[[nodiscard]] value read_binary_impl(nx::nva::Reader &r, u32 depth);

} // namespace

void write_binary(const value &v, nx::nva::Writer &w) {
  w.u8v(nx::cast<u8>(v.kind()));
  switch (v.kind()) {
  case type::null:
    break;
  case type::boolean:
    w.u8v(v.as_bool() ? 1u : 0u);
    break;
  case type::signed_integer:
    w.u64v(nx::cast<u64>(v.as_i64()));
    break;
  case type::unsigned_integer:
    w.u64v(v.as_u64());
    break;
  case type::real: {
    const f64 d = v.as_double();
    u64 bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    w.u64v(bits);
    break;
  }
  case type::string:
    w.str(v.as_string().view());
    break;
  case type::array: {
    const array *const a = v.try_array();
    const u32 n = a != nullptr ? nx::cast<u32>(a->size()) : 0u;
    w.u32v(n);
    for (u32 i = 0; i < n; ++i)
      write_binary((*a)[i], w);
    break;
  }
  case type::object: {
    const object *const o = v.try_object();
    const u32 n = o != nullptr ? nx::cast<u32>(o->size()) : 0u;
    w.u32v(n);
    for (u32 i = 0; i < n; ++i) {
      w.str((*o)[i].key.view());
      write_binary((*o)[i].val, w);
    }
    break;
  }
  }
}

namespace {

value read_binary_impl(nx::nva::Reader &r, const u32 depth) {
  if (depth > MAX_READ_DEPTH) {
    r.fail();
    return value();
  }
  const auto tag = r.u8v();
  if (!r.ok())
    return value();

  switch (nx::cast<type>(tag)) {
  case type::null:
    return value();
  case type::boolean:
    return value(r.u8v() != 0u);
  case type::signed_integer:
    return value(nx::cast<i64>(r.u64v()));
  case type::unsigned_integer:
    return value(r.u64v());
  case type::real: {
    const u64 bits = r.u64v();
    f64 d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return value(d);
  }
  case type::string:
    return value(r.str());
  case type::array: {
    const u32 n = r.u32v();
    if (!r.require_items(n, 1))
      return value();
    value a = value::make_array();
    for (u32 i = 0; i < n && r.ok(); ++i)
      a.push_back(read_binary_impl(r, depth + 1));
    return a;
  }
  case type::object: {
    const u32 n = r.u32v();
    // Every member has at least a four-byte key length and a one-byte value
    // tag, even when both are empty/null.
    if (!r.require_items(n, 5))
      return value();
    value o = value::make_object();
    object &members = o.as_object();
    detail::member_index index;
    for (u32 i = 0; i < n && r.ok(); ++i) {
      const nx::string_view key = r.str();
      value member_value = read_binary_impl(r, depth + 1);
      if (!r.ok())
        break;
      if (value *existing = index.find(members, key)) {
        *existing = std::move(member_value);
        continue;
      }
      members.push_back(member{nx::string(key), std::move(member_value)});
      index.added(members);
    }
    return o;
  }
  }

  r.fail();
  return value();
}

} // namespace

value read_binary(nx::nva::Reader &r) { return read_binary_impl(r, 0); }

} // namespace nx::json
