#include <PereVia/OsgKit.h>

#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <chrono>
#include <exception>
#include <iostream>
#include <string>

void runRuntimeTests();
void enableEffectCapture(osg::Camera* camera, osg::MatrixTransform* projectile);

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--tests") {
            runRuntimeTests();
            return 0;
        }
        const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
        if (argc != 1 && !smoke) {
            std::cerr << "Usage: PereViaOsgTests [--tests | --smoke]\n";
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
            PereVia::OsgKit::Runtime runtime(viewer.get(), camera, root.get());

            // 通过参数结构体创建一个匀速直线运动的立方体，库会自动挂载它。
            //PereVia::OsgKit::URMCubeOptions options;
            //options.position = osg::Vec3d(-4, 0, 1);
            //options.direction = osg::Vec3d(1, 0, 0);
            //options.size = osg::Vec3(0.8f, 0.8f, 0.8f);
            //options.color = osg::Vec4(1.0f, 0.5f, 0.1f, 1.0f);
            //options.speed = 0.5; // 每秒移动 0.5 个场景单位。
            //osg::MatrixTransform* cube = runtime.createURMCube(options);
            //cube->setName("MyMovingCube"); // 也可以作为 osg::Node* 使用。

            // 发射物只检查指定船只，命中后产生闪光、火球、火花和烟雾。
            PereVia::OsgKit::ProjectileOptions shot;
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
            auto projectile = PereVia::OsgKit::createProjectile(runtime, shot); // 明确指定归属的 Runtime。
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
