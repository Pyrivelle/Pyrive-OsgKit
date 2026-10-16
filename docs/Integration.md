# PereVia OSG：场景接入、发射物与爆炸效果

宿主提供 `viewer / camera / root`，库在传入的 root 下创建并挂载 `PereViaRoot`，Runtime 析构时自动摘除它。目前提供测试用匀速立方体，以及可配置的发射物、命中闪光、火球、火花和烟雾。

```cpp
#include <PereVia/OsgKit.h>
#include <osg/MatrixTransform>

// viewer 和 root 已由宿主创建，root 已经挂到场景中。
PereVia::OsgKit::Runtime runtime(viewer, viewer->getCamera(), root);

PereVia::OsgKit::URMCubeOptions options;
options.position.set(-4, 0, 1);
options.direction.set(1, 0, 0);
options.size.set(0.8f, 0.8f, 0.8f);
options.color.set(1, 0.5f, 0.1f, 1);
options.speed = 0.5;
osg::MatrixTransform* cube = runtime.createURMCube(options);

while (!viewer->done()) {
    viewer->frame();
}
// runtime 离开作用域：移除 PereViaRoot，保留宿主自己的节点。
```

构造参数顺序固定为 viewer、camera、root。`runtime` 必须保存为变量，单独创建临时对象会在该语句末尾立刻销毁。这里的“载入库”是链接 DLL 后构造实例，不涉及手动 LoadLibrary。

```text
宿主 root
├─ 宿主已有节点
└─ PereViaRoot        ← Runtime 创建
   └─ MatrixTransform ← 匀速立方体的更新回调、位置与朝向
      └─ Geode        ← 立方体模型和颜色
```

`runtime.root()` 返回库的根节点，便于检查挂接结果。传入的 root 也可以是宿主场景中已有的某个子 Group。库不会替换 viewer 的场景、修改相机或创建窗口。viewer 和 camera 当前只保存为弱引用，暂不执行操作。

## 立方体如何创建和移动

`URM` 是 Uniform Rectilinear Motion，即匀速直线运动。参数结构体按值复制，创建后修改 options 不影响已经创建的物体。

- position：物体中心的起始位置，相对于 PereViaRoot。
- direction：运动方向，同时让模型局部 +X 朝向该方向；自动归一化，不能为零。
- size：模型局部 X/Y/Z 方向的完整边长，必须大于零。
- color：RGBA，各分量为 0～1。
- speed：每秒移动的场景单位数，0 表示静止，不能为负数。

`runtime.createURMCube(options)` 自动挂载节点。通过 Runtime 实例调用，明确物体归属于哪一棵 PereViaRoot；没有全局“当前场景”。返回的 `MatrixTransform*` 也能作为 `osg::Node*` 使用。可以设置名称，通过 `cube->getMatrix().getTrans()` 读取位置，不要 delete。库的场景子树持有它，正常使用时借用指针不超过 Runtime 的生命周期。

模型在局部原点创建，外层 MatrixTransform 控制位置和朝向。该节点的 OSG NodeCallback 在 UpdateTraversal 阶段从 FrameStamp 读取仿真时间，第一次更新记录起始时间，随后按 `位置 = 初始位置 + 单位方向 × 速度 × 经过秒数` 计算。没有独立定时器、线程或 Runtime::frame 调用。相同仿真时间不会重复移动；时间回退到起点之前时保持初始位置。手动设置矩阵中的位置会在下一次更新时被轨迹覆盖。

宿主停止调用 viewer->frame() 时不会执行更新；恢复后的下一帧按宿主给出的仿真时间定位，默认可能出现位置跳变。当前不提供单独的暂停、变速或重新定位 API。

## 发射物与指定目标碰撞

```cpp
PereVia::OsgKit::ProjectileOptions shot;
shot.position.set(0, 0, 8);
shot.direction.set(0, 0, -1);
shot.speed = 1.0;
shot.gravityEnabled = true;
shot.gravity.set(0, 0, -9.81);
shot.collisionTargets = {shipGroup, buildingGroup}; // 宿主已有的 osg::Group*。
shot.sparks.count = 100;
shot.sparks.speed = 5.0;
shot.sparks.duration = 1.5;
shot.explosion.enabled = true;
shot.explosion.radius = 2.0f;
shot.explosion.duration = 0.65;
shot.smoke.enabled = true;
shot.smoke.duration = 3.5;
shot.smoke.radius = 1.2f;
shot.smoke.riseSpeed = 1.2;
auto projectile = PereVia::OsgKit::createProjectile(runtime, shot);
```

无需额外的管理器或更新调用，仍由 `viewer->frame()` 驱动。

