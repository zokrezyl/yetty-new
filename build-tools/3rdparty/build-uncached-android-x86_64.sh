#!/usr/bin/env bash
exec env TARGET_PLATFORM=android-x86_64 "$(dirname "$0")/build-uncached.sh" "$@"
