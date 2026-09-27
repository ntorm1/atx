#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/data/exposure_panel.hpp"
#include "atx/engine/factory/factory.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/objective_ic.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/factory/search_progress.hpp"

namespace {
namespace fac = atx::engine::factory;
namespace data = atx::engine::data;
using atx::usize;
using atx::f64;
using atx::core::Result;
constexpr usize kNames = 64;
constexpr atx::i64 kDay = 86400000000000LL;
const std::string kPriceSource(64, 'a');

struct Fixture {
    atx::engine::alpha::Panel panel;
    data::ExposurePanel exposures;
    std::vector<atx::i64> sessions, decisions, marks, ids;
    std::vector<f64> planted, size_proxy;
    fac::ObjectiveIcInputs inputs() const {
        fac::ObjectiveIcInputs in;
        in.session_keys = sessions; in.decision_times_ns = decisions;
        in.mark_times_ns = marks; in.instrument_ids = ids;
        in.instrument_namespace = "synthetic.security";
        in.expected_exposure_axis_sha256 = exposures.axis_sha256();
        in.expected_exposure_content_sha256 = exposures.content_sha256();
        in.price_source_sha256 = kPriceSource;
        return in;
    }
};

Result<Fixture> fixture(usize dates = 192, usize warmup_missing = 0, bool singular = false,
                        usize mutate_after = std::numeric_limits<usize>::max(),
                        bool missing_finite_price = false, bool nearly_collinear = false) {
    data::ExposurePanelConfig ecfg;
    ecfg.instrument_namespace = "synthetic.security";
    for (usize i = 0; i < kNames; ++i) ecfg.instrument_ids.push_back(static_cast<atx::i64>(i + 1));
    std::vector<atx::i64> marks;
    for (usize d = 0; d < dates; ++d) {
        const auto session = (static_cast<atx::i64>(d) + 15000) * kDay;
        ecfg.session_keys.push_back(session);
        marks.push_back(session + kDay / 2);
        ecfg.decision_times_ns.push_back(marks.back() + 1);
    }
    for (usize k = 0; k < ecfg.descriptor_recipes.size(); ++k)
        ecfg.descriptor_recipes[k] = "synthetic-harmonic-" + std::to_string(k);
    for (const std::string role : {"identity", "membership", "prices", "cap", "industry"})
        ecfg.parents.push_back({role, kPriceSource});
    ecfg.max_working_bytes = 32U * 1024U * 1024U;
    ATX_TRY(auto builder, data::ExposurePanelBuilder::create(ecfg));
    std::vector<f64> close(dates * kNames), planted(dates * kNames);
    std::vector<atx::u8> presence(dates * kNames, 1);
    std::vector<data::ExposureObservation> raw(6 * kNames), cap(kNames);
    std::vector<data::ExposureIndustryObservation> industry(kNames);
    std::vector<data::ExposureInterval> identity(kNames);
    std::vector<atx::u8> yes(kNames, 1);
    std::vector<atx::i64> available(kNames);
    for (usize d = 0; d < dates; ++d) {
        std::fill(available.begin(), available.end(), marks[d] - 1);
        for (usize i = 0; i < kNames; ++i) {
            const f64 theta = 2 * std::numbers::pi_v<f64> * static_cast<f64>(i) / static_cast<f64>(kNames);
            const f64 alpha = std::cos(9 * theta), size = std::cos(7 * theta);
            close[d * kNames + i] = 100 * std::exp(0.001 * static_cast<f64>(d) * (alpha + 0.2 * size));
            planted[d * kNames + i] = alpha;
            if (d >= mutate_after) close[d * kNames + i] *= 1 + static_cast<f64>(i) / 100;
            cap[i] = {1e9 * std::exp(0.4 * size), marks[d] - 2, marks[d] - 1, true};
            identity[i] = {ecfg.session_keys.front() - 1, marks[d] - 1,
                std::numeric_limits<atx::i64>::max(), std::numeric_limits<atx::i64>::max(), true};
            industry[i] = {static_cast<atx::u8>(1 + i % 3), identity[i]};
            for (usize k = 0; k < 6; ++k) {
                const auto frequency = singular && k == 5 ? usize{1} : k + 1;
                f64 value = 2 + std::sin(static_cast<f64>(frequency) * theta);
                if (nearly_collinear && k == 1)
                    value = 2 + std::sin(theta) + std::ldexp(std::cos(8 * theta), -36);
                if (d >= mutate_after) value += 0.1 * std::cos(11 * theta);
                raw[k * kNames + i] = {value, marks[d] - 2, marks[d] - 1, true};
            }
        }
        data::ExposureRowInput row;
        if (d >= warmup_missing) row.raw_descriptors = raw;
        row.market_cap_usd = cap; row.industry = industry; row.identity = identity;
        row.source_present = yes; row.member = yes;
        row.presence_available_ns = available; row.membership_available_ns = available;
        row.presence_qualified = yes; row.membership_qualified = yes;
        ATX_TRY_VOID(builder.append_date(d, row));
    }
    if (missing_finite_price) {
        presence[23 * kNames] = 0;
        close[23 * kNames] = 777; // finite backing value must not become a valid label
    }
    ATX_TRY(auto exposures, builder.finish());
    ATX_TRY(auto panel, atx::engine::alpha::Panel::create(dates, kNames, {"close"},
        {std::move(close)}, std::move(presence)));
    data::ExposureExtractConfig extract;
    extract.allow_synthetic = true;
    const auto good_date = std::min(warmup_missing, dates - 1);
    ATX_TRY(auto row, data::extract_exposure_date(exposures, good_date, exposures.axis_sha256(),
        exposures.content_sha256(), ecfg.decision_times_ns[good_date], extract));
    std::vector<f64> proxy(dates * kNames);
    for (usize d = 0; d < dates; ++d) for (usize r = 0; r < row.original_slots.size(); ++r)
        proxy[d * kNames + row.original_slots[r]] = row.values[r * 7];
    return Fixture{std::move(panel), std::move(exposures), std::move(ecfg.session_keys),
        std::move(ecfg.decision_times_ns), std::move(marks), std::move(ecfg.instrument_ids),
        std::move(planted), std::move(proxy)};
}
fac::ObjectiveIcConfig config() {
    fac::ObjectiveIcConfig cfg;
    cfg.allow_synthetic_exposures = true;
    cfg.min_names = 20; cfg.min_dates = 16;
    cfg.max_working_bytes = 32U * 1024U * 1024U;
    return cfg;
}

Result<Fixture> consumer_fixture() {
    ATX_TRY(auto f, fixture(320));
    auto mixed = f.planted;
    for (usize d = 0; d < f.panel.dates(); ++d) for (usize i = 0; i < kNames; ++i) {
        const f64 theta = 2 * std::numbers::pi_v<f64> * static_cast<f64>(i) / static_cast<f64>(kNames);
        mixed[d * kNames + i] += 8 * f.size_proxy[d * kNames + i] +
            0.7 * std::sin(0.2 * static_cast<f64>(d)) * std::cos(10 * theta);
    }
    auto inverse = mixed;
    for (auto &value : inverse) value = -value;
    ATX_TRY(auto close_id, f.panel.field_id("close"));
    const auto close = f.panel.field_all(close_id);
    ATX_TRY(auto panel, atx::engine::alpha::Panel::create(f.panel.dates(), kNames,
        {"close", "proxy", "mixed", "inverse"},
        {std::vector<f64>(close.begin(), close.end()), f.size_proxy, std::move(mixed), std::move(inverse)}, {}));
    f.panel = std::move(panel);
    return f;
}
atx::engine::exec::ExecutionSimulator consumer_simulator() {
    using namespace atx::engine::exec;
    return ExecutionSimulator{FillCfg{}, SlippageCfg{SlippageMode::VolumeShare, 0, 0, 0, 0},
        ImpactCfg{0, 0.5, 0}, CommissionCfg{CommissionMode::PerShare, 0, 0, 1, 0},
        LatencyCfg{}, VolumeCapCfg{1}};
}
Result<fac::Genome> consumer_genome(std::string_view expression, const atx::engine::alpha::Library &library) {
    ATX_TRY(auto ast, atx::engine::alpha::parse_expr(expression, library));
    ATX_TRY(auto info, atx::engine::alpha::analyze(ast));
    return fac::Genome{std::move(ast), std::move(info), 0};
}
fac::SearchConfig consumer_config(const fac::ResidualFitnessBinding &binding) {
    fac::SearchConfig cfg;
    cfg.master_seed = 71; cfg.population = 3; cfg.generations = 1;
    cfg.elites = 1; cfg.k_tournament = 2; cfg.n_workers = 1;
    cfg.seed_from_grammar = false;
    cfg.objective_mode = fac::ObjectiveMode::ScalarRaw;
    cfg.fitness.objective_rule = fac::FitnessObjectiveRule::ResidualHacIcV2;
    cfg.fitness.residual_binding = &binding;
    return cfg;
}
} // namespace

