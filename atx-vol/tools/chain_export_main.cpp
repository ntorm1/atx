// atx-vol-chain-export — per-contract fair value / fair vol / greeks to Parquet.
//
// Nothing else in atx-vol emits a per-CONTRACT valuation: the oracle bench
// publishes JSON aggregates and `atx-vol-surface-db-build` publishes fitted
// SURFACES. This tool closes that gap for ONE session: load the OPRA board, fit
// each symbol exactly the way the surface database says that symbol is fitted,
// price every listed leg, and write a `tblOptionIntradayHist`-shaped Parquet
// slice a downstream consumer can union with the SpiderRock store.
//
// DELIBERATELY ONE DATE AND AN EXPLICIT UNIVERSE. No sharding, no multi-date
// orchestration, no resume: those belong to a driver above this tool, and
// building them here before one session is demonstrably correct is how a
// backfill produces a million wrong rows quickly.
//
// ## What is ours and what is the vendor's
//
// Column names, physical types, the "Call"/"Put" spelling, the stamp formats and
// the -99 missing sentinel are the VENDOR's, so the two stores union (see
// tools/chain_export.hpp for where each was verified). The NUMBERS are ours:
// `srPrc`/`srVol`/the nine greeks come from `PricerFitter` on OUR board, under
// the greek convention scales pinned from the oracle sweep's winning map.
//
// TWO DIVERGENCES FROM THE VENDOR, stated rather than hidden:
//   * `undSecKey_tk` repeats `okey_tk`. Our OPRA hive's OSI-root namespace and
//     its `underlying` namespace COINCIDE (verified on the 2026-08-21 full
//     board: 6,189 distinct roots, 6,189 distinct underlyings, zero rows where
//     they differ) — SPXW is its own underlier there, not an SPX root. We carry
//     no root->underlier map, and inventing one would be a guess, so an SPXW row
//     says `undSecKey_tk = "SPXW"` where the vendor says `"SPX"`. The stderr
//     census counts the affected rows.
//   * `error` is ALWAYS the -99 sentinel. It is a vendor quality field whose
//     definition we do not have (it is identically 0 in the drop we hold), and
//     the census reports it as a whole-column refusal.
//
// ## ONE CLOCK WHERE THE VENDOR KEEPS TWO (`--time-convention voltime`)
//
// KNOWN AND ACCEPTED, measured rather than assumed. Do NOT "fix" this by adding
// a second clock; read the numbers first.
//
// `tblOptionIntradayHist` publishes BOTH `years` ("SpiderRock volatility time to
// expiration in years") AND `yearsC` ("Calendar time to expiration in years"),
// because only VARIANCE belongs on the vol-time clock — DISCOUNTING and carry
// belong on calendar time. Our board carries a SINGLE `Chain::T`, and
// `hybrid_forward_base(S, r, T, ...)` feeds that same T to `exp(r*T)` and to the
// borrow. So `--time-convention voltime` moves the discounting clock along with
// the variance clock, which is not what the vendor does.
//
// Why that is tolerable here, in the order the argument actually runs:
//   * The borrow is SOLVED from put-call parity using the SAME T, so the solved
//     borrow absorbs the clock difference and the FORWARD comes out right. Read
//     as the C-P parity zero crossing off each side's own `srPrc`, our forward
//     already matches the vendor's to -0.20 bp median over 319 expiries under
//     calendar T. The forward is self-correcting, not coincidentally close.
//   * What is left is pure discounting error, `exp(-r*dT)`. At 7 DTE that is
//     dT = 0.00217 yr at r = 0.043 — 0.9 bp of premium — and it shrinks toward
//     nothing as tenor grows. Sub-basis-point on the shortest tenor we publish.
//
// Where it COULD actually matter, and the reason this note exists rather than a
// silent acceptance: the AMERICAN early-exercise boundary. Exercise decisions
// turn on dividend timing in real CALENDAR days, so a boundary solved against a
// vol-time T is comparing a variance clock to a wall clock. Nothing measured has
// been attributed to it yet; it is the first place to look if a voltime run
// disagrees with the vendor by more than the discounting bound above.
//
// ## Exit codes
//
//   0  rows were emitted and written
//   1  a runtime failure (inputs unreadable, Parquet write failed)
//   2  a usage error (decided before any file is opened)
//   3  the run completed and produced NOTHING — every symbol failed or the
//      board was empty. The stderr census names which stage swallowed it.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/vol/api/core/chain.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/fitting/pricer_fitter.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/marketdata/opra_batch.hpp"
#include "atx/vol/api/marketdata/opra_hive.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/storage/surface_db.hpp"
#include "chain_export.hpp"

