#pragma once
// ── atx::vol::alpha — the input/target adjudicator ──────────────────────────
//
// Answers, BEFORE a fit runs and without reading a single row of data: is this
// feature set legal against this target, and where is its score going to come
// from something other than forecasting skill?
//
// It works purely on the declared `reads` of `spec.hpp`, so it costs
// microseconds and can run on every invocation rather than as a quarterly
// audit. That matters because the two defects it looks for have both already
// shipped in this repo and both were caught by a human reading source:
//
//   * ROUND 8 -- a benchmark predictor that carried its own target's entry
//     mark and scored Spearman +0.9935 against it, read for three rounds as
//     "the baseline beats the GBT".
//   * ROUND 9 -- twelve new features of which three (f12, f14, and f21's ATMF
//     pivot) carry the same hazard, documented only in prose ("Same entry-mark
//     caveat as f12", "must not be read at lag 0").
//
// ── THE DISTINCTION THIS FILE REFUSES TO COLLAPSE ───────────────────────────
//
// FORWARD LEAK and ENTRY-MARK CHANNEL are both "the feature knows something
// about the target", and they have opposite remedies:
//
//   FORWARD LEAK      feature reads a session AFTER t. The number is fiction.
//                     Remedy: delete the feature. Severity Fatal.
//
//   ENTRY-MARK        feature and target share a series+tenor at a session
//   CHANNEL           <= t. Every leg is known at entry; a book can hold this.
//                     The predictor is mechanically correlated with the target
//                     whether or not it forecasts anything, so its score is
//                     not evidence of skill. Remedy: report it AND cross-read
//                     on an axis that excludes the shared leg. Severity Warn.
//
// Collapsing them is how `f5_hv_iv_gap` -- a real, published, tradeable
// channel -- gets deleted as a bug, and how a real bug gets defended as a
// channel. `dh_straddle_pnl_21d` is the clean illustration: it pays
// (rv_fwd - iv_fair_21d), so EVERY feature reading the entry IV mark scores
// against it partly through the channel. That is the trade, not a defect. The
// separation comes from also scoring on `rv_fwd_21d`, which has no IV leg at
// all -- which is exactly what `cross_read_axis()` below recommends.
//
// ── WHAT A FEATURE LAG DOES, COMPUTED RATHER THAN ASSERTED ──────────────────
//
// `--feature-lag k` shifts every feature window k sessions further into the
// past. Whether that CLOSES a channel is arithmetic on the windows, so
// `audit()` takes the lag and reports the post-lag state. A comment claiming
// "lag 2 closes this" becomes a fact the caller can print.

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/alpha/spec.hpp"

