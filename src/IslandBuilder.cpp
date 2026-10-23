#include <PereVia/OsgKit.h>
#include "IslandLOD.h"

#include <osg/Geometry>
#include <osg/BlendFunc>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <mapbox/earcut.hpp>

namespace PereVia::OsgKit {
namespace {
using Triangle = std::array<unsigned, 3>;
// 二维几何判断的数值容差。下面先归一化坐标，再判断共线、相交和退化，
// 因此 epsilon 用于局部坐标中的计算，不等同于场景中固定长度的误差。
constexpr double epsilon = 1e-12;

// 统一报告非法输入/无法安全生成的几何体；调用者可以捕获 invalid_argument。
void invalid(const char* message) { throw std::invalid_argument(message); }

// 二维有向叉积 (B-A)×(C-A)，也等于三角形 ABC 的两倍有向面积。
// >0：A→B→C 逆时针，C 在有向边 AB 左侧；<0：顺时针；接近 0：共线。
double cross(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& c) {
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}

// 判断 P 是否落在线段 AB 上（含端点）。先用叉积判断共线，
// 再用点积 (P-A)·(P-B)<=0 判断 P 是否位于两个端点之间。
// 注意 OSG 向量之间的 operator* 是点积，向量乘标量才是缩放。
bool onSegment(const osg::Vec2d& a, const osg::Vec2d& b, const osg::Vec2d& p) {
    return std::abs(cross(a, b, p)) <= epsilon &&
        (p - a) * (p - b) <= epsilon;
}

// 判断闭线段 AB 和 CD 是否相交，接触端点、共线重叠也算相交。
// 一般情况：C/D 分处 AB 两侧，同时 A/B 分处 CD 两侧，即两个叉积组分别异号。
// 退化/接触情况交给 onSegment 检查，用于拒绝自交轮廓及相互接触的内外环。
bool intersects(const osg::Vec2d& a, const osg::Vec2d& b,
                const osg::Vec2d& c, const osg::Vec2d& d) {
    const double abC = cross(a, b, c), abD = cross(a, b, d);
    const double cdA = cross(c, d, a), cdB = cross(c, d, b);
    return (((abC > epsilon && abD < -epsilon) || (abC < -epsilon && abD > epsilon)) &&
        ((cdA > epsilon && cdB < -epsilon) || (cdA < -epsilon && cdB > epsilon))) ||
        onSegment(a, b, c) || onSegment(a, b, d) || onSegment(c, d, a) || onSegment(c, d, b);
}

// 顶点颜色及透明度均要求为有限数值，RGBA 四个分量都在 [0,1]。
bool validColor(const osg::Vec4& color) {
    for (unsigned i = 0; i < 4; ++i)
        if (!std::isfinite(color[i]) || color[i] < 0 || color[i] > 1) return false;
    return true;
}

// 将已整理的内外环交给 Earcut，返回朝向 +Z 的初始三角形索引。
// rings[0] 是外环，其余为孔洞；points 按相同顺序将这些环依次展开，
// 所以 Earcut 返回的索引可以直接用于 points，而不需要复制/新增几何顶点。
// 本函数只负责平面剖分，不生成高度，也不进行后面的四分细化。
std::vector<Triangle> triangulate(const std::vector<osg::Vec2d>& points, const std::vector<std::vector<osg::Vec2d>>& rings) {
    using Point = std::array<double, 2>;
    std::vector<std::vector<Point>> polygon;
    for (const auto& coast : rings) {
        polygon.emplace_back();
        for (const auto& p : coast) polygon.back().push_back({p.x(), p.y()});
    }
    const auto indices = mapbox::earcut<std::uint32_t>(polygon);
    // 无额外内部顶点的多边形，n 个边界顶点、h 个孔洞应得到 n+2h-2 个三角形。
    // 每个三角形占 3 个索引；这里检查是否缺面，后面还要检查索引、朝向和面积。
    if (indices.size() != (points.size() + 2 * (rings.size() - 1) - 2) * 3)
        invalid("Earcut did not produce a complete island triangulation");
    std::vector<Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    double triangleArea = 0, polygonArea = 0;
    // 鞋带公式：2*S = Σ(x_i*y_(i+1)-y_i*x_(i+1))。
    // 外环为逆时针（正面积）、孔洞为顺时针（负面积），相加自动扣除孔洞。
    // 此处与下面的 triangleArea 都保存“两倍面积”，所以比较时无需除以 2。
    for (const auto& ring : rings)
        for (size_t i = 0; i < ring.size(); ++i)
            polygonArea += cross(osg::Vec2d(), ring[i], ring[(i + 1) % ring.size()]);
    for (size_t i = 0; i < indices.size(); i += 3) {
        Triangle t{indices[i], indices[i + 1], indices[i + 2]};
        for (unsigned index : t)
            if (index >= points.size()) invalid("Earcut returned an invalid island index");
        double area = cross(points[t[0]], points[t[1]], points[t[2]]);
        if (std::abs(area) <= epsilon) invalid("Island triangulation has a degenerate triangle");
        // 将地表三角形统一成逆时针，使三维叉积法线朝 +Z。
        if (area < 0) { std::swap(t[1], t[2]); area = -area; }
        triangleArea += area;
        triangles.push_back(t);
    }
    // 检查三角形投影面积总和是否等于外环面积减去孔洞面积。
    // max(1,|polygonArea|) 组合绝对/相对容差；这是面积守恒检查，不单独证明拓扑正确。
    if (std::abs(triangleArea - std::abs(polygonArea)) > epsilon * std::max(1.0, std::abs(polygonArea)))
        invalid("Island triangulation does not preserve contour area");
    return triangles;
}

// 整理单个边界环：转换到统一的局部坐标，去掉可选的闭合重复首点，
// 拒绝零长度边、自交/接触及折返重叠，移除共线冗余点，最后统一方向。
// low/scale 由外环确定，所有内环共用同一坐标系，不能各自独立归一化。
// outer=true 生成逆时针外环；false 生成顺时针孔洞环。
std::vector<osg::Vec2d> cleanRing(const std::vector<osg::Vec2d>& input,
                               const osg::Vec2d& low, double scale, bool outer) {
    if (input.size() < 3 || input.size() > 2049) invalid("Each island ring must contain at least three points");
    std::vector<osg::Vec2d> points;
    for (const auto& p : input) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) invalid("Island rings must contain finite coordinates");
        // P_local=(P_scene-low)/scale：平移并等比例缩放，不改变角度和相交关系。
        const auto local = (p - low) / scale;
        if (!std::isfinite(local.x()) || !std::isfinite(local.y())) invalid("Island ring coordinates exceed usable extent");
        points.push_back(local);
    }
    // length2 是长度平方，比较时容差也平方，省去一次 sqrt。
    if ((points.front() - points.back()).length2() <= epsilon * epsilon) points.pop_back();
    if (points.size() < 3) invalid("Island ring needs three distinct vertices");
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i]; const auto& b = points[(i + 1) % points.size()];
        if ((a - b).length2() <= epsilon * epsilon) invalid("Island ring has duplicate adjacent points");
        const auto& c = points[(i + 2) % points.size()];
        // 共线且 AB·BC<0 表示沿同一直线折返，会使两条相邻边重叠。
        if (std::abs(cross(a, b, c)) <= epsilon && (b - a) * (c - b) < 0)
            invalid("Island ring has overlapping adjacent edges");
        // 检查不相邻边；相邻边共享一个端点是正常的，因此跳过。
        for (size_t j = i + 1; j < points.size(); ++j) {
            if (j == i + 1 || (i == 0 && j + 1 == points.size())) continue;
            if (intersects(a, b, points[j], points[(j + 1) % points.size()]))
                invalid("Island ring must not intersect or touch itself");
        }
    }
    // 在排除折返之后，共线的中间点只增加三角化难度，不改变边界形状。
    // 删除后邻接关系会变化，因此循环到不再存在可删除的点。
    bool changed = true;
    while (changed && points.size() > 3) {
        changed = false;
        for (size_t i = 0; i < points.size(); ++i) {
            if (std::abs(cross(points[(i + points.size() - 1) % points.size()], points[i],
                               points[(i + 1) % points.size()])) <= epsilon) {
                points.erase(points.begin() + i); changed = true; break;
            }
        }
    }
    // 再次使用鞋带公式判定面积是否退化，并根据符号判定环的方向。
    double area = 0;
    for (size_t i = 0; i < points.size(); ++i)
        area += cross(osg::Vec2d(), points[i], points[(i + 1) % points.size()]);
    if (std::abs(area) <= epsilon) invalid("Island ring has zero area");
    // 所有有向岸线的左侧均为陆地：外环逆时针、孔洞顺时针。
    if ((area > 0) != outer) std::reverse(points.begin(), points.end());
    return points;
}

