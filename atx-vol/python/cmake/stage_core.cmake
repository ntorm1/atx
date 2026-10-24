# cmake -DATXVOL_STAGE_FROM=<dir> -DATXVOL_STAGE_TO=<dir> -P stage_core.cmake
#
# POST_BUILD step of the in-tree `atx-vol-python-core` target (ATX_VOL_PYTHON=ON).
# Copies the freshly linked `_core*.pyd` AND every DLL beside it into
# atx-vol/python/src/atxvol/, the one place the gated suite imports it from:
# `_ctest_pytest_driver.py`, `conftest.py` and test_fit.py's child interpreters
# all require `atxvol._core` to resolve inside this tree's `src`, and Python 3.8+
# resolves an extension's DLL imports from the extension's OWN directory, so the
# vcpkg Arrow/Parquet closure (copied beside the build-tree module by vcpkg's
# applocal POST_BUILD step, which runs before this one) has to travel with it.
# Both patterns are git-ignored (atx-vol/python/.gitignore), and scikit-build-core
# honours .gitignore when it collects `wheel.packages`, so a staged copy never
# leaks into a wheel. The ctest lane passes `--expect-core $<TARGET_FILE:...>` and
# the driver refuses a staged module whose bytes differ from that build output, so
# a stale stage (or one written by another build tree of this worktree) fails
# loudly instead of being tested.
if(NOT IS_DIRECTORY "${ATXVOL_STAGE_FROM}" OR NOT IS_DIRECTORY "${ATXVOL_STAGE_TO}")
    message(FATAL_ERROR
        "stage_core.cmake: ATXVOL_STAGE_FROM='${ATXVOL_STAGE_FROM}' and "
        "ATXVOL_STAGE_TO='${ATXVOL_STAGE_TO}' must both be existing directories")
endif()
file(GLOB _atxvol_stage_files LIST_DIRECTORIES false
    "${ATXVOL_STAGE_FROM}/_core*.pyd"
    "${ATXVOL_STAGE_FROM}/*.dll")
list(LENGTH _atxvol_stage_files _atxvol_stage_n)
if(_atxvol_stage_n EQUAL 0)
    message(FATAL_ERROR "stage_core.cmake: no _core*.pyd in ${ATXVOL_STAGE_FROM}")
endif()
foreach(_atxvol_f IN LISTS _atxvol_stage_files)
    get_filename_component(_atxvol_name "${_atxvol_f}" NAME)
    file(COPY_FILE "${_atxvol_f}" "${ATXVOL_STAGE_TO}/${_atxvol_name}" ONLY_IF_DIFFERENT)
endforeach()
message(STATUS "atxvol: staged ${_atxvol_stage_n} file(s) into ${ATXVOL_STAGE_TO}")
