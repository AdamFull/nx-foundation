#include "simd/simd_test_util.h"

#include "core/foundation/simd/pipeline.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <limits>
#include <tuple>
#include <vector>

using namespace simd_test;

namespace {

// Every size up to several unrolled blocks: the unrolled loop, the one-chunk
// loop and the tail all run.
constexpr int max_size = 4 * 8 * 64 + 3;

template <class T> std::vector<T> sequence(int n, T start, T step) {
  std::vector<T> v(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    v[size_t(i)] = static_cast<T>(start + static_cast<T>(i) * step);
  return v;
}

template <class T>
int first_mismatch(const std::vector<T> &got, const std::vector<T> &want,
                   size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!same(got[i], want[i]))
      return int(i);
  return -1;
}

template <class P> int sizes_of(const P &p, int size_limit) {
  int bad = 0;
  for (int n = 0; n <= size_limit; n += n < 70 ? 1 : 37) {
    const auto a = sequence<int>(n, -100, 7);
    const auto b = sequence<int>(n, 3, -5);
    std::vector<int> o(size_t(n) + 4, 12345);
    const size_t wrote = p(a, b, sd::out(o));
    std::vector<int> want(o.size(), 12345);
    for (int i = 0; i < n; ++i)
      want[size_t(i)] = 3 * a[size_t(i)] - b[size_t(i)];
    bad += wrote != size_t(n);
    bad += first_mismatch(o, want, o.size()) != -1;
  }
  return bad;
}

} // namespace

TEST_CASE("simd pipeline: map stages fuse over every size") {
  const auto p = sd::pipeline([](auto x, auto y) { return 3 * x - y; });
  const int bad = sizes_of(p, max_size);
  CHECK(bad == 0);
  const int bad_one = sizes_of(
      sd::pipeline(sd::unroll<1>, [](auto x, auto y) { return 3 * x - y; }),
      max_size);
  CHECK(bad_one == 0);
  const int bad_odd =
      sizes_of(sd::pipeline(
                   sd::unroll<3>, sd::lanes<5>,
                   [](auto x, auto y) { return std::tuple(3 * x, y); },
                   [](auto x3, auto y) { return x3 - y; }),
               max_size);
  CHECK(bad_odd == 0);
}

TEST_CASE("simd pipeline: stages chain and hand on several values") {
  const auto p =
      sd::pipeline([](auto x) { return std::tuple(x + 1.f, x * 2.f); },
                   [](auto s, auto d) { return std::tuple(s * d, s - d); });
  for (int n : {0, 1, 5, 64, 133}) {
    const auto a = sequence<float>(n, -3.f, 0.25f);
    std::vector<float> o1(static_cast<size_t>(n)), o2(static_cast<size_t>(n));
    const size_t wrote = p(a, sd::out(o1, o2));
    CHECK(wrote == size_t(n));
    std::vector<float> w1(static_cast<size_t>(n)), w2(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const float s = a[size_t(i)] + 1.f, d = a[size_t(i)] * 2.f;
      w1[size_t(i)] = s * d;
      w2[size_t(i)] = s - d;
    }
    const int bad1 = first_mismatch(o1, w1, size_t(n));
    const int bad2 = first_mismatch(o2, w2, size_t(n));
    CHECK(bad1 == -1);
    CHECK(bad2 == -1);
  }
}

