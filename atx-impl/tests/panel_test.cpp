#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/history_panel.hpp"  // kHistField* constants
#include "atx/engine/data/orats_history.hpp"   // kOratsFields
#include "atx/tsdb/load_parquet.hpp"           // build_from_long, LongColumns

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

// ---------------------------------------------------------------------------
// (A) Serialize round-trip tests — synthetic, no seg files needed.
// ---------------------------------------------------------------------------

namespace {
namespace fs = std::filesystem;
using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;

// Build a small owned 2-date × 3-instrument Panel with 2 fields.
// "close": cells [0,1,NaN, 4,5,6]  (NaN at cell [0,2] — date 0, instr 2)
// "volume": cells [10,20,30, 40,50,60]
// universe: instr 2 is out-of-universe on date 0 (byte[2]=0), all others in.
Panel make_test_panel() {
    const atx::usize D = 2;
    const atx::usize I = 3;
    std::vector<std::string> names = {"close", "volume"};

    // close: date-major, row0=[0,1,NaN], row1=[4,5,6]
    std::vector<atx::f64> close_data = {
        0.0, 1.0, std::numeric_limits<atx::f64>::quiet_NaN(),
        4.0, 5.0, 6.0
    };
    // volume: date-major, row0=[10,20,30], row1=[40,50,60]
    std::vector<atx::f64> vol_data = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};

    std::vector<std::vector<atx::f64>> field_data = {close_data, vol_data};

    // universe mask: date-major, 6 bytes
    // date 0: instr 0=in, instr 1=in, instr 2=OUT
    // date 1: all in
    std::vector<std::uint8_t> universe = {1, 1, 0, 1, 1, 1};

    auto r = Panel::create(D, I, std::move(names), std::move(field_data), std::move(universe));
    // If create() fails the test itself will fail at the ASSERT below.
    return std::move(r).value();
}

