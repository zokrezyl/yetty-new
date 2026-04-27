#!/usr/bin/env bash
exec env TARGET_PLATFORM=webasm "$(dirname "$0")/build-uncached.sh" "$@"
