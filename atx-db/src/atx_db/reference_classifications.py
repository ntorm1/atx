"""Reference classification layer — S1.

Implements:
- SicTaxonomyDataset     — SIC divisions + 2-digit major groups (embedded static)
- FamaFrenchTaxonomyDataset — FF12 industries + SIC-range taxonomy_mapping rows
- NaicsTaxonomyDataset   — NAICS 2022 2-digit sectors + partial SIC→NAICS mapping
- EntityClassificationDataset — per-security PIT classifications (SIC primary,
  FAMA_FRENCH_12 and NAICS_2022 derived)

Pure helper exported for tests and downstream use:
- fama_french_12_for_sic(sic: int) -> str

All tests are offline; the SEC fetcher is injectable via Options.

Source: SIC public domain (SEC.gov), Fama-French (Ken French data library, public),
NAICS 2022 (Census Bureau, public domain).
"""
from __future__ import annotations

import datetime as dt
import json
import logging
import time
import uuid
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import pandas as pd
import requests

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import now_utc_naive, record_source_file

logger = logging.getLogger(__name__)

SOURCE_SIC = "SEC SIC list (public domain)"
SOURCE_FF = "Ken French Data Library (public)"
SOURCE_NAICS = "Census Bureau NAICS 2022 (public domain)"
SOURCE_ENTITY = "SEC submissions JSON"
SEC_BULK_SUBMISSIONS_URL = (
    "https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip"
)

# ---------------------------------------------------------------------------
# SIC: 10 divisions (A-J) + 83 two-digit major groups
# ---------------------------------------------------------------------------

# Division code → (sort_order, label)
SIC_DIVISIONS: list[tuple[str, int, str]] = [
    ("A", 1, "Agriculture, Forestry, and Fishing"),
    ("B", 2, "Mining"),
    ("C", 3, "Construction"),
    ("D", 4, "Manufacturing"),
    ("E", 5, "Transportation, Communications, Electric, Gas, and Sanitary Services"),
    ("F", 6, "Wholesale Trade"),
    ("G", 7, "Retail Trade"),
    ("H", 8, "Finance, Insurance, and Real Estate"),
    ("I", 9, "Services"),
    ("J", 10, "Public Administration"),
]

# Maps division letter to the range of 2-digit SIC major-group codes it covers
_DIVISION_RANGES: list[tuple[str, int, int]] = [
    ("A",  1,  9),
    ("B", 10, 14),
    ("C", 15, 17),
    ("D", 20, 39),
    ("E", 40, 49),
    ("F", 50, 51),
    ("G", 52, 59),
    ("H", 60, 67),
    ("I", 70, 89),
    ("J", 91, 99),
]


def _division_for_major_group(mg: int) -> str | None:
    for div, lo, hi in _DIVISION_RANGES:
        if lo <= mg <= hi:
            return div
    return None


# 2-digit SIC major groups — code (str, zero-padded to 2) → label
SIC_MAJOR_GROUPS: list[tuple[str, str]] = [
    ("01", "Crops"),
    ("02", "Livestock and Animal Specialties"),
    ("07", "Agricultural Services"),
    ("08", "Forestry"),
    ("09", "Fishing, Hunting, and Trapping"),
    ("10", "Metal Mining"),
    ("12", "Bituminous Coal and Lignite Mining"),
    ("13", "Oil and Gas Extraction"),
    ("14", "Mining and Quarrying of Nonmetallic Minerals, Except Fuels"),
    ("15", "Building Construction — General Contractors and Operative Builders"),
    ("16", "Heavy Construction, Except Building Construction — Contractors"),
    ("17", "Special Trade Contractors"),
    ("20", "Food and Kindred Products"),
    ("21", "Tobacco Products"),
    ("22", "Textile Mill Products"),
    ("23", "Apparel and Other Finished Products Made from Fabrics and Similar Materials"),
    ("24", "Lumber and Wood Products, Except Furniture"),
    ("25", "Furniture and Fixtures"),
    ("26", "Paper and Allied Products"),
    ("27", "Printing, Publishing, and Allied Industries"),
    ("28", "Chemicals and Allied Products"),
    ("29", "Petroleum Refining and Related Industries"),
    ("30", "Rubber and Miscellaneous Plastics Products"),
    ("31", "Leather and Leather Products"),
    ("32", "Stone, Clay, Glass, and Concrete Products"),
    ("33", "Primary Metal Industries"),
    ("34", "Fabricated Metal Products, Except Machinery and Transportation Equipment"),
    ("35", "Industrial and Commercial Machinery and Computer Equipment"),
    ("36", "Electronic and Other Electrical Equipment and Components, Except Computer Equipment"),
    ("37", "Transportation Equipment"),
    ("38", "Measuring, Analyzing, and Controlling Instruments"),
    ("39", "Miscellaneous Manufacturing Industries"),
    ("40", "Railroad Transportation"),
    ("41", "Local and Suburban Transit and Interurban Highway Passenger Transportation"),
    ("42", "Motor Freight Transportation and Warehousing"),
    ("43", "United States Postal Service"),
    ("44", "Water Transportation"),
    ("45", "Transportation by Air"),
    ("46", "Pipelines, Except Natural Gas"),
    ("47", "Transportation Services"),
    ("48", "Communications"),
    ("49", "Electric, Gas, and Sanitary Services"),
    ("50", "Durable Goods — Wholesale"),
    ("51", "Nondurable Goods — Wholesale"),
    ("52", "Building Materials, Hardware, Garden Supply, and Mobile Home Dealers"),
    ("53", "General Merchandise Stores"),
    ("54", "Food Stores"),
    ("55", "Automotive Dealers and Gasoline Service Stations"),
    ("56", "Apparel and Accessory Stores"),
    ("57", "Home Furniture, Furnishings, and Equipment Stores"),
    ("58", "Eating and Drinking Places"),
    ("59", "Miscellaneous Retail"),
    ("60", "Depository Institutions"),
    ("61", "Nondepository Credit Institutions"),
    ("62", "Security and Commodity Brokers, Dealers, Exchanges, and Services"),
    ("63", "Insurance Carriers"),
    ("64", "Insurance Agents, Brokers, and Service"),
    ("65", "Real Estate"),
    ("67", "Holding and Other Investment Offices"),
    ("70", "Hotels, Rooming Houses, Camps, and Other Lodging Places"),
    ("72", "Personal Services"),
    ("73", "Business Services"),
    ("75", "Automotive Repair, Services, and Parking"),
    ("76", "Miscellaneous Repair Services"),
    ("78", "Motion Picture"),
    ("79", "Amusement and Recreation Services"),
    ("80", "Health Services"),
    ("81", "Legal Services"),
    ("82", "Educational Services"),
    ("83", "Social Services"),
    ("84", "Museums, Art Galleries, and Botanical and Zoological Gardens"),
    ("86", "Membership Organizations"),
    ("87", "Engineering, Accounting, Research, Management, and Related Services"),
    ("88", "Private Households"),
    ("89", "Services, Not Elsewhere Classified"),
    ("91", "Executive, Legislative, and General Government, Except Finance"),
    ("92", "Justice, Public Order, and Safety"),
    ("93", "Finance, Taxation, and Monetary Policy"),
    ("94", "Administration of Human Resource Programs"),
    ("95", "Administration of Environmental Quality and Housing Programs"),
    ("96", "Administration of Economic Programs"),
    ("97", "National Security and International Affairs"),
    ("99", "Nonclassifiable Establishments"),
]

