#include <PereVia/OsgKit.h>

#include <osg/Camera>
#include <osg/Group>
#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/ShapeDrawable>
#include <osg/StateSet>
#include <osg/observer_ptr>
#include <osgViewer/Viewer>
#include <memory>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Pyrive::OsgKit {

namespace {

// 在遍历子节点之前清理已结束的物体，避免物体在自己的回调中删除自己，
// 导致父 Group 遍历时跳过其他物体。结束标记由 Projectile.cpp 设置。
class FinishedNodeCleanup final : public osg::NodeCallback {
public:
    void operator()(osg::Node* node, osg::NodeVisitor* visitor) override {
        auto* group = node->asGroup();
        for (unsigned i = group->getNumChildren(); i > 0; --i) {
            bool finished = false;
            group->getChild(i - 1)->getUserValue("PereVia.Finished", finished);
            if (finished) group->removeChild(i - 1);
        }
        traverse(node, visitor);
    }
};

// 回调直接挂在物体自己的变换节点上，不需要额外的管理器或定时器。
class CubeMotionCallback final : public osg::NodeCallback {
public:
    CubeMotionCallback(const osg::Vec3d& position, const osg::Vec3d& velocity)
        : startPosition_(position), velocity_(velocity) {}

    void operator()(osg::Node* node, osg::NodeVisitor* visitor) override {
        auto* transform = dynamic_cast<osg::MatrixTransform*>(node);
        const osg::FrameStamp* stamp = visitor->getFrameStamp();
        if (transform && stamp && std::isfinite(stamp->getSimulationTime())) {
            const double now = stamp->getSimulationTime();
            if (!started_) {
                startTime_ = now;
                started_ = true;
            }
            // 匀速运动：当前位置 = 初始位置 + 速度向量 × 经过时间。
            // 用绝对时间计算，重复更新同一帧不会重复累加位移。
            const double elapsed = std::max(0.0, now - startTime_);
            osg::Matrixd matrix = transform->getMatrix();
            matrix.setTrans(startPosition_ + velocity_ * elapsed);
            transform->setMatrix(matrix);
        }
        traverse(node, visitor); // 继续更新子节点，不能漏掉。
    }

private:
    osg::Vec3d startPosition_;
    osg::Vec3d velocity_;
    double startTime_ = 0.0;
    bool started_ = false;
};

}

struct Runtime::Impl {
    // 借用宿主资源，不延长宿主的生命周期。
    osg::observer_ptr<osgViewer::Viewer> viewer;
    osg::observer_ptr<osg::Camera> camera;
    osg::observer_ptr<osg::Group> hostRoot;

    // 库拥有自己的节点；宿主 root 挂载它后也会持有一份引用。
    osg::ref_ptr<osg::Group> pereViaRoot = new osg::Group;
};

Runtime::Runtime(osgViewer::Viewer* viewer, osg::Camera* camera, osg::Group* hostRoot)
    : impl_(nullptr) {
    if (!viewer || !camera || !hostRoot)
        throw std::invalid_argument("Runtime requires a viewer, camera and host root");

    auto state = std::make_unique<Impl>();
    state->viewer = viewer;
    state->camera = camera;
    state->hostRoot = hostRoot;
    state->pereViaRoot->setName("PereViaRoot");
    state->pereViaRoot->setUpdateCallback(new FinishedNodeCleanup);

    // 库与宿主场景的连接就在这里。
    if (!hostRoot->addChild(state->pereViaRoot.get()))
        throw std::runtime_error("Could not attach PereViaRoot to the host root");
    impl_ = state.release();
}

Runtime::~Runtime() {
    // 只摘除本实例的节点，不清空宿主场景。
    if (impl_->hostRoot.valid())
        impl_->hostRoot->removeChild(impl_->pereViaRoot.get());
    impl_->pereViaRoot->setUpdateCallback(nullptr);
    delete impl_;
}

osg::Group* Runtime::root() const noexcept {
    return impl_->pereViaRoot.get();
}

osg::MatrixTransform* Runtime::createURMCube(const URMCubeOptions& options) {
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(options.position[i]) || !std::isfinite(options.direction[i]) ||
            !std::isfinite(options.size[i]) || options.size[i] <= 0.0f)
            throw std::invalid_argument("Cube position/direction must be finite and size must be positive");
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(options.color[i]) || options.color[i] < 0.0f || options.color[i] > 1.0f)
            throw std::invalid_argument("Cube color components must be between 0 and 1");
    }
    const double directionLength = options.direction.length();
    if (!std::isfinite(directionLength) || directionLength <= 0.0 ||
        !std::isfinite(options.speed) || options.speed < 0.0)
        throw std::invalid_argument("Cube direction must be nonzero and speed must be nonnegative");
    const osg::Vec3d direction = options.direction / directionLength;

    osg::ref_ptr<osg::ShapeDrawable> drawable = new osg::ShapeDrawable(
        new osg::Box(osg::Vec3(), options.size.x(), options.size.y(), options.size.z()));
    drawable->setColor(options.color);
    osg::ref_ptr<osg::Geode> model = new osg::Geode;
    model->addDrawable(drawable.get());
    if (options.color.a() < 1.0f) {
        model->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
        model->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    }

    // 模型在原点创建，通过外面的 MatrixTransform 控制朝向和位置。
    osg::Quat rotation;
    rotation.makeRotate(osg::Vec3d(1, 0, 0), direction);
    osg::Matrixd matrix = osg::Matrixd::rotate(rotation);
    matrix.setTrans(options.position);
    osg::ref_ptr<osg::MatrixTransform> cube = new osg::MatrixTransform(matrix);
    cube->setName("PereVia.URMCube");
    cube->setDataVariance(osg::Object::DYNAMIC);
    cube->addChild(model.get());
    cube->setUpdateCallback(new CubeMotionCallback(options.position, direction * options.speed));

    impl_->pereViaRoot->addChild(cube.get());
    return cube.get(); // PereViaRoot 已持有引用，局部 ref_ptr 释放后节点仍存在。
}

}
