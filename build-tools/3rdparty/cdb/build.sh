#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
exec nix develop .#assets-cdb --command bash build-tools/3rdparty/cdb/_build.sh "$@"
