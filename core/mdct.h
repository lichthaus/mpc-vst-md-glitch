// mdct.h - radix-2 complex FFT and an FFT-based MDCT (DCT-IV core), written for MD Glitch.
//
// SPDX-License-Identifier: MIT
//
// Mdct::forward takes 2N windowed samples and gives N coefficients; Mdct::inverse gives back
// 2N samples to be windowed again and overlap-added. The pair is orthonormal: with a window
// that meets the Princen-Bradley condition (w[n]^2 + w[n+N]^2 = 1, symmetric), overlap-add
// reconstructs the input exactly.
//
// Folding: with z = [a b c d] (quarters of N/2), MDCT(z) = DCT-IV([-c_r - d, a - b_r]) and the
// inverse unfolds DCT-IV(X) = [t1 t2] into [t2, -t2_r, -t1_r, -t1]. The DCT-IV runs as an N/2-point
// complex FFT between two twiddle passes.
#pragma once
#include <cmath>
#include <vector>

namespace md {

class Fft {
public:
    void init(int n) {
        n_ = n;
        bits_ = 0;
        while ((1 << bits_) < n) bits_++;
        rev_.assign(n, 0);
        for (int i = 0; i < n; i++) {
            int r = 0;
            for (int b = 0; b < bits_; b++) r |= ((i >> b) & 1) << (bits_ - 1 - b);
            rev_[i] = r;
        }
        cos_.resize(n / 2);
        sin_.resize(n / 2);
        for (int i = 0; i < n / 2; i++) {
            cos_[i] = float(std::cos(2 * M_PI * i / n));
            sin_[i] = float(-std::sin(2 * M_PI * i / n));
        }
    }
    // In place, forward (exp(-2 pi i nk / n)), unscaled.
    void forward(float* re, float* im) const {
        const int n = n_;
        for (int i = 0; i < n; i++) {
            const int r = rev_[i];
            if (r > i) { std::swap(re[i], re[r]); std::swap(im[i], im[r]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; j++) {
                    const float wr = cos_[j * step], wi = sin_[j * step];
                    const int a = i + j, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
            }
        }
    }
    int size() const { return n_; }

private:
    int n_ = 0, bits_ = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

class Mdct {
public:
    void init(int n) {  // n = number of coefficients (hop size)
        n_ = n;
        fft_.init(n / 2);
        const int m = n / 2;
        preC_.resize(m); preS_.resize(m); postC_.resize(m); postS_.resize(m);
        for (int i = 0; i < m; i++) {
            preC_[i] = float(std::cos(-M_PI * (i + 0.25) / n));
            preS_[i] = float(std::sin(-M_PI * (i + 0.25) / n));
            postC_[i] = float(std::cos(-M_PI * i / n));
            postS_[i] = float(std::sin(-M_PI * i / n));
        }
        t_.resize(n); re_.resize(m); im_.resize(m);
        scale_ = float(std::sqrt(2.0 / n));
    }
    int size() const { return n_; }

    void forward(const float* z, float* X) {
        const int n = n_, h = n / 2;
        const float* a = z; const float* b = z + h; const float* c = z + n; const float* d = z + n + h;
        for (int i = 0; i < h; i++) {
            t_[i] = -c[h - 1 - i] - d[i];
            t_[h + i] = a[i] - b[h - 1 - i];
        }
        dct4(t_.data(), X);
    }
    void inverse(const float* X, float* y) {
        const int n = n_, h = n / 2;
        dct4(X, t_.data());
        const float* t1 = t_.data(); const float* t2 = t_.data() + h;
        for (int i = 0; i < h; i++) {
            y[i] = t2[i];
            y[h + i] = -t2[h - 1 - i];
            y[n + i] = -t1[h - 1 - i];
            y[n + h + i] = -t1[i];
        }
    }

private:
    // C[k] = scale * sum t[n] cos(pi/N (n+1/2)(k+1/2)); in and out may not alias.
    void dct4(const float* t, float* C) {
        const int n = n_, m = n / 2;
        for (int i = 0; i < m; i++) {
            const float xr = t[2 * i], xi = t[n - 1 - 2 * i];
            re_[i] = xr * preC_[i] - xi * preS_[i];
            im_[i] = xr * preS_[i] + xi * preC_[i];
        }
        fft_.forward(re_.data(), im_.data());
        for (int k = 0; k < m; k++) {
            const float wr = re_[k] * postC_[k] - im_[k] * postS_[k];
            const float wi = re_[k] * postS_[k] + im_[k] * postC_[k];
            C[2 * k] = scale_ * wr;
            C[n - 1 - 2 * k] = -scale_ * wi;
        }
    }
    int n_ = 0;
    float scale_ = 1;
    Fft fft_;
    std::vector<float> preC_, preS_, postC_, postS_, t_, re_, im_;
};

}  // namespace md