namespace {

namespace ce = atx::vol::chainexport;

using atx::vol::AmericanGreeks;
using atx::vol::CarryGreeks;
using atx::vol::ChainValuation;
using atx::vol::CorpusBoard;
using atx::vol::DividendEvent;
using atx::vol::FitPreset;
using atx::vol::OptionChain;
using atx::vol::OptionRef;
using atx::vol::OpraBatchResult;
using atx::vol::OpraHiveSpec;
using atx::vol::OutputField;
using atx::vol::PricerConfig;
using atx::vol::PricerFitter;
using atx::vol::SessionInputs;
using atx::vol::SurfaceDb;
using atx::vol::SurfacePurpose;
using atx::vol::SymbolFitConfig;
using atx::vol::TimeConvention;
using atx::vol::VolaSession;
using atx::vol::VolCurveKind;

using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

constexpr int kExitOk = 0;
constexpr int kExitRuntime = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNothingEmitted = 3;

// ── argv ────────────────────────────────────────────────────────────────────

struct Args {
  std::string hive_root;
  std::string underlier_root;
  std::string db_root;
  std::string date;
  std::string out_path;
  // SR-DIVS: optional Parquet discrete cash-dividend schedule (--dividends).
  // Empty = none supplied, which is the historical behaviour — every dividend
  // then folds into the solved continuous borrow.
  std::string dividends_path;
  std::string snapshot_suffix{"T19:55:00Z"};
  std::vector<std::string> symbols;
  double r{0.0};
  bool r_seen{false};
  unsigned fit_workers{0}; // 0 = auto (hardware concurrency)
  // The clock every chain's T is built on. Calendar365 is the historical
  // behavior and stays the default, so an existing command line is unchanged.
  TimeConvention time_convention{TimeConvention::Calendar365};
  // --pin-curve. Unset is the default and means today's routing: each symbol
  // keeps whatever family its stored SymbolFitConfig / the selector picks, and
  // the run stays bit-identical to one built before this flag existed.
  std::optional<VolCurveKind> pin_curve;
};

// Spelling of a convention for the CLI and the census. Exhaustive over the enum
// (no `default`), so a new convention fails the build here rather than printing
// a wrong provenance line.
[[nodiscard]] const char *time_convention_name(TimeConvention c) noexcept {
  switch (c) {
  case TimeConvention::Calendar365:
    return "calendar365";
  case TimeConvention::VolTime:
    return "voltime";
  }
  return "unknown";
}

void usage() {
  std::fputs(
      "atx-vol-chain-export — per-contract fair value / fair vol / greeks for one session\n"
      "\n"
      "usage:\n"
      "  atx-vol-chain-export --hive <root> --underlier <root> --db <surface-db-root>\n"
      "                       --date YYYY-MM-DD (--symbols A,B,C | --symbols-file <path>)\n"
      "                       --out <parquet> [--snapshot-suffix T19:55:00Z] [--r <rate>]\n"
      "                       [--fit-workers N] [--time-convention calendar365|voltime]\n"
      "                       [--dividends <parquet>] [--fit-workers N]\n"
      "                       [--fit-workers N]\n"
      "                       [--pin-curve <kind>]\n"
      "\n"
      "  --hive             OPRA hive v2 root holding date=<YYYY-MM-DD>/data.parquet\n"
      "  --underlier        underlier NBBO hive root holding\n"
      "                     date=<YYYY-MM-DD>/underlier.parquet\n"
      "  --db               SurfaceDb root; each symbol is refitted with its PERSISTED\n"
      "                     SymbolFitConfig. A symbol absent from the manifest falls back\n"
      "                     to the Populate preset and is counted.\n"
      "  --date             the single session to export\n"
      "  --symbols          comma-joined universe (mutually exclusive with --symbols-file)\n"
      "  --symbols-file     one symbol per line; '#' comments and blanks skipped\n"
      "  --snapshot-suffix  UTC stamp appended to --date (default T19:55:00Z; use\n"
      "                     T20:55:00Z on EST sessions)\n"
      "  --r                flat continuously-compounded fallback rate (required)\n"
      "  --dividends        Parquet discrete cash-dividend schedule; columns\n"
      "                     underlying:string, ex_date:string YYYY-MM-DD, amount:double.\n"
      "                     Omitted, every dividend folds into the solved borrow. A\n"
      "                     malformed or unreadable file is REFUSED, never read as empty.\n"
      "  --out              output Parquet path (parent dirs created)\n"
      "  --fit-workers      symbol-level workers; 0 = hardware concurrency (default)\n"
      "  --time-convention  clock every chain's T is built on (default calendar365):\n"
      "                     calendar365 = (expiry - snapshot)/365.25d;\n"
      "                     voltime     = SpiderRock's hybrid trading/non-trading\n"
      "                     clock. voltime is only defined over the 2024-2032\n"
      "                     holiday-calendar window (2024-2028 observed, 2029-2032\n"
      "                     rule-projected); a SESSION outside it fails that cell,\n"
      "                     an EXPIRY outside it drops that expiry alone. Both are\n"
      "                     counted in the census, never guessed. voltime also\n"
      "                     moves the DISCOUNTING clock, which the vendor keeps\n"
      "                     separate (years vs yearsC) — see the header note.\n"
      "  --pin-curve        force ONE curve family for EVERY symbol, overriding both the\n"
      "                     stored SymbolFitConfig and the Populate-preset fallback:\n"
      "                     convex-dense | essvi | svi | linear-variance | c8-event |\n"
      "                     spline-vol. Unset (default) keeps today's auto-routing.\n"
      "                     A pinned family is a REQUEST: the census reports per symbol\n"
      "                     which family was actually served.\n"
      "\n"
      "exit: 0 ok | 1 runtime failure | 2 usage | 3 ran but emitted nothing\n",
      stderr);
}

[[nodiscard]] bool parse_finite_double(std::string_view text, double &out) {
  const std::string s(text);
  char *end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) {
    return false;
  }
  out = v;
  return true;
}

[[nodiscard]] bool parse_count(std::string_view text, unsigned &out) {
  const std::string s(text);
  char *end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0' || v < 0 || v > 4096) {
    return false;
  }
  out = static_cast<unsigned>(v);
  return true;
}

// Map `--time-convention`'s value onto the enum. An unrecognised spelling is an
// `InvalidArgument` Err rather than a silent fall back to the default: which
// clock produced a chain is not something a typo may decide, and a run that
// silently ignored `--time-convention voltime` would publish calendar numbers
// under a vol-time label.
[[nodiscard]] Result<TimeConvention> parse_time_convention(std::string_view text) {
  if (text == "calendar365") {
    return TimeConvention::Calendar365;
  }
  if (text == "voltime") {
    return TimeConvention::VolTime;
  }
  return atx::core::Err(ErrorCode::InvalidArgument,
                        "--time-convention: expected calendar365|voltime, got '" +
                            std::string(text) + "'");
}

