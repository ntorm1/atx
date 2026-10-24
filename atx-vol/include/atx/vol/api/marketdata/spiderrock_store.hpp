#pragma once

// spiderrock_store — read ONE bucket of the SpiderRock tblOptionIntradayHist oracle
// store (`<root>/date=<YYYY-MM-DD>/bucket_et=<HHMM>/*.parquet`, the layout
// scripts/oracle_ingest.py writes; 64-column schema pinned in
// scripts/oracle_store_metadata.py) into SoA columns, turn a bucket into a
// MarketStateStorage (per-uid scalars from uPrc / rate / sdiv / ddiv, per-contract
// ContractMarks from srVol, vendor price / delta for parity), and write a
// SpiderRock-SHAPED synthetic store so the loader, the engine and the benchmark run
// and are tested on a machine without the licensed drop.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see api/risk/risk_types.hpp banner).
// Lane C may add fields / functions ADDITIVELY and reports them.
//
// ## Conventions carried from bench/oracle/CONVENTIONS.md
//
//   spot   = uPrc (SpiderRock's option-implied underlying, per row; one uid uses the
//            MEDIAN over its rows in the bucket, and the per-row value is kept)
//   rate   = row rate (continuous), one per uid (median over rows)
//   q      = sdiv as a continuous yield (median over rows of the uid)
//   ddiv   = PV of cash dividends to expiry, PER ROW; escrowed as ONE cash event at
//            expiry_ns - 1 ns of amount ddiv (design D4) — a documented approximation
//            to the vendor's lattice; vendor_price lets the user see the gap
//   expiry = okey_yr/mn/dy at 16:00 ET (PM settlement) via expiry_instant_ns
//   years  = SpiderRock's hybrid vol-time year fraction: kept as `vendor_T`
//   sigma  = srVol (decimal). bidIV / askIV / error -99 sentinels are NULL in the
//            store: a row with a NULL srVol or a non-finite required input is
//            counted and dropped, never priced.
//
// Strings in the real store are large_utf8 (polars); both utf8 and large_utf8 are
// read (tools/oracle_cohort_reader.cpp rules).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/core/vol_time.hpp"      // TimeSpec
#include "atx/vol/api/risk/market_state.hpp"  // MarketStateStorage

namespace atx::vol {

// Stable uid assignment for underlier tickers across buckets of one run.
class UnderlierTable {
public:
  // Assigns the next uid (starting at 1) to an unseen ticker; returns the existing one otherwise.
  std::uint32_t intern(std::string_view ticker);
  [[nodiscard]] std::uint32_t find(std::string_view ticker) const noexcept; // 0 if absent
  [[nodiscard]] std::string_view ticker(std::uint32_t uid) const noexcept;  // "" if absent
  [[nodiscard]] std::size_t size() const noexcept { return tickers_.size(); }
  [[nodiscard]] std::span<const std::string> tickers() const noexcept { return tickers_; }

private:
  std::vector<std::string> tickers_; // index = uid - 1
};

struct SpiderRockStoreSpec {
  std::string store_root;              // the directory holding date=<d>/
  std::string date;                    // YYYY-MM-DD
  std::vector<std::string> underliers; // undSecKey_tk values; empty => all (then max_underliers)
  std::size_t max_underliers{0};       // 0 => no cap; else the top-N by row count in the bucket
  TimeSpec time{};                     // the clock T is measured on (Calendar365 default)
};

// One bucket decoded to SoA (rows sorted by (uid, expiry_ns, side, strike)).
struct SpiderRockBucket {
  std::string date;
  std::string bucket_et;      // "HHMM"
  std::int64_t ts_ns{0};      // the bucket's instant (date + HH:MM ET -> UTC)
  std::vector<std::uint32_t> uid;
  std::vector<double> strike;
  std::vector<std::int64_t> expiry_ns;
  std::vector<Side> side;
  std::vector<double> sigma;        // srVol
  std::vector<double> vendor_price; // srPrc
  std::vector<double> vendor_delta; // de
  std::vector<double> vendor_gamma; // ga
  std::vector<double> vendor_vega;  // ve
  std::vector<double> vendor_theta; // th
  std::vector<double> vendor_T;     // years
  std::vector<double> uprc;
  std::vector<double> rate;
  std::vector<double> sdiv;
  std::vector<double> ddiv;
  std::vector<double> bid;
  std::vector<double> ask;
  std::int64_t rows_read{0};
  std::int64_t rows_dropped_null{0};
  std::int64_t rows_dropped_bad_input{0};
  std::vector<std::string> files_opened;
  [[nodiscard]] std::size_t size() const noexcept { return sigma.size(); }
};

// List the `bucket_et=HHMM` partitions present for `date` (ascending). NotFound
// when the date directory is missing.
[[nodiscard]] Result<std::vector<std::string>> list_spiderrock_buckets(std::string_view store_root,
                                                                       std::string_view date);

// Read one bucket. Opens ONLY `<root>/date=<date>/bucket_et=<bucket>`; NotFound when
// it is missing or holds no parquet; propagates reader errors. `table` is updated
// with every ticker admitted (stable uids across calls).
[[nodiscard]] Result<SpiderRockBucket> load_spiderrock_bucket(const SpiderRockStoreSpec &spec,
                                                               std::string_view bucket_et,
                                                               UnderlierTable &table);

// Build the owning state for a bucket (conventions above). Rows of a uid with
// inconsistent (rate, sdiv) are tolerated (medians); a uid with no finite uPrc is
// dropped and counted in `n_uids_dropped`.
struct BucketStateReport {
  std::size_t n_uids{0};
  std::size_t n_uids_dropped{0};
  std::size_t n_marks{0};
};
[[nodiscard]] Result<MarketStateStorage> market_state_from_bucket(const SpiderRockBucket &bucket,
                                                                  const TimeSpec &time,
                                                                  BucketStateReport *report = nullptr);

// ── Synthetic SpiderRock-shaped store ───────────────────────────────────────────
//
// Writes `<out_dir>/date=<date>/bucket_et=<HHMM>/data.parquet` for every bucket in
// `buckets_et`, with the reader's column subset (undSecKey_tk, okey_cp, okey_xx,
// okey_yr/mn/dy, uPrc, rate, sdiv, ddiv, years, srVol, srPrc, de, ga, th, ve, bidIV,
// askIV, error, bidPrc, askPrc, atmVol) as large_utf8 / float64 / int64 exactly like
// the real store. The market MOVES across buckets deterministically from `seed`:
// per-underlier spot random walk (daily vol ~ `spot_vol`), eSSVI surface with level
// and skew shocks per bucket, time decay, so every explain axis is non-zero. srPrc /
// de / ga / th / ve are computed from the library's own American pricer at the
// generated sigma (so vendor-parity is an identity check in synthetic mode).
struct SyntheticSpiderRockSpec {
  std::string date{"2026-08-14"};
  std::vector<std::string> buckets_et{"1000", "1030", "1100", "1130", "1200", "1230",
                                      "1300", "1330", "1400", "1430", "1500"};
  std::size_t n_underliers{100};
  std::size_t n_expiries{8};        // from ~7 days to ~1 year
  std::size_t n_strikes{25};        // per expiry, both sides => rows = uids*exp*strikes*2
  double spot_vol{0.20};
  double level_shock_vol{0.01};     // per-bucket ATM vol shock (absolute, annualized sigma)
  double skew_shock_vol{0.002};
  std::uint64_t seed{1};
};
[[nodiscard]] Status write_synthetic_spiderrock_store(const SyntheticSpiderRockSpec &spec,
                                                      std::string_view out_dir);

} // namespace atx::vol
