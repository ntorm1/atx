"""Standardized, versioned cross-sectional feature store (task R2b).

For one sealed R2a monthly panel run (one basis) and the R1a anomaly catalog this
module builds one immutable, content-addressed *feature version*: for every
formation date, research-admitted catalog feature and security of the ranked
universe, the sign-oriented raw value and five cross-sectional standardizations.
It is the only producer of the ``research_feature_*`` tables that the R3b
evaluation harness reads through ``evaluation.load_feature_table``.

Universe (:data:`UNIVERSE_RULE`, controller ruling on R2b I1)
------------------------------------------------------------
Each feature is standardized per formation over the ranked universe of its
*universe scope* (``research_feature_catalog.universe_scope``):

``valid_primary_lines`` (fundamental, size and valuation features: they need an owner)
    the R2a cohort rows with ``cohort_reason='valid'`` on the issuer's
    ``primary_line``: one line per linked issuer (``owner_basis='linked_primary'``).
``valid_primary_and_unlinked_lines`` (identity-free price-line features)
    the same linked primary lines plus every eligible line without a usable owner
    link (``cohort_reason`` in R2a ``OWNER_LINK_FAILURES``, mostly the delisted tail
    that the reconstructed bridge cannot link), each ranked as its own name
    (``owner_basis='unlinked_line'``). Ranking only linked names would condition
    momentum, reversal and volatility on survival to a current SEC ticker. A
    feature is price-line scoped only when every panel input (the feature, its
    composition legs, its domain operand) is R2a ``price_line`` scoped and not a
    size feature. Caveat (labeled blocker): until identity reconstruction links
    them, two unlinked share classes of one issuer rank as two names.

A value exists only when the line's R2a panel row is ``reason='valid'`` (stale
anchors, lineage failures, missing states and every other cohort exclusion carry no
value). Secondary share classes of linked issuers, rows outside the feature's scope,
non-common and overlapping rows are counted per formation in
``research_feature_dates.reasons_json`` (``cohort:<reason>``,
``secondary_issuer_line``, ``panel:<reason>``), never ranked; the counts of each
formation sum to its R2a ``eligible_members``. ``universe_names`` is the size of the
feature's ranked universe (the coverage denominator: ``coverage_fraction =
valid_names / universe_names``). For every price-line feature
``research_feature_owner_basis`` records per formation and owner basis the number
of valued names and the mean / sample sd of ``signed_raw`` and ``rank_normal``, so
the linked-only versus full-universe difference (the survivorship bias of a
linked-only ranking) is measurable; the long view carries ``owner_basis`` per value.

Admission, sign and domain (R1a catalog)
----------------------------------------
* Features are the catalog's research-eligible rows (``eligible`` and
  ``eligible_with_caveat``), not the raw seed. Blocked rows (and eligible rows whose
  inputs the panel run lacks) produce no values but are listed in
  ``research_feature_catalog`` with their status.
* ``expected_sign`` on every value row equals the catalog sign (+1, -1, or 0 for a
  pre-registered two-sided hypothesis). Values are oriented by it (higher means a
  higher expected return); a two-sided feature keeps its raw direction.
* The machine-readable ``domain`` is enforced before any transform. A value outside
  it is stored with its ``domain_status`` and NULL in every variant, so it is never
  ranked as valid: ``nonpositive_value`` (``positive_value_required``),
  ``missing_denominator`` / ``nonpositive_denominator``
  (``positive_denominator_required``), ``negative_book`` / ``missing_book_operand``
  (``negative_book_excluded``), ``loss_firm`` / ``missing_loss_operand``
  (``loss_firms_separate``) and ``zero_payer`` (``zero_payer_separate``). The last
  two are the catalog's separate indicators: the matrix row keeps ``raw_value`` and
  the status, so the indicator is recoverable without entering the ranking.
* A ``metric:``/``item:`` domain operand of a fundamental feature is read *of the
  same state*: the operand's state in the feature state's fiscal bucket, as visible
  at the feature state's own clock (the derived engine's input rule), from the
  read-only warehouse. A daily feature's operand is the same formation's panel row;
  a composition's operand is its leg.
* ``caveat_code`` is carried per feature; the executable caveats are enforced
  (``sign_flip``/``non_monotone`` through the domain, ``mixed_evidence`` through the
  two-sided sign, ``split_basis`` through the panel's comparable-origin rule, re-checked
  here as ``invalid_value_origin``). ``hypothesis_family`` is carried per feature.

Size (R2a ``size_policy``)
--------------------------
A size feature (``market_cap`` and everything that reads a share count: EV and the
valuation ratios) is used only where the panel row's ``size_status`` is the verified
status (DEI shares); vendor/archive share counts are ``unverified_size`` and excluded
from the feature, from the size-neutral regressor and from the compositions. Counts
are reported per formation and in the version diagnostic.

Variants (:data:`VARIANTS`)
---------------------------
``signed_raw``
    orientation x raw value (in-domain rows).
``winsor``
    per-formation winsorization at the ``winsor_limits`` quantiles (default 1st/99th,
    pandas linear interpolation) of orientation x base, where base is ``ln(raw)`` for a
    ``log_winsor_z`` catalog feature (positive levels) and the raw value otherwise.
``zscore``
    per-formation sample z-score (ddof 1) of ``winsor``.
``rank_normal``
    Blom inverse-normal score of the average rank r among n names:
    Phi^-1((r - 3/8) / (n + 1/4)).
``industry_neutral``
    within-industry percentile rank centered on zero (FF12 by default), exactly
    :func:`atx_db.signal_eval.neutralize_panel_by_industry` (groups of at least
    ``industry_min_group_size`` names), then z-scored per formation so that it has
    unit variance like ``zscore``/``rank_normal``/``size_neutral`` (Fama-MacBeth
    slopes and composite weights are comparable across variants; the raw centered
    rank has sd of about 0.29).
``size_neutral``
    residual of ``rank_normal`` on ln(verified market cap) per formation
    (:func:`atx_db.factors.cross_section.neutralize`), then z-scored. Not produced for
    size-class features (a residual on their own size is identically zero).

A price-line feature's neutral variants need the covariate of the unlinked lines
too; they have no owner (no industry, no verified size), so where they are more
than ``1 - neutral_min_coverage`` of the ranked names the variant is
``thin_covariate_coverage`` (never computed on the linked subset only).

The cross-sectional arithmetic is the existing implementations
(:mod:`atx_db.factors.cross_section` winsorize/zscore/rank/neutralize and
``signal_eval.neutralize_panel_by_industry``), called per formation chunk; bounded
DuckDB SQL does the selection, gating, domain, operands, compositions, guards and
writes. Guards: fewer than ``min_names`` (200) in-domain names makes every variant of
the formation ``thin_cross_section`` (``signed_raw`` keeps its flagged value, nothing
is standardized); a neutral variant whose covariate (industry group, verified size)
covers fewer than ``min_names`` names or less than ``neutral_min_coverage`` of the
in-domain names is ``thin_covariate_coverage`` and NULL (never a silently selected
subset); a variant with no value on a non-thin formation is
``degenerate_cross_section``.

Market-scaled compositions (catalog ``source_kind='composition'``) are formed here:
numerator / denominator at the formation, legs from the same formation's panel rows
(or, for an ``item:`` leg, the owner's latest standardized item visible at the cutoff,
at most the panel's ``max_age_days`` old), denominators from verified size only; the
clock is the latest input clock of both legs.

Classification basis (RX1): ``strict`` joins ``entity_classification`` point in time
(``valid_from``, ``as_of_date``, ``available_at`` at the cutoff); ``reconstructed`` uses
each CIK's current classification backcast (``current_sic_backcast``), labeled.

Research-store sources (E1)
---------------------------
Catalog rows of source kind ``event`` (P3 ``research_event_features``), ``factor_exposure``
(P4 ``research_factor_exposures``) and ``ownership`` (P9 ``research_ownership_features``)
read one source version pinned in the options (``event_version``, ``factor_run_id``,
``ownership_version``); without a pin the row is ``source_version_not_pinned``. The pinned
version must be built on this panel run's seal, carry the source contract this adapter is
written against (:data:`EXTERNAL_QUERY_VERSIONS`) and pass its own validator (P3, P9) or
results digest (P4, a ``complete`` run); its id and content digest enter the version inputs
and its blockers the version blockers (``event:``, ``factor:``, ``ownership:``).
``metric_code`` names the source feature (for P4 the exposure column) and ``metric_window``
its declared window (:data:`EXTERNAL_FEATURES`); these rows take no domain operand. Event
values and the P9 13F features (IO, change in IO, breadth) are owner features on the owner's
primary line and rank the valid primary lines; P4 exposures and the P9 short-interest
features (line level since P9 v2) are line features and rank the unlinked lines too
(:data:`EXTERNAL_LINE_FEATURES`). Every source row must sit on a line of the R2b universe
with the owner basis R2b gives that line (P3 rows on linked primary lines only; P4 and P9
rows carry their ``owner_basis``) and, where the source names an owner, on that line's owner
(a P9 row on an unlinked line names none); anything else, or a duplicate, breaks the pinned
contract and stops the build. Every eligible member gets one row from the source row of the
*same* formation and line: the source already chose the newest state
visible at that cutoff, NULL states included, so an invalid newest state is NULL with its
reason and never an earlier formation's value. A value enters only when its source reason
is valid (P4 ``estimated``) and its own ``available_at`` is at or before the cutoff;
otherwise it is NULL and counted as ``<kind>:<source reason>`` (for example
``factor_exposure:insufficient_obs`` in the beta burn-in), ``<kind>:missing_source_row`` or
``<kind>:not_visible_at_cutoff``, so the universe is never narrowed silently. A source row
flag is counted per formation as ``flag:<flag>`` beside the status counts, outside their
sum: P9 ``io_above_one`` (IO above one is kept, never clipped) and P9
``cusip_survivor_conditioned`` (a 13F value whose CUSIP mapped through a current snapshot:
coverage conditioned on survival). ``age_days`` is the days since
the event session (event) or the source period (ownership). One SUE hypothesis: the panel's
``sue_ni`` and P3's ``sue_ni_event`` (the same states on the event clock, never earlier than
the filing clock, only where an event is visible) are never built together. No table or
column is added, so the store schema stays v3.

Derived versions (public writer)
--------------------------------
:func:`write_derived_feature_version` writes a *derived* feature version: every row of one
sealed constituent version plus new features whose standardized rows the caller supplies
(P5 composites). The rows go through the same writer as this module's own features
(:class:`_FeatureWriter`: matrix rows, date rows, owner-basis diagnostic, digests), so a
derived version digests and validates exactly as a built one.

Versions
--------
``feature_version`` = sha256 of (spec, code, inputs): the spec (panel run, policy
parameters, feature selection), the code digest of this module and the modules whose
arithmetic it calls, and the input digests (panel seal, catalog content, and content
digests of every warehouse relation read: classification, domain operands, item
legs). Versions are immutable: rebuilding identical inputs returns the sealed version
unchanged; any change creates a new version and leaves older ones intact. A
``building``/``failed`` version resumes feature by feature (one transaction each).
The spec records the numpy/pandas/DuckDB versions (the arithmetic depends on them).
Nothing is deleted automatically: :func:`prune_feature_versions` is the explicit
command that removes failed/abandoned versions (and, when asked, sealed versions
superseded within their basis); it is a dry run by default and never deletes the
latest sealed version of a basis (by registration order) or a version that an
evaluation run (R3b) or a qualification ledger (R4) of the store references.

Tables (research store, RX6; schema owned here until registered as a store migration)
------------------------------------------------------------------------------------
``research_feature_versions``, ``research_feature_catalog`` (per version x catalog
row, with the feature's ``universe_scope``), ``research_feature_matrix`` (wide: one
row per formation x security x feature that passed every gate, with ``owner_basis``,
``raw_value``, ``domain_status``, ``age_days``, the clock and the six variants as
columns; inserted feature by feature, so clustered by feature),
``research_feature_context`` (per formation x universe line: ``owner_basis``, owner
CIK, the verified ln market cap regressor and the industry group, shared by every
feature; unlinked lines have no owner covariates), ``research_feature_dates``
(formation x feature x variant: status, counts, ``universe_names`` and coverage
against it, the R2a ``eligible_members``, covariate coverage, the reason counts and,
for price-line features, ``unlinked_excluded`` = ranked unlinked lines without a
value in the variant plus the label ``sample_conditioning =
survivor_conditioned_linked_only`` on a formed variant that excluded any: a
neutral variant there is computed on linked names only),
``research_feature_owner_basis`` (price-line features: formation x owner basis
diagnostic) and the contract view ``research_feature_values`` (long: the matrix
unpivoted, NULLs included, one row per formation x security x feature x variant with
its ``owner_basis``; a variant a feature does not produce is NULL and has no date
rows). Schema version 3 (v1/v2 stores are migrated in place by adding the columns;
versions built under an older schema are refused by the validator and can only be
pruned).
"""

from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import json
import math
import re
import time
from collections.abc import Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from statistics import NormalDist
from typing import Any

import duckdb
import numpy as np
import pandas as pd

from .. import signal_eval as _signal_eval
from .._derived_pit import bucket_sql
from ..derived_registry import DERIVED_SOURCE_NAME, default_derived_definitions
from ..factors import cross_section as _cross_section
from . import catalog as _catalog
from . import panel as _panel
from .catalog import AnomalyCatalogEntry
from .store import ResearchStore

# The query version stays v2 (R3b's adapter accepts v1/v2 only). R2e bumps the store
# schema instead: v3 versions record ``store_schema`` in their spec (so their ids never
# collide with pre-R2e versions) and the validator refuses any other store schema.
QUERY_VERSION = "research-feature-store-v2"
FEATURE_SCHEMA_VERSION = 3
KNOWN_SCHEMA_VERSIONS = (1, 2, 3)
VARIANTS = ("signed_raw", "winsor", "zscore", "rank_normal", "industry_neutral", "size_neutral")
STANDARDIZED_VARIANTS = VARIANTS[1:]
#: Owner/size features on valid primary lines; identity-free price-line features also
#: rank every eligible unlinked line as its own name (controller ruling on R2b I1).
UNIVERSE_RULE = "cohort_reason_valid_on_primary_line_price_line_unlinked_lines"
UNIVERSE_SCOPE_LINKED = "valid_primary_lines"
UNIVERSE_SCOPE_ALL_LINES = "valid_primary_and_unlinked_lines"
UNIVERSE_SCOPES = (UNIVERSE_SCOPE_LINKED, UNIVERSE_SCOPE_ALL_LINES)
OWNER_BASIS_LINKED = "linked_primary"
OWNER_BASIS_UNLINKED = "unlinked_line"
OWNER_BASES = (OWNER_BASIS_LINKED, OWNER_BASIS_UNLINKED)
#: R2a cohort reasons of an eligible line without a usable owner link.
UNLINKED_COHORT_REASONS = tuple(_panel.OWNER_LINK_FAILURES)
UNLINKED_LINES_BLOCKER = "price_line_features_rank_unlinked_lines_unlinked_share_classes_may_double_count_issuers"
#: R2b N1: a formed variant of a price-line feature that leaves out unlinked lines the
#: raw variants rank (the neutral variants: unlinked lines have no owner covariates) is
#: survivor-conditioned. Compare it with the raw variant only on ``owner_basis =
#: linked_primary`` rows and on formations where both are formed.
CONDITIONING_LINKED_ONLY = "survivor_conditioned_linked_only"
NEUTRAL_CONDITIONING_BLOCKER = "price_line_neutral_variants_linked_only_survivor_conditioned"
DEFAULT_CONTROLS = ("market_cap", "book_to_market", "momentum_12_1")
SIZE_FEATURE_ID = "market_cap"
SIZE_CLASS = "size"

STATUS_BUILDING = "building"
STATUS_SEALED = "sealed"
STATUS_UNTESTABLE = "untestable_strict"
STATUS_FAILED = "failed"
SEALED_STATUSES = (STATUS_SEALED, STATUS_UNTESTABLE)

FEATURE_BUILT = "built"
FEATURE_UNTESTABLE = "untestable_strict"
FEATURE_BLOCKED = "blocked_admission"
FEATURE_EXCLUDED = "excluded_by_subset"
FEATURE_INPUT_MISSING = "input_not_in_panel"
FEATURE_OPERAND_MISSING = "domain_operand_not_in_panel"
FEATURE_SOURCE_NOT_PINNED = "source_version_not_pinned"

DATE_FORMED = "formed"
DATE_THIN = "thin_cross_section"
DATE_THIN_COVARIATE = "thin_covariate_coverage"
DATE_DEGENERATE = "degenerate_cross_section"
DATE_EMPTY = "empty_common_cohort"

IN_DOMAIN = "in_domain"
#: Statuses of a row that passed every gate (universe, panel validity, size): it is
#: stored in the matrix; only ``in_domain`` rows are standardized.
DOMAIN_STATUSES = (IN_DOMAIN, "nonpositive_value", "missing_denominator", "nonpositive_denominator",
                   "negative_book", "missing_book_operand", "loss_firm", "missing_loss_operand", "zero_payer",
                   "nonfinite_value")
INVALID_ORIGINS = ("incomparable", "legacy_unspecified", "unavailable")
CLASSIFICATION_BASES = {"reconstructed": "current_sic_backcast", "strict": "pit_entity_classification"}
ITEM_BASES = ("quarterly", "instant")
PREFERRED_VARIANT = {"winsor_z": "zscore", "log_winsor_z": "zscore", "rank_normal": "rank_normal"}

#: Formations per digest query (digests are per formation, so any chunk gives the same bytes).
_DIGEST_CHUNK = 24
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_PIN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$")
_OPERAND = re.compile(r"^(metric|item):([a-z][a-z0-9_]{0,95})$")
_CODE_FILES = (Path(__file__), Path(_catalog.__file__), Path(_panel.__file__), Path(_cross_section.__file__),
               Path(_signal_eval.__file__))
_NORMAL = NormalDist()


class FeatureStoreError(ValueError):
    """The feature store cannot build or validate a version under its contract."""


# ---------------------------------------------------------------------------
# Small canonical helpers
# ---------------------------------------------------------------------------

def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _sql_text(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _sql_list(values: Iterable[str]) -> str:
    return ",".join(_sql_text(value) for value in values)


def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

_MATRIX_COLUMNS: tuple[tuple[str, str], ...] = (
    ("feature_version", "VARCHAR NOT NULL"),
    ("formation_date", "DATE NOT NULL"),
    ("security_id", "VARCHAR NOT NULL"),
    ("feature_id", "VARCHAR NOT NULL"),
    ("owner_basis", "VARCHAR"),          # schema v2 (NULL only on legacy v1 rows)
    ("expected_sign", "INTEGER NOT NULL"),
    ("available_at", "TIMESTAMP NOT NULL"),
    ("raw_value", "DOUBLE"),
    ("domain_status", "VARCHAR NOT NULL"),
    ("age_days", "INTEGER"),
    *((variant, "DOUBLE") for variant in VARIANTS),
)
#: Columns of a matrix row that its per-feature digest covers (constant keys excluded).
_DIGEST_COLUMNS = tuple(name for name, _ in _MATRIX_COLUMNS if name not in ("feature_version", "feature_id"))
#: Per (formation, universe line) covariates shared by every feature of a version.
_CONTEXT_COLUMNS = ("formation_date", "security_id", "owner_basis", "owner_cik", "log_size", "industry_group")
_DATE_COLUMNS = ("feature_version", "formation_date", "feature_id", "variant", "date_status", "eligible_members",
                 "universe_names", "valid_names", "coverage_fraction", "candidate_names", "in_domain_names",
                 "covariate_names", "covariate_coverage", "reasons_json", "unlinked_excluded",
                 "sample_conditioning")
_CATALOG_COLUMNS = ("feature_version", "feature_id", "source_kind", "anomaly_class", "hypothesis_family",
                    "expected_sign", "orientation_sign", "preferred_transform", "preferred_variant", "domain",
                    "caveat_codes", "admission", "is_control", "universe_scope", "inputs_json", "status",
                    "status_reason", "variants_json", "value_rows", "in_domain_rows", "reasons_json", "values_sha256")
#: Price-line features: per formation x owner basis, the valued names and moments.
_OWNER_BASIS_COLUMNS = ("feature_version", "formation_date", "feature_id", "owner_basis", "names", "signed_raw_mean",
                        "signed_raw_sd", "ranked_names", "rank_normal_mean", "rank_normal_sd")
#: Schema v1 -> v2 (R2b fix round 1): columns added in place to a v1 store.
_V2_COLUMNS = (("research_feature_matrix", "owner_basis", "VARCHAR"),
               ("research_feature_context", "owner_basis", "VARCHAR"),
               ("research_feature_dates", "universe_names", "BIGINT"),
               ("research_feature_catalog", "universe_scope", "VARCHAR"))
#: Schema v2 -> v3 (R2e): the survivor-conditioning label of price-line variants (R2b N1).
_V3_COLUMNS = (("research_feature_dates", "unlinked_excluded", "BIGINT"),
               ("research_feature_dates", "sample_conditioning", "VARCHAR"))
#: Every table holding rows of one version (pruning deletes from all of them).
_VERSION_TABLES = ("research_feature_matrix", "research_feature_dates", "research_feature_owner_basis",
                   "research_feature_context", "research_feature_catalog", "research_feature_versions")


def _schema_versions(con: Any) -> set[int]:
    if not con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name = 'research_feature_schema' "
                       "AND NOT temporary").fetchone()[0]:
        return set()
    return {int(row[0]) for row in con.execute("SELECT version FROM research_feature_schema").fetchall()}