// Parse argv. Every diagnostic goes to stderr; `false` means exit 2, decided
// before any file is opened.
[[nodiscard]] bool parse_args(int argc, char **argv, Args &args) {
  bool symbols_seen = false;
  bool symbols_file_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag(argv[i]);
    const auto need_value = [&](std::string_view &out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %.*s requires a value\n", static_cast<int>(flag.size()),
                     flag.data());
        return false;
      }
      out = std::string_view(argv[++i]);
      return true;
    };
    std::string_view value;
    if (flag == "--help" || flag == "-h") {
      usage();
      return false;
    }
    if (flag == "--hive") {
      if (!need_value(value)) {
        return false;
      }
      args.hive_root = std::string(value);
    } else if (flag == "--underlier") {
      if (!need_value(value)) {
        return false;
      }
      args.underlier_root = std::string(value);
    } else if (flag == "--db") {
      if (!need_value(value)) {
        return false;
      }
      args.db_root = std::string(value);
    } else if (flag == "--date") {
      if (!need_value(value)) {
        return false;
      }
      args.date = std::string(value);
    } else if (flag == "--out") {
      if (!need_value(value)) {
        return false;
      }
      args.out_path = std::string(value);
    } else if (flag == "--dividends") {
      // SR-DIVS. Only the PATH is taken here; the file itself is opened in main
      // beside the other inputs, so a usage error still costs no file I/O.
      if (!need_value(value)) {
        return false;
      }
      args.dividends_path = std::string(value);
    } else if (flag == "--snapshot-suffix") {
      if (!need_value(value)) {
        return false;
      }
      args.snapshot_suffix = std::string(value);
    } else if (flag == "--symbols") {
      if (!need_value(value)) {
        return false;
      }
      if (symbols_file_seen) {
        std::fputs("error: --symbols and --symbols-file are mutually exclusive\n", stderr);
        return false;
      }
      symbols_seen = true;
      args.symbols = ce::parse_symbol_csv(value);
    } else if (flag == "--symbols-file") {
      if (!need_value(value)) {
        return false;
      }
      if (symbols_seen) {
        std::fputs("error: --symbols and --symbols-file are mutually exclusive\n", stderr);
        return false;
      }
      symbols_file_seen = true;
      if (!ce::read_symbol_file(std::string(value), args.symbols)) {
        std::fprintf(stderr, "error: --symbols-file: cannot read '%.*s'\n",
                     static_cast<int>(value.size()), value.data());
        return false;
      }
    } else if (flag == "--r") {
      if (!need_value(value)) {
        return false;
      }
      if (!parse_finite_double(value, args.r)) {
        std::fprintf(stderr, "error: --r: not a finite number: '%.*s'\n",
                     static_cast<int>(value.size()), value.data());
        return false;
      }
      args.r_seen = true;
    } else if (flag == "--fit-workers") {
      if (!need_value(value)) {
        return false;
      }
      if (!parse_count(value, args.fit_workers)) {
        std::fprintf(stderr, "error: --fit-workers: expected 0..4096, got '%.*s'\n",
                     static_cast<int>(value.size()), value.data());
        return false;
      }
    } else if (flag == "--time-convention") {
      if (!need_value(value)) {
        return false;
      }
      const Result<TimeConvention> conv = parse_time_convention(value);
      if (!conv.has_value()) {
        std::fprintf(stderr, "error: %s\n", std::string(conv.error().message()).c_str());
        return false;
      }
      args.time_convention = *conv;
    } else if (flag == "--pin-curve") {
      if (!need_value(value)) {
        return false;
      }
      // The ONE canonical spelling table (vol_curve.hpp), never a second one
      // here. `parse_curve_kind` answers InvalidArgument for anything it does
      // not recognise rather than defaulting, which is the whole reason it
      // exists: the ad-hoc chain it replaced folded every typo into ConvexDense,
      // so `--pin-kind essvii` silently fitted a 40-node dense curve.
      const Result<VolCurveKind> kind = atx::vol::parse_curve_kind(value);
      if (!kind.has_value()) {
        std::fprintf(stderr, "error: --pin-curve: %s\n",
                     std::string(kind.error().message()).c_str());
        return false;
      }
      args.pin_curve = *kind;
    } else {
      std::fprintf(stderr, "error: unknown flag '%.*s'\n", static_cast<int>(flag.size()),
                   flag.data());
      usage();
      return false;
    }
  }

  const auto require = [](const std::string &v, const char *name) {
    if (v.empty()) {
      std::fprintf(stderr, "error: %s is required\n", name);
      return false;
    }
    return true;
  };
  bool ok = require(args.hive_root, "--hive") && require(args.underlier_root, "--underlier") &&
            require(args.db_root, "--db") && require(args.date, "--date") &&
            require(args.out_path, "--out");
  if (!args.r_seen) {
    std::fputs("error: --r is required (there is no safe default rate)\n", stderr);
    ok = false;
  }
  if (args.symbols.empty()) {
    // An empty `OpraHiveSpec::symbols` is the hive's DISCOVERY switch, which
    // retains every date's whole table. This tool never wants that silently.
    std::fputs("error: a non-empty --symbols or --symbols-file is required\n", stderr);
    ok = false;
  }
  // The stamp validator doubles as the --date / --snapshot-suffix format check:
  // an empty result means one of the two is malformed.
  if (ok && ce::vendor_stamp(args.date, args.snapshot_suffix).empty()) {
    std::fprintf(stderr,
                 "error: --date must be YYYY-MM-DD and --snapshot-suffix THH:MM:SSZ "
                 "(got '%s' / '%s')\n",
                 args.date.c_str(), args.snapshot_suffix.c_str());
    ok = false;
  }
  if (!ok) {
    usage();
  }
  return ok;
}

// ── The underlier NBBO feed ─────────────────────────────────────────────────

using UnderlierBook = std::unordered_map<std::string, ce::NbboQuote>;

// Read `<root>/date=<date>/underlier.parquet` into a ticker -> NBBO map.
//
// Same fixed-point convention as the OPRA hive: prices are int64 1e-9 dollars
// and INT64_MIN is UNSET. An unset side is left at 0.0, which
// `resolve_underlier` rejects as unusable — the row is present but says nothing.
[[nodiscard]] Result<UnderlierBook> load_underlier_book(const std::string &root,
                                                        const std::string &date) {
  const std::string path = root + "/date=" + date + "/underlier.parquet";
  constexpr std::array<std::string_view, 3> kCols{"underlying", "bid_px", "ask_px"};
  Result<atx::core::io::ParquetTable> table = atx::core::io::read_parquet(path, kCols);
  if (!table.has_value()) {
    return atx::core::Err(table.error().code(),
                          "underlier feed '" + path + "': " + std::string(table.error().message()));
  }
  ATX_TRY(const std::vector<std::string_view> tickers, table->strings("underlying"));
  ATX_TRY(const std::span<const std::int64_t> bid_px,
          table->column_view<std::int64_t>("bid_px"));
  ATX_TRY(const std::span<const std::int64_t> ask_px,
          table->column_view<std::int64_t>("ask_px"));
  if (tickers.size() != bid_px.size() || tickers.size() != ask_px.size()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "underlier feed '" + path + "': column length mismatch");
  }

  constexpr double kPxScale = 1.0e-9;
  const auto decode = [](std::int64_t px) noexcept {
    return px == std::numeric_limits<std::int64_t>::min() ? 0.0
                                                          : static_cast<double>(px) * kPxScale;
  };
  UnderlierBook book;
  book.reserve(tickers.size());
  for (std::size_t i = 0; i < tickers.size(); ++i) {
    book.insert_or_assign(std::string(tickers[i]),
                          ce::NbboQuote{.bid = decode(bid_px[i]), .ask = decode(ask_px[i])});
  }
  return atx::core::Ok(std::move(book));
}

// ── Per-symbol fit + valuation ──────────────────────────────────────────────

// Why one symbol produced no rows. Reported per reason so an operator can tell
// a sparse universe from a broken pipeline.
enum class DropReason : std::uint8_t {
  None = 0,
  CoverageHole,  // the date file is present and simply does not carry the symbol
  LoadFailed,    // the panel could not be built from the date file
  ChainFailed,   // the frame installed no usable underlying
  FitFailed,     // PricerFitter::fit returned an error
  ValueFailed,   // no surface could price the board
};

[[nodiscard]] const char *drop_reason_name(DropReason r) noexcept {
  switch (r) {
  case DropReason::None:
    return "none";
  case DropReason::CoverageHole:
    return "coverage_hole";
  case DropReason::LoadFailed:
    return "load_failed";
  case DropReason::ChainFailed:
    return "chain_failed";
  case DropReason::FitFailed:
    return "fit_failed";
  case DropReason::ValueFailed:
    return "value_failed";
  }
  return "unknown";
}

struct SymbolResult {
  std::string symbol;
  std::vector<ce::ExportRow> rows;
  // Rows this symbol contributed, kept AFTER `rows` is released to the writer:
  // the census is printed once the storage it describes is long gone.
  std::size_t n_rows_emitted{0};
  DropReason drop{DropReason::None};
  std::string detail;                 // the failing Error's message, when any
  bool config_from_db{false};         // false => the Populate-preset fallback
  bool priced_from_mark_fallback{false};
  bool risk_fit_rejected{false};      // fit() erred but a market mark was served
  bool index_namespace{false};        // okey_tk is a cash-settled index root
  std::size_t n_carry_solve_failed{0};
  // Expiries the time convention could not resolve, so their rows were dropped
  // and the REST of this symbol's board kept (opra_panel.hpp). Copied off the
  // panel before the board consumes it, for the same reason `n_rows_emitted` is:
  // the census runs long after that storage is gone.
  std::vector<std::string> uncovered_expiries;
  std::size_t n_uncovered_expiry_rows{0};
  // The family every SERVED slice actually used, and which surface served it.
  // Recorded separately from the pinned family because they can differ: a
  // pinned risk candidate that admission refuses is not substituted, it is
  // simply unserved, and the board is then priced off the mark arm. Empty
  // tally + the default purpose while nothing was served.
  ce::ServedCurveTally served_curves{};
  SurfacePurpose served_purpose{SurfacePurpose::Risk};
};