# ---------------------------------------------------------------------------
# Fama-French 12 industries + SIC range table
# ---------------------------------------------------------------------------

# The canonical Ken French FF12 industries. Exactly 12 codes; #5 is "BusEq"
# (Business Equipment). "HiTec" is an FF5/FF48 label, NOT an FF12 industry, so it
# is deliberately absent.
FF12_INDUSTRIES: list[tuple[str, str]] = [
    ("NoDur",  "Consumer NonDurables — Food, Tobacco, Textiles, Apparel, Leather, Toys"),
    ("Durbl",  "Consumer Durables — Cars, TVs, Furniture, Household Appliances"),
    ("Manuf",  "Manufacturing — Machinery, Trucks, Planes, Off Furn, Paper, Com Printing"),
    ("Enrgy",  "Oil, Gas, and Coal Extraction and Products"),
    ("BusEq",  "Business Equipment — Computers, Software, and Electronic Equipment"),
    ("Telcm",  "Telephone and Television Transmission"),
    ("Shops",  "Wholesale, Retail, and Some Services (Laundries, Repair Shops)"),
    ("Hlth",   "Healthcare, Medical Equipment, and Drugs"),
    ("Money",  "Finance"),
    ("Utils",  "Utilities"),
    ("Chems",  "Chemicals and Allied Products"),
    ("Other",  "Other — Mines, Constr, BldMt, Trans, Hotels, Bus Serv, Entertainment"),
]

# Canonical Ken French FF12 SIC ranges.
# Each entry: (ff12_code, [(lo, hi), ...])  inclusive 4-digit SIC ranges.
# Source: https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_12_ind_port.html
FF12_SIC_RANGES: list[tuple[str, list[tuple[int, int]]]] = [
    ("NoDur", [
        (100, 199), (200, 299), (700, 799), (900, 999),
        (2000, 2099), (2100, 2199), (2200, 2299), (2300, 2399),
        (2700, 2749), (2750, 2799), (2800, 2829), (2840, 2844),
        (3100, 3199), (3940, 3989), (2080, 2085),
        (2086, 2099), (2090, 2099),
        (5140, 5149), (5150, 5159), (5180, 5182), (5190, 5199),
        (5200, 5299), (5600, 5699), (5900, 5999), (3630, 3639),
        (3640, 3649), (3650, 3651), (3652, 3652),
        (3860, 3861), (3870, 3879), (3990, 3999),
        (2047, 2047), (2048, 2048),
    ]),
    ("Durbl", [
        (2510, 2519), (2590, 2599),
        (3630, 3639), (3640, 3649), (3650, 3651), (3652, 3652),
        (3710, 3711), (3714, 3716), (3750, 3751), (3792, 3792),
        (3900, 3939), (3990, 3999),
        (2590, 2599), (3585, 3585), (3589, 3589), (3600, 3629),
        (3670, 3679), (3690, 3699),
        (5000, 5099), (5700, 5736),
    ]),
    ("Manuf", [
        (2520, 2589), (2600, 2699), (2750, 2769), (2770, 2799),
        (3000, 3099), (3200, 3569), (3580, 3584), (3586, 3588),
        (3590, 3599), (3700, 3709), (3712, 3713), (3720, 3749),
        (3752, 3791), (3793, 3799), (3830, 3839), (3860, 3869),
        (3870, 3899), (3900, 3989),
    ]),
    ("Enrgy", [
        (1300, 1399), (2900, 2999),
        (1310, 1389), (2910, 2911), (2990, 2999),
        (5170, 5172),
    ]),
    # Canonical FF12 Chemicals. The lookup is "first range wins"; NoDur (above)
    # already claims 2800-2829 and 2840-2844, so Chems owns the remaining
    # chemical codes (paints, industrial/agricultural chemicals, etc.). These do
    # not collide with the Hlth drug codes (2830-2836) or NoDur's 2840-2844.
    ("Chems", [
        (2850, 2879), (2890, 2899),
    ]),
    ("Hlth", [
        (2830, 2836), (3693, 3693), (3840, 3849), (3850, 3851),
        (5047, 5047), (5122, 5122), (8000, 8099),
        (2833, 2836), (3841, 3851),
    ]),
    ("BusEq", [
        (3570, 3579), (3660, 3669), (3672, 3679), (3812, 3812),
        (3820, 3829), (3840, 3842), (7370, 7379),
        (3571, 3577), (3661, 3661), (3663, 3665), (3669, 3669),
        (3674, 3674), (3812, 3812), (3821, 3827),
        (3829, 3829), (7372, 7372), (7371, 7379),
        (3576, 3576), (3578, 3578),
    ]),
    ("Telcm", [
        (4800, 4899), (4812, 4813), (4899, 4899),
    ]),
    ("Shops", [
        (5000, 5199), (5200, 5999), (7200, 7299), (7600, 7699),
        (5210, 5211), (5251, 5261), (5270, 5271), (5300, 5399),
        (5400, 5411), (5412, 5412), (5500, 5599), (5600, 5699),
        (5700, 5736), (5900, 5940), (5945, 5945), (5960, 5963),
        (5990, 5995), (5999, 5999),
        (7000, 7019), (7040, 7049), (7212, 7219), (7215, 7217),
        (7219, 7219), (7220, 7221), (7230, 7231), (7240, 7241),
        (7250, 7251), (7260, 7269), (7290, 7299),
    ]),
    ("Money", [
        (6000, 6199), (6200, 6299), (6300, 6399), (6400, 6499),
        (6500, 6599), (6700, 6799),
        (6020, 6022), (6025, 6026), (6035, 6036), (6099, 6099),
        (6110, 6111), (6141, 6141), (6153, 6159), (6160, 6163),
        (6020, 6099), (6110, 6199), (6200, 6289), (6311, 6321),
        (6324, 6331), (6351, 6361), (6411, 6411), (6500, 6553),
        (6700, 6726), (6792, 6792), (6794, 6798), (6726, 6726),
    ]),
    ("Utils", [
        (4900, 4949), (4911, 4911), (4931, 4941), (4950, 4959),
        (4961, 4971), (4991, 4991),
    ]),
    ("Other", [
        (100, 999), (1500, 1799), (2000, 3999), (4000, 4799),
        (5000, 5199), (5200, 5999), (6000, 6999), (7000, 8999),
        (9000, 9999),
    ]),
]

