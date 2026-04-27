#!/bin/bash
# Single-glyph diagnostic.
#
# Prints ONE codepoint and sleeps. Visual pass criteria:
#   plasma  -> rainbow gradient at top-left, animated, slowly cycling
#   spinner -> cyan ring with three rotating blobs
#   heart   -> red pulsing heart (so "pink" is correct here)
#   star    -> golden glowing star
#
# Usage:  ./single.sh [name]   # default: plasma
#
# Run inside yetty:
#   ./build-desktop-ytrace-release/yetty -e demo/scripts/shader-glyph/single.sh
#   ./build-desktop-ytrace-release/yetty -e 'demo/scripts/shader-glyph/single.sh spinner'

declare -A cp_for=(
    [spinner]='\xee\x80\x80'
    [pulse]='\xee\x80\x81'
    [plasma]='\xee\x80\x87'
    [heart]='\xee\x80\x85'
    [fire]='\xee\x80\x83'
    [star]='\xee\x80\x8c'
    [mandelbrot]='\xee\x83\xbd'
    [biomine]='\xee\x83\xbb'
)

name="${1:-plasma}"
cp="${cp_for[$name]:-${cp_for[plasma]}}"

clear
printf 'glyph: %s  (cp=%s)\n\n' "$name" "$cp"
# A small block so it's hard to miss
for r in 1 2 3 4; do
    for c in $(seq 1 20); do
        printf "$cp"
    done
    printf '\n'
done
echo
echo 'sleeping 30s — Ctrl+C to exit'
sleep 30
