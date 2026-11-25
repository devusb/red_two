#!/usr/bin/env bash
# Configure, build and sign for iOS (arm64 devices) on a Mac with Xcode, like build_macos.sh.
#   tools/build_ios.sh --profile <app.mobileprovision> --identity "<Apple Development: ...>"
#       [--disc <game.rvz|.iso|main.dol>] [--bundle-id <id>] [--host-recomp <r2recomp>]
#       [--build-dir <dir>] [--configure] [--icon <png>] [--ipa] [--install <device>]
# Result: build/ios/Red Two.app (Dawn and SDL3 linked in statically); --ipa also packs
# build/RedTwo-ios-arm64.ipa, --install puts the app on a connected device (xcrun devicectl).
# Needs: Xcode with the iOS SDK, cmake (3.25+), ninja, and a provisioning profile whose App ID
# has the Extended Virtual Addressing capability. Without --profile the app is built unsigned.
# The recompiler runs on the Mac: --host-recomp names one, else build/macos/r2recomp is used or
# built into build/ios-host.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/ios"
disc=""
configure=0
config="RelWithDebInfo"
profile=""
identity=""
bundle_id=""
host_recomp=""
ipa=0
icon=""
device=""
deployment="16.0"
cmake_args=()
while [ $# -gt 0 ]; do
    case "$1" in
    --disc) disc="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; configure=1; shift 2 ;;
    --build-dir) mkdir -p "$2"; build="$(cd "$2" && pwd)"; shift 2 ;;
    --configure) configure=1; shift ;;
    --config) config="$2"; shift 2 ;;
    --profile) profile="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    --identity) identity="$2"; shift 2 ;;
    --bundle-id) bundle_id="$2"; configure=1; shift 2 ;;
    --host-recomp) host_recomp="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    --icon) icon="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    --ipa) ipa=1; shift ;;
    --install) device="$2"; shift 2 ;;
    -D*) cmake_args+=("$1"); shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done

if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    echo "no iOS SDK: install Xcode and select it (sudo xcode-select -s /Applications/Xcode.app)" >&2
    exit 1
fi
if [ -n "$profile" ] && [ -z "$identity" ]; then
    echo "--profile needs --identity (security find-identity -v -p codesigning lists them)" >&2
    exit 2
fi

if [ ! -f "$root/extern/aurora/CMakeLists.txt" ] || [ ! -f "$root/extern/rcheevos/include/rc_client.h" ]; then
    git -C "$root" submodule update --init --recursive
fi

