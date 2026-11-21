#!/bin/bash
# Checks what an installed app does besides the game (player.sh and the
# starter build.sh writes), on a Mac that a player install just set up:
# vm-test.sh runs it in the VM after the smoke cases.
#   vm-app-check.sh APP_PATH INSTALL_SH [--update]
# - the app has player.sh, game.env and a three-line kit-version;
# - started from Finder, the game writes ~/Library/Logs/<id>.log, and the
#   runtime adds the app menu's actions;
# - the update check at launch, against a stand-in for GitHub's API: no
#   question when the release isn't newer, Later starts the game, Update
#   doesn't, and writes the update script; --update also lets that script
#   run (a whole rebuild, in Terminal) and waits for the new app;
# - Show Saved Games and Show Logs;
# - install.sh --uninstall (answering its questions): the app and the kit
#   go, the saved games stay.
# Prints a line for each check (ok, or FAIL); exits 1 if any failed.
set -u
APP_PATH=$1 INSTALL_SH=$2 do_update=0
[ "${3:-}" = --update ] && do_update=1
R="$APP_PATH/Contents/Resources"
# shellcheck source=/dev/null
. "$R/game.env"
SUPPORT="$HOME/Library/Application Support/$ID"
LOG="$HOME/Library/Logs/$ID.log"
fails=0
ok() { echo "ok    $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
check() { local what=$1; shift; if "$@"; then ok "$what"; else fail "$what"; fi; }
game_running() { pgrep -f "$APP_PATH/Contents/MacOS/$BINARY" > /dev/null; }
stop_game() {
  pkill -f "$APP_PATH/Contents/MacOS/$BINARY"
  for _ in $(seq 20); do game_running || return 0; sleep 0.5; done
  pkill -9 -f "$APP_PATH/Contents/MacOS/$BINARY"
}
pe() { eval "export ${ENV}_$1=\"\$2\""; }   # pe NAME VALUE: exports <ENV>_NAME
ue() { unset "${ENV}_$1"; }

# --- the app's files
check "player.sh, game.env and kit-version in Resources" test -x "$R/player.sh" -a -f "$R/game.env" -a -f "$R/kit-version"
check "kit-version has the full game commit" test "$(sed -n 3p "$R/kit-version" | wc -c | tr -d ' ')" = 41
rev=$(sed -n 3p "$R/kit-version")

# --- the update check at launch, against a stand-in for GitHub's API
api=$(mktemp -d)
mkdir -p "$api/releases" "$api/compare"
echo '{"tag_name":"v9.9.9"}' > "$api/releases/latest"
pe UPDATE_API "file://$api"
stamp="$SUPPORT/update-checked"
launch() { rm -f "$stamp"; /bin/bash "$R/player.sh" launch 2> "$api/err"; echo $?; }
echo '{"status":"identical"}' > "$api/compare/$rev...v9.9.9"
pe TEST_ANSWER Update
check "launch, same release: no question, the game starts" test "$(launch)" = 0
check "launch, same release: no question asked" test ! -s "$api/err"
check "launch: checked (stamp)" test -f "$stamp"
echo '{"status":"ahead"}' > "$api/compare/$rev...v9.9.9"
check "launch, within a day of the last check: nothing" test "$(/bin/bash "$R/player.sh" launch 2>&1; echo $?)" = 0
pe TEST_ANSWER Later
check "launch, newer release, Later: the game starts" test "$(launch)" = 0
check "launch, newer release: asked" grep -q "v9.9.9" "$api/err"
pe NO_UPDATE_CHECK 1
check "launch with NO_UPDATE_CHECK: not asked" test "$(launch)" = 0 -a ! -s "$api/err"
ue NO_UPDATE_CHECK
pe TEST_ANSWER Update
pe INSTALL_URL "file://$INSTALL_SH"
if [ $do_update = 0 ]; then
  # Without --update, the update script is written but Terminal runs a
  # stand-in for it (open is what player.sh starts it with).
  mkdir -p "$api/bin"
  printf '#!/bin/bash\necho "$@" > %q\n' "$api/opened" > "$api/bin/open"
  chmod 755 "$api/bin/open"
  PATH="$api/bin:$PATH"
fi
before=$(stat -f %m "$R/kit-version")
check "launch, newer release, Update: the game doesn't start (10)" test "$(launch)" = 10
check "the update script reruns install.sh" grep -qF "$INSTALL_SH" "$SUPPORT/update.command"
check "the update script keeps the kit's URL" grep -q "${ENV}_KIT_URL" "$SUPPORT/update.command"
if [ $do_update = 0 ]; then
  check "the update script is opened in Terminal" grep -q "update.command" "$api/opened"
  PATH=${PATH#"$api/bin:"}
else
  echo "      (the update: a rebuild in the VM's Terminal, up to an hour)"
  for _ in $(seq 360); do
    [ -f "$R/kit-version" ] && [ "$(stat -f %m "$R/kit-version" 2>/dev/null || echo 0)" != "$before" ] && break
    sleep 10
  done
  check "updated: the app was rebuilt" test "$(stat -f %m "$R/kit-version" 2>/dev/null || echo 0)" != "$before"
  sleep 20; stop_game  # build.sh starts the new app
fi
ue TEST_ANSWER; ue UPDATE_API; ue INSTALL_URL

# --- started from Finder: the log, and the app menu
touch "$stamp"  # open passes on no environment: a fresh stamp skips the check
rm -f "$LOG"
open "$APP_PATH"
for _ in $(seq 60); do grep -q "app menu:" "$LOG" 2>/dev/null && break; sleep 1; done
check "started from Finder: the game is running" game_running
check "started from Finder: output in ~/Library/Logs/$ID.log" test -s "$LOG"
check "the runtime added the app menu's actions" grep -q "app menu: Check for Updates" "$LOG"
grep "app menu:" "$LOG" | head -1 | sed 's/^/      /'

# --- Show Saved Games, Show Logs (Finder windows)
check "Show Saved Games" /bin/bash "$R/player.sh" saves
check "Show Logs" /bin/bash "$R/player.sh" logs
stop_game
check "the game stopped" sh -c '! pgrep -f "$0/Contents/MacOS/$1" > /dev/null' "$APP_PATH" "$BINARY"

# --- install.sh --uninstall, answering its question
mkdir -p "$SUPPORT/drive_c"
echo save > "$SUPPORT/drive_c/vm-app-check.sav"
game_dir_before=$(cat "$SUPPORT/game-dir" 2>/dev/null)
expect -f - "$INSTALL_SH" <<'EOF' > "$api/uninstall.log" 2>&1
set timeout 60
spawn /bin/bash -c "cat '[lindex $argv 0]' | bash -s -- --uninstall"
expect {
  -re {Uninstall\? \[y/N\]} { send "y\r"; exp_continue }
  -re {Delete\? \[y/N\]} { send "n\r"; exp_continue }
  eof
}
EOF
sed 's/^/      /' "$api/uninstall.log" | tail -6
check "uninstalled: the app is gone" test ! -e "$APP_PATH"
check "uninstalled: the kit is gone" test ! -e "$SUPPORT/kit"
check "uninstalled: the saved games stay" test -f "$SUPPORT/drive_c/vm-app-check.sav"
check "uninstalled: the game folder setting stays" test "$(cat "$SUPPORT/game-dir" 2>/dev/null)" = "$game_dir_before"
rm -rf "$api"
echo "$fails failed"
[ $fails = 0 ]
