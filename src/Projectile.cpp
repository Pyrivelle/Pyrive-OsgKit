#include <PereVia/OsgKit.h>
#include "ImpactEffect.h"

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LineWidth>
#include <osg/NodeCallback>
#include <osg/ShapeDrawable>
#include <osg/observer_ptr>
#include <osgUtil/LineSegmentIntersector>
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace PereVia::OsgKit {
namespace {

bool finite(const osg::Vec3d& value) {
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}
bool validColor(const osg::Vec4& color) {
    for (unsigned i = 0; i < 4; ++i)
        if (!std::isfinite(color[i]) || color[i] < 0 || color[i] > 1) return false;
    return true;
}

// 从指定目标开始求交，但先补齐其祖先的变换。这样目标即使位于旋转、
// 平移后的分支，求交结果也使用正确的场景坐标。忽略库自身的整棵子树。
class TargetVisitor final : public osgUtil::IntersectionVisitor {
public:
    TargetVisitor(osgUtil::Intersector* intersector, const osg::Matrixd& parentToWorld,
                  osg::Group* pluginRoot)
        : osgUtil::IntersectionVisitor(intersector), pluginRoot_(pluginRoot) {
        pushModelMatrix(new osg::RefMatrix(parentToWorld));
        push_clone();
    }
    void apply(osg::Group& group) override {
        if (&group != pluginRoot_) osgUtil::IntersectionVisitor::apply(group);
    }
private:
    osg::Group* pluginRoot_;
};

// 一个发射物对应一个回调。内部只有飞行、命中效果、结束三个阶段。
class ProjectileCallback final : public osg::NodeCallback {
public:
    ProjectileCallback(const ProjectileOptions& options, osg::Group* pluginRoot)
        : options_(options), pluginRoot_(pluginRoot) {
        velocity_ = options.direction / options.direction.length() * options.speed;
        acceleration_ = options.gravityEnabled ? options.gravity : osg::Vec3d();
        for (osg::Group* target : options.collisionTargets) targets_.emplace_back(target);
        options_.collisionTargets.clear(); // 创建后只保留上面的弱引用。
    }

    void operator()(osg::Node* node, osg::NodeVisitor* visitor) override {
        auto* projectile = static_cast<osg::MatrixTransform*>(node);
        const osg::FrameStamp* stamp = visitor->getFrameStamp();
        if (!stamp || !std::isfinite(stamp->getSimulationTime()) || finished_) {
            traverse(node, visitor);
            return;
        }
        if (!started_) {
            startTime_ = stamp->getSimulationTime();
            started_ = true;
        }
        const double elapsed = std::max(0.0, stamp->getSimulationTime() - startTime_);
        if (elapsed < flightTime_) return; // 发射后的状态不支持倒放。

        if (!exploded_) {
            // 重力轨迹是曲线，拆成短线段求交；长时间跳帧最多拆 512 段。
            const double end = std::min(elapsed, options_.maxFlightSeconds);
            const double duration = end - flightTime_;
            const unsigned steps = acceleration_.length2() == 0 ? 1 :
                static_cast<unsigned>(std::clamp(std::ceil(duration * 120.0), 1.0, 512.0));
            const double begin = flightTime_;
            for (unsigned i = 1; i <= steps && duration > 0; ++i) {
                const double next = begin + duration * i / steps;
                osg::Vec3d point;
                double ratio = 1.0;
                if (findHit(positionAt(flightTime_), positionAt(next), visitor, point, ratio)) {
                    impactTime_ = flightTime_ + (next - flightTime_) * ratio;
                    osg::Matrixd matrix = projectile->getMatrix();
                    matrix.setTrans(point);
                    projectile->setMatrix(matrix);
                    beginSparks(projectile);
                    impactEffect_.create(projectile, options_.explosion, options_.smoke);
                    exploded_ = true;
                    break;
                }
                flightTime_ = next;
            }
            if (!exploded_) {
                osg::Matrixd matrix = projectile->getMatrix();
                matrix.setTrans(positionAt(end));
                projectile->setMatrix(matrix);
                if (elapsed >= options_.maxFlightSeconds) finish(projectile);
            }
        }
        if (exploded_) {
            const double age = elapsed - impactTime_;
            // 各层效果寿命独立；烟雾通常比火球、火花更久。
            const double duration = std::max({options_.sparks.duration,
                options_.explosion.enabled ? options_.explosion.duration : 0.0,
                options_.smoke.enabled ? options_.smoke.duration : 0.0});
            if (age >= duration) finish(projectile);
            else {
                updateSparks(age);
                impactEffect_.update(age);
            }
        }
        traverse(node, visitor);
    }

private:
    osg::Vec3d positionAt(double seconds) const {
        return options_.position + velocity_ * seconds + acceleration_ * (0.5 * seconds * seconds);
    }

    bool findHit(const osg::Vec3d& start, const osg::Vec3d& end, osg::NodeVisitor* update,
                 osg::Vec3d& point, double& nearest) {
        if ((end - start).length2() == 0 || !pluginRoot_.valid()) return false;
        osg::NodePath pluginPath = update->getNodePath();
        pluginPath.pop_back(); // 去掉发射物，得到 PereViaRoot 的坐标系。
        if (pluginPath.empty()) return false;
        const osg::Matrixd pluginToWorld = osg::computeLocalToWorld(pluginPath);
        osg::Matrixd worldToPlugin;
        if (!worldToPlugin.invert(pluginToWorld)) return false;
        bool found = false;
        for (auto& observer : targets_) {
            osg::Group* target = observer.get();
            if (!target) continue;
            for (osg::NodePath path : target->getParentalNodePaths(pluginPath.front())) {
                // 不检测已移出当前场景的目标，也不检测被隐藏的祖先分支。
                if (path.empty() || path.front() != pluginPath.front() ||
                    std::any_of(path.begin(), path.end(), [](osg::Node* n) { return n->getNodeMask() == 0; }))
                    continue;
                path.pop_back(); // 目标自身的变换由 IntersectionVisitor 处理。
                const osg::Vec3d worldStart = start * pluginToWorld;
                const osg::Vec3d worldEnd = end * pluginToWorld;
                // 微量重叠相邻线段，避免浮点误差漏掉恰好落在线段端点的表面。
                constexpr double padding = 1e-6;
                const osg::Vec3d overlap = (worldEnd - worldStart) * padding;
                osg::ref_ptr<osgUtil::LineSegmentIntersector> segment =
                    new osgUtil::LineSegmentIntersector(worldStart - overlap, worldEnd + overlap);
                segment->setIntersectionLimit(osgUtil::Intersector::LIMIT_NEAREST);
                TargetVisitor query(segment.get(), osg::computeLocalToWorld(path), pluginRoot_.get());
                target->accept(query);
                if (!segment->containsIntersections()) continue;
                const auto hit = segment->getFirstIntersection();
                const double fraction = hit.ratio * (1.0 + 2.0 * padding) - padding;
                if (fraction < -1e-9 || fraction > 1.0 + 1e-9) continue;
                if (!found || fraction < nearest) {
                    nearest = std::clamp(fraction, 0.0, 1.0);
                    point = hit.getWorldIntersectPoint() * worldToPlugin;
                    found = true;
                }
            }
        }
        return found;
    }

    void beginSparks(osg::MatrixTransform* projectile) {
        projectile->removeChildren(0, projectile->getNumChildren()); // 球体在命中后消失。
        vertices_ = new osg::Vec3Array(options_.sparks.count * 2);
        colors_ = new osg::Vec4Array(1);
        geometry_ = new osg::Geometry;
        geometry_->setVertexArray(vertices_.get());
        geometry_->setColorArray(colors_.get(), osg::Array::BIND_OVERALL);
        geometry_->addPrimitiveSet(new osg::DrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices_->size())));
        geometry_->setUseDisplayList(false);
        geometry_->setUseVertexBufferObjects(true);
        geometry_->setDataVariance(osg::Object::DYNAMIC);
        osg::ref_ptr<osg::Geode> sparks = new osg::Geode;
        sparks->setName("PereVia.Sparks");
        sparkNode_ = sparks;
        sparks->addDrawable(geometry_.get());
        auto* state = sparks->getOrCreateStateSet();
        state->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE));
        state->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL, 0.0, 1.0, false));
        state->setAttributeAndModes(new osg::LineWidth(2.0f));
        state->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        projectile->addChild(sparks.get());

        std::mt19937 random(42); // 固定种子便于重现和调试。
        std::uniform_real_distribution<double> sample(-1.0, 1.0);
        for (unsigned i = 0; i < options_.sparks.count; ++i) {
            osg::Vec3d direction;
            do { direction.set(sample(random), sample(random), sample(random)); }
            while (direction.length2() < 0.001 || direction.length2() > 1.0);
            direction.normalize();
            sparkVelocities_.push_back(direction * options_.sparks.speed * (0.75 + 0.25 * sample(random)));
        }
    }

    void updateSparks(double age) {
        sparkNode_->setNodeMask(age < options_.sparks.duration ? ~0u : 0u);
        if (age >= options_.sparks.duration) return;
        for (unsigned i = 0; i < options_.sparks.count; ++i) {
            const double tailAge = std::max(0.0, age - 0.04);
            (*vertices_)[i * 2] = osg::Vec3(sparkVelocities_[i] * age + acceleration_ * (0.5 * age * age));
            (*vertices_)[i * 2 + 1] = osg::Vec3(sparkVelocities_[i] * tailAge + acceleration_ * (0.5 * tailAge * tailAge));
        }
        (*colors_)[0] = options_.sparks.color;
        (*colors_)[0].a() *= static_cast<float>(1.0 - age / options_.sparks.duration);
        vertices_->dirty();
        colors_->dirty();
        geometry_->dirtyBound();
    }

    void finish(osg::MatrixTransform* projectile) {
        finished_ = true;
        projectile->removeChildren(0, projectile->getNumChildren());
        projectile->setNodeMask(0);
        projectile->setUserValue("PereVia.Finished", true); // 下一次根节点更新安全摘除。
        targets_.clear();
        sparkVelocities_.clear();
        geometry_ = nullptr;
        vertices_ = nullptr;
        colors_ = nullptr;
        sparkNode_ = nullptr;
        impactEffect_.clear();
    }

    ProjectileOptions options_;
    osg::observer_ptr<osg::Group> pluginRoot_;
    std::vector<osg::observer_ptr<osg::Group>> targets_;
    osg::Vec3d velocity_, acceleration_;
    double startTime_ = 0.0, flightTime_ = 0.0, impactTime_ = 0.0;
    bool started_ = false, exploded_ = false, finished_ = false;
    std::vector<osg::Vec3d> sparkVelocities_;
    osg::ref_ptr<osg::Vec3Array> vertices_;
    osg::ref_ptr<osg::Vec4Array> colors_;
    osg::ref_ptr<osg::Geometry> geometry_;
    osg::ref_ptr<osg::Geode> sparkNode_;
    Detail::ImpactEffect impactEffect_;
};

}