def _require_feature_schema(con: Any) -> None:
    """Read paths never run DDL: the current schema must already be in place."""
    versions = _schema_versions(con)
    if FEATURE_SCHEMA_VERSION not in versions or versions - set(KNOWN_SCHEMA_VERSIONS):
        raise FeatureStoreError(f"research feature schema v{FEATURE_SCHEMA_VERSION} is not in place "
                                f"(found {sorted(versions)}); build a version first")


def ensure_feature_schema(con: Any) -> None:
    """Create or migrate the ``research_feature_*`` tables and the contract view (schema v3).

    Kept in this module until the research-store owner registers it as a store
    migration; the version table refuses a newer, unknown schema. A v1/v2 store
    gains the later columns in place (legacy rows keep NULL there; their versions
    carry an older query version or store schema, which the validator refuses, so
    they can only be pruned). The contract view is (re)created as a VIEW; an
    existing TABLE of that name is an error, never silently kept.
    """
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_feature_schema (
            version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""")
    versions = {int(row[0]) for row in con.execute("SELECT version FROM research_feature_schema").fetchall()}
    if versions - set(KNOWN_SCHEMA_VERSIONS):
        raise RuntimeError(f"research feature schema has unknown versions {sorted(versions)}; code is older")
    if FEATURE_SCHEMA_VERSION in versions:
        return
    matrix = ",\n            ".join(f"{name} {kind}" for name, kind in _MATRIX_COLUMNS)
    con.execute(f"""
        CREATE TABLE IF NOT EXISTS research_feature_versions (
            feature_version VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            panel_run_id VARCHAR NOT NULL,
            panel_sha256 VARCHAR NOT NULL,
            classification_basis VARCHAR NOT NULL,
            values_sha256 VARCHAR,
            query_version VARCHAR NOT NULL,
            universe_rule VARCHAR NOT NULL,
            spec_json VARCHAR NOT NULL,
            spec_sha256 VARCHAR NOT NULL,
            code_sha256 VARCHAR NOT NULL,
            catalog_sha256 VARCHAR NOT NULL,
            inputs_json VARCHAR NOT NULL,
            inputs_sha256 VARCHAR NOT NULL,
            blockers_json VARCHAR NOT NULL,
            diagnostic_json VARCHAR,
            created_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS research_feature_catalog (
            feature_version VARCHAR NOT NULL,
            feature_id VARCHAR NOT NULL,
            source_kind VARCHAR NOT NULL,
            anomaly_class VARCHAR NOT NULL,
            hypothesis_family VARCHAR NOT NULL,
            expected_sign INTEGER NOT NULL,
            orientation_sign INTEGER NOT NULL,
            preferred_transform VARCHAR NOT NULL,
            preferred_variant VARCHAR,
            domain VARCHAR NOT NULL,
            caveat_codes VARCHAR NOT NULL,
            admission VARCHAR NOT NULL,
            is_control BOOLEAN NOT NULL,
            universe_scope VARCHAR,
            inputs_json VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            status_reason VARCHAR,
            variants_json VARCHAR NOT NULL,
            value_rows BIGINT,
            in_domain_rows BIGINT,
            reasons_json VARCHAR,
            values_sha256 VARCHAR,
            PRIMARY KEY (feature_version, feature_id)
        );
        CREATE TABLE IF NOT EXISTS research_feature_matrix (
            {matrix}
        );
        CREATE TABLE IF NOT EXISTS research_feature_context (
            feature_version VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            owner_basis VARCHAR,
            owner_cik VARCHAR,
            log_size DOUBLE,
            industry_group VARCHAR
        );
        CREATE TABLE IF NOT EXISTS research_feature_dates (
            feature_version VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            feature_id VARCHAR NOT NULL,
            variant VARCHAR NOT NULL,
            date_status VARCHAR NOT NULL,
            eligible_members BIGINT NOT NULL,
            universe_names BIGINT,
            valid_names BIGINT NOT NULL,
            coverage_fraction DOUBLE,
            candidate_names BIGINT NOT NULL,
            in_domain_names BIGINT NOT NULL,
            covariate_names BIGINT,
            covariate_coverage DOUBLE,
            reasons_json VARCHAR NOT NULL,
            unlinked_excluded BIGINT,
            sample_conditioning VARCHAR,
            PRIMARY KEY (feature_version, formation_date, feature_id, variant)
        );
    """)
    for table, column, kind in (*_V2_COLUMNS, *_V3_COLUMNS):  # in place; a no-op on a fresh store
        con.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS {column} {kind}")
    con.execute(f"""
        CREATE TABLE IF NOT EXISTS research_feature_owner_basis (
            feature_version VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            feature_id VARCHAR NOT NULL,
            owner_basis VARCHAR NOT NULL,
            names BIGINT NOT NULL,
            signed_raw_mean DOUBLE,
            signed_raw_sd DOUBLE,
            ranked_names BIGINT NOT NULL,
            rank_normal_mean DOUBLE,
            rank_normal_sd DOUBLE,
            PRIMARY KEY (feature_version, formation_date, feature_id, owner_basis)
        );
        CREATE OR REPLACE VIEW research_feature_values AS
        SELECT feature_version, formation_date, security_id, feature_id, variant, value, expected_sign,
               available_at, domain_status, owner_basis
        FROM research_feature_matrix
        UNPIVOT INCLUDE NULLS (value FOR variant IN ({", ".join(VARIANTS)}));
    """)
    con.execute("INSERT INTO research_feature_schema VALUES (?, ?, ?)",
                [FEATURE_SCHEMA_VERSION, "feature_store_v3_survivor_conditioning", _now()])


# ---------------------------------------------------------------------------
# Options and planning
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FeatureStoreOptions:
    """What determines a feature version (``formation_chunk`` is execution only)."""

    panel_run_id: str
    #: Catalog feature ids to build; ``None`` builds every research-eligible row.
    features: tuple[str, ...] | None = None
    winsor_limits: tuple[float, float] = (0.01, 0.01)
    min_names: int = 200
    industry_min_group_size: int = 5
    neutral_min_coverage: float = 0.80
    taxonomy_code: str = "FAMA_FRENCH_12"
    control_features: tuple[str, ...] = DEFAULT_CONTROLS
    #: The catalog CSV (validated against the engine); ``None`` is the committed seed.
    catalog_path: Path | str | None = None
    #: Pre-parsed catalog rows instead of a file (fixtures); labeled and blocked.
    catalog_entries: tuple[AnomalyCatalogEntry, ...] | None = None
    #: Execution only (output-invariant): formations standardized per pass, and
    #: whether the R2a validator re-checks the panel before building.
    formation_chunk: int = 24
    verify_panel: bool = True
    #: E1 source pins (they determine the version): the sealed P3 event version, the
    #: complete P4 factor run and the sealed P9 ownership version of the same panel run
    #: that the catalog's ``event`` / ``factor_exposure`` / ``ownership`` rows read. A
    #: kind without a pin is ``source_version_not_pinned``; an unused pin is ignored.
    event_version: str | None = None
    factor_run_id: str | None = None
    ownership_version: str | None = None


@dataclass(frozen=True)
class _Leg:
    role: str                  # numerator | denominator
    kind: str                  # metric | item
    code: str
    panel_feature: str | None  # the leg's panel feature id (metric legs)
    size_feature: bool


@dataclass(frozen=True)
class _Operand:
    kind: str                  # state_metric | state_item | panel | leg | item_latest
    code: str
    window: str | None = None
    panel_feature: str | None = None
    size_feature: bool = False
    leg: str | None = None


@dataclass(frozen=True)
class _Plan:
    entry: AnomalyCatalogEntry
    status: str
    reason: str | None = None
    panel_feature: str | None = None
    legs: tuple[_Leg, ...] = ()
    operand: _Operand | None = None
    size_feature: bool = False
    variants: tuple[str, ...] = ()
    universe_scope: str = UNIVERSE_SCOPE_LINKED
    #: E1: (source kind, source feature) of a research-store source row.
    external: tuple[str, str] | None = None

    @property
    def feature_id(self) -> str:
        return self.entry.feature_id

    @property
    def price_line(self) -> bool:
        """Identity-free: ranks unlinked eligible lines as their own names too."""
        return self.universe_scope == UNIVERSE_SCOPE_ALL_LINES

    @property
    def orientation(self) -> int:
        return self.entry.expected_sign if self.entry.expected_sign in (-1, 1) else 1

    @property
    def log_base(self) -> bool:
        return self.entry.preferred_transform == "log_winsor_z"

    @property
    def is_composition(self) -> bool:
        return self.entry.source_kind == "composition"

    def inputs(self) -> dict[str, Any]:
        inputs: dict[str, Any] = {
            "panel_feature": self.panel_feature,
            "legs": [[leg.role, leg.kind, leg.code, leg.panel_feature, leg.size_feature] for leg in self.legs],
            "operand": None if self.operand is None else [
                self.operand.kind, self.operand.code, self.operand.window, self.operand.panel_feature,
                self.operand.size_feature, self.operand.leg],
            "size_feature": self.size_feature,
            "universe_scope": self.universe_scope}
        if self.external is not None:
            inputs["external"] = list(self.external)
        return inputs


@dataclass(frozen=True)
class _PanelContext:
    run_id: str
    status: str
    basis: str
    panel_sha256: str
    query_version: str
    spec: dict[str, Any]
    features: dict[tuple[str, str], str]   # (metric_code, window) -> panel feature id
    size_features: frozenset[str]
    verified_status: str
    max_age_days: int
    formations: tuple[dt.date, ...]
    scopes: dict[str, str] = field(default_factory=dict)   # panel feature id -> R2a feature scope

    def universe_scope(self, features: Sequence[str | None], operand: _Operand | None) -> str:
        """Price-line scope only when every panel input is an R2a price-line, non-size feature."""
        def line(feature: str | None) -> bool:
            return (feature is not None and self.scopes.get(feature) == _panel.SCOPE_PRICE_LINE
                    and feature not in self.size_features)
        identity_free = bool(features) and all(line(feature) for feature in features)
        if operand is not None and operand.kind != "leg":
            identity_free = identity_free and operand.kind == "panel" and line(operand.panel_feature)
        return UNIVERSE_SCOPE_ALL_LINES if identity_free else UNIVERSE_SCOPE_LINKED


def _validate_options(options: FeatureStoreOptions) -> FeatureStoreOptions:
    if not isinstance(options.panel_run_id, str) or not _ID.fullmatch(options.panel_run_id):
        raise FeatureStoreError("panel_run_id must be a lower-case identifier")
    limits = tuple(float(x) for x in options.winsor_limits)
    if len(limits) != 2 or not all(0.0 <= x < 0.5 for x in limits):
        raise FeatureStoreError("winsor_limits must be two quantiles in [0, 0.5)")
    for label, value, low, high in (("min_names", options.min_names, 3, 100_000),
                                    ("industry_min_group_size", options.industry_min_group_size, 2, 10_000),
                                    ("formation_chunk", options.formation_chunk, 1, 600)):
        if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
            raise FeatureStoreError(f"{label} must be an integer {low}..{high}")
    coverage = float(options.neutral_min_coverage)
    if not 0.0 <= coverage <= 1.0:
        raise FeatureStoreError("neutral_min_coverage must be in [0, 1]")
    if not isinstance(options.taxonomy_code, str) or not re.fullmatch(r"[A-Z][A-Z0-9_]{0,63}", options.taxonomy_code):
        raise FeatureStoreError("taxonomy_code must be an upper-case taxonomy code")
    features = None
    if options.features is not None:
        features = tuple(sorted(set(options.features)))
        if not features or len(features) != len(tuple(options.features)) or any(
                not isinstance(f, str) or not _ID.fullmatch(f) for f in features):
            raise FeatureStoreError("features must be unique lower-case catalog ids")
    controls = tuple(options.control_features)
    if len(set(controls)) != len(controls) or any(not isinstance(c, str) or not _ID.fullmatch(c) for c in controls):
        raise FeatureStoreError("control_features must be unique lower-case catalog ids")
    if SIZE_FEATURE_ID not in controls:
        raise FeatureStoreError(f"control_features must include the size control {SIZE_FEATURE_ID!r}")
    if options.catalog_entries is not None and options.catalog_path is not None:
        raise FeatureStoreError("pass catalog_path or catalog_entries, not both")
    pins: dict[str, str | None] = {}
    for name in _PIN_FIELDS.values():
        pin = getattr(options, name)
        if pin is not None and (not isinstance(pin, str) or not _PIN.fullmatch(pin)):
            raise FeatureStoreError(f"{name} must be a source version id")
        pins[name] = pin
    return FeatureStoreOptions(
        panel_run_id=options.panel_run_id, features=features, winsor_limits=(limits[0], limits[1]),
        min_names=options.min_names, industry_min_group_size=options.industry_min_group_size,
        neutral_min_coverage=coverage, taxonomy_code=options.taxonomy_code, control_features=controls,
        catalog_path=options.catalog_path, catalog_entries=options.catalog_entries,
        formation_chunk=options.formation_chunk, verify_panel=bool(options.verify_panel), **pins)


def _load_catalog(options: FeatureStoreOptions) -> tuple[tuple[AnomalyCatalogEntry, ...], str, str]:
    """Catalog rows, their content digest and the source label."""
    if options.catalog_entries is not None:
        entries = tuple(options.catalog_entries)
        if not entries or any(not isinstance(e, AnomalyCatalogEntry) for e in entries):
            raise FeatureStoreError("catalog_entries must be AnomalyCatalogEntry rows")
        ids = [e.feature_id for e in entries]
        if len(set(ids)) != len(ids):
            raise FeatureStoreError("catalog_entries have duplicate feature ids")
        payload = [[getattr(e, name) if not isinstance(getattr(e, name), tuple) else list(getattr(e, name))
                    for name in e.__dataclass_fields__] for e in sorted(entries, key=lambda e: e.feature_id)]
        return entries, _sha(_canonical(payload)), "injected_entries"
    path = _catalog.ANOMALY_CATALOG_PATH if options.catalog_path is None else Path(options.catalog_path)
    entries = _catalog.load_anomaly_catalog(path)
    # The label names the file, not its git state (the content digest pins the bytes;
    # an RX3 export pins the commit).
    return entries, _catalog.anomaly_catalog_sha256(path), "default_seed_file" if options.catalog_path is None \
        else "catalog_file"


def _panel_context(store: ResearchStore, run_id: str, verify: bool) -> _PanelContext:
    con = store.con
    row = con.execute("""
        SELECT status, basis, panel_sha256, query_version, spec_json FROM research_panel_runs WHERE run_id=?
    """, [run_id]).fetchone()
    if row is None:
        raise FeatureStoreError(f"panel run {run_id!r} is absent")
    status, basis, panel_sha, query_version, spec_json = row
    if status not in ("complete", "untestable_strict"):
        raise FeatureStoreError(f"panel run {run_id} is {status!r}; only complete or untestable_strict runs are "
                                "standardized")
    if verify:  # point-in-time, grain and digest checks of the sealed panel (R2a validator)
        _panel.validate_research_panel(store, run_id)
    spec = json.loads(spec_json)
    policy = spec.get("size_policy")
    if not isinstance(policy, dict) or "verified_status" not in policy:
        raise FeatureStoreError(f"panel run {run_id} has no size_policy (R2a query version v3 required)")
    features = {(code, window): feature_id for feature_id, code, window, _ in spec["features"]}
    scopes = {feature_id: str(scope) for feature_id, _, _, scope in spec["features"]}
    formations = tuple(r[0] for r in con.execute("""
        SELECT formation_date FROM research_panel_calendar WHERE run_id=? AND status=?
        ORDER BY formation_date
    """, [run_id, _panel.CALENDAR_FORMED]).fetchall())
    return _PanelContext(run_id, str(status), str(basis), str(panel_sha), str(query_version), spec, features,
                         frozenset(policy.get("size_features", ())), str(policy["verified_status"]),
                         int(spec.get("max_age_days", _panel.DEFAULT_MAX_AGE_DAYS)), formations, scopes)


def _parse_operand(text: str | None) -> tuple[str, str] | None:
    if not text:
        return None
    match = _OPERAND.fullmatch(text)
    if match is None:
        raise FeatureStoreError(f"unparseable catalog operand {text!r}")
    return match.group(1), match.group(2)


#: Catalog source kinds planned as one panel feature. ``panel_native`` is the kind
#: proposed for the R2a panel-native features (P2 price/liquidity natives); the
#: catalog lane decides the name and validates it against ``panel.NATIVE_FEATURES``.
PANEL_FEATURE_SOURCE_KINDS = ("seed_metric", "panel_native")

#: E1: catalog source kinds read from a sealed research-store source version pinned in
#: :class:`FeatureStoreOptions`, keyed by the row's ``metric_code`` (the source feature id,
#: for P4 the exposure column) and its declared ``metric_window``.
SOURCE_EVENT = "event"
SOURCE_FACTOR = "factor_exposure"
SOURCE_OWNERSHIP = "ownership"
EXTERNAL_SOURCE_KINDS = (SOURCE_EVENT, SOURCE_FACTOR, SOURCE_OWNERSHIP)
EXTERNAL_FEATURES: Mapping[str, Mapping[str, str]] = {
    SOURCE_EVENT: {"ear_m1p1": "event", "runup_m21_m2": "event", "days_since_announcement": "event",
                   "sue_ni_event": "event"},
    SOURCE_FACTOR: {"beta_mkt_252d": "252d", "ivol_252d": "252d"},
    SOURCE_OWNERSHIP: {"io_ratio_13f": "13f_quarter", "io_change_13f": "13f_quarter",
                       "breadth_change_13f": "13f_quarter", "short_interest_ratio": "short_interest",
                       "days_to_cover_si": "short_interest"},
}
#: The source contract this adapter is written against; any other version is refused (read
#: from the producers: ``events.QUERY_VERSION``, ``factor_returns.FACTOR_VERSION``,
#: ``ownership_features.QUERY_VERSION``; a producer bump is refused here until re-verified).
EXTERNAL_QUERY_VERSIONS: Mapping[str, str] = {SOURCE_EVENT: "research-earnings-events-v3",
                                              SOURCE_FACTOR: "research-factor-returns-v4",
                                              SOURCE_OWNERSHIP: "research-ownership-features-v2"}
#: Source features that are line features (every eligible line, unlinked lines included, each
#: row with its ``owner_basis``): P4 exposures and, since P9 v2, P9 short interest. The other
#: source features need the owner (P9 13F: ``no_owner_link`` on an unlinked line).
EXTERNAL_LINE_FEATURES: Mapping[str, frozenset[str]] = {
    SOURCE_EVENT: frozenset(),
    SOURCE_FACTOR: frozenset(EXTERNAL_FEATURES[SOURCE_FACTOR]),
    SOURCE_OWNERSHIP: frozenset({"short_interest_ratio", "days_to_cover_si"}),
}
_PIN_FIELDS = {SOURCE_EVENT: "event_version", SOURCE_FACTOR: "factor_run_id", SOURCE_OWNERSHIP: "ownership_version"}
_BLOCKER_PREFIX = {SOURCE_EVENT: "event", SOURCE_FACTOR: "factor", SOURCE_OWNERSHIP: "ownership"}
#: kind -> (version table, key column, content digest column, contract column).
_SOURCE_TABLES = {
    SOURCE_EVENT: ("research_event_versions", "event_version", "features_sha256", "query_version"),
    SOURCE_FACTOR: ("research_factor_runs", "run_id", "results_sha256", "factor_version"),
    SOURCE_OWNERSHIP: ("research_ownership_versions", "ownership_version", "features_sha256", "query_version"),
}
#: A P4 exposure column's status column; ``estimated`` is the valid status.
_FACTOR_STATUS = {"beta_mkt_252d": "status_252d", "ivol_252d": "status_252d"}
_FACTOR_ESTIMATED = "estimated"
#: Key prefix of a source row flag counted beside (not inside) a formation's status counts.
FLAG_PREFIX = "flag:"
#: One SUE hypothesis (E1): the panel's filing-clocked ``sue_ni`` is the cataloged one;
#: P3's ``sue_ni_event`` reads the same states on the event clock (never earlier, and only
#: where an event is visible), so it adds no timing and is never built beside it.
_SUE_PANEL = ("seed_metric", "sue_ni")
_SUE_EVENT = (SOURCE_EVENT, "sue_ni_event")


def _plan_features(entries: Sequence[AnomalyCatalogEntry], context: _PanelContext,
                   options: FeatureStoreOptions) -> list[_Plan]:
    windows = {d.metric_code: d.window for d in default_derived_definitions()}
    # Panel natives (P2) can be a seed row, a composition leg or a domain operand too.
    windows.update({code: spec["metric_window"] for code, spec in _panel.NATIVE_FEATURES.items()})

    def panel_feature(code: str) -> str | None:
        window = windows.get(code)
        return None if window is None else context.features.get((code, window))

    plans: list[_Plan] = []
    for entry in sorted(entries, key=lambda e: e.feature_id):
        variants = tuple(v for v in VARIANTS if not (v == "size_neutral" and entry.anomaly_class == SIZE_CLASS))
        if not entry.is_research_eligible:
            plans.append(_Plan(entry, FEATURE_BLOCKED, entry.admission))
            continue
        if options.features is not None and entry.feature_id not in options.features \
                and entry.feature_id not in options.control_features:
            plans.append(_Plan(entry, FEATURE_EXCLUDED, "feature_subset"))
            continue
        domain = _parse_operand(entry.domain_operand)
        if entry.source_kind in EXTERNAL_SOURCE_KINDS:
            kind, code = entry.source_kind, entry.metric_code or ""
            window = EXTERNAL_FEATURES[kind].get(code)
            if window is None or entry.metric_window != window:
                raise FeatureStoreError(f"{entry.feature_id}: the {kind} source has no feature {code!r} with window "
                                        f"{entry.metric_window!r} (known: {dict(EXTERNAL_FEATURES[kind])})")
            if domain is not None:
                raise FeatureStoreError(f"{entry.feature_id}: a {kind} feature takes no domain operand")
            if getattr(options, _PIN_FIELDS[kind]) is None:
                plans.append(_Plan(entry, FEATURE_SOURCE_NOT_PINNED, f"{_PIN_FIELDS[kind]}:none"))
                continue
            scope = UNIVERSE_SCOPE_ALL_LINES if code in EXTERNAL_LINE_FEATURES[kind] else UNIVERSE_SCOPE_LINKED
            plans.append(_Plan(entry, "planned", None, None, (), None, False, variants, scope, (kind, code)))
            continue
        if entry.source_kind in PANEL_FEATURE_SOURCE_KINDS:
            code = entry.metric_code or ""
            feature = context.features.get((code, entry.metric_window or ""))
            if feature is None:
                plans.append(_Plan(entry, FEATURE_INPUT_MISSING, f"metric:{code}/{entry.metric_window}"))
                continue
            operand = None
            if domain is not None:
                kind, op = domain
                if kind == "metric" and op not in windows:
                    raise FeatureStoreError(f"{entry.feature_id}: operand metric {op} is not a seed metric")
                if entry.metric_window != _panel.MARKET_WINDOW and (
                        kind == "item" or windows[op] != _panel.MARKET_WINDOW):
                    # A fundamental state's operand is read of the same state.
                    operand = _Operand("state_metric" if kind == "metric" else "state_item", op,
                                       windows.get(op) if kind == "metric" else None)
                elif kind == "item":
                    operand = _Operand("item_latest", op)
                else:
                    op_feature = panel_feature(op)
                    if op_feature is None:
                        plans.append(_Plan(entry, FEATURE_OPERAND_MISSING, f"metric:{op}"))
                        continue
                    operand = _Operand("panel", op, windows[op], op_feature, op_feature in context.size_features)
            plans.append(_Plan(entry, "planned", None, feature, (), operand, feature in context.size_features,
                               variants, context.universe_scope([feature], operand)))
            continue
        legs: list[_Leg] = []
        missing = None
        for role, text in (("numerator", entry.numerator), ("denominator", entry.denominator)):
            parsed = _parse_operand(text)
            if parsed is None:
                raise FeatureStoreError(f"{entry.feature_id}: composition without a {role}")
            kind, code = parsed
            leg_feature = panel_feature(code) if kind == "metric" else None
            if kind == "metric" and leg_feature is None:
                missing = f"{role}:metric:{code}"
                break
            legs.append(_Leg(role, kind, code, leg_feature, leg_feature in context.size_features))
        if missing is not None:
            plans.append(_Plan(entry, FEATURE_INPUT_MISSING, missing))
            continue
        operand = None
        if domain is not None:
            kind, op = domain
            leg = next((leg for leg in legs if (leg.kind, leg.code) == (kind, op)), None)
            if leg is not None:
                operand = _Operand("leg", op, leg=leg.role, size_feature=leg.size_feature)
            elif kind == "item":
                operand = _Operand("item_latest", op)
            else:
                op_feature = panel_feature(op)
                if op_feature is None:
                    plans.append(_Plan(entry, FEATURE_OPERAND_MISSING, f"metric:{op}"))
                    continue
                operand = _Operand("panel", op, windows.get(op), op_feature, op_feature in context.size_features)
        plans.append(_Plan(entry, "planned", None, None, tuple(legs), operand,
                           any(leg.size_feature for leg in legs), variants,
                           context.universe_scope([leg.panel_feature for leg in legs], operand)))
    planned = {p.feature_id for p in plans if p.status == "planned"}
    sources = {(p.entry.source_kind, p.entry.metric_code) for p in plans if p.status == "planned"}
    if _SUE_PANEL in sources and _SUE_EVENT in sources:
        raise FeatureStoreError("one SUE hypothesis: the panel's sue_ni and the event-clocked sue_ni_event read the "
                                "same states; catalog (or select) only one of them")
    missing_controls = [c for c in options.control_features if c not in planned]
    if missing_controls:
        raise FeatureStoreError(f"controls {missing_controls} cannot be built from this catalog and panel run "
                                "(the evaluation reads them at rank_normal)")
    unknown = sorted(set(options.features or ()) - {p.feature_id for p in plans})
    if unknown:
        raise FeatureStoreError(f"features {unknown} are not catalog rows")
    return plans


# ---------------------------------------------------------------------------
# Staging (read-only warehouse and panel relations)
# ---------------------------------------------------------------------------

def _stage_universe(con: Any, run_id: str) -> None:
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rf_calendar AS
        SELECT formation_date, cutoff, coalesce(eligible_members, 0) AS eligible_members
        FROM research_panel_calendar WHERE run_id=? AND status=?
    """, [run_id, _panel.CALENDAR_FORMED])
    # ``in_universe``: a linked issuer's primary line (every feature's universe);
    # ``owner_basis``: that, or an eligible line without a usable owner link (price-line
    # features only), else NULL (never ranked).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rf_cohort AS
        SELECT k.formation_date, k.security_id, k.cohort_reason, coalesce(k.primary_line, false) AS primary_line,
               k.owner_cik, k.cohort_reason = 'valid' AND coalesce(k.primary_line, false) AS in_universe,
               CASE WHEN k.cohort_reason = 'valid' AND coalesce(k.primary_line, false) THEN '{OWNER_BASIS_LINKED}'
                    WHEN k.cohort_reason IN ({_sql_list(UNLINKED_COHORT_REASONS)}) THEN '{OWNER_BASIS_UNLINKED}'
                    END AS owner_basis
        FROM research_panel_cohort k JOIN _rf_calendar c ON c.formation_date = k.formation_date
        WHERE k.run_id=? AND coalesce(k.eligible, false)
    """, [run_id])


def _stage_size(con: Any, run_id: str, size_feature: str, verified: str) -> int:
    """``_rf_size``: ln(verified market cap) of every universe line (the size regressor)."""
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _rf_size AS
        SELECT v.formation_date, v.security_id, ln(v.raw_value) AS log_size
        FROM research_panel_values v
        JOIN _rf_cohort k ON k.formation_date=v.formation_date AND k.security_id=v.security_id AND k.in_universe
        WHERE v.run_id=? AND v.feature_id=? AND v.reason='valid' AND v.size_status=?
          AND v.raw_value > 0 AND isfinite(v.raw_value)
    """, [run_id, size_feature, verified])
    return int(con.execute("SELECT count(*) FROM _rf_size").fetchone()[0])


