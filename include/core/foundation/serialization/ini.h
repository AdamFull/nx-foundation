#pragma once

#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

#include <optional>
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

/// @p text with @p key in @p section set to @p value, every other line kept as
/// it was, comments included. Each assignment of the key is rewritten in
/// place, keeping a comment at its end; a missing key goes after the
/// section's last assignment, and a missing section at the end. New lines
/// follow the text's line endings. Text that does not parse is refused, and
/// so is a section or key the grammar cannot hold (a ParseError of
/// InvalidSection or InvalidKey at line 0).
[[nodiscard]] nx::result<nx::string, ParseError> assign(nx::string_view text,
                                                        nx::string_view section,
                                                        nx::string_view key,
                                                        nx::string_view value);

/// @p document cooked: a versioned NVA container ('NXIN') of its entries in
/// order, which is what a Shipping build reads in place of the text.
[[nodiscard]] nx::vector<u8> encode(const Document &document);
/// The document encode() made @p bytes from; nullopt when they are not one,
/// of another version, or truncated.
[[nodiscard]] std::optional<Document> decode(std::span<const u8> bytes);

enum class LoadErrorKind : u8 {
  /// Neither the cooked file nor, where allowed, the source is there.
  NotFound,
  Unreadable,
  /// The cooked file is there but is not a document of this version.
  Malformed,
  /// The source text does not parse (LoadError::parse says where).
  Invalid,
};

struct LoadError {
  LoadErrorKind kind = LoadErrorKind::NotFound;
  ParseError parse;

  [[nodiscard]] nx::string_view message() const noexcept;
};

/// The INI asset at VFS path @p source (as `/config/modio.ini`): its cooked
/// sibling `<source>.nxb` when there is one, else, outside Shipping
/// (asset_policy), the source text parsed. A cooked file that does not decode
/// is not hidden by its source.
[[nodiscard]] nx::result<Document, LoadError> load(nx::string_view source);

} // namespace nx::ini
