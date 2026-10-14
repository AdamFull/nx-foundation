#include "framework/nxtest.h"

#include "core/foundation/core/sha256.h"
#include "core/foundation/strings/utf8_string.h"

namespace {

[[nodiscard]] nx::string hex(const std::array<u8, nx::SHA256_BYTES> &digest) {
  static constexpr char DIGITS[] = "0123456789abcdef";
  nx::string out;
  for (const u8 byte : digest) {
    out.push_back(DIGITS[byte >> 4]);
    out.push_back(DIGITS[byte & 15]);
  }
  return out;
}

[[nodiscard]] nx::string digest_of(const nx::string_view text) {
  return hex(
      nx::sha256({reinterpret_cast<const u8 *>(text.data()), text.size()}));
}

} // namespace

TEST_CASE("sha256: the FIPS 180-4 examples") {
  CHECK(digest_of("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(digest_of("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the length no longer fits the block, so padding takes two.
  CHECK(digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(digest_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijk"
                  "lmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("sha256: whole blocks, and the edges of the last one") {
  const nx::string million(1'000'000, 'a');
  CHECK(digest_of(million.view()) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // 55 bytes is the most one padded block holds; 64 is a block exactly.
  CHECK(digest_of(nx::string(55, 'a').view()) ==
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  CHECK(digest_of(nx::string(64, 'a').view()) ==
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
