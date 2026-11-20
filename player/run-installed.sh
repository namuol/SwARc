#!/bin/bash
# Runs an installed player app for a limited time, as a game's
# tools/try-run.sh runs its dev build: the run script tools/smoke.py uses for
# `--run` (player/vm-test.sh runs a game's smoke cases on the app the player
# install made with it).
#   RECOMP_APP=/Applications/Game.app RECOMP_PREFIX=GAME run-installed.sh [SECONDS] [-- GAME ARGS...]
# Runs from the current folder (the game's repository, or a copy of it). The
# log goes to $<PREFIX>_RUN_LOG, else build/run.log. The app's own starter
# passes the game folder the install saved.
set -u
secs=${1:-30}
shift || true
[ "${1:-}" = -- ] && shift
app=${RECOMP_APP:?RECOMP_APP: the installed app}
exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print CFBundleExecutable' "$app/Contents/Info.plist")"
log_var="${RECOMP_PREFIX:?RECOMP_PREFIX: the variable prefix of the game}_RUN_LOG"
log=${!log_var:-build/run.log}
mkdir -p "$(dirname "$log")"
args=(); [ $# -gt 0 ] && args=(-- "$@")
"$exe" "${args[@]+"${args[@]}"}" > "$log" 2>&1 &
pid=$!
for _ in $(seq "$secs"); do
  kill -0 $pid 2>/dev/null || break
  /bin/sleep 1
done
if kill -0 $pid 2>/dev/null; then
  echo "still running after ${secs}s, stopping it"
  # The starter execs the game, so $pid is the game itself.
  kill $pid; /bin/sleep 1; kill -9 $pid 2>/dev/null
else
  wait $pid; echo "exited with $?"
fi
echo "--- tail"
tail -${TAIL:-25} "$log"
