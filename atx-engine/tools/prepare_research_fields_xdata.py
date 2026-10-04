"""The research field builder with the expansion-X data-lane field module registered (lane XDATA, draft).

DEPRECATED (P9 lane A1, DEC-5): use the one entry ``prepare_research_fields.py --registry field_registry.json --fields
<list|all>``. ``register`` and ``main`` are thin wrappers over it (``field_registry``) and stay for existing callers.

The draft fields are registered here, never in ``prepare_research_fields.py``: the plain builder, every v8 field list
(fields v9 to v12) and every existing field's formula and producer fingerprint stay exactly as they are, so no v8 reuse
count can move. Run this file with the builder's own arguments (same argv, same manifest layout, the builder's own code
identity in the manifest; each draft field's entry names research_fields_xdata.py as ``producer``):

  python atx-engine/tools/prepare_research_fields_xdata.py --role ... --price-source <the role's TickerHistory3>
      --fields <the base list>,div_month_pred,beta_dvol_21,season_y2_5 --reuse <the base fields directory> ...

``FIELDS_XDATA_DRAFT`` lists the TickerHistory3 fields in registry order; ``FIELDS_GOLD_DRAFT`` the sealed gold-panel
reader's fields (research_fields_gold.py; they also need ``--gold-panel-root`` and the stage manifest pins). ``register`` binds the module into a builder namespace exactly
as the builder binds its own modules (``bind`` appends its ``FIELDS`` to ``ALL_FIELDS``; the module object joins
``FIELD_MODULES``, whose hooks then check, reuse and compute its fields). At integration 8 this entry folds into the
FIELDS-V9 draft entry (``prepare_research_fields_draft.DRAFT_MODULES``, branch ``834d5a05``): one tuple element.
"""
from __future__ import annotations

import field_registry                      # same directory: the registry entry these wrappers call (K-P9-1)
import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_gold                # same directory (task GOLD, Ruling PM7-19)
import research_fields_xdata                # same directory

DRAFT_MODULES = (research_fields_xdata, research_fields_gold)
FIELDS_XDATA_DRAFT = ("div_month_pred", "beta_dvol_21", "season_y2_5")
FIELDS_GOLD_DRAFT = ("gp_hl_spread_21", "gp_iv_term_slope")   # need --gold-panel-root and the stage pins


def register(host_namespace: dict) -> list:
    """Deprecated (P9 A1): ``field_registry.bind_modules`` of ``DRAFT_MODULES``. Binds every draft module into the
    builder namespace ``host_namespace`` (``vars(prepare_research_fields)``) and appends it to its ``FIELD_MODULES``; a
    module already registered there is left as it is. Returns the bound module objects."""
    return field_registry.bind_modules(host_namespace, DRAFT_MODULES)


def main(argv=None):
    """Deprecated (P9 A1): ``register`` then the builder's own ``main`` with this file's argv (manifest bytes as before).
    The replacement is ``prepare_research_fields.py --registry field_registry.json`` with the same argv."""
    register(vars(builder))
    builder.main(argv)


if __name__ == "__main__":
    main()
