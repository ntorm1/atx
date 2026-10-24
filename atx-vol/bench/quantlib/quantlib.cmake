# Opt-in standalone comparator dependency, never part of shipping atx-vol.
# Unmodified source subset of the pinned official archive, no network at configure.
function(atx_add_quantlib_comparator)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_package(Boost 1.75 REQUIRED)
    set(_manifest "${CMAKE_CURRENT_SOURCE_DIR}/bench/quantlib/upstream.json")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/quantlib/quantlib_comparator.py"
        --verify-source "${ATX_VOL_PUBLIC_QUANTLIB_DIR}" --manifest "${_manifest}"
        RESULT_VARIABLE _verified OUTPUT_VARIABLE _verify_out ERROR_VARIABLE _verify_err)
    if(NOT _verified EQUAL 0)
        message(FATAL_ERROR "QuantLib source verification failed: ${_verify_out}${_verify_err}")
    endif()
    file(READ "${_manifest}" _data)
    string(JSON _count LENGTH "${_data}" selected_sources)
    math(EXPR _last "${_count} - 1")
    set(_sources "")
    foreach(_i RANGE 0 ${_last})
        string(JSON _file GET "${_data}" selected_sources ${_i} path)
        string(JSON _expected GET "${_data}" selected_sources ${_i} sha256)
        file(SHA256 "${ATX_VOL_PUBLIC_QUANTLIB_DIR}/${_file}" _actual)
        if(NOT _actual STREQUAL _expected)
            message(FATAL_ERROR "Changed QuantLib selected source: ${_file}")
        endif()
        list(APPEND _sources "${ATX_VOL_PUBLIC_QUANTLIB_DIR}/${_file}")
    endforeach()
    file(GLOB_RECURSE _tree CONFIGURE_DEPENDS "${ATX_VOL_PUBLIC_QUANTLIB_DIR}/ql/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_manifest}" "${ATX_VOL_PUBLIC_QUANTLIB_DIR}/LICENSE.TXT" ${_tree})

    # Upstream supported configuration, single-threaded benchmark, no sessions.
    set(PACKAGE_NAME QuantLib)
    set(PACKAGE_VERSION 1.45-dev)
    set(PACKAGE_STRING "QuantLib 1.45-dev")
    set(PACKAGE_TARNAME QuantLib-1.45-dev)
    set(PACKAGE_BUGREPORT "https://github.com/lballabio/QuantLib/issues/")
    set(QL_HAVE_CONFIG_H ON)
    set(QL_FASTER_LAZY_OBJECTS ON)
    set(QL_USE_STD_SHARED_PTR ON)
    foreach(_option QL_ENABLE_PARALLEL_UNIT_TEST_RUNNER QL_ENABLE_SESSIONS
        QL_ENABLE_THREAD_SAFE_OBSERVER_PATTERN QL_ENABLE_TRACING QL_ERROR_FUNCTIONS
        QL_ERROR_LINES QL_EXTRA_SAFETY_CHECKS QL_HIGH_RESOLUTION_DATE QL_THROW_IN_CYCLES
        QL_USE_INDEXED_COUPON QL_NULL_AS_FUNCTIONS QL_ENABLE_OPENMP)
        set(${_option} OFF)
    endforeach()
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/public-quantlib")
    configure_file("${ATX_VOL_PUBLIC_QUANTLIB_DIR}/ql/config.hpp.cfg" "${_generated}/ql/config.hpp" @ONLY)
    configure_file("${ATX_VOL_PUBLIC_QUANTLIB_DIR}/ql/qldefines.hpp.cfg" "${_generated}/ql/qldefines.hpp" @ONLY)
    add_library(atx-vol-public-quantlib STATIC ${_sources})
    target_compile_features(atx-vol-public-quantlib PUBLIC cxx_std_20)
    target_compile_definitions(atx-vol-public-quantlib PRIVATE QL_COMPILATION)
    target_compile_definitions(atx-vol-public-quantlib PUBLIC BOOST_ALL_NO_LIB)
    target_include_directories(atx-vol-public-quantlib SYSTEM PUBLIC
        "${_generated}" "${ATX_VOL_PUBLIC_QUANTLIB_DIR}" ${Boost_INCLUDE_DIRS})
    if(MSVC)
        target_compile_options(atx-vol-public-quantlib PRIVATE /Gy /Gw)
    endif()
    add_executable(atx-vol-quantlib-comparator bench/quantlib/quantlib_main.cpp)
    target_compile_definitions(atx-vol-quantlib-comparator PRIVATE
        "ATX_QUANTLIB_COMPARATOR_IPO=$<BOOL:${CMAKE_INTERPROCEDURAL_OPTIMIZATION}>")
    target_link_libraries(atx-vol-quantlib-comparator PRIVATE atx-vol-public-quantlib
        atx-vol-bench-clocks atx::vol atx::core atx_warnings nlohmann_json::nlohmann_json)
endfunction()

atx_add_quantlib_comparator()
