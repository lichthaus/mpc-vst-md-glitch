// test_mdct.cpp - checks the FFT MDCT against the textbook sum and checks overlap-add
// reconstruction with a sine window; checks that the fast FFT, MDCT, rounding and scale-factor
// code (mdct.h, simd.h) gives exactly the values of the plain code it replaced (bit for bit:
// run on the Live II too, where it is the NEON path).
#include "../core/mdct.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ref {  // the plain radix-2 FFT and DCT-IV as they were before the NEON work (0.2.4)
struct Fft {
    int n = 0, bits = 0;
    std::vector<int> rev;
    std::vector<float> cs, sn;
    void init(int n_) {
        n = n_;
        while ((1 << bits) < n) bits++;
        rev.assign(n, 0);
        for (int i = 0; i < n; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[i] = r;
        }
        cs.resize(n / 2); sn.resize(n / 2);
        for (int i = 0; i < n / 2; i++) { cs[i] = float(std::cos(2 * M_PI * i / n)); sn[i] = float(-std::sin(2 * M_PI * i / n)); }
    }
    void forward(float* re, float* im) const {
        for (int i = 0; i < n; i++) { const int r = rev[i]; if (r > i) { std::swap(re[i], re[r]); std::swap(im[i], im[r]); } }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; j++) {
                    const float wr = cs[j * step], wi = sn[j * step];
                    const int a = i + j, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
        }
    }
};
struct Mdct {
    int n = 0; float scale = 1; Fft fft;
    std::vector<float> preC, preS, postC, postS, t, re, im;
    void init(int n_) {
        n = n_; fft.init(n / 2); const int m = n / 2;
        preC.resize(m); preS.resize(m); postC.resize(m); postS.resize(m);
        for (int i = 0; i < m; i++) {
            preC[i] = float(std::cos(-M_PI * (i + 0.25) / n)); preS[i] = float(std::sin(-M_PI * (i + 0.25) / n));
            postC[i] = float(std::cos(-M_PI * i / n)); postS[i] = float(std::sin(-M_PI * i / n));
        }
        t.resize(n); re.resize(m); im.resize(m); scale = float(std::sqrt(2.0 / n));
    }
    void dct4(const float* x, float* C) {
        const int m = n / 2;
        for (int i = 0; i < m; i++) {
            const float xr = x[2 * i], xi = x[n - 1 - 2 * i];
            re[i] = xr * preC[i] - xi * preS[i];
            im[i] = xr * preS[i] + xi * preC[i];
        }
        fft.forward(re.data(), im.data());
        for (int k = 0; k < m; k++) {
            const float wr = re[k] * postC[k] - im[k] * postS[k];
            const float wi = re[k] * postS[k] + im[k] * postC[k];
            C[2 * k] = scale * wr; C[n - 1 - 2 * k] = -scale * wi;
        }
    }
    void forward(const float* z, float* X) {
        const int h = n / 2; const float *a = z, *b = z + h, *c = z + n, *d = z + n + h;
        for (int i = 0; i < h; i++) { t[i] = -c[h - 1 - i] - d[i]; t[h + i] = a[i] - b[h - 1 - i]; }
        dct4(t.data(), X);
    }
    void inverse(const float* X, float* y) {
        const int h = n / 2; dct4(X, t.data()); const float *t1 = t.data(), *t2 = t.data() + h;
        for (int i = 0; i < h; i++) { y[i] = t2[i]; y[h + i] = -t2[h - 1 - i]; y[n + i] = -t1[h - 1 - i]; y[n + h + i] = -t1[i]; }
    }
};
}  // namespace ref

static bool sameBits(const float* a, const float* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;  // equal values (+0 and -0 count as equal)
    return true;
}

