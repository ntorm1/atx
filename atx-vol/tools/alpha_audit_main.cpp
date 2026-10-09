// atx-vol-alpha-audit — adjudicate a panel's inputs and target before a fit.
//
//   atx-vol-alpha-audit --panel <vrp_panel.tsv> [--target NAME]
//                       [--features PATTERN[,PATTERN...]] [--feature-lag N]
//                       [--warmup N] [--headline] [--list]
//
// Answers four questions about a panel file that nothing in the pipeline
// currently answers together, and answers them in seconds without fitting:
//
//   1. WHAT IS IN THIS FILE. Columns resolved by NAME against the catalogue,
//      with the schema fingerprint. Columns the catalogue does not know and
//      catalogue features the file does not carry are both listed -- schema
//      DRIFT in either direction, which a `# schema=vrp_panel_v3` comment
//      cannot express.
//   2. IS THE DATA USABLE. Per-column finite fraction, range, and the
//      all-NaN / constant columns a fit would silently consume.
//   3. IS THE FEATURE SET LEGAL AGAINST THIS TARGET. Forward leaks (fatal),
//      entry-mark channels (priced, tradeable, reportable), and the feature
//      lag that would close them.
//   4. WHERE DOES THE SKILL COMPONENT LIVE. The decontaminated cross-read
//      axis, if the catalogue has one for this feature set.
//
// Exit code is 1 on a FATAL finding or a load failure, 0 otherwise. A WARN
// does not fail the process: an entry-mark channel is a fact to report, not a
// reason to refuse to run.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "atx/vol/alpha/audit.hpp"
#include "atx/vol/alpha/frame.hpp"
#include "atx/vol/alpha/registry.hpp"

namespace {

using atx::vol::alpha::AuditConfig;
using atx::vol::alpha::AuditReport;
using atx::vol::alpha::ColumnStats;
using atx::vol::alpha::FeatureSpec;
using atx::vol::alpha::FindingKind;
using atx::vol::alpha::PanelFrame;
using atx::vol::alpha::TargetSpec;

struct Args {
  std::string panel;
  std::string target{"rv_fwd_21d"};
  std::vector<std::string> features{"*"};
  std::ptrdiff_t feature_lag{0};
  std::size_t warmup{0};
  bool headline{false};
  bool list{false};
};

void usage() {
  std::fprintf(stderr,
               "usage: atx-vol-alpha-audit --panel <tsv> [--target NAME]\n"
               "         [--features PAT[,PAT...]] [--feature-lag N] [--warmup N]\n"
               "         [--headline] [--list]\n"
               "  --features  glob patterns over catalogue names, e.g. 'f4_term_slope,f1?_*'\n"
               "              (default '*'; a pattern matching nothing is an error)\n"
               "  --target    catalogue target axis (default rv_fwd_21d)\n"
               "  --headline  this target is the gate's headline number\n"
               "  --list      print the catalogue and exit\n");
}

std::vector<std::string> split_commas(const std::string &s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ',') {
      if (i > start) {
        out.push_back(s.substr(start, i - start));
      }
      start = i + 1;
    }
  }
  return out;
}

bool parse_args(int argc, char **argv, Args &args) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto next = [&](std::string &dst) {
      if (i + 1 >= argc) {
        return false;
      }
      dst = argv[++i];
      return true;
    };
    std::string value;
    if (flag == "--panel" && next(value)) {
      args.panel = value;
    } else if (flag == "--target" && next(value)) {
      args.target = value;
    } else if (flag == "--features" && next(value)) {
      args.features = split_commas(value);
    } else if (flag == "--feature-lag" && next(value)) {
      args.feature_lag = std::stoll(value);
    } else if (flag == "--warmup" && next(value)) {
      args.warmup = static_cast<std::size_t>(std::stoull(value));
    } else if (flag == "--headline") {
      args.headline = true;
    } else if (flag == "--list") {
      args.list = true;
    } else {
      std::fprintf(stderr, "unknown or incomplete flag: %s\n", flag.c_str());
      return false;
    }
  }
  if (!args.list && args.panel.empty()) {
    std::fprintf(stderr, "--panel is required\n");
    return false;
  }
  if (args.features.empty()) {
    std::fprintf(stderr, "--features resolved to no patterns\n");
    return false;
  }
  return true;
}

