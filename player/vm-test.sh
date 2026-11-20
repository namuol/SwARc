#!/bin/bash
# Tests games' player installs end to end in throwaway macOS VMs, with tart
# (https://tart.run; on PATH or ~/Applications/tart.app): for each game, a
# fresh VM runs its install.sh with the kit in dist/ (make it first:
# external/SwARc/player/kit.py build), as a player would, in Terminal; checks
# the app loads nothing from outside itself; then runs the game's smoke
# cases (smoke/cases.json, SwARc's tools/smoke.py) on the installed app,
# wanting half the frames each case's min_frames asks for (the VM's GPU is
# slower than the Macs the cases were made on).
#   vm-test.sh [--steam] [--keep-base] [--keep] GAME_REPO...
# Games run one after another, each in a new clone of the base VM
# "swarc-player-base" (Cirrus Labs' vanilla macOS plus Apple's command line
# tools and Pillow for smoke checks, made first if missing; RECOMP_VM_IMAGE
# overrides the image), so each starts from the same clean state. Every VM
# is deleted afterwards, also on failure or Ctrl-C, and so is the base if
# this run made it, with the macOS image if this run downloaded it: they
# take tens of GB. --keep-base keeps the base (and the image) for later
# runs; --keep keeps the last game's VM running, to look at (delete it with
# `tart delete swarc-player-test`).
#
# The player's game copy is the one on this Mac (<PREFIX>_GAME_DIR, else
# the folder the game's own install saved), copied into the VM's ~/Games
# for the install's search to find; files the player supplies go in
# ~/Player and are passed with --supply. --steam leaves the game out, so the
# install offers the Steam download: answer it in the VM's Terminal and
# approve the sign-in by scanning the QR code with the Steam app.
#
# Results: GAME_REPO/build/vm-test/ (install.log, smoke/). The VM's user is
# admin, password admin. Its screen is 1512x982 points, HiDPI.
set -euo pipefail
PLAYER=$(cd "$(dirname "$0")" && pwd)
SWARC=$(dirname "$PLAYER")
TART=$(command -v tart || echo "$HOME/Applications/tart.app/Contents/MacOS/tart")
[ -x "$TART" ] || { echo "vm-test: tart not found (https://tart.run)"; exit 1; }
IMAGE=${RECOMP_VM_IMAGE:-ghcr.io/cirruslabs/macos-tahoe-vanilla:latest}
BASE=swarc-player-base
TEST=swarc-player-test
SH="/Volumes/My Shared Files"
W_PT=1512 H_PT=982

steam=0 keep_base=0 keep=0 repos=()
while [ $# -gt 0 ]; do
  case "$1" in
    --steam) steam=1 ;;
    --keep-base) keep_base=1 ;;
    --keep) keep=1 ;;
    -h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "vm-test: unknown option $1 (try --help)"; exit 2 ;;
    *) repos+=("$(cd "$1" && pwd)") ;;
  esac
  shift
