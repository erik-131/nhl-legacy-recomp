# Localisation DB (`fe/loc/nhl_<lang>.db`)

Tool: `tools/rosters/locdb.py` (tests: `test_locdb.py`, synthetic DB only). Never commit a loc DB or a CSV export.

## Format (decoded, verified on all 8 language files)
- It is a normal big-endian T3db container (`DB 00 08`, endian flag 1 at 4, file size at 8, table count 1 at 0x10, header CRC at 0x14), with one table `LanguageStrings` (type 0x42) described by `nhl_<lang>-meta.xml` (fields hashid int32, stringid string 800, sourcetext string 32000). The strings are not stored in the record: the 127-bit view of `tdb.py` is a misreading (`tdb.py verify` reports 4 CRC errors on it; ignore).
- Table block: dir CRC, 0x24-byte header, header CRC, 3 field descriptors, then max-rows 16-byte records `[key offset, text offset, hashid, 0]` (offsets into the pool; text offset FFFFFFFF = no text, 29 such keys), then the string pool, then a CRC of everything from the descriptors to the end. All CRCs are CRC-32/MPEG-2 (poly 04C11DB7, init FFFFFFFF, no xor), same as tdb.py. Header +0x10 = bytes of the pool in use, +0x14 = max rows, +0x16 = rows (both 25,782 in eng_us).
- Compression: one static Huffman tree at the start of the pool (BFS, 4 bytes per node `[idx,val,idx,val]`, idx 0 = leaf with byte val, right child queued before left, left = bit 0). Keys: 1-byte length (in output bytes) + bits; texts: 2-byte big-endian length + bits; bits MSB first, last byte zero-padded. Texts are UTF-8 (the registered sign is C2 AE). Not RefPack or LZ.
- hashid = CRC-32 (reflected, EDB88320, start 0, no final xor) over the key bytes with bit 0x20 cleared (case-insensitive). Records are sorted by hashid; the game looks up by hashid and sees the first match.
- The coding is deterministic: all 8 files decode and re-encode every string to the stored bytes, and `locdb.py check` shows a rewrite with no changes is byte-identical (eng_us, original and installed). `nhl_rus_ru.db` has 4+ records out of hashid order (EA's own file); `check` reports them, the rewrite is still identical.
- Format description cross-checked against the remaster project's notes (nhl-modding-studio spec referenced there) and re-implemented here.

## Keys (eng_us; 25,782 keys, team keys are the same in every language, texts translated)
Game format strings (found in the image): `NHLTeamName_%s`, `NHLCityName_%s`, `TEAMLINE1_%s`, `TEAMLINE2_%s`, `NHLTeamName_Abbr3_%s`; the `%s` is the team's abbreviation or its number. Both families exist for the NHL teams, so a mod should set both.
For team n (row in exhibitionteams; 0 ANA ... 22 Arizona/PHX ... 29 WSH, 30-33 all-star sides), abbr A:
- full name `NHLTeamName_n` / `NHLTeamName_A` = "Anaheim Ducks®"; city `NHLCityName_n` / `_A` = "Anaheim" (NYI/NYR: "NY Islanders®"); `CITYLINE1_n`, `TEAMLINE1_A` (city line), `TEAMLINE2_A`, `NICKLINE2_n`, `NICKLINE2_A` (nickname, "Ducks™"), `TXT_NICKNAME_A`, `TXT_NICKNAME_ALT_A`, abbreviation `NHLTeamName_Abbr3_n` / `_A`, `X_XLA_TEAM_A`; plus the plain city words (`ANAHEIM`, `SEATTLE`, `Arizona`, `PHOENIX`, `Utah`, `LAS_VEGAS`) and `Logo46` ("Arizona Coyotes®").
- Arizona is number 22 and abbr PHX (display ARZ). There are no keys at all for Seattle or Vegas NHL clubs: `SEAT` is the WHL Seattle Thunderbirds, and no NHLTeamName_SEA/VGK/UTA exist. Use `locdb.py add` for new abbreviations (adds a record, size grows by the pool text + 16 bytes, see risks). Slots 30/31 have all-star names (Eastern/Western All-Stars), 32/33 North American / World.
- The scoreboard abbreviation seen earlier (SEA/VGK) came from nhlng.db; names in lists come from here. Which abbreviation the scoreboard uses for Arizona (ARZ vs the key PHX) depends on nhlng.db, not this file.

## Delivery
Copy the changed file to `cache/guest/fe/loc/nhl_eng_us.db` (the proven loose-file channel; remaster docs record "Pittsburgh" -> custom name visible in game). The installed `_compiled/.../nhl_eng_us.db` is already the maintainer's edited copy (+2,576 bytes of Norwegian loading/what's-new texts appended to the pool, `_original_backup/` has the stock 1,759,044-byte file): the game accepted that size. Base mods on the installed copy.

## Commands
`locdb.py set DB KEY TEXT --out OUT` (or `--set K=V` repeatedly): in place when the string is private to the key and not longer, else appended and the key repointed. Shared strings are never changed for other keys. Characters must exist in the Huffman tree (Latin-1 letters used by the game, UTF-8 sign bytes); a missing byte is an error (workaround: none; the tree is fixed).

## Risks
- Size/records: set with in-place edits keeps the size; append grows the file (proven for +2,576 bytes); `add` also changes the row count (max rows +1), not yet tested in game.
- The table has no index (the meta has one, but see the remaster notes: the loader does not build it): lookup scans records, so duplicates of one hashid are dead; `check` finds them.
- Case and collisions: `set` finds keys by hashid, so `TEAMLINE1_EAS` and `<key>` are one key.
- Other languages: cze_cz, eng_us, fin_fi, fre_fr, ger_de, rus_ru, swe_se exist (plus `nhl_lng_lg.db`, 840 KB, a different table, not decoded); the language is chosen by the game: a mod must patch the language file the player uses (all use the same keys).
- Text length: long names may clip in UI widgets (not tested).

## Utah test copy (not run in game)
`<workdir>/p-loc/nhl_eng_us_utah.db` (1,761,620 bytes, same size as installed): number 22 and PHX keys set to "Utah Mammoth®" / "Utah" / "Mammoth®" / "Mammoth" / abbreviation UTA (16 keys: NHLTeamName_22/PHX, Logo46, NHLCityName_22/PHX, CITYLINE1_22, TEAMLINE1_PHX, NICKLINE2_22/PHX, TEAMLINE2_PHX/ARZ, TXT_NICKNAME_PHX/ALT_PHX, NHLTeamName_Abbr3_22/PHX, X_XLA_TEAM_PHX).
Test: copy to `cache/guest/fe/loc/nhl_eng_us.db`, boot, open Play Now team select (Arizona sits between Philadelphia and Pittsburgh alphabetically by the old city: the list order comes from nhlng.db), check name, city line and the line-card header for "Utah Mammoth"; expect no change in the scoreboard abbreviation (nhlng.db).
