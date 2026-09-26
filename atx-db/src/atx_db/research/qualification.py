"""Metric qualification ledger (task R4): which standardized metrics qualify.

A qualified metric is a pre-registered hypothesis (an R1a catalog row) whose
standardized cross-sectional value predicts medium-horizon forward returns with
honest, family-corrected significance, a stable sign and holdout confirmation. The
ledger is the input to everything built forward: composites and models use only
qualified features, with weights fit on train/validation only.

Inputs and purity
-----------------
The ledger is a pure function of

* one sealed R3b evaluation run (``research_eval_*`` tables, read by the ONE adapter
  :func:`load_evaluation_results`, which also re-hashes the stored rows against the
  run's seal), and
* the frozen policy ``seeds/research_qualification_policy.json``, identified by the
  sha256 of its canonical JSON content (:func:`policy_sha256`, line-ending
  independent), plus the catalog metadata it annotates (hashed into the manifest).

The same inputs give byte-identical JSON/CSV artifacts and the same ledger sha256;
nothing reads a clock.

Frozen policy (RX7)
-------------------
The policy, and the train/validation/holdout split inside it, were committed with
their hash before any real-data evaluation. A policy file whose declared
``policy_sha256`` differs from its content is refused; a known ``policy_version``
whose content hash differs from its pin in :data:`FROZEN_POLICY_SHA256` is refused,
and the research store refuses to register a second hash under a registered version.
Changing the policy therefore always means a new ``policy_version`` (and a new pin).
Unknown keys in any policy section are refused.

From v3 the policy also pins every gate-feeding R3b ``EvaluationSpec`` value
(``evaluation_spec``: horizons, subperiod bounds, ``min_names``, ``min_bucket_names``,
``nyse_min_names``, ``min_formations``, ``verify_panels``); build the RR4 spec with
:func:`evaluation_spec_kwargs`. A run is qualified only when its sealed spec equals
those values, its frozen split is the policy's split, it was evaluated under this
policy *file* (the R3b CLI records the file's byte hash) and the policy was frozen in
the research store (``research_qualify.py freeze``) before the run was created. Only an
explicit ``allow_post_hoc_policy`` passes the last two, and it stamps the manifest,
every feature row and the generated doc. Versions: v1 and v2 are kept as
``seeds/research_qualification_policy_v1.json`` / ``_v2.json``; v3 is current (same
split; every version predates any real-data result). Superseded or spec-less versions
(v1, v2: any version a committed file ``supersedes``, or without ``evaluation_spec``)
stay loadable but are refused for freeze, qualify and persist (N1); only
:func:`verify_ledger` grades under them, to reproduce a historical ledger. A new
version must pin ``evaluation_spec``. A ledger seals the registration time it saw and
the post-hoc reasons that followed; ``verify_ledger`` reproduces from those (N2).

Gates (controller R4 ruling; first failing tier sets the status)
-----------------------------------------------------------------
All on the primary cell: primary variant (``rank_normal``; rank IC and deciles are
identical for the rank-equivalent variants) x primary horizon (3 months) x basis, on
the selection sample (train + validation formations whose label window ends before
the holdout).

1. ``insufficient_coverage``: the cell is not ``tested``; fewer than 120 IC
   observations; or value coverage of the ranked universe >= 60% on fewer than 90% of
   the selection formations (per-formation ``research_eval_series.coverage``; R2b
   writes NULL for unverified size rows, so size/valuation coverage counts verified
   rows only).
2. ``not_significant``: significance on the rank-IC family only, EWC fixed-b
   (``ic_z`` = normal-equivalent z of the robust p; never Newey-West): HLZ ``|z| >= 3``
   or (family BH ``q <= 0.05`` and ``|z| >= 2``).
3. ``sign_reversed``: significant against the catalog ``expected_sign`` (values are
   sign-oriented, so the hypothesis is a positive IC). A two-sided hypothesis
   (``expected_sign = 0``) qualifies on ``|z|``; its realized sign is then fixed and
   reported, and every later check must keep it.
4. ``unstable``: the holdout IC has the opposite sign; fewer than 2 of the 3
   subperiods have the fixed sign; or the small or the large size bucket has the
   opposite sign (microcap false-discovery guard). Buckets are R3b's point-in-time NYSE
   20/50 slices (``size_bucket``, ``venue_basis='nyse_pit'``) when both have >= 36
   formations; under v2 the rule otherwise gates on R3b's labeled verified-cap tercile
   slices (``size_tercile``, ``cap_terciles``) and the row records ``size_venue_basis``.
5. ``candidate``: holdout ``z`` below 1.5 in the fixed direction (or untestable);
   size-bucket or subperiod evidence missing; under v1 only, the deflated Sharpe
   (``dsr_z > 0``: the EW decile long-short Sharpe beats the expected maximum of the
   whole family, ``horizon_periods = h``). Under v2 it is reported and flagged
   (``dsr_pass``), never gating.
6. Otherwise ``qualified_strict`` on the strict basis with strict labels (v3: exact
   allowlists of identity basis, universe basis and universe scope) and
   ``qualified_reconstructed`` on any other basis. A reconstructed-basis pass is never
   labeled strict (RX1); a strict-basis row whose labels are not strict counts as
   reconstructed evidence.

Decile spreads, monotonicity, net-of-cost spreads, Fama-MacBeth, neutral variants and
the 1/6/12-month horizons are supporting evidence, reported, never gating.

One status per feature: ``unstable`` if one basis qualifies while another is
``sign_reversed`` (symmetric veto), else ``qualified_strict`` if a strict basis
qualifies, else ``qualified_reconstructed`` if another basis qualifies, else the status of the reference research
basis (the reconstructed basis, RX1) when it is testable, else the strict basis status
(``untestable_strict`` when the strict cohort is empty).

Refusals
--------
Unproduced cells, partial family subsets and catalog drift are blockers, never a
smaller family (R3b I1): a run that is not sealed, whose rows do not match its seal or
whose ``spec_json`` does not match its ``spec_sha256``, has no frozen split or another
split, another evaluation spec, is not ``family_complete`` or lacks a basis in its
sealed cells, carries a subset / not-produced / catalog-drift blocker
(``*_feature_catalog_differs_from_committed_catalog``,
``family_catalog_injected_not_the_feature_version_snapshot``), has
``not_produced``/``excluded_by_subset`` cells, a DSR ``n_trials`` other than the whole
family, disagrees with the catalog, or fails the RX7 order above raises
:class:`QualificationRefused` and writes nothing. A ledger built without the catalog
check or without re-hashing the run's seal is a stamped dry run and cannot be persisted.

Outputs
-------
Research-store tables ``research_qualification_*`` (policy registry, ledger manifest,
feature ledger, per-basis evidence), versioned JSON/CSV artifacts and the generated
``docs/research/QUALIFIED_SIGNALS.md``.

Policy v4 (tier-1 v2 node 1.10; ``seeds/research_qualification_policy_v4.json``)
-------------------------------------------------------------------------------
v4 grades pre-registered *waves* (``research.trial_registry``), not whole-catalog R3b
runs; v3 stays the legacy R3b/R4 DuckDB-oracle policy (R-5). :func:`load_policy` returns a
:class:`QualificationPolicyV4` for a file with ``policy_id`` (same freeze mechanism:
content hash declared in the file, pinned in :data:`FROZEN_POLICY_SHA256`, registered by
``research_qualify.py freeze``). Evidence classes come from the catalog (``evidence_class``):

* replication (published sign): one-sided EWC p <= 0.05 in the pre-registered direction
  and gating-family BH q <= 0.10; the investable co-primary cell one-sided p <= 0.05; the
  small and large size buckets keep the sign;
* discovery (conjecture, two-sided row, composite): HLZ |z| >= 3 (in the pre-registered
  direction for a signed conjecture), deflated Sharpe probability >= 0.95 with ``n_trials``
  from the trial registry, gating-family BH q <= 0.05, investable one-sided p <= 0.05;
* holdout (final labels only, opened once per wave through the registry): the sign holds
  and the Welch shrink test of holdout vs selection IC is not significant (one-sided
  p >= 0.05).

The gating family is the primary cells (``rank_normal`` x 3 months) of the wave's gating
hypotheses (registered, research-eligible, not reported-only: the ruling C-24
equal-weight twins are reported only), each at its class p-value, plus every earlier
wave's gating hypotheses at p = 1. Coverage is measured against the catalog population on
the feature's history-eligible selection formations. :func:`evidence_from_evaluation`
reads the R3b result frames of a wave run (refusing a spec that differs from the policy's
pinned inputs, an unregistered feature, a catalog sign that differs), :func:`grade_v4`
grades :class:`FeatureEvidence` rows (the power study calls it directly) and
:func:`grade_wave_v4` ties both to the registry (``n_trials``, earlier waves, the holdout
state) and returns a sealed :class:`V4Ledger`. A provisional-label grade never reads the
holdout: it stops at ``selection_pass``.
"""

from __future__ import annotations

import csv
import datetime as dt
import hashlib
import io
import json
import math
import re
from collections.abc import Iterable, Mapping, Sequence
from contextlib import suppress
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import duckdb
import numpy as np
import pandas as pd

from . import stats
from .catalog import CONTROL_CLASSES
from .evaluation import BASES, FrozenSplit, load_frozen_split, results_digest

#: Bumped whenever grading semantics change (M5); the manifest also records the code sha.
QUALIFICATION_VERSION = "research-qualification-v2"
QUALIFICATION_SCHEMA_VERSION = 2
#: The current policy (the RR4 ``--split-file``); superseded versions stay in the seeds as history.
POLICY_PATH = Path(__file__).resolve().parents[1] / "seeds" / "research_qualification_policy.json"
POLICY_HISTORY_PATHS: Mapping[str, Path] = {
    "r4-qualification-v1": POLICY_PATH.with_name("research_qualification_policy_v1.json"),
    "r4-qualification-v2": POLICY_PATH.with_name("research_qualification_policy_v2.json"),
}
#: Versions committed before ``evaluation_spec`` existed; any other version must pin it.
_SPECLESS_HISTORY = frozenset({"r4-qualification-v1", "r4-qualification-v2"})
#: Every committed policy version and its canonical content hash. Editing a frozen
#: policy is refused; a changed policy needs a new version and a new pin here.
FROZEN_POLICY_SHA256: Mapping[str, str] = {
    "r4-qualification-v1": "d33ac7044a5b67d656f37a4f9c0ef61afacc403b4e4ff65ae6eec595452f4f22",
    "r4-qualification-v2": "d517b6c8372ae8ee18d09418be5f400d6b797f1015098c37f0aab1b2813093c2",
    "r4-qualification-v3": "3c26bfb68db4d98c3a25c20ac050cbb45bced1fabbe4bde07930ab18d48d7042",
    #: Frozen 2026-09-26 (tier-1 v2 node 1.10) before any tier-1 v2 wave read a forward return (R-6).
    "r4-qualification-v4": "776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a",
}
#: Policy v4+ files (``policy_id`` schema): graded per registered wave, see the v4 section.
POLICY_V4_PATH = POLICY_PATH.with_name("research_qualification_policy_v4.json")
POLICY_V4_PATHS: Mapping[str, Path] = {"r4-qualification-v4": POLICY_V4_PATH}
V4_POLICY_IDS = frozenset(POLICY_V4_PATHS)

QUALIFIED_STRICT = "qualified_strict"
QUALIFIED_RECONSTRUCTED = "qualified_reconstructed"
CANDIDATE = "candidate"
NOT_SIGNIFICANT = "not_significant"
SIGN_REVERSED = "sign_reversed"
UNSTABLE = "unstable"
INSUFFICIENT_COVERAGE = "insufficient_coverage"
UNTESTABLE_STRICT = "untestable_strict"
STATUSES = (QUALIFIED_STRICT, QUALIFIED_RECONSTRUCTED, CANDIDATE, NOT_SIGNIFICANT, SIGN_REVERSED, UNSTABLE,
            INSUFFICIENT_COVERAGE, UNTESTABLE_STRICT)
QUALIFIED_STATUSES = frozenset({QUALIFIED_STRICT, QUALIFIED_RECONSTRUCTED})
#: The research basis whose outcome a non-qualified feature reports (RX1).
REFERENCE_BASIS = "reconstructed"

#: R3b cell statuses (the adapter's contract; the harness owns the vocabulary).
_CELL_TESTED = "tested"
_CELL_UNTESTABLE = "untestable_strict"
_CELL_MISSING = ("not_produced", "excluded_by_subset")
#: Run blockers that make a run unqualifiable (exact, or ``{basis}_<suffix>:N``): subsets,
#: unproduced cells and any catalog drift (a family built from another catalog snapshot).
_REFUSING_BLOCKERS = frozenset({"partial_family_subset", "family_not_catalog_anchored",
                                "no_frozen_split_selection_uses_all_formations",
                                "family_catalog_injected_not_the_feature_version_snapshot"})
_REFUSING_BLOCKER_SUFFIXES = ("_catalog_cells_not_produced", "_features_outside_catalog", "_features_absent",
                              "_feature_catalog_differs_from_committed_catalog")
#: Refusals that only an explicit post-hoc override may pass (RX7: policy frozen before the run).
POST_HOC_REASONS = ("evaluation_policy_file_differs", "policy_not_registered_in_store",
                    "policy_registered_after_evaluation")


def qualification_code_sha256() -> str:
    """EOL-normalized digest of this module (recorded in every ledger manifest, M5)."""
    data = Path(__file__).read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


class QualificationError(ValueError):
    """Qualification inputs violate the contract (policy, results, store)."""


class PolicyError(QualificationError):
    """A policy that is malformed, not frozen, or changed under a frozen version."""


class QualificationRefused(QualificationError):
    """The evaluation run cannot be qualified; ``reasons`` lists every blocker."""

    def __init__(self, reasons: Sequence[str]) -> None:
        self.reasons = tuple(reasons)
        super().__init__("qualification refused: " + "; ".join(self.reasons))


# ---------------------------------------------------------------------------
# Canonical encoding
# ---------------------------------------------------------------------------

def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _f(value: Any) -> float | None:
    """A finite float or None (NaN, NULL, pd.NA and infinities are None)."""
    if value is None or value is pd.NA:
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _i(value: Any) -> int | None:
    number = _f(value)
    return None if number is None else int(number)


def _b(value: Any) -> bool | None:
    if value is None or value is pd.NA or (isinstance(value, float) and math.isnan(value)):
        return None
    return bool(value)


def _s(value: Any) -> str | None:
    if value is None or value is pd.NA or (isinstance(value, float) and math.isnan(value)):
        return None
    return str(value)


def _signum(value: float | None) -> int:
    if value is None or value == 0:
        return 0
    return 1 if value > 0 else -1


def _g(value: float | None) -> str:
    return "nan" if value is None else f"{value:.4g}"


# ---------------------------------------------------------------------------
# Frozen policy
# ---------------------------------------------------------------------------

_POLICY_KEYS = frozenset({"policy_version", "title", "frozen_on", "authority", "split", "primary", "significance",
                          "deflated_sharpe", "coverage", "holdout", "subperiods", "size_buckets", "strict_evidence",
                          "reported", "statuses", "policy_sha256"})
_POLICY_OPTIONAL_KEYS = frozenset({"supersedes", "evaluation_spec"})
#: Exact keys per section (required, optional): an unknown key is refused (M4), so a policy
#: can never look stricter than the code it runs.
_SECTION_KEYS: Mapping[str, tuple[frozenset[str], frozenset[str]]] = {
    "primary": (frozenset({"horizon_months", "variant", "reported_horizons_months"}), frozenset({"rationale"})),
    "significance": (frozenset({"hlz_min_abs_z", "bh_max_q", "bh_min_abs_z"}), frozenset({"family", "inference"})),
    "deflated_sharpe": (frozenset({"min_z", "n_trials", "series"}), frozenset({"gating", "meaning"})),
    "coverage": (frozenset({"min_value_coverage", "min_formation_share", "min_observations"}), frozenset()),
    "holdout": (frozenset({"slice_name", "min_signed_z"}), frozenset()),
    "subperiods": (frozenset({"names", "min_same_sign"}), frozenset()),
    "size_buckets": (frozenset({"slice_kind", "venue_basis", "buckets", "min_formations"}),
                     frozenset({"fallback_slice_kind", "fallback_venue_basis", "min_venue_pit_share", "meaning"})),
    "strict_evidence": (frozenset({"basis"}),
                        frozenset({"non_strict_label_tokens", "identity_bases", "universe_bases", "universe_scopes"})),
    "reported": (frozenset({"neutral_variants", "neutral_min_abs_z", "decile_monotonicity_min", "net_cost_bps",
                            "psr_benchmark_sharpe"}), frozenset()),
    "evaluation_spec": (frozenset({"horizons_months", "subperiods", "min_names", "min_bucket_names",
                                   "nyse_min_names", "min_formations", "verify_panels"}), frozenset({"meaning"})),
}
#: The pinned R3b EvaluationSpec fields (v3+), compared with the run's sealed spec_json.
EVALUATION_SPEC_FIELDS = ("horizons_months", "subperiods", "min_names", "min_bucket_names", "nyse_min_names",
                          "min_formations", "verify_panels")


@dataclass(frozen=True)
class QualificationPolicy:
    """A parsed, hash-checked policy (see the module docstring for the gates)."""

    version: str
    sha256: str
    content: Mapping[str, Any]
    split: FrozenSplit
    primary_horizon: int
    primary_variant: str
    reported_horizons: tuple[int, ...]
    hlz_min_abs_z: float
    bh_max_q: float
    bh_min_abs_z: float
    dsr_min_z: float
    min_value_coverage: float
    min_formation_share: float
    min_observations: int
    holdout_slice: str
    holdout_min_signed_z: float
    subperiods: tuple[str, ...]
    subperiod_min_same_sign: int
    size_slice_kind: str
    size_venue_basis: str
    size_buckets: tuple[str, ...]
    size_min_formations: int
    strict_basis: str
    #: v1/v2: a token denylist; v3: exact allowlists of strict labels (M1).
    non_strict_tokens: tuple[str, ...]
    strict_identity_bases: tuple[str, ...] | None
    strict_universe_bases: tuple[str, ...] | None
    strict_universe_scopes: tuple[str, ...] | None
    neutral_variants: tuple[str, ...]
    neutral_min_abs_z: float
    monotonicity_min: float
    net_cost_bps: int
    psr_benchmark: float
    #: v1: the deflated Sharpe gates (``candidate``); v2: reported and flagged (``dsr_pass``) only.
    dsr_gating: bool
    #: v2: the size-bucket rule falls back to these labeled slices where NYSE-PIT buckets are too thin.
    size_fallback_kind: str | None
    size_fallback_venue: str | None
    #: v3: NYSE-PIT buckets count only with this mean point-in-time venue share (M7).
    size_min_venue_pit_share: float | None
    #: v3: the pinned R3b EvaluationSpec values (``spec_payload`` form); None before v3.
    evaluation_spec: Mapping[str, Any] | None
    #: sha256 of the policy file's bytes with LF and with CRLF line endings (what the
    #: R3b CLI records as the run's ``policy_sha256``); empty for a mapping.
    file_sha256s: frozenset[str] = field(default_factory=frozenset)
    #: sha256 of the file's bytes as read (what to pass as the run's ``policy_sha256``).
    file_sha256: str | None = None


