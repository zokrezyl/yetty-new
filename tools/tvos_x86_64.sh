#!/bin/bash
#
# Run Yetty tvOS x86_64 build in the Apple TV Simulator
# Supports: macOS only
#
# Usage:
#   First build: make build-tvos_x86_64-ytrace-release (or -debug)
#   Then run:
#     ./tools/tvos_x86_64.sh             # Start simulator with yetty
#     ./tools/tvos_x86_64.sh --kill      # Kill running simulator
#     ./tools/tvos_x86_64.sh --list      # List available simulators
#     ./tools/tvos_x86_64.sh --help      # Show full help

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Configuration
# Apple TV 4K (3rd gen) at 4K is heavy on Intel Macs (SimRenderServer +
# SimMetalHost burn CPU). The 1080p variant is the same device family but
# at 1920x1080 — far more responsive on Intel hosts. Override with
# DEVICE_TYPE_NAME="Apple TV 4K (3rd generation)" if you really want 4K.
DEVICE_TYPE_NAME="${DEVICE_TYPE_NAME:-Apple TV 4K (3rd generation) (at 1080p)}"
SIMULATOR_NAME="${SIMULATOR_NAME:-$DEVICE_TYPE_NAME}"

# Try release build first, then debug
if [ -d "$PROJECT_ROOT/build-tvos_x86_64-ytrace-release/yetty.app" ]; then
    APP_BUNDLE="${APP_BUNDLE:-$PROJECT_ROOT/build-tvos_x86_64-ytrace-release/yetty.app}"
else
    APP_BUNDLE="${APP_BUNDLE:-$PROJECT_ROOT/build-tvos_x86_64-ytrace-debug/yetty.app}"
fi
BUNDLE_ID="com.yetty.terminal"

# Hardware-keyboard layout the tvOS Simulator should advertise to yetty
# (yetty's pressesBegan: receives UIKey.characters which is the post-layout
# value). Default Dvorak; export KEYBOARD_LAYOUT=QWERTY to override, or
# KEYBOARD_LAYOUT=skip to leave the sim's existing setting alone.
KEYBOARD_LAYOUT="${KEYBOARD_LAYOUT:-Dvorak}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }

#-----------------------------------------------------------------------------
# Check macOS
#-----------------------------------------------------------------------------
check_macos() {
    if [[ "$(uname -s)" != "Darwin" ]]; then
        error "This script only works on macOS"
        exit 1
    fi
    info "Running on macOS"
}

#-----------------------------------------------------------------------------
# Check Xcode tools
#-----------------------------------------------------------------------------
check_xcode() {
    if ! command -v xcrun &> /dev/null; then
        error "Xcode command line tools not found"
        echo "Install with: xcode-select --install"
        exit 1
    fi

    if ! xcrun simctl help &> /dev/null; then
        error "simctl not found. Install Xcode from the App Store."
        exit 1
    fi

    success "Xcode tools available"
}

#-----------------------------------------------------------------------------
# Find or create simulator
#
# Apple TV simulators are pre-shipped with Xcode under names like
# "Apple TV 4K (3rd generation) (at 1080p)" — we look for that name first
# and only create a new device if missing.
#-----------------------------------------------------------------------------
find_or_create_simulator() {
    info "Looking for tvOS simulator: $SIMULATOR_NAME"

    SIMULATOR_UDID="$(xcrun simctl list devices -j | python3 -c "
import sys, json
d = json.load(sys.stdin)
target = '$SIMULATOR_NAME'
for runtime, devs in d['devices'].items():
    if 'tvOS' not in runtime: continue
    for dev in devs:
        if dev.get('name') == target:
            print(dev['udid']); sys.exit(0)
" 2>/dev/null)"

    if [ -n "$SIMULATOR_UDID" ]; then
        success "Found simulator: $SIMULATOR_UDID"
        return 0
    fi

    info "Creating new tvOS simulator..."
    local runtime
    runtime="$(xcrun simctl list runtimes -j | python3 -c "
import sys, json
rts = json.load(sys.stdin)['runtimes']
tv = [r for r in rts if 'tvOS' in r.get('name','') and r.get('isAvailable', False)]
print(tv[-1]['identifier'] if tv else '')
" 2>/dev/null)"

    if [ -z "$runtime" ]; then
        error "No tvOS runtime found. Install in Xcode > Settings > Platforms > tvOS."
        exit 1
    fi
    info "Using runtime: $runtime"

    local device_type
    device_type="$(xcrun simctl list devicetypes -j | python3 -c "
import sys, json
target = '$DEVICE_TYPE_NAME'
dts = json.load(sys.stdin)['devicetypes']
for d in dts:
    if d.get('name') == target:
        print(d['identifier']); sys.exit(0)
" 2>/dev/null)"

    if [ -z "$device_type" ]; then
        device_type="com.apple.CoreSimulator.SimDeviceType.Apple-TV-4K-3rd-generation-1080p"
    fi
    info "Using device type: $device_type"

    SIMULATOR_UDID="$(xcrun simctl create "$SIMULATOR_NAME" "$device_type" "$runtime")"
    success "Created simulator: $SIMULATOR_UDID"
}

