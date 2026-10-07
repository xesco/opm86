#!/bin/sh
# Releases OPM86 Player: sets the version, adds the release to CHANGELOG.md,
# commits both, tags the commit v<version> and pushes it, and GitHub then builds
# the three apps and publishes them as a release.
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

# The changes since the last release: each pull request merged into main, by
# its title, and any commit made on main directly, by its subject.
last=$(git describe --tags --abbrev=0 --match 'v*' 2>/dev/null || true)
entries() {
    git log --first-parent --format='%H' ${last:+"$last"..}HEAD | while read -r commit; do
        subject=$(git log -1 --format='%s' "$commit")
        case "$subject" in
            "Merge pull request #"*)
                number=$(echo "$subject" | sed 's/^Merge pull request \(#[0-9]*\).*/\1/')
                echo "- $(git log -1 --format='%b' "$commit" | head -1) ($number)" ;;
            "Release v"* | "Initial commit") ;;
            *) echo "- $subject" ;;
        esac
    done
}
changes=$(entries)
[ -n "$changes" ] || fail "nothing has changed since $last"

# The version lives in CMakeLists.txt's project line; the app reads it from there.
current=$(sed -n 's/^project(OPM86 VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)
sed -i.bak "s/^project(OPM86 VERSION $current /project(OPM86 VERSION $version /" CMakeLists.txt && rm CMakeLists.txt.bak

# The new release goes first in the changelog, under its title.
{
    echo "# Changelog"
    echo
    echo "## $tag ($(date +%Y-%m-%d))"
    echo
    echo "$changes"
    [ -f CHANGELOG.md ] && sed '1{/^# Changelog$/d;}' CHANGELOG.md
} > CHANGELOG.md.new
mv CHANGELOG.md.new CHANGELOG.md

git add CMakeLists.txt CHANGELOG.md
git commit -qm "Release $tag"
git tag -a "$tag" -m "OPM86 Player $tag"
git push -q origin main "$tag"
echo "pushed $tag: GitHub is building the release at https://github.com/xesco/opm86/actions"
