# Boot flow: which screens there are, who drives them, what can be left out

Status 2026-11-07, Linux dev build. Switches are off by default. No publisher names or addresses in this file: screens are named by role,
the rest is kept out of the repo.

## How the boot is built

- The boot screens are small UI movies (the same script-and-timeline format as the menus), not Lua and not game code. Each is a
  file in the front-end tree. The order and the rules live in the UI scripts, in two places: a script that lists the screens and
  one that lists the boot movies, plus a native part that is told "screen X has been entered" by each screen and moves on.
- Order today: language selection (waits for A, times out after ~30 s without a pad) -> a blank "publisher notice" screen (no
  timeline, advances at once) -> the legal text screen -> the title screen with the logo movie playing over it (the movie
  carries a cue that enables Start) -> the autosave notice -> the main menu.
- The Lua layer (`docs/lua-scripting.md`) does not touch any of this: the boot scene file only lists render bins. A Lua cache
  override of the boot scene definition cannot remove a screen.

## What was measured

All seconds are from the first log line of a run (the process starts about 1.5 s earlier), quick start with the 3x boot clock and
the 12-frame logo movie, dev build. Three interleaved runs per row (`p-ctl/ab.sh`); the boot is so repeatable at the 3x clock that
the three runs of a row agree within 0.03 s.

| What | Legal screen opens | Movie starts | Title opens | Main menu file opens |
|---|---|---|---|---|
| Stock quick start | 4.12 | 5.89 | 6.11 | **10.64** |
| Legal screen blank (`nhl_skip_legal`) | 4.12 | 5.89 | 6.12 | **10.65** |
| Legal blank and no language screen | 3.69 | 5.45 | 5.70 | **9.67** |

Reading: **the blank legal screen gives no time at all, the language skip gives 1.0 s.** The legal screen is a wait, not work:
its 1.77 s (5.3 s of guest time, at the 3x clock) is the same with or without the text. The game reads its databases when the screen
opens (a burst of database opens), and a 24x guest clock during the wait made it longer (3.9 s) in the one run tried (variant
removed), so what ends the wait is not understood. What is left on the boot path is ~1.8 s of this wait, the
12-frame movie (0.2 s), and ~4 s from the title to the menu, none of which the clock shortens further.

## What works: leaving the screens out

### Legal text (`nhl_skip_legal`, env `REX_NHL_SKIP_LEGAL=true`, `src/quick_start.cpp`)
The legal screen is served from the "publisher notice" screen file with the one notice name changed from "publisher notice" to
"legal". The native boot flow waits for exactly the notice "legal screen entered" before it goes on; a screen that never sends it
hangs the boot, which is why serving an empty screen (P20) did, and very likely why a copy of the logo screen that sends the
wrong notice did too (inferred from the runs, the native part was not read). The copy has no timeline, so it advances at once. The legal text itself is never drawn; the cloud background stays.
Served at run time, the game files are untouched. If the logo screen file cannot be read, or its notice name is not found exactly
once, the log says `legal screen left in` and the stock screen is used.

### Language screen (`nhl_skip_language`, env `REX_NHL_SKIP_LANGUAGE=true`, `src/rules_hooks.cpp`)
The UI script asks the native "show language selection?" and gets the script's true or false object. With the switch on the
native part answers false, and the script takes the path it uses when no choice is needed. Verified: the boot goes on to the
legal screen with no press, the game language stays the default (English here). **Saves 1.0 s** (9.67 against 10.65 s to the menu).

## What did not work

- Replacing the list of screens: removing the legal screen from the list, or putting another screen in its place. Compared
  with the screen served blank above, every variant tried hung or froze the title: the native flow counts the notices it gets
  (a duplicate "publisher notice" hung the boot; a list without legal reached the title with the movie played but Start never
  came). Not worth more time now that the blank screen works.
- Replacing a screen with a file that has no script (hung, P20, once).
- Boot clock raised to 24x during the legal wait (one run): the wait grew to 3.9 s instead of shrinking; removed.

## What is left

- The ~1.8 s legal wait itself: what ends it is not found (not a plain guest timer, see above). If it is the end of the data
  loading behind the screen, only faster loading shortens it (`roadmap.md` 1k).
- The movie (12 frames, `NHL_QUICK_MOVIE_FRAMES`) and ~4 s from the title to the menu are not shortened by the clock.

## Reproduce

`REX_NHL_QUICK_START=true NHL_BOOT_CLOCK=3 NHL_QUICK_MOVIE_FRAMES=12 REX_NHL_SKIP_LEGAL=true REX_NHL_SKIP_LANGUAGE=true ./nhllegacy`
and read the `[nhl-boot]` and `[nhl-quick]` lines of the log.