TEST(FactoryResidualObjective, SqrtCapProjectionRemovesProxyAndPreservesSmallRealResidual) {
    auto f = fixture(); ASSERT_TRUE(f) << f.error().message();
    auto context = fac::prepare_objective_ic(f->panel, f->exposures, f->inputs(), config());
    ASSERT_TRUE(context) << context.error().message();
    auto scratch = fac::prepare_objective_ic_scratch(*context); ASSERT_TRUE(scratch);
    auto proxy = fac::evaluate_objective_ic(f->size_proxy, *context, *scratch);
    ASSERT_TRUE(proxy) << proxy.error().message();
    EXPECT_EQ(proxy->residual_valid_dates, 0U);
    EXPECT_EQ(proxy->residual_degenerate_dates, f->panel.dates());
    auto planted = fac::evaluate_objective_ic(f->planted, *context, *scratch);
    ASSERT_TRUE(planted);
    EXPECT_EQ(planted->residual_valid_dates, f->panel.dates());
    for (const auto &h : planted->horizons) EXPECT_GT(h.mean, 0.8);
    data::ExposureExtractConfig ec;
    ec.allow_synthetic = true;
    const auto row = data::extract_exposure_date(f->exposures, 0, f->exposures.axis_sha256(),
        f->exposures.content_sha256(), f->decisions[0], ec);
    ASSERT_TRUE(row);
    const f64 largest = *std::max_element(row->market_cap_usd.begin(), row->market_cap_usd.end());
    // Independent normal-equation condition using sqrt-cap statistical weights.
    for (usize column = 0; column < 11; ++column) {
        f64 cross = 0;
        for (usize r = 0; r < row->original_slots.size(); ++r) {
            const auto i = row->original_slots[r];
            const f64 x = column == 0 ? 1 : column <= 7 ? row->values[r * 7 + column - 1]
                : static_cast<f64>(row->ff49[r] == column - 7);
            cross += x * std::sqrt(row->market_cap_usd[r] / largest) * scratch->residuals()[i];
        }
        EXPECT_NEAR(cross / static_cast<f64>(kNames), 0, 1e-12);
    }
    auto small = f->size_proxy;
    for (usize i = 0; i < small.size(); ++i) small[i] += 1e-6 * f->planted[i];
    const auto weak_amplitude = fac::evaluate_objective_ic(small, *context, *scratch);
    ASSERT_TRUE(weak_amplitude);
    EXPECT_EQ(weak_amplitude->residual_valid_dates, f->panel.dates());
    EXPECT_GT(weak_amplitude->horizons[0].mean, 0.8);
    for (auto &v : small) v = -v;
    const auto inverse = fac::evaluate_objective_ic(small, *context, *scratch);
    ASSERT_TRUE(inverse);
    EXPECT_NEAR(inverse->horizons[0].mean, -weak_amplitude->horizons[0].mean, 1e-12);
}

