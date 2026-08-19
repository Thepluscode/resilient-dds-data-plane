#!/usr/bin/env bash
# Merge gate, keyed to the pull request's CURRENT head commit.
#
#   ./scripts/merge_gate.sh <pr-number> <branch>
#
# Why this is not just `gh pr merge --auto`:
#
#   * --auto silently degrades to an IMMEDIATE merge when a repository has
#     auto-merge disabled. No warning, no non-zero exit. A flag whose entire
#     purpose is a precondition is worthless if an unsupported configuration
#     makes it a no-op rather than an error.
#
#   * `gh pr checks` reports checks across commits. A gate built on it can act
#     on a run belonging to a previous push. That is merely annoying when the
#     stale run failed; inverted, a stale SUCCESS would admit a broken head.
#     Everything below filters on headSha.
#
# A gate is only valid if it proves the exact state it claims to gate.
set -u

PR=${1:?usage: merge_gate.sh <pr-number> <branch>}
BRANCH=${2:?usage: merge_gate.sh <pr-number> <branch>}
REPO=${REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}
POLL_SECONDS=${POLL_SECONDS:-30}
MAX_POLLS=${MAX_POLLS:-80}

HEAD=$(gh pr view "$PR" --repo "$REPO" --json headRefOid --jq .headRefOid)
[[ -n "$HEAD" ]] || { echo "ABORT: could not resolve head for PR#$PR" >&2; exit 1; }
echo "gating PR#$PR at head ${HEAD:0:7}"

runs_for_head() {
    gh run list --branch "$BRANCH" --limit 25 \
        --json headSha,name,status,conclusion 2>/dev/null \
        | jq -c --arg s "$HEAD" '[.[] | select(.headSha == $s)]'
}

rs='[]'
for _ in $(seq 1 "$MAX_POLLS"); do
    rs=$(runs_for_head)
    # length > 0 matters: "no runs yet" must not read as "nothing failed".
    [[ "$(jq -r 'length > 0 and all(.status == "completed")' <<<"$rs")" == "true" ]] && break
    sleep "$POLL_SECONDS"
done

echo "--- runs for ${HEAD:0:7} ---"
jq -r '.[] | "  \(.name): \(.conclusion // .status)"' <<<"$rs" | sort -u

if [[ "$(jq -r 'length' <<<"$rs")" == "0" ]]; then
    echo "ABORT: no workflow runs attached to this head - failing closed" >&2
    exit 1
fi
if [[ "$(jq -r 'all(.status == "completed")' <<<"$rs")" != "true" ]]; then
    echo "ABORT: runs still in progress after $((POLL_SECONDS * MAX_POLLS))s" >&2
    exit 1
fi
# The workflow triggers on both push and pull_request, so one commit legitimately
# has two runs per check name. Both must succeed -- do not dedup by name here or
# you may keep the wrong one.
if [[ "$(jq -r 'all(.conclusion == "success")' <<<"$rs")" != "true" ]]; then
    echo "ABORT: a run for this head is not success - not merging" >&2
    exit 1
fi

gh pr ready "$PR" --repo "$REPO" 2>&1 | tail -1
gh pr merge "$PR" --repo "$REPO" --merge 2>&1 | tail -2

git fetch origin -q
echo "main is now: $(git log --oneline -1 origin/main)"

# Delete the branch only on MERGED plus containment. CLOSED is not MERGED, and
# deleting then would destroy the only copy of the work.
state=$(gh pr view "$PR" --repo "$REPO" --json state --jq .state)
if [[ "$state" == "MERGED" ]] && git merge-base --is-ancestor "origin/$BRANCH" origin/main; then
    echo "verified: $BRANCH tip is an ancestor of main"
    git push origin --delete "$BRANCH" 2>&1 | tail -1
    git remote prune origin >/dev/null 2>&1
    git switch -q main && git pull --ff-only -q origin main
    git branch -D "$BRANCH" >/dev/null 2>&1
else
    echo "keeping $BRANCH (PR state=$state)"
fi
echo "local: $(git branch --show-current) $(git log --oneline -1)"
