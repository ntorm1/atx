"""T36: hand-derive the aim-partial-v5 FIXED-rate NAV recipe.json bytes that the T30
writers (98277c45) produce for strategy_nav_replay_test.cpp publication_panel() +
write_artifact() under aim_partial_nav(0.5, 0.1, 1.5), reusing the T30 emulation
(t30-sha/fixture_sha.py, byte-exact on 25 committed C++ outputs and on the T30 pins).

usage: python v5_fixed_recipe_sha.py <t30-sha dir> <worktree root> <T30 target-replay cpp>
The third argument is the T30 strategy_target_replay.cpp (e.g. `git show
98277c45:atx-impl/src/strategy_target_replay.cpp > t30_target.cpp`): the aim_partial
text is regex-extracted from its construction_recipe()."""
import re
import sys

t30_dir, root, target_cpp = sys.argv[1], sys.argv[2], sys.argv[3]
sys.argv = [sys.argv[0], t30_dir, root]
sys.path.insert(0, t30_dir)
import fixture_sha as base  # noqa: E402  (runs the T30 derivation, defines base.nav)
import nljson  # noqa: E402

BS = chr(92)
src = open(target_cpp, encoding='utf-8').read()
m = re.search(r'j\["aim_partial"\] =\s*((?:"(?:[^"' + BS * 2 + r']|' + BS * 2 + r'.)*"\s*)+);',
              src)
if not m:
    raise KeyError('aim_partial')
aim_text = ''.join(re.findall(r'"((?:[^"' + BS * 2 + r']|' + BS * 2 + r'.)*)"', m.group(1)))
if BS in aim_text:
    raise ValueError('escape in aim_partial')
m = re.search(r'const char\* aim_rate\(const TargetReplayConfig&\) \{ return "([a-z-]+)"; \}', src)
if not m:
    raise KeyError('aim_rate')
rate = m.group(1)

nav = dict(base.nav)
nav.update({'rule': 'aim-partial-v5', 'cadence': 1, 'trade_fraction': 0.5,
            'theta': 0.5, 'dust_multiple': 0.1, 'aim_leverage': 1.5, 'rate': rate,
            'aim_partial': aim_text})
print('v5_fixed_rate', rate)
print('v5_fixed_nav_recipe_keys', len(nav))
print('v5_fixed_nav_recipe_file_sha256', base.sha(base.json_file(nav)))
