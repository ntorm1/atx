#pragma once

// Cohort JSON + partition-pruned parquet IO for atx-vol-oracle-bench
// (bench/oracle/CHARTER.md stage 2).
//
// Cohort schema is pinned by bench/oracle/cohorts/README.md:
//   { "name", "dates": [YYYY-MM-DD...], "underliers": [tk...],
//     "buckets_et": [HHMM...], "notes" }
//
// PARTITION PRUNING BY CONSTRUCTION: read_cohort_rows() opens ONLY
// <store>/date=<d>/bucket_et=<b> for the cohort's dates x buckets — it never
// enumerates the store root, so a full-file scan is structurally impossible.
// Within each named partition dir every *.parquet file is scanned with the
// undSecKey_tk == <tk> predicate pushed down to parquet row-group statistics
// (groups whose [min, max] cannot contain the underlier are never read) plus
// an exact per-row filter; only the compared columns are decoded. The scan is
// direct Arrow (not atx-core LazyParquet) so BOTH utf8 and large_utf8 string
// columns read identically — polars, which writes the real store, emits
// large_utf8 (rationale in the .cpp banner).
//
// SENTINEL-NULL RULE: ingest turned SpiderRock's -99 sentinels into nulls on
// bidIV/askIV/error. A row with ANY of those null is COUNTED
// (rows_null_sentinel) and dropped — never priced. A row with a null or
// non-finite REQUIRED numeric input, or an unparseable okey_cp, is counted as
// rows_bad_input and dropped. All numeric store columns are Float64 (pinned by
// the stage-1 ingest lane) — nothing here assumes an integer physical type.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp" // Side, Result

namespace atx::vol::oracle {

struct Cohort {
  std::string name;
  std::vector<std::string> dates;      // YYYY-MM-DD, become date=<d> dirs
  std::vector<std::string> underliers; // undSecKey_tk row-filter values
  std::vector<std::string> buckets_et; // HHMM, become bucket_et=<b> dirs
  std::string notes;
};

// Strict parser for the README schema (ParseError on malformed JSON,
// InvalidArgument on schema violations: missing/empty required keys, a date
// not shaped YYYY-MM-DD, a bucket not 4 digits — dates/buckets become path
// components, so their shape is validated at this boundary). Unknown keys with
// scalar or flat-array values are tolerated; nested objects are rejected.
[[nodiscard]] Result<Cohort> parse_cohort_json(std::string_view text);
[[nodiscard]] Result<Cohort> load_cohort_json(const std::string &path);

// One admitted store row: SpiderRock's own inputs + oracle outputs, exactly
// the columns Mode A compares. All values validated finite by the reader.
struct OracleRow {
  std::string underlier;
  Side side = Side::Call;
  double strike = 0.0; // okey_xx
  double uprc = 0.0;
  double rate = 0.0;
  double sdiv = 0.0;
  double ddiv = 0.0;
  double years = 0.0;
  double sr_vol = 0.0; // srVol
  double sr_prc = 0.0; // srPrc
  double de = 0.0;
  double ga = 0.0;
  double th = 0.0;
  double ve = 0.0;
  double rh = 0.0;
  double ph = 0.0;
  double vo = 0.0;
  double va = 0.0;
  double de_decay = 0.0; // deDecay
  double bid_prc = 0.0;
  double ask_prc = 0.0;
};

struct CohortScan {
  std::vector<OracleRow> rows;
  // Exactly the cohort-named partition dirs, in dates x buckets_et order —
  // the no-full-scan evidence a caller (or test) can assert on.
  std::vector<std::string> partitions_opened;
  std::int64_t rows_null_sentinel = 0;
  std::int64_t rows_bad_input = 0;
};

// NotFound if a cohort-named partition dir is missing or holds no *.parquet
// file (a cohort that names data that does not exist is a caller error, not an
// empty result). Propagates parquet reader errors (missing column, IO).
[[nodiscard]] Result<CohortScan> read_cohort_rows(const Cohort &cohort,
                                                  std::string_view store_root);

} // namespace atx::vol::oracle
