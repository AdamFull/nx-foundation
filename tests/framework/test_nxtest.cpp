#include "framework/nxtest.h"

#include <algorithm>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

/// Says, when shown, whether it is still alive: a check that shows one after
/// it is destroyed shows "gone".
class Marked {
public:
  Marked() : m_id(++s_next) { live().push_back(m_id); }
  Marked(const Marked &) : Marked() {}
  Marked(Marked &&) noexcept : Marked() {}
  Marked &operator=(const Marked &) = delete;
  ~Marked() { std::erase(live(), m_id); }

  explicit operator bool() const { return false; }

  friend std::ostream &operator<<(std::ostream &out, const Marked &marked) {
    const bool alive =
        std::find(live().begin(), live().end(), marked.m_id) != live().end();
    return out << (alive ? "alive" : "gone");
  }

private:
  static std::vector<int> &live() {
    static std::vector<int> ids;
    return ids;
  }
  static inline int s_next = 0;
  int m_id;
};

} // namespace

TEST_CASE("nxtest: a temporary checked alone is still there when its value "
          "is shown") {
  static_assert(std::is_same_v<decltype(nxtest::decomposer{} << Marked()),
                               nxtest::lhs_expr<Marked>>);
  const Marked kept;
  static_assert(std::is_same_v<decltype(nxtest::decomposer{} << kept),
                               nxtest::lhs_expr<const Marked &>>);

  // A failed check shows its value after the statement that made it.
  const auto alone = (nxtest::decomposer{} << Marked());
  CHECK_FALSE(alone.value);
  CHECK(alone.message() == "alive");
}

TEST_CASE("nxtest: a move-only temporary compares and is shown") {
  const auto compared =
      (nxtest::decomposer{} << std::make_unique<int>(3) != nullptr);
  CHECK(compared.value);
  CHECK(compared.message().find("!= nullptr") != std::string::npos);
}
