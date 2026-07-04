// atx::engine::data — incremental history-panel append (p7 S6-1).
//
// Implements append_history_panel (declared in history_panel.hpp): extend an
// already-built Panel with new per-date segments so a single new trading day no
// longer forces a from-scratch rebuild ergonomically, while remaining
// BYTE-IDENTICAL to a full rebuild over the combined range (the S6-1 gate).
//
// See the header doc-comment for the full contract, the causal-ADV determinism
// argument, and the documented in-memory-splice limitation (alpha::Panel carries
// no symbol/date axis). This unit lives in atx-impl (the owned source set) rather
// than atx-engine/src/data so the sprint stays inside its file fence; it defines a
// function in namespace atx::engine::data, which is legal from any TU.

#include "atx/engine/data/history_panel.hpp"

#include <algorithm> // std::sort, std::unique
#include <cmath>     // std::isnan
#include <filesystem>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp" // alpha::TimeWindow
#include "atx/engine/data/panel_digest.hpp"   // digest_panel

#include "atx/tsdb/segment_reader.hpp" // SegmentReader::attach, times()

namespace atx::engine::data {

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// Collect the unique, ascending in-window date stamps (unix nanos) of every
// `*.seg` file in `dir`. A date is in-window iff window.start <= t < window.end.
// Returns Err(InvalidArgument) if the directory cannot be read. An EMPTY result
// (no .seg files, or none in-window) is a valid, non-error outcome — the caller
// distinguishes the no-op (no new dates) from a hard failure.
[[nodiscard]] Result<std::vector<atx::i64>>
in_window_dates(const std::string &dir, const alpha::TimeWindow &window) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<std::string> paths;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".seg") {
      paths.push_back(entry.path().string());
    }
  }
  if (ec) {
    return Err(ErrorCode::InvalidArgument,
               "append_history_panel: cannot read seg directory '" + dir + "': " + ec.message());
  }

  std::vector<atx::i64> dates;
  for (const std::string &p : paths) {
    ATX_TRY(auto rdr, atx::tsdb::SegmentReader::attach(p));
    for (const atx::i64 t : rdr.times()) {
      if (t >= window.start_nanos && t < window.end_nanos) {
        dates.push_back(t);
      }
    }
  }
  std::sort(dates.begin(), dates.end());
  dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
  return Ok(std::move(dates));
}

// Wrap an owned Panel as a HistoryPanel result (digest re-derived from the Panel;
// lineage left empty — the no-op return path carries no new derivation record).
[[nodiscard]] HistoryPanel as_history_panel(alpha::Panel panel) {
  const atx::u64 digest = digest_panel(panel);
  return HistoryPanel{std::move(panel), digest, /*lineage=*/{}};
}

// Verify the rebuilt combined panel's first `d_old` date rows are byte-identical
// to `existing` over existing's instrument columns (a column prefix of the
// combined axis). NaN is treated as equal to NaN (bit-stable missing cells). This
// is the determinism self-check: a true append is byte-identical to a full
// rebuild, so any mismatch here is a latent divergence we surface loudly.
[[nodiscard]] bool prefix_matches(const alpha::Panel &existing, const alpha::Panel &combined) {
  const atx::usize d_old = existing.dates();
  const atx::usize n_old = existing.instruments();
  const atx::usize n_comb = combined.instruments();
  const atx::usize f = existing.num_fields();
  if (combined.num_fields() != f) {
    return false;
  }
  if (combined.dates() < d_old || n_comb < n_old) {
    return false;
  }
  for (atx::usize fi = 0; fi < f; ++fi) {
    if (existing.field_name(fi) != combined.field_name(static_cast<alpha::FieldId>(fi))) {
      return false;
    }
    const std::span<const atx::f64> ec = existing.field_all(static_cast<alpha::FieldId>(fi));
    const std::span<const atx::f64> cc = combined.field_all(static_cast<alpha::FieldId>(fi));
    for (atx::usize d = 0; d < d_old; ++d) {
      for (atx::usize i = 0; i < n_old; ++i) {
        const atx::f64 a = ec[d * n_old + i];
        const atx::f64 b = cc[d * n_comb + i];
        if (std::isnan(a) != std::isnan(b)) {
          return false;
        }
        if (!std::isnan(a) && a != b) {
          return false;
        }
      }
    }
  }
  for (atx::usize d = 0; d < d_old; ++d) {
    for (atx::usize i = 0; i < n_old; ++i) {
      if (existing.in_universe(d, i) != combined.in_universe(d, i)) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

atx::core::Result<HistoryPanel>
append_history_panel(const alpha::Panel &existing, const std::string &new_seg_dir,
                     const HistoryDataConfig &combined_cfg) {
  // Column compaction is a whole-window decision with no stable incremental
  // meaning (a never-in-universe column could flip once new dates arrive,
  // changing the kept set). Refuse it rather than risk a non-identical splice.
  if (combined_cfg.compact_to_universe) {
    return Err(ErrorCode::InvalidArgument,
               "append_history_panel: compact_to_universe is unsupported on the append path "
               "(append on an uncompacted panel)");
  }

  // New dates present in the new-only partition (within the combined window).
  ATX_TRY(const std::vector<atx::i64> new_dates,
          in_window_dates(new_seg_dir, combined_cfg.window));

  // No new in-window dates -> exact no-op: return `existing` unchanged.
  if (new_dates.empty()) {
    return Ok(as_history_panel(existing));
  }

  // Existing panel's last date stamp: the (d_old-1)-th date of the COMBINED
  // partition (the combined build's first d_old rows are exactly the existing
  // range, in ascending date order). An empty existing panel cannot anchor an
  // append.
  const atx::usize d_old = existing.dates();
  if (d_old == 0) {
    return Err(ErrorCode::InvalidArgument,
               "append_history_panel: existing panel has zero dates (nothing to append onto)");
  }
  ATX_TRY(const std::vector<atx::i64> combined_dates,
          in_window_dates(combined_cfg.seg_dir, combined_cfg.window));
  if (combined_dates.size() < d_old) {
    return Err(ErrorCode::InvalidArgument,
               "append_history_panel: combined partition has fewer dates than the existing panel "
               "(combined_cfg.seg_dir must contain the existing segments too)");
  }
  const atx::i64 existing_last_date = combined_dates[d_old - 1];

  // Strictly-after ordering: every new date must post-date the existing panel.
  // An overlapping or out-of-order new segment is a caller error (fail closed).
  for (const atx::i64 t : new_dates) {
    if (t <= existing_last_date) {
      return Err(ErrorCode::InvalidArgument,
                 "append_history_panel: a new segment date is <= the existing panel's last date "
                 "(new segments must be strictly after the existing range)");
    }
  }

  // Build the combined panel (byte-identical full-rebuild semantics over the
  // union range). This is the authoritative, deterministic path.
  ATX_TRY(HistoryPanel combined, build_history_panel(combined_cfg));

  // Determinism self-check: the rebuild must reproduce `existing` as its date
  // prefix. If not, an invariant the append relies on (causal fields, stable
  // axis) has been violated upstream — refuse rather than emit a divergent panel.
  if (!prefix_matches(existing, combined.panel)) {
    return Err(ErrorCode::Internal,
               "append_history_panel: rebuilt prefix is not byte-identical to the existing panel "
               "(determinism invariant violated — refusing to emit a divergent panel)");
  }
  if (combined.panel.dates() <= d_old) {
    return Err(ErrorCode::Internal,
               "append_history_panel: combined rebuild did not extend past the existing panel");
  }

  return Ok(std::move(combined));
}

} // namespace atx::engine::data
