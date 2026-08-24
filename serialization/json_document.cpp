#include "core/foundation/serialization/json_document.h"

#include "core/foundation/serialization/json_binary.h"
#include "core/foundation/serialization/nva.h"
#include "core/foundation/vfs/vfs.h"

namespace nx::json {
namespace {

constexpr u32 FORMAT = nx::nva::fourcc('N', 'X', 'J', 'D');
constexpr u32 CHUNK_DATA = nx::nva::fourcc('D', 'A', 'T', 'A');
constexpr u16 VERSION = 1;

} // namespace

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
  if (const auto cooked = nx::vfs::read(cooked_path.view()))
    if (decode_document({cooked->data(), cooked->size()}, out))
      return true;

  const auto text = nx::vfs::read_text(source);
  if (!text)
    return false;
  auto parsed = parse(text->view());
  if (!parsed)
    return false;
  out = std::move(parsed.value());
  return true;
}

} // namespace nx::json
