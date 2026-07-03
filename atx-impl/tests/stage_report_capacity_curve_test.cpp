// stage_report_capacity_curve_test.cpp — S4-5b [OPT-IN, B9]: stage_report
// emits a first-class BOOK-LEVEL capacity curve -- the (AUM, net_edge_bps)
// sweep and its interpolated zero-crossing (cost::capacity_point) -- instead
// of only the scalar %ADV participation footprint (the existing 7b block,
// left untouched). Additive: absent volume data degenerates the new fields
// to documented inert sentinels and NEVER perturbs the report digest (the
// digest is computed over rep.* numeric series only -- see stage_report.cpp
// step 8 -- both before and after this unit).
//
// Load-bearing checks:
//   (a) CapacityCurveHandComputed -- a fully hand-computable 2-instrument, 5-
//       date fixture (independently verified via a Python oracle script, NOT
//       reusing any C++ code) proves book_gross_edge_bps, the curve's point
//       count, and the interpolated capacity_point_aum against known numbers.
//   (b) CapacityCurveMonotoneNonIncreasing -- the emitted curve's net_edge_bps
//       is non-increasing in aum (the capacity-model CONTRACT; also enforced
//       by cost::capacity_point's own ATX_CHECK, so a violation would abort).
//   (c) HigherVolatilityShiftsCapacityLower -- a higher-volatility variant of
//       the SAME book (bigger price swings, held prices/weights otherwise
//       comparable) realizes a STRICTLY LOWER capacity_point_aum -- the same
//       "one cost surface" sensitivity the spec's "larger Y" criterion probes
//       (sigma and Y enter the cost model identically, Y*sigma, so a fixture
//       lever on sigma is the equivalent, test-controllable knob given the
//       engine-default ImpactCfg{} is not threaded through a CLI flag).
//   (d) CapacityCurveAbsentWithoutVolume_DigestUnchanged -- a research panel
//       with NO "volume" field degenerates capacity_point_aum to +inf,
//       book_gross_edge_bps to 0.0, and an empty curve (0 points) -- AND the
//       report digest is BYTE-IDENTICAL to the SAME fixture WITH a volume
//       field added, proving the capacity-curve computation never reaches
//       the digest either way (the additive/inert contract, stronger than
//       the spec's literal ask since it holds on BOTH paths, not just the
//       absent one).

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx_impl_stage_report_capacity_curve {

namespace fs    = std::filesystem;
namespace alpha = atx::engine::alpha;

// ---------------------------------------------------------------------------
// Helpers (mirrors report_participation_test.cpp's conventions).
// ---------------------------------------------------------------------------

static std::string read_summary_value(const fs::path& summary_path, const std::string& key) {
    std::ifstream f(summary_path);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return "";
}

static bool kv_has_key(const std::vector<std::pair<std::string, std::string>>& kvs,
                       const std::string& key) {
    for (const auto& kv : kvs) {
        if (kv.first == key) return true;
    }
    return false;
}

struct Fixture {
    fs::path work_dir;
    std::string research_path;
    std::string books_path;

    explicit Fixture(const std::string& tag) {
        work_dir = fs::temp_directory_path() / ("atx_impl_s45b_" + tag);
        std::error_code ec;
        fs::remove_all(work_dir, ec);
        fs::create_directories(work_dir, ec);
        research_path = (work_dir / "research.bin").string();
        books_path    = (work_dir / "books.bin").string();
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(work_dir, ec);
    }
};

// 2-instrument, D=5-date research panel. `with_volume` controls whether the
// "volume" field is written at all (the natural "aum_grid supplied" stand-in
// this unit uses -- see sprint-4-progress.md for the documented deviation:
// config.hpp is Sprint-5-owned and cannot gain a new CLI flag this sprint).
static atx::core::Result<std::string>
make_research_panel(const fs::path& out, const std::vector<atx::f64>& closeA,
                    const std::vector<atx::f64>& closeB, atx::f64 volA, atx::f64 volB,
                    bool with_volume) {
    const atx::usize D = closeA.size();
    constexpr atx::usize M = 2;
    std::vector<atx::f64> raw_close_v(D * M);
    std::vector<atx::f64> close_v(D * M);
    std::vector<atx::f64> volume_v(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        raw_close_v[t * M + 0] = closeA[t];
        close_v[t * M + 0]     = closeA[t];
        volume_v[t * M + 0]    = volA;
        raw_close_v[t * M + 1] = closeB[t];
        close_v[t * M + 1]     = closeB[t];
        volume_v[t * M + 1]    = volB;
    }
    std::vector<std::uint8_t> uni(D * M, 1u);
    std::vector<std::string> fields{"raw_close", "close"};
    std::vector<std::vector<atx::f64>> cols{raw_close_v, close_v};
    if (with_volume) {
        fields.push_back("volume");
        cols.push_back(volume_v);
    }
    ATX_TRY(auto panel, alpha::Panel::create(D, M, fields, cols, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// A single rebalance period: weights [wA, wB] over the M=2 universe.
static atx::core::Result<std::string>
make_books_panel(const fs::path& out, atx::f64 wA, atx::f64 wB) {
    constexpr atx::usize S = 1;
    constexpr atx::usize M = 2;
    std::vector<atx::f64> wv{wA, wB};
    std::vector<std::uint8_t> uni(S * M, 1u);
    ATX_TRY(auto panel, alpha::Panel::create(S, M, {"weight"}, {wv}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

static void write_books_meta(const fs::path& books_path, atx::usize dates) {
    std::ofstream f(books_path.string() + ".meta.txt");
    f << "periods=1\n";
    f << "instruments=2\n";
    f << "s=0 period=" << (dates - 1) << " turnover=0.5 cost_bps=0.0\n";
}

struct RunResult {
    atx::f64 capacity_point_aum   = 0.0;
    atx::f64 book_gross_edge_bps  = 0.0;
    atx::usize capacity_curve_points = 0;
    atx::u64 digest = 0;
    bool has_capacity_point_kv = false;
    bool has_gross_edge_kv    = false;
};

static atx::core::Result<RunResult>
run_and_parse(const std::string& research_path, const std::string& books_path,
             const fs::path& report_dir, atx::f64 report_aum) {
    atx::impl::RunConfig cfg;
    cfg.panel      = research_path;
    cfg.books      = books_path;
    cfg.report_out = report_dir.string();
    cfg.report_aum = report_aum;

    ATX_TRY(auto sr, atx::impl::run_report(cfg));

    RunResult rr;
    rr.digest = sr.digest;
    rr.has_capacity_point_kv = kv_has_key(sr.kvs, "capacity_point_aum");
    rr.has_gross_edge_kv     = kv_has_key(sr.kvs, "book_gross_edge_bps");

    const fs::path summary = report_dir / "summary.txt";
    const std::string cap_str   = read_summary_value(summary, "capacity_point_aum");
    const std::string gross_str = read_summary_value(summary, "book_gross_edge_bps");
    const std::string pts_str   = read_summary_value(summary, "capacity_curve_points");
    if (!cap_str.empty())   rr.capacity_point_aum = std::stod(cap_str);
    if (!gross_str.empty()) rr.book_gross_edge_bps = std::stod(gross_str);
    if (!pts_str.empty())   rr.capacity_curve_points = static_cast<atx::usize>(std::stoull(pts_str));
    return atx::core::Ok(rr);
}

// =============================================================================
//  (a) CapacityCurveHandComputed -- independently verified via a Python oracle
//  (2-instrument, D=5-date fixture; wA=0.6, wB=0.4; volA=100000, volB=200000;
//  closeA=[100,102,101,104,103], closeB=[50,51,50.5,52,51.5];
//  report_aum=1,000,000). Oracle numbers (grid = 20 log-spaced points over
//  [0.01,10]x report_aum, linear-interpolated zero-crossing):
//    book_gross_edge_bps ~= 75.70916028254437
//    capacity_point_aum  ~= 3679063.3960469924  (interpolated; analytic
//                            closed-form crossing is 3656397.45 -- the ~0.6%
//                            gap is the grid's linear-interpolation error,
//                            expected and bounded by the log-spacing).
// =============================================================================
TEST(StageReportCapacityCurve, CapacityCurveHandComputed) {
    Fixture fx{"handcheck"};
    const std::vector<atx::f64> closeA{100.0, 102.0, 101.0, 104.0, 103.0};
    const std::vector<atx::f64> closeB{50.0, 51.0, 50.5, 52.0, 51.5};
    auto r_res = make_research_panel(fx.work_dir / "research.bin", closeA, closeB, 100000.0,
                                     200000.0, /*with_volume=*/true);
    ASSERT_TRUE(r_res.has_value()) << r_res.error().message();
    auto r_bk = make_books_panel(fx.work_dir / "books.bin", 0.6, 0.4);
    ASSERT_TRUE(r_bk.has_value()) << r_bk.error().message();
    write_books_meta(fx.work_dir / "books.bin", closeA.size());

    const fs::path report_dir = fx.work_dir / "report";
    auto rr = run_and_parse(*r_res, *r_bk, report_dir, /*report_aum=*/1.0e6);
    ASSERT_TRUE(rr.has_value()) << rr.error().message();

    EXPECT_TRUE(rr->has_capacity_point_kv) << "capacity_point_aum missing from sr.kvs";
    EXPECT_TRUE(rr->has_gross_edge_kv) << "book_gross_edge_bps missing from sr.kvs";
    EXPECT_EQ(rr->capacity_curve_points, 20U) << "the curve must carry kCapacityAumGridPoints=20 points";
    EXPECT_NEAR(rr->book_gross_edge_bps, 75.70916028254437, 1e-4)
        << "book_gross_edge_bps diverged from the Python oracle";
    EXPECT_NEAR(rr->capacity_point_aum, 3679063.3960469924, 50.0)
        << "capacity_point_aum diverged from the Python oracle (interpolated closed form)";
}

// =============================================================================
//  (b) CapacityCurveMonotoneNonIncreasing -- parse capacity_curve.csv and
//  assert net_edge_bps never increases as aum increases (also enforced by
//  cost::capacity_point's own ATX_CHECK -- a violation would have aborted the
//  process rather than merely failing this assertion).
// =============================================================================
TEST(StageReportCapacityCurve, CapacityCurveMonotoneNonIncreasing) {
    Fixture fx{"monotone"};
    const std::vector<atx::f64> closeA{100.0, 102.0, 101.0, 104.0, 103.0};
    const std::vector<atx::f64> closeB{50.0, 51.0, 50.5, 52.0, 51.5};
    auto r_res = make_research_panel(fx.work_dir / "research.bin", closeA, closeB, 100000.0,
                                     200000.0, /*with_volume=*/true);
    ASSERT_TRUE(r_res.has_value()) << r_res.error().message();
    auto r_bk = make_books_panel(fx.work_dir / "books.bin", 0.6, 0.4);
    ASSERT_TRUE(r_bk.has_value()) << r_bk.error().message();
    write_books_meta(fx.work_dir / "books.bin", closeA.size());

    const fs::path report_dir = fx.work_dir / "report";
    auto rr = run_and_parse(*r_res, *r_bk, report_dir, /*report_aum=*/1.0e6);
    ASSERT_TRUE(rr.has_value()) << rr.error().message();

    std::ifstream csv{report_dir / "capacity_curve.csv"};
    ASSERT_TRUE(csv.is_open());
    std::string line;
    std::getline(csv, line); // header
    EXPECT_EQ(line, "aum,net_edge_bps");

    bool have_prev = false;
    atx::f64 prev_aum = 0.0, prev_ne = 0.0;
    atx::usize n_rows = 0;
    while (std::getline(csv, line)) {
        const auto comma = line.find(',');
        ASSERT_NE(comma, std::string::npos);
        const atx::f64 aum = std::stod(line.substr(0, comma));
        const atx::f64 ne  = std::stod(line.substr(comma + 1));
        ++n_rows;
        if (have_prev) {
            EXPECT_GT(aum, prev_aum) << "aum grid must be strictly ascending";
            EXPECT_LE(ne, prev_ne + 1e-6) << "net_edge_bps must be non-increasing in aum";
        }
        prev_aum = aum;
        prev_ne  = ne;
        have_prev = true;
    }
    EXPECT_EQ(n_rows, 20U);
}

// =============================================================================
//  (c) HigherVolatilityShiftsCapacityLower -- a higher-volatility variant of
//  the SAME book realizes a strictly lower capacity_point_aum (Y and sigma
//  enter the cost model identically, Y*sigma, so this is the same sensitivity
//  the spec's "larger Y" acceptance criterion probes).
// =============================================================================
TEST(StageReportCapacityCurve, HigherVolatilityShiftsCapacityLower) {
    Fixture fx_lo{"vol_lo"};
    const std::vector<atx::f64> closeA_lo{100.0, 102.0, 101.0, 104.0, 103.0};
    const std::vector<atx::f64> closeB_lo{50.0, 51.0, 50.5, 52.0, 51.5};
    auto r_res_lo = make_research_panel(fx_lo.work_dir / "research.bin", closeA_lo, closeB_lo,
                                        100000.0, 200000.0, /*with_volume=*/true);
    ASSERT_TRUE(r_res_lo.has_value()) << r_res_lo.error().message();
    auto r_bk_lo = make_books_panel(fx_lo.work_dir / "books.bin", 0.6, 0.4);
    ASSERT_TRUE(r_bk_lo.has_value()) << r_bk_lo.error().message();
    write_books_meta(fx_lo.work_dir / "books.bin", closeA_lo.size());
    auto rr_lo = run_and_parse(*r_res_lo, *r_bk_lo, fx_lo.work_dir / "report", 1.0e6);
    ASSERT_TRUE(rr_lo.has_value()) << rr_lo.error().message();

    Fixture fx_hi{"vol_hi"};
    const std::vector<atx::f64> closeA_hi{100.0, 130.0, 90.0, 140.0, 103.0};
    const std::vector<atx::f64> closeB_hi{50.0, 65.0, 45.0, 70.0, 51.5};
    auto r_res_hi = make_research_panel(fx_hi.work_dir / "research.bin", closeA_hi, closeB_hi,
                                        100000.0, 200000.0, /*with_volume=*/true);
    ASSERT_TRUE(r_res_hi.has_value()) << r_res_hi.error().message();
    auto r_bk_hi = make_books_panel(fx_hi.work_dir / "books.bin", 0.6, 0.4);
    ASSERT_TRUE(r_bk_hi.has_value()) << r_bk_hi.error().message();
    write_books_meta(fx_hi.work_dir / "books.bin", closeA_hi.size());
    auto rr_hi = run_and_parse(*r_res_hi, *r_bk_hi, fx_hi.work_dir / "report", 1.0e6);
    ASSERT_TRUE(rr_hi.has_value()) << rr_hi.error().message();

    EXPECT_LT(rr_hi->capacity_point_aum, rr_lo->capacity_point_aum)
        << "a higher-volatility book must realize a LOWER capacity AUM (the same cost "
           "surface, a bigger Y*sigma erodes the gross edge faster) -- lo="
        << rr_lo->capacity_point_aum << " hi=" << rr_hi->capacity_point_aum;
}

// =============================================================================
//  (d) CapacityCurveAbsentWithoutVolume_DigestUnchanged -- no "volume" field
//  degenerates the capacity fields to documented inert sentinels, AND the
//  stage digest is byte-identical to the SAME fixture WITH volume added --
//  proving the capacity-curve computation never reaches the digest either
//  way (stronger than the spec's literal "absent path only" ask).
// =============================================================================
TEST(StageReportCapacityCurve, CapacityCurveAbsentWithoutVolume_DigestUnchanged) {
    const std::vector<atx::f64> closeA{100.0, 102.0, 101.0, 104.0, 103.0};
    const std::vector<atx::f64> closeB{50.0, 51.0, 50.5, 52.0, 51.5};

    Fixture fx_with{"absent_with"};
    auto r_res_with = make_research_panel(fx_with.work_dir / "research.bin", closeA, closeB,
                                          100000.0, 200000.0, /*with_volume=*/true);
    ASSERT_TRUE(r_res_with.has_value()) << r_res_with.error().message();
    auto r_bk_with = make_books_panel(fx_with.work_dir / "books.bin", 0.6, 0.4);
    ASSERT_TRUE(r_bk_with.has_value()) << r_bk_with.error().message();
    write_books_meta(fx_with.work_dir / "books.bin", closeA.size());
    auto rr_with = run_and_parse(*r_res_with, *r_bk_with, fx_with.work_dir / "report", 1.0e6);
    ASSERT_TRUE(rr_with.has_value()) << rr_with.error().message();

    Fixture fx_without{"absent_without"};
    auto r_res_without = make_research_panel(fx_without.work_dir / "research.bin", closeA, closeB,
                                             100000.0, 200000.0, /*with_volume=*/false);
    ASSERT_TRUE(r_res_without.has_value()) << r_res_without.error().message();
    auto r_bk_without = make_books_panel(fx_without.work_dir / "books.bin", 0.6, 0.4);
    ASSERT_TRUE(r_bk_without.has_value()) << r_bk_without.error().message();
    write_books_meta(fx_without.work_dir / "books.bin", closeA.size());
    auto rr_without = run_and_parse(*r_res_without, *r_bk_without, fx_without.work_dir / "report", 1.0e6);
    ASSERT_TRUE(rr_without.has_value()) << rr_without.error().message();

    // Absent-volume degeneration: the documented inert sentinels.
    EXPECT_TRUE(std::isinf(rr_without->capacity_point_aum))
        << "no volume field -> capacity_point_aum must be the +inf sentinel, got "
        << rr_without->capacity_point_aum;
    EXPECT_DOUBLE_EQ(rr_without->book_gross_edge_bps, 0.0);
    EXPECT_EQ(rr_without->capacity_curve_points, 0U);

    // Present-volume sanity: a real, finite crossing (proven exactly in test (a)).
    EXPECT_FALSE(std::isinf(rr_with->capacity_point_aum));
    EXPECT_EQ(rr_with->capacity_curve_points, 20U);

    // The additive/inert contract: the report DIGEST never depends on whether
    // the capacity curve was computed at all.
    EXPECT_EQ(rr_with->digest, rr_without->digest)
        << "the report digest must be identical whether or not the capacity curve "
           "was computed -- it is over rep.* numeric series only";
}

} // namespace atx_impl_stage_report_capacity_curve
