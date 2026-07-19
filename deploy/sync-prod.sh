#!/usr/bin/env bash
# Publish the dev tree to the PUBLIC prod repo as ONE snapshot commit.
#
# The two repos share no git history, and the private dev history must never be
# pushed (it contains credentials that were only later moved into secrets.h).
# So this mirrors the TREE of a committed ref, not the history:
#   * clone prod, wipe its tracked files, unpack `git archive <ref>` of dev
#   * PROTECTED files keep prod's own copy (generic AGENTS.md / CLAUDE.md --
#     the dev versions carry real server details)
#   * GATE: abort before committing if any pattern from sync-prod.denylist
#     (hostnames, private paths, credentials) appears in the publish tree
#
# Usage:  deploy/sync-prod.sh                    # publish HEAD (committed state only)
#         SYNC_REF=origin/main deploy/sync-prod.sh
# Env:    PROD_REPO -- clone/push URL. CI passes a token URL; the default SSH
#         URL works for a locally authenticated user.
set -euo pipefail

PROD_REPO="${PROD_REPO:-git@github.com:cryox1/sensor-lab-prod.git}"
SYNC_REF="${SYNC_REF:-HEAD}"
PROTECTED=( AGENTS.md CLAUDE.md )            # prod keeps its own generic copies
NEVER_PUBLISH=( deploy/sync-prod.denylist )  # dev-only, never part of the mirror

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DENYLIST="${SCRIPT_DIR}/sync-prod.denylist"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

SRC_SHA="$(git -C "${PROJECT_DIR}" rev-parse --short "${SYNC_REF}")"
SRC_SUBJECT="$(git -C "${PROJECT_DIR}" log -1 --format=%s "${SYNC_REF}")"

echo "==> Cloning prod repo"
git clone --quiet --depth 1 "${PROD_REPO}" "${WORK}/prod"

echo "==> Replacing tree with dev @ ${SRC_SHA} (protected: ${PROTECTED[*]})"
git -C "${WORK}/prod" rm -rq .
git -C "${WORK}/prod" checkout HEAD -- "${PROTECTED[@]}"
TAR_EXCLUDES=()
for p in "${PROTECTED[@]}" "${NEVER_PUBLISH[@]}"; do
  TAR_EXCLUDES+=( "--exclude=${p}" )
done
git -C "${PROJECT_DIR}" archive "${SYNC_REF}" | tar -x -C "${WORK}/prod" "${TAR_EXCLUDES[@]}"

echo "==> Gate: scanning the publish tree for private patterns"
if grep -rniE -f "${DENYLIST}" --exclude-dir=.git "${WORK}/prod"; then
  echo "!! Denylisted pattern(s) found (matches above) -- ABORTED, nothing pushed." >&2
  exit 1
fi

cd "${WORK}/prod"
git add -A
if git diff --cached --quiet; then
  echo "==> Prod already matches dev @ ${SRC_SHA} -- nothing to push."
  exit 0
fi

NAME="$(git -C "${PROJECT_DIR}" config user.name 2>/dev/null || echo 'sensor-lab sync')"
EMAIL="$(git -C "${PROJECT_DIR}" config user.email 2>/dev/null || echo 'sync@users.noreply.github.com')"
git -c user.name="${NAME}" -c user.email="${EMAIL}" \
  commit --quiet -m "sync from dev @ ${SRC_SHA}: ${SRC_SUBJECT}"
git push --quiet origin HEAD:main

echo "==> Published dev @ ${SRC_SHA} to $(git remote get-url origin | sed 's#.*[:/]\([^/]*/[^/]*\)$#\1#' | sed 's/\.git$//')"
