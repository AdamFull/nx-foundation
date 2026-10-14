#pragma once

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/hash.h"
#include "core/foundation/serialization/json.h"

namespace nx::json::detail {

/// Finds the members of an object under construction by key. Small objects
/// are scanned; past a threshold the keys are indexed by hash, so building an
/// object of n members stays linear rather than quadratic.
class member_index {
public:
  /// Existing member with @p key, or null.
  [[nodiscard]] value *find(object &members, const string_view key) const {
    if (!m_indexed) {
      for (member &m : members)
        if (m.key.view() == key)
          return &m.val;
      return nullptr;
    }
    const auto it = m_index.find(hash_fnv1a64(key.data(), key.size()));
    if (it == m_index.end())
      return nullptr;
    member &m = members[it->second];
    if (m.key.view() == key)
      return &m.val;
    // Two distinct keys hashed the same. Astronomically rare for a 64-bit
    // hash, but it must not report "absent" and duplicate the member.
    for (member &candidate : members)
      if (candidate.key.view() == key)
        return &candidate.val;
    return nullptr;
  }

  /// Records the member just appended to @p members.
  void added(const object &members) {
    if (m_indexed) {
      note(members.back(), members.size() - 1);
      return;
    }
    if (members.size() < INDEX_THRESHOLD)
      return;
    m_index.reserve(members.size() * 2);
    for (usize i = 0; i < members.size(); ++i)
      note(members[i], i);
    m_indexed = true;
  }

private:
  static constexpr usize INDEX_THRESHOLD = 32;

  void note(const member &m, const usize at) {
    m_index.insert_or_assign(hash_fnv1a64(m.key.data(), m.key.size()),
                             nx::cast<u32>(at));
  }

  hash_map<u64, u32> m_index;
  bool m_indexed = false;
};

} // namespace nx::json::detail
