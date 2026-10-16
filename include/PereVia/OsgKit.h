#pragma once

#include "Export.h"
#include <osg/Vec3d>
#include <osg/Vec3>
#include <osg/Vec4>
#include <osg/MatrixTransform>
#include <vector>

namespace osg { class Camera; class Group; class MatrixTransform; }
namespace osgViewer { class Viewer; }

namespace PereVia::OsgKit {

// URM = Uniform Rectilinear Motion（匀速直线运动）。位置和方向相对于 PereViaRoot。
struct URMCubeOptions {
    osg::Vec3d position{0, 0, 0};
    osg::Vec3d direction{1, 0, 0}; // 自动归一化，同时作为立方体局部 +X 的朝向。
    osg::Vec3 size{1, 1, 1};      // X/Y/Z 方向的完整边长，必须大于 0。
    osg::Vec4 color{1.0f, 0.6f, 0.1f, 1.0f}; // RGBA，各分量范围为 0～1。
    double speed = 1.0;          // 场景单位/秒，0 表示静止；不能为负数。
};

struct SparkOptions {
    unsigned count = 80;
    double speed = 6.0;
    double duration = 1.2; // 火花持续秒数。
    osg::Vec4 color{1.0f, 0.65f, 0.1f, 1.0f};
};

struct ExplosionOptions {
    bool enabled = true;
    double duration = 0.65; // 闪光与火球持续秒数。
    float radius = 2.0f;    // 火球的主要扩张范围，场景单位。
    osg::Vec4 color{1.0f, 0.35f, 0.04f, 1.0f};
};

struct SmokeOptions {
    bool enabled = true;
    unsigned count = 18;
    double duration = 3.5; // 包含逐渐出现和消散的整个阶段。
    float radius = 1.2f;   // 单团烟雾的大小。
    double riseSpeed = 1.2; // 沿 PereViaRoot 局部 +Z 上升，场景单位/秒。
    osg::Vec4 color{0.32f, 0.31f, 0.30f, 0.8f};
};

struct ProjectileOptions {
    osg::Vec3d position{0, 0, 8};
    osg::Vec3d direction{0, 0, -1}; // 自动归一化。
    double speed = 1.0;
    bool gravityEnabled = true;
    osg::Vec3d gravity{0, 0, -9.81}; // 启用后同时作用于发射物与火花。
    float radius = 0.15f;           // 仅用于球体外观，当前碰撞检测使用中心路径。
    osg::Vec4 color{1.0f, 0.8f, 0.2f, 1.0f};
    double maxFlightSeconds = 10.0; // 未命中到期直接回收，不产生火花。
    std::vector<osg::Group*> collisionTargets; // 只检查这些节点的已加载模型子树。
    SparkOptions sparks;
    ExplosionOptions explosion;
    SmokeOptions smoke;
};

// 宿主创建窗口和场景；Runtime 只在 hostRoot 下挂载自己的 PereViaRoot。
// 在场景线程创建、使用和销毁 Runtime，避免和 OSG 遍历同时修改场景。
class PEREVIA_OSG_API Runtime final {
public:
    Runtime(osgViewer::Viewer* viewer, osg::Camera* camera, osg::Group* hostRoot);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    // 借用指针，生命周期不超过 Runtime。不要手动移动或移除这个根节点。
    osg::Group* root() const noexcept;

    // 创建后自动挂到 PereViaRoot，使用 OSG 帧时间移动，无需宿主额外更新。
    // 返回借用指针，可读取矩阵/设置名称；由场景持有，不要 delete。
    // 第一次 OSG 更新作为运动起点；手动修改位置会在下次更新时被轨迹覆盖。
    osg::MatrixTransform* createURMCube(const URMCubeOptions& options = {});

private:
    // DLL 内部保存 OSG 引用，宿主不需要了解其实现。
    struct Impl;
    Impl* impl_;
};

// 创建到传入 runtime 的 PereViaRoot 下，场景归属由调用者明确指定。
// 位置、方向、重力均使用该 PereViaRoot 局部坐标；目标可在宿主场景其他分支。
// 目标只被借用，创建后修改 options 不影响已发射的物体。
// 返回 ref_ptr，避免自动回收后宿主持有悬空指针；可保留它读取最后的位置。
PEREVIA_OSG_API osg::ref_ptr<osg::MatrixTransform> createProjectile(
    Runtime& runtime, const ProjectileOptions& options = {});

}
