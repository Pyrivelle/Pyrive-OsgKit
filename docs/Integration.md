# PereVia OSG：场景接入、高程地形、岛屿、发射物与爆炸效果

## TerrainBuilder：高程地形

`PereVia::OsgKit::TerrainBuilder::build(const TerrainOptions&)` 输入规则高程栅格，始终返回 `osg::ref_ptr<osg::Node>`。`enableLOD=false` 时实际为独立 Geode；`true` 时为完整网格、简化网格、两图面片三级 `osg::LOD`。调用者自行挂到宿主场景，不需要 Runtime。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `columns` / `rows` | `0`，必须提供 | X / Y 方向采样点数，分别至少 2，总数最多 4,194,304 |
| `elevations` | 空，必须提供 | 行优先 double 数组，数量恰好等于 columns*rows，允许负高程 |
| `origin` | `(0,0,0)` | 第一个采样点的 XY，以及 Z 高程基准 |
| `spacing` | `(1,1)` | X/Y 相邻采样点间距，均必须为有限正数 |
| `elevationScale` | `1` | 高程的垂直比例，必须为有限正数 |
| `lowColor` / `highColor` | 绿色 / 浅岩色 | 根据本次地形实际最低/最高 Z 插值的 RGBA 顶点颜色 |
| `enableLOD` | `false` | 开启与岛屿相同的完整 → 简化 → 面片三级 LOD |
| `lod1TriangleRatio` | `0.05` | 简化网格目标面数比例，(0,1)，实际面数受简化约束影响 |
| `lod0MinPixels` / `lod1MinPixels` | `300` / `60` | 投影尺寸 ≥300 显示完整网格，60～300 显示简化网格，更小显示面片 |
| `impostorResolution` | `256` | 俯视、侧视两张 RGBA 图各自边长，32～1024 |
| `impostorTopAngle` | `45` | 俯仰角绝对值达到此角度选俯视卡片，否则选侧视卡片，(0,90) 度 |

第 r 行、第 c 列的顶点：

```text
i = r*columns+c
x = origin.x+c*spacing.x
y = origin.y+r*spacing.y
z = origin.z+elevationScale*elevations[i]
```

行号沿 +Y 增加；如果输入数据按图像习惯从上向下排列，应由宿主根据实际坐标要求翻转行序。当前不接收经纬度，地图投影/坐标转换由宿主完成。数据必须全部有限，不接受 NoData、NaN 或无穷；不会自动填补缺失高程。数值溢出、间距在大坐标下失去可分辨性、非法颜色和网格尺寸均抛出 `std::invalid_argument`。

相邻四个采样点构成一个单元，沿固定对角线拆为两个朝 +Z 的三角形；共享顶点，避免相邻单元之间产生接缝。规则网格已有邻接关系，不需要 Earcut。LOD0 顶点高程严格采用输入转换值，不进行平滑；通过累加三角形叉积再单位化得到面积加权平滑法线。顶点颜色按 `t=(z-zMin)/(zMax-zMin)` 插值，平坦地形统一取 lowColor，避免除零。UV 覆盖 `[0,1]²`，宿主可自行设置纹理。

完整网格（LOD0）规模公式：

- 顶点数 `V=columns*rows`。
- 单元数 `C=(columns-1)*(rows-1)`，三角面数 `T=2*C`，uint32 索引数 `I=3*T`。
- XY 覆盖范围 `(columns-1)*spacing.x` × `(rows-1)*spacing.y`。
- 数组数据量 `60*V+12*T` 字节：位置 24、法线 12、颜色 16、UV 8 字节/顶点；每面三个 uint32 索引占 12 字节。

500×500 示例包含 250,000 个顶点、249,001 个单元、498,002 个三角面和 1,494,006 个索引，网格数组有效载荷为 20,976,024 字节（约 20.004 MiB），不含输入高程、生成临时数据、OSG 对象和 GPU 副本。模型使用一个 Geometry 和 VBO，生成完成后没有逐帧重新计算高程的回调。

