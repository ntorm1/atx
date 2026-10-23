#pragma once

// ── AlSigmaQFamily::State — the (σ, q) boundary family's prepared state ──────────
//
// L2h-b sigma-q family (2026-10-21). Kept in its own internal header (as L3g moved
// AlSigmaFamily::State into american_sigma_family.hpp) so a later analytics TU can read
// the prepared tensor without touching american_sigma_q_family.cpp. The σ axis reuses
// sigma_family_detail's cheb_node / axis_of verbatim (the P2.5 twin note lives there);
// the q axis is the same Chebyshev-Lobatto construction, linear in q.
// L2h-c (2026-10-21): State is private to the class; QFamilyAccess (the class's friend,
// below) is the one door an internal TU reads it through (review-L2hb m4).
//
// NOT a public API. Not installed. No stability guarantees.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_boundary.hpp"     // amer::AlBoundary / AlWorkspace / Impl
#include "pricing/american_sigma_family.hpp" // sigma_family_detail::kMaxSigmaNodes

namespace atx::vol {

namespace sigma_q_family_detail {

inline constexpr std::uint16_t kMaxQNodes = kAlSigmaQFamilyMaxQNodes;

// ── K1 (r5, 2026-10-22): fused price + analytic vega — the plain-array stages ─────────
//
// THE MAP the fused path differentiates is price_on's: P = clamp(E + Π), Π the premium
// quadrature over the boundary B = xmax · exp(−√max(y, 0)), y interpolated tensor-
// barycentric in (σ, q) at the boundary's τ nodes and barycentric in τ at each premium
// node. vega = dP/dσ at fixed (S, K, q; r, T of the geometry) INCLUDING the σ motion of
// the interpolated transformed boundary: dy_k/dσ through the nodal differentiation
// matrix (sigma_family_detail::DiffTables — exact for the interpolating polynomial,
// stable through and at the nodes: no 0/0), the τ interpolation (linear in y, so it
// commutes with d/dσ), the transform s = √y (s' = y'/(2s)) and the integrand's
// d± = (ln(S/xmax) + s + (r − q) t)/v ± v/2. One premium pass serves both.
//
// OPERATION ORDER. The fused price is price_on's MAP, not its bits: the (σ, q)
// contraction is normalised before the τ interpolation, the premium reads the boundary
// in log form (ln(S e^{−qt} / (B e^{−rt})) = ln(S/xmax) + √y + (r − q) t: no exp / log of
// B per node), the integrand folds 2 z w into one weight, and the precontracted path
// folds the τ weights into the tensor. Every difference is rounding (pinned:
// AlSigmaQFamilyFused.FusedPriceIsPriceOnToRoundingOnTheSpyProbeGrid); price() stays
// price_on, the default evaluation.
//
// LAYERS (no hidden state; every stage is a function of plain arrays, so a batch caller
// can lane it with per-lane σ / q): PremiumNodes (σ-, q-, strike-free; per family),
// QCarry (per q), NodeBoundary (per (σ, q), strike-free), then premium_pass per strike.

// The premium quadrature's order cap (american.cpp's kAlMaxQuad = detail::kMaxQuadNodes).
inline constexpr unsigned kMaxPremiumNodes = 64;
// NearNodeDegenerate threshold: |y_i| <= kFusedKinkRel · max |y| (rounding of the
// interpolant) puts y at the transform's zero, where s = √max(y, 0) has no derivative.
inline constexpr double kFusedKinkRel = 64.0 * std::numeric_limits<double>::epsilon();

enum class FusedStatus : std::uint8_t {
  Ok = 0,                 // price and vega served
  OutsideBox = 1,         // σ below kAlSigmaFamilyMinSigma or outside the σ box, or q outside
                          // the q box
  NearNodeDegenerate = 2, // price served, vega NaN: a premium node's interpolated y sits at the
                          // transform's zero (kFusedKinkRel) with dy/dσ != 0 — s = √y has no
                          // derivative there (the τ = 0 node is y = 0 by construction)
  Unsupported = 3,        // not prepared / moved-from / geometry re-prepared / an input not finite
                          // (or S, K not > 0) / premium order > kMaxPremiumNodes / a stale tile
};

struct FusedPriceVega {
  double price = std::numeric_limits<double>::quiet_NaN(); // NaN unless Ok / NearNodeDegenerate
  double vega = std::numeric_limits<double>::quiet_NaN();  // dP/dσ; NaN unless Ok
  FusedStatus status = FusedStatus::Unsupported;
  AlSigmaQFamilyStatus reason = AlSigmaQFamilyStatus::NotPrepared; // the admit tree's typed status
  // The σ-family exercise rule (american_family_greeks.cpp): the internal spot is inside
  // the current edge, or the served price is clamped to / within 1e-9 K of intrinsic.
  // Informational: vega is the served map's dP/dσ — 0 only where the clamp serves
  // intrinsic — so a caller inverting near the edge sees the true (small) slope.
  bool exercised = false;
};

enum class PremiumNodeKind : std::uint8_t {
  Dead = 0,     // t <= 1e-14: the integrand is 0 (premium_integrand_put's guard)
  Boundary = 1, // rem = T − t > 0: the boundary is the interpolated B(rem)
  Strike = 2,   // rem <= 0: B = K (unreachable for interior Gauss-Legendre nodes; mirrored)
};

// The premium quadrature's σ-, q- and strike-free node data (a function of the
// geometry's T, the τ grid and the Gauss-Legendre premium table only).
struct PremiumNodes {
  unsigned n = 0; // the premium order (the workspace's n_quad_price)
  double T = 0.0;
  double sqrtT = 0.0;
  double half_sqrtT = 0.0;
  double t[kMaxPremiumNodes] = {};      // t_i = z_i², z_i = ½√T (1 + x_i)
  double sqrt_t[kMaxPremiumNodes] = {}; // √t_i
  double wz2[kMaxPremiumNodes] = {};    // 2 z_i w_i (the integrand's 2z times the GL weight)
  double zc[kMaxPremiumNodes] = {};     // al_boundary_at's τ coordinate at rem_i (Boundary nodes)
  PremiumNodeKind kind[kMaxPremiumNodes] = {};
};

// The internal put's carry at one contract q, and its per-node discounts.
struct QCarry {
  double q = 0.0;  // the contract's effective yield
  double rp = 0.0; // the internal put's rate / yield (puts (r, q); calls (q, r))
  double qp = 0.0;
  double dq[kMaxPremiumNodes] = {}; // e^{−qp t_i}
  double dr[kMaxPremiumNodes] = {}; // e^{−rp t_i}
};

// The interpolated boundary at the premium nodes for one (σ, q) — strike-free, so one
// serves every strike at that (σ, q).
struct NodeBoundary {
  double sigma = 0.0;
  double s[kMaxPremiumNodes] = {};  // √max(y_i, 0) (Boundary nodes)
  double ds[kMaxPremiumNodes] = {}; // ds_i/dσ = y'_i / (2 s_i); 0 where y_i <= 0
  double v[kMaxPremiumNodes] = {};  // σ √t_i
  bool skip[kMaxPremiumNodes] = {}; // Dead, or a boundary that underflows (s >= 745)
  double now_factor = 1.0;          // exp(−√max(y(T), 0)): the current edge is xmax · this
  bool degenerate = false;          // a node at the transform's zero (NearNodeDegenerate)
};

} // namespace sigma_q_family_detail

// Immutable after a successful prepare(); read-only in price(). No mutable scratch: the
// interpolated boundary and the barycentric weights live on the caller's stack, so N
// threads pricing through one const family touch only const memory (no write after
// prepare(); prepare() and the geometry's prepare() are documented as exclusive).
struct AlSigmaQFamily::State {
  const AlExpiryGeometry::Impl *g = nullptr;
  Side side = Side::Call;
  double anchor = 0.0;
  // ── set by prepare() ──
  bool ok = false;
  std::uint64_t generation = 0; // the geometry's generation at prepare()
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double u_lo = 0.0; // the σ box on the axis coordinate
  double u_hi = 0.0;
  double q_lo = 0.0; // the q box as given
  double q_hi = 0.0;
  double q_scale = 0.0; // 2 / (q_hi - q_lo) on a non-degenerate box, else 0
  double rate = 0.0;    // the expiry's r (the geometry's)
  std::uint16_t n_sigma = 0;
  std::uint16_t n_q = 0; // 1 on a degenerate box
  std::uint16_t n_boundary = 0;
  // The premium quadrature's workspace header (qx/qw_price, n_quad_price, fast,
  // specialize): a geometry orientation's — `side`'s, or the other side's when `side` is
  // European at the geometry's q (review-L2hb m1). It is q- and side-free (the
  // quadrature takes rate / yield as arguments; both orientations share the geometry's
  // scheme and the process-wide tables), owned by the geometry and immutable until its
  // next prepare(), which `generation` detects.
  const amer::AlWorkspace *ws = nullptr;
  amer::AlBoundary grid{}; // node structure (n, T, z, wbary, x, tau); y/K/xmax per price
  double sz[sigma_family_detail::kMaxSigmaNodes] = {}; // σ Chebyshev-Lobatto nodes in [-1, 1]
  double sw[sigma_family_detail::kMaxSigmaNodes] = {}; // 2nd-kind weights (endpoints halved)
  double qz[sigma_q_family_detail::kMaxQNodes] = {};   // q Chebyshev-Lobatto nodes in [-1, 1]
  double qw[sigma_q_family_detail::kMaxQNodes] = {};
  AlSigmaQFamilySelfCheck check{}; // prepare(…, OffNode)'s measurement (else NotPrepared)
  // series[(k * n_q + j) * n_sigma + s] = dimensionless y[k] at σ-node s, q-node j
  // (packed on the prepared counts so one boundary node's tensor is contiguous). L2h-c:
  // EXACTLY n_boundary · n_q · n_sigma doubles (was a fixed 32 x 8 x 16 capacity, 32 KB),
  // allocated by prepare(), kept across a re-prepare of the same size, released by a
  // refusal; the price path only reads it.
  std::unique_ptr<double[]> series{};
  std::size_t series_size = 0;

