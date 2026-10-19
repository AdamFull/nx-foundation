#include "framework/nxtest.h"

#include "core/foundation/core/foundation.h"

#include <cstring>
#include <limits>
#include <thread>

namespace {

struct Base {
  virtual ~Base() = default;
  int b = 1;
};
struct Derived : Base {
  int d = 2;
};
struct Other : Base {
  int o = 3;
};

struct Plain {
  int x;
  int y;
};

int live_count = 0;

struct Tracked {
  int value;
  Tracked() : value(0) { ++live_count; }
  explicit Tracked(int v) : value(v) { ++live_count; }
  Tracked(const Tracked &o) : value(o.value) { ++live_count; }
  ~Tracked() { --live_count; }
};

}

TEST_CASE("foundation: fixed-width type aliases have the advertised sizes") {
  CHECK(sizeof(i8) == 1);
  CHECK(sizeof(u8) == 1);
  CHECK(sizeof(i16) == 2);
  CHECK(sizeof(u16) == 2);
  CHECK(sizeof(i32) == 4);
  CHECK(sizeof(u32) == 4);
  CHECK(sizeof(i64) == 8);
  CHECK(sizeof(u64) == 8);
  CHECK(sizeof(f32) == 4);
  CHECK(sizeof(f64) == 8);
  CHECK(sizeof(usize) == sizeof(void *));
  CHECK(sizeof(isize) == sizeof(void *));

  CHECK(std::is_signed_v<i8>);
  CHECK(std::is_unsigned_v<u8>);
  CHECK(std::is_signed_v<i64>);
  CHECK(std::is_unsigned_v<u64>);
}

TEST_CASE("foundation: platform/arch/compiler macros are self-consistent") {
  int platforms = 0;
#if defined(NX_PLATFORM_WINDOWS)
  ++platforms;
#endif
#if defined(NX_PLATFORM_ANDROID)
  ++platforms;
#endif
#if defined(NX_PLATFORM_LINUX)
  ++platforms;
#endif
#if defined(NX_PLATFORM_IOS)
  ++platforms;
#endif
#if defined(NX_PLATFORM_MACOS)
  ++platforms;
#endif
#if defined(NX_PLATFORM_UNKNOWN)
  ++platforms;
#endif
  CHECK(platforms == 1);

  CHECK(NX_POINTER_SIZE == sizeof(void *));
  CHECK(NX_ARCH_BITS == NX_POINTER_SIZE * 8);
  CHECK(std::strlen(NX_PLATFORM_NAME) > 0);
  CHECK(std::strlen(NX_ARCH_NAME) > 0);
  CHECK(std::strlen(NX_COMPILER_NAME) > 0);

#if defined(NX_DEBUG) && defined(NX_RELEASE)
  FAIL("both NX_DEBUG and NX_RELEASE are defined");
#endif
#if !defined(NX_DEBUG) && !defined(NX_RELEASE)
  FAIL("neither NX_DEBUG nor NX_RELEASE is defined");
#endif
}

TEST_CASE("foundation: is_pow2 accepts only exact powers of two") {
  CHECK_FALSE(nx::is_pow2(0u));
  CHECK(nx::is_pow2(1u));
  CHECK(nx::is_pow2(2u));
  CHECK_FALSE(nx::is_pow2(3u));
  CHECK(nx::is_pow2(4u));
  CHECK_FALSE(nx::is_pow2(6u));
  CHECK(nx::is_pow2(1024u));
  CHECK_FALSE(nx::is_pow2(1023u));
  CHECK_FALSE(nx::is_pow2(1025u));

  CHECK(nx::is_pow2(u64(1) << 63));
  CHECK_FALSE(nx::is_pow2(u64(3) << 62));
  CHECK(nx::is_pow2(u8(128)));
  CHECK_FALSE(nx::is_pow2(u8(255)));

  for (u32 i = 0; i < 32; ++i) {
    const u32 p = u32(1) << i;
    CHECK(nx::is_pow2(p));
    if (p > 2)
      CHECK_FALSE(nx::is_pow2(p - 1));
  }

  static_assert(nx::is_pow2(64u));
  static_assert(!nx::is_pow2(0u));
}

