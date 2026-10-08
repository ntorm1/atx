# Protobuf's clang-only restrict annotation enters clang-cl's MSVC symbol mangling.
# The pinned MSVC-built vcpkg DLL exports the same methods without that annotation.
# Normalize only this macro in a generated include overlay, never the shared installation.
function(atx_server_protobuf_msvc_abi target)
    if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        return()
    endif()

    get_target_property(protobuf_include_roots protobuf::libprotobuf
        INTERFACE_INCLUDE_DIRECTORIES)
    set(protobuf_header_root "")
    foreach(include_root IN LISTS protobuf_include_roots)
        if(EXISTS "${include_root}/google/protobuf/port_def.inc")
            set(protobuf_header_root "${include_root}")
            break()
        endif()
    endforeach()
    if(NOT protobuf_header_root)
        message(FATAL_ERROR "atx-server: cannot locate protobuf headers for the MSVC ABI overlay")
    endif()

    set(protobuf_port "${protobuf_header_root}/google/protobuf/port_def.inc")
    set(protobuf_version "${protobuf_header_root}/google/protobuf/runtime_version.h")
    file(READ "${protobuf_version}" protobuf_version_text)
    string(REGEX MATCH "#define PROTOBUF_OSS_VERSION ([0-9]+)"
        protobuf_version_match "${protobuf_version_text}")
    if(NOT protobuf_version_match OR NOT CMAKE_MATCH_1 STREQUAL "6033004")
        message(FATAL_ERROR
            "atx-server: protobuf MSVC ABI overlay is verified for C++ runtime 6.33.4 only; "
            "review exported restrict-qualified symbols before updating the pinned dependency")
    endif()

    file(READ "${protobuf_port}" protobuf_port_text)
    string(REPLACE "\r\n" "\n" protobuf_port_text "${protobuf_port_text}")
    set(restrict_original [=[#if defined( __clang__) || defined(__GNUC__)
#define PROTOBUF_RESTRICT __restrict
#else
#define PROTOBUF_RESTRICT
#endif]=])
    string(FIND "${protobuf_port_text}" "${restrict_original}" restrict_position)
    if(restrict_position LESS 0)
        message(FATAL_ERROR
            "atx-server: pinned protobuf restrict macro changed; review the MSVC ABI overlay")
    endif()
    set(restrict_compatible [=[#if (defined(__clang__) || defined(__GNUC__)) && \
    !(defined(_MSC_VER) && defined(__clang__))
#define PROTOBUF_RESTRICT __restrict
#else
#define PROTOBUF_RESTRICT
#endif]=])
    string(REPLACE "${restrict_original}" "${restrict_compatible}"
        protobuf_compatible_text "${protobuf_port_text}")

    set(overlay_root "${CMAKE_CURRENT_BINARY_DIR}/protobuf-msvc-abi")
    file(MAKE_DIRECTORY "${overlay_root}/google/protobuf")
    # configure_file preserves the output timestamp when the patched content is unchanged.
    file(WRITE "${overlay_root}/port_def.inc.in" "${protobuf_compatible_text}")
    configure_file("${overlay_root}/port_def.inc.in"
        "${overlay_root}/google/protobuf/port_def.inc" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${protobuf_port}" "${protobuf_version}")
    target_include_directories(${target} SYSTEM BEFORE PUBLIC
        "$<BUILD_INTERFACE:${overlay_root}>")
endfunction()
