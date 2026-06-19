#include "stages.hpp"

#include <array>
#include <filesystem>
#include <string>

#include "atx/core/error.hpp"
#include "atx/engine/regime/loader.hpp"
#include "atx/engine/regime/source_csv.hpp"
#include "artifacts.hpp"
#include "config.hpp"

namespace atx::impl {

atx::core::Result<StageResult> run_regime(const RunConfig& cfg) {
    namespace fs = std::filesystem;
    namespace rg = atx::engine::regime;

    if (cfg.staging_dir.empty() || cfg.regime_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "regime: --staging-dir and --regime-out required");
    }

    rg::RegimeLoadConfig rc;
    rc.staging_dir = cfg.staging_dir;
    rc.out_path = cfg.regime_out;
    rc.created_at_nanos = 0;  // FIXED for determinism (byte-identical re-runs).

    if (!cfg.min_date.empty()) {
        auto md = rg::date_to_nanos(cfg.min_date);
        if (!md.has_value()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "regime: --min-date must be YYYY-MM-DD");
        }
        rc.min_date_nanos = *md;
    }

    // v1 default staged-file mapping. Only series whose CSV is present in the
    // staging dir are loaded (vvix/move are manually staged; a FRED-only dir is
    // the common case). See regime/README.md for sources.
    const std::array<rg::SeriesSpec, 8> kDefault{{
        {"vix",    "vix.csv",    rg::CsvFormat::Fred,  "VALUE"},
        {"vvix",   "vvix.csv",   rg::CsvFormat::Cboe,  "CLOSE"},
        {"move",   "move.csv",   rg::CsvFormat::Yahoo, "Adj Close"},
        {"dgs2",   "dgs2.csv",   rg::CsvFormat::Fred,  "VALUE"},
        {"dgs10",  "dgs10.csv",  rg::CsvFormat::Fred,  "VALUE"},
        {"hy_oas", "hy_oas.csv", rg::CsvFormat::Fred,  "VALUE"},
        {"ig_oas", "ig_oas.csv", rg::CsvFormat::Fred,  "VALUE"},
        {"nfci",   "nfci.csv",   rg::CsvFormat::Fred,  "VALUE"},
    }};
    bool have_dgs2 = false;
    bool have_dgs10 = false;
    for (const rg::SeriesSpec& s : kDefault) {
        if (fs::exists(fs::path(cfg.staging_dir) / s.file)) {
            rc.series.push_back(s);
            if (s.name == "dgs2")  have_dgs2 = true;
            if (s.name == "dgs10") have_dgs10 = true;
        }
    }
    if (rc.series.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "regime: no known series CSVs found in --staging-dir");
    }
    // Curve derived only when both legs are present.
    if (have_dgs2 && have_dgs10) {
        rc.derived.push_back("t10y2y = dgs10 - dgs2");
    }

    auto st = rg::load_regime_history(rc);
    if (!st) {
        return atx::core::Err(st.error());
    }

    const std::array<atx::i64, 4> d{
        st->series_count, st->dates_written, st->first_date_nanos, st->last_date_nanos};
    StageResult r;
    r.digest = fnv1a64(d.data(), d.size() * sizeof(atx::i64));
    r.kvs = { {"series", std::to_string(st->series_count)},
              {"dates",  std::to_string(st->dates_written)},
              {"first",  std::to_string(st->first_date_nanos)},
              {"last",   std::to_string(st->last_date_nanos)} };
    return atx::core::Ok(std::move(r));
}

} // namespace atx::impl