TEST_CASE(
    "foundation: next_pow2 rounds up and is idempotent on powers of two") {
  CHECK(nx::next_pow2(0u) == 1u);
  CHECK(nx::next_pow2(1u) == 1u);
  CHECK(nx::next_pow2(2u) == 2u);
  CHECK(nx::next_pow2(3u) == 4u);
  CHECK(nx::next_pow2(5u) == 8u);
  CHECK(nx::next_pow2(1000u) == 1024u);
  CHECK(nx::next_pow2(1024u) == 1024u);
  CHECK(nx::next_pow2(1025u) == 2048u);

  CHECK(nx::next_pow2(u64(1) << 62) == (u64(1) << 62));
  CHECK(nx::next_pow2((u64(1) << 62) + 1) == (u64(1) << 63));

  for (u32 x = 1; x < 5000; ++x) {
    const u32 p = nx::next_pow2(x);
    CHECK(nx::is_pow2(p));
    CHECK(p >= x);
    CHECK(p / 2 < x);
  }

  static_assert(nx::next_pow2(17u) == 32u);
}

TEST_CASE(
    "foundation: align_up and align_down round to the requested boundary") {
  CHECK(nx::align_up(0u, 16u) == 0u);
  CHECK(nx::align_up(1u, 16u) == 16u);
  CHECK(nx::align_up(15u, 16u) == 16u);
  CHECK(nx::align_up(16u, 16u) == 16u);
  CHECK(nx::align_up(17u, 16u) == 32u);

  CHECK(nx::align_down(0u, 16u) == 0u);
  CHECK(nx::align_down(15u, 16u) == 0u);
  CHECK(nx::align_down(16u, 16u) == 16u);
  CHECK(nx::align_down(31u, 16u) == 16u);
  CHECK(nx::align_down(32u, 16u) == 32u);

  CHECK(nx::align_up(12345u, 1u) == 12345u);
  CHECK(nx::align_down(12345u, 1u) == 12345u);

  for (u32 a = 1; a <= 256; a <<= 1) {
    for (u32 x = 0; x < 600; ++x) {
      const u32 up = nx::align_up(x, a);
      const u32 down = nx::align_down(x, a);
      CHECK(up % a == 0);
      CHECK(down % a == 0);
      CHECK(up >= x);
      CHECK(down <= x);
      CHECK(up - down <= a);
      if (x % a == 0)
        CHECK(up == x);
      else
        CHECK(up == down + a);
    }
  }

  static_assert(nx::align_up(usize(5), usize(8)) == 8);
  static_assert(nx::align_down(usize(5), usize(8)) == 0);
}

TEST_CASE("foundation: min/max/clamp") {
  CHECK(nx::min(1, 2) == 1);
  CHECK(nx::min(2, 1) == 1);
  CHECK(nx::min(3, 3) == 3);
  CHECK(nx::max(1, 2) == 2);
  CHECK(nx::max(2, 1) == 2);
  CHECK(nx::max(3, 3) == 3);

  CHECK(nx::min(-5, 5) == -5);
  CHECK(nx::max(-5, 5) == 5);
  CHECK(nx::min(1.5f, 2.5f) == 1.5f);
  CHECK(nx::max(1.5, 2.5) == 2.5);

  CHECK(nx::clamp(5, 0, 10) == 5);
  CHECK(nx::clamp(-1, 0, 10) == 0);
  CHECK(nx::clamp(11, 0, 10) == 10);
  CHECK(nx::clamp(0, 0, 10) == 0);
  CHECK(nx::clamp(10, 0, 10) == 10);
  CHECK(nx::clamp(0.5, 0.0, 1.0) == 0.5);

  static_assert(nx::clamp(42, 0, 10) == 10);
  static_assert(nx::min(u8(3), u8(4)) == 3);
}

TEST_CASE("foundation: array_size counts elements, not bytes") {
  int a[7]{};
  double d[3]{};
  char c[1]{};
  CHECK(nx::array_size(a) == 7);
  CHECK(nx::array_size(d) == 3);
  CHECK(nx::array_size(c) == 1);

  static constexpr int k[]{1, 2, 3, 4, 5};
  static_assert(nx::array_size(k) == 5);
}

