#!/usr/bin/env bash
# Developer wrapper: run the sender from source.
#
#   scripts/mirror.sh [--install] [sender options...]
#
# Rebuilds vrscreen-sender when its sources changed and runs it in the foreground. The
# sender itself handles USB (adb reverse, launching the headset app; --no-adb for Wi-Fi
# only) and LAN discovery; Ctrl-C stops it cleanly. For everyday use, build the app
# instead: scripts/build-app.sh --install.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/mirror.sh [--install] [sender options...]

  --install   Build the headset app from source (quest/gradlew assembleDebug) and install
              it with adb first (headset on USB).
  -h, --help  This help, then the sender's options.

All other arguments go to vrscreen-sender, e.g.
  scripts/mirror.sh --no-adb --source test
  scripts/mirror.sh --codec h264 --bitrate 40
EOF
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SENDER="$ROOT/mac/.build/release/vrscreen-sender"
APK="$ROOT/quest/app/build/outputs/apk/debug/app-debug.apk"

INSTALL=0
HELP=0
SENDER_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --install) INSTALL=1 ;;
        -h|--help) HELP=1 ;;
        *) SENDER_ARGS+=("$arg") ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

if [[ ! -x "$SENDER" ]] || [[ -n "$(find "$ROOT/mac/Sources" "$ROOT/mac/Package.swift" -newer "$SENDER" -print -quit)" ]]; then
    say "Building vrscreen-sender (release)"
    swift build -c release --package-path "$ROOT/mac" --product vrscreen-sender
fi

if (( HELP )); then
    usage
    echo
    exec "$SENDER" --help
fi

if (( INSTALL )); then
    say "Building the headset app"
    # Gradle needs a JDK; shells that don't source ~/.zshrc lack JAVA_HOME.
    if [[ -z "${JAVA_HOME:-}" ]] && ! /usr/libexec/java_home >/dev/null 2>&1; then
        jdk="/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home"
        [[ -x "$jdk/bin/java" ]] || die "no JDK found: install one (brew install openjdk@17) or set JAVA_HOME"
        export JAVA_HOME="$jdk"
    fi
    (cd "$ROOT/quest" && ./gradlew assembleDebug)
    command -v adb >/dev/null || die "adb not found (brew install --cask android-platform-tools)"
    say "Installing $APK"
    # adb 37.0.1's mDNS backend crashes the adb server; USB does not need it.
    ADB_MDNS=0 adb install -r "$APK"
fi

exec "$SENDER" ${SENDER_ARGS[@]+"${SENDER_ARGS[@]}"}
