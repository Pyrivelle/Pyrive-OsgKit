#pragma once

#include "Export.h"
#include <osg/Vec3d>
#include <osg/Vec3>
#include <osg/Vec4>
#include <osg/MatrixTransform>
#include <osg/Geode>
#include <osg/Vec2d>
#include <vector>

namespace osg { class Camera; class Group; class MatrixTransform; }
namespace osgViewer { class Viewer; }

namespace Pyrive::OsgKit {

// 规则高程栅格：列沿 +X，行沿 +Y，+Z 向上；坐标使用宿主场景单位。
struct TerrainOptions {
    unsigned columns = 0; // X 方向采样点数，至少 2。
    unsigned rows = 0;    // Y 方向采样点数，至少 2；总采样点数最多 4,194,304。
    // 行优先：elevations[row * columns + column]，数量必须恰好等于 rows*columns。
    // 每项是原始高程，允许负值；不接受 NaN、无穷或 NoData 标记。
    std::vector<double> elevations;
    osg::Vec3d origin{0, 0, 0}; // 第一个采样点的 XY 及高程的 Z 基准。
    osg::Vec2d spacing{1, 1};   // 相邻采样点的 X/Y 间距，均为正数。
    double elevationScale = 1.0; // Z=origin.z+elevationScale*高程，必须为有限正数。
    osg::Vec4 lowColor{0.16f, 0.38f, 0.16f, 1};
    osg::Vec4 highColor{0.88f, 0.86f, 0.78f, 1}; // 按本次网格最低/最高高程插值。
    bool enableLOD = false;        // false 返回 Geode；true 返回三级 osg::LOD。
    double lod1TriangleRatio = 0.05; // LOD1 目标面数比例，(0,1)，受网格/简化约束可能略有偏差。
    float lod0MinPixels = 300.0f;   // 投影尺寸 >= 此值显示完整地形。
    float lod1MinPixels = 60.0f;    // [此值,lod0MinPixels) 显示简化地形，再小显示面片。
    unsigned impostorResolution = 256; // 两张 RGBA 图各自的边长，32..1024。
    double impostorTopAngle = 45.0; // 俯仰角绝对值 >= 此值选俯视图，否则选侧视图。
};

class PEREVIA_OSG_API TerrainBuilder final {
public:
    // 由高程创建规则三角网格；LOD0 精确保留输入采样点，enableLOD 控制 Geode/三级 LOD。
    // 包含平滑法线、颜色、0..1 UV；不依赖 Runtime，不自动挂场景。
    // 当前生成开放地表，不生成侧壁/底面，也不读取高程文件。
    // 非法数据或坐标不可表示时抛出 std::invalid_argument。
    static osg::ref_ptr<osg::Node> build(const TerrainOptions& options);
};

struct IslandOptions {
    // XY 平面中的外海岸，至少 3 点；各环可首尾闭合，可顺/逆时针。
    // 外环和内环的有效点合计最多 2048；坐标使用场景单位，+Z 向上。
    std::vector<osg::Vec2d> contour;
    // 孔洞（如环礁水域），最多 64 个；严格位于外环内，不能相交、接触或嵌套。
    // 地表和底面均留空，沿内岸生成侧壁；水面由宿主自行提供。
    std::vector<std::vector<osg::Vec2d>> holes;
    double seaLevel = 0.0;
    double height = 20.0;       // 地表最高点相对海平面的高度，必须大于 0。
    double baseDepth = 5.0;     // 底面在海平面以下的深度，必须大于 0。
    unsigned subdivisions = 4; // 每轮将三角形四等分，0..7；最多生成 100 万个地表三角形。
    double slopeExponent = 1.0; // >0；越大沿岸越平缓、山顶越集中。
    osg::Vec4 beachColor{0.82f, 0.73f, 0.48f, 1.0f};
    osg::Vec4 landColor{0.22f, 0.48f, 0.19f, 1.0f};
    osg::Vec4 rockColor{0.38f, 0.36f, 0.32f, 1.0f};
    bool enableLOD = false;      // false 返回 Geode；true 返回包含以下三级的 osg::LOD。
    double lod1TriangleRatio = 0.05; // LOD1 目标面数/LOD0 面数，(0,1)；受几何约束可能略有偏差。
    float lod0MinPixels = 300.0f; // 屏幕投影尺寸 >= 此值使用完整网格。
    float lod1MinPixels = 60.0f;  // 此值到 lod0MinPixels 使用简化网格，更远使用面片。
    unsigned impostorResolution = 256; // 两张 RGBA 纹理各自的边长，32..1024。
    double impostorTopAngle = 45.0; // 俯仰角绝对值 >= 此角度选俯视图，(0,90) 度。
};

class PEREVIA_OSG_API IslandBuilder final {
public:
    // 按到海岸的距离生成地形；enableLOD 控制 Geode/三级 LOD，统一返回 Node。
    // 不依赖 Runtime，不自动挂场景；调用者持有返回引用并自行 addChild。
    // 非法轮廓/参数或超出网格上限时抛出 std::invalid_argument。
    static osg::ref_ptr<osg::Node> build(const IslandOptions& options);
};

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
