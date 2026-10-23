#pragma once

#include <PereVia/OsgKit.h>
#include <osg/Billboard>
#include <osg/Geometry>

namespace Pyrive::OsgKit::Detail {

// 仅供库内部使用：负责命中后的闪光、火球和烟雾，不负责碰撞与计时。
// ProjectileCallback 统一传入“命中后经过的秒数”，所有效果共用宿主时间。
class ImpactEffect {
public:
    void create(osg::Group* parent, const ExplosionOptions& explosion, const SmokeOptions& smoke);
    void update(double age);
    void clear();

private:
    struct Sprite {
        osg::ref_ptr<osg::Geometry> geometry;
        osg::ref_ptr<osg::Vec3Array> vertices;
        osg::ref_ptr<osg::Vec4Array> colors;
        osg::Vec3 offset;
        float factor = 1.0f;
        float angle = 0.0f;
    };
    static Sprite addSprite(osg::Billboard* billboard);
    static void setSprite(Sprite& sprite, float radius, float angle, const osg::Vec4& color);

    ExplosionOptions explosion_;
    SmokeOptions smoke_;
    osg::ref_ptr<osg::Billboard> flashNode_, fireNode_, smokeNode_;
    Sprite flash_;
    std::vector<Sprite> fireSprites_, smokeSprites_;
};

}
