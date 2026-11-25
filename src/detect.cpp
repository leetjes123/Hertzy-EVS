#include "detect.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>

namespace evs {

namespace {

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t h = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + h, v.end());
    if (v.size() % 2) return v[h];
    const double hi = v[h];
    return 0.5 * (*std::max_element(v.begin(), v.begin() + h) + hi);
}

inline int refl101(int i, int n) {
    if (n == 1) return 0;
    while (i < 0 || i >= n) i = i < 0 ? -i : 2 * n - i - 2;
    return i;
}

std::vector<float> sep_filter(const std::vector<float>& src, int W, int H, const std::vector<float>& kx, const std::vector<float>& ky) {
    const int rx = (int)kx.size() / 2, ry = (int)ky.size() / 2;
    std::vector<float> tmp(src.size()), dst(src.size());
    std::vector<float> row(W + 2 * rx);
    for (int y = 0; y < H; ++y) {
        const float* s = &src[(size_t)y * W];
        for (int x = -rx; x < W + rx; ++x) row[x + rx] = s[refl101(x, W)];
        float* t = &tmp[(size_t)y * W];
        for (int x = 0; x < W; ++x) {
            float acc = 0;
            for (int k = 0; k < (int)kx.size(); ++k) acc += kx[k] * row[x + k];
            t[x] = acc;
        }
    }
    for (int y = 0; y < H; ++y) {
        float* d = &dst[(size_t)y * W];
        std::fill(d, d + W, 0.f);
        for (int k = 0; k < (int)ky.size(); ++k) {
            const float c = ky[k];
            const float* r = &tmp[(size_t)refl101(y - ry + k, H) * W];
            for (int x = 0; x < W; ++x) d[x] += c * r[x];
        }
    }
    return dst;
}

std::vector<float> gauss_kernel(double sigma) {
    const int n = (int(std::lround(sigma * 4 * 2 + 1))) | 1;
    std::vector<double> k(n);
    double s = 0;
    for (int i = 0; i < n; ++i) { const double x = i - (n - 1) * 0.5; k[i] = std::exp(-x * x / (2 * sigma * sigma)); s += k[i]; }
    std::vector<float> f(n);
    for (int i = 0; i < n; ++i) f[i] = float(k[i] / s);
    return f;
}

struct Edges { std::vector<double> x, y, nx, ny, coh; };

// coherent edge pixels with their unit normal (structure tensor of the log map)
Edges edge_orientations(const std::vector<float>& A, int W, int H, float coh_min, float a_min) {
    std::vector<float> L(A.size());
    for (size_t i = 0; i < A.size(); ++i) L[i] = std::log1p(A[i]);
    const auto g15 = gauss_kernel(1.5), g3 = gauss_kernel(3.0);
    L = sep_filter(L, W, H, g15, g15);
    const std::vector<float> d{-1, 0, 1}, s{1, 2, 1};
    const auto gx = sep_filter(L, W, H, d, s), gy = sep_filter(L, W, H, s, d);
    std::vector<float> xx(A.size()), xy(A.size()), yy(A.size());
    for (size_t i = 0; i < A.size(); ++i) { xx[i] = gx[i] * gx[i]; xy[i] = gx[i] * gy[i]; yy[i] = gy[i] * gy[i]; }
    const auto Jxx = sep_filter(xx, W, H, g3, g3), Jxy = sep_filter(xy, W, H, g3, g3), Jyy = sep_filter(yy, W, H, g3, g3);
    Edges E;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            if (!(A[i] >= a_min)) continue;
            const float dd = Jxx[i] - Jyy[i];
            const float coh = std::sqrt(dd * dd + 4 * Jxy[i] * Jxy[i]) / (Jxx[i] + Jyy[i] + 1e-9f);
            if (!(coh > coh_min)) continue;
            const float th = 0.5f * std::atan2(2 * Jxy[i], dd);
            E.x.push_back(x); E.y.push_back(y);
            E.nx.push_back(std::cos(th)); E.ny.push_back(std::sin(th));
            E.coh.push_back(coh);
        }
    return E;
}

// separable maximum filter, "nearest" borders
std::vector<double> max_filter(const std::vector<double>& a, int rows, int cols, int sr, int sc) {
    std::vector<double> t(a.size()), o(a.size());
    const int hr = sr / 2, hc = sc / 2;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            double m = -1e300;
            for (int k = c - hc; k <= c - hc + sc - 1; ++k) m = std::max(m, a[(size_t)r * cols + std::clamp(k, 0, cols - 1)]);
            t[(size_t)r * cols + c] = m;
        }
    for (int c = 0; c < cols; ++c)
        for (int r = 0; r < rows; ++r) {
            double m = -1e300;
            for (int k = r - hr; k <= r - hr + sr - 1; ++k) m = std::max(m, t[(size_t)std::clamp(k, 0, rows - 1) * cols + c]);
            o[(size_t)r * cols + c] = m;
        }
    return o;
}

