#include "core/foundation/serialization/zstd.h"

// ZSTD_findDecompressedSize; nx::zstd is the static library.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <algorithm>
#include <utility>

namespace nx::zstd {

namespace {

[[nodiscard]] zstd_error error_of(const size_t code) noexcept {
  return ZSTD_getErrorCode(code) == ZSTD_error_memory_allocation
             ? zstd_error::OutOfMemory
             : zstd_error::Corrupt;
}

[[nodiscard]] ZSTD_CCtx *cctx(void *context) noexcept {
  return static_cast<ZSTD_CCtx *>(context);
}

[[nodiscard]] ZSTD_DCtx *dctx(void *context) noexcept {
  return static_cast<ZSTD_DCtx *>(context);
}

/// Runs one streaming step over the rest of @p source, growing @p out by
/// what it writes; for compression, @p op is ZSTD_e_continue or ZSTD_e_end.
[[nodiscard]] zstd_result<void> stream(ZSTD_CCtx *context,
                                       const std::span<const u8> source,
                                       nx::vector<u8> &out,
                                       const ZSTD_EndDirective op) {
  ZSTD_inBuffer in{source.data(), source.size(), 0};
  const usize chunk = ZSTD_CStreamOutSize();
  for (;;) {
    const usize at = out.size();
    out.resize(at + chunk);
    ZSTD_outBuffer to{out.data() + at, chunk, 0};
    const size_t left = ZSTD_compressStream2(context, &to, &in, op);
    out.resize(at + to.pos);
    if (ZSTD_isError(left))
      return err(error_of(left));
    const bool done =
        op == ZSTD_e_end ? left == 0 : in.pos == in.size && to.pos < chunk;
    if (done)
      return {};
  }
}

/// Decompresses from @p in until its frame ends (true) or it runs out
/// (false), counting what it writes against @p limit.
[[nodiscard]] zstd_result<bool> inflate(ZSTD_DCtx *context, ZSTD_inBuffer &in,
                                        nx::vector<u8> &out, const usize limit,
                                        usize &produced) {
  const usize chunk = ZSTD_DStreamOutSize();
  for (;;) {
    const usize at = out.size();
    out.resize(at + chunk);
    ZSTD_outBuffer to{out.data() + at, chunk, 0};
    const size_t left = ZSTD_decompressStream(context, &to, &in);
    out.resize(at + to.pos);
    if (ZSTD_isError(left))
      return err(error_of(left));
    produced += to.pos;
    if (produced > limit)
      return err(zstd_error::TooLarge);
    if (left == 0)
      return true;
    if (in.pos == in.size && to.pos < chunk)
      return false;
  }
}

} // namespace

nx::string_view to_string(const zstd_error error) noexcept {
  switch (error) {
  case zstd_error::Corrupt:
    return "corrupt";
  case zstd_error::TooLarge:
    return "too large";
  case zstd_error::OutOfMemory:
    return "out of memory";
  }
  return "unknown";
}

int min_level() noexcept { return ZSTD_minCLevel(); }

int max_level() noexcept { return ZSTD_maxCLevel(); }

usize compress_bound(const usize size) noexcept {
  return ZSTD_compressBound(size);
}

compressor::compressor(const int level)
    : m_context(ZSTD_createCCtx()),
      m_level(std::clamp(level, min_level(), max_level())) {
  if (m_context == nullptr)
    throw std::bad_alloc();
  ZSTD_CCtx *const context = cctx(m_context);
  (void)ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, m_level);
  (void)ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
}

compressor::~compressor() { ZSTD_freeCCtx(cctx(m_context)); }

compressor::compressor(compressor &&other) noexcept
    : m_context(std::exchange(other.m_context, nullptr)),
      m_level(other.m_level) {}

compressor &compressor::operator=(compressor &&other) noexcept {
  if (this != &other) {
    ZSTD_freeCCtx(cctx(m_context));
    m_context = std::exchange(other.m_context, nullptr);
    m_level = other.m_level;
  }
  return *this;
}

