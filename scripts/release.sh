#!/bin/sh
# Releases OPM86 Player: sets the version, tags it v<version> and pushes both,
# and GitHub then builds the three apps and publishes them as a release.
# Usage: scripts/release.sh 0.2.0 (or make release VERSION=0.2.0)
set -e
version="$1"
tag="v$version"
fail() { echo "release: $1" >&2; exit 1; }

echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || fail "give the version as X.Y.Z, as in make release VERSION=0.2.0"
[ "$(git branch --show-current)" = main ] || fail "check out main first: git checkout main"
[ -z "$(git status --porcelain)" ] || fail "commit or stash the changes first"
git fetch -q origin main --tags
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || fail "main differs from GitHub's; run git pull first"
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && fail "$tag already exists"

# The version lives in CMakeLists.txt's project line; the app reads it from there.
current=$(sed -n 's/^project(OPM86 VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)
if [ "$current" != "$version" ]; then
    sed -i.bak "s/^project(OPM86 VERSION $current /project(OPM86 VERSION $version /" CMakeLists.txt && rm CMakeLists.txt.bak
    git commit -qm "Release $tag" CMakeLists.txt
fi
git tag -a "$tag" -m "OPM86 Player $tag"
git push -q origin main "$tag"
echo "pushed $tag: GitHub is building the release at https://github.com/xesco/opm86/actions"
