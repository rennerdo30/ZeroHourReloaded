# ZeroHourReloaded SDL3 integration (cross-platform window + keyboard/mouse input).
# Replaces legacy DirectInput8 + Win32 CreateWindow/WndProc lifecycle on 64-bit Vulkan builds.

set(ZHR_SDL3_RUNTIME_DLL "" CACHE INTERNAL "SDL3 runtime DLL to stage next to executable")

if(ZHR_USE_SDL3)
    find_package(SDL3 QUIET)

    if(NOT SDL3_FOUND AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        include(FetchContent)
        FetchContent_Declare(
            zhr_sdl3_win64
            URL "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-devel-3.4.20-VC.zip"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_GetProperties(zhr_sdl3_win64)
        if(NOT zhr_sdl3_win64_POPULATED)
            message(STATUS "ZeroHourReloaded: Fetching official SDL3 3.4.20 win64 VC SDK...")
            FetchContent_Populate(zhr_sdl3_win64)
        endif()

        set(_sdl_root "${zhr_sdl3_win64_SOURCE_DIR}")
        if(NOT EXISTS "${_sdl_root}/include/SDL3/SDL.h")
            file(GLOB _sdl_subdirs LIST_DIRECTORIES true "${_sdl_root}/*")
            foreach(_d ${_sdl_subdirs})
                if(EXISTS "${_d}/include/SDL3/SDL.h")
                    set(_sdl_root "${_d}")
                    break()
                endif()
            endforeach()
        endif()

        if(EXISTS "${_sdl_root}/include/SDL3/SDL.h")
            add_library(SDL3::SDL3 SHARED IMPORTED GLOBAL)
            set_target_properties(SDL3::SDL3 PROPERTIES
                IMPORTED_IMPLIB "${_sdl_root}/lib/x64/SDL3.lib"
                IMPORTED_LOCATION "${_sdl_root}/lib/x64/SDL3.dll"
                INTERFACE_INCLUDE_DIRECTORIES "${_sdl_root}/include"
            )
            set(SDL3_FOUND TRUE)
            set(ZHR_SDL3_RUNTIME_DLL "${_sdl_root}/lib/x64/SDL3.dll" CACHE INTERNAL "" FORCE)
            message(STATUS "ZeroHourReloaded SDL3 enabled: root=${_sdl_root}")
        else()
            message(FATAL_ERROR "Failed to locate SDL3 headers in ${_sdl_root}")
        endif()
    elseif(NOT SDL3_FOUND)
        include(FetchContent)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            SDL3
            GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
            GIT_TAG "release-3.4.20"
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(SDL3)
        set(SDL3_FOUND TRUE)
    endif()
endif()
