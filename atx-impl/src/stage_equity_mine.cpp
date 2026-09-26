#include "stage_equity_mine.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/augment.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/data/point_in_time_universe.hpp"
#include "atx/engine/eval/deflated_sharpe.hpp"
#include "atx/engine/eval/stats_ext.hpp"
#include "atx/engine/factory/sketch_index.hpp"
#include "atx/engine/factory/canonical.hpp"
#include "atx/engine/factory/ic_screen.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "artifacts.hpp"
#include "config.hpp"
#include "dispatch.hpp"
#include "panel_artifact.hpp"
#include "research_sim.hpp"
#include "stage_data_provenance.hpp"

namespace atx::impl {

namespace mine {

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace alpha = atx::engine::alpha;
namespace eval = atx::engine::eval;
namespace factory = atx::engine::factory;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] bool same_bits(atx::f64 a, atx::f64 b) noexcept {
    if (std::isnan(a) && std::isnan(b)) return true;
    return std::bit_cast<atx::u64>(a) == std::bit_cast<atx::u64>(b);
}

// Average ranks (1-based) of `v` in place order; ties share their mean rank.
void average_ranks(std::span<const atx::f64> v, std::vector<atx::usize> &order,
                   std::vector<atx::f64> &ranks) {
    const atx::usize n = v.size();
    order.resize(n);
    std::iota(order.begin(), order.end(), atx::usize{0});
    std::sort(order.begin(), order.end(), [&](atx::usize a, atx::usize b) {
        return v[a] < v[b] || (v[a] == v[b] && a < b);
    });
    ranks.assign(n, 0.0);
    atx::usize i = 0;
    while (i < n) {
        atx::usize j = i + 1;
        while (j < n && v[order[j]] == v[order[i]]) ++j;
        const atx::f64 r = 0.5 * static_cast<atx::f64>(i + 1 + j); // mean of i+1..j
        for (atx::usize k = i; k < j; ++k) ranks[order[k]] = r;
        i = j;
    }
}

[[nodiscard]] atx::f64 pearson(std::span<const atx::f64> x, std::span<const atx::f64> y) noexcept {
    const atx::usize n = x.size();
    if (n < 3) return kNaN;
    atx::f64 mx = 0.0, my = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
        mx += x[i];
        my += y[i];
    }
    mx /= static_cast<atx::f64>(n);
    my /= static_cast<atx::f64>(n);
    atx::f64 sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
        syy += (y[i] - my) * (y[i] - my);
    }
    if (!(sxx > 0.0) || !(syy > 0.0)) return kNaN;
    return sxy / std::sqrt(sxx * syy);
}

[[nodiscard]] atx::f64 simple_return(std::span<const atx::f64> close, atx::usize I, atx::usize from,
                                     atx::usize to, atx::usize inst) noexcept {
    const atx::f64 a = close[from * I + inst];
    const atx::f64 b = close[to * I + inst];
    if (!std::isfinite(a) || !std::isfinite(b) || !(a > 0.0) || !(b > 0.0)) return kNaN;
    return b / a - 1.0;
}

[[nodiscard]] atx::f64 mean_of(std::span<const atx::f64> x) noexcept {
    if (x.empty()) return 0.0;
    atx::f64 s = 0.0;
    for (auto v : x) s += v;
    return s / static_cast<atx::f64>(x.size());
}

[[nodiscard]] atx::f64 sample_sd(std::span<const atx::f64> x) noexcept {
    if (x.size() < 2) return 0.0;
    const atx::f64 m = mean_of(x);
    atx::f64 ss = 0.0;
    for (auto v : x) ss += (v - m) * (v - m);
    return std::sqrt(ss / static_cast<atx::f64>(x.size() - 1));
}

// Newey-West t statistic of the mean of x (Bartlett weights, `lags` lags).
[[nodiscard]] atx::f64 newey_west_t(std::span<const atx::f64> x, atx::usize lags) noexcept {
    const atx::usize T = x.size();
    if (T < 3) return 0.0;
    const atx::f64 m = mean_of(x);
    auto gamma = [&](atx::usize l) {
        atx::f64 s = 0.0;
        for (atx::usize t = l; t < T; ++t) s += (x[t] - m) * (x[t - l] - m);
        return s / static_cast<atx::f64>(T);
    };
    atx::f64 lrv = gamma(0);
    const atx::usize L = std::min(lags, T - 1);
    for (atx::usize l = 1; l <= L; ++l) {
        const atx::f64 w = 1.0 - static_cast<atx::f64>(l) / static_cast<atx::f64>(L + 1);
        lrv += 2.0 * w * gamma(l);
    }
    if (!(lrv > 0.0)) return 0.0;
    return m / std::sqrt(lrv / static_cast<atx::f64>(T));
}

} // namespace

// ===========================================================================
// Span construction
// ===========================================================================

