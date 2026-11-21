#!/bin/bash
# What the installed app does besides the game: the actions SwARc's runtime
# puts in its app menu, and the update check its starter makes at launch.
# build.sh puts this in the app's Contents/Resources with the kit's game.env
# and VERSION (kit-version).
#   player.sh check [PID]       Check for Updates: says whether a newer release is out, offers it
#   player.sh saves             Show Saved Games: the game's folder in Application Support, in Finder
#   player.sh logs              Show Logs: the game's log in Finder
#   player.sh uninstall [PID]   Uninstall: deletes the app and its kit, keeps the saved games
#   player.sh uninstall --terminal   the same, asking in the terminal (install.sh --uninstall)
#   player.sh uninstall --yes [--delete-game]   without questions
#   player.sh launch            at launch: once a day, offers a newer release; exits 10 if the
#                               player chose to update (the game then doesn't start)
# PID is the running game's: updating and uninstalling quit it first.
# Updating reruns the game's install.sh in Terminal (it can ask questions
# there). <ENV>_UPDATE_API stands in for https://api.github.com/repos/<repo>
# (a file:// folder with releases/latest and compare/<rev>...<tag>, for
# tests); <ENV>_NO_UPDATE_CHECK=1, <ENV>_BACKGROUND or <ENV>_HEADLESS skip
# the check at launch. <ENV>_INSTALL_URL is where updating gets install.sh
# (file:// works), <ENV>_KIT_URL the kit it downloads. <ENV>_TEST_ANSWER=BUTTON answers every question with
# BUTTON, showing nothing (player/vm-test.sh).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=/dev/null
. "$HERE/game.env"
APP_PATH=$(cd "$HERE/../.." && pwd)
SUPPORT="$HOME/Library/Application Support/$ID"
LOG="$HOME/Library/Logs/$ID.log"
BUILD_LOG="$HOME/Library/Logs/$ID-build.log"
env_of() { eval "printf '%s' \"\${${ENV}_$1:-}\""; }  # env_of GAME_DIR -> $SOLDIERS_GAME_DIR
API=$(env_of UPDATE_API)
API=${API:-https://api.github.com/repos/$REPO}

# dialog TEXT BUTTON...: shows TEXT with the buttons (the last is the default,
# the first is what Escape presses) and prints the one pressed. With
# TERMINAL=1, a question in the terminal: yes is the last button.
TERMINAL=0
dialog() {
  local answer
  answer=$(env_of TEST_ANSWER)
  if [ -n "$answer" ]; then
    echo "question: $1 -> $answer" >&2
    echo "$answer"
    return 0
  fi
  if [ $TERMINAL = 1 ]; then
    local no=$2 yes=${!#}
    printf '\n%s\n' "$1" > /dev/tty
    [ $# = 2 ] && return 0
    printf '%s? [y/N] ' "$yes" > /dev/tty
    read -r REPLY < /dev/tty
    case "$REPLY" in [Yy]*) echo "$yes" ;; *) echo "$no" ;; esac
    return 0
  fi
  osascript - "$NAME" "$@" <<'EOF' 2>/dev/null
on run argv
  set t to item 2 of argv
  set bs to items 3 thru -1 of argv
  try
    return button returned of (display dialog t with title (item 1 of argv) buttons bs ¬
      default button (count of bs) cancel button 1)
  on error
    return item 1 of bs
  end try
end run
EOF
}
say() { dialog "$1" OK > /dev/null; }

# This build's game commit: the kit's VERSION has git describe's name for it,
# then SwARc's commit, then (newer kits) the full commit.
this_rev() {
  local rev
  rev=$(sed -n 3p "$HERE/kit-version" 2>/dev/null)
  [ -n "$rev" ] || rev=$(head -1 "$HERE/kit-version" 2>/dev/null)
  printf '%s' "${rev%-dirty}"
}
json() { plutil -extract "$1" raw -o - - 2>/dev/null; }  # json KEY < JSON
get() { curl -fsS --max-time 5 "$API/$1" 2>/dev/null; }

# Sets TAG to the latest release and STATUS to how it relates to this build:
# newer, same, older, unknown (this build's commit isn't on GitHub), or
# offline (no answer, or no release).
compare() {
  TAG=$(get releases/latest | json tag_name) STATUS=offline
  [ -n "$TAG" ] || return 0
  local rev s
  rev=$(this_rev)
  s=$(get "compare/$rev...$TAG" | json status)
  case "$s" in
    ahead|diverged) STATUS=newer ;;  # the release has commits this build lacks
    identical) STATUS=same ;;
    behind) STATUS=older ;;
    *) STATUS=unknown ;;
  esac
}

