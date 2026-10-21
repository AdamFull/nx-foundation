#pragma once

#include "core/foundation/containers/blob.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <span>

/// Zstandard frames: whole buffers at once, or a frame fed in pieces. Each
/// frame carries a checksum of its content, so damage reads as Corrupt.
/// Decompressing names the most it may produce, since a frame is untrusted.
namespace nx::zstd {

enum class zstd_error : u8 {
  /// Not a frame, damaged, or cut short.
  Corrupt,
  /// It would produce more than the caller's limit.
  TooLarge,
  OutOfMemory,
};

[[nodiscard]] nx::string_view to_string(zstd_error error) noexcept;

template <typename T> using zstd_result = nx::result<T, zstd_error>;

inline constexpr int DEFAULT_LEVEL = 3;

[[nodiscard]] int min_level() noexcept;
[[nodiscard]] int max_level() noexcept;
/// The most one frame of @p size bytes can take.
[[nodiscard]] usize compress_bound(usize size) noexcept;

/// Compresses and decompresses with one context, kept between calls; a
/// context is a few hundred KiB to set up, so code compressing many small
/// buffers keeps one. Not for two threads at once.
class compressor {
public:
  /// Levels outside min_level()..max_level() are clamped.
  explicit compressor(int level = DEFAULT_LEVEL);
  ~compressor();

  compressor(compressor &&other) noexcept;
  compressor &operator=(compressor &&other) noexcept;
  compressor(const compressor &) = delete;
  compressor &operator=(const compressor &) = delete;

  /// Appends one frame holding @p source, its size recorded, to @p out.
  [[nodiscard]] zstd_result<void> compress(std::span<const u8> source,
                                           nx::vector<u8> &out);

  /// Starts a frame fed through feed(), ending any frame not finished.
  /// @p content_size, when known, is recorded in the frame and must then be
  /// what is fed.
  [[nodiscard]] zstd_result<void> begin(u64 content_size = UNKNOWN_SIZE);
  /// Appends what @p source compresses to so far to @p out.
  [[nodiscard]] zstd_result<void> feed(std::span<const u8> source,
                                       nx::vector<u8> &out);
  /// Appends the rest of the frame to @p out.
  [[nodiscard]] zstd_result<void> finish(nx::vector<u8> &out);

  static constexpr u64 UNKNOWN_SIZE = ~u64{0};

private:
  void *m_context = nullptr;
  int m_level = DEFAULT_LEVEL;
};

class decompressor {
public:
  decompressor();
  ~decompressor();

  decompressor(decompressor &&other) noexcept;
  decompressor &operator=(decompressor &&other) noexcept;
  decompressor(const decompressor &) = delete;
  decompressor &operator=(const decompressor &) = delete;

  /// What the frames in @p source hold, at most @p max_size bytes; no frames
  /// at all is Corrupt. Frames that do not record their size are
  /// decompressed in pieces, still within the limit.
  [[nodiscard]] zstd_result<nx::blob<u8>> decompress(std::span<const u8> source,
                                                     usize max_size);

  /// Starts reading a frame through feed().
  void begin(usize max_size);
  /// Appends what @p source decompresses to so far to @p out; true once the
  /// frame has ended, after which the rest of @p source is not read.
  [[nodiscard]] zstd_result<bool> feed(std::span<const u8> source,
                                       nx::vector<u8> &out);

private:
  void *m_context = nullptr;
  usize m_limit = 0;
  usize m_produced = 0;
};

/// One frame of @p source, through a context of its own.
[[nodiscard]] zstd_result<nx::vector<u8>> compress(std::span<const u8> source,
                                                   int level = DEFAULT_LEVEL);
/// What @p source holds, at most @p max_size bytes.
[[nodiscard]] zstd_result<nx::blob<u8>> decompress(std::span<const u8> source,
                                                   usize max_size);

} // namespace nx::zstd