def policy_sha256(content: Mapping[str, Any]) -> str:
    """Canonical content hash of a policy (its own ``policy_sha256`` excluded)."""
    return _sha(_canonical({key: value for key, value in content.items() if key != "policy_sha256"}))


def policy_file_sha256s(path: Path | str) -> frozenset[str]:
    """Byte hashes of a policy file under LF and CRLF line endings (checkout-independent)."""
    data = Path(path).read_bytes().replace(b"\r\n", b"\n")
    return frozenset({hashlib.sha256(data).hexdigest(), hashlib.sha256(data.replace(b"\n", b"\r\n")).hexdigest()})


def _section(content: Mapping[str, Any], name: str) -> Mapping[str, Any]:
    section = content.get(name)
    if not isinstance(section, Mapping):
        raise PolicyError(f"policy section {name!r} must be an object")
    required, optional = _SECTION_KEYS[name]
    if not required <= set(section) <= required | optional:
        raise PolicyError(f"policy section {name!r}: missing {sorted(required - set(section))}, "
                          f"unknown {sorted(set(section) - required - optional)}")
    return section


def _evaluation_spec(section: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize the pinned evaluation spec to R3b's ``spec_payload`` form."""
    horizons = section["horizons_months"]
    if not isinstance(horizons, list) or sorted(set(horizons)) != horizons or \
            any(isinstance(h, bool) or not isinstance(h, int) for h in horizons):
        raise PolicyError("evaluation_spec.horizons_months must be ascending distinct integers")
    subperiods = []
    for item in section["subperiods"]:
        if not isinstance(item, list) or len(item) != 3 or not all(isinstance(v, str) for v in item):
            raise PolicyError("evaluation_spec.subperiods must be [name, start, end] triples")
        try:
            start, end = dt.date.fromisoformat(item[1]), dt.date.fromisoformat(item[2])
        except ValueError as error:
            raise PolicyError(f"evaluation_spec.subperiods {item[0]}: {error}") from error
        if start > end:
            raise PolicyError(f"evaluation_spec.subperiods {item[0]} starts after it ends")
        subperiods.append([item[0], start.isoformat(), end.isoformat()])
    if not isinstance(section["verify_panels"], bool):
        raise PolicyError("evaluation_spec.verify_panels must be a boolean")
    return {"horizons_months": list(horizons), "subperiods": subperiods,
            "min_names": _count(section, "min_names", 3, 100_000),
            "min_bucket_names": _count(section, "min_bucket_names", 5, 100_000),
            "nyse_min_names": _count(section, "nyse_min_names", 1, 100_000),
            "min_formations": _count(section, "min_formations", 2, 10_000),
            "verify_panels": section["verify_panels"]}


def _number(section: Mapping[str, Any], key: str, low: float, high: float) -> float:
    value = section[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not low <= float(value) <= high:
        raise PolicyError(f"policy value {key!r} must be a number in [{low}, {high}]")
    return float(value)


def _count(section: Mapping[str, Any], key: str, low: int, high: int) -> int:
    value = section[key]
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise PolicyError(f"policy value {key!r} must be an integer in [{low}, {high}]")
    return value


def _names(section: Mapping[str, Any], key: str) -> tuple[str, ...]:
    value = section[key]
    if not isinstance(value, list) or not value or any(not isinstance(v, str) or not v for v in value) \
            or len(set(value)) != len(value):
        raise PolicyError(f"policy value {key!r} must be a list of distinct names")
    return tuple(value)


def load_policy(source: Path | str | Mapping[str, Any] = POLICY_PATH, *,
                allow_unpinned: bool = False) -> Any:
    """Load and verify a frozen policy (:class:`QualificationPolicy`, or
    :class:`QualificationPolicyV4` for a v4 ``policy_id`` file).

    Refuses a declared ``policy_sha256`` that differs from the content hash (an edited
    policy), a pinned version whose content hash differs from its pin (a frozen policy
    changed in place) and, unless ``allow_unpinned``, a version that is not pinned.
    """
    file_sha: str | None = None
    if isinstance(source, Mapping):
        content: Any = dict(source)
        file_hashes: frozenset[str] = frozenset()
    else:
        path = Path(source)
        raw = path.read_bytes()
        content = json.loads(raw.decode("utf-8"))
        file_hashes, file_sha = policy_file_sha256s(path), hashlib.sha256(raw).hexdigest()
    if not isinstance(content, dict):
        raise PolicyError("policy must be a JSON object")
    if "policy_id" in content:
        return _load_policy_v4(content, file_hashes, file_sha, allow_unpinned=allow_unpinned)
    if not _POLICY_KEYS <= set(content) <= _POLICY_KEYS | _POLICY_OPTIONAL_KEYS:
        raise PolicyError(f"policy keys must be {sorted(_POLICY_KEYS)} (+ optional {sorted(_POLICY_OPTIONAL_KEYS)}); "
                          f"unknown {sorted(set(content) - _POLICY_KEYS - _POLICY_OPTIONAL_KEYS)}, "
                          f"missing {sorted(_POLICY_KEYS - set(content))}")
    version = content["policy_version"]
    if not isinstance(version, str) or not 0 < len(version) <= 64:
        raise PolicyError("policy_version must be a short string")
    actual = policy_sha256(content)
    if content["policy_sha256"] != actual:
        raise PolicyError(f"policy {version} is not frozen: declared policy_sha256 {content['policy_sha256']!r} "
                          f"!= content sha256 {actual}")
    pinned = FROZEN_POLICY_SHA256.get(version)
    if pinned is not None and pinned != actual:
        raise PolicyError(f"policy {version} is frozen with sha256 {pinned}; its content changed ({actual}). "
                          "A changed policy needs a new policy_version")
    if pinned is None and not allow_unpinned:
        raise PolicyError(f"policy {version} is not pinned in FROZEN_POLICY_SHA256")
    if list(content["statuses"]) != list(STATUSES):
        raise PolicyError(f"policy statuses must be exactly {list(STATUSES)}")
    try:
        split = load_frozen_split(content["split"])
    except ValueError as error:
        raise PolicyError(f"policy split: {error}") from error
    primary = _section(content, "primary")
    significance = _section(content, "significance")
    dsr = _section(content, "deflated_sharpe")
    coverage = _section(content, "coverage")
    holdout = _section(content, "holdout")
    subperiods = _section(content, "subperiods")
    size = _section(content, "size_buckets")
    strict = _section(content, "strict_evidence")
    reported = _section(content, "reported")
    if "evaluation_spec" not in content and version not in _SPECLESS_HISTORY:
        raise PolicyError(f"policy {version} must pin an evaluation_spec (only v1/v2 history predate it)")
    evaluation = _evaluation_spec(_section(content, "evaluation_spec")) if "evaluation_spec" in content else None
    if dsr["n_trials"] != "whole_family" or dsr["series"] != "ls_ew10":
        raise PolicyError("deflated_sharpe must use the whole family and the ls_ew10 series (R3b)")
    if significance.get("family", "rank_ic") != "rank_ic" or \
            significance.get("inference", "ewc_fixed_b_z_equivalent") != "ewc_fixed_b_z_equivalent":
        raise PolicyError("significance is the rank-IC family under EWC fixed-b inference (the only one implemented)")
    allowlists = [key for key in ("identity_bases", "universe_bases", "universe_scopes") if key in strict]
    if ("non_strict_label_tokens" in strict) == bool(allowlists) or (allowlists and len(allowlists) != 3):
        raise PolicyError("strict_evidence needs either non_strict_label_tokens or all three strict allowlists")
    venue_floor = _number(size, "min_venue_pit_share", 0.0, 1.0) if "min_venue_pit_share" in size else None
    horizon = _count(primary, "horizon_months", 1, 12)
    reported_horizons = tuple(sorted(_count({"h": h}, "h", 1, 12) for h in primary["reported_horizons_months"]))
    net_bps = _count(reported, "net_cost_bps", 10, 50)
    if net_bps not in (10, 25, 50):
        raise PolicyError("reported.net_cost_bps must be one of the R3b costs 10/25/50")
    subperiod_names = _names(subperiods, "names")
    if evaluation is not None:
        pinned_names = [name for name, _, _ in evaluation["subperiods"]]
        if not set(subperiod_names) <= set(pinned_names) or horizon not in evaluation["horizons_months"]:
            raise PolicyError("subperiod names and the primary horizon must be pinned in evaluation_spec")
    gating = dsr.get("gating", True)
    fallback_kind, fallback_venue = size.get("fallback_slice_kind"), size.get("fallback_venue_basis")
    if not isinstance(gating, bool) or (fallback_kind is None) != (fallback_venue is None) \
            or any(v is not None and (not isinstance(v, str) or not v) for v in (fallback_kind, fallback_venue)):
        raise PolicyError("deflated_sharpe.gating must be a boolean; size fallback kind and venue come together")
    return QualificationPolicy(
        version=version, sha256=actual, content=content, split=split,
        primary_horizon=horizon, primary_variant=str(primary["variant"]),
        reported_horizons=reported_horizons,
        hlz_min_abs_z=_number(significance, "hlz_min_abs_z", 0.0, 10.0),
        bh_max_q=_number(significance, "bh_max_q", 0.0, 1.0),
        bh_min_abs_z=_number(significance, "bh_min_abs_z", 0.0, 10.0),
        dsr_min_z=_number(dsr, "min_z", -10.0, 10.0),
        min_value_coverage=_number(coverage, "min_value_coverage", 0.0, 1.0),
        min_formation_share=_number(coverage, "min_formation_share", 0.0, 1.0),
        min_observations=_count(coverage, "min_observations", 1, 10_000),
        holdout_slice=str(holdout["slice_name"]),
        holdout_min_signed_z=_number(holdout, "min_signed_z", 0.0, 10.0),
        subperiods=subperiod_names,
        subperiod_min_same_sign=_count(subperiods, "min_same_sign", 1, len(subperiod_names)),
        size_slice_kind=str(size["slice_kind"]), size_venue_basis=str(size["venue_basis"]),
        size_buckets=_names(size, "buckets"), size_min_formations=_count(size, "min_formations", 1, 10_000),
        strict_basis=str(strict["basis"]),
        non_strict_tokens=_names(strict, "non_strict_label_tokens") if "non_strict_label_tokens" in strict else (),
        strict_identity_bases=_names(strict, "identity_bases") if allowlists else None,
        strict_universe_bases=_names(strict, "universe_bases") if allowlists else None,
        strict_universe_scopes=_names(strict, "universe_scopes") if allowlists else None,
        neutral_variants=_names(reported, "neutral_variants"),
        neutral_min_abs_z=_number(reported, "neutral_min_abs_z", 0.0, 10.0),
        monotonicity_min=_number(reported, "decile_monotonicity_min", -1.0, 1.0),
        net_cost_bps=net_bps, psr_benchmark=_number(reported, "psr_benchmark_sharpe", -10.0, 10.0),
        dsr_gating=gating, size_fallback_kind=fallback_kind, size_fallback_venue=fallback_venue,
        size_min_venue_pit_share=venue_floor, evaluation_spec=evaluation,
        file_sha256s=file_hashes, file_sha256=file_sha,
    )


@lru_cache(maxsize=1)
def superseded_policy_versions() -> frozenset[str]:
    """Versions a committed policy file ``supersedes`` (history: loadable, never used to grade)."""
    names = set()
    for path in (POLICY_PATH, *POLICY_HISTORY_PATHS.values()):
        superseded = json.loads(path.read_bytes().decode("utf-8")).get("supersedes")
        if isinstance(superseded, str):
            names.add(superseded)
    return frozenset(names)


def current_policy_problem(policy: Any) -> str | None:
    """Why a policy may not freeze, grade or persist (N1): superseded, or pins no evaluation spec."""
    if policy.version in superseded_policy_versions():
        return f"policy_superseded:{policy.version}"
    if isinstance(policy, QualificationPolicyV4):
        return None  # v4 pins its evaluation inputs (``evaluation_inputs``)
    if policy.evaluation_spec is None:
        return f"policy_pins_no_evaluation_spec:{policy.version}"
    return None


def evaluation_spec_kwargs(policy: QualificationPolicy) -> dict[str, Any]:
    """The R3b ``EvaluationSpec`` keyword arguments this policy pins (split, subperiods,
    thresholds, ``verify_panels``) plus ``policy_sha256`` = the policy file's byte hash.

    RR4 must build its spec from these (``EvaluationSpec(run_id=..., feature_versions=...,
    label_cutoff=..., **evaluation_spec_kwargs(policy))``); any other value is refused here.
    """
    if policy.evaluation_spec is None or policy.file_sha256 is None:
        raise PolicyError(f"policy {policy.version} does not pin an evaluation spec, or was not loaded from a file")
    spec = policy.evaluation_spec
    return {"split": policy.split, "horizons_months": tuple(spec["horizons_months"]),
            "subperiods": tuple((name, dt.date.fromisoformat(start), dt.date.fromisoformat(end))
                                for name, start, end in spec["subperiods"]),
            "min_names": spec["min_names"], "min_bucket_names": spec["min_bucket_names"],
            "nyse_min_names": spec["nyse_min_names"], "min_formations": spec["min_formations"],
            "verify_panels": spec["verify_panels"], "policy_sha256": policy.file_sha256}


# ---------------------------------------------------------------------------
# R3b results adapter (the ONLY reader of research_eval_* tables)
# ---------------------------------------------------------------------------

_CELL_KEYS = ("basis", "feature_id", "variant", "horizon_months")
#: Cell columns R4 reads (R3b result contract ``research-monthly-evaluation-v2``, fix 1 = 4d122b00).
CELL_INPUT_COLUMNS = (
    *_CELL_KEYS, "status", "status_reason", "sample", "anomaly_class", "hypothesis_family", "expected_sign",
    "universe_scope", "identity_basis", "universe_basis",
    "classification_basis", "availability_basis", "label_basis", "formations_usable", "stitched_observed",
    "stitched_policy", "mean_names", "mean_coverage", "min_coverage", "ic_mean", "ic_n", "ic_robust_p",
    "ic_robust_df", "ic_z", "ic_nw_t", "ic_boot_low", "ic_boot_high", "ls_ew10_mean", "ls_ew10_z", "ls_vw10_mean",
    "ls_vw10_z", "ls_nyse_ew10_mean", "ls_nyse_ew10_z", "mono_ew10", "fm_z", "fmc_z", "rank_autocorr_1m",
    "top_turnover_h", "bottom_turnover_h", "net10_mean", "net25_mean", "net50_mean", "family_member", "bh_q",
    "holm_p", "dsr_n_trials", "dsr_sharpe_variance", "sharpe", "sharpe_skew", "sharpe_kurt", "sharpe_n",
)
SLICE_INPUT_COLUMNS = (*_CELL_KEYS, "slice_kind", "slice_name", "formations", "ic_mean", "ic_z", "venue_basis",
                       "venue_pit_share")
SERIES_INPUT_COLUMNS = ("basis", "feature_id", "formation_date", "in_selection", "coverage")


@dataclass(frozen=True)
class EvaluationResults:
    """What R4 reads from one R3b evaluation run (small frames only)."""

    run_id: str
    status: str
    results_sha256: str | None
    #: The digest of the stored rows re-hashed now; None when not verified.
    stored_rows_sha256: str | None
    spec: Mapping[str, Any]
    family: Mapping[str, Any]
    blockers: tuple[str, ...]
    family_complete: bool
    #: Every cell of the run (``CELL_INPUT_COLUMNS``).
    cells: pd.DataFrame
    #: Slices of the primary variant (every horizon): split, subperiod, size buckets.
    slices: pd.DataFrame
    #: Per-formation coverage of the primary variant at the primary horizon.
    coverage: pd.DataFrame
    #: The run row's ``created_at`` (ISO, naive UTC): RX7 ordering against the policy freeze.
    created_at: str | None = None
    #: sha256(spec_json) == the run row's spec_sha256 (M6); None when not checked.
    spec_sha256_ok: bool | None = None


def _table_columns(con: duckdb.DuckDBPyConnection, table: str) -> set[str]:
    rows = con.execute("SELECT column_name FROM duckdb_columns() WHERE database_name = current_database() "
                       "AND schema_name = 'main' AND table_name = ?", [table]).fetchall()
    return {str(row[0]) for row in rows}


def _require(con: duckdb.DuckDBPyConnection, table: str, columns: Sequence[str]) -> set[str]:
    present = _table_columns(con, table)
    if not present:
        raise QualificationError(f"R3b result contract: table {table} is absent (no evaluation run in this store)")
    missing = [name for name in columns if name not in present]
    if missing:
        raise QualificationError(f"R3b result contract: {table} lacks columns {missing}")
    return present


def load_evaluation_results(con: duckdb.DuckDBPyConnection, run_id: str, policy: QualificationPolicy, *,
                            verify_seal: bool = True) -> EvaluationResults:
    """Read one R3b run for qualification (bounded: cells, primary slices, primary coverage).

    ``verify_seal`` re-hashes every stored result row (:func:`evaluation.results_digest`)
    so a ledger is never built from rows that differ from the run's sealed digest.
    """
    _require(con, "research_eval_runs", ("run_id", "status", "spec_json", "spec_sha256", "results_sha256",
                                         "family_json", "blockers_json", "family_complete", "created_at"))
    row = con.execute("SELECT status, spec_json, results_sha256, family_json, blockers_json, family_complete, "
                      "spec_sha256, created_at FROM research_eval_runs WHERE run_id = ?", [run_id]).fetchone()
    if row is None:
        raise QualificationError(f"evaluation run {run_id!r} is absent")
    _require(con, "research_eval_cells", CELL_INPUT_COLUMNS)
    _require(con, "research_eval_slices", SLICE_INPUT_COLUMNS)
    _require(con, "research_eval_series", (*SERIES_INPUT_COLUMNS, "variant", "horizon_months"))
    stored = results_digest(con, run_id)[0] if verify_seal else None
    cells = con.execute(f"SELECT {', '.join(CELL_INPUT_COLUMNS)} FROM research_eval_cells WHERE run_id = ? "
                        f"ORDER BY {', '.join(_CELL_KEYS)}", [run_id]).df()
    slices = con.execute(f"""
        SELECT {', '.join(SLICE_INPUT_COLUMNS)} FROM research_eval_slices
        WHERE run_id = ? AND variant = ?
        ORDER BY basis, feature_id, horizon_months, slice_kind, slice_name
    """, [run_id, policy.primary_variant]).df()
    coverage = con.execute(f"""
        SELECT {', '.join(SERIES_INPUT_COLUMNS)} FROM research_eval_series
        WHERE run_id = ? AND variant = ? AND horizon_months = ?
        ORDER BY basis, feature_id, formation_date
    """, [run_id, policy.primary_variant, policy.primary_horizon]).df()
    return EvaluationResults(
        run_id=run_id, status=str(row[0]), results_sha256=_s(row[2]), stored_rows_sha256=stored,
        spec=json.loads(row[1]) if row[1] else {}, family=json.loads(row[3]) if row[3] else {},
        blockers=tuple(json.loads(row[4])) if row[4] else (), family_complete=bool(row[5]),
        cells=cells, slices=slices, coverage=coverage,
        created_at=None if row[7] is None else pd.Timestamp(row[7]).isoformat(),
        spec_sha256_ok=bool(row[1]) and _sha(str(row[1])) == row[6])


# ---------------------------------------------------------------------------
# Refusals (unqualifiable runs)
# ---------------------------------------------------------------------------

def _catalog_rows(catalog: Iterable[Any] | None) -> dict[str, Any] | None:
    if catalog is None:
        return None
    rows: dict[str, Any] = {}
    for entry in catalog:
        if getattr(entry, "is_research_eligible", True):
            rows[str(entry.feature_id)] = entry
    return rows


def _catalog_digest(rows: Mapping[str, Any] | None) -> str | None:
    if rows is None:
        return None
    return _sha(_canonical([[fid, int(e.expected_sign), str(e.anomaly_class), _s(getattr(e, "hypothesis_family", None)),
                             _s(getattr(e, "prior_evidence", None)), _s(getattr(e, "admission", None)),
                             list(getattr(e, "caveat_codes", ()) or ())]
                            for fid, e in sorted(rows.items())]))


def _post_hoc_reasons(results: EvaluationResults, policy: QualificationPolicy,
                      policy_registered_at: str | None) -> set[str]:
    """RX7: the run must have been evaluated under this frozen policy file, frozen first."""
    reasons: set[str] = set()
    if results.spec.get("policy_sha256") not in policy.file_sha256s:  # empty for an in-memory policy
        reasons.add("evaluation_policy_file_differs")
    if policy_registered_at is None:
        reasons.add("policy_not_registered_in_store")
    elif results.created_at is None or \
            dt.datetime.fromisoformat(policy_registered_at) > dt.datetime.fromisoformat(results.created_at):
        reasons.add("policy_registered_after_evaluation")
    return reasons


def refusal_reasons(results: EvaluationResults, policy: QualificationPolicy,
                    catalog: Iterable[Any] | None = None, *, policy_registered_at: str | None = None,
                    historical: bool = False) -> list[str]:
    """Every reason the run cannot be qualified (empty: qualifiable). Sorted, deterministic.

    ``policy_registered_at`` is the research store's freeze time of this policy version
    (ISO, naive UTC); the post-hoc reasons (:data:`POST_HOC_REASONS`) need it. A superseded
    or spec-less policy is refused (N1) unless ``historical`` (:func:`verify_ledger` only).
    """
    reasons: set[str] = _post_hoc_reasons(results, policy, policy_registered_at)
    problem = None if historical else current_policy_problem(policy)
    if problem is not None:
        reasons.add(problem)
    if results.status != "complete":
        reasons.add(f"evaluation_run_not_sealed:{results.status}")
    if results.stored_rows_sha256 is not None and results.stored_rows_sha256 != results.results_sha256:
        reasons.add("evaluation_rows_do_not_match_seal")
    if results.spec_sha256_ok is False:
        reasons.add("run_spec_json_does_not_match_its_spec_sha256")
    if policy.evaluation_spec is not None:
        for name in EVALUATION_SPEC_FIELDS:
            run_value = results.spec.get(name)
            if isinstance(run_value, list):
                run_value = [list(item) if isinstance(item, (list, tuple)) else item for item in run_value]
            if run_value != policy.evaluation_spec[name]:
                reasons.add(f"evaluation_spec_differs:{name}")
    split = results.spec.get("split")
    if not isinstance(split, Mapping):
        reasons.add("no_frozen_split")
    elif split.get("sha256") != policy.split.sha256:
        reasons.add(f"split_differs_from_policy:{split.get('sha256')}")
    if not results.family_complete:
        reasons.add("family_not_complete")
    for blocker in results.blockers:
        name = blocker.split(":", 1)[0]
        if name in _REFUSING_BLOCKERS or name.endswith(_REFUSING_BLOCKER_SUFFIXES):
            reasons.add(f"run_blocker:{blocker}")
    if policy.primary_horizon not in list(results.spec.get("horizons_months") or ()):
        reasons.add(f"primary_horizon_not_evaluated:{policy.primary_horizon}")
    run_subperiods = {str(item[0]) for item in results.spec.get("subperiods") or ()}
    if not set(policy.subperiods) <= run_subperiods:
        reasons.add("policy_subperiods_not_evaluated:" + ",".join(sorted(set(policy.subperiods) - run_subperiods)))
    cells = results.cells
    if not len(cells):
        reasons.add("evaluation_run_has_no_cells")
        return sorted(reasons)
    status = cells["status"].astype(str)
    for missing in _CELL_MISSING:
        count = int((status == missing).sum())
        if count:
            reasons.add(f"cells_{missing}:{count}")
    tested = cells[status == _CELL_TESTED]
    if len(tested) and not (tested["sample"].astype(str) == "selection").all():
        reasons.add("tested_cells_not_on_the_frozen_selection_sample")
    n_trials = _i(results.family.get("n_trials"))
    trials = {_i(v) for v in tested["dsr_n_trials"]} - {None}
    if trials and (n_trials is None or trials != {n_trials}):
        reasons.add(f"dsr_n_trials_not_the_whole_family:{sorted(trials)}!={n_trials}")
    primary = cells[(cells["variant"] == policy.primary_variant) & (cells["horizon_months"] == policy.primary_horizon)]
    have = set(zip(primary["basis"].astype(str), primary["feature_id"].astype(str), strict=True))
    want = set(zip(cells["basis"].astype(str), cells["feature_id"].astype(str), strict=True))
    if want - have:
        reasons.add(f"primary_cells_missing:{len(want - have)}")
    # M6: completeness re-derived from the sealed cells, not only the run row's flag.
    missing_bases = set(BASES) - set(cells["basis"].astype(str))
    if missing_bases:
        reasons.add("bases_missing_in_sealed_cells:" + ",".join(sorted(missing_bases)))
    rows = _catalog_rows(catalog)
    if rows is not None:
        evaluated = set(cells["feature_id"].astype(str))
        if set(rows) - evaluated:
            reasons.add(f"catalog_features_not_evaluated:{len(set(rows) - evaluated)}")
        if evaluated - set(rows):
            reasons.add(f"features_outside_catalog:{len(evaluated - set(rows))}")
        signs = cells[["feature_id", "expected_sign"]].dropna()
        wrong = {str(f) for f, s in zip(signs["feature_id"], signs["expected_sign"], strict=True)
                 if str(f) in rows and int(s) != int(rows[str(f)].expected_sign)}
        if wrong:
            reasons.add(f"expected_sign_differs_from_catalog:{len(wrong)}")
    return sorted(reasons)


# ---------------------------------------------------------------------------
# Pure grading
# ---------------------------------------------------------------------------

BASIS_COLUMNS: tuple[tuple[str, str], ...] = (
    ("feature_id", "VARCHAR"), ("basis", "VARCHAR"), ("status", "VARCHAR"), ("status_reasons", "VARCHAR"),
    ("evidence_basis", "VARCHAR"), ("cell_status", "VARCHAR"), ("cell_status_reason", "VARCHAR"),
    ("expected_sign", "INTEGER"), ("direction", "INTEGER"), ("raw_direction", "INTEGER"),
    ("identity_basis", "VARCHAR"), ("universe_basis", "VARCHAR"), ("classification_basis", "VARCHAR"),
    ("availability_basis", "VARCHAR"), ("label_basis", "VARCHAR"), ("universe_scope", "VARCHAR"),
    ("gate_coverage", "BOOLEAN"), ("gate_significance", "BOOLEAN"), ("gate_sign", "BOOLEAN"),
    ("gate_holdout_sign", "BOOLEAN"), ("gate_holdout_strength", "BOOLEAN"), ("gate_subperiods", "BOOLEAN"),
    ("gate_size_buckets", "BOOLEAN"), ("dsr_pass", "BOOLEAN"),
    ("ic_n", "INTEGER"), ("effective_n", "INTEGER"), ("selection_formations", "INTEGER"),
    ("covered_formations", "INTEGER"), ("coverage_share", "DOUBLE"), ("mean_coverage", "DOUBLE"),
    ("min_coverage", "DOUBLE"),
    ("ic_mean", "DOUBLE"), ("ic_z", "DOUBLE"), ("ic_robust_p", "DOUBLE"), ("ic_robust_df", "INTEGER"),
    ("bh_q", "DOUBLE"), ("holm_p", "DOUBLE"), ("hlz", "BOOLEAN"), ("ic_nw_t", "DOUBLE"),
    ("ic_boot_low", "DOUBLE"), ("ic_boot_high", "DOUBLE"),
    ("holdout_formations", "INTEGER"), ("holdout_ic_mean", "DOUBLE"), ("holdout_ic_z", "DOUBLE"),
    ("subperiod_same_sign", "INTEGER"), ("subperiod_signs", "VARCHAR"),
    ("micro_ic_mean", "DOUBLE"), ("micro_ic_z", "DOUBLE"), ("small_formations", "INTEGER"),
    ("small_ic_mean", "DOUBLE"), ("small_ic_z", "DOUBLE"), ("large_formations", "INTEGER"),
    ("large_ic_mean", "DOUBLE"), ("large_ic_z", "DOUBLE"), ("size_venue_basis", "VARCHAR"),
    ("dsr", "DOUBLE"), ("dsr_z", "DOUBLE"), ("dsr_benchmark", "DOUBLE"), ("dsr_n_trials", "INTEGER"),
    ("dsr_effective_n", "INTEGER"), ("psr", "DOUBLE"), ("sharpe", "DOUBLE"),
    ("ls_ew10_mean", "DOUBLE"), ("ls_ew10_z", "DOUBLE"), ("ls_vw10_mean", "DOUBLE"), ("ls_vw10_z", "DOUBLE"),
    ("ls_nyse_ew10_mean", "DOUBLE"), ("ls_nyse_ew10_z", "DOUBLE"), ("mono_ew10", "DOUBLE"), ("monotone", "BOOLEAN"),
    ("net_mean", "DOUBLE"), ("net_positive", "BOOLEAN"), ("fm_z", "DOUBLE"), ("fmc_z", "DOUBLE"),
    ("rank_autocorr_1m", "DOUBLE"), ("top_turnover_h", "DOUBLE"), ("bottom_turnover_h", "DOUBLE"),
    ("labels_used", "BIGINT"), ("stitched_observed", "BIGINT"), ("stitched_policy", "BIGINT"),
    ("policy_terminal_share", "DOUBLE"),
    ("neutral_json", "VARCHAR"), ("horizons_json", "VARCHAR"),
)
FEATURE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("feature_id", "VARCHAR"), ("status", "VARCHAR"), ("status_basis", "VARCHAR"), ("status_reasons", "VARCHAR"),
    ("strict_status", "VARCHAR"), ("reconstructed_status", "VARCHAR"),
    ("anomaly_class", "VARCHAR"), ("role", "VARCHAR"), ("expected_sign", "INTEGER"), ("two_sided", "BOOLEAN"),
    ("raw_direction", "INTEGER"), ("hypothesis_family", "VARCHAR"), ("prior_evidence", "VARCHAR"),
    ("exploratory", "BOOLEAN"), ("admission", "VARCHAR"), ("caveat_codes", "VARCHAR"),
    ("identity_basis", "VARCHAR"), ("universe_basis", "VARCHAR"), ("classification_basis", "VARCHAR"),
    ("availability_basis", "VARCHAR"), ("label_basis", "VARCHAR"),
    ("ic_mean", "DOUBLE"), ("ic_z", "DOUBLE"), ("bh_q", "DOUBLE"), ("hlz", "BOOLEAN"), ("dsr_z", "DOUBLE"),
    ("dsr_pass", "BOOLEAN"), ("psr", "DOUBLE"), ("holdout_ic_z", "DOUBLE"), ("size_venue_basis", "VARCHAR"),
    ("coverage_share", "DOUBLE"), ("ic_n", "INTEGER"),
    ("effective_n", "INTEGER"), ("ls_ew10_mean", "DOUBLE"), ("mono_ew10", "DOUBLE"), ("net_mean", "DOUBLE"),
    ("top_turnover_h", "DOUBLE"), ("bottom_turnover_h", "DOUBLE"), ("policy_terminal_share", "DOUBLE"),
    ("post_hoc_policy", "BOOLEAN"),
)
_COPIED_FROM_BASIS = ("identity_basis", "universe_basis", "classification_basis", "availability_basis",
                      "label_basis", "ic_mean", "ic_z", "bh_q", "hlz", "dsr_z", "dsr_pass", "psr", "holdout_ic_z",
                      "size_venue_basis", "coverage_share", "ic_n", "effective_n", "ls_ew10_mean", "mono_ew10",
                      "net_mean", "top_turnover_h", "bottom_turnover_h", "policy_terminal_share")


@dataclass(frozen=True)
class QualificationLedger:
    """A sealed ledger: manifest, one row per feature, one evidence row per feature x basis."""

    ledger_id: str
    manifest: Mapping[str, Any]
    features: tuple[Mapping[str, Any], ...]
    bases: tuple[Mapping[str, Any], ...]
    sha256: str

    def payload(self) -> dict[str, Any]:
        return {"manifest": dict(self.manifest), "features": [dict(r) for r in self.features],
                "bases": [dict(r) for r in self.bases], "ledger_sha256": self.sha256}

    def to_json_bytes(self) -> bytes:
        """The versioned JSON artifact (canonical, byte-reproducible)."""
        text = json.dumps(self.payload(), sort_keys=True, indent=1, ensure_ascii=True, allow_nan=False)
        return (text + "\n").encode("ascii")

    def to_csv_bytes(self) -> bytes:
        """The versioned CSV artifact: the feature ledger, LF line endings."""
        buffer = io.StringIO()
        writer = csv.writer(buffer, lineterminator="\n")
        names = [name for name, _ in FEATURE_COLUMNS]
        writer.writerow(names)
        for row in self.features:
            writer.writerow([_csv_cell(row.get(name)) for name in names])
        return buffer.getvalue().encode("utf-8")

    def status_of(self, feature_id: str) -> str:
        for row in self.features:
            if row["feature_id"] == feature_id:
                return str(row["status"])
        raise KeyError(feature_id)


def _csv_cell(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def _index(frame: pd.DataFrame, keys: Sequence[str]) -> dict[tuple[Any, ...], dict[str, Any]]:
    out: dict[tuple[Any, ...], dict[str, Any]] = {}
    for record in frame.to_dict("records"):
        key = tuple(int(record[k]) if k == "horizon_months" else str(record[k]) for k in keys)
        out[key] = record
    return out


def _coverage_by_key(coverage: pd.DataFrame) -> dict[tuple[str, str], list[tuple[bool, float | None]]]:
    grouped: dict[tuple[str, str], list[tuple[bool, float | None]]] = {}
    for basis, feature_id, selected, value in zip(coverage["basis"], coverage["feature_id"], coverage["in_selection"],
                                                  coverage["coverage"], strict=True):
        grouped.setdefault((str(basis), str(feature_id)), []).append((bool(_b(selected)), _f(value)))
    return grouped


def _is_strict_evidence(basis: str, cell: Mapping[str, Any], policy: QualificationPolicy) -> bool:
    if basis != policy.strict_basis:
        return False
    if policy.strict_identity_bases is not None:  # v3: exact allowlists (M1)
        return (_s(cell.get("identity_basis")) in policy.strict_identity_bases
                and _s(cell.get("universe_basis")) in (policy.strict_universe_bases or ())
                and _s(cell.get("universe_scope")) in (policy.strict_universe_scopes or ()))
    labels = " ".join(str(cell.get(name) or "") for name in ("identity_basis", "universe_basis")).lower()
    return not any(token in labels for token in policy.non_strict_tokens)


def _deflated(cell: Mapping[str, Any], direction: int, h: int, policy: QualificationPolicy
              ) -> tuple[stats.DeflatedSharpe | None, float | None]:
    sharpe, skew, kurt = _f(cell.get("sharpe")), _f(cell.get("sharpe_skew")), _f(cell.get("sharpe_kurt"))
    n_obs, n_trials = _i(cell.get("sharpe_n")), _i(cell.get("dsr_n_trials"))
    variance = _f(cell.get("dsr_sharpe_variance"))
    if None in (sharpe, skew, kurt, n_obs, n_trials, variance):
        return None, None
    assert sharpe is not None and skew is not None and kurt is not None
    assert n_obs is not None and n_trials is not None and variance is not None
    try:
        deflated = stats.deflated_sharpe_ratio(direction * sharpe, n_obs=n_obs, skewness=direction * skew,
                                               kurtosis=kurt, n_trials=n_trials, sharpe_variance=variance,
                                               horizon_periods=h)
        psr = stats.probabilistic_sharpe_ratio(direction * sharpe, policy.psr_benchmark, n_obs=n_obs,
                                               skewness=direction * skew, kurtosis=kurt, horizon_periods=h)
    except (ValueError, ArithmeticError):
        return None, None
    return deflated, psr


def _grade_basis(feature_id: str, basis: str, cells: Mapping[tuple[Any, ...], dict[str, Any]],
                 slices: Mapping[tuple[Any, ...], dict[str, Any]],
                 coverage: Mapping[tuple[str, str], list[tuple[bool, float | None]]],
                 policy: QualificationPolicy) -> dict[str, Any]:
    h, variant = policy.primary_horizon, policy.primary_variant
    cell = cells[(basis, feature_id, variant, h)]
    cell_status = str(cell["status"])
    expected = _i(cell.get("expected_sign"))
    row: dict[str, Any] = {name: None for name, _ in BASIS_COLUMNS}
    row.update({"feature_id": feature_id, "basis": basis, "cell_status": cell_status,
                "cell_status_reason": _s(cell.get("status_reason")), "expected_sign": expected,
                "identity_basis": _s(cell.get("identity_basis")), "universe_basis": _s(cell.get("universe_basis")),
                "classification_basis": _s(cell.get("classification_basis")),
                "availability_basis": _s(cell.get("availability_basis")), "label_basis": _s(cell.get("label_basis")),
                "universe_scope": _s(cell.get("universe_scope"))})
    if cell_status == _CELL_UNTESTABLE:
        row.update({"status": UNTESTABLE_STRICT, "status_reasons": "basis_untestable"})
        return row

    z, mean = _f(cell.get("ic_z")), _f(cell.get("ic_mean"))
    two_sided = expected == 0
    direction = (_signum(z) or _signum(mean) or 1) if two_sided else 1
    raw_direction = direction if two_sided else (expected or 1)
    tested = cell_status == _CELL_TESTED
    ic_n = _i(cell.get("ic_n")) or 0
    rows = coverage.get((basis, feature_id), [])
    selected = [value for chosen, value in rows if chosen]
    covered = sum(1 for value in selected if value is not None and value >= policy.min_value_coverage)
    share = covered / len(selected) if selected else None
    q = _f(cell.get("bh_q"))
    hlz = z is not None and abs(z) >= policy.hlz_min_abs_z
    bh = q is not None and q <= policy.bh_max_q and z is not None and abs(z) >= policy.bh_min_abs_z
    row.update({"direction": direction, "raw_direction": raw_direction, "ic_n": ic_n, "effective_n": ic_n // h,
                "selection_formations": len(selected), "covered_formations": covered, "coverage_share": share,
                "mean_coverage": _f(cell.get("mean_coverage")), "min_coverage": _f(cell.get("min_coverage")),
                "ic_mean": mean, "ic_z": z, "ic_robust_p": _f(cell.get("ic_robust_p")),
                "ic_robust_df": _i(cell.get("ic_robust_df")), "bh_q": q, "holm_p": _f(cell.get("holm_p")),
                "hlz": hlz, "ic_nw_t": _f(cell.get("ic_nw_t")), "ic_boot_low": _f(cell.get("ic_boot_low")),
                "ic_boot_high": _f(cell.get("ic_boot_high"))})

    coverage_fail: list[str] = []
    if not tested:
        why = _s(cell.get("status_reason"))
        coverage_fail.append(f"cell_{cell_status}" + (f"({why})" if why else ""))
    if ic_n < policy.min_observations:
        coverage_fail.append(f"observations={ic_n}<{policy.min_observations}")
    if share is None or share < policy.min_formation_share:
        coverage_fail.append(f"coverage_share={_g(share)}<{policy.min_formation_share:g}")
    significant = tested and (hlz or bh)
    reversed_sign = significant and not two_sided and z is not None and z < 0
    unstable: list[str] = []
    candidate: list[str] = []

    # Holdout: the fixed direction, and |z| >= 1.5 in it.
    hold = slices.get((basis, feature_id, h, "split", policy.holdout_slice), {})
    h_mean, h_z = _f(hold.get("ic_mean")), _f(hold.get("ic_z"))
    hold_sign = None if h_mean is None else direction * h_mean > 0
    hold_strength = h_z is not None and direction * h_z >= policy.holdout_min_signed_z
    if h_mean is None:
        candidate.append("holdout_untestable")
    elif not hold_sign:
        unstable.append(f"holdout_sign_flip(ic={_g(h_mean)})")
    elif not hold_strength:
        candidate.append(f"holdout_z={_g(h_z)}<{policy.holdout_min_signed_z:g}")
    row.update({"holdout_formations": _i(hold.get("formations")), "holdout_ic_mean": h_mean, "holdout_ic_z": h_z,
                "gate_holdout_sign": hold_sign, "gate_holdout_strength": hold_strength})

    # Subperiods: >= 2 of 3 with the fixed sign; missing evidence is not a flip.
    signs, same, missing = [], 0, 0
    for name in policy.subperiods:
        sub_mean = _f(slices.get((basis, feature_id, h, "subperiod", name), {}).get("ic_mean"))
        if sub_mean is None:
            missing += 1
            signs.append(f"{name}:na")
            continue
        agrees = direction * sub_mean > 0
        same += agrees
        signs.append(f"{name}:{'same' if agrees else 'opposite'}")
    sub_ok = same >= policy.subperiod_min_same_sign
    if not sub_ok:
        text = f"subperiods_same_sign={same}/{len(policy.subperiods)}<{policy.subperiod_min_same_sign}"
        (candidate if same + missing >= policy.subperiod_min_same_sign else unstable).append(text)
    row.update({"subperiod_same_sign": same, "subperiod_signs": ",".join(signs), "gate_subperiods": sub_ok})

    # Size buckets: the fixed sign in small AND large. Point-in-time NYSE breakpoints when both
    # buckets have enough formations; else (v2) R3b's labeled verified-cap tercile slices.
    sources = [(policy.size_slice_kind, policy.size_venue_basis)]
    if policy.size_fallback_kind is not None and policy.size_fallback_venue is not None:
        sources.append((policy.size_fallback_kind, policy.size_fallback_venue))

    def bucket_slices(kind: str, venue: str) -> dict[str, dict[str, Any]]:
        return {bucket: item for bucket in ("micro", "small", "large")
                if (item := slices.get((basis, feature_id, h, kind, bucket))) is not None
                and _s(item.get("venue_basis")) in (None, venue)}

    def usable(found: Mapping[str, Mapping[str, Any]], venue: str) -> bool:
        floor = policy.size_min_venue_pit_share if venue == policy.size_venue_basis else None
        return all(_f(found.get(b, {}).get("ic_mean")) is not None
                   and (_i(found.get(b, {}).get("formations")) or 0) >= policy.size_min_formations
                   and (floor is None or (_f(found.get(b, {}).get("venue_pit_share")) or 0.0) >= floor)
                   for b in policy.size_buckets)

    size_kind, size_venue = sources[0]
    chosen_slices = bucket_slices(size_kind, size_venue)
    for kind, venue in sources[1:]:
        if not usable(chosen_slices, size_venue):
            size_kind, size_venue, chosen_slices = kind, venue, bucket_slices(kind, venue)
    size_ok = usable(chosen_slices, size_venue)
    if not size_ok:
        candidate.append(f"size_buckets_untestable({'/'.join(v for _, v in sources)})")
    for bucket in ("micro", "small", "large"):
        item = chosen_slices.get(bucket, {})
        b_mean = _f(item.get("ic_mean"))
        row[f"{bucket}_ic_mean"], row[f"{bucket}_ic_z"] = b_mean, _f(item.get("ic_z"))
        if bucket != "micro":
            row[f"{bucket}_formations"] = _i(item.get("formations"))
        if size_ok and bucket in policy.size_buckets and b_mean is not None and not direction * b_mean > 0:
            size_ok = False
            unstable.append(f"size_{bucket}_sign(ic={_g(b_mean)},{size_venue})")
    row.update({"gate_size_buckets": size_ok, "size_venue_basis": size_venue if chosen_slices else None})

    # Deflated Sharpe of the fixed-direction EW decile long-short (whole family, horizon h):
    # gating under v1, reported and flagged under v2.
    deflated, psr = _deflated(cell, direction, h, policy)
    dsr_ok = deflated is not None and deflated.z > policy.dsr_min_z
    if policy.dsr_gating and deflated is None:
        candidate.append("dsr_unavailable")
    elif policy.dsr_gating and not dsr_ok:
        candidate.append(f"dsr_z={_g(_f(deflated.z) if deflated else None)}<={policy.dsr_min_z:g}")
    row.update({"dsr_pass": dsr_ok, "psr": psr, "sharpe": _f(cell.get("sharpe")),
                "dsr_n_trials": _i(cell.get("dsr_n_trials")),
                "dsr": None if deflated is None else _f(deflated.deflated_sharpe_ratio),
                "dsr_z": None if deflated is None else _f(deflated.z),
                "dsr_benchmark": None if deflated is None else _f(deflated.benchmark_sharpe),
                "dsr_effective_n": None if deflated is None else deflated.effective_n_obs})

    # Supporting evidence (reported, never gating).
    ls_mean = _f(cell.get("ls_ew10_mean"))
    mono = _f(cell.get("mono_ew10"))
    net = _f(cell.get(f"net{policy.net_cost_bps}_mean"))
    if direction < 0 and net is not None and ls_mean is not None:
        net = net - 2.0 * ls_mean  # the reversed long-short pays the same turnover cost
    used = round((_f(cell.get("mean_names")) or 0.0) * (_i(cell.get("formations_usable")) or 0))
    stitched_policy = _i(cell.get("stitched_policy"))
    row.update({"ls_ew10_mean": ls_mean, "ls_ew10_z": _f(cell.get("ls_ew10_z")),
                "ls_vw10_mean": _f(cell.get("ls_vw10_mean")), "ls_vw10_z": _f(cell.get("ls_vw10_z")),
                "ls_nyse_ew10_mean": _f(cell.get("ls_nyse_ew10_mean")),
                "ls_nyse_ew10_z": _f(cell.get("ls_nyse_ew10_z")), "mono_ew10": mono,
                "monotone": None if mono is None else direction * mono >= policy.monotonicity_min,
                "net_mean": net, "net_positive": None if net is None else net > 0,
                "fm_z": _f(cell.get("fm_z")), "fmc_z": _f(cell.get("fmc_z")),
                "rank_autocorr_1m": _f(cell.get("rank_autocorr_1m")),
                "top_turnover_h": _f(cell.get("top_turnover_h")),
                "bottom_turnover_h": _f(cell.get("bottom_turnover_h")),
                "labels_used": used, "stitched_observed": _i(cell.get("stitched_observed")),
                "stitched_policy": stitched_policy,
                "policy_terminal_share": None if not used or stitched_policy is None else stitched_policy / used})
    neutral = {}
    for name in policy.neutral_variants:
        other = cells.get((basis, feature_id, name, h))
        if other is None:
            continue
        other_z = _f(other.get("ic_z"))
        neutral[name] = {"cell_status": str(other["status"]), "ic_mean": _f(other.get("ic_mean")), "ic_z": other_z,
                         "bh_q": _f(other.get("bh_q")),
                         "survives": other_z is not None and direction * other_z >= policy.neutral_min_abs_z}
    horizons = {}
    for other_h in policy.reported_horizons:
        other = cells.get((basis, feature_id, variant, other_h))
        if other is None:
            continue
        other_z = _f(other.get("ic_z"))
        horizons[str(other_h)] = {"cell_status": str(other["status"]), "ic_mean": _f(other.get("ic_mean")),
                                  "ic_z": other_z, "bh_q": _f(other.get("bh_q")),
                                  "hlz": other_z is not None and abs(other_z) >= policy.hlz_min_abs_z}
    row.update({"neutral_json": _canonical(neutral), "horizons_json": _canonical(horizons)})

    evidence = "strict" if _is_strict_evidence(basis, cell, policy) else "reconstructed"
    row.update({"gate_coverage": not coverage_fail, "gate_significance": significant,
                "gate_sign": None if not significant else not reversed_sign, "evidence_basis": evidence})
    if coverage_fail:
        status, reasons = INSUFFICIENT_COVERAGE, coverage_fail
    elif not significant:
        status, reasons = NOT_SIGNIFICANT, [f"ic_z={_g(z)},bh_q={_g(q)}"]
    elif reversed_sign:
        status, reasons = SIGN_REVERSED, [f"ic_z={_g(z)}<0_against_expected_sign"]
    elif unstable:
        status, reasons = UNSTABLE, unstable + candidate
    elif candidate:
        status, reasons = CANDIDATE, candidate
    else:
        status = QUALIFIED_STRICT if evidence == "strict" else QUALIFIED_RECONSTRUCTED
        reasons = [] if evidence == "strict" or basis != policy.strict_basis else ["strict_basis_labels_not_strict"]
    row.update({"status": status, "status_reasons": "; ".join(reasons) or None})
    return row


def _resolve(rows: Mapping[str, dict[str, Any]], policy: QualificationPolicy) -> tuple[str, str, str | None]:
    """(status, status_basis, reasons) of one feature from its per-basis rows."""
    order = sorted(rows, key=lambda b: (b == policy.strict_basis, b != REFERENCE_BASIS, b))
    strict = rows.get(policy.strict_basis)
    passes = [b for b in order if rows[b]["status"] in QUALIFIED_STATUSES]
    reversed_bases = [b for b in order if rows[b]["status"] == SIGN_REVERSED]
    if passes and reversed_bases:  # symmetric veto (M2): one basis qualifies, another reverses the sign
        return UNSTABLE, reversed_bases[0], f"{reversed_bases[0]}_basis_sign_reversed"
    for basis in order:
        if rows[basis]["status"] == QUALIFIED_STRICT:
            return QUALIFIED_STRICT, basis, rows[basis]["status_reasons"]
    if passes:
        return QUALIFIED_RECONSTRUCTED, passes[0], rows[passes[0]]["status_reasons"]
    for basis in order:
        if rows[basis]["status"] != UNTESTABLE_STRICT:
            return str(rows[basis]["status"]), basis, rows[basis]["status_reasons"]
    basis = policy.strict_basis if strict is not None else order[0]
    return UNTESTABLE_STRICT, basis, rows[basis]["status_reasons"]


def qualify(results: EvaluationResults, policy: QualificationPolicy, catalog: Iterable[Any] | None = None, *,
            policy_registered_at: str | None = None, allow_post_hoc_policy: bool = False,
            historical: bool = False) -> QualificationLedger:
    """Grade every feature of a sealed run under a frozen policy (pure; no I/O).

    ``catalog`` (R1a entries; research-eligible rows are the family) is checked against
    the run and annotates each feature; without it the ledger is stamped
    ``catalog_checked=false`` and cannot be persisted. ``policy_registered_at`` is the
    store's freeze time of the policy. A run not evaluated under this frozen policy
    file, or created before the freeze, is refused unless ``allow_post_hoc_policy``,
    which stamps ``post_hoc_policy`` on the manifest, every feature row and the doc.
    A superseded or spec-less policy (v1, v2) is always refused (N1); ``historical``
    waives that for :func:`verify_ledger` alone, and such a ledger is never persisted.
    Raises :class:`QualificationRefused` for an unqualifiable run.
    """
    if isinstance(policy, QualificationPolicyV4):
        raise PolicyError(f"{policy.version} grades registered waves: use grade_wave_v4, not the R4 run ledger")
    catalog_rows = _catalog_rows(catalog)
    reasons = refusal_reasons(results, policy, None if catalog_rows is None else catalog_rows.values(),
                              policy_registered_at=policy_registered_at, historical=historical)
    post_hoc = sorted(set(reasons) & set(POST_HOC_REASONS))
    if allow_post_hoc_policy:
        reasons = [reason for reason in reasons if reason not in POST_HOC_REASONS]
    if reasons:
        raise QualificationRefused(reasons)
    cells = _index(results.cells, _CELL_KEYS)
    slices = _index(results.slices, ("basis", "feature_id", "horizon_months", "slice_kind", "slice_name"))
    coverage = _coverage_by_key(results.coverage)
    bases_of: dict[str, list[str]] = {}
    for basis, feature_id, variant, h in cells:
        if variant == policy.primary_variant and h == policy.primary_horizon:
            bases_of.setdefault(feature_id, []).append(basis)

    basis_rows: list[dict[str, Any]] = []
    feature_rows: list[dict[str, Any]] = []
    for feature_id in sorted(bases_of):
        graded = {basis: _grade_basis(feature_id, basis, cells, slices, coverage, policy)
                  for basis in sorted(bases_of[feature_id])}
        basis_rows.extend(graded[basis] for basis in sorted(graded))
        status, status_basis, why = _resolve(graded, policy)
        chosen = graded[status_basis]
        entry = None if catalog_rows is None else catalog_rows.get(feature_id)
        cell = cells[(status_basis, feature_id, policy.primary_variant, policy.primary_horizon)]
        anomaly_class = _s(getattr(entry, "anomaly_class", None)) or _s(cell.get("anomaly_class"))
        expected = int(entry.expected_sign) if entry is not None else _i(cell.get("expected_sign"))
        prior = _s(getattr(entry, "prior_evidence", None))
        record: dict[str, Any] = {name: None for name, _ in FEATURE_COLUMNS}
        record.update({name: chosen.get(name) for name in _COPIED_FROM_BASIS})
        record.update({
            "feature_id": feature_id, "status": status, "status_basis": status_basis, "status_reasons": why,
            "strict_status": graded[policy.strict_basis]["status"] if policy.strict_basis in graded else None,
            "reconstructed_status": graded[REFERENCE_BASIS]["status"] if REFERENCE_BASIS in graded else None,
            "anomaly_class": anomaly_class,
            "role": None if anomaly_class is None else ("control" if anomaly_class in CONTROL_CLASSES else "anomaly"),
            "expected_sign": expected, "two_sided": None if expected is None else expected == 0,
            "raw_direction": chosen.get("raw_direction"),
            "hypothesis_family": _s(getattr(entry, "hypothesis_family", None)) or _s(cell.get("hypothesis_family")),
            "prior_evidence": prior,
            "exploratory": None if prior is None else prior == "economic_conjecture",
            "admission": _s(getattr(entry, "admission", None)),
            "caveat_codes": "|".join(getattr(entry, "caveat_codes", ()) or ()) or None,
            "post_hoc_policy": bool(post_hoc),
        })
        feature_rows.append(record)
    manifest = _manifest(results, policy, catalog_rows, feature_rows, basis_rows)
    manifest.update({"post_hoc_policy": bool(post_hoc), "post_hoc_reasons": post_hoc,
                     "policy_registered_at": policy_registered_at})
    if post_hoc:
        manifest["blockers"] = sorted({*manifest["blockers"], *(f"post_hoc_policy:{r}" for r in post_hoc)})
    features = tuple({name: _clean(row[name]) for name, _ in FEATURE_COLUMNS} for row in feature_rows)
    bases = tuple({name: _clean(row[name]) for name, _ in BASIS_COLUMNS} for row in basis_rows)
    ledger_id = f"{results.run_id}:{policy.version}"
    manifest = {**manifest, "ledger_id": ledger_id}
    sha = _sha(_canonical({"manifest": manifest, "features": list(features), "bases": list(bases)}))
    return QualificationLedger(ledger_id, manifest, features, bases, sha)


def _clean(value: Any) -> Any:
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, bool) or value is None or isinstance(value, (int, str)):
        return value
    if hasattr(value, "item"):
        return _clean(value.item())
    return str(value)


def _manifest(results: EvaluationResults, policy: QualificationPolicy, catalog_rows: Mapping[str, Any] | None,
              features: Sequence[Mapping[str, Any]], bases: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    counts = {status: 0 for status in STATUSES}
    for row in features:
        counts[str(row["status"])] += 1
    by_basis: dict[str, dict[str, int]] = {}
    for row in bases:
        item = by_basis.setdefault(str(row["basis"]), {})
        item[str(row["status"])] = item.get(str(row["status"]), 0) + 1
    blockers = set(results.blockers)
    supplied = set(by_basis)
    if policy.strict_basis not in supplied:
        blockers.add("strict_basis_not_supplied")
    elif set(by_basis[policy.strict_basis]) == {UNTESTABLE_STRICT}:
        blockers.add("strict_basis_untestable_no_strict_qualification")
    if counts[QUALIFIED_RECONSTRUCTED]:
        blockers.add(f"qualified_on_reconstructed_evidence_only:{counts[QUALIFIED_RECONSTRUCTED]}")
    if catalog_rows is None:
        blockers.add("catalog_not_checked_dry_run_only")
    if results.stored_rows_sha256 is None:
        blockers.add("seal_not_verified_dry_run_only")
    run_policy_file = results.spec.get("policy_sha256")
    size_missing = sum(1 for row in bases if row["status"] != UNTESTABLE_STRICT and any(
        row.get(f"{bucket}_ic_mean") is None or (row.get(f"{bucket}_formations") or 0) < policy.size_min_formations
        for bucket in policy.size_buckets))
    if size_missing:
        blockers.add(f"size_bucket_evidence_missing:{size_missing}")
    fallback = sum(1 for row in bases if policy.size_fallback_venue is not None
                   and row.get("size_venue_basis") == policy.size_fallback_venue)
    if fallback:
        blockers.add(f"size_buckets_on_{policy.size_fallback_venue}_fallback:{fallback}")
    family = results.family
    return {
        "qualification_version": QUALIFICATION_VERSION,
        "qualification_code_sha256": qualification_code_sha256(),
        "catalog_checked": catalog_rows is not None,
        "seal_verified": results.stored_rows_sha256 is not None,
        "policy": {"version": policy.version, "sha256": policy.sha256, "split_sha256": policy.split.sha256,
                   "primary_horizon_months": policy.primary_horizon, "primary_variant": policy.primary_variant},
        "evaluation": {
            "run_id": results.run_id, "results_sha256": results.results_sha256,
            "created_at": results.created_at,
            "spec": {name: results.spec.get(name) for name in EVALUATION_SPEC_FIELDS},
            "evaluation_version": results.spec.get("evaluation_version"),
            "feature_versions": list(results.spec.get("feature_versions") or ()),
            "label_cutoff": results.spec.get("label_cutoff"), "split_sha256": (results.spec.get("split") or {}).get(
                "sha256"), "policy_file_sha256": run_policy_file, "family_complete": results.family_complete,
            "n_trials": _i(family.get("n_trials")), "family_cells": _i(family.get("family_cells")),
            "tested_cells": _i(family.get("tested_cells")), "bh_discoveries": _i(family.get("bh_discoveries")),
            "hlz_passes": _i(family.get("hlz_passes"))},
        "catalog": None if catalog_rows is None else {"sha256": _catalog_digest(catalog_rows),
                                                      "research_eligible_features": len(catalog_rows)},
        "features": len(features),
        "status_counts": counts,
        "basis_status_counts": {basis: dict(sorted(items.items())) for basis, items in sorted(by_basis.items())},
        "blockers": sorted(blockers),
    }


# ---------------------------------------------------------------------------
# Research store (policy registry and ledgers)
# ---------------------------------------------------------------------------

_SCHEMA_DDL = (
    """CREATE TABLE IF NOT EXISTS research_qualification_schema (
        version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS research_qualification_policies (
        policy_version VARCHAR PRIMARY KEY, policy_sha256 VARCHAR NOT NULL, policy_json VARCHAR NOT NULL,
        registered_at TIMESTAMP NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS research_qualification_ledgers (
        ledger_id VARCHAR PRIMARY KEY, policy_version VARCHAR NOT NULL, policy_sha256 VARCHAR NOT NULL,
        evaluation_run_id VARCHAR NOT NULL, evaluation_results_sha256 VARCHAR, qualification_version VARCHAR NOT NULL,
        ledger_sha256 VARCHAR NOT NULL, manifest_json VARCHAR NOT NULL, created_at TIMESTAMP NOT NULL)""",
    "CREATE TABLE IF NOT EXISTS research_qualification_features (ledger_id VARCHAR NOT NULL, "
    + ", ".join(f"{name} {kind}" for name, kind in FEATURE_COLUMNS) + ")",
    "CREATE TABLE IF NOT EXISTS research_qualification_bases (ledger_id VARCHAR NOT NULL, "
    + ", ".join(f"{name} {kind}" for name, kind in BASIS_COLUMNS) + ")",
)


_SCHEMA_VERSIONS = {1: "policy_registry_and_ledgers", 2: "post_hoc_policy_stamp"}


def ensure_qualification_schema(con: duckdb.DuckDBPyConnection) -> None:
    """Create or upgrade the ``research_qualification_*`` tables (module-owned, versioned)."""
    con.execute(_SCHEMA_DDL[0])
    versions = {int(row[0]) for row in con.execute("SELECT version FROM research_qualification_schema").fetchall()}
    if versions - set(_SCHEMA_VERSIONS):
        raise QualificationError(f"research qualification schema has unknown versions {sorted(versions)}")
    if QUALIFICATION_SCHEMA_VERSION in versions:
        return
    for ddl in _SCHEMA_DDL[1:]:
        con.execute(ddl)
    for table, columns in (("research_qualification_features", FEATURE_COLUMNS),
                           ("research_qualification_bases", BASIS_COLUMNS)):
        for name, kind in columns:  # upgrade in place; writers name their columns
            con.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS {name} {kind}")
    for version, name in sorted(_SCHEMA_VERSIONS.items()):
        if version not in versions:
            con.execute("INSERT INTO research_qualification_schema VALUES (?, ?, ?)", [version, name, _now()])


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


class _Transaction:
    def __init__(self, con: duckdb.DuckDBPyConnection) -> None:
        self.con = con

    def __enter__(self) -> duckdb.DuckDBPyConnection:
        self.con.execute("BEGIN TRANSACTION")
        return self.con

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.con.execute("ROLLBACK" if exc_type is not None else "COMMIT")


def register_policy(con: duckdb.DuckDBPyConnection, policy: QualificationPolicy) -> str:
    """Record the policy hash in the research store (RX7); ``'registered'`` or ``'exists'``.

    A registered version is frozen: another content hash under it is refused. A
    superseded or spec-less policy (v1, v2) is never frozen (N1).
    """
    problem = current_policy_problem(policy)
    if problem is not None:
        raise PolicyError(f"{problem}: only the current policy with a pinned evaluation_spec can be frozen")
    with _Transaction(con):
        ensure_qualification_schema(con)
        row = con.execute("SELECT policy_sha256 FROM research_qualification_policies WHERE policy_version = ?",
                          [policy.version]).fetchone()
        if row is not None:
            if row[0] != policy.sha256:
                raise PolicyError(f"policy {policy.version} is registered with sha256 {row[0]}; changing it after "
                                  f"registration ({policy.sha256}) needs a new policy_version")
            return "exists"
        con.execute("INSERT INTO research_qualification_policies VALUES (?, ?, ?, ?)",
                    [policy.version, policy.sha256, _canonical(dict(policy.content)), _now()])
    return "registered"


def _rows_frame(rows: Sequence[Mapping[str, Any]], columns: Sequence[tuple[str, str]], ledger_id: str) -> pd.DataFrame:
    data: dict[str, Any] = {"ledger_id": [ledger_id] * len(rows)}
    for name, kind in columns:
        values = [row.get(name) for row in rows]
        if kind == "DOUBLE":
            data[name] = pd.array([None if v is None else float(v) for v in values], dtype="Float64")
        elif kind in ("INTEGER", "BIGINT"):
            data[name] = pd.array([None if v is None else int(v) for v in values], dtype="Int64")
        elif kind == "BOOLEAN":
            data[name] = pd.array(values, dtype="boolean")
        else:
            data[name] = pd.array(values, dtype=object)
    return pd.DataFrame(data)


def persist_ledger(con: duckdb.DuckDBPyConnection, ledger: QualificationLedger) -> str:
    """Store a ledger once: ``'stored'``, or ``'exists'`` for an identical ledger.

    A sealed ledger is immutable: different content under the same ledger id is refused.
    Dry-run ledgers (built without the catalog check or without re-hashing the run's
    seal) are refused (I1, M3), and so is any ledger under a superseded or spec-less
    policy (N1: those are rebuilt by :func:`verify_ledger` only).
    """
    version = ledger.manifest["policy"]["version"]
    if version in superseded_policy_versions() or version in _SPECLESS_HISTORY:
        raise PolicyError(f"policy_superseded:{version}: a ledger under a superseded policy is never persisted")
    if not ledger.manifest.get("catalog_checked") or not ledger.manifest.get("seal_verified"):
        raise QualificationError("only a ledger built with the catalog check and a verified run seal can be "
                                 "persisted (--no-catalog / --no-verify-seal are dry-run only)")
    with _Transaction(con):
        ensure_qualification_schema(con)
        registered = con.execute("SELECT policy_sha256 FROM research_qualification_policies WHERE policy_version = ?",
                                 [ledger.manifest["policy"]["version"]]).fetchone()
        if registered is None or registered[0] != ledger.manifest["policy"]["sha256"]:
            raise PolicyError("register the policy (register_policy) before persisting a ledger under it")
        row = con.execute("SELECT ledger_sha256 FROM research_qualification_ledgers WHERE ledger_id = ?",
                          [ledger.ledger_id]).fetchone()
        if row is not None:
            if row[0] != ledger.sha256:
                raise QualificationError(f"ledger {ledger.ledger_id} is sealed with sha256 {row[0]}; refusing a "
                                         f"different ledger ({ledger.sha256})")
            return "exists"
        manifest = ledger.manifest
        con.execute("INSERT INTO research_qualification_ledgers VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", [
            ledger.ledger_id, manifest["policy"]["version"], manifest["policy"]["sha256"],
            manifest["evaluation"]["run_id"], manifest["evaluation"]["results_sha256"],
            manifest["qualification_version"], ledger.sha256, _canonical(dict(manifest)), _now()])
        for table, rows, columns in (("research_qualification_features", ledger.features, FEATURE_COLUMNS),
                                     ("research_qualification_bases", ledger.bases, BASIS_COLUMNS)):
            frame = _rows_frame(rows, columns, ledger.ledger_id)
            con.register("_rq_rows", frame)
            try:
                names = ", ".join(["ledger_id", *(name for name, _ in columns)])
                con.execute(f"INSERT INTO {table} ({names}) SELECT {names} FROM _rq_rows")
            finally:
                con.unregister("_rq_rows")
    return "stored"


def stored_ledger_sha256(con: duckdb.DuckDBPyConnection, ledger_id: str) -> str | None:
    if not _table_columns(con, "research_qualification_ledgers"):
        return None
    row = con.execute("SELECT ledger_sha256 FROM research_qualification_ledgers WHERE ledger_id = ?",
                      [ledger_id]).fetchone()
    return None if row is None else str(row[0])


def policy_registered_at(con: duckdb.DuckDBPyConnection, policy: QualificationPolicy) -> str | None:
    """When this policy version was frozen in the research store (ISO, naive UTC), or None.

    A different hash registered under the version is refused.
    """
    if not _table_columns(con, "research_qualification_policies"):
        return None
    row = con.execute("SELECT policy_sha256, registered_at FROM research_qualification_policies "
                      "WHERE policy_version = ?", [policy.version]).fetchone()
    if row is None:
        return None
    if row[0] != policy.sha256:
        raise PolicyError(f"policy {policy.version} is registered with sha256 {row[0]}, not {policy.sha256}")
    return pd.Timestamp(row[1]).isoformat()


def stored_catalog_snapshot(con: duckdb.DuckDBPyConnection, ledger_id: str) -> list[Any]:
    """The catalog annotations a stored ledger was built with (``verify`` reproduces from it, M9)."""
    rows = con.execute("SELECT feature_id, expected_sign, anomaly_class, hypothesis_family, prior_evidence, "
                       "admission, caveat_codes FROM research_qualification_features WHERE ledger_id = ? "
                       "ORDER BY feature_id", [ledger_id]).fetchall()
    return [SimpleNamespace(feature_id=f, expected_sign=int(sign), anomaly_class=cls, hypothesis_family=family,
                            prior_evidence=prior, admission=admission,
                            caveat_codes=tuple(caveats.split("|")) if caveats else (), is_research_eligible=True)
            for f, sign, cls, family, prior, admission, caveats in rows]


def qualify_run(con: duckdb.DuckDBPyConnection, run_id: str, policy: QualificationPolicy, *,
                catalog: Iterable[Any] | None = None, verify_seal: bool = True, persist: bool = True,
                allow_post_hoc_policy: bool = False) -> QualificationLedger:
    """Load a sealed run, grade it, and (``persist``) store the ledger.

    The policy must have been frozen in this store (``research_qualify.py freeze``) before
    the run was created, and the run evaluated under its file; otherwise the run is
    refused unless ``allow_post_hoc_policy`` (stamped). Persisting needs the catalog
    check and a verified seal. The manifest seals the registration time read *before*
    this call registers the policy (None when it was not yet frozen) and the post-hoc
    reasons that followed from it; :func:`verify_ledger` reproduces from those.
    """
    if isinstance(policy, QualificationPolicyV4):
        raise PolicyError(f"{policy.version} grades registered waves: use grade_wave_v4, not the R4 run ledger")
    results = load_evaluation_results(con, run_id, policy, verify_seal=verify_seal)
    ledger = qualify(results, policy, catalog, policy_registered_at=policy_registered_at(con, policy),
                     allow_post_hoc_policy=allow_post_hoc_policy)
    if persist:
        register_policy(con, policy)
        persist_ledger(con, ledger)
    return ledger


def verify_ledger(con: duckdb.DuckDBPyConnection, run_id: str, policy: QualificationPolicy) -> dict[str, Any]:
    """Rebuild a stored ledger from its run and compare the sha (read-only).

    N2: the rebuild uses what the ledger sealed at qualification, not the store's state
    now: its catalog snapshot, its post-hoc flag and its recorded ``policy_registered_at``
    (None when the policy was frozen only by that ledger's own persist). Registration is
    immutable, so a recorded time must still equal the store's; otherwise the verify is
    refused. N1: this is the one path that grades under a superseded or spec-less policy
    (``historical``); nothing it builds is persisted. Raises :class:`QualificationError`
    when no ledger is stored and :class:`QualificationRefused` when the run is refused.
    """
    ledger_id = f"{run_id}:{policy.version}"
    stored = stored_ledger_sha256(con, ledger_id)
    if stored is None:
        raise QualificationError(f"no stored ledger {ledger_id}")
    row = con.execute("SELECT manifest_json FROM research_qualification_ledgers WHERE ledger_id = ?",
                      [ledger_id]).fetchone()
    manifest = json.loads(row[0])
    recorded = manifest.get("policy_registered_at")
    if recorded is not None and recorded != policy_registered_at(con, policy):
        raise QualificationRefused(["policy_registration_differs_from_ledger"])
    results = load_evaluation_results(con, run_id, policy)
    ledger = qualify(results, policy, stored_catalog_snapshot(con, ledger_id), policy_registered_at=recorded,
                     allow_post_hoc_policy=bool(manifest.get("post_hoc_policy")), historical=True)
    return {"ledger_id": ledger_id, "stored_sha256": stored, "recomputed_sha256": ledger.sha256,
            "reproduced": stored == ledger.sha256}


def write_artifacts(ledger: QualificationLedger, out_dir: Path | str) -> tuple[Path, Path]:
    """Write the versioned JSON and CSV artifacts (``<run>__<policy>.json/.csv``)."""
    directory = Path(out_dir)
    directory.mkdir(parents=True, exist_ok=True)
    stem = ledger.ledger_id.replace(":", "__")
    json_path, csv_path = directory / f"{stem}.json", directory / f"{stem}.csv"
    json_path.write_bytes(ledger.to_json_bytes())
    csv_path.write_bytes(ledger.to_csv_bytes())
    return json_path, csv_path


# ---------------------------------------------------------------------------
# Generated documentation
# ---------------------------------------------------------------------------

_STATUS_MEANING = {
    QUALIFIED_STRICT: "passes every gate on the strict (verified identity/universe) basis",
    QUALIFIED_RECONSTRUCTED: "passes every gate on labeled reconstructed evidence (RX1); not certifiable",
    CANDIDATE: "significant with a stable sign, but a strength gate failed or evidence is missing "
               "(holdout z, size buckets, subperiods; the deflated Sharpe only where the policy gates on it)",
    NOT_SIGNIFICANT: "fails the family-corrected rank-IC significance gate",
    SIGN_REVERSED: "significant against the pre-registered sign",
    UNSTABLE: "the sign flips in the holdout or in a small/large size bucket, fewer than 2 of 3 subperiods keep it, "
              "or one basis qualifies while another reverses it",
    INSUFFICIENT_COVERAGE: "too few observations or too little value coverage to test",
    UNTESTABLE_STRICT: "no testable basis (the strict cohort is empty and no other basis was testable)",
}


def _md(value: Any, digits: int = 3) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value).replace("|", "\\|")


def render_qualified_signals_markdown(policy: QualificationPolicy, ledger: QualificationLedger | None = None) -> str:
    """The generated ``docs/research/QUALIFIED_SIGNALS.md`` (policy, and the ledger when given)."""
    split = policy.split
    segments = "; ".join(f"{name} {start.isoformat()}..{end.isoformat()}" for name, start, end in split.segments)
    size_rule = f"`{policy.size_venue_basis}`" + (
        "" if policy.size_min_venue_pit_share is None
        else f" with point-in-time venue share >= {policy.size_min_venue_pit_share:g}") + (
        "" if policy.size_fallback_venue is None
        else f", else the labeled `{policy.size_fallback_venue}` fallback")
    pinned: list[str] = []
    if policy.evaluation_spec is not None:
        spec = policy.evaluation_spec
        pinned = [
            f"- Pinned evaluation inputs (a run whose sealed spec differs is refused): horizons "
            f"{', '.join(str(h) for h in spec['horizons_months'])} months; subperiods "
            + "; ".join(f"`{name}` {start}..{end}" for name, start, end in spec["subperiods"])
            + f"; min_names {spec['min_names']}, min_bucket_names {spec['min_bucket_names']}, nyse_min_names "
            f"{spec['nyse_min_names']}, min_formations {spec['min_formations']}, verify_panels "
            f"{str(spec['verify_panels']).lower()}. Build the RR4 spec with `evaluation_spec_kwargs(policy)`.",
            "- RX7 order: `research_qualify.py freeze` records this policy in the research store before RR4; a run "
            "created before the freeze, or evaluated under another policy file, is refused (an explicit post-hoc "
            "override is stamped on the ledger, every row and this document).",
        ]
    if policy.strict_identity_bases is not None:
        pinned.append(f"- Strict evidence only with identity basis {', '.join(policy.strict_identity_bases)}, "
                      f"universe basis {', '.join(policy.strict_universe_bases or ())} and universe scope "
                      f"{', '.join(policy.strict_universe_scopes or ())}.")
    lines = [
        "# Qualified research signals",
        "",
        "Generated by `atx_db.research.qualification.render_qualified_signals_markdown()` "
        "(`scripts/research_qualify.py`); do not edit by hand.",
        "",
        "## Frozen policy",
        "",
        f"- Policy `{policy.version}`, content sha256 `{policy.sha256}` "
        "(`seeds/research_qualification_policy.json`, pinned in `FROZEN_POLICY_SHA256`).",
        f"- Split `{split.split_version}` (sha256 `{split.sha256}`): {segments}; embargo {split.embargo_months} "
        "months. Selection = train + validation formations whose label window ends before the holdout; "
        "the holdout is never used for selection.",
        f"- Primary cell: {policy.primary_horizon}-month horizon, `{policy.primary_variant}` variant, selection "
        f"sample; reported horizons {', '.join(str(h) for h in policy.reported_horizons)} months.",
        *pinned,
        "",
        "| order | gate | rule | failing status |",
        "|---:|---|---|---|",
        f"| 1 | coverage | cell tested; >= {policy.min_observations} IC observations; value coverage >= "
        f"{policy.min_value_coverage:g} on >= {policy.min_formation_share:g} of selection formations | "
        f"`{INSUFFICIENT_COVERAGE}` |",
        f"| 2 | significance (rank-IC family, EWC z) | HLZ \\|z\\| >= {policy.hlz_min_abs_z:g}, or BH q <= "
        f"{policy.bh_max_q:g} with \\|z\\| >= {policy.bh_min_abs_z:g} | `{NOT_SIGNIFICANT}` |",
        f"| 3 | sign | z > 0 on the sign-oriented value; two-sided rows fix their realized sign | `{SIGN_REVERSED}` |",
        f"| 4 | stability | holdout IC, >= {policy.subperiod_min_same_sign} of {len(policy.subperiods)} subperiods, "
        f"{' and '.join(policy.size_buckets)} size buckets ({size_rule}) keep the sign | `{UNSTABLE}` |",
        f"| 5 | strength | holdout z >= {policy.holdout_min_signed_z:g};"
        + (f" deflated Sharpe z > {policy.dsr_min_z:g} (whole family, horizon h);" if policy.dsr_gating else "")
        + f" size buckets with >= {policy.size_min_formations} formations | `{CANDIDATE}` |",
        "",
        "Supporting evidence (reported, never gating): "
        + ("" if policy.dsr_gating else f"deflated Sharpe (`dsr_pass` = z > {policy.dsr_min_z:g} over the whole "
           "family at horizon h) and PSR; ")
        + "decile spreads (EW/VW, NYSE breakpoints), monotonicity >= "
        f"{policy.monotonicity_min:g}, net spread at {policy.net_cost_bps} bp, Fama-MacBeth, "
        f"{', '.join(policy.neutral_variants)} survival (\\|z\\| >= {policy.neutral_min_abs_z:g}), other horizons.",
        "",
        "| status | meaning |",
        "|---|---|",
        *(f"| `{status}` | {meaning} |" for status, meaning in _STATUS_MEANING.items()),
        "",
        "## Ledger",
        "",
    ]
    if ledger is None:
        lines += [
            "No qualification ledger yet: the heavy evaluation (RR4) and this qualification step (RR5) have not run "
            "on real data. Run `scripts/research_qualify.py qualify --run-id <sealed R3b run>` to publish one.",
            "",
        ]
        return "\n".join(lines)
    manifest = ledger.manifest
    evaluation = manifest["evaluation"]
    if manifest.get("post_hoc_policy"):
        lines += [f"**POST-HOC POLICY: this ledger was graded under a policy that was not frozen before the "
                  f"evaluation run ({', '.join(manifest.get('post_hoc_reasons') or ())}). Its statuses are not "
                  "a pre-registered test.**", ""]
    lines += [
        f"- Ledger `{ledger.ledger_id}`, sha256 `{ledger.sha256}` ({manifest['qualification_version']}, code "
        f"`{str(manifest.get('qualification_code_sha256'))[:12]}`).",
        f"- Evaluation run `{evaluation['run_id']}`, results sha256 `{evaluation['results_sha256']}`, family "
        f"n_trials {evaluation['n_trials']}, tested cells {evaluation['tested_cells']}.",
        f"- Blockers: {', '.join(f'`{b}`' for b in manifest['blockers']) or 'none'}.",
        "",
        "| status | features |",
        "|---|---:|",
        *(f"| `{status}` | {count} |" for status, count in manifest["status_counts"].items()),
        "",
    ]
    header = ("| feature | class | status | basis | dir | IC | z | BH q | DSR z | holdout z | coverage | reasons |",
              "|---|---|---|---|:-:|---:|---:|---:|---:|---:|---:|---|")
    for title, wanted in (("Qualified", QUALIFIED_STATUSES), ("Candidates", frozenset({CANDIDATE})),
                          ("All other features", frozenset(STATUSES) - QUALIFIED_STATUSES - {CANDIDATE})):
        chosen = [row for row in ledger.features if row["status"] in wanted]
        lines += [f"### {title} ({len(chosen)})", ""]
        if not chosen:
            lines += ["None.", ""]
            continue
        lines += list(header)
        order = {status: i for i, status in enumerate(STATUSES)}
        for row in sorted(chosen, key=lambda r: (order[r["status"]], -(abs(r["ic_z"] or 0.0)), r["feature_id"])):
            direction = {1: "+", -1: "-"}.get(row["raw_direction"], "")
            lines.append(
                f"| `{row['feature_id']}` | {_md(row['anomaly_class'])} | `{row['status']}` | {_md(row['status_basis'])} "
                f"| {direction} | {_md(row['ic_mean'], 4)} | {_md(row['ic_z'], 2)} | {_md(row['bh_q'], 4)} "
                f"| {_md(row['dsr_z'], 2)} | {_md(row['holdout_ic_z'], 2)} | {_md(row['coverage_share'], 2)} "
                f"| {_md(row['status_reasons'])} |")
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Policy v4: pre-registered waves (tier-1 v2 node 1.10)
# ---------------------------------------------------------------------------

V4_QUALIFIED_STRICT, V4_QUALIFIED_RECONSTRUCTED = QUALIFIED_STRICT, QUALIFIED_RECONSTRUCTED
SELECTION_PASS = "selection_pass"
HOLDOUT_FAILED = "holdout_failed"
NOT_INVESTABLE = "not_investable"
INSUFFICIENT_EVIDENCE = "insufficient_evidence"
REPORTED_ONLY = "reported_only"
V4_STATUSES = (V4_QUALIFIED_STRICT, V4_QUALIFIED_RECONSTRUCTED, SELECTION_PASS, HOLDOUT_FAILED, NOT_INVESTABLE,
               UNSTABLE, INSUFFICIENT_EVIDENCE, NOT_SIGNIFICANT, SIGN_REVERSED, INSUFFICIENT_COVERAGE, REPORTED_ONLY)
#: Statuses that passed every selection-sample gate (qualified ones also passed the holdout).
V4_PASSING = frozenset({V4_QUALIFIED_STRICT, V4_QUALIFIED_RECONSTRUCTED, SELECTION_PASS})
REPLICATION, DISCOVERY = "replication", "discovery"
GRADE_PROVISIONAL, GRADE_FINAL = "provisional_labels", "final_labels"
GRADE_BASES = (GRADE_PROVISIONAL, GRADE_FINAL)
#: Reported subperiods of a v4 spec (not gating in v4): the three selection-sample subperiods.
V4_REPORTED_SUBPERIODS: tuple[tuple[str, dt.date, dt.date], ...] = (
    ("sub_2013_2016", dt.date(2013, 1, 1), dt.date(2016, 12, 31)),
    ("sub_2017_2020", dt.date(2017, 1, 1), dt.date(2020, 12, 31)),
    ("sub_2021_2023", dt.date(2021, 1, 1), dt.date(2023, 12, 31)),
)
_V4_KEYS = frozenset({
    "policy_id", "title", "frozen_on", "authority", "primary_cell", "gating_family", "reported_family",
    "investable_slice", "replication_gate", "discovery_gate", "holdout", "coverage", "long_horizon", "split",
    "inference", "trial_registry", "evaluation_split", "evaluation_inputs", "evidence_classes", "grade_basis",
    "reported_only", "two_sided_rows", "strict_evidence", "methods", "statuses", "policy_sha256"})
_V4_SECTIONS: Mapping[str, frozenset[str]] = {
    "primary_cell": frozenset({"variant", "horizon_months"}),
    "investable_slice": frozenset({"min_price_usd", "min_me_percentile_nyse"}),
    "replication_gate": frozenset({"one_sided_p_max", "bh_q", "investable_one_sided_p_max",
                                   "size_bucket_sign_agreement"}),
    "discovery_gate": frozenset({"hlz_abs_z_min", "dsr_min", "bh_q", "investable_one_sided_p_max"}),
    "holdout": frozenset({"rule", "shrink_test_one_sided_p_min", "opened_once_per_wave", "requires_final_labels"}),
    "coverage": frozenset({"against", "min_share", "min_formation_share"}),
    "long_horizon": frozenset({"method", "holding_months"}),
    "split": frozenset({"selection_end", "holdout_start"}),
    "evaluation_inputs": frozenset({"horizons_months", "min_names", "min_bucket_names", "nyse_min_names",
                                    "min_formations", "meaning"}),
    "reported_only": frozenset({"feature_ids", "rule"}),
    "strict_evidence": frozenset({"basis", "identity_bases", "universe_bases", "universe_scopes"}),
    "grade_basis": frozenset(GRADE_BASES),
    "evidence_classes": frozenset({"source", REPLICATION, DISCOVERY}),
}
#: The only implemented semantics of v4's named rules (a policy cannot name a rule the code lacks).
_V4_FIXED = {"gating_family": "primary_cells_over_eligible_hypotheses", "reported_family": "all_cells",
             "inference": "ewc_fixed_b", "trial_registry": "cumulative_across_waves"}


@dataclass(frozen=True)
class QualificationPolicyV4:
    """A parsed, hash-checked policy v4 (see the v4 section of the module docstring)."""

    version: str
    sha256: str
    content: Mapping[str, Any]
    split: FrozenSplit
    primary_variant: str
    primary_horizon: int
    selection_end: str
    holdout_start: str
    min_price_usd: float
    min_me_percentile_nyse: int
    replication_one_sided_p_max: float
    replication_bh_q: float
    replication_investable_p_max: float
    replication_size_signs: bool
    discovery_hlz_z: float
    discovery_dsr_min: float
    discovery_bh_q: float
    discovery_investable_p_max: float
    holdout_rule: str
    shrink_p_min: float
    holdout_once: bool
    holdout_final_labels: bool
    coverage_min_share: float
    coverage_min_formation_share: float
    jt_holding_months: tuple[int, ...]
    evaluation_inputs: Mapping[str, Any]
    reported_only: tuple[str, ...]
    strict_basis: str
    strict_identity_bases: tuple[str, ...]
    strict_universe_bases: tuple[str, ...]
    strict_universe_scopes: tuple[str, ...]
    file_sha256s: frozenset[str] = field(default_factory=frozenset)
    file_sha256: str | None = None

    @property
    def evaluation_spec(self) -> Mapping[str, Any]:
        """The pinned evaluation inputs (the CLI prints them as the policy's evaluation spec)."""
        return self.evaluation_inputs

    @property
    def min_formations(self) -> int:
        return int(self.evaluation_inputs["min_formations"])


def _v4_section(content: Mapping[str, Any], name: str) -> Mapping[str, Any]:
    section = content.get(name)
    if not isinstance(section, Mapping) or set(section) != _V4_SECTIONS[name]:
        raise PolicyError(f"policy v4 section {name!r} must hold exactly {sorted(_V4_SECTIONS[name])}")
    return section


def _month_text(value: Any, label: str) -> dt.date:
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}", value):
        raise PolicyError(f"policy v4 {label} must be YYYY-MM")
    return dt.date(int(value[:4]), int(value[5:]), 1)


