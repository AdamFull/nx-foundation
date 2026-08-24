#pragma once

#include "core/foundation/containers/blob.h"

#include <span>

namespace nx {

/// Owns a blob while exposing a read-only slice of it. This is useful for
/// chunked containers: the container allocation stays alive while a decoder
/// or parser sees only one payload, without making a second payload-sized
/// copy.
template <typename T> class owned_blob_slice {
public:
  owned_blob_slice() noexcept = default;

  explicit owned_blob_slice(blob<T> storage) noexcept {
    m_size = storage.size();
    m_storage = std::move(storage);
  }

  owned_blob_slice(blob<T> storage, const usize offset,
                   const usize size) noexcept
      : m_offset(offset), m_size(size), m_storage(std::move(storage)) {}

  owned_blob_slice(const owned_blob_slice &) = delete;
  owned_blob_slice &operator=(const owned_blob_slice &) = delete;
  owned_blob_slice(owned_blob_slice &&) noexcept = default;
  owned_blob_slice &operator=(owned_blob_slice &&) noexcept = default;

  [[nodiscard]] std::span<const T> view() const noexcept {
    if (m_offset > m_storage.size() || m_size > m_storage.size() - m_offset)
      return {};
    if (m_size == 0)
      return {};
    return {m_storage.data() + m_offset, m_size};
  }

  [[nodiscard]] const T *data() const noexcept { return view().data(); }
  [[nodiscard]] usize size() const noexcept { return view().size(); }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
  usize m_offset = 0;
  usize m_size = 0;
  blob<T> m_storage;
};

} // namespace nx
