// The IC stage's trial ledger moved down to atx-engine-research-ledger (platform P9 B2,
// migration slice 8): atx-engine/src/research/ledger/trial_ledger.cpp. This translation unit
// stays in atx-impl-core's source list so the list (another lane's file) is not edited; it
// only checks that the thin re-export header compiles on its own.
#include "trial_ledger.hpp"
