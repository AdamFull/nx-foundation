#include "framework/nxtest.h"

#include "core/foundation/containers/owned_blob_slice.h"

TEST_CASE("owned blob slice: a whole blob is exposed without copying") {
  nx::blob<u8> storage{1, 2, 3, 4};
  const u8 *const original = storage.data();

  nx::owned_blob_slice<u8> bytes(std::move(storage));
  REQUIRE(bytes.size() == 4u);
  CHECK(bytes.data() == original);
  CHECK(bytes.view()[2] == 3u);
}

TEST_CASE("owned blob slice: a subrange keeps the container allocation") {
  nx::blob<u8> storage{10, 20, 30, 40, 50};
  const u8 *const expected = storage.data() + 1;

  nx::owned_blob_slice<u8> bytes(std::move(storage), 1, 3);
  REQUIRE(bytes.size() == 3u);
  CHECK(bytes.data() == expected);
  CHECK(bytes.view()[0] == 20u);
  CHECK(bytes.view()[2] == 40u);
}

TEST_CASE("owned blob slice: invalid and empty ranges expose nothing") {
  nx::owned_blob_slice<u8> empty;
  CHECK(empty.empty());
  CHECK(empty.data() == nullptr);

  nx::owned_blob_slice<u8> past(nx::blob<u8>{1, 2, 3}, 4, 1);
  CHECK(past.empty());

  nx::owned_blob_slice<u8> overflow(nx::blob<u8>{1, 2, 3}, 2, 2);
  CHECK(overflow.empty());

  nx::owned_blob_slice<u8> zero(nx::blob<u8>{1, 2, 3}, 1, 0);
  CHECK(zero.empty());
}

TEST_CASE("owned blob slice: moving transfers its aliased storage") {
  nx::owned_blob_slice<u8> source(nx::blob<u8>{7, 8, 9}, 1, 2);
  const u8 *const aliased = source.data();

  nx::owned_blob_slice<u8> moved(std::move(source));
  REQUIRE(moved.size() == 2u);
  CHECK(moved.data() == aliased);
  CHECK(moved.view()[1] == 9u);
  CHECK(source.empty());
}