// Translate the PricerConfig-representable subset of a stored SymbolFitConfig,
// exactly as `surface_db_populate.cpp`'s `pricer_config_for_symbol` does. The
// fields PricerConfig cannot carry (band_k, al_override/al, calendar_repair,
// and the pinned calib mirror) ride the `session_overlay` below.
//
// ONE DELIBERATE DIFFERENCE from populate: populate arms its own quote-fidelity
// PUBLISH FLOOR (`populate_admission_policy`) and forces `score_parity` on so
// the gate can read its own evidence. That floor is a POPULATE policy, not part
// of the stored config, and it decides whether a fitted surface is published —
// not what the fit computes. An exporter that armed it would drop a whole
// symbol's rows over a publication verdict, so it is left at the default and
// `score_parity` is whatever the stored config says.
[[nodiscard]] PricerConfig pricer_config_for_symbol(const SymbolFitConfig &cfg,
                                                    unsigned inner_threads) {
  PricerConfig out;
  out.preset = cfg.preset;
  out.quality_mode = cfg.surface_policy.quality_mode;
  out.outputs = cfg.surface_policy.outputs;
  out.risk_admission = cfg.surface_policy.risk_admission;
  out.fallback = cfg.surface_policy.fallback;
  if (cfg.pin_curve) {
    out.curve = cfg.curve;
  }
  out.use_correction_cache = cfg.use_correction_cache;
  out.score_parity = cfg.score_parity;
  out.enforce_calendar_floor = cfg.enforce_calendar_floor;
  out.use_deam_cache_for_fit = cfg.use_deam_cache_for_fit;
  out.n_threads = inner_threads;
  out.fit_workers = inner_threads;
  return out;
}

[[nodiscard]] double finite_or_missing(double v) noexcept {
  return std::isfinite(v) ? v : ce::kMissingF64;
}

// Total discrete cash dividend the fit modelled between now and `expiry_ns`.
//
// This is `ddiv` as an INPUT statement, not a claim about the issuer: it reports
// the schedule the pricing actually consumed. An OPRA panel carries no dividend
// schedule today, so it is 0.0 and the whole carry sits in `sdiv` (the fitted
// effective yield `q_eff`) — which is the honest description of how we priced.
[[nodiscard]] double discrete_div_to_expiry(const std::vector<DividendEvent> &divs,
                                            std::int64_t now_ns, std::int64_t expiry_ns) noexcept {
  double total = 0.0;
  for (const DividendEvent &d : divs) {
    if (d.ex_date_ns > now_ns && d.ex_date_ns <= expiry_ns) {
      total += d.amount;
    }
  }
  return total;
}

struct BoardContext {
  std::string symbol;
  std::string date_stamp;    // "YYYY-MM-DD HH:MM:SS.000000"
  std::string trading_date;  // "YYYY-MM-DD"
  double parity_spot{0.0};
  const ce::NbboQuote *feed{nullptr};
  bool sizes_available{false};
};

// Build every row of one fitted board.
void emit_board_rows(const OptionChain &chain, const VolaSession &session,
                     const ChainValuation &valuation, const BoardContext &ctx,
                     SymbolResult &out) {
  const ce::UnderlierFields und =
      ce::resolve_underlier(ctx.symbol, ctx.feed, ctx.parity_spot);
  const double spot = chain.spot();
  const std::vector<DividendEvent> &divs = chain.env().cash_divs;
  const std::int64_t now_ns = chain.now_ns();
  const bool have_greeks = atx::vol::has(valuation.filled, OutputField::Greeks);

  out.rows.reserve(out.rows.size() + valuation.size());
  for (std::size_t i = 0; i < valuation.size(); ++i) {
    const Result<OptionRef> ref = chain.at(valuation.ids[i]);
    if (!ref.has_value()) {
      continue; // the id came from this chain; an unresolvable one is not a row
    }
    ce::ExportRow row;
    row.okey_tk = ctx.symbol;
    row.und_sec_key_tk = ctx.symbol;
    const ce::ExpiryYmd ymd = ce::expiry_ymd_from_ns(ref->expiry_ns);
    row.okey_yr = ymd.year;
    row.okey_mn = ymd.month;
    row.okey_dy = ymd.day;
    row.okey_xx = finite_or_missing(ref->strike);
    row.okey_cp = std::string(ce::side_label(ref->side));

    row.u_bid = und.u_bid;
    row.u_ask = und.u_ask;
    row.u_prc = und.u_prc;

    row.bid_prc = finite_or_missing(ref->bid);
    row.ask_prc = finite_or_missing(ref->ask);
    if (ctx.sizes_available) {
      row.bid_sz = static_cast<std::int64_t>(ref->bid_size);
      row.ask_sz = static_cast<std::int64_t>(ref->ask_size);
    }

    const double model_price = valuation.model_price.empty() ? ce::kMissingF64
                                                             : valuation.model_price[i];
    const double model_iv = valuation.model_iv.empty() ? ce::kMissingF64 : valuation.model_iv[i];
    row.sr_prc = finite_or_missing(model_price);
    row.sr_vol = finite_or_missing(model_iv);

    const double T = ref->T;
    const bool live = T > 0.0 && std::isfinite(T);
    // The SAME (rate, carry) pair `VolaSession::greeks` prices at — its
    // `interp_forward(T)` returns `query_rate_at(in_, T)` and the slice `q_eff`,
    // which are exactly what these two accessors expose (session.cpp). Resolved
    // once so the published `rate`/`sdiv` and the carry solve below cannot
    // disagree with the eight greeks the fitter already produced.
    const double rate_t = live ? session.rate_at(T) : ce::kMissingF64;
    const double q_eff_t = live ? session.q_eff_at(T) : ce::kMissingF64;
    row.years = finite_or_missing(T);
    if (live) {
      row.rate = finite_or_missing(rate_t);
      row.sdiv = finite_or_missing(q_eff_t);
      row.ddiv = finite_or_missing(discrete_div_to_expiry(divs, now_ns, ref->expiry_ns));
    }

    if (have_greeks && i < valuation.greeks.size() && ce::greeks_are_defined(model_iv)) {
      // The NINTH greek: `AmericanGreeks` carries no carry-rho, so dP/dq is a
      // separate solve on those same inputs. Using anything else here would make
      // `ph` inconsistent with the other eight.
      double dp_dq = std::numeric_limits<double>::quiet_NaN();
      if (live && spot > 0.0 && ref->strike > 0.0) {
        const Result<CarryGreeks> carry = atx::vol::american_carry_greeks_al(
            spot, ref->strike, T, model_iv, rate_t, q_eff_t, ref->side,
            session.inputs().deam.al_opts);
        if (carry.has_value()) {
          dp_dq = carry->dP_dq;
        } else {
          ++out.n_carry_solve_failed;
        }
      }
      row.greeks = ce::scale_greeks(valuation.greeks[i], dp_dq, ce::kProductionGreekScales);
    }

    row.date = ctx.date_stamp;
    row.timestamp = ctx.date_stamp;
    row.trading_date = ctx.trading_date;
    // `error` stays sentinel: see the divergence note at the top of this file.
    out.rows.push_back(std::move(row));
  }
}

