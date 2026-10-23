#include "IslandLOD.h"
#include <osg/AlphaFunc>
#include <osg/Billboard>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/LOD>
#include <osg/NodeCallback>
#include <osg/Texture2D>
#include <osgUtil/Simplifier>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Pyrive::OsgKit::Detail {
namespace {
// 统计本模块使用的 GL_TRIANGLES 网格面数：每三个顶点索引对应一个三角形。
// 统计的是 primitive set 实际提交的索引，包含地表、底面及内外侧壁。
size_t faceCount(const osg::Geometry& mesh) {
    size_t count = 0;
    for (unsigned i = 0; i < mesh.getNumPrimitiveSets(); ++i)
        count += mesh.getPrimitiveSet(i)->getNumIndices() / 3;
    return count;
}

// 就地简化 build() 生成的粗网格，target 是期望保留的“总三角面数”。
// osgUtil::Simplifier 通过边折叠减少顶点/三角形，随后恢复 double 场景坐标并重算法线。
// 不修改完整网格 LOD0；简化器受几何/拓扑约束，结果不保证精确等于 target。
void simplify(osg::Geode& geode, double target) {
    auto* mesh = geode.getDrawable(0)->asGeometry();
    const auto* source = static_cast<const osg::Vec3dArray*>(mesh->getVertexArray());
    const osg::Vec3d origin = source->front();
    double scale = 0;
    for (const auto& p : *source) scale = std::max(scale, (p - origin).length());
    // OSG Simplifier 使用 float 顶点。取 O=首顶点、S=max|P-O|，
    // 使用 P_local=(P-O)/S 后再转 float，避免大绝对坐标掩盖局部细节。
    // 简化结束后用 P_scene=O+S*P_local 还原。float 简化本身仍会有数值近似。
    osg::ref_ptr<osg::Vec3Array> local = new osg::Vec3Array;
    for (const auto& p : *source) local->push_back(osg::Vec3((p - origin) / scale));
    mesh->setVertexArray(local.get());
    // Simplifier 接受“保留面数比例”，因此 r=目标面数/粗网格现有面数。
    // r 最大为 1，防止目标比现有面数还大时被解释成加密网格。
    osgUtil::Simplifier simplifier(std::min(1.0, target / faceCount(*mesh)));
    // 保留普通三角形索引，不转为三角带；我们在后面自行计算面积加权法线。
    simplifier.setDoTriStrip(false);
    simplifier.setSmoothing(false);
    simplifier.simplify(*mesh);
    osg::ref_ptr<osg::Vec3dArray> restored = new osg::Vec3dArray;
    const auto* reduced = static_cast<const osg::Vec3Array*>(mesh->getVertexArray());
    for (const auto& p : *reduced) restored->push_back(origin + osg::Vec3d(p) * scale);
    mesh->setVertexArray(restored.get());
    // 边折叠改变三角形后重新计算法线，避免直接沿用旧网格插值法线。
    // 累加 N=(B-A)×(C-A) 再单位化，等价于按各面的面积加权平滑。
    // 叉积前统一除以 scale，不改变单位化后的方向，并降低大数乘积溢出风险。
    std::vector<osg::Vec3d> accumulated(restored->size());
    for (unsigned i = 0; i < mesh->getNumPrimitiveSets(); ++i) {
        const auto* primitive = mesh->getPrimitiveSet(i);
        for (unsigned j = 0; j < primitive->getNumIndices(); j += 3) {
            const unsigned a = primitive->index(j), b = primitive->index(j + 1), c = primitive->index(j + 2);
            const osg::Vec3d normal = (((*restored)[b] - (*restored)[a]) / scale) ^
                                     (((*restored)[c] - (*restored)[a]) / scale);
            accumulated[a] += normal; accumulated[b] += normal; accumulated[c] += normal;
        }
    }
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    for (auto& normal : accumulated) {
        // 对没有非零相邻面积的残留点提供默认方向，避免输出零法线。
        if (normal.normalize() == 0) normal.set(0, 0, 1);
        normals->push_back(osg::Vec3(normal));
    }
    mesh->setNormalArray(normals.get(), osg::Array::BIND_PER_VERTEX);
    mesh->dirtyBound();
    mesh->setUseDisplayList(false);
    mesh->setUseVertexBufferObjects(true);
}

// 完整网格在场景局部坐标中的轴对齐包围盒；center=(low+high)/2。
// 用于把几何投影到图片，并确定面片大小和放置中心。
struct Bounds {
    osg::Vec3d low, high, center;
};

// 软件正交光栅化：从完整网格烘焙一张 size×size RGBA 图片。
// top=true：从 +Z 向 -Z 看，图片坐标为 XY；false：从 -Y 向 +Y 看，坐标为 XZ。
// 每个三角形投影到二维，再通过重心坐标插值深度和颜色，用深度缓冲决定遮挡。
// 未被三角形覆盖的像素保持 alpha=0，因而外轮廓之外和俯视孔洞中都是透明的。
// 此过程无需窗口、GL 上下文或图片插件；光照固定烘焙，不跟随宿主光源变化。
osg::ref_ptr<osg::Image> bake(const osg::Geometry& mesh, const Bounds& bounds,
                            unsigned size, bool top) {
    const auto& vertices = *static_cast<const osg::Vec3dArray*>(mesh.getVertexArray());
    const auto& normals = *static_cast<const osg::Vec3Array*>(mesh.getNormalArray());
    const auto& colors = *static_cast<const osg::Vec4Array*>(mesh.getColorArray());
    const auto& indices = *static_cast<const osg::DrawElementsUInt*>(mesh.getPrimitiveSet(0));
    // 图片水平轴始终取 X，竖轴按视图取 Y（俯视）或 Z（侧视）。
    const unsigned vertical = top ? 1u : 2u;
    const double width = bounds.high.x() - bounds.low.x();
    const double height = bounds.high[vertical] - bounds.low[vertical];
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->allocateImage(size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    std::memset(image->data(), 0, image->getTotalSizeInBytes());
    // 平坦高程从严格侧面看没有投影面积，返回全透明侧图，避免除以零。
    // 装配时会对这种情况固定选择水平的俯视卡片。
    if (width <= 0 || height <= 0) return image;
    // 自定义“越大越近”的深度：俯视取 Z，侧视取 -Y。
    // 初值为负无穷，保证第一个有效片元可以写入。
    std::vector<double> depth(size * size, -std::numeric_limits<double>::infinity());
    osg::Vec3 light(0.3f, -0.4f, 0.85f); light.normalize();
    for (size_t i = 0; i < indices.size(); i += 3) {
        osg::Vec2d p[3]; double z[3]; osg::Vec4 shade[3];
        for (unsigned j = 0; j < 3; ++j) {
            const unsigned index = indices[i + j];
            const auto& v = vertices[index];
            // 包围盒坐标映射到图片：[min,max] → [1,size-1]。
            // 例如 u=((x-xMin)/(xMax-xMin))*(size-2)+1；为边缘留出透明采样余量。
            // 这是正交投影，不除以相机距离，也没有近大远小。
            p[j].set((v.x() - bounds.low.x()) / width * (size - 2) + 1,
                     (v[vertical] - bounds.low[vertical]) / height * (size - 2) + 1);
            z[j] = top ? v.z() : -v.y();
            shade[j] = colors[index];
            // 固定光照：I=0.35+0.65*max(0,N·L)。
            // 0.35 为基础亮度，点积表示 Lambert 漫反射；只缩放 RGB，不改变 alpha。
            const float lighting = 0.35f + 0.65f * std::max(0.0f, normals[index] * light);
            for (unsigned c = 0; c < 3; ++c) shade[j][c] *= lighting;
        }
        // 二维叉积 A×B=ax*by-ay*bx，用于求有向面积和后面的重心坐标。
        auto cross = [](const osg::Vec2d& a, const osg::Vec2d& b) {
            return a.x() * b.y() - a.y() * b.x();
        };
        const double area = cross(p[1] - p[0], p[2] - p[0]);
        if (std::abs(area) < 1e-12) continue;
        // 只扫描该三角形投影包围盒内的像素，并裁到图片范围，避免整图逐像素测试。
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({p[0].x(), p[1].x(), p[2].x()}))));
        const int maxX = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(std::max({p[0].x(), p[1].x(), p[2].x()}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({p[0].y(), p[1].y(), p[2].y()}))));
        const int maxY = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(std::max({p[0].y(), p[1].y(), p[2].y()}))));
        for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x) {
            // 在像素中心采样。A/B/C 的重心权重分别等于其对面子三角形与总面积之比：
            // a=((B-P)×(C-P))/((B-A)×(C-A))，b 同理，c=1-a-b。
            // 三个权重均非负时 P 在三角形内；使用有向面积可同时处理两种绕序。
            const osg::Vec2d point(x + 0.5, y + 0.5);
            const double a = cross(p[1] - point, p[2] - point) / area;
            const double b = cross(p[2] - point, p[0] - point) / area;
            const double c = 1.0 - a - b;
            if (std::min({a, b, c}) < -1e-9) continue;
            // 正交投影下，可用重心权重线性插值深度和顶点颜色：
            // depth=a*zA+b*zB+c*zC，color=a*CA+b*CB+c*CC。
            // 深度小于已记录值表示被更近的面遮挡，跳过该片元。
            const double d = a * z[0] + b * z[1] + c * z[2];
            const size_t index = static_cast<size_t>(y) * size + x;
            if (d < depth[index]) continue;
            const osg::Vec4 color = shade[0] * static_cast<float>(a) + shade[1] * static_cast<float>(b) + shade[2] * static_cast<float>(c);
            if (color.a() <= 0) continue;
            depth[index] = d;
            auto* pixel = image->data(x, y);
            // 将 [0,1] 浮点颜色映射为 0..255 字节；+0.5 实现正数四舍五入。
            for (unsigned channel = 0; channel < 4; ++channel)
                pixel[channel] = static_cast<unsigned char>(std::clamp(color[channel], 0.0f, 1.0f) * 255 + 0.5f);
        }
    }
    return image;
}

