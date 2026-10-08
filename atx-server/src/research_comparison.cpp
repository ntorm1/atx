#include "atx/server/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "research_detail.hpp"

namespace atx::server {
namespace {
using research_detail::Json;
using Period = std::pair<std::string, std::string>;

struct Series {
    const ComparisonInput* input{}; // Borrows caller's span until comparison returns.
    bool synthetic{};
    std::string evaluation;
    std::string dataset_sha256;
    std::map<Period, double> returns;
};

void cancelled(std::stop_token stop, const ComparisonOptions& options) {
    if (stop.stop_requested() || (options.is_cancelled && options.is_cancelled()))
        throw std::runtime_error("comparison cancelled");
}

Json finite(double value) { return std::isfinite(value) ? Json(value) : Json(nullptr); }

void valid_date(std::string_view date) {
    static_cast<void>(research_detail::parse_date(date));
}

std::string dataset_revision(const Json& provenance) {
    const auto legacy = provenance.value("dataset_sha256", std::string{});
    const auto revision = provenance.value("dataset_manifest_sha256", legacy);
    if ((!legacy.empty() && legacy != revision) || (!revision.empty() &&
        (revision.size() != 64 || !std::all_of(revision.begin(), revision.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })))) throw std::invalid_argument("comparison has inconsistent dataset revision evidence");
    return revision;
}

Series read_series(const ComparisonInput& input, const ComparisonOptions& options,
                   std::size_t& row_count, std::stop_token stop) {
    const auto document = research_detail::parse_json(input.result_json);
    if (document.value("schema_version", std::string{}) != "atx.research-result/v1" ||
        !document.contains("daily_returns") || !document.at("daily_returns").is_array())
        throw std::invalid_argument("run lacks complete daily returns; rerun it before comparing");
    const auto& provenance = document.at("provenance");
    if (!provenance.value("daily_returns_complete", false))
        throw std::invalid_argument("comparison requires complete return evidence");
    Series result{&input, provenance.at("synthetic").get<bool>(),
        provenance.value("evaluation", std::string("unspecified")),
        dataset_revision(provenance), {}};
    std::string previous_end;
    for (const auto& row : document.at("daily_returns")) {
        if (++row_count > 200'000) throw std::invalid_argument("comparison row budget exceeded");
        if (row_count % 256 == 0) cancelled(stop, options);
        const auto start = row.at("period_start_date").get<std::string>();
        const auto end = row.at("date").get<std::string>();
        valid_date(start);
        valid_date(end);
        const auto value = row.at("value").get<double>();
        if (start >= end || (!previous_end.empty() && previous_end > start) ||
            !std::isfinite(value) || value <= -1 || value > 1e6)
            throw std::invalid_argument("comparison has invalid or overlapping return periods");
        previous_end = end;
        if ((!options.start_date.empty() && start < options.start_date) ||
            (!options.end_date.empty() && end > options.end_date)) continue;
        if (!result.returns.emplace(Period{start, end}, value).second)
            throw std::invalid_argument("comparison has duplicate return periods");
    }
    return result;
}

std::vector<Series> read_inputs(std::span<const ComparisonInput> inputs,
                                const ComparisonOptions& options, std::stop_token stop) {
    if (inputs.size() < 2 || inputs.size() > 12 || options.max_chart_points < 2 ||
        options.max_chart_points > 2000)
        throw std::invalid_argument("comparison requires 2..12 runs and 2..2000 chart points");
    if (!options.start_date.empty()) valid_date(options.start_date);
    if (!options.end_date.empty()) valid_date(options.end_date);
    if (!options.start_date.empty() && !options.end_date.empty() &&
        options.start_date >= options.end_date)
        throw std::invalid_argument("comparison date range is reversed or empty");
    std::size_t bytes = 0, rows = 0;
    std::set<std::string> ids;
    std::vector<Series> result;
    for (const auto& input : inputs) {
        cancelled(stop, options);
        if (input.result_json.size() > 16U * 1024U * 1024U - bytes)
            throw std::invalid_argument("comparison input budget exceeds 16 MiB");
        bytes += input.result_json.size();
        if (input.job_id.empty() || input.job_id.size() > 128 || input.label.size() > 512 ||
            !ids.insert(input.job_id).second)
            throw std::invalid_argument("comparison requires distinct valid run identifiers");
        result.push_back(read_series(input, options, rows, stop));
        if (result.back().synthetic != result.front().synthetic && !options.allow_mixed_evidence)
            throw std::invalid_argument(
                "synthetic and market evidence require explicit mixed opt-in");
    }
    return result;
}

std::vector<Period> align(const std::vector<Series>& series, const ComparisonOptions& options,
                         std::stop_token stop) {
    std::vector<Period> periods;
    periods.reserve(series.front().returns.size());
    for (const auto& [period, value] : series.front().returns) {
        static_cast<void>(value);
        if (periods.size() % 256 == 0) cancelled(stop, options);
        if (std::all_of(series.begin() + 1, series.end(), [&period](const auto& item) {
                return item.returns.contains(period);
            })) periods.push_back(period);
    }
    if (periods.size() < 3)
        throw std::invalid_argument("comparison requires at least three shared return periods");
    return periods;
}

struct Moments {
    double scale{};
    double mean{};
    double sum_squares{};
    std::vector<double> scaled;
};

Moments moments(const Series& series, const std::vector<Period>& periods) {
    Moments result;
    for (const auto& period : periods)
        result.scale = std::max(result.scale, std::abs(series.returns.at(period)));
    result.scaled.reserve(periods.size());
    for (const auto& period : periods) {
        const auto value = result.scale > 0 ? series.returns.at(period) / result.scale : 0;
        result.scaled.push_back(value);
        const auto delta = value - result.mean;
        result.mean += delta / static_cast<double>(result.scaled.size());
        result.sum_squares += delta * (value - result.mean);
    }
    result.sum_squares = std::max(0.0, result.sum_squares);
    return result;
}

Json correlation(const std::vector<Moments>& stats, const ComparisonOptions& options,
                  std::stop_token stop) {
    auto matrix = Json::array();
    for (const auto& left : stats) {
        cancelled(stop, options);
        auto row = Json::array();
        for (const auto& right : stats) {
            if (left.sum_squares == 0 || right.sum_squares == 0) {
                row.push_back(nullptr);
                continue;
            }
            double product = 0;
            for (std::size_t i = 0; i < left.scaled.size(); ++i)
                product += (left.scaled[i] - left.mean) * (right.scaled[i] - right.mean);
            const auto coefficient = product / std::sqrt(left.sum_squares) /
                                     std::sqrt(right.sum_squares);
            row.push_back(finite(std::clamp(coefficient, -1.0, 1.0)));
        }
        matrix.push_back(std::move(row));
    }
    return matrix;
}

Json describe_run(const Series& series, const Moments& stats, const std::vector<Period>& periods,
                   const ComparisonOptions& options, std::stop_token stop) {
    const auto count = static_cast<double>(periods.size());
    const auto deviation = std::sqrt(stats.sum_squares / (count - 1));
    std::vector<double> growth{1};
    growth.reserve(periods.size() + 1);
    double log_growth = 0, peak_log = 0, drawdown = 0;
    std::map<std::string, double> yearly_logs;
    for (const auto& period : periods) {
        if (growth.size() % 256 == 0) cancelled(stop, options);
        const auto increment = std::log1p(series.returns.at(period));
        log_growth += increment;
        peak_log = std::max(peak_log, log_growth);
        drawdown = std::max(drawdown, -std::expm1(log_growth - peak_log));
        growth.push_back(std::exp(log_growth));
        yearly_logs[period.second.substr(0, 4)] += increment;
    }
    auto curve = Json::array(), yearly = Json::array();
    const auto displayed = std::min(options.max_chart_points, growth.size());
    for (std::size_t i = 0; i < displayed; ++i) {
        const auto index = i * (growth.size() - 1) / (displayed - 1);
        curve.push_back({{"date", index == 0 ? periods.front().first : periods[index - 1].second},
                         {"value", finite(growth[index] * 100)}});
    }
    for (const auto& [year, log_return] : yearly_logs)
        yearly.push_back({{"year", year}, {"value", finite(std::expm1(log_return))}});
    return {{"job_id", series.input->job_id}, {"label", series.input->label},
        {"synthetic", series.synthetic}, {"evaluation", series.evaluation},
        {"dataset_sha256", series.dataset_sha256},
        {"available_observations", series.returns.size()},
        {"excluded_observations", series.returns.size() - periods.size()},
        {"metrics", {{"total_return", finite(std::expm1(log_growth))},
            {"annualized_return", finite(stats.mean * stats.scale * 252)},
            {"annualized_volatility", finite(deviation * stats.scale * std::sqrt(252.0))},
            {"sharpe_ratio", deviation > 0 ? finite(stats.mean / deviation * std::sqrt(252.0))
                                           : Json(nullptr)},
            {"max_drawdown", finite(drawdown)}, {"observations", periods.size()}}},
        {"curve", std::move(curve)}, {"yearly_returns", std::move(yearly)}};
}
} // namespace

std::string compare_research_results(std::span<const ComparisonInput> inputs,
                                     const ComparisonOptions& options, std::stop_token stop) {
    cancelled(stop, options);
    try {
        const auto series = read_inputs(inputs, options, stop);
        const auto periods = align(series, options, stop);
        std::vector<Moments> stats;
        for (const auto& item : series) stats.push_back(moments(item, periods));
        auto runs = Json::array();
        bool mixed = false, gaps = false;
        std::set<std::string> revisions;
        for (std::size_t i = 0; i < series.size(); ++i) {
            runs.push_back(describe_run(series[i], stats[i], periods, options, stop));
            mixed = mixed || series[i].synthetic != series.front().synthetic;
            revisions.insert(series[i].dataset_sha256);
        }
        for (std::size_t i = 1; i < periods.size(); ++i)
            gaps = gaps || periods[i].first != periods[i - 1].second;
        auto warnings = Json::array({
            "Metrics compound only matched return periods; excluded periods are omitted",
            "Annualization assumes 252 observations per year and a zero risk-free rate",
            "Comparison describes research evidence, not a replayed portfolio combination"});
        if (mixed) warnings.push_back("Mixed synthetic and observed market evidence");
        if (gaps)
            warnings.push_back("Matched periods contain gaps; curves omit unmatched exposure");
        if (revisions.size() > 1)
            warnings.push_back("Runs use different or unspecified dataset revisions");
        cancelled(stop, options);
        return Json{{"schema_version", "atx.research-comparison/v1"},
            {"alignment", "intersection_of_exact_return_periods"}, {"observations", periods.size()},
            {"start_date", periods.front().first}, {"end_date", periods.back().second},
            {"mixed_evidence", mixed}, {"has_gaps", gaps},
            {"chart_downsampled", periods.size() + 1 > options.max_chart_points},
            {"correlation", correlation(stats, options, stop)}, {"runs", std::move(runs)},
            {"warnings", std::move(warnings)}}.dump();
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("invalid comparison evidence: ") + error.what());
    }
}
} // namespace atx::server
