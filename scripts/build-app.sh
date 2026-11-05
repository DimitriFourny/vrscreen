#!/usr/bin/env bash
# Builds VRScreen.app (the sender as a menu-bar app, with the headset app inside) in build/.
#
#   scripts/build-app.sh [--install] [--dist]
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build-app.sh [--install] [--dist]

Builds the sender (release) and the headset app (quest/gradlew assembleDebug), and
assembles build/VRScreen.app with the headset APK bundled, signed ad hoc.

Options:
  --install   Quit a running /Applications/VRScreen.app, replace it with the new build
              and start it again if it was running.
  --dist      Also write build/VRScreen-<version>.zip (for a GitHub release).
  -h, --help  Show this help.
EOF
}

INSTALL=0
DIST=0
for arg in "$@"; do
    case "$arg" in
        --install) INSTALL=1 ;;
        --dist) DIST=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option '$arg'" >&2; usage >&2; exit 2 ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION must be MAJOR.MINOR.PATCH, got '$VERSION'"
BUNDLE_ID="com.df0.vrscreen.mac"
APP="$ROOT/build/VRScreen.app"
INSTALLED="/Applications/VRScreen.app"

# 1. The sender.
say "Building vrscreen-sender $VERSION (release)"
swift build -c release --package-path "$ROOT/mac" --product vrscreen-sender
SENDER="$(swift build -c release --package-path "$ROOT/mac" --show-bin-path)/vrscreen-sender"

# 2. The headset app. Gradle needs a JDK; shells that don't source ~/.zshrc lack JAVA_HOME.
say "Building the headset app $VERSION"
if [[ -z "${JAVA_HOME:-}" ]] && ! /usr/libexec/java_home >/dev/null 2>&1; then
    jdk="/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home"
    [[ -x "$jdk/bin/java" ]] || die "no JDK found: install one (brew install openjdk@17) or set JAVA_HOME"
    export JAVA_HOME="$jdk"
fi
(cd "$ROOT/quest" && ./gradlew --quiet assembleDebug)
APK="$ROOT/quest/app/build/outputs/apk/debug/app-debug.apk"
[[ -f "$APK" ]] || die "missing $APK"

# 3. The bundle.
say "Assembling ${APP#"$ROOT"/}"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$SENDER" "$APP/Contents/MacOS/VRScreen"
cp "$ROOT/mac/Resources/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
cp "$APK" "$APP/Contents/Resources/vrscreen-headset.apk"
printf 'APPL????' > "$APP/Contents/PkgInfo"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleDisplayName</key>
	<string>VRScreen</string>
	<key>CFBundleExecutable</key>
	<string>VRScreen</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>CFBundleIdentifier</key>
	<string>$BUNDLE_ID</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>VRScreen</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$VERSION</string>
	<key>CFBundleVersion</key>
	<string>$VERSION</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.utilities</string>
	<key>LSMinimumSystemVersion</key>
	<string>14.2</string>
	<key>LSMultipleInstancesProhibited</key>
	<true/>
	<key>LSUIElement</key>
	<true/>
	<key>NSAudioCaptureUsageDescription</key>
	<string>VRScreen plays your Mac's sound in the headset, in sync with the picture.</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSHumanReadableCopyright</key>
	<string>MIT License. Meta and Meta Quest are trademarks of Meta Platforms, Inc.</string>
	<key>NSLocalNetworkUsageDescription</key>
	<string>VRScreen streams your screen to your Meta Quest over the local network (TCP port 7777) and answers the headset's discovery broadcasts (UDP port 7778).</string>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
	<key>NSSupportsAutomaticTermination</key>
	<false/>
	<key>NSSupportsSuddenTermination</key>
	<false/>
</dict>
</plist>
EOF

# 4. Ad-hoc signature (no Developer ID: see README › Install for Gatekeeper).
say "Signing (ad hoc) and verifying"
codesign --force --deep --sign - --identifier "$BUNDLE_ID" "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"
plutil -lint "$APP/Contents/Info.plist"
say "Built ${APP#"$ROOT"/} ($VERSION)"

# 5. Zip for distribution.
if (( DIST )); then
    ZIP="$ROOT/build/VRScreen-$VERSION.zip"
    rm -f "$ZIP"
    ditto -c -k --keepParent "$APP" "$ZIP"
    say "Wrote ${ZIP#"$ROOT"/} ($(du -h "$ZIP" | cut -f1 | tr -d ' '))"
fi

# 6. Install: quit the running copy (SIGTERM: the same clean shutdown as Quit, which also
# removes its adb reverse), replace it, restart it if it was running.
if (( INSTALL )); then
    was_running=0
    pids="$(pgrep -f "^$INSTALLED/Contents/MacOS/VRScreen" || true)"
    if [[ -n "$pids" ]]; then
        was_running=1
        say "Quitting the running VRScreen"
        kill -TERM $pids
        for _ in $(seq 1 50); do
            pgrep -f "^$INSTALLED/Contents/MacOS/VRScreen" >/dev/null || break
            sleep 0.2
        done
        pgrep -f "^$INSTALLED/Contents/MacOS/VRScreen" >/dev/null && die "VRScreen did not quit; quit it from its menu and retry"
    fi
    say "Installing $INSTALLED"
    rm -rf "$INSTALLED"
    ditto "$APP" "$INSTALLED"
    if (( was_running )); then
        say "Starting VRScreen again"
        open "$INSTALLED"
    fi
fi
