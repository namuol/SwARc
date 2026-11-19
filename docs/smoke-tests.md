# Writing fast, steady smoke tests

Smoke cases (tools/smoke.py) drive a game with a scripted input file
(<ID>_TEST_INPUT, platforms/win32/src/win32/user32_window.cpp) and compare
or check what it shows. They run often, so they should take as long as the
game does and not a second more, and pass on a busy machine. Timings before
and after converting every recomp: docs/smoke-timings.md (12 min one after
another before, about 3 min after, and less with -j).

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
`showIntro` is not its logo videos). Otherwise a key: Escape skips RA2's,
TS's and Soldiers' logos and DX-Ball 2's; Return Spelunky's intro story.
Press it inside the wait for the menu (`press Escape 0.5`), so presses stop
the moment the menu is up, unless the key does something on the menu:

- DX-Ball 2 quits on Escape at its menu, and Tiberian Sun on its game
  choice; a press meant for the intro landed there under load (both failed
  once in parallel runs). Their intros play out: where a stray press is
  fatal, don't press. RA2's Escape on its menu does nothing, so it presses.
- Soldiers opens a Quit dialog on Escape at the menu (and the next Escape
  closes it): pressing through its videos needed `!=` on the dialog's text.
  Hiding the videos (above) made that unnecessary.

Frames presented within 0.3 s of a press don't count towards a match, so a
press that opened something is seen before the wait ends.

## Fresh data

What a game shows can depend on earlier runs (a saved language, a seen
intro, a tutorial already done). `"fresh_data": true` gives each run new
per-user data; `"data": "smoke/data"` seeds it with settings files (the
game's own config, not its art), e.g. Spelunky's config.txt with the
language chosen, which skips the language screen.

## Clicks and timing

- A click is a press and, 80 ms later, a release; games act on the release.
  A wait on the click's line starts after the release.
- Menus that slide in may ignore clicks until they have settled (RA2's main
  menu while its title types out): wait for pixels of the settled screen,
  then leave ~0.5 s.
- Check a script's clicks do what the comment says: DX-Ball 2's board
  screen PLAY click had never worked; the board started from later clicks.

## Shots and references

The shot comes a fixed time after a wait has seen its screen, so it must
show something that time doesn't change. A menu or sidebar is the same
whenever it's taken; mid-game screens are not (DX-Ball 2's ball once cleared
a board before the shot; its shot is now the board before launch, and play
continues after it for crash coverage). Watch for menus with moving 3D
scenes too (Soldiers'): take the reference at the same moment after the
wait as the runs do. Changing when a shot is taken may need
new local references (`--update`, then look at them).

## Many at once

`-j N` runs N repos side by side; each repo's cases still run in turn. Games
then load slower (six at once stalled every main thread for 5-24 s), so
`seconds` and every wait's timeout are multiplied by N
(`<ID>_TEST_WAIT_SCALE`). `-j 3` runs all six recomps in about 100 s; more
jobs don't help (RA2's five cases are the floor).
