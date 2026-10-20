#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

namespace nx::os {

class shared_library {
public:
  shared_library() = default;
  ~shared_library();

  shared_library(const shared_library &) = delete;
  shared_library &operator=(const shared_library &) = delete;
  shared_library(shared_library &&other) noexcept;
  shared_library &operator=(shared_library &&other) noexcept;

  [[nodiscard]] static bool supported() noexcept;

  [[nodiscard]] static string decorate(string_view stem);

  /// Loads the library, closing any held before; the error is the loader's
  /// own account of why it failed.
  result<void, string> open(string_view path);
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return m_handle != nullptr; }

  [[nodiscard]] void *symbol(string_view name) const noexcept;

private:
  void *m_handle = nullptr;
};

}
