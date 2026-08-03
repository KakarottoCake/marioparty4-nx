#!/bin/bash

# macOS equivalent of deploy_to_sd.bat. It only copies the NRO and the user's
# already-extracted files folder; it never downloads or supplies game assets.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NRO_SOURCE=""
ASSET_SOURCE=""

pause_before_exit() {
    printf '\nPress Return to close. '
    read -r _
}

fail() {
    printf '\nERROR: %s\n' "$1"
    pause_before_exit
    exit 1
}

choose_file() {
    /usr/bin/osascript -e 'tell application "Finder" to POSIX path of (choose file with prompt "Select marioparty4_switch.nro")' 2>/dev/null || true
}

choose_folder() {
    /usr/bin/osascript -e 'tell application "Finder" to POSIX path of (choose folder with prompt "Select the folder containing the extracted game files")' 2>/dev/null || true
}

clean_path() {
    local value="$1"
    value="${value#\"}"
    value="${value%\"}"
    value="${value%/}"
    printf '%s' "$value"
}

printf '\nMario Party 4 Switch - macOS SD card installer\n\n'
printf 'This creates:\n  SD/switch/marioparty4_switch.nro\n  SD/switch/files/...\n\n'
printf 'The game assets must come from your own extracted copy of Mario Party 4.\n'
printf 'This script does not download or provide copyrighted game data.\n\n'

if [ -f "$SCRIPT_DIR/marioparty4_switch.nro" ]; then
    NRO_SOURCE="$SCRIPT_DIR/marioparty4_switch.nro"
elif [ -f "$SCRIPT_DIR/src/platform/switch/marioparty4_switch.nro" ]; then
    NRO_SOURCE="$SCRIPT_DIR/src/platform/switch/marioparty4_switch.nro"
fi

if [ -d "$SCRIPT_DIR/files/data" ]; then
    ASSET_SOURCE="$SCRIPT_DIR/files"
fi

if [ -z "$NRO_SOURCE" ]; then
    printf 'The compiled NRO was not found beside this script.\n'
    NRO_SOURCE="$(choose_file)"
    if [ -z "$NRO_SOURCE" ]; then
        read -r -p 'Enter the full path to marioparty4_switch.nro: ' NRO_SOURCE
    fi
    NRO_SOURCE="$(clean_path "$NRO_SOURCE")"
fi
[ -f "$NRO_SOURCE" ] || fail 'NRO file not found.'

if [ -z "$ASSET_SOURCE" ]; then
    printf '\nThe extracted game files folder was not found beside this script.\n'
    printf 'It should contain folders such as data, dll, and mess.\n'
    ASSET_SOURCE="$(choose_folder)"
    if [ -n "$ASSET_SOURCE" ]; then
        ASSET_SOURCE="$(clean_path "$ASSET_SOURCE")"
    fi
fi

if [ -n "$ASSET_SOURCE" ] && [ -d "$ASSET_SOURCE/files/data" ] && [ ! -d "$ASSET_SOURCE/data" ]; then
    ASSET_SOURCE="$ASSET_SOURCE/files"
fi
if [ -n "$ASSET_SOURCE" ] && [ ! -d "$ASSET_SOURCE" ]; then
    printf '\nWARNING: The asset folder was not found. Continuing without assets.\n'
    ASSET_SOURCE=""
fi

printf '\nSelect the mounted SD card volume in the next Finder window.\n'
SD_ROOT="$(/usr/bin/osascript -e 'tell application "Finder" to POSIX path of (choose folder with prompt "Select the mounted Nintendo Switch SD card")' 2>/dev/null || true)"
if [ -z "$SD_ROOT" ]; then
    read -r -p 'Enter the SD card volume path, for example /Volumes/SWITCH: ' SD_ROOT
fi
SD_ROOT="$(clean_path "$SD_ROOT")"
[ -d "$SD_ROOT" ] || fail "SD card volume not found: $SD_ROOT"

TARGET_DIR="$SD_ROOT/switch"
printf '\nTarget: %s\n' "$TARGET_DIR"
read -r -p 'Continue? [y/N] ' CONFIRM
case "$CONFIRM" in
    y|Y|yes|YES) ;;
    *) printf 'Cancelled.\n'; exit 0 ;;
esac

mkdir -p "$TARGET_DIR" || fail 'Could not create the switch folder.'
cp -f "$NRO_SOURCE" "$TARGET_DIR/marioparty4_switch.nro" || fail 'Could not copy the NRO.'
mkdir -p "$TARGET_DIR/files" || fail 'Could not create the files folder.'

if [ -n "$ASSET_SOURCE" ]; then
    /usr/bin/ditto "$ASSET_SOURCE/." "$TARGET_DIR/files" || fail 'Could not copy the game files.'
else
    printf '\nWARNING: No game assets were copied.\n'
    printf 'Put the extracted files folder in %s before launching.\n' "$TARGET_DIR/files"
fi

printf '\nDone. Launch marioparty4_switch.nro through Homebrew Menu.\n'
printf 'The game log should report: Total files found: 357\n'
pause_before_exit