TEST_CASE("foundation: str_copy truncates and always NUL-terminates") {
  char buf[8];

  std::memset(buf, 'X', sizeof(buf));
  CHECK(nx::str_copy(buf, sizeof(buf), "abc") == 3);
  CHECK(std::strcmp(buf, "abc") == 0);

  std::memset(buf, 'X', sizeof(buf));
  CHECK(nx::str_copy(buf, sizeof(buf), "1234567") == 7);
  CHECK(std::strcmp(buf, "1234567") == 0);

  std::memset(buf, 'X', sizeof(buf));
  CHECK(nx::str_copy(buf, sizeof(buf), "12345678") == 7);
  CHECK(std::strcmp(buf, "1234567") == 0);
  CHECK(buf[7] == '\0');

  std::memset(buf, 'X', sizeof(buf));
  CHECK(nx::str_copy(buf, sizeof(buf), "") == 0);
  CHECK(buf[0] == '\0');

  std::memset(buf, 'X', sizeof(buf));
  CHECK(nx::str_copy(buf, 1, "hello") == 0);
  CHECK(buf[0] == '\0');
  CHECK(buf[1] == 'X');

  std::memset(buf, 'X', sizeof(buf));
  CHECK(nx::str_copy(buf, 0, "hello") == 0);
  CHECK(buf[0] == 'X');
}

TEST_CASE("foundation: cast forwards and converts") {
  CHECK(nx::cast<int>(3.9) == 3);
  CHECK(nx::cast<int>(-3.9) == -3);
  CHECK(nx::cast<u8>(300) == u8(300 & 0xff));
  CHECK(nx::cast<f64>(1) == 1.0);

  static_assert(nx::cast<i32>(7u) == 7);

  Derived d;
  Base *b = nx::cast<Base *>(&d);
  CHECK(b == static_cast<Base *>(&d));
}

TEST_CASE("foundation: safe_cast downcasts pointers and references") {
  Derived d;
  Base *b = &d;

  Derived *dp = nx::safe_cast<Derived *>(b);
  REQUIRE(dp != nullptr);
  CHECK(dp->d == 2);

  Base *null_base = nullptr;
  CHECK(nx::safe_cast<Derived *>(null_base) == nullptr);

  Derived &dr = nx::safe_cast<Derived &>(*b);
  CHECK(dr.d == 2);

  Plain p{1, 2};
  void *vp = &p;
  CHECK(static_cast<Plain *>(vp) == &p);
}

TEST_CASE("foundation: mem_alloc honours size and alignment") {
  for (usize align : {usize(8), usize(16), usize(32), usize(64), usize(256)}) {
    void *p = nx::mem_alloc(1000, align);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(p) % align == 0);
    std::memset(p, 0xAB, 1000);
    CHECK(static_cast<unsigned char *>(p)[999] == 0xAB);
    nx::mem_free(p);
  }
}

TEST_CASE("foundation: mem_realloc preserves contents and mem_free(nullptr) is "
          "a no-op") {
  auto *p = static_cast<unsigned char *>(nx::mem_alloc(64, 16));
  REQUIRE(p != nullptr);
  for (int i = 0; i < 64; ++i)
    p[i] = static_cast<unsigned char>(i);

  auto *q = static_cast<unsigned char *>(nx::mem_realloc(p, 4096, 16));
  REQUIRE(q != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(q) % 16 == 0);
  for (int i = 0; i < 64; ++i)
    CHECK(q[i] == static_cast<unsigned char>(i));
  nx::mem_free(q);

  CHECK_NOTHROW(nx::mem_free(nullptr));
}

TEST_CASE("foundation: allocate/release construct and destroy exactly once") {
  live_count = 0;

  Tracked *t = nx::allocate<Tracked>(42);
  REQUIRE(t != nullptr);
  CHECK(t->value == 42);
  CHECK(live_count == 1);
  nx::release(t);
  CHECK(live_count == 0);

  Tracked *u = nx::allocate<Tracked>();
  REQUIRE(u != nullptr);
  CHECK(u->value == 0);
  CHECK(live_count == 1);
  nx::release(u);
  CHECK(live_count == 0);

  CHECK_NOTHROW(nx::release<Tracked>(nullptr));
  CHECK(live_count == 0);

  auto *big = nx::allocate<Plain, 64>();
  REQUIRE(big != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(big) % 64 == 0);
  nx::release(big);
}