zstd_result<void> compressor::compress(const std::span<const u8> source,
                                       nx::vector<u8> &out) {
  const usize at = out.size();
  out.resize(at + compress_bound(source.size()));
  const size_t written =
      ZSTD_compress2(cctx(m_context), out.data() + at, out.size() - at,
                     source.data(), source.size());
  if (ZSTD_isError(written)) {
    out.resize(at);
    return err(error_of(written));
  }
  out.resize(at + written);
  return {};
}

zstd_result<void> compressor::begin(const u64 content_size) {
  ZSTD_CCtx *const context = cctx(m_context);
  (void)ZSTD_CCtx_reset(context, ZSTD_reset_session_only);
  const size_t code = ZSTD_CCtx_setPledgedSrcSize(
      context,
      content_size == UNKNOWN_SIZE ? ZSTD_CONTENTSIZE_UNKNOWN : content_size);
  if (ZSTD_isError(code))
    return err(error_of(code));
  return {};
}

zstd_result<void> compressor::feed(const std::span<const u8> source,
                                   nx::vector<u8> &out) {
  return stream(cctx(m_context), source, out, ZSTD_e_continue);
}

zstd_result<void> compressor::finish(nx::vector<u8> &out) {
  return stream(cctx(m_context), {}, out, ZSTD_e_end);
}

decompressor::decompressor() : m_context(ZSTD_createDCtx()) {
  if (m_context == nullptr)
    throw std::bad_alloc();
}

decompressor::~decompressor() { ZSTD_freeDCtx(dctx(m_context)); }

decompressor::decompressor(decompressor &&other) noexcept
    : m_context(std::exchange(other.m_context, nullptr)),
      m_limit(other.m_limit), m_produced(other.m_produced) {}

decompressor &decompressor::operator=(decompressor &&other) noexcept {
  if (this != &other) {
    ZSTD_freeDCtx(dctx(m_context));
    m_context = std::exchange(other.m_context, nullptr);
    m_limit = other.m_limit;
    m_produced = other.m_produced;
  }
  return *this;
}

zstd_result<nx::blob<u8>>
decompressor::decompress(const std::span<const u8> source,
                         const usize max_size) {
  if (source.empty())
    return err(zstd_error::Corrupt);
  const unsigned long long size =
      ZSTD_findDecompressedSize(source.data(), source.size());
  if (size == ZSTD_CONTENTSIZE_ERROR)
    return err(zstd_error::Corrupt);
  if (size != ZSTD_CONTENTSIZE_UNKNOWN) {
    if (size > max_size)
      return err(zstd_error::TooLarge);
    nx::blob<u8> out(static_cast<usize>(size));
    const size_t written = ZSTD_decompressDCtx(
        dctx(m_context), out.data(), out.size(), source.data(), source.size());
    if (ZSTD_isError(written))
      return err(error_of(written));
    if (written != out.size())
      return err(zstd_error::Corrupt);
    return out;
  }

  nx::vector<u8> out;
  usize produced = 0;
  ZSTD_inBuffer in{source.data(), source.size(), 0};
  while (in.pos < in.size) {
    (void)ZSTD_DCtx_reset(dctx(m_context), ZSTD_reset_session_only);
    zstd_result<bool> ended =
        inflate(dctx(m_context), in, out, max_size, produced);
    if (!ended)
      return err(ended.error());
    if (!*ended)
      return err(zstd_error::Corrupt);
  }
  return nx::blob<u8>(std::span<const u8>(out));
}

void decompressor::begin(const usize max_size) {
  (void)ZSTD_DCtx_reset(dctx(m_context), ZSTD_reset_session_only);
  m_limit = max_size;
  m_produced = 0;
}

zstd_result<bool> decompressor::feed(const std::span<const u8> source,
                                     nx::vector<u8> &out) {
  ZSTD_inBuffer in{source.data(), source.size(), 0};
  return inflate(dctx(m_context), in, out, m_limit, m_produced);
}

zstd_result<nx::vector<u8>> compress(const std::span<const u8> source,
                                     const int level) {
  compressor context(level);
  nx::vector<u8> out;
  if (zstd_result<void> done = context.compress(source, out); !done)
    return err(done.error());
  return out;
}

zstd_result<nx::blob<u8>> decompress(const std::span<const u8> source,
                                     const usize max_size) {
  return decompressor().decompress(source, max_size);
}

} // namespace nx::zstd
