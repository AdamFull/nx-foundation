#pragma once

/**
 * @file id_string.h
 * @brief Interned, hash-identified string (nx::id_string).
 *
 * An nx::id_string is an 8-byte, trivially copyable value that identifies a
 * string by a 64-bit FNV-1a hash of its bytes. Two id_strings are equal when
 * their ids are equal, so comparison, ordering, and hashing are integer
 * operations independent of the string length. This is the design used by
 * stingray/bitsquid's idstring and, conceptually, by Unreal's FName.
 *
 * Unlike a bare hash, the original bytes are recoverable. Every id_string built
 * at run time registers its bytes in a process-wide, thread-safe intern pool,
 * so view(), c_str(), and to_string() return the original text in every build
 * configuration, not only in debug.
 *
 * The id is a pure function of the bytes: it is stable across runs and across
 * machines and can be computed at compile time. This makes it suitable as a
 * baked key in asset pipelines and as a constant-expression switch label:
 *
 * @code
 * switch (event.name.id()) {
 *   case nx::id_string::from_literal("jump").id(): on_jump(); break;
 *   case nx::id_string::from_literal("fire").id(): on_fire(); break;
 * }
 * @endcode
 *
 * Identity is case-sensitive and operates on raw UTF-8 bytes; no normalization
 * is performed. Ordering via operator<=> compares ids, which is fast but not
 * lexicographic; use id_string::lexical_less to sort by text.
 */

#include "foundation/core/foundation.h"
#include "foundation/core/hash.h"
#include "foundation/containers/hash_map.h"
#include "foundation/strings/utf8_string.h"
#include "foundation/threading/sync.h"
#include "foundation/strings/utf8_string_view.h"

#include <cassert>
#include <compare>
#include <cstdio>
#include <cstring>
#include <format>

namespace nx {

namespace detail {

[[nodiscard]] constexpr u64 id_string_hash(string_view s) noexcept {
  if (s.empty())
    return 0;
  const u64 h = hash_fnv1a64(s.data(), s.size());
  return h == 0 ? ~u64{0} : h;
}

/**
 * @brief Process-wide intern pool backing nx::id_string.
 *
 * Maps each id to the canonical bytes that produced it. Bytes are kept in an
 * append-only arena of fixed-size pages so that the const char* handed out by
 * lookups stays valid for the entire lifetime of the program; entries are never
 * moved or freed. A shared_mutex allows concurrent lookups and serializes the
 * rare insert. Reach it through id_string_intern_pool().
 */
class id_string_pool {
public:
  struct entry {
    const char *data;
    u32 length;
  };

  struct stats {
    usize count;
    usize bytes_used;
    usize page_count;
  };

  void register_bytes(u64 id, string_view s) {
    if (id == 0)
      return;
    {
      scoped_shared_lock read(mutex_);
      if (const auto it = map_.find(id); it != map_.end()) {
        assert(string_view(it->second.data, it->second.length) == s &&
               "nx::id_string: 64-bit hash collision between distinct strings");
        return;
      }
    }
    scoped_lock write(mutex_);
    if (const auto it = map_.find(id); it != map_.end()) {
      assert(string_view(it->second.data, it->second.length) == s &&
             "nx::id_string: 64-bit hash collision between distinct strings");
      return;
    }
    map_.emplace(id, store(s));
  }

  [[nodiscard]] u64 intern(string_view s) {
    const u64 id = id_string_hash(s);
    register_bytes(id, s);
    return id;
  }

  [[nodiscard]] bool lookup(u64 id, entry &out) const noexcept {
    if (id == 0) {
      out = entry{"", 0};
      return true;
    }
    scoped_shared_lock read(mutex_);
    const auto it = map_.find(id);
    if (it == map_.end())
      return false;
    out = it->second;
    return true;
  }

  [[nodiscard]] stats snapshot() const noexcept {
    scoped_shared_lock read(mutex_);
    usize used = 0;
    for (const page &p : pages_)
      used += p.used;
    return stats{map_.size(), used, pages_.size()};
  }

private:
  static constexpr usize kPageSize = 64 * 1024;

  struct page {
    char *base;
    usize used;
    usize capacity;
  };

  entry store(string_view s) {
    const usize need = s.size() + 1;
    if (pages_.empty() || pages_.back().capacity - pages_.back().used < need) {
      const usize capacity = nx::max(kPageSize, need);
      char *base = static_cast<char *>(mem_alloc(capacity));
      pages_.push_back(page{base, 0, capacity});
    }
    page &p = pages_.back();
    char *dst = p.base + p.used;
    std::memcpy(dst, s.data(), s.size());
    dst[s.size()] = '\0';
    p.used += need;
    return entry{dst, nx::cast<u32>(s.size())};
  }

