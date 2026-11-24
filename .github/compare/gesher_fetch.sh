#!/usr/bin/env bash
# the other runtime's installer as its users get it: other_fetch.sh <tag|latest> <out dir>.
# The release's other-setup-<version>.exe and SHA256SUMS.txt (the other runtime's publish-release.yml
# publishes exactly these two, tools/ci/publish_release.py), checked against each other.
# With no public release (or "ci" asked for), the installer the other runtime's own CI built and its e2e job
# tested: the other-setup artifact (the same two files) of the newest successful ci.yml run on
# main. Nothing of the other runtime is built here and nothing is written to its repository.
set -euo pipefail
want="$1" out="$2"
repo=other-runtime
mkdir -p "$out"
source=""
if [ "$want" != ci ]; then
  tag="$want"
  if [ "$tag" = latest ]; then
    # gh's "latest" is GitHub's: the newest release that is neither a draft nor a pre-release.
    tag=$(gh release view -R "$repo" --json tagName -q .tagName 2>/dev/null || true)
  fi
  if [ -n "$tag" ] && gh release download "$tag" -R "$repo" -p 'other-setup-*.exe' -p SHA256SUMS.txt -D "$out" --clobber; then
    source="release $tag"
  else
    echo "no public the other runtime release ${tag:-(none)}: taking the installer of the other runtime's newest successful CI run on main"
  fi
fi
if [ -z "$source" ]; then
  for id in $(gh run list -R "$repo" --workflow ci.yml --branch main --status success --limit 20 --json databaseId -q '.[].databaseId'); do
    if gh run download "$id" -R "$repo" -n other-setup -D "$out" 2>/dev/null; then
      source="ci run $id ($(gh run view "$id" -R "$repo" --json headSha -q .headSha))"
      break
    fi
  done
fi
[ -n "$source" ] || { echo "no the other runtime installer found (no release, no CI artifact)"; exit 1; }
cd "$out"
ls other-setup-*.exe >/dev/null
tr -d '\r' < SHA256SUMS.txt | sha256sum -c -  # written on Windows: CRLF lines
setup=$(ls other-setup-*.exe)
echo "the other runtime: $setup from $source" | tee other-source.txt