namespace atx::vol::alpha {

enum class Severity : std::uint8_t { Info, Warn, Fatal };

[[nodiscard]] constexpr std::string_view to_string(Severity sev) noexcept {
  switch (sev) {
  case Severity::Info:
    return "INFO";
  case Severity::Warn:
    return "WARN";
  case Severity::Fatal:
    return "FATAL";
  }
  return "?";
}

enum class FindingKind : std::uint8_t {
  // The feature reads a session strictly after t. Lookahead.
  ForwardLeak,
  // The feature reads a session inside the target's own forward window. A
  // strictly worse case of ForwardLeak: the feature is reading the answer.
  TargetWindowLeak,
  // Feature and target share a series+tenor at a session <= t. Priced channel,
  // not lookahead.
  EntryMarkChannel,
  // Feature and target both read implied-vol marks of the same tenor at the
  // same session, through DIFFERENT reads (strip vs ATMF). Correlated, not
  // identical.
  CorrelatedEntryMark,
  // Two features consume the SAME inputs over the SAME windows. Not
  // necessarily redundant -- the transform on top can differ, and
  // `f2_log_rv21` (log variance) vs `f7_ret_21d` (summed return) are genuinely
  // different information from one window of spot. But it is where redundancy
  // lives, and `f5_hv_iv_gap` vs `f6_vrp_lag` -- the log-ratio and the
  // difference of literally the same two quantities -- is the case worth
  // finding. Info, because "same footprint" is a lead, not a verdict.
  SharedInputFootprint,
  // A feature's declared window is malformed (first > last).
  MalformedWindow,
  // The target is a forecast axis, not a P&L. Legal to report, illegal to
  // headline as a return.
  NonTradeableTarget,
  // The feature needs more trailing history than the panel's warmup provides.
  InsufficientWarmup,
};

[[nodiscard]] constexpr std::string_view to_string(FindingKind kind) noexcept {
  switch (kind) {
  case FindingKind::ForwardLeak:
    return "forward_leak";
  case FindingKind::TargetWindowLeak:
    return "target_window_leak";
  case FindingKind::EntryMarkChannel:
    return "entry_mark_channel";
  case FindingKind::CorrelatedEntryMark:
    return "correlated_entry_mark";
  case FindingKind::SharedInputFootprint:
    return "shared_input_footprint";
  case FindingKind::MalformedWindow:
    return "malformed_window";
  case FindingKind::NonTradeableTarget:
    return "non_tradeable_target";
  case FindingKind::InsufficientWarmup:
    return "insufficient_warmup";
  }
  return "unknown";
}

[[nodiscard]] constexpr Severity severity_of(FindingKind kind) noexcept {
  switch (kind) {
  case FindingKind::ForwardLeak:
  case FindingKind::TargetWindowLeak:
  case FindingKind::MalformedWindow:
    return Severity::Fatal;
  case FindingKind::EntryMarkChannel:
  case FindingKind::NonTradeableTarget:
  case FindingKind::InsufficientWarmup:
    return Severity::Warn;
  case FindingKind::CorrelatedEntryMark:
  case FindingKind::SharedInputFootprint:
    return Severity::Info;
  }
  return Severity::Warn;
}

struct Finding {
  FindingKind kind{FindingKind::ForwardLeak};
  Severity severity{Severity::Fatal};
  std::string subject; // the feature (or target) the finding is about
  std::string detail;  // human-readable, names the shared series and sessions

  [[nodiscard]] bool fatal() const noexcept { return severity == Severity::Fatal; }
};

// Configuration for one adjudication.
struct AuditConfig {
  // Sessions every feature window is shifted into the past before checking.
  // This is `--feature-lag`, and it is the knob that closes entry-mark channels.
  std::ptrdiff_t feature_lag{0};
  // Trailing sessions the panel actually has before its first labeled row. A
  // feature needing more than this emits nothing but NaN; 0 disables the check.
  std::size_t panel_warmup_sessions{0};
  // Is this target being used as the HEADLINE number of a gate? A forecast
  // axis in that seat is reported; a forecast axis merely alongside is not.
  bool target_is_headline{false};
};

struct AuditReport {
  std::vector<Finding> findings;

  [[nodiscard]] bool clean() const noexcept { return findings.empty(); }

  [[nodiscard]] bool has_fatal() const noexcept {
    return std::any_of(findings.begin(), findings.end(),
                       [](const Finding &f) { return f.fatal(); });
  }

  [[nodiscard]] std::size_t count(FindingKind kind) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(),
                      [kind](const Finding &f) { return f.kind == kind; }));
  }

  // Names of the features carrying at least one finding of `kind`, in the
  // order they were audited. This is the list a caller feeds to `--feature-lag`
  // or drops.
  [[nodiscard]] std::vector<std::string> subjects(FindingKind kind) const {
    std::vector<std::string> out;
    for (const Finding &f : findings) {
      if (f.kind == kind &&
          std::find(out.begin(), out.end(), f.subject) == out.end()) {
        out.push_back(f.subject);
      }
    }
    return out;
  }
};