// 根据包围盒生成一张由两个三角形组成的贴图矩形，中心暂时位于原点。
// 俯视卡片位于 XY 平面，侧视卡片位于 XZ 平面，随后由父节点放到岛屿中心。
// 矩形内部的孔洞形状由纹理 alpha 表达，不另外创建复杂的孔洞几何。
osg::ref_ptr<osg::Geometry> card(const Bounds& b, bool top, osg::Image* image) {
    const double halfWidth = (b.high.x() - b.low.x()) * 0.5;
    const double halfHeight = ((top ? b.high.y() : b.high.z()) - (top ? b.low.y() : b.low.z())) * 0.5;
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    for (const auto& p : {osg::Vec2d(-halfWidth, -halfHeight), osg::Vec2d(halfWidth, -halfHeight),
                          osg::Vec2d(halfWidth, halfHeight), osg::Vec2d(-halfWidth, halfHeight)})
        vertices->push_back(top ? osg::Vec3(static_cast<float>(p.x()), static_cast<float>(p.y()), 0) :
            osg::Vec3(static_cast<float>(p.x()), 0, static_cast<float>(p.y())));
    osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
    // 与 bake 的图片映射对应：几何左右/上下边缘映射到 1/size、1-1/size。
    // UV 都是无量纲归一化纹理坐标；白色顶点颜色用于保留烘焙纹理自身颜色。
    const float lo = 1.0f / image->s(), hi = 1.0f - lo;
    uv->push_back({lo, lo}); uv->push_back({hi, lo}); uv->push_back({hi, hi}); uv->push_back({lo, hi});
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    colors->push_back(osg::Vec4(1, 1, 1, 1));
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    geometry->setVertexArray(vertices.get()); geometry->setTexCoordArray(0, uv.get());
    geometry->setColorArray(colors.get(), osg::Array::BIND_OVERALL);
    osg::ref_ptr<osg::DrawElementsUByte> indices = new osg::DrawElementsUByte(GL_TRIANGLES);
    for (unsigned char index : {0, 1, 2, 0, 2, 3}) indices->push_back(index);
    geometry->addPrimitiveSet(indices.get());
    geometry->setUseDisplayList(false); geometry->setUseVertexBufferObjects(true);
    osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
    // 线性采样让远景缩放较平滑，边缘夹取防止纹理在矩形边界重复平铺。
    texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    auto* state = geometry->getOrCreateStateSet();
    state->setTextureAttributeAndModes(0, texture.get());
    state->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    // 纹理已有固定光照，因此关闭二次光照；关闭背面剔除让两面均可见。
    // Alpha Test 仅保留 alpha>0.25 的片元，并保持深度写入。
    // 它不做半透明混合，省去透明卡片的混合排序，但无法准确表示多层半透明材质。
    state->setMode(GL_BLEND, osg::StateAttribute::OFF);
    state->setAttributeAndModes(new osg::AlphaFunc(osg::AlphaFunc::GREATER, 0.25f));
    return geometry;
}

