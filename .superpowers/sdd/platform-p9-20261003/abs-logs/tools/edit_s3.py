"""ABS S-3 (ruling D3W-S): one private Impl::weight_at for add and add_standardised."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from abs_edit import File  # noqa: E402

f = File("C:/atx-wt/pool-28/atx-impl/src/strategy_ic_composition.cpp")

f.once("""  std::vector<u8> row_weighted;
  IcCompositionResult result;
  usize next{};
  bool finished{};
};
namespace {
constexpr usize no_row = std::numeric_limits<usize>::max();
} // namespace
""", """  std::vector<u8> row_weighted;
  IcCompositionResult result;
  usize next{};
  bool finished{};
  // The weight candidate `index` enters with at date `d` (ruling D3W-S S-3: the one place add and
  // add_standardised read it): the weight of the per-date row in force (P9 lane D3W; 0 before the
  // first row), else its pinned (or default) weight.
  [[nodiscard]] f64 weight_at(usize index, usize d) const noexcept;
};
namespace {
constexpr usize no_row = std::numeric_limits<usize>::max();
} // namespace
f64 IcComposition::Impl::weight_at(usize index, usize d) const noexcept {
  if (row_of.empty()) return weights[index];
  const usize r = row_of[d];
  return r == no_row ? 0.0 : row_weights[r][index];
}
""")

f.once("""  if (!p.std_theme.empty()) return add_standardised(index, signal, sign, pool);
  const f64 weight = p.weights[index];
  const auto weight_at = [&p, index, dated, weight](usize d) {
    if (!dated) return weight;
    const usize r = p.row_of[d];
    return r == no_row ? 0.0 : p.row_weights[r][index];
  };
""", """  if (!p.std_theme.empty()) return add_standardised(index, signal, sign, pool);
""")

f.once("""    for (usize d = begin; d < end; ++d) {
      const f64 w = weight_at(d);
      if (dated && !(w > 0)) continue;
      row.clear(); const auto offset = d * p.cfg.instruments;
      for (usize i = 0; i < p.cfg.instruments; ++i)
        if (p.member[offset + i] && std::isfinite(signal[offset + i]))
          row.emplace_back(signal[offset + i], i);
      if (row.size() < 2) continue;
      cb::for_each_centered_rank(row, [&](usize i, f64 r) {
        blend[offset + i] += static_cast<f64>(sign) * w * r;""", """    for (usize d = begin; d < end; ++d) {
      const f64 w = p.weight_at(index, d);
      if (dated && !(w > 0)) continue;
      row.clear(); const auto offset = d * p.cfg.instruments;
      for (usize i = 0; i < p.cfg.instruments; ++i)
        if (p.member[offset + i] && std::isfinite(signal[offset + i]))
          row.emplace_back(signal[offset + i], i);
      if (row.size() < 2) continue;
      cb::for_each_centered_rank(row, [&](usize i, f64 r) {
        blend[offset + i] += static_cast<f64>(sign) * w * r;""")

f.once("""  auto& p = *impl_;
  const f64 weight = p.weights[index];
  f64* const plane = p.std_plane[p.std_theme[index]].data();
  // P9 lane D3W: the weight of the row in force at each date (as add); a zero-weight date makes no
  // cell of the plane present.
  const bool dated = !p.row_of.empty();
  const auto weight_at = [&p, index, dated, weight](usize d) {
    if (!dated) return weight;
    const usize r = p.row_of[d];
    return r == no_row ? 0.0 : p.row_weights[r][index];
  };
  const auto rank_dates = [&](usize begin, usize end, std::vector<Ranked>& row) {
    for (usize d = begin; d < end; ++d) {
      const f64 w = weight_at(d);""", """  auto& p = *impl_;
  f64* const plane = p.std_plane[p.std_theme[index]].data();
  // P9 lane D3W: the weight of the row in force at each date (Impl::weight_at, as add); a
  // zero-weight date makes no cell of the plane present.
  const bool dated = !p.row_of.empty();
  const auto rank_dates = [&](usize begin, usize end, std::vector<Ranked>& row) {
    for (usize d = begin; d < end; ++d) {
      const f64 w = p.weight_at(index, d);""")

f.save()
print("ok")