def _load_policy_v4(content: Mapping[str, Any], file_hashes: frozenset[str], file_sha: str | None, *,
                    allow_unpinned: bool) -> QualificationPolicyV4:
    if set(content) != _V4_KEYS:
        raise PolicyError(f"policy v4 keys must be {sorted(_V4_KEYS)}; unknown {sorted(set(content) - _V4_KEYS)}, "
                          f"missing {sorted(_V4_KEYS - set(content))}")
    version = content["policy_id"]
    if not isinstance(version, str) or not 0 < len(version) <= 64:
        raise PolicyError("policy_id must be a short string")
    actual = policy_sha256(content)
    if content["policy_sha256"] != actual:
        raise PolicyError(f"policy {version} is not frozen: declared policy_sha256 {content['policy_sha256']!r} "
                          f"!= content sha256 {actual}")
    pinned = FROZEN_POLICY_SHA256.get(version)
    if pinned is not None and pinned != actual:
        raise PolicyError(f"policy {version} is frozen with sha256 {pinned}; its content changed ({actual}). "
                          "A changed policy needs a new policy_id")
    if pinned is None and not allow_unpinned:
        raise PolicyError(f"policy {version} is not pinned in FROZEN_POLICY_SHA256")
    for key, expected in _V4_FIXED.items():
        if content[key] != expected:
            raise PolicyError(f"policy v4 {key} must be {expected!r} (the only one implemented)")
    if list(content["statuses"]) != list(V4_STATUSES):
        raise PolicyError(f"policy v4 statuses must be exactly {list(V4_STATUSES)}")
    primary = _v4_section(content, "primary_cell")
    investable = _v4_section(content, "investable_slice")
    replication = _v4_section(content, "replication_gate")
    discovery = _v4_section(content, "discovery_gate")
    holdout = _v4_section(content, "holdout")
    coverage = _v4_section(content, "coverage")
    long_horizon = _v4_section(content, "long_horizon")
    split_months = _v4_section(content, "split")
    inputs = _v4_section(content, "evaluation_inputs")
    reported = _v4_section(content, "reported_only")
    strict = _v4_section(content, "strict_evidence")
    _v4_section(content, "grade_basis")
    _v4_section(content, "evidence_classes")
    if holdout["rule"] != "sign_consistent_and_no_significant_shrink" or coverage["against"] != "catalog_population" \
            or long_horizon["method"] != "jegadeesh_titman_calendar_time":
        raise PolicyError("policy v4 names a holdout rule, coverage denominator or long-horizon method the code lacks")
    for section, key in (("holdout", "opened_once_per_wave"), ("holdout", "requires_final_labels"),
                         ("replication_gate", "size_bucket_sign_agreement")):
        if not isinstance(content[section][key], bool):
            raise PolicyError(f"policy v4 {section}.{key} must be a boolean")
    try:
        split = load_frozen_split(content["evaluation_split"])
    except ValueError as error:
        raise PolicyError(f"policy v4 evaluation_split: {error}") from error
    selection_end, holdout_start = (_month_text(split_months["selection_end"], "split.selection_end"),
                                    _month_text(split_months["holdout_start"], "split.holdout_start"))
    validation_end = split.segments[-2][2]
    if (validation_end.year, validation_end.month) != (selection_end.year, selection_end.month) \
            or split.holdout_start != holdout_start:
        raise PolicyError("policy v4 split months disagree with its evaluation_split")
    horizons = inputs["horizons_months"]
    if not isinstance(horizons, list) or sorted(set(horizons)) != horizons or \
            any(isinstance(h, bool) or not isinstance(h, int) for h in horizons):
        raise PolicyError("evaluation_inputs.horizons_months must be ascending distinct integers")
    horizon = _count(primary, "horizon_months", 1, 12)
    holding = long_horizon["holding_months"]
    if not isinstance(holding, list) or sorted(set(holding)) != holding or not holding or \
            any(isinstance(k, bool) or not isinstance(k, int) or not 1 <= k <= 12 for k in holding):
        raise PolicyError("long_horizon.holding_months must be ascending distinct integers 1..12")
    if horizon not in horizons or 1 not in horizons:
        raise PolicyError("the primary horizon and the 1-month labels (JT holdings) must be evaluated")
    reported_ids = _names({"ids": reported["feature_ids"]}, "ids")
    return QualificationPolicyV4(
        version=version, sha256=actual, content=content, split=split,
        primary_variant=str(primary["variant"]), primary_horizon=horizon,
        selection_end=str(split_months["selection_end"]), holdout_start=str(split_months["holdout_start"]),
        min_price_usd=_number(investable, "min_price_usd", 0.0, 10_000.0),
        min_me_percentile_nyse=_count(investable, "min_me_percentile_nyse", 1, 99),
        replication_one_sided_p_max=_number(replication, "one_sided_p_max", 0.0, 1.0),
        replication_bh_q=_number(replication, "bh_q", 0.0, 1.0),
        replication_investable_p_max=_number(replication, "investable_one_sided_p_max", 0.0, 1.0),
        replication_size_signs=bool(replication["size_bucket_sign_agreement"]),
        discovery_hlz_z=_number(discovery, "hlz_abs_z_min", 0.0, 10.0),
        discovery_dsr_min=_number(discovery, "dsr_min", 0.0, 1.0),
        discovery_bh_q=_number(discovery, "bh_q", 0.0, 1.0),
        discovery_investable_p_max=_number(discovery, "investable_one_sided_p_max", 0.0, 1.0),
        holdout_rule=str(holdout["rule"]), shrink_p_min=_number(holdout, "shrink_test_one_sided_p_min", 0.0, 1.0),
        holdout_once=bool(holdout["opened_once_per_wave"]), holdout_final_labels=bool(holdout["requires_final_labels"]),
        coverage_min_share=_number(coverage, "min_share", 0.0, 1.0),
        coverage_min_formation_share=_number(coverage, "min_formation_share", 0.0, 1.0),
        jt_holding_months=tuple(holding),
        evaluation_inputs={"horizons_months": list(horizons), "min_names": _count(inputs, "min_names", 3, 100_000),
                           "min_bucket_names": _count(inputs, "min_bucket_names", 5, 100_000),
                           "nyse_min_names": _count(inputs, "nyse_min_names", 1, 100_000),
                           "min_formations": _count(inputs, "min_formations", 2, 10_000)},
        reported_only=tuple(sorted(reported_ids)), strict_basis=str(strict["basis"]),
        strict_identity_bases=_names(strict, "identity_bases"), strict_universe_bases=_names(strict, "universe_bases"),
        strict_universe_scopes=_names(strict, "universe_scopes"),
        file_sha256s=file_hashes, file_sha256=file_sha)


