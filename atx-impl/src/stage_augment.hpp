#pragma once

// atx::impl — augment core (Track B1; p7 S2-1).
//
// augment_panel_with_finra is the PURE, axis-parametric core of the (future)
// `augment` subcommand: it appends the three FINRA short-interest features
// (si_dtc, si_util, si_chg) to a research Panel as NEW fields at the END,
// leaving every existing FieldId / column bitwise-unchanged. It is deliberately
// kept free of CLI I/O so it can be unit-tested directly with a synthetic Panel
// + synthetic FINRA parquet + an in-memory axis — no seg files, no symbology, no
// real download.
//
// CLI DEFERRAL (p7 decision D1): the `augment` CLI stage (run_augment) that
// reconstructs the panel axis from the ORATS seg partition + _symbology.parquet
// and writes research_aug.bin is owned by Sprint 7 (it reads RunConfig fields
// that do not exist until the S7 CLI hub lands). S2 ships only this pure core +
// its tests.
//
// The si_util shares denominator is derived from the panel itself: if BOTH
// `market_cap` and `raw_close` fields are present, shares = market_cap/raw_close
// per cell; otherwise si_util falls back to short/ADV inside the loader.

#include <span>
#include <string>
#include <unordered_map>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/dataset_schema.hpp" // DateKey, InstKey
#include "atx/engine/data/finra_short.hpp"    // kFinraDefaultPublicationLagDays

namespace atx::impl {

// Append si_dtc/si_util/si_chg to `panel` and return the augmented Panel.
//   panel        — the research panel (already read from research.bin).
//   finra_root   — FINRA short-interest hive root (date=YYYY-MM-DD/part-*.parquet).
//   panel_dates  — epoch-day axis matching panel.dates() (strictly ascending,
//                  panel.dates() entries).
//   sym_to_inst  — FINRA ticker -> panel instrument column (< panel.instruments()).
//   lag_days     — causal publication lag in calendar days.
//
// The three fields are appended in the order si_dtc, si_util, si_chg, so they
// land at FieldIds num_fields(), +1, +2. Err(InvalidArgument) if a name already
// collides, if the axis disagrees with the panel shape, or if the loader rejects
// the axis; Err(IoError) if the FINRA root cannot be read.
[[nodiscard]] atx::core::Result<atx::engine::alpha::Panel>
augment_panel_with_finra(const atx::engine::alpha::Panel &panel, const std::string &finra_root,
                         std::span<const atx::engine::data::DateKey> panel_dates,
                         const std::unordered_map<std::string, atx::engine::data::InstKey> &
                             sym_to_inst,
                         int lag_days = atx::engine::data::kFinraDefaultPublicationLagDays);

} // namespace atx::impl