struct Line { double n[2], d, half, a0, a1, score; int support; };

}  // namespace

std::vector<uint8_t> activity_mask(const std::vector<float>& count, int W, int H, double k, std::vector<float>* logA) {
    const size_t N = (size_t)W * H;
    std::vector<float> la(N);
    std::vector<double> v(N);
    for (size_t i = 0; i < N; ++i) { la[i] = std::log1p(count[i]); v[i] = la[i]; }
    const double med = median(v);
    for (size_t i = 0; i < N; ++i) v[i] = std::fabs(la[i] - med);
    const double mad = 1.4826 * median(v) + 1e-6;
    std::vector<uint8_t> m(N);
    auto opened = [&](const std::vector<uint8_t>& in, size_t& count) {   // opening with a 2x2 square
        std::vector<uint8_t> e(N, 0), o(N, 0);
        for (int y = 1; y < H; ++y)
            for (int x = 1; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                e[i] = in[i] && in[i - 1] && in[i - W] && in[i - W - 1];
            }
        count = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                o[i] = e[i] || (x + 1 < W && e[i + 1]) || (y + 1 < H && e[i + W]) || (x + 1 < W && y + 1 < H && e[i + W + 1]);
                count += o[i];
            }
        return o;
    };
    for (size_t i = 0; i < N; ++i) m[i] = la[i] > med + k * mad;
    size_t n = 0;
    std::vector<uint8_t> o = opened(m, n);
    if (n < N / 1000) {
        // a noisy scene (events everywhere): the global rule puts the threshold above the structures. Compare each pixel
        // with its 31 x 31 neighbourhood instead and lower the factor until a sensible part of the picture is selected.
        // Single pixels are dominated by shot noise there, so smooth a little first (sigma 1.5 px keeps 2-4 px wide edges).
        const auto g = gauss_kernel(1.5);
        std::vector<float> ls(N);
        {
            const std::vector<float> cs = sep_filter(count, W, H, g, g);
            for (size_t i = 0; i < N; ++i) ls[i] = std::log1p(std::max(0.f, cs[i]));
        }
        const std::vector<float> box(31, 1.f / 31);
        const std::vector<float> bg = sep_filter(ls, W, H, box, box);
        std::vector<double> r(N);
        for (size_t i = 0; i < N; ++i) r[i] = ls[i] - bg[i];
        const double rmed = median(r);
        std::vector<double> ad(N);
        for (size_t i = 0; i < N; ++i) ad[i] = std::fabs(r[i] - rmed);
        const double rmad = 1.4826 * median(ad) + 1e-6;
        for (double kk : {k, 3.0, 2.5, 2.0}) {
            for (size_t i = 0; i < N; ++i) m[i] = r[i] > rmed + kk * rmad;
            o = opened(m, n);
            if (n >= N / 300) break;
        }
    }
    if (logA) *logA = std::move(la);
    return o;
}