void print_catalogue(const atx::vol::alpha::FeatureRegistry &freg,
                     const atx::vol::alpha::TargetRegistry &treg) {
  std::printf("FEATURES (%zu)\n", freg.size());
  for (const FeatureSpec &spec : freg.all()) {
    std::printf("  %-20s %-14s prior=%-9s %s\n", spec.name.c_str(),
                std::string(to_string(spec.unit)).c_str(),
                std::string(to_string(spec.prior)).c_str(),
                spec.citation.empty() ? "(own construct)" : spec.citation.c_str());
  }
  std::printf("\nTARGETS (%zu)\n", treg.size());
  for (const TargetSpec &spec : treg.all()) {
    std::printf("  %-22s %-12s H=%-3zu %s\n", spec.name.c_str(),
                std::string(to_string(spec.unit)).c_str(), spec.horizon_sessions,
                spec.tradeable ? "TRADEABLE" : "forecast-only");
  }
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  const atx::vol::alpha::FeatureRegistry freg = atx::vol::alpha::builtin_features();
  const atx::vol::alpha::TargetRegistry treg = atx::vol::alpha::builtin_targets();

  if (args.list) {
    print_catalogue(freg, treg);
    return 0;
  }

  const TargetSpec *target = treg.find(args.target);
  if (target == nullptr) {
    std::fprintf(stderr, "unknown target '%s'; --list shows the catalogue\n", args.target.c_str());
    return 2;
  }

  std::ifstream in(args.panel, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open panel '%s'\n", args.panel.c_str());
    return 1;
  }
  auto loaded = PanelFrame::read_tsv(in);
  if (!loaded) {
    std::fprintf(stderr, "panel load failed: %s\n", loaded.error().to_string().c_str());
    return 1;
  }
  const PanelFrame &frame = *loaded;

  std::printf("PANEL      %s\n", args.panel.c_str());
  std::printf("  rows           %zu\n", frame.rows());
  std::printf("  columns        %zu\n", frame.cols());
  std::printf("  fingerprint    %016llx\n",
              static_cast<unsigned long long>(frame.schema().fingerprint()));
  for (const std::string &m : frame.meta()) {
    std::printf("  meta           %s\n", m.c_str());
  }

  // ── 1. Schema drift, both directions ──────────────────────────────────────
  std::vector<std::string> unknown_cols;
  for (const std::string &col : frame.schema().names()) {
    if (freg.find(col) == nullptr && treg.find(col) == nullptr) {
      unknown_cols.push_back(col);
    }
  }
  std::vector<std::string> absent_features;
  for (const FeatureSpec &spec : freg.all()) {
    if (!frame.schema().has(spec.name)) {
      absent_features.push_back(spec.name);
    }
  }
  std::printf("\nSCHEMA\n");
  std::printf("  catalogue features present   %zu / %zu\n", freg.size() - absent_features.size(),
              freg.size());
  if (!absent_features.empty()) {
    std::printf("  absent from panel            ");
    for (std::size_t i = 0; i < absent_features.size(); ++i) {
      std::printf("%s%s", i ? ", " : "", absent_features[i].c_str());
    }
    std::printf("\n");
  }
  if (!unknown_cols.empty()) {
    std::printf("  in panel, not in catalogue   ");
    for (std::size_t i = 0; i < unknown_cols.size(); ++i) {
      std::printf("%s%s", i ? ", " : "", unknown_cols[i].c_str());
    }
    std::printf("\n");
  }

  // ── 2. Data quality ───────────────────────────────────────────────────────
  std::printf("\nCOLUMN CENSUS  (finite / rows, range over finite values)\n");
  for (const ColumnStats &s : frame.stats()) {
    if (!s.numeric) {
      std::printf("  %-22s text      %zu rows, %zu empty\n", s.name.c_str(), s.n_rows, s.n_empty);
      continue;
    }
    std::printf("  %-22s %6.1f%%   n=%-6zu nan=%-6zu inf=%-4zu [%.6g, %.6g] mean=%.6g\n",
                s.name.c_str(), 100.0 * s.finite_fraction(), s.n_finite, s.n_nan, s.n_inf, s.min,
                s.max, s.mean);
  }
  const std::vector<std::string> unusable = frame.unusable_columns();
  if (!unusable.empty()) {
    std::printf("\n  UNUSABLE (all-NaN or constant — a fit consumes these silently):\n");
    for (const std::string &name : unusable) {
      std::printf("    %s\n", name.c_str());
    }
  }

  // ── 3. Adjudication ───────────────────────────────────────────────────────
  // Only features the panel actually CARRIES are audited: auditing a column
  // that is not in the file reports a hazard nobody can hit.
  const auto selected = freg.select(args.features);
  if (!selected) {
    std::fprintf(stderr, "\n--features: %s\n", selected.error().to_string().c_str());
    return 2;
  }
  std::vector<const FeatureSpec *> present;
  std::vector<std::string> skipped;
  for (const FeatureSpec *spec : *selected) {
    if (frame.schema().has(spec->name)) {
      present.push_back(spec);
    } else {
      skipped.push_back(spec->name);
    }
  }

  AuditConfig cfg;
  cfg.feature_lag = args.feature_lag;
  cfg.panel_warmup_sessions = args.warmup;
  cfg.target_is_headline = args.headline;
  const AuditReport report = atx::vol::alpha::audit(present, *target, cfg);

  std::printf("\nADJUDICATION   target=%s (%s), features=%zu selected / %zu in panel, lag=%td\n",
              target->name.c_str(), target->tradeable ? "TRADEABLE" : "forecast-only",
              selected->size(), present.size(), args.feature_lag);
  if (!skipped.empty()) {
    std::printf("  selected but absent from the panel (not audited): %zu\n", skipped.size());
  }
  if (report.clean()) {
    std::printf("  CLEAN — no leak, no channel, no census finding.\n");
  } else {
    for (const std::string &line : atx::vol::alpha::format_report(report)) {
      std::printf("  %s\n", line.c_str());
    }
  }

  // ── 4. Where the skill component lives ────────────────────────────────────
  const std::size_t n_channel = report.count(FindingKind::EntryMarkChannel);
  if (n_channel > 0) {
    std::printf("\nCROSS-READ\n");
    std::printf("  %zu feature(s) share an entry leg with '%s'. Their score against it\n"
                "  carries a mechanical component. ",
                n_channel, target->name.c_str());
    const std::string axis =
        atx::vol::alpha::cross_read_axis(present, treg.all(), *target, args.feature_lag);
    if (axis.empty()) {
      std::printf("No catalogue axis is free of every shared leg\n"
                  "  for this feature set; narrow --features or add a decontaminated axis.\n");
    } else {
      std::printf("Re-score them on '%s', which shares\n  no entry leg with this set.\n",
                  axis.c_str());
    }
    std::printf("  A feature lag of 1 or more removes the shared entry session outright.\n");
  }

  if (report.has_fatal()) {
    std::printf("\nRESULT  FATAL — this feature set must not be fitted against this target.\n");
    return 1;
  }
  std::printf("\nRESULT  OK (%zu finding(s), none fatal)\n", report.findings.size());
  return 0;
}