atx::core::Result<SpanPanel> stitch_span(std::span<const SpanSource> sources) {
    if (sources.empty()) return Err(ErrorCode::InvalidArgument, "stitch_span: no sources");
    const alpha::Panel &p0 = sources.front().panel;
    const atx::usize F = p0.num_fields();
    std::vector<std::string> names;
    names.reserve(F);
    for (atx::usize f = 0; f < F; ++f) names.emplace_back(p0.field_name(f));
    std::vector<atx::i64> keys;
    for (atx::usize s = 0; s < sources.size(); ++s) {
        const SpanSource &src = sources[s];
        if (src.panel.num_fields() != F) {
            return Err(ErrorCode::InvalidArgument, "stitch_span: field count differs in source " +
                                                       std::to_string(s));
        }
        for (atx::usize f = 0; f < F; ++f) {
            if (src.panel.field_name(f) != names[f]) {
                return Err(ErrorCode::InvalidArgument,
                           "stitch_span: field '" + names[f] + "' differs in source " +
                               std::to_string(s));
            }
        }
        if (src.session_keys.size() != src.panel.dates() ||
            src.instrument_ids.size() != src.panel.instruments()) {
            return Err(ErrorCode::InvalidArgument, "stitch_span: axes/panel shape mismatch");
        }
        for (atx::usize d = 1; d < src.session_keys.size(); ++d) {
            if (src.session_keys[d] <= src.session_keys[d - 1]) {
                return Err(ErrorCode::InvalidArgument,
                           "stitch_span: session keys not strictly ascending");
            }
        }
        keys.insert(keys.end(), src.session_keys.begin(), src.session_keys.end());
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    std::vector<atx::i64> instrument_ids;
    std::vector<atx::u32> owner_source;
    atx::usize overlap_cells = 0;
    atx::usize overlap_mismatch_cells = 0;
    std::unordered_map<atx::i64, atx::usize> col_of;
    for (const SpanSource &src : sources) {
        for (const atx::i64 id : src.instrument_ids) {
            if (col_of.emplace(id, instrument_ids.size()).second) instrument_ids.push_back(id);
        }
    }
    const atx::usize D = keys.size();
    const atx::usize I = instrument_ids.size();
    const atx::usize cells = D * I;
    std::vector<std::vector<atx::f64>> data(F, std::vector<atx::f64>(cells, kNaN));
    std::vector<std::uint8_t> universe(cells, 0);
    std::vector<std::uint8_t> filled(cells, 0);
    owner_source.assign(D, std::numeric_limits<atx::u32>::max());

    for (atx::usize s = 0; s < sources.size(); ++s) {
        const SpanSource &src = sources[s];
        const atx::usize sI = src.instrument_ids.size();
        std::vector<atx::usize> cols(sI);
        for (atx::usize i = 0; i < sI; ++i) cols[i] = col_of.at(src.instrument_ids[i]);
        std::vector<std::span<const atx::f64>> fcols(F);
        for (atx::usize f = 0; f < F; ++f) {
            fcols[f] = src.panel.field_all(static_cast<alpha::FieldId>(f));
        }
        for (atx::usize sd = 0; sd < src.session_keys.size(); ++sd) {
            const auto it = std::lower_bound(keys.begin(), keys.end(), src.session_keys[sd]);
            const auto d = static_cast<atx::usize>(it - keys.begin());
            if (owner_source[d] == std::numeric_limits<atx::u32>::max()) {
                owner_source[d] = static_cast<atx::u32>(s);
            }
            for (atx::usize si = 0; si < sI; ++si) {
                const atx::usize c = d * I + cols[si];
                const atx::usize sc = sd * sI + si;
                if (filled[c] == 0) {
                    filled[c] = 1;
                    for (atx::usize f = 0; f < F; ++f) data[f][c] = fcols[f][sc];
                    universe[c] = src.panel.in_universe(sd, si) ? 1 : 0;
                    continue;
                }
                ++overlap_cells;
                for (atx::usize f = 0; f < F; ++f) {
                    if (!same_bits(data[f][c], fcols[f][sc])) {
                        ++overlap_mismatch_cells;
                        break;
                    }
                }
            }
        }
    }
    ATX_TRY(auto panel, alpha::Panel::create(D, I, std::move(names), std::move(data),
                                             std::move(universe)));
    return Ok(SpanPanel{std::move(panel), std::move(keys), std::move(instrument_ids),
                        std::move(owner_source), overlap_cells, overlap_mismatch_cells});
}

atx::core::Result<std::vector<atx::u8>>
asof_membership_mask(const atx::engine::data::PitMembershipImage &image, atx::usize cut,
                     std::span<const atx::i64> session_keys,
                     std::span<const atx::i64> instrument_ids) {
    for (const auto &r : image.rebalances) {
        if (cut >= r.cuts.size()) {
            return Err(ErrorCode::InvalidArgument,
                       "asof_membership_mask: cut " + std::to_string(cut) + " out of range");
        }
    }
    if (image.rebalances.empty()) {
        return Err(ErrorCode::InvalidArgument, "asof_membership_mask: no rebalances");
    }
    std::vector<atx::usize> order(image.rebalances.size());
    std::iota(order.begin(), order.end(), atx::usize{0});
    std::stable_sort(order.begin(), order.end(), [&](atx::usize a, atx::usize b) {
        return image.rebalances[a].effective_session_key <
               image.rebalances[b].effective_session_key;
    });
    const atx::usize D = session_keys.size();
    const atx::usize I = instrument_ids.size();
    std::vector<atx::u8> mask(D * I, 0);
    std::vector<atx::u8> row(I, 0);
    atx::usize next = 0;
    bool active = false;
    for (atx::usize d = 0; d < D; ++d) {
        bool changed = false;
        while (next < order.size() &&
               image.rebalances[order[next]].effective_session_key <= session_keys[d]) {
            ++next;
            changed = true;
        }
        if (changed) {
            const auto &ids = image.rebalances[order[next - 1]].cuts[cut].security_ids;
            for (atx::usize i = 0; i < I; ++i) {
                row[i] = std::binary_search(ids.begin(), ids.end(), instrument_ids[i]) ? 1 : 0;
            }
            active = true;
        }
        if (active) std::copy(row.begin(), row.end(), mask.begin() + static_cast<std::ptrdiff_t>(d * I));
    }
    return Ok(std::move(mask));
}

std::vector<atx::u8> owner_union_mask(const SpanPanel &span, std::span<const SpanSource> sources) {
    const atx::usize D = span.session_keys.size();
    const atx::usize I = span.instrument_ids.size();
    std::vector<std::vector<atx::u8>> in_source(sources.size(), std::vector<atx::u8>(I, 0));
    for (atx::usize s = 0; s < sources.size(); ++s) {
        std::unordered_set<atx::i64> ids(sources[s].instrument_ids.begin(),
                                         sources[s].instrument_ids.end());
        for (atx::usize i = 0; i < I; ++i) in_source[s][i] = ids.count(span.instrument_ids[i]) ? 1 : 0;
    }
    std::vector<atx::u8> mask(D * I, 0);
    for (atx::usize d = 0; d < D; ++d) {
        const atx::u32 o = span.owner_source[d];
        if (o >= sources.size()) continue;
        std::copy(in_source[o].begin(), in_source[o].end(),
                  mask.begin() + static_cast<std::ptrdiff_t>(d * I));
    }
    return mask;
}

// ===========================================================================
// Scoring
// ===========================================================================

void finalize_score(SignalScore &s, const ScoreCfg &cfg) {
    const atx::f64 ann = std::sqrt(cfg.periods_per_year);
    const atx::f64 sg = sample_sd(s.gross);
    const atx::f64 sn = sample_sd(s.net);
    s.sharpe_gross = sg > 0.0 ? mean_of(s.gross) / sg * ann : 0.0;
    s.mean_net = mean_of(s.net);
    s.sharpe_net = sn > 0.0 ? s.mean_net / sn * ann : 0.0;
    s.t_nw = newey_west_t(s.net, cfg.nw_lags);
    s.p_one_sided = 0.5 * std::erfc(s.t_nw / std::sqrt(2.0));
    s.mean_turnover = mean_of(s.turnover);
    std::vector<atx::f64> ic;
    ic.reserve(s.ic.size());
    for (auto v : s.ic) {
        if (std::isfinite(v)) ic.push_back(v);
    }
    s.ic_mean[0] = ic.empty() ? 0.0 : mean_of(ic);
    const atx::f64 isd = sample_sd(ic);
    s.icir = isd > 0.0 ? s.ic_mean[0] / isd : 0.0;
}

SignalScore flip_score(const SignalScore &in, const ScoreCfg &cfg) {
    SignalScore s = in;
    for (atx::usize t = 0; t < s.gross.size(); ++t) {
        const atx::f64 cost = in.gross[t] - in.net[t];
        s.gross[t] = -in.gross[t];
        s.net[t] = s.gross[t] - cost;
    }
    for (auto &v : s.ic) v = -v;
    for (auto &v : s.ic_mean) v = -v;
    finalize_score(s, cfg);
    return s;
}

atx::usize ReturnGuard::count_in(atx::usize begin, atx::usize end) const noexcept {
    atx::usize n = 0;
    for (const Excluded &e : excluded) n += (e.date >= begin && e.date < end) ? 1 : 0;
    return n;
}

atx::core::Result<ReturnGuard> build_return_guard(const alpha::Panel &panel, atx::u32 close_field,
                                                  const ScoreCfg &cfg) {
    if (close_field >= panel.num_fields()) {
        return Err(ErrorCode::InvalidArgument, "build_return_guard: bad close field");
    }
    const atx::usize D = panel.dates();
    const atx::usize I = panel.instruments();
    ReturnGuard g;
    g.dates = D;
    g.instruments = I;
    g.bad_prefix.assign(D * I, 0);
    if (!cfg.guard_returns || D == 0) return Ok(std::move(g));
    const auto close = panel.field_all(close_field);
    std::span<const atx::f64> raw;
    if (!cfg.raw_close_field.empty()) {
        if (auto rid = panel.field_id(cfg.raw_close_field)) {
            raw = panel.field_all(*rid);
            g.has_raw = true;
        }
    }
    const auto log_ret = [](std::span<const atx::f64> px, atx::usize a, atx::usize b) {
        const atx::f64 x = px[a];
        const atx::f64 y = px[b];
        if (!std::isfinite(x) || !std::isfinite(y) || !(x > 0.0) || !(y > 0.0)) return kNaN;
        return std::log(y / x);
    };
    const bool cap_on = cfg.max_abs_log_return > 0.0;
    for (atx::usize d = 1; d < D; ++d) {
        for (atx::usize i = 0; i < I; ++i) {
            const atx::usize c = d * I + i;
            const atx::usize p = (d - 1) * I + i;
            atx::u32 bad = 0;
            const atx::f64 la = log_ret(close, p, c);
            if (std::isfinite(la)) {
                const atx::f64 lr = g.has_raw ? log_ret(raw, p, c) : kNaN;
                const bool cap = cap_on && std::abs(la) > cfg.max_abs_log_return;
                const bool disagree =
                    std::isfinite(lr) && std::abs(la) > std::abs(lr) + cfg.adj_raw_log_tol;
                if (cap || disagree) {
                    bad = 1;
                    g.excluded.push_back(ReturnGuard::Excluded{
                        d, i, std::expm1(la), std::isfinite(lr) ? std::expm1(lr) : kNaN, cap});
                }
            }
            g.bad_prefix[c] = g.bad_prefix[p] + bad;
        }
    }
    return Ok(std::move(g));
}

atx::core::Result<SignalScore> score_signal(std::span<const atx::f64> signal, atx::f64 sign,
                                            const alpha::Panel &panel, atx::u32 close_field,
                                            std::span<const atx::u8> member, EvalWindow window,
                                            const ScoreCfg &cfg, const ReturnGuard *guard) {
    const atx::usize D = panel.dates();
    const atx::usize I = panel.instruments();
    if (signal.size() != D * I || member.size() != D * I) {
        return Err(ErrorCode::InvalidArgument, "score_signal: signal/member shape mismatch");
    }
    if (window.begin > window.end || window.end > D) {
        return Err(ErrorCode::InvalidArgument, "score_signal: window out of range");
    }
    if (close_field >= panel.num_fields() || (sign != 1.0 && sign != -1.0)) {
        return Err(ErrorCode::InvalidArgument, "score_signal: bad close field or sign");
    }
    const auto close = panel.field_all(close_field);
    ReturnGuard local_guard;
    if (cfg.guard_returns && guard == nullptr) {
        ATX_TRY(local_guard, build_return_guard(panel, close_field, cfg));
        guard = &local_guard;
    }
    if (!cfg.guard_returns) guard = nullptr;
    if (guard != nullptr && (guard->dates != D || guard->instruments != I ||
                             guard->bad_prefix.size() != D * I)) {
        return Err(ErrorCode::InvalidArgument, "score_signal: return guard shape mismatch");
    }
    const atx::usize T = window.size();
    const atx::f64 cost_rate = cfg.cost_bps * 1e-4;
    SignalScore s;
    s.gross.assign(T, 0.0);
    s.net.assign(T, 0.0);
    s.turnover.assign(T, 0.0);
    s.ic.assign(T, kNaN);
    std::array<atx::f64, 3> ic_sum{};
    std::array<atx::usize, 3> ic_n{};
    std::vector<atx::f64> prev(I, 0.0), cur(I, 0.0);
    std::vector<atx::f64> vals, ranks, xs, ys, xr, yr;
    std::vector<atx::usize> names, order;
    atx::usize traded = 0;
    atx::f64 names_sum = 0.0;

    for (atx::usize t = 0; t < T; ++t) {
        const atx::usize d = window.begin + t;
        const atx::usize entry = d + cfg.delay; // trade close
        if (entry + 1 >= D) continue;           // unscorable: realized return beyond the span
        vals.clear();
        names.clear();
        for (atx::usize i = 0; i < I; ++i) {
            const atx::usize c = d * I + i;
            const atx::f64 v = signal[c];
            const atx::f64 px = close[c];
            if (member[c] == 0 || !std::isfinite(v) || !std::isfinite(px) || !(px > 0.0)) continue;
            vals.push_back(sign * v);
            names.push_back(i);
        }
        std::fill(cur.begin(), cur.end(), 0.0);
        bool trade = names.size() >= cfg.min_names && names.size() >= 2;
        if (trade) {
            average_ranks(vals, order, ranks);
            const atx::f64 mid = 0.5 * static_cast<atx::f64>(names.size() + 1);
            atx::f64 l1 = 0.0;
            for (auto r : ranks) l1 += std::abs(r - mid);
            if (l1 > 0.0) {
                for (atx::usize k = 0; k < names.size(); ++k) cur[names[k]] = (ranks[k] - mid) / l1;
            } else {
                trade = false;
            }
        }
        atx::f64 to = 0.0;
        for (atx::usize i = 0; i < I; ++i) to += std::abs(cur[i] - prev[i]);
        s.turnover[t] = to;
        prev.swap(cur);
        if (!trade) {
            s.net[t] = -cost_rate * to;
            continue;
        }
        ++traded;
        names_sum += static_cast<atx::f64>(names.size());
        atx::f64 pnl = 0.0;
        for (const atx::usize i : names) {
            if (guard != nullptr && guard->bad(entry, entry + 1, i)) {
                ++s.excluded_returns;
                continue;
            }
            const atx::f64 r = simple_return(close, I, entry, entry + 1, i);
            if (std::isfinite(r)) pnl += prev[i] * r;
        }
        s.gross[t] = pnl;
        s.net[t] = pnl - cost_rate * to;
        for (atx::usize h = 0; h < cfg.ic_horizons.size(); ++h) {
            const atx::usize end = entry + cfg.ic_horizons[h];
            if (cfg.ic_horizons[h] == 0 || end >= D) continue;
            xs.clear();
            ys.clear();
            for (atx::usize k = 0; k < names.size(); ++k) {
                if (guard != nullptr && guard->bad(entry, end, names[k])) continue;
                const atx::f64 r = simple_return(close, I, entry, end, names[k]);
                if (!std::isfinite(r)) continue;
                xs.push_back(vals[k]);
                ys.push_back(r);
            }
            average_ranks(xs, order, xr);
            average_ranks(ys, order, yr);
            const atx::f64 ic = pearson(xr, yr);
            if (!std::isfinite(ic)) continue;
            if (h == 0) s.ic[t] = ic;
            ic_sum[h] += ic;
            ++ic_n[h];
        }
    }
    // A flat exit (to cash) is priced on the day it happens; the zero-names days
    // above already carry -cost * turnover in net with zero gross.
    s.coverage = T > 0 ? static_cast<atx::f64>(traded) / static_cast<atx::f64>(T) : 0.0;
    s.mean_names = traded > 0 ? names_sum / static_cast<atx::f64>(traded) : 0.0;
    finalize_score(s, cfg);
    for (atx::usize h = 1; h < 3; ++h) {
        s.ic_mean[h] = ic_n[h] > 0 ? ic_sum[h] / static_cast<atx::f64>(ic_n[h]) : 0.0;
    }
    return Ok(std::move(s));
}

// ===========================================================================
// Seeds and parsing
// ===========================================================================

std::vector<SeedExpr> literature_seeds() {
    // Families from the cross-sectional anomaly literature, written in the DSL
    // over the identified panel's fields (close/open/high/low/volume/returns/
    // vwap/cap/adv20 and the ORATS implied-vol columns). Signs follow the
    // published direction; the miner re-fixes each sign on TRAIN anyway.
    return {
        {"rank(delay(close, 21) / delay(close, 252) - 1)", "lit:momentum_12_1"},
        {"rank(delay(close, 21) / delay(close, 126) - 1)", "lit:momentum_6_1"},
        {"-1 * rank(close / delay(close, 5) - 1)", "lit:reversal_1w"},
        {"-1 * rank(close / delay(close, 21) - 1)", "lit:reversal_1m"},
        {"-1 * rank(returns)", "lit:reversal_1d"},
        {"-1 * indneutralize(returns, IndClass.sector)", "lit:reversal_1d_sector"},
        {"-1 * indneutralize(close / delay(close, 5) - 1, IndClass.sector)",
         "lit:reversal_1w_sector"},
        {"-1 * rank(stddev(returns, 63))", "lit:low_vol_63"},
        {"-1 * rank(stddev(returns, 21))", "lit:low_vol_21"},
        {"rank(ts_mean(abs(returns) / (close * volume), 21))", "lit:amihud_21"},
        {"-1 * rank(volume / adv20)", "lit:volume_shock"},
        {"-1 * correlation(rank(close), rank(volume), 10)", "lit:pv_corr_10"},
        {"-1 * rank((high - low) / close)", "lit:range"},
        {"-1 * rank(open / delay(close, 1) - 1)", "lit:overnight_gap"},
        {"-1 * rank(close / open - 1)", "lit:intraday_reversal"},
        {"-1 * rank(log(cap))", "lit:size"},
        {"rank(close / ts_max(close, 252))", "lit:high_52w"},
        {"-1 * rank(close / vwap - 1)", "lit:vwap_reversal"},
        {"-1 * rank(atmCenI_21d)", "lit:iv_level"},
        {"rank(atmCenI_126d - atmCenI_21d)", "lit:iv_term_slope"},
        {"-1 * rank(delta(atmCenI_21d, 5))", "lit:iv_change_1w"},
        {"-1 * rank(atmCenI_21d - stddev(returns, 21) * 15.87)", "lit:iv_minus_rv"},
        {"-1 * rank(ts_sum(returns, 5) * (volume / adv20))", "lit:volume_weighted_reversal"},
        {"rank(ts_mean(returns, 252) / stddev(returns, 252))", "lit:sharpe_momentum"},
        {"-1 * rank(ts_mean(returns, 21) - indneutralize(ts_mean(returns, 21), IndClass.sector))",
         "lit:sector_momentum_1m"},
    };
}

std::vector<SeedExpr> parse_fixture_seeds(std::string_view text) {
    std::vector<SeedExpr> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        const std::size_t b = line.find_first_not_of(" \t");
        if (b == std::string_view::npos || line[b] == '#') continue;
        const std::size_t colon = line.find(':', b);
        if (colon == std::string_view::npos) continue;
        const std::string_view id = line.substr(b, colon - b);
        const std::string_view dsl = line.substr(colon + 1);
        const std::size_t s = dsl.find_first_not_of(" \t");
        if (s != std::string_view::npos && !id.empty()) {
            out.push_back(SeedExpr{std::string{dsl.substr(s)}, "wq101:" + std::string{id}});
        }
    }
    return out;
}

