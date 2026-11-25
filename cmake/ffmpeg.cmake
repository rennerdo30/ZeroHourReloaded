# ZeroHourReloaded FFmpeg integration (LGPL shared SDK on Windows x64, pkg-config on Linux/macOS).
# Replaces 32-bit proprietary binkw32.dll for all Zero Hour .bik campaign/briefing/intro videos.

set(ZHR_FFMPEG_RUNTIME_DLLS "" CACHE INTERNAL "FFmpeg runtime DLLs to stage next to executable")

if(RTS_BUILD_OPTION_FFMPEG)
    if(NOT FFMPEG_FOUND)
        find_package(FFMPEG QUIET)
    endif()

    if(NOT FFMPEG_FOUND AND NOT WIN32)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
            if(FFMPEG_FOUND)
                set(FFMPEG_LIBRARIES PkgConfig::FFMPEG)
            endif()
        endif()
    endif()

    if(NOT FFMPEG_FOUND AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        include(FetchContent)
        FetchContent_Declare(
            zhr_ffmpeg_win64
            URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_GetProperties(zhr_ffmpeg_win64)
        if(NOT zhr_ffmpeg_win64_POPULATED)
            message(STATUS "ZeroHourReloaded: Fetching official LGPL FFmpeg 8.1 win64-shared dev SDK...")
            FetchContent_Populate(zhr_ffmpeg_win64)
        endif()

        set(_ff_root "${zhr_ffmpeg_win64_SOURCE_DIR}")
        if(NOT EXISTS "${_ff_root}/include/libavcodec/avcodec.h")
            file(GLOB _ff_subdirs LIST_DIRECTORIES true "${_ff_root}/*")
            foreach(_d ${_ff_subdirs})
                if(EXISTS "${_d}/include/libavcodec/avcodec.h")
                    set(_ff_root "${_d}")
                    break()
                endif()
            endforeach()
        endif()

        if(EXISTS "${_ff_root}/include/libavcodec/avcodec.h")
            set(FFMPEG_FOUND TRUE)
            set(FFMPEG_INCLUDE_DIRS "${_ff_root}/include")
            set(FFMPEG_LIBRARY_DIRS "${_ff_root}/lib")
            set(FFMPEG_LIBRARIES
                "${_ff_root}/lib/avcodec.lib"
                "${_ff_root}/lib/avformat.lib"
                "${_ff_root}/lib/avutil.lib"
                "${_ff_root}/lib/swscale.lib"
            )
            file(GLOB _ff_dlls "${_ff_root}/bin/*.dll")
            set(ZHR_FFMPEG_RUNTIME_DLLS "${_ff_dlls}" CACHE INTERNAL "" FORCE)
            message(STATUS "ZeroHourReloaded FFmpeg enabled: root=${_ff_root}")
        else()
            message(FATAL_ERROR "Failed to locate FFmpeg headers in ${_ff_root}")
        endif()
    endif()
endif()
