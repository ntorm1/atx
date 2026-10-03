add_library(atx-engine ${ATX_LIB_TYPE}
    src/engine.cpp
    src/book/claims_state.cpp
    src/book/replay.cpp
    src/book/security_transition.cpp
    # platform v8 R-6: target tracking (spo-v3's solver; ADMM on the factor structure)
    src/book/target_tracking.cpp
    src/factory/canonical.cpp
    src/factory/crossover.cpp
    src/factory/factory.cpp
    src/factory/fitness.cpp
    src/factory/execution_objective.cpp
    src/factory/execution_cash_claim.cpp
    src/factory/execution_stock_transition.cpp
    src/factory/ic_screen.cpp
    src/factory/genome.cpp
    src/factory/mutation.cpp
    src/factory/op_catalog.cpp
    src/factory/param_search.cpp
    src/factory/pool_view.cpp
    src/factory/research_driver.cpp
    src/factory/search_driver.cpp
    src/eval/cpcv.cpp
    src/eval/cpcv_date.cpp
    src/risk/constraints.cpp
    src/risk/cov_ewma.cpp
    src/risk/eigen_adjust.cpp
    src/risk/vol_regime.cpp
    src/risk/specific_risk.cpp
    src/eval/cross_section_ic.cpp
    src/eval/pbo.cpp
    src/eval/perf_metrics.cpp
    # breadth / effective-N instrumentation (S10-5)
    src/eval/breadth.cpp
    # p8-S5-3: the robustness battery (admission-time noise/sub-universe/
    # alt-neutralization/param-perturbation screening)
    src/eval/robustness_battery.cpp
    src/alpha/bytecode.cpp
    src/alpha/dag.cpp
    src/alpha/lexer.cpp
    src/alpha/oracle.cpp
    src/alpha/oracle_formulaic.cpp
    src/alpha/oracle_lit.cpp
    src/alpha/panel.cpp
    src/alpha/parser.cpp
    src/alpha/registry.cpp
    src/alpha/segment_panel.cpp
    src/alpha/streams.cpp
    src/alpha/typecheck.cpp
    src/alpha/unparse.cpp
    src/learn/elastic_net.cpp
    src/learn/ensemble.cpp
    src/learn/feature_matrix.cpp
    src/learn/panel_dataset.cpp
    src/learn/gbt.cpp
    src/learn/hmm.cpp
    src/learn/latent.cpp
    src/learn/learned_source.cpp
    src/learn/linear_alpha.cpp
    # live NN signal source + NN library bridge (p2 S5-5a — integration capstone)
    src/learn/nn_source.cpp
    src/learn/pipeline.cpp
    src/learn/sequence_features.cpp
    # sequence alphas (p2 S5-2b-ii — TCN + GRU-lite learned alphas over windows)
    src/learn/tcn_alpha.cpp
    # autoencoder statistical-factor alpha (p2 S5-3b — GKX latent-factor extractor)
    src/learn/autoencoder_alpha.cpp
    src/learn/train.cpp
    # nn substrate (p2 S5-2a — deterministic CPU NN framework + generic layers)
    src/learn/nn/layers.cpp
    src/learn/nn/loss.cpp
    src/learn/nn/optimizer.cpp
    src/learn/nn/trainer.cpp
    # nn sequence layers (p2 S5-2b-i — causal conv / TCN block / GRU-lite + MGU / pool)
    src/learn/nn/seq_layers.cpp
    # data / signal / feature / reference abstraction layer (p2 S6 — BYO-data)
    src/data/adapt_factor.cpp
    src/data/adapt_feature.cpp
    src/data/adapt_panel.cpp
    src/data/adapt_signal.cpp
    # split + dividend total-return adjustment (p3 S1-3)
    src/data/adjust.cpp
    src/data/align.cpp
    src/data/catalog.cpp
    src/data/catalog_report.cpp
    src/data/context.cpp
    # security-master ingestion -> PIT corporate-action Dataset (p3 S1-2)
    src/data/corporate_actions.cpp
    src/data/data_handler.cpp
    src/data/dataset.cpp
    # real-data end-to-end Panel assembly: databento ⋈ corp-actions ⋈ universe (p3 S1-5)
    src/data/real_panel.cpp
    src/data/shm_bar_feed.cpp
    # universe construction: market-cap / ADV / sector / PIT membership (p3 S1-4)
    src/data/universe.cpp
    src/data/point_in_time_universe.cpp
    # ORATS history loader: stream zip TSV -> per-date .seg partition (p3 S3-2)
    src/data/orats_history.cpp
    # ORATS history panel helpers: orats_total_return_close (p3 S3-3)
    src/data/history_panel.cpp
    src/data/panel_store.cpp
    # FINRA consolidated short-interest causal loader (p7 S2-1 / track-b B1)
    src/data/finra_short.cpp
    src/parallel/batch_eval.cpp
    src/parallel/parallel_run.cpp
    src/parallel/process_executor.cpp
    src/parallel/scheduler.cpp
    src/parallel/shm_segment.cpp
    src/parallel/workload_eval.cpp
    src/parallel/workload_mine.cpp
    src/parallel/workload_streams.cpp
    src/validation/bias_audit.cpp
    # conviction (S10-1) — continuous [0,1] confidence from DSR/PBO/stability/explainability
    src/combine/conviction.cpp
    # regime-conditioned combination (S10-3)
    src/combine/regime_combiner.cpp
    # crowding / capacity de-correlation (S10-4)
    src/combine/crowding.cpp
    # risk (p2 S8.8a — header/source split: heavy method bodies moved out of the
    # header-only risk module into the library to collapse the include fan-out).
    # NOT auto-globbed — list each src/risk/*.cpp explicitly.
    src/risk/factor_model.cpp
    src/risk/garleanu_pedersen.cpp
    src/risk/multi_horizon.cpp
    # fractional-Kelly sizing (S10-2)
    src/risk/kelly_sizing.cpp
    # fund (p2 S2 — multi-strategy meta-book)
    src/fund/meta_allocator.cpp
    src/fund/cross_sleeve_risk.cpp
    src/fund/netting.cpp
    src/fund/meta_book.cpp
    # regime (macro/regime data: offline CSV -> sealed segment + panel augmenter)
    src/regime/source_csv.cpp
    src/regime/align.cpp
    src/regime/loader.cpp
    src/regime/store.cpp
    src/regime/with_regime_fields.cpp
    # disk hive-parquet options store + quant (feat/disk-data-layer)
    src/data/disk.cpp
    src/quant/trading_calendar.cpp
    # quant-platform swarm lanes (2026-09-22): scaffolded TUs, one owner each.
    src/alpha/subtree_cache.cpp
    src/factory/rewrite.cpp
    src/factory/fidelity.cpp
    src/eval/trial_registry.cpp
    src/eval/trial_epoch.cpp
    src/data/exposure_panel.cpp
    src/data/price_exposure_provider.cpp
    src/data/fundamental_clock_artifact.cpp
    src/data/strategy_data.cpp
    # platform v8 H-3: role adapters lifted from the IC runner (DSL overlay panel, return guard)
    src/data/role_panel.cpp
    # platform v8 H-3: the research IC fitness of the signal-fitness search path (mining verb)
    src/factory/research_ic_fitness.cpp
    src/factory/objective_ic.cpp
    src/eval/multiple_testing.cpp
    src/eval/lockbox_audit.cpp
    src/combine/signal_combiner.cpp
    src/combine/signal_cube.cpp
    src/combine/orthogonalize.cpp
    src/combine/marginal_rank_ic.cpp
    src/risk/model_validation.cpp
    src/risk/qp_solver.cpp
    src/risk/qp_factor_admm.cpp
    src/risk/exposures.cpp
    src/risk/hybrid_factor_model.cpp
    src/risk/cost_terms.cpp
    src/book/replay_cost.cpp
    # v8 R-4/R-5: desired-target shaping kernels (hold band, capped pro rata redistribution)
    src/book/target_shaping.cpp
    # v8 R-8: ex-ante risk target (risk-target-v1: the aim's leverage from the gross-1 book's vol)
    src/book/risk_target.cpp
    src/cost/cost_surface.cpp
    src/cost/spread_estimators.cpp
    src/cost/borrow_tiers.cpp
    src/cost/fim_adjustment.cpp
    src/cost/modeled_inputs.cpp
)
add_library(atx::engine ALIAS atx-engine)

