"""The research field builder with the expansion-X data-lane field module registered (lane XDATA, draft).

The draft fields are registered here, never in ``prepare_research_fields.py``: the plain builder, every v8 field list
(fields v9 to v12) and every existing field's formula and producer fingerprint stay exactly as they are, so no v8 reuse
count can move. Run this file with the builder's own arguments (same argv, same manifest layout, the builder's own code
identity in the manifest; each draft field's entry names research_fields_xdata.py as ``producer``):

  python atx-engine/tools/prepare_research_fields_xdata.py --role ... --price-source <the role's TickerHistory3>
      --fields <the base list>,div_month_pred,beta_dvol_21,season_y2_5 --reuse <the base fields directory> ...

``FIELDS_XDATA_DRAFT`` lists the fields in registry order. ``register`` binds the module into a builder namespace exactly
as the builder binds its own modules (``bind`` appends its ``FIELDS`` to ``ALL_FIELDS``; the module object joins
``FIELD_MODULES``, whose hooks then check, reuse and compute its fields). At integration 8 this entry folds into the
FIELDS-V9 draft entry (``prepare_research_fields_draft.DRAFT_MODULES``, branch ``834d5a05``): one tuple element.
"""
from __future__ import annotations

import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_xdata                # same directory

DRAFT_MODULES = (research_fields_xdata,)
FIELDS_XDATA_DRAFT = ("div_month_pred", "beta_dvol_21", "season_y2_5")


def register(host_namespace: dict) -> list:
    """Bind every draft module into the builder namespace ``host_namespace`` (``vars(prepare_research_fields)``) and
    append it to its ``FIELD_MODULES``; a module already registered there is left as it is. Returns the bound module
    objects."""
    modules = host_namespace["FIELD_MODULES"]
    bound = []
    for module in DRAFT_MODULES:
        present = [m for m in modules if type(m).__module__ == module.__name__]
        if present:
            bound += present
            continue
        m = module.bind(host_namespace)
        modules.append(m)
        bound.append(m)
    return bound


def main(argv=None):
    register(vars(builder))
    builder.main(argv)


if __name__ == "__main__":
    main()