# Our fixes to Aurora, as build_linux.sh
aurora="$root/extern/aurora"
clean=0
[ -z "$(git -C "$aurora" status --porcelain --untracked-files=no)" ] && clean=1
for patch in "$root"/patches/aurora/*.patch; do
    if [ $clean -eq 0 ] && git -C "$aurora" apply --reverse --check "$patch" 2>/dev/null; then continue; fi
    if git -C "$aurora" apply "$patch" 2>/dev/null; then echo "applied $(basename "$patch")"; continue; fi
    if [ $clean -eq 1 ]; then echo "could not apply $(basename "$patch") to extern/aurora" >&2; exit 1; fi
    echo "note: $(basename "$patch") neither applies nor reverses on the modified extern/aurora (assumed applied)"
done

# the profile: its team, its App ID (the bundle ID unless a wildcard) and its entitlements
work="$build/sign"
mkdir -p "$work"
if [ -n "$profile" ]; then
    security cms -D -i "$profile" > "$work/profile.plist"
    /usr/libexec/PlistBuddy -x -c 'Print :Entitlements' "$work/profile.plist" > "$work/entitlements.plist"
    team="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.developer.team-identifier' "$work/profile.plist")"
    app_id="$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:application-identifier' "$work/profile.plist")"
    app_id="${app_id#"$team".}"
    if [ -z "$bundle_id" ]; then
        case "$app_id" in
        *\**) ;;
        *) bundle_id="$app_id" ;;
        esac
    fi
    if ! /usr/libexec/PlistBuddy -c 'Print :com.apple.developer.kernel.extended-virtual-addressing' "$work/entitlements.plist" >/dev/null 2>&1; then
        echo "the profile does not grant com.apple.developer.kernel.extended-virtual-addressing:" >&2
        echo "turn on Extended Virtual Addressing for its App ID and download the profile again" >&2
        exit 1
    fi
fi
[ -z "$bundle_id" ] && bundle_id="io.github.gminustwo.redtwo"
if [ -f "$build/CMakeCache.txt" ] && [ "$(sed -n 's/^R2_IOS_BUNDLE_ID:STRING=//p' "$build/CMakeCache.txt")" != "$bundle_id" ]; then
    configure=1
fi

if [ $configure -eq 1 ] || [ ! -f "$build/build.ninja" ]; then
    if [ -z "$disc" ] && [ -f "$build/CMakeCache.txt" ]; then
        disc="$(sed -n 's/^R2_DOL:FILEPATH=//p' "$build/CMakeCache.txt")"
    fi
    if [ -z "$host_recomp" ] && [ -f "$build/CMakeCache.txt" ]; then
        host_recomp="$(sed -n 's/^R2_HOST_RECOMP:FILEPATH=//p' "$build/CMakeCache.txt")"
    fi
    if [ -z "$host_recomp" ] && [ -x "$root/build/macos/r2recomp" ]; then
        host_recomp="$root/build/macos/r2recomp"
    fi
    if [ -z "$host_recomp" ] || [ ! -x "$host_recomp" ]; then
        host="$root/build/ios-host"
        cmake -S "$root" -B "$host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
            -DAURORA_SDL3_PROVIDER=vendor "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" \
            -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
        cmake --build "$host" --target r2recomp
        host_recomp="$host/r2recomp"
    fi
    args=(-S "$root" -B "$build" -G Ninja "-DCMAKE_BUILD_TYPE=$config"
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=$deployment"
        -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static
        -DAURORA_DAWN_PROVIDER=auto -DAURORA_DAWN_LINKAGE=static
        -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
        "-DR2_HOST_RECOMP=$host_recomp" "-DR2_IOS_BUNDLE_ID=$bundle_id")
    rm -f "$build/CMakeCache.txt"
    [ -n "$disc" ] && args+=("-DR2_DOL=$disc")
    cmake "${args[@]}" ${cmake_args[@]+"${cmake_args[@]}"}
fi
cmake --build "$build" --target red_two

app="$build/Red Two.app"
# the starter shader list (tools/make_shader_seed.py): Aurora reads it from the app's folder
if [ -f "$root/build/initial_pipeline_cache.db" ]; then
    cp "$root/build/initial_pipeline_cache.db" "$app/"
fi
# the app icon: --icon, else your own res/icon.png, else Red Two's own res/red_two.png
if [ -z "$icon" ]; then
    icon="$root/res/red_two.png"
    [ -f "$root/res/icon.png" ] && icon="$root/res/icon.png"
fi
if [ -f "$icon" ]; then
    for spec in 60x60@2x:120 60x60@3x:180 76x76@2x~ipad:152 83.5x83.5@2x~ipad:167; do
        sips -s format png -z "${spec#*:}" "${spec#*:}" "$icon" --out "$app/AppIcon${spec%%:*}.png" >/dev/null
    done
fi

if [ -z "$profile" ]; then
    echo "built $app (unsigned: pass --profile and --identity to sign it)"
    exit 0
fi
/usr/libexec/PlistBuddy -c "Set :application-identifier $team.$bundle_id" "$work/entitlements.plist"
/usr/libexec/PlistBuddy -c 'Delete :keychain-access-groups' "$work/entitlements.plist" 2>/dev/null || true
cp "$profile" "$app/embedded.mobileprovision"
codesign --force --sign "$identity" --entitlements "$work/entitlements.plist" --generate-entitlement-der "$app"
codesign --verify --strict "$app"
echo "built $app ($bundle_id, team $team)"

if [ $ipa -eq 1 ]; then
    out="$root/build/RedTwo-ios-arm64.ipa"
    rm -rf "$work/Payload" "$out"
    mkdir -p "$work/Payload"
    cp -R "$app" "$work/Payload/"
    (cd "$work" && zip -qry "$out" Payload)
    echo "packed $out"
fi
if [ -n "$device" ]; then
    xcrun devicectl device install app --device "$device" "$app"
fi