atx::core::Result<atx::i64> parse_iso_date_ns(std::string_view text) {
    const auto bad = [&]() -> atx::core::Result<atx::i64> {
        return Err(ErrorCode::InvalidArgument,
                   "bad date (want YYYY-MM-DD): '" + std::string{text} + "'");
    };
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return bad();
    int y = 0, m = 0, d = 0;
    const auto num = [&](std::size_t off, std::size_t len, int &v) {
        const char *first = text.data() + off;
        const auto r = std::from_chars(first, first + len, v);
        return r.ec == std::errc{} && r.ptr == first + len;
    };
    if (!num(0, 4, y) || !num(5, 2, m) || !num(8, 2, d)) return bad();
    if (m < 1 || m > 12 || d < 1 || d > 31) return bad();
    // days_from_civil (H. Hinnant), proleptic Gregorian.
    const int yy = y - (m <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const auto yoe = static_cast<unsigned>(yy - era * 400);
    const auto mp = static_cast<unsigned>(m > 2 ? m - 3 : m + 9);
    const unsigned doy = (153U * mp + 2U) / 5U + static_cast<unsigned>(d) - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    const atx::i64 days =
        static_cast<atx::i64>(era) * 146097 + static_cast<atx::i64>(doe) - 719468;
    return Ok(days * 86400LL * 1000000000LL);
}

std::string_view train_dsr_rule_label(TrainDsrRule rule) noexcept {
    switch (rule) {
    case TrainDsrRule::ClusterMcFloorV2: return "cluster-mc-floor-v2";
    case TrainDsrRule::SummaryRawNV2: return "summary-raw-n-v2";
    case TrainDsrRule::SummaryNEffV1: return "summary-n-eff-v1";
    }
    return "unknown";
}

std::string trial_family_of(std::string_view origin) {
    const auto cut = origin.find_first_of(":+");
    return std::string{origin.substr(0, cut)};
}

std::string trial_theme_of(std::string_view origin) {
    const auto decay = origin.find("+decay");
    return std::string{origin.substr(0, decay)};
}

// ===========================================================================
// Mining
// ===========================================================================

namespace {

[[nodiscard]] atx::core::Result<alpha::Program> compile_dsl(const alpha::Library &lib,
                                                           std::string_view dsl) {
    ATX_TRY(auto ast, alpha::parse_expr(dsl, lib));
    ATX_TRY(auto ana, alpha::analyze(ast));
    return alpha::compile(ast, ana);
}

[[nodiscard]] atx::core::Result<std::vector<atx::f64>>
evaluate_dsl(const alpha::Library &lib, alpha::Engine &engine, std::string_view dsl) {
    ATX_TRY(auto prog, compile_dsl(lib, dsl));
    ATX_TRY(auto ss, engine.evaluate(prog));
    if (ss.alphas.empty()) return Err(ErrorCode::InvalidArgument, "program has no root");
    return Ok(std::move(ss.alphas.front().values));
}

[[nodiscard]] atx::core::Result<atx::u32> close_field_of(const alpha::Panel &panel) {
    ATX_TRY(auto id, panel.field_id("close"));
    return Ok(static_cast<atx::u32>(id));
}

// The role's prebuilt guard when it matches the panel, else one built into
// `local`; nullptr when the guard is disabled.
[[nodiscard]] atx::core::Result<const ReturnGuard *>
guard_for(const MineData &m, atx::u32 close_id, const ScoreCfg &cfg, ReturnGuard &local) {
    if (!cfg.guard_returns) return Ok(static_cast<const ReturnGuard *>(nullptr));
    if (!m.guard.empty() && m.guard.dates == m.panel->dates() &&
        m.guard.instruments == m.panel->instruments()) {
        return Ok(&m.guard);
    }
    ATX_TRY(local, build_return_guard(*m.panel, close_id, cfg));
    return Ok(static_cast<const ReturnGuard *>(&local));
}

[[nodiscard]] atx::core::Status check_data(const MineData &m, std::string_view role) {
    if (m.panel == nullptr) {
        return Err(ErrorCode::InvalidArgument, std::string{role} + ": null panel");
    }
    if (m.member.size() != m.panel->cells()) {
        return Err(ErrorCode::InvalidArgument, std::string{role} + ": member mask shape");
    }
    if (m.window.end > m.panel->dates() || m.window.size() < 3) {
        return Err(ErrorCode::InvalidArgument, std::string{role} + ": window out of range");
    }
    return Ok();
}

[[nodiscard]] bool degenerate(const SignalScore &s) {
    return !(s.coverage > 0.0) || !(sample_sd(s.net) > 0.0) || !std::isfinite(s.sharpe_net);
}

[[nodiscard]] atx::u64 candidate_hash(const CandidateRow &row) {
    const std::string key = row.dsl + (row.sign > 0.0 ? "|+1" : "|-1");
    return fnv1a64(key.data(), key.size());
}

[[nodiscard]] std::string screen_recipe(const factory::IcScreenConfig &c,
                                        const MineData &train, const ScoreCfg &score,
                                        const ReturnGuard *guard) {
    // Float bit patterns avoid rounding distinct experimental recipes together.
    const atx::usize first_cell = c.window_begin * train.panel->instruments();
    const atx::usize train_cells = (c.window_end - c.window_begin) * train.panel->instruments();
    nlohmann::json j{{"rule", factory::ic_screen_rule_name(c.rule)}, {"horizons", c.horizons},
        {"delay", c.execution_delay}, {"begin", c.window_begin}, {"end", c.window_end},
        {"maturity_end", c.maturity_end}, {"min_names", c.min_names}, {"min_dates", c.min_dates},
        {"practical_abs_ic_bits", std::bit_cast<atx::u64>(c.practical_abs_ic)},
        {"confidence_bits", std::bit_cast<atx::u64>(c.confidence_multiplier)},
        {"max_cache_bytes", c.max_cache_bytes}, {"price_field", "close"},
        {"dates", train.panel->dates()}, {"instruments", train.panel->instruments()},
        {"membership_hash", fnv1a64(train.member.data() + first_cell, train_cells)},
        {"guard_enabled", guard != nullptr}, {"raw_close_field", score.raw_close_field},
        {"guard_max_log_bits", std::bit_cast<atx::u64>(score.max_abs_log_return)},
        {"guard_adj_raw_tolerance_bits", std::bit_cast<atx::u64>(score.adj_raw_log_tol)}};
    if (guard != nullptr) j["guard_prefix_hash"] = fnv1a64(guard->bad_prefix.data() + first_cell,
        train_cells * sizeof(atx::u32));
    return j.dump();
}

// Run `fn(index, engine)` over [0, n) on up to `threads` workers. Each worker
// owns an Engine over `panel`; index i is processed by exactly one worker which
// writes only slot i of the caller's outputs, so results do not depend on the
// thread count or schedule.
// SAFETY: `panel` and the Library are read-only for the whole fan-out; the only
// shared mutable state is the atomic work counter.
template <class Fn>
void parallel_over(atx::usize n, atx::usize threads, const alpha::Panel &panel, Fn fn) {
    const atx::usize w = std::max<atx::usize>(1, std::min(threads, n));
    const auto invoke = [&](atx::usize i, alpha::Engine &engine, atx::usize worker) {
        if constexpr (std::is_invocable_v<Fn, atx::usize, alpha::Engine &, atx::usize>)
            fn(i, engine, worker);
        else fn(i, engine);
    };
    if (w <= 1) {
        alpha::Engine engine{panel};
        for (atx::usize i = 0; i < n; ++i) invoke(i, engine, 0);
        return;
    }
    std::atomic<atx::usize> next{0};
    std::vector<std::jthread> pool;
    pool.reserve(w);
    for (atx::usize t = 0; t < w; ++t) {
        pool.emplace_back([&, t] {
            alpha::Engine engine{panel};
            for (atx::usize i = next.fetch_add(1); i < n; i = next.fetch_add(1)) invoke(i, engine, t);
        });
    }
}

void score_train_row(const alpha::Library &lib, alpha::Engine &engine, const MineData &train,
                     atx::u32 close_id, const ScoreCfg &cfg, const ReturnGuard *guard,
                     const factory::IcScreenCache *cache, factory::IcScreenScratch *scratch,
                     CandidateRow &row) {
    if (!row.error.empty() || row.ic_rejected) return;
    auto sig = evaluate_dsl(lib, engine, row.dsl);
    if (!sig) {
        row.error = "eval: " + sig.error().message();
        return;
    }
    if (cache != nullptr && scratch != nullptr) {
        row.ic_screen_evaluated = true;
        auto screened = factory::screen_ic(*sig, *cache, *scratch);
        if (screened) {
            row.ic_screen_reason = screened->reason;
            row.ic_rejected = screened->reject;
            if (row.ic_rejected) return; // No backtest, sign choice or synthetic P&L.
        } else {
            row.ic_screen_unavailable = true; // Uncertainty retains the candidate.
        }
    }
    auto sc = score_signal(*sig, 1.0, *train.panel, close_id, train.member, train.window, cfg,
                           guard);
    if (!sc) {
        row.error = "score: " + sc.error().message();
        return;
    }
    if (sc->sharpe_gross < 0.0) {
        row.sign = -1.0;
        row.train = flip_score(*sc, cfg);
    } else {
        row.train = std::move(*sc);
    }
    row.scored = true;
}

[[nodiscard]] atx::core::Status register_trials(MineOutcome &out, eval::TrialRegistry &registry,
                                               TrainDsrRule dsr_rule, atx::usize train_len) {
    for (CandidateRow &row : out.candidates) {
        if (row.ic_rejected) {
            const std::string reason{factory::ic_screen_reason_name(row.ic_screen_reason)};
            const std::string key = "ic-screen|" + std::to_string(row.canonical_hash) + "|" +
                                    out.ic_screen_recipe + "|" + reason;
            row.config_hash = fnv1a64(key.data(), key.size());
            row.dsr_train = kNaN;
            eval::TrialMeta meta;
            meta.window_end = train_len - 1U;
            meta.sample = eval::TrialSample::InSample;
            // Equivalent seeds/search outputs share one screen identity. Their
            // row origins remain in candidates.csv; durable metadata is a pure
            // function of that identity, independent of discovery order.
            meta.family_tag = eval::trial_tag("ic-screened");
            meta.theme_tag = row.canonical_hash;
            ATX_TRY_VOID(registry.record_screened(eval::TrialKind::MinerExpr, row.config_hash,
                meta, eval::trial_tag(out.ic_screen_recipe), eval::trial_tag(reason)));
            continue;
        }
        if (!row.scored) continue;
        if (degenerate(row.train)) {
            row.scored = false;
            row.error = "degenerate pnl";
            ++out.degenerate;
            continue;
        }
        row.config_hash = candidate_hash(row);
        const atx::f64 sd = sample_sd(row.train.net);
        // E-16: train pnl is IN-SAMPLE on the calendar window [0, T - 1]; the registry
        // calendar may extend past it (train + validation), and the family / theme
        // tags let accounting() group trials that share a seed family.
        eval::TrialMeta meta;
        meta.window_start = 0;
        meta.window_end = row.train.net.empty() ? 0 : row.train.net.size() - 1U;
        meta.fidelity = 0;
        meta.sample = eval::TrialSample::InSample;
        meta.family_tag = eval::trial_tag(trial_family_of(row.origin));
        meta.theme_tag = eval::trial_tag(trial_theme_of(row.origin));
        auto rec = registry.record(eval::TrialKind::MinerExpr, row.config_hash, meta,
                                   row.train.net, row.train.mean_net / sd);
        if (!rec) {
            if (rec.error().code() != ErrorCode::InvalidArgument) return Err(rec.error());
            row.scored = false;
            row.error = "registry refused: " + rec.error().message();
            ++out.degenerate;
        }
    }
    out.trials = registry.summary();
    out.chain_head = registry.chain_head();
    if (out.trials.n_screened > 0) {
        dsr_rule = TrainDsrRule::SummaryRawNV2;
        out.dsr_fallback_reason = "screen-only trials have unknown P&L correlation; "
                                  "DSR uses total raw N and observed-P&L SR variance "
                                  "floored by each candidate's marginal sampling variance";
    }
    // E-01 wiring: the cluster-N DSR through TrialRegistry::accounting(). When the
    // registry cannot produce it (empty, or more than max_trials trials) every row
    // falls back to the summary DSR (N = n_raw) and the report says why. The summary
    // rules (--dsr-rule) skip accounting altogether.
    std::optional<eval::TrialAccounting> acct;
    if (dsr_rule == TrainDsrRule::ClusterMcFloorV2) {
        if (out.trials.n_raw > 0) {
            auto computed = registry.accounting(eval::TrialAccountingConfig{});
            if (computed) {
                acct = std::move(*computed);
            } else {
                out.dsr_fallback_reason = computed.error().message();
            }
        } else {
            out.dsr_fallback_reason = "no recorded trials";
        }
    }
    // The rule actually applied: the default falls back to raw-N without accounting.
    TrainDsrRule applied = dsr_rule;
    if (dsr_rule == TrainDsrRule::ClusterMcFloorV2 && !acct) {
        applied = TrainDsrRule::SummaryRawNV2;
    }
    out.dsr_rule = std::string{train_dsr_rule_label(applied)};
    const eval::SummaryDsrRule summary_rule = applied == TrainDsrRule::SummaryNEffV1
                                                  ? eval::SummaryDsrRule::NEffCrossVarV1
                                                  : eval::SummaryDsrRule::RawNCrossVarV2;
    if (acct) {
        out.dsr_clusters = acct->clusters.n_clusters;
        out.dsr_sr_star_mc = acct->mc.sorted_max.empty() ? 0.0 : acct->mc.mean;
    }
    for (CandidateRow &row : out.candidates) {
        if (!row.scored) continue;
        const atx::f64 sd = sample_sd(row.train.net);
        const atx::f64 sr = row.train.mean_net / sd;
        const atx::f64 skew = eval::skewness(row.train.net);
        const atx::f64 kurt = eval::excess_kurtosis(row.train.net);
        if (acct) {
            const auto result = eval::deflated_sharpe(sr, *acct, row.train.net.size(), skew, kurt);
            row.dsr_train = result.result.dsr;
            row.dsr_selection_benchmark = result.result.sr_star;
        } else {
            auto result = eval::deflated_sharpe(sr, out.trials, row.train.net.size(), skew,
                                               kurt, summary_rule);
            if (out.trials.n_screened > 0) {
                // Reuse the existing marginal estimator (including its T and
                // skew/kurtosis convention). For fixed total N, max of the two
                // benchmarks is exactly the max-variance floor; observed
                // registry moments remain untouched.
                const auto marginal = eval::deflated_sharpe(sr, row.train.net.size(), skew,
                    kurt, static_cast<atx::usize>(out.trials.n_raw), std::nullopt);
                if (!std::isfinite(result.dsr) || !std::isfinite(marginal.dsr)) {
                    result.dsr = kNaN;
                    result.sr_star = kNaN;
                } else if (marginal.sr_star > result.sr_star) {
                    result = marginal;
                    row.dsr_marginal_floor_applied = true;
                    ++out.dsr_marginal_floor_count;
                }
            }
            row.dsr_train = result.dsr;
            row.dsr_selection_benchmark = result.sr_star;
        }
    }
    return Ok();
}

void select_family(MineOutcome &out, const MineConfig &cfg, atx::usize pnl_len) {
    std::vector<atx::usize> eligible;
    for (atx::usize i = 0; i < out.candidates.size(); ++i) {
        const CandidateRow &r = out.candidates[i];
        if (r.scored && r.train.sharpe_net > cfg.min_train_sharpe &&
            r.train.coverage >= cfg.min_coverage) {
            eligible.push_back(i);
        }
    }
    std::sort(eligible.begin(), eligible.end(), [&](atx::usize a, atx::usize b) {
        const auto &ra = out.candidates[a];
        const auto &rb = out.candidates[b];
        if (ra.train.sharpe_net != rb.train.sharpe_net) {
            return ra.train.sharpe_net > rb.train.sharpe_net;
        }
        return ra.dsl < rb.dsl;
    });
    factory::SketchIndex sketch(pnl_len);
    for (const atx::usize i : eligible) {
        if (out.family.size() >= cfg.max_validate) break;
        const auto &net = out.candidates[i].train.net;
        if (sketch.size() > 0) {
            const auto nb = sketch.topk(net, 1);
            if (!nb.empty() && std::abs(nb.front().corr) >= cfg.max_corr) {
                ++out.family_rejected_corr;
                continue;
            }
        }
        sketch.add(i, net);
        out.family.push_back(i);
    }
}

[[nodiscard]] bool gate_pass(const MineConfig &cfg, atx::f64 p_by, atx::f64 p_rw,
                             atx::f64 sharpe_net) noexcept {
    const bool by_ok = p_by <= cfg.fdr_q;
    const bool rw_ok = p_rw <= cfg.rw_alpha;
    bool pass = false;
    switch (cfg.gate) {
    case GateMode::By: pass = by_ok; break;
    case GateMode::RomanoWolf: pass = rw_ok; break;
    case GateMode::Both: pass = by_ok && rw_ok; break;
    }
    return pass && sharpe_net > 0.0;
}

} // namespace

atx::core::Result<MineOutcome> mine_train(const alpha::Library &lib, const MineData &train,
                                          std::span<const SeedExpr> seeds, const MineConfig &cfg,
                                          eval::TrialRegistry &registry) {
    ATX_TRY_VOID(check_data(train, "train"));
    // E-16: the calendar may extend past the train window (train + validation).
    if (registry.config().pnl_len < train.window.size()) {
        return Err(ErrorCode::InvalidArgument,
                   "mine_train: registry pnl_len " + std::to_string(registry.config().pnl_len) +
                       " < train window " + std::to_string(train.window.size()));
    }
    const alpha::Panel &panel = *train.panel;
    ATX_TRY(const atx::u32 close_id, close_field_of(panel));
    MineOutcome out;
    out.ic_screen = cfg.search.ic_screen;
    out.ic_screen.execution_delay = cfg.score.delay;
    out.ic_screen.window_begin = train.window.begin;
    out.ic_screen.window_end = train.window.end;
    out.ic_screen.maturity_end = train.window.end;
    const bool screen_enabled = out.ic_screen.rule != factory::IcScreenRule::DisabledV1;
    if (screen_enabled && registry.format() != eval::TrialLogFormat::V3) {
        return Err(ErrorCode::InvalidArgument, "mine_train: IC screening requires TrialRegistry V3");
    }
    ReturnGuard local_guard;
    ATX_TRY(const ReturnGuard *guard, guard_for(train, close_id, cfg.score, local_guard));
    std::optional<factory::IcScreenCache> screen_cache;
    if (screen_enabled) {
        out.ic_screen_recipe = screen_recipe(out.ic_screen, train, cfg.score, guard);
        auto prepared = factory::prepare_ic_screen(panel, out.ic_screen, train.member,
            guard != nullptr ? std::span<const atx::u32>{guard->bad_prefix} : std::span<const atx::u32>{});
        if (prepared) screen_cache = std::move(*prepared);
        else {
            ++out.ic_screen_unavailable;
            out.ic_screen_unavailable_reason = prepared.error().message();
        }
    }
    std::unordered_set<std::string> seen;
    std::vector<std::string> valid_seed_dsl;
    for (const SeedExpr &s : seeds) {
        if (!seen.insert(s.dsl).second) continue;
        CandidateRow row;
        row.dsl = s.dsl;
        row.origin = s.origin;
        auto prog = compile_dsl(lib, s.dsl);
        if (!prog) {
            row.error = "seed invalid: " + prog.error().message();
            ++out.seeds_invalid;
        } else {
            valid_seed_dsl.push_back(s.dsl);
            if (screen_enabled) {
                ATX_TRY(auto ast, alpha::parse_expr(s.dsl, lib));
                ATX_TRY(auto genome, factory::analyze_into(std::move(ast)));
                row.canonical_hash = factory::canonical_hash(genome, cfg.search.canon);
            }
        }
        out.candidates.push_back(std::move(row));
    }

    if (cfg.run_search && !valid_seed_dsl.empty()) {
        std::vector<std::string> fields = cfg.search_fields;
        if (fields.empty()) {
            for (atx::usize f = 0; f < panel.num_fields(); ++f) {
                fields.emplace_back(panel.field_name(f));
            }
        }
        const atx::engine::WeightPolicy policy{};
        const auto sim = frictionless_sim();
        const atx::engine::combine::AlphaStore pool{};
        factory::SearchDriver driver{lib, panel, policy, sim, valid_seed_dsl, fields};
        auto search_cfg = cfg.search;
        search_cfg.ic_screen = out.ic_screen;
        // A failed preparation must not make SearchDriver rebuild without the
        // caller's membership/ReturnGuard support. The whole screen fails open.
        if (screen_enabled && !screen_cache) search_cfg.ic_screen.rule = factory::IcScreenRule::DisabledV1;
        const factory::SearchResult res = driver.run(search_cfg, pool, nullptr, nullptr,
                                                     screen_cache ? &*screen_cache : nullptr);
        if (res.ic_screen_cache_mismatch || res.ic_screen_resume_mismatch) {
            return Err(ErrorCode::InvalidArgument,
                       "mine_train: SearchDriver rejected incompatible IC screening state");
        }
        out.search_digest = res.digest;
        out.search_trial_count = res.trial_count;
        out.search_fidelity_evals = res.fidelity_evals;
        out.search_fidelity_rejected = res.fidelity_rejected;
        out.search_fingerprint_hits = res.fingerprint_hits;
        out.search_ic_screen_evaluations = res.ic_screen_evaluations;
        out.search_ic_screen_unavailable = res.ic_screen_unavailable;
        out.search_ic_screen_rejected = res.ic_rejected_hashes.size();
        out.search_ic_prepass_vm_evaluations = res.ic_prepass_vm_evaluations;
        out.search_ic_screen_resume_mismatch = res.ic_screen_resume_mismatch;
        const auto rejected = [&](atx::u64 hash) {
            return std::binary_search(res.ic_rejected_hashes.begin(), res.ic_rejected_hashes.end(), hash);
        };
        for (auto &row : out.candidates) {
            if (row.error.empty() && rejected(row.canonical_hash)) {
                row.ic_rejected = true;
                row.ic_screen_reason = factory::IcScreenReason::PracticalNull;
            }
        }
        for (const factory::Genome &g : res.all_scored) {
            std::string dsl = alpha::unparse(g.ast);
            if (!seen.insert(dsl).second) continue;
            CandidateRow row;
            row.dsl = std::move(dsl);
            row.origin = "search";
            row.canonical_hash = g.canon_hash;
            row.ic_rejected = rejected(g.canon_hash);
            if (row.ic_rejected) row.ic_screen_reason = factory::IcScreenReason::PracticalNull;
            out.candidates.push_back(std::move(row));
        }
    }

    const atx::usize workers = std::max<atx::usize>(1, std::min(cfg.threads, out.candidates.size()));
    std::vector<std::optional<factory::IcScreenScratch>> scratches(workers);
    if (screen_cache) {
        for (auto &scratch : scratches) {
            auto prepared = factory::prepare_ic_screen_scratch(*screen_cache);
            if (prepared) scratch = std::move(*prepared);
            else {
                ++out.ic_screen_unavailable;
                out.ic_screen_unavailable_reason = prepared.error().message();
            }
        }
    }
    parallel_over(out.candidates.size(), cfg.threads, panel,
                  [&](atx::usize i, alpha::Engine &engine, atx::usize worker) {
                      score_train_row(lib, engine, train, close_id, cfg.score, guard,
                                      screen_cache ? &*screen_cache : nullptr,
                                      scratches[worker] ? &*scratches[worker] : nullptr,
                                      out.candidates[i]);
                  });
    for (const auto &row : out.candidates) {
        out.ic_screen_evaluations += row.ic_screen_evaluated ? 1U : 0U;
        out.ic_screen_unavailable += row.ic_screen_unavailable ? 1U : 0U;
        out.ic_screen_rejected += row.ic_rejected ? 1U : 0U;
    }
    ATX_TRY_VOID(register_trials(out, registry, cfg.dsr_rule, train.window.size()));
    select_family(out, cfg, train.window.size());
    return Ok(std::move(out));
}

atx::core::Status mine_validate(const alpha::Library &lib, const MineData &validation,
                                const MineConfig &cfg, MineOutcome &out) {
    ATX_TRY_VOID(check_data(validation, "validation"));
    out.admitted.clear();
    if (out.family.empty()) return Ok();
    const alpha::Panel &panel = *validation.panel;
    ATX_TRY(const atx::u32 close_id, close_field_of(panel));
    const atx::usize K = out.family.size();
    ReturnGuard local_guard;
    ATX_TRY(const ReturnGuard *guard, guard_for(validation, close_id, cfg.score, local_guard));
    std::vector<std::string> errors(K);
    parallel_over(K, cfg.threads, panel, [&](atx::usize k, alpha::Engine &engine) {
        CandidateRow &row = out.candidates[out.family[k]];
        auto sig = evaluate_dsl(lib, engine, row.dsl);
        if (!sig) {
            errors[k] = sig.error().message();
            return;
        }
        auto sc = score_signal(*sig, row.sign, panel, close_id, validation.member,
                               validation.window, cfg.score, guard);
        if (!sc) {
            errors[k] = sc.error().message();
            return;
        }
        row.validation = std::move(*sc);
        row.validated = true;
    });
    // Hypothesis K+1: the pre-registered equal-weight blend of the whole family.
    out.family_blend_scored = false;
    if (K >= 2) {
        std::vector<CandidateRow> members(K);
        for (atx::usize k = 0; k < K; ++k) {
            members[k].dsl = out.candidates[out.family[k]].dsl;
            members[k].sign = out.candidates[out.family[k]].sign;
        }
        ATX_TRY(out.family_blend_validation, evaluate_blend(lib, validation, members, cfg.score));
        out.family_blend_scored = true;
    }
    const atx::usize H = K + (out.family_blend_scored ? 1 : 0);
    const atx::usize T = validation.window.size();
    std::vector<atx::f64> p(H, 1.0);
    std::vector<atx::f64> mat(H * T, 0.0);
    const auto put = [&](atx::usize h, const SignalScore &s) {
        p[h] = s.p_one_sided;
        std::copy(s.net.begin(), s.net.end(), mat.begin() + static_cast<std::ptrdiff_t>(h * T));
    };
    for (atx::usize k = 0; k < K; ++k) {
        CandidateRow &row = out.candidates[out.family[k]];
        if (!row.validated) {
            row.error = "validation: " + errors[k];
            continue;
        }
        put(k, row.validation);
    }
    if (out.family_blend_scored) put(K, out.family_blend_validation);
    const auto p_by = eval::p_adjust_by(p);
    const eval::PnlMatrix pm{mat, H, T};
    ATX_TRY(const auto rw, eval::romano_wolf(pm, cfg.boot, cfg.rw_alpha));
    for (atx::usize k = 0; k < K; ++k) {
        CandidateRow &row = out.candidates[out.family[k]];
        row.p_by = p_by[k];
        row.p_rw = rw.p_adjusted[k];
        row.admitted = row.validated &&
                       gate_pass(cfg, row.p_by, row.p_rw, row.validation.sharpe_net);
        if (row.admitted) out.admitted.push_back(out.family[k]);
    }
    if (out.family_blend_scored) {
        out.family_blend_p_by = p_by[K];
        out.family_blend_p_rw = rw.p_adjusted[K];
        out.family_blend_admitted =
            gate_pass(cfg, out.family_blend_p_by, out.family_blend_p_rw,
                      out.family_blend_validation.sharpe_net);
    }
    return Ok();
}

atx::core::Result<MineOutcome> mine(const alpha::Library &lib, const MineData &train,
                                    const MineData &validation, std::span<const SeedExpr> seeds,
                                    const MineConfig &cfg, eval::TrialRegistry &registry) {
    ATX_TRY(auto out, mine_train(lib, train, seeds, cfg, registry));
    ATX_TRY_VOID(mine_validate(lib, validation, cfg, out));
    return Ok(std::move(out));
}

namespace {

// Add this alpha's centered cross-sectional rank, in (-0.5, 0.5), to the blend
// accumulators over the window's dates (eligibility as in score_signal).
void accumulate_blend(std::span<const atx::f64> sig, atx::f64 sign, const MineData &data,
                      std::span<const atx::f64> close, std::vector<atx::f64> &sum,
                      std::vector<atx::u32> &cnt) {
    const atx::usize I = data.panel->instruments();
    std::vector<atx::f64> vals, ranks;
    std::vector<atx::usize> names, order;
    for (atx::usize d = data.window.begin; d < data.window.end; ++d) {
        vals.clear();
        names.clear();
        for (atx::usize i = 0; i < I; ++i) {
            const atx::usize c = d * I + i;
            if (data.member[c] == 0 || !std::isfinite(sig[c]) || !std::isfinite(close[c]) ||
                !(close[c] > 0.0)) {
                continue;
            }
            vals.push_back(sign * sig[c]);
            names.push_back(i);
        }
        if (names.size() < 2) continue;
        average_ranks(vals, order, ranks);
        const auto n = static_cast<atx::f64>(names.size());
        for (atx::usize k = 0; k < names.size(); ++k) {
            const atx::usize c = d * I + names[k];
            sum[c] += (ranks[k] - 0.5) / n - 0.5;
            ++cnt[c];
        }
    }
}

} // namespace

atx::core::Result<SignalScore> evaluate_blend(const alpha::Library &lib, const MineData &data,
                                              std::span<const CandidateRow> rows,
                                              const ScoreCfg &cfg) {
    ATX_TRY_VOID(check_data(data, "blend"));
    if (rows.empty()) return Err(ErrorCode::InvalidArgument, "evaluate_blend: no rows");
    const alpha::Panel &panel = *data.panel;
    ATX_TRY(const atx::u32 close_id, close_field_of(panel));
    const atx::usize cells = panel.cells();
    const auto close = panel.field_all(close_id);
    std::vector<atx::f64> sum(cells, 0.0);
    std::vector<atx::u32> cnt(cells, 0);
    alpha::Engine engine{panel};
    for (const CandidateRow &row : rows) {
        ATX_TRY(auto sig, evaluate_dsl(lib, engine, row.dsl));
        accumulate_blend(sig, row.sign, data, close, sum, cnt);
    }
    std::vector<atx::f64> blend(cells, kNaN);
    for (atx::usize c = 0; c < cells; ++c) {
        if (cnt[c] > 0) blend[c] = sum[c] / static_cast<atx::f64>(cnt[c]);
    }
    ReturnGuard local_guard;
    ATX_TRY(const ReturnGuard *guard, guard_for(data, close_id, cfg, local_guard));
    return score_signal(blend, 1.0, panel, close_id, data.member, data.window, cfg, guard);
}

atx::core::Result<std::vector<HoldoutRow>>
evaluate_holdout(const alpha::Library &lib, const MineData &holdout,
                 std::span<const CandidateRow> admitted, const ScoreCfg &cfg) {
    ATX_TRY_VOID(check_data(holdout, "holdout"));
    const alpha::Panel &panel = *holdout.panel;
    ATX_TRY(const atx::u32 close_id, close_field_of(panel));
    const atx::usize cells = panel.cells();
    const auto close = panel.field_all(close_id);
    std::vector<HoldoutRow> rows;
    std::vector<atx::f64> blend_sum(cells, 0.0);
    std::vector<atx::u32> blend_n(cells, 0);
    ReturnGuard local_guard;
    ATX_TRY(const ReturnGuard *guard, guard_for(holdout, close_id, cfg, local_guard));
    alpha::Engine engine{panel};
    for (const CandidateRow &row : admitted) {
        ATX_TRY(auto sig, evaluate_dsl(lib, engine, row.dsl));
        ATX_TRY(auto sc, score_signal(sig, row.sign, panel, close_id, holdout.member,
                                      holdout.window, cfg, guard));
        rows.push_back(HoldoutRow{row.dsl, std::move(sc)});
        accumulate_blend(sig, row.sign, holdout, close, blend_sum, blend_n);
    }
    if (!admitted.empty()) {
        std::vector<atx::f64> blend(cells, kNaN);
        for (atx::usize c = 0; c < cells; ++c) {
            if (blend_n[c] > 0) blend[c] = blend_sum[c] / static_cast<atx::f64>(blend_n[c]);
        }
        ATX_TRY(auto sc, score_signal(blend, 1.0, panel, close_id, holdout.member, holdout.window,
                                      cfg, guard));
        rows.push_back(HoldoutRow{"<equal-weight blend>", std::move(sc)});
    }
    return Ok(std::move(rows));
}

} // namespace mine

