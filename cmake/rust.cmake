# ZeroHourReloaded Rust FFI integration (zhr_ffi static library).
# Automatically inherits `.cargo/config.toml` (sccache, rust-lld, external target-dir).

option(ZHR_BUILD_RUST "Build ZeroHourReloaded Rust subsystems (zhr_ffi)" ON)

if(ZHR_BUILD_RUST)
    find_program(ZHR_CARGO_EXECUTABLE NAMES cargo)
    if(NOT ZHR_CARGO_EXECUTABLE)
        message(FATAL_ERROR "ZHR_BUILD_RUST is ON, but `cargo` was not found in PATH. Install Rust via https://rustup.rs or set -DZHR_BUILD_RUST=OFF.")
    endif()

    # Determine Cargo target triple matching CMake architecture on Windows.
    set(ZHR_CARGO_TARGET_ARGS "")
    if(WIN32 AND MSVC)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(ZHR_RUST_TARGET "x86_64-pc-windows-msvc")
        else()
            set(ZHR_RUST_TARGET "i686-pc-windows-msvc")
        endif()
        list(APPEND ZHR_CARGO_TARGET_ARGS --target "${ZHR_RUST_TARGET}")
    else()
        set(ZHR_RUST_TARGET "")
    endif()

    # Query cargo metadata once to respect machine-local `.cargo/config.toml` target-dir!
    execute_process(
        COMMAND "${ZHR_CARGO_EXECUTABLE}" metadata --no-deps --format-version 1
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _zhr_cargo_meta
        RESULT_VARIABLE _zhr_cargo_meta_res
    )
    if(NOT _zhr_cargo_meta_res EQUAL 0)
        message(FATAL_ERROR "Failed to run `cargo metadata` in ${CMAKE_SOURCE_DIR}")
    endif()
    string(JSON ZHR_CARGO_TARGET_DIR GET "${_zhr_cargo_meta}" "target_directory")
    cmake_path(CONVERT "${ZHR_CARGO_TARGET_DIR}" TO_CMAKE_PATH_LIST ZHR_CARGO_TARGET_DIR NORMALIZE)

    set(_zhr_target_subdir "")
    if(ZHR_RUST_TARGET)
        set(_zhr_target_subdir "/${ZHR_RUST_TARGET}")
    endif()

    set(_zhr_profile_flag "$<IF:$<CONFIG:Debug>,--profile=dev,$<IF:$<CONFIG:RelWithDebInfo>,--profile=release-fast,--release>>")

    if(WIN32 AND MSVC)
        set(ZHR_FFI_LIB_NAME "zhr_ffi.lib")
    else()
        set(ZHR_FFI_LIB_NAME "libzhr_ffi.a")
    endif()

    set(ZHR_FFI_LIB_BASE "${ZHR_CARGO_TARGET_DIR}${_zhr_target_subdir}")

    add_custom_target(zhr_ffi_cargo ALL
        COMMAND "${ZHR_CARGO_EXECUTABLE}" build -p zhr-ffi ${ZHR_CARGO_TARGET_ARGS} ${_zhr_profile_flag}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building Rust crate zhr-ffi (via cargo + sccache)"
        USES_TERMINAL
    )

    add_library(zhr_ffi STATIC IMPORTED GLOBAL)
    add_dependencies(zhr_ffi zhr_ffi_cargo)
    set_target_properties(zhr_ffi PROPERTIES
        IMPORTED_LOCATION "${ZHR_FFI_LIB_BASE}/release/${ZHR_FFI_LIB_NAME}"
        IMPORTED_LOCATION_DEBUG "${ZHR_FFI_LIB_BASE}/debug/${ZHR_FFI_LIB_NAME}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${ZHR_FFI_LIB_BASE}/release-fast/${ZHR_FFI_LIB_NAME}"
        IMPORTED_LOCATION_RELEASE "${ZHR_FFI_LIB_BASE}/release/${ZHR_FFI_LIB_NAME}"
        IMPORTED_LOCATION_MINSIZEREL "${ZHR_FFI_LIB_BASE}/release/${ZHR_FFI_LIB_NAME}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/rust/zhr-ffi/include"
    )

    if(WIN32)
        target_link_libraries(zhr_ffi INTERFACE ws2_32 userenv ntdll bcrypt advapi32)
    elseif(APPLE)
        target_link_libraries(zhr_ffi INTERFACE "-framework Security" "-framework CoreFoundation")
    else()
        target_link_libraries(zhr_ffi INTERFACE pthread dl m)
    endif()

    message(STATUS "ZeroHourReloaded Rust FFI enabled: cargo=${ZHR_CARGO_EXECUTABLE}, target_dir=${ZHR_CARGO_TARGET_DIR}")
endif()
