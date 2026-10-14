#include "framework/nxtest.h"

#include "core/foundation/containers/sync_slot_map.h"
#include "core/foundation/core/foundation.h"

namespace {

using handle_t = nx::handle32;
using map_t = nx::sync_slot_map<int, handle_t>;

} // namespace

TEST_CASE("sync_slot_map: an entry stays where it was while others are "
          "inserted and erased") {
  map_t m;
  const handle_t kept = m.insert(7);
  const int *const at = m.try_get(kept);
  REQUIRE(at != nullptr);

  nx::vector<handle_t> others;
  for (int i = 0; i < 1000; ++i)
    others.push_back(m.insert(int{i}));
  CHECK(m.try_get(kept) == at);
  CHECK(*at == 7);

  for (const handle_t h : others)
    CHECK(m.erase(h));
  CHECK(m.try_get(kept) == at);
  CHECK(*at == 7);
}

TEST_CASE("sync_slot_map: an erased handle reports absent and visits none") {
  map_t m;
  const handle_t a = m.insert(1);
  const handle_t b = m.insert(2);
  CHECK(m.erase(a));
  CHECK_FALSE(m.erase(a));
  CHECK_FALSE(m.contains(a));
  CHECK(m.try_get(a) == nullptr);
  REQUIRE(m.try_get(b) != nullptr);
  CHECK(*m.try_get(b) == 2);

  int visited = 0;
  m.for_each([&](int &value) { visited += value; });
  CHECK(visited == 2);
  m.clear();
  CHECK_FALSE(m.contains(b));
}
