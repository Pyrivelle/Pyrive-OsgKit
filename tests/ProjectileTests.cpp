#include <PereVia/OsgKit.h>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Billboard>
#include <osg/FrameStamp>
#include <osg/Image>
#include <osg/State>
#include <osg/ShapeDrawable>
#include <osgViewer/Viewer>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(const osg::Vec3d& a, const osg::Vec3d& b) {
    if ((a - b).length() < 1e-5) return true;
    std::cerr << "Expected (" << b.x() << ", " << b.y() << ", " << b.z() << "), got ("
        << a.x() << ", " << a.y() << ", " << a.z() << ")\n";
    return false;
}

struct Host {
    osg::ref_ptr<osgViewer::Viewer> viewer = new osgViewer::Viewer;
    osg::ref_ptr<osg::Group> root = new osg::Group;
    PereVia::OsgKit::Runtime runtime{viewer.get(), viewer->getCamera(), root.get()};
    Host() { viewer->setSceneData(root.get()); }
    void frame(double time) { viewer->advance(time); viewer->updateTraversal(); }
};

osg::ref_ptr<osg::MatrixTransform> box(const osg::Vec3d& center) {
    osg::ref_ptr<osg::Geode> model = new osg::Geode;
    model->addDrawable(new osg::ShapeDrawable(new osg::Box(osg::Vec3(), 2.0f)));
    osg::ref_ptr<osg::MatrixTransform> group = new osg::MatrixTransform;
    group->setMatrix(osg::Matrixd::translate(center));
    group->addChild(model.get());
    return group;
}

void selectedTargetsAndSparks() {
    Host host;
    auto ignored = box(osg::Vec3d(2, 0, 0));
    auto target = box(osg::Vec3d(6, 0, 0));
    auto farther = box(osg::Vec3d(10, 0, 0));
    host.root->addChild(ignored.get());
    host.root->addChild(target.get());
    host.root->addChild(farther.get());
    const int targetReferences = target->referenceCount();
    PereVia::OsgKit::ProjectileOptions options;
    options.position.set(0, 0, 0);
    options.direction.set(7, 0, 0);
    options.speed = 100.0; // 一帧越过整个目标，必须检查运动路径。
    options.gravityEnabled = false;
    options.collisionTargets = {farther.get(), target.get()}; // 故意把较远目标写在前面。
    options.sparks.duration = 1.0;
    options.explosion.enabled = false;
    options.smoke.enabled = false;
    auto projectile = PereVia::OsgKit::createProjectile(host.runtime, options);
    require(target->referenceCount() == targetReferences && target->getNumParents() == 1,
        "Binding targets changed their ownership or parentage");
    host.frame(0.0);
    host.frame(0.1);
    require(near(projectile->getMatrix().getTrans(), osg::Vec3d(5, 0, 0)),
        "Projectile hit an unselected target or failed to choose the nearest selected target");
    auto* sparks = dynamic_cast<osg::Geode*>(projectile->getChild(0));
    require(sparks && dynamic_cast<osg::Geometry*>(sparks->getDrawable(0)),
        "Impact did not produce spark geometry");
    auto* vertices = dynamic_cast<osg::Vec3Array*>(sparks->getDrawable(0)->asGeometry()->getVertexArray());
    require(vertices && vertices->size() == options.sparks.count * 2 && (*vertices)[0].length() > 0,
        "Spark vertices were not animated");
    host.frame(0.5);
    require(near(projectile->getMatrix().getTrans(), osg::Vec3d(5, 0, 0)), "Impact position drifted");
    host.frame(1.2); // 效果结束后隐藏。
    host.frame(1.3); // 下一次根节点遍历安全摘除。
    require(projectile->getNumParents() == 0 && projectile->getNumChildren() == 0,
        "Finished sparks were not reclaimed");
    require(host.root->containsNode(target.get()) && host.root->containsNode(ignored.get()),
        "Projectile cleanup changed host targets");
}

void gravityAndMisses() {
    Host host;
    PereVia::OsgKit::ProjectileOptions options;
    options.position.set(0, 0, 10);
    options.direction.set(1, 0, 0);
    options.speed = 2.0;
    options.gravity.set(0, 0, -2);
    options.maxFlightSeconds = 3.0;
    auto falling = PereVia::OsgKit::createProjectile(host.runtime, options);
    options.gravityEnabled = false;
    auto straight = PereVia::OsgKit::createProjectile(host.runtime, options);
    host.frame(0);
    host.frame(2);
    require(near(falling->getMatrix().getTrans(), osg::Vec3d(4, 0, 6)), "Gravity trajectory was wrong");
    require(near(straight->getMatrix().getTrans(), osg::Vec3d(4, 0, 10)), "Disabled gravity still acted");
    host.frame(4);
    host.frame(5);
    require(falling->getNumParents() == 0 && straight->getNumParents() == 0,
        "Unhit projectiles were not reclaimed at their lifetime");
    require(falling->getNumChildren() == 0, "A missed projectile produced an explosion");
}