TEST(FactoryResidualObjective, NearCollinearExactProxyCannotRankCancellationDust) {
    auto f = fixture(192, 0, false, std::numeric_limits<usize>::max(), false, true);
    ASSERT_TRUE(f) << f.error().message();
    auto context = fac::prepare_objective_ic(f->panel, f->exposures, f->inputs(), config());
    ASSERT_TRUE(context) << context.error().message();
    auto scratch = fac::prepare_objective_ic_scratch(*context); ASSERT_TRUE(scratch);
    data::ExposureExtractConfig ec;
    ec.allow_synthetic = true;
    const auto row = data::extract_exposure_date(f->exposures, 0, f->exposures.axis_sha256(),
        f->exposures.content_sha256(), f->decisions[0], ec);
    ASSERT_TRUE(row);
    // Exact stored-column combination: normalization roundoff must not turn
    // an approximate raw-input proxy into an intentionally genuine residual.
    std::vector<f64> proxy(f->panel.dates() * kNames);
    for (usize d = 0; d < f->panel.dates(); ++d)
        for (usize r = 0; r < row->original_slots.size(); ++r)
            proxy[d * kNames + row->original_slots[r]] =
                std::ldexp(row->values[r * 7 + 2] - row->values[r * 7 + 1], 36);
    const auto pure = fac::evaluate_objective_ic(proxy, *context, *scratch);
    ASSERT_TRUE(pure);
    EXPECT_EQ(pure->rank_deficient_dates, 0U); // exercise accepted near-collinear QR
    EXPECT_EQ(pure->residual_degenerate_dates, f->panel.dates());
    EXPECT_EQ(pure->residual_valid_dates, 0U);
    for (const auto &d : scratch->date_diagnostics()) {
        EXPECT_EQ(d.rank, d.design_columns);
        EXPECT_GT(d.qr_pivot_ratio, 0);
        EXPECT_LT(d.qr_pivot_ratio, 1e-9);
        EXPECT_GT(d.fitted_absolute_product_scale, 1e9);
        EXPECT_LE(d.max_abs_residual, d.residual_roundoff_floor);
    }
    // A residual distinguishable at this design's disclosed numerical scale
    // survives. The separate well-conditioned fixture retains amplitude 1e-6.
    for (usize i = 0; i < proxy.size(); ++i) proxy[i] += 2 * f->planted[i];
    const auto real = fac::evaluate_objective_ic(proxy, *context, *scratch);
    ASSERT_TRUE(real);
    EXPECT_EQ(real->rank_deficient_dates, 0U);
    EXPECT_EQ(real->residual_valid_dates, f->panel.dates());
    EXPECT_GT(real->horizons[0].mean, 0.8);
    for (const auto &d : scratch->date_diagnostics())
        EXPECT_GT(d.max_abs_residual, d.residual_roundoff_floor);
}