# Quits the game (PID) and reruns the installer in Terminal once it's gone.
update() {
  local pid=${1:-} cmd="$SUPPORT/update.command" url install
  url=$(env_of KIT_URL) install=$(env_of INSTALL_URL)
  install=${install:-https://raw.githubusercontent.com/$REPO/main/install.sh}
  mkdir -p "$SUPPORT"
  {
    echo '#!/bin/bash'
    echo "# $APP's Check for Updates wrote this: it reruns the installer, which"
    echo "# downloads the newest build kit and rebuilds $APP.app."
    [ -n "$pid" ] && echo "while kill -0 $pid 2>/dev/null; do sleep 1; done"
    [ -n "$url" ] && printf 'export %s_KIT_URL=%q\n' "$ENV" "$url"
    printf 'curl -fsSL %q | bash\n' "$install"
  } > "$cmd"
  chmod 755 "$cmd"
  open -a Terminal "$cmd"
  [ -n "$pid" ] && kill "$pid" 2>/dev/null
  return 0
}

stop_game() {  # stop_game PID: asks it to quit, then makes sure
  local pid=${1:-} i
  [ -n "$pid" ] || return 0
  kill "$pid" 2>/dev/null || return 0
  for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || return 0; sleep 0.5; done
  kill -9 "$pid" 2>/dev/null
  return 0
}

case "${1:-}" in
launch)
  [ -n "$(env_of BACKGROUND)$(env_of HEADLESS)$(env_of NO_UPDATE_CHECK)" ] && exit 0
  stamp="$SUPPORT/update-checked"
  # Once a day: nothing when the last check was less than a day ago.
  [ -f "$stamp" ] && [ -z "$(find "$stamp" -mtime +0 2>/dev/null)" ] && exit 0
  mkdir -p "$SUPPORT" && touch "$stamp"
  compare
  [ "$STATUS" = newer ] || exit 0
  [ "$(dialog "A new version of $NAME is out ($TAG).

Updating rebuilds the game from your copy of it, which takes $BUILD_TIME. Your saved games stay." \
       Later Update)" = Update ] || exit 0
  update
  exit 10
  ;;
check)
  pid=${2:-}
  compare
  case "$STATUS" in
  newer)
    [ "$(dialog "A new version of $NAME is out ($TAG).

Updating quits the game (save your progress first), then rebuilds it from your copy in Terminal, which takes $BUILD_TIME. Your saved games stay." \
         Later Update)" = Update ] && update "$pid" ;;
  same|older) say "$NAME is up to date ($TAG is the newest release)." ;;
  unknown) say "This build of $NAME ($(head -1 "$HERE/kit-version")) isn't a release, so there's nothing to compare it with. The newest release is $TAG." ;;
  *) say "Couldn't find out whether there's a newer version of $NAME: no answer from GitHub, or no releases yet. https://github.com/$REPO/releases has them." ;;
  esac
  ;;
saves)
  mkdir -p "$SUPPORT"
  open "$SUPPORT"
  ;;
logs)
  for f in "$LOG" "$BUILD_LOG"; do
    [ -f "$f" ] && { open -R "$f"; exit 0; }
  done
  open "$HOME/Library/Logs"
  ;;
uninstall)
  shift
  yes=0 delete_game=0 pid=""
  for a in "$@"; do
    case "$a" in
    --yes) yes=1 ;; --delete-game) delete_game=1 ;; --terminal) TERMINAL=1 ;; *) pid=$a ;;
    esac
  done
  # Only ever the app this script is in.
  [ "$(basename "$APP_PATH")" = "$APP.app" ] || { echo "not in $APP.app: $APP_PATH" >&2; exit 1; }
  steam="$SUPPORT/$STEAM_FOLDER"
  if [ $yes = 0 ]; then
    [ "$(dialog "Uninstall $NAME?

This quits the game and deletes $APP_PATH and its build files. Your saved games and settings stay in $SUPPORT, for if you install it again." \
         Cancel Uninstall)" = Uninstall ] || exit 0
    if [ -d "$steam" ]; then
      size=$(du -sh "$steam" 2>/dev/null | cut -f1)
      [ "$(dialog "Also delete the copy of $STEAM_TITLE downloaded from Steam (${size:-several GB})?

Keep it to reinstall without downloading it again." Keep Delete)" = Delete ] && delete_game=1
    fi
  fi
  stop_game "$pid"
  rm -rf "$APP_PATH" "$SUPPORT/kit" "$HOME/Library/Caches/$ID-build" \
    "$SUPPORT/update-checked" "$SUPPORT/update.command"
  if [ $delete_game = 1 ] && [ -d "$steam" ]; then
    rm -rf "$steam"
    [ "$(head -1 "$SUPPORT/game-dir" 2>/dev/null)" = "$steam" ] && rm -f "$SUPPORT/game-dir"
  fi
  if [ $yes = 0 ] && [ $TERMINAL = 0 ]; then
    say "$NAME is uninstalled. Your saved games and settings are in $SUPPORT."
  else
    echo "Uninstalled $APP_PATH. Saved games and settings: $SUPPORT"
  fi
  ;;
*)
  sed -n '6,14p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
  ;;
esac
