#include <PereVia/OsgKit.h>
#include "TerrainExample.h"
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/NodeCallback>
#include <osg/Texture2D>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void reject(const PereVia::OsgKit::TerrainOptions& options) {
    bool rejected = false;
    try { PereVia::OsgKit::TerrainBuilder::build(options); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Invalid terrain input was accepted");
}

size_t validateMesh(const osg::Geometry& mesh) {
    const auto* vertices = dynamic_cast<const osg::Vec3dArray*>(mesh.getVertexArray());
    const auto* normals = dynamic_cast<const osg::Vec3Array*>(mesh.getNormalArray());
    const auto* colors = dynamic_cast<const osg::Vec4Array*>(mesh.getColorArray());
    const auto* uv = dynamic_cast<const osg::Vec2Array*>(mesh.getTexCoordArray(0));
    require(vertices && normals && colors && uv && vertices->size() == normals->size() &&
        vertices->size() == colors->size() && vertices->size() == uv->size(), "LOD terrain arrays are inconsistent");
    for (size_t i = 0; i < vertices->size(); ++i) {
        const auto& p = (*vertices)[i];
        require(std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z()), "LOD terrain vertex is non-finite");
        require(std::abs((*normals)[i].length() - 1) < 1e-5, "LOD terrain normal must have unit length");
        require((*uv)[i].x() >= -1e-5 && (*uv)[i].x() <= 1.00001 &&
            (*uv)[i].y() >= -1e-5 && (*uv)[i].y() <= 1.00001, "LOD terrain UV is outside the source map");
    }
    size_t count = 0;
    for (unsigned i = 0; i < mesh.getNumPrimitiveSets(); ++i) {
        const auto* primitive = mesh.getPrimitiveSet(i);
        require(primitive->getMode() == GL_TRIANGLES && primitive->getNumIndices() % 3 == 0, "LOD terrain must use triangles");
        for (unsigned j = 0; j < primitive->getNumIndices(); ++j)
            require(primitive->index(j) < vertices->size(), "LOD terrain index out of range");
        count += primitive->getNumIndices() / 3;
    }
    require(count > 0, "LOD terrain mesh must not be empty");
    return count;
}

class ViewProbe final : public osg::NodeVisitor {
public:
    explicit ViewProbe(const osg::Vec3& eye) : osg::NodeVisitor(CULL_VISITOR, TRAVERSE_ACTIVE_CHILDREN), eye_(eye) {}
    osg::Vec3 getEyePoint() const override { return eye_; }
    void apply(osg::Geode& node) override { selected = node.getName(); ++visits; }
    std::string selected;
    unsigned visits = 0;
private:
    osg::Vec3 eye_;
};