TEST(FactoryResidualObjective, WarmupMissingAndRankDeficiencyAreUnavailableNotZeroScores) {
    auto warm = fixture(192, 8); ASSERT_TRUE(warm) << warm.error().message();
    auto context = fac::prepare_objective_ic(warm->panel, warm->exposures, warm->inputs(), config());
    ASSERT_TRUE(context) << context.error().message();
    auto scratch = fac::prepare_objective_ic_scratch(*context); ASSERT_TRUE(scratch);
    auto result = fac::evaluate_objective_ic(warm->planted, *context, *scratch); ASSERT_TRUE(result);
    EXPECT_EQ(result->residual_valid_dates, 184U);
    for (usize d = 0; d < 8; ++d) {
        EXPECT_EQ(scratch->date_diagnostics()[d].status, fac::ObjectiveIcDateStatus::ExposureUnavailable);
        EXPECT_EQ(scratch->date_diagnostics()[d].known_member_names, kNames);
        EXPECT_TRUE(std::isnan(scratch->rank_ic_series(0)[d]));
    }
    auto bad = fixture(192, 0, true); ASSERT_TRUE(bad);
    auto bad_context = fac::prepare_objective_ic(bad->panel, bad->exposures, bad->inputs(), config());
    ASSERT_TRUE(bad_context);
    auto bad_scratch = fac::prepare_objective_ic_scratch(*bad_context); ASSERT_TRUE(bad_scratch);
    const auto bad_result = fac::evaluate_objective_ic(bad->planted, *bad_context, *bad_scratch);
    ASSERT_TRUE(bad_result);
    EXPECT_EQ(bad_result->rank_deficient_dates, bad->panel.dates());
    EXPECT_FALSE(bad_result->horizons[0].inference_defined);
    EXPECT_EQ(bad_result->horizons[0].valid_dates, 0U);
}

