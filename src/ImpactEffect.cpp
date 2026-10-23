#include "ImpactEffect.h"

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Image>
#include <osg/Texture2D>
#include <algorithm>
#include <cmath>
#include <random>

namespace Pyrive::OsgKit::Detail {
namespace {

// 在内存中生成柔边云团贴图。无外部图片文件，复制 DLL 即可使用。
osg::ref_ptr<osg::Texture2D> makeTexture(bool smoke) {
    constexpr int size = 64;
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->allocateImage(size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (x + 0.5f) * 2.0f / size - 1.0f;
            const float v = (y + 0.5f) * 2.0f / size - 1.0f;
            const float edge = std::max(0.0f, 1.0f - u * u - v * v);
            const float cloud = 0.73f + 0.16f * std::sin(u * 12 + v * 8) +
                0.11f * std::cos(u * 17 - v * 13);
            const float alpha = edge * edge * (smoke ? cloud : 1.0f);
            unsigned char* pixel = image->data(x, y);
            pixel[0] = pixel[1] = pixel[2] = static_cast<unsigned char>(255 * (smoke ? 0.8f + 0.2f * cloud : 1.0f));
            pixel[3] = static_cast<unsigned char>(255 * std::clamp(alpha, 0.0f, 1.0f));
        }
    }
    osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image.get());
    texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
    return texture;
}

osg::ref_ptr<osg::Billboard> makeBillboard(const char* name, osg::Texture2D* texture, bool additive) {
    osg::ref_ptr<osg::Billboard> billboard = new osg::Billboard;
    billboard->setName(name);
    billboard->setMode(osg::Billboard::POINT_ROT_EYE);
    billboard->setNormal(osg::Vec3(0, 0, 1)); // 精灵四边形位于局部 XY 平面。
    billboard->setDataVariance(osg::Object::DYNAMIC);
    auto* state = billboard->getOrCreateStateSet();
    state->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    state->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
    state->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
    // 火光用叠加混合增强亮度；烟雾用普通透明混合遮住后方颜色。
    state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA));
    state->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL, 0, 1, false));
    state->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    return billboard;
}

}

ImpactEffect::Sprite ImpactEffect::addSprite(osg::Billboard* billboard) {
    Sprite sprite;
    sprite.vertices = new osg::Vec3Array(4);
    sprite.colors = new osg::Vec4Array(1);
    osg::ref_ptr<osg::Vec2Array> texcoords = new osg::Vec2Array;
    texcoords->push_back(osg::Vec2(0, 0));
    texcoords->push_back(osg::Vec2(1, 0));
    texcoords->push_back(osg::Vec2(0, 1));
    texcoords->push_back(osg::Vec2(1, 1));
    sprite.geometry = new osg::Geometry;
    sprite.geometry->setVertexArray(sprite.vertices.get());
    sprite.geometry->setColorArray(sprite.colors.get(), osg::Array::BIND_OVERALL);
    sprite.geometry->setTexCoordArray(0, texcoords.get());
    sprite.geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    sprite.geometry->setUseDisplayList(false);
    sprite.geometry->setUseVertexBufferObjects(true);
    sprite.geometry->setDataVariance(osg::Object::DYNAMIC);
    billboard->addDrawable(sprite.geometry.get());
    return sprite;
}

void ImpactEffect::setSprite(Sprite& sprite, float radius, float angle, const osg::Vec4& color) {
    const float c = std::cos(angle), s = std::sin(angle);
    for (unsigned i = 0; i < 4; ++i) {
        const float x = (i % 2 == 0 ? -radius : radius);
        const float y = (i < 2 ? -radius : radius);
        (*sprite.vertices)[i].set(c * x - s * y, s * x + c * y, 0);
    }
    (*sprite.colors)[0] = color;
    sprite.vertices->dirty();
    sprite.colors->dirty();
    sprite.geometry->dirtyBound();
}