void terrainLODTests(const osg::Vec3dArray& originalVertices) {
    auto options = exampleTerrainOptions();
    options.enableLOD = true; options.impostorResolution = 64;
    auto node = PereVia::OsgKit::TerrainBuilder::build(options);
    auto* lod = dynamic_cast<osg::LOD*>(node.get());
    require(lod && lod->getNumChildren() == 3 && lod->getNumParents() == 0, "Terrain LOD must contain three standalone levels");
    require(lod->getRangeMode() == osg::LOD::PIXEL_SIZE_ON_SCREEN && lod->getMinRange(0) == 300 &&
        lod->getMinRange(1) == 60 && lod->getMaxRange(1) == lod->getMinRange(0) &&
        lod->getMinRange(2) == 0 && lod->getMaxRange(2) == lod->getMinRange(1), "Terrain LOD pixel ranges are incorrect");
    const auto* highMesh = dynamic_cast<osg::Geode*>(lod->getChild(0))->getDrawable(0)->asGeometry();
    const auto* lowMesh = dynamic_cast<osg::Geode*>(lod->getChild(1))->getDrawable(0)->asGeometry();
    const size_t high = validateMesh(*highMesh), low = validateMesh(*lowMesh);
    require(high == 498002 && low > high * 0.04 && low < high * 0.06, "Terrain LOD1 must approach five percent faces");
    require(*static_cast<const osg::Vec3dArray*>(highMesh->getVertexArray()) == originalVertices,
        "Enabling LOD must not modify original terrain samples");
    auto* selector = lod->getChild(2)->asGroup()->getChild(0)->asGroup();
    require(selector->getNumChildren() == 2, "Terrain impostor must contain two view cards");
    auto* callback = dynamic_cast<osg::NodeCallback*>(selector->getCullCallback());
    require(callback, "Terrain view-selection callback missing");
    ViewProbe overhead(osg::Vec3(0, 0, 100)), horizontal(osg::Vec3(0, -100, 0));
    (*callback)(selector, &overhead); (*callback)(selector, &horizontal);
    require(overhead.visits == 1 && overhead.selected == "PereVia.Terrain.TopCard" &&
        horizontal.visits == 1 && horizontal.selected == "PereVia.Terrain.SideCard",
        "Terrain must select exactly one card for each camera");
    for (unsigned view = 0; view < 2; ++view) {
        auto* mesh = dynamic_cast<osg::Geode*>(selector->getChild(view))->getDrawable(0)->asGeometry();
        require(mesh->getPrimitiveSet(0)->getNumIndices() == 6, "Terrain card must have two triangles");
        auto* texture = dynamic_cast<osg::Texture2D*>(mesh->getStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
        require(texture && texture->getImage() && texture->getImage()->s() == 64, "Terrain card texture missing");
        unsigned opaque = 0, transparent = 0;
        for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x)
            if (texture->getImage()->data(x, y)[3]) ++opaque; else ++transparent;
        require(opaque > 100 && transparent > 100, "Terrain card must contain rendered terrain and transparent background");
    }
    unsigned reportedHigh = 0, reportedLow = 0;
    lod->getUserValue("PereVia.LOD0Triangles", reportedHigh); lod->getUserValue("PereVia.LOD1Triangles", reportedLow);
    require(reportedHigh == high && reportedLow == low, "Terrain LOD statistics must report actual mesh sizes");

    // 最小网格、全平地形、长条形和大原点：都应可创建三级节点而不产生 NaN/空粗网格。
    PereVia::OsgKit::TerrainOptions small;
    small.columns = 2; small.rows = 2; small.elevations = {-3, -3, -3, -3}; small.enableLOD = true;
    small.impostorResolution = 32;
    auto flat = PereVia::OsgKit::TerrainBuilder::build(small);
    auto* flatLod = dynamic_cast<osg::LOD*>(flat.get());
    require(flatLod && validateMesh(*dynamic_cast<osg::Geode*>(flatLod->getChild(1))->getDrawable(0)->asGeometry()) == 2,
        "Flat minimum grid must retain a valid LOD1");
    auto* flatSelector = flatLod->getChild(2)->asGroup()->getChild(0)->asGroup();
    ViewProbe flatSide(osg::Vec3(0, -100, 0));
    (*dynamic_cast<osg::NodeCallback*>(flatSelector->getCullCallback()))(flatSelector, &flatSide);
    require(flatSide.selected == "PereVia.Terrain.TopCard", "Flat terrain must avoid its empty side projection");
    for (bool transpose : {false, true}) {
        small.columns = transpose ? 20 : 2; small.rows = transpose ? 2 : 20;
        small.elevations.resize(size_t(small.columns) * small.rows);
        for (size_t i = 0; i < small.elevations.size(); ++i) small.elevations[i] = std::sin(double(i) * 0.2);
        small.origin.set(10000000, -20000000, -100);
        auto thin = PereVia::OsgKit::TerrainBuilder::build(small);
        auto* thinLod = dynamic_cast<osg::LOD*>(thin.get());
        require(thinLod && validateMesh(*dynamic_cast<osg::Geode*>(thinLod->getChild(1))->getDrawable(0)->asGeometry()) > 0,
            "Narrow terrain must retain a valid coarse mesh");
    }
    auto invalid = small;
    invalid.lod1TriangleRatio = 0; reject(invalid);
    invalid = small; invalid.lod1TriangleRatio = 1; reject(invalid);
    invalid = small; invalid.lod1TriangleRatio = std::numeric_limits<double>::quiet_NaN(); reject(invalid);
    invalid = small; invalid.lod1MinPixels = invalid.lod0MinPixels; reject(invalid);
    invalid = small; invalid.lod1MinPixels = 0; reject(invalid);
    invalid = small; invalid.impostorResolution = 4; reject(invalid);
    invalid = small; invalid.impostorTopAngle = 90; reject(invalid);
    invalid.enableLOD = false;
    require(dynamic_cast<osg::Geode*>(PereVia::OsgKit::TerrainBuilder::build(invalid).get()) != nullptr,
        "Disabled terrain LOD must ignore its unused parameters");
    std::cout << "Terrain LOD tests passed: " << high << " -> " << low << " -> 2 visible triangles\n";
}
}

