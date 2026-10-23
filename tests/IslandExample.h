#pragma once
#include <PereVia/OsgKit.h>
#include <cmath>

// 宿主示例：80 点曲折海岸，两个独立内环水域；无随机数，便于复现。
inline Pyrive::OsgKit::IslandOptions complexIslandOptions() {
    Pyrive::OsgKit::IslandOptions options;
    constexpr double tau = 6.283185307179586;
    for (unsigned i = 0; i < 80; ++i) {
        const double a = tau * i / 80;
        const double r = 1.0 + 0.14 * std::sin(3 * a + 0.3) + 0.10 * std::sin(7 * a) +
                         0.045 * std::cos(13 * a);
        options.contour.emplace_back(220 * r * std::cos(a), 170 * r * std::sin(a));
    }
    auto hole = [&](unsigned count, double x, double y, double rx, double ry, double phase) {
        options.holes.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            const double a = tau * i / count;
            const double r = 1.0 + 0.13 * std::sin(3 * a + phase);
            options.holes.back().emplace_back(x + rx * r * std::cos(a), y + ry * r * std::sin(a));
        }
    };
    hole(20, -65, 10, 44, 32, 0.2);
    hole(16, 65, -25, 28, 23, 0.7);
    options.height = 42;
    options.baseDepth = 12;
    options.slopeExponent = 1.25;
    options.subdivisions = 4;
    options.enableLOD = true;
    options.impostorResolution = 512;
    return options;
}