// ===========================================================================
// CLI stage
// ===========================================================================

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace alpha = atx::engine::alpha;
namespace eval = atx::engine::eval;
using nlohmann::json;

struct MineArgs {
    alpha::VwapRule vwap_rule{alpha::VwapRule::RawDailyCloseV2};
    atx::engine::factory::IcScreenConfig ic_screen = [] {
        atx::engine::factory::IcScreenConfig c;
        c.rule = atx::engine::factory::IcScreenRule::EquivalenceV3;
        c.practical_abs_ic = 0.002;
        c.confidence_multiplier = 3.5;
        return c;
    }();
    std::vector<std::string> train_ctx, val_ctx, hold_ctx;
    std::string membership;
    atx::usize membership_cut{0};
    // I-16: "as-of-v2" (default) REQUIRES --membership; "year-union-v1" is the
    // explicit, labelled pre-W0 fallback and takes no image.
    std::string membership_rule{"as-of-v2"};
    bool allow_same_close{false}; // B-02: --delay 0 needs this opt-in
    // RULES §2: the report-only train DSR rule; summary-n-eff-v1 re-derives pre-W0.
    mine::TrainDsrRule dsr_rule{mine::TrainDsrRule::ClusterMcFloorV2};
    std::string train_start, val_start, hold_start, hold_end;
    std::string seal{"2020-01-01"};
    std::string out;
    std::string fixture;
    std::string extra_seeds;
    std::vector<atx::usize> smooth_windows;
    bool literature{true};
    bool search{true};
    bool fidelity{true};
    bool semantic{true};
    bool output_dedup{true};
    bool quiet{false};
    // Holdout discipline: "off" (default) never loads the holdout contexts, so
    // smoke and development runs cannot read the holdout; "publish" scores it
    // once. prior_reads records earlier reads of the same holdout period.
    bool holdout_publish{false};
    atx::usize holdout_prior_reads{0};
    bool guard_returns{true};
    atx::f64 max_abs_log_return{1.5};
    atx::f64 adj_raw_log_tol{0.10};
    atx::u64 seed{20260923};
    atx::usize population{64};
    atx::usize generations{8};
    atx::usize threads{2};
    atx::f64 cost_bps{5.0};
    atx::usize delay{1};
    atx::usize min_names{100};
    atx::usize max_validate{100};
    atx::f64 max_corr{0.7};
    atx::f64 min_coverage{0.5};
    atx::f64 fdr_q{0.10};
    atx::f64 rw_alpha{0.10};
    atx::usize n_boot{1000};
    atx::f64 mean_block{10.0};
    mine::GateMode gate{mine::GateMode::By};
    atx::u64 max_working_bytes{6'000'000'000ULL};
};

[[nodiscard]] std::vector<std::string> split_list(std::string_view s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t e = s.find(';', pos);
        const std::string_view item = s.substr(pos, e == std::string_view::npos ? s.npos : e - pos);
        if (!item.empty()) out.emplace_back(item);
        if (e == std::string_view::npos) break;
        pos = e + 1;
    }
    return out;
}