_CIK_SQL = "lpad(CAST(TRY_CAST({column} AS BIGINT) AS VARCHAR), 10, '0')"


def _stage_classification(store: ResearchStore, basis: str, taxonomy: str) -> dict[str, Any]:
    """``_rf_class``: one industry group per universe line (``classification_basis`` labels how)."""
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _rf_class "
                "(formation_date DATE, security_id VARCHAR, classification_group VARCHAR)")
    label = f"{CLASSIFICATION_BASES[basis]}:{taxonomy}"
    if not (store.warehouse_has("entity_classification") and store.warehouse_has("taxonomy")):
        return {"classification_basis": label, "source": "missing", "rows": 0}
    ids = store.warehouse_has("security_identifier_history")
    regex_cik = "lpad(nullif(regexp_extract(ec.security_id, 'CIK-([0-9]{1,10})$', 1), ''), 10, '0')"
    if basis == "strict":
        # Point in time: the classification row and (when the warehouse has dated
        # identifier history) the CIK of the classified security must both be valid
        # at the formation and known by its cutoff.
        if ids:
            mapped = f"""
                SELECT ec.*, {_CIK_SQL.format(column='ih.id_value')} AS cik, ih.valid_from AS id_from,
                       ih.valid_to AS id_to, ih.as_of_date AS id_as_of, ih.available_at AS id_at
                FROM classified ec JOIN security_identifier_history ih
                  ON ih.security_id = ec.security_id AND ih.id_type = 'CIK'"""
            id_valid = ("AND m.id_from <= u.formation_date AND coalesce(m.id_to, DATE '9999-12-31') > "
                        "u.formation_date AND m.id_as_of <= u.formation_date "
                        "AND coalesce(m.id_at, c.cutoff) <= c.cutoff")
        else:
            mapped = f"SELECT ec.*, {regex_cik} AS cik FROM classified ec"
            id_valid = ""
        con.execute(f"""
            INSERT INTO _rf_class
            WITH classified AS (
                SELECT ec.security_id, ec.node_code, ec.valid_from, ec.valid_to, ec.as_of_date, ec.available_at,
                       ec.source_loaded_at, ec.classification_id
                FROM entity_classification ec JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id AND t.code = ?
            ), mapped AS ({mapped})
            -- node_code is NOT NULL (entity_classification DDL): plain arg_max cannot skip the newest row.
            SELECT u.formation_date, u.security_id,
                   arg_max(m.node_code, (m.available_at, m.as_of_date, m.valid_from, m.source_loaded_at,
                                         m.classification_id))
            FROM _rf_cohort u
            JOIN _rf_calendar c ON c.formation_date = u.formation_date
            JOIN mapped m ON m.cik = u.owner_cik
             AND m.valid_from <= u.formation_date AND coalesce(m.valid_to, DATE '9999-12-31') > u.formation_date
             AND m.as_of_date <= u.formation_date AND m.available_at <= c.cutoff {id_valid}
            WHERE u.in_universe
            GROUP BY u.formation_date, u.security_id
        """, [taxonomy])
    else:
        id_map = (f"LEFT JOIN (SELECT security_id, max({_CIK_SQL.format(column='id_value')}) AS cik "
                  "FROM security_identifier_history WHERE id_type = 'CIK' GROUP BY security_id) ih "
                  "ON ih.security_id = ec.security_id" if ids else "")
        cik = f"coalesce(ih.cik, {regex_cik})" if ids else regex_cik
        con.execute(f"""
            INSERT INTO _rf_class
            WITH classified AS (
                SELECT {cik} AS cik, ec.node_code, ec.valid_to, ec.available_at, ec.as_of_date, ec.valid_from,
                       ec.source_loaded_at, ec.classification_id
                FROM entity_classification ec
                JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id AND t.code = ?
                {id_map}
            ), current AS (
                SELECT cik, node_code FROM classified WHERE cik IS NOT NULL
                QUALIFY row_number() OVER (
                    PARTITION BY cik
                    ORDER BY (valid_to IS NULL) DESC, available_at DESC NULLS LAST, as_of_date DESC,
                             valid_from DESC, source_loaded_at DESC, classification_id DESC) = 1
            )
            SELECT u.formation_date, u.security_id, c.node_code
            FROM _rf_cohort u JOIN current c ON c.cik = u.owner_cik
            WHERE u.in_universe
        """, [taxonomy])
    rows = int(con.execute("SELECT count(*) FROM _rf_class").fetchone()[0])
    return {"classification_basis": label, "source": "entity_classification", "rows": rows}


def _stage_items(store: ResearchStore, codes: Sequence[str], formations: Sequence[dt.date], chunk: int) -> int:
    """``_rf_item``: each linked universe line's latest standardized item visible at the cutoff.

    The owner is the line's CIK (every fundamentals owner id of that CIK, as
    market_daily's owner members); the state is the latest period visible at the
    cutoff (period end, then availability, then source/rule/basis/id), the derived
    engine's item order. As in the engine, an explicit NULL/non-finite latest state is
    kept (``arg_max_null``) and makes the leg invalid; it never falls back to an older
    finite state. The source scan is semi-joined to the universe's owner CIKs.
    """
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _rf_item (code VARCHAR, formation_date DATE, security_id VARCHAR, "
                "value DOUBLE, available_at TIMESTAMP, period_end DATE)")
    if not codes:
        return 0
    if not store.warehouse_has("fundamental_standardized"):
        raise FeatureStoreError("warehouse lacks fundamental_standardized (item legs/operands)")
    owner_cik = ("lpad(coalesce(nullif(regexp_extract(f.security_id, 'CIK-([0-9]{1,10})$', 1), ''), "
                 "nullif(ltrim(f.cik, '0'), '')), 10, '0')")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rf_item_src AS
        SELECT {owner_cik} AS cik, f.canonical_code AS code, f.period_end, f.value, f.available_at, f.source,
               f.rule_id, f.basis, f.standardized_id
        FROM fundamental_standardized f
        WHERE f.canonical_code IN ({_sql_list(codes)}) AND f.basis IN ({_sql_list(ITEM_BASES)})
          AND f.available_at IS NOT NULL
          AND {owner_cik} IN (SELECT DISTINCT owner_cik FROM _rf_cohort WHERE in_universe AND owner_cik IS NOT NULL)
    """)
    for start in range(0, len(formations), chunk):
        part = formations[start:start + chunk]
        con.execute("""
            INSERT INTO _rf_item
            SELECT f.code, u.formation_date, u.security_id,
                   arg_max_null(f.value, (f.period_end, f.available_at, f.source, f.rule_id, f.basis,
                                          f.standardized_id)),
                   arg_max(f.available_at, (f.period_end, f.available_at, f.source, f.rule_id, f.basis,
                                            f.standardized_id)),
                   max(f.period_end)
            FROM _rf_cohort u
            JOIN _rf_calendar c ON c.formation_date = u.formation_date
            JOIN _rf_item_src f ON f.cik = u.owner_cik AND f.available_at <= c.cutoff
                               AND f.period_end <= u.formation_date
            WHERE u.in_universe AND u.formation_date BETWEEN ? AND ?
            GROUP BY f.code, u.formation_date, u.security_id
        """, [part[0], part[-1]])
    return int(con.execute("SELECT count(*) FROM _rf_item").fetchone()[0])


def _stage_operands(store: ResearchStore, run_id: str, plans: Sequence[_Plan], verified: str) -> dict[str, int]:
    """``_rf_operand(feature_id, formation_date, security_id, value)`` for every domain operand.

    ``state_metric``/``state_item``: the operand of the *same state*, i.e. the
    operand's state in the feature state's fiscal bucket as visible at the feature
    state's clock (the derived engine's input rule: a metric's latest revision, an
    item's latest period then availability then source/rule/basis/id). ``panel``: the
    same formation's panel row (verified size when the operand is a size feature),
    for every line a feature can rank. ``item_latest``: the staged latest item. A
    missing operand has no row.

    NULL states (engine rule, migration 0315: NULL = a clocked invalid state; rank
    states before testing the value): the selected state is the latest one even when
    its value is NULL (``arg_max_null``); plain ``arg_max`` would skip it and fall
    back to an older valid revision of the same bucket. A NULL operand is then
    ``missing_*_operand`` (excluded, counted), never an older value.
    """
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _rf_operand (feature_id VARCHAR, formation_date DATE, "
                "security_id VARCHAR, value DOUBLE)")
    counts: dict[str, int] = {}
    bucket = "coalesce({alias}.target_bucket, " + bucket_sql("{alias}.period_end") + ")"
    for plan in plans:
        operand = plan.operand
        if operand is None or operand.kind == "leg":
            continue
        if operand.kind in ("state_metric", "state_item"):
            if not store.warehouse_has("derived_metric_values"):
                raise FeatureStoreError("warehouse lacks derived_metric_values (same-state domain operands)")
            con.execute(f"""
                CREATE OR REPLACE TEMP TABLE _rf_state AS
                SELECT v.formation_date, v.security_id, d.security_id AS owner_id,
                       {bucket.format(alias='d')} AS bucket, d.available_at AS state_at
                FROM research_panel_values v
                JOIN _rf_cohort k ON k.formation_date=v.formation_date AND k.security_id=v.security_id
                                 AND k.in_universe
                JOIN derived_metric_values d ON d.derived_value_id = v.derived_value_id
                WHERE v.run_id=? AND v.feature_id=? AND v.reason='valid'
            """, [run_id, plan.panel_feature])
            if operand.kind == "state_metric":
                con.execute(f"""
                    INSERT INTO _rf_operand
                    SELECT ?, s.formation_date, s.security_id,
                           arg_max_null(o.value, (o.available_at, o.derived_value_id))
                    FROM _rf_state s
                    JOIN derived_metric_values o
                      ON o.security_id = s.owner_id AND o.source = ? AND o.metric_code = ?
                     AND o.metric_window = ? AND {bucket.format(alias='o')} = s.bucket
                     AND o.available_at <= s.state_at
                    GROUP BY s.formation_date, s.security_id
                """, [plan.feature_id, DERIVED_SOURCE_NAME, operand.code, operand.window])
            else:
                if not store.warehouse_has("fundamental_standardized"):
                    raise FeatureStoreError("warehouse lacks fundamental_standardized (same-state item operands)")
                con.execute(f"""
                    INSERT INTO _rf_operand
                    SELECT ?, s.formation_date, s.security_id,
                           arg_max_null(f.value, (f.period_end, f.available_at, f.source, f.rule_id, f.basis,
                                                  f.standardized_id))
                    FROM _rf_state s
                    JOIN fundamental_standardized f
                      ON f.security_id = s.owner_id AND f.canonical_code = ?
                     AND f.basis IN ({_sql_list(ITEM_BASES)}) AND f.available_at IS NOT NULL
                     AND {bucket_sql('f.period_end')} = s.bucket AND f.available_at <= s.state_at
                    GROUP BY s.formation_date, s.security_id
                """, [plan.feature_id, operand.code])
        elif operand.kind == "panel":
            con.execute("""
                INSERT INTO _rf_operand
                SELECT ?, v.formation_date, v.security_id,
                       CASE WHEN v.reason='valid' AND isfinite(v.raw_value) AND (NOT ? OR v.size_status = ?)
                            THEN v.raw_value END
                FROM research_panel_values v
                JOIN _rf_cohort k ON k.formation_date=v.formation_date AND k.security_id=v.security_id
                                 AND (k.in_universe OR (? AND k.owner_basis IS NOT NULL))
                WHERE v.run_id=? AND v.feature_id=?
            """, [plan.feature_id, operand.size_feature, verified, plan.price_line, run_id, operand.panel_feature])
        elif operand.kind == "item_latest":
            con.execute("""
                INSERT INTO _rf_operand
                SELECT ?, formation_date, security_id, value FROM _rf_item WHERE code = ?
            """, [plan.feature_id, operand.code])
        counts[plan.feature_id] = int(con.execute(
            "SELECT count(*) FROM _rf_operand WHERE feature_id=?", [plan.feature_id]).fetchone()[0])
    return counts


def _store_has(store: ResearchStore, table: str) -> bool:
    """Whether the research catalog itself (not the warehouse, not temp) has ``table``."""
    return bool(store.con.execute("""
        SELECT count(*) FROM duckdb_tables()
        WHERE database_name = ? AND schema_name = 'main' AND table_name = ? AND NOT temporary
    """, [store.catalog, table]).fetchone()[0])


def _load_external_source(store: ResearchStore, kind: str, pin: str, context: _PanelContext) -> dict[str, Any]:
    """Validate one pinned source version: this panel seal, its contract, its own validator.

    P3 and P9 versions pass :func:`events.validate_event_version` /
    :func:`ownership_features.validate_ownership_version` (seal invariants, including no value
    visible after its cutoff, and both content digests). A P4 run has no validator: it must be
    ``complete`` (``untestable_strict`` on an untestable panel) with its results digest intact.
    """
    table, key, digest_column, contract_column = _SOURCE_TABLES[kind]
    where = f"{kind} source {pin!r}"
    if not _store_has(store, table):
        raise FeatureStoreError(f"{where}: the research store has no {table}")
    row = store.con.execute(f"""
        SELECT status, panel_run_id, panel_sha256, {contract_column}, blockers_json, {digest_column}
        FROM {table} WHERE {key} = ?
    """, [pin]).fetchone()
    if row is None:
        raise FeatureStoreError(f"{where} is absent")
    status, run_id, panel_sha, contract, blockers_json, digest = row
    if contract != EXTERNAL_QUERY_VERSIONS[kind]:
        raise FeatureStoreError(f"{where} was built by {contract!r}; this adapter reads "
                                f"{EXTERNAL_QUERY_VERSIONS[kind]!r} only")
    if run_id != context.run_id or panel_sha != context.panel_sha256:
        raise FeatureStoreError(f"{where} is built on panel run {run_id} ({panel_sha}), not on the seal of "
                                f"{context.run_id} ({context.panel_sha256})")
    try:
        if kind == SOURCE_EVENT:
            from . import events as _events

            _events.validate_event_version(store, pin)
        elif kind == SOURCE_OWNERSHIP:
            from . import ownership_features as _ownership

            _ownership.validate_ownership_version(store, pin)
        else:
            from . import factor_returns as _factors  # imports this module: never at module level

            wanted = FEATURE_UNTESTABLE if context.status == STATUS_UNTESTABLE else "complete"
            if status != wanted:
                raise FeatureStoreError(f"{where} is {status!r}, not {wanted!r}")
            if _factors.results_digest(store.con, pin)[0] != digest:
                raise FeatureStoreError(f"{where}: result rows differ from their sealed digest")
    except FeatureStoreError:
        raise
    except (ValueError, duckdb.Error) as error:
        raise FeatureStoreError(f"{where} fails its validator: {error}") from error
    return {"version": pin, "status": str(status), "query_version": str(contract), "sha256": str(digest),
            "blockers": [str(b) for b in json.loads(blockers_json or "[]")]}


def _external_sources(store: ResearchStore, context: _PanelContext, options: FeatureStoreOptions,
                      plans: Sequence[_Plan]) -> dict[str, dict[str, Any]]:
    """The validated source of every kind a planned feature reads (unused pins are ignored)."""
    kinds = sorted({p.external[0] for p in plans if p.status == "planned" and p.external is not None})
    return {kind: _load_external_source(store, kind, getattr(options, _PIN_FIELDS[kind]), context)
            for kind in kinds}


def _external_source_sql(kind: str, code: str) -> str:
    """One source feature's rows: formation_date, security_id, source_basis, source_cik, value,
    source_reason, available_at, age_days, value_flag, conditioning_flag (parameters: the pin,
    then the feature id except for P4).

    ``source_basis`` is the owner basis the source gives the row (P3 is dense over the linked
    primary lines only, so its rows are ``linked_primary``); ``source_cik`` the owner it names
    (NULL for P4, which names none, and for a P9 row on an unlinked line).
    """
    if kind == SOURCE_FACTOR:
        status = _FACTOR_STATUS[code]
        return f"""
            SELECT formation_date, security_id, owner_basis AS source_basis, CAST(NULL AS VARCHAR) AS source_cik,
                   CAST({code} AS DOUBLE) AS value,
                   CASE WHEN {status} = '{_FACTOR_ESTIMATED}' THEN 'valid' ELSE {status} END AS source_reason,
                   available_at, CAST(NULL AS INTEGER) AS age_days, CAST(NULL AS VARCHAR) AS value_flag,
                   CAST(NULL AS VARCHAR) AS conditioning_flag
            FROM research_factor_exposures WHERE run_id = ?"""
    if kind == SOURCE_EVENT:
        return f"""
            SELECT formation_date, security_id, '{OWNER_BASIS_LINKED}' AS source_basis, owner_cik AS source_cik,
                   CAST(raw_value AS DOUBLE) AS value, reason AS source_reason, available_at,
                   CAST(date_diff('day', event_session, formation_date) AS INTEGER) AS age_days,
                   CAST(NULL AS VARCHAR) AS value_flag, CAST(NULL AS VARCHAR) AS conditioning_flag
            FROM research_event_features WHERE event_version = ? AND feature_id = ?"""
    from ..ownership_identity import CONDITIONING_SURVIVOR

    return f"""
        SELECT formation_date, security_id, owner_basis AS source_basis, owner_cik AS source_cik,
               CAST(raw_value AS DOUBLE) AS value, reason AS source_reason, available_at,
               CAST(date_diff('day', source_period, formation_date) AS INTEGER) AS age_days, value_flag,
               CASE WHEN sample_conditioning = {_sql_text(CONDITIONING_SURVIVOR)} THEN sample_conditioning END
                 AS conditioning_flag
        FROM research_ownership_features WHERE ownership_version = ? AND feature_id = ?"""


def _stage_external_rows(con: Any, plan: _Plan, pin: str) -> None:
    """``_rf_rows`` of a research-store source feature over every eligible cohort row.

    The source row of the same formation and line (the source's newest state visible at
    that cutoff, NULL included) gives the value only when its reason is valid and its own
    clock is at or before the cutoff; a missing row, an invalid reason or a late clock is
    NULL with ``<kind>:<reason>``. The pinned contract: every source row is on a line of the
    R2b universe (a linked primary or an unlinked eligible line), with the owner basis R2b
    gives that line and, when the source names an owner, that line's owner (none on an
    unlinked line); one row per formation and line. A break stops the build. A source row on
    a line outside the feature's scope (a P9 13F row on an unlinked line) is in the contract
    and never read: the universe gate counts that line first.
    """
    if plan.external is None:
        raise FeatureStoreError(f"{plan.feature_id} is not a research-store source feature")
    kind, code = plan.external
    params = [pin] if kind == SOURCE_FACTOR else [pin, code]
    con.execute(f"CREATE OR REPLACE TEMP TABLE _rf_source AS {_external_source_sql(kind, code)}", params)
    owner_check = ("false" if kind == SOURCE_FACTOR else
                   f"CASE WHEN k.owner_basis = '{OWNER_BASIS_LINKED}' THEN s.source_cik IS DISTINCT FROM k.owner_cik "
                   "ELSE s.source_cik IS NOT NULL END")
    duplicates, outside, other_basis, other_owner = (int(v) for v in con.execute(f"""
        SELECT count(*) - count(DISTINCT (s.formation_date, s.security_id)),
               count(*) FILTER (WHERE k.owner_basis IS NULL),
               count(*) FILTER (WHERE k.owner_basis IS NOT NULL AND s.source_basis IS DISTINCT FROM k.owner_basis),
               count(*) FILTER (WHERE k.owner_basis IS NOT NULL AND {owner_check})
        FROM _rf_source s
        LEFT JOIN _rf_cohort k ON k.formation_date = s.formation_date AND k.security_id = s.security_id
    """).fetchone())
    if duplicates or outside or other_basis or other_owner:
        raise FeatureStoreError(f"{plan.feature_id}: {kind} source {pin} breaks its contract (duplicates="
                                f"{duplicates}, rows_outside_universe={outside}, owner_basis_mismatch={other_basis}, "
                                f"owner_mismatch={other_owner})")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rf_rows AS
        WITH r AS (
            SELECT k.formation_date, k.security_id, k.owner_basis, k.cohort_reason, k.primary_line, c.cutoff,
                   s.security_id IS NOT NULL AS has_source, s.source_reason, s.value AS raw_value,
                   s.available_at AS source_at, s.age_days, s.value_flag, s.conditioning_flag
            FROM _rf_cohort k
            JOIN _rf_calendar c ON c.formation_date = k.formation_date
            LEFT JOIN _rf_source s ON s.formation_date = k.formation_date AND s.security_id = k.security_id
        ), g AS (
            SELECT r.*, CASE
                {_universe_gate(plan, 'r.')}
                WHEN NOT r.has_source THEN '{kind}:missing_source_row'
                WHEN r.source_reason IS DISTINCT FROM 'valid' THEN '{kind}:' || coalesce(r.source_reason, 'no_reason')
                WHEN r.source_at IS NULL OR r.source_at > r.cutoff THEN '{kind}:not_visible_at_cutoff'
                WHEN r.raw_value IS NULL OR NOT isfinite(r.raw_value) THEN 'nonfinite_value'
                END AS gate
            FROM r
        )
        SELECT formation_date, security_id, owner_basis, raw_value,
               CASE WHEN gate IS NULL THEN source_at END AS available_at, age_days,
               CAST(NULL AS VARCHAR) AS size_status, value_flag, conditioning_flag,
               {_status_sql(plan.entry, 'raw_value', None, plan.log_base)} AS status
        FROM g
    """)


