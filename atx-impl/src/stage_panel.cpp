#include "stages.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>  // std::filesystem::exists (ATX_PANEL_INCREMENTAL append path)
#include <fstream>
#include <string>

#include "artifacts.hpp"          // fnv1a64, to_hex16
#include "serialize_panel.hpp"    // write_panel

#include "atx/core/error.hpp"
#include "atx/engine/alpha/augment.hpp"        // with_alpha101_fields (opt-in panel augmentation)
#include "atx/engine/alpha/segment_panel.hpp"  // alpha::TimeWindow
#include "atx/engine/data/history_panel.hpp"   // build_history_panel, HistoryDataConfig
#include "atx/engine/data/orats_history.hpp"   // detail::date_to_nanos
#include "atx/engine/data/universe.hpp"        // UniverseConfig

namespace atx::impl {

namespace {

// Acquire the history panel for run_panel: a full rebuild on the default path, or
// the opt-in incremental append when built with ATX_PANEL_INCREMENTAL (OFF in all
// default/CI builds, so the default path is byte-identical to pre-S6).
//
// Incremental path (guarded): when an existing panel.bin is present at
// cfg.panel_out AND a new-seg directory is supplied (cfg.out is repurposed as that
// directory until Sprint 7 declares a dedicated config field), load the existing
// panel and append only the strictly-later dates via append_history_panel — whose
// output is byte-identical to a full rebuild over the combined range (proven by the
// S6-1 byte-identity test). On any miss (no existing panel.bin / no new-seg dir) it
// falls back to the full rebuild, never a silent partial panel.
[[nodiscard]] atx::core::Result<atx::engine::data::HistoryPanel>
acquire_history_panel([[maybe_unused]] const RunConfig& cfg,
                      const atx::engine::data::HistoryDataConfig& hc) {
#if defined(ATX_PANEL_INCREMENTAL)
    if (!cfg.panel_out.empty() && !cfg.out.empty() && std::filesystem::exists(cfg.panel_out)) {
        ATX_TRY(auto existing, read_panel(cfg.panel_out));
        return atx::engine::data::append_history_panel(existing, cfg.out, hc);
    }
#endif // ATX_PANEL_INCREMENTAL
    return atx::engine::data::build_history_panel(hc);
}

} // namespace

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
    // S7-WIRES: cfg.incremental_panel: bool — Sprint 7 declares the opt-in flag (and
    // a new-seg directory field) in config.hpp and threads them through the CLI. Until
    // then the incremental path is compiled out (ATX_PANEL_INCREMENTAL is undefined in
    // every default/CI build), so this stage is byte-identical to pre-S6.
    atx::engine::data::HistoryDataConfig hc{cfg.segs, w, u};
    hc.compact_to_universe = cfg.compact_universe; // drop never-in-universe columns
    ATX_TRY(auto hp, acquire_history_panel(cfg, hc));

    // S7-3: opt-in Alpha101 panel augmentation. Default (augment_panel=false, adv_windows
    // empty) skips this entirely -> panel.bin byte-identical. Empty list falls back to the
    // single legacy --adv-window.
    if (cfg.augment_panel || !cfg.adv_windows.empty()) {
        const std::vector<atx::u16> adv_wins =
            cfg.adv_windows.empty()
                ? std::vector<atx::u16>{static_cast<atx::u16>(cfg.adv_window)}
                : cfg.adv_windows;
        ATX_TRY(hp.panel, atx::engine::alpha::with_alpha101_fields(hp.panel, adv_wins));
    }

    // 5. Serialize.
    ATX_TRY(auto wd, write_panel(hp.panel, cfg.panel_out));

    // 6. Write provenance sidecar (<panel_out>.meta.txt).
    // The sidecar is informational only — it does NOT affect panel.bin or its digest.
    // A write failure is surfaced as Err(IoError) per the repo "never ignore an error" bar.
    {
        // Capture wall-clock UTC for the build timestamp (sidecar is non-deterministic by design).
        const auto now = std::chrono::system_clock::now();
        const std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm gm{};
#if defined(_WIN32)
        gmtime_s(&gm, &tt);
#else
        gmtime_r(&tt, &gm);
#endif
        char ts_buf[32]{};
        std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &gm);

        // Runtime values reflect actual augmentation state (S7-3 wired).
        const bool did_augment = cfg.augment_panel || !cfg.adv_windows.empty();
        std::string adv_windows_val = "none";
        if (did_augment) {
            if (cfg.adv_windows.empty()) {
                adv_windows_val = std::to_string(cfg.adv_window);
            } else {
                adv_windows_val.clear();
                for (std::size_t i = 0; i < cfg.adv_windows.size(); ++i) {
                    if (i) adv_windows_val += ",";
                    adv_windows_val += std::to_string(cfg.adv_windows[i]);
                }
            }
        }
        const char* augmented_val = did_augment ? "true" : "false";

        const std::string meta_path = cfg.panel_out + ".meta.txt";
        std::ofstream mf(meta_path, std::ios::out | std::ios::trunc);
        if (!mf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "panel: cannot open sidecar for writing: " + meta_path);
        }

        mf << "atx_panel_meta_v1\n"
           << "built_utc=" << ts_buf << "\n"
           << "panel_bin=" << cfg.panel_out << "\n"
           << "universe_min_adv_usd=" << cfg.min_adv_usd << "\n"
           << "universe_top_n_by_adv=" << cfg.top_n_by_adv << "\n"
           << "universe_min_price=" << cfg.min_price << "\n"
           << "universe_require_sector=" << (cfg.require_sector ? "true" : "false") << "\n"
           << "adv_windows=" << adv_windows_val << "\n"
           << "augmented=" << augmented_val << "\n"
           << "engine_digest=" << to_hex16(hp.digest) << "\n"
           << "dates=" << hp.panel.dates() << "\n"
           << "instruments=" << hp.panel.instruments() << "\n"
           << "fields=" << hp.panel.num_fields() << "\n";

        if (!mf.good()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "panel: sidecar write failed: " + meta_path);
        }
        mf.close();
        if (!mf.good()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "panel: sidecar close failed: " + meta_path);
        }
    }

    // 7. Return StageResult.
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