# Build a fast lookup dict: sic_4digit -> ff12_code
# We process in order so earlier (more specific) ranges win.
# The "Other" range at the end catches anything not yet matched.
_FF12_LOOKUP: dict[int, str] = {}


def _build_ff12_lookup() -> dict[int, str]:
    lookup: dict[int, str] = {}
    # Process all industries except Other first, in the order listed
    for code, ranges in FF12_SIC_RANGES[:-1]:  # skip Other
        for lo, hi in ranges:
            for sic in range(lo, hi + 1):
                if sic not in lookup:
                    lookup[sic] = code
    # Fill remaining with Other
    for sic in range(100, 10000):
        if sic not in lookup:
            lookup[sic] = "Other"
    return lookup


_FF12_LOOKUP = _build_ff12_lookup()


def fama_french_12_for_sic(sic: int) -> str:
    """Return the Fama-French 12-industry code for a 4-digit SIC code.

    Uses the canonical Ken French SIC-range table embedded in this module.
    Returns 'Other' for any SIC not matched by a named industry range.
    """
    return _FF12_LOOKUP.get(sic, "Other")


# ---------------------------------------------------------------------------
# NAICS 2022 — 20 two-digit sectors
# ---------------------------------------------------------------------------

# The 20 canonical NAICS 2022 two-digit sectors. Three sectors span a 2-digit
# range and are single sectors with hyphenated codes: 31-33 (Manufacturing),
# 44-45 (Retail Trade), 48-49 (Transportation and Warehousing). Every code
# referenced by SIC_TO_NAICS_PARTIAL below must exist here as a node.
NAICS_2022_SECTORS: list[tuple[str, str]] = [
    ("11", "Agriculture, Forestry, Fishing and Hunting"),
    ("21", "Mining, Quarrying, and Oil and Gas Extraction"),
    ("22", "Utilities"),
    ("23", "Construction"),
    ("31-33", "Manufacturing"),
    ("42", "Wholesale Trade"),
    ("44-45", "Retail Trade"),
    ("48-49", "Transportation and Warehousing"),
    ("51", "Information"),
    ("52", "Finance and Insurance"),
    ("53", "Real Estate and Rental and Leasing"),
    ("54", "Professional, Scientific, and Technical Services"),
    ("55", "Management of Companies and Enterprises"),
    ("56", "Administrative and Support and Waste Management and Remediation Services"),
    ("61", "Educational Services"),
    ("62", "Health Care and Social Assistance"),
    ("71", "Arts, Entertainment, and Recreation"),
    ("72", "Accommodation and Food Services"),
    ("81", "Other Services (except Public Administration)"),
    ("92", "Public Administration"),
]

# Partial SIC→NAICS mapping (documented subset; marked approximate).
# Format: (sic_2digit_prefix, naics_2digit_code)
# This is a well-known partial crosswalk (not the full Census bridge table).
# The full 6-digit NAICS + complete Census crosswalk is a future extension.
SIC_TO_NAICS_PARTIAL: list[tuple[str, str]] = [
    ("01", "11"),  # Crops -> Agriculture
    ("02", "11"),  # Livestock -> Agriculture
    ("07", "11"),  # Agricultural Services -> Agriculture
    ("08", "11"),  # Forestry -> Agriculture
    ("09", "11"),  # Fishing -> Agriculture
    ("10", "21"),  # Metal Mining -> Mining
    ("12", "21"),  # Coal Mining -> Mining
    ("13", "21"),  # Oil and Gas -> Mining
    ("14", "21"),  # Nonmetallic Minerals -> Mining
    ("15", "23"),  # Building Construction -> Construction
    ("16", "23"),  # Heavy Construction -> Construction
    ("17", "23"),  # Special Trade -> Construction
    ("20", "31-33"),  # Food -> Manufacturing
    ("21", "31-33"),  # Tobacco -> Manufacturing
    ("22", "31-33"),  # Textile Mill -> Manufacturing
    ("23", "31-33"),  # Apparel -> Manufacturing
    ("24", "31-33"),  # Lumber -> Manufacturing
    ("25", "31-33"),  # Furniture -> Manufacturing
    ("26", "31-33"),  # Paper -> Manufacturing
    ("27", "31-33"),  # Printing -> Manufacturing
    ("28", "31-33"),  # Chemicals -> Manufacturing
    ("29", "31-33"),  # Petroleum Refining -> Manufacturing
    ("30", "31-33"),  # Rubber -> Manufacturing
    ("31", "31-33"),  # Leather -> Manufacturing
    ("32", "31-33"),  # Stone/Clay/Glass -> Manufacturing
    ("33", "31-33"),  # Primary Metal -> Manufacturing
    ("34", "31-33"),  # Fabricated Metal -> Manufacturing
    ("35", "31-33"),  # Machinery -> Manufacturing
    ("36", "31-33"),  # Electronic Equipment -> Manufacturing
    ("37", "31-33"),  # Transportation Equipment -> Manufacturing
    ("38", "31-33"),  # Instruments -> Manufacturing
    ("39", "31-33"),  # Misc Manufacturing -> Manufacturing
    ("40", "48-49"),  # Railroad -> Transportation
    ("41", "48-49"),  # Transit -> Transportation
    ("42", "48-49"),  # Motor Freight -> Transportation
    ("44", "48-49"),  # Water Transport -> Transportation
    ("45", "48-49"),  # Air Transport -> Transportation
    ("47", "48-49"),  # Transport Services -> Transportation
    ("48", "51"),     # Communications -> Information
    ("49", "22"),     # Electric/Gas/Sanitary -> Utilities
    ("50", "42"),     # Durable Wholesale -> Wholesale
    ("51", "42"),     # Nondurable Wholesale -> Wholesale
    ("52", "44-45"),  # Building Materials Retail -> Retail
    ("53", "44-45"),  # General Merchandise -> Retail
    ("54", "44-45"),  # Food Stores -> Retail
    ("55", "44-45"),  # Auto Dealers -> Retail
    ("56", "44-45"),  # Apparel Stores -> Retail
    ("57", "44-45"),  # Home Furniture Stores -> Retail
    ("58", "72"),     # Eating/Drinking -> Accommodation
    ("59", "44-45"),  # Misc Retail -> Retail
    ("60", "52"),  # Depository Institutions -> Finance
    ("61", "52"),  # Nondepository Credit -> Finance
    ("62", "52"),  # Securities -> Finance
    ("63", "52"),  # Insurance Carriers -> Finance
    ("64", "52"),  # Insurance Agents -> Finance
    ("65", "53"),  # Real Estate -> Real Estate
    ("67", "55"),  # Holding Companies -> Management of Companies
    ("70", "72"),  # Hotels -> Accommodation
    ("72", "81"),  # Personal Services -> Other Services
    ("73", "54"),  # Business Services -> Professional Services
    ("75", "81"),  # Auto Repair -> Other Services
    ("76", "81"),  # Misc Repair -> Other Services
    ("78", "71"),  # Motion Picture -> Arts
    ("79", "71"),  # Amusement -> Arts
    ("80", "62"),  # Health Services -> Health Care
    ("81", "54"),  # Legal Services -> Professional Services
    ("82", "61"),  # Educational Services -> Education
    ("83", "62"),  # Social Services -> Health Care
    ("86", "81"),  # Membership Organizations -> Other Services
    ("87", "54"),  # Engineering/Research -> Professional Services
    ("91", "92"),  # Executive/Legislative -> Public Administration
    ("92", "92"),  # Justice/Public Order -> Public Administration
    ("93", "92"),  # Finance/Taxation -> Public Administration
    ("94", "92"),  # Human Resources Admin -> Public Administration
    ("95", "92"),  # Environmental Admin -> Public Administration
    ("96", "92"),  # Economic Programs -> Public Administration
    ("97", "92"),  # National Security -> Public Administration
    ("99", "92"),  # Nonclassifiable -> Public Administration
]