// 水平射线的奇偶规则：从 P 向 +X 发射射线，与环相交奇数次则在内部。
// 调用者已用 intersects 排除环之间的接触，所以这里不负责区分“恰在边界上”。
bool inside(const osg::Vec2d& point, const std::vector<osg::Vec2d>& ring) {
    bool result = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const auto& a = ring[i]; const auto& b = ring[j];
        // 第一项用半开区间判断该边是否跨过 P 的水平线，避免顶点被重复计数，
        // 并保证 b.y-a.y 非零。第二项求交点 x=a.x+(b.x-a.x)*(P.y-a.y)/(b.y-a.y)，
        // 只统计位于 P 右侧的交点。
        if ((a.y() > point.y()) != (b.y() > point.y()) &&
            point.x() < (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) + a.x())
            result = !result;
    }
    return result;
}
}

// 岛屿创建总入口。数据流：
// 参数校验 → 整理内外环 → Earcut 平面剖分 → 重心插点/四分细化 → 距岸求高
// → 地表法线/颜色 → 底面与内外侧壁 → Geode →（可选）三级 LOD。
// 计算和资源创建都发生在本次调用中，不会给生成的岛屿安装逐帧重建回调。
// 返回 Node 是为了统一实际的 Geode/LOD 类型；本函数不向宿主场景自动 addChild。
osg::ref_ptr<osg::Node> IslandBuilder::build(const IslandOptions& options) {
    // 1. 校验参数。只有启用 LOD 才读取 LOD 专用设置；屏幕尺寸阈值必须有序，
    // 面数比例必须在 (0,1)，纹理分辨率和观察角度也必须在规定范围内。
    if (options.enableLOD &&
        (!std::isfinite(options.lod1TriangleRatio) || options.lod1TriangleRatio <= 0 || options.lod1TriangleRatio >= 1 ||
         !std::isfinite(options.lod0MinPixels) || !std::isfinite(options.lod1MinPixels) ||
         options.lod1MinPixels <= 0 || options.lod0MinPixels <= options.lod1MinPixels ||
         options.impostorResolution < 32 || options.impostorResolution > 1024 ||
         !std::isfinite(options.impostorTopAngle) || options.impostorTopAngle <= 0 || options.impostorTopAngle >= 90))
        invalid("Invalid island LOD ratio, pixel ranges, texture resolution or view angle");
    // 除了要求高度/深度为有限正数，还检查加减 seaLevel 后确实有高度差：
    // 很大的浮点基值可能使一个很小的正高度在相加时被舍入掉。
    if (!std::isfinite(options.seaLevel) || !std::isfinite(options.height) || options.height <= 0 ||
        !std::isfinite(options.baseDepth) || options.baseDepth <= 0 ||
        !std::isfinite(options.seaLevel + options.height) ||
        !std::isfinite(options.seaLevel - options.baseDepth) ||
        options.seaLevel + options.height <= options.seaLevel ||
        options.seaLevel - options.baseDepth >= options.seaLevel ||
        !std::isfinite(options.slopeExponent) || options.slopeExponent <= 0 ||
        options.subdivisions > 7 || !validColor(options.beachColor) ||
        !validColor(options.landColor) || !validColor(options.rockColor))
        invalid("Invalid island height, depth, subdivisions, slope or color");
    if (options.contour.size() < 3 || options.contour.size() > 2049)
        invalid("Island contour must contain 3..2048 points (plus optional closing point)");
    if (options.holes.size() > 64) invalid("Island supports at most 64 holes");
    size_t inputPoints = options.contour.size();
    for (const auto& hole : options.holes) inputPoints += hole.size();
    // 原始输入允许“每个环额外重复一次首点”；整理后再检查有效点合计 <=2048。
    if (inputPoints > 2048 + options.holes.size() + 1)
        invalid("Island rings exceed the total vertex limit (including optional closing points)");

    // 2. 计算外环包围盒。scale=max(X跨度,Y跨度) 保持 XY 等比例缩放，
    // 使外环局部坐标落在单位正方形内，减小平移/整体缩放对几何判断的影响。
    osg::Vec2d low = options.contour.front(), high = low;
    for (const auto& p : options.contour) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) invalid("Island contour must be finite");
        for (unsigned j = 0; j < 2; ++j) {
            low[j] = std::min(low[j], p[j]); high[j] = std::max(high[j], p[j]);
        }
    }
    const double scale = std::max(high.x() - low.x(), high.y() - low.y());
    if (!std::isfinite(scale) || scale <= 0) invalid("Island contour has no usable extent");
    std::vector<std::vector<osg::Vec2d>> coasts;
    // cleanRing 整理每个环，同时保证沿任何岸线前进时，陆地始终位于左侧。
    coasts.push_back(cleanRing(options.contour, low, scale, true));
    for (const auto& hole : options.holes) coasts.push_back(cleanRing(hole, low, scale, false));
    // 3. 检查不同环之间的关系：先逐边排除相交/接触，再用 inside 判定包含。
    // 两个简单环不相交时，一个代表点就能判定它们的包含关系。
    // 孔洞必须在外环内，孔洞之间既不能重叠，也不能互相包含（本接口不支持嵌套岛）。
    for (size_t i = 1; i < coasts.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            for (size_t a = 0; a < coasts[i].size(); ++a)
                for (size_t b = 0; b < coasts[j].size(); ++b)
                    if (intersects(coasts[i][a], coasts[i][(a + 1) % coasts[i].size()],
                                   coasts[j][b], coasts[j][(b + 1) % coasts[j].size()]))
                        invalid("Island rings must not intersect or touch each other");
            if (j == 0) {
                if (!inside(coasts[i].front(), coasts[0])) invalid("Island holes must be strictly inside the outer ring");
            } else if (inside(coasts[i].front(), coasts[j]) || inside(coasts[j].front(), coasts[i]))
                invalid("Island holes must not overlap or nest");
        }
    }
    // 4. 把所有环展开成一个顶点表；boundaries 单独保留每一圈的有序索引。
    // 后续细分、距岸计算和侧壁构建仍以每个环为单位，不能把不同环首尾相连。
    std::vector<osg::Vec2d> points;
    std::vector<std::vector<unsigned>> boundaries;
    for (const auto& coast : coasts) {
        boundaries.emplace_back();
        for (const auto& p : coast) {
            boundaries.back().push_back(static_cast<unsigned>(points.size()));
            points.push_back(p);
        }
    }
    if (points.size() > 2048) invalid("Island rings exceed 2048 total vertices");
    // 5. 在分配大网格之前估算面数：
    // n=内外岸总点数，h=孔洞数，s=细分次数。
    // 初始 Earcut 面数 F=n+2h-2；重心插点变成 3F 面；每轮四分再乘以 4。
    // 因此地表面数 T=3*(n+2h-2)*4^s。此处的 100 万上限只针对地表，
    // 最终还要加上同样多的底面，以及每条细分岸线边对应的两个侧壁三角形。
    const size_t coastPointCount = points.size();
    const size_t initialFaces = coastPointCount + 2 * options.holes.size() - 2;
    size_t triangleCount = initialFaces * 3;
    for (unsigned level = 0; level < options.subdivisions; ++level) triangleCount *= 4;
    if (triangleCount > 1000000) invalid("Island mesh exceeds one million surface triangles");
    // triangulate 将孔洞作为 Earcut 的内环输入，并校验结果索引、面数和面积。
    const auto ears = triangulate(points, coasts);
    std::vector<Triangle> triangles;
    // 6. 对每个初始三角形 ABC 插入重心 G=(A+B+C)/3，拆成 ABG、BCG、CAG。
    // 重心位于有效三角形内部，所以不会落进已挖空的孔洞；即便 s=0，
    // 也有离岸线非零距离的内部采样点，避免所有顶点都在海岸、整座岛完全平坦。
    for (const auto& t : ears) {
        const unsigned center = static_cast<unsigned>(points.size());
        points.push_back((points[t[0]] + points[t[1]] + points[t[2]]) / 3.0);
        for (unsigned j = 0; j < 3; ++j) triangles.push_back({t[j], t[(j + 1) % 3], center});
    }
    // 7. 每轮用三个边中点，把 ABC 拆成三个角部三角形和一个中央三角形。
    // 只改变采样密度，不平滑/移动原来的岸线；每轮地表面数乘 4，岸线边数乘 2。
    for (unsigned level = 0; level < options.subdivisions; ++level) {
        std::unordered_map<std::uint64_t, unsigned> midpoints;
        auto midpoint = [&](unsigned a, unsigned b) {
            // 无向边键：把较小的顶点索引放高 32 位，较大的放低 32 位。
            // (a,b) 与 (b,a) 得到相同 key，使相邻三角形共享同一个中点索引。
            // 缓存每轮重新建立，避免把不同细分层级的边混为一谈。
            const std::uint64_t key = (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            const auto found = midpoints.find(key);
            if (found != midpoints.end()) return found->second;
            const unsigned index = static_cast<unsigned>(points.size());
            // 线段中点 M=(A+B)/2；这里只处理 XY，Z 会在统一求高阶段生成。
            points.push_back((points[a] + points[b]) * 0.5);
            midpoints.emplace(key, index);
            return index;
        };
        std::vector<Triangle> refined;
        refined.reserve(triangles.size() * 4);
        for (const auto& t : triangles) {
            const unsigned ab = midpoint(t[0], t[1]), bc = midpoint(t[1], t[2]), ca = midpoint(t[2], t[0]);
            refined.push_back({t[0], ab, ca}); refined.push_back({ab, t[1], bc});
            refined.push_back({ca, bc, t[2]}); refined.push_back({ab, bc, ca});
        }
        // 每个岸线环同步插入相同的中点；这样之后的侧壁顶边能和地表岸线对齐。
        for (auto& boundary : boundaries) {
            std::vector<unsigned> refinedBoundary;
            for (size_t i = 0; i < boundary.size(); ++i) {
                refinedBoundary.push_back(boundary[i]);
                refinedBoundary.push_back(midpoint(boundary[i], boundary[(i + 1) % boundary.size()]));
            }
            boundary.swap(refinedBoundary);
        }
        triangles.swap(refined);
    }
    // 8. 为每个采样点 P 求到所有原始内外岸线段的最短距离 d(P)。
    // 不使用“到边界顶点”的距离，否则稀疏边界会产生不自然的波纹；
    // coasts 保留未细分的线段，插入中点不改变岸线形状，没必要遍历更多小线段。
    // 这一阶段开销约为 O(采样点数 * 原始内外岸总边数)。
    std::vector<double> distances(points.size(), std::numeric_limits<double>::max());
    double maxDistance = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        for (const auto& coast : coasts)
            for (size_t j = 0; j < coast.size(); ++j) {
                const auto edge = coast[(j + 1) % coast.size()] - coast[j];
                // E=B-A；无限直线上的投影参数 t=((P-A)·E)/(E·E)。
                // clamp(t,0,1) 把投影限制在线段 AB 内，最近点 Q=A+tE。
                // |P-A-tE| 即 |P-Q|；对所有线段取 min 得到距岸距离。
                // cleanRing 已拒绝零长度边，所以分母 edge.length2() 不为零。
                const double t = std::clamp(((points[i] - coast[j]) * edge) / edge.length2(), 0.0, 1.0);
                distances[i] = std::min(distances[i], (points[i] - coast[j] - edge * t).length());
            }
        maxDistance = std::max(maxDistance, distances[i]);
    }
    // 明确把边界距离置零，消除投影计算中的舍入误差，保证海岸 Z 精确等于 seaLevel。
    for (const auto& boundary : boundaries)
        for (unsigned i : boundary) distances[i] = 0; // 内外海岸精确落在海平面。
    if (maxDistance <= epsilon) invalid("Island contour is too thin to generate terrain");
    // 9. 创建 OSG 网格数组：位置用 double，法线和 RGBA 颜色用 float，索引用 uint32。
    // osg::ref_ptr 负责引用计数；这些数组最后交给 Geometry 持有。
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray;
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    for (size_t i = 0; i < points.size(); ++i) {
        // 归一化高度 h=(d/dMax)^p，实际 Z=seaLevel+height*h。
        // d=0 时在海岸，d=dMax 时达到指定岛高；p>1 会压低中低地形、集中山顶。
        // dMax 是“当前离散采样网格”的最大岸距，不是连续区域的精确最大值，
        // 因而改变采样密度后，局部高度和最高点位置可能有小幅变化。
        const double h = std::pow(distances[i] / maxDistance, options.slopeExponent);
        // 撤销先前的平移/缩放，恢复宿主场景的 XY；Z 使用上面的高度公式。
        vertices->push_back(osg::Vec3d(low.x() + points[i].x() * scale,
            low.y() + points[i].y() * scale, options.seaLevel + options.height * h));
        // 颜色线性插值：C=(1-blend)*沙滩色+blend*陆地色。
        // blend=min(1,4h)，所以从海岸到 1/4 岛高完成颜色过渡；RGBA 都参与插值。
        const float blend = static_cast<float>(std::min(1.0, h * 4.0));
        colors->push_back(options.beachColor * (1 - blend) + options.landColor * blend);
    }
    // 10. 计算地表平滑法线：N_face=(B-A)×(C-A)，OSG 的 operator^ 表示三维叉积。
    // 叉积方向由顶点绕序决定，模长等于三角形面积的两倍。
    // 将“未归一化”的面法线累加到三个顶点，相当于按三角形面积加权；
    // 最后再单位化 N_vertex=ΣN_face/|ΣN_face|，避免每个面都有明显的独立明暗边界。
    std::vector<osg::Vec3d> accumulated(points.size());
    for (const auto& t : triangles) {
        // 所有边向量除以相同尺度 S 后再叉积，结果整体乘 1/S²。
        // 该公因子不改变最终单位法线的方向和相对面积权重，并降低大数乘积溢出风险。
        const double normalScale = std::max(scale, options.height);
        osg::Vec3d normal = (((*vertices)[t[1]] - (*vertices)[t[0]]) / normalScale) ^
                            (((*vertices)[t[2]] - (*vertices)[t[0]]) / normalScale);
        for (unsigned i : t) { accumulated[i] += normal; indices->push_back(i); }
    }
    for (auto& normal : accumulated) {
        normal.normalize(); normals->push_back(osg::Vec3(normal));
    }
    // 11. 将地表的 XY 顶点布局复制到平面 Z=seaLevel-baseDepth，构成底面。
    // bottom 是底面顶点在统一数组中的起始偏移，底面索引=bottom+地表索引。
    // 沿用同一剖分会保留孔洞；底面反转顶点绕序，其法线应朝 -Z。
    // 顶底分开存储顶点，避免它们的法线被平均；当前底面保留了完整细分密度。
    const unsigned bottom = static_cast<unsigned>(vertices->size());
    for (const auto& p : points) {
        vertices->push_back(osg::Vec3d(low.x() + p.x() * scale, low.y() + p.y() * scale,
                                     options.seaLevel - options.baseDepth));
        normals->push_back(osg::Vec3(0, 0, -1)); colors->push_back(options.rockColor);
    }
    for (const auto& t : triangles)
        for (unsigned j : {2u, 1u, 0u}) indices->push_back(bottom + t[j]);
    // 12. 为每条细分后的内外岸线边 AB 连接一个竖直矩形侧壁，再拆成两个三角形。
    // 顺序为 顶A→底A→底B→顶B，保证面朝陆地外部。每段侧壁独立使用四个顶点，
    // 与地表/底面法线隔离；这些重复顶点是硬边效果需要的，并非重复绘制整座岛。
    for (const auto& boundary : boundaries) for (size_t i = 0; i < boundary.size(); ++i) {
        const unsigned a = boundary[i], b = boundary[(i + 1) % boundary.size()];
        const unsigned start = static_cast<unsigned>(vertices->size());
        // 单位岸线方向 E=(ex,ey)，右侧水平法线为 (ey,-ex,0)。
        // cleanRing 让陆地在有向岸线左侧，所以右法线对外岸指向外海，对内岸指向孔洞。
        osg::Vec2d edge = points[b] - points[a]; edge.normalize();
        for (unsigned index : {a, bottom + a, bottom + b, b}) {
            const osg::Vec3d vertex = (*vertices)[index];
            vertices->push_back(vertex);
            normals->push_back(osg::Vec3(static_cast<float>(edge.y()), static_cast<float>(-edge.x()), 0));
            colors->push_back(options.rockColor);
        }
        for (unsigned index : {0u, 1u, 2u, 0u, 2u, 3u}) indices->push_back(start + index);
    }
    // 13. 将地表、底面和侧壁合并为一个索引式 GL_TRIANGLES Geometry。
    // BIND_PER_VERTEX 表示每个顶点各有一条法线和颜色；所有三角形共享这些数组。
    // 关闭旧式显示列表、启用 VBO，让 OSG 在渲染时管理 GPU 缓冲。
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    geometry->setVertexArray(vertices.get());
    geometry->setNormalArray(normals.get(), osg::Array::BIND_PER_VERTEX);
    geometry->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(indices.get());
    geometry->setUseDisplayList(false);
    geometry->setUseVertexBufferObjects(true);
    osg::ref_ptr<osg::Geode> island = new osg::Geode;
    island->setName("PereVia.Island");
    island->addDrawable(geometry.get());
    // 有半透明颜色时启用常规 alpha 混合：输出=源颜色*源alpha+背景*(1-源alpha)，
    // 并放入透明排序队列。这是几何层设置，远景面片另有 Alpha Test 策略。
    if (options.beachColor.a() < 1 || options.landColor.a() < 1 || options.rockColor.a() < 1) {
        auto* state = island->getOrCreateStateSet();
        state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        state->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    }
    // 14. 不启用 LOD 时，到这里即可返回完整 Geode（对外统一按 Node 持有）。
    if (!options.enableLOD) return island;
    // 启用 LOD：先用更低细分密度重采样，再交给 createIslandLOD 中的边折叠，
    // 避免直接对数十万面高精度网格做昂贵的简化。
    IslandOptions coarseOptions = options;
    // 递归调用只创建粗网格，必须关闭 LOD，防止递归生成无限层级。
    coarseOptions.enableLOD = false;
    // 总索引数/3 为完整模型面数（含底面与侧壁），乘比例得到 LOD1 目标面数。
    const double target = (indices->size() / 3) * options.lod1TriangleRatio;
    while (coarseOptions.subdivisions > 0) {
        const unsigned next = coarseOptions.subdivisions - 1;
        // 下一档细分数 k 的总面数：
        // 顶底 = 2*[3F*4^k]；侧壁 = 2*[n*2^k]，合计 6F*4^k+2n*2^k。
        // 若再降一档会少于目标，就停止，保留足够面数供后续边折叠逼近目标。
        const double count = 6.0 * initialFaces * std::pow(4.0, next) +
                             2.0 * coastPointCount * std::pow(2.0, next);
        if (count < target) break;
        coarseOptions.subdivisions = next;
    }
    // 此递归分支 enableLOD=false，故 coarse 的实际类型必定是 Geode。
    // createIslandLOD：简化粗网格作为 LOD1，从完整网格烘焙两图面片作为 LOD2，
    // 最后按屏幕投影尺寸组织 LOD0/1/2；实际面数可能因简化的拓扑限制偏离目标。
    auto coarse = IslandBuilder::build(coarseOptions);
    return Detail::createIslandLOD(island.get(), static_cast<osg::Geode*>(coarse.get()), options);
}
}
