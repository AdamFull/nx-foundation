#include "framework/nxtest.h"

#include "core/foundation/core/handle.h"

#include <set>
#include <unordered_map>

namespace {
struct TextureTag {};
struct BufferTag {};

using h32 = nx::handle32;
using h64 = nx::handle64;
}

TEST_CASE("handle: bit layout constants follow from Repr and IndexBits") {
  CHECK(h32::index_bits == 24);
  CHECK(h32::version_bits == 8);
  CHECK(h32::index_mask == 0x00ffffffu);
  CHECK(h32::version_mask == 0xffu);
  CHECK(h32::null_index == 0x00ffffffu);
  CHECK(h32::max_index == 0x00fffffeu);
  CHECK(h32::max_version == 0xffu);

  CHECK(h64::index_bits == 32);
  CHECK(h64::version_bits == 32);
  CHECK(h64::index_mask == 0xffffffffull);
  CHECK(h64::version_mask == 0xffffffffull);
  CHECK(h64::max_index == 0xfffffffeull);

  static_assert(h32::index_bits + h32::version_bits == 32);
  static_assert(h64::index_bits + h64::version_bits == 64);

  using narrow = nx::basic_handle<u32, 1>;
  CHECK(narrow::index_bits == 1);
  CHECK(narrow::version_bits == 31);
  CHECK(narrow::index_mask == 1u);
  CHECK(narrow::max_index == 0u);

  using wide = nx::basic_handle<u16, 15>;
  CHECK(wide::version_bits == 1);
  CHECK(wide::index_mask == 0x7fffu);
  CHECK(wide::version_mask == 1u);
}

TEST_CASE("handle: the default-constructed handle is null and invalid") {
  constexpr h32 d;
  static_assert(!d.valid());
  CHECK_FALSE(d.valid());
  CHECK_FALSE(static_cast<bool>(d));
  CHECK(d.index() == h32::null_index);
  CHECK(d.raw() == 0xffffffffu);

  CHECK(h32::null() == d);
  CHECK_FALSE(h32::null().valid());
  CHECK_FALSE(h64::null().valid());
  CHECK(h64::null().raw() == 0xffffffffffffffffull);
}

TEST_CASE("handle: from_parts round-trips index and version") {
  const h32 h = h32::from_parts(1234, 7);
  CHECK(h.index() == 1234u);
  CHECK(h.version() == 7u);
  CHECK(h.valid());
  CHECK(static_cast<bool>(h));

  const h32 lo = h32::from_parts(0, 0);
  CHECK(lo.index() == 0u);
  CHECK(lo.version() == 0u);
  CHECK(lo.valid());
  CHECK(lo.raw() == 0u);

  const h32 hi = h32::from_parts(h32::max_index, h32::max_version);
  CHECK(hi.index() == h32::max_index);
  CHECK(hi.version() == h32::max_version);
  CHECK(hi.valid());

  for (u32 idx : {0u, 1u, 255u, 65535u, h32::max_index}) {
    for (u32 ver = 0; ver <= h32::max_version; ++ver) {
      const h32 x = h32::from_parts(idx, ver);
      CHECK(x.index() == idx);
      CHECK(x.version() == ver);
    }
  }

  const h64 w = h64::from_parts(0xdeadbeefu, 0x12345678u);
  CHECK(w.index() == 0xdeadbeefull);
  CHECK(w.version() == 0x12345678ull);
}