def load_policy_v4(source: Path | str | Mapping[str, Any] = POLICY_V4_PATH) -> QualificationPolicyV4:
    """Load the frozen policy v4 (refuses anything that is not a v4 policy)."""
    policy = load_policy(source)
    if not isinstance(policy, QualificationPolicyV4):
        raise PolicyError(f"{source} is not a v4 qualification policy")
    return policy


def load_policy_v4_by_id(policy_id: str) -> QualificationPolicyV4:
    """The committed v4+ policy file of ``policy_id``."""
    if policy_id not in POLICY_V4_PATHS:
        raise PolicyError(f"unknown v4 policy {policy_id!r}")
    return load_policy_v4(POLICY_V4_PATHS[policy_id])


def evaluation_spec_kwargs_v4(policy: QualificationPolicyV4) -> dict[str, Any]:
    """The R3b ``EvaluationSpec`` keyword arguments a v4 wave run must use: the frozen split, the
    pinned inputs, the investable slice, the JT holdings, population coverage and the policy file
    hash (``EvaluationSpec(run_id=..., **evaluation_spec_kwargs_v4(policy))``). ``verify_panels`` is
    the caller's (a Parquet-store run has no R2a panel to verify)."""
    if policy.file_sha256 is None:
        raise PolicyError(f"policy {policy.version} was not loaded from a file")
    inputs = policy.evaluation_inputs
    return {"split": policy.split, "horizons_months": tuple(inputs["horizons_months"]),
            "subperiods": V4_REPORTED_SUBPERIODS, "min_names": inputs["min_names"],
            "min_bucket_names": inputs["min_bucket_names"], "nyse_min_names": inputs["nyse_min_names"],
            "min_formations": inputs["min_formations"], "investable_min_price": policy.min_price_usd,
            "investable_nyse_percentile": policy.min_me_percentile_nyse,
            "jt_holding_months": policy.jt_holding_months, "population_coverage": True,
            "policy_sha256": policy.file_sha256}