`createProjectile` 是命名空间下的公开函数，显式接收 `Runtime&`，发射物挂到这个实例的 PereViaRoot 下。函数没有全局默认场景，也不会创建新的 Runtime。常规接入仍只初始化一次 Runtime，并将同一实例传给后续创建操作。测试立方体继续使用 `runtime.createURMCube(options)`。

```text
配置创建 → 飞行 → 命中指定目标表面 → 闪光、火球、火花、烟雾 → 全部结束后回收
                 └─ 未命中且超过 maxFlightSeconds → 直接回收
```

| 参数 | 含义 |
| --- | --- |
| position / direction / speed | 起始中心位置、归一化后的方向、非负初速度大小 |
| gravityEnabled / gravity | 是否施加恒定重力，以及加速度向量；同时作用于发射物和火花 |
| radius / color | 发射物球体的显示半径、RGBA 颜色 |
| collisionTargets | 只遍历列表中的目标子树；空列表表示不检测碰撞 |
| maxFlightSeconds | 最大飞行时间，默认 10 秒；超时不爆炸 |
| sparks.count / speed / duration / color | 火花数量（1～4096）、初始扩散速度、持续秒数、颜色 |
| explosion.enabled / duration / radius / color | 闪光和火球开关、持续时间、火球扩张大小、颜色 |
| smoke.enabled / count / duration / radius / riseSpeed / color | 烟雾开关、烟团数（1～256）、持续时间、烟团大小、上升速度、颜色 |

发射物的位置、方向和重力采用 PereViaRoot 局部坐标。第一次更新记录起始时间，轨迹按 `p = p0 + v0*t + 0.5*g*t*t` 计算。关闭重力时是直线运动。火花以命中位置为原点，向周围扩散、逐渐淡出；本阶段不对火花本身做碰撞检测。

爆炸与烟雾默认启用。命中后先出现短暂亮闪光和膨胀火球，随后烟团沿局部 +Z 上升、扩散、膨胀并逐渐淡出。烟雾上升是视觉效果，不受发射物的重力开关影响。火花、火球和烟雾各自按配置结束，整个发射物在三者中最长的持续时间结束后统一回收；烟雾不会随火花提前消失。可以将 explosion.enabled 和 smoke.enabled 设为 false 恢复仅火花的效果。

闪光、火球与烟雾使用面向相机的 OSG Billboard 和内存生成的柔边纹理。火光采用叠加混合，烟雾采用普通透明混合；它们保留深度检测但不写入深度。没有新增图片资源或第三方依赖。这是简单的精灵视觉效果，不模拟体积烟雾、场景照明、风场或烟雾与模型的碰撞。

目标仍由宿主持有，库只保存 observer_ptr 弱引用。目标不会被重新挂载；销毁或脱离当前场景后不再参与检测。目标可以位于其他父变换分支；求交会补齐祖先变换，并检查同一目标在场景中的各条挂载路径。正常的相对变换已验证。父分支变化会在后续检测时读取，目标的当前几何会被使用，但没有对运动目标做连续扫掠。

碰撞复用 OSG 的 LineSegmentIntersector，对运动路径与目标的已加载模型表面求交。只在指定子树内检查，不扫描其他场景内容。若指定了包含 PereViaRoot 的大分支，求交会跳过本 Runtime 的整棵输出子树。多个目标按本段轨迹上最近的接触点触发一次火花。

**当前是点与表面的碰撞：radius 只控制外观，不代表球体扫掠半径。** 发射点应位于模型外部；不提供“起点已经位于实体内部”的体积检测。重力曲线用短线段近似，正常按约 1/120 秒细分，一次跳帧最多 512 段；没有固定步仿真器或独立计时线程。目标列表应传普通已加载的模型 Group，尚未验证嵌套 Camera、特殊 LOD/Switch 祖先的实例选择语义。

`PereVia::OsgKit::createProjectile(runtime, options)` 返回 `osg::ref_ptr<osg::MatrixTransform>`。与测试立方体不同，发射物会自动回收，因此返回强引用让宿主仍能安全读取最后位置。`projectile->getNumParents() == 0` 可检查是否已从场景摘除。回收先隐藏和清空物体，下一次 PereViaRoot 更新时摘除，避免在父节点遍历过程中移除当前子节点。宿主不需要继续观察时释放 ref_ptr 即可；不要将发射物重新挂到其他位置或替换其更新回调。

配置在创建时复制，后续修改配置不会改变已创建的发射物。使用单调递增的宿主仿真时间；发射物的命中和回收状态不支持倒放。API 的创建和场景引用操作继续要求在同一个 SingleThreaded 场景线程、更新遍历之外调用。

## 从哪里阅读

