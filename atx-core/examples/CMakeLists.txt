# atx-core examples (opt-in via ATX_BUILD_EXAMPLES). These link third-party clients
# directly and are NOT part of the default build or the test suite.

# Databento Historical API smoke test. Links the official client only (not atx-core).
add_executable(databento_aapl databento_aapl.cpp)
target_link_libraries(databento_aapl PRIVATE databento::databento)

# Free pre-flight cost estimator (Metadata endpoints only — no data egress).
add_executable(databento_cost databento_cost.cpp)
target_link_libraries(databento_cost PRIVATE databento::databento)

# Full XOM OPRA option-chain ohlcv-1m -> disk, cost-gated (OPRA is per-record billed).
add_executable(databento_opra_xom databento_opra_xom.cpp)
target_link_libraries(databento_opra_xom PRIVATE databento::databento)

# Full XOM OPRA chain L1/BBO snapshot (bbo-1m) -> disk, cost-gated.
add_executable(databento_xom_bbo databento_xom_bbo.cpp)
target_link_libraries(databento_xom_bbo PRIVATE databento::databento)
