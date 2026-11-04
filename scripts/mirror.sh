#!/usr/bin/env bash
# Mirror the Mac's display to a Quest headset, over USB and/or Wi-Fi, in one command.
#
#   scripts/mirror.sh [options] [sender options...] [-- sender options...]
#
# Always runs quest-mirror-sender with LAN discovery, so the headset can find the Mac
# over Wi-Fi. If the headset is attached over USB it also sets up `adb reverse` (the
# app tries that first) and launches the app. Ctrl-C stops the sender and removes the
# adb reverse this script created.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/mirror.sh [options] [sender options...] [-- sender options...]

Options:
  --port N            TCP port (default 7777). Over USB the same port is used on the
                      headset; for a non-default port the app is told via the
                      debug.questmirror.port property on launch.
  --discovery-port N  UDP port answering LAN discovery (default 7778).
  --serial S          adb serial of the headset (default: $ANDROID_SERIAL or the only device).
  --install           Build the Quest app (quest/gradlew assembleDebug) and install it
                      first (needs the headset on USB).
  --no-launch         Do not start the Quest app.
  --wifi-only         Do not use adb at all; the headset connects over Wi-Fi.
  -h, --help          Show this help.

Any other arguments are passed to quest-mirror-sender, e.g.
  scripts/mirror.sh --codec h264 --bitrate 40
  scripts/mirror.sh --wifi-only -- --source test
EOF
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SENDER="$ROOT/mac/.build/release/quest-mirror-sender"
APP_ID="com.df0.questmirror"
ACTIVITY="$APP_ID/android.app.NativeActivity"
APK="$ROOT/quest/app/build/outputs/apk/debug/app-debug.apk"
DEFAULT_PORT=7777

PORT=$DEFAULT_PORT
DISCOVERY_PORT=7778
INSTALL=0
LAUNCH=1
WIFI_ONLY=0
SENDER_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) PORT="${2:?--port needs a value}"; shift 2 ;;
        --discovery-port) DISCOVERY_PORT="${2:?--discovery-port needs a value}"; shift 2 ;;
        --serial) export ANDROID_SERIAL="${2:?--serial needs a value}"; shift 2 ;;
        --install) INSTALL=1; shift ;;
        --no-launch) LAUNCH=0; shift ;;
        --wifi-only) WIFI_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; SENDER_ARGS+=("$@"); break ;;
        *) SENDER_ARGS+=("$1"); shift ;;
    esac
done
for p in "$PORT" "$DISCOVERY_PORT"; do
    if ! [[ "$p" =~ ^[0-9]+$ ]] || (( p < 1 || p > 65535 )); then
        echo "error: invalid port '$p'" >&2
        exit 2
    fi
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

(( INSTALL && WIFI_ONLY )) && die "--install needs the headset on USB; drop --wifi-only"

# adb 37.0.1's mDNS backend crashes the adb server; USB does not need it.
export ADB_MDNS=0

# 1. Is the headset attached over USB?
USB=0
if (( !WIFI_ONLY )); then
    if ! command -v adb >/dev/null; then
        warn "adb not found; continuing with Wi-Fi only"
    else
        state="$(adb get-state 2>&1 || true)"
        case "$state" in
            device) USB=1 ;;
            *unauthorized*) warn "headset on USB is not authorized (accept the USB debugging prompt in the headset); continuing with Wi-Fi only" ;;
            *"more than one"*) warn "several adb devices attached; pick one with --serial. Continuing with Wi-Fi only" ;;
            *) say "No headset on USB" ;;
        esac
    fi
fi
(( INSTALL && !USB )) && die "--install needs the headset attached and authorized over USB"

# 2. Sender binary, rebuilt when any Mac source is newer.
if [[ ! -x "$SENDER" ]] || [[ -n "$(find "$ROOT/mac/Sources" "$ROOT/mac/Package.swift" -newer "$SENDER" -print -quit)" ]]; then
    say "Building quest-mirror-sender (release)"
    swift build -c release --package-path "$ROOT/mac" --product quest-mirror-sender
fi

# 3. USB: install, adb reverse (removed on exit unless it existed before), launch.
CREATED_REVERSE=0
cleanup() {
    if (( CREATED_REVERSE )); then
        adb reverse --remove "tcp:$PORT" >/dev/null 2>&1 && say "Removed adb reverse tcp:$PORT" \
            || warn "could not remove adb reverse tcp:$PORT"
    fi
}
trap cleanup EXIT
if (( USB )); then
    say "Headset on USB: $(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r') ($(adb get-serialno))"
    if (( INSTALL )); then
        say "Building the Quest app"
        (cd "$ROOT/quest" && ./gradlew assembleDebug)
        say "Installing $APK"
        adb install -r "$APK"
    fi
    existing="$(adb reverse --list 2>/dev/null | awk -v r="tcp:$PORT" '$2 == r { print $3 }')"
    if [[ "$existing" == "tcp:$PORT" ]]; then
        say "adb reverse tcp:$PORT tcp:$PORT already set up (left in place on exit)"
    else
        [[ -n "$existing" ]] && warn "replacing existing adb reverse tcp:$PORT -> $existing"
        adb reverse "tcp:$PORT" "tcp:$PORT" >/dev/null
        CREATED_REVERSE=1
        say "adb reverse tcp:$PORT tcp:$PORT"
    fi
    if (( LAUNCH )); then
        if (( PORT != DEFAULT_PORT )); then
            adb shell setprop debug.questmirror.port "$PORT"
        elif [[ -n "$(adb shell getprop debug.questmirror.port | tr -d '\r')" ]]; then
            adb shell setprop debug.questmirror.port "''"
        fi
        say "Launching $ACTIVITY"
        adb shell am start -n "$ACTIVITY" >/dev/null
    fi
fi

# 4. How the headset will connect.
discovery=1
for arg in ${SENDER_ARGS[@]+"${SENDER_ARGS[@]}"}; do
    [[ "$arg" == "--no-discovery" ]] && discovery=0
done
addresses=""
for interface in $(ipconfig getiflist 2>/dev/null); do
    ip="$(ipconfig getifaddr "$interface" 2>/dev/null || true)"
    [[ -n "$ip" ]] && addresses+="${addresses:+, }$ip ($interface)"
done
if (( USB )); then
    say "Headset connects over USB: 127.0.0.1:$PORT on the headset -> adb reverse -> this Mac (tried first)"
fi
if (( discovery )); then
    say "Over Wi-Fi it finds this Mac by UDP discovery on port $DISCOVERY_PORT and connects to ${addresses:-<no IPv4 address>} port $PORT"
else
    say "Discovery is off: over Wi-Fi the app needs debug.questmirror.host set to one of ${addresses:-<no IPv4 address>}"
fi
if (( !USB )) && (( LAUNCH )); then
    say "Start Quest Mirror on the headset (same Wi-Fi network as this Mac)"
fi

# 5. Sender in the foreground. Ctrl-C reaches it directly; it shuts down cleanly and
# the EXIT trap removes the reverse.
say "Starting quest-mirror-sender on TCP $PORT (Ctrl-C to stop)"
status=0
"$SENDER" --port "$PORT" --discovery-port "$DISCOVERY_PORT" ${SENDER_ARGS[@]+"${SENDER_ARGS[@]}"} || status=$?
exit "$status"
