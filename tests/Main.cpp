#include <PereVia/OsgKit.h>
#include "IslandExample.h"
#include "TerrainExample.h"

#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osg/Image>
#include <osg/Geometry>
#include <osg/LOD>
#include <osgGA/GUIEventHandler>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>

void runRuntimeTests();
void runIslandBuilderTests();
void runTerrainBuilderTests();
void enableEffectCapture(osg::Camera* camera, osg::MatrixTransform* projectile);

namespace {
osg::ref_ptr<osg::Node> buildDemoTerrain() {
    const auto options = exampleTerrainOptions();
    const auto start = std::chrono::steady_clock::now();
    auto node = Pyrive::OsgKit::TerrainBuilder::build(options);
    const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto* lod = dynamic_cast<const osg::LOD*>(node.get());
    const auto* full = dynamic_cast<const osg::Geode*>(lod ? lod->getChild(0) : node.get());
    const auto* mesh = full->getDrawable(0)->asGeometry();
    const auto* indices = mesh->getPrimitiveSet(0);
    const size_t bytes = mesh->getVertexArray()->getTotalDataSize() + mesh->getNormalArray()->getTotalDataSize() +
        mesh->getColorArray()->getTotalDataSize() + mesh->getTexCoordArray(0)->getTotalDataSize() + indices->getNumIndices() * sizeof(unsigned);
    double low = 0, high = 0;
    node->getUserValue("PereVia.MinElevation", low); node->getUserValue("PereVia.MaxElevation", high);
    std::cout << std::fixed << std::setprecision(3) << "Terrain generation statistics (scene units)\n"
        << "  Samples: " << options.columns << " x " << options.rows << "; spacing: " << options.spacing.x() << ", " << options.spacing.y() << '\n'
        << "  Vertices: " << mesh->getVertexArray()->getNumElements() << "; triangles: " << indices->getNumIndices() / 3
        << "; indices: " << indices->getNumIndices() << '\n'
        << "  XY extent: " << (options.columns - 1) * options.spacing.x() << " x " << (options.rows - 1) * options.spacing.y() << '\n'
        << "  Elevation range: " << low << " .. " << high << '\n'
        << "  Mesh array payload: " << bytes << " bytes (" << bytes / 1048576.0 << " MiB)\n"
        << "  CPU build time (all levels; excludes sample generation): " << milliseconds << " ms\n";
    if (lod) {
        unsigned reduced = 0; double ratio = 0;
        lod->getUserValue("PereVia.LOD1Triangles", reduced); lod->getUserValue("PereVia.LOD1ActualRatio", ratio);
        std::cout << "  LOD triangles: " << indices->getNumIndices() / 3 << " -> " << reduced
            << " (" << ratio * 100 << " percent) -> 2 visible card triangles\n"
            << "  Two RGBA textures: " << options.impostorResolution << " x " << options.impostorResolution
            << "; " << 2ull * options.impostorResolution * options.impostorResolution * 4 << " bytes\n";
    }
    return node;
}

bool complexDemo = false;
Pyrive::OsgKit::IslandOptions demoIslandOptions() {
    if (complexDemo) return complexIslandOptions();
    Pyrive::OsgKit::IslandOptions options;
    options.contour = {{-60, -25}, {-30, -45}, {5, -38}, {40, -45},
        {65, -10}, {45, 20}, {15, 10}, {5, 40}, {-35, 45}, {-55, 15}};
    options.height = 25;
    options.subdivisions = 5;
    options.enableLOD = true;
    return options;
}

// 读取实际生成的数组与索引；时间仅包含 build，不含窗口初始化和渲染。
osg::ref_ptr<osg::Node> buildDemoIsland() {
    const auto options = demoIslandOptions();
    const auto start = std::chrono::steady_clock::now();
    auto island = Pyrive::OsgKit::IslandBuilder::build(options);
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    const auto* lod = dynamic_cast<const osg::LOD*>(island.get());
    const auto* full = dynamic_cast<const osg::Geode*>(lod ? lod->getChild(0) : island.get());
    const auto* mesh = full->getDrawable(0)->asGeometry();
    const auto* vertices = static_cast<const osg::Vec3dArray*>(mesh->getVertexArray());
    const auto* indices = static_cast<const osg::DrawElementsUInt*>(mesh->getPrimitiveSet(0));
    osg::Vec3d minimum = vertices->front(), maximum = minimum;
    for (const auto& p : *vertices) {
        for (unsigned j = 0; j < 3; ++j) {
            minimum[j] = std::min(minimum[j], p[j]); maximum[j] = std::max(maximum[j], p[j]);
        }
    }
    size_t topTriangles = 0, bottomTriangles = 0, wallTriangles = 0;
    double topArea = 0, bottomArea = 0, wallArea = 0, volume = 0;
    const auto origin = vertices->front();
    for (size_t i = 0; i < indices->size(); i += 3) {
        const auto& a = (*vertices)[(*indices)[i]];
        const auto& b = (*vertices)[(*indices)[i + 1]];
        const auto& c = (*vertices)[(*indices)[i + 2]];
        const auto normal = (b - a) ^ (c - a);
        const double area = normal.length() * 0.5;
        if (normal.z() > 0) { ++topTriangles; topArea += area; }
        else if (normal.z() < 0) { ++bottomTriangles; bottomArea += area; }
        else { ++wallTriangles; wallArea += area; }
        volume += (a - origin) * ((b - origin) ^ (c - origin)) / 6.0;
    }
    const size_t bytes = vertices->getTotalDataSize() + mesh->getNormalArray()->getTotalDataSize() +
        mesh->getColorArray()->getTotalDataSize() + indices->size() * sizeof(unsigned);
    std::cout << std::fixed << std::setprecision(3)
        << "Island generation statistics (scene units)\n"
        << "  Outer contour points: " << options.contour.size() << "; holes: " << options.holes.size()
        << "; subdivisions: " << options.subdivisions << '\n'
        << "  Sea level: " << options.seaLevel << "; height: " << options.height
        << "; base depth: " << options.baseDepth << "; slope exponent: " << options.slopeExponent << '\n'
        << "  LOD0 geometry statistics; primitive sets: " << mesh->getNumPrimitiveSets() << '\n'
        << "  Stored vertices: " << vertices->size() << "; indices: " << indices->size() << '\n'
        << "  Triangles - surface: " << topTriangles << "; bottom: " << bottomTriangles
        << "; walls: " << wallTriangles << "; total: " << indices->size() / 3 << '\n'
        << "  Bounds: [" << minimum.x() << ", " << minimum.y() << ", " << minimum.z()
        << "] .. [" << maximum.x() << ", " << maximum.y() << ", " << maximum.z() << "]\n"
        << "  Area - surface: " << topArea << "; bottom/footprint: " << bottomArea
        << "; walls: " << wallArea << "; total: " << topArea + bottomArea + wallArea << '\n'
        << "  Enclosed volume: " << std::abs(volume) << '\n'
        << "  Mesh array payload: " << bytes << " bytes (" << bytes / 1048576.0 << " MiB)\n"
        << "  CPU build time: " << milliseconds << " ms\n";
    if (lod) {
        unsigned lowFaces = 0; double ratio = 0;
        lod->getUserValue("PereVia.LOD1Triangles", lowFaces);
        lod->getUserValue("PereVia.LOD1ActualRatio", ratio);
        std::cout << "  LOD1 triangles: " << lowFaces << " (" << ratio * 100 << " percent); LOD2: 2 visible triangles\n"
            << "  Two RGBA textures: " << options.impostorResolution << " x " << options.impostorResolution
            << "; " << 2ull * options.impostorResolution * options.impostorResolution * 4 << " bytes\n";
    }
    for (size_t i = 0; i < options.holes.size(); ++i)
        std::cout << "  Hole " << i + 1 << " points: " << options.holes[i].size() << '\n';
    return island;
}

void forceLevel(osg::LOD& lod, int level, const osg::LOD::RangeList& automatic = {}) {
    if (level < 0) { lod.setRangeList(automatic); return; }
    for (unsigned i = 0; i < 3; ++i) {
        lod.setRange(i, static_cast<int>(i) == level ? 0.0f : 1.0f,
            static_cast<int>(i) == level ? std::numeric_limits<float>::max() : 1.0f);
    }
}

class LODKeys final : public osgGA::GUIEventHandler {
public:
    explicit LODKeys(osg::LOD* lod) : lod_(lod), automatic_(lod->getRangeList()) {}
    bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter&) override {
        if (event.getEventType() != osgGA::GUIEventAdapter::KEYDOWN || event.getKey() < '0' || event.getKey() > '3') return false;
        const int level = event.getKey() - '1';
        forceLevel(*lod_, level, automatic_);
        std::cout << "LOD: " << (level < 0 ? "automatic" : std::to_string(level)) << '\n';
        return true;
    }
private:
    osg::ref_ptr<osg::LOD> lod_;
    osg::LOD::RangeList automatic_;
};