| 文件 | 内容 |
| --- | --- |
| `tests/Main.cpp` | 创建视景、构造 Runtime、配置运动立方体及对船只发射 |
| `include/PereVia/OsgKit.h` | Runtime 接口与立方体、发射物、火花、爆炸及烟雾参数 |
| `src/Runtime.cpp` | 场景挂接、立方体创建、运动回调与退出清理 |
| `src/Projectile.cpp` | 飞行轨迹、指定目标求交、火花绘制和结束标记 |
| `src/ImpactEffect.h/.cpp` | 内部绘制辅助：闪光、火球与烟雾的创建和动画 |
| `tests/RuntimeTests.cpp` | 接入流程与宿主仿真时间驱动运动的无窗口验证 |
| `tests/ProjectileTests.cpp` | 目标选择、重力、坐标转换、目标引用与效果回收验证 |

当前没有 Runtime::frame，OSG 的渲染循环由宿主直接运行。没有新增 System 注册框架、任务队列或额外项目。

## 构建和运行

保留 C++17、OSG 3.6.5、MSVC v143、x64，依赖复用 `C:\CProj\vcpkg`。

`PereVia.sln` 只有两个项目：`PereViaOsg`（DLL）和 `PereViaOsgTests`（宿主窗口及接入验证）。测试通过公共头文件和 DLL 的导入 LIB 使用库，不编译库源码。

```powershell
.\configure.ps1
.\build.ps1 -Configuration Release                  # 仅构建库并打包
.\build.ps1 -Configuration Release -RunTests        # 构建并运行无窗口验证
.\build.ps1 -Configuration Debug -RunTests
.\build\Release\PereViaOsgTests.exe                 # 基础窗口，Esc 退出
.\build\Release\PereViaOsgTests.exe --smoke         # 短暂打开窗口并自动退出
```

首次下载后先运行 configure.ps1，生成 build 目录内的两个项目文件，再打开根目录 PereVia.sln。Visual Studio 中生成 `PereViaOsgTests`，设为启动项目，F5 即可看到窗口。中间的立方体属于宿主，一枚球体从 z=8 落向船只，命中 z=2 的顶面后产生闪光、火球、火花和烟雾。Main 中的橙色匀速立方体示例目前保留为注释，可自行启用。相机保持 Z 轴朝上；--smoke 模式运行约 6 秒后自动退出，并在当前工作目录的 build 文件夹保存 effect-flash.ppm、effect-fire.ppm、effect-smoke.ppm 三张真实帧缓冲截图。截图功能只属于测试宿主，不进入 DLL。

单独构建库会在 `dist` 输出公共头文件、Debug/Release 的 DLL、导入 LIB 和 OSG 运行依赖。宿主需要匹配的 OSG 开发依赖；CMake 接入可使用 `find_package(PereViaOsg CONFIG REQUIRED)` 和 `PereVia::OsgKit` 目标。

## 当前生命周期约定

- 宿主持有 viewer、camera、root。库只借用这些资源，拥有自己的 PereViaRoot。
- 正常使用时先创建宿主资源，再创建 Runtime；退出时先销毁 Runtime，再释放宿主资源。
- 在同一个场景线程创建和销毁 Runtime，避开 OSG 遍历。示例使用 SingleThreaded。
- 三个参数都不能为空，否则构造抛出 `std::invalid_argument`。
- DLL 与宿主使用相同 MSVC、OSG、x64、Debug/Release 和共享 CRT（/MD 或 /MDd）。这次简化已更换公共接口，旧宿主需重新编译。

## 本次验证

Debug、Release 均已重新编译并通过 `--tests`。检查了挂载、析构移除、宿主节点保留、多个实例相互独立及空参数拒绝，以及立方体初始位置、非单位方向、朝向、仿真时间驱动位移、重复遍历、静止和无效参数拒绝。发射物验证覆盖了只命中指定目标、最近接触点、高速穿越路径、重力开关、变换分支坐标、过期与脱离场景的目标、接触前不爆炸、火花动态顶点、效果结束与未命中回收，以及多个物体同时回收的遍历安全性。两个配置均已输出 DLL、LIB 和公共头文件，测试项目通过发布包头文件及导入 LIB 链接调用。

新增效果验证覆盖了命中前不创建烟雾、命中后各层效果出现、火光和火花独立结束、烟雾继续上升膨胀及淡出、最长寿命结束后的回收，以及关闭爆炸和烟雾后仍可单独使用火花。

Release 的 `--smoke` 已运行并正常退出，输出 `Attached PereViaRoot to HostRoot`。发射物最后位置为 (0, 0, 2)、父节点数为 0，退出后宿主仍保留船只 Group。已保存并检查实际渲染的闪光、橙色火球与灰色烟团截图。
