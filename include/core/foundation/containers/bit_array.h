#pragma once

#include <bit>
#include <compare>

#include "core/foundation/core/foundation.h"

namespace nx {

/// Stores @p N bits packed into 64-bit words, so 512 flags cost 64 bytes rather than the 512 a
/// `bool[]` would. Not thread-safe: concurrent writes to bits sharing a word race.
template <usize N> class bit_array {
  static_assert(N > 0, "bit_array requires N > 0");

  using word_type = u64;
  static constexpr usize BITS_PER_WORD = 64;
  static constexpr usize WORD_COUNT = (N + BITS_PER_WORD - 1) / BITS_PER_WORD;
  /// Bits in use in the final word (0 means the word is full).
  static constexpr usize TAIL_BITS = N % BITS_PER_WORD;
  static constexpr word_type TAIL_MASK =
      TAIL_BITS == 0 ? ~word_type{0} : (word_type{1} << TAIL_BITS) - 1;

public:
  using size_type = usize;

  /// Returned by find_first()/find_next() when no set bit remains.
  static constexpr usize npos = ~usize{0};

  /// Mutable proxy returned by operator[]. A single bit has no address, so `array[i] = true` needs
  /// a proxy. Behaves like a bool otherwise.
  class reference {
  public:
    constexpr reference(bit_array &owner, const usize pos) noexcept
        : m_owner(&owner), m_pos(pos) {}

    constexpr reference &operator=(const bool value) noexcept {
      m_owner->set(m_pos, value);
      return *this;
    }

    constexpr reference &operator=(const reference &other) noexcept {
      m_owner->set(m_pos, nx::cast<bool>(other));
      return *this;
    }

    [[nodiscard]] constexpr operator bool() const noexcept {
      return m_owner->test(m_pos);
    }

    /// Value of the referenced bit, inverted.
    [[nodiscard]] constexpr bool operator~() const noexcept {
      return !m_owner->test(m_pos);
    }

    constexpr reference &flip() noexcept {
      m_owner->flip(m_pos);
      return *this;
    }

  private:
    bit_array *m_owner;
    usize m_pos;
  };

  constexpr bit_array() noexcept = default;

  /// Initializes the low bits from @p value; higher bits start clear.
  constexpr explicit bit_array(const u64 value) noexcept {
    m_words[0] = value;
    if constexpr (WORD_COUNT == 1)
      m_words[0] &= TAIL_MASK;
  }

  [[nodiscard]] static constexpr usize size() noexcept { return N; }

  [[nodiscard]] constexpr bool test(const usize pos) const noexcept {
    NX_ASSERT(pos < N && "bit_array: index out of range");
    return (m_words[pos / BITS_PER_WORD] &
            (word_type{1} << (pos % BITS_PER_WORD))) != 0;
  }

  [[nodiscard]] constexpr bool operator[](const usize pos) const noexcept {
    return test(pos);
  }

  [[nodiscard]] constexpr reference operator[](const usize pos) noexcept {
    NX_ASSERT(pos < N && "bit_array: index out of range");
    return reference(*this, pos);
  }

  constexpr bit_array &set() noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      m_words[i] = ~word_type{0};
    trim();
    return *this;
  }

  constexpr bit_array &set(const usize pos, const bool value = true) noexcept {
    NX_ASSERT(pos < N && "bit_array: index out of range");
    const word_type bit = word_type{1} << (pos % BITS_PER_WORD);
    word_type &word = m_words[pos / BITS_PER_WORD];
    word = value ? (word | bit) : (word & ~bit);
    return *this;
  }

