#include "framework/nxtest.h"

#include "core/foundation/strings/base64.h"

namespace {

[[nodiscard]] nx::string encoded(const nx::string_view text) {
  return nx::base64_encode(
      {reinterpret_cast<const u8 *>(text.data()), text.size()});
}

} // namespace

TEST_CASE("base64: the RFC 4648 vectors") {
  CHECK(encoded("") == "");
  CHECK(encoded("f") == "Zg==");
  CHECK(encoded("fo") == "Zm8=");
  CHECK(encoded("foo") == "Zm9v");
  CHECK(encoded("foob") == "Zm9vYg==");
  CHECK(encoded("fooba") == "Zm9vYmE=");
  CHECK(encoded("foobar") == "Zm9vYmFy");
  const u8 high[] = {0xff, 0xfe, 0xfd};
  CHECK(nx::base64_encode(high) == "//79");
}

TEST_CASE("base64: the URL-safe alphabet, unpadded") {
  const auto url = [](const nx::string_view text) {
    return nx::base64url_encode(
        {reinterpret_cast<const u8 *>(text.data()), text.size()});
  };
  CHECK(url("") == "");
  CHECK(url("f") == "Zg");
  CHECK(url("fo") == "Zm8");
  CHECK(url("foo") == "Zm9v");
  CHECK(url("foob") == "Zm9vYg");
  CHECK(url("foobar") == "Zm9vYmFy");
  const u8 high[] = {0xfb, 0xff};
  CHECK(nx::base64url_encode(high) == "-_8");
  CHECK(nx::base64_encode(high) == "+/8=");
}