TEST_CASE("handle: from_parts masks out-of-range index and version") {
  const h32 h = h32::from_parts(0xffffffffu, 0xffffffffu);
  CHECK(h.index() == h32::index_mask);
  CHECK(h.version() == h32::max_version);
  CHECK(h.raw() == 0xffffffffu);
  CHECK_FALSE(h.valid());

  const h32 g = h32::from_parts(5, 0x1ff);
  CHECK(g.index() == 5u);
  CHECK(g.version() == 0xffu);

  const h32 f = h32::from_parts(0x0100'0005u, 3);
  CHECK(f.index() == 5u);
  CHECK(f.version() == 3u);
}

TEST_CASE("handle: from_raw and raw are inverses") {
  for (u32 v : {0u, 1u, 0x00ffffffu, 0x12345678u, 0xfffffffeu, 0xffffffffu}) {
    CHECK(h32::from_raw(v).raw() == v);
  }
  const h32 h = h32::from_parts(99, 3);
  CHECK(h32::from_raw(h.raw()) == h);

  CHECK(h.raw() == ((3u << 24) | 99u));
}

TEST_CASE("handle: validity depends on the index only, not the version") {
  for (u32 ver = 0; ver <= h32::max_version; ++ver)
    CHECK_FALSE(h32::from_parts(h32::null_index, ver).valid());

  CHECK(h32::from_parts(h32::max_index, 0).valid());
  CHECK(h32::from_parts(h32::max_index, h32::max_version).valid());

  CHECK(h32::from_raw(0).valid());
}

TEST_CASE("handle: invalidate resets to the canonical null value") {
  h32 h = h32::from_parts(10, 2);
  REQUIRE(h.valid());
  h.invalidate();
  CHECK_FALSE(h.valid());
  CHECK(h == h32::null());
  CHECK(h.raw() == 0xffffffffu);

  h.invalidate();
  CHECK(h == h32::null());
}

TEST_CASE("handle: equality and ordering compare the packed value") {
  const h32 a = h32::from_parts(1, 0);
  const h32 b = h32::from_parts(1, 0);
  const h32 c = h32::from_parts(1, 1);
  const h32 d = h32::from_parts(2, 0);

  CHECK(a == b);
  CHECK_FALSE(a != b);
  CHECK(a != c);
  CHECK(a != d);

  CHECK(a < c);
  CHECK(a < d);
  CHECK(d < c);
  CHECK(a <= b);
  CHECK(c > a);
  CHECK(c >= a);

  static_assert(h32::from_parts(1, 0) == h32::from_parts(1, 0));
  static_assert(h32::from_parts(1, 0) != h32::from_parts(1, 1));

  CHECK(h32::from_parts(h32::max_index, h32::max_version) < h32::null());
}

TEST_CASE(
    "handle: tagged handles are distinct types with identical behaviour") {
  using TexHandle = nx::typed_handle32<TextureTag>;
  using BufHandle = nx::typed_handle32<BufferTag>;

  static_assert(!std::is_same_v<TexHandle, BufHandle>);
  static_assert(!std::is_convertible_v<TexHandle, BufHandle>);
  static_assert(std::is_same_v<TexHandle::tag_type, TextureTag>);
  static_assert(std::is_same_v<h32::tag_type, void>);
  static_assert(std::is_same_v<h32::repr_type, u32>);

  const TexHandle t = TexHandle::from_parts(3, 1);
  const BufHandle b = BufHandle::from_parts(3, 1);
  CHECK(t.raw() == b.raw());
  CHECK(t.index() == 3u);
  CHECK(b.index() == 3u);

  using TexHandle64 = nx::typed_handle64<TextureTag>;
  CHECK(TexHandle64::from_parts(1, 2).version() == 2ull);
}

TEST_CASE(
    "handle: std::hash distinguishes handles and works in unordered_map") {
  const std::hash<h32> hasher;
  CHECK(hasher(h32::from_parts(1, 0)) == hasher(h32::from_parts(1, 0)));
  CHECK(hasher(h32::from_parts(1, 0)) != hasher(h32::from_parts(1, 1)));
  CHECK(hasher(h32::from_parts(1, 0)) != hasher(h32::from_parts(2, 0)));

  std::set<std::size_t> seen;
  for (u32 i = 0; i < 500; ++i)
    seen.insert(hasher(h32::from_parts(i, i % 256)));
  CHECK(seen.size() == 500);

  std::unordered_map<h32, int> m;
  for (u32 i = 0; i < 200; ++i)
    m[h32::from_parts(i, 1)] = static_cast<int>(i);
  CHECK(m.size() == 200);
  CHECK(m[h32::from_parts(37, 1)] == 37);
  CHECK(m.find(h32::from_parts(37, 2)) ==
        m.end());

  const std::hash<h64> hasher64;
  CHECK(hasher64(h64::from_parts(1, 2)) == hasher64(h64::from_parts(1, 2)));
  CHECK(hasher64(h64::from_parts(1, 2)) != hasher64(h64::from_parts(2, 1)));
}

TEST_CASE("handle: is a trivial, pointer-sized-or-smaller value type") {
  static_assert(std::is_trivially_copyable_v<h32>);
  static_assert(std::is_trivially_destructible_v<h32>);
  static_assert(sizeof(h32) == sizeof(u32));
  static_assert(sizeof(h64) == sizeof(u64));
  static_assert(sizeof(nx::typed_handle32<TextureTag>) == sizeof(u32));
  CHECK(sizeof(h32) == 4);
}

TEST_CASE("handle: version wraps within its field, as slot reuse requires") {
  const u32 idx = 42;
  h32 h = h32::from_parts(idx, h32::max_version);
  CHECK(h.version() == h32::max_version);

  h = h32::from_parts(idx, static_cast<u32>(h.version() + 1));
  CHECK(h.version() == 0u);
  CHECK(h.index() == idx);
}