def v4_spec_problems(spec: Any, policy: QualificationPolicyV4) -> list[str]:
    """Why an evaluation spec (``EvaluationSpec`` or its payload) cannot feed a v4 grade (sorted)."""
    payload = spec if isinstance(spec, Mapping) else _spec_payload(spec)
    wanted = {**policy.evaluation_inputs, "investable_min_price": policy.min_price_usd,
              "investable_nyse_percentile": policy.min_me_percentile_nyse,
              "jt_holding_months": list(policy.jt_holding_months), "population_coverage": True}
    problems = [f"evaluation_spec_differs:{name}" for name, value in sorted(wanted.items())
                if payload.get(name) != value]
    split = payload.get("split")
    if not isinstance(split, Mapping) or split.get("sha256") != policy.split.sha256:
        problems.append("evaluation_split_differs")
    if payload.get("policy_sha256") not in policy.file_sha256s:
        problems.append("evaluation_policy_file_differs")
    return sorted(problems)


def _spec_payload(spec: Any) -> dict[str, Any]:
    from .evaluation import spec_payload, validate_spec

    return spec_payload(validate_spec(spec))


def one_sided_p(p_two_sided: float | None, t: float | None, direction: int) -> float | None:
    """One-sided p-value in ``direction`` from a two-sided p and its t: p/2 when direction*t > 0."""
    p, stat = _f(p_two_sided), _f(t)
    if p is None or stat is None:
        return None
    return p / 2.0 if direction * stat > 0 else 1.0 - p / 2.0


