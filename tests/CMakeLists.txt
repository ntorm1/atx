# Repo-wide test support, shared by EVERY test executable in the tree.
#
# FIX-3/F3-B. FIX-2/F2-A proved that a test process which resolves its scratch path
# through `std::filesystem::temp_directory_path()` shares that path with every other
# concurrently running test process on the machine, and that the near-universal
# `remove_all()`-on-entry in those fixtures makes the collision DESTRUCTIVE. F2-A fixed
# the class for atx-vol only, by listing `process_scratch.cpp` in that suite's source
# list. 124 fixture files across all five suites have the same shape, so the mechanism
# is hoisted here and linked by every test target instead.
#
# OBJECT, not STATIC, and this is load-bearing: `process_scratch.cpp` deliberately
# exports nothing a test references — it does its work in a namespace-scope dynamic
# initializer that must run before main(). A static archive member with no referenced
# symbol is dropped by the linker, which would remove the isolation with no diagnostic
# whatsoever. Linking an OBJECT library puts its objects on the executable's link line
# unconditionally, so the initializer is always present.
add_library(atx-test-scratch OBJECT support/process_scratch.cpp)
target_link_libraries(atx-test-scratch PRIVATE atx_warnings)
