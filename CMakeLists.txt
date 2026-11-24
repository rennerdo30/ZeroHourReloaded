cmake_minimum_required(VERSION 3.28)

# Use packagename_ROOT for FindPackage.
if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

# Disable default MSVC setting CRT type so we can set it ourselves.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Disable default MSVC warning level so we can set it ourselves.
if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif()

# Allow specifying MSVC debug configurations.
#if(POLICY CMP0141)
#    cmake_policy(SET CMP0141 NEW)
#endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

include(FeatureSummary)
include(CMakeDependentOption)

# We don't support in tree builds, so help people make the right choice.
if (CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "Building in-source is not supported! Create a build dir and remove ${CMAKE_SOURCE_DIR}/CMakeCache.txt")
endif()

# Define VCPKG feature before the project block
# since they need to be initialized before the toolchain is
include(cmake/config-vcpkg.cmake)

# Top level project, doesn't really affect anything.
project(genzh LANGUAGES C CXX)

# This file handles extra settings wanted/needed for different compilers.
include(cmake/compilers.cmake)

# Debug symbol stripping for Release builds (MinGW)
include(cmake/debug_strip.cmake)

include(FetchContent)

# MinGW-w64 specific configuration
if(MINGW)
    include(cmake/mingw.cmake)
endif()

# ZeroHourReloaded renderer backend selection:
# Default to native Rust Vulkan 1.3 on 64-bit / non-Windows; DX8 remains selectable on 32-bit Windows.
if(NOT WIN32 OR ${CMAKE_SIZEOF_VOID_P} EQUAL 8)
    set(_zhr_default_vulkan ON)
else()
    set(_zhr_default_vulkan OFF)
endif()
option(ZHR_USE_VULKAN "Use the Rust Vulkan 1.3 renderer backend instead of DX8" ${_zhr_default_vulkan})
add_feature_info(VulkanBackend ZHR_USE_VULKAN "Build with Rust Vulkan 1.3 renderer backend (zhr-vulkan)")

# Find/Add build dependencies and stubs shared by all projects
if((WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows") AND ${CMAKE_SIZEOF_VOID_P} EQUAL 4 AND NOT ZHR_USE_VULKAN)
    include(cmake/dx8.cmake)
elseif(WIN32 OR "${CMAKE_SYSTEM}" MATCHES "Windows")
    # 64-bit / Vulkan builds do not link 32-bit d3d8.lib / d3dx8.lib.
    # Provide Windows SDK dinput8/dxguid + header-only d3d8 struct definitions during DX8Wrapper migration.
    include(cmake/dx8-headers-only.cmake)
else()
    add_library(d3d8lib INTERFACE)
endif()

if (IS_VS6_BUILD)
    include(cmake/stlport.cmake)
else()
    add_library(stlport INTERFACE)
endif()

include(cmake/config.cmake)
include(cmake/gamespy.cmake)
if (IS_VS6_BUILD)
    add_library(gamemath INTERFACE)
else()
    include(cmake/gamemath.cmake)
endif()
include(cmake/lzhl.cmake)
include(cmake/stb.cmake)

if(NOT IS_VS6_BUILD)
    include(cmake/rust.cmake)
endif()

if(RTS_BUILD_OPTION_PROFILE_TRACY)
    include(cmake/tracy.cmake)
else()
    add_library(core_profile_tracy INTERFACE)
endif()

if(RTS_BUILD_OPTION_TESTS)
    # Must come before the subdirectories that add tests.
    enable_testing()
    include(cmake/googletest.cmake)
endif()

if(RTS_BUILD_OPTION_BENCHMARKS)
    include(cmake/googlebenchmark.cmake)
endif()

add_subdirectory(Dependencies)

add_subdirectory(resources)

add_subdirectory(Core)

# Add main build targets
if(RTS_BUILD_ZEROHOUR)
    add_subdirectory(GeneralsMD)
endif()

if(RTS_BUILD_GENERALS)
    add_subdirectory(Generals)
endif()

if(RTS_BUILD_OPTION_TESTS)
    add_subdirectory(Tests)
endif()

if(RTS_BUILD_OPTION_BENCHMARKS)
    add_subdirectory(Benchmarks)
endif()

feature_summary(WHAT ENABLED_FEATURES DESCRIPTION "Enabled features:")
feature_summary(WHAT DISABLED_FEATURES DESCRIPTION "Disabled features:")
