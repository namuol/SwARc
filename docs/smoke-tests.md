# Writing fast, steady smoke tests

Smoke cases (tools/smoke.py) drive a game with a scripted input file
(<ID>_TEST_INPUT, platforms/win32/src/win32/user32_window.cpp) and compare
or check what it shows. They run often, so they should take as long as the
game does and not a second more, and pass on a busy machine. Timings before
and after converting every recomp: docs/smoke-timings.md.

## What a game's smoke set must have

- **Gameplay, with its essential inputs, each one checked.** At least one
  case gets into play (e.g. a level, a mission, a match, a lot) and uses the
  inputs a player cannot do without: movement and the camera, the mouse
  buttons (e.g. fire, select, order), the keys the game is played with. Each
  input's *effect* is checked by a later wait, check or compared shot (e.g.
  the ammo count drops after a shot; a unit's panel opens when it is clicked;
  the view leaves its pixels after a step). An input that is only sent
  proves nothing: a lost click passes.
- **As short as the game allows.** Skip what does not need covering: a
  game's own dev switch or hook that starts a level (Soldiers'
  SOLDIERS_BENCH, Source's +map, Crysis' +map with -devmode), a seeded
  saved game past an unskippable cutscene (Max Payne 2), a map that starts
  with the weapon in hand (Portal's testchmb_a_02).
- **No redundant cases.** A case whose steps another case already performs
  goes, flaky or not; don't add one that repeats another's coverage.
- **Documented coverage.** smoke/COVERAGE.md beside cases.json says what
  each case reaches, which inputs it uses, what checks their effect, and
  what is not covered; keep it current with every change to a case.
- **At least 90% reliable.** After adding or changing a case, run it up to
  10 times (headless, one case at a time): it passes with at most 1
  failure, and fails at the 2nd (the 10th run is not needed when the first
  9 passed). Keep each failure's log and shot, with the load average. Then
  commit and push it. Run cases that will run side by side (-j, a repo's
  "parallel") that way too, under load: that is where frame-rate-dependent
  steps break.

## Wait for screens, don't sleep through them

Fixed times have to cover the slowest run ever seen, so every step carries
padding (RA2 waited 10 s per menu for transitions that take 1-3 s), and a
slow run still breaks them. Instead:

    0 wait 40 724,74=ffff39 524,262=ff3431 568,558=8c8e94 press Escape 0.5
    0.5 warp 718 220
    0.6 click
    0.6 wait 20 732,594=421818 700,262=d60000 656,354=7b797b

`wait` stops the script's clock until all the listed pixels of the presented
frame match (within `tol`, default 24 per channel); later times count on
from there. `x,y!=rrggbb` requires a pixel *not* to be a colour (a dialog's
text absent). `press <key> <every>` presses a key meanwhile (skipping
videos). A timeout is a failure naming the pixels it saw, so a broken step
is obvious. The log has each wait's duration ("waits L4 6.7s, ..."), the
first place to look when tuning.

End with `exit` (after the shot): the run then stops as soon as it has what
it needs, and `seconds` is only a timeout.

## Picking pixels

Record a run's frames (<ID>_SHOT_EVERY=10 and <ID>_SHOT_DIR, or a smoke
case's `shot_every`), then:

    python3 -I tools/waitpick.py --after F1.bmp F2.bmp F3.bmp --before B1.bmp B2.bmp [--region x0,y0,x1,y1]

`--after`: frames of the screen to wait for, from different moments (so
animation is left out). `--before`: every screen that comes earlier in the
script (the previous menu, loading screens, fades). It prints points to
paste. When no single pixel tells every before frame apart it picks a set
that does. `--exact` lets thin text count (menus draw text at the same
pixels every time); `tools/pixel.py FILE x,y ...` prints pixels.

What went wrong while converting, and the rule it gave:

- **Random content.** A start position or a generated level differs every
  run: pick from UI (sidebars, HUDs, menus), never from the map. (RA2 and
  TS in-game waits first had a map pixel.)
- **Animation.** Title lights that cycle colours looked still in the three
  frames sampled (DX-Ball 2). Give `--after` frames spread over seconds, and
  prefer buttons and text to logos.
- **"Before" frames that are already the screen.** A transition's last
  frames often are the finished screen; if waitpick finds nothing, drop the
  latest before frame and look at it.
- **Wrong frame size.** In-game RA2 is 1920x1080 and its menus 800x600;
  waits are in frame pixels, pointer events in the script's own coordinates.

## Skipping intros

Look for a switch first: The Sims has `-skip_intro` (case `"args"`). Then
look at how the game finds its intro: Soldiers plays whatever
`resource\video\intro` holds, so its case hides that folder's entries from
the game (`"env": {"{prefix}_TEST_HIDE": "resource\\video\\intro\\"}`, a
listing filter; nothing on disk changes) and goes straight to loading, with
no keys pressed. (Trace the game's file and video calls with <ID>_TRACE=1
and <ID>_TRACE_FILTER=File, or the name of the video API.) Check
the EXE's strings for switches and the game's settings files for an intro
option (RA2's `[Intro] Play=no` is the intro movie only; Soldiers' profile
`showIntro` is not its logo videos). Otherwise a key: Escape skips RA2's, TS's and Soldiers' logos and DX-Ball
2's; Return Spelunky's intro story. Press it once, after a wait for the
screen it skips, not repeatedly until the next screen is up: a press can
reach the game late, under load, and land on a screen where the key does
something else.

- RA2's Escape on the Single Player menu goes back: presses through its
  logos until the main menu was up sometimes landed there (classic-skirmish
  hung in a parallel run). Its cases now wait for the logo's pixels and
  press once (the logos otherwise play 13 s).
- Tiberian Sun quits on Escape at its game choice. Its cases wait for the
  intro's first frame that is not black (`320,100!=000000 tol 8`) and press
  once; the same for its menu animation (wait for the game choice to go,
  `!=`, then press).
- DX-Ball 2 quits on Escape at its menu; its logo plays out.
- Soldiers opens a Quit dialog on Escape at the menu (and the next Escape
  closes it): pressing through its videos needed `!=` on the dialog's text.
  Hiding the videos (above) made that unnecessary.

Movies (a campaign's briefing) are skipped the same way: wait for the
movie's first second (waitpick on its first frames; it moves on soon), press
once. In a mission Escape opens the options menu, so the in-mission wait
should also fail with it open (pixels it covers, or `!=` on its own). A
loading screen between a movie and the mission may be presented only once
or twice (TS): too briefly to wait for.

Frames presented within 0.3 s of a press don't count towards a match, so a
press that opened something is seen before the wait ends.

## Fresh data

What a game shows can depend on earlier runs (a saved language, a seen
intro, a tutorial already done). `"fresh_data": true` gives each run new
per-user data; `"data": "smoke/data"` seeds it with settings files (the
game's own config, not its art), e.g. Spelunky's config.txt with the
language chosen, which skips the language screen. Tiberian Sun's seeds the
game's SUN.INI with its intro counted as seen; with no SUN.INI at all the
game picks another screen size. `"data"` at the top level of cases.json is
every case's.

`"fresh_data"` also keeps a user's own settings out of the run: Crysis'
per-user install/autoexec.cfg often holds a `map ...` line that would load
another level. A saved game in `"data"` (Max Payne 2's quicksave) starts
a case past what cannot be skipped.

## Clicks and timing

- A click is a press and, 80 ms later, a release; games act on the release.
  A wait on the click's line starts after the release.
- Menus that slide in may ignore clicks until they have settled (RA2's main
  menu while its title types out): wait for pixels of the settled screen,
  then leave ~0.5 s.
- Check a script's clicks do what the comment says: DX-Ball 2's board
  screen PLAY click had never worked; the board started from later clicks.

- A wait on the same line time as a held key or click (`key W 1`,
  `click left 1.0`) starts only after the release. An `exit` timed before
  a wait's start would end the run without the wait ever running, so the
  script is rejected as a bad line ("exit at ... comes before the wait on
  line N") and the case fails. Put the wait at the release time and the
  exit after it.
- `key` takes SDL's key names; write a name's spaces as `_` (`Left_Shift`,
  `Left_Ctrl`). A held Shift types shifted characters: `key Left_Shift 0.3`
  with the next key 0.1 s later.
- A click right after a level or a weapon appears may be lost (Portal's
  first shot, a rifle still being drawn in Crysis or HL2): leave ~1 s, more
  under load, and wait for the shot's effect.

## Shots and references

The shot comes a fixed time after a wait has seen its screen, so it must
show something that time doesn't change. A menu or sidebar is the same
whenever it's taken; mid-game screens are not (DX-Ball 2's ball once cleared
a board before the shot; its shot is now the board before launch, and play
continues after it for crash coverage). Watch for menus with moving 3D
scenes too (Soldiers'): take the reference at the same moment after the
wait as the runs do. Changing when a shot is taken may need
new local references (`--update`, then look at them).

How far a held key moves the player depends on the frame rate (fewer
simulated seconds under load), so a shot after walking differs from run to
run. Put the steps whose result is fixed (a raw mouse delta turns the
same at any frame rate) before the compared shot, and check a walk with
`!=` on pixels it must leave rather than with a shot (Portal's chamber).
HUD text that is drawn translucently washes out over a bright background:
look at a dark part of the level before waiting on it (Crysis' ammo).

## Many at once

`-j N` runs up to N games at once. Games then load slower (six at once
stalled every main thread for 5-24 s), so `seconds` and every wait's timeout
are multiplied by the number that can run at once (`<ID>_TEST_WAIT_SCALE`).

A repo's cases run side by side when its cases.json says `"parallel": true`.
Each case then needs its own log and per-user data: smoke.py passes
`<PREFIX>_RUN_LOG` (the run script writes the game's log there) and
`$XDG_DATA_HOME` (fresh, or seeded from `"data"`), so a repo opting in must
have a run script that takes the log path. `"build"` (a command) builds
once before the cases and sets `<PREFIX>_NO_BUILD=1`, which the run script
honours: several cases' builds of one tree would collide, and an up-to-date
check per case costs time. Without `"parallel"` a repo's cases run one at a
time, as before. RA2 (six cases) and TS (three) run side by side: RA2 in
25 s with `-j 6`, TS in 10 s.
