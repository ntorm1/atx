// Checkpoint 14 / T6, design §9.3 (writer determinism) plus the AR-2
// record-date boundary and the §7.2 runtime membership assertion.
//
// The fixture is a SYNTHETIC context published through the real
// `equity-baseline` stage, so `equity-ic` consumes a genuine published
// evaluation.bin / combo.bin pair and the identity binding of §7.2 is exercised
// rather than stubbed. Nothing here is market data and no number below is
// evidence about any security.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "panel_artifact.hpp"
#include "stage_equity_baseline.hpp"
#include "equity_baseline_views.hpp"
#include "stage_equity_ic.hpp"
#include "trial_ledger.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using Json = nlohmann::json;
using atx::core::ErrorCode;
using atx::engine::alpha::Panel;

constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::usize kWarmup = 256;
constexpr atx::usize kEvaluationDates = 70; // > max(H) = 63, so every horizon is configurable
constexpr atx::usize kNames = 16;           // 6 audited ids + 10 plain, so _ex34 keeps 10 >= Q
constexpr const char *kEvaluationStart = "2013-04-04";
constexpr const char *kEvaluationEnd = "2013-06-13"; // exclusive; 70 observations

// Six real required-mark ids appear as instruments so the _ex34 restriction and
// the terminal flags are exercised; the remaining ten are inert fillers.
const std::array<const char *, kNames> kInstrumentIds{
    "37648", "35715", "39970", "146189", "150340", "351548", "900001", "900002", "900003",
    "900004", "900005", "900006", "900007", "900008", "900009", "900010"};

std::string contents(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

atx::usize line_count(const std::string &text) {
    return static_cast<atx::usize>(std::count(text.begin(), text.end(), '\n'));
}

std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (const char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) { fields.push_back(current); current.clear(); continue; }
        current += c;
    }
    fields.push_back(current);
    return fields;
}