#-----------------------------------------------------------------------------
# Boot simulator
#-----------------------------------------------------------------------------
boot_simulator() {
    local state
    state="$(xcrun simctl list devices -j | python3 -c "
import sys, json
d = json.load(sys.stdin)
udid = '$SIMULATOR_UDID'
for runtime, devs in d['devices'].items():
    for dev in devs:
        if dev['udid'] == udid:
            print(dev.get('state','Unknown')); sys.exit(0)
" 2>/dev/null)"

    if [ "$state" = "Booted" ]; then
        success "Simulator already booted"
        return 0
    fi

    info "Booting simulator..."
    xcrun simctl boot "$SIMULATOR_UDID" 2>/dev/null || true
    open -a Simulator --args -CurrentDeviceUDID "$SIMULATOR_UDID"

    info "Waiting for simulator to boot..."
    local count=0
    while [ $count -lt 60 ]; do
        state="$(xcrun simctl list devices -j | python3 -c "
import sys, json
d = json.load(sys.stdin)
udid = '$SIMULATOR_UDID'
for runtime, devs in d['devices'].items():
    for dev in devs:
        if dev['udid'] == udid:
            print(dev.get('state','Unknown')); sys.exit(0)
" 2>/dev/null)"
        if [ "$state" = "Booted" ]; then
            success "Simulator booted"
            return 0
        fi
        sleep 1
        count=$((count + 1))
        echo -n "."
    done
    echo ""
    error "Simulator boot timeout"
    exit 1
}

#-----------------------------------------------------------------------------
# Set Simulator hardware-keyboard layout via the shared helper. tvOS has
# no Settings.app keyboard pane, so this preference write is the only way
# to change it. yetty's pressesBegan: receives UIKey.characters (post-layout),
# so the sim's setting determines what character yetty sees per physical key.
#-----------------------------------------------------------------------------
set_keyboard_layout() {
    if [ "$KEYBOARD_LAYOUT" = "skip" ]; then
        info "Keyboard layout: leaving sim's existing setting (KEYBOARD_LAYOUT=skip)"
        return 0
    fi
    "$SCRIPT_DIR/sim-keyboard.sh" "$KEYBOARD_LAYOUT" "$SIMULATOR_UDID"
}

#-----------------------------------------------------------------------------
# Install and run app
#-----------------------------------------------------------------------------
install_and_run() {
    if [ ! -d "$APP_BUNDLE" ]; then
        error "App bundle not found: $APP_BUNDLE"
        echo ""
        echo "Build first with:"
        echo "  make build-tvos_x86_64-ytrace-release"
        echo ""
        exit 1
    fi

    info "Reinstalling app: $APP_BUNDLE"
    xcrun simctl uninstall "$SIMULATOR_UDID" "$BUNDLE_ID" 2>/dev/null || true
    xcrun simctl install "$SIMULATOR_UDID" "$APP_BUNDLE"

    info "Launching app (YTRACE on)..."
    SIMCTL_CHILD_YTRACE_DEFAULT_ON=yes \
        xcrun simctl launch --terminate-running-process --console "$SIMULATOR_UDID" "$BUNDLE_ID" &

    success "Yetty launched on $SIMULATOR_NAME — focus the Apple TV Simulator window."
    success "Hardware keyboard pass-through: I/O > Keyboard > Connect Hardware Keyboard (⌘K)."
}

kill_simulator() {
    info "Shutting down simulators..."
    xcrun simctl shutdown all 2>/dev/null || true
    killall "Simulator" 2>/dev/null || true
    success "Simulators shut down"
}

list_simulators() {
    echo "Available tvOS Simulators:"
    echo ""
    xcrun simctl list devices available | sed -n '/-- tvOS/,/-- /p'
}

main() {
    echo "========================================"
    echo "  Yetty tvOS x86_64 Simulator"
    echo "========================================"
    echo ""

    case "${1:-}" in
        --kill|-k)  kill_simulator; exit 0 ;;
        --list|-l)  list_simulators; exit 0 ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "PREREQUISITE:"
            echo "  make build-tvos_x86_64-ytrace-release"
            echo ""
            echo "OPTIONS:"
            echo "  (no options)    Start simulator and install/launch the app"
            echo "  --kill, -k      Kill all running simulators"
            echo "  --list, -l      List available tvOS simulators"
            echo "  --help, -h      Show this help"
            echo ""
            echo "ENV:"
            echo "  KEYBOARD_LAYOUT   Hardware keyboard layout for the sim."
            echo "                    Default: Dvorak. Valid: Dvorak,"
            echo "                    DvorakLeft, DvorakRight, QWERTY, Colemak,"
            echo "                    or 'skip' to leave the sim's setting."
            echo "  DEVICE_TYPE_NAME  Override sim device (default: '$DEVICE_TYPE_NAME')."
            echo "  SIMULATOR_NAME    Override sim instance name."
            echo "  APP_BUNDLE        Override the .app path."
            exit 0
            ;;
    esac

    check_macos
    check_xcode
    find_or_create_simulator
    boot_simulator
    set_keyboard_layout
    install_and_run
}

main "$@"
