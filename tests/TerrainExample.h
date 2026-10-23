#pragma once
#include <PereVia/OsgKit.h>
#include <cmath>

// 500×500 个高程采样点。模拟数据由宿主生成，TerrainBuilder 不依赖此公式。
inline Pyrive::OsgKit::TerrainOptions exampleTerrainOptions() {
    Pyrive::OsgKit::TerrainOptions options;
    options.columns = 500; options.rows = 500;
    options.origin.set(-249.5, -249.5, 0);
    options.spacing.set(1, 1); // 499×499 场景单位；若需正好 500×500，可设 500.0/499。
    options.enableLOD = true;
    options.elevations.reserve(size_t(options.columns) * options.rows);
    for (unsigned r = 0; r < options.rows; ++r) for (unsigned c = 0; c < options.columns; ++c) {
        const double x = options.origin.x() + c, y = options.origin.y() + r;
        // 椭圆高斯峰 G=exp(-0.5*((x-cx)^2/sx²+(y-cy)^2/sy²))。
        // 叠加山峰、负高斯山谷与不同频率的起伏，产生可重复的连续高程。
        auto gaussian = [&](double cx, double cy, double sx, double sy) {
            const double dx = (x - cx) / sx, dy = (y - cy) / sy;
            return std::exp(-0.5 * (dx * dx + dy * dy));
        };
        options.elevations.push_back(12 + 150 * gaussian(-80, 45, 65, 90) +
            100 * gaussian(95, -80, 80, 55) + 75 * gaussian(110, 120, 50, 65) -
            32 * gaussian(0, 10, 20, 160) + 14 * std::sin(0.035 * x + 0.018 * y) * std::cos(0.027 * y) +
            4 * std::sin(0.14 * x) * std::cos(0.11 * y));
    }
    return options;
}
