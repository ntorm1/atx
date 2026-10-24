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
//   ddiv   = PV of cash dividends to expiry, PER ROW. SpiderRock publishes ONE
//            (rate, sdiv, ddiv) tuple per EXPIRY, so the state carries one
//            ExpiryCarry{expiry_ns, pv_divs, rate, q} per (uid, expiry) from the
//            medians over that expiry's rows (design D4 revised): the forward
//            F = (S - pv_divs) e^{(r - q) T} reproduces the vendor's forward basis
//            per expiry; `cash_divs` stays EMPTY. The remaining gap to the vendor's
//            discrete-dividend lattice is visible through vendor_price.
//   expiry = okey_yr/mn/dy at 16:00 ET (PM settlement) via expiry_instant_ns
//   years  = SpiderRock's hybrid vol-time year fraction: kept as `vendor_T`
//   sigma  = srVol (decimal). bidIV / askIV / error -99 sentinels are NULL in the
//            store: a row with a NULL srVol or a non-finite required input is
//            counted and dropped, never priced.
//
// Strings in the polars-written oracle store are large_utf8 and in the partitioned
// intraday drop (2026-10-14) plain utf8; both are read (tools/oracle_cohort_reader.cpp
// rules).
//
// Vendor Greek UNITS (CONVENTIONS.md resolved map): de / ga are per unit of spot,
// ve is per vol POINT (= dP/dsigma * kSpiderRockVegaScale), th is per business day
// (= dP/dt * kSpiderRockThetaScale, same sign as the library's calendar theta). The
// bucket keeps the vendor columns in vendor units; a parity consumer divides by the
// scale before comparing with library Greeks.

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
  // ── Lane C additions (appended) ──
  // Keep only exType == "American" rows when the partition has an `exType` column
  // (the licensed intraday drop does; the synthetic and older oracle stores do not,
  // and then every row is American). Applied BEFORE the top-N row count, so
  // European / Forward-framework index products (SPX, NDX, XSP, RUT, ...) never enter
  // a top-N universe: the risk kernel is American-on-spot and would fail vendor parity
  // on them by construction. Dropped rows are counted in `dropped_non_american`.
  bool american_only{true};
  // Keep only rows whose option root `okey_tk` equals the underlier ticker when the
  // partition has an `okey_tk` column. A different root is an adjusted / non-standard
  // deliverable: its (expiry, side, strike) key collides with the standard contract's
  // (bucket 1000 of 2026-10-14: 16,355 colliding keys, all distinct roots) and its
  // price is not a function of the underlier's spot. Applied before the top-N count;
  // counted in `dropped_nonstandard_root`. A NULL root passes.
  bool standard_roots_only{true};
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
  // ── Lane C additions (appended) ──
  // Rows of underliers outside the admitted set (explicit list / top-N cap),
  // including whole row groups pruned by the ticker statistics; not part of
  // rows_dropped_*: they were never candidates. Every row of the partition is
  // counted once: rows_read == size() + rows_dropped_null + rows_dropped_bad_input
  // + rows_not_selected + dropped_non_american (a NULL ticker counts as
  // rows_dropped_null; a pruned row group counts as rows_not_selected).
  std::int64_t rows_not_selected{0};
  // Rows dropped by SpiderRockStoreSpec::american_only (exType != "American",
  // including a NULL exType), counted before underlier admission.
  std::int64_t dropped_non_american{0};
  // KEPT rows with at least one optional vendor / carry cell (srPrc, de, ga, th, ve,
  // sdiv, ddiv, bidPrc, askPrc) NULL or a vendor sentinel (-99 anywhere, sdiv <=
  // -9.98), carried as NaN. A sentinel in a required column drops the row as
  // rows_dropped_null. Not part of the rows_read identity (the rows are kept).
  std::int64_t rows_null_vendor{0};
  // Rows dropped by SpiderRockStoreSpec::standard_roots_only (okey_tk != the
  // underlier ticker), counted before underlier admission; part of the identity:
  // rows_read == size() + rows_dropped_null + rows_dropped_bad_input +
  //              rows_not_selected + dropped_non_american + dropped_nonstandard_root.
  std::int64_t dropped_nonstandard_root{0};
};

// Vendor-unit scales (see "Vendor Greek UNITS" above): ve = vega * kSpiderRockVegaScale,
// th = theta * kSpiderRockThetaScale (BUS_252 day).
inline constexpr double kSpiderRockVegaScale = 0.01;
inline constexpr double kSpiderRockThetaScale = 1.0 / 252.0;

// The bucket's instant: `date` + HH:MM US/Eastern -> UTC epoch ns, derived as
// expiry_instant_ns(date, Pm) - (16:00 - HH:MM) (exact: DST resolves at day
// granularity). InvalidArgument on a malformed date / bucket.
[[nodiscard]] Result<std::int64_t> spiderrock_bucket_instant_ns(std::string_view date,
                                                                std::string_view bucket_et);

// The effective continuous carry implied by one ExpiryCarry row (design D4):
// F = (spot - pv_divs) e^{(rate - q) T}, q_eff = rate - ln(F / spot) / T. NaN when
// T <= 0, spot <= 0 or pv_divs >= spot. The synthetic generator prices its vendor
// columns at exactly this carry.
[[nodiscard]] double spiderrock_q_eff(double spot, double pv_divs, double rate, double q,
                                      double T) noexcept;

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
  // ── Lane C additions (appended): the D4 dividend approximation, made visible ──
  std::size_t n_uids_with_divs{0};    // uids with any ExpiryCarry::pv_divs > 0
  std::size_t n_expiry_rows{0};       // ExpiryCarry rows written (one per (uid, expiry))
  double max_ddiv_spread{0.0};        // max over (uid, expiry) of max(ddiv) - min(ddiv)
  std::size_t n_duplicate_marks{0};   // exact duplicate (expiry, side, strike) rows dropped
  std::size_t n_rows_bad_sigma{0};    // rows with a non-finite / non-positive sigma skipped
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
  // ── Lane C additions (appended) ──
  unsigned n_threads{0};            // pricing fan-out degree (0 = executor auto)
};
[[nodiscard]] Status write_synthetic_spiderrock_store(const SyntheticSpiderRockSpec &spec,
                                                      std::string_view out_dir);

// What a synthetic write produced (the overload below fills it).
struct SyntheticStoreReport {
  std::size_t rows_written{0};
  std::size_t pricing_failures{0}; // rows whose vendor columns are NULL (pricer refused)
  std::size_t files_written{0};
  double seconds{0.0};             // wall clock of the whole write
  double pricing_seconds{0.0};     // of which: the fanned-out American pricing
};
[[nodiscard]] Status write_synthetic_spiderrock_store(const SyntheticSpiderRockSpec &spec,
                                                      std::string_view out_dir,
                                                      SyntheticStoreReport &report);

} // namespace atx::vol