done
[ ${#repos[@]} -gt 0 ] || { sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

# --- What each game needs, from its recomplauncher.json ---------------------
cfg() {  # cfg REPO KEY: one value of the x-swarc block, as kit.py reads it
  /usr/bin/python3 -I - "$1" "$2" <<'EOF'
import json, sys
doc = json.load(open(sys.argv[1] + "/recomplauncher.json"))
x = doc["x-swarc"]
v = {"ENV": x["id"].upper().replace("-", "_"), "ASSET": x["id"] + "-kit-macos-arm64.tar.gz",
     "SUPPLIED": " ".join(s["file"] for s in x.get("supplied", [])),
     "FOLDER": x["steam"]["folder"]}.get(sys.argv[2], x.get(sys.argv[2]))
print(v)
EOF
}
host_game_dir() {  # the game copy on this Mac
  local env id d
  env=$(cfg "$1" ENV) id=$(cfg "$1" id)
  d=$(eval "printf '%s' \"\${${env}_GAME_DIR:-}\"")
  [ -n "$d" ] || d=$(head -1 "$HOME/Library/Application Support/$id/game-dir" 2>/dev/null || true)
  printf '%s' "$d"
}
for r in "${repos[@]}"; do
  [ -f "$r/recomplauncher.json" ] || { echo "vm-test: $r has no recomplauncher.json"; exit 1; }
  [ -f "$r/dist/$(cfg "$r" ASSET)" ] ||
    { echo "vm-test: no kit in $r/dist: run external/SwARc/player/kit.py build there first"; exit 1; }
  tar -xzOf "$r/dist/$(cfg "$r" ASSET)" "$(cfg "$r" id)-kit/build.sh" | cmp -s - "$PLAYER/build.sh" ||
    { echo "vm-test: $r's kit has an older player/build.sh: run external/SwARc/player/kit.py build there"; exit 1; }
  [ -f "$r/smoke/cases.json" ] || { echo "vm-test: $r has no smoke/cases.json"; exit 1; }
  if [ $steam = 0 ]; then
    g=$(host_game_dir "$r")
    [ -d "$g" ] || { echo "vm-test: no game copy for $r (set $(cfg "$r" ENV)_GAME_DIR)"; exit 1; }
  fi
done

# --- VMs -------------------------------------------------------------------
made_base=0 vm="" pulled=0
"$TART" list --source oci --quiet 2>/dev/null | grep -qF "${IMAGE%:*}" || pulled=1
cleanup() {
  if [ -n "$vm" ] && ! { [ $keep = 1 ] && [ "$vm" = $TEST ]; }; then
    "$TART" stop "$vm" > /dev/null 2>&1 || true
    "$TART" delete "$vm" > /dev/null 2>&1 && echo "== deleted $vm"
  fi
  if [ $made_base = 1 ] && [ $keep_base = 0 ]; then
    "$TART" delete "$BASE" > /dev/null 2>&1 && echo "== deleted $BASE"
    if [ $pulled = 1 ]; then  # the image this run downloaded (its tag and digest entries), not others
      repo=${IMAGE%:*}
      "$TART" list --source oci --quiet 2>/dev/null | grep -F "$repo" | while IFS= read -r e; do
        "$TART" delete "$e" > /dev/null 2>&1 && echo "== deleted the cached image $e"
      done
    fi
  fi
  return 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM

vm_ssh() {  # vm_ssh CMD: as admin (password admin), via expect
  local ip
  ip=$("$TART" ip "$vm") || return 1
  expect -f - "$ip" "$1" <<'EOF'
set timeout -1
log_user 0
spawn ssh -q -t -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o PubkeyAuthentication=no admin@[lindex $argv 0] [lindex $argv 1]
expect "assword:"
send "admin\r"
log_user 1
expect eof
catch wait result
exit [lindex $result 3]
EOF
}
boot() {  # boot LOGDIR [--dir=...]...: run $vm, detached, until ssh answers
  local logdir=$1; shift
  mkdir -p "$logdir"
  nohup "$TART" run "$vm" "$@" > "$logdir/tart.log" 2>&1 < /dev/null &
  local pid=$!
  disown
  for _ in $(seq 100); do
    kill -0 $pid 2>/dev/null || { echo "vm-test: tart exited:"; cat "$logdir/tart.log"; exit 1; }
    vm_ssh true > /dev/null 2>&1 && return 0
    sleep 3
  done
  echo "vm-test: $vm didn't come up"; exit 1
}
in_terminal() {  # in_terminal NAME SCRIPT: run SCRIPT in the VM's Terminal (the GUI session), wait for it
  # A .command file opens in Terminal and runs there, as a player would run
  # install.sh; the game's windows need the GUI session too.
  local name=$1 script=$2
  printf '#!/bin/bash\n%s\necho $? > ~/%s.status\n' "$script" "$name" > "$OUT/$name.command"
  vm_ssh "rm -f ~/$name.status; cp \"$SH/out/$name.command\" ~/Player/ && chmod +x ~/Player/$name.command &&
          open ~/Player/$name.command" > /dev/null
  until vm_ssh "test -f ~/$name.status" > /dev/null 2>&1; do sleep 10; done
  vm_ssh "cat ~/$name.status" | tr -d '\r\n'
}

if "$TART" get "$BASE" > /dev/null 2>&1; then
  echo "== using the existing $BASE"
else
  echo "== making $BASE: $IMAGE plus Apple's command line tools (the image is a 25 GB download the first time)"
  made_base=1
  "$TART" clone "$IMAGE" "$BASE"
  "$TART" set "$BASE" --cpu 8 --memory 16384 --display ${W_PT}x${H_PT}pt --no-display-refit
  vm=$BASE
  # Their install window (what build.sh opens on a Mac without them) wants
  # more free space than the image has, so softwareupdate installs them.
  boot "${repos[0]}/build/vm-test"
  echo "== Apple's command line tools, in $BASE (5–15 minutes, quiet until done; --keep-base skips this next time)"
  vm_ssh 'f=/tmp/.com.apple.dt.CommandLineTools.installondemand.in-progress; touch $f;
          label=$(softwareupdate -l 2>/dev/null | sed -n "s/^\* Label: \(Command Line Tools.*\)$/\1/p" | sort -V | tail -1);
          echo "$label"; echo admin | sudo -S softwareupdate -i "$label" 2>&1 | tail -1; rm -f $f' | tr -d '\r'
  vm_ssh 'xcode-select -p' | tr -d '\r' | grep -q . || { echo "vm-test: the command line tools didn't install"; exit 1; }
  # Pillow, for smoke cases' "check" scripts (a player's Mac never needs it).
  # System-wide: the checks run with python3 -I, which ignores the user's packages.
  vm_ssh 'echo admin | sudo -S -H /usr/bin/python3 -m pip install --quiet --disable-pip-version-check pillow 2>&1 | grep -v WARNING;
          /usr/bin/python3 -I -c "import PIL; print(\"Pillow\", PIL.__version__)"' |
    tr -d '\r'
  # A clean shutdown: `tart stop` can lose what was just written to the disk.
  vm_ssh 'sync; echo admin | sudo -S shutdown -h now' > /dev/null 2>&1 || true
  for _ in $(seq 60); do "$TART" list --source local | grep -q "^local *$BASE .*stopped" && break; sleep 3; done
  "$TART" stop "$vm" > /dev/null 2>&1 || true
  vm=""
fi

# --- Each game -------------------------------------------------------------
results=()
for r in "${repos[@]}"; do
  name=$(basename "$r") app=$(cfg "$r" app) env=$(cfg "$r" ENV) asset=$(cfg "$r" ASSET)
  OUT="$r/build/vm-test"
  [ -d "$OUT" ] && mv "$OUT" "$OUT-$(date +%Y%m%d-%H%M%S)"  # keep the last test's logs
  mkdir -p "$OUT"
  echo; echo "=== $name"
  vm=$TEST
  "$TART" stop "$vm" > /dev/null 2>&1 || true
  "$TART" delete "$vm" > /dev/null 2>&1 || true
  "$TART" clone "$BASE" "$vm"
  dirs=(--dir="recomp:$r:ro" --dir="swarc:$SWARC:ro" --dir="out:$OUT")
  game=""
  [ $steam = 0 ] && { game=$(host_game_dir "$r"); dirs+=(--dir="game:$game:ro"); }
  boot "$OUT" "${dirs[@]}"

  supply=""
  excludes=""
  for f in $(cfg "$r" SUPPLIED); do
    excludes+=" --exclude $f"
    supply+=" --supply ~/Player/$f"
  done
  if [ $steam = 1 ]; then
    echo "== the player's files: no game (the install offers the Steam download)"
    copy_game=true
  else
    echo "== the player's files: the game in ~/Games"
    copy_game="rsync -a$excludes \"$SH/game/\" \"\$HOME/Games/$(cfg "$r" FOLDER)/\""
    for f in $(cfg "$r" SUPPLIED); do copy_game+=" && cp \"$SH/game/$f\" ~/Player/"; done
  fi
  vm_ssh "mkdir -p ~/Games ~/Player && $copy_game && cp \"$SH/recomp/install.sh\" \"$SH/recomp/dist/$asset\" ~/Player/ &&
          sw_vers -productVersion && (command -v brew || echo no Homebrew); df -h ~ | tail -1" | tr -d '\r'

  echo "== install.sh, in the VM's Terminal"
  st=$(in_terminal install "cd ~/Player
cat install.sh | ${env}_KIT_URL=\"file://\$HOME/Player/$asset\" bash -s --$supply --no-launch 2>&1 | tee ~/install.log
( exit \${PIPESTATUS[1]} )")
  vm_ssh 'cp ~/install.log "/Volumes/My Shared Files/out/"; tail -8 ~/install.log' | tr -d '\r'
  if [ "$st" != 0 ]; then
    echo "vm-test: $name: FAIL: install.sh exited $st ($OUT/install.log)"
    results+=("FAIL  $name (install)")
  else
    echo "== the app"
    vm_ssh "A=\"/Applications/$app.app\"; [ -d \"\$A\" ] || A=\"\$HOME/Applications/$app.app\";
            [ -d \"\$A\" ] || { echo 'NOAPP: no $app.app in /Applications or ~/Applications'; exit 0; }
            echo \"\$A\" > ~/app.path; echo \"\$A\"; codesign -v \"\$A\" && echo signed || echo UNSIGNED;
            n=0; for f in \"\$A\"/Contents/MacOS/* \"\$A\"/Contents/Frameworks/*.dylib; do
              file \"\$f\" | grep -q Mach-O || continue; n=\$((n + 1))
              otool -L \"\$f\" | tail -n +2; otool -l \"\$f\" | grep -A2 LC_RPATH | grep ' path'
            done > ~/loads.txt; echo \"\$n Mach-O files\"; [ \$n -gt 1 ] || echo 'NOAPP: no binaries in the app';
            grep -E '/opt/|/Users/|/usr/local|/Volumes' ~/loads.txt && echo OUTSIDE || echo 'loads nothing from outside the app'" |
      tr -d '\r' | tee "$OUT/app.txt"
    echo "== smoke cases, on the installed app"
    # A writable copy of the repo's smoke files (results go into it).
    st=$(in_terminal smoke "rsync -a --exclude .git --exclude out --exclude build --exclude dist --exclude external \\
  --exclude game \"$SH/recomp/\" ~/repo/
cd ~/repo && RECOMP_APP=\"\$(cat ~/app.path)\" /usr/bin/python3 -I \"$SH/swarc/tools/smoke.py\" \\
  --run \"$SH/swarc/player/run-installed.sh\" --frames-scale 0.5 . 2>&1 | tee ~/smoke.log
s=\${PIPESTATUS[0]}; rsync -a ~/repo/build/smoke \"$SH/out/\"; cp ~/smoke.log \"$SH/out/\"; ( exit \$s )")
    tail -15 "$OUT/smoke.log" 2>/dev/null || true
    if grep -qE '^(OUTSIDE|NOAPP|UNSIGNED)' "$OUT/app.txt"; then
      results+=("FAIL  $name (the app: $OUT/app.txt)")
    elif [ "$st" != 0 ]; then
      results+=("FAIL  $name (smoke: $OUT/smoke/)")
    else
      results+=("PASS  $name")
    fi
  fi
  if [ $keep = 1 ] && [ "$r" = "${repos[${#repos[@]}-1]}" ]; then
    echo "== $vm is still running (--keep): tart delete $vm when done"
  else
    "$TART" stop "$vm" > /dev/null 2>&1 || true
    "$TART" delete "$vm" && echo "== deleted $vm"
    vm=""
  fi
done

echo; echo "=== vm-test"
printf '%s\n' "${results[@]}"
! printf '%s\n' "${results[@]}" | grep -q '^FAIL'
