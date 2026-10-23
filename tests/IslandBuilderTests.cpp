#include <PereVia/OsgKit.h>
#include "IslandExample.h"
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/Texture2D>
#include <osg/NodeCallback>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void verifyMesh(const Pyrive::OsgKit::IslandOptions& options, double area) {
    auto island = Pyrive::OsgKit::IslandBuilder::build(options);
    auto* geode = dynamic_cast<osg::Geode*>(island.get());
    require(geode && geode->getNumDrawables() == 1 && island->getNumParents() == 0,
        "Island must return standalone geometry");
    auto* mesh = dynamic_cast<osg::Geometry*>(geode->getDrawable(0));
    require(mesh != nullptr, "Island drawable must be geometry");
    auto* vertices = dynamic_cast<osg::Vec3dArray*>(mesh->getVertexArray());
    auto* normals = dynamic_cast<osg::Vec3Array*>(mesh->getNormalArray());
    auto* colors = dynamic_cast<osg::Vec4Array*>(mesh->getColorArray());
    auto* indices = dynamic_cast<osg::DrawElementsUInt*>(mesh->getPrimitiveSet(0));
    require(vertices && normals && colors && indices && normals->size() == vertices->size() &&
        colors->size() == vertices->size(), "Island arrays are inconsistent");
    double low = std::numeric_limits<double>::max(), high = -low, surfaceArea = 0, bottomArea = 0;
    // 按几何位置焊接：每条边必须恰好连接两个三角形，且方向相反。
    std::map<std::array<double, 3>, unsigned> positions;
    std::vector<unsigned> welded;
    for (size_t i = 0; i < vertices->size(); ++i) {
        const auto& p = (*vertices)[i];
        require(std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z()), "Non-finite island vertex");
        require(std::abs((*normals)[i].length() - 1) < 1e-5, "Island normal must be unit length");
        low = std::min(low, p.z()); high = std::max(high, p.z());
        const std::array<double, 3> key{p.x(), p.y(), p.z()};
        auto entry = positions.emplace(key, static_cast<unsigned>(positions.size()));
        welded.push_back(entry.first->second);
    }
    std::map<std::pair<unsigned, unsigned>, std::pair<unsigned, int>> edges;
    for (size_t i = 0; i < indices->size(); i += 3) {
        const unsigned a = (*indices)[i], b = (*indices)[i + 1], c = (*indices)[i + 2];
        require(a < vertices->size() && b < vertices->size() && c < vertices->size(), "Invalid island index");
        const auto normal = ((*vertices)[b] - (*vertices)[a]) ^ ((*vertices)[c] - (*vertices)[a]);
        require(normal.length2() > 0, "Degenerate island triangle");
        if (normal.z() > 0) surfaceArea += normal.z() * 0.5;
        if (normal.z() < 0) bottomArea -= normal.z() * 0.5;
        for (unsigned j = 0; j < 3; ++j) {
            unsigned from = welded[(*indices)[i + j]], to = welded[(*indices)[i + (j + 1) % 3]];
            auto& count = edges[{std::min(from, to), std::max(from, to)}];
            ++count.first; count.second += from < to ? 1 : -1;
        }
    }
    for (const auto& edge : edges)
        require(edge.second.first == 2 && edge.second.second == 0, "Island is not a closed oriented mesh");
    const auto euler = static_cast<long long>(positions.size()) - static_cast<long long>(edges.size()) +
                       static_cast<long long>(indices->size() / 3);
    require(euler == 2 - 2 * static_cast<long long>(options.holes.size()),
        "Island topology does not preserve the requested holes");
    require(std::abs(surfaceArea - area) < area * 1e-8 && std::abs(bottomArea - area) < area * 1e-8,
        "Island triangulation changed coastline area");
    require(std::abs(low - (options.seaLevel - options.baseDepth)) < 1e-8 &&
        std::abs(high - (options.seaLevel + options.height)) < 1e-8, "Island height/depth mismatch");
    auto again = Pyrive::OsgKit::IslandBuilder::build(options);
    auto* other = dynamic_cast<osg::Geode*>(again.get())->getDrawable(0)->asGeometry();
    require(*vertices == *dynamic_cast<osg::Vec3dArray*>(other->getVertexArray()), "Island must be deterministic");
}

