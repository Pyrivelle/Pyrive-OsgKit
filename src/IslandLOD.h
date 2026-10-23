#pragma once
#include <PereVia/OsgKit.h>
#include <string>

namespace Pyrive::OsgKit::Detail {
// 岛屿和高程地形共享的内部三层装配/烘焙参数，不暴露第三方实现给宿主。
struct MeshLODOptions {
    double triangleRatio;
    float fullMinPixels, coarseMinPixels;
    unsigned resolution;
    double topAngle;
    std::string name;
};
osg::ref_ptr<osg::Node> createMeshLOD(osg::Geode* full, osg::Geode* coarse,
    const MeshLODOptions& options);
osg::ref_ptr<osg::Node> createIslandLOD(osg::Geode* full, osg::Geode* coarse,
    const IslandOptions& options);
}