template <class T>
[[nodiscard]] atx::core::Status parse_num(std::string_view flag, std::string_view v, T &out) {
    if constexpr (std::is_floating_point_v<T>) {
        try {
            std::size_t used = 0;
            const double x = std::stod(std::string{v}, &used);
            if (used != v.size() || !std::isfinite(x)) throw std::invalid_argument("x");
            out = static_cast<T>(x);
        } catch (...) {
            return Err(ErrorCode::InvalidArgument,
                       "--" + std::string{flag} + ": bad number '" + std::string{v} + "'");
        }
    } else {
        const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
        if (r.ec != std::errc{} || r.ptr != v.data() + v.size()) {
            return Err(ErrorCode::InvalidArgument,
                       "--" + std::string{flag} + ": bad integer '" + std::string{v} + "'");
        }
    }
    return Ok();
}

[[nodiscard]] atx::core::Status apply_value(MineArgs &a, std::string_view f, std::string_view v) {
    ATX_TRY(const bool screen_option, apply_ic_screen_option(a.ic_screen, f, v));
    if (screen_option) return Ok();
    if (f == "vwap-rule") {
        const auto rule = alpha::parse_vwap_rule(v);
        if (!rule) return Err(ErrorCode::InvalidArgument,
            "--vwap-rule must be raw-daily-close-v2 or adjusted-typical-v1");
        a.vwap_rule = *rule;
        return Ok();
    }
    if (f == "train-contexts") { a.train_ctx = split_list(v); return Ok(); }
    if (f == "validation-contexts") { a.val_ctx = split_list(v); return Ok(); }
    if (f == "holdout-contexts") { a.hold_ctx = split_list(v); return Ok(); }
    if (f == "membership") { a.membership = std::string{v}; return Ok(); }
    if (f == "membership-cut") return parse_num(f, v, a.membership_cut);
    if (f == "membership-rule") {
        if (v != "as-of-v2" && v != "year-union-v1") {
            return Err(ErrorCode::InvalidArgument,
                       "--membership-rule must be as-of-v2 or year-union-v1");
        }
        a.membership_rule = std::string{v};
        return Ok();
    }
    if (f == "dsr-rule") {
        if (v == "cluster-mc-floor-v2") a.dsr_rule = mine::TrainDsrRule::ClusterMcFloorV2;
        else if (v == "summary-raw-n-v2") a.dsr_rule = mine::TrainDsrRule::SummaryRawNV2;
        else if (v == "summary-n-eff-v1") a.dsr_rule = mine::TrainDsrRule::SummaryNEffV1;
        else {
            return Err(ErrorCode::InvalidArgument,
                       "--dsr-rule must be cluster-mc-floor-v2|summary-raw-n-v2|summary-n-eff-v1");
        }
        return Ok();
    }
    if (f == "train-start") { a.train_start = std::string{v}; return Ok(); }
    if (f == "validation-start") { a.val_start = std::string{v}; return Ok(); }
    if (f == "holdout-start") { a.hold_start = std::string{v}; return Ok(); }
    if (f == "holdout-end") { a.hold_end = std::string{v}; return Ok(); }
    if (f == "seal") { a.seal = std::string{v}; return Ok(); }
    if (f == "out") { a.out = std::string{v}; return Ok(); }
    if (f == "fixture") { a.fixture = std::string{v}; return Ok(); }
    if (f == "extra-seeds") { a.extra_seeds = std::string{v}; return Ok(); }
    if (f == "smooth-windows") {
        a.smooth_windows.clear();
        for (const auto &item : split_list(v)) {
            atx::usize w = 0;
            ATX_TRY_VOID(parse_num(f, item, w));
            if (w < 2 || w > 63) {
                return Err(ErrorCode::InvalidArgument, "--smooth-windows: each in [2, 63]");
            }
            a.smooth_windows.push_back(w);
        }
        return Ok();
    }
    if (f == "seed") return parse_num(f, v, a.seed);
    if (f == "population") return parse_num(f, v, a.population);
    if (f == "generations") return parse_num(f, v, a.generations);
    if (f == "threads") return parse_num(f, v, a.threads);
    if (f == "cost-bps") return parse_num(f, v, a.cost_bps);
    if (f == "delay") return parse_num(f, v, a.delay);
    if (f == "min-names") return parse_num(f, v, a.min_names);
    if (f == "max-validate") return parse_num(f, v, a.max_validate);
    if (f == "max-corr") return parse_num(f, v, a.max_corr);
    if (f == "min-coverage") return parse_num(f, v, a.min_coverage);
    if (f == "fdr-q") return parse_num(f, v, a.fdr_q);
    if (f == "rw-alpha") return parse_num(f, v, a.rw_alpha);
    if (f == "n-boot") return parse_num(f, v, a.n_boot);
    if (f == "mean-block") return parse_num(f, v, a.mean_block);
    if (f == "max-working-bytes") return parse_num(f, v, a.max_working_bytes);
    if (f == "holdout") {
        if (v == "off") a.holdout_publish = false;
        else if (v == "publish") a.holdout_publish = true;
        else return Err(ErrorCode::InvalidArgument, "--holdout must be off|publish");
        return Ok();
    }
    if (f == "holdout-prior-reads") return parse_num(f, v, a.holdout_prior_reads);
    if (f == "max-abs-log-return") return parse_num(f, v, a.max_abs_log_return);
    if (f == "adj-raw-log-tol") return parse_num(f, v, a.adj_raw_log_tol);
    if (f == "gate") {
        if (v == "by") a.gate = mine::GateMode::By;
        else if (v == "rw") a.gate = mine::GateMode::RomanoWolf;
        else if (v == "both") a.gate = mine::GateMode::Both;
        else return Err(ErrorCode::InvalidArgument, "--gate must be by|rw|both");
        return Ok();
    }
    return Err(ErrorCode::InvalidArgument, "equity-mine: unknown flag --" + std::string{f});
}

[[nodiscard]] atx::core::Result<MineArgs> parse_mine_args(int argc, char **argv) {
    MineArgs a;
    for (int i = 2; i < argc; ++i) {
        const std::string_view tok{argv[i]};
        if (tok.size() < 3 || tok.substr(0, 2) != "--") {
            return Err(ErrorCode::InvalidArgument,
                       "equity-mine: unexpected argument '" + std::string{tok} + "'");
        }
        const std::string_view f = tok.substr(2);
        if (f == "no-search") { a.search = false; continue; }
        if (f == "no-literature") { a.literature = false; continue; }
        if (f == "no-fidelity") { a.fidelity = false; continue; }
        if (f == "no-semantic-canon") { a.semantic = false; continue; }
        if (f == "no-output-dedup") { a.output_dedup = false; continue; }
        if (f == "quiet") { a.quiet = true; continue; }
        if (f == "no-return-guard") { a.guard_returns = false; continue; }
        if (f == "allow-same-close") {
            // As parse_args: valueless means true; one following boolean literal
            // (true / false / 1 / 0) is consumed as the value.
            std::string_view value;
            if (i + 1 < argc) {
                const std::string_view next{argv[i + 1]};
                if (!next.empty() && parse_bool_flag_value(f, next).has_value()) {
                    value = next;
                    ++i;
                }
            }
            ATX_TRY(const bool allow, parse_bool_flag_value(f, value));
            a.allow_same_close = allow;
            continue;
        }
        if (i + 1 >= argc) {
            return Err(ErrorCode::InvalidArgument, "equity-mine: --" + std::string{f} +
                                                       " needs a value");
        }
        ATX_TRY_VOID(apply_value(a, f, argv[++i]));
    }
    if (a.train_ctx.empty() || a.val_ctx.empty() || a.out.empty() || a.train_start.empty() ||
        a.val_start.empty() || a.hold_start.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity-mine: --train-contexts --validation-contexts --train-start "
                   "--validation-start --holdout-start --out are required");
    }
    if (a.holdout_publish && (a.hold_ctx.empty() || a.hold_end.empty())) {
        return Err(ErrorCode::InvalidArgument,
                   "equity-mine: --holdout publish requires --holdout-contexts and --holdout-end");
    }
    if (!(a.max_abs_log_return > 0.0) || a.adj_raw_log_tol < 0.0) {
        return Err(ErrorCode::InvalidArgument,
                   "equity-mine: --max-abs-log-return must be > 0 and --adj-raw-log-tol >= 0");
    }
    if (!(a.fdr_q > 0.0 && a.fdr_q < 1.0) || !(a.rw_alpha > 0.0 && a.rw_alpha < 1.0) ||
        a.n_boot == 0 || a.mean_block < 1.0 || a.threads == 0 || a.max_validate == 0 ||
        !(a.max_corr > 0.0 && a.max_corr <= 1.0) || a.cost_bps < 0.0 || a.min_names < 2) {
        return Err(ErrorCode::InvalidArgument, "equity-mine: a numeric flag is out of range");
    }
    // W0-I0b / I-16: no silent year-union fallback. The as-of rule needs the image;
    // the pre-W0 fallback must be named and then takes no image.
    if (a.membership_rule == "as-of-v2" && a.membership.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity-mine: --membership <membership.bin> is required (as-of point-in-time "
                   "mask); --membership-rule year-union-v1 reproduces the pre-W0 context "
                   "year-union fallback, a within-year selection look-ahead");
    }
    if (a.membership_rule == "year-union-v1" && !a.membership.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity-mine: --membership-rule year-union-v1 takes no --membership");
    }
    // W0-I0b / B-02: a zero delay trades at the close that produced the signal.
    if (a.delay < 1 && !a.allow_same_close) {
        return Err(ErrorCode::InvalidArgument,
                   "equity-mine: --delay 0 fills at the signal close; pass --allow-same-close "
                   "to request it explicitly");
    }
    return Ok(std::move(a));
}

