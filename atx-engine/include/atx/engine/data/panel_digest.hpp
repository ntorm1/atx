#pragma once

// atx::engine::data — shared Panel digest helper (p3 S3-5 refactor).
//
// digest_panel(const alpha::Panel&) -> u64 is the determinism-pin oracle shared
// by real_panel.cpp (S1-5) and history_panel.cpp (S3-5). It hashes the Panel's
// fields in their canonical stored order (field-dictionary order) via
// signal_set_digest. Body is unchanged from the private static extracted from
// real_panel.cpp — the S1-5 golden digest (0x2a22a873483d9157) is byte-identical.

#include <span>
#include <string>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/parallel/digest.hpp" // signal_set_digest

namespace atx::engine::data {

// Digest the Panel's fields in canonical order: one SignalSet alpha per field
// (name = field name, values = the field's date-major column), in the Panel's
// field-dictionary order. This is signal_set_digest over the fields in the order
// the caller assembled them (the pin). Two Panels with identical shape, field
// order, and bit-identical values produce the same digest.
[[nodiscard]] inline atx::u64 digest_panel(const alpha::Panel &panel) {
  alpha::SignalSet ss;
  ss.dates = panel.dates();
  ss.instruments = panel.instruments();
  ss.alphas.reserve(panel.num_fields());
  for (atx::usize f = 0; f < panel.num_fields(); ++f) {
    const std::span<const atx::f64> col = panel.field_all(static_cast<alpha::FieldId>(f));
    ss.alphas.push_back(alpha::SignalSet::Alpha{std::string{panel.field_name(f)},
                                                std::vector<atx::f64>(col.begin(), col.end())});
  }
  return parallel::signal_set_digest(ss);
}

} // namespace atx::engine::data