osg::ref_ptr<osg::MatrixTransform> createProjectile(Runtime& runtime, const ProjectileOptions& options) {
    const double directionLength = options.direction.length();
    if (!finite(options.position) || !finite(options.direction) || !finite(options.gravity) ||
        !std::isfinite(directionLength) || directionLength <= 0 ||
        !std::isfinite(options.speed) || options.speed < 0 ||
        !std::isfinite(options.radius) || options.radius <= 0 ||
        !std::isfinite(options.maxFlightSeconds) || options.maxFlightSeconds <= 0 ||
        !validColor(options.color) || !validColor(options.sparks.color) ||
        !std::isfinite(options.sparks.speed) || options.sparks.speed < 0 ||
        !std::isfinite(options.sparks.duration) || options.sparks.duration <= 0 ||
        options.sparks.count == 0 || options.sparks.count > 4096)
        throw std::invalid_argument("Invalid projectile or spark options (spark count must be 1..4096)");
    if (options.explosion.enabled &&
        (!std::isfinite(options.explosion.duration) || options.explosion.duration <= 0 ||
         !std::isfinite(options.explosion.radius) || options.explosion.radius <= 0 || !validColor(options.explosion.color)))
        throw std::invalid_argument("Invalid explosion options");
    if (options.smoke.enabled &&
        (options.smoke.count == 0 || options.smoke.count > 256 ||
         !std::isfinite(options.smoke.duration) || options.smoke.duration <= 0 ||
         !std::isfinite(options.smoke.radius) || options.smoke.radius <= 0 ||
         !std::isfinite(options.smoke.riseSpeed) || options.smoke.riseSpeed < 0 || !validColor(options.smoke.color)))
        throw std::invalid_argument("Invalid smoke options (smoke count must be 1..256)");
    for (auto* target : options.collisionTargets)
        if (!target) throw std::invalid_argument("Collision target cannot be null");

    osg::ref_ptr<osg::ShapeDrawable> drawable = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(), options.radius));
    drawable->setColor(options.color);
    osg::ref_ptr<osg::Geode> model = new osg::Geode;
    model->addDrawable(drawable.get());
    if (options.color.a() < 1.0f) {
        model->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
        model->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    }
    osg::ref_ptr<osg::MatrixTransform> projectile = new osg::MatrixTransform;
    projectile->setName("PereVia.Projectile");
    projectile->setDataVariance(osg::Object::DYNAMIC);
    projectile->setMatrix(osg::Matrixd::translate(options.position));
    projectile->addChild(model.get());
    // 函数不保存全局 Runtime，直接使用调用者指定的挂接点。
    osg::Group* pluginRoot = runtime.root();
    projectile->setUpdateCallback(new ProjectileCallback(options, pluginRoot));
    pluginRoot->addChild(projectile.get());
    return projectile;
}

}
