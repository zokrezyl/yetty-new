#!/usr/bin/env bash
# Set the iOS / tvOS Simulator's hardware-keyboard layout.
#
# Why: the Simulator's "Hardware Keyboard" defaults to U.S. QWERTY regardless
# of the host's keyboard layout. iOS reads HID scan codes from the host and
# translates them through ITS layout — so a Dvorak-on-host user pressing
# the physical T key gets `t` in yetty (US-QWERTY translation), not `k`
# (Dvorak translation).
#
# Yetty's pressesBegan: reads UIKey.characters, which is post-layout — once
# the Simulator's layout is right, yetty does the right thing.
#
# Usage:
#   tools/sim-keyboard.sh [layout] [device]
#     layout  one of: Dvorak | DvorakLeft | DvorakRight | QWERTY (default Dvorak)
#     device  Simulator device name or UDID (default: "yetty-simulator")
#
#   Examples:
#     tools/sim-keyboard.sh                            # Dvorak on yetty-simulator
#     tools/sim-keyboard.sh QWERTY                    # back to US QWERTY
#     tools/sim-keyboard.sh Dvorak "iPhone 16"
#     tools/sim-keyboard.sh Dvorak "Apple TV 4K (3rd generation) (at 1080p)"

set -euo pipefail

LAYOUT="${1:-Dvorak}"
DEVICE="${2:-yetty-simulator}"

case "$LAYOUT" in
    Dvorak|DvorakLeft|DvorakRight|QWERTY) ;;
    *)
        echo "error: unknown layout '$LAYOUT' (use Dvorak, DvorakLeft, DvorakRight, or QWERTY)" >&2
        exit 2
        ;;
esac

# Resolve to UDID. simctl accepts names but defaults write needs a stable target.
UDID="$(xcrun simctl list devices -j 2>/dev/null \
    | python3 -c "
import sys, json
d = json.load(sys.stdin)
target = '$DEVICE'
for runtime, devs in d['devices'].items():
    for dev in devs:
        if dev.get('name') == target or dev.get('udid') == target:
            print(dev['udid']); sys.exit(0)
sys.exit(1)
" )" || { echo "error: device '$DEVICE' not found" >&2; exit 1; }

echo "device: $DEVICE ($UDID)"
echo "layout: en_US@hw=$LAYOUT;sw=$LAYOUT"

# Boot if not running — defaults write needs a live device for some keys.
STATE="$(xcrun simctl list devices -j | python3 -c "
import sys, json
d = json.load(sys.stdin)
udid = '$UDID'
for runtime, devs in d['devices'].items():
    for dev in devs:
        if dev['udid'] == udid:
            print(dev['state']); sys.exit(0)
" )"

if [ "$STATE" != "Booted" ]; then
    echo "booting $DEVICE..."
    xcrun simctl boot "$UDID"
    sleep 3
fi

# Two preference keys: AppleKeyboards (the enabled list) +
# AppleKeyboardsExpanded (which Settings.app reads). Set both.
PLIST="$HOME/Library/Developer/CoreSimulator/Devices/$UDID/data/Library/Preferences/.GlobalPreferences.plist"

xcrun simctl spawn "$UDID" defaults write -g AppleKeyboards -array "en_US@hw=$LAYOUT;sw=$LAYOUT"
xcrun simctl spawn "$UDID" defaults write -g AppleKeyboardsExpanded -dict-add "en_US@hw=$LAYOUT;sw=$LAYOUT" 1
xcrun simctl spawn "$UDID" defaults write -g KeyboardLastUsedKeyboards -array "en_US@hw=$LAYOUT;sw=$LAYOUT"
xcrun simctl spawn "$UDID" defaults write -g KeyboardLastUsedKeyboard "en_US@hw=$LAYOUT;sw=$LAYOUT"

# Restart the device so iOS re-reads the keyboard prefs at next launch.
echo "restarting device so iOS picks up the new layout..."
xcrun simctl shutdown "$UDID"
xcrun simctl boot "$UDID"

echo
echo "done. Connect Hardware Keyboard must be ON in the simulator menubar"
echo "(I/O > Keyboard > Connect Hardware Keyboard, or ⌘K)."
echo
echo "verify with:"
echo "  plutil -p '$PLIST' | grep -E 'AppleKeyboards|KeyboardLastUsed'"
