#!/usr/bin/env python3
"""TRAIN-only admission screens ``v3-admit-v1`` / ``v4-prior-v1`` / ``v4-prior-v2`` and weight fits ``mv-shrink-0.9-nonneg-v1`` / ``ew-theme-v1`` / ``ew-theme-aim-v1`` / ``ew-theme-v6``.

Writes into a new output directory (published atomically, never overwritten):
  composition_weights.json  ``atx.dsl-composition-weights/v1`` (``/v2`` for ``ew-theme-v6``, the only
                            composition with a ``theme_redistribution`` block), read by ``atx-equity-strategy-ic
                            --composition-weights PATH --composition-weights-sha256 SHA``. It holds a
                            weight >= 0 for every library id, the top-level ``train_manifest_sha256``,
                            and ``signs`` (id -> 1 | -1 for every id whose applied sign is nonzero,
                            so for every id with a weight above 0).
  admission.json / .csv     ``atx.dsl-admission/v1`` decision table (only with ``--screen v3-admit-v1``).

Inputs are pinned by SHA-256: the library, the TRAIN role manifest, the runner's TRAIN
``orientations.json`` and ``summary.json`` of that same TRAIN-only IC run. The summary gives the
candidate cache layout: ``roles[train].candidate_cache.directory`` (ROOT/<train manifest sha>, base
candidates), ``.fields_directory`` (ROOT/<fields manifest sha>, candidates reading extra research
fields) and ``.vm_identity``. Sidecars are checked as the runner's ``cached_payload_sha`` does.
A v2 runner's summary also lists ``.entries[]`` (id, layout v1|v2, sidecar, payload, SHAs): each
candidate is then read from its named entry, v2 ``<id>.<dsl16>.{f64,json}`` or v1 read in place.
Nothing after 2022-12-31 is read: the role must end by 2023-01-01T00:00Z, every cache sidecar must
name the ``train`` role, and the summary must describe a TRAIN-only run.

Per scored decision d in [score_begin, score_end - 2), candidate k:
  1. Exposures at d as ``strategy_price_exposures.cpp`` (price-risk-v1) defines them:
     beta over 252 intervals against the equal-weight market of valid guarded returns across ALL
     instruments (>= 126 pairs); vol, the sample SD over 63 intervals (>= 32); log of the mean raw
     dollar volume over 63 sessions (unusable days add 0). Used rows are member && present && all
     three exposures finite. Exposures are z-scored over the used rows (sample SD) and clipped to +-5.
  2. q_k(d) is the centered tied rank of signal_k[d] over used rows with a finite signal, and 0 on
     other used rows. It is residualized by OLS on [1, z_beta, z_vol, z_ladv] over the used rows,
     then scaled to sum|q| = 1. This book is UNSIGNED; a sign s only negates it (exactly).
  3. f_k(d) = sum_i q_i r_i(d+2), where r(d+2) = close[d+2]/close[d+1] - 1 is a valid guarded return
     (else it contributes 0). A decision with no live book (flat) is NaN in the stored series.
  4. tau_k = mean over consecutive scored decisions of sum_i |q_k(d)_i - q_k(d-1)_i|. Deployment
     is excluded and there is no drift.

Screen v3-admit-v1 (declared by root before any v3 measurement). FIT is decision sessions in
[2020-01-01, 2022-01-01) and HOLD is [2022-01-01, 2023-01-01). Statistics use live (finite) days.
  orientation  s_k = sign(mean f_k over FIT). A disagreement with the runner's sign is reported.
  checks       insufficient (< 250 live FIT days), turnover (tau_k > 0.70), unstable
               (not (FIT Sharpe of s_k f_k > 0 and HOLD mean of s_k f_k > 0)). Every failed check is
               listed; the status is the first failure in that order.
  redundancy   survivors in descending FIT Sharpe order (ties: library order). k is admitted unless
               |corr(f_k, f_j)| > 0.70 over FIT days where both are live, for an already-admitted j.
               Then it is reject_redundant(j), naming the largest |rho|. A pair with < 250 common days
               (or undefined rho) counts as uncorrelated and is noted.
Weights: mu and S over ALL TRAIN decisions of s_k f_k with flat days as 0; Sh = 0.1 S + 0.9 diag(S),
w = solve(Sh, mu), w = max(w, 0), w /= sum(w). ``--composition mv-shrink-0.9-nonneg-netcost-v1`` uses
mu_k - 0.0018 * tau_k instead (net of 18 bps per unit turnover); nothing else changes. Only admitted candidates (screen mode), or every
runner-oriented candidate (``--screen none``, the T9 rule), take part. Everyone else gets weight 0.

v4 (``--orientation prior --screen v4-prior-v1 --composition ew-theme-v1``, the three go together;
declared in the v4 pre-registration R3/R4 before any v4 TRAIN read). Every candidate carries ``theme``,
``tier`` and ``prior_sign`` (library candidate keys and/or the pinned ``--recipe`` per-candidate list
``candidates``/``lineage``; both sources must agree). TRAIN is every decision session in
[2020-01-01, 2023-01-01); statistics use live (finite) days.
  orientation  s_k = prior_sign = +1 (the sign is embedded in the DSL: higher value = long); no sign is
               estimated or flipped; prior_sign 0 -> reject_no_prior (weight 0); -1 is refused.
  checks       insufficient (< 250 live TRAIN days), turnover (tau_k > 0.70), veto (Newey-West HAC t of
               the mean of f_k over live TRAIN days < -2.0; Bartlett kernel, lag 5, autocovariances
               divided by n, t = mean / sqrt(LRV / n); undefined t (constant, n < 2) -> no veto).
               First failure wins.
  redundancy   survivors in (tier, roster order) order: tier grades A+ < A < A- < B+ < B < B- < C+ < C <
               C- < D (or integer tiers ascending); never by a TRAIN statistic. k is admitted unless
               |corr(f_k, f_j)| > 0.90 over TRAIN days both live for an admitted j (< 250 common days
               or undefined rho: uncorrelated, noted).
  ew-theme-v1  w_k = 1 / (T * n_theme(k)) for admitted k, T = themes with >= 1 admitted member,
               n_theme = admitted members of that theme; no mean or covariance estimation (an admitted
               zero-variance series is degenerate: weight 0, not a member). The weights file pins
               signs (+1 for every prior-signed candidate) so the runner uses them.

v4.2 (``--screen v4-prior-v2``, same orientation, composition and inputs; v4.2 pre-registration R3',
declared after the v4 gate and before any v4.2 TRAIN read): v4-prior-v1 plus a structural cost-consistency
check, standalone TRAIN tau_k > 0.08 -> reject_turnover_cost. Check order: no_prior, insufficient,
turnover (0.70), turnover_cost (0.08), veto; first failure wins. A theme left without an admitted member
drops out of the 1/themes weights (as in ew-theme-v1). ``v4-prior-v1`` output is unchanged.

v5 (``--composition ew-theme-aim-v1`` with the same prior orientation/screens; v5 pre-registration R4', declared
before any v5 TRAIN read): ew-theme-v1 scaled by the Garleanu-Pedersen aim gain of each member, measured from
second moments of the TRAIN signal only (no means, no covariances, no returns).
  ranks        z_k(d) = per-decision centered tied ranks of signal_k over the context's used rows with a finite
               signal (``live``), standardized to mean 0 / unit population SD; NaN on decisions with < 50 live
               names or zero rank variance (all tied), and outside TRAIN.
  rho_k(j)     mean over TRAIN decisions d of c_j(d) = sum_i z_k(d)_i z_k(d-j)_i / n_both(d) over the names
               finite on both days (n_both >= 50), at exact lags 0..21 and 28, 35, ..., 126.
  g_k          theta * sum_{j=0..126} (1-theta)^j rho_k(j), theta = 0.05, rho linearly interpolated between exact
               lags, a NaN lag counts 0; clipped to [0.05, 1].
  weights      w_k = (g_k / (T * n_theme(k))) / sum_m (g_m / (T * n_theme(m))) over admitted non-degenerate
               members (normalised globally, not within a theme).
  report only  per-candidate rho and g, half-sample gains (first / second half of the TRAIN decisions) and the
               coverage-effective theme weight (mean over TRAIN decisions of sum_{k in theme} w_k c_k(d) /
               sum_k w_k c_k(d), c_k(d) = live names / used names) land in ``provenance.aim``; nothing in them
               feeds back into the weights. ``ew-theme-v1`` output bytes are unchanged (no aim keys).

v6 (``--composition ew-theme-v6`` with the same prior orientation/screens; v4-prereg.md "## v6 revision" item V6-W,
declared before any v6 TRAIN read; the rule text is binding): the admitted non-degenerate members of the screen, then
  (a) theme low_risk is dropped (its members take weight 0);
  (b) options_implied is merged into short_interest (one theme, members equal within it);
  (c) fast-sleeve shrink: a member whose standalone daily TRAIN tau_k >= 0.08 (the admission table's value,
      ``admission.json`` key ``candidates[].tau`` of the ``status: admitted`` row) keeps 1/3 of its within-theme weight
      1/n_theme; the freed mass is reallocated pro rata to the theme's other (slow) members. A theme whose members are
      all fast has no other member: the mass returns pro rata to the same members, i.e. their weights are unchanged;
  (d) w_k = within_k / T over the T resulting themes with >= 1 member (equal theme weights). Coverage redistribution
      (a member missing for a name on a day keeps its mass inside the theme, spread over the members present for that
      name and day) cannot be written as fixed per-candidate weights: the weights file carries a top-level
      ``theme_redistribution`` block {rule: within-theme-v1, composition: ew-theme-v6, themes: {id: theme}} that the IC
      runner (strategy_ic_composition.cpp) applies per name and day: blend_i = sum_theme W_theme * sum_{k present}
      w_k s_k r_k,i / sum_{k present} w_k, W_theme = sum of the theme's weights; a theme with no present member adds
      nothing. ``provenance.v6`` records the rule, threshold, shrunk / dropped / merged members and the input SHAs.
      The file's schema is ``atx.dsl-composition-weights/v2`` (the runner accepts v2 iff the block is present).
  Only ``ew-theme-v6`` writes these keys: ``ew-theme-v1`` and ``ew-theme-aim-v1`` bytes are unchanged (schema v1).

Incremental: with ``--work-dir`` the per-day price-risk context and each candidate's unsigned factor
record (f_k, tau_k, live counts) are persisted and SHA-verified on read. A mismatch means recompute.
Records are keyed by the work key (``atx.fit-work-key/v1``: semantics tag, TRAIN role manifest SHA, VM
identity, screen id, DSL SHA and the candidate's field payload SHAs, i.e. the summary entry's
``field_payload_sha256`` map; a field candidate of a v1 runner summary, which lists no map, falls back to
the fields manifest SHA), stored as ``<work>/<train-sha>/<semantics-tag>/factors/k-<key>.json``, so an
unchanged candidate is reused when other fields or other candidates change. A record is also bound to its
cache payload SHA, the VM identity, the context digest and this script's SHA-256, so any edit of this file
recomputes everything. stderr reports ``fit: computed K, reused M``. ``--max-seconds``
and ``--max-new-candidates`` stop cleanly between candidates with exit code 3 and publish nothing; a
rerun computes only what is missing. Outputs are byte-identical whichever path produced them.
``ew-theme-aim-v1`` adds a per-candidate aim record (rho at the exact lags, g, half-sample gains, per-decision
coverage) next to the factor record, bound the same way (script SHA-256, context digest, cache payload); the
other compositions never read or write it. The exit-3 JSON on stdout (``status: incomplete``) is the
partial-pass marker: rerun the same command to resume.
Exit codes: 0 complete; 1 refused (nothing published); 3 incomplete (rerun); 4 admission published,
no weights (nothing admitted or no positive weight). Numpy only, single-threaded BLAS.
"""
from __future__ import annotations

import os

