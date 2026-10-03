// The 13F stage reader, holdings half: the row screen, the quarter's position rows from the
// holdings parts, quarter positions and the share-basis factors (research_fields_holdings.py
// F13_ROWS_RULE; research_fields_mgr13f.py quarter_rows, quarter_positions, churn's BASIS_RULE).
// Declared in thirteenf.hpp.
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

#include "atx/engine/research/fields/sources/parquet_columns.hpp"
#include "atx/engine/research/fields/sources/thirteenf.hpp"

namespace atx::engine::research::fields {
namespace {

constexpr std::string_view kShares = "SH";
constexpr std::array<std::string_view, 6> kHoldingColumns{
    "accession", "cusip", "shares", "sshprnamt_type", "put_call", "value_usd"};

// Per key (ascending): the median of its values ((v[(c-1)/2] + v[c/2]) / 2 over the sorted values,
// research_fields_holdings.py group_median) and the count.
struct GroupMedians {
  std::vector<i64> keys;
  std::vector<f64> medians;
  std::vector<usize> counts;
};

[[nodiscard]] GroupMedians group_medians(std::vector<std::pair<i64, f64>> pairs) {
  std::sort(pairs.begin(), pairs.end());
  GroupMedians out;
  usize start = 0;
  while (start < pairs.size()) {
    usize end = start + 1;
    while (end < pairs.size() && pairs[end].first == pairs[start].first) {
      ++end;
    }
    const usize count = end - start;
    out.keys.push_back(pairs[start].first);
    out.medians.push_back(
        (pairs[start + (count - 1) / 2].second + pairs[start + count / 2].second) / 2.0);
    out.counts.push_back(count);
    start = end;
  }
  return out;
}

// The position of `key` in ascending `keys`, nullopt when absent.
[[nodiscard]] std::optional<usize> find_key(const std::vector<i64> &keys, i64 key) noexcept {
  const auto it = std::lower_bound(keys.begin(), keys.end(), key);
  if (it == keys.end() || *it != key) {
    return std::nullopt;
  }
  return static_cast<usize>(it - keys.begin());
}

[[nodiscard]] f64 implied_price(const PositionRow &r) noexcept { return r.value / r.shares; }

// Pairs (filer, security) of rows sorted by that key, summed in row order; filer totals summed in
// pair order (the numpy bincount orders of quarter_positions).
void sum_pairs(const std::vector<PositionRow> &kept, QuarterPositions &out) {
  usize start = 0;
  while (start < kept.size()) {
    usize end = start + 1;
    f64 shares = kept[start].shares;
    f64 value = kept[start].value;
    while (end < kept.size() && kept[end].filer == kept[start].filer &&
           kept[end].sid == kept[start].sid) {
      shares += kept[end].shares;
      value += kept[end].value;
      ++end;
    }
    out.pair_filer.push_back(kept[start].filer);
    out.pair_sid.push_back(kept[start].sid);
    out.pair_shares.push_back(shares);
    out.pair_value.push_back(value);
    start = end;
  }
  for (usize p = 0; p < out.pair_filer.size(); ++p) {
    if (out.filers.empty() || out.filers.back() != out.pair_filer[p]) {
      out.filers.push_back(out.pair_filer[p]);
      out.filer_value.push_back(0.0);
    }
    out.filer_value.back() += out.pair_value[p];
  }
}

} // namespace

QuarterFilers quarter_filers(const ThirteenfIndex &index, usize q) {
  std::vector<usize> order = index.quarters[q].effective;
  std::sort(order.begin(), order.end(), [&index](usize a, usize b) {
    return index.filings[a].accession < index.filings[b].accession;
  });
  QuarterFilers out;
  for (const usize i : order) {
    const ThirteenfFiling &f = index.filings[i];
    out.accession.push_back(f.accession);
    out.filer.push_back(f.filer);
    out.factor.push_back(index.unit_factor_of(f.accession));
  }
  return out;
}

std::optional<PositionRow> screen_holding(const QuarterFilers &filers,
                                          const ThirteenfQuarter &quarter,
                                          const HoldingRow &row) {
  const auto at = std::lower_bound(
      filers.accession.begin(), filers.accession.end(), row.accession,
      [](const std::string &a, std::string_view key) { return std::string_view(a) < key; });
  if (at == filers.accession.end() || std::string_view(*at) != row.accession) {
    return std::nullopt;
  }
  if (row.sshprnamt_type != kShares || !row.put_call.empty()) {
    return std::nullopt;
  }
  if (!(std::isfinite(row.shares) && row.shares > 0.0 && std::isfinite(row.value) &&
        row.value >= 0.0)) {
    return std::nullopt;
  }
  const auto k = static_cast<usize>(at - filers.accession.begin());
  i64 sid = -1;
  const auto &map = quarter.cusip_sid;
  const auto hit = std::lower_bound(
      map.begin(), map.end(), row.cusip,
      [](const std::pair<std::string, i64> &a, std::string_view key) {
        return std::string_view(a.first) < key;
      });
  if (hit != map.end() && std::string_view(hit->first) == row.cusip) {
    sid = hit->second;
  }
  return PositionRow{filers.filer[k], sid, row.shares, row.value * filers.factor[k]};
}

core::Result<std::vector<PositionRow>> read_quarter_rows(PinnedStage &stage,
                                                         const ThirteenfIndex &index, usize q,
                                                         ThirteenfCounts &counts) {
  std::vector<PositionRow> rows;
  const ThirteenfQuarter &quarter = index.quarters[q];
  if (!quarter.has_filing || quarter.sealed || quarter.effective.empty()) {
    return core::Ok(std::move(rows));
  }
  const QuarterFilers filers = quarter_filers(index, q);
  std::vector<std::string> parts;
  for (const usize i : quarter.effective) {
    parts.push_back(index.filings[i].source_period);
  }
  std::sort(parts.begin(), parts.end());
  parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
  const auto visit = [&filers, &quarter, &rows](const core::io::ParquetTable &t) -> core::Status {
    ATX_TRY(const auto accession, t.strings("accession"));
    ATX_TRY(const auto cusip, t.strings("cusip"));
    ATX_TRY(const auto type, t.strings("sshprnamt_type"));
    ATX_TRY(const auto put_call, t.strings("put_call"));
    ATX_TRY(const auto shares, real_column(t, "shares"));
    ATX_TRY(const auto value, real_column(t, "value_usd"));
    const usize n = accession.size();
    if (cusip.size() != n || type.size() != n || put_call.size() != n || shares.size() != n ||
        value.size() != n) {
      return core::Err(core::ErrorCode::Internal, "thirteenf holdings: column lengths differ");
    }
    for (usize i = 0; i < n; ++i) {
      const HoldingRow row{accession[i], cusip[i], type[i], put_call[i], shares[i], value[i]};
      if (const auto kept = screen_holding(filers, quarter, row)) {
        rows.push_back(*kept);
      }
    }
    return core::Ok();
  };
  for (const std::string &part : parts) {
    if (thirteenf_part_is_sealed(part)) { // filed on or after the seal: never opened
      ++counts.holdings_parts_not_read_sealed;
      continue;
    }
    const std::string rel = "parts/source=" + part + "/holdings.parquet";
    ATX_TRY_VOID(stage.for_each_group(rel, kHoldingColumns, visit));
  }
  counts.holdings_rows_kept += static_cast<u64>(rows.size());
  return core::Ok(std::move(rows));
}

QuarterPositions quarter_positions(std::vector<PositionRow> rows, f64 outlier) {
  QuarterPositions out;
  const auto unmapped = [](const PositionRow &r) { return r.sid < 0; };
  rows.erase(std::remove_if(rows.begin(), rows.end(), unmapped), rows.end());
  out.rows_mapped = static_cast<u64>(rows.size());
  std::vector<std::pair<i64, f64>> priced;
  for (const PositionRow &r : rows) {
    if (r.value > 0.0) {
      priced.emplace_back(r.sid, implied_price(r));
    }
  }
  const GroupMedians reference = group_medians(std::move(priced));
  std::vector<PositionRow> kept;
  kept.reserve(rows.size());
  for (const PositionRow &r : rows) {
    if (r.value > 0.0) {
      const f64 ratio = implied_price(r) / reference.medians[*find_key(reference.keys, r.sid)];
      if (ratio > outlier || ratio < 1.0 / outlier) {
        ++out.rows_price_outlier;
        continue;
      }
    }
    kept.push_back(r);
  }
  std::stable_sort(kept.begin(), kept.end(), [](const PositionRow &a, const PositionRow &b) {
    return std::tie(a.filer, a.sid) < std::tie(b.filer, b.sid);
  });
  sum_pairs(kept, out);
  std::vector<std::pair<i64, f64>> kept_priced;
  for (const PositionRow &r : kept) {
    if (r.value > 0.0) {
      kept_priced.emplace_back(r.sid, implied_price(r));
    }
  }
  GroupMedians prices = group_medians(std::move(kept_priced));
  out.price_sid = std::move(prices.keys);
  out.price = std::move(prices.medians);
  return out;
}

f64 price_of(const QuarterPositions &positions, i64 sid) noexcept {
  const auto at = find_key(positions.price_sid, sid);
  return at ? positions.price[*at] : std::numeric_limits<f64>::quiet_NaN();
}

std::vector<f64> basis_factors(const QuarterPositions &prev, const QuarterPositions &cur,
                               std::span<const i64> sids) {
  std::vector<std::pair<i64, f64>> ratios; // (security, N_q / N_(q-1)) of pairs held in both
  usize a = 0;
  usize b = 0;
  while (a < prev.pair_filer.size() && b < cur.pair_filer.size()) {
    const auto ka = std::tie(prev.pair_filer[a], prev.pair_sid[a]);
    const auto kb = std::tie(cur.pair_filer[b], cur.pair_sid[b]);
    if (ka < kb) {
      ++a;
    } else if (kb < ka) {
      ++b;
    } else {
      ratios.emplace_back(prev.pair_sid[a], cur.pair_shares[b] / prev.pair_shares[a]);
      ++a;
      ++b;
    }
  }
  const GroupMedians s = group_medians(std::move(ratios));
  const f64 moved_bound = std::log(kBasisShareTolerance);
  const f64 offset_bound = std::log(kBasisValueTolerance);
  std::vector<f64> out(sids.size(), 1.0);
  for (usize i = 0; i < sids.size(); ++i) {
    const auto at = find_key(s.keys, sids[i]);
    if (!at || s.counts[*at] < kBasisMinHolders) {
      continue;
    }
    const f64 median = s.medians[*at];
    const f64 offset =
        std::fabs(std::log(median * price_of(cur, sids[i]) / price_of(prev, sids[i])));
    if (std::fabs(std::log(median)) > moved_bound && offset <= offset_bound) {
      out[i] = median;
    }
  }
  return out;
}

} // namespace atx::engine::research::fields