std::vector<Roi> detect_lines(const std::vector<float>& count, int W, int H, const LineOptions& opt) {
    std::vector<float> la;
    const std::vector<uint8_t> mask = activity_mask(count, W, H, opt.maskK, &la);
    // orientation-guided Hough
    std::vector<float> Am(la.size());
    for (size_t i = 0; i < la.size(); ++i) Am[i] = mask[i] ? std::expm1(la[i]) : 0.f;
    const Edges E = edge_orientations(Am, W, H, 0.5f, 1e-6f);
    const double a_step = 0.25, r_step = 2.0, spread = 2.0, diag = std::hypot(W, H);
    const int na = (int)std::lround(180 / a_step);
    const int nr = (int)std::floor((2 * diag + r_step) / r_step) + 1;
    std::vector<double> acc((size_t)nr * na, 0.0);
    for (size_t i = 0; i < E.x.size(); ++i) {
        const double w = la[(size_t)E.y[i] * W + (size_t)E.x[i]] * E.coh[i];
        double th0 = std::atan2(E.ny[i], E.nx[i]) * 180 / M_PI;
        th0 = std::fmod(th0 + 360.0, 180.0);
        for (double o = -spread; o <= spread + 1e-9; o += a_step) {
            const double th = std::fmod(th0 + o + 180.0, 180.0), q = th * M_PI / 180;
            const double r = E.x[i] * std::cos(q) + E.y[i] * std::sin(q);
            const int ai = (int)(std::lround(th / a_step) % na);
            const int ri = (int)std::lround((r + diag) / r_step);
            if (ri >= 0 && ri < nr) acc[(size_t)ri * na + ai] += w;
        }
    }
    const double amax = *std::max_element(acc.begin(), acc.end());
    const bool dbg = std::getenv("EVS_DEBUG") != nullptr;
    if (dbg) {
        size_t nm = 0;
        for (uint8_t m : mask) nm += m;
        std::fprintf(stderr, "lines: mask %zu px, coherent edges %zu, hough max %.1f\n", nm, E.x.size(), amax);
    }
    std::vector<Roi> out;
    if (amax <= 0) return out;
    const std::vector<double> mf = max_filter(acc, nr, na, 31, 25);
    std::vector<size_t> pk;
    for (size_t i = 0; i < acc.size(); ++i) if (acc[i] == mf[i] && acc[i] >= opt.rel * amax) pk.push_back(i);
    std::sort(pk.begin(), pk.end(), [&](size_t a, size_t b) { return acc[a] > acc[b]; });
    if ((int)pk.size() > opt.maxLines * 3) pk.resize(opt.maxLines * 3);
    if (dbg) std::fprintf(stderr, "lines: %zu hough peaks above %.2f x max\n", pk.size(), opt.rel);
    // mask pixels
    std::vector<double> Px, Py, Wv;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (mask[(size_t)y * W + x]) { Px.push_back(x); Py.push_back(y); Wv.push_back(la[(size_t)y * W + x]); }
    const size_t np = Px.size();
    std::vector<Line> lines;
    for (size_t pi : pk) {
        const int ri = (int)(pi / na), ai = (int)(pi % na);
        const double th = ai * a_step * M_PI / 180;
        double n[2] = {std::cos(th), std::sin(th)}, d = -diag + ri * r_step;
        for (double band : {12.0, 6.0}) {   // weighted TLS on the pixels near the line
            double sw = 0, mx = 0, my = 0;
            int cnt = 0;
            for (size_t i = 0; i < np; ++i)
                if (std::fabs(Px[i] * n[0] + Py[i] * n[1] - d) < band) { sw += Wv[i]; mx += Wv[i] * Px[i]; my += Wv[i] * Py[i]; ++cnt; }
            if (cnt < 50 || sw <= 0) break;
            mx /= sw; my /= sw;
            double cxx = 0, cxy = 0, cyy = 0;
            for (size_t i = 0; i < np; ++i)
                if (std::fabs(Px[i] * n[0] + Py[i] * n[1] - d) < band) {
                    const double zx = Px[i] - mx, zy = Py[i] - my;
                    cxx += Wv[i] * zx * zx; cxy += Wv[i] * zx * zy; cyy += Wv[i] * zy * zy;
                }
            // eigenvector of the smallest eigenvalue of [[cxx cxy][cxy cyy]]
            const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy, l = tr / 2 - std::sqrt(std::max(0.0, tr * tr / 4 - det));
            double ex = cxy, ey = l - cxx;
            if (std::hypot(ex, ey) < 1e-12) { ex = l - cyy; ey = cxy; }
            if (std::hypot(ex, ey) < 1e-12) { ex = n[0]; ey = n[1]; }
            const double en = std::hypot(ex, ey);
            ex /= en; ey /= en;
            if (ex * n[0] + ey * n[1] < 0) { ex = -ex; ey = -ey; }
            n[0] = ex; n[1] = ey;
            d = n[0] * mx + n[1] * my;
        }
        // band half-width from the perpendicular profile
        std::vector<double> prof(91, 0.0);
        int cnt = 0;
        for (size_t i = 0; i < np; ++i) {
            const double off = Px[i] * n[0] + Py[i] * n[1] - d;
            if (std::fabs(off) < 45) { const int k = (int)std::lround(off + 45); if (k >= 0 && k <= 90) prof[k] += Wv[i]; ++cnt; }
        }
        if (cnt < 50) continue;
        std::vector<double> ps(91, 0.0);
        for (int k = 0; k <= 90; ++k) for (int j = -1; j <= 1; ++j) if (k + j >= 0 && k + j <= 90) ps[k] += prof[k + j] / 3;
        int c = 35;
        for (int k = 35; k < 56; ++k) if (ps[k] > ps[c]) c = k;
        const double lv = 0.25 * ps[c];
        int lo = c, hi = c;
        while (lo > 0 && ps[lo - 1] > lv) --lo;
        while (hi < 90 && ps[hi + 1] > lv) ++hi;
        d += (lo + hi) / 2.0 - 45;
        const double half = std::max(4.0, (hi - lo) / 2.0 + 2);
        const double u[2] = {-n[1], n[0]};
        std::vector<double> along;
        for (size_t i = 0; i < np; ++i)
            if (std::fabs(Px[i] * n[0] + Py[i] * n[1] - d) < half) along.push_back(Px[i] * u[0] + Py[i] * u[1]);
        if (along.size() < 50) continue;
        std::sort(along.begin(), along.end());
        auto pct = [&](double q) { const double p = q * (along.size() - 1); const size_t a = (size_t)p; const double f = p - a; return a + 1 < along.size() ? along[a] * (1 - f) + along[a + 1] * f : along[a]; };
        const double a0 = pct(0.01), a1 = pct(0.99);
        int nslot = 0;
        long long last = LLONG_MIN;
        for (double a : along) { const long long s = (long long)std::floor(a / 4); if (s != last) { ++nslot; last = s; } }
        const int sup = 4 * nslot;
        if (dbg) std::fprintf(stderr, "lines: candidate d %.0f half %.1f support %d px\n", d, half, sup);
        if (sup < opt.minLen) continue;
        lines.push_back(Line{{n[0], n[1]}, d, half, a0, a1, acc[pi], sup});
    }
    // merge duplicates: near-parallel and overlapping bands
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.score > b.score; });
    std::vector<Line> keep;
    for (const Line& L : lines) {
        bool dup = false;
        for (const Line& K : keep) {
            const double ang = std::acos(std::min(1.0, std::fabs(L.n[0] * K.n[0] + L.n[1] * K.n[1]))) * 180 / M_PI;
            if (ang < 2.0) {
                const double am = (L.a0 + L.a1) / 2, mx = am * -L.n[1] + L.d * L.n[0], my = am * L.n[0] + L.d * L.n[1];
                if (std::fabs(mx * K.n[0] + my * K.n[1] - K.d) < L.half + K.half) { dup = true; break; }
            }
        }
        if (!dup) keep.push_back(L);
        if ((int)keep.size() >= opt.maxLines) break;
    }
    // left -> right by midpoint x
    std::vector<std::pair<double, Roi>> tmp;
    for (const Line& L : keep) {
        const double ux = -L.n[1], uy = L.n[0];
        Roi r;
        r.type = RoiType::Line;
        r.pts = {QPointF(L.d * L.n[0] + L.a0 * ux, L.d * L.n[1] + L.a0 * uy), QPointF(L.d * L.n[0] + L.a1 * ux, L.d * L.n[1] + L.a1 * uy)};
        r.half = L.half;
        r.source = "lines";
        tmp.emplace_back((r.pts[0].x() + r.pts[1].x()) / 2, r);
    }
    std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& t : tmp) out.push_back(std::move(t.second));
    return out;
}

