include(CMakeFindDependencyMacro)
find_dependency(OpenSceneGraph 3.6.5 COMPONENTS osg osgGA osgViewer osgUtil)
include("${CMAKE_CURRENT_LIST_DIR}/OsgDependency.cmake")
if(NOT TARGET Pyrive::OsgKit)
    get_filename_component(_perevia_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_library(Pyrive::OsgKit SHARED IMPORTED)
    set_target_properties(Pyrive::OsgKit PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_perevia_prefix}/include"
        INTERFACE_COMPILE_FEATURES cxx_std_17
        INTERFACE_LINK_LIBRARIES "PereViaOsgDependency::OSG")
    include("${CMAKE_CURRENT_LIST_DIR}/PereViaOsg-debug.cmake" OPTIONAL)
    include("${CMAKE_CURRENT_LIST_DIR}/PereViaOsg-release.cmake" OPTIONAL)
    unset(_perevia_prefix)
endif()