[[nodiscard]] atx::core::Result<std::string> read_text(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Err(ErrorCode::IoError, "cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return Ok(ss.str());
}

[[nodiscard]] std::vector<atx::u16> adv_windows_of(std::span<const mine::SeedExpr> seeds) {
    std::set<unsigned> w{20};
    for (const auto &s : seeds) {
        const std::string &t = s.dsl;
        for (std::size_t i = 0; i + 3 < t.size(); ++i) {
            const bool boundary = i == 0 || (std::isalnum(static_cast<unsigned char>(t[i - 1])) == 0 &&
                                             t[i - 1] != '_' && t[i - 1] != '.');
            if (!boundary || t.compare(i, 3, "adv") != 0) continue;
            unsigned v = 0;
            std::size_t j = i + 3;
            bool any = false;
            while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j])) != 0 && v < 100000U) {
                v = v * 10U + static_cast<unsigned>(t[j] - '0');
                ++j;
                any = true;
            }
            if (any && v >= 1 && v <= 400) w.insert(v);
        }
    }
    return {w.begin(), w.end()};
}

struct ContextInfo {
    std::string path;
    std::string artifact_id;
    std::string payload_sha256;
    atx::usize dates{};
    atx::usize instruments{};
};

[[nodiscard]] atx::core::Result<mine::SpanSource> load_context(const std::string &path,
                                                              atx::i64 seal_ns,
                                                              ContextInfo &info) {
    ATX_TRY(auto art, read_panel_artifact(path, 4'000'000'000ULL));
    if (art.identity.instrument_namespace != kSpiderRockSecurityIdNamespace) {
        return Err(ErrorCode::InvalidArgument, path + ": not a spiderrock-identified context");
    }
    for (const atx::i64 k : art.identity.session_keys) {
        if (k >= seal_ns) {
            return Err(ErrorCode::InvalidArgument,
                       path + ": carries a session at or after the seal; refusing to read it");
        }
    }
    mine::SpanSource src{std::move(art.panel), std::move(art.identity.session_keys), {}};
    src.instrument_ids.reserve(art.identity.instrument_ids.size());
    for (const std::string &s : art.identity.instrument_ids) {
        atx::i64 v = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {
            return Err(ErrorCode::InvalidArgument, path + ": non-integer instrument id '" + s + "'");
        }
        src.instrument_ids.push_back(v);
    }
    info.path = path;
    info.artifact_id = art.artifact_id;
    info.payload_sha256 = art.payload_sha256;
    info.dates = src.panel.dates();
    info.instruments = src.panel.instruments();
    return Ok(std::move(src));
}

// E-16: the number of distinct sessions in [start_ns, end_ns) across `paths`, read
// from each context's manifest axes only (no numeric payload is loaded), so the
// registry calendar can span train + validation before the validation span exists.
[[nodiscard]] atx::core::Result<atx::usize>
count_role_sessions(const std::vector<std::string> &paths, atx::i64 start_ns, atx::i64 end_ns) {
    std::set<atx::i64> keys;
    for (const auto &p : paths) {
        ATX_TRY(const auto text, read_text(p + ".manifest.json"));
        json manifest;
        try {
            manifest = json::parse(text);
            for (const auto &key : manifest.at("axes").at("session_keys")) {
                const auto s = key.get<std::string>();
                atx::i64 v = 0;
                const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
                if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {
                    return Err(ErrorCode::ParseError, p + ": noncanonical manifest session key");
                }
                if (v >= start_ns && v < end_ns) keys.insert(v);
            }
        } catch (const std::exception &e) {
            return Err(ErrorCode::ParseError, p + ": unreadable manifest axes: " + e.what());
        }
    }
    return Ok(keys.size());
}

struct Role {
    std::string name;
    mine::SpanPanel span;
    mine::MineData data;
    std::vector<ContextInfo> contexts;
    std::string membership_rule;
    atx::usize member_cells{};
    atx::usize guard_excluded_window{};
};

[[nodiscard]] atx::core::Result<Role>
build_role(std::string name, const std::vector<std::string> &paths, atx::i64 start_ns,
           atx::i64 end_ns, const std::string &end_label, atx::i64 seal_ns,
           const atx::engine::data::PitMembershipImage *image, atx::usize cut,
           std::span<const atx::u16> adv_windows, atx::u64 max_bytes,
           const mine::ScoreCfg &score, alpha::VwapRule vwap_rule) {
    std::vector<ContextInfo> contexts;
    std::vector<mine::SpanSource> sources;
    for (const auto &p : paths) {
        ContextInfo info;
        ATX_TRY(auto src, load_context(p, seal_ns, info));
        sources.push_back(std::move(src));
        contexts.push_back(std::move(info));
    }
    ATX_TRY(auto span, mine::stitch_span(sources));
    // A role's span must end before the next role starts: search, scoring, the
    // last signals' realized returns and the long IC horizons all read every
    // session of the span, so a later session would leak the next role's data.
    if (!span.session_keys.empty() && span.session_keys.back() >= end_ns) {
        return Err(ErrorCode::InvalidArgument,
                   name + ": contexts carry sessions at or after this role's end (" + end_label +
                       "); they would leak the next role's data into this one. Pass contexts "
                       "that end before it");
    }
    const atx::usize D = span.session_keys.size();
    const atx::usize I = span.instrument_ids.size();
    const atx::u64 est = static_cast<atx::u64>(D) * I * 8ULL *
                         (span.panel.num_fields() + adv_windows.size() + 10ULL) * 2ULL;
    if (est > max_bytes) {
        return Err(ErrorCode::InvalidArgument,
                   name + ": estimated working set " + std::to_string(est) +
                       " bytes exceeds --max-working-bytes");
    }
    std::vector<atx::u8> member;
    std::string rule;
    if (image != nullptr) {
        ATX_TRY(member, mine::asof_membership_mask(*image, cut, span.session_keys,
                                                   span.instrument_ids));
        rule = "as-of-pit-membership";
    } else {
        member = mine::owner_union_mask(span, sources);
        rule = "context-year-union-not-as-of";
    }
    sources.clear();
    ATX_TRY(auto aug, alpha::with_alpha101_fields(span.panel, adv_windows,
        alpha::DollarVolumeBasis::RawCloseV2, vwap_rule));
    span.panel = std::move(aug);
    const auto lo = std::lower_bound(span.session_keys.begin(), span.session_keys.end(), start_ns);
    const auto hi = std::lower_bound(span.session_keys.begin(), span.session_keys.end(), end_ns);
    mine::EvalWindow w{static_cast<atx::usize>(lo - span.session_keys.begin()),
                       static_cast<atx::usize>(hi - span.session_keys.begin())};
    if (w.size() < 20) {
        return Err(ErrorCode::InvalidArgument, name + ": evaluation window has < 20 sessions");
    }
    atx::usize member_cells = 0;
    for (atx::usize d = w.begin; d < w.end; ++d) {
        for (atx::usize i = 0; i < I; ++i) member_cells += member[d * I + i];
    }
    ATX_TRY(const auto close_id, span.panel.field_id("close"));
    ATX_TRY(auto guard, mine::build_return_guard(span.panel, static_cast<atx::u32>(close_id), score));
    // Realized one-day returns this role's pnl can touch: signal dates in the
    // window realize at d + delay + 1.
    const atx::usize realized_end = std::min<atx::usize>(D, w.end + score.delay + 1);
    const atx::usize excl_window = guard.count_in(w.begin + score.delay + 1, realized_end);
    // data.panel is bound by the caller once the Role sits at its final address.
    Role role{std::move(name), std::move(span), mine::MineData{nullptr, std::move(member), w, {}},
              std::move(contexts), std::move(rule), member_cells, excl_window};
    role.data.guard = std::move(guard);
    return Ok(std::move(role));
}

[[nodiscard]] std::string csv_quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string num(atx::f64 v) {
    if (!std::isfinite(v)) return "nan";
    std::ostringstream ss;
    ss.precision(6);
    ss << v;
    return ss.str();
}

[[nodiscard]] json score_json(const mine::SignalScore &s) {
    return json{{"sharpe_net", s.sharpe_net},     {"sharpe_gross", s.sharpe_gross},
                {"mean_net_bps", s.mean_net * 1e4}, {"t_nw", s.t_nw},
                {"p_one_sided", s.p_one_sided},    {"ic_h1", s.ic_mean[0]},
                {"ic_h5", s.ic_mean[1]},           {"ic_h21", s.ic_mean[2]},
                {"icir_h1", s.icir},               {"turnover", s.mean_turnover},
                {"coverage", s.coverage},          {"mean_names", s.mean_names},
                {"excluded_return_terms", s.excluded_returns}};
}

[[nodiscard]] std::string score_cols(const mine::SignalScore &s) {
    return num(s.sharpe_net) + ',' + num(s.sharpe_gross) + ',' + num(s.ic_mean[0]) + ',' +
           num(s.ic_mean[1]) + ',' + num(s.ic_mean[2]) + ',' + num(s.icir) + ',' +
           num(s.mean_turnover) + ',' + num(s.coverage) + ',' + num(s.t_nw);
}

constexpr std::string_view kScoreHeader =
    "sharpe_net,sharpe_gross,ic_h1,ic_h5,ic_h21,icir_h1,turnover,coverage,t_nw";

// kScoreHeader with every column name prefixed (e.g. "train_").
[[nodiscard]] std::string score_header(std::string_view prefix) {
    std::string out;
    std::size_t pos = 0;
    while (pos < kScoreHeader.size()) {
        std::size_t e = kScoreHeader.find(',', pos);
        if (e == std::string_view::npos) e = kScoreHeader.size();
        if (!out.empty()) out += ',';
        out += prefix;
        out += kScoreHeader.substr(pos, e - pos);
        pos = e + 1;
    }
    return out;
}

[[nodiscard]] atx::core::Status write_file(const std::filesystem::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return Err(ErrorCode::IoError, "cannot write " + p.string());
    f << s;
    f.close();
    if (!f) return Err(ErrorCode::IoError, "write failed " + p.string());
    return Ok();
}

[[nodiscard]] std::string candidates_csv(const mine::MineOutcome &o) {
    const bool screening = o.ic_screen.rule != atx::engine::factory::IcScreenRule::DisabledV1;
    std::string s = "idx,origin,sign,scored,in_family,";
    s += score_header("train_") + ",dsr_train,error,dsl";
    if (screening) s += ",ic_screened,ic_screen_reason,trial_config_hash,dsr_marginal_floor,dsr_selection_benchmark";
    s += '\n';
    std::vector<atx::u8> fam(o.candidates.size(), 0);
    for (auto i : o.family) fam[i] = 1;
    for (atx::usize i = 0; i < o.candidates.size(); ++i) {
        const auto &c = o.candidates[i];
        // Metadata-only trials have no score: empty numeric cells are not zeros.
        const std::string train_cols = c.ic_rejected
            ? std::string(static_cast<atx::usize>(std::count(kScoreHeader.begin(), kScoreHeader.end(), ',')), ',')
            : score_cols(c.train);
        s += std::to_string(i) + ',' + csv_quote(c.origin) + ',' + num(c.sign) + ',' +
             (c.scored ? "1" : "0") + ',' + (fam[i] ? "1" : "0") + ',' + train_cols +
             ',' + (c.ic_rejected ? std::string{} : num(c.dsr_train)) + ',' + csv_quote(c.error) + ',' + csv_quote(c.dsl);
        if (screening) s += ',' + std::string{c.ic_rejected ? "1" : "0"} + ',' +
            csv_quote(std::string{atx::engine::factory::ic_screen_reason_name(c.ic_screen_reason)}) + ',' + to_hex16(c.config_hash) + ',' +
            (c.scored ? (c.dsr_marginal_floor_applied ? "1" : "0") : "") + ',' +
            (c.scored ? num(c.dsr_selection_benchmark) : std::string{});
        s += '\n';
    }
    return s;
}

[[nodiscard]] std::string validation_csv(const mine::MineOutcome &o) {
    std::string s = "rank,idx,origin,sign,train_sharpe_net," + score_header("val_") +
                    ",p_raw,p_by,p_rw,admitted,dsl\n";
    for (atx::usize k = 0; k < o.family.size(); ++k) {
        const auto &c = o.candidates[o.family[k]];
        s += std::to_string(k) + ',' + std::to_string(o.family[k]) + ',' + csv_quote(c.origin) +
             ',' + num(c.sign) + ',' + num(c.train.sharpe_net) + ',' + score_cols(c.validation) +
             ',' + num(c.validation.p_one_sided) + ',' + num(c.p_by) + ',' + num(c.p_rw) + ',' +
             (c.admitted ? "1" : "0") + ',' + csv_quote(c.dsl) + '\n';
    }
    return s;
}

[[nodiscard]] std::string library_tsv(const mine::MineOutcome &o) {
    std::string s = "alpha\tsign\torigin\ttrain_sharpe_net\tval_sharpe_net\ttrain_ic_h1\t"
                    "train_ic_h5\ttrain_ic_h21\tval_ic_h1\tval_ic_h5\tval_ic_h21\tval_icir_h1\t"
                    "val_turnover\tp_by\tp_rw\tdsr_train\tdsl\n";
    for (atx::usize k = 0; k < o.admitted.size(); ++k) {
        const auto &c = o.candidates[o.admitted[k]];
        s += "mine_" + std::to_string(k) + '\t' + num(c.sign) + '\t' + c.origin + '\t' +
             num(c.train.sharpe_net) + '\t' + num(c.validation.sharpe_net) + '\t' +
             num(c.train.ic_mean[0]) + '\t' + num(c.train.ic_mean[1]) + '\t' +
             num(c.train.ic_mean[2]) + '\t' + num(c.validation.ic_mean[0]) + '\t' +
             num(c.validation.ic_mean[1]) + '\t' + num(c.validation.ic_mean[2]) + '\t' +
             num(c.validation.icir) + '\t' + num(c.validation.mean_turnover) + '\t' +
             num(c.p_by) + '\t' + num(c.p_rw) + '\t' + num(c.dsr_train) + '\t' + c.dsl + '\n';
    }
    return s;
}

constexpr std::string_view kFamilyBlend = "<family equal-weight blend>";

[[nodiscard]] std::string holdout_csv(const std::vector<mine::HoldoutRow> &rows,
                                      const mine::MineOutcome &o) {
    std::string s = "row,sign," + std::string{kScoreHeader} + ",p_one_sided,dsl\n";
    for (atx::usize k = 0; k < rows.size(); ++k) {
        const bool blend = k >= o.admitted.size();
        const atx::f64 sign = blend ? 1.0 : o.candidates[o.admitted[k]].sign;
        const std::string label = !blend ? "mine_" + std::to_string(k)
                                  : rows[k].dsl == kFamilyBlend ? std::string{"family_blend"}
                                                                : std::string{"admitted_blend"};
        s += label + ',' + num(sign) +
             ',' + score_cols(rows[k].score) + ',' + num(rows[k].score.p_one_sided) + ',' +
             csv_quote(rows[k].dsl) + '\n';
    }
    return s;
}

[[nodiscard]] json guard_json(const Role &r, const mine::ScoreCfg &score) {
    const mine::ReturnGuard &g = r.data.guard;
    atx::usize cap = 0;
    for (const auto &e : g.excluded) cap += e.cap ? 1 : 0;
    json ex = json::array();
    for (const auto &e : g.excluded) {
        if (ex.size() >= 64) break;
        ex.push_back({{"session_key", r.span.session_keys.at(e.date)},
                      {"security_id", r.span.instrument_ids.at(e.inst)},
                      {"adj_return", e.adj_return},
                      {"raw_return", std::isfinite(e.raw_return) ? json(e.raw_return) : json()},
                      {"reason", e.cap ? "abs-log-return-cap" : "adjusted-exceeds-raw"}});
    }
    return json{{"enabled", score.guard_returns},
                {"has_raw_close", g.has_raw},
                {"max_abs_log_return", score.max_abs_log_return},
                {"adj_raw_log_tol", score.adj_raw_log_tol},
                {"excluded_one_day_cells_span", g.excluded.size()},
                {"excluded_by_cap_span", cap},
                {"excluded_by_raw_disagreement_span", g.excluded.size() - cap},
                {"excluded_one_day_cells_realized_window", r.guard_excluded_window},
                {"examples", ex}};
}

[[nodiscard]] json role_json(const Role &r) {
    json ctx = json::array();
    for (const auto &c : r.contexts) {
        ctx.push_back({{"path", c.path},
                       {"artifact_id", c.artifact_id},
                       {"payload_sha256", c.payload_sha256},
                       {"dates", c.dates},
                       {"instruments", c.instruments}});
    }
    const auto &k = r.span.session_keys;
    return json{{"contexts", ctx},
                {"span_dates", k.size()},
                {"span_instruments", r.span.instrument_ids.size()},
                {"window_begin_key", k.at(r.data.window.begin)},
                {"window_last_key", k.at(r.data.window.end - 1)},
                {"window_sessions", r.data.window.size()},
                {"membership_rule", r.membership_rule},
                {"member_cells_in_window", r.member_cells},
                {"overlap_cells", r.span.overlap_cells},
                {"overlap_mismatch_cells", r.span.overlap_mismatch_cells}};
}

void log_line(std::ostream &err, bool quiet, const std::string &msg) {
    if (quiet) return;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    err << "[equity-mine t=" << std::chrono::duration_cast<std::chrono::seconds>(now).count()
        << "] " << msg << '\n';
    err.flush();
}

[[nodiscard]] atx::core::Result<std::vector<mine::SeedExpr>> collect_seeds(const MineArgs &a) {
    std::vector<mine::SeedExpr> seeds;
    // Literature families first: when the seed list exceeds the search population
    // the SearchDriver keeps the leading seeds, and these are the economically
    // motivated ones.
    if (a.literature) {
        auto lit = mine::literature_seeds();
        seeds.insert(seeds.end(), lit.begin(), lit.end());
    }
    if (!a.fixture.empty()) {
        ATX_TRY(auto text, read_text(a.fixture));
        auto fx = mine::parse_fixture_seeds(text);
        seeds.insert(seeds.end(), fx.begin(), fx.end());
    }
    if (!a.extra_seeds.empty()) {
        ATX_TRY(auto text, read_text(a.extra_seeds));
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line.front() == '#') continue;
            seeds.push_back(mine::SeedExpr{line, "extra"});
        }
    }
    if (seeds.empty()) {
        return Err(ErrorCode::InvalidArgument, "equity-mine: no seed expressions");
    }
    // Turnover-reducing variants: a daily-rebalanced rank book pays cost on every
    // change of rank, so each base seed is also offered linearly decayed over each
    // --smooth-windows length. Every variant is a separate registered trial.
    const atx::usize base = seeds.size();
    for (const atx::usize w : a.smooth_windows) {
        for (atx::usize i = 0; i < base; ++i) {
            seeds.push_back(mine::SeedExpr{
                "decay_linear(" + seeds[i].dsl + ", " + std::to_string(w) + ")",
                seeds[i].origin + "+decay" + std::to_string(w)});
        }
    }
    return Ok(std::move(seeds));
}

