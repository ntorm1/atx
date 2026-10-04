#pragma once

// Descriptors of the group `cache` (K-P9-13, sql-design section 3.7), in table order.
// PRIVATE header: included only by cache_ops.cpp (and the schema gtests). Editing a
// descriptor changes the printed schema: the fixture
// atx-engine/tests/fixtures/research_store/schema/cache.json must change with it.

#include "atx/engine/research/store/rows_cache.hpp"
#include "research/store/detail/table.hpp"
#include "research/store/tables_common.hpp"

namespace atx::engine::research::store {

inline constexpr auto kRecordTable = table<RecordRow>(
    "record",
    TableOpts{.version = 1,
              .since = 1,
              .append_only = false,
              .volatile_table = false,
              .check = "(body IS NULL) <> (body_file IS NULL)"},
    col<Sql::Text>("root", &RecordRow::root, kKey),
    col<Sql::Text>("kind", &RecordRow::kind, kKey),
    col<Sql::Sha256>("key_sha256", &RecordRow::key_sha256, kKey),
    col<Sql::Json>("key", &RecordRow::key),
    col<Sql::Json>("body", &RecordRow::body),
    col<Sql::RelPath>("body_file", &RecordRow::body_file),
    col<Sql::Int>("body_bytes", &RecordRow::body_bytes),
    col<Sql::Sha256>("content_sha256", &RecordRow::content_sha256));

inline constexpr auto kIcSignalTable = table<IcSignalRow>(
    "ic_signal",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Sha256>("key_sha256", &IcSignalRow::key_sha256, kKey),
    col<Sql::Text>("schema", &IcSignalRow::schema),
    col<Sql::Sha256>("role_sha256", &IcSignalRow::role_sha256),
    col<Sql::Text>("id", &IcSignalRow::id),
    col<Sql::Sha256>("dsl_sha256", &IcSignalRow::dsl_sha256),
    col<Sql::Text>("vm_identity", &IcSignalRow::vm_identity),
    col<Sql::Text>("eval_mode", &IcSignalRow::eval_mode),
    col<Sql::RelPath>("payload", &IcSignalRow::payload),
    col<Sql::Sha256>("payload_sha256", &IcSignalRow::payload_sha256),
    col<Sql::Int>("payload_bytes", &IcSignalRow::payload_bytes),
    col<Sql::Json>("sidecar", &IcSignalRow::sidecar));

inline constexpr auto kIcResultTable = table<IcResultRow>(
    "ic_result",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::RelPath>("entry_dir", &IcResultRow::entry_dir, kKey),
    col<Sql::Text>("id", &IcResultRow::id, kKey),
    col<Sql::Sha256>("key_sha256", &IcResultRow::key_sha256),
    col<Sql::Json>("record", &IcResultRow::record),
    col<Sql::Sha256>("record_sha256", &IcResultRow::record_sha256));

inline constexpr auto kPairKeyTable = table<PairKeyRow>(
    "pair_key",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Sha256>("key_sha256", &PairKeyRow::key_sha256, kKey),
    col<Sql::Int>("version", &PairKeyRow::version),
    col<Sql::Text>("key_text", &PairKeyRow::key_text));

inline constexpr auto kPairStatTable = table<PairStatRow>(
    "pair_stat",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = false, .check = {}},
    col<Sql::Sha256>("key_sha256", &PairStatRow::key_sha256, kKey),
    col<Sql::Sha256>("payload_lo", &PairStatRow::payload_lo, kKey),
    col<Sql::Sha256>("payload_hi", &PairStatRow::payload_hi, kKey),
    col<Sql::U64>("sum_bits", &PairStatRow::sum_bits),
    col<Sql::Int>("dates", &PairStatRow::dates));

} // namespace atx::engine::research::store