class IslandCapture final : public osg::Camera::DrawCallback {
public:
    explicit IslandCapture(std::string path = "build/island.ppm") : path_(std::move(path)) {}
    void operator()(osg::RenderInfo& info) const override {
        if (captured_) return;
        const auto* viewport = info.getCurrentCamera()->getViewport();
        const int width = static_cast<int>(viewport->width());
        const int height = static_cast<int>(viewport->height());
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->readPixels(static_cast<int>(viewport->x()), static_cast<int>(viewport->y()),
            width, height, GL_RGB, GL_UNSIGNED_BYTE);
        std::filesystem::create_directories("build");
        std::ofstream file(path_, std::ios::binary);
        file << "P6\n" << width << ' ' << height << "\n255\n";
        for (int row = height - 1; row >= 0; --row)
            file.write(reinterpret_cast<const char*>(image->data(0, row)), width * 3);
        if (!file) throw std::runtime_error("Could not write island capture");
        captured_ = true;
    }
private:
    std::string path_;
    mutable bool captured_ = false;
};
}

int main(int argc, char** argv) {
    try {
        complexDemo = argc == 2 && std::string(argv[1]).find("--island-complex") == 0;
        if (argc == 2 && std::string(argv[1]) == "--tests") {
            runRuntimeTests();
            runIslandBuilderTests();
            runTerrainBuilderTests();
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--terrain-stats") {
            buildDemoTerrain(); return 0;
        }
        if (argc == 2 && (std::string(argv[1]) == "--island-stats" || std::string(argv[1]) == "--island-complex-stats")) {
            buildDemoIsland();
            return 0;
        }
        const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
        const bool terrainSmoke = argc == 2 && std::string(argv[1]) == "--terrain-smoke";
        const bool terrainLodSmoke = argc == 2 && std::string(argv[1]) == "--terrain-lod-smoke";
        const bool terrainDemo = terrainSmoke || terrainLodSmoke || (argc == 2 && std::string(argv[1]) == "--terrain");
        const bool islandSmoke = argc == 2 && (std::string(argv[1]) == "--island-smoke" || std::string(argv[1]) == "--island-complex-smoke");
        const bool lodSmoke = argc == 2 && (std::string(argv[1]) == "--island-lod-smoke" || std::string(argv[1]) == "--island-complex-lod-smoke");
        const bool islandDemo = islandSmoke || lodSmoke || (argc == 2 &&
            (std::string(argv[1]) == "--island" || std::string(argv[1]) == "--island-complex"));
        if (argc != 1 && !smoke && !islandDemo && !terrainDemo) {
            std::cerr << "Usage: PereViaOsgTests [--tests | --smoke | --terrain[-stats|-smoke|-lod-smoke] | --island[-complex][-stats|-smoke|-lod-smoke]]\n";
            return 2;
        }

        // 第一步：宿主构建一个 OSG 视景，获得 viewer、camera、root。
        osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->setName("HostRoot");
        viewer->setThreadingModel(osgViewer::ViewerBase::SingleThreaded);
        viewer->setUpViewInWindow(100, 100, 1000, 700);
        viewer->setSceneData(root.get());
        osg::Camera* camera = viewer->getCamera();
        camera->setClearColor(osg::Vec4(0.12f, 0.16f, 0.21f, 1.0f));
        if (complexDemo) camera->setClearColor(osg::Vec4(0.07f, 0.28f, 0.39f, 1));

        if (terrainDemo) {
            auto terrain = buildDemoTerrain(); root->addChild(terrain.get());
            auto* lod = dynamic_cast<osg::LOD*>(terrain.get());
            if (lod) viewer->addEventHandler(new LODKeys(lod));
            std::cout << "Keys: 0 automatic LOD, 1 full mesh, 2 simplified mesh, 3 impostor\n";
            osg::ref_ptr<osgGA::TrackballManipulator> orbit = new osgGA::TrackballManipulator;
            orbit->setVerticalAxisFixed(true); orbit->setAllowThrow(false);
            orbit->setHomePosition(osg::Vec3d(650, -800, 620), osg::Vec3d(0, 0, 50), osg::Vec3d(0, 0, 1));
            viewer->setCameraManipulator(orbit.get()); viewer->home();
            if (terrainLodSmoke && lod) {
                viewer->realize();
                if (!viewer->isRealized()) throw std::runtime_error("Terrain LOD viewer could not open graphics context");
                for (int level = 0; level < 4; ++level) {
                    forceLevel(*lod, std::min(level, 2));
                    orbit->setHomePosition(level == 3 ? osg::Vec3d(0, -950, 160) : osg::Vec3d(650, -800, 1400),
                        osg::Vec3d(0, 0, 60), osg::Vec3d(0, 0, 1));
                    viewer->home();
                    camera->setPostDrawCallback(new IslandCapture("build/terrain-lod" + std::to_string(level) + ".ppm"));
                    for (unsigned frame = 0; frame < 3 && !viewer->done(); ++frame) viewer->frame();
                }
                return 0;
            }
            if (terrainSmoke) {
                camera->setPostDrawCallback(new IslandCapture("build/terrain.ppm"));
                viewer->realize();
                if (!viewer->isRealized()) throw std::runtime_error("Terrain viewer could not open graphics context");
                for (unsigned frame = 0; frame < 3 && !viewer->done(); ++frame) viewer->frame();
                return 0;
            }
            return viewer->run();
        }

        if (islandDemo) {
            auto island = buildDemoIsland();
            auto* lod = dynamic_cast<osg::LOD*>(island.get());
            if (lod) viewer->addEventHandler(new LODKeys(lod));
            std::cout << "Keys: 0 automatic LOD, 1 full mesh, 2 simplified mesh, 3 impostor\n";
            root->addChild(island.get());
            // 水面属于宿主示例；库仍只返回岛屿模型。
            if (complexDemo) {
                osg::ref_ptr<osg::Geometry> water = new osg::Geometry;
                osg::ref_ptr<osg::Vec3Array> waterVertices = new osg::Vec3Array;
                for (const auto& p : {osg::Vec3(-420, -350, 0), osg::Vec3(420, -350, 0),
                    osg::Vec3(420, 350, 0), osg::Vec3(-420, 350, 0)}) waterVertices->push_back(p);
                water->setVertexArray(waterVertices.get());
                osg::ref_ptr<osg::Vec4Array> waterColors = new osg::Vec4Array;
                waterColors->push_back(osg::Vec4(0.07f, 0.28f, 0.39f, 1));
                water->setColorArray(waterColors.get(), osg::Array::BIND_OVERALL);
                osg::ref_ptr<osg::DrawElementsUByte> waterIndices = new osg::DrawElementsUByte(GL_TRIANGLES);
                for (unsigned char i : {0, 1, 2, 0, 2, 3}) waterIndices->push_back(i);
                water->addPrimitiveSet(waterIndices.get());
                water->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
                osg::ref_ptr<osg::Geode> sea = new osg::Geode;
                sea->setName("HostDemoSea"); sea->addDrawable(water.get()); root->addChild(sea.get());
            }
            osg::ref_ptr<osgGA::TrackballManipulator> orbit = new osgGA::TrackballManipulator;
            orbit->setVerticalAxisFixed(true);
            orbit->setHomePosition(complexDemo ? osg::Vec3d(520, -680, 720) : osg::Vec3d(140, -180, 130),
                osg::Vec3d(0, 0, 0), osg::Vec3d(0, 0, 1));
            viewer->setCameraManipulator(orbit.get());
            viewer->home();
            if (lodSmoke && lod) {
                viewer->realize();
                if (!viewer->isRealized()) throw std::runtime_error("LOD viewer could not open graphics context");
                for (int level = 0; level < 4; ++level) {
                    forceLevel(*lod, std::min(level, 2));
                    const double scale = complexDemo ? 3.5 : 1.0;
                    orbit->setHomePosition((level == 3 ? osg::Vec3d(0, -240, 30) : osg::Vec3d(140, -180, 330)) * scale,
                        osg::Vec3d(0, 0, 10), osg::Vec3d(0, 0, 1));
                    viewer->home();
                    camera->setPostDrawCallback(new IslandCapture(std::string(complexDemo ? "build/island-complex-lod" : "build/island-lod") + std::to_string(level) + ".ppm"));
                    for (unsigned frame = 0; frame < 3 && !viewer->done(); ++frame) viewer->frame();
                }
                return 0;
            }
            if (islandSmoke) {
                camera->setPostDrawCallback(new IslandCapture(complexDemo ? "build/island-complex.ppm" : "build/island.ppm"));
                viewer->realize();
                if (!viewer->isRealized()) throw std::runtime_error("Island viewer could not open a graphics context");
                for (unsigned frame = 0; frame < 3 && !viewer->done(); ++frame) viewer->frame();
                return 0;
            }
            return viewer->run();
        }

        // 这个立方体属于宿主场景，仅方便观察基础窗口。
        osg::ref_ptr<osg::Geode> hostObject = new osg::Geode;
        hostObject->setName("HostCube");
        hostObject->addDrawable(new osg::ShapeDrawable(new osg::Box(osg::Vec3(0, 0, 1), 2)));
        osg::ref_ptr<osg::Group> ship = new osg::Group;
        ship->setName("ShipCollisionTarget");
        ship->addChild(hostObject.get());
        root->addChild(ship.get());

        osg::ref_ptr<osgGA::TrackballManipulator> manipulator = new osgGA::TrackballManipulator;
        manipulator->setVerticalAxisFixed(true);
        manipulator->setAllowThrow(false);
        manipulator->setHomePosition(osg::Vec3d(8, -12, 8), osg::Vec3d(0, 0, 1), osg::Vec3d(0, 0, 1));
        viewer->setCameraManipulator(manipulator.get());
        viewer->home();

        // 第二步：构造并保存库实例，传入 viewer、camera、root。
        // 参数顺序固定，实例需要一直活到使用结束。
        {
            Pyrive::OsgKit::Runtime runtime(viewer.get(), camera, root.get());

            // 通过参数结构体创建一个匀速直线运动的立方体，库会自动挂载它。
            //Pyrive::OsgKit::URMCubeOptions options;
            //options.position = osg::Vec3d(-4, 0, 1);
            //options.direction = osg::Vec3d(1, 0, 0);
            //options.size = osg::Vec3(0.8f, 0.8f, 0.8f);
            //options.color = osg::Vec4(1.0f, 0.5f, 0.1f, 1.0f);
            //options.speed = 0.5; // 每秒移动 0.5 个场景单位。
            //osg::MatrixTransform* cube = runtime.createURMCube(options);
            //cube->setName("MyMovingCube"); // 也可以作为 osg::Node* 使用。

            // 发射物只检查指定船只，命中后产生闪光、火球、火花和烟雾。
            Pyrive::OsgKit::ProjectileOptions shot;
            shot.position.set(0, 0, 8);
            shot.direction.set(0, 0, -1);
            shot.speed = 1.0;
            shot.gravityEnabled = true;
            shot.gravity.set(0, 0, -9.81);
            shot.collisionTargets = {ship.get()}; // 可以继续添加其他确定的目标 Group。
            shot.sparks.count = 100;
            shot.sparks.speed = 5.0;
            shot.sparks.duration = 1.5;
            shot.explosion.radius = 2.0f;
            shot.explosion.duration = 0.65;
            shot.smoke.radius = 1.2f;
            shot.smoke.duration = 3.5;
            shot.smoke.riseSpeed = 1.2;
            auto projectile = Pyrive::OsgKit::createProjectile(runtime, shot); // 明确指定归属的 Runtime。
            if (smoke) enableEffectCapture(camera, projectile.get()); // 保存三个阶段的实际渲染截图。

            // 第三步：库已自动在 root 下面挂载自己的 PereViaRoot。
            // 这里无需手动 addChild，也无需额外的框架更新调用。
            std::cout << "Attached " << runtime.root()->getName() << " to " << root->getName() << '\n';
            const auto start = std::chrono::steady_clock::now();
            while (!viewer->done()) {
                viewer->frame();
                if (smoke && std::chrono::steady_clock::now() - start >= std::chrono::seconds(6))
                    break;
            }
            //const osg::Vec3d position = cube->getMatrix().getTrans();
            //std::cout << "Moving cube position: " << position.x() << ", "
            //    << position.y() << ", " << position.z() << '\n';
            const osg::Vec3d impact = projectile->getMatrix().getTrans();
            std::cout << "Projectile final position: " << impact.x() << ", " << impact.y() << ", "
                << impact.z() << "; parents remaining: " << projectile->getNumParents() << '\n';
        } // runtime 析构：摘除 PereViaRoot，宿主立方体继续保留。
        std::cout << "Runtime unloaded; host children remaining: " << root->getNumChildren() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
