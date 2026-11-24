# Header-only DirectX 8 type definitions for 64-bit / Vulkan builds during incremental DX8Wrapper migration.
# Does NOT link 32-bit d3d8.lib or d3dx8.lib; links 64-bit Windows SDK dinput8 + dxguid for Win32 input.

FetchContent_Declare(
    dx8_headers
    GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
    GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
)

FetchContent_GetProperties(dx8_headers)
if(NOT dx8_headers_POPULATED)
    FetchContent_Populate(dx8_headers)
endif()

add_library(d3d8lib INTERFACE)
target_include_directories(d3d8lib INTERFACE "${dx8_headers_SOURCE_DIR}")
target_link_libraries(d3d8lib INTERFACE dinput8 dxguid)
target_compile_definitions(d3d8lib INTERFACE ZHR_USE_VULKAN=1)
