#!/usr/bin/env bash
# The staged runtime for a probe: stage.sh <branch> <out dir>. The newest CI
# run of the branch, waited for until its build has uploaded runtime-stage
# (the probe is pushed with the code it looks at, so that build has just
# started); "main" takes the newest run that has one.
set -euo pipefail
branch="$1" out="$2"
has_stage() {
  gh api "repos/$GITHUB_REPOSITORY/actions/runs/$1/artifacts" \
    -q '.artifacts[] | select(.name=="runtime-stage" and .expired==false) | .id' | grep -q .
}
run=""
if [ "$branch" != main ]; then
  for i in $(seq 1 80); do
    id=$(gh run list -R "$GITHUB_REPOSITORY" --workflow ci.yml --branch "$branch" --limit 1 --json databaseId -q '.[0].databaseId')
    if [ -n "$id" ] && has_stage "$id"; then run=$id; break; fi
    state=$(gh run view "$id" -R "$GITHUB_REPOSITORY" --json status,conclusion -q '.status + " " + (.conclusion // "")' 2>/dev/null || true)
    echo "waiting for run $id of $branch to upload runtime-stage ($state)"
    case "$state" in completed*) break ;; esac
    sleep 30
  done
fi
if [ -z "$run" ]; then
  for id in $(gh run list -R "$GITHUB_REPOSITORY" --workflow ci.yml --branch main --limit 10 --json databaseId -q '.[].databaseId'); do
    if has_stage "$id"; then run=$id; break; fi
  done
  echo "no build of $branch: main's"
fi
echo "runtime-stage of run $run ($(gh run view "$run" -R "$GITHUB_REPOSITORY" --json headSha,headBranch -q '.headBranch + " " + .headSha'))"
gh run download "$run" -R "$GITHUB_REPOSITORY" -n runtime-stage -D "$out"
