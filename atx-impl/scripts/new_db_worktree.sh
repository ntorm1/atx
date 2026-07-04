#!/usr/bin/env bash
#
# new_db_worktree.sh — worktree lifecycle helper for the atx-impl/db warehouse work
# (pf2 and successors). Each sprint is implemented in an isolated git worktree so the
# schema churn never touches the primary tree, then merged back into the integration
# mainline at sprint end.
#
# The 14 GB `atx_impl.duckdb` and the multi-GB `*.bak` files are git-ignored, so a fresh
# worktree does NOT copy them. Offline pytest builds in-memory / template DuckDBs, so the
# full `atx-impl/db/tests` suite runs in a worktree with no live DB. Live-DB smoke is
# operator-run against the shared DB path (see LIVE-SMOKE note below), never in the worktree.
#
# Usage:
#   scripts/new_db_worktree.sh new    <slug> [base]   # create branch + worktree off <base>
#   scripts/new_db_worktree.sh finish <slug> [base]   # merge branch into <base>, remove worktree
#   scripts/new_db_worktree.sh list                   # list db-work worktrees
#   scripts/new_db_worktree.sh help
#
# Examples:
#   scripts/new_db_worktree.sh new pf2-s1            # -> branch feat/pf2-s1 at C:/atx-wt/pf2-s1
#   scripts/new_db_worktree.sh finish pf2-s1         # merge feat/pf2-s1 back into the mainline
#
# Config (env overrides):
#   WT_ROOT        parent dir for worktrees        (default: <repo-parent>/atx-wt)
#   BRANCH_PREFIX  branch name prefix              (default: feat/)
#   BASE_BRANCH    base + merge-target branch      (default: current branch of the repo)
#
# "Merge back to local main" = merge the sprint branch back into BASE_BRANCH, i.e. the
# integration mainline you launched from (feat/warehouse-parity today — it carries pf1).
# Set BASE_BRANCH=main explicitly if you truly mean the literal `main` branch.

set -euo pipefail

# ----------------------------------------------------------------------------- helpers
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*" >&2; }

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || die "not inside a git repo"
CUR_BRANCH="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)"

WT_ROOT="${WT_ROOT:-$(dirname "$REPO_ROOT")/atx-wt}"
BRANCH_PREFIX="${BRANCH_PREFIX:-feat/}"
BASE_BRANCH="${BASE_BRANCH:-$CUR_BRANCH}"

validate_slug() {
  local slug="$1"
  [[ -n "$slug" ]] || die "missing <slug> (e.g. pf2-s1)"
  [[ "$slug" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "bad slug '$slug' (allowed: alnum . _ -)"
}

branch_exists()   { git -C "$REPO_ROOT" show-ref --verify --quiet "refs/heads/$1"; }
worktree_path_of() { # print the worktree path that has branch $1 checked out, if any
  git -C "$REPO_ROOT" worktree list --porcelain \
    | awk -v b="refs/heads/$1" 'BEGIN{p=""} /^worktree /{p=substr($0,10)} /^branch /{if($2==b){print p; exit}}'
}

# ----------------------------------------------------------------------------- commands
cmd_new() {
  local slug="${1:-}"; validate_slug "$slug"
  local base="${2:-$BASE_BRANCH}"
  local branch="${BRANCH_PREFIX}${slug}"
  local path="${WT_ROOT}/${slug}"

  git -C "$REPO_ROOT" show-ref --verify --quiet "refs/heads/$base" \
    || die "base branch '$base' does not exist"
  [[ -e "$path" ]]        && die "worktree path already exists: $path"
  branch_exists "$branch" && die "branch already exists: $branch (pick another slug or delete it)"

  mkdir -p "$WT_ROOT"
  info ">> creating worktree '$slug'"
  info "     branch : $branch  (off $base)"
  info "     path   : $path"
  git -C "$REPO_ROOT" worktree add -b "$branch" "$path" "$base"

  cat >&2 <<EOF

>> ready. next steps:
     cd "$path"
     # implement the sprint here (subagent-driven-development), TDD, offline tests:
     python -m pytest atx-impl/db/tests -q
     # commit within this worktree (stage explicit paths; never git add -A)

>> LIVE-SMOKE: the 14GB atx_impl.duckdb is git-ignored and absent here. Run operator
   live-DB smoke against the shared DB in the primary tree, e.g.:
     python atx-impl/scripts/<build_*>.py --db-path "$REPO_ROOT/atx-impl/db/atx_impl.duckdb"
   (back up before any live migration apply — see pf2 clause F / PF2-S2).

>> when the sprint is green + reviewed, from anywhere in the repo:
     scripts/new_db_worktree.sh finish "$slug" "$base"
EOF
}

cmd_finish() {
  local slug="${1:-}"; validate_slug "$slug"
  local base="${2:-$BASE_BRANCH}"
  local branch="${BRANCH_PREFIX}${slug}"
  local path="${WT_ROOT}/${slug}"

  [[ -d "$path" ]]        || die "no worktree at $path (already finished?)"
  branch_exists "$branch" || die "branch '$branch' not found"

  # the sprint worktree must be fully committed before we merge/remove it
  if [[ -n "$(git -C "$path" status --porcelain)" ]]; then
    die "worktree $path has uncommitted changes — commit or discard them first"
  fi

  # merge in whichever worktree currently has <base> checked out (usually the primary tree)
  local base_wt; base_wt="$(worktree_path_of "$base")"
  [[ -n "$base_wt" ]] || die "base branch '$base' is not checked out in any worktree; check it out, then rerun"
  if [[ -n "$(git -C "$base_wt" status --porcelain -- atx-impl/db 2>/dev/null)" ]]; then
    info "warning: '$base' tree ($base_wt) has local changes under atx-impl/db; merge may conflict"
  fi

  info ">> merging $branch -> $base  (in $base_wt)"
  git -C "$base_wt" merge --no-ff "$branch" -m "Merge $branch into $base"

  info ">> removing worktree $path"
  git -C "$REPO_ROOT" worktree remove "$path"

  cat >&2 <<EOF

>> done. $branch merged into $base and worktree removed.
   branch kept for history (delete with: git branch -d $branch).
   run the suite on the merged mainline before moving on:
     python -m pytest atx-impl/db/tests -q
EOF
}

cmd_list() {
  info ">> db-work worktrees under $WT_ROOT:"
  git -C "$REPO_ROOT" worktree list | grep -E "atx-wt/|$BRANCH_PREFIX" || info "   (none)"
}

usage() { sed -n '3,40p' "$0" >&2; }

# ----------------------------------------------------------------------------- dispatch
case "${1:-help}" in
  new)    shift; cmd_new    "$@" ;;
  finish) shift; cmd_finish "$@" ;;
  list)   shift; cmd_list   "$@" ;;
  help|-h|--help) usage ;;
  *) die "unknown command '${1:-}' (try: new | finish | list | help)" ;;
esac
