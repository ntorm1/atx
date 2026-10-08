"""ABS: reflow the runner hunks of edit_runner.py to 100 columns."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from abs_edit import File  # noqa: E402

r = File("C:/atx-wt/pool-28/atx-impl/src/strategy_ic_runner.cpp")
r.once("""    const std::span<const u8> absent_mask=absent?std::span<const u8>(absent->absent):std::span<const u8>{};
""", """    const std::span<const u8> absent_mask=
        absent?std::span<const u8>(absent->absent):std::span<const u8>{};
""")
r.once("""    // Present only with --absent-members (P9 lane ABS): the list the pass applied. Not a method input
    // either: the recipe and every cache key are those of the run on the members' data removed.
""", """    // Present only with --absent-members (P9 lane ABS): the list the pass applied. Not a method
    // input either: the recipe and every cache key are those of the run on the data removed.
""")
r.once("""  // absent-members-v1 (P9 lane ABS): each of its two flags at most once ([0] the list, [1] its pin).
""", """  // absent-members-v1 (P9 lane ABS): each of its two flags at most once ([0] list, [1] pin).
""")
r.once("""               "  --absent-members: absent-members-v1, an atx.od3-flagged-members/v1 list ({schema, members:\\n"
               "    [id, ...]}; other keys ignored); each listed candidate is treated on every date as a\\n"
               "    member with no data (its mass stays in its theme; weights, signs and verify unchanged);\\n"
               "    summary.json records absent_members. Each flag at most once; refuses --no-composition.\\n"
""", """               "  --absent-members: absent-members-v1, an atx.od3-flagged-members/v1 list\\n"
               "    ({schema, members: [id, ...]}, other keys ignored): each listed candidate is\\n"
               "    treated on every date as a member with no data (its mass stays in its theme;\\n"
               "    weights, signs and verify unchanged); summary.json records absent_members.\\n"
               "    Each flag at most once; refused with --no-composition.\\n"
""")
r.save()
print("ok")
