#include "core/foundation/serialization/ini.h"

namespace nx::ini {
namespace {

[[nodiscard]] constexpr bool ascii_space(const char c) noexcept {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

[[nodiscard]] constexpr nx::string_view trim(nx::string_view text) noexcept {
  while (!text.empty() && ascii_space(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && ascii_space(text.back()))
    text.remove_suffix(1);
  return text;
}

[[nodiscard]] constexpr bool valid_section(const nx::string_view section) {
  for (const char c : section)
    if (c == '[' || c == ']' || c == '\r' || c == '\n')
      return false;
  return true;
}

[[nodiscard]] constexpr bool valid_key(const nx::string_view key) {
  if (key.empty() || trim(key) != key)
    return false;
  for (const char c : key)
    if (c == '=' || c == '[' || c == ']' || c == '\r' || c == '\n')
      return false;
  return true;
}

[[nodiscard]] constexpr bool needs_quotes(const nx::string_view value) {
  if (value.empty())
    return false;
  if (trim(value) != value)
    return true;
  for (const char c : value)
    if (c == ';' || c == '#' || c == '"' || c == '\r' || c == '\n' ||
        c == '\t')
      return true;
  return false;
}

void append_value(nx::string &out, const nx::string_view value) {
  if (!needs_quotes(value)) {
    out.append(value);
    return;
  }

  out.push_back(u8'"');
  for (const char c : value) {
    switch (c) {
    case '\\':
      out.append("\\\\");
      break;
    case '"':
      out.append("\\\"");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  out.push_back(u8'"');
}

[[nodiscard]] nx::result<nx::string, ParseError>
parse_value(const nx::string_view source, const usize line,
            const usize value_column) {
  const nx::string_view value = trim(source);
  const usize leading = nx::cast<usize>(value.data() - source.data());
  if (value.empty() || value.front() != '"') {
    usize comment = nx::string_view::npos;
    for (usize i = 0; i < value.size(); ++i) {
      if ((value[i] == ';' || value[i] == '#') &&
          (i == 0 || ascii_space(value[i - 1]))) {
        comment = i;
        break;
      }
    }
    return nx::string(trim(value.substr(0, comment)));
  }

  nx::string decoded;
  for (usize i = 1; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '"') {
      const nx::string_view trailing = trim(value.substr(i + 1));
      if (!trailing.empty() && trailing.front() != ';' &&
          trailing.front() != '#') {
        return nx::err(ParseError{ParseErrorKind::TrailingCharacters, line,
                                  value_column + leading + i + 1});
      }
      return decoded;
    }
    if (c != '\\') {
      decoded.push_back(c);
      continue;
    }
    if (++i >= value.size()) {
      return nx::err(ParseError{ParseErrorKind::UnterminatedString, line,
                                value_column + leading + i});
    }
    switch (value[i]) {
    case '\\':
      decoded.push_back(u8'\\');
      break;
    case '"':
      decoded.push_back(u8'"');
      break;
    case 'n':
      decoded.push_back(u8'\n');
      break;
    case 'r':
      decoded.push_back(u8'\r');
      break;
    case 't':
      decoded.push_back(u8'\t');
      break;
    default:
      return nx::err(ParseError{ParseErrorKind::InvalidEscape, line,
                                value_column + leading + i + 1});
    }
  }

  return nx::err(ParseError{ParseErrorKind::UnterminatedString, line,
                            value_column + leading + value.size()});
}

} // namespace

nx::string_view ParseError::message() const noexcept {
  switch (kind) {
  case ParseErrorKind::InvalidSection:
    return "invalid section header";
  case ParseErrorKind::MissingAssignment:
    return "expected a key/value assignment";
  case ParseErrorKind::InvalidKey:
    return "invalid key";
  case ParseErrorKind::UnterminatedString:
    return "unterminated quoted value";
  case ParseErrorKind::InvalidEscape:
    return "invalid escape sequence";
  case ParseErrorKind::TrailingCharacters:
    return "unexpected characters after quoted value";
  }
  return "invalid INI document";
}

const nx::string *Document::find(const nx::string_view section,
                                 const nx::string_view key) const noexcept {
  for (const Entry &entry : m_entries)
    if (entry.section == section && entry.key == key)
      return &entry.value;
  return nullptr;
}

nx::string *Document::find(const nx::string_view section,
                           const nx::string_view key) noexcept {
  for (Entry &entry : m_entries)
    if (entry.section == section && entry.key == key)
      return &entry.value;
  return nullptr;
}

bool Document::set(const nx::string_view section, const nx::string_view key,
                   const nx::string_view value) {
  if (!valid_section(section) || !valid_key(key))
    return false;
  if (nx::string *const existing = find(section, key)) {
    *existing = nx::string(value);
    return true;
  }
  m_entries.push_back(
      Entry{nx::string(section), nx::string(key), nx::string(value)});
  return true;
}

bool Document::erase(const nx::string_view section,
                     const nx::string_view key) noexcept {
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (it->section == section && it->key == key) {
      m_entries.erase(it);
      return true;
    }
  }
  return false;
}

nx::string Document::serialize() const {
  nx::string out;
  const auto write_entries = [this, &out](const nx::string_view section) {
    for (const Entry &entry : m_entries) {
      if (entry.section != section)
        continue;
      out.append(entry.key.view());
      out.append(" = ");
      append_value(out, entry.value.view());
      out.push_back(u8'\n');
    }
  };

  write_entries({});
  for (usize i = 0; i < m_entries.size(); ++i) {
    const nx::string_view section = m_entries[i].section.view();
    if (section.empty())
      continue;
    bool seen = false;
    for (usize previous = 0; previous < i; ++previous) {
      if (m_entries[previous].section == section) {
        seen = true;
        break;
      }
    }
    if (seen)
      continue;
    if (!out.empty())
      out.push_back(u8'\n');
    out.push_back(u8'[');
    out.append(section);
    out.append("]\n");
    write_entries(section);
  }
  return out;
}

nx::result<Document, ParseError> parse(nx::string_view text) {
  Document document;
  nx::string section;
  usize line_number = 1;
  usize offset = 0;

  if (text.size() >= 3 && nx::cast<u8>(text[0]) == 0xefu &&
      nx::cast<u8>(text[1]) == 0xbbu && nx::cast<u8>(text[2]) == 0xbfu)
    offset = 3;

  while (offset < text.size()) {
    const usize end = text.find('\n', offset);
    const usize count = end == nx::string_view::npos ? text.size() - offset
                                                      : end - offset;
    nx::string_view line = text.substr(offset, count);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    const nx::string_view content = trim(line);
    const usize leading = nx::cast<usize>(content.data() - line.data());

    if (!content.empty() && content.front() != ';' &&
        content.front() != '#') {
      if (content.front() == '[') {
        const usize close = content.find(']');
        if (close == nx::string_view::npos) {
          return nx::err(ParseError{ParseErrorKind::InvalidSection,
                                    line_number, leading + 1});
        }
        const nx::string_view name = trim(content.substr(1, close - 1));
        const nx::string_view trailing = trim(content.substr(close + 1));
        if (name.empty() || !valid_section(name) ||
            (!trailing.empty() && trailing.front() != ';' &&
             trailing.front() != '#')) {
          return nx::err(ParseError{ParseErrorKind::InvalidSection,
                                    line_number, leading + 1});
        }
        section = nx::string(name);
      } else {
        const usize assignment = content.find('=');
        if (assignment == nx::string_view::npos) {
          return nx::err(ParseError{ParseErrorKind::MissingAssignment,
                                    line_number, leading + 1});
        }
        const nx::string_view key = trim(content.substr(0, assignment));
        if (!valid_key(key)) {
          return nx::err(ParseError{ParseErrorKind::InvalidKey, line_number,
                                    leading + 1});
        }
        auto value = parse_value(content.substr(assignment + 1), line_number,
                                 leading + assignment + 2);
        if (!value)
          return nx::err(value.error());
        (void)document.set(section.view(), key, value->view());
      }
    }

    if (end == nx::string_view::npos)
      break;
    offset = end + 1;
    ++line_number;
  }
  return document;
}

} // namespace nx::ini