def _relation_digest(con: Any, relation: str, columns: Sequence[str], order: str,
                     formations: Sequence[dt.date], chunk: int, where: str = "TRUE",
                     params: Sequence[Any] = ()) -> str:
    """Content digest of a formation-keyed relation, bounded per formation chunk."""
    row_json = "to_json(struct_pack(" + ",".join(f"{c} := {c}" for c in columns) + "))"
    parts: list[list[str]] = []
    for start in range(0, len(formations), chunk):
        part = formations[start:start + chunk]
        for day, digest, rows in con.execute(f"""
            SELECT formation_date, sha256(string_agg({row_json}, chr(10) ORDER BY {order})), count(*)
            FROM {relation} WHERE {where} AND formation_date BETWEEN ? AND ?
            GROUP BY formation_date ORDER BY formation_date
        """, [*params, part[0], part[-1]]).fetchall():
            parts.append([day.isoformat(), digest, str(rows)])
    return _sha(_canonical(parts))


# ---------------------------------------------------------------------------
# Per-feature rows (gates, domain, compositions)
# ---------------------------------------------------------------------------

def _domain_case(entry: AnomalyCatalogEntry, value: str, operand: str | None) -> str:
    """SQL: ``'in_domain'`` or the out-of-domain status of a gated row."""
    rule = entry.domain_rule
    if rule in ("unrestricted", "guarded_in_definition"):
        return f"'{IN_DOMAIN}'"
    if rule == "positive_value_required":
        return f"CASE WHEN {value} > 0 THEN '{IN_DOMAIN}' ELSE 'nonpositive_value' END"
    if rule == "zero_payer_separate":
        return f"CASE WHEN {value} = 0 THEN 'zero_payer' ELSE '{IN_DOMAIN}' END"
    labels = {"positive_denominator_required": ("missing_denominator", "nonpositive_denominator"),
              "negative_book_excluded": ("missing_book_operand", "negative_book"),
              "loss_firms_separate": ("missing_loss_operand", "loss_firm")}
    if rule not in labels:
        raise FeatureStoreError(f"{entry.feature_id}: unknown domain rule {rule!r}")
    missing, outside = labels[rule]
    if operand is None:
        if rule == "positive_denominator_required":
            raise FeatureStoreError(f"{entry.feature_id}: positive_denominator_required needs an operand")
        return f"CASE WHEN {value} <= 0 THEN '{outside}' ELSE '{IN_DOMAIN}' END"
    return (f"CASE WHEN {operand} IS NULL OR NOT isfinite({operand}) THEN '{missing}' "
            f"WHEN {operand} <= 0 THEN '{outside}' ELSE '{IN_DOMAIN}' END")


def _status_sql(entry: AnomalyCatalogEntry, value: str, operand: str | None, log_base: bool = False) -> str:
    """SQL: the row status, i.e. the first failing gate (column ``gate``), else the domain.

    An operand rule is decided before the value is looked at (a zero denominator is
    ``nonpositive_denominator``, not a non-finite ratio); a value rule needs a finite
    value first. A ``log_winsor_z`` feature ranks ln(value), so a value at or below
    zero is ``nonpositive_value`` whatever the catalog domain says (never an in-domain
    row with every variant NULL).
    """
    domain = _domain_case(entry, value, operand)
    nonfinite = f"CASE WHEN {value} IS NULL OR NOT isfinite({value}) THEN 'nonfinite_value' END"
    if operand is not None:
        status = f"coalesce(gate, nullif({domain}, '{IN_DOMAIN}'), {nonfinite}, '{IN_DOMAIN}')"
    else:
        status = f"coalesce(gate, {nonfinite}, {domain})"
    if log_base and entry.domain_rule != "positive_value_required":
        status = f"CASE WHEN ({status}) = '{IN_DOMAIN}' AND {value} <= 0 THEN 'nonpositive_value' ELSE {status} END"
    return status


def _universe_gate(plan: _Plan, prefix: str = "") -> str:
    """SQL ``WHEN`` clauses: the universe gates of a row (cohort, then secondary line).

    A price-line feature admits an eligible unlinked line as its own name; every
    feature admits a linked issuer's primary line only.
    """
    unlinked = f" AND {prefix}owner_basis IS DISTINCT FROM '{OWNER_BASIS_UNLINKED}'" if plan.price_line else ""
    return (f"WHEN {prefix}cohort_reason <> 'valid'{unlinked} THEN 'cohort:' || {prefix}cohort_reason "
            f"WHEN {prefix}cohort_reason = 'valid' AND NOT {prefix}primary_line "
            f"THEN '{_panel.SECONDARY_LINE_REASON}'")


def _stage_seed_rows(con: Any, run_id: str, plan: _Plan, verified: str) -> None:
    """``_rf_rows`` of a seed-metric feature: every eligible panel row with its status."""
    entry = plan.entry
    operand = "operand_value" if plan.operand is not None else None
    size_gate = (f"WHEN r.size_status IS DISTINCT FROM {_sql_text(verified)} THEN 'unverified_size'"
                 if plan.size_feature else "")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rf_rows AS
        WITH r AS (
            SELECT v.formation_date, v.security_id, k.owner_basis, k.cohort_reason, k.primary_line,
                   v.reason AS panel_reason, v.value_origin, CAST(v.raw_value AS DOUBLE) AS raw_value,
                   greatest(v.available_at, v.latest_input_clock) AS available_at,
                   v.age_days, v.size_status, o.value AS operand_value
            FROM research_panel_values v
            JOIN _rf_cohort k ON k.formation_date = v.formation_date AND k.security_id = v.security_id
            LEFT JOIN _rf_operand o ON o.feature_id = ? AND o.formation_date = v.formation_date
                                   AND o.security_id = v.security_id
            WHERE v.run_id = ? AND v.feature_id = ?
        ), g AS (
            SELECT r.*, CASE
                {_universe_gate(plan, 'r.')}
                WHEN r.panel_reason <> 'valid' THEN 'panel:' || r.panel_reason
                WHEN r.value_origin IN ({_sql_list(INVALID_ORIGINS)}) THEN 'invalid_value_origin'
                WHEN r.raw_value IS NULL OR NOT isfinite(r.raw_value) THEN 'nonfinite_value'
                {size_gate}
                END AS gate
            FROM r
        )
        SELECT formation_date, security_id, owner_basis, raw_value, available_at, age_days, size_status,
               {_status_sql(entry, 'raw_value', operand, plan.log_base)} AS status
        FROM g
    """, [plan.feature_id, run_id, plan.panel_feature])


def _leg_sql(leg: _Leg, max_age_days: int) -> tuple[str, list[Any]]:
    if leg.kind == "metric":
        return (f"""
            SELECT formation_date, security_id, CAST(raw_value AS DOUBLE) AS value,
                   CASE WHEN reason <> 'valid' THEN 'panel:' || reason
                        WHEN value_origin IN ({_sql_list(INVALID_ORIGINS)}) THEN 'invalid_value_origin'
                        WHEN raw_value IS NULL OR NOT isfinite(raw_value) THEN 'nonfinite_value'
                        ELSE 'valid' END AS status,
                   greatest(available_at, latest_input_clock) AS available_at, age_days, size_status
            FROM research_panel_values WHERE run_id = ? AND feature_id = ?
        """, [leg.panel_feature])
    return (f"""
        SELECT formation_date, security_id, value,
               CASE WHEN value IS NULL OR NOT isfinite(value) THEN 'invalid_item'
                    WHEN date_diff('day', period_end, formation_date) > {int(max_age_days)} THEN 'stale_item'
                    ELSE 'valid' END AS status,
               available_at, CAST(date_diff('day', period_end, formation_date) AS INTEGER) AS age_days,
               CAST(NULL AS VARCHAR) AS size_status
        FROM _rf_item WHERE code = ?
    """, [leg.code])


def _stage_composition_rows(con: Any, run_id: str, plan: _Plan, verified: str, max_age_days: int) -> None:
    """``_rf_rows`` of a market-scaled composition over every eligible cohort row."""
    entry = plan.entry
    numerator, denominator = plan.legs
    num_sql, num_params = _leg_sql(numerator, max_age_days)
    den_sql, den_params = _leg_sql(denominator, max_age_days)
    num_params = [run_id, *num_params] if numerator.kind == "metric" else num_params
    den_params = [run_id, *den_params] if denominator.kind == "metric" else den_params
    gates = []
    for alias, leg in (("n", numerator), ("d", denominator)):
        gates.append(f"WHEN {alias}_status IS NULL THEN '{leg.role}:missing'")
        gates.append(f"WHEN {alias}_status <> 'valid' THEN '{leg.role}:' || {alias}_status")
    for alias, leg in (("n", numerator), ("d", denominator)):
        if leg.size_feature:
            gates.append(f"WHEN {alias}_size IS DISTINCT FROM {_sql_text(verified)} THEN 'unverified_size'")
    operand_leg = plan.operand.leg if plan.operand is not None and plan.operand.kind == "leg" else None
    if operand_leg != "denominator":
        # A market value at or below zero is not a price scale; a declared denominator
        # domain (e.g. EV) is reported by the domain rule instead.
        gates.append("WHEN d_value <= 0 THEN 'denominator:nonpositive_value'")
    operand_expr = None
    operand_join = ""
    if plan.operand is not None:
        if operand_leg is not None:
            operand_expr = "n_value" if operand_leg == "numerator" else "d_value"
        else:
            operand_expr = "o_value"
            operand_join = ("LEFT JOIN _rf_operand o ON o.feature_id = ? AND o.formation_date = b.formation_date "
                            "AND o.security_id = b.security_id")
    ratio = "CASE WHEN d_value <> 0 THEN n_value / d_value END"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rf_rows AS
        WITH n AS ({num_sql}), d AS ({den_sql}), r AS (
            SELECT b.formation_date, b.security_id, b.owner_basis, b.cohort_reason, b.primary_line,
                   n.value AS n_value, n.status AS n_status, n.available_at AS n_at, n.age_days AS n_age,
                   n.size_status AS n_size, d.value AS d_value, d.status AS d_status, d.available_at AS d_at,
                   d.age_days AS d_age, d.size_status AS d_size
                   {', o.value AS o_value' if operand_join else ''}
            FROM _rf_cohort b
            LEFT JOIN n ON n.formation_date = b.formation_date AND n.security_id = b.security_id
            LEFT JOIN d ON d.formation_date = b.formation_date AND d.security_id = b.security_id
            {operand_join}
        ), g AS (
            SELECT r.*, {ratio} AS raw_value, CASE
                {_universe_gate(plan)}
                {' '.join(gates)}
                END AS gate
            FROM r
        )
        SELECT formation_date, security_id, owner_basis, raw_value,
               CASE WHEN gate IS NULL THEN greatest(n_at, d_at) END AS available_at,
               CAST(greatest(n_age, d_age) AS INTEGER) AS age_days,
               CASE WHEN {numerator.size_feature} THEN n_size WHEN {denominator.size_feature} THEN d_size END
                 AS size_status,
               {_status_sql(entry, 'raw_value', operand_expr, plan.log_base)} AS status
        FROM g
    """, [*num_params, *den_params, *([plan.feature_id] if operand_join else [])])


# ---------------------------------------------------------------------------
# Cross-sectional standardization (existing implementations, per formation)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class StandardizationPolicy:
    """Cross-sectional policy of one version (all per formation)."""

    winsor_limits: tuple[float, float] = (0.01, 0.01)
    min_names: int = 200
    industry_min_group_size: int = 5
    neutral_min_coverage: float = 0.80
    taxonomy_code: str = "FAMA_FRENCH_12"


def _inverse_normal(p: np.ndarray) -> np.ndarray:
    unique, inverse = np.unique(p, return_inverse=True)
    values = np.fromiter((_NORMAL.inv_cdf(float(x)) for x in unique), dtype=float, count=len(unique))
    return values[inverse]


def _floats(series: pd.Series) -> np.ndarray:
    return pd.to_numeric(series, errors="coerce").astype("float64").to_numpy()


