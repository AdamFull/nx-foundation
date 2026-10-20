#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#include <span>

namespace nx::crash {

inline constexpr usize NOTE_KEY_CAPACITY = 32;
inline constexpr usize NOTE_VALUE_CAPACITY = 480;
inline constexpr usize NOTE_CAPACITY = 24;

inline constexpr usize LOG_TAIL_CAPACITY = 192;
inline constexpr usize ATTACHMENT_NAME_CAPACITY = 64;
inline constexpr usize ATTACHMENT_SIZE_CAPACITY = 64u * 1024u * 1024u;

struct Config {
  string_view directory;
  string_view application;
  string_view version;
  bool minidump = true;
  bool minidump_full_memory = false;
  u32 keep = 20;
  bool log_tail = true;
};

bool install(const Config &config = {});
void shutdown();

[[nodiscard]] bool installed() noexcept;

[[nodiscard]] string_view directory() noexcept;

void note(string_view key, string_view value);
void clear_note(string_view key);

struct Attachment {
  /// A filename component, not a path. Only ASCII letters, digits, '.', '_'
  /// and '-' are accepted by report().
  string_view name;
  std::span<const u8> bytes;
};

struct Fatal {
  string_view reason;
  string_view detail = {};
  string_view kind = "fatal";
  std::span<const Attachment> attachments = {};
};

bool report(const Fatal &fatal);
[[nodiscard]] string_view last_report() noexcept;
[[nodiscard]] vector<string> pending_reports();

} // namespace nx::crash