// 在每个相机的剔除遍历中，只选择俯视/侧视卡片之一。
// 父变换已把岛屿中心移至局部原点，所以 getEyePoint() 就是中心指向相机的向量。
// 不修改子节点开关或 node mask，不同相机可对同一个模型选择不同卡片。
class ViewSelector final : public osg::NodeCallback {
public:
    // angle 以度输入，先乘 π/180 转弧度，再预计算 sin(angle)。
    explicit ViewSelector(double angle, bool planar = false)
        : threshold_(std::sin(angle * 3.141592653589793 / 180.0)), planar_(planar) {}
    void operator()(osg::Node* node, osg::NodeVisitor* visitor) override {
        if (visitor->getVisitorType() != osg::NodeVisitor::CULL_VISITOR) { traverse(node, visitor); return; }
        const osg::Vec3d eye = visitor->getEyePoint();
        // 俯仰角 θ 满足 sin(|θ|)=|eye.z|/|eye|。
        // 将 |θ|>=angle 改写为 |eye.z|>=|eye|*sin(angle)，避免每帧计算 asin。
        // abs 表示从上方或下方接近竖直观察时都选俯视卡片，是两图方案的近似。
        const bool top = planar_ || std::abs(eye.z()) >= eye.length() * threshold_;
        node->asGroup()->getChild(top ? 0 : 1)->accept(*visitor);
    }
private:
    double threshold_;
    bool planar_;
};
}

