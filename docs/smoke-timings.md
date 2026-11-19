# Smoke test timings

Wall time per case as tools/smoke.py reports it (build check, run, compare),
on the same Mac (M-series, macOS 26), one case at a time.

## Before (2026-11-19, SwARc 079b9f6, scripts with fixed delays)

From the verification run of all recomps that morning.

| Case | Time | Run length (`seconds`) |
|---|---|---|
| sims-1-recomp (tools/smoke.sh, own runner) | ~105 s (100 s run + build/check) | 100 |
| soldiers-heroes-of-wwii-recomp/menu | 52 s | 45 |
| ra2-yr-recomp/skirmish | 86 s | 80 |
| ra2-yr-recomp/side-dropdown | 60 s | 58 |
| ra2-yr-recomp/map-list | 58 s | 56 |
| ra2-yr-recomp/classic-skirmish | 78 s | 76 |
| ra2-yr-recomp/classic-side-dropdown | 60 s | 58 |
| tiberian-sun-recomp/skirmish | 63 s | 57 |
| tiberian-sun-recomp/campaign-list | 44 s | 43 |
| dx-ball-2-recomp/play | 46 s | 40 |
| spelunky-recomp/play | 72 s | 70 |
| **All, one after another** | **~724 s (12 min)** | |

RA2/YR alone: 342 s; Tiberian Sun: 107 s.

## After (2026-11-19, every recomp converted)

All six recomps, one after another (`smoke.py` with every repo, no `-j`):
**11/11 passed in 259 s, against ~724 s before (2.8x faster).**

| Case | Before | After |
|---|---|---|
| sims-1-recomp/lot | ~105 s | 55 s |
| soldiers-heroes-of-wwii-recomp/menu | 52 s | 15 s (12 s with a reference taken as the menu appears) |
| ra2-yr-recomp/skirmish | 86 s | 26 s |
| ra2-yr-recomp/side-dropdown | 60 s | 15 s |
| ra2-yr-recomp/map-list | 58 s | 14 s |
| ra2-yr-recomp/classic-skirmish | 78 s | 23 s |
| ra2-yr-recomp/classic-side-dropdown | 60 s | 14 s |
| tiberian-sun-recomp/skirmish | 63 s | 20 s |
| tiberian-sun-recomp/campaign-list | 44 s | 12 s |
| dx-ball-2-recomp/play | 46 s | 24 s |
| spelunky-recomp/play | 72 s | 34 s |

Per recomp, as converted:

### ra2-yr-recomp (waits + exit, logos skipped)

Three runs in a row, all passed (while other builds ran):

| Case | Before | After |
|---|---|---|
| skirmish | 86 s | 24-25 s |
| side-dropdown | 60 s | 14 s |
| map-list | 58 s | 14-15 s |
| classic-skirmish | 78 s | 22-23 s |
| classic-side-dropdown | 60 s | 14 s |
| **suite** | **342 s** | **91-92 s** |

### tiberian-sun-recomp (waits + exit, intro skipped)

| Case | Before | After |
|---|---|---|
| skirmish | 63 s | 19 s |
| campaign-list | 44 s | 12 s |
| **suite** | **107 s** | **32 s** |

### sims-1-recomp (tools/smoke.sh: -skip_intro, waits + exit)

| Case | Before | After |
|---|---|---|
| tools/smoke.sh (enter the tutorial lot, Sim drawn) | ~105 s (100 s run) | 57 s |

### soldiers-heroes-of-wwii-recomp (Escape through the logo videos, wait + exit)

No switch or setting skips its logo videos (`-noVideo` and the profile's
`showIntro` don't). Escape does; on the menu it toggles the Quit dialog,
so the wait also requires the dialog's text absent ("!=").

| Case | Before | After |
|---|---|---|
| menu | 52 s | 15 s; 12 s with a new reference taken as the menu appears (the menu's camera moves, so the old one needed the shot 3.5 s in) |

### dx-ball-2-recomp (waits + exit; the logo plays)

| Case | Before | After |
|---|---|---|
| play | 46 s | 24 s |

Skipping the logo with one Escape (21 s) failed once in a parallel run:
Escape on the main menu quits, and under load a press meant for the logo
can land there, so the logo now plays out (3 s more).

The old script's board-screen PLAY click never worked (the board started
from its later clicks on the list); now the set is clicked twice. The
shot is mid-game, so its difference from the reference (taken 15 s into
play) is 10% against a 15% limit: a reference of the board before launch
would be steadier.

### spelunky-recomp (fresh data seeded with its config.txt, waits + exit)

The seeded config (language chosen) skips the language screen; Return
skips the intro story. The door animation before the menu can't be
skipped (Space doesn't).

| Case | Before | After |
|---|---|---|
| play | 72 s | 35 s |

## All at once (`-j`)

At first, two of three `-j 6` runs had failures: with six games loading at
once, every main thread stalled 5-24 s (watchdog hang reports from all of
them at the same moment: e.g. Spelunky decoding its PNGs), and waits timed
out. Timeouts now scale with N (`<ID>_TEST_WAIT_SCALE`). Then:

| Run | Result |
|---|---|
| `-j 6` (three runs) | 11/11 in 109 s, 107 s, 106 s |
| `-j 3` | 11/11 in 102 s |

`-j 3` is as fast (RA2's five cases, ~92 s one after another, are the
floor) and loads the machine half as much: **~100 s for everything, from
~12 min.**
