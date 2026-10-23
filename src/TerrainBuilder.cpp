#include <PereVia/OsgKit.h>
#include "IslandLOD.h"
#include <osg/BlendFunc>
#include <osg/Geometry>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace Pyrive::OsgKit {
namespace {
bool finite(const osg::Vec3d& p) {
    return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z());
}
bool validColor(const osg::Vec4& color) {
    for (unsigned i = 0; i < 4; ++i)
        if (!std::isfinite(color[i]) || color[i] < 0 || color[i] > 1) return false;
    return true;
}
}

// 规则高程已有完整的邻接关系，因此不需要 Earcut：直接将每个网格单元拆成两面。
// 流程：数据校验 → 坐标/高度映射 → 建立共享顶点和索引 → 面积加权法线 → OSG 节点。
osg::ref_ptr<osg::Node> TerrainBuilder::build(const TerrainOptions& options) {
    if (options.enableLOD &&
        (!std::isfinite(options.lod1TriangleRatio) || options.lod1TriangleRatio <= 0 || options.lod1TriangleRatio >= 1 ||
         !std::isfinite(options.lod0MinPixels) || !std::isfinite(options.lod1MinPixels) ||
         options.lod1MinPixels <= 0 || options.lod0MinPixels <= options.lod1MinPixels ||
         options.impostorResolution < 32 || options.impostorResolution > 1024 ||
         !std::isfinite(options.impostorTopAngle) || options.impostorTopAngle <= 0 || options.impostorTopAngle >= 90))
        throw std::invalid_argument("Invalid terrain LOD ratio, pixel ranges, resolution or view angle");
    // 1. 先用 64 位计算乘积，防止 unsigned 的 rows*columns 溢出绕过检查。
    const std::uint64_t count = std::uint64_t(options.columns) * options.rows;
    if (options.columns < 2 || options.rows < 2 || count > 4194304 || count != options.elevations.size())
        throw std::invalid_argument("Terrain requires rows/columns >=2 and exactly rows*columns elevations (max 4194304)");
    if (!finite(options.origin) || !std::isfinite(options.spacing.x()) || !std::isfinite(options.spacing.y()) ||
        options.spacing.x() <= 0 || options.spacing.y() <= 0 || !std::isfinite(options.elevationScale) ||
        options.elevationScale <= 0 || !validColor(options.lowColor) || !validColor(options.highColor))
        throw std::invalid_argument("Invalid terrain origin, spacing, elevation scale or color");

    // 2. 物理跨度是 (采样点数-1)*间距。500 个点只有 499 段，不能误算成 500 段。
    const double width = (options.columns - 1.0) * options.spacing.x();
    const double depth = (options.rows - 1.0) * options.spacing.y();
    if (!std::isfinite(width) || !std::isfinite(depth))
        throw std::invalid_argument("Terrain horizontal extent overflows");
    std::vector<double> xs(options.columns), ys(options.rows), heights(static_cast<size_t>(count));
    for (unsigned c = 0; c < options.columns; ++c) {
        xs[c] = options.origin.x() + c * options.spacing.x();
        if (!std::isfinite(xs[c]) || (c > 0 && xs[c] <= xs[c - 1]))
            throw std::invalid_argument("Terrain X coordinates overflow or lose sample spacing precision");
    }
    for (unsigned r = 0; r < options.rows; ++r) {
        ys[r] = options.origin.y() + r * options.spacing.y();
        if (!std::isfinite(ys[r]) || (r > 0 && ys[r] <= ys[r - 1]))
            throw std::invalid_argument("Terrain Y coordinates overflow or lose sample spacing precision");
    }
    double minimum = std::numeric_limits<double>::max(), maximum = -minimum;
    for (size_t i = 0; i < heights.size(); ++i) {
        // 严格保留输入高程，只执行用户指定的比例和基准转换，不进行平滑或随机修改。
        heights[i] = options.origin.z() + options.elevations[i] * options.elevationScale;
        if (!std::isfinite(options.elevations[i]) || !std::isfinite(heights[i]))
            throw std::invalid_argument("Terrain elevations and resulting Z coordinates must be finite");
        minimum = std::min(minimum, heights[i]); maximum = std::max(maximum, heights[i]);
    }
    const double heightRange = maximum - minimum;
    if (!std::isfinite(heightRange)) throw std::invalid_argument("Terrain height range overflows");

    // 3. 每个采样点只存一份，供相邻单元共享。位置为：
    // P(c,r)=(origin.x+c*dx, origin.y+r*dy, origin.z+scale*elevations[r*columns+c])。
    // UV=(c/(columns-1),r/(rows-1))，把整个栅格映射到 [0,1]²，便于宿主叠加纹理。
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray;
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
    vertices->reserve(heights.size()); colors->reserve(heights.size()); uv->reserve(heights.size());
    for (unsigned r = 0; r < options.rows; ++r) for (unsigned c = 0; c < options.columns; ++c) {
        const size_t index = size_t(r) * options.columns + c;
        vertices->push_back(osg::Vec3d(xs[c], ys[r], heights[index]));
        // C=(1-t)*lowColor+t*highColor；平坦地形无高度差，统一使用低处颜色。
        const float t = heightRange > 0 ? static_cast<float>((heights[index] - minimum) / heightRange) : 0;
        colors->push_back(options.lowColor * (1 - t) + options.highColor * t);
        uv->push_back(osg::Vec2(static_cast<float>(c) / (options.columns - 1),
                               static_cast<float>(r) / (options.rows - 1)));
    }

    // 4. 一个网格单元：c----d，拆成 (a,b,d)、(a,d,c)，沿对角线 a-d 切分。
    //                 |  / |  四角索引来自相邻两行，两面的 XY 绕序均为逆时针，法线朝上。
    //                 a----b  三角面数=2*(columns-1)*(rows-1)，索引数=面数*3。
    osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    indices->reserve(size_t(options.columns - 1) * (options.rows - 1) * 6);
    std::vector<osg::Vec3d> accumulated(heights.size());
    const double normalScale = std::max({width, depth, heightRange});
    auto triangle = [&](unsigned a, unsigned b, unsigned c) {
        indices->push_back(a); indices->push_back(b); indices->push_back(c);
        // OSG ^ 是叉积；未单位化叉积模长为面面积的两倍。
        // 累加后再单位化，使共享顶点具有面积加权的平滑法线。
        // 统一缩放边向量可减小溢出风险，不改变最后的法线方向及面积相对权重。
        const osg::Vec3d normal = (((*vertices)[b] - (*vertices)[a]) / normalScale) ^
                                 (((*vertices)[c] - (*vertices)[a]) / normalScale);
        if (!finite(normal) || normal.z() <= 0)
            throw std::invalid_argument("Terrain triangles are numerically degenerate");
        accumulated[a] += normal; accumulated[b] += normal; accumulated[c] += normal;
    };
    for (unsigned r = 0; r + 1 < options.rows; ++r) for (unsigned c = 0; c + 1 < options.columns; ++c) {
        const unsigned a = r * options.columns + c, b = a + 1, leftAbove = a + options.columns, d = leftAbove + 1;
        triangle(a, b, d); triangle(a, d, leftAbove);
    }
    normals->reserve(heights.size());
    for (auto& normal : accumulated) {
        // 先按最大分量缩放，再 normalize，避免非常陡峭的有限法线在求长度时溢出。
        const double largest = std::max({std::abs(normal.x()), std::abs(normal.y()), std::abs(normal.z())});
        if (!finite(normal) || largest <= 0) throw std::invalid_argument("Terrain normal cannot be represented");
        normal /= largest; normal.normalize(); normals->push_back(osg::Vec3(normal));
    }

    // 5. 所有单元放进一个索引式 Geometry；启用 VBO，返回独立 Geode，不自动挂接。
    osg::ref_ptr<osg::Geometry> mesh = new osg::Geometry;
    mesh->setVertexArray(vertices.get()); mesh->setNormalArray(normals.get(), osg::Array::BIND_PER_VERTEX);
    mesh->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX); mesh->setTexCoordArray(0, uv.get());
    mesh->addPrimitiveSet(indices.get()); mesh->setUseDisplayList(false); mesh->setUseVertexBufferObjects(true);
    osg::ref_ptr<osg::Geode> terrain = new osg::Geode;
    terrain->setName("PereVia.Terrain"); terrain->addDrawable(mesh.get());
    if (options.lowColor.a() < 1 || options.highColor.a() < 1) {
        auto* state = terrain->getOrCreateStateSet();
        state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        state->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    }
    terrain->setUserValue("PereVia.TriangleCount", static_cast<unsigned>(indices->size() / 3));
    terrain->setUserValue("PereVia.MinElevation", minimum); terrain->setUserValue("PereVia.MaxElevation", maximum);
    if (!options.enableLOD) return terrain;

    // 6. LOD1 先在同一 XY 覆盖范围内建立更稀疏的规则栅格，避免直接对近 50 万面
    // 做边折叠。目标单元数 C'=C*ratio；按原网格纵横比例分配行列段数，向上取整
    // 保留略多于目标的面数，再交给共享简化器逼近约 5%。任一轴至少保留一段。
    const double targetCells = double(options.columns - 1) * (options.rows - 1) * options.lod1TriangleRatio;
    const double aspect = double(options.columns - 1) / (options.rows - 1);
    unsigned columnSegments = static_cast<unsigned>(std::ceil(std::sqrt(targetCells * aspect)));
    columnSegments = std::clamp(columnSegments, 1u, options.columns - 1);
    columnSegments = std::min(columnSegments, std::max(1u, static_cast<unsigned>(std::ceil(targetCells))));
    unsigned rowSegments = static_cast<unsigned>(std::ceil(targetCells / columnSegments));
    rowSegments = std::clamp(rowSegments, 1u, options.rows - 1);
    columnSegments = std::max(columnSegments, static_cast<unsigned>(std::ceil(targetCells / rowSegments)));

    TerrainOptions coarse;
    coarse.columns = columnSegments + 1; coarse.rows = rowSegments + 1;
    coarse.origin = options.origin; coarse.spacing.set(width / columnSegments, depth / rowSegments);
    coarse.elevationScale = options.elevationScale; coarse.lowColor = options.lowColor; coarse.highColor = options.highColor;
    // enableLOD 保持默认 false，递归只生成粗 Geode，不再递归创建新的 LOD。
    coarse.elevations.reserve(size_t(coarse.columns) * coarse.rows);
    auto lerp = [](double a, double b, double t) {
        // 同号用差值式、异号用加权式，避免两种形式各自可能出现的中间值溢出。
        return std::signbit(a) == std::signbit(b) ? a + (b - a) * t : a * (1 - t) + b * t;
    };
    for (unsigned r = 0; r < coarse.rows; ++r) for (unsigned c = 0; c < coarse.columns; ++c) {
        // 粗采样点映射回原始行列坐标，四邻点做双线性插值：
        // h=(1-ty)*[(1-tx)*h00+tx*h10]+ty*[(1-tx)*h01+tx*h11]。
        // 这是 LOD1 的近似重采样；LOD0 始终保留全部原始高程。
        const double x = std::min(double(options.columns - 1), double(c) * (options.columns - 1) / columnSegments);
        const double y = std::min(double(options.rows - 1), double(r) * (options.rows - 1) / rowSegments);
        const unsigned x0 = std::min(static_cast<unsigned>(x), options.columns - 2);
        const unsigned y0 = std::min(static_cast<unsigned>(y), options.rows - 2);
        const double tx = x - x0, ty = y - y0;
        const size_t a = size_t(y0) * options.columns + x0, b = a + options.columns;
        coarse.elevations.push_back(lerp(lerp(options.elevations[a], options.elevations[a + 1], tx),
            lerp(options.elevations[b], options.elevations[b + 1], tx), ty));
    }
    auto coarseNode = TerrainBuilder::build(coarse);
    auto* coarseGeode = static_cast<osg::Geode*>(coarseNode.get());
    auto* coarseMesh = coarseGeode->getDrawable(0)->asGeometry();
    auto* coarseColors = static_cast<osg::Vec4Array*>(coarseMesh->getColorArray());
    const auto* coarseVertices = static_cast<const osg::Vec3dArray*>(coarseMesh->getVertexArray());
    // 重采样可能错过最高点；两级颜色仍统一使用完整地形的 Z 范围，避免整体色带跳变。
    for (size_t i = 0; i < coarseColors->size(); ++i) {
        const float t = heightRange > 0 ? static_cast<float>(std::clamp(((*coarseVertices)[i].z() - minimum) / heightRange, 0.0, 1.0)) : 0;
        (*coarseColors)[i] = options.lowColor * (1 - t) + options.highColor * t;
    }
    // 7. 共用岛屿的简化、两图烘焙和相机角度选择，按屏幕尺寸组织完整/简化/面片三级。
    auto lod = Detail::createMeshLOD(terrain.get(), coarseGeode, {options.lod1TriangleRatio,
        options.lod0MinPixels, options.lod1MinPixels, options.impostorResolution, options.impostorTopAngle, "PereVia.Terrain"});
    lod->setUserValue("PereVia.MinElevation", minimum); lod->setUserValue("PereVia.MaxElevation", maximum);
    return lod;
}
}
