#!/bin/bash
# Demo: ypaint text rendering via YAML

# YAML document with text primitive
YAML='body:
  - text:
      position: [50, 50]
      content: "Hello YPaint!"
      font-size: 48
      color: "#ff0000"
  - text:
      position: [50, 100]
      content: "This is MSDF text rendering"
      font-size: 16
      color: "#00ff00"
  - text:
      position: [50, 130]
      content: "Multiple lines of text"
      font-size: 14
      color: "#0000ff"
'

# Base64 encode the YAML
PAYLOAD=$(echo -n "$YAML" | base64 -w0)

# Send via OSC 666674 (ypaint scroll mode)
# Format: ESC ] 666674 ; --yaml ; <base64_payload> BEL
printf '\033]666674;--yaml;%s\007' "$PAYLOAD"