TEST(FactoryResidualObjective, CausalMaturityAndExplicitPricePresenceSurviveFutureMutation) {
    auto a = fixture(240); auto b = fixture(240, 0, false, 192);
    ASSERT_TRUE(a); ASSERT_TRUE(b);
    auto cfg = config(); cfg.maturity_end = 192;
    auto ca = fac::prepare_objective_ic(a->panel, a->exposures, a->inputs(), cfg);
    auto cb = fac::prepare_objective_ic(b->panel, b->exposures, b->inputs(), cfg);
    ASSERT_TRUE(ca); ASSERT_TRUE(cb);
    auto sa = fac::prepare_objective_ic_scratch(*ca); auto sb = fac::prepare_objective_ic_scratch(*cb);
    ASSERT_TRUE(sa); ASSERT_TRUE(sb);
    const auto ra = fac::evaluate_objective_ic(a->planted, *ca, *sa);
    const auto rb = fac::evaluate_objective_ic(b->planted, *cb, *sb);
    ASSERT_TRUE(ra); ASSERT_TRUE(rb);
    for (usize k = 0; k < 3; ++k) {
        EXPECT_EQ(ra->horizons[k].mean, rb->horizons[k].mean);
        EXPECT_EQ(ra->horizons[k].calendar_dates, 192U - 1U - ra->horizons[k].horizon);
        for (usize d = 0; d < 240; ++d) {
            const auto x = sa->rank_ic_series(k)[d], y = sb->rank_ic_series(k)[d];
            if (std::isnan(x)) EXPECT_TRUE(std::isnan(y)); else EXPECT_EQ(x, y);
        }
    }
    auto absent = fixture(240, 0, false, std::numeric_limits<usize>::max(), true); ASSERT_TRUE(absent);
    auto cx = fac::prepare_objective_ic(absent->panel, absent->exposures, absent->inputs(), cfg); ASSERT_TRUE(cx);
    auto sx = fac::prepare_objective_ic_scratch(*cx); ASSERT_TRUE(sx);
    const auto rx = fac::evaluate_objective_ic(absent->planted, *cx, *sx); ASSERT_TRUE(rx);
    EXPECT_LT(rx->horizons[0].paired_names, ra->horizons[0].paired_names);
    EXPECT_TRUE(std::isnan(sx->residuals()[23 * kNames]));
}

