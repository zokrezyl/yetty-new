#!/bin/bash
# YPaint Scrolling Layer: Clear
# Removes all drawings from the scrolling layer (OSC 666675)

printf '\033]666675;--clear;\033\\'
echo "YPaint scrolling layer cleared"
