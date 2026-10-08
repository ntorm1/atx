# The stable C ABI avoids coupling the server to DuckDB's internal C++ ABI.
# Release assets are pinned to the version already used by the atx-db warehouse.
set(ATX_DUCKDB_ROOT "" CACHE PATH "Existing DuckDB 1.5.1 C API distribution (offline builds)")
if(NOT ATX_DUCKDB_ROOT)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        set(_duckdb_platform windows-amd64)
        set(_duckdb_sha b85febb52a7b2e6d6891fcfffd8685ea974012f952cdc8dadef9c81e281d730d)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
           CMAKE_SYSTEM_PROCESSOR MATCHES "^(amd64|x86_64|X86_64)$")
        set(_duckdb_platform linux-amd64)
        set(_duckdb_sha 21aec66a60eae1696270ba715a481ab066a88d99a62718d0577579ac1a7a4834)
    else()
        message(FATAL_ERROR "Set ATX_DUCKDB_ROOT to a DuckDB 1.5.1 C API distribution on this platform")
    endif()
    set(_duckdb_cache "${CMAKE_SOURCE_DIR}/deps/duckdb-1.5.1")
    set(_duckdb_archive "${_duckdb_cache}/libduckdb-${_duckdb_platform}.zip")
    set(ATX_DUCKDB_ROOT "${_duckdb_cache}/${_duckdb_platform}")
    file(MAKE_DIRECTORY "${_duckdb_cache}")
    if(NOT EXISTS "${_duckdb_archive}")
        file(DOWNLOAD
            "https://github.com/duckdb/duckdb/releases/download/v1.5.1/libduckdb-${_duckdb_platform}.zip"
            "${_duckdb_archive}" EXPECTED_HASH "SHA256=${_duckdb_sha}"
            TLS_VERIFY ON STATUS _duckdb_download_status)
        list(GET _duckdb_download_status 0 _duckdb_download_code)
        if(NOT _duckdb_download_code EQUAL 0)
            message(FATAL_ERROR "DuckDB download failed: ${_duckdb_download_status}")
        endif()
    endif()
    file(SHA256 "${_duckdb_archive}" _duckdb_actual_sha)
    if(NOT _duckdb_actual_sha STREQUAL _duckdb_sha)
        message(FATAL_ERROR "DuckDB archive checksum mismatch: ${_duckdb_archive}")
    endif()
    if(NOT EXISTS "${ATX_DUCKDB_ROOT}/duckdb.h")
        file(ARCHIVE_EXTRACT INPUT "${_duckdb_archive}" DESTINATION "${ATX_DUCKDB_ROOT}")
    endif()
endif()
if(NOT EXISTS "${ATX_DUCKDB_ROOT}/duckdb.h")
    message(FATAL_ERROR "DuckDB C API header missing: ${ATX_DUCKDB_ROOT}/duckdb.h")
endif()
add_library(atx_duckdb SHARED IMPORTED GLOBAL)
set_target_properties(atx_duckdb PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ATX_DUCKDB_ROOT}")
if(WIN32)
    set_target_properties(atx_duckdb PROPERTIES
        IMPORTED_IMPLIB "${ATX_DUCKDB_ROOT}/duckdb.lib"
        IMPORTED_LOCATION "${ATX_DUCKDB_ROOT}/duckdb.dll")
elseif(APPLE)
    set_target_properties(atx_duckdb PROPERTIES
        IMPORTED_LOCATION "${ATX_DUCKDB_ROOT}/libduckdb.dylib")
else()
    set_target_properties(atx_duckdb PROPERTIES
        IMPORTED_LOCATION "${ATX_DUCKDB_ROOT}/libduckdb.so")
endif()

function(atx_server_copy_duckdb target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:atx_duckdb>"
                "$<TARGET_FILE_DIR:${target}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../THIRD_PARTY_NOTICES.txt"
                "$<TARGET_FILE_DIR:${target}>/atx-server-THIRD_PARTY_NOTICES.txt"
            VERBATIM)
    endif()
endfunction()