TEST(FactoryResidualObjective, CalendarHacMatchesIndependentLagSumAndPersistenceSkipsGaps) {
    auto f = fixture(320); ASSERT_TRUE(f);
    auto cfg = config(); cfg.min_date_fraction = 0.5;
    auto c = fac::prepare_objective_ic(f->panel, f->exposures, f->inputs(), cfg); ASSERT_TRUE(c);
    auto s = fac::prepare_objective_ic_scratch(*c); ASSERT_TRUE(s);
    auto signal = f->planted;
    for (usize d = 0; d < 320; ++d) for (usize i = 0; i < kNames; ++i) {
        const f64 theta = 2 * std::numbers::pi_v<f64> * static_cast<f64>(i) / static_cast<f64>(kNames);
        signal[d * kNames + i] += 0.7 * std::sin(0.2 * static_cast<f64>(d)) * std::cos(10 * theta);
        if (d % 5 == 0) signal[d * kNames + i] = std::numeric_limits<f64>::quiet_NaN();
    }
    const auto result = fac::evaluate_objective_ic(signal, *c, *s); ASSERT_TRUE(result);
    EXPECT_EQ(result->persistence_pairs, 192U); // only adjacent valid calendar days, never compressed pairs
    for (usize k = 0; k < 3; ++k) {
        const auto &h = result->horizons[k];
        ASSERT_TRUE(h.inference_defined) << k;
        EXPECT_LT(h.valid_dates, h.calendar_dates);
        EXPECT_GE(h.hac_lag, h.horizon - 1);
        std::vector<f64> influence(h.calendar_dates);
        for (usize d = 0; d < h.calendar_dates; ++d)
            influence[d] = std::isfinite(s->rank_ic_series(k)[d]) ? s->rank_ic_series(k)[d] - h.mean : 0;
        f64 sum = 0;
        for (const auto x : influence) sum += x * x;
        for (usize lag = 1; lag <= h.hac_lag; ++lag) {
            f64 cross = 0;
            for (usize d = lag; d < influence.size(); ++d) cross += influence[d] * influence[d - lag];
            const f64 weight = h.effective_kernel == fac::ObjectiveIcKernel::Uniform ? 1
                : 1 - static_cast<f64>(lag) / static_cast<f64>(h.hac_lag + 1);
            sum += 2 * weight * cross;
        }
        const f64 n = static_cast<f64>(h.valid_dates);
        EXPECT_NEAR(h.standard_error * h.standard_error, sum / (n * (n - 1)), 1e-13);
        EXPECT_NEAR(h.hac_ir, h.t_stat / std::sqrt(n), 1e-13);
    }
}

TEST(FactoryResidualObjective, RefusesUnboundAxesSourceClocksScratchAndWorkingBudget) {
    auto f = fixture(); ASSERT_TRUE(f);
    auto in = f->inputs(); auto cfg = config();
    const std::string wrong(64, 'b');
    in.price_source_sha256 = wrong;
    EXPECT_FALSE(fac::prepare_objective_ic(f->panel, f->exposures, in, cfg));
    in = f->inputs(); in.expected_exposure_content_sha256 = wrong;
    EXPECT_FALSE(fac::prepare_objective_ic(f->panel, f->exposures, in, cfg));
    in = f->inputs(); auto ids = f->ids; std::swap(ids[0], ids[1]); in.instrument_ids = ids;
    EXPECT_FALSE(fac::prepare_objective_ic(f->panel, f->exposures, in, cfg));
    in = f->inputs(); auto marks = f->marks; marks[0] = f->decisions[0]; in.mark_times_ns = marks;
    EXPECT_FALSE(fac::prepare_objective_ic(f->panel, f->exposures, in, cfg));
    in = f->inputs(); cfg.max_working_bytes = f->exposures.bytes();
    EXPECT_FALSE(fac::prepare_objective_ic(f->panel, f->exposures, in, cfg));
    cfg = config(); cfg.execution_delay = 0;
    EXPECT_FALSE(fac::prepare_objective_ic(f->panel, f->exposures, in, cfg));
    cfg = config();
    auto c = fac::prepare_objective_ic(f->panel, f->exposures, in, cfg); ASSERT_TRUE(c);
    fac::ObjectiveIcScratch empty;
    EXPECT_FALSE(fac::evaluate_objective_ic(f->planted, *c, empty));
}

