#!/usr/bin/env bash
# Build all uncached 3rdparty libs for linux-x86_64 (the host).
# Also picks up .noarch libs (built once, on linux only).
exec env TARGET_PLATFORM=linux-x86_64 "$(dirname "$0")/build-uncached.sh" "$@"
