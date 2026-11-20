#!/bin/bash
# Builds the game's app from the player's own copy of the game, with the
# prebuilt kit this script ships in (SwARc's player/kit.py makes the kit; the
# game's install.sh downloads it and runs this). Needs only Apple's command
# line tools, which it installs. Which game, and what it needs, is in game.env
# beside it (written by kit.py from the game's recomplauncher.json).
#   build.sh [--game-dir FOLDER] [--supply FILE]... [--no-launch]
# Finds the game (an existing install, or downloads it with your Steam
# account), translates it to C++, compiles it, and installs the app in
# /Applications (or ~/Applications). Log: ~/Library/Logs/<id>-build.log.
set -uo pipefail
KIT=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=/dev/null
. "$KIT/game.env"

DD_URL=https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_3.4.0/DepotDownloader-macos-arm64.zip
DD_SHA=60e80c7c496f3f9a079cd3c62036b35d088c27bc0149baf38f009eb57a52f6a5
SUPPORT="$HOME/Library/Application Support/$ID"
SAVED_DIR_FILE="$SUPPORT/game-dir"
STEAM_GAME_DIR="$SUPPORT/$STEAM_FOLDER"
WORK="$HOME/Library/Caches/$ID-build"
LOG="$HOME/Library/Logs/$ID-build.log"

game_dir_arg="" launch=1 supplied_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir) game_dir_arg=${2:-}; shift 2 ;;
    --supply|--exe) supplied_args+=("${2:-}"); shift 2 ;;
    --no-launch) launch=0; shift ;;
    -h|--help) sed -n '7,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1 (try --help)"; exit 2 ;;
  esac
done

bold=$'\033[1m' red=$'\033[31m' green=$'\033[32m' yellow=$'\033[33m' off=$'\033[0m'
say()  { printf '\n%s==> %s%s\n' "$bold" "$*" "$off"; }
ok()   { printf '    %s✓%s %s\n' "$green" "$off" "$*"; }
warn() { printf '    %s!%s %s\n' "$yellow" "$off" "$*"; }
die() {
  printf '\n%sStopped:%s %s\n' "$red" "$off" "$1"
  [ $# -gt 1 ] && printf '%s\n' "${@:2}"
  printf '\nWhen you have fixed this, run it again; it picks up where it stopped.\n'
  exit 1
}
ask() { printf '%s ' "$1"; { read -r REPLY < /dev/tty; } 2>/dev/null || REPLY=""; }
yes_no() {  # yes_no "question" default(y|n)
  local hint="[Y/n]"; [ "$2" = n ] && hint="[y/N]"
  ask "    $1 $hint"
  case "${REPLY:-$2}" in [Yy]*) return 0 ;; *) return 1 ;; esac
}
run() {  # run "what" command...: quietly, the end of the log on failure
  local what=$1; shift
  printf '\n--- %s: %s\n' "$what" "$*" >> "$LOG"
  if ! "$@" >> "$LOG" 2>&1; then
    printf '\n%s--- last lines of the log ---%s\n' "$yellow" "$off"
    tail -25 "$LOG"
    die "$what failed." "The full log is $LOG." \
        "If you can't tell what went wrong, open an issue on https://github.com/$REPO/issues and attach that file."
  fi
}
sha256() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }
env_of() { eval "printf '%s' \"\${${ENV}_$1:-}\""; }  # env_of GAME_DIR -> $SOLDIERS_GAME_DIR

BUNDLE_ID=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$KIT/app/Info.plist")
mkdir -p "$SUPPORT" "$(dirname "$LOG")"
printf '\n=== build %s, kit %s ===\n' "$(date)" "$(head -1 "$KIT/VERSION" 2>/dev/null)" >> "$LOG"
echo "${bold}$NAME — native Mac build${off}"
echo "This builds the game from your own copy and installs $APP.app in your Applications folder."