// Fit ONE board and emit its rows. Pure w.r.t. shared state: it reads only its
// own entry, the (thread-safe, const) SurfaceDb manifest and the immutable feed
// book, and writes only its own result slot.
void export_symbol(atx::vol::OpraBatchEntry &entry, const SurfaceDb *db,
                   const UnderlierBook &feed, const SymbolFitConfig &fallback_cfg,
                   const std::optional<VolCurveKind> &pin_curve,
                   const std::string &date_stamp, unsigned inner_threads, SymbolResult &out) {
  out.symbol = entry.symbol;
  out.index_namespace = ce::is_cash_settled_index_root(entry.symbol);
  if (!entry.panel.has_value()) {
    out.drop = entry.coverage_hole ? DropReason::CoverageHole : DropReason::LoadFailed;
    out.detail = std::string(entry.panel.error().message());
    return;
  }

  const double parity_spot = entry.panel->implied_spot;
  const bool sizes_available = entry.panel->bid_size_available && entry.panel->ask_size_available;
  // Read BEFORE the board consumes the panel: the census outlives this storage.
  out.uncovered_expiries = entry.panel->uncovered_expiries;
  out.n_uncovered_expiry_rows = entry.panel->n_dropped_uncovered_expiry;
  CorpusBoard board =
      atx::vol::corpus_board_from_opra(entry.date, entry.symbol, std::move(entry.panel.value()));

  Result<OptionChain> chain = OptionChain::from_frame(board.frame, board.env);
  if (!chain.has_value()) {
    out.drop = DropReason::ChainFailed;
    out.detail = std::string(chain.error().message());
    return;
  }

  SymbolFitConfig cfg = fallback_cfg;
  if (db != nullptr) {
    const Result<SymbolFitConfig> stored = db->symbol_config(entry.symbol);
    if (stored.has_value()) {
      cfg = *stored;
      out.config_from_db = true;
    }
  }
  // AFTER the config is resolved, so --pin-curve reaches BOTH branches above.
  // Pinning only `fallback_cfg` would leave every symbol present in the
  // manifest auto-routing, and the run would silently mix two families.
  ce::apply_curve_pin(pin_curve, cfg);

  PricerFitter fitter(pricer_config_for_symbol(cfg, inner_threads));
  const Status fit_status =
      fitter.fit(*chain, [&cfg, inner_threads](SessionInputs &in) {
        atx::vol::apply_symbol_config(cfg, in);
        in.fit_workers = inner_threads;
      });
  if (!fit_status.has_value()) {
    // A v2 dual fit publishes its mark and its risk surface INDEPENDENTLY, so a
    // REJECTED risk surface (arb gates, a failed carry solve) can leave a
    // perfectly good market mark served while `fit()` still reports the risk
    // error. Measured on the 2026-08-21 10-name run: QQQ and IWM both landed
    // here, and dropping them cost ~4.5k rows over a publication verdict.
    // Pricing off the mark is a LOWER-GRADE answer, so it is taken only here,
    // it keeps the risk error as the row's `detail`, and the census counts it.
    out.detail = std::string(fit_status.error().message());
    if (fitter.market_mark_surface() == nullptr) {
      out.drop = DropReason::FitFailed;
      return;
    }
    out.risk_fit_rejected = true;
  }

  constexpr OutputField kFields =
      OutputField::ModelPrice | OutputField::ModelIV | OutputField::Greeks;
  // Default-purpose FIRST (fail-closed: a config requesting Risk is answered by
  // the admitted risk surface or not at all). A rejected risk surface is not a
  // reason to lose the whole board, so the mark interpolant is the explicit,
  // COUNTED second attempt rather than a silent substitution.
  Result<ChainValuation> valuation =
      atx::core::Err(ErrorCode::Unavailable, "risk surface rejected; mark attempted");
  const atx::vol::FittedSurface *surface = nullptr;
  if (!out.risk_fit_rejected) {
    valuation = fitter.value_chain(*chain, kFields, inner_threads);
    surface = fitter.surface();
  }
  if (!valuation.has_value()) {
    valuation = fitter.value_chain(*chain, kFields, SurfacePurpose::MarketMark, inner_threads);
    surface = fitter.market_mark_surface();
    if (valuation.has_value()) {
      out.priced_from_mark_fallback = true;
    }
  }
  if (!valuation.has_value() || surface == nullptr) {
    out.drop = DropReason::ValueFailed;
    out.detail = valuation.has_value() ? "no served surface to read carry from"
                                       : std::string(valuation.error().message());
    return;
  }

  out.served_purpose = surface->purpose();
  out.served_curves = ce::tally_served_curves(surface->session());

  const auto feed_it = feed.find(entry.symbol);
  const BoardContext ctx{.symbol = entry.symbol,
                         .date_stamp = date_stamp,
                         .trading_date = entry.date,
                         .parity_spot = parity_spot,
                         .feed = (feed_it == feed.end()) ? nullptr : &feed_it->second,
                         .sizes_available = sizes_available};
  emit_board_rows(*chain, surface->session(), *valuation, ctx, out);
}

// ── The ordered streaming write ─────────────────────────────────────────────
//
// Two properties have to hold at once and they pull against each other:
//
//   * the file must be BYTE-IDENTICAL for any `--fit-workers`, so row groups
//     have to land in ENTRY order (date-major x the caller's symbol order) and
//     never in completion order; and
//   * peak memory must not grow with the universe, so a board's rows have to be
//     released as soon as they are on disk — which means writing DURING the fit
//     rather than after it. Accumulating first is exactly the defect: on the
//     2026-08-21 full board that path held 1,636,354 assembled rows, then a
//     second SoA copy of them, then a whole-universe Arrow table, and died with
//     "build table: Out of memory" after 164 s having written nothing.
//
// So producers fit boards in whatever order they claim them and ONE consumer
// drains strictly in index order, writes each board as a row group and frees it.
// A producer that runs far ahead of the drain head is throttled at
// `kPendingRowBudget` instead of being allowed to buffer the universe behind one
// slow board.
//
// NO DEADLOCK, and the argument is short: a producer only ever blocks AFTER
// publishing the index it holds. So if every producer is blocked, every claimed
// index has been published; claims come from a monotonic counter, so the claimed
// set is a prefix; so the drain head is claimed and published, and the consumer
// runs. The consumer never blocks on anything but a not-yet-published head.
class OrderedBoardSink {
public:
  // ~125 MB of assembled rows at the measured ~500 B/row. Large enough that the
  // throttle never fires on a healthy run (the whole 2026-08-21 universe is
  // 1.6 M rows spread over 2,834 boards averaging 577), small enough that one
  // pathologically slow head board cannot turn into the old whole-universe
  // buffer.
  static constexpr std::size_t kPendingRowBudget = 1u << 18;

