#!/usr/bin/env bash
# Push a `lib-<libname>-<version>` tag for one 3rdparty library, where
# <version> is read from build-tools/3rdparty/<libname>/version.
#
# Pushing the tag triggers .github/workflows/build-3rdparty-<libname>.yml,
# which builds <libname> across every target platform and attaches the
# tarballs to a release of the same name.
#
# Usage:
#   build-tools/push-lib-tag.sh <libname>          # normal run
#   build-tools/push-lib-tag.sh <libname> -f       # force-move existing tag
#   build-tools/push-lib-tag.sh <libname> --remote upstream

set -euo pipefail

usage() {
    awk 'NR==1{next} /^[^#]/{exit} {sub(/^# ?/,""); print}' "$0"
}

LIBNAME=""
FORCE=0
REMOTE="origin"
while [ $# -gt 0 ]; do
    case "$1" in
        -f|--force) FORCE=1 ;;
        --remote)   shift; REMOTE="$1" ;;
        -h|--help)  usage; exit 0 ;;
        -*)         echo "unknown flag: $1" >&2; usage >&2; exit 2 ;;
        *)
            if [ -n "$LIBNAME" ]; then
                echo "extra positional arg: $1 (libname already=$LIBNAME)" >&2
                exit 2
            fi
            LIBNAME="$1"
            ;;
    esac
    shift
done

[ -n "$LIBNAME" ] || { echo "error: <libname> required" >&2; usage >&2; exit 2; }

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

LIB_DIR="$REPO_ROOT/build-tools/3rdparty/$LIBNAME"
VERSION_FILE="$LIB_DIR/version"
WORKFLOW_FILE="$REPO_ROOT/.github/workflows/build-3rdparty-${LIBNAME}.yml"

[ -d "$LIB_DIR" ] || { echo "no such lib dir: $LIB_DIR" >&2; exit 1; }
[ -f "$VERSION_FILE" ] || { echo "missing $VERSION_FILE" >&2; exit 1; }
[ -f "$WORKFLOW_FILE" ] || {
    echo "warning: $WORKFLOW_FILE not found — pushing the tag will not trigger any build" >&2
}

VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || { echo "$VERSION_FILE is empty" >&2; exit 1; }

TAG="lib-${LIBNAME}-${VERSION}"

# Refuse to create the tag if the target remote isn't configured.
if ! REMOTE_URL="$(git remote get-url "$REMOTE" 2>/dev/null)"; then
    echo "error: git remote '$REMOTE' is not configured." >&2
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
    git tag -a "$TAG" -m "${LIBNAME} ${VERSION}"
fi

# Remote tag check
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
echo "Done. The build-3rdparty-${LIBNAME} workflow should now be running:"
echo "  https://github.com/$(git remote get-url "$REMOTE" \
        | sed -E 's#.*[:/]([^/]+/[^/]+?)(\.git)?$#\1#')/actions"
