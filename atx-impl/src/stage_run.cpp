#include "stages.hpp"

#include <filesystem>
#include <string>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "artifacts.hpp"
#include "config.hpp"

namespace atx::impl {

namespace fs = std::filesystem;

atx::core::Result<StageResult> run_all(const RunConfig& cfg)
{
    // Validate: work dir and zip are mandatory for run mode.
    if (cfg.out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "run: --out (work dir) required");
    }
    if (cfg.zip.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "run: --zip required");
    }

    const fs::path work = cfg.out;
    {
        std::error_code ec;
        fs::create_directories(work, ec);
        if (ec) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "run: cannot create work dir: " + cfg.out);
        }
    }

    // 1. load
    RunConfig c_load = cfg;
    c_load.out = (work / "segs").string();
    ATX_TRY(auto d_load, run_load(c_load));

    // 2. panel
    RunConfig c_panel = cfg;
    c_panel.segs      = (work / "segs").string();
    c_panel.panel_out = (work / "panel.bin").string();
    ATX_TRY(auto d_panel, run_panel(c_panel));

    // 3. discover
    RunConfig c_disc = cfg;
    c_disc.panel     = (work / "panel.bin").string();
    c_disc.alpha_out = (work / "alphas").string();
    // A1 — route the default pipeline through library accumulation. The
    // accumulation/library machinery lives entirely in run_discover_gated, which
    // run_discover only enters when cfg.gated is true (stage_discover.cpp:880);
    // the ungated top-N path never touches library_dir. So we must turn ON the
    // quality gate AND point it at a stable library dir:
    //   * gated => route through factory::Factory::mine_into + AlphaGate floors
    //     + the persistent library::Library (the mega-alpha database).
    //   * library_dir non-empty => accumulate (accumulate ==
    //     !cfg.library_dir.empty(), stage_discover.cpp:385) instead of a per-run
    //     wipe, AND auto-enable OOS-default admission of 0.25 (eff_oos_fraction,
    //     stage_discover.cpp:397-400) because run_all does not set --oos-fraction.
    // accumulate keys off library_dir (NOT set_flags), so no set_flags insert is
    // needed; an explicit user --oos-fraction still overrides the 0.25 default.
    c_disc.gated       = true;
    c_disc.library_dir = (work / "_library").string();
    ATX_TRY(auto d_disc, run_discover(c_disc));

    // 4. combine
    RunConfig c_comb = cfg;
    c_comb.panel     = (work / "panel.bin").string();
    c_comb.alphas    = (work / "alphas").string();
    c_comb.combo_out = (work / "combo.bin").string();
    // A1 — feed combine from the SAME accumulated library. With library_dir set,
    // run_combine takes the from_library branch (stage_combine.cpp:73),
    // enumerating admitted records by AlphaId; the loose c_comb.alphas above
    // becomes an ignored harmless fallback (left as-is intentionally).
    c_comb.library_dir = c_disc.library_dir;
    ATX_TRY(auto d_comb, run_combine(c_comb));

    // 5. optimize
    RunConfig c_opt = cfg;
    c_opt.panel     = (work / "panel.bin").string();
    c_opt.combo     = (work / "combo.bin").string();
    c_opt.books_out = (work / "books.bin").string();
    ATX_TRY(auto d_opt, run_optimize(c_opt));

    // 6. report
    RunConfig c_rep = cfg;
    c_rep.panel      = (work / "panel.bin").string();
    c_rep.books      = (work / "books.bin").string();
    c_rep.report_out = cfg.report_out.empty()
                           ? (work / "report").string()
                           : cfg.report_out;
    ATX_TRY(auto d_rep, run_report(c_rep));

    // Fold the 6 stage digests in fixed order into the run digest.
    atx::u64 ds[6] = {
        d_load.digest,
        d_panel.digest,
        d_disc.digest,
        d_comb.digest,
        d_opt.digest,
        d_rep.digest,
    };
    const atx::u64 run_digest = fnv1a64(ds, sizeof(ds));

    StageResult sr;
    sr.digest = run_digest;
    sr.kvs = {
        {"load",     to_hex16(d_load.digest)},
        {"panel",    to_hex16(d_panel.digest)},
        {"discover", to_hex16(d_disc.digest)},
        {"combine",  to_hex16(d_comb.digest)},
        {"optimize", to_hex16(d_opt.digest)},
        {"report",   to_hex16(d_rep.digest)},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