TEST_CASE("foundation: allocate_array/release_array run every constructor and "
          "destructor") {
  live_count = 0;

  Tracked *arr = nx::allocate_array<Tracked>(10);
  REQUIRE(arr != nullptr);
  CHECK(live_count == 10);
  for (usize i = 0; i < 10; ++i)
    CHECK(arr[i].value == 0);
  nx::release_array(arr, 10);
  CHECK(live_count == 0);

  CHECK(nx::allocate_array<Tracked>(0) == nullptr);
  CHECK_NOTHROW(nx::release_array<Tracked>(nullptr, 0));
  CHECK(live_count == 0);

  int *ints = nx::allocate_array<int>(16);
  REQUIRE(ints != nullptr);
  for (int i = 0; i < 16; ++i)
    ints[i] = i;
  CHECK(ints[15] == 15);
  nx::release_array(ints, 16);
}

TEST_CASE("foundation: DefaultAllocator round-trips through std::vector") {
  std::vector<int, nx::DefaultAllocator<int>> v;
  for (int i = 0; i < 500; ++i)
    v.push_back(i);
  CHECK(v.size() == 500);
  CHECK(v[0] == 0);
  CHECK(v[499] == 499);

  nx::DefaultAllocator<int> a;
  nx::DefaultAllocator<double> b(a);
  CHECK(a == nx::DefaultAllocator<int>(b));
}

TEST_CASE("foundation: nx::vector and nx::make_shared work with the engine "
          "allocator") {
  nx::vector<int> v;
  for (int i = 0; i < 1000; ++i)
    v.push_back(i * 2);
  CHECK(v.size() == 1000);
  CHECK(v.back() == 1998);
  v.clear();
  CHECK(v.empty());

  live_count = 0;
  {
    nx::shared_ptr<Tracked> sp = nx::make_shared<Tracked>(7);
    REQUIRE(sp != nullptr);
    CHECK(sp->value == 7);
    CHECK(live_count == 1);
    CHECK(sp.use_count() == 1);

    {
      nx::shared_ptr<Tracked> sp2 = sp;
      CHECK(sp2.use_count() == 2);
      CHECK(sp2.get() == sp.get());
    }
    CHECK(sp.use_count() == 1);
  }
  CHECK(live_count == 0);
}

TEST_CASE("shared_ptr: one pointer wide, and the count sits in front of the "
          "object") {
  CHECK(sizeof(nx::shared_ptr<Tracked>) == sizeof(void *));
  CHECK(sizeof(nx::shared_ptr<void>) == sizeof(void *));

  const nx::shared_ptr<Tracked> sp = nx::make_shared<Tracked>(3);
  const auto *const object = reinterpret_cast<const u8 *>(sp.get());
  const auto *const head =
      reinterpret_cast<const u8 *>(nx::detail::shared_header(sp.get()));
  CHECK(object - head == sizeof(nx::detail::SharedHeader));
}

TEST_CASE("shared_ptr: an over-aligned type still finds its header") {
  struct alignas(64) Wide {
    int value = 9;
  };

  const nx::shared_ptr<Wide> sp = nx::make_shared<Wide>();
  REQUIRE(sp != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(sp.get()) % 64 == 0);
  CHECK(sp->value == 9);
  CHECK(sp.use_count() == 1);
}

TEST_CASE("shared_ptr: erasing to void keeps the object alive and destroys it "
          "as itself") {
  live_count = 0;
  {
    nx::shared_ptr<void> erased;
    {
      const nx::shared_ptr<Tracked> sp = nx::make_shared<Tracked>(11);
      erased = sp;
      CHECK(erased.get() == sp.get());
      CHECK(sp.use_count() == 2);
    }
    CHECK(live_count == 1);
    CHECK(erased.use_count() == 1);
  }
  CHECK(live_count == 0);
}