[[nodiscard]] mine::MineConfig make_config(const MineArgs &a, const alpha::Panel &train_panel) {
    mine::MineConfig cfg;
    cfg.run_search = a.search;
    cfg.search.master_seed = a.seed;
    cfg.search.population = a.population;
    cfg.search.generations = a.generations;
    cfg.search.n_workers = a.threads;
    cfg.search.n_immigrants = std::max<atx::usize>(a.population / 10, 4);
    cfg.search.canon.semantic = a.semantic;
    cfg.search.output_dedup = a.output_dedup;
    cfg.search.fidelity.enabled = a.fidelity;
    cfg.search.ic_screen = a.ic_screen;
    cfg.search.ic_screen.execution_delay = a.delay;
    for (atx::usize f = 0; f < train_panel.num_fields(); ++f) {
        const std::string name{train_panel.field_name(f)};
        // raw_close duplicates close up to corporate actions; earnFlag/nEarnCnt_5d
        // are binary/count columns the numeric grammar should not swap into.
        if (name == "raw_close" || name == "earnFlag" || name == "nEarnCnt_5d") continue;
        cfg.search_fields.push_back(name);
    }
    cfg.score.cost_bps = a.cost_bps;
    cfg.score.delay = a.delay;
    cfg.score.min_names = a.min_names;
    cfg.score.guard_returns = a.guard_returns;
    cfg.score.max_abs_log_return = a.max_abs_log_return;
    cfg.score.adj_raw_log_tol = a.adj_raw_log_tol;
    cfg.max_validate = a.max_validate;
    cfg.max_corr = a.max_corr;
    cfg.min_coverage = a.min_coverage;
    cfg.fdr_q = a.fdr_q;
    cfg.rw_alpha = a.rw_alpha;
    cfg.boot.n_boot = a.n_boot;
    cfg.boot.mean_block = a.mean_block;
    cfg.boot.seed = a.seed;
    cfg.boot.threads = a.threads;
    cfg.gate = a.gate;
    cfg.threads = a.threads;
    cfg.dsr_rule = a.dsr_rule;
    return cfg;
}

[[nodiscard]] std::string_view gate_name(mine::GateMode g) {
    switch (g) {
    case mine::GateMode::By: return "benjamini-yekutieli";
    case mine::GateMode::RomanoWolf: return "romano-wolf";
    case mine::GateMode::Both: return "benjamini-yekutieli-and-romano-wolf";
    }
    return "unknown";
}

struct Windows {
    atx::i64 seal, train_start, val_start, hold_start, hold_end;
};

[[nodiscard]] atx::core::Result<Windows> parse_windows(const MineArgs &a) {
    Windows w{};
    ATX_TRY(w.seal, mine::parse_iso_date_ns(a.seal));
    ATX_TRY(w.train_start, mine::parse_iso_date_ns(a.train_start));
    ATX_TRY(w.val_start, mine::parse_iso_date_ns(a.val_start));
    ATX_TRY(w.hold_start, mine::parse_iso_date_ns(a.hold_start));
    if (a.hold_end.empty()) {
        w.hold_end = w.seal; // unused unless the holdout is published
    } else {
        ATX_TRY(w.hold_end, mine::parse_iso_date_ns(a.hold_end));
    }
    if (!(w.train_start < w.val_start && w.val_start < w.hold_start && w.hold_start < w.hold_end)) {
        return Err(ErrorCode::InvalidArgument,
                   "equity-mine: need train-start < validation-start < holdout-start < holdout-end");
    }
    if (w.hold_end > w.seal) {
        return Err(ErrorCode::InvalidArgument, "equity-mine: holdout-end is after the seal");
    }
    return Ok(w);
}