def standardize_frame(frame: pd.DataFrame, *, feature_id: str, orientation: int, log_base: bool,
                      variants: Sequence[str], policy: StandardizationPolicy,
                      ) -> tuple[pd.DataFrame, dict[tuple[pd.Timestamp, str], dict[str, Any]]]:
    """Standardize one feature's candidate rows of one or more formations.

    ``frame`` columns: ``formation_date``, ``security_id``, ``raw_value``,
    ``domain_status`` (``in_domain`` rows are standardized), ``industry_group`` and
    ``log_size`` (NaN/None where absent). Returns the frame sorted by (formation,
    security) with one column per variant, and per (formation, variant) statistics
    (``date_status``, ``valid_names``, ``in_domain_names``, ``covariate_names``,
    ``covariate_coverage``). Every formation is independent of the others.
    """
    if orientation not in (-1, 1):
        raise FeatureStoreError("orientation must be +1 or -1")
    frame = frame.sort_values(["formation_date", "security_id"], kind="stable").reset_index(drop=True)
    size = len(frame)
    dates = pd.to_datetime(frame["formation_date"]).dt.normalize()
    raw = _floats(frame["raw_value"])
    in_domain = (frame["domain_status"] == IN_DOMAIN).to_numpy(dtype=bool) & np.isfinite(raw)
    base = np.where(in_domain, raw, np.nan)
    if log_base:
        with np.errstate(divide="ignore", invalid="ignore"):
            base = np.log(base)
        in_domain &= np.isfinite(base)
    signed = orientation * base
    out = {variant: np.full(size, np.nan) for variant in VARIANTS}
    out["signed_raw"] = np.where(in_domain, orientation * raw, np.nan)
    counts = pd.Series(in_domain, index=dates.index).groupby(dates.to_numpy()).sum()
    thin = {pd.Timestamp(day) for day, n in counts.items() if n < policy.min_names}
    work = in_domain & ~dates.isin(thin).to_numpy()
    idx = np.flatnonzero(work)
    covariate: dict[tuple[pd.Timestamp, str], tuple[int, float]] = {}
    failing: dict[str, set[pd.Timestamp]] = {"industry_neutral": set(), "size_neutral": set()}
    if len(idx):
        work_frame = pd.DataFrame({"as_of_date": dates.to_numpy()[idx], "value": signed[idx]}, index=idx)
        winsor = _cross_section.winsorize(work_frame, partition_columns=("as_of_date",),
                                          limits=policy.winsor_limits)
        out["winsor"][idx] = _floats(winsor["value"])
        zscore = _cross_section.zscore(winsor, partition_columns=("as_of_date",))
        out["zscore"][idx] = _floats(zscore["value"])
        percent = _cross_section.rank(work_frame, partition_columns=("as_of_date",), method="average")
        names = work_frame.groupby("as_of_date")["value"].transform("count").to_numpy(dtype=float)
        position = np.round(_floats(percent["value"]) * (names - 1.0) * 2.0) / 2.0 + 1.0
        out["rank_normal"][idx] = _inverse_normal((position - 0.375) / (names + 0.25))
        in_counts = {pd.Timestamp(day): int(n) for day, n in counts.items()}
        if "industry_neutral" in variants:
            groups = frame["industry_group"].to_numpy(dtype=object)[idx]
            classified = np.array([g is not None and not (isinstance(g, float) and math.isnan(g)) for g in groups],
                                  dtype=bool)
            panel_frame = pd.DataFrame({"security_id": frame["security_id"].to_numpy()[idx],
                                        "as_of_date": dates.to_numpy()[idx], "factor_id": feature_id,
                                        "value": signed[idx], "_position": idx})
            classes = pd.DataFrame({"security_id": panel_frame["security_id"].to_numpy()[classified],
                                    "as_of_date": panel_frame["as_of_date"].to_numpy()[classified],
                                    "classification_group": groups[classified].astype(str)})
            result = _signal_eval.neutralize_panel_by_industry(
                panel_frame, classes, taxonomy_code=policy.taxonomy_code,
                min_group_size=policy.industry_min_group_size, min_coverage=policy.neutral_min_coverage,
                strict=False)
            usable = result.panel
            positions = usable["_position"].to_numpy(dtype=np.int64)
            usable_counts = usable.groupby("as_of_date").size()
            for day, n_in in in_counts.items():
                if day in thin:
                    continue
                n_use = int(usable_counts.get(day, 0))
                share = n_use / n_in if n_in else 0.0
                covariate[(day, "industry_neutral")] = (n_use, share)
                if n_use < policy.min_names or share < policy.neutral_min_coverage:
                    failing["industry_neutral"].add(day)
            keep = ~pd.to_datetime(usable["as_of_date"]).dt.normalize().isin(failing["industry_neutral"]).to_numpy()
            if keep.any():
                # Unit variance per formation, like the other standardized variants.
                kept = usable.loc[keep, ["as_of_date", "value"]]
                unit = _cross_section.zscore(kept, partition_columns=("as_of_date",))
                out["industry_neutral"][positions[keep]] = _floats(unit["value"])
        if "size_neutral" in variants:
            log_size = _floats(frame["log_size"])
            has = work & np.isfinite(log_size) & np.isfinite(out["rank_normal"])
            size_counts = pd.Series(has, index=dates.index).groupby(dates.to_numpy()).sum()
            for day, n_in in in_counts.items():
                if day in thin:
                    continue
                n_size = int(size_counts.get(day, 0))
                share = n_size / n_in if n_in else 0.0
                covariate[(day, "size_neutral")] = (n_size, share)
                if n_size < policy.min_names or share < policy.neutral_min_coverage:
                    failing["size_neutral"].add(day)
            sidx = np.flatnonzero(has & ~dates.isin(failing["size_neutral"]).to_numpy())
            if len(sidx):
                size_frame = pd.DataFrame({"as_of_date": dates.to_numpy()[sidx], "value": out["rank_normal"][sidx],
                                           "log_size": log_size[sidx]}, index=sidx)
                residual = _cross_section.neutralize(size_frame, by=["log_size"], partition_columns=("as_of_date",))
                standardized = _cross_section.zscore(residual[["as_of_date", "value"]],
                                                     partition_columns=("as_of_date",))
                out["size_neutral"][sidx] = _floats(standardized["value"])
    result_frame = frame.copy()
    for variant in VARIANTS:
        result_frame[variant] = out[variant] if variant in variants else np.nan
    stats: dict[tuple[pd.Timestamp, str], dict[str, Any]] = {}
    unique_days = [pd.Timestamp(day) for day in pd.unique(dates.to_numpy())]
    for variant in variants:
        valid = pd.Series(np.isfinite(result_frame[variant].to_numpy(dtype=float)), index=dates.index)
        valid_counts = valid.groupby(dates.to_numpy()).sum()
        for day in unique_days:
            n_valid = int(valid_counts.get(day, 0))
            if day in thin:
                status = DATE_THIN
            elif variant in failing and day in failing[variant]:
                status = DATE_THIN_COVARIATE
            elif n_valid == 0:
                status = DATE_DEGENERATE
            else:
                status = DATE_FORMED
            names, share = covariate.get((day, variant), (None, None))
            stats[(day, variant)] = {"date_status": status, "valid_names": n_valid,
                                     "in_domain_names": int(counts.get(day, 0)),
                                     "covariate_names": names, "covariate_coverage": share}
    return result_frame, stats


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FeatureVersionResult:
    feature_version: str
    status: str
    basis: str
    panel_run_id: str
    features_built: int
    features_not_built: int
    value_rows: int
    values_sha256: str
    blockers: tuple[str, ...]
    reused: bool = False


def _spec_payload(options: FeatureStoreOptions, context: _PanelContext, plans: Sequence[_Plan],
                  catalog_source: str, sources: Mapping[str, Mapping[str, Any]] | None = None) -> dict[str, Any]:
    payload = _base_spec_payload(options, context, plans, catalog_source)
    if sources:  # E1: only a version that reads a research-store source carries these keys
        payload["external_sources"] = {kind: source["version"] for kind, source in sorted(sources.items())}
        payload["external_source_rule"] = (
            "the source row of the same formation and line; a value only when the source reason is valid "
            "and its own available_at <= cutoff, else NULL counted as <kind>:<reason>")
    return payload


def _base_spec_payload(options: FeatureStoreOptions, context: _PanelContext, plans: Sequence[_Plan],
                       catalog_source: str) -> dict[str, Any]:
    return {
        "query_version": QUERY_VERSION,
        "store_schema": FEATURE_SCHEMA_VERSION,
        "survivor_conditioning": f"{CONDITIONING_LINKED_ONLY} on formed price-line variants that leave out "
                                 "ranked unlinked lines (the neutral variants; no owner covariates)",
        "panel_run_id": context.run_id,
        "basis": context.basis,
        "universe_rule": UNIVERSE_RULE,
        "unlinked_cohort_reasons": list(UNLINKED_COHORT_REASONS),
        "coverage_denominator": "universe_names",
        "industry_neutral": "per-formation zscore of signal_eval within-group centered rank",
        "operand_state_rule": "latest state at the feature state's clock, explicit NULL kept (arg_max_null)",
        "libraries": {"numpy": np.__version__, "pandas": pd.__version__, "duckdb": duckdb.__version__},
        "variants": list(VARIANTS),
        "winsor_limits": list(options.winsor_limits),
        "min_names": options.min_names,
        "industry_min_group_size": options.industry_min_group_size,
        "neutral_min_coverage": options.neutral_min_coverage,
        "taxonomy_code": options.taxonomy_code,
        "classification_basis": f"{CLASSIFICATION_BASES[context.basis]}:{options.taxonomy_code}",
        "rank_normal": "blom (r - 3/8) / (n + 1/4), average ranks",
        "size_regressor": "ln(market_cap) where size_status = verified",
        "size_status_verified": context.verified_status,
        "size_features": sorted(context.size_features),
        "item_max_age_days": context.max_age_days,
        "control_features": list(options.control_features),
        "feature_subset": None if options.features is None else list(options.features),
        "catalog_source": catalog_source,
        "features": [[p.feature_id, p.status, p.reason, p.inputs(), list(p.variants)] for p in plans],
    }


def _catalog_row(version: str, plan: _Plan, status: str, reason: str | None, **extra: Any) -> list[Any]:
    entry = plan.entry
    return [version, entry.feature_id, entry.source_kind, entry.anomaly_class, entry.hypothesis_family,
            int(entry.expected_sign), plan.orientation,
            entry.preferred_transform, PREFERRED_VARIANT.get(entry.preferred_transform), entry.domain,
            _canonical(list(entry.caveat_codes)), entry.admission, entry.is_control,
            plan.universe_scope if plan.status == "planned" else None, _canonical(plan.inputs()),
            status, reason, _canonical(list(plan.variants)), extra.get("value_rows"), extra.get("in_domain_rows"),
            extra.get("reasons_json"), extra.get("values_sha256")]


def _insert_catalog_rows(con: Any, rows: Sequence[list[Any]]) -> None:
    if rows:
        con.executemany(f"INSERT INTO research_feature_catalog ({','.join(_CATALOG_COLUMNS)}) "
                        f"VALUES ({','.join('?' * len(_CATALOG_COLUMNS))})", rows)


def _insert_frame(con: Any, table: str, frame: pd.DataFrame, casts: Mapping[str, str]) -> None:
    if not len(frame):
        return
    con.register("_rf_insert_frame", frame)
    try:
        select = ", ".join(casts.get(name, name) for name in frame.columns)
        con.execute(f"INSERT INTO {table} ({', '.join(frame.columns)}) SELECT {select} FROM _rf_insert_frame")
    finally:
        con.unregister("_rf_insert_frame")


_MATRIX_CASTS = {
    "formation_date": "CAST(formation_date AS DATE)",
    "available_at": "CAST(available_at AS TIMESTAMP)",
    "age_days": "TRY_CAST(age_days AS INTEGER)",
    "expected_sign": "CAST(expected_sign AS INTEGER)",
    **{name: f"CAST(CASE WHEN isfinite({name}) THEN {name} END AS DOUBLE)" for name in ("raw_value", *VARIANTS)},
}
_DATE_CASTS = {
    "formation_date": "CAST(formation_date AS DATE)",
    "universe_names": "TRY_CAST(universe_names AS BIGINT)",
    "covariate_names": "TRY_CAST(covariate_names AS BIGINT)",
    "covariate_coverage": "CAST(covariate_coverage AS DOUBLE)",
    "coverage_fraction": "CAST(coverage_fraction AS DOUBLE)",
    "unlinked_excluded": "TRY_CAST(unlinked_excluded AS BIGINT)",
    "sample_conditioning": "CAST(sample_conditioning AS VARCHAR)",
}
_OWNER_MOMENTS = ("signed_raw_mean", "signed_raw_sd", "rank_normal_mean", "rank_normal_sd")
_OWNER_BASIS_CASTS = {
    "formation_date": "CAST(formation_date AS DATE)",
    "names": "CAST(names AS BIGINT)",
    "ranked_names": "CAST(ranked_names AS BIGINT)",
    **{name: f"CAST(CASE WHEN isfinite({name}) THEN {name} END AS DOUBLE)" for name in _OWNER_MOMENTS},
}


def _feature_digest(con: Any, version: str, feature_id: str, formations: Sequence[dt.date], chunk: int) -> str:
    where, params = "feature_version = ? AND feature_id = ?", [version, feature_id]
    values = _relation_digest(con, "research_feature_matrix", _DIGEST_COLUMNS, "security_id", formations, chunk,
                              where, params)
    dates = _relation_digest(con, "research_feature_dates", _DATE_COLUMNS[1:], "variant", formations, chunk,
                             where, params)
    owner = _relation_digest(con, "research_feature_owner_basis", _OWNER_BASIS_COLUMNS[1:], "owner_basis",
                             formations, chunk, where, params)
    return _sha(_canonical({"values": values, "dates": dates, "owner_basis": owner}))


def _moments(values: np.ndarray) -> tuple[int, float | None, float | None]:
    finite = values[np.isfinite(values)]
    n = len(finite)
    mean = float(np.mean(finite)) if n else None
    sd = float(np.std(finite, ddof=1)) if n > 1 else None
    return n, mean, sd


def _owner_basis_stats(frame: pd.DataFrame) -> dict[tuple[dt.date, str], tuple[Any, ...]]:
    """Per (formation, owner basis) of standardized rows: valued names and moments.

    ``names``/``signed_raw`` over the in-domain valued rows (thin formations
    included), ``ranked_names``/``rank_normal`` over the ranked ones. Rows are in
    (formation, security) order, so the floats do not depend on the chunking.
    """
    days = pd.to_datetime(frame["formation_date"]).dt.date.to_numpy()
    basis = frame["owner_basis"].to_numpy(dtype=object)
    signed = frame["signed_raw"].to_numpy(dtype=float)
    ranked = frame["rank_normal"].to_numpy(dtype=float)
    stats: dict[tuple[dt.date, str], tuple[Any, ...]] = {}
    for day in sorted(set(days.tolist())):
        on_day = days == day
        for owner_basis in OWNER_BASES:
            select = on_day & (basis == owner_basis)
            stats[(day, owner_basis)] = (*_moments(signed[select]), *_moments(ranked[select]))
    return stats


def _unlinked_excluded(frame: pd.DataFrame, variants: Sequence[str]) -> dict[tuple[dt.date, str], int]:
    """Per (formation, variant): ranked unlinked lines (``rank_normal`` valued) without a value.

    Non-zero only for the neutral variants of a price-line feature, whose formed
    cross-sections are then linked-only (survivor-conditioned, R2b N1).
    """
    days = pd.to_datetime(frame["formation_date"]).dt.date.to_numpy()
    ranked = ((frame["owner_basis"].to_numpy(dtype=object) == OWNER_BASIS_UNLINKED)
              & np.isfinite(frame["rank_normal"].to_numpy(dtype=float)))
    counts: dict[tuple[dt.date, str], int] = {}
    for day in sorted(set(days.tolist())):
        on_day = ranked & (days == day)
        for variant in variants:
            counts[(day, variant)] = int((on_day & ~np.isfinite(frame[variant].to_numpy(dtype=float))).sum())
    return counts


class _FeatureWriter:
    """Writes one feature of a version: matrix rows, date rows, owner-basis diagnostic, digest.

    The one write path of R2b's own features (:func:`_build_feature`) and of the features a
    derived version adds (:func:`write_derived_feature_version`), so both digest and validate
    alike. :meth:`add` takes standardized rows of *whole* formations (the output of
    :func:`standardize_frame` or an equivalent, with its per (formation, variant) statistics)
    and inserts them; :meth:`finish` writes the date rows from the per-formation ``reasons``
    and, for a price-line feature, the owner-basis diagnostic, and returns the feature digest
    and its build summary. The caller owns the transaction(s) and the catalog row.
    """

    def __init__(self, con: Any, version: str, feature_id: str, *, expected_sign: int, variants: Sequence[str],
                 price_line: bool, formations: Sequence[dt.date], universe: Mapping[dt.date, Mapping[str, int]],
                 digest_chunk: int) -> None:
        self.con, self.version, self.feature_id = con, version, feature_id
        self.expected_sign, self.variants, self.price_line = int(expected_sign), tuple(variants), price_line
        self.formations, self.universe, self.digest_chunk = tuple(formations), universe, digest_chunk
        self.stats: dict[tuple[dt.date, str], dict[str, Any]] = {}
        self.owner_stats: dict[tuple[dt.date, str], tuple[Any, ...]] = {}
        self.excluded: dict[tuple[dt.date, str], int] = {}
        self.candidates: dict[dt.date, int] = {}
        #: Stored rows per formation and domain status (a derived feature's reason counts).
        self.domain_counts: dict[dt.date, dict[str, int]] = {}
        self.total_rows = 0
        self.in_domain_rows = 0

    def names(self, day: dt.date) -> tuple[int, int]:
        """(eligible members, ranked universe names) of a formation for this feature's scope."""
        counts = self.universe.get(day, {})
        linked, unlinked = int(counts.get("linked", 0)), int(counts.get("unlinked", 0))
        return int(counts.get("eligible", 0)), linked + (unlinked if self.price_line else 0)

    def add(self, standardized: pd.DataFrame, stats: Mapping[tuple[Any, str], Mapping[str, Any]]) -> None:
        if not len(standardized):
            return
        days = pd.to_datetime(standardized["formation_date"]).dt.date
        per_day = standardized.groupby(days).size()
        split = sorted(day for day in per_day.index if day in self.candidates)
        if split:
            raise FeatureStoreError(f"{self.feature_id}: formations {split} arrive in two chunks (whole formations "
                                    "per chunk: standardization is per formation)")
        for (day, variant), value in stats.items():
            self.stats[(pd.Timestamp(day).date(), variant)] = dict(value)
        for day, n in per_day.items():
            self.candidates[day] = int(n)
        for (day, status), n in standardized.groupby([days, standardized["domain_status"]]).size().items():
            self.domain_counts.setdefault(day, {})[str(status)] = int(n)
        self.total_rows += len(standardized)
        self.in_domain_rows += int((standardized["domain_status"] == IN_DOMAIN).sum())
        if self.price_line:
            self.owner_stats.update(_owner_basis_stats(standardized))
            self.excluded.update(_unlinked_excluded(standardized, self.variants))
        matrix = pd.DataFrame({
            "feature_version": self.version,
            "formation_date": standardized["formation_date"],
            "security_id": standardized["security_id"],
            "feature_id": self.feature_id,
            "owner_basis": standardized["owner_basis"],
            "expected_sign": self.expected_sign,
            "available_at": standardized["available_at"],
            "raw_value": _floats(standardized["raw_value"]),
            "domain_status": standardized["domain_status"],
            "age_days": standardized["age_days"],
            **{variant: standardized[variant].to_numpy(dtype=float) for variant in VARIANTS},
        })
        _insert_frame(self.con, "research_feature_matrix", matrix, _MATRIX_CASTS)

    def finish(self, reasons: Mapping[dt.date, Mapping[str, int]]) -> tuple[str, dict[str, Any]]:
        """Date rows, owner-basis diagnostic and digest; ``reasons`` partition each formation's members."""
        date_rows = []
        conditioned: dict[str, int] = {}
        for day in self.formations:
            members, names = self.names(day)
            counts = reasons.get(day, {})
            for variant in self.variants:
                item = self.stats.get((day, variant))
                if item is None:  # no candidate row at all: nothing to standardize
                    item = {"date_status": DATE_THIN, "valid_names": 0, "in_domain_names": 0,
                            "covariate_names": None, "covariate_coverage": None}
                left_out = self.excluded.get((day, variant), 0) if self.price_line else None
                label = CONDITIONING_LINKED_ONLY if left_out and item["date_status"] == DATE_FORMED else None
                if label:
                    conditioned[variant] = conditioned.get(variant, 0) + 1
                date_rows.append({
                    "feature_version": self.version, "formation_date": day, "feature_id": self.feature_id,
                    "variant": variant, "date_status": item["date_status"], "eligible_members": members,
                    "universe_names": names, "valid_names": int(item["valid_names"]),
                    "coverage_fraction": (item["valid_names"] / names) if names else None,
                    "candidate_names": int(self.candidates.get(day, 0)),
                    "in_domain_names": int(item["in_domain_names"]),
                    "covariate_names": item["covariate_names"], "covariate_coverage": item["covariate_coverage"],
                    "reasons_json": _canonical(dict(sorted(counts.items()))), "unlinked_excluded": left_out,
                    "sample_conditioning": label})
        _insert_frame(self.con, "research_feature_dates", pd.DataFrame(date_rows, columns=list(_DATE_COLUMNS)),
                      _DATE_CASTS)
        ranked_by_basis: dict[str, int] = {}
        if self.price_line:
            empty = (0, None, None, 0, None, None)
            owner_rows = [[self.version, day, self.feature_id, owner_basis,
                           *self.owner_stats.get((day, owner_basis), empty)]
                          for day in self.formations for owner_basis in OWNER_BASES]
            owner_frame = pd.DataFrame(owner_rows, columns=list(_OWNER_BASIS_COLUMNS)).astype(
                {name: "float64" for name in _OWNER_MOMENTS})
            _insert_frame(self.con, "research_feature_owner_basis", owner_frame, _OWNER_BASIS_CASTS)
            for row in owner_rows:
                ranked_by_basis[row[3]] = ranked_by_basis.get(row[3], 0) + int(row[7])
        totals: dict[str, int] = {}
        for counts in reasons.values():
            for key, n in counts.items():
                totals[key] = totals.get(key, 0) + n
        digest = _feature_digest(self.con, self.version, self.feature_id, self.formations, self.digest_chunk)
        thin = {variant: sum(1 for day in self.formations
                             if (self.stats.get((day, variant)) or {"date_status": DATE_THIN})["date_status"]
                             != DATE_FORMED) for variant in self.variants}
        result: dict[str, Any] = {"rows": self.total_rows, "in_domain": self.in_domain_rows, "reasons": totals,
                                  "not_formed_dates": thin}
        if self.price_line:
            result["ranked_rows_by_owner_basis"] = ranked_by_basis
            result["survivor_conditioned_dates"] = dict(sorted(conditioned.items()))
        return digest, result