int main() {
    int fails = 0;
    for (int n : {32, 64, 128, 256}) {
        md::Mdct m; m.init(n);
        std::vector<float> z(2 * n), X(n), Xr(n), y(2 * n);
        for (auto& v : z) v = float(std::rand()) / float(RAND_MAX) - 0.5f;
        m.forward(z.data(), X.data());
        double err = 0, mx = 0;
        for (int k = 0; k < n; k++) {
            double s = 0;
            for (int i = 0; i < 2 * n; i++) s += z[i] * std::cos(M_PI / n * (i + 0.5 + n / 2.0) * (k + 0.5));
            s *= std::sqrt(2.0 / n);
            err = std::fmax(err, std::fabs(s - X[k]));
            mx = std::fmax(mx, std::fabs(s));
        }
        // Overlap-add reconstruction with a sine window over three frames.
        std::vector<float> w(2 * n), x(4 * n), out(4 * n, 0.0f);
        for (int i = 0; i < 2 * n; i++) w[i] = float(std::sin(M_PI * (i + 0.5) / (2 * n)));
        for (auto& v : x) v = float(std::rand()) / float(RAND_MAX) - 0.5f;
        for (int f = 0; f < 3; f++) {
            for (int i = 0; i < 2 * n; i++) z[i] = x[f * n + i] * w[i];
            m.forward(z.data(), X.data());
            m.inverse(X.data(), y.data());
            for (int i = 0; i < 2 * n; i++) out[f * n + i] += y[i] * w[i];
        }
        double rerr = 0;
        for (int i = n; i < 3 * n; i++) rerr = std::fmax(rerr, std::fabs(out[i] - x[i]));
        const bool ok = err < 1e-4 * mx && rerr < 1e-4;
        std::printf("N=%3d  vs direct %.2e (max %.2f)  OLA error %.2e  %s\n", n, err, mx, rerr, ok ? "ok" : "FAIL");
        fails += !ok;
    }
    // Fast code against the plain code, value for value.
    for (int n : {16, 32, 64, 128, 256}) {
        md::Mdct m; m.init(n);
        ref::Mdct r; r.init(n);
        std::vector<float> z(2 * n), X(n), Xr(n), y(2 * n), yr(2 * n);
        bool same = true;
        for (int trial = 0; trial < 200; trial++) {
            const float amp = std::ldexp(1.0f, trial % 40 - 30);
            for (auto& v : z) v = amp * (float(std::rand()) / float(RAND_MAX) - 0.5f);
            m.forward(z.data(), X.data()); r.forward(z.data(), Xr.data());
            m.inverse(X.data(), y.data()); r.inverse(X.data(), yr.data());
            same = same && sameBits(X.data(), Xr.data(), n) && sameBits(y.data(), yr.data(), 2 * n);
        }
        std::printf("N=%3d  fast MDCT == plain MDCT (200 frames)  %s\n", n, same ? "ok" : "FAIL");
        fails += !same;
    }
    // rne() == nearbyint and scaleFactor() == the libm formula, on a sweep of floats (every float
    // in a run takes tools-free seconds; this samples every 61st bit pattern plus the step edges).
    {
        long bad = 0;
        for (uint32_t u = 0; u <= 0x4a800000u; u += 61) {
            float x; std::memcpy(&x, &u, 4);
            for (float v : {x, -x}) {
                const float a = md::rneMagic(v), b = md::rne(v), c = std::nearbyint(v);
                uint32_t ua, ub, uc; std::memcpy(&ua, &a, 4); std::memcpy(&ub, &b, 4); std::memcpy(&uc, &c, 4);
                bad += ua != uc || ub != uc;
            }
        }
        for (uint32_t u = 1; u < 0x7f800000u; u += 61) {
            float p; std::memcpy(&p, &u, 4);
            bad += md::scaleFactor(p) != md::scaleFactorExact(p);
        }
        for (int s = -50; s < 30; s++)
            for (int k = -40; k <= 40; k++) {  // around every step 2^(s/3)
                const float p = std::nextafter(float(std::exp2(s / 3.0)), 0.0f) * (1.0f + float(k) * 3e-8f);
                bad += md::scaleFactor(p) != md::scaleFactorExact(p);
            }
        std::printf("rounding and scale factors: %ld mismatches  %s\n", bad, bad ? "FAIL" : "ok");
        fails += bad != 0;
    }
    return fails;
}
