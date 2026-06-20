#include "stages.hpp"

#include <string>

#include "artifacts.hpp"          // fnv1a64, to_hex16
#include "serialize_panel.hpp"    // write_panel

#include "atx/core/error.hpp"
#include "atx/engine/alpha/segment_panel.hpp" // alpha::TimeWindow
#include "atx/engine/data/history_panel.hpp"  // build_history_panel, HistoryDataConfig
#include "atx/engine/data/orats_history.hpp"  // detail::date_to_nanos
#include "atx/engine/data/universe.hpp"       // UniverseConfig

namespace atx::impl {

atx::core::Result<StageResult> run_panel(const RunConfig& cfg) {
    // 1. Validate required fields.
    if (cfg.segs.empty() || cfg.panel_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "panel: --segs and --out (panel_out) required");
    }

    // 2. Build TimeWindow.
    atx::engine::alpha::TimeWindow w{};
    if (!cfg.start.empty()) {
        auto ns = atx::engine::data::detail::date_to_nanos(cfg.start);
        if (!ns.has_value()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "panel: --start is not a valid date: " + cfg.start);
        }
        w.start_nanos = *ns;
    }
    if (!cfg.end.empty()) {
        auto ns = atx::engine::data::detail::date_to_nanos(cfg.end);
        if (!ns.has_value()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "panel: --end is not a valid date: " + cfg.end);
        }
        w.end_nanos = *ns;
    }

    // 3. Build UniverseConfig.
    atx::engine::data::UniverseConfig u{};
    u.min_adv_usd = cfg.min_adv_usd;
    u.top_n_by_adv = static_cast<atx::usize>(cfg.top_n_by_adv < 0 ? 0 : cfg.top_n_by_adv);
    u.min_price = cfg.min_price;            // raw-close floor (0 = off)
    u.require_sector = cfg.require_sector;  // single-stock (GICS) screen
    // adv_window=21 and min_mktcap_usd=0 remain at their defaults.

    // 4. Build history panel.
    atx::engine::data::HistoryDataConfig hc{cfg.segs, w, u};
    hc.compact_to_universe = cfg.compact_universe; // drop never-in-universe columns
    ATX_TRY(auto hp, atx::engine::data::build_history_panel(hc));

    // 5. Serialize.
    ATX_TRY(auto wd, write_panel(hp.panel, cfg.panel_out));

    // 6. Return StageResult.
    StageResult sr;
    sr.digest = wd;
    sr.kvs = {
        {"dates",         std::to_string(hp.panel.dates())},
        {"instruments",   std::to_string(hp.panel.instruments())},
        {"fields",        std::to_string(hp.panel.num_fields())},
        {"engine_digest", to_hex16(hp.digest)},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
