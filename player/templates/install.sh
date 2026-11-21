#!/bin/bash
# One-paste installer for players. In Terminal:
#   curl -fsSL https://raw.githubusercontent.com/@REPO@/main/install.sh | bash
# Downloads the latest build kit (a GitHub release asset) to
# ~/Library/Application Support/@ID@/kit and runs its build.sh, which
# installs Apple's command line tools if needed, finds or downloads the game
# with your Steam account, and builds @APP@.app in your Applications folder.
# Running it again updates the kit and rebuilds. @ENV@_KIT_URL overrides the
# kit's URL (file:// works). Arguments go to build.sh (--game-dir, --supply,
# --no-launch). --uninstall instead deletes the app and its build kit, and
# keeps the saved games (what the app menu's Uninstall does):
#   curl -fsSL https://raw.githubusercontent.com/@REPO@/main/install.sh | bash -s -- --uninstall
# Everything is in main(), so a partly downloaded script runs nothing.
#
# Generated from SwARc's player/templates/install.sh by
# `external/SwARc/player/kit.py sync`: change it there, not here.
set -uo pipefail

main() {
  local url=${@ENV@_KIT_URL:-https://github.com/@REPO@/releases/latest/download/@ASSET@}
  local support="$HOME/Library/Application Support/@ID@"
  local bold=$'\033[1m' red=$'\033[31m' off=$'\033[0m'
  fail() { printf '\n%sStopped:%s %s\n' "$red" "$off" "$*"; exit 1; }

  if [ "${1:-}" = --uninstall ]; then
    local a
    # The game's app, whatever its name: its game.env names this game.
    for a in /Applications/*.app "$HOME"/Applications/*.app; do
      [ -f "$a/Contents/Resources/player.sh" ] && (. "$a/Contents/Resources/game.env" && [ "$ID" = @ID@ ]) &&
        exec /bin/bash "$a/Contents/Resources/player.sh" uninstall --terminal
    done
    fail "@APP@.app isn't in /Applications or ~/Applications (or it's from before uninstalling was added: drag it to the Trash)."
  fi

  [ "$(uname -s)" = Darwin ] || fail "this is for macOS."
  [ "$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ] ||
    fail "this needs a Mac with Apple Silicon (M1 or newer). Intel Macs are not supported yet."

  printf '%s==> Downloading the build kit%s\n' "$bold" "$off"
  local tmp
  tmp=$(mktemp -d) || fail "couldn't make a temporary folder."
  curl -fL --progress-bar -o "$tmp/kit.tar.gz" "$url" || fail "couldn't download the build kit from $url."
  tar -xzf "$tmp/kit.tar.gz" -C "$tmp" && [ -x "$tmp/@KIT@/build.sh" ] ||
    fail "the build kit download is damaged; paste the line again."
  mkdir -p "$support"
  rm -rf "$support/kit"
  mv "$tmp/@KIT@" "$support/kit" || fail "couldn't put the kit in $support."
  rm -rf "$tmp"
  exec "$support/kit/build.sh" "$@" < /dev/tty
}

main "$@"