void reject(const Pyrive::OsgKit::IslandOptions& options) {
    bool rejected = false;
    try { Pyrive::OsgKit::IslandBuilder::build(options); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Invalid island input was accepted");
}

size_t triangles(const osg::Geometry& mesh) {
    size_t result = 0;
    for (unsigned i = 0; i < mesh.getNumPrimitiveSets(); ++i) {
        const auto* primitive = mesh.getPrimitiveSet(i);
        require(primitive->getMode() == GL_TRIANGLES, "LOD mesh must use triangles");
        require(primitive->getNumIndices() % 3 == 0, "LOD indices must be triples");
        for (unsigned j = 0; j < primitive->getNumIndices(); ++j)
            require(primitive->index(j) < mesh.getVertexArray()->getNumElements(), "LOD index out of range");
        result += primitive->getNumIndices() / 3;
    }
    return result;
}

// 用实际 double 顶点检查射线，不依赖 OSG 各发行版对 Vec3d TriangleFunctor 的支持。
bool segmentHit(osg::Node* node, const osg::Vec3d& start, const osg::Vec3d& end, osg::Vec3d* normal = nullptr) {
    const auto* mesh = dynamic_cast<osg::Geode*>(node)->getDrawable(0)->asGeometry();
    const auto& vertices = *static_cast<const osg::Vec3dArray*>(mesh->getVertexArray());
    const auto direction = end - start;
    double nearest = std::numeric_limits<double>::max();
    for (unsigned i = 0; i < mesh->getNumPrimitiveSets(); ++i) {
        const auto* primitive = mesh->getPrimitiveSet(i);
        for (unsigned j = 0; j < primitive->getNumIndices(); j += 3) {
            const auto& a = vertices[primitive->index(j)];
            const auto e1 = vertices[primitive->index(j + 1)] - a;
            const auto e2 = vertices[primitive->index(j + 2)] - a;
            const auto p = direction ^ e2;
            const double determinant = e1 * p;
            if (std::abs(determinant) < 1e-12) continue;
            const auto offset = start - a;
            const double u = (offset * p) / determinant;
            const auto q = offset ^ e1;
            const double v = (direction * q) / determinant;
            const double t = (e2 * q) / determinant;
            if (u < -1e-9 || v < -1e-9 || u + v > 1 + 1e-9 || t < 0 || t > 1 || t >= nearest) continue;
            nearest = t;
            if (normal) { *normal = e1 ^ e2; normal->normalize(); }
        }
    }
    return nearest != std::numeric_limits<double>::max();
}

bool verticalHit(osg::Node* node, double x, double y) {
    return segmentHit(node, osg::Vec3d(x, y, 1000), osg::Vec3d(x, y, -1000));
}

double ringArea(const std::vector<osg::Vec2d>& ring) {
    double area = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const auto& a = ring[i]; const auto& b = ring[(i + 1) % ring.size()];
        area += (a.x() * b.y() - a.y() * b.x()) * 0.5;
    }
    return std::abs(area);
}