# ---------------------------------------------------------------------------
# Helpers for DB operations
# ---------------------------------------------------------------------------

def _taxonomy_id_for(store: DuckDBStore, code: str) -> str | None:
    row = store.con.execute(
        "SELECT taxonomy_id FROM taxonomy WHERE code = ?", [code]
    ).fetchone()
    return row[0] if row else None


def _node_id_for(store: DuckDBStore, taxonomy_id: str, node_code: str) -> str | None:
    row = store.con.execute(
        "SELECT node_id FROM taxonomy_node WHERE taxonomy_id = ? AND node_code = ?",
        [taxonomy_id, node_code],
    ).fetchone()
    return row[0] if row else None


def _upsert_taxonomy(
    store: DuckDBStore,
    *,
    code: str,
    name: str,
    provider: str,
    version: str,
    is_hierarchical: bool,
    description: str,
    source: str,
) -> str:
    """Insert or ignore taxonomy; return the taxonomy_id."""
    existing = _taxonomy_id_for(store, code)
    if existing:
        return existing
    taxonomy_id = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"taxonomy:{code}"))
    store.con.execute(
        """
        INSERT OR IGNORE INTO taxonomy
            (taxonomy_id, code, name, provider, version, is_hierarchical,
             description, source, source_loaded_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, now())
        """,
        [taxonomy_id, code, name, provider, version, is_hierarchical,
         description, source],
    )
    return taxonomy_id