namespace audit_detail {

[[nodiscard]] inline std::string window_str(const Window &w) {
  return "[t" + (w.first < 0 ? std::to_string(w.first) : "+" + std::to_string(w.first)) + ", t" +
         (w.last < 0 ? std::to_string(w.last) : "+" + std::to_string(w.last)) + "]";
}

[[nodiscard]] inline std::string ref_str(const SeriesRef &ref) {
  std::string out(to_string(ref.series));
  if (ref.tenor_sessions != 0) {
    out += "@" + std::to_string(ref.tenor_sessions) + "d";
  }
  out += window_str(ref.window);
  return out;
}

// The sessions two windows share, as an inclusive range. Callers check
// `overlaps()` first.
[[nodiscard]] inline Window intersect(const Window &a, const Window &b) noexcept {
  return Window{std::max(a.first, b.first), std::min(a.last, b.last)};
}

// Two read-sets have the same input footprint when they cover the same
// quantities over the same windows.
[[nodiscard]] inline bool reads_equal(const std::vector<SeriesRef> &a,
                                      const std::vector<SeriesRef> &b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  // Specs are written smallest-first by hand and the catalogue is closed, so an
  // order-sensitive compare would produce spurious misses; compare as sets by
  // requiring every element of `a` to be matched by some element of `b`. Sizes
  // are equal and elements within one spec are distinct, so this is a bijection.
  for (const SeriesRef &ra : a) {
    const bool found = std::any_of(b.begin(), b.end(), [&ra](const SeriesRef &rb) {
      return ra.same_quantity(rb) && ra.window == rb.window;
    });
    if (!found) {
      return false;
    }
  }
  return true;
}

} // namespace audit_detail

// ── The adjudication ────────────────────────────────────────────────────────
//
// `features` are borrowed, never owned; they typically come straight out of
// `FeatureRegistry::select()`.
[[nodiscard]] inline AuditReport audit(std::span<const FeatureSpec *const> features,
                                       const TargetSpec &target, const AuditConfig &cfg) {
  AuditReport report;
  const auto add = [&report](FindingKind kind, std::string subject, std::string detail) {
    report.findings.push_back(Finding{kind, severity_of(kind), std::move(subject), std::move(detail)});
  };

  if (cfg.target_is_headline && !target.tradeable) {
    add(FindingKind::NonTradeableTarget, target.name,
        "'" + target.name +
            "' is a forecast axis, not a holdable P&L; a gate headline on it "
            "quotes a forecast as a return. Report it alongside a tradeable "
            "axis instead.");
  }

  const std::vector<SeriesRef> target_entry_legs = target.entry_legs();
  const std::ptrdiff_t target_reach = target.forward_reach();

  for (const FeatureSpec *feature : features) {
    if (feature == nullptr) {
      continue;
    }
    const std::string &fname = feature->name;

    // Warmup is a property of the UNLAGGED windows plus the lag itself: a
    // lagged feature needs `lag` more sessions of history than it declares.
    if (cfg.panel_warmup_sessions > 0) {
      const std::size_t need =
          feature->warmup_sessions() + (cfg.feature_lag > 0 ? static_cast<std::size_t>(cfg.feature_lag) : 0U);
      if (need > cfg.panel_warmup_sessions) {
        add(FindingKind::InsufficientWarmup, fname,
            fname + " needs " + std::to_string(need) + " trailing sessions (window " +
                std::to_string(feature->warmup_sessions()) + " + lag " +
                std::to_string(cfg.feature_lag) + ") but the panel provides " +
                std::to_string(cfg.panel_warmup_sessions) + "; every row inside the warmup is NaN.");
      }
    }

    for (const SeriesRef &raw : feature->reads) {
      if (!raw.window.well_formed()) {
        add(FindingKind::MalformedWindow, fname,
            fname + " declares " + audit_detail::ref_str(raw) + " with first > last.");
        continue;
      }
      const SeriesRef ref{raw.series, raw.tenor_sessions, raw.window.lagged(cfg.feature_lag)};

      // 1. Lookahead, in two grades. Reading inside the target's own forward
      //    window is the severe form and is reported instead of, not as well
      //    as, the general one.
      if (!ref.window.causal()) {
        const bool inside_target_window =
            target_reach > 0 && ref.window.last > 0 &&
            std::any_of(target.reads.begin(), target.reads.end(), [&ref](const SeriesRef &tr) {
              return ref.same_quantity(tr) && tr.window.last > 0 && ref.window.overlaps(tr.window);
            });
        if (inside_target_window) {
          add(FindingKind::TargetWindowLeak, fname,
              fname + " reads " + audit_detail::ref_str(ref) +
                  " which lands inside the forward window target '" + target.name +
                  "' is built from. The feature is reading the answer.");
        } else {
          add(FindingKind::ForwardLeak, fname,
              fname + " reads " + audit_detail::ref_str(ref) +
                  " at feature lag " + std::to_string(cfg.feature_lag) +
                  "; the window ends after the entry session.");
        }
        continue;
      }

      // 2. Shared entry legs. Same quantity + overlapping sessions, both at or
      //    before t: a priced channel, not a leak.
      for (const SeriesRef &leg : target_entry_legs) {
        if (!ref.window.overlaps(leg.window)) {
          continue;
        }
        if (ref.same_quantity(leg)) {
          const Window shared = audit_detail::intersect(ref.window, leg.window);
          add(FindingKind::EntryMarkChannel, fname,
              fname + " and target '" + target.name + "' both read " +
                  std::string(to_string(ref.series)) +
                  (ref.tenor_sessions != 0 ? "@" + std::to_string(ref.tenor_sessions) + "d" : "") +
                  " over " + audit_detail::window_str(shared) +
                  ". Known at entry, so tradeable -- but the score carries a "
                  "mechanical component independent of forecasting skill. "
                  "Cross-read on an axis without this leg.");
        } else if (is_implied_mark(ref.series) && is_implied_mark(leg.series) &&
                   ref.tenor_sessions == leg.tenor_sessions) {
          add(FindingKind::CorrelatedEntryMark, fname,
              fname + " reads " + std::string(to_string(ref.series)) + " while target '" +
                  target.name + "' reads " + std::string(to_string(leg.series)) +
                  " at the same tenor and session. Different reads of the same "
                  "implied mark: correlated, not identical.");
        }
      }
    }
  }

  // 3. Features drawing on the same inputs over the same windows.
  for (std::size_t i = 0; i < features.size(); ++i) {
    if (features[i] == nullptr) {
      continue;
    }
    for (std::size_t j = i + 1; j < features.size(); ++j) {
      if (features[j] == nullptr) {
        continue;
      }
      if (audit_detail::reads_equal(features[i]->reads, features[j]->reads)) {
        add(FindingKind::SharedInputFootprint, features[i]->name,
            features[i]->name + " and " + features[j]->name +
                " read the same series over the same windows. The transform on "
                "top may still differ -- check whether one is a monotone "
                "re-expression of the other before treating both as "
                "independent evidence.");
      }
    }
  }

  return report;
}