TEST(FactoryResidualConsumer, ActualFitnessRanksResidualAndInverseWithoutInventingPnl) {
    auto f = consumer_fixture(); ASSERT_TRUE(f) << f.error().message();
    auto context = fac::prepare_objective_ic(f->panel, f->exposures, f->inputs(), config());
    ASSERT_TRUE(context);
    auto binding = fac::prepare_residual_fitness_binding(*context, f->panel); ASSERT_TRUE(binding);
    auto cfg = consumer_config(*binding).fitness;
    auto scratch = fac::prepare_objective_ic_scratch(*context); ASSERT_TRUE(scratch);
    cfg.residual_scratch = &*scratch;
    atx::engine::alpha::Library library;
    atx::engine::combine::AlphaStore empty;
    atx::engine::WeightPolicy policy;
    const auto sim = consumer_simulator();
    auto mixed = consumer_genome("mixed", library); ASSERT_TRUE(mixed);
    auto inverse = consumer_genome("inverse", library); ASSERT_TRUE(inverse);
    auto proxy = consumer_genome("proxy", library); ASSERT_TRUE(proxy);
    const auto good = fac::pool_aware_fitness(*mixed, empty, f->panel, policy, sim, cfg);
    const auto opposite = fac::pool_aware_fitness(*inverse, empty, f->panel, policy, sim, cfg);
    const auto unavailable = fac::pool_aware_fitness(*proxy, empty, f->panel, policy, sim, cfg);
    ASSERT_TRUE(good); ASSERT_TRUE(opposite); ASSERT_TRUE(unavailable);
    ASSERT_TRUE(good->residual_available); ASSERT_TRUE(opposite->residual_available);
    EXPECT_GT(good->raw, 0);
    EXPECT_NEAR(good->raw, -opposite->raw, 1e-10);
    EXPECT_FALSE(unavailable->residual_available);
    EXPECT_EQ(unavailable->raw, fac::kRejectedRaw);
    EXPECT_GT(opposite->raw, unavailable->raw); // meaningful negative beats missing
    f64 expected = 0;
    for (const auto &h : good->residual_ic.horizons) {
        ASSERT_TRUE(h.inference_defined);
        expected += h.hac_ir / 3;
    }
    EXPECT_DOUBLE_EQ(good->raw, expected);
    EXPECT_TRUE(good->descriptor.empty());
    EXPECT_TRUE(std::isnan(good->wq)); EXPECT_TRUE(std::isnan(good->dsr));
    EXPECT_TRUE(std::isnan(good->turnover));
    EXPECT_EQ(good->residual_ic.context_sha256, context->identity_sha256());
}

TEST(FactoryResidualConsumer, ActualSearchRetainsUnavailableTrialsAndWorkerInvariantScores) {
    auto f = consumer_fixture(); ASSERT_TRUE(f);
    auto recipe = config(); recipe.workers = 2;
    auto context = fac::prepare_objective_ic(f->panel, f->exposures, f->inputs(), recipe); ASSERT_TRUE(context);
    auto binding = fac::prepare_residual_fitness_binding(*context, f->panel); ASSERT_TRUE(binding);
    auto cfg = consumer_config(*binding);
    atx::engine::alpha::Library library;
    atx::engine::combine::AlphaStore empty;
    atx::engine::WeightPolicy policy;
    const auto sim = consumer_simulator();
    fac::SearchDriver driver{library, f->panel, policy, sim,
        {"proxy", "mixed", "inverse"}, {"close", "proxy", "mixed", "inverse"}};
    const auto one = driver.run(cfg, empty);
    ASSERT_FALSE(one.residual_invalid) << one.residual_error;
    cfg.n_workers = 2;
    const auto two = driver.run(cfg, empty);
    ASSERT_FALSE(two.residual_invalid) << two.residual_error;
    EXPECT_EQ(one.digest, two.digest);
    EXPECT_EQ(one.trial_count, 3U);
    EXPECT_EQ(one.all_scored.size(), 3U);
    ASSERT_EQ(one.residual_scores.size(), 3U);
    ASSERT_EQ(two.residual_scores.size(), 3U);
    ASSERT_EQ(one.residual_unavailable_hashes.size(), 1U);
    EXPECT_EQ(one.admitted_candidates.size(), 2U);
    EXPECT_EQ(one.ic_screen_evaluations, 0U);
    for (usize i = 0; i < one.residual_scores.size(); ++i) {
        EXPECT_EQ(one.residual_scores[i].canon_hash, two.residual_scores[i].canon_hash);
        EXPECT_EQ(one.residual_scores[i].status, two.residual_scores[i].status);
        EXPECT_EQ(one.residual_scores[i].score, two.residual_scores[i].score);
    }
    for (const auto &g : one.admitted_candidates)
        EXPECT_NE(g.canon_hash, one.residual_unavailable_hashes.front());
    std::vector<atx::u64> keys{one.residual_unavailable_hashes.front()};
    std::vector<fac::CachedScore> scores{fac::residual_unavailable_score()};
    const auto encoded = fac::serialize_cache(keys, scores);
    std::vector<atx::u64> restored_keys;
    std::vector<fac::CachedScore> restored_scores;
    ASSERT_TRUE(fac::deserialize_cache(encoded, restored_keys, restored_scores));
    ASSERT_EQ(restored_scores.size(), 1U);
    EXPECT_EQ(restored_scores.front().origin, fac::ScoreOrigin::ResidualUnavailable);
}