void ImpactEffect::create(osg::Group* parent, const ExplosionOptions& explosion, const SmokeOptions& smoke) {
    explosion_ = explosion;
    smoke_ = smoke;
    if (explosion.enabled) {
        auto texture = makeTexture(false);
        flashNode_ = makeBillboard("PereVia.ExplosionFlash", texture.get(), true);
        flash_ = addSprite(flashNode_.get());
        flashNode_->setPosition(0, osg::Vec3(0, 0, explosion.radius * 0.25f));
        fireNode_ = makeBillboard("PereVia.Fireball", texture.get(), true);
        for (unsigned i = 0; i < 8; ++i) {
            Sprite sprite = addSprite(fireNode_.get());
            sprite.angle = static_cast<float>(i) * 2.39996f;
            sprite.offset.set(std::cos(sprite.angle), std::sin(sprite.angle), 0.3f + (i % 3) * 0.3f);
            sprite.factor = 0.8f + (i % 3) * 0.15f;
            fireSprites_.push_back(sprite);
        }
        parent->addChild(flashNode_.get());
        parent->addChild(fireNode_.get());
    }
    if (smoke.enabled) {
        auto texture = makeTexture(true);
        smokeNode_ = makeBillboard("PereVia.Smoke", texture.get(), false);
        std::mt19937 random(73);
        std::uniform_real_distribution<float> sample(-1.0f, 1.0f);
        for (unsigned i = 0; i < smoke.count; ++i) {
            Sprite sprite = addSprite(smokeNode_.get());
            sprite.offset.set(sample(random), sample(random), 0.3f + 0.2f * sample(random));
            sprite.factor = 0.8f + 0.2f * sample(random);
            sprite.angle = sample(random) * 3.14159f;
            smokeSprites_.push_back(sprite);
        }
        parent->addChild(smokeNode_.get());
    }
    update(0.0);
}

void ImpactEffect::update(double age) {
    if (flashNode_) {
        const double flashDuration = std::min(0.16, explosion_.duration);
        const float flash = static_cast<float>(std::clamp(age / flashDuration, 0.0, 1.0));
        flashNode_->setNodeMask(age < flashDuration ? ~0u : 0u);
        setSprite(flash_, explosion_.radius * (0.4f + flash), 0,
            osg::Vec4(1, 0.95f, 0.7f, (1 - flash) * explosion_.color.a()));

        const float progress = static_cast<float>(std::clamp(age / explosion_.duration, 0.0, 1.0));
        fireNode_->setNodeMask(age < explosion_.duration ? ~0u : 0u);
        osg::Vec4 color = explosion_.color;
        const float white = std::max(0.0f, 1.0f - progress * 4);
        for (unsigned i = 0; i < 3; ++i) color[i] += (1 - color[i]) * white;
        color.a() *= (1 - progress) * (1 - progress);
        for (unsigned i = 0; i < fireSprites_.size(); ++i) {
            Sprite& sprite = fireSprites_[i];
            const float expansion = 1.0f - std::exp(-progress * 5);
            osg::Vec3 position = sprite.offset * (explosion_.radius * 0.5f * expansion);
            position.z() += explosion_.radius * 0.35f;
            fireNode_->setPosition(i, position);
            setSprite(sprite, explosion_.radius * sprite.factor * (0.25f + expansion * 0.5f),
                sprite.angle + progress * 0.3f, color);
        }
        flashNode_->dirtyBound();
        fireNode_->dirtyBound();
    }
    if (smokeNode_) {
        smokeNode_->setNodeMask(age < smoke_.duration ? ~0u : 0u);
        for (unsigned i = 0; i < smokeSprites_.size(); ++i) {
            Sprite& sprite = smokeSprites_[i];
            const double delay = std::min(0.4, smoke_.duration * 0.25) * i / smokeSprites_.size();
            const double localAge = std::max(0.0, age - delay);
            const float progress = static_cast<float>(std::clamp(localAge / (smoke_.duration - delay), 0.0, 1.0));
            osg::Vec4 color = smoke_.color;
            for (unsigned c = 0; c < 3; ++c) color[c] += (0.65f - color[c]) * progress * 0.5f;
            const float appear = static_cast<float>(std::clamp(localAge / 0.18, 0.0, 1.0));
            color.a() *= appear * (1 - progress) * (1 - progress);
            const float spread = smoke_.radius * (0.2f + progress * 1.8f);
            osg::Vec3 position = sprite.offset * spread;
            position.z() += static_cast<float>(smoke_.riseSpeed * localAge * sprite.factor);
            smokeNode_->setPosition(i, position);
            setSprite(sprite, smoke_.radius * sprite.factor * (0.35f + progress * 1.5f),
                sprite.angle + progress * 0.4f, color);
        }
        smokeNode_->dirtyBound();
    }
}

void ImpactEffect::clear() {
    flashNode_ = nullptr;
    fireNode_ = nullptr;
    smokeNode_ = nullptr;
    flash_ = Sprite{};
    fireSprites_.clear();
    smokeSprites_.clear();
}

}