// build() 的 LOD 收尾阶段：full 作为 LOD0，粗网格减面后作为 LOD1，
// 从 full 烘焙两图并创建择一显示的卡片组作为 LOD2，最后装入 osg::LOD。
// 输入几何体由 build() 新建，本函数会设置它们的名称并就地简化 coarse。
osg::ref_ptr<osg::Node> createMeshLOD(osg::Geode* full, osg::Geode* coarse,
                                    const MeshLODOptions& options) {
    auto* fullMesh = full->getDrawable(0)->asGeometry();
    const size_t fullFaces = faceCount(*fullMesh);
    // 目标=完整模型总面数*保留比例；至少以 4 面为目标，避免低面数输入要求接近零面。
    simplify(*coarse, std::max(4.0, fullFaces * options.triangleRatio));
    // 重新统计简化器的实际输出，不能把目标比例当作最终实际比例。
    const size_t reducedFaces = faceCount(*coarse->getDrawable(0)->asGeometry());
    full->setName(options.name + ".LOD0"); coarse->setName(options.name + ".LOD1");
    coarse->setUserValue("PereVia.TriangleCount", static_cast<unsigned>(reducedFaces));
    const auto& vertices = *static_cast<const osg::Vec3dArray*>(fullMesh->getVertexArray());
    // 从 LOD0 计算包围盒和中心。两张图使用相同的完整模型，
    // 避免简化层的采样误差被烘焙进最远一级的轮廓和颜色。
    Bounds bounds{vertices.front(), vertices.front(), {}};
    for (const auto& p : vertices) for (unsigned j = 0; j < 3; ++j) {
        bounds.low[j] = std::min(bounds.low[j], p[j]); bounds.high[j] = std::max(bounds.high[j], p[j]);
    }
    bounds.center = (bounds.low + bounds.high) * 0.5;
    auto topImage = bake(*fullMesh, bounds, options.resolution, true);
    auto sideImage = bake(*fullMesh, bounds, options.resolution, false);
    osg::ref_ptr<osg::Geode> top = new osg::Geode;
    top->setName(options.name + ".TopCard"); top->addDrawable(card(bounds, true, topImage.get()).get());
    // 俯视图固定在 XY 平面；侧视图交给 Billboard，仅绕局部 Z 轴转向相机。
    // 侧图拍摄方向固定为从 -Y 看向 +Y，因此未旋转卡片的正面法线设为 (0,-1,0)。
    osg::ref_ptr<osg::Billboard> side = new osg::Billboard;
    side->setName(options.name + ".SideCard");
    side->setMode(osg::Billboard::AXIAL_ROT); side->setAxis(osg::Vec3(0, 0, 1));
    side->setNormal(osg::Vec3(0, -1, 0)); side->addDrawable(card(bounds, false, sideImage.get()).get());
    // 两张卡片都常驻，但 ViewSelector 在剔除遍历中只提交一个子节点，
    // 因而 LOD2 每次实际绘制 2 个三角面，而非两张图叠加的 4 面。
    osg::ref_ptr<osg::Group> selector = new osg::Group;
    selector->addChild(top.get()); selector->addChild(side.get());
    selector->setCullCallback(new ViewSelector(options.topAngle, bounds.high.z() == bounds.low.z()));
    osg::ref_ptr<osg::MatrixTransform> impostor = new osg::MatrixTransform;
    impostor->setName(options.name + ".LOD2");
    // card 使用以零为中心的局部坐标，此处用 double 平移恢复模型在场景中的中心位置。
    impostor->setMatrix(osg::Matrixd::translate(bounds.center)); impostor->addChild(selector.get());
    osg::ref_ptr<osg::LOD> lod = new osg::LOD;
    lod->setName(options.name); lod->setRangeMode(osg::LOD::PIXEL_SIZE_ON_SCREEN);
    // 使用 LOD0 的固定球体，避免面片转动造成切换阈值变化。
    lod->setCenter(full->getBound().center()); lod->setRadius(full->getBound().radius());
    // PIXEL_SIZE_ON_SCREEN 使用屏幕投影尺寸（受相机 LODScale 影响），并非场景距离。
    // OSG 范围是 [min,max)：完整层 [阈值0,+∞)，简化层 [阈值1,阈值0)，
    // 面片层 [0,阈值1)，边界相接而不重叠，因此普通剔除遍历只选一级。
    lod->addChild(full, options.fullMinPixels, std::numeric_limits<float>::max());
    lod->addChild(coarse, options.coarseMinPixels, options.fullMinPixels);
    lod->addChild(impostor.get(), 0, options.coarseMinPixels);
    // 向宿主公开实际面数和 reduced/full 比例，便于统计；这些资源均在创建时生成并常驻，
    // LOD 只控制绘制选择，不自动卸载未显示级别的几何体或纹理。
    lod->setUserValue("PereVia.LOD0Triangles", static_cast<unsigned>(fullFaces));
    lod->setUserValue("PereVia.LOD1Triangles", static_cast<unsigned>(reducedFaces));
    lod->setUserValue("PereVia.LOD1ActualRatio", static_cast<double>(reducedFaces) / fullFaces);
    return lod;
}

// 保持岛屿原来的内部调用入口，实际与 TerrainBuilder 共用同一套装配和烘焙逻辑。
osg::ref_ptr<osg::Node> createIslandLOD(osg::Geode* full, osg::Geode* coarse,
                                      const IslandOptions& options) {
    return createMeshLOD(full, coarse, {options.lod1TriangleRatio, options.lod0MinPixels,
        options.lod1MinPixels, options.impostorResolution, options.impostorTopAngle, "PereVia.Island"});
}
}
