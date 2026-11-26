#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace SuperSimpleSF2 {

struct VelocityCurve {
    double p1x = 0.33;
    double p1y = 0.33;
    double p2x = 0.67;
    double p2y = 0.67;

    void normalizedClamped(double& x1, double& y1, double& x2, double& y2) const {
        x1 = std::clamp(p1x, 0.0, 1.0);
        y1 = std::clamp(p1y, 0.0, 1.0);
        x2 = std::clamp(p2x, 0.0, 1.0);
        y2 = std::clamp(p2y, 0.0, 1.0);
        if (x1 > x2) {
            std::swap(x1, x2);
            std::swap(y1, y2);
        }
        if (x2 - x1 < 0.02) {
            if (x1 > 0.02) x1 = x2 - 0.02;
            else x2 = std::min(1.0, x1 + 0.02);
        }
    }

    // Builds 2..4 monotone-cubic knots in [0, 1] x [0, 1].
    // When P1 is snapped to x=0 and P2 to x=1, the curve becomes a direct line
    // between (0, y1) and (1, y2) — allowing a flat Fixed velocity at any level.
    int buildKnots(std::array<double, 4>& xs, std::array<double, 4>& ys) const {
        double x1, y1, x2, y2;
        normalizedClamped(x1, y1, x2, y2);

        const bool snapLeft = (x1 <= 0.015);
        const bool snapRight = (x2 >= 0.985);

        int n = 0;
        if (snapLeft) {
            xs[n] = 0.0;
            ys[n] = y1;
            ++n;
        } else {
            xs[n] = 0.0;
            ys[n] = 0.0;
            ++n;
            xs[n] = x1;
            ys[n] = y1;
            ++n;
        }

        if (snapRight) {
            xs[n] = 1.0;
            ys[n] = y2;
            ++n;
        } else {
            xs[n] = x2;
            ys[n] = y2;
            ++n;
            xs[n] = 1.0;
            ys[n] = 1.0;
            ++n;
        }
        return n;
    }

    float evaluate(float inVel) const {
        const double x = std::clamp(static_cast<double>(inVel), 0.0, 1.0);
        std::array<double, 4> xs{};
        std::array<double, 4> ys{};
        const int n = buildKnots(xs, ys);
        if (n < 2) return static_cast<float>(x);

        if (x <= xs[0]) return static_cast<float>(std::clamp(ys[0], 0.0, 1.0));
        if (x >= xs[n - 1]) return static_cast<float>(std::clamp(ys[n - 1], 0.0, 1.0));

        if (n == 2) {
            const double t = (x - xs[0]) / (xs[1] - xs[0]);
            return static_cast<float>(std::clamp(ys[0] + t * (ys[1] - ys[0]), 0.0, 1.0));
        }

        // Fritsch-Carlson monotone cubic Hermite tangents
        std::array<double, 3> secants{};
        for (int i = 0; i < n - 1; ++i) {
            const double dx = xs[i + 1] - xs[i];
            secants[i] = (dx > 1e-9) ? ((ys[i + 1] - ys[i]) / dx) : 0.0;
        }

        std::array<double, 4> tangents{};
        tangents[0] = secants[0];
        tangents[n - 1] = secants[n - 2];
        for (int i = 1; i < n - 1; ++i) {
            if (secants[i - 1] * secants[i] <= 0.0) {
                tangents[i] = 0.0;
            } else {
                tangents[i] = 0.5 * (secants[i - 1] + secants[i]);
            }
        }

        for (int i = 0; i < n - 1; ++i) {
            if (std::abs(secants[i]) < 1e-9) {
                tangents[i] = 0.0;
                tangents[i + 1] = 0.0;
            } else {
                const double a = tangents[i] / secants[i];
                const double b = tangents[i + 1] / secants[i];
                const double h2 = a * a + b * b;
                if (h2 > 9.0) {
                    const double scale = 3.0 / std::sqrt(h2);
                    tangents[i] = scale * a * secants[i];
                    tangents[i + 1] = scale * b * secants[i];
                }
            }
        }

        int seg = 0;
        for (int i = 0; i < n - 1; ++i) {
            if (x <= xs[i + 1]) {
                seg = i;
                break;
            }
        }

        const double h = xs[seg + 1] - xs[seg];
        if (h <= 1e-9) return static_cast<float>(std::clamp(ys[seg], 0.0, 1.0));
        const double t = (x - xs[seg]) / h;
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        const double h10 = t3 - 2.0 * t2 + t;
        const double h01 = -2.0 * t3 + 3.0 * t2;
        const double h11 = t3 - t2;

        const double y = h00 * ys[seg] + h10 * h * tangents[seg] +
                         h01 * ys[seg + 1] + h11 * h * tangents[seg + 1];
        return static_cast<float>(std::clamp(y, 0.0, 1.0));
    }
};

} // namespace SuperSimpleSF2
