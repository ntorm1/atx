"""The research field builder with the draft field modules registered (lane FIELDS-V9, ruling PM5-13).

Draft field lists are registered here, never in ``prepare_research_fields.py``: the plain builder, every v8 field list
(fields v9 to v12, built before the v8 freeze gate) and every existing field's formula and producer fingerprint stay
exactly as they are, so the reuse counts of the v8 builds cannot move. Run this file with the builder's own arguments
(same argv, same manifest layout, the builder's own code identity in the manifest; each draft field's entry names its
module as ``producer``):

  python atx-engine/tools/prepare_research_fields_draft.py --role ...
      --fields <fields v12>,nt_first_126,earn_season_rank --reuse <the fields v12 directory> ...

* ``FIELDS_V13_DRAFT``: fields v13 (draft) = fields v12 + these, in this order. ``nt_first_126`` (library v9 draft C-3
  ``nt_late``) and ``earn_season_rank`` (C-4 ``earn_season``) of research_fields_v9.py; uncompiled DSL, unrun on data.

``register`` binds each draft module into a builder namespace exactly as the builder binds its own modules (``bind``
appends the module's ``FIELDS`` to ``ALL_FIELDS`` after every field registered before; the module object joins
``FIELD_MODULES``, whose hooks then check, reuse and compute its fields). Promoting a draft list to a real version is
the builder hook of research_fields_v9.py's docstring, made after the gate that freezes the previous version.
"""
from __future__ import annotations

import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_v9                  # same directory

DRAFT_VERSION = "v13"
DRAFT_MODULES = (research_fields_v9,)
FIELDS_V13_DRAFT = ("nt_first_126", "earn_season_rank")


def register(host_namespace: dict) -> list:
    """Bind every draft module into the builder namespace ``host_namespace`` (``vars(prepare_research_fields)``) and
    append it to its ``FIELD_MODULES``; a module already registered there is left as it is. Returns the bound
    module objects."""
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
