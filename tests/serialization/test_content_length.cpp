#include "framework/nxtest.h"

#include "core/foundation/serialization/content_length.h"
#include "core/foundation/strings/utf8_string.h"

TEST_CASE("content length: a complete bounded frame identifies its body") {
  const nx::string_view bytes =
      "Content-Type: application/json\r\nContent-Length:\t2 \r\n\r\n{}tail";
  const nx::ContentLengthFrame frame = nx::content_length_frame(bytes, 1024);

  REQUIRE(frame.status == nx::ContentLengthStatus::Ready);
  CHECK(bytes.substr(frame.body_offset, frame.body_size) == "{}");
  CHECK(bytes.substr(frame.consumed) == "tail");
}

TEST_CASE("content length: an incomplete header and body remain incomplete") {
  CHECK(nx::content_length_frame("Content-Len", 1024).status ==
        nx::ContentLengthStatus::Incomplete);
  CHECK(nx::content_length_frame("Content-Length: 4\r\n\r\n{}", 1024).status ==
        nx::ContentLengthStatus::Incomplete);
}

TEST_CASE("content length: malformed and ambiguous lengths are rejected") {
  CHECK(nx::content_length_frame("Other: 2\r\n\r\n{}", 1024).status ==
        nx::ContentLengthStatus::Invalid);
  CHECK(nx::content_length_frame(
            "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", 1024)
            .status == nx::ContentLengthStatus::Invalid);
  CHECK(nx::content_length_frame("Content-Length: 1 0\r\n\r\n", 1024).status ==
        nx::ContentLengthStatus::Invalid);
  CHECK(nx::content_length_frame(
            "Content-Length: 184467440737095516160\r\n\r\n", 1024)
            .status == nx::ContentLengthStatus::Invalid);
}

TEST_CASE("content length: an unterminated header has a fixed limit") {
  nx::string bytes(33, 'x');
  CHECK(nx::content_length_frame(bytes.view(), 1024, 32).status ==
        nx::ContentLengthStatus::Invalid);
}
