cmake_minimum_required(VERSION 3.21)
project(PereVia VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported configurations" FORCE)
set(CMAKE_SUPPRESS_REGENERATION ON)
find_package(OpenSceneGraph 3.6.5 REQUIRED COMPONENTS osg osgGA osgViewer osgUtil)
include(cmake/OsgDependency.cmake)

add_library(PereViaOsg SHARED
    src/Runtime.cpp
    src/Projectile.cpp
    src/ImpactEffect.cpp src/ImpactEffect.h
    include/PereVia/Export.h include/PereVia/OsgKit.h
)
add_library(PereVia::OsgKit ALIAS PereViaOsg)
target_compile_definitions(PereViaOsg PRIVATE PEREVIA_OSG_BUILD)
target_compile_features(PereViaOsg PUBLIC cxx_std_17)
target_include_directories(PereViaOsg PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(PereViaOsg PUBLIC PereViaOsgDependency::OSG)
set_target_properties(PereViaOsg PROPERTIES DEBUG_POSTFIX d)

add_executable(PereViaOsgTests EXCLUDE_FROM_ALL
    tests/Main.cpp tests/RuntimeTests.cpp tests/ProjectileTests.cpp
)
add_dependencies(PereViaOsgTests PereViaOsg)
target_compile_features(PereViaOsgTests PRIVATE cxx_std_17)
# Compile against the packaged public headers/import LIB, not private sources.
target_include_directories(PereViaOsgTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/dist/include")
target_link_libraries(PereViaOsgTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/lib/$<CONFIG>/PereViaOsg$<$<CONFIG:Debug>:d>.lib"
    PereViaOsgDependency::OSG)
set_target_properties(PereViaOsgTests PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD TRUE)
if(MSVC)
    foreach(target PereViaOsg PereViaOsgTests)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        set_target_properties(${target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endforeach()
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT PereViaOsgTests)
endif()
source_group("Implementation" FILES src/Runtime.cpp src/Projectile.cpp src/ImpactEffect.cpp src/ImpactEffect.h)
source_group("Public API" FILES include/PereVia/Export.h include/PereVia/OsgKit.h)
source_group("Tests" FILES tests/Main.cpp tests/RuntimeTests.cpp tests/ProjectileTests.cpp)
enable_testing()
add_test(NAME PereViaOsgRuntime COMMAND PereViaOsgTests --tests)

add_custom_command(TARGET PereViaOsg POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DDLL=$<TARGET_FILE:PereViaOsg>" "-DIMPLIB=$<TARGET_LINKER_FILE:PereViaOsg>"
        "-DPDB=$<TARGET_PDB_FILE:PereViaOsg>" "-DCONFIG=$<CONFIG>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DPACKAGE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/dist"
        "-DOSG_ROOT=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        "-DCOMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
        "-DPEREVIA_VERSION=${PROJECT_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/PackageSdk.cmake"
    VERBATIM
)
add_custom_command(TARGET PereViaOsgTests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/bin/$<CONFIG>" "$<TARGET_FILE_DIR:PereViaOsgTests>"
    VERBATIM
)
