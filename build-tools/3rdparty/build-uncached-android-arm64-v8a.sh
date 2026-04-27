#!/usr/bin/env bash
exec env TARGET_PLATFORM=android-arm64-v8a "$(dirname "$0")/build-uncached.sh" "$@"
