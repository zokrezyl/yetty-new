#!/usr/bin/env bash
# Create the `assets-<version>` tag (version read from assets.version) and
# push it to origin. Pushing the tag triggers .github/workflows/build-assets.yml,
# which builds every asset and attaches the tarballs to the matching GitHub
# release.
#
# Usage:
#   build-tools/push-assets-tag.sh            # normal run
#   build-tools/push-assets-tag.sh -f         # force-move an existing tag
#   build-tools/push-assets-tag.sh --remote upstream   # push to non-default remote

set -euo pipefail

FORCE=0
REMOTE="origin"
while [ $# -gt 0 ]; do
    case "$1" in
        -f|--force) FORCE=1 ;;
        --remote)   shift; REMOTE="$1" ;;
        -h|--help)
            awk 'NR==1{next} /^[^#]/{exit} {sub(/^# ?/,""); print}' "$0"
            exit 0
            ;;
        *)  echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

VERSION_FILE="$REPO_ROOT/assets.version"
[ -f "$VERSION_FILE" ] || { echo "missing $VERSION_FILE" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || { echo "$VERSION_FILE is empty" >&2; exit 1; }

TAG="assets-$VERSION"

# Refuse to create the tag if the target remote isn't configured —
# otherwise we'd cut a local tag we can't push and the caller would
# have to clean it up manually.
if ! REMOTE_URL="$(git remote get-url "$REMOTE" 2>/dev/null)"; then
    echo "error: git remote '$REMOTE' is not configured in this repo." >&2
    echo "       set it with:  git remote add $REMOTE <url>" >&2
    echo "       or pass --remote <name> to use a different one." >&2
    echo "       configured remotes: $(git remote | paste -sd, -)" >&2
    exit 1
fi
echo "target remote: $REMOTE -> $REMOTE_URL"

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "error: working tree has uncommitted changes — commit or stash them first." >&2
    git status --short >&2
    exit 1
fi

# Local tag check
if git rev-parse --verify "refs/tags/$TAG" >/dev/null 2>&1; then
    if [ "$FORCE" -eq 1 ]; then
        echo "moving local tag $TAG -> $(git rev-parse --short HEAD)"
        git tag -f "$TAG"
    else
        echo "tag $TAG already exists locally (use -f to move it)" >&2
        exit 1
    fi
else
    echo "creating tag $TAG"
    git tag -a "$TAG" -m "Asset release $VERSION"
fi

# Remote tag check (informational only)
if git ls-remote --tags --exit-code "$REMOTE" "refs/tags/$TAG" >/dev/null 2>&1; then
    if [ "$FORCE" -eq 1 ]; then
        echo "force-pushing $TAG to $REMOTE"
        git push --force "$REMOTE" "refs/tags/$TAG"
    else
        echo "tag $TAG already exists on $REMOTE (use -f to overwrite)" >&2
        exit 1
    fi
else
    echo "pushing $TAG to $REMOTE"
    git push "$REMOTE" "refs/tags/$TAG"
fi

echo ""
echo "Done. The build-assets workflow should now be running:"
echo "  https://github.com/$(git remote get-url "$REMOTE" \
        | sed -E 's#.*[:/]([^/]+/[^/]+?)(\.git)?$#\1#')/actions"
