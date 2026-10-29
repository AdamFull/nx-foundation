"""Fits the coefficients in include/core/foundation/simd/float_math.h.

Each polynomial is a weighted minimax fit (Lawson's iteration), rounded to
float one coefficient at a time with the rest refitted. The script prints the
arrays as C++ and, for each, the error of evaluating it in float without FMA
in the header's operation order. Needs numpy.

    python tools/fit_float_math.py
"""
import math

import numpy as np

f32 = np.float32
erf = np.vectorize(math.erf)
erfc = np.vectorize(math.erfc)


def grid(lo, hi, n=200001):
    return np.linspace(lo, hi, n)


def chebyshev_subset(s, count=1500):
    lo, hi = s.min(), s.max()
    k = np.arange(count)
    nodes = (lo + hi) / 2 + (hi - lo) / 2 * np.cos(np.pi * (k + 0.5) / count)
    return np.unique(np.searchsorted(s, np.sort(nodes)).clip(0, len(s) - 1))


def lawson(A, target, weight, iters=80):
    u = np.ones(len(target))
    for _ in range(iters):
        sw = np.sqrt(u) * weight
        c, *_ = np.linalg.lstsq(A * sw[:, None], target * sw, rcond=None)
        u = u * (np.abs(weight * (target - A @ c)) + 1e-300)
        u /= u.sum()
    return c


def polynomial(s, target, weight, degree):
    idx = chebyshev_subset(s)
    s, target, weight = s[idx], target[idx], weight[idx]
    A = np.vander(s, degree + 1, increasing=True)
    cs = []
    for j in range(degree + 1):
        known = A[:, :j] @ np.array(cs, dtype=np.float64) if j else 0.0
        cs.append(f32(lawson(A[:, j:], target - known, weight)[0]))
    return cs


def rational(x, f, nd, dd, iters=60):
    """x N(x^2) / D(x^2) with D(0) = 1, by linearized reweighting."""
    idx = chebyshev_subset(x, 2000)
    xs, fs = x[idx], f[idx]
    ss = xs * xs
    A = np.hstack([np.vander(ss, nd + 1, increasing=True) * xs[:, None],
                   -np.vander(ss, dd + 1, increasing=True)[:, 1:] * fs[:, None]])
    u = np.ones(len(xs))
    d = np.ones(len(xs))
    for _ in range(iters):
        w = np.sqrt(u) / (d * np.abs(fs))
        c, *_ = np.linalg.lstsq(A * w[:, None], fs * w, rcond=None)
        n = np.vander(ss, nd + 1, increasing=True) @ c[:nd + 1]
        d = 1 + np.vander(ss, dd + 1, increasing=True)[:, 1:] @ c[nd + 1:]
        u = u * (np.abs(xs * n / d - fs) / np.abs(fs) + 1e-300)
        u /= u.sum()
    return [f32(v) for v in c[:nd + 1]], [f32(1)] + [f32(v) for v in c[nd + 1:]]


def horner(cs, s):
    p = np.full_like(s, f32(cs[-1]))
    for c in reversed(cs[:-1]):
        p = (p * s).astype(f32) + f32(c)
    return p.astype(f32)


def ulps(got, ref):
    a = np.maximum(np.abs(ref).astype(f32), np.finfo(f32).tiny)
    return (np.abs(got.astype(np.float64) - ref) /
            (np.nextafter(a, f32(np.inf)) - a)).max()


def emit(name, cs, error):
    values = ", ".join(f"{float(c):.9e}f" for c in cs)
    print(f"inline constexpr float {name}[] = {{{values}}};  // {error}")


def main():
    h = math.log(2) / 2 + 0.002
    r = grid(-h, h)
    r = r[np.abs(r) > 1e-9]
    rv = r.astype(f32)
    r2 = rv * rv
    em1 = np.expm1(r)
    cs = polynomial(r, (em1 - r) / r**2, r**2 / np.abs(em1), 5)
    emit("expm1_p", cs, f"{ulps(horner(cs, rv) * r2 + rv, np.expm1(rv.astype(np.float64))):.2f} ulp")
    cs = polynomial(r, (np.exp(r) - 1 - r) / r**2, r**2 / np.exp(r), 2)
    got = (horner(cs, rv) * r2 + rv).astype(f32) + f32(1)
    emit("fast_exp_p", cs, f"{ulps(got, np.exp(rv.astype(np.float64))):.0f} ulp")

    f = grid(-0.5005, 0.5005)
    f = f[np.abs(f) > 1e-9]
    fv = f.astype(f32)
    cs = polynomial(f, (np.exp2(f) - 1) / f, f / np.exp2(f), 5)
    emit("exp2_q", cs, f"{ulps((horner(cs, fv) * fv).astype(f32) + f32(1), np.exp2(fv.astype(np.float64))):.2f} ulp")

    f = grid(math.sqrt(0.5) - 1 - 0.001, math.sqrt(2) - 1 + 0.001)
    f = f[np.abs(f) > 1e-7]
    fv = f.astype(f32)
    f2 = fv * fv
    cs = polynomial(f, (np.log1p(f) - f + f * f / 2) / f**3,
                    np.abs(f**3 / np.log1p(f)), 7)
    got = fv + ((horner(cs, fv) * fv).astype(f32) * f2 - f2 * f32(0.5))
    emit("log_l", cs, f"{ulps(got, np.log1p(fv.astype(np.float64))):.2f} ulp")

    x = grid(1e-4, 0.6255)
    xv = x.astype(f32)
    x2 = xv * xv
    cs = polynomial(x * x, (np.tanh(x) - x) / x**3, np.abs(x**3 / np.tanh(x)), 4)
    got = xv + (horner(cs, x2) * x2).astype(f32) * xv
    emit("tanh_t", cs, f"{ulps(got, np.tanh(xv.astype(np.float64))):.2f} ulp")

    x = grid(1e-4, 0.876)
    xv = x.astype(f32)
    cs = polynomial(x * x, erf(x) / x - 1, x / erf(x), 6)
    got = xv + xv * horner(cs, xv * xv)
    emit("erf_h", cs, f"{ulps(got, erf(xv.astype(np.float64))):.2f} ulp")

    x = grid(0.8745, 3.9195)
    xv = x.astype(f32)
    cs = polynomial(x - 2.4, erfc(x) * np.exp(x * x), np.exp(-x * x) / erf(x), 10)
    e = np.exp(-(xv * xv).astype(f32).astype(np.float64)).astype(f32)
    got = f32(1) - (e * horner(cs, (xv - f32(2.4)).astype(f32))).astype(f32)
    emit("erfc_g", cs, f"{ulps(got, erf(xv.astype(np.float64))):.2f} ulp")

    for name, fn, top in (("tanh", np.tanh, 9.0109), ("erf", erf, 3.9193)):
        x = grid(1e-4, top)
        n, d = rational(x, fn(x), 3, 3)
        xv = x.astype(f32)
        got = (xv * horner(n, xv * xv)).astype(f32) / horner(d, xv * xv)
        ref = fn(xv.astype(np.float64))
        rel = (np.abs(got.astype(np.float64) - ref) / np.abs(ref)).max()
        emit(f"fast_{name}_n", n, f"{rel:.1e} relative")
        emit(f"fast_{name}_d", d, "")


main()