def _build_feature(store: ResearchStore, version: str, plan: _Plan, context: _PanelContext,
                   options: FeatureStoreOptions, policy: StandardizationPolicy,
                   universe: Mapping[dt.date, Mapping[str, int]]) -> dict[str, Any]:
    """Stage, gate, standardize and write one feature (the caller's transaction).

    ``universe``: per formation the R2a ``eligible`` members and the ``linked`` /
    ``unlinked`` line counts of the ranked universes.
    """
    con = store.con
    if plan.external is not None:
        _stage_external_rows(con, plan, getattr(options, _PIN_FIELDS[plan.external[0]]))
    elif plan.is_composition:
        _stage_composition_rows(con, context.run_id, plan, context.verified_status, context.max_age_days)
    else:
        _stage_seed_rows(con, context.run_id, plan, context.verified_status)
    reasons: dict[dt.date, dict[str, int]] = {}
    for day, status, n in con.execute("SELECT formation_date, status, count(*) FROM _rf_rows GROUP BY ALL "
                                      "ORDER BY ALL").fetchall():
        reasons.setdefault(day, {})[str(status)] = int(n)
    candidates_sql = f"r.status IN ({_sql_list(DOMAIN_STATUSES)})"
    if plan.external is not None:
        # Source row flags of stored rows (P9 io_above_one: kept, never clipped; P9
        # cusip_survivor_conditioned), counted beside the status counts (the validator
        # leaves them out of the member sum).
        for day, flag, n in con.execute(f"""
            SELECT r.formation_date, r.flag, count(*) FROM (
                SELECT formation_date, status, value_flag AS flag FROM _rf_rows
                UNION ALL
                SELECT formation_date, status, conditioning_flag AS flag FROM _rf_rows
            ) r
            WHERE r.flag IS NOT NULL AND {candidates_sql} GROUP BY ALL ORDER BY ALL
        """).fetchall():
            reasons.setdefault(day, {})[f"{FLAG_PREFIX}{flag}"] = int(n)
    late = int(con.execute(f"""
        SELECT count(*) FROM _rf_rows r JOIN _rf_calendar c ON c.formation_date = r.formation_date
        WHERE {candidates_sql} AND (r.available_at IS NULL OR r.available_at > c.cutoff)
    """).fetchone()[0])
    if late:
        raise FeatureStoreError(f"{plan.feature_id}: {late} candidate values are not visible at the formation "
                                "cutoff")
    writer = _FeatureWriter(con, version, plan.feature_id, expected_sign=int(plan.entry.expected_sign),
                            variants=plan.variants, price_line=plan.price_line, formations=context.formations,
                            universe=universe, digest_chunk=options.formation_chunk)
    formations = context.formations
    for start in range(0, len(formations), options.formation_chunk):
        part = formations[start:start + options.formation_chunk]
        frame = con.execute(f"""
            SELECT r.formation_date, r.security_id, r.owner_basis, r.raw_value, r.available_at, r.age_days,
                   r.size_status, r.status AS domain_status, g.classification_group AS industry_group,
                   s.log_size
            FROM _rf_rows r
            LEFT JOIN _rf_class g ON g.formation_date = r.formation_date AND g.security_id = r.security_id
            LEFT JOIN _rf_size s ON s.formation_date = r.formation_date AND s.security_id = r.security_id
            WHERE {candidates_sql} AND r.formation_date BETWEEN ? AND ?
            ORDER BY r.formation_date, r.security_id
        """, [part[0], part[-1]]).df()
        if not len(frame):
            continue
        standardized, part_stats = standardize_frame(
            frame, feature_id=plan.feature_id, orientation=plan.orientation, log_base=plan.log_base,
            variants=plan.variants, policy=policy)
        writer.add(standardized, part_stats)
    digest, result = writer.finish(reasons)
    _insert_catalog_rows(con, [_catalog_row(version, plan, FEATURE_BUILT, None, value_rows=writer.total_rows,
                                            in_domain_rows=writer.in_domain_rows,
                                            reasons_json=_canonical(dict(sorted(result["reasons"].items()))),
                                            values_sha256=digest)])
    return result


def _blockers(context: _PanelContext, plans: Sequence[_Plan], options: FeatureStoreOptions,
              catalog_source: str, staged: Mapping[str, Any]) -> list[str]:
    blockers = ["research_only_not_release_eligible"]
    if context.basis == "reconstructed":
        blockers.append("reconstructed_identity_universe_and_availability_not_certifiable")
        blockers.append("classification_current_sic_backcast_not_point_in_time")
    if context.status == STATUS_UNTESTABLE:
        blockers.append("strict_basis_untestable")
    for blocker in staged.get("panel_blockers", []):
        blockers.append(f"panel:{blocker}")
    for kind, source in sorted(staged.get("external_sources", {}).items()):
        blockers.extend(f"{_BLOCKER_PREFIX[kind]}:{blocker}" for blocker in source["blockers"])
    not_built = [p for p in plans
                 if p.status in (FEATURE_INPUT_MISSING, FEATURE_OPERAND_MISSING, FEATURE_SOURCE_NOT_PINNED)]
    if not_built:
        blockers.append(f"catalog_features_not_built:{len(not_built)}")
    if options.features is not None:
        blockers.append("partial_feature_subset")
    if catalog_source == "injected_entries":
        blockers.append("catalog_injected_not_the_committed_seed")
    if context.status != STATUS_UNTESTABLE:
        if not staged.get("classification", {}).get("rows"):
            blockers.append("no_industry_classification_rows")
        if not staged.get("size_rows"):
            blockers.append("no_verified_size_rows")
        if staged.get("unlinked_line_rows") and any(p.price_line for p in plans if p.status == "planned"):
            blockers.append(UNLINKED_LINES_BLOCKER)
    return blockers