def holdout_shrink_test(selection_mean: float | None, selection_t: float | None, selection_df: float | None,
                        holdout_mean: float | None, holdout_t: float | None, holdout_df: float | None,
                        direction: int) -> tuple[float | None, float | None, float | None]:
    """Welch shrink test of the holdout IC against the selection IC in ``direction``.

    EWC standard errors ``|mean / t|``; ``t = d (m_h - m_s) / sqrt(se_h^2 + se_s^2)``,
    Welch-Satterthwaite df; returns ``(t, df, one-sided p = P(T <= t))`` (None when
    untestable). A small p means the holdout IC is significantly below the selection IC.
    """
    values = [_f(v) for v in (selection_mean, selection_t, selection_df, holdout_mean, holdout_t, holdout_df)]
    if any(v is None for v in values):
        return None, None, None
    m_s, t_s, df_s, m_h, t_h, df_h = (float(v) for v in values)  # type: ignore[arg-type]
    if t_s == 0 or t_h == 0 or df_s <= 0 or df_h <= 0:
        return None, None, None
    a, b = (m_h / t_h) ** 2, (m_s / t_s) ** 2
    if not a + b > 0:
        return None, None, None
    df = (a + b) ** 2 / (a * a / df_h + b * b / df_s)
    stat = direction * (m_h - m_s) / math.sqrt(a + b)
    p_two = stats.student_t_two_sided_p(stat, df)
    return stat, df, (p_two / 2.0 if stat < 0 else 1.0 - p_two / 2.0)


