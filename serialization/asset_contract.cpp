#include "core/foundation/serialization/asset_contract.h"

#include <algorithm>

namespace nx::asset_contract {
namespace {

[[nodiscard]] constexpr bool ends_with(const string_view value,
                                       const string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] constexpr bool starts_with(const string_view value,
                                         const string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] constexpr bool valid_suffix(const string_view suffix) noexcept {
  return suffix.size() > 1 && suffix.front() == '.';
}

inline constexpr string_view AUTHORING_ONLY_SUFFIXES[] = {
    ".meta", ".aseprite", ".slang", ".spv", ".metallib", ".refl.json", ".md"};

[[nodiscard]] constexpr bool
reserved_source_suffix(const string_view suffix) noexcept {
  if (ends_with(suffix, ".nxb"))
    return true;
  for (const string_view reserved : AUTHORING_ONLY_SUFFIXES)
    if (ends_with(suffix, reserved))
      return true;
  return false;
}

} // namespace

string_view to_string(const RegistryError error) noexcept {
  switch (error) {
  case RegistryError::None:
    return "none";
  case RegistryError::Frozen:
    return "the registry is frozen";
  case RegistryError::EmptyName:
    return "the family name is empty";
  case RegistryError::EmptyFormats:
    return "the family has no formats";
  case RegistryError::EmptySuffix:
    return "a suffix is empty";
  case RegistryError::InvalidSourceSuffix:
    return "a source suffix is invalid or reserved";
  case RegistryError::InvalidPublishedSuffix:
    return "a published suffix is invalid, too broad, or not a .nxb suffix";
  case RegistryError::InvalidFormat:
    return "an NVA format tag is zero";
  case RegistryError::DuplicateName:
    return "the family name is already registered";
  case RegistryError::DuplicateSourceSuffix:
    return "a source suffix is already registered";
  case RegistryError::DuplicatePublishedSuffix:
    return "a published suffix is already registered";
  }
  return "unknown registry error";
}

RegistryError
Registry::add(const string_view name, const std::span<const Format> formats,
              Handler *const handler,
              const std::span<const string_view> embedded_sources) {
  if (m_frozen)
    return RegistryError::Frozen;
  if (name.empty())
    return RegistryError::EmptyName;
  if (formats.empty())
    return RegistryError::EmptyFormats;
  for (const HandlerRecord &registered : m_handlers)
    if (registered.name == name)
      return RegistryError::DuplicateName;

  for (usize i = 0; i < formats.size(); ++i) {
    const Format &format = formats[i];
    if (format.source_suffix.empty() || format.published_suffix.empty())
      return RegistryError::EmptySuffix;
    if (!valid_suffix(format.source_suffix) ||
        reserved_source_suffix(format.source_suffix))
      return RegistryError::InvalidSourceSuffix;
    if (!valid_suffix(format.published_suffix) ||
        format.published_suffix == ".nxb" ||
        !ends_with(format.published_suffix, ".nxb"))
      return RegistryError::InvalidPublishedSuffix;
    if (format.nva_format == 0)
      return RegistryError::InvalidFormat;

    for (const RegisteredFormat &registered : m_formats) {
      if (registered.format.source_suffix == format.source_suffix)
        return RegistryError::DuplicateSourceSuffix;
      if (registered.format.published_suffix == format.published_suffix)
        return RegistryError::DuplicatePublishedSuffix;
    }
    for (const string_view registered : m_embedded_sources)
      if (registered == format.source_suffix)
        return RegistryError::DuplicateSourceSuffix;
    for (usize previous = 0; previous < i; ++previous) {
      if (formats[previous].source_suffix == format.source_suffix)
        return RegistryError::DuplicateSourceSuffix;
      if (formats[previous].published_suffix == format.published_suffix)
        return RegistryError::DuplicatePublishedSuffix;
    }
  }

  for (usize i = 0; i < embedded_sources.size(); ++i) {
    const string_view suffix = embedded_sources[i];
    if (!valid_suffix(suffix) || reserved_source_suffix(suffix))
      return RegistryError::InvalidSourceSuffix;
    for (const RegisteredFormat &registered : m_formats)
      if (registered.format.source_suffix == suffix)
        return RegistryError::DuplicateSourceSuffix;
    for (const string_view registered : m_embedded_sources)
      if (registered == suffix)
        return RegistryError::DuplicateSourceSuffix;
    for (const Format &format : formats)
      if (format.source_suffix == suffix)
        return RegistryError::DuplicateSourceSuffix;
    for (usize previous = 0; previous < i; ++previous)
      if (embedded_sources[previous] == suffix)
        return RegistryError::DuplicateSourceSuffix;
  }

  const Handler next = nx::cast<Handler>(m_handlers.size());
  m_handlers.push_back({name});
  for (const Format &format : formats)
    m_formats.push_back({format, next});
  for (const string_view suffix : embedded_sources)
    m_embedded_sources.push_back(suffix);
  if (handler != nullptr)
    *handler = next;
  return RegistryError::None;
}

void Registry::freeze() {
  if (m_frozen)
    return;
  m_source_order.resize(m_formats.size());
  m_published_order.resize(m_formats.size());
  m_embedded_order.resize(m_embedded_sources.size());
  for (usize i = 0; i < m_formats.size(); ++i) {
    m_source_order[i] = i;
    m_published_order[i] = i;
  }
  for (usize i = 0; i < m_embedded_sources.size(); ++i)
    m_embedded_order[i] = i;
  std::sort(m_source_order.begin(), m_source_order.end(),
            [this](const usize lhs, const usize rhs) {
              const string_view a = m_formats[lhs].format.source_suffix;
              const string_view b = m_formats[rhs].format.source_suffix;
              return a.size() != b.size() ? a.size() > b.size() : a < b;
            });
  std::sort(m_published_order.begin(), m_published_order.end(),
            [this](const usize lhs, const usize rhs) {
              const string_view a = m_formats[lhs].format.published_suffix;
              const string_view b = m_formats[rhs].format.published_suffix;
              return a.size() != b.size() ? a.size() > b.size() : a < b;
            });
  std::sort(m_embedded_order.begin(), m_embedded_order.end(),
            [this](const usize lhs, const usize rhs) {
              const string_view a = m_embedded_sources[lhs];
              const string_view b = m_embedded_sources[rhs];
              return a.size() != b.size() ? a.size() > b.size() : a < b;
            });
  m_frozen = true;
}

string_view Registry::handler_name(const Handler handler) const noexcept {
  return handler < m_handlers.size() ? m_handlers[handler].name : string_view{};
}

SourceMatch Registry::classify_source(const string_view path) const noexcept {
  for (const string_view suffix : AUTHORING_ONLY_SUFFIXES)
    if (ends_with(path, suffix))
      return {.kind = SourceKind::AuthoringOnly};
  if (ends_with(path, ".nxb"))
    return {.kind = SourceKind::PreCooked};
  if (!m_frozen)
    return {};
  for (const usize index : m_source_order) {
    const RegisteredFormat &registered = m_formats[index];
    if (ends_with(path, registered.format.source_suffix))
      return {.kind = SourceKind::Cookable,
              .handler = registered.handler,
              .format = &registered.format};
  }
  for (const usize index : m_embedded_order)
    if (ends_with(path, m_embedded_sources[index]))
      return {.kind = SourceKind::AuthoringOnly};
  return {};
}

std::optional<PublishedMatch>
Registry::find_published(const string_view path) const noexcept {
  if (!m_frozen)
    return std::nullopt;
  const bool shader =
      starts_with(path, "shaders/") || starts_with(path, "shaders\\");
  if (shader && ends_with(path, ".nxb")) {
    static constexpr Format shader_format{".slang", ".nxb",
                                          nva::fourcc('S', 'H', 'D', 'R')};
    return PublishedMatch{.format = &shader_format};
  }
  for (const usize index : m_published_order) {
    const RegisteredFormat &registered = m_formats[index];
    if (ends_with(path, registered.format.published_suffix))
      return PublishedMatch{.handler = registered.handler,
                            .format = &registered.format};
  }
  return std::nullopt;
}

std::optional<u32>
Registry::published_format(const string_view path) const noexcept {
  const std::optional<PublishedMatch> match = find_published(path);
  return match ? std::optional<u32>(match->format->nva_format) : std::nullopt;
}

} // namespace nx::asset_contract
