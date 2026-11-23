#!/bin/bash
# A game repository's developer commands, run as ./recomp in it (the game's
# generated recomp starts this, from the SwARc checkout it pins).
#   ./recomp setup              Homebrew packages, SwARc at the pin, generated files
#   ./recomp status             SwARc, the generated files, the kit, the installed app
#   ./recomp sync               after pulling: SwARc at the pin, generated files
#   ./recomp kit [--force]      the kit (dist/), only if it's out of date
#   ./recomp install [ARGS]     the kit if needed, then install.sh with it, as a player
#                               would (ARGS go to build.sh: --no-launch, --game-dir DIR)
#   ./recomp test [ARGS]        the kit if needed, then vm-test.sh on this repo
#                               (ARGS: --keep-base, --keep, --update, --steam)
#   ./recomp uninstall          install.sh --uninstall
#   ./recomp run [GAME ARGS]    build the developer build (out/release) if anything changed, then
#                               play it on game/ (or <ENV>_GAME_DIR); GAME ARGS go to the game
#   ./recomp pin [REF]          pin SwARc's REF (default: the fork's main), then sync
# kit, install and test stop when external/SwARc isn't the pinned commit or
# has uncommitted changes (a kit says which SwARc made it); --allow-unpinned
# goes ahead anyway, for trying SwARc changes before landing them.
set -uo pipefail
SWARC=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$SWARC/../.." && pwd)
PLAYER="$SWARC/player"
cd "$ROOT"
bold=$'\033[1m' red=$'\033[31m' green=$'\033[32m' yellow=$'\033[33m' off=$'\033[0m'
[ -t 1 ] || bold="" red="" green="" yellow="" off=""
die() { printf '%srecomp:%s %s\n' "$red" "$off" "$*" >&2; exit 1; }
note() { printf '%s==>%s %s\n' "$bold" "$off" "$*"; }
usage() { sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'; }

env_out=$(/usr/bin/python3 -I "$PLAYER/kit.py" env --root "$ROOT") || die "kit.py env failed (recomplauncher.json?)"
eval "$env_out"

pin() { sed -n 's/^PIN=//p' tools/swarc.sh; }
swarc_head() { git -C "$SWARC" rev-parse HEAD; }
swarc_dirty() { [ -n "$(git -C "$SWARC" status --porcelain)" ]; }
# Why the SwARc checkout isn't the pinned commit, or nothing.
unpinned() {
  local p h; p=$(pin) h=$(swarc_head)
  if [ "$h" != "$p" ]; then echo "external/SwARc is at ${h:0:7}, not the pinned ${p:0:7}"
  elif swarc_dirty; then echo "external/SwARc has uncommitted changes"
  fi
}
need_pinned() {  # need_pinned ARGS...: sets ARGS without --allow-unpinned
  local allow=0 a; ARGS=()
  for a in "$@"; do [ "$a" = --allow-unpinned ] && allow=1 || ARGS+=("$a"); done
  local why; why=$(unpinned)
  if [ -n "$why" ]; then
    [ $allow = 1 ] || die "$why. Run ./recomp sync (or ./recomp pin), or pass --allow-unpinned to use it as it is."
    printf '%s!%s %s (--allow-unpinned)\n' "$yellow" "$off" "$why"
  fi
}
generated_stale() { ! /usr/bin/python3 -I "$PLAYER/kit.py" sync --check --root "$ROOT" > /dev/null 2>&1; }
# Why the kit in dist/ isn't this checkout's, or nothing.
kit_stale() {
  local v="dist/$KIT_NAME/VERSION"
  [ -f "dist/$ASSET" ] && [ -f "$v" ] || { echo "there's no kit in dist/"; return; }
  local rev sw sha; rev=$(sed -n 1p "$v") sw=$(sed -n 2p "$v") sha=$(sed -n 3p "$v")
  [ "$sha" = "$(git rev-parse HEAD)" ] || { echo "it was built from commit ${sha:0:7}, not ${HEAD_SHORT}"; return; }
  [ "$sw" = "$(git -C "$SWARC" rev-parse --short HEAD)" ] || { echo "it was built with SwARc $sw, not $(git -C "$SWARC" rev-parse --short HEAD)"; return; }
  case "$rev" in *-dirty) echo "it was built from uncommitted changes"; return ;; esac
  git diff --quiet HEAD -- || { echo "this checkout has uncommitted changes"; return; }
  swarc_dirty && { echo "external/SwARc has uncommitted changes"; return; }
}
HEAD_SHORT=$(git rev-parse --short HEAD)
# The installed app: one in an Applications folder whose game.env is this game's.
installed() {
  local a
  for a in /Applications/*.app "$HOME"/Applications/*.app; do
    [ -f "$a/Contents/Resources/game.env" ] && (. "$a/Contents/Resources/game.env" && [ "$ID" = "$1" ]) &&
      { echo "$a"; return 0; }
  done
  return 1
}
sync_generated() {
  local out; out=$(/usr/bin/python3 -I "$PLAYER/kit.py" sync --root "$ROOT") || die "kit.py sync failed"
  [ -n "$out" ] && printf '%s\n%s(commit them with the change that caused it)%s\n' "$out" "$yellow" "$off"
  return 0
}

cmd_setup() {
  xcode-select -p > /dev/null 2>&1 || die "Apple's command line tools are missing: xcode-select --install"
  command -v brew > /dev/null || die "Homebrew is missing: https://brew.sh"
  local missing=() b
  for b in ${BREW[@]+"${BREW[@]}"}; do brew list --versions "$b" > /dev/null 2>&1 || missing+=("$b"); done
  if [ ${#missing[@]} -gt 0 ]; then
    note "brew install ${missing[*]}"
    brew install "${missing[@]}" || die "brew install failed"
  else
    note "Homebrew packages: all ${#BREW[@]} installed"
  fi
  cmd_sync
}

cmd_sync() {
  local why; why=$(unpinned)
  if [ -n "$why" ]; then
    swarc_dirty && die "$why; commit or stash them first (or ./recomp pin a commit with them)"
    note "SwARc: checking out the pinned ${PIN_SHORT}"
    tools/swarc.sh sync > /dev/null || die "tools/swarc.sh sync failed"
    exec "$ROOT/recomp" sync  # the pinned commit's recomp.sh finishes
  fi
  note "SwARc at the pinned ${PIN_SHORT}"
  sync_generated
}

cmd_status() {
  echo "${bold}$DISPLAY_NAME${off} ($ID), at $HEAD_SHORT$(git diff --quiet HEAD -- || echo ", with uncommitted changes")"
  local why; why=$(unpinned)
  if [ -n "$why" ]; then printf '  SwARc       %s%s%s (./recomp sync, or ./recomp pin)\n' "$yellow" "$why" "$off"
  else echo "  SwARc       ${PIN_SHORT}, the pinned commit"; fi
  if generated_stale; then printf '  generated   %sout of date with SwARc%s (./recomp sync)\n' "$yellow" "$off"
  else echo "  generated   install.sh, recomp, kit.yml: current"; fi
  why=$(kit_stale)
  if [ -n "$why" ]; then printf '  kit         %sto build: %s%s (./recomp kit)\n' "$yellow" "$why" "$off"
  else echo "  kit         dist/$ASSET: current"; fi
  local app
  if app=$(installed "$ID"); then
    local v="$app/Contents/Resources/kit-version"
    echo "  installed   $app (commit $(sed -n 3p "$v" | cut -c1-7), SwARc $(sed -n 2p "$v"))"
  else
    echo "  installed   no"
  fi
}

cmd_kit() {
  local force=0 a rest=()
  for a in "$@"; do [ "$a" = --force ] && force=1 || rest+=("$a"); done
  need_pinned ${rest[@]+"${rest[@]}"}
  [ ${#ARGS[@]} = 0 ] || die "kit: unknown option ${ARGS[0]}"
  sync_generated
  local why; why=$(kit_stale)
  if [ -z "$why" ] && [ $force = 0 ]; then
    note "kit: dist/$ASSET is current"
    return 0
  fi
  note "kit: building dist/$ASSET (${why:-forced})"
  /usr/bin/python3 -I "$PLAYER/kit.py" build --root "$ROOT" || die "the kit build failed"
}

# The developer build: what the README's cmake and lift steps make in
# out/release, rebuilt here only where sources changed (seconds after a
# small change; the installer's player build starts over each time).
cmd_run() {
  [ -f out/release/build.ninja ] || die "no developer build in out/release yet: set one up as README.md says (cmake, the lift)"
  note "building out/release ($BINARY)"
  cmake --build out/release --target "$BINARY" || die "the build failed"
  local bin="out/release/$BINARY.app/Contents/MacOS/$BINARY" dir=()
  [ -x "$bin" ] || die "the build made no $bin"
  if [ -z "$(eval "printf '%s' \"\${${ENV}_GAME_DIR:-}\"")" ]; then
    [ -d game ] || die "no game/ folder: link the game's files there, or set ${ENV}_GAME_DIR"
    dir=(--game-dir "$ROOT/game")
  fi
  note "running $bin${*:+ -- $*}"
  if [ $# -gt 0 ]; then exec "$bin" ${dir[@]+"${dir[@]}"} -- "$@"; else exec "$bin" ${dir[@]+"${dir[@]}"}; fi
}

cmd_install() {
  need_pinned "$@"
  local args=(${ARGS[@]+"${ARGS[@]}"})
  cmd_kit --allow-unpinned
  note "install.sh with dist/$ASSET"
  eval "export ${ENV}_KIT_URL=\"file://\$ROOT/dist/\$ASSET\""
  exec /bin/bash install.sh ${args[@]+"${args[@]}"}
}

cmd_test() {
  need_pinned "$@"
  local args=(${ARGS[@]+"${ARGS[@]}"})
  cmd_kit --allow-unpinned
  exec "$PLAYER/vm-test.sh" ${args[@]+"${args[@]}"} "$ROOT"
}

cmd_pin() {
  local ref=${1:-origin/main}
  swarc_dirty && die "external/SwARc has uncommitted changes; commit them and push them to the fork first"
  git -C "$SWARC" fetch -q origin || die "couldn't fetch SwARc"
  local sha; sha=$(git -C "$SWARC" rev-parse --verify -q "$ref^{commit}") || die "SwARc has no $ref"
  git -C "$SWARC" checkout -q "$sha" || die "couldn't check out $ref"
  tools/swarc.sh pin || exit 1
  exec "$ROOT/recomp" sync  # the new commit's recomp.sh finishes
}

PIN_SHORT=$(pin | cut -c1-7)
cmd=${1:-help}
[ $# -gt 0 ] && shift
case "$cmd" in
  setup) cmd_setup ;;
  status) cmd_status ;;
  sync) cmd_sync ;;
  kit) cmd_kit "$@" ;;
  install) cmd_install "$@" ;;
  test) cmd_test "$@" ;;
  uninstall) exec /bin/bash install.sh --uninstall ;;
  run) cmd_run "$@" ;;
  pin) cmd_pin "$@" ;;
  help|-h|--help) usage ;;
  *) usage >&2; exit 2 ;;
esac