TEST_CASE("simd pipeline: element types convert at the edges") {
  std::vector<std::uint8_t> pixels(301);
  for (size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = std::uint8_t(i * 7);
  std::vector<float> unit(pixels.size());
  const auto to_unit = sd::pipeline([](auto p) {
    return sd::rebind_t<float, decltype(p)>(p) * (1.f / 255.f);
  });
  to_unit(pixels, sd::out(unit));
  int bad = 0;
  for (size_t i = 0; i < pixels.size(); ++i)
    bad += !same(unit[i], float(pixels[i]) * (1.f / 255.f));
  CHECK(bad == 0);

  std::vector<std::uint8_t> back(pixels.size());
  const auto to_bytes = sd::pipeline([](auto f) { return f * 255.f + 0.5f; });
  to_bytes(unit, sd::out(sd::flag_convert, back));
  bad = 0;
  for (size_t i = 0; i < pixels.size(); ++i)
    bad += back[i] != pixels[i];
  CHECK(bad == 0);
}

TEST_CASE("simd pipeline: the widest element type fills one register") {
  int seen = 0;
  const auto record = [&seen](auto x) {
    seen = decltype(x)::size();
    return x;
  };
  std::vector<float> f(40, 1.f), fo(40);
  sd::pipeline(record)(f, sd::out(fo));
  CHECK(seen == sd::vec<float>::size());

  std::vector<std::uint8_t> b(40, 1);
  sd::pipeline([](auto p) { return sd::rebind_t<float, decltype(p)>(p); },
               record)(b, sd::out(fo));
  CHECK(seen == sd::vec<float>::size());

  std::vector<double> d(40, 1.0);
  sd::pipeline(
      [](auto x, auto y) { return x + sd::rebind_t<double, decltype(y)>(y); },
      record)(d, f, sd::out(d));
  CHECK(seen == sd::vec<double>::size());

  std::vector<float> narrow(40);
  sd::pipeline(record)(d, sd::out(sd::flag_convert, narrow));
  CHECK(seen == sd::vec<double>::size());

  sd::pipeline(sd::lanes<8>, [&seen](sd::vec<float, 8> x) {
    seen = 8;
    return x;
  })(f, sd::out(fo));
  CHECK(seen == 8);
}

TEST_CASE("simd pipeline: tail lanes only ever hold values from the data") {
  for (int n = 1; n < 80; ++n) {
    const auto a = sequence<int>(n, 1, 1);
    std::vector<int> o(static_cast<size_t>(n));
    int strangers = 0;
    sd::pipeline([&strangers, n](auto x) {
      strangers += sd::reduce_count(x < 1 || x > n);
      return x;
    })(a, sd::out(o));
    CHECK(strangers == 0);
    std::vector<int> pairs(static_cast<size_t>(2 * n));
    for (int i = 0; i < 2 * n; ++i)
      pairs[size_t(i)] = i / 2 + 1;
    int foreign = 0;
    sd::pipeline([&foreign, n](auto l, auto r, auto i) {
      foreign += sd::reduce_count(l < 1 || l > n || r != l || i < 0 || i >= n);
      return l;
    })(sd::interleaved<2>(pairs), sd::index, sd::out(o));
    CHECK(foreign == 0);
    if (strangers != 0 || foreign != 0)
      continue;
    sd::pipeline([](auto x) { return 1000 / x; })(a, sd::out(o));
    int bad = 0;
    for (int i = 0; i < n; ++i)
      bad += o[size_t(i)] != 1000 / a[size_t(i)];
    CHECK(bad == 0);
  }
}

TEST_CASE("simd pipeline: keep_if packs the kept elements in order") {
  for (int n = 0; n <= max_size; n += n < 70 ? 1 : 41) {
    const auto a = sequence<int>(n, -50, 3);
    const auto b = sequence<double>(n, 0.5, 1.0);
    std::vector<int> ka(size_t(n) + 2, -1);
    std::vector<double> kb(size_t(n) + 2, -1.0);
    const size_t kept = sd::pipeline(
        sd::keep_if([](auto x, auto) { return (x & 1) == 0; }),
        sd::keep_if([](auto x, auto) { return x > -20; }), [](auto x, auto y) {
          return std::tuple(x, y * 2.0);
        })(a, b, sd::out(ka, kb));
    std::vector<int> wa;
    std::vector<double> wb;
    for (int i = 0; i < n; ++i)
      if ((a[size_t(i)] & 1) == 0 && a[size_t(i)] > -20) {
        wa.push_back(a[size_t(i)]);
        wb.push_back(b[size_t(i)] * 2.0);
      }
    CHECK(kept == wa.size());
    const int bad_a = first_mismatch(ka, wa, wa.size());
    const int bad_b = first_mismatch(kb, wb, wb.size());
    CHECK(bad_a == -1);
    CHECK(bad_b == -1);
    CHECK(ka[wa.size()] == -1);
  }
}

TEST_CASE("simd pipeline: an output may be its input") {
  auto a = sequence<int>(203, 0, 1);
  const auto original = a;
  sd::pipeline([](auto x) { return x * x; })(a, sd::out(a));
  int bad = 0;
  for (size_t i = 0; i < a.size(); ++i)
    bad += a[i] != original[i] * original[i];
  CHECK(bad == 0);

  a = original;
  const size_t kept = sd::pipeline(
      sd::keep_if([](auto x) { return x % 5 == 2; }))(a, sd::out(a));
  bad = 0;
  for (size_t i = 0; i < kept; ++i)
    bad += a[i] != int(5 * i + 2);
  CHECK(kept == 41);
  CHECK(bad == 0);
}

TEST_CASE("simd pipeline: folds") {
  for (int n : {0, 1, 3, 17, 64, 255, 1000, max_size}) {
    const auto a = sequence<int>(n, -400, 3);
    long long want_sum = 0;
    int want_min = std::numeric_limits<int>::max();
    int want_max = std::numeric_limits<int>::lowest();
    int want_and = ~0, want_or = 0, want_xor = 0;
    for (int x : a) {
      want_sum += x;
      want_min = std::min(want_min, x);
      want_max = std::max(want_max, x);
      want_and &= x;
      want_or |= x;
      want_xor ^= x;
    }
    const auto [s, lo, hi] = sd::pipeline(
        [](auto x) {
          return std::tuple(sd::rebind_t<long long, decltype(x)>(x), x, x);
        },
        sd::fold(sd::sum, sd::minimum, sd::maximum))(a);
    CHECK(s == want_sum);
    CHECK(lo == want_min);
    CHECK(hi == want_max);
    const auto [band, bor, bxor] = sd::pipeline(
        [](auto x) { return std::tuple(x, x, x); },
        sd::fold(std::bit_and<>{}, std::bit_or<>{}, std::bit_xor<>{}))(a);
    CHECK(band == want_and);
    CHECK(bor == want_or);
    CHECK(bxor == want_xor);
  }

  const std::vector<double> ratios = {1.5, 2.0, 0.5, 4.0, 0.25, 3.0, 1.0};
  const double prod = sd::pipeline(sd::fold(sd::product))(ratios);
  CHECK(prod == 4.5);

  const std::vector<float> none;
  CHECK(sd::pipeline(sd::fold(sd::sum))(none) == 0.f);
  CHECK(sd::pipeline(sd::fold(sd::minimum))(none) ==
        std::numeric_limits<float>::infinity());

  const auto widest = sd::reducer(
      [](auto a, auto b) { return sd::select(sd::abs(b) > sd::abs(a), b, a); },
      0.f);
  const std::vector<float> v = {1.f, -7.f, 3.f, 6.5f, -2.f};
  CHECK(sd::pipeline(sd::fold(widest))(v) == -7.f);
}

TEST_CASE("simd pipeline: a fold over kept lanes ignores the rest") {
  for (int n = 0; n < 300; n += 7) {
    const auto a = sequence<int>(n, 0, 1);
    const auto b = sequence<double>(n, 0.0, 0.5);
    const double s = sd::pipeline(
        sd::keep_if([](auto x, auto) { return x % 4 == 1; }),
        [](auto, auto y) { return y; }, sd::fold(sd::sum))(a, b);
    double want = 0;
    for (int i = 0; i < n; ++i)
      if (i % 4 == 1)
        want += b[size_t(i)];
    CHECK(s == want);
  }
}

TEST_CASE("simd pipeline: float sums are deterministic and close") {
  const auto a = sequence<float>(10007, -1.f, 0.0003f);
  const auto total = sd::pipeline(sd::fold(sd::sum));
  const float first = total(a);
  const float second = total(a);
  CHECK(same(first, second));
  double exact = 0;
  for (float x : a)
    exact += x;
  CHECK(std::abs(double(first) - exact) <= 1e-6 * 10007 * 3.0);
}

TEST_CASE("simd pipeline: pipelines nest and are reused") {
  const auto scale = sd::pipeline([](auto x) { return x * 2.f; },
                                  [](auto x) { return x + 1.f; });
  const auto twice = sd::pipeline(scale, scale);
  const auto total = sd::pipeline(scale, sd::fold(sd::sum));
  const auto a = sequence<float>(77, 0.f, 1.f);
  std::vector<float> o(a.size());
  for (int round = 0; round < 2; ++round) {
    twice(a, sd::out(o));
    int bad = 0;
    for (size_t i = 0; i < a.size(); ++i)
      bad += o[i] != (a[i] * 2.f + 1.f) * 2.f + 1.f;
    CHECK(bad == 0);
  }
  float want = 0;
  for (float x : a)
    want += x * 2.f + 1.f;
  CHECK(total(a) == want);
}

TEST_CASE("simd pipeline: complex elements") {
  std::vector<std::complex<float>> z(23), o(23);
  for (size_t i = 0; i < z.size(); ++i)
    z[i] = {float(i), 1.f - float(i)};
  sd::pipeline([](auto x) { return x * x; })(z, sd::out(o));
  int bad = 0;
  for (size_t i = 0; i < z.size(); ++i)
    bad += o[i] != z[i] * z[i];
  CHECK(bad == 0);
}

namespace {

constexpr int constexpr_pipeline() {
  std::array<int, 29> a{}, o{};
  for (int i = 0; i < 29; ++i)
    a[size_t(i)] = i;
  const size_t kept =
      sd::pipeline(sd::keep_if([](auto x) { return x % 3 == 0; }),
                   [](auto x) { return x * 10; })(a, sd::out(o));
  const int total = sd::pipeline(sd::fold(sd::sum))(o);
  return int(kept) * 100000 + total;
}

// Swaps the channels of 7 stereo frames, keeping frames whose index is even,
// and sums the indices of the kept frames.
constexpr int constexpr_interleaved() {
  std::array<int, 14> stereo{}, kept{};
  for (int i = 0; i < 14; ++i)
    stereo[size_t(i)] = i;
  const size_t frames = sd::pipeline(
      sd::keep_if([](auto, auto, auto i) { return i % 2 == 0; }),
      [](auto l, auto r, auto) { return std::tuple(r, l); })(
      sd::interleaved<2>(stereo), sd::index, sd::out(sd::interleaved<2>(kept)));
  const int indices = sd::pipeline([](auto, auto i) { return i; },
                                   sd::fold(sd::sum))(stereo, sd::index);
  return int(frames) * 10000 + kept[0] * 1000 + kept[1] * 100 + kept[7] * 10 +
         (indices == 91 ? 1 : 0);
}

} // namespace

TEST_CASE("simd pipeline: runs in constant expressions") {
  static_assert(constexpr_pipeline() == 10 * 100000 + 1350);
  CHECK(constexpr_pipeline() == 10 * 100000 + 1350);
  // Frames 0, 2, 4, 6 kept; frame 0 swapped is (1, 0), frame 6 is (13, 12).
  static_assert(constexpr_interleaved() == 4 * 10000 + 1 * 1000 + 0 + 120 + 1);
  const int value = constexpr_interleaved();
  CHECK(value == 4 * 10000 + 1000 + 120 + 1);
}

TEST_CASE("simd pipeline: the index counts elements from zero") {
  for (int n : {0, 1, 5, 64, 255, 1001}) {
    const auto a = sequence<float>(n, 0.f, 1.f);
    std::vector<int> idx(static_cast<size_t>(n), -1);
    std::vector<float> ramp(static_cast<size_t>(n), -1.f);
    // Exact with or without a fused multiply-add.
    const float start = 0.5f, step = 0.125f;
    sd::pipeline([start, step](auto, auto i, auto f) {
      return std::tuple(i, start + step * f);
    })(a, sd::index, sd::index_as<float>, sd::out(idx, ramp));
    int bad = 0;
    for (int i = 0; i < n; ++i) {
      bad += idx[size_t(i)] != i;
      bad += ramp[size_t(i)] != start + step * float(i);
    }
    CHECK(bad == 0);
    const long long sum =
        sd::pipeline([](auto, auto i) { return i; },
                     sd::fold(sd::sum))(a, sd::index_as<long long>);
    CHECK(sum == (long long)n * (n - 1) / 2);
  }
}

TEST_CASE("simd pipeline: interleaved stereo in and out") {
  for (int frames : {0, 1, 3, 4, 7, 64, 255, 256, 1027}) {
    const auto original = sequence<float>(2 * frames, -100.f, 0.5f);
    auto stereo = original;
    const size_t wrote = sd::pipeline([](auto l, auto r) {
      return std::tuple(r * 2.f, l);
    })(sd::interleaved<2>(stereo), sd::out(sd::interleaved<2>(stereo)));
    CHECK(wrote == size_t(frames));
    int bad = 0;
    for (int f = 0; f < frames; ++f) {
      bad += stereo[size_t(2 * f)] != original[size_t(2 * f + 1)] * 2.f;
      bad += stereo[size_t(2 * f + 1)] != original[size_t(2 * f)];
    }
    CHECK(bad == 0);

    const auto loudest = [](auto l, auto r) {
      return sd::max(sd::abs(l), sd::abs(r));
    };
    std::vector<float> mono(static_cast<size_t>(frames));
    std::vector<short> pcm(static_cast<size_t>(2 * frames));
    sd::pipeline([loudest](auto l, auto r) {
      return std::tuple(loudest(l, r), l, r);
    })(sd::interleaved<2>(original),
       sd::out(sd::flag_convert, mono, sd::interleaved<2>(pcm)));
    float want_peak = -std::numeric_limits<float>::infinity();
    bad = 0;
    for (int f = 0; f < frames; ++f) {
      const float l = original[size_t(2 * f)], r = original[size_t(2 * f + 1)];
      bad += mono[size_t(f)] != std::max(std::abs(l), std::abs(r));
      bad +=
          pcm[size_t(2 * f)] != short(l) || pcm[size_t(2 * f + 1)] != short(r);
      want_peak = std::max(want_peak, mono[size_t(f)]);
    }
    CHECK(bad == 0);
    const float peak = sd::pipeline(loudest, sd::fold(sd::maximum))(
        sd::interleaved<2>(original));
    CHECK(peak == want_peak);
  }
}

TEST_CASE("simd pipeline: keep_if over interleaved pixels") {
  for (int pixels : {0, 1, 15, 16, 17, 100, 333}) {
    std::vector<std::uint8_t> rgba(static_cast<size_t>(4 * pixels));
    for (size_t i = 0; i < rgba.size(); ++i)
      rgba[i] = std::uint8_t(i * 37 + 11);
    std::vector<std::uint8_t> bgra(rgba.size(), 7), red(size_t(pixels), 7);
    const size_t kept = sd::pipeline(sd::keep_if([](auto, auto, auto, auto a) {
                                       return a > std::uint8_t(128);
                                     }),
                                     [](auto r, auto g, auto b, auto a) {
                                       return std::tuple(b, g, r, a, r);
                                     })(sd::interleaved<4>(rgba),
                                        sd::out(sd::interleaved<4>(bgra), red));
    size_t want = 0;
    int bad = 0;
    for (int p = 0; p < pixels; ++p) {
      const std::uint8_t *px = rgba.data() + 4 * p;
      if (px[3] <= 128)
        continue;
      const std::uint8_t *out = bgra.data() + 4 * want;
      bad += out[0] != px[2] || out[1] != px[1] || out[2] != px[0] ||
             out[3] != px[3] || red[want] != px[0];
      ++want;
    }
    CHECK(kept == want);
    CHECK(bad == 0);
    if (want < size_t(pixels)) {
      CHECK(bgra[4 * want] == 7);
      CHECK(red[want] == 7);
    }
  }
}