// Write the test panel and return the temp path.
std::string panel_tmp_path(const char* tag) {
    return (fs::temp_directory_path() /
            (std::string("atx_impl_panel_test_") + tag + ".bin")).string();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AtxImplPanel.RoundTripPreservesPanel
// ---------------------------------------------------------------------------
TEST(AtxImplPanel, RoundTripPreservesPanel) {
    Panel orig = make_test_panel();
    const std::string path = panel_tmp_path("roundtrip");
    fs::remove(fs::path(path));

    // Write.
    auto wr = atx::impl::write_panel(orig, path);
    ASSERT_TRUE(wr.has_value()) << wr.error().message();

    // Read back.
    auto rr = atx::impl::read_panel(path);
    ASSERT_TRUE(rr.has_value()) << rr.error().message();
    const Panel& p = *rr;

    // Shape.
    EXPECT_EQ(p.dates(),       2u);
    EXPECT_EQ(p.instruments(), 3u);
    EXPECT_EQ(p.num_fields(),  2u);

    // Field names.
    EXPECT_EQ(p.field_name(0u), "close");
    EXPECT_EQ(p.field_name(1u), "volume");

    // Field values — NaN-aware comparison.
    {
        auto orig_cs = orig.field_all(static_cast<FieldId>(0));
        auto read_cs = p.field_all(static_cast<FieldId>(0));
        ASSERT_EQ(orig_cs.size(), read_cs.size());
        for (atx::usize c = 0; c < orig_cs.size(); ++c) {
            if (std::isnan(orig_cs[c])) {
                EXPECT_TRUE(std::isnan(read_cs[c]))
                    << "cell " << c << " expected NaN, got " << read_cs[c];
            } else {
                EXPECT_EQ(orig_cs[c], read_cs[c]) << "cell " << c << " mismatch";
            }
        }
    }
    {
        auto orig_vs = orig.field_all(static_cast<FieldId>(1));
        auto read_vs = p.field_all(static_cast<FieldId>(1));
        ASSERT_EQ(orig_vs.size(), read_vs.size());
        for (atx::usize c = 0; c < orig_vs.size(); ++c) {
            EXPECT_EQ(orig_vs[c], read_vs[c]) << "volume cell " << c << " mismatch";
        }
    }

    // Universe mask.
    for (atx::usize d = 0; d < 2u; ++d) {
        for (atx::usize i = 0; i < 3u; ++i) {
            EXPECT_EQ(p.in_universe(d, i), orig.in_universe(d, i))
                << "universe mismatch at (d=" << d << ", i=" << i << ")";
        }
    }

    fs::remove(fs::path(path));
}

// ---------------------------------------------------------------------------
// AtxImplPanel.DigestStableAndVerified
// ---------------------------------------------------------------------------
TEST(AtxImplPanel, DigestStableAndVerified) {
    Panel orig = make_test_panel();
    const std::string pathA = panel_tmp_path("digest_a");
    const std::string pathB = panel_tmp_path("digest_b");
    fs::remove(fs::path(pathA));
    fs::remove(fs::path(pathB));

    // Write twice.
    auto r1 = atx::impl::write_panel(orig, pathA);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    auto r2 = atx::impl::write_panel(orig, pathB);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    // Digests must be equal and non-zero.
    EXPECT_NE(*r1, atx::u64{0}) << "digest must be non-zero";
    EXPECT_EQ(*r1, *r2) << "digest must be stable across writes";

    // Corrupt one trailer byte of pathB and verify read_panel returns ParseError.
    {
        // Open the file and flip the last byte (part of the 8-byte trailer).
        std::fstream f(pathB, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open()) << "cannot open " << pathB << " for corruption";
        f.seekp(-1, std::ios::end);
        char byte{};
        f.seekg(-1, std::ios::end);
        f.read(&byte, 1);
        byte = static_cast<char>(~static_cast<unsigned char>(byte));
        f.seekp(-1, std::ios::end);
        f.write(&byte, 1);
    }

    auto bad = atx::impl::read_panel(pathB);
    ASSERT_FALSE(bad.has_value()) << "expected ParseError for corrupted file";
    EXPECT_EQ(bad.error().code(), atx::core::ErrorCode::ParseError);

    fs::remove(fs::path(pathA));
    fs::remove(fs::path(pathB));
}

// ---------------------------------------------------------------------------
// (B) run_panel integration test — build real segments then panel.
//
// Mirrors OratsE2ESmoke::SyntheticPartitionRunsUnchangedRobustPipeline:
//   60 dates x 30 instruments, volume/shares sized to clear the ADV screen,
//   min_adv_usd=0 to guarantee all instruments pass.
// ---------------------------------------------------------------------------

namespace {

// Write one date's .seg file with all 16 ORATS fields for n_instr symbols
// into `dir` with filename `name`, timestamp `dn` (unix nanos).
// close[i] = base_close + i; cumReturnFactor = 1.0; shares = 2e8; volume = 1e6.
void write_seg_day(const fs::path& dir, const std::string& name, atx::i64 dn,
                   int n_instr, atx::f64 base_close) {
    const auto r = static_cast<atx::usize>(n_instr);
    atx::tsdb::LongColumns cols;
    cols.field_names.assign(atx::engine::data::kOratsFields.begin(),
                             atx::engine::data::kOratsFields.end());
    cols.times.assign(r, dn);
    cols.symbols.reserve(r);
    for (int i = 0; i < n_instr; ++i) {
        cols.symbols.push_back(std::to_string(10001 + i));
    }
    cols.values.assign(atx::engine::data::kOratsFields.size(),
                        std::vector<atx::f64>(r, 0.0));
    // idx 3 = close
    for (int i = 0; i < n_instr; ++i) {
        cols.values[3][static_cast<atx::usize>(i)] = base_close + static_cast<atx::f64>(i);
    }
    // idx 6 = volume (1M shares/day -> dollar_volume >> $1M ADV floor)
    cols.values[6].assign(r, 1.0e6);
    // idx 7 = shares (200M -> market_cap >> floor)
    cols.values[7].assign(r, 2.0e8);
    // idx 10 = cumReturnFactor = 1.0 (TRI = raw close)
    cols.values[10].assign(r, 1.0);

    auto ok = atx::tsdb::build_from_long(cols, (dir / name).string(), 0);
    ASSERT_TRUE(ok.has_value()) << "write_seg_day failed for " << name;
}

} // anonymous namespace

TEST(AtxImplPanel, BuildsPanelFromSegments) {
    // ---- build synthetic temp partition: 60 dates x 30 instruments ----
    const fs::path seg_dir = fs::temp_directory_path() / "atx_impl_panel_synth_seg";
    {
        std::error_code ec;
        fs::remove_all(seg_dir, ec);
        fs::create_directories(seg_dir, ec);
    }

    constexpr int kDates = 60;
    constexpr int kInstr = 30;
    const atx::i64 kDayNanos = 86400LL * 1'000'000'000LL;
    const atx::i64 kDay0 = 18263LL * kDayNanos; // 2020-01-02 midnight UTC

    for (int d = 0; d < kDates; ++d) {
        const atx::i64 dn = kDay0 + static_cast<atx::i64>(d) * kDayNanos;
        const std::string fname = "seg_" + std::to_string(1000 + d) + ".seg";
        const atx::f64 base = 50.0 + static_cast<atx::f64>(d) * 0.1;
        write_seg_day(seg_dir, fname, dn, kInstr, base);
    }

    // ---- build the panel via run_panel ----
    const std::string panel_out =
        (fs::temp_directory_path() / "atx_impl_panel_synth_out.bin").string();
    fs::remove(fs::path(panel_out));

    atx::impl::RunConfig cfg;
    cfg.segs       = seg_dir.string();
    cfg.panel_out  = panel_out;
    // Empty start/end -> default TimeWindow (all dates).
    cfg.min_adv_usd   = 0.0;  // admit all synthetic instruments
    cfg.top_n_by_adv  = 0;    // no count cap

    auto r = atx::impl::run_panel(cfg);
    ASSERT_TRUE(r.has_value()) << "run_panel failed: " << r.error().message();

    // Verify kvs.
    auto find_kv = [&](const std::string& key) -> std::string {
        for (const auto& [k, v] : r->kvs) {
            if (k == key) return v;
        }
        return "";
    };
    EXPECT_EQ(find_kv("fields"), "12") << "expected 12 history panel fields";
    EXPECT_GE(std::stoul(find_kv("dates")), 1u) << "expected at least 1 date";
    EXPECT_GE(std::stoul(find_kv("instruments")), 1u) << "expected at least 1 instrument";

    // ---- read_panel the output and inspect ----
    auto pr = atx::impl::read_panel(panel_out);
    ASSERT_TRUE(pr.has_value()) << "read_panel failed: " << pr.error().message();
    const Panel& p = *pr;

    EXPECT_GE(p.dates(),       1u) << "read panel: expected >=1 dates";
    EXPECT_GE(p.instruments(), 1u) << "read panel: expected >=1 instruments";
    EXPECT_EQ(p.num_fields(),  12u) << "read panel: expected exactly 12 fields";

    // Confirm the three required field names resolve.
    auto close_id  = p.field_id(atx::engine::data::kHistFieldClose);
    auto mktcap_id = p.field_id(atx::engine::data::kHistFieldMarketCap);
    auto sector_id = p.field_id(atx::engine::data::kHistFieldSector);
    EXPECT_TRUE(close_id.has_value())  << "panel missing 'close' field";
    EXPECT_TRUE(mktcap_id.has_value()) << "panel missing 'market_cap' field";
    EXPECT_TRUE(sector_id.has_value()) << "panel missing 'sector' field";

    // ---- clean up ----
    {
        std::error_code ec;
        fs::remove_all(seg_dir, ec);
    }
    fs::remove(fs::path(panel_out));
}

// ---------------------------------------------------------------------------
// AtxImplPanel.SidecarMetaWrittenAlongsidePanel (S5-3)
//
// Verifies that run_panel emits a <panel_out>.meta.txt sidecar beside the
// binary panel, that the sidecar is parseable as key=value lines, that the
// required keys are present, and that the numeric shape fields match what the
// panel itself reports.  built_utc is intentionally NOT value-asserted (non-
// deterministic wall-clock timestamp).
// ---------------------------------------------------------------------------

TEST(AtxImplPanel, SidecarMetaWrittenAlongsidePanel) {
    // ---- build the same synthetic partition as BuildsPanelFromSegments ----
    const fs::path seg_dir =
        fs::temp_directory_path() / "atx_impl_sidecar_synth_seg";
    {
        std::error_code ec;
        fs::remove_all(seg_dir, ec);
        fs::create_directories(seg_dir, ec);
    }

    constexpr int kDates = 10; // small — we only care about the sidecar, not shape
    constexpr int kInstr = 5;
    const atx::i64 kDayNanos = 86400LL * 1'000'000'000LL;
    const atx::i64 kDay0     = 18263LL * kDayNanos; // 2020-01-02 midnight UTC

    for (int d = 0; d < kDates; ++d) {
        const atx::i64 dn    = kDay0 + static_cast<atx::i64>(d) * kDayNanos;
        const std::string fn = "seg_" + std::to_string(2000 + d) + ".seg";
        write_seg_day(seg_dir, fn, dn, kInstr, 100.0 + static_cast<atx::f64>(d));
    }

    const std::string panel_out =
        (fs::temp_directory_path() / "atx_impl_sidecar_out.bin").string();
    const std::string meta_path = panel_out + ".meta.txt";
    fs::remove(fs::path(panel_out));
    fs::remove(fs::path(meta_path));

    atx::impl::RunConfig cfg;
    cfg.segs      = seg_dir.string();
    cfg.panel_out = panel_out;
    cfg.min_adv_usd  = 0.0;
    cfg.top_n_by_adv = 0;

    auto r = atx::impl::run_panel(cfg);
    ASSERT_TRUE(r.has_value()) << "run_panel failed: " << r.error().message();

    // ---- the sidecar must exist ----
    ASSERT_TRUE(fs::exists(meta_path)) << "sidecar not found: " << meta_path;

    // ---- parse the sidecar into a string->string map ----
    std::ifstream mf(meta_path);
    ASSERT_TRUE(mf.is_open()) << "cannot open sidecar: " << meta_path;

    // First line is the bare version header "atx_panel_meta_v1" (no '=').
    std::string first_line;
    ASSERT_TRUE(std::getline(mf, first_line));
    EXPECT_EQ(first_line, "atx_panel_meta_v1") << "sidecar: unexpected header";

    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(mf, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue; // skip blank / malformed
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }

    // ---- required keys present ----
    const std::vector<std::string> required_keys = {
        "built_utc", "panel_bin", "engine_digest",
        "dates", "instruments", "fields",
        "augmented", "adv_windows",
        "universe_min_adv_usd", "universe_top_n_by_adv",
        "universe_min_price", "universe_require_sector",
    };
    for (const auto& k : required_keys) {
        EXPECT_TRUE(kv.count(k) != 0) << "sidecar missing key: " << k;
    }

    // ---- engine_digest is non-empty ----
    EXPECT_FALSE(kv["engine_digest"].empty()) << "sidecar: engine_digest is empty";

    // ---- default (non-augmented) build values ----
    EXPECT_EQ(kv["adv_windows"], "none")  << "sidecar: unexpected adv_windows value";
    EXPECT_EQ(kv["augmented"],   "false") << "sidecar: unexpected augmented value";

    // ---- numeric shape fields match the StageResult kvs ----
    auto find_kv = [&](const std::string& key) -> std::string {
        for (const auto& [k, v] : r->kvs) {
            if (k == key) return v;
        }
        return "";
    };
    EXPECT_EQ(kv["fields"],      find_kv("fields"))      << "sidecar fields != StageResult fields";
    EXPECT_EQ(kv["dates"],       find_kv("dates"))       << "sidecar dates != StageResult dates";
    EXPECT_EQ(kv["instruments"], find_kv("instruments")) << "sidecar instruments != StageResult instruments";

    // ---- clean up ----
    mf.close(); // close before removal so Windows can unlink the sidecar
    {
        std::error_code ec;
        fs::remove_all(seg_dir, ec);
    }
    fs::remove(fs::path(panel_out));
    fs::remove(fs::path(meta_path));
}