  OrderedBoardSink(std::vector<SymbolResult> &results, ce::ChainExportWriter &writer)
      : results_(results), writer_(writer), done_(results.size(), 0u) {}

  OrderedBoardSink(const OrderedBoardSink &) = delete;
  OrderedBoardSink &operator=(const OrderedBoardSink &) = delete;

  // PRODUCER: entry `i` is complete. Called exactly once per index, from any
  // worker thread. Blocks while the unwritten backlog is over budget.
  void publish(std::size_t i) {
    std::unique_lock<std::mutex> lock(mutex_);
    // Recorded before the drain takes the rows away: the census print reports
    // per-symbol row counts long after the rows themselves are gone.
    results_[i].n_rows_emitted = results_[i].rows.size();
    pending_rows_ += results_[i].n_rows_emitted;
    done_[i] = 1u;
    drainable_.notify_one();
    space_.wait(lock, [this] { return pending_rows_ <= kPendingRowBudget; });
  }

  // CONSUMER: write every entry in index order on the calling thread.
  //
  // Draining continues past a write failure — the rows still have to be taken
  // and released or the producers stay blocked on a backlog that never shrinks —
  // but nothing further is WRITTEN once one write has failed: a Parquet stream
  // that has faulted is not one to keep appending to.
  //
  // @return the FIRST write error, or Ok.
  [[nodiscard]] Status drain_all() {
    using clock = std::chrono::steady_clock;
    std::optional<atx::core::Error> first_error;
    for (std::size_t i = 0; i < results_.size(); ++i) {
      std::vector<ce::ExportRow> board;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        drainable_.wait(lock, [this, i] { return done_[i] != 0u; });
        board.swap(results_[i].rows); // taken OUT of the shared vector
      }
      if (!board.empty() && !first_error.has_value()) {
        const auto t0 = clock::now();
        const Status wrote = writer_.write_board(board);
        write_seconds_ += std::chrono::duration<double>(clock::now() - t0).count();
        if (!wrote.has_value()) {
          first_error = wrote.error();
        }
      }
      // Released BEFORE the backlog is decremented, not at end of scope: the
      // producers this is about to unblock must not see the budget freed while
      // the rows that filled it are still resident.
      board = std::vector<ce::ExportRow>{};
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_rows_ -= results_[i].n_rows_emitted;
      }
      space_.notify_all();
    }
    return first_error.has_value() ? atx::core::Err(*first_error) : atx::core::Ok();
  }

  // Seconds spent inside the Parquet writer. Not a share of wall time: the write
  // runs concurrently with the fits it is draining.
  [[nodiscard]] double write_seconds() const noexcept { return write_seconds_; }

private:
  std::vector<SymbolResult> &results_;
  ce::ChainExportWriter &writer_;
  std::mutex mutex_;
  std::condition_variable drainable_; // consumer waits for the head to be done
  std::condition_variable space_;     // producers wait for the backlog to drain
  std::vector<std::uint8_t> done_;    // guarded by mutex_
  std::size_t pending_rows_{0};       // guarded by mutex_
  double write_seconds_{0.0};         // consumer-only
};

// ── The census ──────────────────────────────────────────────────────────────

[[nodiscard]] const char *purpose_name(SurfacePurpose p) noexcept {
  switch (p) {
  case SurfacePurpose::MarketMark:
    return "mark";
  case SurfacePurpose::Risk:
    return "risk";
  }
  return "unknown";
}