def _upsert_taxonomy_node(
    store: DuckDBStore,
    *,
    taxonomy_id: str,
    node_code: str,
    node_label: str,
    parent_node_id: str | None,
    level: int,
    sort_order: int,
) -> str:
    """Insert or ignore taxonomy_node; return the node_id."""
    existing = _node_id_for(store, taxonomy_id, node_code)
    if existing:
        return existing
    node_id = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{taxonomy_id}:{node_code}"))
    store.con.execute(
        """
        INSERT OR IGNORE INTO taxonomy_node
            (node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        [node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order],
    )
    return node_id


# ---------------------------------------------------------------------------
# Dataset 1: SicTaxonomyDataset
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class SicTaxonomyOptions:
    run_id: str | None = None


class SicTaxonomyDataset(Dataset):
    """Seed the SIC taxonomy: 10 divisions (level 1) + 83 two-digit major groups (level 2).

    4-digit leaf nodes (level 3) are created on demand by EntityClassificationDataset
    when a security's specific SIC code is first seen.  Idempotent — safe to run multiple times.
    """

    dataset_id = "sic_taxonomy"
    source_name = SOURCE_SIC

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SicTaxonomyOptions) -> DatasetLoadResult:
        taxonomy_id = _upsert_taxonomy(
            store,
            code="SIC",
            name="Standard Industrial Classification",
            provider="SEC / US Government",
            version="current",
            is_hierarchical=True,
            description=(
                "US Standard Industrial Classification system. "
                "10 divisions (A-J), ~83 two-digit major groups, "
                "and 4-digit industry codes created on demand."
            ),
            source=SOURCE_SIC,
        )

        # --- Bulk insert: Division nodes (level 1, no parent) ---
        div_rows = []
        div_node_ids: dict[str, str] = {}
        for code, sort_order, label in SIC_DIVISIONS:
            nid = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{taxonomy_id}:{code}"))
            div_node_ids[code] = nid
            div_rows.append((nid, taxonomy_id, code, label, None, 1, sort_order))

        # --- Bulk insert: Major-group nodes (level 2) ---
        mg_rows = []
        for sort, (mg_code, mg_label) in enumerate(SIC_MAJOR_GROUPS):
            mg_int = int(mg_code)
            div_letter = _division_for_major_group(mg_int)
            parent_id = div_node_ids.get(div_letter) if div_letter else None
            nid = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{taxonomy_id}:{mg_code}"))
            mg_rows.append((nid, taxonomy_id, mg_code, mg_label, parent_id, 2, sort))

        all_node_rows = div_rows + mg_rows
        node_df = pd.DataFrame(
            all_node_rows,
            columns=["node_id", "taxonomy_id", "node_code", "node_label",
                     "parent_node_id", "level", "sort_order"],
        )
        store.con.register("_sic_node_seed", node_df)
        try:
            store.con.execute(
                """
                INSERT OR REPLACE INTO taxonomy_node
                    (node_id, taxonomy_id, node_code, node_label,
                     parent_node_id, level, sort_order)
                SELECT node_id, taxonomy_id, node_code, node_label,
                       parent_node_id, level, sort_order
                FROM _sic_node_seed
                """
            )
        finally:
            store.con.unregister("_sic_node_seed")

        rows_loaded = len(all_node_rows)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=SOURCE_SIC,
            details={"divisions": len(SIC_DIVISIONS), "major_groups": len(mg_rows)},
        )


def ensure_sic_leaf_node(store: DuckDBStore, sic4: int) -> tuple[str, str]:
    """Ensure a level-3 leaf node exists for `sic4` under its major-group parent.

    Returns (taxonomy_id, node_id) for the leaf.
    Called by EntityClassificationDataset at classification time.
    """
    taxonomy_id = _taxonomy_id_for(store, "SIC")
    if taxonomy_id is None:
        raise RuntimeError("SIC taxonomy not seeded — run SicTaxonomyDataset first")

    sic_str = str(sic4).zfill(4)
    existing = _node_id_for(store, taxonomy_id, sic_str)
    if existing:
        return taxonomy_id, existing

    # Find major-group parent (first 2 digits, zero-padded)
    mg_code = str(sic4 // 100).zfill(2)
    parent_id = _node_id_for(store, taxonomy_id, mg_code)

    node_id = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{taxonomy_id}:{sic_str}"))
    store.con.execute(
        """
        INSERT OR IGNORE INTO taxonomy_node
            (node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order)
        VALUES (?, ?, ?, ?, ?, 3, 0)
        """,
        [node_id, taxonomy_id, sic_str, f"SIC {sic_str}", parent_id],
    )
    return taxonomy_id, node_id


# ---------------------------------------------------------------------------
# Dataset 2: FamaFrenchTaxonomyDataset
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FamaFrenchTaxonomyOptions:
    run_id: str | None = None


class FamaFrenchTaxonomyDataset(Dataset):
    """Seed FAMA_FRENCH_12 taxonomy nodes and SIC-range taxonomy_mapping rows.

    Uses the canonical Ken French SIC-range definitions (public).
    Idempotent — safe to run multiple times.
    """

    dataset_id = "fama_french_taxonomy"
    source_name = SOURCE_FF

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FamaFrenchTaxonomyOptions) -> DatasetLoadResult:
        taxonomy_id = _upsert_taxonomy(
            store,
            code="FAMA_FRENCH_12",
            name="Fama-French 12 Industries",
            provider="Ken French Data Library",
            version="canonical",
            is_hierarchical=False,
            description=(
                "12-industry Fama-French classification mapped from 4-digit SIC ranges. "
                "Source: https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/"
            ),
            source=SOURCE_FF,
        )

        # --- Bulk insert: 12 industry nodes (de-duplicate BusEq) ---
        seen_codes: set[str] = set()
        node_rows = []
        node_ids: dict[str, str] = {}
        for sort, (code, label) in enumerate(FF12_INDUSTRIES):
            nid = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{taxonomy_id}:{code}"))
            node_ids[code] = nid
            if code not in seen_codes:
                seen_codes.add(code)
                node_rows.append((nid, taxonomy_id, code, label, None, 1, sort))

        node_df = pd.DataFrame(
            node_rows,
            columns=["node_id", "taxonomy_id", "node_code", "node_label",
                     "parent_node_id", "level", "sort_order"],
        )
        store.con.register("_ff12_node_seed", node_df)
        try:
            store.con.execute(
                """
                INSERT OR REPLACE INTO taxonomy_node
                    (node_id, taxonomy_id, node_code, node_label,
                     parent_node_id, level, sort_order)
                SELECT node_id, taxonomy_id, node_code, node_label,
                       parent_node_id, level, sort_order
                FROM _ff12_node_seed
                """
            )
        finally:
            store.con.unregister("_ff12_node_seed")

        node_count = len(node_rows)

        # --- Bulk insert: taxonomy_mapping rows (SIC ranges → FF12) ---
        sic_taxonomy_id = _taxonomy_id_for(store, "SIC")
        mapping_count = 0
        if sic_taxonomy_id:
            mapping_rows = []
            for ff_code, ranges in FF12_SIC_RANGES:
                if ff_code not in node_ids:
                    continue
                for lo, hi in ranges:
                    mapping_id = str(uuid.uuid5(
                        uuid.NAMESPACE_DNS,
                        f"mapping:SIC:{lo}-{hi}:FF12:{ff_code}",
                    ))
                    mapping_rows.append((
                        mapping_id,
                        sic_taxonomy_id,
                        f"{lo}-{hi}",
                        taxonomy_id,
                        ff_code,
                        "many_to_one",
                        1.0,
                        SOURCE_FF,
                    ))
            if mapping_rows:
                map_df = pd.DataFrame(
                    mapping_rows,
                    columns=["mapping_id", "from_taxonomy_id", "from_node_code",
                             "to_taxonomy_id", "to_node_code", "relationship",
                             "confidence", "source"],
                )
                store.con.register("_ff12_mapping_seed", map_df)
                try:
                    store.con.execute(
                        """
                        INSERT OR REPLACE INTO taxonomy_mapping
                            (mapping_id, from_taxonomy_id, from_node_code,
                             to_taxonomy_id, to_node_code, relationship,
                             confidence, source)
                        SELECT mapping_id, from_taxonomy_id, from_node_code,
                               to_taxonomy_id, to_node_code, relationship,
                               confidence, source
                        FROM _ff12_mapping_seed
                        """
                    )
                finally:
                    store.con.unregister("_ff12_mapping_seed")
                mapping_count = len(mapping_rows)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=node_count + mapping_count,
            source=SOURCE_FF,
            details={"ff12_nodes": node_count, "mapping_ranges": mapping_count},
        )


# ---------------------------------------------------------------------------
# Dataset 3: NaicsTaxonomyDataset
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class NaicsTaxonomyOptions:
    run_id: str | None = None


class NaicsTaxonomyDataset(Dataset):
    """Seed NAICS_2022 taxonomy: 20 2-digit sector nodes.

    Also writes a partial SIC→NAICS taxonomy_mapping (documented subset;
    relationship='approximate'). The full 6-digit NAICS and complete Census
    crosswalk is a future extension.  Idempotent.
    """

    dataset_id = "naics_taxonomy"
    source_name = SOURCE_NAICS

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: NaicsTaxonomyOptions) -> DatasetLoadResult:
        taxonomy_id = _upsert_taxonomy(
            store,
            code="NAICS_2022",
            name="North American Industry Classification System 2022",
            provider="US Census Bureau",
            version="2022",
            is_hierarchical=True,
            description=(
                "NAICS 2022 2-digit sector codes (20 sectors). "
                "Full 6-digit hierarchy and complete Census SIC crosswalk are future extensions."
            ),
            source=SOURCE_NAICS,
        )

        # --- Bulk insert: NAICS sector nodes ---
        node_rows = []
        node_ids: dict[str, str] = {}
        for sort, (sector_code, sector_label) in enumerate(NAICS_2022_SECTORS):
            nid = str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{taxonomy_id}:{sector_code}"))
            node_ids[sector_code] = nid
            node_rows.append((nid, taxonomy_id, sector_code, sector_label, None, 1, sort))

        node_df = pd.DataFrame(
            node_rows,
            columns=["node_id", "taxonomy_id", "node_code", "node_label",
                     "parent_node_id", "level", "sort_order"],
        )
        store.con.register("_naics_node_seed", node_df)
        try:
            store.con.execute(
                """
                INSERT OR REPLACE INTO taxonomy_node
                    (node_id, taxonomy_id, node_code, node_label,
                     parent_node_id, level, sort_order)
                SELECT node_id, taxonomy_id, node_code, node_label,
                       parent_node_id, level, sort_order
                FROM _naics_node_seed
                """
            )
        finally:
            store.con.unregister("_naics_node_seed")

        node_count = len(node_rows)

        # --- Bulk insert: partial SIC→NAICS mapping ---
        sic_taxonomy_id = _taxonomy_id_for(store, "SIC")
        mapping_count = 0
        if sic_taxonomy_id:
            mapping_rows = []
            for sic_2digit, naics_2digit in SIC_TO_NAICS_PARTIAL:
                if naics_2digit not in node_ids:
                    continue
                mapping_id = str(uuid.uuid5(
                    uuid.NAMESPACE_DNS,
                    f"mapping:SIC:{sic_2digit}:NAICS:{naics_2digit}",
                ))
                mapping_rows.append((
                    mapping_id,
                    sic_taxonomy_id,
                    sic_2digit,
                    taxonomy_id,
                    naics_2digit,
                    "approximate",
                    0.9,
                    SOURCE_NAICS,
                ))
            if mapping_rows:
                map_df = pd.DataFrame(
                    mapping_rows,
                    columns=["mapping_id", "from_taxonomy_id", "from_node_code",
                             "to_taxonomy_id", "to_node_code", "relationship",
                             "confidence", "source"],
                )
                store.con.register("_naics_mapping_seed", map_df)
                try:
                    store.con.execute(
                        """
                        INSERT OR REPLACE INTO taxonomy_mapping
                            (mapping_id, from_taxonomy_id, from_node_code,
                             to_taxonomy_id, to_node_code, relationship,
                             confidence, source)
                        SELECT mapping_id, from_taxonomy_id, from_node_code,
                               to_taxonomy_id, to_node_code, relationship,
                               confidence, source
                        FROM _naics_mapping_seed
                        """
                    )
                finally:
                    store.con.unregister("_naics_mapping_seed")
                mapping_count = len(mapping_rows)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=node_count + mapping_count,
            source=SOURCE_NAICS,
            details={"naics_sectors": node_count, "sic_naics_mappings": mapping_count},
        )


# ---------------------------------------------------------------------------
# SEC submission fetcher
# ---------------------------------------------------------------------------

_DEFAULT_USER_AGENT = "atx-db reference-classifications loader nathan.tormaschy@gmail.com"
_SEC_SUBMISSION_URL = "https://data.sec.gov/submissions/CIK{cik:010d}.json"
_MAX_REQUESTS_PER_SEC = 5


def _make_real_fetcher(
    user_agent: str = _DEFAULT_USER_AGENT,
    request_timeout: int = 30,
) -> Callable[[str | int], dict | None]:
    """Return a real SEC-fetching callable (rate-limited ≤5 req/s)."""
    session = requests.Session()
    session.headers.update({"User-Agent": user_agent, "Accept": "application/json"})
    min_interval = 1.0 / _MAX_REQUESTS_PER_SEC
    last_call: list[float] = [0.0]

    def fetch(cik: str | int) -> dict | None:
        elapsed = time.monotonic() - last_call[0]
        if elapsed < min_interval:
            time.sleep(min_interval - elapsed)
        last_call[0] = time.monotonic()
        url = _SEC_SUBMISSION_URL.format(cik=int(cik))
        try:
            resp = session.get(url, timeout=request_timeout)
            resp.raise_for_status()
            return resp.json()
        except Exception as exc:
            logger.warning("fetch_submission(%s) failed: %s", cik, exc)
            return None

    return fetch


def _make_csv_fetcher(path: str | Path) -> Callable[[str | int], dict | None]:
    """Return an OFFLINE fetcher backed by a local CIK->SIC CSV.

    The CSV needs a ``cik`` column and one of ``sic`` / ``sic_code`` /
    ``assigned_sic``; an optional ``sic_description`` is passed through. CIKs are
    normalized by integer value so zero-padded and unpadded forms both resolve.
    This lets reference classification be populated without any SEC network call
    (e.g. from a vendor or SEC bulk-submissions extract), mirroring the warehouse's
    injectable-source convention.
    """
    frame = pd.read_csv(path, dtype=str, keep_default_na=False)
    lower = {str(c).strip().lower(): c for c in frame.columns}
    cik_col = lower.get("cik")
    sic_col = lower.get("sic") or lower.get("sic_code") or lower.get("assigned_sic")
    desc_col = lower.get("sic_description") or lower.get("sicdescription")
    if cik_col is None or sic_col is None:
        raise ValueError("SIC CSV requires a 'cik' column and a 'sic'/'sic_code'/'assigned_sic' column")

    lookup: dict[str, dict] = {}
    for _, row in frame.iterrows():
        cik_raw = str(row[cik_col]).strip()
        sic_raw = str(row[sic_col]).strip()
        if not cik_raw or not sic_raw:
            continue
        try:
            key = str(int(cik_raw))
        except ValueError:
            key = cik_raw
        payload = {"sic": sic_raw}
        if desc_col is not None:
            payload["sicDescription"] = str(row[desc_col]).strip()
        lookup[key] = payload

    def fetch(cik: str | int) -> dict | None:
        raw = str(cik).strip()
        key = str(int(raw)) if raw.isdigit() else raw
        return lookup.get(key)

    return fetch


def _make_submissions_zip_fetcher(path: str | Path) -> Callable[[str | int], dict | None]:
    """Return an OFFLINE fetcher backed by the SEC bulk ``submissions.zip``.

    SEC publishes a free bulk archive at
    ``https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip``
    containing one ``CIK##########.json`` per filer, each carrying a top-level
    ``sic`` / ``sicDescription``. Lookups are lazy (one member read per CIK) so the
    ~1 GB archive never needs to be loaded into memory. Download is a one-time
    operator step; this fetcher and all tests run purely against a local file.
    """
    archive = zipfile.ZipFile(path)
    names = set(archive.namelist())

    def fetch(cik: str | int) -> dict | None:
        raw = str(cik).strip()
        try:
            member = f"CIK{int(raw):010d}.json"
        except ValueError:
            return None
        if member not in names:
            return None
        with archive.open(member) as handle:
            data = json.load(handle)
        sic = data.get("sic")
        if not sic:
            return None
        return {"sic": str(sic), "sicDescription": data.get("sicDescription")}

    return fetch


# ---------------------------------------------------------------------------
# Dataset 4: EntityClassificationDataset
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class EntityClassificationOptions:
    """Options for EntityClassificationDataset.

    fetcher: callable(cik) -> dict | None
        Injected for testing; defaults to the real SEC submissions endpoint.
        Must return a dict with at least 'sic' (str) and optionally 'sicDescription'.
        Return None or raise to indicate a fetch failure (security is skipped).
    symbols: optional tuple of symbol strings to restrict which securities are processed.
    user_agent: User-Agent header for the default real SEC fetcher.
    request_timeout: seconds for the default real SEC fetcher.
    run_id: optional run tracking id.
    """
    fetcher: Callable[[str | int], dict | None] | None = None
    sic_file: Path | None = None
    submissions_zip: Path | None = None
    symbols: tuple[str, ...] | None = None
    user_agent: str = _DEFAULT_USER_AGENT
    request_timeout: int = 30
    run_id: str | None = None


class EntityClassificationDataset(Dataset):
    """Classify securities with PIT SIC + derived FF12 + NAICS entity_classification rows.

    For each security in `securities`:
    1. Resolve CIK from security_identifier_history.
    2. Fetch SEC submissions JSON (or use injected fetcher) to get SIC.
    3. Ensure the 4-digit SIC leaf node exists under its major group.
    4. Write a primary entity_classification row under SIC (valid_to NULL = open).
    5. Derive and write secondary rows under FAMA_FRENCH_12 and NAICS_2022.
    6. Bitemporal: if a different primary SIC was already open, close it first.
    """

    dataset_id = "entity_classification"
    source_name = SOURCE_ENTITY

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: EntityClassificationOptions,
    ) -> DatasetLoadResult:
        if options.fetcher is not None:
            fetcher = options.fetcher
        elif options.submissions_zip is not None:
            submissions_zip = Path(options.submissions_zip).resolve()
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=SEC_BULK_SUBMISSIONS_URL,
                cache_path=submissions_zip,
                status="cached",
                metadata={"source_kind": "SEC nightly bulk submissions archive"},
                compute_hash=True,
            )
            fetcher = _make_submissions_zip_fetcher(submissions_zip)
        elif options.sic_file is not None:
            fetcher = _make_csv_fetcher(options.sic_file)
        else:
            fetcher = _make_real_fetcher(
                user_agent=options.user_agent,
                request_timeout=options.request_timeout,
            )

        # Resolve taxonomy IDs (must already be seeded)
        sic_taxonomy_id = _taxonomy_id_for(store, "SIC")
        ff12_taxonomy_id = _taxonomy_id_for(store, "FAMA_FRENCH_12")
        naics_taxonomy_id = _taxonomy_id_for(store, "NAICS_2022")

        if sic_taxonomy_id is None:
            raise RuntimeError("SIC taxonomy not seeded — run SicTaxonomyDataset first")

        # Fetch securities with their CIKs
        if options.symbols:
            placeholders = ",".join("?" * len(options.symbols))
            rows = store.con.execute(
                f"""
                SELECT DISTINCT s.security_id, ih.id_value AS cik
                FROM securities s
                JOIN security_identifier_history ih
                    ON ih.security_id = s.security_id AND ih.id_type = 'CIK'
                WHERE s.primary_symbol IN ({placeholders})
                ORDER BY s.security_id
                """,
                list(options.symbols),
            ).fetchall()
        else:
            rows = store.con.execute(
                """
                SELECT DISTINCT s.security_id, ih.id_value AS cik
                FROM securities s
                JOIN security_identifier_history ih
                    ON ih.security_id = s.security_id AND ih.id_type = 'CIK'
                ORDER BY s.security_id
                """
            ).fetchall()

        # Use the UTC date so valid_from/as_of_date share the same time axis as
        # available_at/now_ts (now_utc_naive). dt.date.today() is local and would
        # disagree with the UTC axis by a day near midnight.
        now_ts = now_utc_naive()
        today = now_ts.date()
        rows_written = 0

        for security_id, cik in rows:
            try:
                submission = fetcher(cik)
            except Exception as exc:
                logger.warning("Skipping %s (CIK %s): fetcher error: %s", security_id, cik, exc)
                continue

            if submission is None:
                logger.warning("Skipping %s (CIK %s): fetcher returned None", security_id, cik)
                continue

            sic_raw = submission.get("sic")
            if not sic_raw:
                logger.info("Skipping %s (CIK %s): no SIC in submission", security_id, cik)
                continue

            try:
                sic4 = int(sic_raw)
            except (ValueError, TypeError):
                logger.warning("Skipping %s: invalid SIC %r", security_id, sic_raw)
                continue

            sic_str = str(sic4).zfill(4)

            # Ensure leaf node exists
            _, sic_leaf_node_id = ensure_sic_leaf_node(store, sic4)

            # Bitemporal: check existing open primary SIC interval
            existing = store.con.execute(
                """
                SELECT classification_id, node_code
                FROM entity_classification
                WHERE security_id = ?
                  AND taxonomy_id = ?
                  AND is_primary = true
                  AND valid_to IS NULL
                """,
                [security_id, sic_taxonomy_id],
            ).fetchone()

            if existing is not None:
                existing_id, existing_node_code = existing
                if existing_node_code == sic_str:
                    # Same SIC — no change needed for primary SIC row
                    # Still re-derive FF12/NAICS below to fill in if missing
                    _write_derived_rows(
                        store,
                        security_id=security_id,
                        sic4=sic4,
                        ff12_taxonomy_id=ff12_taxonomy_id,
                        naics_taxonomy_id=naics_taxonomy_id,
                        today=today,
                        now_ts=now_ts,
                        run_id=options.run_id,
                        source=SOURCE_ENTITY,
                    )
                    continue
                else:
                    # Different SIC — close the old interval
                    store.con.execute(
                        "UPDATE entity_classification SET valid_to = ? WHERE classification_id = ?",
                        [today, existing_id],
                    )

            # Write primary SIC row
            classification_id = str(uuid.uuid4())
            store.con.execute(
                """
                INSERT INTO entity_classification
                    (classification_id, security_id, taxonomy_id, node_id, node_code,
                     is_primary, valid_from, valid_to, as_of_date, available_at,
                     source_loaded_at, run_id, source)
                VALUES (?, ?, ?, ?, ?, true, ?, NULL, ?, ?, now(), ?, ?)
                """,
                [
                    classification_id,
                    security_id,
                    sic_taxonomy_id,
                    sic_leaf_node_id,
                    sic_str,
                    today,
                    today,
                    now_ts,
                    options.run_id,
                    SOURCE_ENTITY,
                ],
            )
            rows_written += 1

            # Derived FF12 and NAICS rows
            rows_written += _write_derived_rows(
                store,
                security_id=security_id,
                sic4=sic4,
                ff12_taxonomy_id=ff12_taxonomy_id,
                naics_taxonomy_id=naics_taxonomy_id,
                today=today,
                now_ts=now_ts,
                run_id=options.run_id,
                source=SOURCE_ENTITY,
            )

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_written,
            source=SOURCE_ENTITY,
            details={"securities_processed": len(rows)},
        )


def _write_derived_rows(
    store: DuckDBStore,
    *,
    security_id: str,
    sic4: int,
    ff12_taxonomy_id: str | None,
    naics_taxonomy_id: str | None,
    today: dt.date,
    now_ts: dt.datetime,
    run_id: str | None,
    source: str,
) -> int:
    """Write derived FF12 and NAICS classification rows (is_primary=False).

    Skips if the row already exists with an open interval and same node_code.
    Returns the number of new rows written.
    """
    written = 0
    sic2 = str(sic4 // 100).zfill(2)

    # FF12 derived row
    if ff12_taxonomy_id:
        ff_code = fama_french_12_for_sic(sic4)
        ff_node_id = _node_id_for(store, ff12_taxonomy_id, ff_code)
        if ff_node_id:
            # Bitemporal: close any OPEN derived FF12 interval whose node_code
            # differs (e.g. the primary SIC moved across an FF12 boundary
            # BusEq -> NoDur). Without this, a new open row would be inserted
            # beside the stale one -> two open intervals for (security_id, taxonomy_id).
            store.con.execute(
                """
                UPDATE entity_classification
                SET valid_to = ?
                WHERE security_id = ? AND taxonomy_id = ?
                  AND is_primary = false AND valid_to IS NULL
                  AND node_code <> ?
                """,
                [today, security_id, ff12_taxonomy_id, ff_code],
            )
            existing_ff = store.con.execute(
                """
                SELECT classification_id FROM entity_classification
                WHERE security_id = ? AND taxonomy_id = ?
                  AND node_code = ? AND is_primary = false AND valid_to IS NULL
                """,
                [security_id, ff12_taxonomy_id, ff_code],
            ).fetchone()
            if existing_ff is None:
                store.con.execute(
                    """
                    INSERT INTO entity_classification
                        (classification_id, security_id, taxonomy_id, node_id, node_code,
                         is_primary, valid_from, valid_to, as_of_date, available_at,
                         source_loaded_at, run_id, source)
                    VALUES (?, ?, ?, ?, ?, false, ?, NULL, ?, ?, now(), ?, ?)
                    """,
                    [
                        str(uuid.uuid4()),
                        security_id,
                        ff12_taxonomy_id,
                        ff_node_id,
                        ff_code,
                        today,
                        today,
                        now_ts,
                        run_id,
                        source,
                    ],
                )
                written += 1

    # NAICS derived row
    if naics_taxonomy_id:
        # Look up NAICS sector from partial SIC→NAICS mapping in DB
        naics_row = store.con.execute(
            """
            SELECT to_node_code FROM taxonomy_mapping
            WHERE from_taxonomy_id = (SELECT taxonomy_id FROM taxonomy WHERE code = 'SIC')
              AND from_node_code = ?
              AND to_taxonomy_id = ?
            LIMIT 1
            """,
            [sic2, naics_taxonomy_id],
        ).fetchone()
        if naics_row:
            naics_code = naics_row[0]
            naics_node_id = _node_id_for(store, naics_taxonomy_id, naics_code)
            if naics_node_id:
                # Bitemporal: close any OPEN derived NAICS interval whose
                # node_code differs (primary SIC moved across a NAICS boundary).
                store.con.execute(
                    """
                    UPDATE entity_classification
                    SET valid_to = ?
                    WHERE security_id = ? AND taxonomy_id = ?
                      AND is_primary = false AND valid_to IS NULL
                      AND node_code <> ?
                    """,
                    [today, security_id, naics_taxonomy_id, naics_code],
                )
                existing_naics = store.con.execute(
                    """
                    SELECT classification_id FROM entity_classification
                    WHERE security_id = ? AND taxonomy_id = ?
                      AND node_code = ? AND is_primary = false AND valid_to IS NULL
                    """,
                    [security_id, naics_taxonomy_id, naics_code],
                ).fetchone()
                if existing_naics is None:
                    store.con.execute(
                        """
                        INSERT INTO entity_classification
                            (classification_id, security_id, taxonomy_id, node_id, node_code,
                             is_primary, valid_from, valid_to, as_of_date, available_at,
                             source_loaded_at, run_id, source)
                        VALUES (?, ?, ?, ?, ?, false, ?, NULL, ?, ?, now(), ?, ?)
                        """,
                        [
                            str(uuid.uuid4()),
                            security_id,
                            naics_taxonomy_id,
                            naics_node_id,
                            naics_code,
                            today,
                            today,
                            now_ts,
                            run_id,
                            source,
                        ],
                    )
                    written += 1

    return written
