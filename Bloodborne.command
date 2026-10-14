#!/usr/bin/env bash
# Double-click in Finder to play: opens Terminal and runs run.sh (which rebuilds the game
# process if the sources changed). Keep the Terminal window open while playing; closing it
# ends the game.
#
# Settings live in launch.conf next to this file (not committed), as shell assignments:
#   BB_GAME_DIR=/path/to/CUSA03173   the game folder (asked for on first launch)
#   BB_DATA_DIR=/path/to/data        saves, shader caches and bbport.ini (default: this folder)
# Any other run.sh variable (BB_FPS, BB_CONFIG, ...) can be set there too.
set -uo pipefail
cd -- "$(dirname -- "$0")" || exit 1
repo=$PWD
conf=$repo/launch.conf

finish() {
    echo
    read -r -p 'Press Return to close this window.' _
    exit "$1"
}

if [[ -f $conf ]]; then
    set -a
    # shellcheck disable=SC1090
    source "$conf"
    set +a
fi

if [[ ! -x out/bb-probe || ! -x out/bbgpu-host ]]; then
    echo 'The port is not built yet. In Terminal, from this folder, run once:'
    echo '  bash scripts/macos/bootstrap_deps.sh'
    echo '  BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh'
    echo '  bash build_macos.sh'
    finish 1
fi

# The game folder: launch.conf, else ../CUSA03173, else ask (and remember the answer).
if [[ -z ${BB_GAME_DIR:-} && -f ../CUSA03173/eboot.bin ]]; then
    BB_GAME_DIR=$(cd ../CUSA03173 && pwd)
fi
if [[ -z ${BB_GAME_DIR:-} || ! -f $BB_GAME_DIR/eboot.bin ]]; then
    picked=$(osascript -e 'POSIX path of (choose folder with prompt "Choose your Bloodborne game folder (CUSA03173, the one containing eboot.bin):")' 2>/dev/null) || {
        echo 'No game folder chosen.'
        finish 1
    }
    picked=${picked%/}
    if [[ ! -f $picked/eboot.bin ]]; then
        echo "No eboot.bin in $picked."
        finish 1
    fi
    BB_GAME_DIR=$picked
    { grep -v '^BB_GAME_DIR=' "$conf" 2>/dev/null; printf 'BB_GAME_DIR=%q\n' "$BB_GAME_DIR"; } > "$conf.tmp" &&
        mv "$conf.tmp" "$conf"
    echo "Game folder saved in launch.conf: $BB_GAME_DIR"
fi
export BB_GAME_DIR
[[ -n ${BB_DATA_DIR:-} ]] && export BB_DATA_DIR

echo "Game:  $BB_GAME_DIR"
echo "Data:  ${BB_DATA_DIR:-$repo}"
echo
bash run.sh "$@"
status=$?
if (( status != 0 )); then
    echo "The game exited with status $status."
    finish "$status"
fi