  // The refusal tree shared by every price entry (one sigma at a time).
  [[nodiscard]] AlSigmaQFamilyStatus admit(double S, double K, double sigma,
                                           double q) const noexcept;
  // admit()'s (S, sigma, q) part — every check but the strike's.
  [[nodiscard]] AlSigmaQFamilyStatus admit_point(double S, double sigma, double q) const noexcept;
  // The interpolated dimensionless boundary at (sigma, q) into bnd.y (bnd = a copy of
  // grid). Precondition: admit_point() == Ok.
  void interpolate(double sigma, double q, amer::AlBoundary &bnd) const noexcept;
  // Rescale an interpolated boundary to the contract's internal strike (T8 homogeneity;
  // xmax = K min(1, rate/yield) at the STRIKE's own carry) and run the European leg +
  // premium quadrature at that carry. Writes bnd.K / bnd.xmax only.
  [[nodiscard]] double price_on(amer::AlBoundary &bnd, double S, double K, double sigma,
                                double q) const noexcept;
  // interpolate + price_on. Precondition: admit() == Ok.
  [[nodiscard]] double price_at(double S, double K, double sigma, double q) const noexcept;
  // K1 (r5): the interpolated boundary at (sigma, q) and its partials at every τ node k:
  // y[k], dy_dsigma[k] and — when dy_dq is non-null and the q axis is not collapsed —
  // dy_dq[k]; returns whether dy_dq was written (a collapsed box has no q coordinate).
  // Each partial is the interpolating polynomial's own (DiffTables on the axis times the
  // axis chain dz/dσ = 2/(u_hi − u_lo) · du/dσ, dz/dq = q_scale), exact at a node hit.
  // y equals interpolate()'s to rounding (normalised weights, q contracted first).
  // Precondition: admit_point() == Ok. Each array holds n_boundary entries.
  [[nodiscard]] bool interpolate_derivs(double sigma, double q, double *y, double *dy_dsigma,
                                        double *dy_dq) const noexcept;
  // K1 (r5): this family's premium node data (the geometry's T, the τ grid, the
  // quadrature header ws). False (pn.n = 0) when the premium order exceeds
  // kMaxPremiumNodes. Precondition: ok.
  [[nodiscard]] bool lay_premium_nodes(sigma_q_family_detail::PremiumNodes &pn) const noexcept;
  // K1 (r5): the boundary at the premium nodes for (sigma, q) from the node tensor
  // (interpolate_derivs, then the τ barycentric per node as al_boundary_at evaluates it).
  // Precondition: admit_point() == Ok and pn from lay_premium_nodes.
  void boundary_at_nodes(const sigma_q_family_detail::PremiumNodes &pn, double sigma, double q,
                         sigma_q_family_detail::NodeBoundary &out) const noexcept;
  // The OffNode self-check on a fully published state (AlSigmaQFamilySelfCheck); `geo` /
  // `sc` are prepare()'s scratch, reused for the check points' cold solves.
  [[nodiscard]] AlSigmaQFamilySelfCheck
  measure_self_check(amer::AlOrientedGeometry &geo, amer::AlPreparedScratch &sc) const noexcept;
};

namespace sigma_q_family_detail {

// The class's friend: hands an internal TU the prepared state (const; null when the
// family is moved-from). L2h-c (review-L2hb m4).
struct QFamilyAccess {
  [[nodiscard]] static const AlSigmaQFamily::State *state(const AlSigmaQFamily &f) noexcept {
    return f.st_.get();
  }
  // A0 (r6): also AlSigmaQFamilyRateBumps' friend. The set's dY/dr node tensor (the
  // family's [(k · n_q + j) · n_sigma + s] layout) when `b` is non-null, prepared, and
  // was prepared from `f` (identity checked by value: side, anchor, (T, r), boxes, node
  // counts, axis, quadrature and two sentinel rows of the node tensor); otherwise null
  // and `why` = NoCarryBumps (b null) or CarryBumpRefused. Defined in
  // american_sigma_q_family_greeks.cpp. Precondition: state(f) is prepared.
  [[nodiscard]] static const double *rate_tensor(const AlSigmaQFamily &f,
                                                 const AlSigmaQFamilyRateBumps *b,
                                                 AlFamilyGreekReason &why) noexcept;
  // A0 (r6): AlSigmaQFamilyRateBumps::prepare's body (the two bumped builds) into `out`'s
  // state; `out` is left unprepared on any refusal. `h` is the bump: prepare() passes
  // kAlSigmaFamilyCarryBump; a measurement (the O(h²) truncation discriminator) may pass
  // another (> 0, finite; otherwise InvalidArgument).
  [[nodiscard]] static Status prepare_rate_bumps(const AlSigmaQFamily &base,
                                                 AlSigmaQFamilyRateBumps &out,
                                                 double h = kAlSigmaFamilyCarryBump);
};

// ── K1 (r5, 2026-10-22): fused price + vega entries (contract: the block at the top) ──
//
// Every entry below is const, allocation-free and thread-safe on a const family / table
// (stack scratch only) except build_premium_table, which allocates the table's tensor.

// The admit tree's status as a fused status (Ok -> Ok; σ guard / σ box / q box ->
// OutsideBox; every other refusal -> Unsupported).
[[nodiscard]] FusedStatus fused_status_of(AlSigmaQFamilyStatus a) noexcept;

// ── The σ / q basis helpers every evaluation route shares (pure functions, no state) ───
//
// Exposed so a lane kernel (K2) evaluates the same chain instead of re-deriving it.

// The normalised barycentric basis b_s(zq) on nodes z[0..n) / weights w[0..n) (n <=
// amer::kAlMaxNodes): b_s = (w_s / (zq − z_s)) / Σ_t (w_t / (zq − z_t)); an exact node
// hit is the unit vector e_hit.
void bary_basis(const double *z, const double *w, unsigned n, double zq, double *b) noexcept;

// c_t = Σ_s b_s D1[s][t] — the basis's z-derivative through the nodal differentiation
// matrix (the L3g form: no quotient rule, so no near-node cancellation and no 0/0 at a
// hit). Precondition: 2 <= n <= sigma_family_detail::kMaxSigmaNodes.
void bary_basis_derivative(const double *b, unsigned n, double *c) noexcept;

// σ -> the family z coordinate (clamped to [−1, 1] for rounding only, as interpolate()
// clamps) and dz/dσ (the unclamped chain: 2 / (u_hi − u_lo), times 1/σ on the log axis).
struct SigmaZ {
  double z = 0.0;
  double dz = 0.0;
};
[[nodiscard]] SigmaZ sigma_z(AlSigmaFamilyAxis axis, double u_lo, double u_hi,
                             double sigma) noexcept;

// The carry at the contract's q for `side` on the expiry's rate r: (rp, qp) and the
// per-node discounts of pn's live nodes.
void carry_at(const PremiumNodes &pn, Side side, double r, double q, QCarry &out) noexcept;

// The shared tail of every boundary-at-nodes route: from y_i, dy_i/dσ at the premium
// nodes (n_boundary-free) and y at the current edge to the strike-free NodeBoundary the
// premium pass reads — s = √max(y, 0), ds = dy/(2s), v = σ√t, the underflow skip, the
// edge factor and the NearNodeDegenerate decision (|y_i| <= kFusedKinkRel · max |y| with
// dy_i != 0 at a Boundary node). y / dy hold pn.n entries (non-Boundary entries ignored).
void finish_node_boundary(const PremiumNodes &pn, const double *y, const double *dy, double y_now,
                          double sigma, NodeBoundary &out) noexcept;

// The premium + European pass for the internal put (Sp, Kp) at nb.sigma: price and, when
// want_vega, vega = dP/dσ with the boundary motion (nb.ds). `fast` selects the family's
// AlProfile::Fast arithmetic (Cody Φ / φ, the fast European leg) — the workspace flag.
// Never refuses (the caller admitted the inputs); NearNodeDegenerate when nb.degenerate
// and the price is not clamped to intrinsic.
[[nodiscard]] FusedPriceVega premium_pass(const PremiumNodes &pn, const QCarry &c,
                                          const NodeBoundary &nb, double Sp, double Kp, bool fast,
                                          bool want_vega) noexcept;

// price(S, K, sigma, q)'s map with its analytic vega, from ONE premium pass: refuses as
// price() does (status / reason), else Ok (or NearNodeDegenerate: price, vega NaN).
[[nodiscard]] FusedPriceVega price_and_vega(const AlSigmaQFamily &f, double S, double K,
                                            double sigma, double q) noexcept;

// The strikes K[0..n) at ONE (sigma, q): the boundary at the premium nodes, the carry and
// the node data are computed once and every strike runs only its premium + European pass;
// out_price[i] / out_vega[i] are price_and_vega(f, S, K[i], sigma, q)'s to the bit.
// Unequal span lengths -> Unsupported, nothing written. An (S, sigma, q) refusal writes
// NaN everywhere and returns it; a strike not finite and > 0 gets NaN while the rest are
// priced, and the call returns Unsupported (reason InvalidArgument); NearNodeDegenerate
// (strike-free) serves prices with NaN vegas. Otherwise Ok.
[[nodiscard]] FusedStatus price_vega_strikes(const AlSigmaQFamily &f, double S,
                                             std::span<const double> K, double sigma, double q,
                                             std::span<double> out_price,
                                             std::span<double> out_vega) noexcept;

// ── Time-grid precontraction (deep review §5.3): the batch caller's tables ─────────────
//
// PremiumTable: a SNAPSHOT of one prepared family (it never reads the family again, so a
// later re-prepare of the family or its geometry cannot make it stale) with the τ
// interpolation folded into the node tensor once:
//   h[(i · n_q + j) · n_sigma + s] = Σ_k ℓ_k(zc_i) · Y[k, j, s],  i < nodes.n,
// ℓ_k the τ-barycentric basis at premium node i (an exact τ-node hit is e_hit), plus row
// i = nodes.n = the current edge (τ = T, ℓ = e_last). Rows of non-Boundary nodes are 0.
// QTile: the table contracted at ONE contract q (the q a strike keeps through an IV
// inversion) — g[i · n_sigma + s] = Σ_j b_j(q) h[(i · n_q + j) · n_sigma + s] and the
// carry — so a σ evaluation is two n_sigma-dots per premium node (y_i and dy_i/dσ), no
// τ interpolation and no q work. (A q-contracted τ-series tile that interpolates in τ per
// call measured 12-18% slower and was deleted; see docs/sigma-q-fused-vega.md.)
//
// Everything a PremiumTable holds but its heap tensor: trivially copyable, so the
// table's move operations copy it whole and then reset the source.
struct PremiumTableHeader {
  bool ok = false;
  std::uint64_t stamp = 0; // process-unique per successful build (a tile records it)
  Side side = Side::Put;
  AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  bool fast = false; // the family's quadrature arithmetic (AlWorkspace::fast)
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double u_lo = 0.0;
  double u_hi = 0.0;
  double q_lo = 0.0;
  double q_hi = 0.0;
  double q_scale = 0.0;
  double rate = 0.0;
  std::uint16_t n_sigma = 0;
  std::uint16_t n_q = 0;
  std::uint16_t n_boundary = 0;
  double sz[sigma_family_detail::kMaxSigmaNodes] = {};
  double sw[sigma_family_detail::kMaxSigmaNodes] = {};
  double qz[kMaxQNodes] = {};
  double qw[kMaxQNodes] = {};
  PremiumNodes nodes{};
};
static_assert(std::is_trivially_copyable_v<PremiumTableHeader>);

struct PremiumTable : PremiumTableHeader {
  std::unique_ptr<double[]> h{}; // (nodes.n + 1) · n_q · n_sigma doubles
  std::size_t h_size = 0;

