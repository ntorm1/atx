#include "atx/vol/surface_db_build.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/chain.hpp"          // OptionChain (board -> Underlying, corpus_board_fit path)
#include "atx/vol/curve_selector.hpp" // select_curve, production_selector_config, SelectorResult
#include "atx/vol/fit_policy.hpp"     // select_fit_policy, FitDecision
#include "atx/vol/session.hpp"        // make_session_inputs, SessionInputs
#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs
#include "atx/vol/universe.hpp"       // Underlying, Chain
#include "atx/vol/vol_curve.hpp"      // CurveConfig (dense index recipe)
#include "surface_db_seed.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

SymbolFitConfig seed_symbol_config(std::string_view symbol, FitPreset preset,
                                   std::string_view index_symbol) {
  SymbolFitConfig cfg = symbol_config_from_preset(preset);
  if (!index_symbol.empty() && symbol == index_symbol) {
    cfg.pin_curve = true;
    cfg.curve = CurveConfig{}; // default = the dense index recipe (node_cap 40)
  }
  return cfg;
}

namespace {

// A board can be selected on only when its underlying carries at least one
// expiry with >= 2 strikes: a single point cannot pin or select a curve (the
// held-out selector has nothing to hold out, and a pinned family has no smile).
// A board gutted below this is a fail-closed selection failure.
[[nodiscard]] bool underlying_selectable(const Underlying &under) noexcept {
  for (const Chain &c : under.chains) {
    if (c.n_strikes() >= 2u) {
      return true;
    }
  }
  return false;
}

// Lower the board's chain to the market/pricing context `select_curve` consumes,
// mirroring PricerFitter::fit's SessionInputs assembly (make_session_inputs +
// per-expiry term rates + dividends), then map it onto SurfaceParityInputs
// exactly as pricer_fitter.cpp does before its own select_curve call.
[[nodiscard]] SurfaceParityInputs parity_inputs_from_chain(const OptionChain &chain,
                                                           FitPreset preset) {
  SessionInputs in = make_session_inputs(preset, chain.spot(), chain.rate(), chain.now_ns());
  const Underlying &under = chain.underlying();
  if (chain.env().yield.size() > 0u) {
    in.expiry_rate_T.reserve(under.chains.size());
    in.expiry_rates.reserve(under.chains.size());
    for (const Chain &expiry : under.chains) {
      in.expiry_rate_T.push_back(expiry.T);
      in.expiry_rates.push_back(chain.env().rate_at(expiry.T));
    }
  }
  in.cash_divs = chain.env().cash_divs;

  SurfaceParityInputs sp;
  sp.S = in.S;
  sp.r = in.r;
  sp.expiry_rate_T = in.expiry_rate_T;
  sp.expiry_rates = in.expiry_rates;
  sp.cash_divs = in.cash_divs;
  sp.now_ts_ns = in.now_ts_ns;
  sp.deam = in.deam;
  sp.calib = in.calib;
  sp.band_k = in.band_k;
  sp.repair = in.calendar_repair;
  sp.fit_workers = 1u; // single board; never nest a fan-out here
  sp.score_parity = in.score_parity;
  sp.enforce_calendar_floor = in.enforce_calendar_floor;
  sp.use_deam_cache_for_fit = in.use_deam_cache_for_fit;
  return sp;
}

// Produce the per-symbol config for one board, or return false when selection
// failed (fail-closed: the caller stores a disabled config). Never throws.
[[nodiscard]] bool config_for_board(const CorpusBoard &board, const AutoConfigSpec &spec,
                                    SymbolFitConfig &out) noexcept {
  try {
    Result<OptionChain> chain = OptionChain::from_frame(board.frame, board.env);
    if (!chain.has_value()) {
      return false;
    }
    const Underlying &under = chain->underlying();
    if (!underlying_selectable(under)) {
      return false;
    }

    // Board-feature classification: pins the curve FAMILY. The numerical preset
    // tier stays the operator-selected `spec.preset` (matching
    // populate_universe_streaming's seeding and the StoresConfigPerSymbol
    // contract); the policy decides the family, not the tier.
    const FitDecision decision = select_fit_policy(under, board.symbol, board.fit_context, {});
    SymbolFitConfig cfg = symbol_config_from_preset(spec.preset);
    cfg.pin_curve = true;
    cfg.curve = decision.curve;

    if (spec.deep_selection) {
      const SurfaceParityInputs sp = parity_inputs_from_chain(*chain, cfg.preset);
      const Result<SelectorResult> selected =
          select_curve(under, sp, production_selector_config());
      if (selected.has_value()) {
        cfg.curve = selected->chosen; // pin the held-out winner
        cfg.pin_curve = true;
      } else {
        const ErrorCode code = selected.error().code();
        // NotFound (no scorable holdout) / Unavailable (budget) are not defects:
        // fall back to the fit-policy decision curve already pinned above. Any
        // other error (e.g. InvalidArgument) is a hard selection failure.
        if (code != ErrorCode::NotFound && code != ErrorCode::Unavailable) {
          return false;
        }
      }
    }

    out = std::move(cfg);
    return true;
  } catch (...) {
    // A selection helper must never let an exception escape the per-symbol loop
    // (e.g. bad_alloc from a fit); record the symbol as a fail-closed disable.
    return false;
  }
}

} // namespace