class StageEquityIc : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_equity_ic_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root)) {
                fs::create_directory(data());
                return;
            }
        }
        FAIL() << "Cannot reserve fixture root";
    }

    void TearDown() override {
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (ec) return;
        const auto resolved = fs::weakly_canonical(root, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_equity_ic_")) {
            fs::remove_all(resolved, ec);
        }
    }

    [[nodiscard]] fs::path data() const { return root / "data"; }
    [[nodiscard]] fs::path baseline_dir() const { return data() / "baseline"; }
    [[nodiscard]] fs::path ledger() const { return root / "trial-ledger.jsonl"; }

    // A required-mark audit shaped like the real one: `audit_id` plus `gaps`,
    // each carrying required_gap.security_id. `ids` is the id set it declares.
    void write_audit(const std::vector<atx::i64> &ids, const fs::path &directory) const {
        fs::create_directories(directory);
        Json gaps = Json::array();
        for (const auto id : ids) {
            gaps.push_back(Json{{"classification", "source_row_absent"},
                {"required_gap", Json{{"security_id", std::to_string(id)},
                                      {"session_key_ns", "1367366400000000000"},
                                      {"session_date_utc", "2013-05-01"}}}});
        }
        std::ofstream out(directory / "manifest.json", std::ios::binary);
        out << Json{{"audit_id", std::string(64, '7')}, {"gaps", gaps}}.dump(2) << '\n';
    }

    // The 34 ids the frozen partition expects: the six that are instruments here
    // plus 28 fillers. `drop` removes one so the §7.2 assertion can be provoked.
    [[nodiscard]] std::vector<atx::i64> audit_ids(atx::i64 drop = 0) const {
        std::vector<atx::i64> ids{37648, 35715, 39970, 146189, 150340, 351548};
        for (atx::i64 filler = 0; ids.size() < 34U; ++filler) ids.push_back(700000 + filler);
        if (drop != 0) ids.erase(std::remove(ids.begin(), ids.end(), drop), ids.end());
        return ids;
    }

    // Publish a synthetic identified context and run the real baseline over it.
    // `evaluation_dates` / `evaluation_end` must agree; a count below max(H) is
    // how the C-1 failure path is reached (the engine rejects the horizon set
    // AFTER the pre-registration line is already in the ledger).
    void build_baseline(atx::usize evaluation_dates = kEvaluationDates,
                        const char *evaluation_end = kEvaluationEnd,
                        const std::string &context_name = "context.bin",
                        const std::string &baseline_name = "baseline") {
        const atx::usize dates = kWarmup + evaluation_dates;
        const auto evaluation = *atx::engine::data::detail::date_to_nanos(kEvaluationStart);
        const auto first = evaluation - static_cast<atx::i64>(kWarmup) * kDay;
        std::vector<double> close(dates * kNames);
        std::vector<atx::u8> mask(dates * kNames, 0);
        for (atx::usize d = 0; d < dates; ++d) {
            for (atx::usize i = 0; i < kNames; ++i) {
                // Distinct monotone ramps: both momentum expressions are finite
                // and the cross-section is never constant, so no IC degenerates.
                close[d * kNames + i] = 50.0 + 3.0 * static_cast<double>(i) +
                    (0.4 + 0.05 * static_cast<double>(i)) * static_cast<double>(d);
                if (d >= kWarmup) mask[d * kNames + i] = 1;
            }
        }
        std::vector<double> raw = close;
        for (auto &value : raw) value *= 0.5;
        std::vector<double> earn_flag(dates * kNames, 0.0);
        std::vector<double> iv_21(dates * kNames);
        std::vector<double> iv_126(dates * kNames);
        std::vector<double> sector(dates * kNames);
        for (atx::usize d = 0; d < dates; ++d) {
            for (atx::usize i = 0; i < kNames; ++i) {
                const auto cell = d * kNames + i;
                earn_flag[cell] = d % 63 == 10 ? 1.0 : 0.0;
                iv_21[cell] = 0.30 + 0.01 * static_cast<double>(i);
                iv_126[cell] = 0.28 + 0.01 * static_cast<double>(i);
                sector[cell] = static_cast<double>(i % 2 + 10);
            }
        }
        // Checkpoint 21 OHLC-range families read adjusted high/low/open;
        // checkpoint 22 short-interest families read si_shares and market_cap
        // (any positive finite).
        std::vector<double> si_shares = close;
        for (auto &v : si_shares) v *= 1e6;
        std::vector<double> market_cap = raw;
        for (auto &v : market_cap) v *= 1e8;
        std::vector<double> high = close;
        std::vector<double> low = close;
        std::vector<double> open = close;
        for (atx::usize cell = 0; cell < close.size(); ++cell) {
            high[cell] = close[cell] * 1.01;
            low[cell] = close[cell] * 0.98;
            open[cell] = close[cell] * 0.995;
        }
        auto panel = Panel::create(dates, kNames,
            {"close", "raw_close", "volume", "earnFlag", "atmCenI_21d", "atmCenI_126d", "sector",
             "high", "low", "open", "si_shares", "market_cap"},
            {close, raw, std::vector<double>(dates * kNames, 1e6), earn_flag, iv_21, iv_126,
             sector, high, low, open, si_shares, market_cap}, std::move(mask));
        ASSERT_TRUE(panel.has_value()) << panel.error().message();
        impl::PanelIdentity identity;
        identity.instrument_namespace = impl::kSpiderRockSecurityIdNamespace;
        for (atx::usize i = 0; i < kNames; ++i) {
            identity.instrument_ids.emplace_back(kInstrumentIds[i]);
            identity.original_instrument_indices.push_back(i);
        }
        for (atx::usize d = 0; d < dates; ++d) {
            identity.session_keys.push_back(first + static_cast<atx::i64>(d) * kDay);
        }
        identity.parents = {{"synthetic-fixture", std::string(64, 'a')}};
        identity.recipe = Json{{"version", "tickerhistory-panel-v2-identified"},
            {"start_inclusive_nanos", std::to_string(first)},
            {"end_exclusive_nanos", std::to_string(first + static_cast<atx::i64>(dates) * kDay)},
            {"research_ohlc", "raw-OHLC*cumulReturnFactor-pointwise"},
            {"raw_close", "unadjusted-as-traded"},
            {"volume", "raw-reported-volume-no-total-return-factor-rescaling"},
            {"universe", {{"adv_basis", "raw_close*raw_volume"},
                {"top_n_tie_break", "original-instrument-index"}, {"top_n_by_adv", 1000},
                {"min_adv_usd", 20e6}, {"min_raw_price_exclusive", 5.0}, {"adv_window_bars", 21},
                {"min_mktcap_usd", 0.0}, {"require_sector", false},
                {"current_session_data_included", true},
                {"adv_missing", "full-trailing-window-any-NaN-invalid"}}},
            {"compact_to_universe", true},
            {"compaction", "keep-original-order-if-ever-in-universe-in-selected-window"},
            {"augmentation", {{"enabled", false}, {"adv_windows", Json::array()}}},
            {"historical_availability", "unknown-archive-snapshot"},
            {"fixture", "synthetic-only-no-market-or-performance-evidence"}}.dump();
        const auto written =
            impl::write_panel_artifact(*panel, (data() / context_name).string(), identity);
        ASSERT_TRUE(written.has_value()) << written.error().message();
        impl::RunConfig cfg;
        cfg.subcommand = "equity-baseline";
        cfg.panel = (data() / context_name).string();
        cfg.out = (data() / baseline_name).string();
        cfg.equity_evaluation_start = kEvaluationStart;
        cfg.equity_evaluation_end = evaluation_end;
        cfg.report_aum = 1000;
        const auto built = impl::run_equity_baseline(cfg);
        ASSERT_TRUE(built.has_value()) << built.error().message();
    }

    [[nodiscard]] impl::RunConfig ic_config(const std::string &out,
                                            const char *evaluation_end = kEvaluationEnd,
                                            const std::string &context_name = "context.bin",
                                            const std::string &baseline_name = "baseline") const {
        impl::RunConfig cfg;
        cfg.subcommand = "equity-ic";
        cfg.panel = (data() / context_name).string();
        cfg.equity_baseline_dir = (data() / baseline_name).string();
        cfg.out = (root / out).string();
        cfg.equity_evaluation_start = kEvaluationStart;
        cfg.equity_evaluation_end = evaluation_end;
        cfg.equity_trial_ledger = ledger().string();
        return cfg;
    }
};
} // namespace