  mutable rw_lock mutex_;
  hash_map<u64, entry> map_;
  vector<page> pages_;
};

[[nodiscard]] inline id_string_pool &id_string_intern_pool() noexcept {
  static id_string_pool *pool = new id_string_pool();
  return *pool;
}

} // namespace detail

/**
 * @brief Interned string identified by a 64-bit hash; see id_string.h.
 *
 * Holds a single u64 id. The empty id_string has id 0 and is the default and
 * the canonical form of the empty string. Run-time construction interns the
 * bytes so the text is always recoverable; compile-time construction via
 * from_literal computes the id only.
 */
class id_string {
public:
  using id_type = u64;

  constexpr id_string() noexcept = default;

  constexpr id_string(string_view s) noexcept : id_(detail::id_string_hash(s)) {
    if (!std::is_constant_evaluated() && !s.empty())
      detail::id_string_intern_pool().register_bytes(id_, s);
  }

  constexpr id_string(const char *s) noexcept : id_string(string_view(s)) {}

  id_string(const string &s) noexcept : id_string(s.view()) {}

  /// Reconstruct from a previously serialized id without interning bytes.
  [[nodiscard]] static constexpr id_string from_id(id_type id) noexcept {
    id_string out;
    out.id_ = id;
    return out;
  }

  /// Compile-time construction. Computes the id only; bytes are not interned.
  [[nodiscard]] static consteval id_string
  from_literal(string_view s) noexcept {
    id_string out;
    out.id_ = detail::id_string_hash(s);
    return out;
  }

  [[nodiscard]] constexpr id_type id() const noexcept { return id_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return id_ == 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return id_ != 0;
  }

  /// True when the original bytes are present in the intern pool.
  [[nodiscard]] bool is_resolvable() const noexcept {
    detail::id_string_pool::entry e;
    return detail::id_string_intern_pool().lookup(id_, e);
  }

  /// Original bytes if known, otherwise an empty view.
  [[nodiscard]] string_view view() const noexcept {
    detail::id_string_pool::entry e;
    if (detail::id_string_intern_pool().lookup(id_, e))
      return string_view(e.data, e.length);
    return string_view{};
  }

  /// Null-terminated original bytes if known, otherwise "".
  [[nodiscard]] const char *c_str() const noexcept {
    detail::id_string_pool::entry e;
    if (detail::id_string_intern_pool().lookup(id_, e))
      return e.data;
    return "";
  }

  [[nodiscard]] string to_string() const { return string(view()); }

  void swap(id_string &o) noexcept {
    const id_type t = id_;
    id_ = o.id_;
    o.id_ = t;
  }

  [[nodiscard]] friend constexpr bool operator==(id_string a,
                                                 id_string b) noexcept {
    return a.id_ == b.id_;
  }
  [[nodiscard]] friend constexpr std::strong_ordering
  operator<=>(id_string a, id_string b) noexcept {
    return a.id_ <=> b.id_;
  }

  [[nodiscard]] friend constexpr bool operator==(id_string a,
                                                 string_view b) noexcept {
    return a.id_ == detail::id_string_hash(b);
  }

  /// Comparator that orders id_strings by their resolved text.
  struct lexical_less {
    [[nodiscard]] bool operator()(id_string a, id_string b) const noexcept {
      return a.view() < b.view();
    }
  };

private:
  id_type id_ = 0;
};

static_assert(sizeof(id_string) == sizeof(u64));
static_assert(std::is_trivially_copyable_v<id_string>);

NX_FORCE_INLINE void swap(id_string &a, id_string &b) noexcept { a.swap(b); }

/// Snapshot of intern-pool usage for diagnostics and budgeting.
[[nodiscard]] inline detail::id_string_pool::stats id_string_stats() noexcept {
  return detail::id_string_intern_pool().snapshot();
}

inline namespace literals {
/// Interns at run time and yields a resolvable id_string.
[[nodiscard]] inline id_string operator""_id(const char *s, usize n) {
  return id_string(string_view(s, n));
}
} // namespace literals

} // namespace nx

template <> struct std::hash<nx::id_string> {
  [[nodiscard]] std::size_t operator()(const nx::id_string &s) const noexcept {
    return nx::cast<std::size_t>(s.id());
  }
};

template <class CharT>
struct std::formatter<nx::id_string, CharT>
    : std::formatter<std::basic_string_view<CharT>, CharT> {
  template <class FormatContext>
  auto format(const nx::id_string &s, FormatContext &ctx) const {
    using Base = std::formatter<std::basic_string_view<CharT>, CharT>;
    if (s.empty())
      return Base::format({}, ctx);
    const nx::string_view v = s.view();
    if constexpr (std::is_same_v<CharT, char>) {
      if (!v.empty())
        return Base::format(v, ctx);
      char buf[24];
      const int len = std::snprintf(buf, sizeof(buf), "0x%016llx",
                                    nx::cast<unsigned long long>(s.id()));
      return Base::format(std::string_view(buf, nx::cast<std::size_t>(len)),
                          ctx);
    } else {
      const nx::string owned = s.to_string();
      const std::u32string wide = owned.u32string();
      return std::formatter<std::basic_string_view<CharT>, CharT>::format(
          std::basic_string_view<CharT>(
              reinterpret_cast<const CharT *>(wide.data()), wide.size()),
          ctx);
    }
  }
};