Result<AutoConfigReport> generate_symbol_configs(SurfaceDb &db, std::span<const CorpusBoard> boards,
                                                 const AutoConfigSpec &spec) {
  // Pick one config board per distinct symbol: the `spec.config_date` board when
  // present, else the symbol's earliest board. std::map keys by symbol so the
  // walk is deterministic (canonical-symbol order) and re-runnable.
  std::map<std::string, const CorpusBoard *> chosen;
  for (const CorpusBoard &board : boards) {
    const CorpusBoard *&slot = chosen[board.symbol];
    if (slot == nullptr) {
      slot = &board;
      continue;
    }
    const bool board_is_config_date = !spec.config_date.empty() && board.date == spec.config_date;
    const bool slot_is_config_date = !spec.config_date.empty() && slot->date == spec.config_date;
    if (board_is_config_date && !slot_is_config_date) {
      slot = &board; // prefer the requested config date
    } else if (board_is_config_date == slot_is_config_date && board.date < slot->date) {
      slot = &board; // tie on config-date match -> earliest date wins
    }
  }

  AutoConfigReport report;
  for (const auto &[symbol, board_ptr] : chosen) {
    ++report.n_symbols;

    // Idempotent by default: an already-configured symbol is left untouched so a
    // re-run never clobbers an operator override.
    if (!spec.overwrite_existing && db.symbol_config(symbol).has_value()) {
      ++report.n_skipped_existing;
      continue;
    }

    // The index leg is pinned to the dense recipe (shared with
    // populate_universe_streaming), bypassing per-board selection.
    if (!spec.index_symbol.empty() && symbol == spec.index_symbol) {
      const SymbolFitConfig cfg = seed_symbol_config(symbol, spec.preset, spec.index_symbol);
      if (const Status up = db.upsert_symbol(symbol, cfg); !up.has_value()) {
        return Err(up.error());
      }
      ++report.n_configured;
      continue;
    }

    SymbolFitConfig cfg;
    if (!config_for_board(*board_ptr, spec, cfg)) {
      // Fail closed: store the preset config DISABLED and record the symbol; the
      // top-level call still succeeds so one bad board never sinks the build.
      cfg = symbol_config_from_preset(spec.preset);
      cfg.enabled = false;
      if (const Status up = db.upsert_symbol(symbol, cfg); !up.has_value()) {
        return Err(up.error());
      }
      ++report.n_disabled_failed;
      report.failed_symbols.push_back(symbol);
      continue;
    }

    if (const Status up = db.upsert_symbol(symbol, cfg); !up.has_value()) {
      return Err(up.error());
    }
    ++report.n_configured;
  }

  std::sort(report.failed_symbols.begin(), report.failed_symbols.end());
  return Ok(std::move(report));
}

} // namespace atx::vol