std::vector<Roi> detect_blobs(const std::vector<float>& count, int W, int H, const BlobOptions& o) {
    const std::vector<uint8_t> mask = activity_mask(count, W, H, o.maskK, nullptr);
    std::vector<int> lab((size_t)W * H, 0);
    struct Blob { int label; int area; double act; };
    std::vector<Blob> blobs;
    std::vector<int> stack;
    int next = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i0 = (size_t)y * W + x;
            if (!mask[i0] || lab[i0]) continue;
            ++next;
            Blob b{next, 0, 0};
            stack.assign(1, (int)i0);
            lab[i0] = next;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                ++b.area;
                b.act += count[i];
                const int cx = i % W, cy = i / W;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        const size_t j = (size_t)ny * W + nx;
                        if (mask[j] && !lab[j]) { lab[j] = next; stack.push_back((int)j); }
                    }
            }
            if (b.area >= o.minArea) blobs.push_back(b);
        }
    std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.act > b.act; });
    if ((int)blobs.size() > o.maxBlobs) blobs.resize(o.maxBlobs);
    std::vector<Roi> out;
    for (const Blob& b : blobs) {
        Roi r;
        r.type = RoiType::Mask;
        r.maskW = W;
        r.maskH = H;
        r.mask.assign((size_t)W * H, 0);
        for (size_t i = 0; i < lab.size(); ++i) if (lab[i] == b.label) r.mask[i] = 1;
        for (int g = 0; g < o.grow; ++g) {   // dilate by one pixel (4-neighbourhood) per step
            std::vector<uint8_t> m2 = r.mask;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const size_t i = (size_t)y * W + x;
                    if (r.mask[i]) continue;
                    if ((x > 0 && r.mask[i - 1]) || (x + 1 < W && r.mask[i + 1]) || (y > 0 && r.mask[i - W]) || (y + 1 < H && r.mask[i + W])) m2[i] = 1;
                }
            r.mask.swap(m2);
        }
        r.source = "blobs";
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace evs
