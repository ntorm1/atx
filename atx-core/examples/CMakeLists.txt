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

# OFFLINE OPRA cbbo-1m DBN -> Parquet converter (pull_opra_cbbo_1m_to_parquet schema).
# No network / no cost. Links atx::core for io::write_parquet AND databento for the DBN read.
add_executable(opra_dbn_to_parquet opra_dbn_to_parquet.cpp)
target_link_libraries(opra_dbn_to_parquet PRIVATE atx::core databento::databento)

# Bulk OPRA cbbo-1m puller (P2-5): many symbols x date range -> per-(symbol,date) Parquet,
# cost-gated with a FREE preflight + --dry-run. Links atx::core (estimate_cost /
# pull_opra_cbbo_1m_to_parquet) AND databento for the Metadata preflight client.
add_executable(databento_bulk_opra databento_bulk_opra.cpp)
target_link_libraries(databento_bulk_opra PRIVATE atx::core databento::databento)
