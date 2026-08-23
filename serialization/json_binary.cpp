#include "core/foundation/serialization/json_binary.h"

#include <cstring>

namespace nx::json {

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

value read_binary(nx::nva::Reader &r) {
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
    value a = value::make_array();
    // No reserve on the raw count: a corrupt length would ask for a huge one.
    for (u32 i = 0; i < n && r.ok(); ++i)
      a.push_back(read_binary(r));
    return a;
  }
  case type::object: {
    const u32 n = r.u32v();
    value o = value::make_object();
    for (u32 i = 0; i < n && r.ok(); ++i) {
      const nx::string_view key = r.str();
      value member_value = read_binary(r);
      if (!r.ok())
        break;
      (void)o.insert(key, std::move(member_value));
    }
    return o;
  }
  }

  // An unknown tag: the stream is not one we wrote. Run it dry so the caller
  // sees a failed read rather than a null that looks like a real value.
  while (r.ok())
    r.u8v();
  return value();
}

} // namespace nx::json