// --- ruling AR-2, both sides of the record date ----------------------------

TEST(EquityIcTerminalValue, ObservationAtOrBeforeRecordDateIncludesSpecialDividend) {
    const auto at = *atx::engine::data::detail::date_to_nanos("2013-10-28");
    const auto before = *atx::engine::data::detail::date_to_nanos("2013-10-25");
    EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(35715, at), 13.75 + 0.13);
    EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(35715, before), 13.75 + 0.13);
}

TEST(EquityIcTerminalValue, ObservationAfterRecordDateExcludesSpecialDividend) {
    const auto after = *atx::engine::data::detail::date_to_nanos("2013-10-29");
    const auto later = *atx::engine::data::detail::date_to_nanos("2013-11-01");
    EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(35715, after), 13.75);
    EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(35715, later), 13.75);
}

TEST(EquityIcTerminalValue, EventWithoutARecordDateIsTheConsiderationAlone) {
    for (const char *date : {"2013-04-04", "2013-10-28", "2013-12-31"}) {
        const auto key = *atx::engine::data::detail::date_to_nanos(date);
        EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(37648, key), 72.50);
        EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(39970, key), 38.68);
    }
}

TEST(EquityIcTerminalValue, UnevidencedOrUnknownIdIsZeroAndPcsIsNeverPriced) {
    const auto key = *atx::engine::data::detail::date_to_nanos("2013-05-01");
    EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(146189, key), 0.0);
    EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(150340, key), 0.0);
    EXPECT_DOUBLE_EQ(impl::equity_ic_terminal_value(999999, key), 0.0);
}

// --- §9.3 writer determinism and the published schema ----------------------