void transformedBranchesAndDeadTargets() {
    Host host;
    // 发射物与目标分别位于不同的父变换下，需要正确处理两边的坐标。
    osg::ref_ptr<osg::MatrixTransform> mount = new osg::MatrixTransform;
    mount->setMatrix(osg::Matrixd::rotate(osg::PI_2, osg::Vec3d(0, 0, 1)) *
                     osg::Matrixd::translate(10, 0, 0));
    host.root->addChild(mount.get());
    PereVia::OsgKit::Runtime nested(host.viewer.get(), host.viewer->getCamera(), mount.get());
    osg::ref_ptr<osg::MatrixTransform> targetParent = new osg::MatrixTransform;
    targetParent->setMatrix(osg::Matrixd::translate(10, 4, 0));
    auto target = box(osg::Vec3d(0, 2, 0));
    targetParent->addChild(target.get());
    host.root->addChild(targetParent.get());
    PereVia::OsgKit::ProjectileOptions options;
    options.position.set(0, 0, 0);
    options.direction.set(1, 0, 0);
    options.speed = 10;
    options.gravityEnabled = false;
    options.collisionTargets = {target.get()};
    auto projectile = PereVia::OsgKit::createProjectile(nested, options);
    require(nested.root()->containsNode(projectile.get()) && !host.runtime.root()->containsNode(projectile.get()),
        "Projectile was not attached to the explicitly supplied Runtime");
    host.frame(0);
    host.frame(1);
    require(near(projectile->getMatrix().getTrans(), osg::Vec3d(5, 0, 0)),
        "Collision between transformed branches used the wrong coordinates");

    // 一个目标从场景移除但仍存活，另一个被彻底销毁；都不应再参与检测。
    auto removed = box(osg::Vec3d(2, 0, 0));
    auto destroyed = box(osg::Vec3d(4, 0, 0));
    host.root->addChild(removed.get());
    host.root->addChild(destroyed.get());
    options.collisionTargets = {removed.get(), destroyed.get()};
    auto missed = PereVia::OsgKit::createProjectile(host.runtime, options);
    require(host.runtime.root()->containsNode(missed.get()) && !nested.root()->containsNode(missed.get()),
        "Creating a second projectile used the wrong Runtime");
    host.root->removeChild(removed.get());
    host.root->removeChild(destroyed.get());
    destroyed = nullptr;
    host.frame(2);
    host.frame(2.8);
    require(near(missed->getMatrix().getTrans(), osg::Vec3d(8, 0, 0)),
        "An expired or detached target still affected the projectile");
}

void gravityContactAndCleanupOrder() {
    Host host;
    auto target = box(osg::Vec3d(0, 0, 0));
    host.root->addChild(target.get());
    PereVia::OsgKit::ProjectileOptions options;
    options.position.set(0, 0, 5);
    options.direction.set(0, 0, -1);
    options.speed = 0;
    options.gravity.set(0, 0, -2);
    options.collisionTargets = {host.root.get()}; // 允许指定大分支，必须排除库自己的节点。
    options.sparks.duration = 0.1;
    options.explosion.enabled = false;
    options.smoke.enabled = false;
    auto one = PereVia::OsgKit::createProjectile(host.runtime, options);
    auto two = PereVia::OsgKit::createProjectile(host.runtime, options);
    host.frame(0);
    host.frame(1.9);
    require(near(one->getMatrix().getTrans(), osg::Vec3d(0, 0, 1.39)),
        "Projectile exploded before actual contact");
    host.frame(2.0);
    host.frame(2.05);
    require(near(one->getMatrix().getTrans(), osg::Vec3d(0, 0, 1)), "Gravity impact was not at the surface");
    host.frame(2.2);
    host.frame(2.3);
    require(one->getNumParents() == 0 && two->getNumParents() == 0,
        "Removing one completed object skipped another object's update");
}

osg::Node* findChild(osg::Group* group, const char* name) {
    for (unsigned i = 0; i < group->getNumChildren(); ++i)
        if (group->getChild(i)->getName() == name) return group->getChild(i);
    return nullptr;
}