TEST_CASE("shared_ptr: assignment and reset") {
  live_count = 0;
  nx::shared_ptr<Tracked> a = nx::make_shared<Tracked>(1);
  nx::shared_ptr<Tracked> b = nx::make_shared<Tracked>(2);
  CHECK(live_count == 2);

  a = b;
  CHECK(live_count == 1);
  CHECK(a->value == 2);
  CHECK(a.use_count() == 2);

  const nx::shared_ptr<Tracked> *self = &a;
  a = *self;
  CHECK(live_count == 1);
  CHECK(a.use_count() == 2);

  a = std::move(b);
  CHECK(a.use_count() == 1);
  CHECK_FALSE(static_cast<bool>(b));

  a.reset();
  CHECK(live_count == 0);
  CHECK(a == nullptr);
  CHECK(a.use_count() == 0);
}

TEST_CASE("shared_ptr: a const view shares the count with the mutable one") {
  const nx::shared_ptr<nx::vector<int>> values =
      nx::make_shared<nx::vector<int>>();
  values->push_back(4);

  const nx::shared_ptr<const nx::vector<int>> view = values;
  CHECK(values.use_count() == 2);
  CHECK(view->size() == 1u);
  CHECK(view.get() == values.get());
}

TEST_CASE("shared_ptr: the count survives threads copying and dropping it") {
  live_count = 0;
  {
    const nx::shared_ptr<Tracked> sp = nx::make_shared<Tracked>(5);
    std::atomic<u64> seen{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
      threads.emplace_back([&sp, &seen] {
        u64 sum = 0;
        for (int i = 0; i < 20000; ++i) {
          const nx::shared_ptr<Tracked> copy = sp;
          const nx::shared_ptr<void> erased = copy;
          sum += static_cast<u64>(copy->value);
        }
        seen.fetch_add(sum, std::memory_order_relaxed);
      });
    for (std::thread &t : threads)
      t.join();

    CHECK(seen.load() == 8ull * 20000ull * 5ull);
    CHECK(sp.use_count() == 1u);
    CHECK(live_count == 1);
  }
  CHECK(live_count == 0);
}

namespace {

struct Ledger {
  int allocations = 0;
  int frees = 0;
  usize bytes = 0;
};

template <typename T> struct CountingAllocator {
  using value_type = T;

  explicit CountingAllocator(Ledger *ledger) noexcept : book(ledger) {}
  template <typename U>
  CountingAllocator(const CountingAllocator<U> &other) noexcept
      : book(other.book) {}

  [[nodiscard]] T *allocate(const usize n) {
    ++book->allocations;
    book->bytes += n * sizeof(T);
    return static_cast<T *>(nx::mem_alloc(n * sizeof(T), alignof(T)));
  }
  void deallocate(T *p, usize) noexcept {
    ++book->frees;
    nx::mem_free(p);
  }
  template <typename U>
  bool operator==(const CountingAllocator<U> &other) const noexcept {
    return book == other.book;
  }

  Ledger *book;
};

}

TEST_CASE("allocate_shared: the block comes from the allocator and goes back "
          "to it") {
  live_count = 0;
  Ledger book;
  {
    const nx::shared_ptr<Tracked> sp =
        nx::allocate_shared<Tracked>(CountingAllocator<Tracked>(&book), 21);
    CHECK(book.allocations == 1);
    CHECK(book.frees == 0);
    CHECK(book.bytes >= sizeof(Tracked) + sizeof(nx::detail::SharedHeader));
    CHECK(sp->value == 21);
    CHECK(live_count == 1);

    const nx::shared_ptr<void> erased = sp;
    CHECK(erased.use_count() == 2u);
  }
  CHECK(book.allocations == 1);
  CHECK(book.frees == 1);
  CHECK(live_count == 0);
}

TEST_CASE("allocate_shared: two allocators do not free through each other") {
  Ledger first;
  Ledger second;
  {
    const nx::shared_ptr<Tracked> a =
        nx::allocate_shared<Tracked>(CountingAllocator<Tracked>(&first), 1);
    const nx::shared_ptr<Tracked> b =
        nx::allocate_shared<Tracked>(CountingAllocator<Tracked>(&second), 2);
    CHECK(first.allocations == 1);
    CHECK(second.allocations == 1);
  }
  CHECK(first.frees == 1);
  CHECK(second.frees == 1);
}

TEST_CASE("allocate_shared: a stateful allocator still lands the object on its "
          "own alignment") {
  struct alignas(64) Wide {
    int value = 7;
  };

  Ledger book;
  {
    const nx::shared_ptr<Wide> sp =
        nx::allocate_shared<Wide>(CountingAllocator<Wide>(&book));
    CHECK(reinterpret_cast<uintptr_t>(sp.get()) % 64 == 0);
    CHECK(sp->value == 7);
  }
  CHECK(book.frees == 1);
}

TEST_CASE("life_token: a watch outlives the token and says so") {
  nx::life_watch watch;
  CHECK_FALSE(watch.alive());
  {
    const nx::life_token token;
    watch = token.watch();
    CHECK(watch.alive());
    CHECK(static_cast<bool>(watch));
  }
  CHECK_FALSE(watch.alive());
}

namespace {

struct Pad {
  virtual ~Pad() = default;
  char bytes[64] = {};
};
struct Iface {
  virtual ~Iface() = default;
};
struct Impl final : Pad, Iface {};

struct CountingDeleter {
  int *calls = nullptr;

  void operator()(int *const value) const noexcept {
    ++*calls;
    nx::release(value);
  }
};

struct alignas(64) OverAligned {
  char bytes[8] = {};
};

struct NonVirtualBase {
  int value = 0;
};
struct NonVirtualDerived : NonVirtualBase {};

class FactoryOnly {
public:
  static nx::unique_ptr<FactoryOnly> make(const int value) {
    return nx::make_unique<FactoryOnly>(value);
  }
  int value;

private:
  explicit FactoryOnly(const int v) : value(v) {}

  template <typename T, typename... Args>
  friend nx::unique_ptr<T> nx::make_unique(Args &&...);
};

struct Counted {
  static inline int alive = 0;
  Counted() { ++alive; }
  ~Counted() { --alive; }
};

template <typename P>
concept resets_from_raw = requires(P p, int *raw) { p.reset(raw); };

} // namespace

TEST_CASE("foundation: nx::unique_ptr refuses raw pointers") {
  static_assert(!std::is_constructible_v<nx::unique_ptr<int>, int *>);
  static_assert(!std::is_convertible_v<int *, nx::unique_ptr<int>>);
  static_assert(!resets_from_raw<nx::unique_ptr<int>>);
  static_assert(resets_from_raw<nx::unique_ptr<int, CountingDeleter>>);
  static_assert(sizeof(nx::unique_ptr<int>) == sizeof(int *));

  nx::unique_ptr<int> empty;
  CHECK(empty == nullptr);
  CHECK_FALSE(empty);
}

TEST_CASE("foundation: nx::adopt_unique takes back a released pointer") {
  nx::unique_ptr<Counted> owned = nx::make_unique<Counted>();
  CHECK(Counted::alive == 1);
  Counted *const raw = owned.release();
  CHECK(owned == nullptr);
  {
    const nx::unique_ptr<Counted> back = nx::adopt_unique(raw);
    CHECK(back.get() == raw);
  }
  CHECK(Counted::alive == 0);

  Counted *const allocated = nx::allocate<Counted>();
  nx::unique_ptr<Counted> from_allocate = nx::adopt_unique(allocated);
  CHECK(Counted::alive == 1);
  from_allocate.reset();
  CHECK(Counted::alive == 0);
}

TEST_CASE("foundation: nx::make_unique honours over-alignment") {
  for (int i = 0; i < 16; ++i) {
    const nx::unique_ptr<OverAligned> p = nx::make_unique<OverAligned>();
    CHECK(reinterpret_cast<uintptr_t>(p.get()) % alignof(OverAligned) == 0);
  }
}

TEST_CASE("foundation: nx::make_unique reaches a befriended private ctor") {
  const nx::unique_ptr<FactoryOnly> made = FactoryOnly::make(42);
  REQUIRE(made != nullptr);
  CHECK(made->value == 42);
}

TEST_CASE("foundation: nx::unique_ptr converts to a virtual base") {
  const Impl impl;
  const void *object = &impl;
  const void *subobject = static_cast<const Iface *>(&impl);
  CHECK(object != subobject);

  static_assert(
      std::is_convertible_v<nx::unique_ptr<Impl>, nx::unique_ptr<Iface>>);
  static_assert(
      std::is_convertible_v<nx::unique_ptr<Impl>, nx::unique_ptr<const Impl>>);
  static_assert(!std::is_convertible_v<nx::unique_ptr<NonVirtualDerived>,
                                       nx::unique_ptr<NonVirtualBase>>);
  static_assert(
      !std::is_convertible_v<nx::unique_ptr<Iface>, nx::unique_ptr<Impl>>);

  nx::unique_ptr<Iface> owned = nx::make_unique<Impl>();
  CHECK(owned != nullptr);
  CHECK(static_cast<const void *>(owned.get()) != nullptr);
  owned = nx::make_unique<Impl>();
  owned.reset();
  CHECK(owned == nullptr);
}

TEST_CASE("foundation: nx::unique_ptr accepts a custom deleter") {
  int calls = 0;
  {
    nx::unique_ptr<int, CountingDeleter> owned(nx::allocate<int>(7),
                                               CountingDeleter{&calls});
    REQUIRE(owned != nullptr);
    CHECK(*owned == 7);
  }
  CHECK(calls == 1);
}

TEST_CASE("foundation: cpu_relax is callable and harmless") {
  for (int i = 0; i < 8; ++i)
    nx::detail::cpu_relax();
  CHECK(true);
}

namespace {

struct ListNode {
  nx::shared_ptr<ListNode> next;
  int value = 0;
  explicit ListNode(const int v) : value(v) {}
};

struct alignas(64) WideSlot {
  float lanes[16] = {};
};

struct ThrowsOnThird {
  static inline int built = 0;
  static inline int alive = 0;
  ThrowsOnThird() {
    if (++built == 3)
      throw 3;
    ++alive;
  }
  ~ThrowsOnThird() { --alive; }
};

} // namespace

TEST_CASE(
    "foundation: shared_ptr move-assign from a member of its own target") {
  nx::shared_ptr<ListNode> head = nx::make_shared<ListNode>(1);
  head->next = nx::make_shared<ListNode>(2);
  head->next->next = nx::make_shared<ListNode>(3);
  head = std::move(head->next);
  REQUIRE(head != nullptr);
  CHECK(head->value == 2);
  CHECK(head.use_count() == 1);
  head = std::move(head->next);
  REQUIRE(head != nullptr);
  CHECK(head->value == 3);
  head.reset();
  CHECK(head == nullptr);
}

TEST_CASE(
    "foundation: shared_ptr copy-assign from a member of its own target") {
  nx::shared_ptr<ListNode> head = nx::make_shared<ListNode>(1);
  head->next = nx::make_shared<ListNode>(2);
  head = head->next;
  REQUIRE(head != nullptr);
  CHECK(head->value == 2);
  CHECK(head.use_count() == 1);
}

TEST_CASE("foundation: nx::Allocator honours over-alignment") {
  static_assert(std::is_same_v<nx::Allocator<int>, nx::DefaultAllocator<int>>);
  nx::vector<WideSlot> slots;
  for (int i = 0; i < 33; ++i) {
    slots.emplace_back();
    CHECK(reinterpret_cast<uintptr_t>(slots.data()) % alignof(WideSlot) == 0);
  }
}

TEST_CASE("foundation: DefaultAllocator rejects a size that overflows") {
  nx::DefaultAllocator<u64> alloc;
  CHECK_THROWS_AS((void)alloc.allocate(alloc.max_size() + 1),
                  std::bad_array_new_length);
}

TEST_CASE("foundation: allocate_array rejects a size that overflows") {
  CHECK(nx::allocate_array<u64>(usize(-1) / 4) == nullptr);
}

TEST_CASE("foundation: allocate_array destroys what it built when one throws") {
  ThrowsOnThird::built = 0;
  ThrowsOnThird::alive = 0;
  CHECK_THROWS_AS((void)nx::allocate_array<ThrowsOnThird>(5), int);
  CHECK(ThrowsOnThird::alive == 0);
}

TEST_CASE("foundation: mem_alloc honours alignments below a pointer") {
  for (const usize align : {usize(1), usize(2), usize(4)}) {
    void *const p = nx::mem_alloc(3, align);
    REQUIRE(p != nullptr);
    nx::mem_free(p);
  }
}