void print_census(const std::vector<SymbolResult> &results, const ce::ExportCensus &census,
                  const OpraBatchResult &batch, TimeConvention time_convention,
                  const ce::DividendCensus &divs, const std::optional<VolCurveKind> &pin_curve,
                  double load_s, double fit_s, double write_s) {

  std::size_t n_ok = 0;
  std::size_t n_from_db = 0;
  std::size_t n_mark_fallback = 0;
  std::size_t n_risk_rejected = 0;
  std::size_t n_index_ns = 0;
  std::size_t n_index_rows = 0;
  std::size_t n_carry_failed = 0;
  ce::ServedCurveTally served_all;
  std::size_t n_off_pin = 0; // symbols served with ANY slice off the pinned family
  // Sized off the enum so a new reason cannot silently overflow the histogram.
  std::array<std::size_t, static_cast<std::size_t>(DropReason::ValueFailed) + 1> by_reason{};
  for (const SymbolResult &r : results) {
    n_carry_failed += r.n_carry_solve_failed;
    served_all.merge(r.served_curves);
    if (pin_curve.has_value() && r.drop == DropReason::None &&
        r.served_curves.count(*pin_curve) != r.served_curves.total()) {
      ++n_off_pin;
    }
    // Counted for EVERY symbol: the config is resolved before the fit, so a
    // dropped symbol still answers "was this fitted the way the db says?".
    n_from_db += r.config_from_db ? 1u : 0u;
    if (r.drop == DropReason::None) {
      ++n_ok;
      n_mark_fallback += r.priced_from_mark_fallback ? 1u : 0u;
      n_risk_rejected += r.risk_fit_rejected ? 1u : 0u;
      if (r.index_namespace) {
        ++n_index_ns;
        n_index_rows += r.n_rows_emitted;
      }
    } else {
      ++by_reason[static_cast<std::size_t>(r.drop)];
    }
  }

  std::fprintf(stderr, "\n── chain-export census ─────────────────────────────────\n");
  // Provenance, not statistics: a chain parquet carries no column naming its
  // clock, so this line is the only record of which one produced the file.
  std::fprintf(stderr, "time convention         %s  (every Chain::T in this file)\n",
               time_convention_name(time_convention));
  std::fprintf(stderr, "rows written            %zu\n", census.rows);
  std::fprintf(stderr, "symbols requested       %zu\n", results.size());
  std::fprintf(stderr, "  fitted + priced       %zu\n", n_ok);
  std::fprintf(stderr, "  config from SurfaceDb %zu  (rest used the Populate-preset fallback)\n",
               n_from_db);
  std::fprintf(stderr, "  priced off mark       %zu  (risk surface unserved)\n", n_mark_fallback);
  std::fprintf(stderr, "  risk fit REJECTED     %zu  (mark-grade rows; reason below)\n",
               n_risk_rejected);
  for (const SymbolResult &r : results) {
    if (r.risk_fit_rejected) {
      std::fprintf(stderr, "    %-10s %s\n", r.symbol.c_str(), r.detail.c_str());
    }
  }
  // WHICH FAMILY PRODUCED THESE NUMBERS. `--pin-curve` is a request the fit can
  // refuse: a pinned risk candidate that admission rejects is not substituted
  // with another family (both fallback ladders are gated on an auto-routed fit),
  // it is left unserved and the board is priced off the mark arm instead. So the
  // pinned family and the served family are reported as two separate facts, and
  // `off pin` counts the symbols where they disagree.
  std::fprintf(stderr, "  curve pinned          %s\n",
               pin_curve.has_value() ? atx::vol::to_string(*pin_curve)
                                     : "(none -- stored config / auto-routed)");
  std::fprintf(stderr, "  curve SERVED          %s  (fitted slices, all served surfaces)\n",
               served_all.describe().c_str());
  if (pin_curve.has_value()) {
    std::fprintf(stderr, "  served off the pin    %zu symbols\n", n_off_pin);
  }
  for (const SymbolResult &r : results) {
    if (r.drop == DropReason::None) {
      std::fprintf(stderr, "    %-10s %-5s %s\n", r.symbol.c_str(),
                   purpose_name(r.served_purpose), r.served_curves.describe().c_str());
    }
  }
  for (std::size_t i = 1; i < by_reason.size(); ++i) {
    if (by_reason[i] > 0) {
      std::fprintf(stderr, "  dropped: %-13s %zu\n",
                   drop_reason_name(static_cast<DropReason>(i)), by_reason[i]);
    }
  }
  for (const SymbolResult &r : results) {
    if (r.drop != DropReason::None) {
      std::fprintf(stderr, "    %-10s %-14s %s\n", r.symbol.c_str(), drop_reason_name(r.drop),
                   r.detail.c_str());
    }
  }
  std::fprintf(stderr, "hive cells              loaded=%zu missing=%zu error=%zu holes=%zu\n",
               batch.n_loaded, batch.n_missing, batch.n_error, batch.n_coverage_holes);
  // Expiries the clock could not resolve, dropped with the rest of the board
  // kept. Printed unconditionally under voltime, INCLUDING the zero, because "no
  // expiry was dropped" and "this line was never reached" are two readings an
  // operator has to tell apart — and a silently-shrunk long end is precisely
  // what the drop would otherwise become. The batch counters cover every LOADED
  // cell; the per-symbol names come from `results`, which outlives the panels.
  if (time_convention == TimeConvention::VolTime) {
    std::fprintf(stderr,
                 "expiries out of clock   %zu rows over %zu of %zu loaded cells (that expiry "
                 "dropped, rest of the board kept)\n",
                 batch.n_uncovered_expiry_rows, batch.n_cells_with_uncovered_expiries,
                 batch.n_loaded);
    constexpr std::size_t kMaxNamedSymbols = 10;
    std::size_t named = 0;
    for (const SymbolResult &r : results) {
      if (r.uncovered_expiries.empty()) {
        continue;
      }
      if (named == kMaxNamedSymbols) {
        std::fputs("    ... (more symbols not listed)\n", stderr);
        break;
      }
      std::string list;
      for (const std::string &iso : r.uncovered_expiries) {
        if (!list.empty()) {
          list.push_back(' ');
        }
        list.append(iso);
      }
      std::fprintf(stderr, "    %-10s %zu rows  %s\n", r.symbol.c_str(),
                   r.n_uncovered_expiry_rows, list.c_str());
      ++named;
    }
  }
  // SR-DIVS. Two lines, because "no schedule was supplied" and "a schedule was
  // supplied and matched none of these symbols" produce IDENTICAL rows and mean
  // opposite things — the first is the old continuous-borrow behaviour on
  // purpose, the second is a symbol-spelling bug nobody would otherwise see.
  if (!divs.supplied) {
    std::fputs("discrete dividends      NONE supplied (--dividends omitted; every dividend "
               "folds into the solved borrow)\n",
               stderr);
  } else {
    std::fprintf(stderr, "discrete dividends      %zu loaded over %zu underliers from '%s'\n",
                 divs.events, divs.underliers, divs.path.c_str());
    std::fprintf(stderr,
                 "  schedule attached to  %zu of %zu requested symbols (%zu dividends); the "
                 "other %zu got an EMPTY schedule\n",
                 divs.symbols_matched, results.size(), divs.events_attached,
                 results.size() - std::min(divs.symbols_matched, results.size()));
  }
  std::fprintf(stderr, "carry-rho solves failed %zu  (ph sentinel on those rows)\n",
               n_carry_failed);
  std::fprintf(stderr,
               "index-namespace rows    %zu across %zu symbols  (undSecKey_tk repeats the OSI "
               "root; see the header note)\n",
               n_index_rows, n_index_ns);
  // The fit and the write OVERLAP (boards are written as they complete), so the
  // second figure is the wall clock of the whole produce+write phase and the
  // third is the time spent inside the Parquet writer, not a share of it.
  std::fprintf(stderr, "timings                 load=%.2fs fit+price+write=%.2fs (writer %.2fs)\n",
               load_s, fit_s, write_s);

  std::fprintf(stderr, "sentinels written (-99 / empty), per column:\n");
  std::size_t printed = 0;
  for (std::size_t c = 0; c < ce::kColumnCount; ++c) {
    const std::size_t n = census.sentinels[c];
    if (n == 0) {
      continue;
    }
    ++printed;
    const double pct = census.rows > 0 ? 100.0 * static_cast<double>(n) /
                                             static_cast<double>(census.rows)
                                       : 0.0;
    std::fprintf(stderr, "  %-14.*s %10zu  (%.2f%%)\n",
                 static_cast<int>(ce::kColumnNames[c].size()), ce::kColumnNames[c].data(), n, pct);
  }
  if (printed == 0) {
    std::fputs("  (none)\n", stderr);
  }
  std::fprintf(stderr, "sentinel total          %zu\n", census.sentinel_total());
  std::fprintf(stderr, "────────────────────────────────────────────────────────\n");
}

} // namespace