@dataclass(frozen=True)
class FeatureEvidence:
    """What a v4 grade reads for one feature on one basis (the primary cell and its slices).

    IC statistics are EWC fixed-b on sign-oriented values (a signed hypothesis is a positive
    IC); ``*_p`` are two-sided robust p-values, ``*_t`` robust t, ``*_df`` EWC df.
    ``coverage_share``: share of the feature's history-eligible selection formations with
    catalog-population coverage >= ``coverage.min_share``. Size buckets and the investable
    slice carry their formations; holdout fields are None unless the wave's holdout was opened
    on final labels.
    """

    feature_id: str
    basis: str
    evidence_class: str
    expected_sign: int
    reported_only: bool = False
    cell_status: str = "tested"
    strict_labels: bool = False
    ic_mean: float | None = None
    ic_t: float | None = None
    ic_df: float | None = None
    ic_p: float | None = None
    ic_n: int | None = None
    coverage_share: float | None = None
    coverage_formations: int = 0
    investable_mean: float | None = None
    investable_t: float | None = None
    investable_p: float | None = None
    investable_formations: int = 0
    small_mean: float | None = None
    small_formations: int = 0
    large_mean: float | None = None
    large_formations: int = 0
    size_venue_basis: str | None = None
    sharpe: float | None = None
    sharpe_skew: float | None = None
    sharpe_kurt: float | None = None
    sharpe_n: int | None = None
    sharpe_variance: float | None = None
    holdout_mean: float | None = None
    holdout_t: float | None = None
    holdout_df: float | None = None
    holdout_formations: int | None = None
    annotations: Mapping[str, Any] = field(default_factory=dict)


V4_ROW_COLUMNS: tuple[str, ...] = (
    "feature_id", "basis", "status", "status_reasons", "evidence_class", "evidence_basis", "reported_only",
    "expected_sign", "two_sided", "direction", "cell_status", "family_scope",
    "ic_mean", "ic_t", "ic_df", "ic_n", "ic_z", "p_two_sided", "p_one_sided", "p_against", "family_p", "gating_q",
    "dsr", "dsr_z", "dsr_n_trials", "coverage_share", "coverage_formations",
    "investable_ic_mean", "investable_p_one_sided", "investable_formations",
    "small_ic_mean", "small_formations", "large_ic_mean", "large_formations", "size_venue_basis",
    "gate_coverage", "gate_sign", "gate_significance", "gate_investable", "gate_size_buckets", "gate_holdout",
    "holdout_graded", "holdout_ic_mean", "holdout_formations", "holdout_sign_consistent", "holdout_shrink_t",
    "holdout_shrink_df", "holdout_shrink_p",
)
V4_FEATURE_COLUMNS: tuple[str, ...] = (
    "feature_id", "status", "status_basis", "status_reasons", *[c for c in V4_ROW_COLUMNS
                                                               if c not in ("feature_id", "basis", "status",
                                                                            "status_reasons")],
    "anomaly_class", "hypothesis_family", "jkp_theme", "population", "wave",
)


def _z_signed(p: float | None, t: float | None) -> float | None:
    p, t = _f(p), _f(t)
    if p is None or t is None:
        return None
    return math.copysign(stats.normal_equivalent_z(p), t) if t != 0 else 0.0


def _v4_direction(item: FeatureEvidence, z: float | None) -> int:
    if item.expected_sign != 0:
        return 1  # values are sign-oriented: the hypothesis is a positive IC
    return _signum(z) or _signum(_f(item.ic_mean)) or 1


def _v4_family_p(item: FeatureEvidence, p_one: float | None) -> float | None:
    if item.cell_status != "tested":
        return None
    return p_one if item.evidence_class == REPLICATION else _f(item.ic_p)


def _grade_v4_row(item: FeatureEvidence, policy: QualificationPolicyV4, q: float | None, *, n_trials: int,
                  grade_holdout: bool) -> dict[str, Any]:
    z = _z_signed(item.ic_p, item.ic_t)
    direction = _v4_direction(item, z)
    two_sided = item.expected_sign == 0
    replication = item.evidence_class == REPLICATION
    p_one = one_sided_p(item.ic_p, item.ic_t, direction)
    p_against = None if two_sided else one_sided_p(item.ic_p, item.ic_t, -1)
    tested = item.cell_status == "tested" and p_one is not None
    row: dict[str, Any] = {name: None for name in V4_ROW_COLUMNS}
    row.update({"feature_id": item.feature_id, "basis": item.basis, "evidence_class": item.evidence_class,
                "reported_only": item.reported_only, "expected_sign": item.expected_sign, "two_sided": two_sided,
                "direction": direction, "cell_status": item.cell_status,
                "family_scope": "reported" if item.reported_only else "gating",
                "ic_mean": _f(item.ic_mean), "ic_t": _f(item.ic_t), "ic_df": _f(item.ic_df), "ic_n": item.ic_n,
                "ic_z": z, "p_two_sided": _f(item.ic_p), "p_one_sided": p_one, "p_against": p_against,
                "family_p": None if item.reported_only else _v4_family_p(item, p_one), "gating_q": q,
                "coverage_share": _f(item.coverage_share), "coverage_formations": item.coverage_formations,
                "investable_ic_mean": _f(item.investable_mean), "investable_formations": item.investable_formations,
                "small_ic_mean": _f(item.small_mean), "small_formations": item.small_formations,
                "large_ic_mean": _f(item.large_mean), "large_formations": item.large_formations,
                "size_venue_basis": item.size_venue_basis, "holdout_graded": grade_holdout})
    evidence = "strict" if item.basis == policy.strict_basis and item.strict_labels else "reconstructed"
    row["evidence_basis"] = evidence
    # Deflated Sharpe of the direction-oriented EW decile long-short (discovery gate; reported for all).
    dsr = None
    sharpe, skew, kurt = _f(item.sharpe), _f(item.sharpe_skew), _f(item.sharpe_kurt)
    variance, n_obs = _f(item.sharpe_variance), item.sharpe_n
    if None not in (sharpe, skew, kurt, variance) and n_obs and n_trials >= 1:
        assert sharpe is not None and skew is not None and kurt is not None and variance is not None
        with suppress(ValueError, ArithmeticError):
            deflated = stats.deflated_sharpe_ratio(direction * sharpe, n_obs=int(n_obs), skewness=direction * skew,
                                                   kurtosis=kurt, n_trials=int(n_trials), sharpe_variance=variance,
                                                   horizon_periods=policy.primary_horizon)
            dsr = deflated.deflated_sharpe_ratio
            row["dsr_z"] = _f(deflated.z)
    row.update({"dsr": _f(dsr), "dsr_n_trials": n_trials})
    investable_p = one_sided_p(item.investable_p, item.investable_t, direction)
    row["investable_p_one_sided"] = investable_p
    if item.reported_only:
        row.update({"status": REPORTED_ONLY, "status_reasons": "reported_only_never_gated"})
        return row

    # 1. Coverage of the catalog population on the history-eligible selection formations.
    coverage_fail = []
    if not tested:
        coverage_fail.append(f"cell_{item.cell_status}")
    share = _f(item.coverage_share)
    if share is None or share < policy.coverage_min_formation_share:
        coverage_fail.append(f"coverage_share={_g(share)}<{policy.coverage_min_formation_share:g}")
    if item.coverage_formations < policy.min_formations:
        coverage_fail.append(f"coverage_formations={item.coverage_formations}<{policy.min_formations}")
    row["gate_coverage"] = not coverage_fail
    if coverage_fail:
        row.update({"status": INSUFFICIENT_COVERAGE, "status_reasons": "; ".join(coverage_fail)})
        return row
    # 2. Sign against a pre-registered direction.
    if replication:
        reversed_sign = p_against is not None and p_against <= policy.replication_one_sided_p_max
    else:
        reversed_sign = not two_sided and z is not None and z <= -policy.discovery_hlz_z
    row["gate_sign"] = not reversed_sign
    if reversed_sign:
        row.update({"status": SIGN_REVERSED, "status_reasons": f"ic_z={_g(z)}_against_the_pre_registered_sign"})
        return row
    # 3. Significance of the evidence class (the gating family's BH q included).
    reasons: list[str] = []
    if replication:
        if p_one is None or p_one > policy.replication_one_sided_p_max:
            reasons.append(f"p_one_sided={_g(p_one)}>{policy.replication_one_sided_p_max:g}")
        if q is None or q > policy.replication_bh_q:
            reasons.append(f"bh_q={_g(q)}>{policy.replication_bh_q:g}")
    else:
        if z is None or direction * z < policy.discovery_hlz_z:
            reasons.append(f"|z|={_g(None if z is None else abs(z))}<{policy.discovery_hlz_z:g}")
        if q is None or q > policy.discovery_bh_q:
            reasons.append(f"bh_q={_g(q)}>{policy.discovery_bh_q:g}")
        if dsr is None or dsr < policy.discovery_dsr_min:
            reasons.append(f"dsr={_g(dsr)}<{policy.discovery_dsr_min:g}")
    row["gate_significance"] = not reasons
    if reasons:
        row.update({"status": NOT_SIGNIFICANT, "status_reasons": "; ".join(reasons)})
        return row
    # 4. Investable co-primary cell.
    p_max = policy.replication_investable_p_max if replication else policy.discovery_investable_p_max
    if investable_p is None or item.investable_formations < policy.min_formations:
        row.update({"gate_investable": None, "status": INSUFFICIENT_EVIDENCE,
                    "status_reasons": f"investable_untestable(formations={item.investable_formations})"})
        return row
    row["gate_investable"] = investable_p <= p_max
    if not row["gate_investable"]:
        row.update({"status": NOT_INVESTABLE, "status_reasons": f"investable_p_one_sided={_g(investable_p)}>{p_max:g}"})
        return row
    # 5. Size buckets (replication): small and large keep the sign.
    if replication and policy.replication_size_signs:
        buckets = (("small", _f(item.small_mean), item.small_formations),
                   ("large", _f(item.large_mean), item.large_formations))
        missing = [name for name, mean, formations in buckets if mean is None or formations < policy.min_formations]
        if missing:
            row.update({"gate_size_buckets": None, "status": INSUFFICIENT_EVIDENCE,
                        "status_reasons": f"size_buckets_untestable({','.join(missing)};{item.size_venue_basis})"})
            return row
        flipped = [f"size_{name}_sign(ic={_g(mean)})" for name, mean, _ in buckets
                   if mean is not None and not direction * mean > 0]
        row["gate_size_buckets"] = not flipped
        if flipped:
            row.update({"status": UNSTABLE, "status_reasons": "; ".join(flipped)})
            return row
    # 6. Holdout (final labels, opened once): the sign holds and no significant shrink.
    if not grade_holdout:
        row.update({"status": SELECTION_PASS, "status_reasons": "holdout_sealed"})
        return row
    hold_mean = _f(item.holdout_mean)
    stat, df, p_shrink = holdout_shrink_test(item.ic_mean, item.ic_t, item.ic_df, item.holdout_mean,
                                             item.holdout_t, item.holdout_df, direction)
    row.update({"holdout_ic_mean": hold_mean, "holdout_formations": item.holdout_formations,
                "holdout_shrink_t": stat, "holdout_shrink_df": df, "holdout_shrink_p": p_shrink})
    if hold_mean is None or p_shrink is None:
        row.update({"gate_holdout": None, "status": INSUFFICIENT_EVIDENCE, "status_reasons": "holdout_untestable"})
        return row
    sign_ok = direction * hold_mean > 0
    row["holdout_sign_consistent"] = sign_ok
    row["gate_holdout"] = sign_ok and p_shrink >= policy.shrink_p_min
    if not row["gate_holdout"]:
        why = [] if sign_ok else [f"holdout_sign_flip(ic={_g(hold_mean)})"]
        if p_shrink < policy.shrink_p_min:
            why.append(f"holdout_shrink_p={_g(p_shrink)}<{policy.shrink_p_min:g}")
        row.update({"status": HOLDOUT_FAILED, "status_reasons": "; ".join(why)})
        return row
    row.update({"status": V4_QUALIFIED_STRICT if evidence == "strict" else V4_QUALIFIED_RECONSTRUCTED,
                "status_reasons": None if evidence == "strict" else "reconstructed_evidence"})
    return row


