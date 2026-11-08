#!/bin/sh
# install_roster_set.sh INSTALL_DIR NAME NHLNG_DB [DESCRIPTION]
#   Installs a roster set: INSTALL_DIR/mods/roster-NAME/{mod.toml,db/nhlng.db}. Select it with NHL_ROSTERS=NAME, or
#   rosters=NAME in INSTALL_DIR/nhl_enhancements.ini, or the pregame menu "Rosters" row. See docs/roster-choice.md.
#   The file is copied to a temp name and renamed, so a running game keeps the old inode.
set -eu
D=${1:?install dir}; N=${2:?set name}; DB=${3:?nhlng.db}; DESC=${4:-Roster set $N}
case "$N" in *[!A-Za-z0-9_-]*|"") echo "bad name" >&2; exit 1;; esac
[ -f "$DB" ] || { echo "no such file $DB" >&2; exit 1; }
M=$D/mods/roster-$N
mkdir -p "$M/db"
cp "$DB" "$M/db/.nhlng.db.new" && mv -f "$M/db/.nhlng.db.new" "$M/db/nhlng.db"
printf 'name = Rosters: %s\ndescription = %s\n' "$N" "$DESC" > "$M/mod.toml"
echo "installed $M ($(wc -c < "$M/db/nhlng.db") bytes)"