地形保持开放地表，没有侧壁、底面或封闭体积，也不包含分块流式加载、跨地形块的 LOD 接缝缝合及地理栅格文件读取。

启用 LOD 时，先按照目标面数和原始行列比例建立稀疏规则网格，在原始高程上做双线性重采样；XY 覆盖范围保持一致，LOD1 不保留每一个原始采样点。然后使用与岛屿共享的 `osgUtil::Simplifier` 边折叠逼近目标面数。重采样会丢失部分细小山峰、山谷和高频起伏，是中远景近似；完整级别始终保持输入高程。LOD1 的颜色沿用 LOD0 的高程范围，避免因粗网格错过最高点而整体改变色带。

LOD2 与岛屿共用 CPU 两图烘焙和按相机角度择一显示的逻辑，每次实际提交 2 个三角面。纹理从 LOD0 烘焙，默认两张 256×256 RGBA 图共 512 KiB 原始像素；固定烘焙光照，使用 Alpha Test。全平地形严格侧面无投影面积，会生成透明侧图并固定选择俯视卡片，避免零高度范围除零。普通起伏地形根据俯仰角选图，侧图绕 Z 轴朝向相机。侧图只从一个固定方位拍摄，远景的轮廓、深度和光照均为近似，不用于精确碰撞。

默认硬切换，无淡入淡出或滞回。三级资源在 build 中一次生成并常驻；减少绘制开销，不自动卸载内存。根节点的 `PereVia.LOD0Triangles`、`PereVia.LOD1Triangles`、`PereVia.LOD1ActualRatio` 记录实际面数和比例，`PereVia.MinElevation`/`PereVia.MaxElevation` 为完整地形的 Z 范围。未开启 LOD 时仍返回原始完整 Geode，且忽略 LOD 专用参数。

模拟数据见 `tests/TerrainExample.h`，使用固定的高斯峰/谷与多频正弦函数生成 500×500 个高程，默认启用三级 LOD。F5 默认 `--terrain`；按 0/1/2/3 切换自动/完整/简化/面片。`--terrain-stats` 无窗口输出实际高程范围、各级面数和内存统计；`--terrain-smoke` 保存当前相机下截图；`--terrain-lod-smoke` 强制分别显示完整网格、简化网格、俯视面片和侧视面片，保存四张截图。测试逐点检查 250,000 个高程，验证 LOD0 不变、LOD1 面数/法线/UV、两张透明纹理、相机选择、平坦/长条地形及非法参数。

## IslandBuilder：岛屿生成

`PereVia::OsgKit::IslandBuilder::build(const IslandOptions&)` 始终返回 `osg::ref_ptr<osg::Node>`。`enableLOD=false` 时实际为 Geode，`true` 时为三级 LOD。它不依赖 `Runtime`，不自动挂接场景；调用者使用 `root->addChild(island.get())` 展示，保留引用即可继续使用模型。原先显式接收 Geode 的代码需要改为 Node，并重新编译宿主。参见 README 的最小示例，运行测试宿主的 `--island` 参数可以旋转、缩放查看岛屿与 LOD 切换。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `contour` | 空（必须提供） | XY 平面海岸点，3～2048 个；按边界顺序排列，可顺/逆时针，可重复首点闭合 |
| `holes` | 空 | 内环孔洞列表，最多 64 个；各环至少 3 点，内外环有效顶点总数最多 2048 |
| `seaLevel` | `0` | 海岸的 Z 坐标 |
| `height` | `20` | 最高地表采样点高于海平面的高度，>0 |
| `baseDepth` | `5` | 封闭底面低于海平面的深度，>0 |
| `subdivisions` | `4` | 三角形四等分次数，0～7；整座地表最多 100 万个三角形 |
| `slopeExponent` | `1` | 距岸高度曲线的指数，>0；增大使沿岸更平缓、山顶更集中 |
| `beachColor` | 沙色 | 岸边颜色，RGBA 各分量 0～1 |
| `landColor` | 绿色 | 地表颜色，从岸线到四分之一岛高逐渐过渡 |
| `rockColor` | 灰褐色 | 侧壁和底面颜色 |
| `enableLOD` | `false` | 开启完整网格 → 简化网格 → 两图面片三级 LOD |
| `lod1TriangleRatio` | `0.05` | 简化网格的目标面数比例，(0,1)；不是严格面数保证 |
| `lod0MinPixels` | `300` | 屏幕投影尺寸达到此值显示 LOD0 |
| `lod1MinPixels` | `60` | 到上一阈值之间显示 LOD1，再小显示 LOD2 |
| `impostorResolution` | `256` | 俯视、侧视两张 RGBA 图各自的边长，32～1024 |
| `impostorTopAngle` | `45` | 观察俯仰角绝对值达到此角度使用俯视图，否则使用侧视图 |

