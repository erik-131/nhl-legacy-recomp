// 32-team Play Now needs NO code hook: the SELECT TEAMS list is data driven (see docs/team-hooks.md). With the
// widened DB + meta (tools/rosters/build_32.py) rows 30/31 appear in the NHL list on their own, and their names come
// from loc keys added with tools/rosters/locdb.py. The earlier hook on the league-0 loader sub_82A1E910 (which never ran
// in the Play Now path) was removed. This translation unit is kept (listed in CMakeLists.txt) as the place for future
// team-list hooks, e.g. for conference/division structure or franchise mode.
#include "generated/default/nhllegacy_init.h"