# Deterministic reductions: pin BLAS/LAPACK to one thread before numpy loads.
for _var in ("OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "OMP_NUM_THREADS"):
    os.environ.setdefault(_var, "1")

import argparse  # noqa: E402
import hashlib  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
from pathlib import Path  # noqa: E402
import shutil  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402

import numpy as np  # noqa: E402

import composition_rules  # noqa: E402  (v8 R-1: composition ew-theme-std-v1, pure functions in this directory)

RULE_ID = "mv-shrink-0.9-nonneg-v1"
# Root preregistration (before any v3 measurement): the same fit with a net mean vector,
# mu_k = mean_TRAIN(s_k f_k) - c * tau_k, c = 18 bps per unit of one-way GMV turnover (S2 on TRAIN v1).
NETCOST_RULE_ID = "mv-shrink-0.9-nonneg-netcost-v1"
NETCOST_C = 0.0018
# v4 pre-registration R4: equal theme weights split equally over admitted members; nothing estimated.
EW_THEME_RULE_ID = "ew-theme-v1"
# v5 pre-registration R4': ew-theme weights scaled by the Garleanu-Pedersen aim gain from TRAIN rank autocorrelation.
AIM_RULE_ID = "ew-theme-aim-v1"
AIM_THETA = 0.05                                          # R4': fixed, equals the reference construction theta
AIM_LAGS = list(range(0, 22)) + list(range(28, 127, 7))  # exact lags; others linearly interpolated
AIM_GAIN_MIN, AIM_MAX_LAG = 0.05, 126
AIM_MIN_NAMES = 50                                        # live names per decision (ranks) and per lag pair
# v6 revision V6-W (declared before any v6 TRAIN read): ew-theme-v1 members, (a) drop low_risk, (b) merge
# options_implied into short_interest, (c) fast-sleeve shrink x 1/3 at standalone tau >= .08 (mass stays in the theme),
# (d) equal theme weights with within-theme coverage redistribution applied by the IC runner per name and day.
V6_RULE_ID = "ew-theme-v6"
V6_DROPPED_THEMES = ("low_risk",)
V6_MERGED_THEMES = {"options_implied": "short_interest"}
V6_FAST_TAU = 0.08          # standalone daily TRAIN tau_k >= this marks a fast sleeve (admission.json candidates[].tau)
V6_FAST_FACTOR = 1.0 / 3.0  # a fast member's within-theme weight multiplier
V6_REDISTRIBUTION = "within-theme-v1"
PRIOR_COMPOSITIONS = (EW_THEME_RULE_ID, AIM_RULE_ID, V6_RULE_ID)
COMPOSITIONS = (RULE_ID, NETCOST_RULE_ID, EW_THEME_RULE_ID, AIM_RULE_ID, V6_RULE_ID)
PRIOR_COMPOSITIONS, COMPOSITIONS = (PRIOR_COMPOSITIONS + (composition_rules.STD_RULE_ID,),  # v8 R-1 registration
                                    COMPOSITIONS + (composition_rules.STD_RULE_ID,))
SHRINK_LAMBDA = 0.9  # Sh = 0.1 * S + 0.9 * diag(S), written literally below
SCREEN_ID = "v3-admit-v1"
# v4 pre-registration R3: prior-signed admission, TRAIN only vetoes and measures.
PRIOR_SCREEN_ID = "v4-prior-v1"
# v4.2 pre-registration R3': v4-prior-v1 plus reject tau_k > 0.08 (cost consistency at $1bn; structural).
PRIOR_SCREEN_V2_ID = "v4-prior-v2"
PRIOR_SCREENS = (PRIOR_SCREEN_ID, PRIOR_SCREEN_V2_ID)
SCREENS = ("none", SCREEN_ID, PRIOR_SCREEN_ID, PRIOR_SCREEN_V2_ID)
ORIENTATIONS = ("train", "prior")
V4_TAU_LIMIT, V4_RHO_LIMIT, V4_MIN_TRAIN_DAYS, V4_VETO_T, NW_LAG = 0.70, 0.90, 250, -2.0, 5
V4_THEMES = ("value", "profitability_quality", "investment_issuance", "earnings_momentum", "price_momentum",
             "low_risk", "short_interest", "reversal_seasonality", "options_implied")
# Platform-v7 pre-registration (v7-prereg.md "Library v7.0"; library-v7-draft 2 / 3.5c): themes appended after the v4
# list. A prior-metadata theme may be any of PRIOR_THEMES; the weights provenance lists an appended theme under
# themes_preregistered only when some candidate declares it, so a library without one (v6.1, v7.0: ownership_flow is
# empty until wave 2) keeps its bytes. ew-theme-v1 counts only themes with an admitted member, so an empty theme never
# changes a weight.
V7_APPENDED_THEMES = ("ownership_flow",)
PRIOR_THEMES = V4_THEMES + V7_APPENDED_THEMES
TIER_GRADES = ("A+", "A", "A-", "B+", "B", "B-", "C+", "C", "C-", "D")  # strongest first
V4_STATUSES = ("admitted", "reject_no_prior", "reject_insufficient", "reject_turnover", "reject_veto",
               "reject_redundant")
V42_COST_TAU_LIMIT = 0.08
V42_STATUSES = ("admitted", "reject_no_prior", "reject_insufficient", "reject_turnover", "reject_turnover_cost",
                "reject_veto", "reject_redundant")
WEIGHTS_SCHEMA = "atx.dsl-composition-weights/v1"
# ew-theme-v6 only: v2 carries the theme_redistribution block; the IC runner accepts v2 iff the
# block is present and v1 iff absent, so a runner predating within-theme-v1 refuses v2 loudly.
WEIGHTS_SCHEMA_V2 = "atx.dsl-composition-weights/v2"
ADMISSION_SCHEMA = "atx.dsl-admission/v1"
LIBRARY_SCHEMA = "atx.dsl-ic-library/v1"
ORIENTATIONS_SCHEMA = "atx.dsl-ic-orientations/v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
CACHE_SCHEMA = "atx.dsl-candidate-signal/v1"
# strategy_ic_runner.cpp v2 content-keyed entries: ROOT/<role sha>/[fp_<fk16>/]<id>.<dsl16>.{f64,json},
# named per candidate by the summary's candidate_cache.entries[] (v1 entries read in place too).
CACHE_SCHEMA_V2 = "atx.dsl-candidate-signal/v2"
SIGNAL_KEY_SCHEMA = "atx.dsl-candidate-signal-key/v2"
CACHE_LAYOUT = "date-major-little-endian-f64;non-finite-stored-as-quiet-NaN"
VM_EVAL_MODE = "ResearchFast;full-historical-asof-member-mask"
FACTOR_SCHEMA = "atx.fit-candidate-factor/v1"
WORK_KEY_SCHEMA = "atx.fit-work-key/v1"
CONTEXT_SCHEMA = "atx.fit-price-risk-context/v1"
# Bump these when anything that changes the context or a factor record changes: old work is ignored.
CONTEXT_SEMANTICS = ("price-risk-v1;beta252-min126-all-instrument-market;vol63-min32;ladv63;"
                     "used=member&present&ok;z-sample-sd-clip5;min50;pivot1e-8;qr-basis;fwd=r[d+2]-valid-else-0;v1")
FACTOR_SEMANTICS = ("unsigned-centered-tied-rank;used-rows-finite-signal;ols-residual-2-pass;gross1;"
                    "residual>1e-9*entry;f=sum(q*fwd);flat=NaN;tau=mean-consecutive-no-drift;v1")
SEMANTICS_TAG = hashlib.sha256(f"{CONTEXT_SEMANTICS}|{FACTOR_SEMANTICS}".encode()).hexdigest()[:16]
AIM_SCHEMA = "atx.fit-candidate-aim/v1"
AIM_SEMANTICS = ("z=centered-tied-rank-over-used&finite-signal;standardized-mean0-population-sd;min50;all-tied->NaN;"
                 "TRAIN-decisions[2020-01-01,2023-01-01)-only;c_j(d)=sum(z_d*z_d-j)/n_both,n_both>=50;"
                 "rho_j=mean_d-finite(c_j);lags0-21,28-126/7;interp-linear;NaN->0;g=theta*sum_0..126(1-theta)^j*rho;"
                 "clip[0.05,1];halves=TRAIN-decision-split-floor(n/2):d<h|d-j>=h;coverage=live/used-rows;v1")
AIM_TAG = hashlib.sha256(AIM_SEMANTICS.encode()).hexdigest()[:16]
OUTPUT_WEIGHTS, OUTPUT_ADMISSION, OUTPUT_ADMISSION_CSV = (
    "composition_weights.json", "admission.json", "admission.csv")
FIT_BEGIN_NS = 1_577_836_800_000_000_000  # 2020-01-01T00:00Z
HOLD_BEGIN_NS = 1_640_995_200_000_000_000  # 2022-01-01T00:00Z
TRAIN_END_NS = 1_672_531_200_000_000_000  # 2023-01-01T00:00Z, exclusive: TRAIN is 2020-2022
DAY_NS = 86_400_000_000_000
METADATA_LIMIT = 1 << 20  # runner's metadata_text() bound; the weights file must fit too
SUMMARY_LIMIT = 16 << 20  # a runner summary.json grows with the library; bounded-runner bind cap
# strategy_ic_runner.cpp cache_root/legacy_entry: this identity keeps DIR/<sha>/ and accepts keyless
# sidecars recorded by exactly these engine builds; every other identity lives in DIR/<identity>/.
LEGACY_VM_IDENTITY = "dslvm1_clang18.1"
LEGACY_ENGINE_SHAS = ("429cbe43d275a49ad3cae89dfa8aa591846a2e4f", "6d85ac2a8b7aca6f28cea0e651cdcfc55d77aa29")
# Any edit to this file changes this SHA; cached contexts/records bound to another one are recomputed.
SCRIPT_SHA256 = hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()
TAU_LIMIT, RHO_LIMIT = 0.70, 0.70
MIN_FIT_DAYS, MIN_COMMON_DAYS = 250, 250
ANNUALIZATION = 252
EXIT_OK, EXIT_REFUSED, EXIT_INCOMPLETE, EXIT_NO_WEIGHTS = 0, 1, 3, 4
# price-risk-v1: PriceExposureConfig defaults and strategy_price_exposures.cpp constants.
BETA_WINDOW, VOL_WINDOW, ADV_WINDOW = 252, 63, 63
MIN_RETURN_PAIRS, MIN_NAMES, CLIP_Z = 126, 50, 5.0
VOL_MIN_COUNT = max(2, (VOL_WINDOW + 1) // 2)
GUARD_ABS_LOG, GUARD_EXCESS_LOG = 1.5, 0.10
MIN_PIVOT, RELATIVE_SD_FLOOR, MIN_RESIDUAL_FRACTION = 1e-8, 1e-12, 1e-9


class FitError(Exception):
    """A loud refusal: bad pin, contract violation or degenerate fit."""


def require(condition, message: str) -> None:
    if not condition:
        raise FitError(message)


def is_hash(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def pinned_bytes(path: Path, pin: str, what: str) -> bytes:
    require(is_hash(pin), f"{what}: external lowercase SHA-256 required")
    data = Path(path).read_bytes()
    require(0 < len(data) <= METADATA_LIMIT, f"{what}: missing or over 1 MiB")
    require(hashlib.sha256(data).hexdigest() == pin, f"{what}: SHA-256 pin differs")
    return data


def unique_json(data: bytes, what: str):
    def pairs(items):
        keys = [k for k, _ in items]
        require(len(keys) == len(set(keys)), f"{what}: duplicate JSON key")
        return dict(items)

    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FitError(f"{what}: JSON parse: {exc}") from exc


# ---------------------------------------------------------------- pinned inputs
def load_library(path: Path, pin: str) -> list[dict]:
    j = unique_json(pinned_bytes(path, pin, "library"), "library")
    require(j.get("schema") == LIBRARY_SCHEMA, "library: schema")
    rows = j.get("candidates")
    require(isinstance(rows, list) and 0 < len(rows) <= 256, "library: bounded candidate list")
    out, seen = [], set()
    for row in rows:
        cid, family, dsl = row.get("id"), row.get("family"), row.get("dsl")
        require(isinstance(cid, str) and isinstance(family, str) and isinstance(dsl, str) and dsl,
                "library: candidate id/family/dsl")
        require(cid not in seen, f"library: duplicate candidate {cid}")
        seen.add(cid)
        out.append({"id": cid, "family": family,
                    "dsl_sha256": hashlib.sha256(dsl.encode("utf-8")).hexdigest()})
    return out


def load_orientations(path: Path, pin: str, library: list[dict], library_sha: str,
                      train_sha: str) -> tuple[list[int], str]:
    j = unique_json(pinned_bytes(path, pin, "orientations"), "orientations")
    require(j.get("schema") == ORIENTATIONS_SCHEMA, "orientations: schema")
    require(j.get("library_sha256") == library_sha, "orientations: library_sha256 differs")
    require(j.get("train_manifest_sha256") == train_sha, "orientations: train_manifest_sha256 differs")
    require(is_hash(j.get("recipe_sha256")), "orientations: recipe_sha256")
    rows = j.get("candidates")
    require(isinstance(rows, list) and len(rows) == len(library), "orientations: candidate count")
    signs = []
    for row, c in zip(rows, library):
        require(row.get("id") == c["id"] and row.get("family") == c["family"] and
                row.get("dsl_sha256") == c["dsl_sha256"], f"orientations: candidate identity {c['id']}")
        sign = row.get("sign")
        require(type(sign) is int and sign in (-1, 0, 1), f"orientations: sign {c['id']}")
        signs.append(sign)
    return signs, j["recipe_sha256"]


PRIOR_KEYS = ("theme", "tier", "prior_sign")


def _tier_token(value):
    return value.strip().replace("\u2212", "-").replace("\u2013", "-") if isinstance(value, str) else value


def load_priors(library_path: Path, library_sha: str, recipe_path: Path | None, recipe_sha: str | None,
                library: list[dict]) -> dict:
    """theme / tier / prior_sign per candidate from the pinned library and/or the pinned recipe.

    A key may come from the library candidate object or the recipe's per-candidate list
    (``candidates`` and/or ``lineage``, matched by id); where both carry it they must agree.
    """
    lib_doc = unique_json(pinned_bytes(library_path, library_sha, "library"), "library")
    sources = [("library", {row.get("id"): row for row in lib_doc["candidates"]})]
    if recipe_path is not None:
        rec = unique_json(pinned_bytes(recipe_path, recipe_sha, "recipe"), "recipe")
        ref = rec.get("library")
        require(isinstance(ref, dict) and ref.get("sha256") == library_sha,
                "recipe: library.sha256 differs from --library-sha256")
        known = {c["id"] for c in library}
        lists = [rec[key] for key in ("candidates", "lineage") if isinstance(rec.get(key), list)]
        require(bool(lists), "recipe: no per-candidate list (candidates or lineage)")
        for rows in lists:
            by_id = {}
            for row in rows:
                require(isinstance(row, dict) and row.get("id") in known, "recipe: per-candidate row for an unknown id")
                require(row["id"] not in by_id, f"recipe: duplicate per-candidate row {row['id']}")
                by_id[row["id"]] = row
            sources.append(("recipe", by_id))
    themes, tiers, prior_signs, used = [], [], [], set()
    for c in library:
        vals = {}
        for name, by_id in sources:
            row = by_id.get(c["id"]) or {}
            for key in PRIOR_KEYS:
                if key in row:
                    v = _tier_token(row[key]) if key == "tier" else row[key]
                    require(key not in vals or (type(vals[key]) is type(v) and vals[key] == v),
                            f"prior metadata: {key} of {c['id']} disagrees between sources")
                    vals[key] = v
                    used.add(name)
        missing = [key for key in PRIOR_KEYS if key not in vals]
        require(not missing, f"prior metadata: {c['id']} lacks {missing}")
        require(isinstance(vals["theme"], str) and vals["theme"] in PRIOR_THEMES,
                f"prior metadata: theme of {c['id']} is not a pre-registered v4 theme {V4_THEMES} or appended "
                f"theme {V7_APPENDED_THEMES}")
        sign = vals["prior_sign"]
        require(type(sign) is int and sign in (1, 0, -1), f"prior metadata: prior_sign of {c['id']}")
        require(sign != -1, f"prior metadata: prior_sign -1 for {c['id']}; v4 embeds the prior sign in the DSL (+1)")
        tier = vals["tier"]
        require((isinstance(tier, str) and tier in TIER_GRADES) or (type(tier) is int and tier >= 0),
                f"prior metadata: tier of {c['id']} must be one of {TIER_GRADES} or an integer >= 0")
        themes.append(vals["theme"])
        tiers.append(tier)
        prior_signs.append(sign)
    require(len({type(t) for t in tiers}) == 1, "prior metadata: tiers mix grades and integers")
    rank = [TIER_GRADES.index(t) if isinstance(t, str) else t for t in tiers]
    return {"themes": themes, "tiers": tiers, "tier_rank": rank, "prior_signs": prior_signs,
            "source": "+".join(n for n in ("library", "recipe") if n in used), "recipe_sha256": recipe_sha
            if recipe_path is not None else None,
            "tier_order": list(TIER_GRADES) if isinstance(tiers[0], str) else "integer-ascending"}


class RoleManifest:
    """A pinned ``atx.recent-research-role/v1`` manifest restricted to TRAIN.

    Construction verifies the manifest pin, the small axes and the TRAIN seal. The price payload is
    read (and receipt-verified) only by ``payload()``, i.e. only when a context must be built.
    """

    def __init__(self, manifest: Path, pin: str):
        j = unique_json(pinned_bytes(manifest, pin, "role manifest"), "role manifest")
        require(j.get("schema") == ROLE_SCHEMA and j.get("status") == "complete", "role: schema/status")
        self.sha = pin
        self.dates, self.instruments = int(j["dates"]), int(j["instruments"])
        self.score_begin, self.score_end = int(j["score_begin"]), int(j["score_end"])
        start_ns, end_ns = int(j["score_start_ns"]), int(j["score_end_ns"])
        self.source_sha256 = j.get("source_sha256")
        d, n = self.dates, self.instruments
        require(0 < d <= 4096 and 0 < n <= 20000, "role: dimensions")
        require(0 <= self.score_begin < self.score_end == d, "role: score window")
        require(self.score_end - 2 - self.score_begin >= 2, "role: fewer than two scored decisions")
        # TRAIN only: nothing on or after 2023-01-01 may be scored or read.
        require(0 < start_ns < end_ns <= TRAIN_END_NS,
                "role: not TRAIN-only (score_end_ns after 2023-01-01T00:00Z)")
        self._files, self._base = j["files"], Path(manifest).parent
        self.sessions = self._read("sessions.i64", "<i8", d)
        self.ids = self._read("ids.u64", "<u8", n)
        s = self.sessions
        require(s[0] > 0 and bool(np.all(np.diff(s) > 0)) and int(s[-1]) < end_ns and
                bool(np.all(s % DAY_NS == 0)), "role: session axis")
        require(int(s[-1]) < TRAIN_END_NS, "role: session after 2022-12-31")
        require(int(np.searchsorted(s, start_ns, side="left")) == self.score_begin, "role: score boundary")
        require(self.ids[0] != 0 and bool(np.all(self.ids[1:] > self.ids[:-1])), "role: instrument axis")
        self.begin, self.end = self.score_begin, self.score_end - 2  # scored decisions with a d+2 label

    def _read(self, name: str, dtype: str, count: int) -> np.ndarray:
        receipt = self._files[name]
        size = count * np.dtype(dtype).itemsize
        require(int(receipt["bytes"]) == size and is_hash(receipt["sha256"]), f"role: receipt {name}")
        data = (self._base / name).read_bytes()
        require(len(data) == size, f"role: file extent {name}")
        require(hashlib.sha256(data).hexdigest() == receipt["sha256"], f"role: payload SHA {name}")
        return np.frombuffer(data, dtype=dtype)

    def payload(self) -> dict:
        d, n = self.dates, self.instruments
        out = {"present": self._read("present.u8", "u1", d * n).reshape(d, n),
               "member": self._read("member.u8", "u1", d * n).reshape(d, n)}
        require(int(out["present"].max(initial=0)) <= 1 and int(out["member"].max(initial=0)) <= 1,
                "role: non-binary mask")
        for key, name in (("close", "close.f64"), ("raw_close", "raw_close.f64"), ("volume", "volume.f64")):
            out[key] = self._read(name, "<f8", d * n).reshape(d, n)
        present = out["present"].astype(bool)
        with np.errstate(invalid="ignore"):
            for key, strict in (("close", True), ("raw_close", True), ("volume", False)):
                x = out[key]
                good = np.isfinite(x) & ((x > 0) if strict else (x >= 0))
                require(bool(np.all(np.where(present, good, np.isnan(x)))), "role: missing/finite field contract")
        return out

    def window(self) -> dict:
        s = self.sessions
        return {"decision_begin": self.begin, "decision_end_exclusive": self.end,
                "decisions": self.end - self.begin,
                "first_decision_session_ns": int(s[self.begin]), "last_decision_session_ns": int(s[self.end - 1]),
                "first_label_session_ns": int(s[self.begin + 2]), "last_label_session_ns": int(s[self.end + 1]),
                "turnover_transitions": self.end - self.begin - 1}


class CacheLayout:
    """The TRAIN IC run's candidate cache layout, taken from its pinned ``summary.json``.

    ``roles[train].candidate_cache``: ``directory`` = ROOT/<train manifest sha>,
    ``fields_directory`` = ROOT/<fields manifest sha> (only when the library reads extra fields),
    ``vm_identity`` (absent in pre-identity summaries = the legacy identity). ROOT is the runner's
    ``--candidate-cache`` DIR for the legacy identity, else DIR/<identity>. Relative directories
    resolve against the working directory, which is the runner's cwd under the bounded runner.
    """

    def __init__(self, path: Path, pin: str, role: "RoleManifest", orientations_sha: str, orientation_recipe: str):
        require(is_hash(pin), "runner summary: external lowercase SHA-256 required")
        data = Path(path).read_bytes()
        require(0 < len(data) <= SUMMARY_LIMIT, "runner summary: missing or over 16 MiB")
        require(hashlib.sha256(data).hexdigest() == pin, "runner summary: SHA-256 pin differs")
        j = unique_json(data, "runner summary")
        require(j.get("status") == "complete", "runner summary: run not complete")
        require(j.get("orientations_artifact_sha256") == orientations_sha and
                j.get("recipe_sha256") == orientation_recipe,
                "runner summary: not the run that wrote the pinned orientations")
        roles = j.get("roles")
        # TRAIN-only runs only: a summary holding validation results is never read.
        require(isinstance(roles, list) and len(roles) == 1 and isinstance(roles[0], dict) and
                roles[0].get("role") == "train", "runner summary: must be a TRAIN-only IC run (roles == [train])")
        train = roles[0]
        require(train.get("manifest_sha256") == role.sha, "runner summary: TRAIN manifest differs")
        cache = train.get("candidate_cache")
        require(isinstance(cache, dict) and isinstance(cache.get("directory"), str),
                "runner summary: the TRAIN run did not use --candidate-cache")
        identity = cache.get("vm_identity", LEGACY_VM_IDENTITY)
        require(isinstance(identity, str) and bool(identity), "runner summary: vm_identity")
        self.vm_identity = identity
        self.directory = Path(cache["directory"])
        require(self.directory.name == role.sha, "runner summary: cache directory is not ROOT/<train manifest sha>")
        root = self.directory.parent
        require(identity == LEGACY_VM_IDENTITY or root.name == identity,
                "runner summary: cache directory is not DIR/<vm identity>/<sha> for a non-legacy identity")
        fields = train.get("research_fields")
        self.fields_sha = fields.get("manifest_sha256") if isinstance(fields, dict) else None
        require(self.fields_sha is None or is_hash(self.fields_sha), "runner summary: research_fields manifest SHA")
        self.fields_directory = None
        if "fields_directory" in cache:
            require(isinstance(cache["fields_directory"], str) and self.fields_sha is not None,
                    "runner summary: fields_directory without a pinned research fields manifest")
            self.fields_directory = Path(cache["fields_directory"])
            require(self.fields_directory.name == self.fields_sha and self.fields_directory.parent == root,
                    "runner summary: fields_directory is not ROOT/<fields manifest sha>")
        # A v2 runner names every candidate's exact entry (either layout); a v1 summary has none and
        # its entries are probed under directory / fields_directory.
        self.entries = None
        if "entries" in cache:
            listed = cache["entries"]
            require(isinstance(listed, list), "runner summary: candidate_cache.entries must be a list")
            self.entries = {}
            for e in listed:
                require(isinstance(e, dict) and isinstance(e.get("id"), str) and e.get("layout") in ("v1", "v2") and
                        isinstance(e.get("sidecar"), str) and isinstance(e.get("payload"), str) and
                        is_hash(e.get("payload_sha256")) and isinstance(e.get("field_payload_sha256"), dict) and
                        all(isinstance(k, str) and is_hash(v) for k, v in e["field_payload_sha256"].items()),
                        "runner summary: malformed candidate_cache entry")
                require(e["id"] not in self.entries, f"runner summary: duplicate candidate_cache entry {e['id']}")
                self.entries[e["id"]] = e

    def resolve(self, cand: dict, role: "RoleManifest") -> dict:
        """The one cache entry of this candidate, validated as cached_payload_sha() does.

        With ``entries`` (v2 runner) the summary names it (resolve_listed). Otherwise (v1 runner) an
        entry is the candidate's when its sidecar names this id and DSL SHA; a same-id entry of
        another DSL (an older library) is ignored. Exactly one entry must be the candidate's.
        """
        if self.entries is not None:
            return self.resolve_listed(cand, role)
        cid = cand["id"]
        found = []
        for directory, fields_sha in ((self.directory, None), (self.fields_directory, self.fields_sha)):
            if directory is None or not (directory / f"{cid}.json").is_file():
                continue
            data = (directory / f"{cid}.json").read_bytes()
            require(0 < len(data) <= METADATA_LIMIT, f"candidate cache: sidecar extent {cid}")
            j = unique_json(data, f"candidate cache {cid}")
            if j.get("candidate_id") == cid and j.get("dsl_sha256") == cand["dsl_sha256"]:
                found.append((directory, fields_sha, j))
        require(bool(found), f"candidate cache: missing entry {cid} (run the TRAIN IC runner with --candidate-cache)")
        require(len(found) == 1, f"candidate cache: {cid} has entries in both the base and the fields directory")
        directory, fields_sha, j = found[0]
        sha = j.get("payload_sha256")
        if fields_sha is None:
            fields_match = "fields_manifest_sha256" not in j
        else:
            fields_match = j.get("fields_manifest_sha256") == fields_sha
        if "vm_identity" in j:
            vm_match = j.get("vm_identity") == self.vm_identity
        else:  # keyless sidecars: base entries recorded by the verified legacy builds only
            vm_match = (self.vm_identity == LEGACY_VM_IDENTITY and fields_sha is None and
                        j.get("engine_git_sha") in LEGACY_ENGINE_SHAS)
        require(j.get("schema") == CACHE_SCHEMA and j.get("role_manifest_sha256") == role.sha and
                j.get("eval_mode") == VM_EVAL_MODE and j.get("layout") == CACHE_LAYOUT and
                j.get("dates") == role.dates and j.get("instruments") == role.instruments and
                j.get("bytes") == role.dates * role.instruments * 8 and j.get("payload") == f"{cid}.f64" and
                is_hash(sha), f"candidate cache: entry mismatch {cid}")
        require(fields_match, f"candidate cache: fields manifest mismatch {cid}")
        require(vm_match, f"candidate cache: VM identity mismatch {cid}")
        require(j.get("role") == "train", f"candidate cache: entry {cid} is not a TRAIN-role signal")
        # A v1 summary lists no per-field payload map: a base candidate reads none, a field candidate's is unknown.
        return {"id": cid, "directory": directory, "payload": directory / f"{cid}.f64", "payload_sha256": sha,
                "fields_manifest_sha256": fields_sha, "field_payload_sha256": {} if fields_sha is None else None}

    def resolve_listed(self, cand: dict, role: "RoleManifest") -> dict:
        """The summary-named entry of this candidate, checked against its sidecar.

        v2: ROOT/<train sha>/<id>.<dsl16>.json for a base candidate, ROOT/<train sha>/fp_<fk16>/... for
        a field candidate (fk16 = SHA-256 of its field lines); the sidecar must record the runner's
        signal key, recomputed here. v1 (read in place): ROOT/<train sha>/<id>.json, or
        ROOT/<fields manifest sha>/<id>.json for a field candidate -- possibly an older manifest whose
        field payloads the runner matched (--cache-legacy-fields). Base vs field is the entry's
        field_payload_sha256; a field candidate's work is keyed by the pinned fields manifest either
        way, so records and admission rows do not depend on the layout.
        """
        cid = cand["id"]
        e = self.entries.get(cid)
        require(e is not None, f"candidate cache: missing entry {cid} (run the TRAIN IC runner with --candidate-cache)")
        fields, v2 = e["field_payload_sha256"], e["layout"] == "v2"
        require(not fields or self.fields_sha is not None,
                f"candidate cache: field entry {cid} without a pinned research fields manifest")
        sidecar, payload = Path(e["sidecar"]), Path(e["payload"])
        directory = sidecar.parent
        stem = f"{cid}.{cand['dsl_sha256'][:16]}" if v2 else cid
        if not fields:
            where = directory == self.directory
        elif v2:
            where = directory == self.directory / f"fp_{hashlib.sha256(field_lines(fields).encode()).hexdigest()[:16]}"
        else:
            where = directory.parent == self.directory.parent and is_hash(directory.name)
        require(where and sidecar.name == f"{stem}.json" and payload == directory / f"{stem}.f64",
                f"candidate cache: entry path mismatch {cid}")
        require(sidecar.is_file(), f"candidate cache: missing entry {cid} (run the TRAIN IC runner with --candidate-cache)")
        data = sidecar.read_bytes()
        require(0 < len(data) <= METADATA_LIMIT, f"candidate cache: sidecar extent {cid}")
        j = unique_json(data, f"candidate cache {cid}")
        require(isinstance(j, dict) and j.get("candidate_id") == cid and j.get("dsl_sha256") == cand["dsl_sha256"],
                f"candidate cache: entry {cid} records another candidate or DSL")
        require(j.get("role_manifest_sha256") == role.sha and j.get("eval_mode") == VM_EVAL_MODE and
                j.get("layout") == CACHE_LAYOUT and j.get("dates") == role.dates and
                j.get("instruments") == role.instruments and j.get("bytes") == role.dates * role.instruments * 8 and
                j.get("payload") == f"{stem}.f64" and j.get("payload_sha256") == e["payload_sha256"],
                f"candidate cache: entry mismatch {cid}")
        if v2:
            key = signal_key_sha256(self.vm_identity, role, cand["dsl_sha256"], fields)
            require(j.get("schema") == CACHE_SCHEMA_V2 and j.get("field_payload_sha256") == fields and
                    j.get("signal_key_sha256") == key and e.get("signal_key_sha256") == key,
                    f"candidate cache: signal key mismatch {cid}")
            fields_match, vm_match = True, j.get("vm_identity") == self.vm_identity
        else:
            require(j.get("schema") == CACHE_SCHEMA, f"candidate cache: entry mismatch {cid}")
            fields_match = (j.get("fields_manifest_sha256") == directory.name if fields
                            else "fields_manifest_sha256" not in j)
            if "vm_identity" in j:
                vm_match = j.get("vm_identity") == self.vm_identity
            else:  # keyless sidecars: base entries recorded by the verified legacy builds only
                vm_match = (self.vm_identity == LEGACY_VM_IDENTITY and not fields and
                            j.get("engine_git_sha") in LEGACY_ENGINE_SHAS)
        require(fields_match, f"candidate cache: fields manifest mismatch {cid}")
        require(vm_match, f"candidate cache: VM identity mismatch {cid}")
        require(j.get("role") == "train", f"candidate cache: entry {cid} is not a TRAIN-role signal")
        return {"id": cid, "directory": directory, "payload": payload, "payload_sha256": e["payload_sha256"],
                "fields_manifest_sha256": self.fields_sha if fields else None, "field_payload_sha256": dict(fields)}


def field_lines(fields: dict) -> str:
    """strategy_ic_runner.cpp field_lines(): one ``field=<name>:<payload sha256>`` line per field, by name."""
    return "".join(f"field={name}:{fields[name]}\n" for name in sorted(fields))


def signal_key_sha256(vm_identity: str, role: "RoleManifest", dsl_sha: str, fields: dict) -> str:
    """strategy_ic_runner.cpp signal_key_text(), hashed: the v2 candidate signal cache key."""
    text = (f"{SIGNAL_KEY_SCHEMA}\nvm_identity={vm_identity}\neval_mode={VM_EVAL_MODE}\nlayout={CACHE_LAYOUT}\n"
            f"role_manifest_sha256={role.sha}\ndates={role.dates}\ninstruments={role.instruments}\n"
            f"dsl_sha256={dsl_sha}\n{field_lines(fields)}")
    return hashlib.sha256(text.encode()).hexdigest()


def work_key_text(role_sha: str, vm_identity: str, screen: str, dsl_sha: str, entry: dict) -> str:
    """The WorkStore key of one candidate's records (``atx.fit-work-key/v1``, one ``\\n``-terminated line each).

    The content key of the signal (role, VM identity, DSL, the field payloads it reads) plus the screen id and
    the semantics tag. A field candidate of a v1 runner summary lists no field payload map, so its key carries
    the fields manifest SHA instead (coarser: any fields change recomputes it)."""
    lines = [WORK_KEY_SCHEMA, f"semantics_tag={SEMANTICS_TAG}", f"role_manifest_sha256={role_sha}",
             f"vm_identity={vm_identity}", f"screen={screen}", f"dsl_sha256={dsl_sha}"]
    fields = entry.get("field_payload_sha256")
    text = "".join(f"{line}\n" for line in lines)
    if fields is None:
        require(is_hash(entry.get("fields_manifest_sha256")), f"work key: {entry.get('id')} has no field payload "
                                                              "map and no fields manifest")
        return text + f"fields_manifest_sha256={entry['fields_manifest_sha256']}\n"
    return text + field_lines(fields)


def work_key_sha256(role_sha: str, vm_identity: str, screen: str, dsl_sha: str, entry: dict) -> str:
    return hashlib.sha256(work_key_text(role_sha, vm_identity, screen, dsl_sha, entry).encode()).hexdigest()


def load_candidate_signal(entry: dict, role: "RoleManifest") -> np.ndarray:
    """The verified raw (unoriented) VM signal, date-major (dates, instruments)."""
    cid, size = entry["id"], role.dates * role.instruments * 8
    payload = Path(entry["payload"]).read_bytes()
    require(len(payload) == size, f"candidate cache: payload extent {cid}")
    require(hashlib.sha256(payload).hexdigest() == entry["payload_sha256"],
            f"candidate cache: payload SHA-256 mismatch {cid}")
    return np.frombuffer(payload, dtype="<f8").reshape(role.dates, role.instruments)


# ------------------------------------------------------- price-risk-v1 exposures
class PricePanel:
    """Valid guarded interval returns, equal-weight market and usable dollar volume.

    Mirrors strategy_price_exposures.cpp: interval t spans sessions (t-1, t]. Its return
    is valid iff both endpoints are present with finite positive close and raw_close,
    the simple return is finite, and |log adj| <= 1.5 and <= |log raw| + 0.10.
    """

    def __init__(self, close: np.ndarray, raw: np.ndarray, volume: np.ndarray, present: np.ndarray):
        d, n = close.shape
        self.returns = np.full((d, n), np.nan)
        pres = present.astype(bool)
        chunk = 128
        with np.errstate(all="ignore"):
            for a in range(1, d, chunk):
                b = min(d, a + chunk)
                c0, c1, r0, r1 = close[a - 1:b - 1], close[a:b], raw[a - 1:b - 1], raw[a:b]
                p0 = pres[a - 1:b - 1] & np.isfinite(c0) & np.isfinite(r0) & (c0 > 0) & (r0 > 0)
                p1 = pres[a:b] & np.isfinite(c1) & np.isfinite(r1) & (c1 > 0) & (r1 > 0)
                both = p0 & p1
                r = c1 / c0 - 1.0
                log_adj = np.log(c1) - np.log(c0)
                log_raw = np.log(r1) - np.log(r0)
                bad = (~np.isfinite(r) | (np.abs(log_adj) > GUARD_ABS_LOG) |
                       (np.abs(log_adj) > np.abs(log_raw) + GUARD_EXCESS_LOG))
                self.returns[a:b] = np.where(both & ~bad, r, np.nan)
            valid = ~np.isnan(self.returns)
            count = valid.sum(axis=1)
            total = np.where(valid, self.returns, 0.0).sum(axis=1)
            self.market = np.where(count > 0, total / np.maximum(count, 1), np.nan)
            dollars = raw * volume
            usable = pres & np.isfinite(raw) & (raw > 0) & np.isfinite(volume) & (volume >= 0) & np.isfinite(dollars)
            self.dollars = np.where(usable, dollars, 0.0)

    def exposures(self, d: int, cols: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """(beta, vol, log_adv) at decision d for instrument columns ``cols`` and the ok mask."""
        block = max(BETA_WINDOW, VOL_WINDOW)
        first = d + 1 - block if d >= block else 1
        intervals = d + 1 - first if d >= first else 0
        beta_rows, vol_rows = min(BETA_WINDOW, intervals), min(VOL_WINDOW, intervals)
        out = np.full((len(cols), 3), np.nan)
        with np.errstate(all="ignore"):
            if beta_rows:
                # Two-pass over valid (return, market) pairs, as beta_of().
                r = self.returns[d + 1 - beta_rows:d + 1][:, cols]  # owned copy
                mk = self.market[d + 1 - beta_rows:d + 1]
                invalid = np.isnan(r)
                if np.isnan(mk).any():
                    invalid |= np.isnan(mk)[:, None]
                n = r.shape[0] - np.count_nonzero(invalid, axis=0)
                safe = np.maximum(n, 1)
                mkt = np.broadcast_to(mk[:, None], r.shape).copy()
                r[invalid] = 0.0
                mkt[invalid] = 0.0
                r -= r.sum(axis=0) / safe
                mkt -= mkt.sum(axis=0) / safe
                r[invalid] = 0.0
                mkt[invalid] = 0.0
                cov = np.einsum("ij,ij->j", r, mkt)
                var = np.einsum("ij,ij->j", mkt, mkt)
                out[:, 0] = np.where((n >= MIN_RETURN_PAIRS) & (var > 0), cov / np.where(var > 0, var, 1.0), np.nan)
            if vol_rows:
                # Two-pass sample SD over valid returns, as vol_of().
                r = self.returns[d + 1 - vol_rows:d + 1][:, cols]
                invalid = np.isnan(r)
                n = r.shape[0] - np.count_nonzero(invalid, axis=0)
                r[invalid] = 0.0
                r -= r.sum(axis=0) / np.maximum(n, 1)
                r[invalid] = 0.0
                squares = np.einsum("ij,ij->j", r, r)
                out[:, 1] = np.where(n >= VOL_MIN_COUNT, np.sqrt(squares / np.maximum(n - 1, 1)), np.nan)
            if d + 1 >= ADV_WINDOW:
                mean_dollars = self.dollars[d + 1 - ADV_WINDOW:d + 1][:, cols].sum(axis=0) / ADV_WINDOW
                out[:, 2] = np.where(mean_dollars > 0, np.log(np.where(mean_dollars > 0, mean_dollars, 1.0)), np.nan)
        ok = np.isfinite(out).all(axis=1)
        return out, ok


def neutralization_basis(exposures: np.ndarray) -> tuple[np.ndarray | None, str | None]:
    """Orthonormal basis of [1, z_beta, z_vol, z_ladv] over the used rows, or a refusal.

    Refusals mirror neutralize_target: too few names, a constant exposure column, or a
    Jacobi-equilibrated Cholesky pivot <= 1e-8. The C++ solves the normal equations with
    one refinement step. QR gives the same OLS residual to rounding.
    """
    m = exposures.shape[0]
    if m < MIN_NAMES:
        return None, "too-few-usable-names"
    z = np.empty((m, 3))
    for k in range(3):
        x = exposures[:, k]
        mean = x.sum() / m
        sd = math.sqrt(float(((x - mean) ** 2).sum()) / (m - 1))
        if not (math.isfinite(sd) and sd > RELATIVE_SD_FLOOR * float(np.abs(x).max())):
            return None, "constant-exposure"
        z[:, k] = np.clip((x - mean) / sd, -CLIP_Z, CLIP_Z)
    design = np.column_stack([np.ones(m), z])
    a = design.T @ design
    scale = 1.0 / np.sqrt(np.diag(a))
    c = a * scale[:, None] * scale[None, :]
    lower = np.zeros((4, 4))
    for j in range(4):
        pivot = c[j, j] - float(lower[j, :j] @ lower[j, :j])
        if not pivot > MIN_PIVOT:
            return None, "ill-conditioned-exposures"
        lower[j, j] = math.sqrt(pivot)
        for i in range(j + 1, 4):
            lower[i, j] = (c[i, j] - float(lower[i, :j] @ lower[j, :j])) / lower[j, j]
    basis, _ = np.linalg.qr(design)
    return basis, None


def centered_tied_ranks(values: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Row-wise centered tied ranks over ``valid`` cells, 0 elsewhere.

    Same value as each_centered_rank (strategy_ic_composition.cpp): a tie group
    [b, e) of the ascending order gets (b + (e - 1)) / (2 (n - 1)) - 0.5. Rows with
    fewer than two valid cells are all zero.
    """
    t, n = values.shape
    keyed = np.where(valid, values, np.inf)
    order = np.argsort(keyed, axis=1)  # order within a tie group does not change its rank
    ordered = np.take_along_axis(keyed, order, axis=1)
    count = valid.sum(axis=1)
    idx = np.arange(n)
    starts = np.ones((t, n), dtype=bool)
    starts[:, 1:] = ordered[:, 1:] != ordered[:, :-1]
    ends = np.ones((t, n), dtype=bool)
    ends[:, :-1] = ordered[:, :-1] != ordered[:, 1:]
    first = np.maximum.accumulate(np.where(starts, idx, 0), axis=1)
    last = np.minimum.accumulate(np.where(ends, idx, n - 1)[:, ::-1], axis=1)[:, ::-1]
    denominator = 2.0 * np.maximum(count - 1, 1).astype(np.float64)
    ranks = (first.astype(np.float64) + last.astype(np.float64)) / denominator[:, None] - 0.5
    keep = (idx[None, :] < count[:, None]) & (count[:, None] >= 2)
    out = np.zeros((t, n))
    np.put_along_axis(out, order, np.where(keep, ranks, 0.0), axis=1)
    return out


def canonical_bytes(document: dict) -> bytes:
    return (json.dumps(document, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def canonical_compact(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def write_synced(path: Path, data: bytes, mode: str = "wb") -> None:
    with Path(path).open(mode) as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


class Context:
    """Per-decision neutralization bases and forward returns, shared by every candidate.

    ``digest`` is the SHA-256 of the canonical metadata. That metadata holds the per-array SHA-256s,
    the role manifest SHA, the window and the refused decisions. A factor record names the digest of
    the context it was computed under.
    """

    ARRAYS = (("columns", "<i8"), ("used", "u1"), ("basis", "<f8"), ("forward", "<f8"), ("used_rows", "<i8"))

    def __init__(self, role_sha: str, begin: int, end: int, arrays: dict, refused: list, meta: dict | None = None):
        self.role_sha, self.begin, self.end, self.refused = role_sha, begin, end, refused
        self.columns = arrays["columns"]
        self.used = arrays["used"].astype(bool)
        self.basis, self.forward, self.used_rows = arrays["basis"], arrays["forward"], arrays["used_rows"]
        if meta is None:
            meta = {"schema": CONTEXT_SCHEMA, "semantics": CONTEXT_SEMANTICS, "role_manifest_sha256": role_sha,
                    "script_sha256": SCRIPT_SHA256,
                    "decision_begin": begin, "decision_end_exclusive": end, "refused": refused,
                    "arrays": {name: {"dtype": dtype, "shape": list(arrays[name].shape),
                                      "sha256": hashlib.sha256(self._bytes(name, dtype)).hexdigest()}
                               for name, dtype in self.ARRAYS}}
        self.meta = meta
        self.digest = hashlib.sha256(canonical_compact(meta)).hexdigest()

    def _bytes(self, name: str, dtype: str) -> bytes:
        return np.ascontiguousarray(getattr(self, name).astype(dtype, copy=False)).tobytes()

    @classmethod
    def build(cls, role: RoleManifest, log=None) -> "Context":
        started = time.perf_counter()
        p = role.payload()
        panel = PricePanel(p["close"], p["raw_close"], p["volume"], p["present"])
        member = p["member"].astype(bool) & p["present"].astype(bool)  # the runner's effective member
        del p
        bases: list[tuple[np.ndarray, np.ndarray] | None] = []
        used_any = np.zeros(role.instruments, dtype=bool)
        refused: list[dict] = []
        for d in range(role.begin, role.end):
            cols = np.flatnonzero(member[d])
            exposures, ok = panel.exposures(d, cols)
            basis, reason = neutralization_basis(exposures[ok])
            if basis is None:
                refused.append({"decision_index": d, "reason": reason, "used_rows": int(ok.sum())})
                bases.append(None)
                continue
            used = cols[ok]
            bases.append((used, basis))
            used_any[used] = True
        columns = np.flatnonzero(used_any).astype(np.int64)
        position = np.full(role.instruments, -1, dtype=np.int64)
        position[columns] = np.arange(len(columns))
        t, width = role.end - role.begin, len(columns)
        used_mask = np.zeros((t, width), dtype=bool)
        basis_all = np.zeros((t, 4, width))
        used_rows = np.zeros(t, dtype=np.int64)
        for row, item in enumerate(bases):
            if item is None:
                continue
            used, basis = item
            p_ = position[used]
            used_mask[row, p_] = True
            basis_all[row][:, p_] = basis.T
            used_rows[row] = len(used)
        forward = panel.returns[role.begin + 2:role.end + 2][:, columns]
        forward = np.where(np.isnan(forward), 0.0, forward)
        ctx = cls(role.sha, role.begin, role.end, {"columns": columns, "used": used_mask, "basis": basis_all,
                                                     "forward": forward, "used_rows": used_rows}, refused)
        if log:
            log(f"fit: context built decisions={t} columns={width} refused={len(refused)} "
                f"seconds={time.perf_counter() - started:.2f}")
        return ctx

    def save(self, directory: Path) -> None:
        """Replace the context directory: build aside, move the old one aside, rename, delete the old.

        A kill at any point leaves either a complete context, no context (rebuilt next run), or a
        stray ``.partial-*``/``.old-*`` sibling that is never read.
        """
        directory = Path(directory)
        partial = directory.with_name(directory.name + f".partial-{os.getpid()}")
        old = directory.with_name(directory.name + f".old-{os.getpid()}")
        for stray in (partial, old):
            if stray.exists():
                shutil.rmtree(stray)
        partial.mkdir(parents=True)
        for name, dtype in self.ARRAYS:
            write_synced(partial / f"{name}.bin", self._bytes(name, dtype))
        write_synced(partial / "context.json", canonical_bytes(self.meta))
        if directory.exists():
            os.rename(directory, old)
        os.rename(partial, directory)
        if old.exists():
            shutil.rmtree(old, ignore_errors=True)

    @classmethod
    def load(cls, directory: Path, role: RoleManifest) -> "Context | None":
        """The verified cached context, or None (absent, stale or corrupt: rebuild)."""
        directory = Path(directory)
        try:
            meta = json.loads((directory / "context.json").read_bytes())
            if (meta.get("schema") != CONTEXT_SCHEMA or meta.get("semantics") != CONTEXT_SEMANTICS or
                    meta.get("script_sha256") != SCRIPT_SHA256 or meta.get("role_manifest_sha256") != role.sha or meta.get("decision_begin") != role.begin or
                    meta.get("decision_end_exclusive") != role.end or not isinstance(meta.get("refused"), list)):
                return None
            arrays = {}
            for name, dtype in cls.ARRAYS:
                spec = meta["arrays"][name]
                data = (directory / f"{name}.bin").read_bytes()
                if spec["dtype"] != dtype or hashlib.sha256(data).hexdigest() != spec["sha256"]:
                    return None
                arrays[name] = np.frombuffer(data, dtype=dtype).reshape(spec["shape"])
            t, width = role.end - role.begin, arrays["columns"].shape[0]
            if (arrays["used"].shape != (t, width) or arrays["basis"].shape != (t, 4, width) or
                    arrays["forward"].shape != (t, width) or arrays["used_rows"].shape != (t,)):
                return None
            return cls(role.sha, role.begin, role.end, arrays, meta["refused"], meta)
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            return None

    def used_rows_summary(self) -> dict:
        live = self.used_rows[self.used_rows > 0]
        return {"min": int(live.min()) if live.size else 0, "max": int(live.max()) if live.size else 0}

    def _project_out(self, x: np.ndarray) -> np.ndarray:
        coef = np.einsum("tkn,tn->tk", self.basis, x)  # basis rows are orthonormal on used rows
        return x - np.einsum("tkn,tk->tn", self.basis, coef)

    def book(self, signal: np.ndarray, sign: int = 1) -> tuple[np.ndarray, np.ndarray]:
        """Neutralized gross-1 standalone book q (decisions x columns) and its live-decision mask."""
        slab = signal[self.begin:self.end][:, self.columns]
        valid = self.used & np.isfinite(slab)
        q = centered_tied_ranks(slab, valid)
        if sign < 0:
            q = -q
        entry = np.abs(q).sum(axis=1)
        residual = self._project_out(self._project_out(q))  # one refinement step, as the C++
        gross = np.abs(residual).sum(axis=1)
        live = (entry > 0) & np.isfinite(gross) & (gross > MIN_RESIDUAL_FRACTION * entry)
        out = np.zeros_like(residual)
        out[live] = residual[live] / gross[live, None]
        return out, live

    def factor_returns(self, q: np.ndarray) -> np.ndarray:
        return (q * self.forward).sum(axis=1)


# ------------------------------------------------------------- factor records
def seal(body: dict) -> dict:
    out = dict(body)
    out["content_sha256"] = hashlib.sha256(canonical_compact(body)).hexdigest()
    return out


def factor_record(context: Context, signal: np.ndarray, entry: dict, cand: dict, vm_identity: str) -> dict:
    """The candidate's unsigned factor series (None on flat days), tau and live count."""
    q, live = context.book(signal, 1)
    f = context.factor_returns(q)
    return seal({
        "schema": FACTOR_SCHEMA, "context_semantics": CONTEXT_SEMANTICS, "factor_semantics": FACTOR_SEMANTICS,
        "script_sha256": SCRIPT_SHA256, "role_manifest_sha256": context.role_sha, "context_sha256": context.digest,
        "cache_payload_sha256": entry["payload_sha256"], "fields_manifest_sha256": entry["fields_manifest_sha256"],
        "work_key_sha256": entry["work_key_sha256"], "field_payload_sha256": entry["field_payload_sha256"],
        "vm_identity": vm_identity, "candidate_id": cand["id"], "dsl_sha256": cand["dsl_sha256"],
        "decisions": int(len(f)), "f_unsigned": [float(x) if ok else None for x, ok in zip(f, live)],
        "tau": standalone_turnover(q), "live_decisions": int(live.sum()),
        "context_refused": context.refused, "context_used_rows_unrefused": context.used_rows_summary()})


def _floats_or_none(values) -> list:
    return [float(x) if math.isfinite(x) else None for x in values]


def aim_record(context: Context, signal: np.ndarray, entry: dict, cand: dict, vm_identity: str,
               train_mask: np.ndarray) -> dict:
    """The candidate's ew-theme-aim-v1 inputs (R4'): rho at the exact lags, g, half-sample gains, coverage.

    ``train_mask`` flags the context decisions inside TRAIN; every other decision's ranks are NaN. Only
    second moments of the TRAIN signal ranks are read (no returns, no means, no covariances).
    """
    slab = signal[context.begin:context.end][:, context.columns]
    live = context.used & np.isfinite(slab)
    used = context.used_rows
    coverage = np.where(used > 0, live.sum(axis=1) / np.maximum(used, 1), 0.0)
    z = standardized_ranks(slab, live)
    del slab, live
    z[~np.asarray(train_mask, dtype=bool)] = np.nan
    profile = aim_profile(z, train_mask)
    del z
    return seal({
        "schema": AIM_SCHEMA, "aim_semantics": AIM_SEMANTICS, "context_semantics": CONTEXT_SEMANTICS,
        "script_sha256": SCRIPT_SHA256, "role_manifest_sha256": context.role_sha, "context_sha256": context.digest,
        "cache_payload_sha256": entry["payload_sha256"], "fields_manifest_sha256": entry["fields_manifest_sha256"],
        "work_key_sha256": entry["work_key_sha256"], "field_payload_sha256": entry["field_payload_sha256"],
        "vm_identity": vm_identity, "candidate_id": cand["id"], "dsl_sha256": cand["dsl_sha256"],
        "decisions": int(context.end - context.begin), "theta": AIM_THETA, "lags": list(AIM_LAGS),
        "rho": _floats_or_none(profile["rho"]), "rho_half": [_floats_or_none(r) for r in profile["rho_half"]],
        "gain": profile["gain"], "gain_unclipped": profile["gain_unclipped"], "gain_half": profile["gain_half"],
        "rank_decisions": profile["rank_decisions"], "half_split_decision": profile["half_split_decision"],
        "coverage": [float(x) for x in coverage]})


def aim_record_valid(j, entry: dict, role: RoleManifest, vm_identity: str) -> bool:
    try:
        if not isinstance(j, dict):
            return False
        body = {k: v for k, v in j.items() if k != "content_sha256"}
        t = role.end - role.begin
        rho, halves, cov, half = j.get("rho"), j.get("rho_half"), j.get("coverage"), j.get("gain_half")
        opt = lambda xs: isinstance(xs, list) and len(xs) == len(AIM_LAGS) and all(  # noqa: E731
            v is None or type(v) is float for v in xs)
        return (j.get("content_sha256") == hashlib.sha256(canonical_compact(body)).hexdigest() and
                j.get("schema") == AIM_SCHEMA and j.get("aim_semantics") == AIM_SEMANTICS and
                j.get("context_semantics") == CONTEXT_SEMANTICS and j.get("script_sha256") == SCRIPT_SHA256 and
                j.get("role_manifest_sha256") == role.sha and
                j.get("cache_payload_sha256") == entry["payload_sha256"] and
                j.get("work_key_sha256") == entry["work_key_sha256"] and
                j.get("vm_identity") == vm_identity and is_hash(j.get("context_sha256")) and
                j.get("decisions") == t and j.get("theta") == AIM_THETA and j.get("lags") == AIM_LAGS and
                opt(rho) and isinstance(halves, list) and len(halves) == 2 and all(opt(h) for h in halves) and
                type(j.get("gain")) is float and AIM_GAIN_MIN <= j["gain"] <= 1.0 and
                type(j.get("gain_unclipped")) is float and isinstance(half, list) and len(half) == 2 and
                all(type(g) is float for g in half) and type(j.get("rank_decisions")) is int and
                type(j.get("half_split_decision")) is int and isinstance(cov, list) and len(cov) == t and
                all(type(v) is float for v in cov))
    except (ValueError, TypeError, KeyError):
        return False


def record_valid(j, entry: dict, role: RoleManifest, vm_identity: str) -> bool:
    try:
        if not isinstance(j, dict):
            return False
        body = {k: v for k, v in j.items() if k != "content_sha256"}
        f = j.get("f_unsigned")
        return (j.get("content_sha256") == hashlib.sha256(canonical_compact(body)).hexdigest() and
                j.get("schema") == FACTOR_SCHEMA and j.get("context_semantics") == CONTEXT_SEMANTICS and
                j.get("factor_semantics") == FACTOR_SEMANTICS and j.get("script_sha256") == SCRIPT_SHA256 and
                j.get("role_manifest_sha256") == role.sha and
                j.get("cache_payload_sha256") == entry["payload_sha256"] and
                j.get("work_key_sha256") == entry["work_key_sha256"] and
                j.get("vm_identity") == vm_identity and is_hash(j.get("context_sha256")) and
                j.get("decisions") == role.end - role.begin and isinstance(f, list) and len(f) == role.end - role.begin
                and all(v is None or type(v) is float for v in f) and type(j.get("tau")) is float and
                type(j.get("live_decisions")) is int)
    except (ValueError, TypeError):
        return False


class WorkStore:
    """Persistent incremental state: ``<work>/<train-sha>/<semantics-tag>/{context,factors}``.

    Records are named by their work key: ``factors/k-<work key sha256>.json`` (``work_key_text``). Records of
    the pre-v7 layout (``factors/<payload sha>[.f-<fields sha>].json``) are never read."""

    def __init__(self, root: Path, role: RoleManifest, vm_identity: str):
        self.role, self.vm_identity = role, vm_identity
        self.base = Path(root) / role.sha / SEMANTICS_TAG
        self.factors = self.base / "factors"
        self.context_dir = self.base / "context"
        self.aims = self.base / f"aim-{AIM_TAG}"  # ew-theme-aim-v1 only; other compositions never touch it

    def _path(self, work_key: str, directory: Path | None = None) -> Path:
        require(is_hash(work_key), "work store: malformed work key")
        return (directory or self.factors) / f"k-{work_key}.json"

    def get(self, entry: dict) -> dict | None:
        try:
            j = json.loads(self._path(entry["work_key_sha256"]).read_bytes())
        except (OSError, ValueError):
            return None
        return j if record_valid(j, entry, self.role, self.vm_identity) else None

    def get_aim(self, entry: dict) -> dict | None:
        try:
            j = json.loads(self._path(entry["work_key_sha256"], self.aims).read_bytes())
        except (OSError, ValueError):
            return None
        return j if aim_record_valid(j, entry, self.role, self.vm_identity) else None

    def put(self, record: dict, directory: Path | None = None) -> None:
        directory = directory or self.factors
        directory.mkdir(parents=True, exist_ok=True)
        path = self._path(record["work_key_sha256"], directory)
        partial = path.with_name(path.name + f".partial-{os.getpid()}")
        write_synced(partial, canonical_compact(record))
        os.replace(partial, path)

    def put_aim(self, record: dict) -> None:
        self.put(record, self.aims)

    def load_context(self) -> Context | None:
        return Context.load(self.context_dir, self.role)

    def save_context(self, context: Context) -> None:
        context.save(self.context_dir)


def standalone_turnover(q: np.ndarray) -> float:
    """Mean over consecutive decisions of sum_i |q(d)_i - q(d-1)_i|; deployment excluded."""
    return float(np.abs(np.diff(q, axis=0)).sum(axis=1).mean())


# ------------------------------------------------------ v5 R4' aim gain (ew-theme-aim-v1)
def standardized_ranks(signal: np.ndarray, live: np.ndarray, min_names: int = AIM_MIN_NAMES) -> np.ndarray:
    """Per-day centered, unit-variance ranks over live names; NaN elsewhere.

    Ranks are the fitter's tie-aware ``centered_tied_ranks`` (an affine map of average ranks, so the
    standardized values equal those of ``scipy.stats.rankdata``); standardized to mean 0 and population SD 1
    over the day's live names. A day with fewer than ``min_names`` live names or zero rank variance (every
    live name tied, e.g. a constant signal) is all NaN.
    """
    live = np.asarray(live, dtype=bool) & np.isfinite(signal)
    ranks = centered_tied_ranks(signal, live)
    count = live.sum(axis=1)
    safe = np.maximum(count, 1).astype(np.float64)
    dev = np.where(live, ranks - (np.where(live, ranks, 0.0).sum(axis=1) / safe)[:, None], 0.0)
    var = (dev * dev).sum(axis=1) / safe
    ok = (count >= min_names) & (var > 0)
    sd = np.sqrt(np.where(ok, var, 1.0))
    return np.where(live & ok[:, None], dev / sd[:, None], np.nan)


def lag_correlations(z: np.ndarray, lags: list[int], min_names: int = AIM_MIN_NAMES) -> np.ndarray:
    """(len(lags), days): c_j(d) = sum_i z_d,i z_d-j,i / n_both(d) over names finite on both days.

    NaN where d < j or n_both(d) < ``min_names``. NaN rows of ``z`` (days without ranks) have no finite name.
    """
    days = z.shape[0]
    finite = np.isfinite(z)
    zf = np.where(finite, z, 0.0)
    ones = finite.astype(np.float64)
    out = np.full((len(lags), days), np.nan)
    for n, j in enumerate(lags):
        if j >= days:
            continue
        a, b, fa, fb = (zf, zf, ones, ones) if j == 0 else (zf[j:], zf[:-j], ones[j:], ones[:-j])
        num = np.einsum("ij,ij->i", a, b)
        cnt = np.einsum("ij,ij->i", fa, fb)
        ok = cnt >= min_names
        out[n, j:] = np.where(ok, num / np.where(ok, cnt, 1.0), np.nan)
    return out


def _finite_row_means(c: np.ndarray) -> np.ndarray:
    ok = np.isfinite(c)
    cnt = ok.sum(axis=1)
    total = np.where(ok, c, 0.0).sum(axis=1)
    return np.where(cnt > 0, total / np.maximum(cnt, 1), np.nan)


def rank_autocorrelation(z: np.ndarray, lags: list[int], min_names: int = AIM_MIN_NAMES) -> np.ndarray:
    """rho_bar(j) for j in lags: mean over days of the cross-sectional correlation of z_d and z_{d-j} over names finite on both days."""
    return _finite_row_means(lag_correlations(z, lags, min_names))


def aim_gain(rho_at_lags: np.ndarray, lags: list[int], theta: float = AIM_THETA, max_lag: int = AIM_MAX_LAG,
             clip: bool = True) -> float:
    """g = theta * sum_{j=0..max_lag} (1-theta)^j rho(j), rho interpolated linearly between exact lags; NaN lags -> 0."""
    rho = np.interp(np.arange(max_lag + 1), lags, np.nan_to_num(np.asarray(rho_at_lags, dtype=np.float64), nan=0.0))
    g = theta * float(np.sum((1 - theta) ** np.arange(max_lag + 1) * rho))
    return float(min(1.0, max(AIM_GAIN_MIN, g))) if clip else g


def aim_profile(z: np.ndarray, train_mask: np.ndarray, lags: list[int] = AIM_LAGS) -> dict:
    """rho and g over all TRAIN decisions, plus the two half-sample versions (report only).

    Halves split the TRAIN decisions at h = first + floor(n / 2): a lag-j pair (d, d-j) belongs to the
    first half when d < h and to the second when d - j >= h (pairs straddling h are in neither).
    """
    train = np.flatnonzero(np.asarray(train_mask, dtype=bool))
    c = lag_correlations(z, lags)
    rho = _finite_row_means(c)
    h = int(train[0] + len(train) // 2) if train.size else 0
    first = _finite_row_means(c[:, :h])
    second = np.full(len(lags), np.nan)
    for n, j in enumerate(lags):
        if h + j < c.shape[1]:
            second[n] = _finite_row_means(c[n:n + 1, h + j:])[0]
    return {"rho": rho, "rho_half": [first, second], "gain": aim_gain(rho, lags),
            "gain_unclipped": aim_gain(rho, lags, clip=False),
            "gain_half": [aim_gain(first, lags), aim_gain(second, lags)],
            "rank_decisions": int(np.isfinite(z).any(axis=1).sum()), "half_split_decision": h}


def shrink_solution(mu: np.ndarray, cov: np.ndarray) -> np.ndarray:
    shrunk = 0.1 * cov + 0.9 * np.diag(np.diag(cov))
    return np.linalg.solve(shrunk, mu)


def fit_weights(factors: np.ndarray, cost_drag: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """(normalized nonnegative weights, raw MV solution) for factor rows (candidates x decisions).

    ``cost_drag`` (per row, netcost variant only) is subtracted from the mean vector; the covariance,
    shrinkage, clip and normalization are unchanged.
    """
    mu = factors.mean(axis=1)
    if cost_drag is not None:
        mu = mu - cost_drag
    cov = np.atleast_2d(np.cov(factors, ddof=1))
    try:
        raw = shrink_solution(mu, cov)
    except np.linalg.LinAlgError as exc:
        raise FitError(f"fit: shrunk covariance is singular: {exc}") from exc
    require(bool(np.all(np.isfinite(raw))), "fit: non-finite MV solution")
    clipped = np.where(raw > 0, raw, 0.0)
    total = float(clipped.sum())
    require(total > 0, "fit: every MV weight is <= 0 after the nonnegative clip")
    return clipped / total, raw


def factor_stats(f: np.ndarray) -> dict:
    mean = float(f.mean())
    sd = float(f.std(ddof=1))
    sharpe = mean / sd * math.sqrt(ANNUALIZATION) if sd > 0 else None
    return {"factor_mean": mean, "factor_sd": sd, "factor_sharpe_annualized": sharpe}


# ---------------------------------------------------------------------- screen
def _stats(x: np.ndarray) -> tuple[float | None, float | None]:
    """(mean, annualized Sharpe) of live values; Sharpe is None below 2 values or at zero SD."""
    if x.size == 0:
        return None, None
    mean = float(x.mean())
    if x.size < 2:
        return mean, None
    sd = float(x.std(ddof=1))
    return mean, (mean / sd * math.sqrt(ANNUALIZATION) if sd > 0 else None)


def pair_correlation(a: np.ndarray, b: np.ndarray, mask: np.ndarray) -> tuple[float | None, int]:
    """Pearson rho over ``mask`` days where both are live, and that day count."""
    both = mask & np.isfinite(a) & np.isfinite(b)
    n = int(both.sum())
    if n < 2:
        return None, n
    x, y = a[both] - a[both].mean(), b[both] - b[both].mean()
    den = math.sqrt(float((x * x).sum()) * float((y * y).sum()))
    return (float((x * y).sum()) / den if den > 0 else None), n


def screen_v3(factors: np.ndarray, taus: list[float], ids: list[str], fit_mask: np.ndarray,
              hold_mask: np.ndarray) -> list[dict]:
    """v3-admit-v1 decisions for unsigned factor rows (candidates x decisions, NaN = flat day)."""
    rows = []
    for k, f in enumerate(factors):
        live = np.isfinite(f)
        fit, hold = live & fit_mask, live & hold_mask
        raw_mean, _ = _stats(f[fit])
        s = 0 if raw_mean is None or raw_mean == 0 else (1 if raw_mean > 0 else -1)
        g = s * f
        fit_mean, fit_sharpe = _stats(g[fit])
        hold_mean, hold_sharpe = _stats(g[hold])
        failed = []
        if int(fit.sum()) < MIN_FIT_DAYS:
            failed.append("insufficient")
        if taus[k] > TAU_LIMIT:
            failed.append("turnover")
        if not (fit_sharpe is not None and fit_sharpe > 0 and hold_mean is not None and hold_mean > 0):
            failed.append("unstable")
        rows.append({"s_k": s, "tau": taus[k], "fit_days": int(fit.sum()), "fit_mean": fit_mean,
                     "fit_sharpe": fit_sharpe, "hold_days": int(hold.sum()), "hold_mean": hold_mean,
                     "hold_sharpe": hold_sharpe, "failed_checks": failed,
                     "status": "reject_" + failed[0] if failed else None, "redundant_with": None,
                     "redundant_rho": None, "admission_rank": None, "low_overlap_with": [],
                     "undefined_rho_with": []})
    survivors = sorted((k for k, r in enumerate(rows) if r["status"] is None),
                       key=lambda k: (-rows[k]["fit_sharpe"], k))
    admitted: list[int] = []
    for k in survivors:
        worst = None
        for j in admitted:
            rho, n = pair_correlation(factors[k], factors[j], fit_mask)
            if n < MIN_COMMON_DAYS:
                rows[k]["low_overlap_with"].append(ids[j])  # treated as uncorrelated, noted
                continue
            if rho is None:
                rows[k]["undefined_rho_with"].append(ids[j])  # zero variance on common days: uncorrelated
                continue
            if abs(rho) > RHO_LIMIT and (worst is None or abs(rho) > worst[0]):
                worst = (abs(rho), j)
        if worst is None:
            admitted.append(k)
            rows[k]["status"], rows[k]["admission_rank"] = "admitted", len(admitted)
        else:
            rows[k]["status"], rows[k]["redundant_with"] = "reject_redundant", ids[worst[1]]
            rows[k]["redundant_rho"] = worst[0]
    for k, row in enumerate(rows):
        best = None
        for j in admitted:
            if j == k:
                continue
            rho, n = pair_correlation(factors[k], factors[j], fit_mask)
            if rho is not None and n >= MIN_COMMON_DAYS and (best is None or abs(rho) > best[0]):
                best = (abs(rho), j)
        row["max_abs_rho"] = best[0] if best else None
        row["max_abs_rho_with"] = ids[best[1]] if best else None
    return rows


def newey_west_t(x: np.ndarray, lag: int = NW_LAG) -> float | None:
    """t of the mean with a Newey-West (Bartlett, ``lag``) long-run variance; None when undefined."""
    n = int(x.size)
    if n < 2 or bool(np.all(x == x[0])):  # a constant series has no defined t (rounding would invent one)
        return None
    e = x - x.mean()
    lrv = float(e @ e) / n
    for ell in range(1, min(lag, n - 1) + 1):
        lrv += 2.0 * (1.0 - ell / (lag + 1.0)) * float(e[ell:] @ e[:-ell]) / n
    if not (math.isfinite(lrv) and lrv > 0):
        return None
    return float(x.mean()) / math.sqrt(lrv / n)


def screen_v4(factors: np.ndarray, taus: list[float], ids: list[str], train_mask: np.ndarray,
              tier_rank: list[int], prior_signs: list[int], cost_tau_limit: float | None = None) -> list[dict]:
    """v4-prior-v1 decisions for factor rows oriented by the DSL (s_k = +1; NaN = flat day).

    ``cost_tau_limit`` (v4-prior-v2) adds the turnover_cost check after turnover; None is v4-prior-v1."""
    rows = []
    for k, f in enumerate(factors):
        live = np.isfinite(f) & train_mask
        x = f[live]
        mean, sharpe = _stats(x)
        t_hac = newey_west_t(x)
        failed = []
        if prior_signs[k] == 0:
            failed.append("no_prior")
        if int(live.sum()) < V4_MIN_TRAIN_DAYS:
            failed.append("insufficient")
        if taus[k] > V4_TAU_LIMIT:
            failed.append("turnover")
        if cost_tau_limit is not None and taus[k] > cost_tau_limit:
            failed.append("turnover_cost")
        if t_hac is not None and t_hac < V4_VETO_T:
            failed.append("veto")
        rows.append({"s_k": prior_signs[k], "tau": taus[k], "train_days": int(live.sum()), "train_mean": mean,
                     "train_sharpe": sharpe, "hac_t": t_hac, "failed_checks": failed,
                     "status": "reject_" + failed[0] if failed else None, "redundant_with": None,
                     "redundant_rho": None, "admission_rank": None, "low_overlap_with": [],
                     "undefined_rho_with": []})
    # Declared order: (tier, roster order); a TRAIN statistic never orders the greedy pass.
    survivors = sorted((k for k, r in enumerate(rows) if r["status"] is None), key=lambda k: (tier_rank[k], k))
    admitted: list[int] = []
    for k in survivors:
        worst = None
        for j in admitted:
            rho, n = pair_correlation(factors[k], factors[j], train_mask)
            if n < MIN_COMMON_DAYS:
                rows[k]["low_overlap_with"].append(ids[j])
                continue
            if rho is None:
                rows[k]["undefined_rho_with"].append(ids[j])
                continue
            if abs(rho) > V4_RHO_LIMIT and (worst is None or abs(rho) > worst[0]):
                worst = (abs(rho), j)
        if worst is None:
            admitted.append(k)
            rows[k]["status"], rows[k]["admission_rank"] = "admitted", len(admitted)
        else:
            rows[k]["status"], rows[k]["redundant_with"] = "reject_redundant", ids[worst[1]]
            rows[k]["redundant_rho"] = worst[0]
    for k, row in enumerate(rows):
        best = None
        for j in admitted:
            if j == k:
                continue
            rho, n = pair_correlation(factors[k], factors[j], train_mask)
            if rho is not None and n >= MIN_COMMON_DAYS and (best is None or abs(rho) > best[0]):
                best = (abs(rho), j)
        row["max_abs_rho"] = best[0] if best else None
        row["max_abs_rho_with"] = ids[best[1]] if best else None
    return rows


def ew_theme_weights(themes: list[str]) -> tuple[np.ndarray, dict]:
    """ew-theme-v1 over the members that take part: 1 / (themes present * members of the theme)."""
    present = sorted(set(themes))
    counts = {t: themes.count(t) for t in present}
    weights = np.array([1.0 / (len(present) * counts[t]) for t in themes])
    table = {t: {"admitted_count": counts[t], "theme_weight": 1.0 / len(present),
                 "member_weight": 1.0 / (len(present) * counts[t])} for t in present}
    return weights, table


def ew_theme_aim_weights(themes: list[str], gains: list[float]) -> tuple[np.ndarray, dict]:
    """ew-theme-aim-v1 (R4'): w_k proportional to g_k / (themes present * members of the theme), normalised globally."""
    present = sorted(set(themes))
    counts = {t: themes.count(t) for t in present}
    raw = np.array([g / (len(present) * counts[t]) for t, g in zip(themes, gains)])
    require(bool(np.all(np.isfinite(raw))) and float(raw.sum()) > 0, "fit: aim gains must be finite with a positive sum")
    weights = raw / raw.sum()
    table = {t: {"admitted_count": counts[t], "nominal_theme_weight": 1.0 / len(present),
                 "aim_theme_weight": float(sum(w for w, th in zip(weights, themes) if th == t))} for t in present}
    return weights, table


def ew_theme_v6_weights(ids: list[str], themes: list[str], taus: list[float]) -> tuple[np.ndarray, dict, dict]:
    """ew-theme-v6 (v6 revision V6-W) over the members that take part (admitted, non-degenerate), in input order.

    (b) theme' = options_implied -> short_interest, else the prior theme; (a) a low_risk member gets weight 0.
    (c) within theme', b = 1/n; a member with tau_k >= V6_FAST_TAU keeps b/3 and the freed mass goes pro rata (by b)
        to the theme's slow members; with no slow member it returns to the same members (weights stay b).
    (d) w_k = within_k / T, T = themes' with >= 1 kept member. Returns (weights, theme table, detail)."""
    require(len(ids) == len(themes) == len(taus), "fit: ew-theme-v6 inputs differ in length")
    require(all(math.isfinite(t) for t in taus), "fit: ew-theme-v6 needs a finite standalone tau per member")
    mapped = [V6_MERGED_THEMES.get(t, t) for t in themes]
    kept = [k for k, t in enumerate(mapped) if t not in V6_DROPPED_THEMES]
    present = sorted({mapped[k] for k in kept})
    weights = np.zeros(len(ids))
    table: dict = {}
    for theme in present:
        members = [k for k in kept if mapped[k] == theme]
        base = 1.0 / len(members)
        fast = [k for k in members if taus[k] >= V6_FAST_TAU]
        slow = [k for k in members if not taus[k] >= V6_FAST_TAU]
        within = {k: base for k in members}
        if fast and slow:
            freed = 0.0
            for k in fast:
                within[k] = base * V6_FAST_FACTOR
                freed += base - within[k]
            slow_mass = base * len(slow)
            for k in slow:
                within[k] = base + freed * base / slow_mass  # pro rata by the pre-shrink weight
        for k in members:
            weights[k] = within[k] / len(present)
        table[theme] = {
            "members": [ids[k] for k in members], "admitted_count": len(members),
            "source_themes": sorted({themes[k] for k in members}), "theme_weight": 1.0 / len(present),
            "base_within_weight": base, "fast_members": [ids[k] for k in fast],
            "shrink": "applied" if fast and slow else ("none-all-fast" if fast else "none"),
            "within_theme_weights": {ids[k]: within[k] for k in members},
            "member_weights": {ids[k]: float(weights[k]) for k in members}}
    detail = {
        "dropped_members": [ids[k] for k in range(len(ids)) if mapped[k] in V6_DROPPED_THEMES],
        "merged_members": {ids[k]: mapped[k] for k in range(len(ids)) if themes[k] in V6_MERGED_THEMES},
        "shrunk_members": [ids[k] for t in present for k in kept if mapped[k] == t
                           and table[t]["shrink"] == "applied" and taus[k] >= V6_FAST_TAU],
        "fast_not_shrunk_all_fast_theme": [ids[k] for t in present for k in kept if mapped[k] == t
                                           and table[t]["shrink"] == "none-all-fast"],
        "fast_in_dropped_theme": [ids[k] for k in range(len(ids))
                                  if mapped[k] in V6_DROPPED_THEMES and taus[k] >= V6_FAST_TAU],
        "member_tau": {ids[k]: taus[k] for k in range(len(ids))}}
    return weights, table, detail


ADMISSION_STATUSES = ("admitted", "reject_insufficient", "reject_turnover", "reject_unstable", "reject_redundant")
CSV_COLUMNS = ("id", "family", "status", "failed_checks", "redundant_with", "redundant_rho", "admission_rank",
               "s_k", "runner_sign", "sign_agrees", "tau", "fit_days", "fit_mean", "fit_sharpe", "hold_days",
               "hold_mean", "hold_sharpe", "max_abs_rho", "max_abs_rho_with", "low_overlap_with",
               "undefined_rho_with", "cache_entry", "cache_payload_sha256")
V4_CSV_COLUMNS = ("id", "family", "theme", "tier", "prior_sign", "status", "failed_checks", "redundant_with",
                  "redundant_rho", "admission_rank", "s_k", "runner_sign", "sign_agrees", "tau", "train_days",
                  "train_mean", "train_sharpe", "hac_t", "max_abs_rho", "max_abs_rho_with", "low_overlap_with",
                  "undefined_rho_with", "cache_entry", "cache_payload_sha256")


def admission_csv(candidates: list[dict], columns: tuple[str, ...] = CSV_COLUMNS) -> bytes:
    def cell(v) -> str:
        if v is None:
            return ""
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, list):
            return ";".join(str(x) for x in v)
        if isinstance(v, float):
            return repr(v)
        return str(v)

    lines = [",".join(columns)]
    for row in candidates:
        cells = [cell(row[c]) for c in columns]
        require(all("," not in x and "\n" not in x for x in cells), "admission CSV: unsafe cell")
        lines.append(",".join(cells))
    return ("\n".join(lines) + "\n").encode("utf-8")


# ---------------------------------------------------------------------- output
def pending_path(out: Path) -> Path:
    return Path(out).with_name("." + Path(out).name + ".pending")


def publish_directory(out: Path, files: dict[str, bytes]) -> None:
    """Exclusive, all-or-nothing publication: a reader sees no directory or every file complete."""
    out = Path(out)
    require(not out.exists(), f"output exists; refusing overwrite: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    pending = pending_path(out)
    try:
        pending.mkdir()
    except FileExistsError as exc:
        raise FitError(f"stale or concurrent partial output; inspect and remove: {pending}") from exc
    try:
        for name in sorted(files):
            write_synced(pending / name, files[name], "xb")
        os.rename(pending, out)  # refuses an existing target on Windows
    except FileExistsError as exc:
        shutil.rmtree(pending, ignore_errors=True)
        raise FitError(f"output appeared concurrently; refusing overwrite: {out}") from exc
    except BaseException:
        shutil.rmtree(pending, ignore_errors=True)
        raise


class Incomplete(Exception):
    """A clean budget stop: completed candidates are persisted; rerun to continue."""

    def __init__(self, summary: dict):
        super().__init__("incomplete")
        self.summary = summary


def ensure_records(args, role: RoleManifest, library: list[dict], entries: list[dict], vm_identity: str,
                   started: float, log, aim: bool = False) -> tuple[list[dict], int, int, list[dict] | None]:
    """Every candidate's factor record (and, with ``aim``, its aim record) under one context.

    Returns (records, computed now, reused, aim records or None). Without ``aim`` this is exactly the
    pre-v5 path: no aim record is read, computed or written.
    """
    screen = getattr(args, "screen", "none")
    for e, cand in zip(entries, library):  # the records' key: content key of the signal + screen (work_key_text)
        e["work_key_sha256"] = work_key_sha256(role.sha, vm_identity, screen, cand["dsl_sha256"], e)
    store = WorkStore(args.work_dir, role, vm_identity) if args.work_dir else None
    records: list[dict | None] = [store.get(e) if store else None for e in entries]
    aims: list[dict | None] | None = ([store.get_aim(e) if store else None for e in entries] if aim else None)
    digests = {r["context_sha256"] for r in records if r is not None}
    if aims is not None:
        digests |= {a["context_sha256"] for a in aims if a is not None}
    if (all(r is not None for r in records) and (aims is None or all(a is not None for a in aims)) and
            len(digests) == 1):
        if log:
            log(f"fit: computed 0, reused {len(records)}")
        return records, 0, len(records), aims  # type: ignore[return-value]
    context = store.load_context() if store else None
    if context is None:
        context = Context.build(role, log)
        if store:
            store.save_context(context)
    elif log:
        log(f"fit: context reused digest={context.digest[:12]} seconds={time.perf_counter() - started:.2f}")

    def stale(r) -> bool:
        return r is None or r["context_sha256"] != context.digest

    todo = [k for k in range(len(records)) if stale(records[k]) or (aims is not None and stale(aims[k]))]
    reused = len(records) - len(todo)
    decisions = role.sessions[role.begin:role.end]
    train_mask = (decisions >= FIT_BEGIN_NS) & (decisions < TRAIN_END_NS)
    computed, slowest = 0, 0.0
    for k in todo:
        if args.max_new_candidates is not None and computed >= args.max_new_candidates:
            break
        if args.max_seconds is not None and time.perf_counter() - started + slowest > args.max_seconds:
            break
        tick = time.perf_counter()
        signal = load_candidate_signal(entries[k], role)
        if stale(records[k]):
            records[k] = factor_record(context, signal, entries[k], library[k], vm_identity)
            if store:
                store.put(records[k])  # type: ignore[arg-type]
        if aims is not None and stale(aims[k]):
            aims[k] = aim_record(context, signal, entries[k], library[k], vm_identity, train_mask)
            if store:
                store.put_aim(aims[k])  # type: ignore[arg-type]
        del signal
        computed += 1
        slowest = max(slowest, time.perf_counter() - tick)
        if log:
            extra = "" if aims is None else f" g={aims[k]['gain']:.3f}"  # type: ignore[index]
            log(f"fit: {k + 1}/{len(library)} {library[k]['id']} tau={records[k]['tau']:.4f} "  # type: ignore[index]
                f"live={records[k]['live_decisions']}{extra} seconds={time.perf_counter() - tick:.2f}")  # type: ignore[index]
    remaining = [library[k]["id"] for k in todo if stale(records[k]) or (aims is not None and stale(aims[k]))]
    if log:
        log(f"fit: computed {computed}, reused {reused}" + (f", remaining {len(remaining)}" if remaining else ""))
    if remaining:
        raise Incomplete({"status": "incomplete", "partial": True, "computed_this_run": computed, "reused": reused,
                          "remaining": len(remaining), "next": remaining[0],
                          "seconds": round(time.perf_counter() - started, 2)})
    return records, computed, reused, aims  # type: ignore[return-value]


def fit(args, log=None) -> tuple[int, dict]:
    started = time.perf_counter()
    require(args.screen in SCREENS, f"--screen must be one of {SCREENS}")
    require(args.composition in COMPOSITIONS, f"--composition must be one of {COMPOSITIONS}")
    orientation = getattr(args, "orientation", "train")
    recipe_path, recipe_sha = getattr(args, "recipe", None), getattr(args, "recipe_sha256", None)
    require(orientation in ORIENTATIONS, f"--orientation must be one of {ORIENTATIONS}")
    prior = args.screen in PRIOR_SCREENS
    require(prior == (orientation == "prior"), "--orientation prior and --screen v4-prior-v1/v2 go together")
    require(prior == (args.composition in PRIOR_COMPOSITIONS),
            "--composition ew-theme-v1|ew-theme-aim-v1|ew-theme-v6 and --screen v4-prior-v1/v2 go together")
    require(prior or (recipe_path is None and recipe_sha is None), "--recipe is read only by --screen v4-prior-v1/v2")
    require((recipe_path is None) == (recipe_sha is None), "--recipe and --recipe-sha256 go together")
    netcost = args.composition == NETCOST_RULE_ID
    require(args.work_dir is not None or (args.max_seconds is None and args.max_new_candidates is None),
            "--max-seconds/--max-new-candidates need --work-dir (nothing would persist)")
    require(args.max_seconds is None or (math.isfinite(args.max_seconds) and args.max_seconds > 0),
            "--max-seconds must be finite and > 0")
    require(args.max_new_candidates is None or args.max_new_candidates >= 0, "--max-new-candidates must be >= 0")
    library = load_library(args.library, args.library_sha256)
    runner_signs, orientation_recipe = load_orientations(args.orientations, args.orientations_sha256, library,
                                                         args.library_sha256, args.train_sha256)
    priors = load_priors(args.library, args.library_sha256, recipe_path, recipe_sha, library) if prior else None
    out = Path(args.output)
    require(not out.exists(), f"output exists; refusing overwrite: {out}")
    require(not pending_path(out).exists(), f"stale or concurrent partial output; inspect and remove: "
                                            f"{pending_path(out)}")
    role = RoleManifest(args.train, args.train_sha256)
    layout = CacheLayout(args.runner_summary, args.runner_summary_sha256, role, args.orientations_sha256,
                         orientation_recipe)
    entries = [layout.resolve(c, role) for c in library]
    shas = [e["payload_sha256"] for e in entries]
    records, computed, reused, aims = ensure_records(args, role, library, entries, layout.vm_identity, started, log,
                                                     aim=args.composition == AIM_RULE_ID)

    ids = [c["id"] for c in library]
    factors = np.array([[np.nan if v is None else v for v in r["f_unsigned"]] for r in records], dtype=np.float64)
    taus = [r["tau"] for r in records]
    context_sha = records[0]["context_sha256"]
    decision_sessions = role.sessions[role.begin:role.end]
    fit_mask = (decision_sessions >= FIT_BEGIN_NS) & (decision_sessions < HOLD_BEGIN_NS)
    hold_mask = (decision_sessions >= HOLD_BEGIN_NS) & (decision_sessions < TRAIN_END_NS)
    script_sha = SCRIPT_SHA256
    inputs = {"library_sha256": args.library_sha256, "train_manifest_sha256": args.train_sha256,
              "role_source_sha256": role.source_sha256, "orientations_sha256": args.orientations_sha256,
              "orientations_recipe_sha256": orientation_recipe, "runner_summary_sha256": args.runner_summary_sha256,
              "vm_identity": layout.vm_identity, "fields_manifest_sha256": layout.fields_sha,
              "script_sha256": script_sha, "semantics_tag": SEMANTICS_TAG, "context_sha256": context_sha}
    cache_entry = ["base" if e["fields_manifest_sha256"] is None else "fields" for e in entries]
    window = role.window()
    if prior:
        return fit_prior(args, library, priors, runner_signs, factors, taus, shas, cache_entry, records, inputs,
                         window, decision_sessions, computed, reused, out, started, aims)
    files: dict[str, bytes] = {}
    admission_sha = None
    if args.screen == SCREEN_ID:
        rows = screen_v3(factors, taus, ids, fit_mask, hold_mask)
        signs = [r["s_k"] for r in rows]
        eligible = {k for k, r in enumerate(rows) if r["status"] == "admitted"}
        candidates = []
        for k, (cand, row) in enumerate(zip(library, rows)):
            candidates.append({"id": cand["id"], "family": cand["family"], "status": row["status"],
                               "failed_checks": row["failed_checks"], "redundant_with": row["redundant_with"],
                               "redundant_rho": row["redundant_rho"], "undefined_rho_with": row["undefined_rho_with"],
                               "cache_entry": cache_entry[k],
                               "admission_rank": row["admission_rank"], "s_k": row["s_k"],
                               "runner_sign": runner_signs[k], "sign_agrees": runner_signs[k] == row["s_k"],
                               "tau": row["tau"], "tau_over_limit": row["tau"] > TAU_LIMIT,
                               "fit_days": row["fit_days"], "fit_mean": row["fit_mean"], "fit_sharpe": row["fit_sharpe"],
                               "hold_days": row["hold_days"], "hold_mean": row["hold_mean"],
                               "hold_sharpe": row["hold_sharpe"], "max_abs_rho": row["max_abs_rho"],
                               "max_abs_rho_with": row["max_abs_rho_with"], "low_overlap_with": row["low_overlap_with"],
                               "cache_payload_sha256": shas[k]})
        admitted_order = sorted(eligible, key=lambda k: rows[k]["admission_rank"])
        admission = {
            "schema": ADMISSION_SCHEMA, "screen": SCREEN_ID,
            "rules": {"orientation": "s_k=sign(mean unsigned f_k over live FIT decisions);runner sign reported not used",
                      "fit_window_ns": [FIT_BEGIN_NS, HOLD_BEGIN_NS], "hold_window_ns": [HOLD_BEGIN_NS, TRAIN_END_NS],
                      "window_basis": "decision session; statistics over live (non-flat) decisions",
                      "tau_limit": TAU_LIMIT, "rho_limit": RHO_LIMIT, "min_fit_days": MIN_FIT_DAYS,
                      "min_common_days": MIN_COMMON_DAYS, "sharpe_annualization": ANNUALIZATION,
                      "stability": "FIT annualized Sharpe(s_k f_k) > 0 and HOLD mean(s_k f_k) > 0",
                      "redundancy": "survivors by descending FIT Sharpe (ties library order); |pearson rho| over "
                                    "FIT days both live > rho_limit vs an admitted candidate -> reject_redundant "
                                    "(largest |rho|); < min_common_days or undefined rho -> uncorrelated, noted",
                      "status_precedence": list(ADMISSION_STATUSES[1:]),
                      "context": CONTEXT_SEMANTICS, "factor": FACTOR_SEMANTICS},
            "inputs": inputs, "window": window,
            "counts": {s: sum(1 for r in rows if r["status"] == s) for s in ADMISSION_STATUSES},
            "admitted": [ids[k] for k in admitted_order],
            "sign_conflicts": [c["id"] for c in candidates if not c["sign_agrees"]],
            "candidates": candidates}
        files[OUTPUT_ADMISSION] = canonical_bytes(admission)
        files[OUTPUT_ADMISSION_CSV] = admission_csv(candidates)
        admission_sha = hashlib.sha256(files[OUTPUT_ADMISSION]).hexdigest()
        status_of = {k: ("fitted" if r["status"] == "admitted" else r["status"]) for k, r in enumerate(rows)}
    else:
        signs = list(runner_signs)
        eligible = {k for k, s in enumerate(signs) if s != 0}
        status_of = {k: ("fitted" if signs[k] != 0 else "unoriented-sign-0") for k in range(len(library))}

    zero_filled = np.where(np.isnan(factors), 0.0, factors)
    weight_rows = []
    for k, cand in enumerate(library):
        row = {"id": cand["id"], "family": cand["family"], "sign": signs[k], "runner_sign": runner_signs[k],
               "status": status_of[k], "dsl_sha256": cand["dsl_sha256"], "cache_payload_sha256": shas[k],
               "cache_entry": cache_entry[k],
               "tau": taus[k], "tau_over_limit": taus[k] > TAU_LIMIT,
               "flat_decisions": int(np.isnan(factors[k]).sum()), "weight": 0.0, "mv_solution": None,
               "clipped": False}
        if signs[k] == 0:
            row.update(factor_mean=None, factor_sd=None, factor_sharpe_annualized=None)
        else:
            row.update(factor_stats(signs[k] * zero_filled[k]))
            if k in eligible and not row["factor_sd"] > 0:
                row["status"] = "degenerate-zero-variance"
        weight_rows.append(row)
    active = [k for k in sorted(eligible) if weight_rows[k]["status"] == "fitted"]
    no_weights = None
    try:
        require(active, "fit: no eligible candidate with a non-degenerate factor series")
        matrix = np.vstack([signs[k] * zero_filled[k] for k in active])
        drag = np.array([NETCOST_C * taus[k] for k in active]) if netcost else None
        weights, raw = fit_weights(matrix, drag)
    except FitError as exc:
        if args.screen != SCREEN_ID:
            raise
        no_weights = str(exc)
    summary = {"status": "complete", "output": str(out), "screen": args.screen, "composition": args.composition,
               "candidates": len(library),
               "computed_this_run": computed, "reused": reused,
               "refused_decisions": len(records[0]["context_refused"])}
    if args.screen == SCREEN_ID:
        summary.update(admitted=len(eligible), counts=admission["counts"], sign_conflicts=admission["sign_conflicts"],
                       admission_sha256=admission_sha)
    if no_weights is not None:
        publish_directory(out, files)
        summary.update(status="published-without-weights", reason=no_weights,
                       files={n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())},
                       seconds=round(time.perf_counter() - started, 2))
        return EXIT_NO_WEIGHTS, summary
    for k, w, r in zip(active, weights, raw):
        weight_rows[k].update(weight=float(w), mv_solution=float(r), clipped=not r > 0)
        if netcost:
            weight_rows[k]["cost_drag"] = NETCOST_C * taus[k]
    weighted_tau = float(sum(row["weight"] * row["tau"] for row in weight_rows))
    conflicts = [row["id"] for row in weight_rows if row["weight"] > 0 and row["runner_sign"] != row["sign"]]
    document = {
        "schema": WEIGHTS_SCHEMA,
        "library_sha256": args.library_sha256,
        "train_manifest_sha256": args.train_sha256,
        "signs": {row["id"]: row["sign"] for row in weight_rows if row["sign"] != 0},
        "weights": {row["id"]: row["weight"] for row in weight_rows},
        "provenance": {
            "rule": args.composition, "lambda": SHRINK_LAMBDA,
            "shrinkage": "Sh=0.1*S+0.9*diag(S);S=sample-covariance-ddof1;w=solve(Sh,mu);w=max(w,0);w/=sum(w)",
            "screen": args.screen, "admission_sha256": admission_sha,
            "signs": ("v3-admit-v1-FIT-mean-sign;apply-pinned-signs" if args.screen == SCREEN_ID
                      else "TRAIN-orientations-artifact-sign;sign0-weight0"),
            "fit_series": "sign*f over ALL TRAIN scored decisions; flat decisions contribute 0",
            "factor": FACTOR_SEMANTICS, "neutralization": CONTEXT_SEMANTICS,
            "turnover": "tau=mean_d(sum_i|q(d)_i-q(d-1)_i|);consecutive-scored-TRAIN-decisions;"
                        "deployment-excluded;no-price-drift",
            "tau_limit": TAU_LIMIT, "tau_flagged": [row["id"] for row in weight_rows if row["tau_over_limit"]],
            "weighted_standalone_turnover": weighted_tau,
            "library_sha256": args.library_sha256, "role_manifest_sha256": args.train_sha256,
            "role_source_sha256": role.source_sha256, "orientations_sha256": args.orientations_sha256,
            "orientations_recipe_sha256": orientation_recipe, "runner_summary_sha256": args.runner_summary_sha256,
            "vm_identity": layout.vm_identity, "fields_manifest_sha256": layout.fields_sha,
            "script_sha256": script_sha,
            "semantics_tag": SEMANTICS_TAG, "context_sha256": context_sha, "window": window,
            "neutralization_refused_decisions": records[0]["context_refused"],
            "used_rows_unrefused": records[0]["context_used_rows_unrefused"],
            "fitted_candidates": len(active), "sign_conflicts_weighted": conflicts,
            "blend_in_sample_TRAIN_diagnostic": factor_stats(weights @ matrix),
            "candidates": weight_rows,
        },
    }
    if netcost:  # the default composition's bytes carry no netcost keys
        document["provenance"].update(
            netcost_c=NETCOST_C,
            netcost_mu="mu_k=mean_TRAIN(s_k*f_k)-c*tau_k;c per unit one-way GMV turnover/day;"
                       "covariance/shrink/clip/normalization/screen/signs unchanged")
    files[OUTPUT_WEIGHTS] = canonical_bytes(document)
    require(len(files[OUTPUT_WEIGHTS]) <= METADATA_LIMIT, "output: weights JSON exceeds the runner's 1 MiB bound")
    publish_directory(out, files)
    summary.update(files={n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())},
                   weights_sha256=hashlib.sha256(files[OUTPUT_WEIGHTS]).hexdigest(),
                   nonzero_weights=sum(1 for row in weight_rows if row["weight"] > 0),
                   weighted_standalone_turnover=weighted_tau, tau_flagged=document["provenance"]["tau_flagged"],
                   sign_conflicts_weighted=conflicts, seconds=round(time.perf_counter() - started, 2))
    return EXIT_OK, summary


def fit_prior(args, library: list[dict], priors: dict, runner_signs: list[int], factors: np.ndarray,
              taus: list[float], shas: list[str], cache_entry: list[str], records: list[dict], inputs: dict,
              window: dict, decision_sessions: np.ndarray, computed: int, reused: int, out: Path,
              started: float, aims: list[dict] | None = None) -> tuple[int, dict]:
    """v4-prior-v1/v2 admission + ew-theme-v1 weights (pre-registration R3/R4, v4.2 R3'). Nothing is estimated but tau.

    ``ew-theme-aim-v1`` (v5 R4') scales the same member set by the aim gains in ``aims``; the admission
    table is identical and the ew-theme-v1 document carries no aim key (its bytes are unchanged).
    ``ew-theme-v6`` (v6 V6-W) re-weights the same member set by ``ew_theme_v6_weights`` and adds the top-level
    ``theme_redistribution`` block and ``provenance.v6``; the admission table is again identical.
    """
    ids = [c["id"] for c in library]
    screen = args.screen
    aim = args.composition == AIM_RULE_ID
    v6 = args.composition == V6_RULE_ID  # only ew-theme-v6 writes theme_redistribution / provenance.v6
    require(aim == (aims is not None), "fit: aim records exist exactly for --composition ew-theme-aim-v1")
    v2 = screen == PRIOR_SCREEN_V2_ID  # v4-prior-v1 emits exactly its pre-v4.2 bytes (no cost keys)
    statuses = V42_STATUSES if v2 else V4_STATUSES
    themes, tiers, prior_signs = priors["themes"], priors["tiers"], priors["prior_signs"]
    train_mask = (decision_sessions >= FIT_BEGIN_NS) & (decision_sessions < TRAIN_END_NS)
    inputs = dict(inputs, recipe_sha256=priors["recipe_sha256"], prior_metadata_source=priors["source"])
    rows = screen_v4(factors, taus, ids, train_mask, priors["tier_rank"], prior_signs,
                     cost_tau_limit=V42_COST_TAU_LIMIT if v2 else None)
    candidates = []
    for k, (cand, row) in enumerate(zip(library, rows)):
        candidates.append({"id": cand["id"], "family": cand["family"], "theme": themes[k], "tier": tiers[k],
                           "prior_sign": prior_signs[k], "status": row["status"], "failed_checks": row["failed_checks"],
                           "redundant_with": row["redundant_with"], "redundant_rho": row["redundant_rho"],
                           "undefined_rho_with": row["undefined_rho_with"], "cache_entry": cache_entry[k],
                           "admission_rank": row["admission_rank"], "s_k": row["s_k"], "runner_sign": runner_signs[k],
                           "sign_agrees": runner_signs[k] == row["s_k"], "tau": row["tau"],
                           "tau_over_limit": row["tau"] > V4_TAU_LIMIT, "train_days": row["train_days"],
                           "train_mean": row["train_mean"], "train_sharpe": row["train_sharpe"], "hac_t": row["hac_t"],
                           "max_abs_rho": row["max_abs_rho"], "max_abs_rho_with": row["max_abs_rho_with"],
                           "low_overlap_with": row["low_overlap_with"], "cache_payload_sha256": shas[k]})
        if v2:
            candidates[-1]["tau_over_cost_limit"] = row["tau"] > V42_COST_TAU_LIMIT
    admitted_order = sorted((k for k, r in enumerate(rows) if r["status"] == "admitted"),
                            key=lambda k: rows[k]["admission_rank"])
    hac = {"estimator": "newey-west", "kernel": "bartlett", "lag": NW_LAG, "autocovariance_divisor": "n",
           "demeaned": True, "t": "mean/sqrt(LRV/n)", "series": "f_k over live TRAIN decisions (s_k=+1)",
           "undefined": "n<2, constant series or LRV<=0 -> no veto"}
    admission = {
        "schema": ADMISSION_SCHEMA, "screen": screen,
        "rules": {"orientation": "s_k=prior_sign=+1 (sign embedded in the DSL); no sign estimation or flip; "
                                 "prior_sign 0 -> reject_no_prior; runner sign reported not used",
                  "train_window_ns": [FIT_BEGIN_NS, TRAIN_END_NS],
                  "window_basis": "decision session; statistics over live (non-flat) TRAIN decisions",
                  "min_train_days": V4_MIN_TRAIN_DAYS, "tau_limit": V4_TAU_LIMIT, "veto_t": V4_VETO_T,
                  "veto": "HAC t of mean f_k over live TRAIN decisions < veto_t -> reject_veto", "hac": hac,
                  "rho_limit": V4_RHO_LIMIT, "min_common_days": MIN_COMMON_DAYS,
                  "redundancy": "survivors by (tier, roster order); |pearson rho| over TRAIN days both live > "
                                "rho_limit vs an admitted candidate -> reject_redundant (largest |rho|); "
                                "< min_common_days or undefined rho -> uncorrelated, noted",
                  "tier_order": priors["tier_order"], "sharpe_annualization": ANNUALIZATION,
                  "status_precedence": list(statuses[1:]),
                  "context": CONTEXT_SEMANTICS, "factor": FACTOR_SEMANTICS},
        "inputs": inputs, "window": window,
        "counts": {s: sum(1 for r in rows if r["status"] == s) for s in statuses},
        "admitted": [ids[k] for k in admitted_order],
        "sign_conflicts": [c["id"] for c in candidates if not c["sign_agrees"]],
        "candidates": candidates}
    if v2:
        admission["rules"].update(
            cost_tau_limit=V42_COST_TAU_LIMIT,
            cost_screen="standalone TRAIN tau_k > cost_tau_limit -> reject_turnover_cost (v4.2 R3': cost consistency "
                        "at $1bn; structural, not performance); checked after turnover, before veto")
    files = {OUTPUT_ADMISSION: canonical_bytes(admission),
             OUTPUT_ADMISSION_CSV: admission_csv(candidates, V4_CSV_COLUMNS)}
    admission_sha = hashlib.sha256(files[OUTPUT_ADMISSION]).hexdigest()

    zero_filled = np.where(np.isnan(factors), 0.0, factors)
    weight_rows = []
    for k, cand in enumerate(library):
        sign = prior_signs[k]
        row = {"id": cand["id"], "family": cand["family"], "theme": themes[k], "tier": tiers[k],
               "prior_sign": sign, "sign": sign, "runner_sign": runner_signs[k],
               "status": "fitted" if rows[k]["status"] == "admitted" else rows[k]["status"],
               "dsl_sha256": cand["dsl_sha256"], "cache_payload_sha256": shas[k], "cache_entry": cache_entry[k],
               "tau": taus[k], "tau_over_limit": taus[k] > V4_TAU_LIMIT,
               "flat_decisions": int(np.isnan(factors[k]).sum()), "weight": 0.0}
        if sign == 0:
            row.update(factor_mean=None, factor_sd=None, factor_sharpe_annualized=None)
        else:
            row.update(factor_stats(sign * zero_filled[k]))
            if row["status"] == "fitted" and not row["factor_sd"] > 0:
                row["status"] = "degenerate-zero-variance"
        if aim:  # report the gain on every row; only fitted members use it
            row["aim_gain"] = aims[k]["gain"]  # type: ignore[index]
        weight_rows.append(row)
    active = [k for k in admitted_order if weight_rows[k]["status"] == "fitted"]
    summary = {"status": "complete", "output": str(out), "screen": screen, "composition": args.composition,
               "orientation": "prior", "candidates": len(library), "computed_this_run": computed, "reused": reused,
               "refused_decisions": len(records[0]["context_refused"]), "admitted": len(admitted_order),
               "counts": admission["counts"], "sign_conflicts": admission["sign_conflicts"],
               "admission_sha256": admission_sha}
    if not active:
        publish_directory(out, files)
        summary.update(status="published-without-weights", reason="fit: no admitted candidate with a non-degenerate "
                       "factor series", files={n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())},
                       seconds=round(time.perf_counter() - started, 2))
        return EXIT_NO_WEIGHTS, summary
    if aim:
        weights, theme_table = ew_theme_aim_weights([themes[k] for k in active],
                                                    [aims[k]["gain"] for k in active])  # type: ignore[index]
        composition_text = ("w_k=(g_k/(T*n_theme(k)))/sum_m(g_m/(T*n_theme(m))) over admitted non-degenerate k "
                            "(normalised globally); g_k=theta*sum_{j=0..126}(1-theta)^j*rho_k(j) clipped to [0.05,1], "
                            "rho_k from TRAIN rank autocorrelation; T=themes with >=1 such member; no mean or "
                            "covariance estimation")
        fit_series = ("none (aim-scaled equal theme weights from TRAIN signal-rank second moments); diagnostic uses "
                      "s_k*f over ALL TRAIN scored decisions, flat decisions 0")
    elif v6:
        # (c) reads the standalone tau of each member's admission row: admission.json candidates[].tau (status admitted).
        weights, theme_table, v6_detail = ew_theme_v6_weights([ids[k] for k in active], [themes[k] for k in active],
                                                              [rows[k]["tau"] for k in active])
        composition_text = ("ew-theme-v6: theme'=options_implied->short_interest, low_risk dropped (weight 0); within "
                            "theme' b=1/n, a member with admission tau_k>=0.08 keeps b/3 and the freed mass goes pro rata "
                            "(by b) to the theme's slow members (none slow: weights stay b); w_k=within_k/T, T=themes' "
                            "with >=1 member; missing members' mass stays in the theme per name and day "
                            "(theme_redistribution within-theme-v1, applied by the IC runner); no mean or covariance "
                            "estimation")
        fit_series = ("none (prior-fixed theme weights; TRAIN standalone tau only flags fast sleeves); diagnostic uses "
                      "s_k*f over ALL TRAIN scored decisions, flat decisions 0, without the per-name within-theme "
                      "redistribution")
        if not float(np.sum(weights)) > 0:  # every member sat in a dropped theme: admission table only
            publish_directory(out, files)
            summary.update(status="published-without-weights", reason="fit: ew-theme-v6 leaves no member outside the "
                           "dropped themes", files={n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())},
                           seconds=round(time.perf_counter() - started, 2))
            return EXIT_NO_WEIGHTS, summary
    elif args.composition == composition_rules.STD_RULE_ID:  # v8 R-1: the rule lives in composition_rules.py
        std = composition_rules.ew_theme_std([ids[k] for k in active], [themes[k] for k in active],
                                             [tiers[k] for k in active], error=FitError)
        weights, theme_table, composition_text, fit_series = std.weights, std.theme_table, std.text, std.fit_series
    else:
        weights, theme_table = ew_theme_weights([themes[k] for k in active])
        composition_text = ("w_k=1/(T*n_theme(k)) over admitted non-degenerate k; T=themes with >=1 such member; "
                            "no mean or covariance estimation")
        fit_series = "none (equal theme weights); diagnostic uses s_k*f over ALL TRAIN scored decisions, flat decisions 0"
    for k, w in zip(active, weights):
        weight_rows[k]["weight"] = float(w)
    if v6:  # theme' tables carry their own member lists (merged themes); dropped members are marked
        for k, row in enumerate(weight_rows):
            row["theme_v6"] = V6_MERGED_THEMES.get(themes[k], themes[k])
            if k in active and row["theme_v6"] in V6_DROPPED_THEMES:
                row["status"] = "fitted-theme-dropped-v6"
    else:
        for theme, entry in theme_table.items():
            entry["admitted"] = [ids[k] for k in active if themes[k] == theme]
    matrix = np.vstack([prior_signs[k] * zero_filled[k] for k in active])
    weighted_tau = float(sum(row["weight"] * row["tau"] for row in weight_rows))
    conflicts = [row["id"] for row in weight_rows if row["weight"] > 0 and row["runner_sign"] != row["sign"]]
    document = {
        "schema": WEIGHTS_SCHEMA,
        "library_sha256": args.library_sha256,
        "train_manifest_sha256": args.train_sha256,
        "signs": {row["id"]: row["sign"] for row in weight_rows if row["sign"] != 0},
        "weights": {row["id"]: row["weight"] for row in weight_rows},
        "provenance": {
            "rule": args.composition,
            "composition": composition_text,
            "themes": theme_table, "themes_present": sorted(theme_table),
            "themes_declared": sorted(set(themes)),
            "themes_preregistered": list(V4_THEMES) + [t for t in V7_APPENDED_THEMES if t in themes],
            "screen": screen, "orientation": "prior", "admission_sha256": admission_sha,
            "signs": f"{screen}: s_k=prior_sign=+1 embedded in the DSL; no flips; apply-pinned-signs",
            "prior_metadata_source": priors["source"], "recipe_sha256": priors["recipe_sha256"],
            "fit_series": fit_series,
            "factor": FACTOR_SEMANTICS, "neutralization": CONTEXT_SEMANTICS,
            "turnover": "tau=mean_d(sum_i|q(d)_i-q(d-1)_i|);consecutive-scored-TRAIN-decisions;"
                        "deployment-excluded;no-price-drift",
            "tau_limit": V4_TAU_LIMIT, "tau_flagged": [row["id"] for row in weight_rows if row["tau_over_limit"]],
            "weighted_standalone_turnover": weighted_tau,
            "library_sha256": args.library_sha256, "role_manifest_sha256": args.train_sha256,
            "role_source_sha256": inputs["role_source_sha256"], "orientations_sha256": args.orientations_sha256,
            "orientations_recipe_sha256": inputs["orientations_recipe_sha256"],
            "runner_summary_sha256": args.runner_summary_sha256, "vm_identity": inputs["vm_identity"],
            "fields_manifest_sha256": inputs["fields_manifest_sha256"], "script_sha256": inputs["script_sha256"],
            "semantics_tag": SEMANTICS_TAG, "context_sha256": inputs["context_sha256"], "window": window,
            "neutralization_refused_decisions": records[0]["context_refused"],
            "used_rows_unrefused": records[0]["context_used_rows_unrefused"],
            "fitted_candidates": len(active), "sign_conflicts_weighted": conflicts,
            "blend_in_sample_TRAIN_diagnostic": factor_stats(weights @ matrix),
            "candidates": weight_rows,
        },
    }
    if v2:
        document["provenance"].update(
            cost_tau_limit=V42_COST_TAU_LIMIT,
            cost_rejected=[row["id"] for row in weight_rows if row["status"] == "reject_turnover_cost"])
    if aim:  # ew-theme-v1 bytes carry no aim key
        document["provenance"]["aim"] = aim_provenance(ids, themes, aims, active, weights,  # type: ignore[arg-type]
                                                       decision_sessions)
    if v6:  # ew-theme-v1 / ew-theme-aim-v1 bytes carry neither key and stay schema v1
        document["schema"] = WEIGHTS_SCHEMA_V2
        document["theme_redistribution"] = {
            "rule": V6_REDISTRIBUTION, "composition": V6_RULE_ID,
            "themes": {row["id"]: row["theme_v6"] for row in weight_rows if row["weight"] > 0}}
        document["provenance"]["v6"] = v6_provenance(v6_detail, theme_table, admission_sha, args, inputs,
                                                     priors["recipe_sha256"])
    if args.composition == composition_rules.STD_RULE_ID:  # schema v2, theme_standardise block, provenance.std
        composition_rules.attach_std(document, std)
    files[OUTPUT_WEIGHTS] = canonical_bytes(document)
    require(len(files[OUTPUT_WEIGHTS]) <= METADATA_LIMIT, "output: weights JSON exceeds the runner's 1 MiB bound")
    publish_directory(out, files)
    summary.update(files={n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())},
                   weights_sha256=hashlib.sha256(files[OUTPUT_WEIGHTS]).hexdigest(),
                   nonzero_weights=sum(1 for row in weight_rows if row["weight"] > 0),
                   themes_present=sorted(theme_table), weighted_standalone_turnover=weighted_tau,
                   tau_flagged=document["provenance"]["tau_flagged"], sign_conflicts_weighted=conflicts,
                   seconds=round(time.perf_counter() - started, 2))
    if aim:
        gains = [aims[k]["gain"] for k in active]  # type: ignore[index]
        summary.update(aim_gain_min=min(gains), aim_gain_max=max(gains),
                       aim_theme_weights={t: e["aim_theme_weight"] for t, e in sorted(theme_table.items())})
    if v6:
        summary.update(shrunk_members=v6_detail["shrunk_members"], dropped_members=v6_detail["dropped_members"],
                       theme_redistribution=V6_REDISTRIBUTION)
    return EXIT_OK, summary


def v6_provenance(detail: dict, theme_table: dict, admission_sha: str, args, inputs: dict, recipe_sha: str | None) -> dict:
    """``provenance.v6``: the V6-W rule, its constants, what (a)-(c) did to which member, (d)'s runner-side
    redistribution and every input SHA the weights depend on."""
    return {
        "rule": V6_RULE_ID,
        "preregistration": "v4-prereg.md '## v6 revision' item V6-W (declared 2026-09-27 before any v6 TRAIN read)",
        "dropped_themes": list(V6_DROPPED_THEMES), "merged_themes": dict(V6_MERGED_THEMES),
        "dropped_members": detail["dropped_members"], "merged_members": detail["merged_members"],
        "fast_tau_threshold": V6_FAST_TAU, "fast_tau_test": "tau_k >= fast_tau_threshold",
        "fast_factor": V6_FAST_FACTOR,
        "fast_tau_source": ("admission.json key candidates[].tau of the status=admitted row (this run's admission "
                            "table, admission_sha256 below): standalone daily one-way TRAIN turnover "
                            "mean_d sum_i|q(d)_i-q(d-1)_i| over consecutive scored TRAIN decisions"),
        "shrunk_members": detail["shrunk_members"],
        "shrunk_tau": {i: detail["member_tau"][i] for i in detail["shrunk_members"]},
        "fast_not_shrunk_all_fast_theme": detail["fast_not_shrunk_all_fast_theme"],
        "fast_in_dropped_theme": detail["fast_in_dropped_theme"],
        "member_tau": detail["member_tau"],
        "within_theme_rule": ("b=1/n_theme'; fast (tau>=threshold): b*fast_factor; freed=sum_fast(b-b*fast_factor) "
                              "added to each slow member pro rata by b; a theme' with no slow member keeps b"),
        "theme_rule": "w_k=within_k/T, T=themes' with >=1 member (equal theme weights)",
        "themes": sorted(theme_table),
        "coverage_redistribution": {
            "rule": V6_REDISTRIBUTION, "block": "theme_redistribution (top level)",
            "applied_by": "atx-equity-strategy-ic (strategy_ic_composition.cpp) per name and decision day",
            "formula": ("blend_i=sum_theme W_theme*sum_{k in theme, present at i} w_k*s_k*r_k,i/sum_{k present} w_k; "
                        "W_theme=sum_{k in theme} w_k; present=member with a finite signal on a day ranking >= 2 "
                        "names; a theme with no present member adds nothing")},
        "inputs": {"library_sha256": args.library_sha256, "train_manifest_sha256": args.train_sha256,
                   "recipe_sha256": recipe_sha, "orientations_sha256": args.orientations_sha256,
                   "orientations_recipe_sha256": inputs["orientations_recipe_sha256"],
                   "runner_summary_sha256": args.runner_summary_sha256, "admission_sha256": admission_sha,
                   "role_source_sha256": inputs["role_source_sha256"],
                   "fields_manifest_sha256": inputs["fields_manifest_sha256"], "vm_identity": inputs["vm_identity"],
                   "script_sha256": inputs["script_sha256"], "context_sha256": inputs["context_sha256"]},
    }


def aim_provenance(ids: list[str], themes: list[str], aims: list[dict], active: list[int], weights: np.ndarray,
                   decision_sessions: np.ndarray) -> dict:
    """``provenance.aim``: the R4' constants and, report only, rho/g per candidate, half-sample gains and the
    coverage-effective theme weight. Nothing here feeds back into the weights."""
    train = (decision_sessions >= FIT_BEGIN_NS) & (decision_sessions < TRAIN_END_NS)
    coverage = np.array([aims[k]["coverage"] for k in active], dtype=np.float64)  # members x decisions
    w = np.asarray(weights, dtype=np.float64)
    total = w @ coverage
    ok = train & (total > 0)
    effective = {}
    for theme in sorted({themes[k] for k in active}):
        rows = [n for n, k in enumerate(active) if themes[k] == theme]
        part = w[rows] @ coverage[rows]
        effective[theme] = float(np.mean(part[ok] / total[ok])) if ok.any() else None
    return {
        "theta": AIM_THETA, "lags": list(AIM_LAGS), "max_lag": AIM_MAX_LAG, "gain_min": AIM_GAIN_MIN, "gain_max": 1.0,
        "min_names": AIM_MIN_NAMES, "semantics": AIM_SEMANTICS,
        "gain_formula": "g_k=theta*sum_{j=0..max_lag}(1-theta)^j*rho_k(j);rho linear-interpolated between exact "
                        "lags;NaN lag->0;clip[gain_min,gain_max]",
        "rho_definition": "rho_k(j)=mean over TRAIN decisions d of sum_i z_k(d)_i*z_k(d-j)_i/n_both(d) over names "
                          "finite on both days (n_both>=min_names); z=per-decision standardized centered tied ranks "
                          "over used rows with a finite signal",
        "rho": {ids[k]: a["rho"] for k, a in enumerate(aims)},
        "gain": {ids[k]: a["gain"] for k, a in enumerate(aims)},
        "gain_unclipped": {ids[k]: a["gain_unclipped"] for k, a in enumerate(aims)},
        "gain_half": {ids[k]: a["gain_half"] for k, a in enumerate(aims)},
        "half_split_decision_index": aims[0]["half_split_decision"],
        "gain_half_note": "report only (risk register): first/second half of the TRAIN decisions; never re-tuned",
        "coverage_mean": {ids[k]: (float(np.mean(np.asarray(a["coverage"])[train])) if train.any() else None)
                          for k, a in enumerate(aims)},
        "coverage_effective_theme_weight": effective,
        "coverage_definition": "mean over TRAIN decisions of sum_{k in theme} w_k*c_k(d)/sum_k w_k*c_k(d); "
                               "c_k(d)=used rows with a finite signal_k / used rows; report only",
        "members": [ids[k] for k in active],
    }


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--library-sha256", required=True)
    p.add_argument("--train", type=Path, required=True, help="TRAIN role manifest.json")
    p.add_argument("--train-sha256", required=True)
    p.add_argument("--orientations", type=Path, required=True, help="TRAIN orientations.json from the IC runner")
    p.add_argument("--orientations-sha256", required=True)
    p.add_argument("--runner-summary", type=Path, required=True,
                   help="summary.json of the TRAIN-only IC run that wrote --orientations (candidate cache layout)")
    p.add_argument("--runner-summary-sha256", required=True)
    p.add_argument("--screen", required=True, choices=SCREENS,
                   help="v3-admit-v1 (admission screen + fit on admitted), none (T9: runner signs, all oriented) "
                        "or v4-prior-v1 (prior signs; with --orientation prior --composition ew-theme-v1) or "
                        "v4-prior-v2 (v4-prior-v1 plus reject tau_k > 0.08, v4.2 R3')")
    p.add_argument("--composition", default=RULE_ID, choices=COMPOSITIONS,
                   help="weight fit: mv-shrink-0.9-nonneg-v1 (default) or its netcost variant "
                        "(mu_k minus 0.0018 * tau_k); the screen and signs are unchanged; ew-theme-v1 for v4; "
                        "ew-theme-aim-v1 for v5 (R4': ew-theme scaled by the TRAIN rank-autocorrelation aim gain); "
                        "ew-theme-v6 for v6 V6-W (drop low_risk, options_implied into short_interest, fast-sleeve "
                        "x1/3 at tau >= .08, equal themes, within-theme coverage redistribution in the IC runner)")
    p.add_argument("--orientation", default="train", choices=ORIENTATIONS,
                   help="train (default: runner/screen signs) or prior (v4: s_k=+1 embedded in the DSL)")
    p.add_argument("--recipe", type=Path, default=None,
                   help="v4: library recipe carrying theme/tier/prior_sign per candidate (candidates/lineage)")
    p.add_argument("--recipe-sha256", default=None)
    p.add_argument("--output", type=Path, required=True, help="new output directory (never overwritten)")
    p.add_argument("--work-dir", type=Path, default=None,
                   help="persistent incremental state (context + per-candidate factor records keyed by the work key: "
                        "role, VM identity, screen, DSL SHA and field payload SHAs; aim records for ew-theme-aim-v1); "
                        "stderr reports 'fit: computed K, reused M'")
    p.add_argument("--max-seconds", type=float, default=None,
                   help="soft budget: stop cleanly before a candidate that would overrun it (exit 3, stdout "
                        "{status: incomplete, partial: true}; completed candidates persist, rerun resumes)")
    p.add_argument("--max-new-candidates", type=int, default=None,
                   help="compute at most N missing candidates this run (exit 3 if more remain)")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        code, summary = fit(args, log=lambda line: print(line, file=sys.stderr, flush=True))
    except Incomplete as stop:
        print(json.dumps(stop.summary, sort_keys=True))
        return EXIT_INCOMPLETE
    except FitError as exc:
        print(f"fit_composition_weights: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    print(json.dumps(summary, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