void runTerrainBuilderTests() {
    PereVia::OsgKit::TerrainOptions options;
    options.columns = 3; options.rows = 2;
    options.elevations = {-5, -3, -1, -2, 0, 2};
    options.origin.set(10, 20, 100); options.spacing.set(2, 3); options.elevationScale = 2;
    auto node = PereVia::OsgKit::TerrainBuilder::build(options);
    auto* geode = dynamic_cast<osg::Geode*>(node.get());
    require(geode && geode->getNumParents() == 0 && geode->getNumDrawables() == 1, "Terrain must be standalone Geode");
    auto* mesh = geode->getDrawable(0)->asGeometry();
    auto* vertices = dynamic_cast<osg::Vec3dArray*>(mesh->getVertexArray());
    auto* normals = dynamic_cast<osg::Vec3Array*>(mesh->getNormalArray());
    auto* colors = dynamic_cast<osg::Vec4Array*>(mesh->getColorArray());
    auto* uv = dynamic_cast<osg::Vec2Array*>(mesh->getTexCoordArray(0));
    auto* indices = dynamic_cast<osg::DrawElementsUInt*>(mesh->getPrimitiveSet(0));
    require(vertices && normals && colors && uv && indices && vertices->size() == 6 && indices->size() == 12,
        "Terrain arrays or counts are incorrect");
    require((*vertices)[0] == osg::Vec3d(10, 20, 90) && (*vertices)[5] == osg::Vec3d(14, 23, 104),
        "Terrain origin, spacing or elevation scale is incorrect");
    const unsigned expected[] = {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4};
    for (unsigned i = 0; i < 12; ++i) require((*indices)[i] == expected[i], "Terrain grid winding or row order is incorrect");
    osg::Vec3 expectedNormal(-2, -2, 1); expectedNormal.normalize();
    for (const auto& normal : *normals)
        require((normal - expectedNormal).length() < 1e-5, "Plane normals must match analytical slope including spacing/scale");
    require((*uv)[0] == osg::Vec2(0, 0) && (*uv)[5] == osg::Vec2(1, 1), "Terrain UV endpoints are incorrect");
    require((*colors)[0] == options.lowColor && (*colors)[5] == options.highColor, "Terrain elevation colors are incorrect");
    const auto valid = options;
    options.elevations.assign(6, -12);
    auto flat = PereVia::OsgKit::TerrainBuilder::build(options);
    auto* flatMesh = dynamic_cast<osg::Geode*>(flat.get())->getDrawable(0)->asGeometry();
    for (const auto& normal : *static_cast<osg::Vec3Array*>(flatMesh->getNormalArray()))
        require((normal - osg::Vec3(0, 0, 1)).length() < 1e-6, "Flat terrain normals must point up");
    require((*static_cast<osg::Vec4Array*>(flatMesh->getColorArray()))[0] == options.lowColor,
        "Flat terrain color must not divide by zero");
    options = valid; options.columns = 1; reject(options);
    options = valid; options.rows = 0; reject(options);
    options = valid; options.elevations.pop_back(); reject(options);
    options = valid; options.elevations.push_back(0); reject(options);
    options = valid; options.spacing.x() = 0; reject(options);
    options = valid; options.spacing.y() = -1; reject(options);
    options = valid; options.elevationScale = 0; reject(options);
    options = valid; options.elevations[2] = std::numeric_limits<double>::quiet_NaN(); reject(options);
    options = valid; options.elevations[2] = std::numeric_limits<double>::infinity(); reject(options);
    options = valid; options.origin.x() = std::numeric_limits<double>::infinity(); reject(options);
    options = valid; options.lowColor.r() = 2; reject(options);
    options = valid; options.columns = std::numeric_limits<unsigned>::max(); options.rows = options.columns; reject(options);
    options = valid; options.spacing.x() = std::numeric_limits<double>::max(); reject(options);
    options = valid; options.origin.x() = 1e30; reject(options); // +2 无法形成可分辨的横向采样点。
    options = valid; options.elevations[0] = std::numeric_limits<double>::max(); reject(options);

    // 实际的 500×500 数据逐点验证，不只检查包围盒或抽样几个位置。
    auto largeOptions = exampleTerrainOptions();
    largeOptions.enableLOD = false;
    auto large = PereVia::OsgKit::TerrainBuilder::build(largeOptions);
    auto* largeMesh = dynamic_cast<osg::Geode*>(large.get())->getDrawable(0)->asGeometry();
    const auto& largeVertices = *static_cast<osg::Vec3dArray*>(largeMesh->getVertexArray());
    const auto& largeNormals = *static_cast<osg::Vec3Array*>(largeMesh->getNormalArray());
    const auto& largeIndices = *static_cast<osg::DrawElementsUInt*>(largeMesh->getPrimitiveSet(0));
    require(largeVertices.size() == 250000 && largeIndices.size() == 1494006, "500x500 terrain size mismatch");
    for (unsigned r = 0; r < 500; ++r) for (unsigned c = 0; c < 500; ++c) {
        const size_t i = size_t(r) * 500 + c;
        require(largeVertices[i] == osg::Vec3d(-249.5 + c, -249.5 + r, largeOptions.elevations[i]),
            "Terrain changed an input elevation or transposed rows/columns");
        require(std::isfinite(largeNormals[i].x()) && std::abs(largeNormals[i].length() - 1) < 1e-5 && largeNormals[i].z() > 0,
            "Terrain normal is not finite, normalized and upward");
    }
    for (size_t i = 0; i < largeIndices.size(); i += 3) {
        const unsigned a = largeIndices[i], b = largeIndices[i + 1], c = largeIndices[i + 2];
        require(a < 250000 && b < 250000 && c < 250000, "Terrain index out of range");
        const auto normal = (largeVertices[b] - largeVertices[a]) ^ (largeVertices[c] - largeVertices[a]);
        require(normal.z() > 0, "Terrain triangle must be non-degenerate and face upward");
    }
    terrainLODTests(largeVertices);
    std::cout << "TerrainBuilder tests passed: 500x500 samples, 250000 vertices, 498002 triangles\n";
}
