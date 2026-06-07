# atx-core examples (opt-in via ATX_BUILD_EXAMPLES). These link third-party clients
# directly and are NOT part of the default build or the test suite.

# Databento Historical API smoke test. Links the official client only (not atx-core).
add_executable(databento_aapl databento_aapl.cpp)
target_link_libraries(databento_aapl PRIVATE databento::databento)
