"""Reference classification layer — S1 (+ P6: FF49 and the activation-ladder snapshot stage).

Implements:
- SicTaxonomyDataset     — SIC divisions + 2-digit major groups (embedded static)
- FamaFrenchTaxonomyDataset — FF12 and FF49 industries + SIC-range taxonomy_mapping rows
- NaicsTaxonomyDataset   — NAICS 2022 2-digit sectors + partial SIC→NAICS mapping
- EntityClassificationDataset — per-security classifications (SIC primary,
  FAMA_FRENCH_12, FAMA_FRENCH_49 and NAICS_2022 derived) for the legacy jobs path
- refresh_entity_classification_snapshot — the set-based activation-ladder writer
  (``entity_classification`` stage) over the retained SEC ``submissions.zip``

Pure helpers exported for tests and downstream use:
- fama_french_12_for_sic(sic: int) -> str
- fama_french_49_for_sic(sic: int) -> str | None

Classification basis (P6): SEC ``submissions`` carry only each filer's CURRENT SIC.
Rows written from a snapshot are labeled ``classification_basis='current_sic_snapshot'``
and are valid from the snapshot's receipt date, never earlier. Applying today's SIC to
earlier dates (the research ``current_sic_backcast``) is a known bias and is labeled
as such by its consumers; it is never point-in-time SIC history.

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
from collections import Counter
from collections.abc import Callable, Iterable
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

# The canonical Ken French FF12 industries. Exactly 12 codes (#6 in French's file is
# "BusEq", Business Equipment; this list's order is only the node sort order).
# "HiTec" is an FF5 label, NOT an FF12 industry, so it is deliberately absent.
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

# Ken French FF12 SIC ranges, verbatim from "Detail for 12 Industry Portfolios"
# (Siccodes12), Ken French Data Library:
# https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_12_ind_port.html
# Each entry: (ff12_code, [(lo, hi), ...]) inclusive 4-digit SIC ranges. The ranges are
# disjoint (validated at import); industry 12 "Other" is the residual — every SIC not
# listed ("everything else"), so it has no ranges of its own here.
#
# P6 (2026-09-25): this table replaced an earlier hand-built, overlapping
# "first range wins" table that disagreed with French's definition on 1,644 of the
# 9,900 SIC code points 0100-9999 (e.g. electronic components 3670-3679 -> Durbl,
# chemicals 2800-2829 -> NoDur, coal 1200-1299 -> Other, wholesale 5000-5099 -> Durbl,
# retail 5200-5299/5600-5699/5900-5999 -> NoDur, 4950-4991 -> Utils). The
# mapping version below changes with it so every consumer digest changes honestly.
FF12_MAPPING_VERSION = "french_siccodes12_v2"
FF12_OTHER = "Other"
FF12_SIC_RANGES: list[tuple[str, list[tuple[int, int]]]] = [
    ("NoDur", [(100, 999), (2000, 2399), (2700, 2749), (2770, 2799), (3100, 3199), (3940, 3989)]),
    ("Durbl", [
        (2500, 2519), (2590, 2599), (3630, 3659), (3710, 3711), (3714, 3714), (3716, 3716),
        (3750, 3751), (3792, 3792), (3900, 3939), (3990, 3999),
    ]),
    ("Manuf", [
        (2520, 2589), (2600, 2699), (2750, 2769), (3000, 3099), (3200, 3569), (3580, 3629),
        (3700, 3709), (3712, 3713), (3715, 3715), (3717, 3749), (3752, 3791), (3793, 3799),
        (3830, 3839), (3860, 3899),
    ]),
    ("Enrgy", [(1200, 1399), (2900, 2999)]),
    ("Chems", [(2800, 2829), (2840, 2899)]),
    ("BusEq", [(3570, 3579), (3660, 3692), (3694, 3699), (3810, 3829), (7370, 7379)]),
    ("Telcm", [(4800, 4899)]),
    ("Utils", [(4900, 4949)]),
    ("Shops", [(5000, 5999), (7200, 7299), (7600, 7699)]),
    ("Hlth", [(2830, 2839), (3693, 3693), (3840, 3859), (8000, 8099)]),
    ("Money", [(6000, 6999)]),
]

SIC_MIN, SIC_MAX = 100, 9999


def _build_disjoint_lookup(
    table: Iterable[tuple[str, Iterable[tuple[int, int]]]], name: str,
) -> dict[int, str]:
    """SIC -> code over a disjoint range table; fails loudly on any overlap (encoding error)."""
    lookup: dict[int, str] = {}
    for code, ranges in table:
        for lo, hi in ranges:
            if not SIC_MIN <= lo <= hi <= SIC_MAX:
                raise ValueError(f"{name}: invalid SIC range {lo}-{hi} for {code}")
            for sic in range(lo, hi + 1):
                prior = lookup.setdefault(sic, code)
                if prior != code:
                    raise ValueError(f"{name}: SIC {sic} is listed under both {prior} and {code}")
    return lookup


def _complement_ranges(covered: Iterable[int]) -> list[tuple[int, int]]:
    """Inclusive SIC ranges in [SIC_MIN, SIC_MAX] not in ``covered`` (the residual industry)."""
    taken = set(covered)
    ranges: list[tuple[int, int]] = []
    start: int | None = None
    for sic in range(SIC_MIN, SIC_MAX + 2):
        free = sic <= SIC_MAX and sic not in taken
        if free and start is None:
            start = sic
        elif not free and start is not None:
            ranges.append((start, sic - 1))
            start = None
    return ranges


_FF12_NAMED = _build_disjoint_lookup(FF12_SIC_RANGES, "FF12")
_FF12_LOOKUP: dict[int, str] = {
    sic: _FF12_NAMED.get(sic, FF12_OTHER) for sic in range(SIC_MIN, SIC_MAX + 1)
}


def fama_french_12_for_sic(sic: int) -> str:
    """Return the Fama-French 12-industry code for a 4-digit SIC code.

    Uses French's Siccodes12 table embedded in this module. Returns 'Other' for any
    SIC not listed under a named industry (French's industry 12 is "everything else").
    """
    return _FF12_LOOKUP.get(sic, FF12_OTHER)


# ---------------------------------------------------------------------------
# Fama-French 49 industries (P6)
# ---------------------------------------------------------------------------

# Ken French FF49 industry definitions, verbatim from "Detail for 49 Industry
# Portfolios" (Siccodes49), Ken French Data Library:
# https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_49_ind_port.html
# Each entry: (number, code, label, ranges); one (lo, hi) tuple per line of French's
# file, inclusive 4-digit SIC. Ranges are disjoint (validated at import). Unlike FF12,
# industry 49 "Other" ("Almost Nothing") is defined by its own four ranges, so a SIC
# listed under no industry has NO FF49 industry: fama_french_49_for_sic returns None,
# the ladder writes no FF49 row and counts it (never assigned to Other by default).
FF49_MAPPING_VERSION = "french_siccodes49_v1"
FF49_INDUSTRIES: list[tuple[int, str, str, tuple[tuple[int, int], ...]]] = [
    (1, "Agric", "Agriculture", ((100, 199), (200, 299), (700, 799), (910, 919), (2048, 2048))),
    (2, "Food", "Food Products", (
        (2000, 2009), (2010, 2019), (2020, 2029), (2030, 2039), (2040, 2046), (2050, 2059),
        (2060, 2063), (2070, 2079), (2090, 2092), (2095, 2095), (2098, 2099),
    )),
    (3, "Soda", "Candy & Soda", ((2064, 2068), (2086, 2086), (2087, 2087), (2096, 2096), (2097, 2097))),
    (4, "Beer", "Beer & Liquor", ((2080, 2080), (2082, 2082), (2083, 2083), (2084, 2084), (2085, 2085))),
    (5, "Smoke", "Tobacco Products", ((2100, 2199),)),
    (6, "Toys", "Recreation", (
        (920, 999), (3650, 3651), (3652, 3652), (3732, 3732), (3930, 3931), (3940, 3949),
    )),
    (7, "Fun", "Entertainment", (
        (7800, 7829), (7830, 7833), (7840, 7841), (7900, 7900), (7910, 7911), (7920, 7929),
        (7930, 7933), (7940, 7949), (7980, 7980), (7990, 7999),
    )),
    (8, "Books", "Printing and Publishing", (
        (2700, 2709), (2710, 2719), (2720, 2729), (2730, 2739), (2740, 2749), (2770, 2771),
        (2780, 2789), (2790, 2799),
    )),
    (9, "Hshld", "Consumer Goods", (
        (2047, 2047), (2391, 2392), (2510, 2519), (2590, 2599), (2840, 2843), (2844, 2844),
        (3160, 3161), (3170, 3171), (3172, 3172), (3190, 3199), (3229, 3229), (3260, 3260),
        (3262, 3263), (3269, 3269), (3230, 3231), (3630, 3639), (3750, 3751), (3800, 3800),
        (3860, 3861), (3870, 3873), (3910, 3911), (3914, 3914), (3915, 3915), (3960, 3962),
        (3991, 3991), (3995, 3995),
    )),
    (10, "Clths", "Apparel", (
        (2300, 2390), (3020, 3021), (3100, 3111), (3130, 3131), (3140, 3149), (3150, 3151),
        (3963, 3965),
    )),
    (11, "Hlth", "Healthcare", ((8000, 8099),)),
    (12, "MedEq", "Medical Equipment", ((3693, 3693), (3840, 3849), (3850, 3851))),
    (13, "Drugs", "Pharmaceutical Products", (
        (2830, 2830), (2831, 2831), (2833, 2833), (2834, 2834), (2835, 2835), (2836, 2836),
    )),
    (14, "Chems", "Chemicals", (
        (2800, 2809), (2810, 2819), (2820, 2829), (2850, 2859), (2860, 2869), (2870, 2879),
        (2890, 2899),
    )),
    (15, "Rubbr", "Rubber and Plastic Products", (
        (3031, 3031), (3041, 3041), (3050, 3053), (3060, 3069), (3070, 3079), (3080, 3089),
        (3090, 3099),
    )),
    (16, "Txtls", "Textiles", (
        (2200, 2269), (2270, 2279), (2280, 2284), (2290, 2295), (2297, 2297), (2298, 2298),
        (2299, 2299), (2393, 2395), (2397, 2399),
    )),
    (17, "BldMt", "Construction Materials", (
        (800, 899), (2400, 2439), (2450, 2459), (2490, 2499), (2660, 2661), (2950, 2952),
        (3200, 3200), (3210, 3211), (3240, 3241), (3250, 3259), (3261, 3261), (3264, 3264),
        (3270, 3275), (3280, 3281), (3290, 3293), (3295, 3299), (3420, 3429), (3430, 3433),
        (3440, 3441), (3442, 3442), (3446, 3446), (3448, 3448), (3449, 3449), (3450, 3451),
        (3452, 3452), (3490, 3499), (3996, 3996),
    )),
    (18, "Cnstr", "Construction", (
        (1500, 1511), (1520, 1529), (1530, 1539), (1540, 1549), (1600, 1699), (1700, 1799),
    )),
    (19, "Steel", "Steel Works Etc", (
        (3300, 3300), (3310, 3317), (3320, 3325), (3330, 3339), (3340, 3341), (3350, 3357),
        (3360, 3369), (3370, 3379), (3390, 3399),
    )),
    (20, "FabPr", "Fabricated Products", ((3400, 3400), (3443, 3443), (3444, 3444), (3460, 3469), (3470, 3479))),
    (21, "Mach", "Machinery", (
        (3510, 3519), (3520, 3529), (3530, 3530), (3531, 3531), (3532, 3532), (3533, 3533),
        (3534, 3534), (3535, 3535), (3536, 3536), (3538, 3538), (3540, 3549), (3550, 3559),
        (3560, 3569), (3580, 3580), (3581, 3581), (3582, 3582), (3585, 3585), (3586, 3586),
        (3589, 3589), (3590, 3599),
    )),
    (22, "ElcEq", "Electrical Equipment", (
        (3600, 3600), (3610, 3613), (3620, 3621), (3623, 3629), (3640, 3644), (3645, 3645),
        (3646, 3646), (3648, 3649), (3660, 3660), (3690, 3690), (3691, 3692), (3699, 3699),
    )),
    (23, "Autos", "Automobiles and Trucks", (
        (2296, 2296), (2396, 2396), (3010, 3011), (3537, 3537), (3647, 3647), (3694, 3694),
        (3700, 3700), (3710, 3710), (3711, 3711), (3713, 3713), (3714, 3714), (3715, 3715),
        (3716, 3716), (3792, 3792), (3790, 3791), (3799, 3799),
    )),
    (24, "Aero", "Aircraft", ((3720, 3720), (3721, 3721), (3723, 3724), (3725, 3725), (3728, 3729))),
    (25, "Ships", "Shipbuilding, Railroad Equipment", ((3730, 3731), (3740, 3743))),
    (26, "Guns", "Defense", ((3760, 3769), (3795, 3795), (3480, 3489))),
    (27, "Gold", "Precious Metals", ((1040, 1049),)),
    (28, "Mines", "Non-Metallic and Industrial Metal Mining", (
        (1000, 1009), (1010, 1019), (1020, 1029), (1030, 1039), (1050, 1059), (1060, 1069),
        (1070, 1079), (1080, 1089), (1090, 1099), (1100, 1119), (1400, 1499),
    )),
    (29, "Coal", "Coal", ((1200, 1299),)),
    (30, "Oil", "Petroleum and Natural Gas", (
        (1300, 1300), (1310, 1319), (1320, 1329), (1330, 1339), (1370, 1379), (1380, 1380),
        (1381, 1381), (1382, 1382), (1389, 1389), (2900, 2912), (2990, 2999),
    )),
    (31, "Util", "Utilities", (
        (4900, 4900), (4910, 4911), (4920, 4922), (4923, 4923), (4924, 4925), (4930, 4931),
        (4932, 4932), (4939, 4939), (4940, 4942),
    )),
    (32, "Telcm", "Communication", (
        (4800, 4800), (4810, 4813), (4820, 4822), (4830, 4839), (4840, 4841), (4880, 4889),
        (4890, 4890), (4891, 4891), (4892, 4892), (4899, 4899),
    )),
    (33, "PerSv", "Personal Services", (
        (7020, 7021), (7030, 7033), (7200, 7200), (7210, 7212), (7214, 7214), (7215, 7216),
        (7217, 7217), (7219, 7219), (7220, 7221), (7230, 7231), (7240, 7241), (7250, 7251),
        (7260, 7269), (7270, 7290), (7291, 7291), (7292, 7299), (7395, 7395), (7500, 7500),
        (7520, 7529), (7530, 7539), (7540, 7549), (7600, 7600), (7620, 7620), (7622, 7622),
        (7623, 7623), (7629, 7629), (7630, 7631), (7640, 7641), (7690, 7699), (8100, 8199),
        (8200, 8299), (8300, 8399), (8400, 8499), (8600, 8699), (8800, 8899), (7510, 7515),
    )),
    (34, "BusSv", "Business Services", (
        (2750, 2759), (3993, 3993), (7218, 7218), (7300, 7300), (7310, 7319), (7320, 7329),
        (7330, 7339), (7340, 7342), (7349, 7349), (7350, 7351), (7352, 7352), (7353, 7353),
        (7359, 7359), (7360, 7369), (7374, 7374), (7376, 7376), (7377, 7377), (7378, 7378),
        (7379, 7379), (7380, 7380), (7381, 7382), (7383, 7383), (7384, 7384), (7385, 7385),
        (7389, 7390), (7391, 7391), (7392, 7392), (7393, 7393), (7394, 7394), (7396, 7396),
        (7397, 7397), (7399, 7399), (7519, 7519), (8700, 8700), (8710, 8713), (8720, 8721),
        (8730, 8734), (8740, 8748), (8900, 8910), (8911, 8911), (8920, 8999), (4220, 4229),
    )),
    (35, "Hardw", "Computers", (
        (3570, 3579), (3680, 3680), (3681, 3681), (3682, 3682), (3683, 3683), (3684, 3684),
        (3685, 3685), (3686, 3686), (3687, 3687), (3688, 3688), (3689, 3689), (3695, 3695),
    )),
    (36, "Softw", "Computer Software", ((7370, 7372), (7375, 7375), (7373, 7373))),
    (37, "Chips", "Electronic Equipment", (
        (3622, 3622), (3661, 3661), (3662, 3662), (3663, 3663), (3664, 3664), (3665, 3665),
        (3666, 3666), (3669, 3669), (3670, 3679), (3810, 3810), (3812, 3812),
    )),
    (38, "LabEq", "Measuring and Control Equipment", (
        (3811, 3811), (3820, 3820), (3821, 3821), (3822, 3822), (3823, 3823), (3824, 3824),
        (3825, 3825), (3826, 3826), (3827, 3827), (3829, 3829), (3830, 3839),
    )),
    (39, "Paper", "Business Supplies", ((2520, 2549), (2600, 2639), (2670, 2699), (2760, 2761), (3950, 3955))),
    (40, "Boxes", "Shipping Containers", ((2440, 2449), (2640, 2659), (3220, 3221), (3410, 3412))),
    (41, "Trans", "Transportation", (
        (4000, 4013), (4040, 4049), (4100, 4100), (4110, 4119), (4120, 4121), (4130, 4131),
        (4140, 4142), (4150, 4151), (4170, 4173), (4190, 4199), (4200, 4200), (4210, 4219),
        (4230, 4231), (4240, 4249), (4400, 4499), (4500, 4599), (4600, 4699), (4700, 4700),
        (4710, 4712), (4720, 4729), (4730, 4739), (4740, 4749), (4780, 4780), (4782, 4782),
        (4783, 4783), (4784, 4784), (4785, 4785), (4789, 4789),
    )),
    (42, "Whlsl", "Wholesale", (
        (5000, 5000), (5010, 5015), (5020, 5023), (5030, 5039), (5040, 5042), (5043, 5043),
        (5044, 5044), (5045, 5045), (5046, 5046), (5047, 5047), (5048, 5048), (5049, 5049),
        (5050, 5059), (5060, 5060), (5063, 5063), (5064, 5064), (5065, 5065), (5070, 5078),
        (5080, 5080), (5081, 5081), (5082, 5082), (5083, 5083), (5084, 5084), (5085, 5085),
        (5086, 5087), (5088, 5088), (5090, 5090), (5091, 5092), (5093, 5093), (5094, 5094),
        (5099, 5099), (5100, 5100), (5110, 5113), (5120, 5122), (5130, 5139), (5140, 5149),
        (5150, 5159), (5160, 5169), (5170, 5172), (5180, 5182), (5190, 5199),
    )),
    (43, "Rtail", "Retail", (
        (5200, 5200), (5210, 5219), (5220, 5229), (5230, 5231), (5250, 5251), (5260, 5261),
        (5270, 5271), (5300, 5300), (5310, 5311), (5320, 5320), (5330, 5331), (5334, 5334),
        (5340, 5349), (5390, 5399), (5400, 5400), (5410, 5411), (5412, 5412), (5420, 5429),
        (5430, 5439), (5440, 5449), (5450, 5459), (5460, 5469), (5490, 5499), (5500, 5500),
        (5510, 5529), (5530, 5539), (5540, 5549), (5550, 5559), (5560, 5569), (5570, 5579),
        (5590, 5599), (5600, 5699), (5700, 5700), (5710, 5719), (5720, 5722), (5730, 5733),
        (5734, 5734), (5735, 5735), (5736, 5736), (5750, 5799), (5900, 5900), (5910, 5912),
        (5920, 5929), (5930, 5932), (5940, 5940), (5941, 5941), (5942, 5942), (5943, 5943),
        (5944, 5944), (5945, 5945), (5946, 5946), (5947, 5947), (5948, 5948), (5949, 5949),
        (5950, 5959), (5960, 5969), (5970, 5979), (5980, 5989), (5990, 5990), (5992, 5992),
        (5993, 5993), (5994, 5994), (5995, 5995), (5999, 5999),
    )),
    (44, "Meals", "Restaurants, Hotels, Motels", (
        (5800, 5819), (5820, 5829), (5890, 5899), (7000, 7000), (7010, 7019), (7040, 7049),
        (7213, 7213),
    )),
    (45, "Banks", "Banking", (
        (6000, 6000), (6010, 6019), (6020, 6020), (6021, 6021), (6022, 6022), (6023, 6024),
        (6025, 6025), (6026, 6026), (6027, 6027), (6028, 6029), (6030, 6036), (6040, 6059),
        (6060, 6062), (6080, 6082), (6090, 6099), (6100, 6100), (6110, 6111), (6112, 6113),
        (6120, 6129), (6130, 6139), (6140, 6149), (6150, 6159), (6160, 6169), (6170, 6179),
        (6190, 6199),
    )),
    (46, "Insur", "Insurance", (
        (6300, 6300), (6310, 6319), (6320, 6329), (6330, 6331), (6350, 6351), (6360, 6361),
        (6370, 6379), (6390, 6399), (6400, 6411),
    )),
    (47, "RlEst", "Real Estate", (
        (6500, 6500), (6510, 6510), (6512, 6512), (6513, 6513), (6514, 6514), (6515, 6515),
        (6517, 6519), (6520, 6529), (6530, 6531), (6532, 6532), (6540, 6541), (6550, 6553),
        (6590, 6599), (6610, 6611),
    )),
    (48, "Fin", "Trading", (
        (6200, 6299), (6700, 6700), (6710, 6719), (6720, 6722), (6723, 6723), (6724, 6724),
        (6725, 6725), (6726, 6726), (6730, 6733), (6740, 6779), (6790, 6791), (6792, 6792),
        (6793, 6793), (6794, 6794), (6795, 6795), (6798, 6798), (6799, 6799),
    )),
    (49, "Other", "Almost Nothing", ((4950, 4959), (4960, 4961), (4970, 4971), (4990, 4991))),
]
_FF49_LOOKUP: dict[int, str] = _build_disjoint_lookup(
    ((code, ranges) for _, code, _, ranges in FF49_INDUSTRIES), "FF49",
)
if [number for number, *_ in FF49_INDUSTRIES] != list(range(1, 50)):
    raise ValueError("FF49_INDUSTRIES must list French's industries 1..49 in order")


def fama_french_49_for_sic(sic: int) -> str | None:
    """Return the Fama-French 49-industry code for a 4-digit SIC, or None when unlisted.

    Uses French's Siccodes49 table embedded in this module. A SIC listed under no
    industry (e.g. 9995, 9999, 2049) has no FF49 industry; it is never defaulted to
    49 "Other", which French defines by its own four ranges.
    """
    return _FF49_LOOKUP.get(sic)


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


def _ensure_taxonomy_version(
    store: DuckDBStore, *, code: str, version: str, description: str,
) -> str:
    """Upsert a taxonomy and move an existing row to ``version`` (mapping-version bump)."""
    taxonomy_id = _upsert_taxonomy(
        store,
        code=code,
        name={"FAMA_FRENCH_12": "Fama-French 12 Industries",
              "FAMA_FRENCH_49": "Fama-French 49 Industries"}.get(code, code),
        provider="Ken French Data Library",
        version=version,
        is_hierarchical=False,
        description=description,
        source=SOURCE_FF,
    )
    store.con.execute(
        """
        UPDATE taxonomy SET version = ?, description = ?, source_loaded_at = now()
        WHERE taxonomy_id = ? AND (version IS DISTINCT FROM ? OR description IS DISTINCT FROM ?)
        """,
        [version, description, taxonomy_id, version, description],
    )
    return taxonomy_id


def _seed_ff_taxonomy(
    store: DuckDBStore,
    *,
    code: str,
    tag: str,
    version: str,
    description: str,
    nodes: list[tuple[str, str]],
    ranges: list[tuple[str, list[tuple[int, int]]]],
) -> tuple[int, int]:
    """Seed one Fama-French taxonomy: nodes + the EXACT SIC-range crosswalk.

    The crosswalk is replaced as a set (stale ranges from an earlier mapping version
    are deleted), so ``taxonomy_mapping`` always equals the embedded table.
    """
    taxonomy_id = _ensure_taxonomy_version(store, code=code, version=version, description=description)
    node_df = pd.DataFrame(
        [
            (str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{taxonomy_id}:{node}")), taxonomy_id, node, label,
             None, 1, sort)
            for sort, (node, label) in enumerate(nodes)
        ],
        columns=["node_id", "taxonomy_id", "node_code", "node_label", "parent_node_id", "level", "sort_order"],
    )
    store.con.register("_ff_node_seed", node_df)
    try:
        store.con.execute(
            """
            INSERT OR REPLACE INTO taxonomy_node
                (node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order)
            SELECT node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order
            FROM _ff_node_seed
            """
        )
    finally:
        store.con.unregister("_ff_node_seed")

    sic_taxonomy_id = _taxonomy_id_for(store, "SIC")
    if not sic_taxonomy_id:
        return len(node_df), 0
    map_df = pd.DataFrame(
        [
            (str(uuid.uuid5(uuid.NAMESPACE_DNS, f"mapping:SIC:{lo}-{hi}:{tag}:{node}")), sic_taxonomy_id,
             f"{lo}-{hi}", taxonomy_id, node, "many_to_one", 1.0, f"{SOURCE_FF} ({version})")
            for node, node_ranges in ranges
            for lo, hi in node_ranges
        ],
        columns=["mapping_id", "from_taxonomy_id", "from_node_code", "to_taxonomy_id", "to_node_code",
                 "relationship", "confidence", "source"],
    )
    store.con.execute(
        "DELETE FROM taxonomy_mapping WHERE from_taxonomy_id = ? AND to_taxonomy_id = ?",
        [sic_taxonomy_id, taxonomy_id],
    )
    store.con.register("_ff_mapping_seed", map_df)
    try:
        store.con.execute(
            """
            INSERT OR REPLACE INTO taxonomy_mapping
                (mapping_id, from_taxonomy_id, from_node_code, to_taxonomy_id, to_node_code,
                 relationship, confidence, source)
            SELECT mapping_id, from_taxonomy_id, from_node_code, to_taxonomy_id, to_node_code,
                   relationship, confidence, source
            FROM _ff_mapping_seed
            """
        )
    finally:
        store.con.unregister("_ff_mapping_seed")
    return len(node_df), len(map_df)


class FamaFrenchTaxonomyDataset(Dataset):
    """Seed FAMA_FRENCH_12 and FAMA_FRENCH_49 nodes and their SIC-range crosswalks.

    Uses French's Siccodes12 / Siccodes49 definitions (Ken French Data Library,
    public). FF12 "Other" is mapped from the residual SIC ranges (everything not
    listed); FF49 maps only French's listed ranges. Idempotent — safe to rerun; a
    mapping-version change replaces the crosswalk and bumps ``taxonomy.version``.
    """

    dataset_id = "fama_french_taxonomy"
    source_name = SOURCE_FF

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FamaFrenchTaxonomyOptions) -> DatasetLoadResult:
        _ = options
        ff12_nodes, ff12_ranges = _seed_ff_taxonomy(
            store,
            code="FAMA_FRENCH_12",
            tag="FF12",
            version=FF12_MAPPING_VERSION,
            description=(
                "12-industry Fama-French classification mapped from 4-digit SIC ranges "
                "(French Siccodes12; industry 12 Other = every unlisted SIC). "
                "Source: https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/"
                "Data_Library/det_12_ind_port.html"
            ),
            nodes=FF12_INDUSTRIES,
            ranges=[*FF12_SIC_RANGES, (FF12_OTHER, _complement_ranges(_FF12_NAMED))],
        )
        ff49_nodes, ff49_ranges = _seed_ff_taxonomy(
            store,
            code="FAMA_FRENCH_49",
            tag="FF49",
            version=FF49_MAPPING_VERSION,
            description=(
                "49-industry Fama-French classification mapped from 4-digit SIC ranges "
                "(French Siccodes49; SIC codes listed under no industry have no FF49 industry). "
                "Source: https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/"
                "Data_Library/det_49_ind_port.html"
            ),
            nodes=[(code, label) for _, code, label, _ in FF49_INDUSTRIES],
            ranges=[(code, list(ranges)) for _, code, _, ranges in FF49_INDUSTRIES],
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=ff12_nodes + ff12_ranges + ff49_nodes + ff49_ranges,
            source=SOURCE_FF,
            details={
                "ff12_nodes": ff12_nodes, "mapping_ranges": ff12_ranges,
                "ff49_nodes": ff49_nodes, "ff49_mapping_ranges": ff49_ranges,
                "mapping_versions": {"FAMA_FRENCH_12": FF12_MAPPING_VERSION,
                                     "FAMA_FRENCH_49": FF49_MAPPING_VERSION},
            },
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

_DEFAULT_USER_AGENT = "atx-db/0.1 atx-research@example.com"
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
        ff49_taxonomy_id = _taxonomy_id_for(store, "FAMA_FRENCH_49")
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
                        ff49_taxonomy_id=ff49_taxonomy_id,
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
                ff49_taxonomy_id=ff49_taxonomy_id,
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
    ff49_taxonomy_id: str | None = None,
) -> int:
    """Write derived FF12, FF49 and NAICS classification rows (is_primary=False).

    Skips if the row already exists with an open interval and same node_code.
    Returns the number of new rows written.
    """
    written = 0
    sic2 = str(sic4 // 100).zfill(2)

    # FF49 derived row (none for a SIC French lists under no FF49 industry: any open FF49
    # interval from an earlier SIC is then closed, never left to outlive its SIC)
    ff49_code = fama_french_49_for_sic(sic4)
    if ff49_taxonomy_id and ff49_code is None:
        _close_open_derived_rows(store, security_id=security_id, taxonomy_id=ff49_taxonomy_id, today=today)
    elif ff49_taxonomy_id and ff49_code is not None:
        ff49_node_id = _node_id_for(store, ff49_taxonomy_id, ff49_code)
        if ff49_node_id:
            written += _write_open_derived_row(
                store, security_id=security_id, taxonomy_id=ff49_taxonomy_id, node_id=ff49_node_id,
                node_code=ff49_code, today=today, now_ts=now_ts, run_id=run_id, source=source,
            )

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
        if not naics_row:
            # No NAICS-2 sector for this SIC: close any open NAICS interval from an earlier SIC.
            _close_open_derived_rows(store, security_id=security_id, taxonomy_id=naics_taxonomy_id, today=today)
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


def _close_open_derived_rows(store: DuckDBStore, *, security_id: str, taxonomy_id: str, today: dt.date) -> None:
    """Close every open derived interval of ``taxonomy_id`` (the new SIC maps to no industry there)."""
    store.con.execute(
        """
        UPDATE entity_classification SET valid_to = ?
        WHERE security_id = ? AND taxonomy_id = ? AND is_primary = false AND valid_to IS NULL
        """,
        [today, security_id, taxonomy_id],
    )


def _write_open_derived_row(
    store: DuckDBStore,
    *,
    security_id: str,
    taxonomy_id: str,
    node_id: str,
    node_code: str,
    today: dt.date,
    now_ts: dt.datetime,
    run_id: str | None,
    source: str,
) -> int:
    """Close a differing open derived interval and open ``node_code`` unless already open."""
    store.con.execute(
        """
        UPDATE entity_classification SET valid_to = ?
        WHERE security_id = ? AND taxonomy_id = ? AND is_primary = false
          AND valid_to IS NULL AND node_code <> ?
        """,
        [today, security_id, taxonomy_id, node_code],
    )
    existing = store.con.execute(
        """
        SELECT classification_id FROM entity_classification
        WHERE security_id = ? AND taxonomy_id = ? AND node_code = ?
          AND is_primary = false AND valid_to IS NULL
        """,
        [security_id, taxonomy_id, node_code],
    ).fetchone()
    if existing is not None:
        return 0
    store.con.execute(
        """
        INSERT INTO entity_classification
            (classification_id, security_id, taxonomy_id, node_id, node_code,
             is_primary, valid_from, valid_to, as_of_date, available_at,
             source_loaded_at, run_id, source)
        VALUES (?, ?, ?, ?, ?, false, ?, NULL, ?, ?, now(), ?, ?)
        """,
        [str(uuid.uuid4()), security_id, taxonomy_id, node_id, node_code, today, today, now_ts, run_id, source],
    )
    return 1


# ---------------------------------------------------------------------------
# P6: activation-ladder writer over the retained SEC submissions snapshot
# ---------------------------------------------------------------------------

CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT = "current_sic_snapshot"
CLASSIFICATION_BASIS_NOTE = (
    "each filer's CURRENT SIC from the retained SEC submissions snapshot; valid from the snapshot "
    "receipt date, never earlier. Not point-in-time SIC history: applying it to earlier dates "
    "(research current_sic_backcast) is a known, labeled bias."
)
SOURCE_SIC_SNAPSHOT = f"SEC submissions.zip SIC [{CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT}]"
NAICS_MAPPING_VERSION = "sic2_partial_approximate_v1"
CLASSIFICATION_MAPPING_VERSIONS: dict[str, str] = {
    "FAMA_FRENCH_12": FF12_MAPPING_VERSION,
    "FAMA_FRENCH_49": FF49_MAPPING_VERSION,
    "NAICS_2022": NAICS_MAPPING_VERSION,
}
_OWNER_CIK_PATTERN = "CIK-([0-9]{1,10})$"
# entity_classification lineage columns added by migration 0328 (written when present).
CLASSIFICATION_LINEAGE_COLUMNS = ("classification_basis", "mapping_version")


@dataclass(frozen=True)
class SicSnapshot:
    """The retained submissions archive a ladder run classifies from.

    ``received_at`` (UTC-naive) is when the archive bytes were received -- its cache
    receipt, else its file mtime (``receipt_basis``). It is every written row's
    ``available_at``; its date is the rows' ``valid_from`` and ``as_of_date``.
    """

    path: Path
    sha256: str
    received_at: dt.datetime
    receipt_basis: str

    @property
    def valid_from(self) -> dt.date:
        return self.received_at.date()

    def as_detail(self) -> dict[str, object]:
        return {
            "path": str(self.path),
            "sha256": self.sha256,
            "received_at": self.received_at.isoformat(),
            "receipt_basis": self.receipt_basis,
            "valid_from": self.valid_from.isoformat(),
        }


def _has_table(store: DuckDBStore, table: str) -> bool:
    row = store.con.execute(
        "SELECT count(*) FROM information_schema.tables WHERE table_name = ?", [table]
    ).fetchone()
    return bool(row and row[0])


def _has_columns(store: DuckDBStore, table: str, columns: Iterable[str]) -> bool:
    present = {str(row[0]) for row in store.con.execute(
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_catalog = current_database() AND table_schema = current_schema() AND table_name = ?",
        [table],
    ).fetchall()}
    return set(columns) <= present


def classification_targets(store: DuckDBStore) -> pd.DataFrame:
    """Every ``security_id`` the ladder classifies, with its 10-digit CIK.

    Union of (a) ids carrying a CIK identifier (security master; the newest open
    interval wins) and (b) Company Facts accounting owners, whose ids embed the CIK
    (``SEC-CIK-##########`` and ``SEC-COMPANYFACTS-UNRESOLVED-CIK-##########``) --
    the latter reach delisted issuers that no longer appear in the current ticker
    map. One row per security_id; an embedded CIK wins over a differing identifier
    CIK (counted as ``cik_conflict``).
    """
    owners = (
        f"""SELECT security_id, lpad(regexp_extract(security_id, '{_OWNER_CIK_PATTERN}', 1), 10, '0') AS cik
            FROM (SELECT DISTINCT security_id FROM sec_company_facts)
            WHERE regexp_matches(security_id, '{_OWNER_CIK_PATTERN}')"""
        if _has_table(store, "sec_company_facts")
        else "SELECT CAST(NULL AS VARCHAR) AS security_id, CAST(NULL AS VARCHAR) AS cik WHERE false"
    )
    return store.con.execute(f"""
        WITH idh AS (
            SELECT security_id,
                   arg_max(lpad(CAST(try_cast(trim(id_value) AS BIGINT) AS VARCHAR), 10, '0'),
                           (valid_to IS NULL, valid_from, available_at)) AS cik
            FROM security_identifier_history
            WHERE id_type = 'CIK' AND try_cast(trim(id_value) AS BIGINT) > 0
            GROUP BY security_id
        ), owners AS ({owners})
        SELECT coalesce(o.security_id, i.security_id) AS security_id,
               coalesce(o.cik, i.cik) AS cik,
               i.security_id IS NOT NULL AS from_identifier_history,
               o.security_id IS NOT NULL AS from_company_facts,
               coalesce(i.cik <> o.cik, false) AS cik_conflict,
               NOT EXISTS (SELECT 1 FROM securities s
                           WHERE s.security_id = coalesce(o.security_id, i.security_id)) AS outside_securities
        FROM idh i FULL OUTER JOIN owners o ON o.security_id = i.security_id
        WHERE coalesce(o.cik, i.cik) IS NOT NULL
        ORDER BY 1
    """).df()


def read_snapshot_sic(
    read_member: Callable[[str], bytes | None], ciks: Iterable[str],
) -> tuple[dict[str, int], Counter[str]]:
    """Read each CIK's current SIC from ``CIK##########.json`` members (bounded: one at a time).

    ``read_member`` returns the member bytes or None when the archive has no such
    member. Missing members, blank and invalid SIC values are counted, never guessed.
    """
    sics: dict[str, int] = {}
    stats: Counter[str] = Counter()
    for cik in sorted(set(ciks)):
        payload = read_member(f"CIK{cik}.json")
        if payload is None:
            stats["cik_member_missing"] += 1
            continue
        try:
            data = json.loads(payload)
        except ValueError:
            stats["member_unreadable"] += 1
            continue
        raw = str((data.get("sic") if isinstance(data, dict) else None) or "").strip()
        if not raw:
            stats["sic_blank"] += 1
            continue
        try:
            sic = int(raw)
        except ValueError:
            stats["sic_invalid"] += 1
            continue
        if sic == 0:  # SEC's "0000" = no SIC assigned
            stats["sic_blank"] += 1
            continue
        if not SIC_MIN <= sic <= SIC_MAX:
            stats["sic_invalid"] += 1
            continue
        sics[cik] = sic
        stats["sic_read"] += 1
    return sics, stats


def _ensure_sic_leaves(store: DuckDBStore, sic_taxonomy_id: str, sics: Iterable[int]) -> dict[str, str]:
    """Create missing level-3 SIC leaves set-based; return SIC node_code -> node_id."""
    nodes = dict(store.con.execute(
        "SELECT node_code, node_id FROM taxonomy_node WHERE taxonomy_id = ?", [sic_taxonomy_id]
    ).fetchall())
    leaves = [
        (str(uuid.uuid5(uuid.NAMESPACE_DNS, f"node:{sic_taxonomy_id}:{sic:04d}")), sic_taxonomy_id,
         f"{sic:04d}", f"SIC {sic:04d}", nodes.get(f"{sic // 100:02d}"), 3, 0)
        for sic in sorted(set(sics)) if f"{sic:04d}" not in nodes
    ]
    if leaves:
        frame = pd.DataFrame(leaves, columns=["node_id", "taxonomy_id", "node_code", "node_label",
                                              "parent_node_id", "level", "sort_order"])
        store.con.register("_ec_sic_leaves", frame)
        try:
            store.con.execute("""
                INSERT OR IGNORE INTO taxonomy_node
                    (node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order)
                SELECT node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order
                FROM _ec_sic_leaves
            """)
        finally:
            store.con.unregister("_ec_sic_leaves")
        nodes = dict(store.con.execute(
            "SELECT node_code, node_id FROM taxonomy_node WHERE taxonomy_id = ?", [sic_taxonomy_id]
        ).fetchall())
    return nodes


def refresh_entity_classification_snapshot(
    store: DuckDBStore,
    *,
    snapshot: SicSnapshot,
    read_member: Callable[[str], bytes | None],
    run_id: str | None = None,
) -> dict[str, object]:
    """Classify every target id from one retained submissions snapshot (set-based, no network).

    Writes, per target whose CIK carries a SIC in the snapshot: the primary SIC row
    and derived FAMA_FRENCH_12, FAMA_FRENCH_49 (when French lists the SIC) and
    NAICS_2022 (partial, approximate) rows. Every new row has ``valid_from`` =
    ``as_of_date`` = the snapshot's receipt date and ``available_at`` = its receipt
    time; the ``source`` label carries ``classification_basis=current_sic_snapshot``,
    the archive hash prefix and, for derived rows, the mapping version -- also written to
    the 0328 ``classification_basis`` / ``mapping_version`` columns when the table has them
    (``mapping_version`` NULL on SIC rows), exactly as 0328 back-fills them from ``source``. Per
    (security, taxonomy, primary): the same open code is kept (idempotent rerun); a
    differing open interval is closed at the snapshot date and the new code opened;
    when the new SIC has no industry in a derived taxonomy (FF49-unlisted, no NAICS-2
    sector) the open derived interval is closed and nothing opened
    (``closed_no_mapping``); an open interval from a LATER snapshot (receipt time,
    then archive hash) is never overwritten (``stale_snapshot_skipped``). Nothing is
    backdated and no absent SIC is inferred.
    """
    SicTaxonomyDataset().load(store, SicTaxonomyOptions())
    FamaFrenchTaxonomyDataset().load(store, FamaFrenchTaxonomyOptions())
    NaicsTaxonomyDataset().load(store, NaicsTaxonomyOptions())
    taxonomy_ids = {code: _taxonomy_id_for(store, code) for code in ("SIC", *CLASSIFICATION_MAPPING_VERSIONS)}
    missing = sorted(code for code, taxonomy_id in taxonomy_ids.items() if taxonomy_id is None)
    if missing:
        raise RuntimeError(f"classification taxonomies not seeded: {missing}")

    targets = classification_targets(store)
    sics, sic_stats = read_snapshot_sic(read_member, targets["cik"].tolist())
    sic_nodes = _ensure_sic_leaves(store, str(taxonomy_ids["SIC"]), sics.values())
    derived_nodes = {
        code: dict(store.con.execute(
            "SELECT node_code, node_id FROM taxonomy_node WHERE taxonomy_id = ?", [taxonomy_ids[code]]
        ).fetchall())
        for code in CLASSIFICATION_MAPPING_VERSIONS
    }
    naics_for_sic2 = dict(SIC_TO_NAICS_PARTIAL)

    # Row-level provenance: the archive hash prefix rides in `source` (M8) and is the
    # deterministic tie-break between snapshots received at the same instant (M1).
    archive_tag = snapshot.sha256[:16]
    sic_source = f"{SOURCE_SIC_SNAPSHOT} archive:{archive_tag}"
    # (security_id, taxonomy_id, node_id, node_code, is_primary, source, mapping_version, close_only)
    rows: list[tuple[str, str, str | None, str | None, bool, str, str | None, bool]] = []
    unclassified: list[str] = []
    ff49_unlisted = naics_unmapped = 0
    for security_id, cik in zip(targets["security_id"], targets["cik"], strict=True):
        sic = sics.get(cik)
        if sic is None:
            unclassified.append(security_id)
            continue
        sic_code = f"{sic:04d}"
        rows.append((security_id, str(taxonomy_ids["SIC"]), sic_nodes[sic_code], sic_code, True, sic_source,
                     None, False))
        derived = {
            "FAMA_FRENCH_12": fama_french_12_for_sic(sic),
            "FAMA_FRENCH_49": fama_french_49_for_sic(sic),
            "NAICS_2022": naics_for_sic2.get(f"{sic // 100:02d}"),
        }
        ff49_unlisted += derived["FAMA_FRENCH_49"] is None
        naics_unmapped += derived["NAICS_2022"] is None
        for code, node_code in derived.items():
            version = CLASSIFICATION_MAPPING_VERSIONS[code]
            # A SIC with no industry in this taxonomy is a CLOSE-ONLY candidate: an open
            # row of that taxonomy (from an earlier SIC) is closed at the snapshot date and
            # nothing is opened -- a stale industry never outlives the SIC that implied it.
            rows.append((security_id, str(taxonomy_ids[code]),
                         None if node_code is None else derived_nodes[code][node_code], node_code, False,
                         f"{sic_source}; {version}", version, node_code is None))
    candidates = pd.DataFrame(rows, columns=["security_id", "taxonomy_id", "node_id", "node_code", "is_primary",
                                             "source", "mapping_version", "close_only"])
    snap = snapshot.valid_from
    con = store.con
    con.register("_ec_candidates_df", candidates)
    try:
        con.execute("""
            CREATE OR REPLACE TEMP TABLE _ec_candidates AS
            SELECT CAST(security_id AS VARCHAR) AS security_id, CAST(taxonomy_id AS VARCHAR) AS taxonomy_id,
                   CAST(node_id AS VARCHAR) AS node_id, CAST(node_code AS VARCHAR) AS node_code,
                   CAST(is_primary AS BOOLEAN) AS is_primary, CAST(source AS VARCHAR) AS source,
                   CAST(mapping_version AS VARCHAR) AS mapping_version, CAST(close_only AS BOOLEAN) AS close_only
            FROM _ec_candidates_df
        """)
    finally:
        con.unregister("_ec_candidates_df")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _ec_plan AS
        WITH open_rows AS (
            SELECT e.security_id, e.taxonomy_id, e.is_primary, e.node_code, e.valid_from,
                   coalesce(e.available_at, CAST(e.valid_from AS TIMESTAMP)) AS known_at,
                   regexp_extract(coalesce(e.source, ''), 'archive:([0-9a-f]{16})', 1) AS archive_tag
            FROM entity_classification e
            WHERE e.valid_to IS NULL AND e.security_id IN (SELECT security_id FROM _ec_candidates)
        ), flags AS (
            -- An open row from a LATER snapshot is never overwritten by this one. Snapshots
            -- are ordered by receipt time, then archive hash (deterministic same-instant
            -- tie-break), so out-of-order same-day reruns cannot leave a key with no open row.
            SELECT c.security_id, c.taxonomy_id, c.is_primary,
                   coalesce(bool_or(o.valid_from > ? OR o.known_at > ?
                                    OR (o.known_at = ? AND o.archive_tag > ?)), false) AS newer_open,
                   coalesce(bool_or(o.node_code = c.node_code), false) AS same_open
            FROM _ec_candidates c
            LEFT JOIN open_rows o
              ON o.security_id = c.security_id AND o.taxonomy_id = c.taxonomy_id AND o.is_primary = c.is_primary
            GROUP BY c.security_id, c.taxonomy_id, c.is_primary
        )
        SELECT c.*, f.newer_open, f.same_open,
               sha256(concat_ws('|', 'entity_classification', ?, c.security_id, c.taxonomy_id,
                                coalesce(c.node_code, ''), CAST(c.is_primary AS VARCHAR), c.source))
                   AS classification_id
        FROM _ec_candidates c
        JOIN flags f USING (security_id, taxonomy_id, is_primary)
    """, [snap, snapshot.received_at, snapshot.received_at, archive_tag, snapshot.sha256])
    close_counts = {
        code: (int(closed), int(same_day), int(no_mapping), int(other_version))
        for code, closed, same_day, no_mapping, other_version in con.execute("""
            SELECT t.code, count(*), count(*) FILTER (WHERE e.valid_from = ?),
                   count(*) FILTER (WHERE p.close_only),
                   count(*) FILTER (WHERE p.mapping_version IS NOT NULL
                                      AND NOT contains(coalesce(e.source, ''), p.mapping_version))
            FROM entity_classification e
            JOIN _ec_plan p ON p.security_id = e.security_id AND p.taxonomy_id = e.taxonomy_id
             AND p.is_primary = e.is_primary
            JOIN taxonomy t ON t.taxonomy_id = p.taxonomy_id
            WHERE NOT p.newer_open AND e.valid_to IS NULL AND e.node_code IS DISTINCT FROM p.node_code
            GROUP BY t.code
        """, [snap]).fetchall()
    }
    con.execute("""
        UPDATE entity_classification SET valid_to = ?
        FROM _ec_plan p
        WHERE entity_classification.security_id = p.security_id
          AND entity_classification.taxonomy_id = p.taxonomy_id
          AND entity_classification.is_primary = p.is_primary
          AND NOT p.newer_open
          AND entity_classification.valid_to IS NULL
          AND entity_classification.node_code IS DISTINCT FROM p.node_code
    """, [snap])
    plan_counts = {
        code: (int(stale), int(unchanged), int(inserted), int(collision))
        for code, stale, unchanged, inserted, collision in con.execute("""
            SELECT t.code,
                   count(*) FILTER (WHERE p.newer_open) AS stale,
                   count(*) FILTER (WHERE NOT p.newer_open AND p.same_open) AS unchanged,
                   count(*) FILTER (WHERE NOT p.newer_open AND NOT p.same_open
                                      AND e.classification_id IS NULL) AS inserted,
                   count(*) FILTER (WHERE NOT p.newer_open AND NOT p.same_open
                                      AND e.classification_id IS NOT NULL) AS id_collision
            FROM _ec_plan p JOIN taxonomy t ON t.taxonomy_id = p.taxonomy_id
            LEFT JOIN entity_classification e ON e.classification_id = p.classification_id
            WHERE NOT p.close_only
            GROUP BY t.code
        """).fetchall()
    }
    # 0328 lineage columns, written when present: the row's basis and, for derived rows, the
    # SIC-to-taxonomy mapping version (NULL for SIC rows). A pre-0328 table keeps its shape.
    lineage = _has_columns(store, "entity_classification", CLASSIFICATION_LINEAGE_COLUMNS)
    con.execute(f"""
        INSERT INTO entity_classification
            (classification_id, security_id, taxonomy_id, node_id, node_code, is_primary,
             valid_from, valid_to, as_of_date, available_at, run_id, source
             {", classification_basis, mapping_version" if lineage else ""})
        SELECT p.classification_id, p.security_id, p.taxonomy_id, p.node_id, p.node_code, p.is_primary,
               ?, NULL, ?, ?, ?, p.source {", ?, p.mapping_version" if lineage else ""}
        FROM _ec_plan p
        WHERE NOT p.close_only AND NOT p.newer_open AND NOT p.same_open
          AND NOT EXISTS (SELECT 1 FROM entity_classification e WHERE e.classification_id = p.classification_id)
    """, [snap, snap, snapshot.received_at, run_id,
          *((CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT,) if lineage else ())])
    open_counts = dict(con.execute("""
        SELECT t.code, count(DISTINCT e.security_id)
        FROM entity_classification e JOIN taxonomy t ON t.taxonomy_id = e.taxonomy_id
        WHERE e.valid_to IS NULL AND e.security_id IN (SELECT security_id FROM _ec_candidates)
        GROUP BY t.code
    """).fetchall())
    # Ids whose CIK carries no SIC in THIS snapshot keep any earlier open rows (absence is
    # not evidence of change); counted so a stale carry is visible (M7).
    open_without_current_sic = 0
    if unclassified:
        con.register("_ec_unclassified_df", pd.DataFrame({"security_id": unclassified}))
        try:
            row = con.execute("""
                SELECT count(DISTINCT e.security_id) FROM entity_classification e
                JOIN _ec_unclassified_df u ON u.security_id = e.security_id
                WHERE e.valid_to IS NULL
            """).fetchone()
        finally:
            con.unregister("_ec_unclassified_df")
        open_without_current_sic = int(row[0]) if row else 0
    con.execute("DROP TABLE IF EXISTS _ec_plan")
    con.execute("DROP TABLE IF EXISTS _ec_candidates")

    record_source_file(
        store,
        dataset_id="entity_classification",
        source_url=SEC_BULK_SUBMISSIONS_URL,
        cache_path=snapshot.path,
        status="cached",
        sha256=snapshot.sha256,
        metadata={"source_kind": "SEC nightly bulk submissions archive (retained)",
                  "classification_basis": CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT, **snapshot.as_detail()},
    )
    by_taxonomy: dict[str, dict[str, int]] = {}
    for code in sorted(set(plan_counts) | set(close_counts) | set(open_counts)):
        stale, unchanged, inserted, collision = plan_counts.get(code, (0, 0, 0, 0))
        closed, _, no_mapping, other_version = close_counts.get(code, (0, 0, 0, 0))
        by_taxonomy[code] = {
            "inserted": inserted, "unchanged": unchanged, "stale_snapshot_skipped": stale,
            "id_collision": collision, "closed": closed,
            # closed because the new SIC has no industry in this taxonomy (close-only candidate)
            "closed_no_mapping": no_mapping,
            # closed rows written under another mapping version (e.g. the pre-Siccodes12 FF12 table):
            # their earlier interval still carries that version's code (M3)
            "closed_other_mapping_version": other_version,
            "open_ids": int(open_counts.get(code, 0)),
        }
    closed = sum(item[0] for item in close_counts.values())
    superseded_same_day = sum(item[1] for item in close_counts.values())
    classified_ids = int(targets["cik"].isin(list(sics)).sum())
    return {
        "classification_basis": CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT,
        "basis_note": CLASSIFICATION_BASIS_NOTE,
        "source": sic_source,
        "snapshot": snapshot.as_detail(),
        "mapping_versions": dict(CLASSIFICATION_MAPPING_VERSIONS),
        "targets": {
            "ids": len(targets),
            "ciks": int(targets["cik"].nunique()),
            "from_identifier_history": int(targets["from_identifier_history"].sum()),
            "from_company_facts": int(targets["from_company_facts"].sum()),
            "cik_conflict": int(targets["cik_conflict"].sum()),
            # Company Facts owners (e.g. SEC-COMPANYFACTS-UNRESOLVED-CIK-*, delisted issuers) have no
            # `securities` row; orphan_entity_classification_security_ids accepts the unresolved-owner ids.
            "outside_securities": int(targets["outside_securities"].sum()),
            "classified_ids": classified_ids,
            "unclassified_ids": len(targets) - classified_ids,
        },
        "sic_read": {key: int(value) for key, value in sorted(sic_stats.items())},
        "ff49_unlisted_sic_ids": int(ff49_unlisted),
        "naics_unmapped_ids": int(naics_unmapped),
        "rows_inserted": sum(item["inserted"] for item in by_taxonomy.values()),
        "intervals_closed": closed,
        "superseded_same_day": superseded_same_day,
        "open_without_current_sic": open_without_current_sic,
        "by_taxonomy": by_taxonomy,
    }