TEST_F(StageEquityIc, TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput) {
    ASSERT_NO_FATAL_FAILURE(build_baseline());
    write_audit(audit_ids(), data() / "equity_source_reconciliation_2013_20260919");

    const auto first = impl::run_equity_ic(ic_config("ic1"));
    ASSERT_TRUE(first.has_value()) << first.error().message();
    const auto second = impl::run_equity_ic(ic_config("ic2"));
    ASSERT_TRUE(second.has_value()) << second.error().message();

    const std::array<const char *, 7> deterministic{"seal.json", "coverage.csv", "ic.csv",
        "ic_decay.csv", "signal_autocorr.csv", "quantile_spread.csv", "ic_summary.json"};
    for (const auto *name : deterministic) {
        const auto a = contents(root / "ic1" / name);
        const auto b = contents(root / "ic2" / name);
        ASSERT_FALSE(a.empty()) << name;
        EXPECT_EQ(a, b) << name << " is not reproducible";
    }
    // request.json and manifest.json legitimately differ: each run binds to its
    // OWN append-only ledger line, which carries that run's UTC stamp.
    EXPECT_TRUE(fs::exists(root / "ic1" / "request.json"));
    EXPECT_TRUE(fs::exists(root / "ic1" / "manifest.json"));
    EXPECT_FALSE(fs::exists(root / "ic1" / ".pending"));

    // (2 baseline + blend + families) signals x 2 variants x 2 restrictions x sum over H of (T - h).
    const atx::usize signal_count =
        impl::kEquityBaselineDsl.size() + 1U + impl::kEquityFamilyDsl.size();
    const atx::usize blocks = signal_count * 2U * 2U;
    // Checkpoint 18 declares only the NEW family configurations per run.
    const int declared_per_run = static_cast<int>(
        (impl::kEquityFamilyDsl.size() - impl::kEquityFamilyRetainedCount) * 5U * 2U * 2U);
    const atx::usize date_rows = blocks * ((kEvaluationDates - 1U) + (kEvaluationDates - 5U) +
        (kEvaluationDates - 10U) + (kEvaluationDates - 21U) + (kEvaluationDates - 63U));
    EXPECT_EQ(line_count(contents(root / "ic1" / "ic.csv")), date_rows + 1U);
    EXPECT_EQ(line_count(contents(root / "ic1" / "coverage.csv")), date_rows + 1U);
    EXPECT_EQ(line_count(contents(root / "ic1" / "ic_decay.csv")), blocks * 5U + 1U);
    EXPECT_EQ(line_count(contents(root / "ic1" / "signal_autocorr.csv")), signal_count + 1U);
    // One comment row, one header, then 10 bucket rows + 1 SPREAD row per block.
    EXPECT_EQ(line_count(contents(root / "ic1" / "quantile_spread.csv")),
              blocks * 5U * 11U + 2U);

    // Ruling NEW-4's scale sentence is carried where the column is read.
    const auto spread_text = contents(root / "ic1" / "quantile_spread.csv");
    ASSERT_EQ(spread_text.rfind("# ", 0), 0U);
    EXPECT_NE(spread_text.find("2.0, not 1.0"), std::string::npos);

    // §10.5's statement is verbatim in ic_summary.json.
    const auto summary = Json::parse(contents(root / "ic1" / "ic_summary.json"));
    EXPECT_EQ(summary.at("trial_count_declared"), declared_per_run);
    EXPECT_EQ(summary.at("checkpoint"), 22);
    EXPECT_EQ(summary.at("alignment"),
              "signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1");
    EXPECT_NE(summary.at("sign_and_shape_statement").get<std::string>().find(
                  "sign-and-shape evidence only"), std::string::npos);
    EXPECT_EQ(summary.at("series").size(), blocks * 5U);
    EXPECT_EQ(summary.at("common_sample_dates"), kEvaluationDates - 63U);

    // §7.4 null encoding: h = 63 has n = 7 < 20, so its interval is unreportable
    // and BOTH serializers say so — JSON null, CSV "" — never 0 and never NaN.
    bool checked = false;
    for (const auto &row : summary.at("series")) {
        if (row.at("horizon") != 63) continue;
        const auto &full = row.at("full");
        EXPECT_TRUE(full.at("ic_mean_ci").at("lo").is_null());
        EXPECT_TRUE(full.at("ic_mean_ci").at("hi").is_null());
        EXPECT_FALSE(full.at("ic_mean_ci").at("reportable").get<bool>());
        EXPECT_EQ(full.at("ic_mean_ci").at("unreportable_reason"), 1);
        EXPECT_EQ(full.at("ic_mean_ci").at("unreportable_reason_text"),
                  "series-shorter-than-twenty");
        // §3.13: the common block is the prefix [0, T - max(H)) = 7 dates for
        // EVERY horizon, and every prefix date emits here, so the block reports
        // its mean with no gap — while its interval stays null at n = 7 < 20.
        const auto &common = row.at("common");
        EXPECT_TRUE(common.at("summary_reportable").get<bool>());
        EXPECT_EQ(common.at("dates_emitted"), kEvaluationDates - 63U);
        EXPECT_EQ(common.at("common_prefix_gaps"), 0);
        EXPECT_FALSE(common.at("ic_mean").is_null());
        EXPECT_TRUE(common.at("ic_mean_ci").at("lo").is_null());
        EXPECT_EQ(common.at("ic_mean_ci").at("unreportable_reason"), 1);
        // §11.8 / §11.9 I-4: a CLOSED SPREAD GATE nulls the four spread moments.
        // The spread series here is the same 7 dates, so its own gate is shut and
        // a 0.0 spread mean over an unreportable series must never be published.
        EXPECT_FALSE(row.at("spread_reportable").get<bool>());
        EXPECT_NE(row.at("spread_unreportable_reason"), 0);
        for (const auto *field : {"spread_gross_mean", "spread_gross_sd", "spread_net_mean",
                                  "spread_net_sd"}) {
            EXPECT_TRUE(full.at(field).is_null()) << field << " must be null at h=63";
            EXPECT_TRUE(common.at(field).is_null()) << field << " (common) must be null";
        }
        EXPECT_FALSE(full.at("spread_reportable").get<bool>());
        EXPECT_TRUE(full.at("spread_gross_ci").at("lo").is_null());
        EXPECT_TRUE(full.at("spread_net_ci").at("lo").is_null());
        // M-2: "false with no reason" is unreadable; a closed gate always names one.
        EXPECT_NE(full.at("spread_gross_ci").at("unreportable_reason"), 0);
        checked = true;
    }
    EXPECT_TRUE(checked) << "no horizon-63 summary row was emitted";

    std::istringstream decay(contents(root / "ic1" / "ic_decay.csv"));
    std::string header;
    std::getline(decay, header);
    const auto columns = split_csv(header);
    ASSERT_EQ(columns.size(), 19U);
    EXPECT_EQ(columns[5], "ic_mean");
    EXPECT_EQ(columns[6], "ic_lo");
    bool saw_63 = false;
    for (std::string line; std::getline(decay, line);) {
        const auto fields = split_csv(line);
        ASSERT_EQ(fields.size(), 19U);
        if (fields[3] != "63") continue;
        EXPECT_TRUE(fields[6].empty()) << "h=63 ic_lo must be empty, got " << fields[6];
        EXPECT_TRUE(fields[7].empty());
        EXPECT_EQ(fields[11], "0");
        saw_63 = true;
    }
    EXPECT_TRUE(saw_63);

    // §11.9 I-4, CSV half: the same closed spread gate in quantile_spread.csv.
    std::istringstream spread_csv(spread_text);
    std::string comment, spread_header;
    std::getline(spread_csv, comment);
    std::getline(spread_csv, spread_header);
    const auto spread_columns = split_csv(spread_header);
    ASSERT_EQ(spread_columns.size(), 28U);
    EXPECT_EQ(spread_columns[8], "gross_mean");
    EXPECT_EQ(spread_columns[23], "spread_reportable");
    EXPECT_EQ(spread_columns[24], "spread_unreportable_reason");
    EXPECT_EQ(spread_columns[27], "mean_dollar_adv");
    atx::usize closed_spread_rows = 0;
    for (std::string line; std::getline(spread_csv, line);) {
        const auto fields = split_csv(line);
        ASSERT_EQ(fields.size(), 28U) << line;
        if (fields[4] != "SPREAD" || fields[1] != "63") continue;
        for (const std::size_t column : {8U, 9U, 12U, 13U}) {
            EXPECT_TRUE(fields[column].empty())
                << spread_columns[column] << " must be empty at h=63, got " << fields[column];
        }
        EXPECT_EQ(fields[23], "0");
        EXPECT_NE(fields[24], "0");
        ++closed_spread_rows;
    }
    EXPECT_EQ(closed_spread_rows, blocks);

    // §4.5: two lines per run, never one. Two runs => four links, chain intact.
    const auto head = impl::verify_trial_ledger(ledger().string());
    ASSERT_TRUE(head.has_value()) << head.error().message();
    EXPECT_EQ(head->lines, 4U);
    const auto declared = impl::declared_trials_for_checkpoint(ledger().string(), 22);
    ASSERT_TRUE(declared.has_value()) << declared.error().message();
    EXPECT_EQ(*declared, 2U * static_cast<atx::usize>(declared_per_run)); // two pre-registration lines

    const auto request = Json::parse(contents(root / "ic1" / "request.json"));
    EXPECT_EQ(request.at("trial_ledger").at("trial_id"), "iteration22-cross-section-ic-0001");
    const auto request2 = Json::parse(contents(root / "ic2" / "request.json"));
    EXPECT_EQ(request2.at("trial_ledger").at("trial_id"), "iteration22-cross-section-ic-0002");
    EXPECT_EQ(request.at("required_mark_audit").at("required_mark_id_count"), 34);
    EXPECT_EQ(request.at("required_mark_audit").at("terminal_unevidenced_ids"),
              Json::array({146189}));
    ASSERT_EQ(request.at("terminal_evidence").size(), 3U);
    EXPECT_EQ(request.at("terminal_evidence").at(1).at("security_id"), 35715);
    EXPECT_EQ(request.at("terminal_evidence").at(1).at("record_date"), "2013-10-28");

    const auto manifest = Json::parse(contents(root / "ic1" / "manifest.json"));
    EXPECT_EQ(manifest.at("status"), "complete");
    EXPECT_EQ(manifest.at("terminal_evidence").at("pcs_applied"), false);
    EXPECT_EQ(manifest.at("predictions_confirmed").at("modulo_fallbacks"), 0);
    EXPECT_NE(manifest.at("cost_model_provenance").get<std::string>().find(
                  "no call into replay.cpp borrow_charge"), std::string::npos);
    EXPECT_EQ(manifest.at("predictions_confirmed").at("dates_below_min_names_max_over_blocks"), 0);
    // §11.9 I-1/I-3: §7.3's two runtime keys are present; the peak names its
    // measuring party rather than reading as "not measured at all".
    EXPECT_FALSE(manifest.at("runtime").at("runtime_seconds").is_null());
    EXPECT_GE(manifest.at("runtime").at("runtime_seconds").get<double>(), 0.0);
    EXPECT_TRUE(manifest.at("runtime").at("peak_working_set_bytes").is_null());
    EXPECT_NE(manifest.at("runtime").at("peak_working_set_source").get<std::string>().find(
                  "iteration14_run_equity_ic.py"), std::string::npos);
    // §11.9 I-2: the embedded design-note binding reaches request.json.
    EXPECT_EQ(request.at("design_note").at("path"),
              "atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md");
    EXPECT_EQ(request.at("design_note").at("sha256").get<std::string>().size(), 64U);

    // §5.2: a live gate that a 2013-only panel does not trip.
    const auto seal = Json::parse(contents(root / "ic1" / "seal.json"));
    EXPECT_EQ(seal.at("policy"), "RejectSealedV1");
    EXPECT_EQ(seal.at("dates_at_or_after_sealed"), 0);
    EXPECT_EQ(seal.at("dates_at_or_after_validation"), 0);
    EXPECT_EQ(seal.at("statement"),
              "non-vacuous by code; vacuous by data for a 2013-only panel");
}