void holeTests() {
    Pyrive::OsgKit::IslandOptions options;
    options.contour = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    options.holes = {{{3, 3}, {7, 3}, {7, 7}, {3, 7}}, {{12, 12}, {17, 12}, {17, 17}, {12, 17}}};
    options.subdivisions = 2;
    verifyMesh(options, 359);
    for (auto& ring : options.holes) { std::reverse(ring.begin(), ring.end()); ring.push_back(ring.front()); }
    std::reverse(options.contour.begin(), options.contour.end());
    options.contour.push_back(options.contour.front());
    verifyMesh(options, 359);
    auto mesh = Pyrive::OsgKit::IslandBuilder::build(options);
    require(!verticalHit(mesh.get(), 5, 5) && !verticalHit(mesh.get(), 14, 14) && verticalHit(mesh.get(), 10, 10),
        "Hole interiors must remain empty through surface and bottom");
    osg::Vec3d wallNormal;
    require(segmentHit(mesh.get(), osg::Vec3d(5, 5, -2), osg::Vec3d(10, 5, -2), &wallNormal) && wallNormal.x() < -0.9,
        "Inner wall normal must face into the hole");
    auto valid = options;
    options.holes.push_back({{4, 4}, {6, 4}, {6, 6}, {4, 6}}); reject(options); // nested
    options = valid; options.holes.push_back({{6, 6}, {9, 6}, {9, 9}, {6, 9}}); reject(options); // overlap
    options = valid; options.holes = {{{0, 2}, {3, 2}, {3, 5}, {0, 5}}}; reject(options); // touch outer
    options = valid; options.holes = {{{19, 2}, {21, 2}, {21, 5}, {19, 5}}}; reject(options); // cross outer
    options = valid; options.holes = {{{25, 2}, {27, 2}, {27, 5}, {25, 5}}}; reject(options); // outside
    options = valid; options.holes.push_back({}); reject(options);
    options = valid; options.holes[0][0].x() = std::numeric_limits<double>::infinity(); reject(options);
    options = valid; options.holes.resize(65, valid.holes[0]); reject(options);
    options = valid; options.holes = {{{3, 3}, {7, 7}, {3, 7}, {7, 3}}}; reject(options); // self-cross

    auto complex = complexIslandOptions();
    double area = ringArea(complex.contour);
    for (const auto& hole : complex.holes) area -= ringArea(hole);
    complex.enableLOD = false; complex.subdivisions = 1;
    verifyMesh(complex, area);
    complex.enableLOD = true; complex.subdivisions = 3; complex.impostorResolution = 128;
    auto complexNode = Pyrive::OsgKit::IslandBuilder::build(complex);
    auto* lod = dynamic_cast<osg::LOD*>(complexNode.get());
    require(lod && lod->getNumChildren() == 3, "Holed island LOD missing");
    for (unsigned level = 0; level < 2; ++level)
        require(!verticalHit(lod->getChild(level), -65, 10) && !verticalHit(lod->getChild(level), 65, -25) &&
            verticalHit(lod->getChild(level), 0, 0), "LOD mesh must retain hole interiors and land");
    auto* selector = lod->getChild(2)->asGroup()->getChild(0)->asGroup();
    auto* top = dynamic_cast<osg::Geode*>(selector->getChild(0));
    auto* texture = dynamic_cast<osg::Texture2D*>(top->getDrawable(0)->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
    osg::Vec2d low = complex.contour.front(), high = low;
    for (const auto& p : complex.contour) for (unsigned j = 0; j < 2; ++j) {
        low[j] = std::min(low[j], p[j]); high[j] = std::max(high[j], p[j]);
    }
    for (const osg::Vec2d center : {osg::Vec2d(-65, 10), osg::Vec2d(65, -25)}) {
        const unsigned x = static_cast<unsigned>((center.x() - low.x()) / (high.x() - low.x()) * 126 + 1);
        const unsigned y = static_cast<unsigned>((center.y() - low.y()) / (high.y() - low.y()) * 126 + 1);
        require(texture->getImage()->data(x, y)[3] == 0, "Top impostor must retain transparent holes");
    }
    std::cout << "Island inner-ring and complex coastline tests passed\n";
}

void lodTests() {
    Pyrive::OsgKit::IslandOptions options;
    options.contour = {{-60, -25}, {-30, -45}, {5, -38}, {40, -45},
        {65, -10}, {45, 20}, {15, 10}, {5, 40}, {-35, 45}, {-55, 15}};
    options.enableLOD = true; options.subdivisions = 5; options.height = 25;
    options.impostorResolution = 64;
    auto node = Pyrive::OsgKit::IslandBuilder::build(options);
    auto* lod = dynamic_cast<osg::LOD*>(node.get());
    require(lod && lod->getNumChildren() == 3 && node->getNumParents() == 0,
        "enableLOD must return a standalone three-level LOD");
    require(lod->getRangeMode() == osg::LOD::PIXEL_SIZE_ON_SCREEN &&
        lod->getMinRange(0) == options.lod0MinPixels && lod->getMinRange(1) == options.lod1MinPixels &&
        lod->getMaxRange(1) == lod->getMinRange(0) && lod->getMinRange(2) == 0 &&
        lod->getMaxRange(2) == lod->getMinRange(1), "LOD pixel ranges must have no gaps or overlaps");
    auto* full = dynamic_cast<osg::Geode*>(lod->getChild(0));
    auto* coarse = dynamic_cast<osg::Geode*>(lod->getChild(1));
    require(full && coarse, "First two LOD children must be geodes");
    const size_t high = triangles(*full->getDrawable(0)->asGeometry());
    const size_t low = triangles(*coarse->getDrawable(0)->asGeometry());
    require(high == 49792 && low > 0 && low < high * 0.08 && low > high * 0.02,
        "LOD1 must approach the 5 percent face target");
    auto* transform = lod->getChild(2)->asGroup();
    require(transform && transform->getNumChildren() == 1, "Impostor transform missing");
    auto* selector = transform->getChild(0)->asGroup();
    require(selector && selector->getNumChildren() == 2 && selector->getCullCallback(), "Impostor view selector missing");
    class ViewProbe final : public osg::NodeVisitor {
    public:
        explicit ViewProbe(const osg::Vec3& eye) : osg::NodeVisitor(CULL_VISITOR, TRAVERSE_ACTIVE_CHILDREN), eye_(eye) {}
        osg::Vec3 getEyePoint() const override { return eye_; }
        void apply(osg::Geode& geode) override { selected = geode.getName(); ++visits; }
        std::string selected;
        unsigned visits = 0;
    private:
        osg::Vec3 eye_;
    };
    auto* callback = dynamic_cast<osg::NodeCallback*>(selector->getCullCallback());
    require(callback != nullptr, "Impostor selector must be a node callback");
    ViewProbe overhead(osg::Vec3(0, 0, 100)), horizontal(osg::Vec3(0, -100, 0));
    (*callback)(selector, &overhead); (*callback)(selector, &horizontal);
    require(overhead.visits == 1 && overhead.selected == "PereVia.Island.TopCard" &&
        horizontal.visits == 1 && horizontal.selected == "PereVia.Island.SideCard",
        "Per-camera selection must traverse exactly one appropriate card");
    require(selector->getChild(0)->getNodeMask() != 0 && selector->getChild(1)->getNodeMask() != 0,
        "View selector must not mutate shared node masks");
    for (unsigned view = 0; view < 2; ++view) {
        auto* geode = dynamic_cast<osg::Geode*>(selector->getChild(view));
        auto* mesh = geode->getDrawable(0)->asGeometry();
        require(triangles(*mesh) == 2, "Each impostor must have two triangles");
        auto* texture = dynamic_cast<osg::Texture2D*>(mesh->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        require(texture && texture->getImage() && texture->getImage()->s() == 64, "Baked texture missing");
        unsigned opaque = 0, transparent = 0;
        for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
            if (texture->getImage()->data(x, y)[3]) ++opaque; else ++transparent;
        }
        require(opaque > 100 && transparent > 100, "Impostor texture must have island pixels and transparent background");
    }
    options.lod1TriangleRatio = 0; reject(options);
    options.lod1TriangleRatio = 0.05; options.lod1MinPixels = options.lod0MinPixels; reject(options);
    options.lod1MinPixels = 60; options.impostorResolution = 4096; reject(options);
    options.impostorResolution = 64; options.impostorTopAngle = 90; reject(options);
    // 未启用 LOD 时不读取 LOD 专用参数。
    options.enableLOD = false;
    require(dynamic_cast<osg::Geode*>(Pyrive::OsgKit::IslandBuilder::build(options).get()) != nullptr,
        "Disabled LOD must still return Geode");
    options = Pyrive::OsgKit::IslandOptions{};
    options.contour = {{0, 0}, {10, 0}, {0, 10}};
    options.enableLOD = true; options.subdivisions = 0; options.impostorResolution = 32;
    auto sparseNode = Pyrive::OsgKit::IslandBuilder::build(options);
    auto* sparse = dynamic_cast<osg::LOD*>(sparseNode.get());
    require(sparse && triangles(*dynamic_cast<osg::Geode*>(sparse->getChild(1))->getDrawable(0)->asGeometry()) > 0,
        "Sparse source mesh must not collapse to an empty LOD1");
    std::cout << "Island LOD triangles: " << high << " -> " << low << " -> 2 (one selected card)\n";
}
}