  constexpr bit_array &reset() noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      m_words[i] = 0;
    return *this;
  }

  constexpr bit_array &reset(const usize pos) noexcept {
    return set(pos, false);
  }

  constexpr bit_array &flip() noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      m_words[i] = ~m_words[i];
    trim();
    return *this;
  }

  constexpr bit_array &flip(const usize pos) noexcept {
    NX_ASSERT(pos < N && "bit_array: index out of range");
    m_words[pos / BITS_PER_WORD] ^= word_type{1} << (pos % BITS_PER_WORD);
    return *this;
  }

  /// Number of set bits.
  [[nodiscard]] constexpr usize count() const noexcept {
    usize total = 0;
    for (usize i = 0; i < WORD_COUNT; ++i)
      total += nx::cast<usize>(std::popcount(m_words[i]));
    return total;
  }

  [[nodiscard]] constexpr bool all() const noexcept {
    for (usize i = 0; i + 1 < WORD_COUNT; ++i)
      if (m_words[i] != ~word_type{0})
        return false;
    return m_words[WORD_COUNT - 1] == TAIL_MASK;
  }

  [[nodiscard]] constexpr bool any() const noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      if (m_words[i] != 0)
        return true;
    return false;
  }

  [[nodiscard]] constexpr bool none() const noexcept { return !any(); }

  /// Index of the lowest set bit, or npos when empty.
  [[nodiscard]] constexpr usize find_first() const noexcept {
    return scan_from(0);
  }

  /// Index of the lowest set bit strictly above @p pos, or npos.
  [[nodiscard]] constexpr usize find_next(const usize pos) const noexcept {
    if (pos >= N - 1)
      return npos;
    return scan_from(pos + 1);
  }

  constexpr bit_array &operator&=(const bit_array &other) noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      m_words[i] &= other.m_words[i];
    return *this;
  }

  constexpr bit_array &operator|=(const bit_array &other) noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      m_words[i] |= other.m_words[i];
    return *this;
  }

  constexpr bit_array &operator^=(const bit_array &other) noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      m_words[i] ^= other.m_words[i];
    return *this;
  }

  [[nodiscard]] constexpr bit_array operator~() const noexcept {
    bit_array result(*this);
    result.flip();
    return result;
  }

  constexpr bit_array &operator<<=(const usize shift) noexcept {
    if (shift >= N)
      return reset();
    if (shift == 0)
      return *this;

    const usize word_shift = shift / BITS_PER_WORD;
    const usize bit_shift = shift % BITS_PER_WORD;

    if (bit_shift == 0) {
      for (usize i = WORD_COUNT; i-- > word_shift;)
        m_words[i] = m_words[i - word_shift];
    } else {
      for (usize i = WORD_COUNT; i-- > word_shift;) {
        const word_type high = m_words[i - word_shift] << bit_shift;
        const word_type low =
            i - word_shift == 0
                ? word_type{0}
                : m_words[i - word_shift - 1] >> (BITS_PER_WORD - bit_shift);
        m_words[i] = high | low;
      }
    }
    for (usize i = 0; i < word_shift; ++i)
      m_words[i] = 0;

    trim();
    return *this;
  }

  constexpr bit_array &operator>>=(const usize shift) noexcept {
    if (shift >= N)
      return reset();
    if (shift == 0)
      return *this;

    const usize word_shift = shift / BITS_PER_WORD;
    const usize bit_shift = shift % BITS_PER_WORD;

    if (bit_shift == 0) {
      for (usize i = 0; i < WORD_COUNT - word_shift; ++i)
        m_words[i] = m_words[i + word_shift];
    } else {
      for (usize i = 0; i < WORD_COUNT - word_shift; ++i) {
        const word_type low = m_words[i + word_shift] >> bit_shift;
        const word_type high = i + word_shift + 1 >= WORD_COUNT
                                   ? word_type{0}
                                   : m_words[i + word_shift + 1]
                                         << (BITS_PER_WORD - bit_shift);
        m_words[i] = low | high;
      }
    }
    for (usize i = WORD_COUNT - word_shift; i < WORD_COUNT; ++i)
      m_words[i] = 0;

    return *this;
  }

  [[nodiscard]] friend constexpr bit_array
  operator&(bit_array lhs, const bit_array &rhs) noexcept {
    return lhs &= rhs;
  }
  [[nodiscard]] friend constexpr bit_array
  operator|(bit_array lhs, const bit_array &rhs) noexcept {
    return lhs |= rhs;
  }
  [[nodiscard]] friend constexpr bit_array
  operator^(bit_array lhs, const bit_array &rhs) noexcept {
    return lhs ^= rhs;
  }
  [[nodiscard]] friend constexpr bit_array
  operator<<(bit_array lhs, const usize shift) noexcept {
    return lhs <<= shift;
  }
  [[nodiscard]] friend constexpr bit_array
  operator>>(bit_array lhs, const usize shift) noexcept {
    return lhs >>= shift;
  }

  [[nodiscard]] constexpr bool
  operator==(const bit_array &other) const noexcept {
    for (usize i = 0; i < WORD_COUNT; ++i)
      if (m_words[i] != other.m_words[i])
        return false;
    return true;
  }

  [[nodiscard]] constexpr const u64 *data() const noexcept { return m_words; }
  [[nodiscard]] static constexpr usize word_count() noexcept {
    return WORD_COUNT;
  }

private:
  /// Clears the padding bits above N in the final word.
  constexpr void trim() noexcept { m_words[WORD_COUNT - 1] &= TAIL_MASK; }

  /// Lowest set bit at or above @p pos, scanning a word at a time.
  [[nodiscard]] constexpr usize scan_from(const usize pos) const noexcept {
    if (pos >= N)
      return npos;

    usize word_index = pos / BITS_PER_WORD;
    // Mask off the bits below pos in the first word we look at.
    word_type word =
        m_words[word_index] & (~word_type{0} << (pos % BITS_PER_WORD));

    for (;;) {
      if (word != 0)
        return word_index * BITS_PER_WORD +
               nx::cast<usize>(std::countr_zero(word));
      if (++word_index >= WORD_COUNT)
        return npos;
      word = m_words[word_index];
    }
  }

  word_type m_words[WORD_COUNT] = {};
};

}