# --- This Mac --------------------------------------------------------------
say "Checking this Mac"
[ "$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ] ||
  die "this needs a Mac with Apple Silicon (M1 or newer). Intel Macs are not supported yet."
macos=$(sw_vers -productVersion) need=$(cat "$KIT/MIN_MACOS")
[ "$(printf '%s\n%s\n' "$need" "$macos" | sort -V | head -1)" = "$need" ] ||
  die "this needs macOS $need or newer; this Mac has $macos."
free_gb=$(( $(df -k "$HOME" | awk 'NR==2 {print $4}') / 1024 / 1024 ))
[ "$free_gb" -ge "$DISK_GB" ] ||
  die "there are only $free_gb GB free; the game and the build need about $DISK_GB GB."
ok "macOS $macos on Apple Silicon, $free_gb GB free"

# --- Apple's command line tools (the compiler) -----------------------------
have_clt() { xcode-select -p > /dev/null 2>&1 && xcrun --find clang > /dev/null 2>&1; }
if ! have_clt; then
  say "Installing Apple's command line developer tools"
  xcode-select --install > /dev/null 2>&1 || true
  echo "    A window asks to install them: click Install, then Agree. This takes 5–15 minutes;"
  echo "    this continues by itself when they're installed."
  waited=0
  until have_clt; do
    sleep 10; waited=$((waited + 10))
    if [ $waited -ge 30 ] && ! pgrep -f "Install Command Line Developer Tools" > /dev/null; then
      have_clt && break
      die "the command line developer tools aren't installed (the install window was closed)." \
          "Run this again and click Install in the window that appears."
    fi
  done
fi
ok "Apple's command line tools"

# --- Your copy of the game -------------------------------------------------
say "Finding your copy of the game"
is_game_dir() {
  local f
  for f in "${FILES[@]}"; do
    case "$f" in */) [ -d "$1/$f" ] || return 1 ;; *) [ -f "$1/$f" ] || return 1 ;; esac
  done
}
is_supported() {  # every pinned file matches its hash
  local i
  for ((i = 0; i < HASH_N; i++)); do
    [ "$(sha256 "$1/${HASH_FILES[$i]}")" = "${HASH_SHAS[$i]}" ] || return 1
  done
}
# $FIND files under a Wine bottle, a wrapper app or a plain folder.
search_in() {
  local base=$1 drives=() d
  [ -d "$base" ] || return 0
  for d in "$base"/drive_c "$base"/*/drive_c "$base"/Contents/SharedSupport/prefix/drive_c \
           "$base"/*.app/Contents/SharedSupport/prefix/drive_c "$base"/*/*.app/Contents/SharedSupport/prefix/drive_c; do
    [ -d "$d" ] && drives+=("$d")
  done
  [ ${#drives[@]} -eq 0 ] && drives=("$base")
  find "${drives[@]}" -maxdepth 7 -type f -name "$FIND" 2>/dev/null
}
# Supported installs: Wine, CrossOver, Whisky, Kegworks/Wineskin, Spotlight.
find_game() {
  {
    echo "$STEAM_GAME_DIR/$FIND"
    search_in "$HOME/Applications"
    search_in /Applications
    search_in "$HOME/Library/Application Support/CrossOver/Bottles"
    search_in "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Bottles"
    search_in "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Data/Bottles"
    search_in "$HOME/.wine"
    search_in "$HOME/Games"
    mdfind -name "$FIND" 2>/dev/null
  } | grep "/${FIND//./\\.}\$" | while IFS= read -r f; do
    d=$(dirname "$f"); is_game_dir "$d" && is_supported "$d" && { echo "$d"; break; }
  done
}
clean_path() {  # what Terminal pastes for a dragged file or folder -> a path
  local p=$1
  p="${p#"${p%%[![:space:]]*}"}"; p="${p%"${p##*[![:space:]]}"}"
  p="${p#[\'\"]}"; p="${p%[\'\"]}"
  p=$(printf '%s' "$p" | sed 's/\\\(.\)/\1/g')
  printf '%s' "${p%/}"
}
pick_folder() {
  osascript - "$NAME" "$FIND" <<'EOF' 2>/dev/null
on run argv
  POSIX path of (choose folder with prompt "Choose the folder of " & item 1 of argv & " (the one with " & item 2 of argv & " in it). A Wine bottle or wrapper app that has the game installed works too.")
end run
EOF
}
steam_download() {
  local dd="$SUPPORT/DepotDownloader"
  if [ ! -x "$dd/DepotDownloader" ]; then
    echo "    Getting DepotDownloader (https://github.com/SteamRE/DepotDownloader)..."
    mkdir -p "$dd"
    run "Downloading DepotDownloader" curl -fsSL -o "$dd/dd.zip" "$DD_URL"
    [ "$(sha256 "$dd/dd.zip")" = "$DD_SHA" ] ||
      die "the DepotDownloader download doesn't match the expected file." "Try again later."
    run "Unpacking DepotDownloader" unzip -o -q "$dd/dd.zip" -d "$dd"
    rm -f "$dd/dd.zip"
  fi
  echo
  echo "    Steam will show a QR code here. Open the Steam app on your phone, tap the shield"
  echo "    (Steam Guard) and choose \"Scan a QR code\", then approve the sign-in. Your password"
  echo "    never passes through this setup. Then the game downloads ($STEAM_SIZE)."
  echo
  mkdir -p "$STEAM_GAME_DIR"
  # Not run(): the QR code and the progress must show, on the terminal itself.
  # Every depot of the game in one call (one sign-in), into one folder, as
  # Steam installs them.
  "$dd/DepotDownloader" -app "$STEAM_APP" -depot "${STEAM_DEPOTS[@]}" -manifest "${STEAM_MANIFESTS[@]}" \
    -os windows -qr -dir "$STEAM_GAME_DIR" < /dev/tty ||
    die "the download from Steam didn't finish." "Run this again to retry; it continues where it stopped."
}
what_files() { local IFS=,; printf '%s' "${FILES[*]}" | sed 's/,/, /g; s/\/$//; s/\/,/,/g'; }

game=""
for candidate in "$game_dir_arg" "$(env_of GAME_DIR)" "$(head -1 "$SAVED_DIR_FILE" 2>/dev/null)"; do
  [ -n "$candidate" ] && candidate=$(clean_path "$candidate")
  if [ -n "$candidate" ] && is_game_dir "$candidate"; then game=$candidate; break; fi
  [ -n "$candidate" ] && [ "$candidate" = "$(clean_path "$game_dir_arg")" ] &&
    die "there's no game in $candidate ($(what_files))."
done
[ -n "$game" ] || { echo "    Looking for an installed copy..."; game=$(find_game); }
if [ -z "$game" ]; then
  echo "    No installed copy found. You need $STEAM_TITLE on your Steam account;"
  echo "    it's downloaded straight from Steam, without installing Steam."
  if yes_no "Download it with your Steam account?" y; then
    steam_download
    is_game_dir "$STEAM_GAME_DIR" || die "the download from Steam didn't include the game." \
      "Check that this Steam account owns $STEAM_TITLE, then run this again."
    game=$STEAM_GAME_DIR
  fi
fi
tried_picker=0
while [ -z "$game" ]; do
  REPLY=""
  if [ $tried_picker = 0 ]; then
    tried_picker=1
    echo "    A Finder window is open: choose the game's folder there (it contains $FIND)."
    REPLY=$(pick_folder)
  fi
  if [ -z "$REPLY" ]; then
    echo "    Drag the game's folder (it contains $FIND) into this window and press Return."
    ask "    Game folder:"
  fi
  [ -n "$REPLY" ] || die "no game folder given."
  p=$(clean_path "$REPLY")
  [ -f "$p" ] && p=$(dirname "$p")
  if is_game_dir "$p"; then
    game=$p
  else
    inside=$(search_in "$p" | head -1)
    if [ -n "$inside" ] && is_game_dir "$(dirname "$inside")"; then game=$(dirname "$inside")
    else warn "No $(what_files) in $p."; fi
  fi
done
game=${game%/}
build_note=""; [ -n "$STEAM_BUILD" ] && build_note=" (Steam build $STEAM_BUILD)"
is_supported "$game" ||
  die "the game in $game is not the version of $STEAM_TITLE this build is for$build_note." \
      "$OTHER_VERSIONS Let this download the game from Steam" \
      "instead: run it again and choose the Steam download when no installed copy is found."
printf '%s\n' "$game" > "$SAVED_DIR_FILE"
ok "$game"

# --- Files you supply ------------------------------------------------------
# Some games' Steam executables have their code encrypted by Steam's DRM; the
# build then needs a readable copy, which the player provides (this script
# doesn't make it). It goes in the game folder, where the game loads it from.
for ((i = 0; i < SUPPLIED_N; i++)); do
  want=${SUPPLIED_FILES[$i]} want_sha=${SUPPLIED_SHAS[$i]}
  say "Finding $want"
  file=""
  candidates=("${supplied_args[@]+"${supplied_args[@]}"}")
  [ "$i" = 0 ] && candidates+=("$(env_of EXE)")
  candidates+=("$game/$want")
  for candidate in "${candidates[@]}"; do
    [ -n "$candidate" ] || continue
    candidate=$(clean_path "$candidate")
    [ -d "$candidate" ] && candidate="$candidate/$want"
    if [ -f "$candidate" ] && [ "$(sha256 "$candidate")" = "$want_sha" ]; then file=$candidate; break; fi
  done
  if [ -z "$file" ]; then
    echo "    This build needs $want: ${SUPPLIED_ABOUT[$i]}."
    echo "    You supply that file; it isn't in $game."
    echo "    Drag it into this window and press Return (or press Return to stop)."
    ask "    $want:"
    [ -n "$REPLY" ] && file=$(clean_path "$REPLY")
  fi
  [ -n "$file" ] && [ -f "$file" ] ||
    die "$want is missing." \
        "Put it in the game folder ($game), or pass its path:" \
        "  $KIT/build.sh --supply /path/to/$want" \
        "You supply this file; see https://github.com/$REPO#readme."
  [ "$(sha256 "$file")" = "$want_sha" ] ||
    die "$file is not the $want this build is for." "Its sha-256 must be $want_sha."
  if [ "$file" != "$game/$want" ]; then
    run "Copying $want into the game folder" cp "$file" "$game/$want"
  fi
  ok "$game/$want"
done

# --- Build -----------------------------------------------------------------
say "Building the game ($BUILD_TIME)"
rm -rf "$WORK"; mkdir -p "$WORK/obj"
names=$(printf '%s, ' "${LIFT_FILES[@]}"); names=${names%, }
echo "    Translating $names to C++..."
lift=(--exe) args=()
for ((i = 0; i < LIFT_N; i++)); do
  img=${LIFT_FILES[$i]} sym="$WORK/symbols-${LIFT_FILES[$i]//\//_}"  # images can be in subfolders
  run "Reading $img" /usr/bin/python3 -I "$KIT/bin/pe_symbols.py" "$game/$img" --out "$sym" --seeds --quiet
  [ $i -gt 0 ] && lift=(--module)
  args+=("${lift[@]}" "$game/$img" --symbols "$sym" --manifest "$KIT/data/${LIFT_MANIFESTS[$i]}")
  [ -n "${LIFT_BASES[$i]}" ] && args+=(--base "${LIFT_BASES[$i]}")
done
run "Translating" "$KIT/bin/recomp_lift" "${LIFT_ARGS[@]+"${LIFT_ARGS[@]}"}" "${args[@]}" --out "$WORK/generated"
SDK=$(xcrun --show-sdk-path)
CXX=$(xcrun --find clang++)
lifted_flags=()
while IFS= read -r f; do [ -n "$f" ] && lifted_flags+=("$f"); done < "$KIT/flags/lifted.txt"
# The SDK's own C++ headers: older command line tools leave stale ones beside the compiler.
lifted_flags+=(-I "$KIT/include" -isysroot "$SDK" -nostdinc++ -isystem "$SDK/usr/include/c++/v1")
sources=("$WORK"/generated/*.cpp "$KIT/src/lifted_table.cpp")
jobs=$(/usr/sbin/sysctl -n hw.ncpu)
echo "    Compiling ${#sources[@]} files on $jobs cores..."
started=$SECONDS
export CXX WORK
printf '%s\0' "${sources[@]}" |
  xargs -0 -P "$jobs" -I{} sh -c '"$CXX" "$@" -c "$0" -o "$WORK/obj/$(basename "$0").o" || { echo "FAILED: $0"; exit 255; }' \
    {} "${lifted_flags[@]}" >> "$LOG" 2>&1 &
compile=$!
# Progress, in place on a terminal: files done of all, and the time so far.
total=${#sources[@]}
while kill -0 $compile 2>/dev/null; do
  if [ -t 1 ]; then
    done_n=$(find "$WORK/obj" -name '*.o' | wc -l | tr -d ' ')
    printf '\r    %d of %d files (%d%%), %d:%02d so far ' "$done_n" "$total" $((done_n * 100 / total)) \
      $(((SECONDS - started) / 60)) $(((SECONDS - started) % 60))
  fi
  sleep 2
done
[ -t 1 ] && printf '\r\033[K'
wait $compile || { tail -25 "$LOG"; die "compiling failed." "The full log is $LOG."; }
link_flags=() libs=()
while IFS= read -r f; do [ -n "$f" ] && link_flags+=("$f"); done < "$KIT/flags/link.txt"
while IFS= read -r f; do [ -n "$f" ] && libs+=("$KIT/lib/$f"); done < "$KIT/flags/libs.txt"
echo "    Linking..."
B="$WORK/$APP.app/Contents"
mkdir -p "$B/MacOS" "$B/Frameworks" "$B/Resources"
run "Linking" "$CXX" "${link_flags[@]}" -isysroot "$SDK" -o "$B/MacOS/$BINARY" \
  "$WORK"/obj/*.o "$KIT"/obj/*.o "${libs[@]}" -Wl,-rpath,@executable_path/../Frameworks
cp "$KIT/app/Info.plist" "$B/Info.plist"
# Started from Finder, the game has no --game-dir: this starter passes the
# folder found above (saved in $SAVED_DIR_FILE, which a later run updates).
# Its own name: the disk ignores case, so "Soldiers" would be "soldiers".
cat > "$B/MacOS/launcher" <<EOF
#!/bin/bash
here=\$(cd "\$(dirname "\$0")" && pwd)
dir=\${${ENV}_GAME_DIR:-\$(head -1 "\$HOME/Library/Application Support/$ID/game-dir" 2>/dev/null)}
exec "\$here/$BINARY" --game-dir "\$dir" "\$@"
EOF
chmod 755 "$B/MacOS/launcher"
run "Configuring the app" /usr/libexec/PlistBuddy -c "Set :CFBundleExecutable launcher" "$B/Info.plist"
cp "$KIT/VERSION" "$B/Resources/kit-version"
cp "$KIT"/lib/*.dylib "$B/Frameworks/"
cp "$KIT/licenses/THIRD-PARTY.txt" "$B/Resources/"
run "Signing" codesign --force --sign - "$B/MacOS/$BINARY"
run "Signing" codesign --force --sign - "$WORK/$APP.app"
ok "Built in $(( (SECONDS - started) / 60 )) min $(( (SECONDS - started) % 60 )) s"

# --- Install ---------------------------------------------------------------
ours() { [ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$1/Contents/Info.plist" 2>/dev/null)" = "$BUNDLE_ID" ]; }
# Where it was installed before, else /Applications when this account can
# write there, else ~/Applications. RECOMP_APPS_DIR overrides (for tests).
apps=${RECOMP_APPS_DIR:-}
if [ -z "$apps" ]; then
  for d in /Applications "$HOME/Applications"; do
    if [ -d "$d/$APP.app" ] && ours "$d/$APP.app"; then apps=$d; break; fi
  done
fi
if [ -z "$apps" ]; then
  if [ -w /Applications ] && [ ! -e "/Applications/$APP.app" ]; then apps=/Applications; else apps="$HOME/Applications"; fi
fi
APP_PATH="$apps/$APP.app"
if [ -e "$APP_PATH" ] && ! ours "$APP_PATH"; then
  die "$APP_PATH already exists and isn't this game." "Rename or move it, then run this again."
fi
mkdir -p "$apps"
rm -rf "$APP_PATH"
run "Installing" mv "$WORK/$APP.app" "$APP_PATH"
rm -rf "$WORK"  # the translated code: derived from the game, and large
ok "$APP_PATH"
echo
if [ "$apps" = "$HOME/Applications" ]; then
  echo "${green}${bold}Done.${off} ${bold}$APP${off} is in the Applications folder inside your home folder"
  echo "(~/Applications). To open it in Finder: Go > Home, then open Applications."
else
  echo "${green}${bold}Done.${off} ${bold}$APP${off} is in $apps."
fi
echo "Or press Cmd-Space, type $APP and press Return."
if [ $launch = 1 ]; then
  echo "Starting the game..."
  open "$APP_PATH"
fi