# JSON is private to these artifact implementations. Keep its include path
# source-local so adding this dependency does not invalidate every engine TU/PCH.
set_property(SOURCE src/combine/signal_cube.cpp src/eval/trial_epoch.cpp
    src/factory/objective_ic.cpp src/data/strategy_data.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")

target_include_directories(atx-engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-engine PUBLIC cxx_std_20)

# Opt-in PCH (dev speed): parse Eigen + atx-core vocabulary once, not once per TU.
# PRIVATE so it never leaks into consumers' include hygiene. See atx-engine/pch.hpp.
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine PRIVATE pch.hpp)
endif()

# Keep fast IC screening optimized in the iterative Debug tree without changing
# the engine-wide PCH, Debug ABI, precise FP defaults, or other source objects.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(SOURCE src/factory/ic_screen.cpp APPEND PROPERTY COMPILE_OPTIONS
        /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/factory/ic_screen.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    # v8 F-2: the marginal-IC read-out runs one residualisation (Eigen COD) and three rank
    # passes per candidate and date over a full role; /Od Eigen makes that a many-minute run.
    set_property(SOURCE src/combine/orthogonalize.cpp src/combine/marginal_rank_ic.cpp
        APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/combine/orthogonalize.cpp src/combine/marginal_rank_ic.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    # v8 R-6: the NAV replay solves one tracking problem per rebalance decision and book
    # (~1,500 solves of ~100 ADMM iterations over ~1,850 names per TRAIN cell).
    set_property(SOURCE src/book/target_tracking.cpp
        APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/book/target_tracking.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    # v8 H-3: the role adapters moved out of the IC runner TUs, which run /O2 in Debug; keep
    # the full-role return guard (four logs per cell) at the speed it had there.
    set_property(SOURCE src/data/role_panel.cpp APPEND PROPERTY COMPILE_OPTIONS
        /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/data/role_panel.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Engine builds on top of core; the warnings gate (§8) applies to engine TUs.
# atx::tsdb is PUBLIC: ShmBarFeed (a header in this target's public include tree)
# exposes atx::tsdb::SegmentReader in its interface, so consumers need it too.
target_link_libraries(atx-engine
    PUBLIC
        atx::core
        atx::tsdb
    PRIVATE
        atx_warnings
)

# The ORATS loader's writer pool uses std::thread; link the platform thread lib.
find_package(Threads REQUIRED)
target_link_libraries(atx-engine PUBLIC Threads::Threads)

# The dedicated multi-process executor worker (S7-3, §0.8). ProcessExecutor spawns
# this beside the test/host module; it lands in build/bin/ via the global
# CMAKE_RUNTIME_OUTPUT_DIRECTORY, so it sits next to the atx-engine-<group>-tests
# binaries.
add_executable(atx-shm-worker src/parallel/shm_worker_main.cpp)
target_link_libraries(atx-shm-worker PRIVATE atx::engine atx_warnings)

# Platform core migration slice 1 (docs/plans/2026-10-02-platform-core-migration.md): the engine's research
# field-builder layer as its own small library. It needs atx::core and header-only engine pieces only
# (research_window.hpp), never the atx-engine archive or its PCH, so an edit here rebuilds this library and its
# test target alone. EXCLUDE_FROM_ALL: no existing build compiles it unless a consumer asks for it.
add_library(atx-engine-research-fields ${ATX_LIB_TYPE} EXCLUDE_FROM_ALL
    src/research/fields/asof_series.cpp
    src/research/fields/clock.cpp
    src/research/fields/field_spec.cpp
    src/research/fields/field_stats.cpp
    src/research/fields/field_writer.cpp
    src/research/fields/file_io.cpp
    src/research/fields/finra_asof_field.cpp
    src/research/fields/finra_inputs.cpp
    src/research/fields/research_fields_cli.cpp
    src/research/fields/role_axes.cpp
    src/research/fields/role_rows.cpp
    src/research/fields/trailing_mean.cpp
    src/research/fields/volume_mean_field.cpp)
add_library(atx::engine-research-fields ALIAS atx-engine-research-fields)
target_include_directories(atx-engine-research-fields
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(atx-engine-research-fields PUBLIC cxx_std_20)
target_link_libraries(atx-engine-research-fields
    PUBLIC
        atx::core
    PRIVATE
        atx_warnings
        nlohmann_json::nlohmann_json)

# Slice 3: the field builder's executable (`atx-research-fields build --spec S --receipt R`), the
# engine path of prepare_research_fields.py --engine-fields. EXCLUDE_FROM_ALL like its library.
add_executable(atx-research-fields EXCLUDE_FROM_ALL src/research/fields/research_fields_main.cpp)
target_link_libraries(atx-research-fields PRIVATE atx::engine-research-fields atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(ATX_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Platform P9 B1 (DEC-7; migration plan section 1 `admission/`): the admission screen (screen_v4 on eval::hac,
# greedy redundancy, the PM7-35 sign predicate, the report-only traded-horizon columns) as its own small library.
# Like the fields library it needs atx::core and header-only engine pieces only (eval/hac.hpp,
# data/research_window.hpp), never the atx-engine archive or its PCH. EXCLUDE_FROM_ALL: nothing builds it unless
# asked. The executable screens a K-P9-4 factor series directory (`atx-research-admission screen ...`).
add_library(atx-engine-research-admission ${ATX_LIB_TYPE} EXCLUDE_FROM_ALL
    src/research/admission/admission_cli.cpp
    src/research/admission/admission_csv.cpp
    src/research/admission/screen.cpp
    src/research/admission/traded_horizon.cpp)
add_library(atx::engine-research-admission ALIAS atx-engine-research-admission)
target_include_directories(atx-engine-research-admission
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(atx-engine-research-admission PUBLIC cxx_std_20)
target_link_libraries(atx-engine-research-admission
    PUBLIC
        atx::core
    PRIVATE
        atx_warnings
        nlohmann_json::nlohmann_json)
add_executable(atx-research-admission EXCLUDE_FROM_ALL src/research/admission/admission_main.cpp)
target_link_libraries(atx-research-admission PRIVATE atx::engine-research-admission atx_warnings)
