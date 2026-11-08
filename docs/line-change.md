# Forced manual line changes

Status 2026-11-07: implemented in the dev build, off by default, **verified in a match** (see Verification). Switch: `REX_NHL_MANUAL_LINES=true` (cvar `nhl_manual_lines`).
Code: `src/rules_hooks.cpp`. No original names or addresses here.

## What the game does

- The game has a per-side option "Line Changes" with the values Manual and Auto (home and away are separate options, in the
  controller / game settings screens; the help text says "select Manual for user controlled line changes, Auto for hands-off").
  The option is part of the player profile (a large settings record per user slot), so it is saved with the profile.
- At the start of a session the game allocates a small array of two such records (index = side, 0 home and 1 away) and fills it
  from the profiles. Everything in a match reads the line change mode from that array. The mode is one 16-bit field in each
  record. In the runs both records read 2 (the default "Auto"), 1 is manual. Readers treat any non-zero value as "the user
  manages the line" except the stoppage auto change, which only skips a user-controlled side for exactly 1. The game copies the
  profile into the records again while a match starts, so the override cannot be applied once at the start.
- Seven places read the field (one small accessor used from many callers, plus six functions that read it themselves): the
  stoppage auto line changes (skipped for a user-controlled side only when the value is exactly 1), the AI strategy, the HUD
  logic (line change indicator, "show the CPU away line") and the input handling that lets a button press change lines.

## What the override does

- At the entry of each of the seven readers, and right before the stoppage function reads the mode of a side, the field is
  written to 1 in both records (idempotent, a few instructions), so every reader sees manual whatever the in-game option or the
  saved profile says. Both sides are covered, so it works for two human players and for a user who plays the away team.
- Nothing is written to the profile or the settings screens: the option in the menu still shows what the user chose, and
  switching the override off gives the game's own behaviour back.
- The writes are logged: `[nhl-lines] side N line change mode 2 -> 1` (more than once per run, because the game restores the
  profile value now and then; each time it is forced again before a reader looks).

## Why not set the option itself

The option lives in the saved profile and in a settings manager that copies it into the match array; changing the stored value
would also change what the settings screens show, and would be written to the save. The match array is read-only for the game
during a match, so overriding the readers is the smaller change.

## Verification (2026-11-07, dev, quick route Play Now, Anaheim away, the user on the away side)

- **Stock run** (`NHL_LOG_LINE_CHANGES=1`, dev log of every executed line change): at the first whistle the stoppage function
  changes the lines of both sides: `event side 1 lines 1 1` (the human team) and `event side 0 lines 2 1` (the CPU team).
- **Forced manual** (`REX_NHL_MANUAL_LINES=true`): the log shows `mode 2 -> 1` for both sides, the stoppage user test returns 1
  for side 1 and 0 for side 0 with mode 1 before it, and at the whistle **only the CPU side changes** (`event side 0 lines 0 0`);
  no event for the human side. That is the intended effect: the game no longer changes the user's lines at a stoppage.
- Both sides are covered by the same write (two records); the human side was side 1 here, side 0 is the CPU in the quick route,
  so the "user on the home side" and "two humans" cases were not run.
- No screenshot shows the line change indicator: the HUD element is not visible in either run (the logs are the evidence).
- An earlier version of the switch read the wrong word for the array pointer, so it wrote into some other object and had no effect
  (the log showed `mode 0 -> 1`, not `2 -> 1`); a stock run with that build looked identical to a forced one. Fixed and rerun.

## Not covered

- Online matches (the game takes the mode from the lobby settings there; untested).
- A value other than 1 and 2 (0 is not used by the game's own settings): the dev switch `NHL_LINE_VALUE=n` writes n instead.