def grade_v4(evidence: Sequence[FeatureEvidence], policy: QualificationPolicyV4, *, n_trials: int,
             prior_gating_hypotheses: int = 0, grade_holdout: bool = False) -> list[dict[str, Any]]:
    """Grade every (feature, basis) row of one wave under policy v4 (pure; no I/O).

    Per basis, the gating family's BH q-values are computed over the non-reported-only rows at
    their evidence-class p-values (untested = p 1) plus ``prior_gating_hypotheses`` earlier-wave
    hypotheses at p = 1 (``evaluation.gating_bh_q``). ``n_trials`` is the trial registry's
    cumulative configuration count (DSR). ``grade_holdout`` only for a wave whose holdout was
    opened on final labels. Returns one row per input (``V4_ROW_COLUMNS``), in input order.
    """
    from .evaluation import gating_bh_q

    if isinstance(n_trials, bool) or not isinstance(n_trials, int) or n_trials < 1:
        raise QualificationError("n_trials must be a positive integer (trial_registry.trials_so_far())")
    keys = [(item.basis, item.feature_id) for item in evidence]
    if len(set(keys)) != len(keys):
        raise QualificationError("one evidence row per (basis, feature)")
    q_values: dict[tuple[str, str], float | None] = {}
    for basis in sorted({item.basis for item in evidence}):
        members = {}
        for item in evidence:
            if item.basis != basis or item.reported_only:
                continue
            z = _z_signed(item.ic_p, item.ic_t)
            members[item.feature_id] = _v4_family_p(item, one_sided_p(item.ic_p, item.ic_t,
                                                                       _v4_direction(item, z)))
        for feature_id, q in gating_bh_q(members, prior_hypotheses=prior_gating_hypotheses).items():
            q_values[(basis, feature_id)] = q
    return [_grade_v4_row(item, policy, q_values.get((item.basis, item.feature_id)), n_trials=n_trials,
                          grade_holdout=grade_holdout) for item in evidence]


def _resolve_v4(rows: Mapping[str, Mapping[str, Any]], policy: QualificationPolicyV4) -> tuple[str, str, str | None]:
    """(status, status_basis, reasons) of one feature: RX1 reference-basis reporting with a symmetric veto."""
    order = sorted(rows, key=lambda b: (b == policy.strict_basis, b != REFERENCE_BASIS, b))
    passing = [b for b in order if rows[b]["status"] in V4_PASSING]
    reversed_bases = [b for b in order if rows[b]["status"] == SIGN_REVERSED]
    if passing and reversed_bases:
        return UNSTABLE, reversed_bases[0], f"{reversed_bases[0]}_basis_sign_reversed"
    strict = rows.get(policy.strict_basis)
    if strict is not None and strict["status"] == V4_QUALIFIED_STRICT:
        return V4_QUALIFIED_STRICT, policy.strict_basis, strict["status_reasons"]
    basis = order[0]
    return str(rows[basis]["status"]), basis, rows[basis]["status_reasons"]


def _catalog_by_id(catalog: Iterable[Any]) -> dict[str, Any]:
    return {str(entry.feature_id): entry for entry in catalog}


def _selection_coverage(series: pd.DataFrame, basis: str, feature_id: str, first: str | None,
                        policy: QualificationPolicyV4) -> tuple[float | None, int]:
    part = series[(series["basis"].astype(str) == basis) & (series["feature_id"].astype(str) == feature_id)]
    if not len(part):
        return None, 0
    chosen = part["in_selection"].fillna(False).astype(bool).to_numpy()
    if first is not None:
        chosen &= pd.to_datetime(part["formation_date"]).to_numpy() >= np.datetime64(first)
    coverage = pd.to_numeric(part["coverage"], errors="coerce").to_numpy(dtype=float)[chosen]
    if not len(coverage):
        return None, 0
    covered = np.isfinite(coverage) & (coverage >= policy.coverage_min_share)
    return float(covered.mean()), len(coverage)


def evidence_from_evaluation(cells: pd.DataFrame, slices: pd.DataFrame, series: pd.DataFrame,
                             policy: QualificationPolicyV4, *, spec: Any, catalog: Iterable[Any],
                             registration: Any, grade_holdout: bool = False) -> list[FeatureEvidence]:
    """:class:`FeatureEvidence` rows of a wave run's R3b result frames (every registered feature x basis).

    ``cells`` / ``slices`` / ``series``: R3b ``research_eval_*`` frames of the wave run
    (``series`` at the primary variant and horizon is enough); ``spec`` the run's
    ``EvaluationSpec`` (or payload); ``catalog`` the catalog entries the wave was registered
    from; ``registration`` its ``trial_registry.WaveRegistration``. Holdout slices are read
    only with ``grade_holdout``. Raises :class:`QualificationRefused` for a spec that differs
    from the policy's pinned inputs, a feature evaluated but not registered, a registered
    feature missing from the catalog or a sign that differs from it.
    """
    reasons = v4_spec_problems(spec, policy)
    by_id = _catalog_by_id(catalog)
    registered = tuple(registration.feature_ids)
    reported = set(registration.reported_only)
    first_formations = dict(getattr(registration, "first_formations", {}) or {})
    evaluated = set(cells["feature_id"].astype(str)) if len(cells) else set()
    if evaluated - set(registered):
        reasons.append(f"features_not_registered:{len(evaluated - set(registered))}")
    if set(registered) - set(by_id):
        reasons.append(f"registered_features_not_in_catalog:{len(set(registered) - set(by_id))}")
    h, variant = policy.primary_horizon, policy.primary_variant
    primary = cells[(cells["variant"].astype(str) == variant) & (pd.to_numeric(cells["horizon_months"]) == h)] \
        if len(cells) else cells
    for record in primary.to_dict("records"):
        entry = by_id.get(str(record["feature_id"]))
        if entry is not None and _i(record.get("expected_sign")) != int(entry.expected_sign):
            reasons.append(f"expected_sign_differs_from_catalog:{record['feature_id']}")
    if reasons:
        raise QualificationRefused(sorted(set(reasons)))
    cell_index = {(str(r["basis"]), str(r["feature_id"])): r for r in primary.to_dict("records")}
    slice_index = _index(slices[(slices["variant"].astype(str) == variant)
                                & (pd.to_numeric(slices["horizon_months"]) == h)],
                         ("basis", "feature_id", "slice_kind", "slice_name")) if len(slices) else {}
    primary_series = series[(series["variant"].astype(str) == variant)
                            & (pd.to_numeric(series["horizon_months"]) == h)] if len(series) else series
    bases = sorted(set(cells["basis"].astype(str))) if len(cells) else []
    from .evaluation import INVESTABLE_SLICE_KIND, INVESTABLE_SLICE_NAME, SUPPLIED_SIZE_SLICE_KIND

    out: list[FeatureEvidence] = []
    for basis in bases:
        for feature_id in registered:
            entry = by_id[feature_id]
            cell = cell_index.get((basis, feature_id), {})
            coverage, formations = _selection_coverage(primary_series, basis, feature_id,
                                                       first_formations.get(feature_id), policy) \
                if len(primary_series) else (None, 0)
            inv = slice_index.get((basis, feature_id, INVESTABLE_SLICE_KIND, INVESTABLE_SLICE_NAME), {})
            size_venue, small, large = None, {}, {}
            for kind in (SUPPLIED_SIZE_SLICE_KIND, "size_bucket", "size_tercile"):
                s, lg = (slice_index.get((basis, feature_id, kind, name), {}) for name in ("small", "large"))
                if all(_f(x.get("ic_mean")) is not None and (_i(x.get("formations")) or 0) >= policy.min_formations
                       for x in (s, lg)):
                    size_venue, small, large = kind, s, lg
                    break
            hold = slice_index.get((basis, feature_id, "split", "holdout"), {}) if grade_holdout else {}
            labels_strict = (_s(cell.get("identity_basis")) in policy.strict_identity_bases
                             and _s(cell.get("universe_basis")) in policy.strict_universe_bases
                             and _s(cell.get("universe_scope")) in policy.strict_universe_scopes)
            out.append(FeatureEvidence(
                feature_id=feature_id, basis=basis, evidence_class=str(entry.evidence_class),
                expected_sign=int(entry.expected_sign), reported_only=feature_id in reported,
                cell_status=str(cell.get("status") or "not_produced"), strict_labels=bool(labels_strict),
                ic_mean=_f(cell.get("ic_mean")), ic_t=_f(cell.get("ic_robust_t")), ic_df=_f(cell.get("ic_robust_df")),
                ic_p=_f(cell.get("ic_robust_p")), ic_n=_i(cell.get("ic_n")), coverage_share=coverage,
                coverage_formations=formations,
                investable_mean=_f(inv.get("ic_mean")), investable_t=_f(inv.get("ic_robust_t")),
                investable_p=_f(inv.get("ic_robust_p")), investable_formations=_i(inv.get("formations")) or 0,
                small_mean=_f(small.get("ic_mean")), small_formations=_i(small.get("formations")) or 0,
                large_mean=_f(large.get("ic_mean")), large_formations=_i(large.get("formations")) or 0,
                size_venue_basis=size_venue, sharpe=_f(cell.get("sharpe")), sharpe_skew=_f(cell.get("sharpe_skew")),
                sharpe_kurt=_f(cell.get("sharpe_kurt")), sharpe_n=_i(cell.get("sharpe_n")),
                sharpe_variance=_f(cell.get("dsr_sharpe_variance")),
                holdout_mean=_f(hold.get("ic_mean")), holdout_t=_f(hold.get("ic_robust_t")),
                holdout_df=_f(hold.get("ic_robust_df")), holdout_formations=_i(hold.get("formations")),
                annotations={"anomaly_class": _s(getattr(entry, "anomaly_class", None)),
                             "hypothesis_family": _s(getattr(entry, "hypothesis_family", None)),
                             "jkp_theme": _s(getattr(entry, "jkp_theme", None)),
                             "population": _s(getattr(entry, "population", None)),
                             "wave": _s(getattr(entry, "wave", None))}))
    return out


@dataclass(frozen=True)
class V4Ledger:
    """A sealed v4 wave ledger: manifest, one row per feature, one row per feature x basis."""

    ledger_id: str
    manifest: Mapping[str, Any]
    features: tuple[Mapping[str, Any], ...]
    bases: tuple[Mapping[str, Any], ...]
    sha256: str

    def payload(self) -> dict[str, Any]:
        return {"manifest": dict(self.manifest), "features": [dict(r) for r in self.features],
                "bases": [dict(r) for r in self.bases], "ledger_sha256": self.sha256}

    def to_json_bytes(self) -> bytes:
        text = json.dumps(self.payload(), sort_keys=True, indent=1, ensure_ascii=True, allow_nan=False)
        return (text + "\n").encode("ascii")

    def to_csv_bytes(self) -> bytes:
        buffer = io.StringIO()
        writer = csv.writer(buffer, lineterminator="\n")
        writer.writerow(V4_FEATURE_COLUMNS)
        for row in self.features:
            writer.writerow([_csv_cell(row.get(name)) for name in V4_FEATURE_COLUMNS])
        return buffer.getvalue().encode("utf-8")

    def status_of(self, feature_id: str) -> str:
        for row in self.features:
            if row["feature_id"] == feature_id:
                return str(row["status"])
        raise KeyError(feature_id)


def ledger_from_evidence_v4(evidence: Sequence[FeatureEvidence], policy: QualificationPolicyV4, *, wave: str,
                            registration_id: str, catalog_digest: str, n_trials: int, prior_gating_hypotheses: int,
                            grade_basis: str, holdout_opened: bool, run: Mapping[str, Any] | None = None) -> V4Ledger:
    """Grade the evidence (:func:`grade_v4`), resolve one status per feature and seal the ledger."""
    if grade_basis not in GRADE_BASES:
        raise QualificationError(f"grade_basis must be one of {GRADE_BASES}")
    grade_holdout = grade_basis == GRADE_FINAL and holdout_opened
    graded = grade_v4(evidence, policy, n_trials=n_trials, prior_gating_hypotheses=prior_gating_hypotheses,
                      grade_holdout=grade_holdout)
    annotations = {item.feature_id: item.annotations for item in evidence}
    by_feature: dict[str, dict[str, Mapping[str, Any]]] = {}
    for row in graded:
        by_feature.setdefault(str(row["feature_id"]), {})[str(row["basis"])] = row
    features = []
    for feature_id in sorted(by_feature):
        status, basis, why = _resolve_v4(by_feature[feature_id], policy)
        chosen = by_feature[feature_id][basis]
        record = {name: chosen.get(name) for name in V4_FEATURE_COLUMNS if name in chosen}
        record.update({"feature_id": feature_id, "status": status, "status_basis": basis, "status_reasons": why,
                       **{k: v for k, v in annotations.get(feature_id, {}).items()}})
        features.append({name: _clean(record.get(name)) for name in V4_FEATURE_COLUMNS})
    bases = [{name: _clean(row.get(name)) for name in V4_ROW_COLUMNS} for row in
             sorted(graded, key=lambda r: (str(r["feature_id"]), str(r["basis"])))]
    counts = {status: 0 for status in V4_STATUSES}
    for row in features:
        counts[str(row["status"])] += 1
    blockers = []
    if grade_basis == GRADE_PROVISIONAL:
        blockers.append("provisional_labels_selection_sample_only")
    elif not holdout_opened:
        blockers.append("final_labels_holdout_not_opened")
    if counts[V4_QUALIFIED_RECONSTRUCTED]:
        blockers.append(f"qualified_on_reconstructed_evidence_only:{counts[V4_QUALIFIED_RECONSTRUCTED]}")
    manifest = {
        "qualification_version": QUALIFICATION_VERSION, "qualification_code_sha256": qualification_code_sha256(),
        "policy": {"version": policy.version, "sha256": policy.sha256, "split_sha256": policy.split.sha256,
                   "primary_variant": policy.primary_variant, "primary_horizon_months": policy.primary_horizon},
        "wave": wave, "registration_id": registration_id, "catalog_digest": catalog_digest,
        "grade_basis": grade_basis, "holdout_opened": holdout_opened, "holdout_graded": grade_holdout,
        "n_trials": n_trials, "prior_gating_hypotheses": prior_gating_hypotheses,
        "gating_hypotheses": sum(1 for item in evidence if not item.reported_only) // max(
            1, len({item.basis for item in evidence})),
        "features": len(features), "status_counts": counts, "blockers": sorted(blockers),
        "run": dict(run or {}),
    }
    ledger_id = f"{wave}:{policy.version}:{grade_basis}"
    manifest = {**manifest, "ledger_id": ledger_id}
    sha = _sha(_canonical({"manifest": manifest, "features": features, "bases": bases}))
    return V4Ledger(ledger_id, manifest, tuple(features), tuple(bases), sha)


def grade_wave_v4(cells: pd.DataFrame, slices: pd.DataFrame, series: pd.DataFrame, policy: QualificationPolicyV4,
                  *, wave: str, spec: Any, catalog: Iterable[Any], catalog_digest: str, grade_basis: str,
                  registry: Any = None, run: Mapping[str, Any] | None = None) -> V4Ledger:
    """Grade one registered wave's R3b result frames under policy v4 (the trial registry supplies the
    registration, ``n_trials``, the earlier waves' gating hypotheses and the holdout state).

    Refuses an unregistered wave, a catalog digest or policy other than the registration's, and
    everything :func:`evidence_from_evaluation` refuses. A provisional-label grade never reads the
    holdout.
    """
    from .trial_registry import RegistryError, TrialRegistry

    registry = registry if registry is not None else TrialRegistry()
    try:
        registration = registry.require_registration(wave, catalog_digest=catalog_digest, policy_sha=policy.sha256)
    except RegistryError as error:
        raise QualificationRefused([f"wave_registration:{error}"]) from error
    opened = registry.holdout_opened(wave)
    grade_holdout = grade_basis == GRADE_FINAL and opened
    catalog_rows = list(catalog)
    evidence = evidence_from_evaluation(cells, slices, series, policy, spec=spec, catalog=catalog_rows,
                                        registration=registration, grade_holdout=grade_holdout)
    return ledger_from_evidence_v4(evidence, policy, wave=wave, registration_id=registration.registration_id,
                                   catalog_digest=catalog_digest, n_trials=registry.trials_so_far(),
                                   prior_gating_hypotheses=registry.gating_hypotheses_before(wave),
                                   grade_basis=grade_basis, holdout_opened=opened, run=run)


__all__ = [
    "BASIS_COLUMNS",
    "CANDIDATE",
    "DISCOVERY",
    "EVALUATION_SPEC_FIELDS",
    "FEATURE_COLUMNS",
    "FROZEN_POLICY_SHA256",
    "GRADE_BASES",
    "GRADE_FINAL",
    "GRADE_PROVISIONAL",
    "HOLDOUT_FAILED",
    "INSUFFICIENT_COVERAGE",
    "INSUFFICIENT_EVIDENCE",
    "NOT_INVESTABLE",
    "NOT_SIGNIFICANT",
    "POLICY_HISTORY_PATHS",
    "POLICY_PATH",
    "POLICY_V4_PATH",
    "POLICY_V4_PATHS",
    "POST_HOC_REASONS",
    "QUALIFICATION_VERSION",
    "QUALIFIED_RECONSTRUCTED",
    "QUALIFIED_STATUSES",
    "QUALIFIED_STRICT",
    "REPLICATION",
    "REPORTED_ONLY",
    "SELECTION_PASS",
    "SIGN_REVERSED",
    "STATUSES",
    "UNSTABLE",
    "UNTESTABLE_STRICT",
    "V4_FEATURE_COLUMNS",
    "V4_PASSING",
    "V4_POLICY_IDS",
    "V4_ROW_COLUMNS",
    "V4_STATUSES",
    "EvaluationResults",
    "FeatureEvidence",
    "PolicyError",
    "QualificationError",
    "QualificationLedger",
    "QualificationPolicy",
    "QualificationPolicyV4",
    "QualificationRefused",
    "V4Ledger",
    "current_policy_problem",
    "ensure_qualification_schema",
    "evaluation_spec_kwargs",
    "evaluation_spec_kwargs_v4",
    "evidence_from_evaluation",
    "grade_v4",
    "grade_wave_v4",
    "holdout_shrink_test",
    "ledger_from_evidence_v4",
    "load_evaluation_results",
    "load_policy",
    "load_policy_v4",
    "load_policy_v4_by_id",
    "one_sided_p",
    "persist_ledger",
    "policy_file_sha256s",
    "policy_registered_at",
    "policy_sha256",
    "qualification_code_sha256",
    "qualify",
    "qualify_run",
    "refusal_reasons",
    "register_policy",
    "render_qualified_signals_markdown",
    "stored_catalog_snapshot",
    "stored_ledger_sha256",
    "superseded_policy_versions",
    "v4_spec_problems",
    "verify_ledger",
    "write_artifacts",
]
