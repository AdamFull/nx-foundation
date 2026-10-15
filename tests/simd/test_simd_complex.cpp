#include "simd/simd_test_util.h"

#include <array>
#include <bit>
#include <cmath>
#include <complex>

using namespace simd_test;

namespace {

template <class F, int N> void complex_ops() {
  using C = std::complex<F>;
  using V = sd::vec<C, N>;
  using R = sd::vec<F, N>;
  rng<F> r(N * 71 + sizeof(F));
  // Integral parts keep every product exact, so the result cannot depend on
  // whether the compiler fused a multiply and an add.
  const auto part = [&] { return F(static_cast<int>(r.next() % 41) - 20); };
  for (int round = 0; round < 4; ++round) {
    const V a([&](int) { return C(part(), part()); });
    const V b([&](int) {
      const C c(part(), part());
      return c == C(0, 0) ? C(1, 0) : c;
    });

    check_lanes("complex +", a + b, [&](int i) { return a[i] + b[i]; });
    check_lanes("complex -", a - b, [&](int i) { return a[i] - b[i]; });
    check_lanes("complex *", a * b, [&](int i) { return a[i] * b[i]; });
    check_lanes("complex /", a / b, [&](int i) { return a[i] / b[i]; });
    check_lanes("complex negate", -a, [&](int i) { return -a[i]; });
    check_lanes("complex + real", a + C(F(2), F(0)),
                [&](int i) { return a[i] + C(F(2), F(0)); });
    check_lanes("complex * real broadcast", a * F(3),
                [&](int i) { return a[i] * F(3); });
    V c = a;
    c *= b;
    check_lanes("complex *=", c, [&](int i) { return a[i] * b[i]; });
    check_mask("complex ==", a == b, [&](int i) { return a[i] == b[i]; });
    check_mask("complex == self", a == a, [](int) { return true; });
    check_mask("complex !=", a != b, [&](int i) { return a[i] != b[i]; });

    check_lanes("real()", a.real(), [&](int i) { return a[i].real(); });
    check_lanes("imag()", a.imag(), [&](int i) { return a[i].imag(); });
    check_lanes("real(v)", sd::real(a), [&](int i) { return a[i].real(); });
    check_lanes("imag(v)", sd::imag(a), [&](int i) { return a[i].imag(); });
    V d = a;
    d.real(b.real());
    check_lanes("set real", d,
                [&](int i) { return C(b[i].real(), a[i].imag()); });
    d.imag(R(F(5)));
    check_lanes("set imag", d, [&](int i) { return C(b[i].real(), F(5)); });
    check_lanes("from parts", V(a.real(), b.imag()),
                [&](int i) { return C(a[i].real(), b[i].imag()); });
    check_lanes("from real part", V(a.real()),
                [&](int i) { return C(a[i].real(), F(0)); });

    check_lanes("norm", sd::norm(a), [&](int i) { return std::norm(a[i]); });
    check_lanes("conj", sd::conj(a), [&](int i) { return std::conj(a[i]); });
    check_lanes("abs complex", sd::abs(a),
                [&](int i) { return std::abs(a[i]); });
    check_lanes("arg", sd::arg(a), [&](int i) { return std::arg(a[i]); });

    const V small = a / C(F(16), F(0));
#define CHECK_COMPLEX(fn)                                                      \
  check_lanes(#fn, sd::fn(small), [&](int i) { return std::fn(small[i]); })
    CHECK_COMPLEX(proj);
    CHECK_COMPLEX(exp);
    CHECK_COMPLEX(log);
    CHECK_COMPLEX(log10);
    CHECK_COMPLEX(sqrt);
    CHECK_COMPLEX(sin);
    CHECK_COMPLEX(asin);
    CHECK_COMPLEX(cos);
    CHECK_COMPLEX(acos);
    CHECK_COMPLEX(tan);
    CHECK_COMPLEX(atan);
    CHECK_COMPLEX(sinh);
    CHECK_COMPLEX(asinh);
    CHECK_COMPLEX(cosh);
    CHECK_COMPLEX(acosh);
    CHECK_COMPLEX(tanh);
    CHECK_COMPLEX(atanh);
#undef CHECK_COMPLEX
    check_lanes("complex pow", sd::pow(small, b),
                [&](int i) { return std::pow(small[i], b[i]); });
    const R radius = sd::fabs(a.real());
    const R angle = a.imag() / F(8);
    check_lanes("polar", sd::polar(radius, angle),
                [&](int i) { return std::polar(radius[i], angle[i]); });
    check_lanes("polar default angle", sd::polar(radius),
                [&](int i) { return std::polar(radius[i]); });

    C sum(0, 0);
    for (int i = 0; i < N; ++i)
      sum += a[i];
    CHECK(same(sd::reduce(a), sum));
    const auto k = a.real() < b.real();
    const typename V::mask_type ck(k);
    check_lanes("complex select", sd::select(ck, a, b),
                [&](int i) { return k[i] ? a[i] : b[i]; });

    C buffer[N + 1];
    buffer[N] = C(F(-7), F(-7));
    sd::unchecked_store(a, buffer, N);
    for (int i = 0; i < N; ++i)
      CHECK(same(buffer[i], a[i]));
    CHECK(same(buffer[N], C(F(-7), F(-7))));
    check_lanes("complex load", sd::unchecked_load<V>(buffer, N),
                [&](int i) { return a[i]; });
    check_lanes("complex partial load", sd::partial_load<V>(buffer, N / 2),
                [&](int i) { return i < N / 2 ? a[i] : C(); });

    const sd::vec<std::complex<double>, N> widened(a);
    check_lanes("complex widen", widened,
                [&](int i) { return std::complex<double>(a[i]); });
  }
}

} // namespace

TEST_CASE("simd complex: arithmetic and functions match std::complex") {
  complex_ops<float, 1>();
  complex_ops<float, 2>();
  complex_ops<float, 3>();
  complex_ops<float, 4>();
  complex_ops<float, 8>();
  complex_ops<double, 1>();
  complex_ops<double, 2>();
  complex_ops<double, 4>();
  complex_ops<double, 5>();
  static_assert(std::is_same_v<
                sd::vec<std::complex<float>, 4>::mask_type,
                sd::basic_mask<8, sd::vec<std::complex<float>, 4>::abi_type>>);
  static_assert(sd::vec<std::complex<double>>::size() >= 1);
  static_assert(std::is_convertible_v<float, sd::vec<std::complex<float>>>);
  static_assert(!std::is_convertible_v<double, sd::vec<std::complex<float>>>);
}
