#pragma once

#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

#include <span>

namespace nx::ini {

enum class ParseErrorKind : u8 {
  InvalidSection,
  MissingAssignment,
  InvalidKey,
  UnterminatedString,
  InvalidEscape,
  TrailingCharacters,
};

struct ParseError {
  ParseErrorKind kind = ParseErrorKind::MissingAssignment;
  usize line = 1;
  usize column = 1;

  [[nodiscard]] nx::string_view message() const noexcept;
};

struct Entry {
  nx::string section;
  nx::string key;
  nx::string value;
};

/// An owning, insertion-ordered INI document.
///
/// Section and key names are case-sensitive. Repeated assignments update the
/// first matching entry, so the last authored value wins without producing
/// duplicate values when the document is serialized again.
class Document {
public:
  [[nodiscard]] const nx::string *find(nx::string_view section,
                                       nx::string_view key) const noexcept;
  [[nodiscard]] nx::string *find(nx::string_view section,
                                 nx::string_view key) noexcept;
  [[nodiscard]] bool contains(nx::string_view section,
                              nx::string_view key) const noexcept {
    return find(section, key) != nullptr;
  }

  /// Adds or replaces a value. False means the section or key cannot be
  /// represented safely by the INI grammar.
  [[nodiscard]] bool set(nx::string_view section, nx::string_view key,
                         nx::string_view value);
  [[nodiscard]] bool erase(nx::string_view section,
                           nx::string_view key) noexcept;

  [[nodiscard]] std::span<const Entry> entries() const noexcept {
    return m_entries;
  }
  [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
  void clear() noexcept { m_entries.clear(); }

  /// Writes a deterministic UTF-8 document with LF line endings.
  [[nodiscard]] nx::string serialize() const;

private:
  nx::vector<Entry> m_entries;
};

/// Parses UTF-8 INI text. Blank lines, ';'/'#' comments, a UTF-8 BOM, quoted
/// values and common backslash escapes are accepted. Inline comments begin at
/// a comment marker preceded by whitespace; quote values when the marker is
/// part of the value.
[[nodiscard]] nx::result<Document, ParseError> parse(nx::string_view text);

} // namespace nx::ini