int main(int argc, char **argv) {
  using clock = std::chrono::steady_clock;
  const auto seconds_since = [](clock::time_point t0) {
    return std::chrono::duration<double>(clock::now() - t0).count();
  };

  Args args;
  if (!parse_args(argc, argv, args)) {
    return kExitUsage;
  }
  const std::string date_stamp = ce::vendor_stamp(args.date, args.snapshot_suffix);

  // Printed before ANY input is opened, so every run that gets past argv says
  // which clock it was asked for — the census at the end only reaches a run that
  // survived the loader.
  std::fprintf(stderr, "time convention: %s\n", time_convention_name(args.time_convention));
  // SR-DIVS: the discrete cash-dividend schedule, read FIRST — before the
  // underlier feed, the SurfaceDb and the board — because it is pure input
  // validation and a malformed file should cost no I/O at all. A defect here is
  // fatal by construction: `load_dividend_schedules` never returns an empty
  // schedule, since an empty one is indistinguishable from omitting the flag.
  ce::DividendCensus div_census;
  ce::DividendSchedules dividends;
  if (!args.dividends_path.empty()) {
    Result<ce::DividendSchedules> loaded = ce::load_dividend_schedules(args.dividends_path);
    if (!loaded.has_value()) {
      std::fprintf(stderr, "error: %s\n", std::string(loaded.error().message()).c_str());
      return kExitRuntime;
    }
    dividends = std::move(*loaded);
    div_census.supplied = true;
    div_census.path = args.dividends_path;
    div_census.underliers = dividends.size();
    for (const auto &[symbol, events] : dividends) {
      (void)symbol;
      div_census.events += events.size();
    }
    // Counted over the REQUESTED universe, not over the file: the reader's
    // question is "did MY symbols get a schedule", and a file covering names
    // this run never asked for answers it with a misleading yes.
    for (const std::string &symbol : args.symbols) {
      const auto it = dividends.find(symbol);
      if (it != dividends.end() && !it->second.empty()) {
        ++div_census.symbols_matched;
        div_census.events_attached += it->second.size();
      }
    }
  }

  const Result<UnderlierBook> feed = load_underlier_book(args.underlier_root, args.date);
  if (!feed.has_value()) {
    std::fprintf(stderr, "error: %s\n", std::string(feed.error().message()).c_str());
    return kExitRuntime;
  }
  std::fprintf(stderr, "underlier feed: %zu tickers\n", feed->size());

  Result<SurfaceDb> db = SurfaceDb::open(args.db_root);
  if (!db.has_value()) {
    std::fprintf(stderr, "error: --db '%s': %s\n", args.db_root.c_str(),
                 std::string(db.error().message()).c_str());
    return kExitRuntime;
  }

  OpraHiveSpec hive;
  hive.root_dir = args.hive_root;
  hive.date_lo = args.date;
  hive.date_hi = args.date;
  hive.symbols = args.symbols;
  hive.snapshot_suffix = args.snapshot_suffix;
  hive.r = args.r;
  // The ONE place this run's clock is chosen. Everything downstream — the
  // loader's own year-fractions, QuoteFrame::time, and the Chain::T that
  // data_install bakes from it — inherits it from here.
  hive.time.convention = args.time_convention;
  hive.cash_divs = std::move(dividends); // SR-DIVS; empty unless --dividends was given
  hive.n_threads = 1; // one date: the hive's fan-out is per-DATE, so it cannot help

  const auto t_load = clock::now();
  Result<OpraBatchResult> batch = atx::vol::load_opra_hive(hive);
  if (!batch.has_value()) {
    std::fprintf(stderr, "error: hive load: %s\n", std::string(batch.error().message()).c_str());
    return kExitRuntime;
  }
  const double load_s = seconds_since(t_load);
  std::fprintf(stderr, "hive: %zu cells (loaded=%zu) in %.2fs\n", batch->n_total, batch->n_loaded,
               load_s);

  const SymbolFitConfig fallback_cfg = atx::vol::symbol_config_from_preset(FitPreset::Populate);
  std::vector<SymbolResult> results(batch->entries.size());

  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const unsigned requested = (args.fit_workers == 0) ? hw : args.fit_workers;
  // One board cannot be split across symbol workers, so a single-symbol run puts
  // the whole budget INSIDE the fit instead. Either way the result is identical:
  // both `PricerFitter::fit` and `value_chain` are documented deterministic for
  // any thread count.
  const bool outer_parallel = batch->entries.size() > 1 && requested > 1;
  const unsigned outer_workers =
      outer_parallel ? std::min<unsigned>(requested,
                                          static_cast<unsigned>(batch->entries.size()))
                     : 1u;
  const unsigned inner_threads = outer_parallel ? 1u : requested;

  // Opened BEFORE the first fit: a bad --out path is a usage-grade mistake and
  // finding it after 164 s of fitting is exactly the failure mode this rewrite
  // exists to remove.
  ce::ChainExportWriter writer;
  if (const Status opened = writer.open(args.out_path); !opened.has_value()) {
    std::fprintf(stderr, "error: parquet open '%s': %s\n", args.out_path.c_str(),
                 std::string(opened.error().message()).c_str());
    return kExitRuntime;
  }

  // Producers fit; ONE consumer (this thread) writes each board in ENTRY order
  // and releases it. See OrderedBoardSink for why the two must be separated and
  // why it cannot deadlock.
  const auto t_fit = clock::now();
  OrderedBoardSink sink(results, writer);
  Status drained = atx::core::Ok();
  {
    std::atomic<std::size_t> next{0};
    const auto worker = [&]() {
      for (;;) {
        const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= batch->entries.size()) {
          return;
        }
        // A throw out of one board (bad_alloc under memory pressure is the live
        // case) must fail that CELL, not the run: the same rule
        // surface_db_populate applies to its fit workers.
        try {
          export_symbol(batch->entries[i], &db.value(), *feed, fallback_cfg, args.pin_curve,
                        date_stamp, inner_threads, results[i]);
        } catch (const std::exception &e) {
          results[i].symbol = batch->entries[i].symbol;
          results[i].drop = DropReason::FitFailed;
          results[i].detail = e.what();
        } catch (...) {
          results[i].symbol = batch->entries[i].symbol;
          results[i].drop = DropReason::FitFailed;
          results[i].detail = "unknown exception";
        }
        // Handed over on EVERY path, dropped boards included: the consumer waits
        // on each index in turn and a skipped publish would hang the drain.
        sink.publish(i);
      }
    };
    std::vector<std::jthread> pool;
    pool.reserve(outer_workers);
    for (unsigned w = 0; w < outer_workers; ++w) {
      pool.emplace_back(worker);
    }
    drained = sink.drain_all();
  } // the pool joins here: every board has been published and written
  const double fit_s = seconds_since(t_fit);
  const double write_s = sink.write_seconds();

  const Status closed = writer.close();
  const bool write_ok = drained.has_value() && closed.has_value();
  if (!drained.has_value()) {
    std::fprintf(stderr, "error: parquet write '%s': %s\n", args.out_path.c_str(),
                 std::string(drained.error().message()).c_str());
  } else if (!closed.has_value()) {
    std::fprintf(stderr, "error: parquet close '%s': %s\n", args.out_path.c_str(),
                 std::string(closed.error().message()).c_str());
  }

  const ce::ExportCensus &census = writer.census();
  print_census(results, census, *batch, args.time_convention, div_census, args.pin_curve,
               load_s, fit_s, write_s);

  // A run that emitted nothing, or one whose write failed, must leave NO file:
  // the stream was opened up front, so what is on disk is an empty or
  // footerless Parquet that a consumer would otherwise be free to read.
  if (census.rows == 0 || !write_ok) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path{args.out_path}, ec);
  }
  if (census.rows == 0) {
    std::fputs("error: the run produced no rows\n", stderr);
    return kExitNothingEmitted;
  }
  if (!write_ok) {
    return kExitRuntime;
  }
  std::fprintf(stderr, "wrote %zu rows in %lld row groups -> %s\n", census.rows,
               static_cast<long long>(writer.row_groups_written()), args.out_path.c_str());
  return kExitOk;
}