  PremiumTable() = default;
  ~PremiumTable() = default;
  PremiumTable(const PremiumTable &) = delete;
  PremiumTable &operator=(const PremiumTable &) = delete;
  // A move hands the snapshot over — stamp included, so a tile cut from the source serves
  // on the target — and leaves the source exactly as a default-constructed table (ok
  // false, stamp 0, no tensor, h_size 0): every entry refuses it (Unsupported) and
  // build_premium_table into it allocates afresh.
  PremiumTable(PremiumTable &&other) noexcept;
  PremiumTable &operator=(PremiumTable &&other) noexcept;
  // Back to the default-constructed state (frees the tensor).
  void reset() noexcept;
  // Heap + inline bytes this table owns.
  [[nodiscard]] std::size_t bytes() const noexcept;
};

struct QTile {
  bool ok = false;
  std::uint64_t stamp = 0; // the table's stamp at contraction
  QCarry carry{};
  // g[i · n_sigma + s], i in [0, nodes.n] (row nodes.n = the current edge).
  double g[(kMaxPremiumNodes + 1) * sigma_family_detail::kMaxSigmaNodes] = {};
};

// Snapshot + precontract a prepared family (allocates h). Unsupported (table unprepared)
// when the family is not prepared, its geometry was re-prepared, or the premium order
// exceeds kMaxPremiumNodes.
[[nodiscard]] FusedStatus build_premium_table(const AlSigmaQFamily &f, PremiumTable &out);

// Contract a table at q. OutsideBox when q is outside the table's q box, Unsupported
// when the table is not ok (unbuilt, refused or moved-from) or q is not finite (tile.ok
// false either way).
[[nodiscard]] FusedStatus contract_q(const PremiumTable &t, double q, QTile &tile) noexcept;

// The strike-free NodeBoundary at sigma from a table and a tile of it: the stage
// price_vega_on runs before premium_pass (two n_sigma-dots per premium node, then
// finish_node_boundary). No checks — the caller has admitted what price_vega_on admits:
// t.ok, tile.ok, tile.stamp == t.stamp, sigma finite and inside [t.sigma_lo, t.sigma_hi].
void node_boundary_from_tile(const PremiumTable &t, const QTile &tile, double sigma,
                             NodeBoundary &out) noexcept;

// price_and_vega's map at (S, K, sigma, tile.q) from a table and a tile of it. Refuses
// Unsupported for a table / tile that is not ok or a tile of another build, an S / K
// not finite and > 0 or a sigma not finite; OutsideBox for the σ guard / box.
[[nodiscard]] FusedPriceVega price_vega_on(const PremiumTable &t, const QTile &tile, double S,
                                           double K, double sigma) noexcept;

// ── A0 (r6, 2026-10-23): the Greek bundle's stages (american_sigma_q_family_greeks.cpp) ──
//
// family_greeks on a (σ, q) family runs K1's stages (PremiumNodes, QCarry, NodeBoundary)
// plus the boundary's motion in the internal put's two carry parameters, then ONE
// premium pass that accumulates the price exactly as premium_pass does (the price, and
// the vega on a served row, are price_and_vega's bits) together with the S', σ and carry
// derivatives, and maps the internal put's bundle to the option's. The stages are
// exposed so a test can force an input the public entry cannot reach (a degenerate
// node); the contract of the result is the public one (api/pricing/american.hpp, A0).

// ds_i/dθ for θ the internal put's rate r' and yield q' at the premium nodes (0 off
// Boundary nodes and where y_i <= 0), and whether a Boundary node's y_i sits at the
// transform's zero (|y_i| <= kFusedKinkRel · max |y|) with dy_i/dθ != 0. `have_*` false:
// that motion is not available (no q axis / no rate-bump set) and its row is 0.
struct NodeCarryMotion {
  double ds_rate[kMaxPremiumNodes] = {};
  double ds_yield[kMaxPremiumNodes] = {};
  bool have_rate = false;
  bool have_yield = false;
  bool rate_degenerate = false;
  bool yield_degenerate = false;
};

// Which internal-put derivatives a pass accumulates (the price always).
struct GreeksNeed {
  bool s1 = false;    // ∂P/∂S'
  bool s2 = false;    // ∂²P/∂S'²
  bool sigma = false; // dP/dσ, boundary motion included
  bool rate = false;  // dP/dr': frozen terms + the xmax term + motion.ds_rate
  bool yield = false; // dP/dq': frozen terms + the xmax term + motion.ds_yield
};

// The internal put's bundle at (S', K', nb.sigma). On an exercised row every derivative
// is the exercise one (dS = −1, the rest 0).
struct GreeksInternal {
  double price = std::numeric_limits<double>::quiet_NaN();
  double dS = 0.0;
  double dSS = 0.0;
  double dsig = 0.0;
  double drate = 0.0;
  double dyield = 0.0;
  bool exercised = false;        // FusedPriceVega::exercised's rule (K1's, read as is)
  bool inside = false;           // S' <= the current edge xmax · nb.now_factor
  bool sigma_degenerate = false; // nb.degenerate (the σ motion has no derivative)
  // ln(S' / (xmax · nb.now_factor)) / (σ√T): the distance to the current edge in σ√T —
  // the near-edge band's coordinate (A0 fix round 1, review-A0 MJ1).
  double edge_sd = std::numeric_limits<double>::quiet_NaN();
};

// The pass. `fast` = the family's AlProfile::Fast arithmetic (the workspace flag).
void greeks_pass(const PremiumNodes &pn, const QCarry &c, const NodeBoundary &nb,
                 const NodeCarryMotion &m, double Sp, double Kp, bool fast,
                 const GreeksNeed &need, GreeksInternal &out) noexcept;

// What the option mapping needs beyond the internal bundle.
struct GreeksContext {
  Side side = Side::Put;
  double S = 0.0;     // the option's spot
  double r = 0.0;     // the option's rate (the geometry's)
  double q = 0.0;     // the option's yield (the contract's)
  double sigma = 0.0;
  double Sp = 0.0;    // the internal put's spot / strike
  double Kp = 0.0;
  bool q_axis = false; // the family has a q coordinate (n_q > 1)
  // Served when the motion's rate-bump side was fed by a matching set; otherwise the
  // refusal a requested rho carries (NoCarryBumps / CarryBumpRefused).
  AlFamilyGreekReason rho_source = AlFamilyGreekReason::NoCarryBumps;
};

// The internal pass's needs for `req` on this side (θ needs Δ and Γ; rho / phi only
// where their motion is available).
[[nodiscard]] GreeksNeed greeks_need(const AlSigmaQGreeksRequest &req, const GreeksContext &ctx,
                                     bool convention_ok) noexcept;

// The option's bundle (status Ok) from the internal one: homogeneity for calls, the PDE
// identity for θ, a reason for every Greek, the convention echoed from `req`.
[[nodiscard]] AlSigmaQFamilyGreeks map_greeks(const GreeksInternal &in, const NodeCarryMotion &m,
                                              const GreeksContext &ctx,
                                              const AlSigmaQGreeksRequest &req) noexcept;

} // namespace sigma_q_family_detail

} // namespace atx::vol
