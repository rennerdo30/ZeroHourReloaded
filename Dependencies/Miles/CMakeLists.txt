set(MILES_SRC
    mss/mss.h
    MilesLoader.cpp
    MilesLoader.h
)

add_library(deps_miles STATIC)

target_sources(deps_miles PRIVATE ${MILES_SRC})

target_include_directories(deps_miles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mss
)

target_link_libraries(deps_miles PRIVATE
    depsi_always # Gives force included headers. Link early.
    deps_utility
)

if(ZHR_BUILD_RUST)
    target_compile_definitions(deps_miles PRIVATE ZHR_USE_RUST_AUDIO=1)
    target_include_directories(deps_miles PRIVATE "${CMAKE_SOURCE_DIR}/rust/zhr-ffi/include")
endif()