void runIslandBuilderTests() {
    Pyrive::OsgKit::IslandOptions options;
    options.contour = {{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}};
    options.seaLevel = 3; options.height = 7; options.baseDepth = 2;
    options.subdivisions = 2;
    verifyMesh(options, 64); // L 形海湾不能被扇形三角化填平。
    std::reverse(options.contour.begin(), options.contour.end());
    options.contour.push_back(options.contour.front());
    verifyMesh(options, 64);
    options.contour = {{0, 0}, {5, 0}, {10, 0}, {10, 10}, {0, 10}};
    options.subdivisions = 0;
    verifyMesh(options, 100);
    options.contour = {{0, 0}, {10, 0}, {0, 10}};
    verifyMesh(options, 50);
    options.subdivisions = 4; options.slopeExponent = 2;
    verifyMesh(options, 50);
    auto valid = options;
    // 多重凹湾；不仅检查面积，还验证焊接后每条边的闭合与朝向。
    options.contour = {{0, 0}, {12, 0}, {12, 12}, {9, 12}, {9, 3},
        {7, 3}, {7, 10}, {5, 10}, {5, 3}, {3, 3}, {3, 12}, {0, 12}};
    options.subdivisions = 2;
    verifyMesh(options, 104);
    // 128 点凹星形覆盖 Earcut 的 >80 点空间索引路径。
    options.contour.clear();
    double starArea = 0;
    constexpr unsigned starPoints = 128;
    for (unsigned i = 0; i < starPoints; ++i) {
        const double angle = i * 6.283185307179586 / starPoints;
        const double radius = i % 2 == 0 ? 10.0 : 6.0;
        options.contour.emplace_back(radius * std::cos(angle), radius * std::sin(angle));
    }
    for (unsigned i = 0; i < starPoints; ++i) {
        const auto& a = options.contour[i]; const auto& b = options.contour[(i + 1) % starPoints];
        starArea += (a.x() * b.y() - a.y() * b.x()) * 0.5;
    }
    options.subdivisions = 1;
    verifyMesh(options, starArea);
    std::reverse(options.contour.begin(), options.contour.end());
    verifyMesh(options, starArea);
    // API 允许的最大轮廓点数。
    options.contour.clear();
    for (unsigned i = 0; i < 2048; ++i) {
        const double angle = i * 6.283185307179586 / 2048;
        options.contour.emplace_back(10 * std::cos(angle), 10 * std::sin(angle));
    }
    options.subdivisions = 0;
    verifyMesh(options, 0.5 * 2048 * 100 * std::sin(6.283185307179586 / 2048));
    options = valid;
    options.contour = {}; reject(options);
    options.contour = {{0, 0}, {1, 0}, {2, 0}}; reject(options);
    options.contour = {{0, 0}, {10, 10}, {0, 10}, {10, 0}}; reject(options);
    options.contour = {{0, 0}, {10, 0}, {10, 0}, {0, 10}}; reject(options);
    options.contour = {{0, 0}, {10, 0}, {5, 0}, {0, 10}}; reject(options);
    options = valid; options.contour[0].x() = std::numeric_limits<double>::quiet_NaN(); reject(options);
    options = valid; options.height = 0; reject(options);
    options = valid; options.baseDepth = -1; reject(options);
    options = valid; options.seaLevel = std::numeric_limits<double>::infinity(); reject(options);
    options = valid; options.subdivisions = 8; reject(options);
    options = valid; options.slopeExponent = 0; reject(options);
    options = valid; options.landColor.r() = 2; reject(options);
    options = valid; options.subdivisions = 7;
    // 超过 100 万三角形需要至少 23 个顶点。
    options.contour.clear();
    for (unsigned i = 0; i < 24; ++i) {
        const double angle = i * 6.283185307179586 / 24;
        options.contour.emplace_back(std::cos(angle), std::sin(angle));
    }
    reject(options);
    lodTests();
    holeTests();
    std::cout << "IslandBuilder tests passed\n";
}