void explosionAndSmoke() {
    Host host;
    auto target = box(osg::Vec3d(0, 0, 0));
    host.root->addChild(target.get());
    PereVia::OsgKit::ProjectileOptions options;
    options.position.set(0, 0, 3);
    options.direction.set(0, 0, -1);
    options.speed = 10;
    options.gravityEnabled = false;
    options.collisionTargets = {target.get()};
    options.sparks.duration = 0.3;
    options.explosion.duration = 0.5;
    options.smoke.duration = 2.0;
    options.smoke.count = 12;
    auto projectile = PereVia::OsgKit::createProjectile(host.runtime, options);
    host.frame(0);
    require(!findChild(projectile.get(), "PereVia.Smoke"), "Smoke appeared before contact");
    host.frame(0.25); // t=0.2 命中，此时效果年龄为 0.05 秒。
    auto* flash = findChild(projectile.get(), "PereVia.ExplosionFlash");
    auto* fire = findChild(projectile.get(), "PereVia.Fireball");
    auto* smoke = dynamic_cast<osg::Billboard*>(findChild(projectile.get(), "PereVia.Smoke"));
    auto* sparks = findChild(projectile.get(), "PereVia.Sparks");
    require(flash && fire && smoke && sparks, "Impact did not create all effect layers");
    require(flash->getNodeMask() && fire->getNodeMask() && smoke->getNodeMask(), "Initial effect layers were hidden");
    require(smoke->getNumDrawables() == options.smoke.count, "Smoke count was not applied");
    const osg::Vec3 earlyPosition = smoke->getPosition(0);
    const float earlyRadius = smoke->getDrawable(0)->getBound().radius();
    host.frame(0.9); // 火光与火花结束，但烟雾必须继续存在并上升、膨胀。
    require(!flash->getNodeMask() && !fire->getNodeMask() && !sparks->getNodeMask(),
        "Flash, fire or sparks outlived their configured duration");
    require(smoke->getNodeMask() && projectile->getNumParents() == 1, "Smoke was removed with the sparks");
    require(smoke->getPosition(0).z() > earlyPosition.z() && smoke->getDrawable(0)->getBound().radius() > earlyRadius,
        "Smoke did not rise and expand");
    auto* colors = dynamic_cast<osg::Vec4Array*>(smoke->getDrawable(0)->asGeometry()->getColorArray());
    const float middleAlpha = (*colors)[0].a();
    host.frame(1.8);
    require((*colors)[0].a() < middleAlpha, "Smoke did not fade out");
    host.frame(2.3);
    host.frame(2.4);
    require(projectile->getNumParents() == 0 && projectile->getNumChildren() == 0,
        "Completed explosion and smoke were not reclaimed");

    // 关闭两层新增效果后，仍然可以单独使用原有火花。
    options.explosion.enabled = false;
    options.smoke.enabled = false;
    auto sparksOnly = PereVia::OsgKit::createProjectile(host.runtime, options);
    host.frame(3.0);
    host.frame(3.25);
    require(findChild(sparksOnly.get(), "PereVia.Sparks") &&
        !findChild(sparksOnly.get(), "PereVia.Fireball") && !findChild(sparksOnly.get(), "PereVia.Smoke"),
        "Disabled effect layers were still created");
}
}

void runProjectileTests() {
    selectedTargetsAndSparks();
    gravityAndMisses();
    transformedBranchesAndDeadTargets();
    gravityContactAndCleanupOrder();
    explosionAndSmoke();
}

namespace {
// 仅供 --smoke 的渲染验证：从实际帧缓冲保存闪光、火球与烟雾三个阶段。
// PPM 无需图片插件，避免为了截图给功能库增加依赖。
class EffectCapture final : public osg::Camera::DrawCallback {
public:
    explicit EffectCapture(osg::MatrixTransform* projectile) : projectile_(projectile) {}
    void operator()(osg::RenderInfo& info) const override {
        if (captured_ == 3 || !projectile_.valid()) return;
        const double time = info.getState()->getFrameStamp()->getSimulationTime();
        if (!started_) {
            if (projectile_->getNumChildren() < 2) return;
            impactTime_ = time;
            started_ = true;
        }
        const double times[] = {0.08, 0.32, 1.3};
        if (time - impactTime_ < times[captured_]) return;
        const osg::Viewport* viewport = info.getCurrentCamera()->getViewport();
        const int width = static_cast<int>(viewport->width()), height = static_cast<int>(viewport->height());
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->readPixels(static_cast<int>(viewport->x()), static_cast<int>(viewport->y()),
            width, height, GL_RGB, GL_UNSIGNED_BYTE);
        std::filesystem::create_directories("build");
        const char* paths[] = {"build/effect-flash.ppm", "build/effect-fire.ppm", "build/effect-smoke.ppm"};
        const char* path = paths[captured_];
        std::ofstream file(path, std::ios::binary);
        file << "P6\n" << width << ' ' << height << "\n255\n";
        for (int row = height - 1; row >= 0; --row)
            file.write(reinterpret_cast<const char*>(image->data(0, row)), width * 3);
        if (!file) throw std::runtime_error("Could not write effect capture");
        std::cout << "Captured " << path << '\n';
        ++captured_;
    }
private:
    osg::observer_ptr<osg::MatrixTransform> projectile_;
    mutable bool started_ = false;
    mutable double impactTime_ = 0;
    mutable unsigned captured_ = 0;
};
}

void enableEffectCapture(osg::Camera* camera, osg::MatrixTransform* projectile) {
    camera->setPostDrawCallback(new EffectCapture(projectile));
}
