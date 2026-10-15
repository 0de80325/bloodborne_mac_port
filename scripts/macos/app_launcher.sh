#!/bin/bash
# Bloodborne.app's launcher (Contents/MacOS/Bloodborne), installed by scripts/macos/package_app.sh.
# Finds the game folder (asks once, remembered in launch.conf in the data folder), checks Rosetta
# and the game files, then runs the bundled run.sh in the background. The game's log goes to
# ~/Library/Logs/Bloodborne/game.log (the previous session's: game.previous.log); if the game
# stops with an error, a dialog offers the log.
# Saves and settings: ~/Library/Application Support/Bloodborne (BB_DATA_DIR overrides).
contents=$(cd "$(dirname "$0")/.." && pwd)
port=$contents/Resources/port
python=$contents/Resources/python/bin/python3
data=${BB_DATA_DIR:-$HOME/Library/Application Support/Bloodborne}
logs=$HOME/Library/Logs/Bloodborne
conf=$data/launch.conf
mkdir -p "$logs" "$data/user/savedata"

alert() { # title, message
    osascript - "$1" "$2" >/dev/null 2>&1 <<'OSA'
on run argv
    display alert (item 1 of argv) message (item 2 of argv) as critical
end run
OSA
}

if pgrep -qx bbgpu-host || pgrep -qx bb-probe; then
    alert "Bloodborne is already running" "Only one copy of the game can run at a time."
    exit 0
fi

# The game process is x86-64 code: it needs Rosetta 2.
if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
    answer=$(osascript 2>/dev/null <<'OSA'
set r to display alert "Bloodborne needs Rosetta 2" message "The game runs its original PlayStation 4 code through Rosetta 2, Apple's translator for Intel code. Install it now? (macOS asks for an administrator password.)" buttons {"Cancel", "Install"} default button "Install"
return button returned of r
OSA
)
    [[ $answer == Install ]] || exit 0
    if ! osascript -e 'do shell script "/usr/sbin/softwareupdate --install-rosetta --agree-to-license" with administrator privileges' >/dev/null 2>&1 ||
       ! arch -x86_64 /usr/bin/true 2>/dev/null; then
        alert "Rosetta 2 was not installed" "In Terminal, run: softwareupdate --install-rosetta"
        exit 1
    fi
fi

# The game folder: launch.conf, else ask (and remember the answer).
BB_GAME_DIR=
if [[ -f $conf ]]; then
    BB_GAME_DIR=$(set -a; source "$conf" >/dev/null 2>&1; printf '%s' "${BB_GAME_DIR:-}")
fi
if [[ -z $BB_GAME_DIR || ! -f $BB_GAME_DIR/eboot.bin ]]; then
    picked=$(osascript -e 'POSIX path of (choose folder with prompt "Choose your Bloodborne game folder (CUSA03173 with update 1.09, the folder containing eboot.bin):")' 2>/dev/null) || exit 0
    picked=${picked%/}
    if [[ ! -f $picked/eboot.bin ]]; then
        alert "No game found" "There is no eboot.bin in $picked. Choose the folder that contains eboot.bin, sce_sys and dvdroot_ps4."
        exit 1
    fi
    BB_GAME_DIR=$picked
    { grep -v '^BB_GAME_DIR=' "$conf" 2>/dev/null; printf 'BB_GAME_DIR=%q\n' "$BB_GAME_DIR"; } > "$conf.tmp" && mv "$conf.tmp" "$conf"
fi
if ! problem=$(PYTHONDONTWRITEBYTECODE=1 "$python" "$port/scripts/game_check.py" "$BB_GAME_DIR" 2>&1); then
    alert "This game folder cannot be used" "$BB_GAME_DIR

$problem

To choose another folder, delete $conf and open Bloodborne again."
    exit 1
fi

[[ -f $logs/game.log ]] && mv -f "$logs/game.log" "$logs/game.previous.log"
export BB_DATA_DIR=$data BB_GAME_DIR BB_PREBUILT=1 BB_PROBE=$contents/MacOS/bb-probe PYTHON=$python \
    PYTHONDONTWRITEBYTECODE=1 VK_DRIVER_FILES=$port/MoltenVK_icd.json
(
    status=0
    /bin/bash "$port/run.sh" > "$logs/game.log" 2>&1 || status=$?
    if (( status != 0 )); then
        answer=$(osascript - "$status" "$logs/game.log" 2>/dev/null <<'OSA'
on run argv
    set r to display alert "Bloodborne stopped" message ("The game exited with status " & (item 1 of argv) & ". The log is " & (item 2 of argv) & ".") as critical buttons {"Show Log", "OK"} default button "OK"
    return button returned of r
end run
OSA
)
        [[ $answer == "Show Log" ]] && open -a Console "$logs/game.log"
    fi
) < /dev/null > /dev/null 2>&1 &
disown
exit 0