// --- §11.9 C-1: the pre-registration append is a point of no return --------

TEST_F(StageEquityIc, AFailureAfterPreRegistrationStillAppendsAFailedTerminalLine) {
    // 40 evaluation observations is shorter than max(H) = 63, so
    // plan_cross_section_ic rejects the frozen horizon set — and it runs AFTER
    // the pre-registration line is already in the ledger. §4.5: "A failed run
    // keeps both lines."
    ASSERT_NO_FATAL_FAILURE(
        build_baseline(40, "2013-05-14", "short_context.bin", "short_baseline"));
    write_audit(audit_ids(), data() / "equity_source_reconciliation_2013_20260919");

    const auto failed = impl::run_equity_ic(
        ic_config("ic_short", "2013-05-14", "short_context.bin", "short_baseline"));
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code(), ErrorCode::OutOfRange);

    const auto head = impl::verify_trial_ledger(ledger().string());
    ASSERT_TRUE(head.has_value()) << head.error().message();
    ASSERT_EQ(head->lines, 2U);

    std::ifstream in(ledger(), std::ios::binary);
    std::string first_line, second_line;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, first_line)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, second_line)));
    const Json pre = Json::parse(first_line);
    const Json done = Json::parse(second_line);
    EXPECT_EQ(pre.at("status"), "pre-registered");
    EXPECT_EQ(pre.at("result").at("outcome"), "pending");
    EXPECT_EQ(done.at("status"), "failed");
    EXPECT_EQ(done.at("result").at("outcome"), "failed");
    EXPECT_EQ(done.at("trial_id"), pre.at("trial_id"));
    ASSERT_FALSE(done.at("result").at("failure_sha256").is_null());
    EXPECT_EQ(done.at("result").at("failure_sha256").get<std::string>().size(), 64U);
    EXPECT_TRUE(done.at("result").at("manifest_sha256").is_null());
    // §11.9 I-1: wall time is the producer's; the peak is the runner's.
    EXPECT_FALSE(done.at("runtime").at("wall_seconds").is_null());
    EXPECT_TRUE(done.at("runtime").at("peak_working_set_bytes").is_null());

    // The digest on the terminal line names a failure.json that exists, and the
    // directory still advertises itself as incomplete.
    EXPECT_TRUE(fs::exists(root / "ic_short" / "failure.json"));
    EXPECT_TRUE(fs::exists(root / "ic_short" / ".pending"));
    EXPECT_FALSE(fs::exists(root / "ic_short" / "manifest.json"));

    // §11.9 I-2: the design-note parent binds BOTH lines to the frozen revision.
    for (const Json *line : {&pre, &done}) {
        bool saw_design_note = false;
        for (const auto &parent : line->at("parents")) {
            if (parent.at("role") != "design-note") continue;
            saw_design_note = true;
            EXPECT_EQ(parent.at("sha256").get<std::string>().size(), 64U);
        }
        EXPECT_TRUE(saw_design_note) << "a ledger line carries no design-note parent";
    }
}

