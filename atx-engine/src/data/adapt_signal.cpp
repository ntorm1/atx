#include "atx/engine/data/adapt_signal.hpp"

#include <span>        // std::span
#include <string>      // std::string (expr_source, hash buffer)
#include <string_view> // std::string_view (kExternalTag)
#include <utility>     // std::move
#include <vector>      // std::vector

#include "atx/core/hash.hpp" // atx::core::hash_bytes (deterministic content hash)

#include "atx/engine/alpha/streams.hpp"   // alpha::extract_streams, AlphaStreams
#include "atx/engine/combine/metrics.hpp" // combine::compute_metrics, AlphaMetrics
#include "atx/engine/data/align.hpp"      // align_onto, AlignedView
#include "atx/engine/library/record.hpp"  // library::Provenance

namespace atx::engine::data {

namespace {

// book_size for compute_metrics: the weights extract_streams produces are already
// gross-normalized notional FRACTIONS (Σ|w| == gross_leverage), so the turnover
// divisor is 1.0 — the SAME convention the mined-alpha path uses (factory book_size
// default 1.0, the "extract_streams convention").
constexpr atx::f64 kBookSize = 1.0;

// The external-tag marker hashed into every external candidate's canon_hash. A mined
// genome's factory::canonical_hash hashes a DSL genome and NEVER this literal, so the
// external hash space is disjoint from the genome-hash space by construction.
constexpr std::string_view kExternalTag = "external";

// Build a SignalSet over the panel axis from the aligned signal columns. Each
// aligned column is date-major over the canonical (panel) axis already, so it maps
// 1:1 onto a SignalSet::Alpha. dates/instruments are the panel's (extract_streams
// requires the SignalSet shape to equal the panel shape).
[[nodiscard]] alpha::SignalSet signal_set_from(const alpha::Panel &panel, const AlignedView &av) {
  alpha::SignalSet ss;
  ss.dates = panel.dates();
  ss.instruments = panel.instruments();
  ss.alphas.reserve(av.columns.size());
  for (atx::usize c = 0; c < av.columns.size(); ++c) {
    alpha::SignalSet::Alpha a;
    a.name = av.columns[c];
    a.values = av.aligned_columns[c]; // copy the date-major column verbatim
    ss.alphas.push_back(std::move(a));
  }
  return ss;
}

// The externally-sourced expr_source tag for column `name` of dataset `source`:
//   "<external:<source>:<name>>"
[[nodiscard]] std::string external_expr_source(const std::string &source, const std::string &name) {
  std::string s;
  s.reserve(source.size() + name.size() + 12U);
  s.append("<external:");
  s.append(source);
  s.push_back(':');
  s.append(name);
  s.push_back('>');
  return s;
}

// Deterministic content hash over (external-tag, dataset source, column name). No
// RNG / clock input. Distinct columns / sources -> distinct hashes; the same inputs
// always hash identically. The literal "external" tag keeps this disjoint from the
// mined genome-hash space (which never hashes that tag).
//
// SAFETY (cross-run dedup-key stability): this canon_hash is persisted as the
// library DedupIndex key, but atx::core::hash_bytes is wyhash with a COMPILE-TIME
// secret — its header documents it is NOT stable across process restarts /
// platforms / library upgrades (only WITHIN one process). So cross-run dedup of
// external signals relies on the pinned wyhash secret staying fixed; if the
// ankerl/wyhash pin changes, every persisted external canon_hash shifts and the
// dedup DB must be regenerated (an existing external entry would no longer dedup
// against a re-ingested identical signal). NOTE: the MINED path's persisted key
// (factory::canonical_hash) deliberately uses a cross-run-STABLE FNV-1a fold for
// exactly this reason — but that fold is private to factory/canonical.cpp and
// operates on an Ast, not raw bytes, so there is no reusable cross-process-stable
// byte-hash to match here. If one is exposed in core, switch to it and drop this
// caveat. Within-run determinism (what every S6.5 test asserts) holds regardless.
[[nodiscard]] atx::u64 external_canon_hash(const std::string &source, const std::string &name) {
  std::string buf;
  buf.reserve(kExternalTag.size() + source.size() + name.size() + 2U);
  buf.append(kExternalTag);
  buf.push_back('\0'); // unambiguous field separator (no source/name concat collision)
  buf.append(source);
  buf.push_back('\0');
  buf.append(name);
  // SAFETY: buf.data() points at buf.size() live bytes for the duration of the call.
  return atx::core::hash_bytes(buf.data(), buf.size());
}

// Build candidate `k`'s pnl / pos_flat spans into the OWNED `streams` and fill its
// metrics + provenance + canon_hash. Split out so signal_to_candidates stays short.
[[nodiscard]] library::AlphaCandidate make_candidate(const alpha::AlphaStreams &streams,
                                                     atx::usize k, const std::string &source,
                                                     const std::string &column, atx::usize as_of) {
  const atx::usize np = streams.n_periods();
  const atx::usize ni = streams.n_instruments();

  library::AlphaCandidate c{};
  c.pnl = streams.pnl(k);
  // The alpha-k position block is [k*np*ni, (k+1)*np*ni) in the flat owner buffer.
  c.pos_flat = std::span<const atx::f64>{streams.pos_flat.data() + k * np * ni, np * ni};
  c.metrics = combine::compute_metrics(c.pnl, c.pos_flat, ni, kBookSize);

  library::Provenance prov;
  prov.expr_source = external_expr_source(source, column);
  prov.parent_hashes = {};
  prov.mutation_op = 0U;
  prov.seed = 0U;
  c.prov = std::move(prov);

  c.canon_hash = external_canon_hash(source, column);
  c.as_of = as_of;
  c.source = nullptr; // no live re-eval handle (external signal)
  return c;
}

} // namespace

atx::core::Result<SignalAdmission> signal_to_candidates(const Dataset &signal, const Dataset &price,
                                                        const alpha::Panel &panel,
                                                        const exec::ExecutionSimulator &sim,
                                                        const WeightPolicy &policy,
                                                        atx::usize as_of) {
  // 1. Align the signal columns onto the canonical (panel) price axis.
  ATX_TRY(const AlignedView av, align_onto(price, signal));

  // 2. Build a SignalSet over the panel axis (one alpha per aligned column).
  const alpha::SignalSet ss = signal_set_from(panel, av);

  // 3. Realize per-column pnl + position streams (REUSES extract_streams; no new
  //    portfolio / P&L logic). Err on a panel/SignalSet shape mismatch or missing
  //    "close" field — propagated.
  ATX_TRY(alpha::AlphaStreams streams, extract_streams(ss, policy, panel, sim));

  // 4. Move the owner into the result FIRST, then build candidate spans into the
  //    final-resting `result.streams` (so the spans are unambiguously valid across
  //    the by-value return — vector move preserves the buffer pointers).
  SignalAdmission result;
  result.streams = std::move(streams);
  const std::string &source = signal.provenance().source;

  result.candidates.reserve(av.columns.size());
  for (atx::usize k = 0; k < av.columns.size(); ++k) {
    result.candidates.push_back(make_candidate(result.streams, k, source, av.columns[k], as_of));
  }
  return atx::core::Ok(std::move(result));
}

} // namespace atx::engine::data
