#include "core/foundation/serialization/json_document.h"

#include "core/foundation/serialization/asset_policy.h"
#include "core/foundation/serialization/json_binary.h"
#include "core/foundation/serialization/nva.h"
#include "core/foundation/vfs/vfs.h"

namespace nx::json {
namespace {

constexpr u32 FORMAT = nx::nva::fourcc('N', 'X', 'J', 'D');
constexpr u32 CHUNK_DATA = nx::nva::fourcc('D', 'A', 'T', 'A');
constexpr u16 VERSION = 1;

[[nodiscard]] bool authoring_member(const nx::string_view key) noexcept {
  return (!key.empty() && key.front() == '_') || key == "$comment";
}

} // namespace

void strip_asset_comments(value &root) {
  if (root.is_array()) {
    for (value &element : root.as_array())
      strip_asset_comments(element);
    return;
  }
  if (!root.is_object())
    return;

  object &members = root.as_object();
  usize kept = 0;
  for (usize i = 0; i < members.size(); ++i) {
    if (authoring_member(members[i].key.view()))
      continue;
    strip_asset_comments(members[i].val);
    if (kept != i)
      members[kept] = std::move(members[i]);
    ++kept;
  }
  members.resize(kept);
}

result<value> parse_asset_document(const nx::string_view text) {
  result<value> parsed = parse(text, {.allow_trailing_commas = true,
                                      .allow_comments = true,
                                      .reject_duplicate_keys = true,
                                      .validate_utf8 = true});
  if (!parsed)
    return nx::err(parsed.error());
  strip_asset_comments(parsed.value());
  return parsed;
}

result<nx::string> normalize_asset_document(const nx::string_view text) {
  result<value> parsed = parse_asset_document(text);
  if (!parsed)
    return nx::err(parsed.error());
  return write(parsed.value());
}

nx::vector<u8> encode_document(const value &root) {
  nx::nva::Writer data;
  write_binary(root, data);

  nx::nva::ChunkWriter container(FORMAT, VERSION);
  container.add(CHUNK_DATA, data.span());
  return container.finish();
}

bool decode_document(const std::span<const u8> bytes, value &out) {
  const auto view = nx::nva::View::open(bytes, FORMAT);
  if (!view || view->version() != VERSION)
    return false;
  const std::span<const u8> data = view->chunk(CHUNK_DATA);
  if (data.empty())
    return false;

  nx::nva::Reader reader(data);
  value decoded = read_binary(reader);
  if (!reader.ok() || reader.remaining() != 0)
    return false;
  out = std::move(decoded);
  return true;
}

nx::string document_cooked_path(const nx::string_view source) {
  nx::string out(source);
  out += ".nxb";
  return out;
}

bool load_document_file(const nx::string_view source, value &out) {
  const nx::string cooked_path = document_cooked_path(source);
  const nx::vfs::FileInfo cooked_info = nx::vfs::stat(cooked_path.view());
  if (cooked_info.exists) {
    const auto cooked = nx::vfs::read(cooked_path.view());
    return cooked && decode_document({cooked->data(), cooked->size()}, out);
  }
  if (!nx::asset_policy::can_fallback_to_authored_source(cooked_info.exists))
    return false;

  const auto text = nx::vfs::read_text(source);
  if (!text)
    return false;
  auto parsed = parse_asset_document(text->view());
  if (!parsed)
    return false;
  out = std::move(parsed.value());
  return true;
}

} // namespace nx::json