// --- §7.2 / §11.4: the inference the run depends on is checked by the run ---

TEST_F(StageEquityIc, RequiredMarkAuditFailuresRejectBeforePublishingAManifest) {
    ASSERT_NO_FATAL_FAILURE(build_baseline());
    const auto audit = data() / "equity_source_reconciliation_2013_20260919";

    const auto missing = impl::run_equity_ic(ic_config("ic_missing"));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(root / "ic_missing" / "manifest.json"));
    EXPECT_TRUE(fs::exists(root / "ic_missing" / "failure.json"));

    // 34 ids, but PCS replaced by a filler: the membership assertion must fire
    // rather than the cardinality one, because the partition rests on PCS.
    auto without_pcs = audit_ids(146189);
    without_pcs.push_back(700098);
    ASSERT_EQ(without_pcs.size(), 34U);
    write_audit(without_pcs, audit);
    const auto no_pcs = impl::run_equity_ic(ic_config("ic_no_pcs"));
    ASSERT_FALSE(no_pcs);
    EXPECT_EQ(no_pcs.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(no_pcs.error().message().find("PCS 146189"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "ic_no_pcs" / "manifest.json"));

    auto wrong_set = audit_ids();
    wrong_set.push_back(700099); // 35 ids: the cardinality assertion must fire
    write_audit(wrong_set, audit);
    const auto wrong_count = impl::run_equity_ic(ic_config("ic_count"));
    ASSERT_FALSE(wrong_count);
    EXPECT_EQ(wrong_count.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(wrong_count.error().message().find("expected 34"), std::string::npos);

    // Nothing above appended a ledger line: the pre-registration happens only
    // after every input binding has succeeded.
    EXPECT_FALSE(fs::exists(ledger()));
}
