#!/bin/bash
# Print all shader glyphs with their names. Mirrors the first screen of
# demo.sh without the demo machinery.
DIR="$(cd "$(dirname "$0")" && pwd)"
cat "$DIR/files/glyphs.txt"
