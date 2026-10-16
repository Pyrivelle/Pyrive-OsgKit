#include <PereVia/OsgKit.h>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osgViewer/Viewer>
#include <iostream>
#include <stdexcept>

void runProjectileTests();

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void cubeMotion() {
    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    osg::ref_ptr<osg::Group> root = new osg::Group;
    viewer->setSceneData(root.get());
    PereVia::OsgKit::Runtime runtime(viewer.get(), viewer->getCamera(), root.get());
    PereVia::OsgKit::URMCubeOptions options;
    options.position.set(1, 2, 3);
    options.direction.set(0, 5, 0); // 非单位向量，移动速度仍然应为 2。
    options.speed = 2.0;
    osg::MatrixTransform* cube = runtime.createURMCube(options);
    require(runtime.root()->containsNode(cube), "Cube was not automatically attached");
    require((cube->getMatrix().getTrans() - options.position).length() < 1e-9,
        "Cube was not created at its starting position");

    // 指定仿真时间、执行原生更新遍历；无窗口、无需等待实际时间。
    viewer->advance(10.0);
    viewer->updateTraversal();
    require((cube->getMatrix().getTrans() - options.position).length() < 1e-9,
        "First update should establish the motion start time");
    viewer->advance(12.0);
    viewer->updateTraversal();
    const osg::Vec3d expected(1, 6, 3);
    require((cube->getMatrix().getTrans() - expected).length() < 1e-9,
        "Cube did not follow direction and speed using the host simulation time");
    viewer->updateTraversal();
    require((cube->getMatrix().getTrans() - expected).length() < 1e-9,
        "Repeated traversal moved the cube twice");

    const osg::Vec3d facing = osg::Vec3d(1, 0, 0) * cube->getMatrix() - expected;
    require((facing - osg::Vec3d(0, 1, 0)).length() < 1e-9, "Cube heading did not match direction");
    options.speed = 0;
    osg::MatrixTransform* stationary = runtime.createURMCube(options);
    viewer->advance(13.0);
    viewer->updateTraversal();
    viewer->advance(14.0);
    viewer->updateTraversal();
    require((stationary->getMatrix().getTrans() - options.position).length() < 1e-9,
        "A zero speed cube moved");

    const unsigned children = runtime.root()->getNumChildren();
    options.direction.set(0, 0, 0);
    bool rejected = false;
    try { runtime.createURMCube(options); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && runtime.root()->getNumChildren() == children,
        "Invalid cube options changed the scene");
}
}

// 验证载入、挂载、生命周期和立方体运动。不创建窗口。
void runRuntimeTests() {
    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    osg::ref_ptr<osg::Group> hostRoot = new osg::Group;
    osg::ref_ptr<osg::Group> hostContent = new osg::Group;
    hostRoot->addChild(hostContent.get());
    viewer->setSceneData(hostRoot.get());
    const int hostReferences = hostRoot->referenceCount();

    osg::ref_ptr<osg::Group> retainedPluginRoot;
    {
        PereVia::OsgKit::Runtime runtime(viewer.get(), viewer->getCamera(), hostRoot.get());
        retainedPluginRoot = runtime.root();
        require(hostRoot->getNumChildren() == 2, "PereViaRoot was not attached");
        require(hostRoot->containsNode(runtime.root()), "Wrong attachment point");
        require(runtime.root()->getName() == "PereViaRoot", "Wrong plugin root name");
        require(runtime.root()->getNumChildren() == 0, "Runtime created unexpected features");
        require(viewer->getSceneData() == hostRoot.get(), "Runtime replaced the host scene");
        require(hostRoot->referenceCount() == hostReferences, "Runtime owns the host root");
        {
            PereVia::OsgKit::Runtime another(viewer.get(), viewer->getCamera(), hostRoot.get());
            require(another.root() != runtime.root(), "Instances share an output root");
            require(hostRoot->getNumChildren() == 3, "Second instance was not attached");
        }
        require(hostRoot->containsNode(runtime.root()), "Another instance removed our root");
    }
    require(hostRoot->getNumChildren() == 1 && hostRoot->containsNode(hostContent.get()),
        "Destruction changed the host content");
    require(retainedPluginRoot->getNumParents() == 0, "Destruction did not detach PereViaRoot");

    for (int missing = 0; missing < 3; ++missing) {
        bool rejected = false;
        try {
            PereVia::OsgKit::Runtime invalid(missing == 0 ? nullptr : viewer.get(),
                missing == 1 ? nullptr : viewer->getCamera(), missing == 2 ? nullptr : hostRoot.get());
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && hostRoot->getNumChildren() == 1, "Invalid input changed the scene");
    }

    // 宿主先释放独立挂接点时，Runtime 析构也不能访问失效的 root。
    osg::ref_ptr<osg::Group> temporaryRoot = new osg::Group;
    {
        PereVia::OsgKit::Runtime runtime(viewer.get(), viewer->getCamera(), temporaryRoot.get());
        temporaryRoot = nullptr;
    }
    cubeMotion();
    runProjectileTests();
    std::cout << "Runtime, collision, sparks, explosion and smoke tests passed.\n";
}