TEST(FactoryResidualConsumer, RefusesRawScreenWrongBindingResumeAndLegacyAdmission) {
    auto f = consumer_fixture(); ASSERT_TRUE(f);
    auto context = fac::prepare_objective_ic(f->panel, f->exposures, f->inputs(), config()); ASSERT_TRUE(context);
    auto binding = fac::prepare_residual_fitness_binding(*context, f->panel); ASSERT_TRUE(binding);
    auto cfg = consumer_config(*binding);
    atx::engine::alpha::Library library;
    atx::engine::combine::AlphaStore empty;
    atx::engine::WeightPolicy policy;
    const auto sim = consumer_simulator();
    fac::SearchDriver driver{library, f->panel, policy, sim, {"mixed"}, {"close", "mixed"}};
    cfg.ic_screen = fac::equivalence_ic_screen_config();
    const auto wrong_gate = driver.run(cfg, empty);
    EXPECT_TRUE(wrong_gate.residual_invalid);
    EXPECT_EQ(wrong_gate.trial_count, 0U);
    cfg = consumer_config(*binding); cfg.fidelity.enabled = true;
    EXPECT_TRUE(driver.run(cfg, empty).residual_invalid);
    cfg = consumer_config(*binding); cfg.deflate_selection = true;
    EXPECT_TRUE(driver.run(cfg, empty).residual_invalid);
    cfg = consumer_config(*binding); cfg.fitness.residual_binding = nullptr;
    EXPECT_TRUE(driver.run(cfg, empty).residual_invalid);
    cfg = consumer_config(*binding); cfg.n_workers = 100000;
    EXPECT_TRUE(driver.run(cfg, empty).residual_invalid); // refused before any thread launch
    cfg = consumer_config(*binding); cfg.n_workers = 0;
    EXPECT_TRUE(driver.run(cfg, empty).residual_invalid); // no implicit machine-size budget
    cfg = consumer_config(*binding);
    fac::SearchResumeState resume;
    EXPECT_TRUE(driver.run(cfg, empty, nullptr, &resume).residual_invalid);
    auto changed = fixture(320, 0, false, 0); ASSERT_TRUE(changed);
    EXPECT_FALSE(fac::prepare_residual_fitness_binding(*context, changed->panel));
    auto copy = f->panel; // matching bytes can rebind, but cannot reuse another object's binding
    EXPECT_FALSE(binding->matches(copy));
    EXPECT_TRUE(fac::prepare_residual_fitness_binding(*context, copy));
    fac::Factory factory{library, f->panel, sim, policy};
    fac::FactoryConfig fc;
    fc.search = cfg;
    const auto refused = factory.mine(fc, empty, atx::engine::combine::AlphaGate{});
    EXPECT_TRUE(refused.residual_invalid);
    EXPECT_EQ(refused.evaluated, 0U);
    EXPECT_EQ(empty.n_alphas(), 0U);
}
