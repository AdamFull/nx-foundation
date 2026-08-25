#include "core/foundation/serialization/resource_bundle.h"

#include "core/foundation/serialization/nva.h"

#include <algorithm>
#include <limits>

namespace nx::resource_bundle {
namespace {

constexpr u32 CHUNK_METADATA = nva::fourcc('M', 'E', 'T', 'A');
constexpr u32 CHUNK_INDEX = nva::fourcc('I', 'N', 'D', 'X');
constexpr u32 CHUNK_DATA = nva::fourcc('D', 'A', 'T', 'A');

[[nodiscard]] constexpr char ascii_lower(const char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                      : value;
}

[[nodiscard]] bool reserved_windows_name(const string_view segment) noexcept {
  const usize dot = segment.find('.');
  const string_view stem = segment.substr(0, dot);
  const auto equals = [stem](const string_view wanted) {
    if (stem.size() != wanted.size())
      return false;
    for (usize i = 0; i < stem.size(); ++i)
      if (ascii_lower(stem[i]) != wanted[i])
        return false;
    return true;
  };
  if (equals("con") || equals("prn") || equals("aux") || equals("nul"))
    return true;
  if (stem.size() == 4 &&
      (ascii_lower(stem[0]) == 'c' || ascii_lower(stem[0]) == 'l')) {
    const bool device = ascii_lower(stem[0]) == 'c' &&
                        ascii_lower(stem[1]) == 'o' &&
                        ascii_lower(stem[2]) == 'm';
    const bool printer = ascii_lower(stem[0]) == 'l' &&
                         ascii_lower(stem[1]) == 'p' &&
                         ascii_lower(stem[2]) == 't';
    return (device || printer) && stem[3] >= '1' && stem[3] <= '9';
  }
  return false;
}

[[nodiscard]] bool valid_limits(const Limits &limits) noexcept {
  return limits.max_entries != 0 && limits.max_name_bytes != 0 &&
         limits.max_name_bytes <= std::numeric_limits<u32>::max() &&
         limits.max_data_bytes <= std::numeric_limits<u32>::max() &&
         limits.max_metadata_bytes <= std::numeric_limits<u32>::max();
}

} // namespace

bool valid_name(const string_view name, const usize max_bytes) noexcept {
  if (name.empty() || name.size() > max_bytes || name.front() == '/' ||
      name.back() == '/' || !utf8::validate_utf8(name.data(), name.size()))
    return false;
  usize segment = 0;
  for (usize i = 0; i <= name.size(); ++i) {
    if (i != name.size() && name[i] != '/') {
      const u8 c = static_cast<u8>(name[i]);
      if (c < 0x20 || c == 0x7f || name[i] == '\\' || name[i] == ':' ||
          name[i] == '<' || name[i] == '>' || name[i] == '"' ||
          name[i] == '|' || name[i] == '?' || name[i] == '*')
        return false;
      continue;
    }
    const string_view part = name.substr(segment, i - segment);
    if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
        part.back() == ' ' || reserved_windows_name(part))
      return false;
    segment = i + 1;
  }
  return true;
}

std::optional<vector<u8>> encode(const u32 format, const u16 version,
                                 const std::span<const u8> metadata,
                                 const std::span<const Source> sources,
                                 const Limits limits) {
  if (format == 0 || version == 0 || !valid_limits(limits) || sources.empty() ||
      sources.size() > limits.max_entries ||
      metadata.size() > limits.max_metadata_bytes)
    return std::nullopt;
  try {
    vector<const Source *> ordered;
    ordered.reserve(sources.size());
    usize total = 0;
    for (const Source &source : sources) {
      if (!valid_name(source.name, limits.max_name_bytes) ||
          source.bytes.empty() || source.bytes.size() > limits.max_data_bytes ||
          total > limits.max_data_bytes - source.bytes.size())
        return std::nullopt;
      total += source.bytes.size();
      ordered.push_back(&source);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Source *const lhs, const Source *const rhs) {
                return lhs->name < rhs->name;
              });
    for (usize i = 1; i < ordered.size(); ++i)
      if (ordered[i - 1]->name == ordered[i]->name)
        return std::nullopt;

    nva::Writer index;
    nva::Writer data;
    index.u32v(nx::cast<u32>(ordered.size()));
    usize offset = 0;
    for (const Source *const source : ordered) {
      index.str(source->name);
      index.u64v(nx::cast<u64>(offset));
      index.u64v(nx::cast<u64>(source->bytes.size()));
      data.raw(source->bytes);
      offset += source->bytes.size();
    }

    nva::ChunkWriter container(format, version);
    if (!metadata.empty())
      container.add(CHUNK_METADATA, metadata);
    container.add(CHUNK_INDEX, index.span());
    container.add(CHUNK_DATA, data.span());
    return container.finish();
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<View> View::open(const std::span<const u8> bytes,
                               const u32 format, const u16 version,
                               const Limits limits) {
  if (format == 0 || version == 0 || !valid_limits(limits))
    return std::nullopt;
  try {
    const std::optional<nva::View> container = nva::View::open(bytes, format);
    if (!container || container->version() != version ||
        container->flags() != 0)
      return std::nullopt;
    const std::span<const u8> metadata = container->chunk(CHUNK_METADATA);
    const std::span<const u8> index = container->chunk(CHUNK_INDEX);
    const std::span<const u8> data = container->chunk(CHUNK_DATA);
    if (metadata.size() > limits.max_metadata_bytes || index.empty() ||
        data.empty() || data.size() > limits.max_data_bytes)
      return std::nullopt;

    nva::Reader reader(index);
    const u32 count = reader.u32v();
    if (count == 0 || count > limits.max_entries ||
        !reader.require_items(count, 20))
      return std::nullopt;

    View out;
    out.m_metadata = metadata;
    out.m_data = data;
    out.m_entries.reserve(count);
    usize expected_offset = 0;
    string_view previous;
    for (u32 i = 0; i < count; ++i) {
      const string_view name = reader.str();
      const u64 offset64 = reader.u64v();
      const u64 size64 = reader.u64v();
      if (!reader.ok() || !valid_name(name, limits.max_name_bytes) ||
          (i != 0 && !(previous < name)) ||
          offset64 != nx::cast<u64>(expected_offset) || size64 == 0 ||
          size64 > nx::cast<u64>(data.size() - expected_offset))
        return std::nullopt;
      const usize size = nx::cast<usize>(size64);
      out.m_entries.push_back({name, expected_offset, size});
      expected_offset += size;
      previous = name;
    }
    if (!reader.ok() || reader.remaining() != 0 ||
        expected_offset != data.size())
      return std::nullopt;
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

std::span<const u8> View::find(const string_view name) const noexcept {
  const auto found =
      std::lower_bound(m_entries.begin(), m_entries.end(), name,
                       [](const Entry &entry, const string_view wanted) {
                         return entry.name < wanted;
                       });
  if (found == m_entries.end() || found->name != name)
    return {};
  return m_data.subspan(found->offset, found->size);
}

} // namespace nx::resource_bundle
