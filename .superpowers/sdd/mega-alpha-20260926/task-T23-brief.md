# T23 — prior-sign admission v4-prior-v1 + composition ew-theme-v1 in the fitter (S-M)
Spec: C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/v4-prereg.md §R3 and §R4 exactly (s_k=+1, no flips; reject <250 finite TRAIN days, tau>0.70, veto HAC t<-2.0 of the
full-TRAIN mean f_k (declare the HAC lag rule in provenance: Newey-West, lag = 5), greedy |rho|<=0.90 ordered by
(tier, roster order); weights 1/(themes present) split equally over admitted members of each theme). Theme and
tier come from the library recipe (T24 adds theme/tier/prior_sign/citation per candidate; accept them via the
library JSON or recipe — read the v3 library JSON format to design it; coordinate by field names 'theme',
'tier', 'prior_sign'). File atx-impl/tools/fit_composition_weights.py (+ tests). Default behaviour and existing
cache keys byte-identical (the fitter hashes its own source into its work cache — T16 found any edit re-keys the
cache: acceptable ONLY if the v3 frozen pins are still reproducible from git history; state the consequence).
Weights JSON carries pinned signs (+1) so the runner uses pinned-candidate-signs.
