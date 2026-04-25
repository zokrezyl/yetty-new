#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
exec nix develop .#assets-riscv --command bash build-tools/assets/alpine-disk/_build.sh "$@"