接受一个外轮廓和多个内环孔洞，支持凹轮廓及共线采样点。内环必须严格位于外环内部，彼此不能相交、接触、重叠或嵌套；不支持多岛。每环可顺/逆时针，可重复首点闭合，内部统一为外环逆时针、内环顺时针。不合法或近乎退化的轮廓、非有限数值、非法参数及超过网格上限均抛出 `std::invalid_argument`。连续重复点会被拒绝，首尾闭合点除外。

内环代表模型中贯穿地表和底面的空洞，沿内环生成朝向孔洞的侧壁。高度取到所有内外海岸的最近距离，因此内岸也逐渐降至 `seaLevel`。库不填充水面；可以由宿主在 `seaLevel` 放置海面，使孔洞呈现为环礁水域或岛内水域。若需要封底的湖床，则需要另一种地形定义。

```cpp
PereVia::OsgKit::IslandOptions options;
options.contour = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
options.holes = {{{30, 30}, {60, 30}, {60, 60}, {30, 60}}};
options.enableLOD = true;
osg::ref_ptr<osg::Node> island = PereVia::OsgKit::IslandBuilder::build(options);
root->addChild(island.get());
```

生成器使用内置的 [mapbox/earcut.hpp](https://github.com/mapbox/earcut.hpp) 对轮廓耳切三角化，再细分并按离海岸的最近距离升高，因此保留海湾；高度按本次网格的最大岸距归一化。修改网格精度时，最高点位置及局部地形可能略有变化。结果包含平滑地表法线、独立侧壁/底面法线、顶点颜色和 VBO，可使用宿主现有的光照显示。模型包含封闭底面，不生成海水、纹理或植被；没有随机数，同参数可重复生成。

Earcut 是私有的仅头文件依赖，固定版本及原始 ISC 许可证位于 `third_party/earcut`，构建无需联网或另装库，宿主无需包含它的头文件。与原来的逐点扫描耳切法相比，Earcut 使用链表与 Z-order 空间索引加速候选点筛选，并包含退化情况处理。我们仍先拒绝非法轮廓，再检查输出索引、初始面数、非零面积和朝向以及总投影面积；不把它对错误输入的容错作为正确性保证。

当前使用 `mapbox::earcut<uint32_t>` 的多环输入，未启用上游可选的 Delaunay `refine` 后处理。更换剖分器可能改变三角形连接方式，因此同参数与旧算法相比的局部高度采样、面积及体积可能有小幅变化。Earcut 只优化初始剖分，不减少统一细分、距岸求高及密集底面的开销。

演示程序的 `--island` / `--island-smoke` 会在控制台输出当前模型统计；`--island-stats` 仅生成和统计，不创建窗口。统计包括输入参数、存储顶点数、索引数、地表/底面/侧壁三角面数、包围盒、三维表面积、封闭体积、网格数组有效载荷和 CPU 生成时间。面积与体积分别使用场景单位的平方和立方。数组内存不包含 OSG 对象、容器预留、临时数据、驱动开销或额外的 GPU 副本；CPU 时间不包含窗口初始化及渲染。

设清理后的内外环总顶点数为 `n`，孔洞数为 `h`，细分次数为 `s`，则当前实现：

- 地表三角面数 `T = 3 * (n + 2*h - 2) * 4^s`：耳切得到 `n+2*h-2` 面，插入各面重心成为 3 倍，再逐轮四分。
- 细分后的海岸边数 `B = n * 2^s`。
- 包含底面和内外侧壁的总三角面数 `2*T + 2*B`；存储顶点数 `T + 5*B + 2 - 2*h`。顶点计数包含为法线分界重复的顶点。
- 当前位置使用 3 个 double，法线使用 3 个 float，颜色使用 4 个 float，每个存储顶点有效载荷为 52 字节；每个三角形的 3 个 uint32 索引为 12 字节。

提高细分次数会使地表和底面三角面数每轮增至 4 倍；尺寸、高度和体积本身不会改变面数。LOD0 采用统一细分，未实现 Delaunay 三角形质量优化或按曲率细分。底面沿用地表的完整细分结构，约占一半三角面，是后续减少面数的优化点。生成时每个地表顶点遍历全部岸线边求最近距离，轮廓点数较多时会明显增加 CPU 开销；生成结果没有逐帧地形计算回调。

开启 LOD 后，LOD1 先使用较低细分密度重采样，再通过 `osgUtil::Simplifier` 边折叠逼近目标。它是独立网格，因此局部山脊、法线及轮廓可能有轻微变化；细分非常低或拓扑限制时无法保证正好 5%。LOD0/LOD1 面数与实际比例可从根节点的 `PereVia.LOD0Triangles`、`PereVia.LOD1Triangles`、`PereVia.LOD1ActualRatio` 用户值读取。较高减面比例需要结合实际视角检查效果。

LOD2 在创建时用 CPU 正交光栅化完整网格，生成透明俯视图（沿 -Z 看 XY）和侧视图（沿 +Y 看 XZ）。没有 GPU 上下文要求，不需要文件或图片插件。两张 256×256 RGBA 图的原始像素共 512 KiB，不含驱动副本。每张卡片有 2 个三角面，相机剔除遍历按局部观察角度仅提交其中一张；侧视卡片绕局部 Z 轴朝向相机，俯视卡片固定在 XY 平面。选择不修改共享节点状态，适用于不同相机分别遍历。

面片采用固定方向烘焙光照和 Alpha Test，保持深度写入。极远级不保留真实地形深度、动态光照、准确侧向轮廓、半透明分层或碰撞形状；低视角从不同方位看仍共用一张侧图。这是两图方案的近似，只应在屏幕尺寸较小时启用。默认硬切换，未加入淡入淡出或滞回；阈值处可能有跳变。不要将面片层作为高精度碰撞目标。

三级资源在创建时生成并常驻，LOD 减少每帧绘制开销，不自动卸载 CPU/GPU 资源，也不代表减少总内存。相同模型的多个变换实例可共享返回节点；大量不同岛屿应在加载阶段生成。演示按 0/1/2/3 选择自动/LOD0/LOD1/LOD2，`--island-lod-smoke` 输出四张实际渲染截图用于检查。

复杂示例数据见 `tests/IslandExample.h`：80 点曲折外岸、20 点与 16 点两个内环，总计 116 点；岛高 42、底深 12、4 次细分，完整网格 184,960 面。F5 默认运行 `--island-complex`，宿主添加蓝色静态海面。`--island-complex-stats` 无窗口打印统计，`--island-complex-smoke` 保存单张截图，`--island-complex-lod-smoke` 分别保存各级截图；`--island` 仍可查看原先的 10 点示例。

## Runtime 与发射物

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
