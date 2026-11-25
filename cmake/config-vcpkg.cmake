# Contains options for VCPKG features

if(NOT WIN32 OR CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_zhr_default_ffmpeg ON)
else()
    set(_zhr_default_ffmpeg OFF)
endif()

option(RTS_BUILD_OPTION_FFMPEG "Enable FFmpeg support" ${_zhr_default_ffmpeg})
if(RTS_BUILD_OPTION_FFMPEG)
    list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
endif()
