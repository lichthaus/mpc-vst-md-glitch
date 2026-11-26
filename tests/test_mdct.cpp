// test_mdct.cpp - checks the FFT MDCT against the textbook sum and checks overlap-add
// reconstruction with a sine window.
#include "../core/mdct.h"
#include <cstdio>
#include <cstdlib>

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
    return fails;
}