def build_feature_version(store: ResearchStore, options: FeatureStoreOptions) -> FeatureVersionResult:
    """Build (or return, or resume) the feature version of one sealed panel run.

    The version id is known before any value is written; an identical sealed
    version is returned unchanged (``reused=True``).
    """
    options = _validate_options(options)
    con = store.con
    with store.transaction():
        ensure_feature_schema(con)
    started = time.perf_counter()
    phases: dict[str, float] = {}
    context = _panel_context(store, options.panel_run_id, options.verify_panel)
    entries, catalog_sha, catalog_source = _load_catalog(options)
    plans = _plan_features(entries, context, options)
    buildable = [p for p in plans if p.status == "planned"]
    sources = _external_sources(store, context, options, buildable)
    phases["plan"] = time.perf_counter() - started
    blockers_json = con.execute("SELECT blockers_json FROM research_panel_runs WHERE run_id=?",
                                [context.run_id]).fetchone()[0]
    staged: dict[str, Any] = {"panel_blockers": json.loads(blockers_json) if blockers_json else []}
    if sources:
        staged["external_sources"] = sources
    chunk = options.formation_chunk
    started = time.perf_counter()
    _stage_universe(con, context.run_id)
    universe: dict[dt.date, dict[str, int]] = {
        day: {"eligible": int(eligible), "linked": int(linked), "unlinked": int(unlinked)}
        for day, eligible, linked, unlinked in con.execute(f"""
            SELECT c.formation_date, c.eligible_members,
                   count(k.security_id) FILTER (WHERE k.owner_basis = '{OWNER_BASIS_LINKED}'),
                   count(k.security_id) FILTER (WHERE k.owner_basis = '{OWNER_BASIS_UNLINKED}')
            FROM _rf_calendar c LEFT JOIN _rf_cohort k ON k.formation_date = c.formation_date
            GROUP BY ALL ORDER BY 1
        """).fetchall()}
    staged["unlinked_line_rows"] = sum(counts["unlinked"] for counts in universe.values())
    if context.status == STATUS_UNTESTABLE:
        con.execute("CREATE OR REPLACE TEMP TABLE _rf_size (formation_date DATE, security_id VARCHAR, "
                    "log_size DOUBLE)")
        con.execute("CREATE OR REPLACE TEMP TABLE _rf_class (formation_date DATE, security_id VARCHAR, "
                    "classification_group VARCHAR)")
        con.execute("CREATE OR REPLACE TEMP TABLE _rf_item (code VARCHAR, formation_date DATE, "
                    "security_id VARCHAR, value DOUBLE, available_at TIMESTAMP, period_end DATE)")
        con.execute("CREATE OR REPLACE TEMP TABLE _rf_operand (feature_id VARCHAR, formation_date DATE, "
                    "security_id VARCHAR, value DOUBLE)")
        staged.update(size_rows=0, classification={"classification_basis": f"{CLASSIFICATION_BASES[context.basis]}"
                                                   f":{options.taxonomy_code}", "source": "not_read", "rows": 0},
                      item_rows=0, operand_rows={})
    else:
        size_feature = context.features.get((SIZE_FEATURE_ID, _panel.MARKET_WINDOW))
        if size_feature is None:
            raise FeatureStoreError("the panel run has no market_cap feature (size regressor and control)")
        staged["size_rows"] = _stage_size(con, context.run_id, size_feature, context.verified_status)
        staged["classification"] = _stage_classification(store, context.basis, options.taxonomy_code)
        item_codes = sorted({leg.code for p in buildable for leg in p.legs if leg.kind == "item"}
                            | {p.operand.code for p in buildable if p.operand and p.operand.kind == "item_latest"})
        staged["item_rows"] = _stage_items(store, item_codes, context.formations, chunk)
        staged["operand_rows"] = _stage_operands(store, context.run_id, buildable, context.verified_status)
    phases["stage"] = time.perf_counter() - started
    started = time.perf_counter()
    inputs = {
        "panel": {"run_id": context.run_id, "panel_sha256": context.panel_sha256,
                  "query_version": context.query_version, "status": context.status, "basis": context.basis},
        "catalog_sha256": catalog_sha,
        "classification_sha256": _relation_digest(con, "_rf_class", ("formation_date", "security_id",
                                                                     "classification_group"),
                                                  "security_id", context.formations, chunk),
        "operands_sha256": _relation_digest(con, "_rf_operand", ("feature_id", "formation_date", "security_id",
                                                                 "value"), "feature_id, security_id",
                                            context.formations, chunk),
        "items_sha256": _relation_digest(con, "_rf_item", ("code", "formation_date", "security_id", "value",
                                                           "available_at", "period_end"), "code, security_id",
                                         context.formations, chunk),
    }
    if sources:  # the pinned source versions and their content digests (E1)
        inputs["external_sources"] = {kind: {key: source[key] for key in ("version", "status", "query_version",
                                                                           "sha256")}
                                      for kind, source in sorted(sources.items())}
    phases["input_digests"] = time.perf_counter() - started
    spec = _spec_payload(options, context, plans, catalog_source, sources)
    spec_json = _canonical(spec)
    spec_sha, code_sha, inputs_json = _sha(spec_json), _code_sha(), _canonical(inputs)
    inputs_sha = _sha(inputs_json)
    version = _sha(_canonical({"spec_sha256": spec_sha, "code_sha256": code_sha, "inputs_sha256": inputs_sha}))
    classification_basis = staged["classification"]["classification_basis"]
    blockers = _blockers(context, plans, options, catalog_source, staged)
    existing = con.execute("SELECT status, values_sha256, blockers_json FROM research_feature_versions "
                           "WHERE feature_version=?", [version]).fetchone()
    if existing is not None and existing[0] in SEALED_STATUSES:
        _, built, rows = _combined_digest(con, version, "")
        return FeatureVersionResult(version, str(existing[0]), context.basis, context.run_id, built,
                                    len(plans) - built, rows, str(existing[1]), tuple(json.loads(existing[2])),
                                    reused=True)
    listed = [_catalog_row(version, p, p.status, p.reason) for p in plans if p.status != "planned"]
    if context.status == STATUS_UNTESTABLE:
        listed += [_catalog_row(version, p, FEATURE_UNTESTABLE, "panel_untestable_strict") for p in buildable]
    with store.transaction():
        if existing is None:
            con.execute("""
                INSERT INTO research_feature_versions
                (feature_version, status, basis, panel_run_id, panel_sha256, classification_basis,
                 query_version, universe_rule, spec_json, spec_sha256, code_sha256, catalog_sha256, inputs_json,
                 inputs_sha256, blockers_json, created_at)
                VALUES (?, 'building', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, [version, context.basis, context.run_id, context.panel_sha256, classification_basis,
                  QUERY_VERSION, UNIVERSE_RULE, spec_json, spec_sha, code_sha, catalog_sha, inputs_json,
                  inputs_sha, _canonical(blockers), _now()])
            _insert_catalog_rows(con, listed)
            # The shared covariates of every line some feature ranks (size regressor,
            # industry group); an unlinked line has no owner, so no owner covariates.
            con.execute("""
                INSERT INTO research_feature_context
                    (feature_version, formation_date, security_id, owner_basis, owner_cik, log_size, industry_group)
                SELECT ?, u.formation_date, u.security_id, u.owner_basis,
                       CASE WHEN u.in_universe THEN u.owner_cik END, s.log_size, g.classification_group
                FROM _rf_cohort u
                LEFT JOIN _rf_size s ON s.formation_date = u.formation_date AND s.security_id = u.security_id
                LEFT JOIN _rf_class g ON g.formation_date = u.formation_date AND g.security_id = u.security_id
                WHERE u.in_universe OR (? AND u.owner_basis IS NOT NULL)
                ORDER BY u.formation_date, u.security_id
            """, [version, context.status != STATUS_UNTESTABLE and any(p.price_line for p in buildable)])
        else:
            con.execute("UPDATE research_feature_versions SET status='building' WHERE feature_version=?", [version])
    policy = StandardizationPolicy(options.winsor_limits, options.min_names, options.industry_min_group_size,
                                   options.neutral_min_coverage, options.taxonomy_code)
    feature_stats: dict[str, Any] = {}
    try:
        started = time.perf_counter()
        if context.status == STATUS_UNTESTABLE:
            with store.transaction():
                done = {row[0] for row in con.execute(
                    "SELECT DISTINCT feature_id FROM research_feature_dates WHERE feature_version=?",
                    [version]).fetchall()}
                _insert_frame(con, "research_feature_dates", _empty_date_rows(
                    version, [(p.feature_id, p.variants) for p in buildable if p.feature_id not in done],
                    context.formations, universe), _DATE_CASTS)
        else:
            done = {row[0] for row in con.execute(
                "SELECT feature_id FROM research_feature_catalog WHERE feature_version=? AND status=?",
                [version, FEATURE_BUILT]).fetchall()}
            for plan in buildable:
                if plan.feature_id in done:
                    continue
                with store.transaction():
                    feature_stats[plan.feature_id] = _build_feature(store, version, plan, context, options, policy,
                                                                    universe)
        phases["features"] = time.perf_counter() - started
        return _finalize(store, version, context, plans, blockers, staged, phases, feature_stats)
    except Exception as exc:
        with store.transaction():
            con.execute("UPDATE research_feature_versions SET status=?, diagnostic_json=? "
                        "WHERE feature_version=? AND status='building'",
                        [STATUS_FAILED, _canonical({"error": type(exc).__name__, "message": str(exc)}), version])
        raise


def _context_digest(con: Any, version: str, formations: Sequence[dt.date], chunk: int) -> str:
    return _relation_digest(con, "research_feature_context", _CONTEXT_COLUMNS, "security_id", formations, chunk,
                            "feature_version = ?", [version])


def _combined_digest(con: Any, version: str, context_sha: str) -> tuple[str, int, int]:
    """``values_sha256``: the per-feature digests (feature order) and the shared context digest."""
    rows = con.execute("""
        SELECT feature_id, values_sha256, coalesce(value_rows, 0) FROM research_feature_catalog
        WHERE feature_version=? AND status=? ORDER BY feature_id
    """, [version, FEATURE_BUILT]).fetchall()
    digest = _sha(_canonical({"features": [[feature, sha] for feature, sha, _ in rows], "context": context_sha}))
    return digest, len(rows), sum(int(n) for _, _, n in rows)


def _finalize(store: ResearchStore, version: str, context: _PanelContext, plans: Sequence[_Plan],
              blockers: Sequence[str], staged: Mapping[str, Any], phases: Mapping[str, float],
              feature_stats: Mapping[str, Any]) -> FeatureVersionResult:
    con = store.con
    buildable = [p for p in plans if p.status == "planned"]
    values_sha, built, value_rows = _combined_digest(
        con, version, _context_digest(con, version, context.formations, _DIGEST_CHUNK))
    if context.status != STATUS_UNTESTABLE and built != len(buildable):
        raise FeatureStoreError(f"{len(buildable) - built} planned features were not built")
    status = STATUS_UNTESTABLE if context.status == STATUS_UNTESTABLE else STATUS_SEALED
    by_status: dict[str, int] = {}
    for plan in plans:
        key = plan.status if plan.status != "planned" else (FEATURE_UNTESTABLE if status == STATUS_UNTESTABLE
                                                            else FEATURE_BUILT)
        by_status[key] = by_status.get(key, 0) + 1
    domain_totals: dict[str, int] = {}
    for row in con.execute("""
        SELECT domain_status, count(*) FROM research_feature_matrix WHERE feature_version=? GROUP BY 1 ORDER BY 1
    """, [version]).fetchall():
        domain_totals[str(row[0])] = int(row[1])
    unverified = 0
    for (reasons_json,) in con.execute("SELECT reasons_json FROM research_feature_catalog WHERE feature_version=? "
                                       "AND reasons_json IS NOT NULL", [version]).fetchall():
        unverified += int(json.loads(reasons_json).get("unverified_size", 0))
    # R2b N1: survivor-conditioned (linked-only) formed variants are labeled per date
    # row; the version carries the count as a blocker so no consumer misses it.
    conditioned = int(con.execute("SELECT count(*) FROM research_feature_dates WHERE feature_version=? "
                                  "AND sample_conditioning IS NOT NULL", [version]).fetchone()[0])
    blockers = [b for b in blockers if not b.startswith(f"{NEUTRAL_CONDITIONING_BLOCKER}:")]
    if conditioned:
        blockers.append(f"{NEUTRAL_CONDITIONING_BLOCKER}:{conditioned}")
    diagnostic = {
        "formations": len(context.formations),
        "features_by_status": dict(sorted(by_status.items())),
        "matrix_rows": value_rows,
        "matrix_rows_by_domain_status": domain_totals,
        "unverified_size_rows_excluded": unverified,
        "survivor_conditioned_date_rows": conditioned,
        "staged": {k: v for k, v in staged.items() if k != "panel_blockers"},
        "phase_seconds": {k: round(v, 3) for k, v in phases.items()},
        "features": feature_stats,
        "research_db_bytes": store.path.stat().st_size if store.path.is_file() else None,
    }
    con.execute("CHECKPOINT")
    with store.transaction():
        con.execute("""
            UPDATE research_feature_versions SET status=?, values_sha256=?, diagnostic_json=?, blockers_json=?,
                   finished_at=?
            WHERE feature_version=? AND status='building'
        """, [status, values_sha, _canonical(diagnostic), _canonical(blockers), _now(), version])
    return FeatureVersionResult(version, status, context.basis, context.run_id, built, len(plans) - built,
                                value_rows, values_sha, tuple(blockers))


def _empty_date_rows(version: str, features: Iterable[tuple[str, Sequence[str]]], formations: Sequence[dt.date],
                     universe: Mapping[dt.date, Mapping[str, int]]) -> pd.DataFrame:
    """``empty_common_cohort`` date rows of features on an ``untestable_strict`` panel (no values)."""
    rows = [{"feature_version": version, "formation_date": day, "feature_id": feature_id, "variant": variant,
             "date_status": DATE_EMPTY, "eligible_members": universe.get(day, {}).get("eligible", 0),
             "universe_names": 0, "valid_names": 0, "coverage_fraction": None, "candidate_names": 0,
             "in_domain_names": 0, "covariate_names": None, "covariate_coverage": None,
             "reasons_json": _canonical({}), "unlinked_excluded": None, "sample_conditioning": None}
            for feature_id, variants in features for day in formations for variant in variants]
    return pd.DataFrame(rows, columns=list(_DATE_COLUMNS))


# ---------------------------------------------------------------------------
# Derived versions (public writer; P5 composites)
# ---------------------------------------------------------------------------

#: Spec and input keys :func:`write_derived_feature_version` sets itself.
_DERIVED_SPEC_KEYS = ("kind", "constituent_feature_version", "constituent_values_sha256", "features")
_DERIVED_INPUT_KEYS = ("constituent_version", "constituent_values_sha256", "external_sources")
_DERIVED_PREFIX = re.compile(r"^[a-z][a-z0-9]{0,15}_$")
#: Reason of an eligible member outside a derived feature's ranked lines.
DERIVED_NOT_RANKED = "not_a_ranked_line"
#: Catalog reason of a derived feature on an ``untestable_strict`` constituent.
DERIVED_UNTESTABLE_REASON = "constituent_untestable_strict"
#: Columns a derived row chunk must carry (``domain_status`` and ``age_days`` are optional).
DERIVED_ROW_COLUMNS = ("formation_date", "security_id", "owner_basis", "raw_value", "available_at", *VARIANTS)


@dataclass(frozen=True)
class DerivedFeature:
    """The catalog row of one feature a derived version adds (:func:`write_derived_feature_version`).

    Per formation its reasons partition the R2a eligible members: stored rows count under
    their ``domain_status``, ranked lines of ``universe_scope`` without a stored row under
    ``unvalued_reason``, and every other eligible member under ``not_a_ranked_line``.
    """

    feature_id: str
    source_kind: str
    anomaly_class: str
    hypothesis_family: str
    expected_sign: int
    preferred_transform: str
    inputs: Mapping[str, Any] = field(default_factory=dict)
    universe_scope: str = UNIVERSE_SCOPE_ALL_LINES
    variants: tuple[str, ...] = VARIANTS
    domain: str = "none"
    caveat_codes: tuple[str, ...] = ()
    admission: str = "eligible"
    is_control: bool = False
    unvalued_reason: str = "not_valued"

    @property
    def orientation(self) -> int:
        return self.expected_sign if self.expected_sign in (-1, 1) else 1

    @property
    def price_line(self) -> bool:
        return self.universe_scope == UNIVERSE_SCOPE_ALL_LINES

    def catalog_inputs(self) -> dict[str, Any]:
        return {**dict(self.inputs), "universe_scope": self.universe_scope}


@dataclass(frozen=True)
class DerivedVersionSpec:
    """A derived feature version: every row of one sealed constituent version plus ``features``.

    The version spec is the constituent's spec updated with ``spec`` and the keys the writer
    sets (``kind``, ``constituent_feature_version``, ``constituent_values_sha256`` and
    ``features``: the constituent's plus these); the version inputs are ``inputs`` plus
    ``constituent_version``, ``constituent_values_sha256`` and the constituent's pinned
    research-store ``external_sources`` (re-checked by :func:`validate_feature_version`). The
    id is ``version_prefix`` + the first 40 hex digits of sha256(spec digest, inputs digest):
    it pins content, not code (``code_sha256`` is recorded; empty records this module's).
    ``blockers`` are added to the constituent's; ``diagnostic`` is stored as given plus counts.
    """

    constituent_version: str
    kind: str
    features: tuple[DerivedFeature, ...]
    spec: Mapping[str, Any] = field(default_factory=dict)
    inputs: Mapping[str, Any] = field(default_factory=dict)
    blockers: tuple[str, ...] = ()
    diagnostic: Mapping[str, Any] = field(default_factory=dict)
    code_sha256: str = ""
    version_prefix: str = "drv_"


@dataclass(frozen=True)
class DerivedFeatureRows:
    """Standardized rows of one derived feature over *whole* formations, with their statistics.

    ``frame``: :data:`DERIVED_ROW_COLUMNS` (``available_at`` at or before the formation's
    cutoff; a variant outside the feature's ``variants`` all NaN), optionally
    ``domain_status`` (default ``in_domain``) and ``age_days``; at most one row per
    (formation, line), on the feature's ranked lines only. ``stats``: for every (formation,
    variant) of the frame the :func:`standardize_frame` statistics (``date_status``,
    ``valid_names``, ``in_domain_names``, ``covariate_names``, ``covariate_coverage``).
    """

    feature_id: str
    frame: pd.DataFrame
    stats: Mapping[tuple[Any, str], Mapping[str, Any]]


@contextlib.contextmanager
def _transaction(con: Any) -> Iterator[Any]:
    con.execute("BEGIN TRANSACTION")
    try:
        yield con
    except Exception:
        con.execute("ROLLBACK")
        raise
    else:
        con.execute("COMMIT")


def _validate_derived_spec(spec: DerivedVersionSpec) -> None:
    if not isinstance(spec, DerivedVersionSpec):
        raise FeatureStoreError("spec must be a DerivedVersionSpec")
    if not isinstance(spec.constituent_version, str) or not _PIN.fullmatch(spec.constituent_version):
        raise FeatureStoreError("constituent_version must be a feature version id")
    if not isinstance(spec.kind, str) or not _ID.fullmatch(spec.kind):
        raise FeatureStoreError("kind must be a lower-case identifier")
    if not isinstance(spec.version_prefix, str) or not _DERIVED_PREFIX.fullmatch(spec.version_prefix):
        raise FeatureStoreError("version_prefix must be 1-16 lower-case characters and an underscore")
    reserved = sorted(set(spec.spec) & set(_DERIVED_SPEC_KEYS)) + sorted(set(spec.inputs) & set(_DERIVED_INPUT_KEYS))
    if reserved:
        raise FeatureStoreError(f"spec/inputs keys {reserved} are set by the writer")
    if not spec.features or not all(isinstance(f, DerivedFeature) for f in spec.features):
        raise FeatureStoreError("a derived version adds one or more DerivedFeature rows")
    ids = [f.feature_id for f in spec.features]
    if len(set(ids)) != len(ids):
        raise FeatureStoreError(f"derived feature ids are not unique: {ids}")
    for feature in spec.features:
        where = f"derived feature {feature.feature_id!r}"
        if not isinstance(feature.feature_id, str) or not _ID.fullmatch(feature.feature_id):
            raise FeatureStoreError(f"{where}: the id must be a lower-case identifier")
        if feature.universe_scope not in UNIVERSE_SCOPES:
            raise FeatureStoreError(f"{where}: universe_scope must be one of {UNIVERSE_SCOPES}")
        if feature.inputs.get("universe_scope", feature.universe_scope) != feature.universe_scope:
            raise FeatureStoreError(f"{where}: inputs name another universe scope")
        if not feature.variants or len(set(feature.variants)) != len(feature.variants) \
                or not set(feature.variants) <= set(VARIANTS):
            raise FeatureStoreError(f"{where}: variants must be unique members of {VARIANTS}")
        if feature.expected_sign not in (-1, 0, 1) or isinstance(feature.expected_sign, bool):
            raise FeatureStoreError(f"{where}: expected_sign must be -1, 0 or +1")
        if not _ID.fullmatch(feature.unvalued_reason) or feature.unvalued_reason == DERIVED_NOT_RANKED \
                or feature.unvalued_reason in DOMAIN_STATUSES:
            raise FeatureStoreError(f"{where}: unvalued_reason must be its own lower-case reason")


def _derived_constituent(con: Any, version: str) -> dict[str, Any]:
    row = con.execute("""
        SELECT status, basis, panel_run_id, panel_sha256, classification_basis, catalog_sha256, spec_json,
               values_sha256, blockers_json, inputs_json
        FROM research_feature_versions WHERE feature_version = ?
    """, [version]).fetchone()
    if row is None or row[0] not in SEALED_STATUSES:
        raise FeatureStoreError(f"constituent feature version {version!r} is absent or not sealed")
    spec = json.loads(row[6])
    if spec.get("store_schema") != FEATURE_SCHEMA_VERSION or spec.get("query_version") != QUERY_VERSION:
        raise FeatureStoreError(f"constituent version {version} predates feature store schema "
                                f"{FEATURE_SCHEMA_VERSION} / {QUERY_VERSION}")
    if "constituent_feature_version" in spec:
        raise FeatureStoreError(f"{version} is itself a derived version: derived versions never seed derived ones")
    return {"status": row[0], "basis": row[1], "panel_run_id": row[2], "panel_sha256": row[3],
            "classification_basis": row[4], "catalog_sha256": row[5], "spec": spec, "values_sha256": row[7],
            "blockers": json.loads(row[8] or "[]"), "inputs": json.loads(row[9] or "{}")}


def _derived_chunk(chunk: DerivedFeatureRows, feature: DerivedFeature,
                   cutoffs: Mapping[dt.date, dt.datetime]) -> pd.DataFrame:
    """A derived chunk checked against the row contract, sorted by (formation, line)."""
    where = f"derived feature {feature.feature_id}"
    missing = [c for c in DERIVED_ROW_COLUMNS if c not in chunk.frame.columns]
    if missing:
        raise FeatureStoreError(f"{where}: rows lack {missing}")
    frame = chunk.frame.copy()
    if "domain_status" not in frame.columns:
        frame["domain_status"] = IN_DOMAIN
    if "age_days" not in frame.columns:
        frame["age_days"] = None
    frame = frame.sort_values(["formation_date", "security_id"], kind="stable").reset_index(drop=True)
    days = pd.to_datetime(frame["formation_date"]).dt.date
    unknown = sorted(set(days) - set(cutoffs))
    if unknown:
        raise FeatureStoreError(f"{where}: rows on {unknown[:3]} (not formed formations of the panel run)")
    if frame.duplicated(["formation_date", "security_id"]).any():
        raise FeatureStoreError(f"{where}: more than one row per formation and line")
    allowed = OWNER_BASES if feature.price_line else (OWNER_BASIS_LINKED,)
    if not frame["owner_basis"].isin(allowed).all():
        raise FeatureStoreError(f"{where}: owner_basis outside {allowed}")
    if not frame["domain_status"].isin(DOMAIN_STATUSES).all():
        raise FeatureStoreError(f"{where}: domain_status outside {DOMAIN_STATUSES}")
    clocks = pd.to_datetime(frame["available_at"])
    late = clocks.isna() | (clocks > pd.to_datetime(days.map(cutoffs)))
    if late.any():
        raise FeatureStoreError(f"{where}: {int(late.sum())} rows are not visible at their formation cutoff")
    stray = [v for v in VARIANTS if v not in feature.variants and np.isfinite(_floats(frame[v])).any()]
    if stray:
        raise FeatureStoreError(f"{where}: values in variants {stray} it does not produce")
    stated = {(pd.Timestamp(day).date(), variant) for day, variant in chunk.stats}
    lacking = sorted({(day, v) for day in set(days) for v in feature.variants} - stated)
    if lacking:
        raise FeatureStoreError(f"{where}: no statistics for {lacking[:3]}")
    return frame


def _derived_catalog_row(version: str, feature: DerivedFeature, status: str, reason: str | None,
                         **extra: Any) -> list[Any]:
    return [version, feature.feature_id, feature.source_kind, feature.anomaly_class, feature.hypothesis_family,
            int(feature.expected_sign), feature.orientation, feature.preferred_transform,
            PREFERRED_VARIANT.get(feature.preferred_transform), feature.domain,
            _canonical(list(feature.caveat_codes)), feature.admission, bool(feature.is_control),
            feature.universe_scope, _canonical(feature.catalog_inputs()), status, reason,
            _canonical(list(feature.variants)), extra.get("value_rows"), extra.get("in_domain_rows"),
            extra.get("reasons_json"), extra.get("values_sha256")]


def write_derived_feature_version(conn: Any, *, spec: DerivedVersionSpec,
                                  rows: Iterable[DerivedFeatureRows] | None) -> str:
    """Write the derived feature version ``spec`` describes and return its id (P5 composites).

    ``conn``: an open research-store connection (``ResearchStore.con``) outside a
    transaction; the writer runs its own. Every row of the sealed constituent version is
    copied (one transaction per feature, so a rerun resumes after the last copied feature),
    then ``rows`` are written through the same writer as R2b's own features (one transaction
    per chunk: the chunk size bounds each transaction), then the version is sealed with its
    date rows, owner-basis diagnostic, catalog rows and digests in one transaction.
    ``rows`` is consumed after the constituent copy, so a generator may read the
    constituent's covariates (``research_feature_context`` of ``constituent_version``: the
    copy is identical). A sealed version with the same id is returned unchanged without
    consuming ``rows``; a building or failed one resumes (its derived rows are rewritten).
    ``rows`` is ``None`` exactly when the constituent is ``untestable_strict``: the derived
    features are then untestable too (``empty_common_cohort`` date rows). The version
    passes :func:`validate_feature_version`. Raises :class:`FeatureStoreError`.
    """
    con = conn
    _validate_derived_spec(spec)
    with _transaction(con):
        ensure_feature_schema(con)
    base = _derived_constituent(con, spec.constituent_version)
    untestable = base["status"] == STATUS_UNTESTABLE
    if untestable != (rows is None):
        raise FeatureStoreError("derived rows are required exactly when the constituent version is sealed "
                                "(None for an untestable_strict constituent)")
    run_id = base["panel_run_id"]
    calendar = con.execute("SELECT formation_date, cutoff, eligible_members FROM research_panel_calendar "
                           "WHERE run_id = ? AND status = ? ORDER BY formation_date",
                           [run_id, _panel.CALENDAR_FORMED]).fetchall()
    formations = [row[0] for row in calendar]
    cutoffs = {row[0]: row[1] for row in calendar}
    universe: dict[dt.date, dict[str, int]] = {
        row[0]: {"eligible": int(row[2] or 0), "linked": 0, "unlinked": 0} for row in calendar}
    for day, owner_basis, n in con.execute(f"""
        SELECT k.formation_date, {_expected_owner_basis_sql("k")} AS owner_basis, count(*)
        FROM research_panel_cohort k WHERE k.run_id = ? GROUP BY ALL
    """, [run_id]).fetchall():
        if owner_basis is not None and day in universe:
            universe[day]["linked" if owner_basis == OWNER_BASIS_LINKED else "unlinked"] = int(n)
    features = tuple(spec.features)
    ids = [f.feature_id for f in features]
    constituent_ids = [r[0] for r in con.execute(
        "SELECT feature_id FROM research_feature_catalog WHERE feature_version = ? ORDER BY feature_id",
        [spec.constituent_version]).fetchall()]
    clash = sorted(set(constituent_ids) & set(ids))
    if clash:
        raise FeatureStoreError(f"{spec.constituent_version} already has features named {clash}")
    version_spec = {**base["spec"], **dict(spec.spec), "kind": spec.kind,
                    "constituent_feature_version": spec.constituent_version,
                    "constituent_values_sha256": base["values_sha256"],
                    "features": [*base["spec"].get("features", []),
                                 *([f.feature_id, "planned", None, f.catalog_inputs(), list(f.variants)]
                                   for f in features)]}
    inputs = {**dict(spec.inputs), "constituent_version": spec.constituent_version,
              "constituent_values_sha256": base["values_sha256"]}
    if base["inputs"].get("external_sources"):
        inputs["external_sources"] = base["inputs"]["external_sources"]
    spec_json, inputs_json = _canonical(version_spec), _canonical(inputs)
    spec_sha, inputs_sha = _sha(spec_json), _sha(inputs_json)
    version = spec.version_prefix + _sha(_canonical({"spec_sha256": spec_sha, "inputs_sha256": inputs_sha}))[:40]
    existing = con.execute("SELECT status FROM research_feature_versions WHERE feature_version = ?",
                           [version]).fetchone()
    if existing is not None and existing[0] in SEALED_STATUSES:
        return version
    with _transaction(con):
        if existing is None:
            con.execute("""
                INSERT INTO research_feature_versions (feature_version, status, basis, panel_run_id, panel_sha256,
                    classification_basis, query_version, universe_rule, spec_json, spec_sha256, code_sha256,
                    catalog_sha256, inputs_json, inputs_sha256, blockers_json, created_at)
                VALUES (?, 'building', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '[]', ?)
            """, [version, base["basis"], run_id, base["panel_sha256"], base["classification_basis"], QUERY_VERSION,
                  UNIVERSE_RULE, spec_json, spec_sha, spec.code_sha256 or _code_sha(), base["catalog_sha256"],
                  inputs_json, inputs_sha, _now()])
            names = ", ".join(_CONTEXT_COLUMNS)
            con.execute(f"INSERT INTO research_feature_context (feature_version, {names}) "
                        f"SELECT ?, {names} FROM research_feature_context WHERE feature_version = ?",
                        [version, spec.constituent_version])
        else:
            con.execute("UPDATE research_feature_versions SET status='building' WHERE feature_version=?", [version])
    try:
        # The constituent's features, one transaction each (its catalog row last: present = copied).
        copied = {r[0] for r in con.execute("SELECT feature_id FROM research_feature_catalog "
                                            "WHERE feature_version = ?", [version]).fetchall()}
        tables = (("research_feature_matrix", tuple(name for name, _ in _MATRIX_COLUMNS)),
                  ("research_feature_dates", _DATE_COLUMNS),
                  ("research_feature_owner_basis", _OWNER_BASIS_COLUMNS),
                  ("research_feature_catalog", _CATALOG_COLUMNS))
        for feature_id in constituent_ids:
            if feature_id in copied:
                continue
            with _transaction(con):
                for table, columns in tables:
                    names = ", ".join(c for c in columns if c != "feature_version")
                    con.execute(f"INSERT INTO {table} (feature_version, {names}) SELECT ?, {names} FROM {table} "
                                "WHERE feature_version = ? AND feature_id = ?",
                                [version, spec.constituent_version, feature_id])
        for feature_id in ids:  # rows of an interrupted earlier attempt
            with _transaction(con):
                for table, _ in tables:
                    con.execute(f"DELETE FROM {table} WHERE feature_version = ? AND feature_id = ?",
                                [version, feature_id])
        con.execute("CHECKPOINT")
        writers = {f.feature_id: _FeatureWriter(con, version, f.feature_id, expected_sign=f.expected_sign,
                                                variants=f.variants, price_line=f.price_line,
                                                formations=formations, universe=universe,
                                                digest_chunk=_DIGEST_CHUNK) for f in features}
        by_id = {f.feature_id: f for f in features}
        for chunk in rows or ():
            if not isinstance(chunk, DerivedFeatureRows) or chunk.feature_id not in by_id:
                raise FeatureStoreError(f"unexpected derived rows {getattr(chunk, 'feature_id', chunk)!r}")
            frame = _derived_chunk(chunk, by_id[chunk.feature_id], cutoffs)
            with _transaction(con):
                writers[chunk.feature_id].add(frame, chunk.stats)
        con.execute("CHECKPOINT")
        with _transaction(con):
            if untestable:
                _insert_frame(con, "research_feature_dates",
                              _empty_date_rows(version, [(f.feature_id, f.variants) for f in features], formations,
                                               universe), _DATE_CASTS)
                _insert_catalog_rows(con, [_derived_catalog_row(version, f, FEATURE_UNTESTABLE,
                                                                DERIVED_UNTESTABLE_REASON) for f in features])
                status = STATUS_UNTESTABLE
            else:
                for feature in features:
                    writer = writers[feature.feature_id]
                    reasons: dict[dt.date, dict[str, int]] = {}
                    for day in formations:
                        members, names_of_day = writer.names(day)
                        stored = writer.candidates.get(day, 0)
                        if stored > names_of_day or names_of_day > members:
                            raise FeatureStoreError(f"{feature.feature_id} {day}: {stored} stored rows, "
                                                    f"{names_of_day} ranked lines, {members} eligible members")
                        counts = dict(writer.domain_counts.get(day, {}))
                        if names_of_day > stored:
                            counts[feature.unvalued_reason] = names_of_day - stored
                        if members > names_of_day:
                            counts[DERIVED_NOT_RANKED] = members - names_of_day
                        reasons[day] = counts
                    digest, summary = writer.finish(reasons)
                    _insert_catalog_rows(con, [_derived_catalog_row(
                        version, feature, FEATURE_BUILT, None, value_rows=writer.total_rows,
                        in_domain_rows=writer.in_domain_rows,
                        reasons_json=_canonical(dict(sorted(summary["reasons"].items()))), values_sha256=digest)])
                status = STATUS_SEALED
            conditioned = int(con.execute("SELECT count(*) FROM research_feature_dates WHERE feature_version = ? "
                                          "AND sample_conditioning IS NOT NULL", [version]).fetchone()[0])
            blockers = [b for b in base["blockers"] if not str(b).startswith(f"{NEUTRAL_CONDITIONING_BLOCKER}:")]
            blockers += list(spec.blockers)
            if conditioned:
                blockers.append(f"{NEUTRAL_CONDITIONING_BLOCKER}:{conditioned}")
            values_sha, _, _ = _combined_digest(con, version, _context_digest(con, version, formations, _DIGEST_CHUNK))
            diagnostic = {**dict(spec.diagnostic), "derived_features": ids, "formations": len(formations),
                          "survivor_conditioned_date_rows": conditioned}
            con.execute("UPDATE research_feature_versions SET status = ?, values_sha256 = ?, blockers_json = ?, "
                        "diagnostic_json = ?, finished_at = ? WHERE feature_version = ?",
                        [status, values_sha, _canonical(sorted(set(blockers))), _canonical(diagnostic), _now(),
                         version])
    except Exception as exc:
        with _transaction(con):
            con.execute("UPDATE research_feature_versions SET status=?, diagnostic_json=? "
                        "WHERE feature_version=? AND status='building'",
                        [STATUS_FAILED, _canonical({"error": type(exc).__name__, "message": str(exc)}), version])
        raise
    return version


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def _expected_owner_basis_sql(alias: str) -> str:
    """SQL: the owner basis an R2a cohort row implies (NULL: the line is never ranked)."""
    eligible = f"coalesce({alias}.eligible, false)"
    return (f"CASE WHEN {eligible} AND {alias}.cohort_reason = 'valid' AND coalesce({alias}.primary_line, false) "
            f"THEN '{OWNER_BASIS_LINKED}' WHEN {eligible} AND {alias}.cohort_reason IN "
            f"({_sql_list(UNLINKED_COHORT_REASONS)}) THEN '{OWNER_BASIS_UNLINKED}' END")


@dataclass(frozen=True)
class FeatureVersionValidation:
    feature_version: str
    status: str
    basis: str
    features_built: int
    value_rows: int
    values_sha256: str
    checks: dict[str, int] = field(default_factory=dict)


def validate_feature_version(store: ResearchStore, feature_version: str, *,
                             verify_panel: bool = True) -> FeatureVersionValidation:
    """Reject a changed, unsealed or contract-violating feature version.

    Re-derives every per-feature digest from the stored rows and checks, per
    feature: the universe clause of its scope (a value only on an eligible valid
    primary line, or for a price-line feature also on an eligible unlinked line) and
    an ``owner_basis`` equal to the one the R2a cohort implies, clocks at or before
    the formation cutoff, NULL variants on out-of-domain rows and on formations whose
    variant is not ``formed`` (``signed_raw`` excepted at thin formations), unique
    grain, complete formation x variant date rows whose reasons sum to the R2a
    ``eligible_members`` and whose ``universe_names`` equal the cohort's ranked lines,
    the owner-basis diagnostic's counts, and a catalog sign on every row. The panel
    run must still carry the seal the version was built from (and, by default, pass
    its own validator). A read path: it never runs DDL.
    """
    con = store.con
    _require_feature_schema(con)
    row = con.execute("""
        SELECT status, basis, panel_run_id, panel_sha256, values_sha256, spec_json, spec_sha256, blockers_json,
               diagnostic_json, inputs_json
        FROM research_feature_versions WHERE feature_version=?
    """, [feature_version]).fetchone()
    if row is None or row[0] not in SEALED_STATUSES:
        raise FeatureStoreError(f"feature version {feature_version!r} is absent or not sealed")
    status, basis, run_id, panel_sha, values_sha, spec_json, spec_sha, blockers_json, diagnostic_json, \
        inputs_json = row
    if _sha(spec_json) != spec_sha:
        raise FeatureStoreError("feature version spec digest mismatch")
    spec = json.loads(spec_json)
    if spec.get("query_version") != QUERY_VERSION:
        raise FeatureStoreError("unsupported feature store query version")
    if spec.get("store_schema") != FEATURE_SCHEMA_VERSION:
        raise FeatureStoreError(f"feature version was built under store schema {spec.get('store_schema', 2)} "
                                f"(before R2e: revived market values, unlabeled survivor conditioning); "
                                f"rebuild it under schema {FEATURE_SCHEMA_VERSION}")
    panel_row = con.execute("SELECT panel_sha256, status FROM research_panel_runs WHERE run_id=?",
                            [run_id]).fetchone()
    if panel_row is None or panel_row[0] != panel_sha:
        raise FeatureStoreError(f"panel run {run_id} no longer carries the seal this version was built from")
    if verify_panel:
        _panel.validate_research_panel(store, run_id)
    # E1: every pinned research-store source still carries the seal the version read.
    for kind, pinned in sorted((json.loads(inputs_json or "{}").get("external_sources") or {}).items()):
        if kind not in _SOURCE_TABLES:
            raise FeatureStoreError(f"unknown research-store source kind {kind!r} in the version inputs")
        table, key, digest_column, _ = _SOURCE_TABLES[kind]
        seal = con.execute(f"SELECT status, {digest_column} FROM {table} WHERE {key}=?",
                           [pinned["version"]]).fetchone() if _store_has(store, table) else None
        if seal is None or [str(seal[0]), str(seal[1])] != [pinned["status"], pinned["sha256"]]:
            raise FeatureStoreError(f"{kind} source {pinned['version']} no longer carries the seal this version "
                                    "was built from")
    formations = [r[0] for r in con.execute("""
        SELECT formation_date FROM research_panel_calendar WHERE run_id=? AND status=? ORDER BY formation_date
    """, [run_id, _panel.CALENDAR_FORMED]).fetchall()]
    chunk = _DIGEST_CHUNK
    catalog = con.execute("""
        SELECT feature_id, status, expected_sign, variants_json, values_sha256, coalesce(value_rows, 0),
               universe_scope
        FROM research_feature_catalog WHERE feature_version=? ORDER BY feature_id
    """, [feature_version]).fetchall()
    planned = [item for item in spec["features"] if item[1] == "planned"]
    if len(catalog) != len(spec["features"]) or {c[0] for c in catalog} != {item[0] for item in spec["features"]}:
        raise FeatureStoreError("feature catalog rows do not match the version spec")
    scopes = {item[0]: item[3].get("universe_scope") for item in planned}
    checks = {"features": 0, "rows": 0}
    built = [c for c in catalog if c[1] == FEATURE_BUILT]
    if status == STATUS_SEALED and len(built) != len(planned):
        raise FeatureStoreError("a sealed version must have built every planned feature")
    expected_basis = _expected_owner_basis_sql("k")
    universe = {day: (int(eligible or 0), int(linked), int(unlinked))
                for day, eligible, linked, unlinked in con.execute(f"""
                    SELECT c.formation_date, c.eligible_members,
                           count(k.security_id) FILTER (WHERE {expected_basis} = '{OWNER_BASIS_LINKED}'),
                           count(k.security_id) FILTER (WHERE {expected_basis} = '{OWNER_BASIS_UNLINKED}')
                    FROM research_panel_calendar c
                    LEFT JOIN research_panel_cohort k ON k.run_id = c.run_id AND k.formation_date = c.formation_date
                    WHERE c.run_id=? AND c.status=? GROUP BY ALL
                """, [run_id, _panel.CALENDAR_FORMED]).fetchall()}
    for feature_id, _, sign, variants_json, stored_sha, value_rows, scope in built:
        variants = json.loads(variants_json)
        if scope not in UNIVERSE_SCOPES or scope != scopes.get(feature_id):
            raise FeatureStoreError(f"{feature_id}: universe scope {scope!r} differs from the version spec")
        all_lines = scope == UNIVERSE_SCOPE_ALL_LINES
        if _feature_digest(con, feature_version, feature_id, formations, chunk) != stored_sha:
            raise FeatureStoreError(f"{feature_id}: stored rows differ from their sealed digest")
        valued = " OR ".join(f"m.{v} IS NOT NULL" for v in VARIANTS)
        formed = ", ".join(f"bool_or(variant = '{v}' AND date_status = '{DATE_FORMED}') AS {v}_ok"
                           for v in STANDARDIZED_VARIANTS)
        unformed = " OR ".join(f"(m.{v} IS NOT NULL AND NOT coalesce(m.{v}_ok, false))"
                               for v in STANDARDIZED_VARIANTS)
        formed_columns = ", ".join(f"s.{v}_ok" for v in STANDARDIZED_VARIANTS)
        allowed = (f"coalesce(expected = '{OWNER_BASIS_LINKED}'"
                   + (f" OR expected = '{OWNER_BASIS_UNLINKED}'" if all_lines else "") + ", false)")
        counts = con.execute(f"""
            WITH s AS (
                SELECT formation_date, {formed} FROM research_feature_dates
                WHERE feature_version=? AND feature_id=? GROUP BY formation_date
            ), x AS (
                SELECT m.*, c.formation_date AS calendar_date, c.cutoff, {formed_columns},
                       {expected_basis} AS expected
                FROM research_feature_matrix m
                LEFT JOIN s ON s.formation_date = m.formation_date
                LEFT JOIN research_panel_calendar c ON c.run_id=? AND c.formation_date=m.formation_date
                                                    AND c.status='{_panel.CALENDAR_FORMED}'
                LEFT JOIN research_panel_cohort k ON k.run_id=? AND k.formation_date=m.formation_date
                                                  AND k.security_id=m.security_id
                WHERE m.feature_version=? AND m.feature_id=?
            )
            SELECT count(*),
                   count(*) - count(DISTINCT (m.formation_date, m.security_id)),
                   count(*) FILTER (WHERE ({valued}) AND NOT {allowed}),
                   count(*) FILTER (WHERE m.owner_basis IS DISTINCT FROM expected OR NOT {allowed}),
                   count(*) FILTER (WHERE calendar_date IS NULL OR m.available_at > cutoff),
                   count(*) FILTER (WHERE m.domain_status <> '{IN_DOMAIN}' AND ({valued})),
                   count(*) FILTER (WHERE {unformed}),
                   count(*) FILTER (WHERE m.expected_sign <> ?)
            FROM x AS m
        """, [feature_version, feature_id, run_id, run_id, feature_version, feature_id, sign]).fetchone()
        rows, duplicates, outside, basis, late, domain, unformed, signs = (int(v) for v in counts)
        if rows != value_rows or duplicates or outside or basis or late or domain or unformed or signs:
            raise FeatureStoreError(
                f"{feature_id}: contract violated (rows={rows}/{value_rows}, duplicates={duplicates}, "
                f"outside_universe={outside}, owner_basis_mismatch={basis}, after_cutoff={late}, "
                f"valued_out_of_domain={domain}, valued_unformed={unformed}, sign_mismatch={signs})")
        date_rows = con.execute("SELECT formation_date, eligible_members, universe_names, reasons_json, variant, "
                                "date_status, unlinked_excluded, sample_conditioning "
                                "FROM research_feature_dates WHERE feature_version=? AND feature_id=?",
                                [feature_version, feature_id]).fetchall()
        if len(date_rows) != len(formations) * len(variants):
            raise FeatureStoreError(f"{feature_id}: {len(date_rows)} date rows, expected formations x variants")
        # Survivor conditioning (R2b N1), re-derived from the matrix: ranked unlinked
        # lines without a value in the variant, and the label on formed variants.
        left_out: dict[tuple[dt.date, str], int] = {}
        if all_lines and status == STATUS_SEALED:
            missing = ", ".join(f"count(*) FILTER (WHERE owner_basis = '{OWNER_BASIS_UNLINKED}' "
                                f"AND rank_normal IS NOT NULL AND {v} IS NULL)" for v in VARIANTS)
            for day, *per_variant in con.execute(f"""
                SELECT formation_date, {missing} FROM research_feature_matrix
                WHERE feature_version=? AND feature_id=? GROUP BY formation_date
            """, [feature_version, feature_id]).fetchall():
                left_out.update({(day, v): int(n) for v, n in zip(VARIANTS, per_variant, strict=True)})
        for day, members, names, reasons_json, variant, date_status, excluded, label in date_rows:
            eligible, linked, unlinked = universe.get(day, (0, 0, 0))
            expected_names = (linked + unlinked if all_lines else linked) if status == STATUS_SEALED else 0
            # Row statuses partition the members; source flags (E1) are counted beside them.
            counted = sum(n for key, n in json.loads(reasons_json).items() if not key.startswith(FLAG_PREFIX))
            if counted != (members if status == STATUS_SEALED else 0) \
                    or members != eligible or names != expected_names:
                raise FeatureStoreError(
                    f"{feature_id} {day}: date row accounting broken (reasons sum "
                    f"{counted}, eligible_members {members}/{eligible}, "
                    f"universe_names {names}/{expected_names})")
            want = left_out.get((day, variant), 0) if all_lines and status == STATUS_SEALED else None
            want_label = CONDITIONING_LINKED_ONLY if want and date_status == DATE_FORMED else None
            if excluded != want or label != want_label:
                raise FeatureStoreError(
                    f"{feature_id} {day} {variant}: survivor-conditioning label broken (unlinked_excluded "
                    f"{excluded}/{want}, sample_conditioning {label!r}/{want_label!r})")
        owner_rows, owner_bad = (int(v) for v in con.execute("""
            WITH m AS (
                SELECT formation_date, owner_basis, count(signed_raw) AS names, count(rank_normal) AS ranked
                FROM research_feature_matrix WHERE feature_version=? AND feature_id=? GROUP BY ALL
            )
            SELECT count(*), count(*) FILTER (WHERE o.names <> coalesce(m.names, 0)
                                              OR o.ranked_names <> coalesce(m.ranked, 0))
            FROM research_feature_owner_basis o
            LEFT JOIN m ON m.formation_date = o.formation_date AND m.owner_basis = o.owner_basis
            WHERE o.feature_version=? AND o.feature_id=?
        """, [feature_version, feature_id, feature_version, feature_id]).fetchone())
        expected_owner_rows = len(formations) * len(OWNER_BASES) if all_lines and status == STATUS_SEALED else 0
        if owner_rows != expected_owner_rows or owner_bad:
            raise FeatureStoreError(f"{feature_id}: owner-basis diagnostic has {owner_rows} rows (expected "
                                    f"{expected_owner_rows}), {owner_bad} disagree with the matrix")
        checks["features"] += 1
        checks["rows"] += rows
    context_rows, context_duplicates, context_outside, context_owner = (int(v) for v in con.execute(f"""
        SELECT count(*), count(*) - count(DISTINCT (x.formation_date, x.security_id)),
               count(*) FILTER (WHERE x.owner_basis IS DISTINCT FROM {expected_basis} OR x.owner_basis IS NULL),
               count(*) FILTER (WHERE x.owner_basis = '{OWNER_BASIS_UNLINKED}'
                                AND (x.owner_cik IS NOT NULL OR x.log_size IS NOT NULL
                                     OR x.industry_group IS NOT NULL))
        FROM research_feature_context x
        LEFT JOIN research_panel_cohort k ON k.run_id=? AND k.formation_date=x.formation_date
                                          AND k.security_id=x.security_id
        WHERE x.feature_version=?
    """, [run_id, feature_version]).fetchone())
    if context_duplicates or context_outside or context_owner:
        raise FeatureStoreError(f"feature context violates the universe grain (duplicates={context_duplicates}, "
                                f"outside_universe={context_outside}, unlinked_with_owner_covariates="
                                f"{context_owner})")
    checks["context_rows"] = context_rows
    # The version-level summary of the (digested, re-derived) date-row labels (R2e m8):
    # blockers_json and the diagnostic are outside every digest, so re-count them here.
    conditioned = int(con.execute("SELECT count(*) FROM research_feature_dates WHERE feature_version=? "
                                  "AND sample_conditioning IS NOT NULL", [feature_version]).fetchone()[0])
    stated = sorted(b for b in json.loads(blockers_json or "[]")
                    if str(b).startswith(f"{NEUTRAL_CONDITIONING_BLOCKER}:"))
    diagnostic = json.loads(diagnostic_json or "{}")
    if stated != ([f"{NEUTRAL_CONDITIONING_BLOCKER}:{conditioned}"] if conditioned else []) \
            or diagnostic.get("survivor_conditioned_date_rows", conditioned) != conditioned:
        raise FeatureStoreError(f"survivor-conditioning summary {stated} / diagnostic "
                                f"{diagnostic.get('survivor_conditioned_date_rows')} disagrees with "
                                f"{conditioned} labeled date rows")
    checks["survivor_conditioned_date_rows"] = conditioned
    combined, _, total = _combined_digest(con, feature_version,
                                          _context_digest(con, feature_version, formations, chunk))
    if combined != values_sha:
        raise FeatureStoreError("feature version digest mismatch")
    return FeatureVersionValidation(feature_version, str(status), str(basis), len(built), total, str(values_sha),
                                    checks)


# ---------------------------------------------------------------------------
# Pruning (explicit command only)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FeaturePruneResult:
    dry_run: bool
    #: Versions deleted (or, in a dry run, that would be deleted) with their status.
    pruned: dict[str, str]
    #: The latest sealed version of each basis (registration order): never pruned.
    protected: dict[str, str]
    #: Rows per table deleted (or that would be).
    rows: dict[str, int]
    #: Versions referenced from outside the feature store, with their referrers: never pruned.
    referenced: dict[str, tuple[str, ...]] = field(default_factory=dict)


def _version_references(store: ResearchStore) -> dict[str, tuple[str, ...]]:
    """Feature versions the research store references outside the feature store tables.

    The references are:
    - an evaluation run's ``spec_json`` ``feature_versions`` (R3b; its result rows,
      e.g. ``research_eval_series``, belong to the run);
    - a qualification ledger's ``manifest_json`` ``evaluation.feature_versions`` (R4);
    - any other base table of the research catalog with a ``feature_version`` column.
    An unreadable reference refuses the prune (fail closed): a referrer table without
    its id/payload columns, a payload that is not JSON, not an object, or lacks the
    list at its path (R2e m6). There is no override: a failed evaluation run still pins
    its versions until its run row is removed.
    """
    con = store.con
    columns: dict[str, set[str]] = {}
    for table, column in con.execute("""
        SELECT c.table_name, c.column_name FROM duckdb_columns() c
        JOIN duckdb_tables() t ON t.database_name = c.database_name AND t.schema_name = c.schema_name
                              AND t.table_name = c.table_name
        WHERE c.database_name = ? AND c.schema_name = 'main' AND NOT t.temporary
    """, [store.catalog]).fetchall():
        columns.setdefault(str(table), set()).add(str(column))
    refs: dict[str, set[str]] = {}

    def add(version: Any, referrer: str) -> None:
        if not isinstance(version, str):
            raise FeatureStoreError(f"{referrer}: unreadable feature version reference {version!r}; not pruning")
        refs.setdefault(version, set()).add(referrer)

    json_refs = (("research_eval_runs", "run_id", "spec_json", ("feature_versions",)),
                 ("research_qualification_ledgers", "ledger_id", "manifest_json", ("evaluation", "feature_versions")))
    for table, key, payload, path in json_refs:
        if table not in columns:
            continue
        if not {key, payload} <= columns[table]:
            raise FeatureStoreError(f"{table} lacks {key}/{payload}: its feature version references cannot be "
                                    "read; not pruning")
        for ident, text in con.execute(f"SELECT {key}, {payload} FROM {table}").fetchall():
            try:
                node: Any = json.loads(text)
            except (TypeError, ValueError) as exc:
                raise FeatureStoreError(f"{table}:{ident}: unreadable {payload}; not pruning") from exc
            for step in path:
                if not isinstance(node, dict) or step not in node:
                    raise FeatureStoreError(f"{table}:{ident}: {payload} has no {'.'.join(path)}; not pruning")
                node = node[step]
            if not isinstance(node, list):
                raise FeatureStoreError(f"{table}:{ident}: {'.'.join(path)} is not a list; not pruning")
            for version in node:
                add(version, f"{table}:{ident}")
    for table, names in sorted(columns.items()):
        if table in _VERSION_TABLES or "feature_version" not in names:
            continue
        for (version,) in con.execute(f"SELECT DISTINCT feature_version FROM {table} "
                                      "WHERE feature_version IS NOT NULL").fetchall():
            add(version, table)
    return {version: tuple(sorted(referrers)) for version, referrers in sorted(refs.items())}


def prune_feature_versions(store: ResearchStore, *, versions: Sequence[str] | None = None,
                           superseded: bool = False, dry_run: bool = True) -> FeaturePruneResult:
    """Delete failed/abandoned feature versions and, on request, superseded sealed ones.

    Nothing is ever pruned automatically; this is the explicit command, and it is a
    dry run unless ``dry_run=False``. Candidates are every version that is not sealed
    (``failed``, or ``building`` left by an interrupted build: rebuilding identical
    inputs would resume it, so prune it only when it is abandoned) and, with
    ``superseded=True``, every sealed version that is not the latest sealed version
    of its basis. "Latest" is registration order (``created_at``, then id), not
    ``finished_at``, which a resumed build moves. It is keyed on the basis only (R2e
    m5): a later sensitivity build (for example winsor 2%) becomes the latest, so an
    *unreferenced* primary build of the same basis is prunable with
    ``superseded=True``; an evaluated one is reference-protected. Never deleted, even
    when named:
    - the latest sealed version of a basis;
    - any version an evaluation run (R3b), a qualification ledger (R4) or another
      research table references (:func:`_version_references`; listed in
      ``referenced``).
    ``versions`` restricts the candidates to the named ones (each must exist and be
    a candidate). Every row of a pruned version goes (matrix, dates, owner-basis
    diagnostic, context, catalog, version row), one transaction per version.
    """
    con = store.con
    ensure_feature_schema(con)
    rows = con.execute("""
        SELECT feature_version, status, basis,
               row_number() OVER (PARTITION BY basis, status IN (?, ?)
                                  ORDER BY created_at DESC, feature_version DESC) AS recency
        FROM research_feature_versions ORDER BY feature_version
    """, list(SEALED_STATUSES)).fetchall()
    referenced = _version_references(store)
    status_of = {str(version): str(state) for version, state, _, _ in rows}
    protected = {str(version): str(basis) for version, state, basis, recency in rows
                 if state in SEALED_STATUSES and recency == 1}
    candidates = {str(version): str(state) for version, state, _, _ in rows
                  if str(version) not in referenced
                  and (state not in SEALED_STATUSES or (superseded and str(version) not in protected))}
    if versions is not None:
        named = list(dict.fromkeys(versions))
        for version in named:
            if version not in status_of:
                raise FeatureStoreError(f"feature version {version!r} is absent")
            if version in protected:
                raise FeatureStoreError(f"feature version {version} is the latest sealed version of basis "
                                        f"{protected[version]!r}; it is never pruned")
            if version in referenced:
                raise FeatureStoreError(f"feature version {version} is referenced by {list(referenced[version])}; "
                                        "it is never pruned")
            if version not in candidates:
                raise FeatureStoreError(f"feature version {version} is {status_of[version]!r}; pass superseded=True "
                                        "to prune a sealed version")
        candidates = {version: candidates[version] for version in named}
    counts: dict[str, int] = {table: 0 for table in _VERSION_TABLES}
    for version in sorted(candidates):
        for table in _VERSION_TABLES:
            counts[table] += int(con.execute(f"SELECT count(*) FROM {table} WHERE feature_version=?",
                                             [version]).fetchone()[0])
        if not dry_run:
            with store.transaction():
                for table in _VERSION_TABLES:
                    con.execute(f"DELETE FROM {table} WHERE feature_version=?", [version])
    if candidates and not dry_run:
        con.execute("CHECKPOINT")
    return FeaturePruneResult(dry_run, dict(sorted(candidates.items())), dict(sorted(protected.items())), counts,
                              {version: refs for version, refs in referenced.items() if version in status_of})


__all__ = [
    "CONDITIONING_LINKED_ONLY",
    "DATE_DEGENERATE",
    "DATE_EMPTY",
    "DATE_FORMED",
    "DATE_THIN",
    "DATE_THIN_COVARIATE",
    "DEFAULT_CONTROLS",
    "DERIVED_NOT_RANKED",
    "DERIVED_ROW_COLUMNS",
    "DERIVED_UNTESTABLE_REASON",
    "DOMAIN_STATUSES",
    "EXTERNAL_FEATURES",
    "EXTERNAL_LINE_FEATURES",
    "EXTERNAL_QUERY_VERSIONS",
    "EXTERNAL_SOURCE_KINDS",
    "FEATURE_BLOCKED",
    "FEATURE_BUILT",
    "FEATURE_EXCLUDED",
    "FEATURE_INPUT_MISSING",
    "FEATURE_OPERAND_MISSING",
    "FEATURE_SCHEMA_VERSION",
    "FEATURE_SOURCE_NOT_PINNED",
    "FLAG_PREFIX",
    "IN_DOMAIN",
    "NEUTRAL_CONDITIONING_BLOCKER",
    "OWNER_BASES",
    "OWNER_BASIS_LINKED",
    "OWNER_BASIS_UNLINKED",
    "QUERY_VERSION",
    "SOURCE_EVENT",
    "SOURCE_FACTOR",
    "SOURCE_OWNERSHIP",
    "STANDARDIZED_VARIANTS",
    "UNIVERSE_RULE",
    "UNIVERSE_SCOPES",
    "UNIVERSE_SCOPE_ALL_LINES",
    "UNIVERSE_SCOPE_LINKED",
    "UNLINKED_COHORT_REASONS",
    "UNLINKED_LINES_BLOCKER",
    "VARIANTS",
    "DerivedFeature",
    "DerivedFeatureRows",
    "DerivedVersionSpec",
    "FeaturePruneResult",
    "FeatureStoreError",
    "FeatureStoreOptions",
    "FeatureVersionResult",
    "FeatureVersionValidation",
    "StandardizationPolicy",
    "build_feature_version",
    "ensure_feature_schema",
    "prune_feature_versions",
    "standardize_frame",
    "validate_feature_version",
    "write_derived_feature_version",
]