// ── The decontaminated cross-read ───────────────────────────────────────────
//
// Given a target and the audit's entry-mark findings, which OTHER registered
// target excludes every shared leg? That axis is where the skill component of
// the score lives, and reporting the pair is the whole remedy for an
// entry-mark channel.
//
// Returns the name of the first target in `candidates` that shares no entry
// leg with any of `features`, or empty when every candidate is contaminated
// for this feature set.
[[nodiscard]] inline std::string cross_read_axis(std::span<const FeatureSpec *const> features,
                                                 std::span<const TargetSpec> candidates,
                                                 const TargetSpec &current,
                                                 std::ptrdiff_t feature_lag) {
  for (const TargetSpec &cand : candidates) {
    if (cand.name == current.name) {
      continue;
    }
    AuditConfig cfg;
    cfg.feature_lag = feature_lag;
    const AuditReport rep = audit(features, cand, cfg);
    if (rep.count(FindingKind::EntryMarkChannel) == 0 && !rep.has_fatal()) {
      return cand.name;
    }
  }
  return {};
}

// One line per finding, stable order, safe to write into a run artifact.
[[nodiscard]] inline std::vector<std::string> format_report(const AuditReport &report) {
  std::vector<std::string> lines;
  lines.reserve(report.findings.size());
  for (const Finding &f : report.findings) {
    lines.push_back(std::string(to_string(f.severity)) + " " + std::string(to_string(f.kind)) + " " +
                    f.subject + ": " + f.detail);
  }
  return lines;
}

} // namespace atx::vol::alpha