[[nodiscard]] atx::core::Result<StageResult> run_mine_stage(const MineArgs &a, std::ostream &err,
                                                           bool &created_out) {
    ATX_TRY(const Windows w, parse_windows(a));
    const std::filesystem::path out{a.out};
    if (std::filesystem::exists(out)) {
        return Err(ErrorCode::InvalidArgument, "equity-mine: --out already exists: " + a.out);
    }
    ATX_TRY(auto seeds, collect_seeds(a));
    const auto adv = adv_windows_of(seeds);

    std::optional<atx::engine::data::PitMembershipImage> image;
    std::string membership_sha;
    // parse_mine_args already refused an as-of run without an image (I-16).
    if (!a.membership.empty()) {
        ATX_TRY(auto bytes, read_text(a.membership));
        ATX_TRY(auto img, atx::engine::data::decode_membership_bin(bytes));
        ATX_TRY(membership_sha, atx::core::sha256_hex(std::string_view{bytes}));
        image = std::move(img);
    }
    const auto *img = image ? &*image : nullptr;
    std::filesystem::create_directories(out);
    created_out = true;

    static const alpha::Library lib;
    json report;
    report["schema"] = "atx.equity-mine.gate-report";
    report["schema_version"] = 1;
    report["seal"] = a.seal;
    report["membership"] = {{"path", a.membership}, {"sha256", membership_sha},
                            {"cut", a.membership_cut}, {"rule", a.membership_rule}};

    // E-16: one registry calendar for train AND validation (train trials occupy its
    // first train_T periods); counted from manifests before any span is built.
    ATX_TRY(const atx::usize val_sessions,
            count_role_sessions(a.val_ctx, w.val_start, w.hold_start));

    mine::ScoreCfg score_cfg;
    score_cfg.delay = a.delay;
    score_cfg.guard_returns = a.guard_returns;
    score_cfg.max_abs_log_return = a.max_abs_log_return;
    score_cfg.adj_raw_log_tol = a.adj_raw_log_tol;

    // ---- TRAIN: search + honest scoring + registry + family ---------------
    log_line(err, a.quiet, "building train span");
    mine::MineOutcome outcome;
    atx::usize train_T = 0;
    {
        ATX_TRY(Role train, build_role("train", a.train_ctx, w.train_start, w.val_start,
                                       "--validation-start " + a.val_start, w.seal, img,
                                       a.membership_cut, adv, a.max_working_bytes, score_cfg, a.vwap_rule));
        train.data.panel = &train.span.panel;
        report["train"] = role_json(train);
        report["train"]["return_guard"] = guard_json(train, score_cfg);
        train_T = train.data.window.size();
        eval::TrialRegistryConfig rc;
        rc.pnl_len = train_T + val_sessions;
        rc.sketch_dim = 256;
        if (a.ic_screen.rule != atx::engine::factory::IcScreenRule::DisabledV1)
            rc.format = eval::TrialLogFormat::V3;
        ATX_TRY(auto registry, eval::TrialRegistry::open(out / "trial_registry.bin", rc));
        const auto cfg = make_config(a, train.span.panel);
        log_line(err, a.quiet, "train span " + std::to_string(train.span.session_keys.size()) +
                                   "x" + std::to_string(train.span.instrument_ids.size()) +
                                   "; mining " + std::to_string(seeds.size()) + " seeds");
        ATX_TRY(outcome, mine::mine_train(lib, train.data, seeds, cfg, registry));
        log_line(err, a.quiet, "train done: candidates " + std::to_string(outcome.candidates.size()) +
                                   " family " + std::to_string(outcome.family.size()));
    }
    // ---- VALIDATION: one pass, gate ---------------------------------------
    {
        log_line(err, a.quiet, "building validation span");
        ATX_TRY(Role val, build_role("validation", a.val_ctx, w.val_start, w.hold_start,
                                     "--holdout-start " + a.hold_start, w.seal, img,
                                     a.membership_cut, adv, a.max_working_bytes, score_cfg, a.vwap_rule));
        val.data.panel = &val.span.panel;
        report["validation"] = role_json(val);
        report["validation"]["return_guard"] = guard_json(val, score_cfg);
        const auto cfg = make_config(a, val.span.panel);
        ATX_TRY_VOID(mine::mine_validate(lib, val.data, cfg, outcome));
        log_line(err, a.quiet, "validation done: admitted " + std::to_string(outcome.admitted.size()));
    }
    // ---- HOLDOUT: only with --holdout publish; reported, never selected on --
    std::vector<mine::HoldoutRow> holdout;
    report["holdout"] = {{"mode", a.holdout_publish ? "publish" : "off"},
                         {"evaluated", false},
                         {"prior_reads", a.holdout_prior_reads},
                         {"status", a.holdout_prior_reads == 0 ? "fresh" : "reused"}};
    if (a.holdout_publish) {
        log_line(err, a.quiet, "building holdout span");
        ATX_TRY(Role hold, build_role("holdout", a.hold_ctx, w.hold_start, w.hold_end,
                                      "--holdout-end " + a.hold_end, w.seal, img,
                                      a.membership_cut, adv, a.max_working_bytes, score_cfg, a.vwap_rule));
        hold.data.panel = &hold.span.panel;
        report["holdout"].update(role_json(hold));
        report["holdout"]["return_guard"] = guard_json(hold, score_cfg);
        report["holdout"]["evaluated"] = true;
        std::vector<mine::CandidateRow> adm;
        for (auto i : outcome.admitted) adm.push_back(outcome.candidates[i]);
        const auto cfg = make_config(a, hold.span.panel);
        ATX_TRY(holdout, mine::evaluate_holdout(lib, hold.data, adm, cfg.score));
        if (outcome.family_blend_scored) {
            std::vector<mine::CandidateRow> fam;
            for (auto i : outcome.family) {
                mine::CandidateRow r;
                r.dsl = outcome.candidates[i].dsl;
                r.sign = outcome.candidates[i].sign;
                fam.push_back(std::move(r));
            }
            ATX_TRY(auto fb, mine::evaluate_blend(lib, hold.data, fam, cfg.score));
            holdout.push_back(mine::HoldoutRow{std::string{kFamilyBlend}, std::move(fb)});
        }
    }

    // ---- publish -----------------------------------------------------------
    atx::usize scored = 0;
    for (const auto &c : outcome.candidates) scored += c.scored ? 1 : 0;
    report["config"] = {{"seed", a.seed},
                        {"population", a.population},
                        {"generations", a.generations},
                        {"search", a.search},
                        {"fidelity", a.fidelity},
                        {"semantic_canon", a.semantic},
                        {"output_dedup", a.output_dedup},
                        {"cost_bps_per_unit_turnover", a.cost_bps},
                        {"delay_sessions", a.delay},
                        {"allow_same_close", a.allow_same_close},
                        // I-23: the scorer's statistics knobs, recorded (were implicit).
                        {"nw_lags", score_cfg.nw_lags},
                        {"ic_horizons", score_cfg.ic_horizons},
                        {"periods_per_year", score_cfg.periods_per_year},
                        {"membership_rule", a.membership_rule},
                        {"vwap_rule", alpha::vwap_rule_name(a.vwap_rule)},
                        {"vwap_basis", a.vwap_rule == alpha::VwapRule::RawDailyCloseV2
                            ? "raw" : "adjusted_level"},
                        {"vwap_is_intraday_observation", false},
                        {"dsr_rule_requested", mine::train_dsr_rule_label(a.dsr_rule)},
                        {"min_names", a.min_names},
                        {"max_validate", a.max_validate},
                        {"max_corr", a.max_corr},
                        {"min_coverage", a.min_coverage},
                        {"gate", gate_name(a.gate)},
                        {"fdr_q", a.fdr_q},
                        {"rw_alpha", a.rw_alpha},
                        {"n_boot", a.n_boot},
                        {"mean_block", a.mean_block},
                        {"book", "rank-weighted dollar-neutral long/short, gross 1, daily rebalance"}};
    report["counts"] = {{"seeds", seeds.size()},
                        {"seeds_invalid", outcome.seeds_invalid},
                        {"candidates", outcome.candidates.size()},
                        {"scored", scored},
                        {"degenerate", outcome.degenerate},
                        {"family", outcome.family.size()},
                        {"family_rejected_corr", outcome.family_rejected_corr},
                        {"admitted", outcome.admitted.size()}};
    const auto &screen = outcome.ic_screen;
    report["ic_screen"] = {
        {"rule", atx::engine::factory::ic_screen_rule_name(screen.rule)},
        {"horizons", screen.horizons}, {"execution_delay", screen.execution_delay},
        {"window_begin", screen.window_begin}, {"window_end", screen.window_end},
        {"maturity_end", screen.maturity_end}, {"window_role", "train"},
        {"min_names", screen.min_names}, {"min_dates", screen.min_dates},
        {"practical_abs_ic", screen.practical_abs_ic}, {"confidence_multiplier", screen.confidence_multiplier},
        {"max_cache_bytes", screen.max_cache_bytes}, {"recipe", outcome.ic_screen_recipe},
        {"mine_evaluations", outcome.ic_screen_evaluations},
        {"mine_unavailable", outcome.ic_screen_unavailable},
        {"unavailable_reason", outcome.ic_screen_unavailable_reason},
        {"rejected_candidate_rows", outcome.ic_screen_rejected},
        {"search_evaluations", outcome.search_ic_screen_evaluations},
        {"search_unavailable", outcome.search_ic_screen_unavailable},
        {"search_rejected_unique", outcome.search_ic_screen_rejected},
        {"search_prepass_vm_evaluations", outcome.search_ic_prepass_vm_evaluations},
        {"search_resume_mismatch", outcome.search_ic_screen_resume_mismatch}};
    report["search"] = {{"digest", to_hex16(outcome.search_digest)},
                        {"trial_count", outcome.search_trial_count},
                        {"fidelity_evals", outcome.search_fidelity_evals},
                        {"fidelity_rejected", outcome.search_fidelity_rejected},
                        {"fingerprint_hits", outcome.search_fingerprint_hits}};
    const auto &t = outcome.trials;
    report["trials"] = {{"n_raw", t.n_raw},       {"n_eff", t.n_eff},
                        {"n_eff_uncorrected", t.n_eff_uncorrected},
                        {"mean_sr_per_period", t.mean_sr}, {"var_sr", t.var_sr},
                        {"max_sr_per_period", t.max_sr},   {"pnl_len", t.pnl_len},
                        {"train_window_len", train_T},
                        {"calendar", "train + validation sessions; train trials on [0, "
                                     "train_window_len - 1], TrialSample::InSample"},
                        {"n_in_sample", t.n_in_sample},
                        {"n_out_of_sample", t.n_out_of_sample},
                        {"n_full_pnl", t.n_full_pnl}, {"n_screened", t.n_screened},
                        {"pnl_statistics_complete", t.pnl_statistics_complete},
                        {"n_eff_full_pnl", t.n_eff_full_pnl},
                        {"n_eff_uncorrected_full_pnl", t.n_eff_uncorrected_full_pnl},
                        {"sr_moments_scope", "observed full-P&L trials only"},
                        {"log_format", a.ic_screen.rule == atx::engine::factory::IcScreenRule::DisabledV1 ? 2 : 3},
                        {"dsr_rule", outcome.dsr_rule},
                        {"dsr_fallback_reason", outcome.dsr_fallback_reason},
                        {"dsr_partial_variance_floor", t.n_screened > 0
                            ? "max(observed_full_pnl_var_sr, candidate_marginal_sharpe_variance); "
                              "marginal estimator from eval::deflated_sharpe; total raw N"
                            : "not applied"},
                        {"dsr_marginal_floor_count", outcome.dsr_marginal_floor_count},
                        {"dsr_clusters", outcome.dsr_clusters},
                        {"dsr_sr_star_mc_per_period", outcome.dsr_sr_star_mc},
                        {"chain_head", {{"records", outcome.chain_head.records},
                                        {"head", to_hex16(outcome.chain_head.head)}}},
                        {"registry_hash", to_hex16(t.registry_hash)}};
    json adm = json::array();
    for (atx::usize k = 0; k < outcome.admitted.size(); ++k) {
        const auto &c = outcome.candidates[outcome.admitted[k]];
        adm.push_back({{"id", "mine_" + std::to_string(k)}, {"dsl", c.dsl}, {"sign", c.sign},
                       {"origin", c.origin}, {"train", score_json(c.train)},
                       {"validation", score_json(c.validation)}, {"p_by", c.p_by},
                       {"p_rw", c.p_rw}, {"dsr_train", c.dsr_train}});
        if (a.holdout_publish) adm.back()["holdout"] = score_json(holdout[k].score);
    }
    report["admitted"] = adm;
    if (a.holdout_publish && !outcome.admitted.empty()) {
        report["holdout_admitted_blend"] = score_json(holdout[outcome.admitted.size()].score);
    }
    if (outcome.family_blend_scored) {
        report["family_blend"] = {{"members", outcome.family.size()},
                                  {"validation", score_json(outcome.family_blend_validation)},
                                  {"p_by", outcome.family_blend_p_by},
                                  {"p_rw", outcome.family_blend_p_rw},
                                  {"admitted", outcome.family_blend_admitted}};
        if (a.holdout_publish) {
            report["family_blend"]["holdout"] = score_json(holdout.back().score);
        }
    }
    report["qualifications"] = json::array(
        {"session keys are labels, not availability times; one-session execution delay assumed",
         a.vwap_rule == alpha::VwapRule::RawDailyCloseV2
             ? "vwap is a raw daily-close proxy; adjusted close/vwap spans different price bases"
             : "vwap is the legacy adjusted typical-price proxy, not observed intraday VWAP",
         "adjusted OHLC levels retain snapshot factors; cross-sectional basis lint remains outstanding",
         a.membership_rule == "as-of-v2"
             ? "membership as-of the PIT top-N cut; contexts are year-union compacted"
             : "membership: context year-union (pre-W0 look-ahead, requested explicitly)",
         "flat cost per unit of one-way traded weight; no borrow, impact or capacity model",
         "a missing realized return contributes zero (no delisting return imputation)",
         a.holdout_publish
             ? (a.holdout_prior_reads == 0
                    ? "holdout evaluated once in this run; nothing re-selected on it"
                    : "holdout evaluated in this run; the same period was read " +
                          std::to_string(a.holdout_prior_reads) +
                          " time(s) before, so it is a reused holdout (descriptive only)")
             : "holdout not evaluated (--holdout off)",
         "realized returns failing the adjusted-vs-raw / |log| cap guard are treated as missing",
         "sessions >= seal never read"});

    std::vector<std::pair<std::string, std::string>> files = {
        {"candidates.csv", candidates_csv(outcome)},
        {"validation.csv", validation_csv(outcome)},
        {"library.tsv", library_tsv(outcome)}};
    if (a.holdout_publish) files.emplace_back("holdout.csv", holdout_csv(holdout, outcome));
    files.emplace_back("gate_report.json", report.dump(2));
    for (const auto &[name, body] : files) ATX_TRY_VOID(write_file(out / name, body));
    json manifest;
    manifest["schema"] = "atx.equity-mine.manifest";
    manifest["schema_version"] = 1;
    json listed = json::array();
    std::vector<std::string> names;
    for (const auto &f : files) names.push_back(f.first);
    names.emplace_back("trial_registry.bin");
    for (const auto &n : names) {
        ATX_TRY(auto sha, atx::core::sha256_file((out / n).string()));
        listed.push_back({{"path", n}, {"sha256", sha},
                          {"bytes", std::filesystem::file_size(out / n)}});
    }
    manifest["files"] = listed;
    manifest["inputs"] = {{"train", report["train"]["contexts"]},
                          {"validation", report["validation"]["contexts"]},
                          {"holdout", a.holdout_publish ? report["holdout"]["contexts"]
                                                        : json::array()},
                          {"membership_sha256", membership_sha}};
    // E-16: the registry's tamper-evident head, exported OUTSIDE the log.
    manifest["trial_registry_chain_head"] = {{"records", outcome.chain_head.records},
                                             {"head", to_hex16(outcome.chain_head.head)}};
    ATX_TRY(const auto exe_sha, current_executable_sha256());
    manifest["producer_executable_sha256"] = exe_sha;
    ATX_TRY_VOID(write_file(out / "manifest.json", manifest.dump(2)));

    std::string joined;
    for (auto i : outcome.admitted) {
        joined += outcome.candidates[i].dsl + (outcome.candidates[i].sign > 0 ? "|+1\n" : "|-1\n");
    }
    StageResult sr;
    sr.digest = fnv1a64(joined.data(), joined.size());
    sr.kvs = {{"candidates", std::to_string(outcome.candidates.size())},
              {"scored", std::to_string(scored)},
              {"family", std::to_string(outcome.family.size())},
              {"admitted", std::to_string(outcome.admitted.size())},
              {"n_eff", num(t.n_eff)},
              {"family_blend_admitted", outcome.family_blend_admitted ? "1" : "0"},
              {"out", a.out}};
    return Ok(std::move(sr));
}

} // namespace

int dispatch_equity_mine(int argc, char **argv, std::ostream &out, std::ostream &err) {
    auto args = parse_mine_args(argc, argv);
    if (!args) {
        err << args.error().message() << '\n';
        return 2;
    }
    bool created = false;
    auto r = run_mine_stage(*args, err, created);
    if (!r) {
        err << r.error().message() << '\n';
        if (created) {
            const json fail{{"status", "failed"}, {"error", r.error().message()}};
            std::ofstream f(std::filesystem::path{args->out} / "failure.json");
            f << fail.dump(2);
        }
        return 1;
    }
    if (!args->quiet) emit_digest_line(out, "equity-mine", r->digest, r->kvs);
    return 0;
}

} // namespace atx::impl
