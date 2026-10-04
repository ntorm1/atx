#pragma once

// The IC stage's append-only, SHA-256 hash-chained trial ledger, moved down to the engine
// (platform P9 B2, migration slice 8): atx/engine/research/ledger/trial_ledger.hpp holds the
// design notes, the chain rule and the code (library atx-engine-research-ledger). This thin
// header re-exports its names into atx::impl so the stages and tests that use them
// (stage_equity_ic.cpp, stage_equity_universe.cpp, trial_ledger_test.cpp) are unchanged.

#include "atx/engine/research/ledger/trial_ledger.hpp"

namespace atx::impl {

using atx::engine::research::ledger::kNonTrialPurposes;
using atx::engine::research::ledger::kTrialLedgerGenesisSha256;
using atx::engine::research::ledger::kTrialLedgerSchema;

using atx::engine::research::ledger::TrialLedgerBootstrap;
using atx::engine::research::ledger::TrialLedgerCost;
using atx::engine::research::ledger::TrialLedgerEntry;
using atx::engine::research::ledger::TrialLedgerFitBoundary;
using atx::engine::research::ledger::TrialLedgerHead;
using atx::engine::research::ledger::TrialLedgerParent;
using atx::engine::research::ledger::TrialLedgerRecipe;
using atx::engine::research::ledger::TrialLedgerResult;
using atx::engine::research::ledger::TrialLedgerRuntime;
using atx::engine::research::ledger::TrialLedgerSeal;
using atx::engine::research::ledger::TrialLedgerSignal;
using atx::engine::research::ledger::TrialLedgerSourceExclusions;
using atx::engine::research::ledger::TrialLedgerWindow;

using atx::engine::research::ledger::append_trial;
using atx::engine::research::ledger::declared_trials_for_checkpoint;
using atx::engine::research::ledger::is_non_trial_purpose;
using atx::engine::research::ledger::pre_registered_lines_for_checkpoint;
using atx::engine::research::ledger::serialize_trial_entry;
using atx::engine::research::ledger::trial_ledger_manifest_path;
using atx::engine::research::ledger::verify_trial_ledger;

} // namespace atx::impl
