#pragma once

/**
 * @file shared_library.h
 * @brief Loading code at run time and finding a symbol in it (nx::os).
 */

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace nx::os {

/// Move-only: it owns the OS handle, and two owners would unload it twice.
class shared_library {
public:
  shared_library() = default;
  ~shared_library();

  shared_library(const shared_library &) = delete;
  shared_library &operator=(const shared_library &) = delete;
  shared_library(shared_library &&other) noexcept;
  shared_library &operator=(shared_library &&other) noexcept;

  /// Whether this platform loads code at run time at all.
  [[nodiscard]] static bool supported() noexcept;

  [[nodiscard]] static string decorate(string_view stem);

  bool open(string_view path);
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return m_handle != nullptr; }

  [[nodiscard]] void *symbol(string_view name) const noexcept;

  [[nodiscard]] string_view last_error() const noexcept { return m_error; }

private:
  void *m_handle = nullptr;
  string m_error;
};

} // namespace nx::os
