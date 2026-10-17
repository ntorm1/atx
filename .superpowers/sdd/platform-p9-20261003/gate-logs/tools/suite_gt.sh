#!/usr/bin/env bash
# GATE step 2 (plan 4.2 "suites"; root-gate-brief step 2): the whole gtest targets of one build, one after another,
# each through tools/gt.sh (one log per target, one summary line per run in gate-logs/gt-exits.txt). The engine risk
# group runs `Risk*` minus the 31-minute dense oracle (`RiskQpAugment.MatchesDenseOracleAcrossBattery`), as M2i did;
# `RiskCovContainer.*` is inside `Risk*`. Every other target runs whole (filter '*').
# usage: suite_gt.sh rel|dbg [TARGET ...]   (no TARGET: the full list below)
set -u
build=$1; shift
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/gate-logs
tools=$logs/tools
if [ "$build" = "rel" ]; then bin=$root/build-equity-rel/bin; else bin=$root/build-equity/bin; fi
all="atx-impl-strategy-ic-tests atx-impl-strategy-target-tests atx-impl-strategy-tests atx-impl-strategy-mine-tests
atx-impl-tests atx-engine-alpha-tests atx-engine-factory-tests atx-engine-book-tests atx-engine-risk-tests
atx-engine-research-fields-tests atx-engine-research-admission-tests atx-engine-research-store-tests
atx-engine-research-catalog-tests atx-engine-research-composition-tests atx-engine-research-ledger-tests
atx-engine-research-eval-tests"
targets=${*:-$all}
fails=0
for t in $targets; do
  filter='*'
  [ "$t" = "atx-engine-risk-tests" ] && filter='Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery'
  bash "$tools/gt.sh" "$build-$t" "$bin/$t.exe" "$logs/gt-$build-$t.log" "$filter" || fails=$((fails + 1))
done
echo "suite_gt $build done: targets with nonzero exit $fails"
exit $fails
