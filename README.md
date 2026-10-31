# LiD Daily Quests

The online game's 808 one-off event quests, back in LET IT DIE (Steam,
offline) as daily quests: a few of them, picked at random for each player,
open on the quest board every game day. One a day (a setting) pays a decal
that cannot be had any other way offline on its first clear; the others pay
Death Metals.

- **Daily Quests** (`mods/Daily Quests`): the mod for the LET IT DIE DB Mod
  Manager - the quests, their goals and their rewards.
- **lid-dailyquests.dll** (`dll/`): a plugin for the
  [LiD Plugin Loader](https://github.com/NicoMullin/LiD-Plugin-Loader) that
  picks the day's quests.

## What you get

Set it in the manager, on the Daily Quests mod's **Configuration** tab:

| Setting                     | Default                       | You can choose                                    |
|-----------------------------|-------------------------------|---------------------------------------------------|
| Quests a day                | 5                             | 1 to 30                                           |
| Decal quests a day          | 1                             | 0 to 30                                           |
| Death Metal quests pay      | 1 to 5 Death Metals           | up to 4 to 20 Death Metals, or the online rewards |

- A new set opens at the game's daily reset (10:00 UTC), or when the
  manager's **Start a new game day** is used. The picks stay the same all
  day, restarts included, and every player gets their own.
- 80 quests, one for each of the 80 decals that no stew, shop, mushroom,
  death box or quest gives offline, pay that decal on the first clear. 33 of
  them are collab decals the game only shows with the **LET IT DIE Crossover
  Content** pack on; quests paying those only come up while the pack is on.
- The other quests pay Death Metals on every clear, and a decal quest does
  after its first.
- Death Metals need the **Death Metals** mod. Without it the offline game pays
  each Death Metal as 5,000 Kill Coins.

## How it works

**The mod.** The one-off quests survive offline only in
`master_quest_online`, and with no goal: not one has a row in any
`master_quest_param_*` table, so "Defeat #0 x#1" has nothing to defeat.
[tools/build_daily_quests.py](tools/build_daily_quests.py) writes them into
`master_quest` from a stock `masters.db`:

- each keeps its name, kind, count, difficulty and picture;
- its goal is borrowed from a weekly quest of the same kind and description,
  the one whose count is closest (so a "x40" quest gets a common enemy and a
  "x2" one a boss), with that quest's floor requirement; Tengoku floor quests
  get their floor directly;
- 80 of them, one per decal, pay that decal on the first clear; the rest
  pay Death Metals (`RWD_MEDAL_N`, set by the mod's settings);
- it is **parked**: `start_date` 1 and `end_date` 2, both long past, so the
  game shows none of them. (0 would mean "always open".)

The mod also writes `master_const_int` `LID_DAILY_QUEST_COUNT` and
`LID_DAILY_DECAL_COUNT`, and the Death Metal rewards (`RWD_MEDAL_N`, which the
Death Metals mod turns into real Death Metals).

**The plugin.** Each day it picks `LID_DAILY_QUEST_COUNT` of the parked
quests: `LID_DAILY_DECAL_COUNT` of them from the decal quests, the rest from
the others (if one side runs short, the other makes up the count). They are
shuffled by the save's login count and a random number made once for this
install (kept in `lid-dailyquests.ini`). The game raises the login count once
for every day it counts, so a real new day and the manager's "Start a new game
day" both bring new picks; if the count is not found, the picks follow the
game day (10:00 UTC) instead. A quest whose decal the game cannot show is
left out: the decal must not be console-only (`master_skill.platform` 0) and
its icon (`CookedPCConsole\UI_<decal>_SF.upk`) must be there. The picks get the
day as their dates, wherever the game asks:

- the game's built-in offline server checks that a quest is open before it
  lists it, takes it or clears it. The plugin takes the one call to that check
  and sets the dates in the server's copy of the quest first: today for a pick,
  the parking dates for the rest;
- the client reads its copy through `BrgDatabase.GetQuestList`,
  `GetQuestByIndex` and `GetQuest`. The plugin takes those three slots of the
  database's function table (on the game's thread) and sets the dates in what
  they return.

Safety:

- Every piece of game code is found by its exact bytes, exactly once, or
  nothing is changed and `lid-dailyquests.log` says so.
- Changed in the game: one 5-byte call (plus a 12-byte stub), three
  function-table slots, and the main loop's `GetForegroundWindow` import.
  The save's login count is only read.
- Only quests in the pool are touched; in the client's copies only once their
  parking dates are seen where the dates belong.
- `masters.db` is opened read-only. No network, no registry. It writes only
  `lid-dailyquests.log` and `lid-dailyquests.ini` next to itself.
- Delete `lid-dailyquests.dll` and the game is exactly stock again: the pool
  stays parked, so no daily quest shows.

## Why the DLL is built by GitHub

A DLL can do anything, so you should not have to take one on trust. Every
release on this page is built by GitHub Actions from the source in this
repository, on GitHub's own machines, and the tests run before anything is
published. You can check the DLL in two ways:

1. **Signed build record.** Each release DLL has an attestation: a signed
   record, kept by GitHub, of the exact workflow run and commit that built it.
   With the [GitHub CLI](https://cli.github.com/):

   ```
   gh attestation verify lid-dailyquests.dll --repo NicoMullin/LiD-Daily-Quests
   ```

2. **Rebuild it yourself.** The build is reproducible: the same source and the
   same compiler give a byte-identical file. See
   [Building it yourself](#building-it-yourself) and compare the SHA-256 with
   the one on the release page.

## Installing

**The easy way.** In the LET IT DIE DB Mod Manager, close the game, click
**Get mods...** and then **Get** next to **Daily Quests**. It downloads this
repository's newest release, checks it against the SHA-256 GitHub lists for
it, and once you say yes adds the mod and puts `lid-dailyquests.dll` in the
game's `Binaries\Win64\lid-plugins` - with the
[LiD Plugin Loader](https://github.com/NicoMullin/LiD-Plugin-Loader) too, if
you do not have it yet. Then tick **Daily Quests** and click **Save Mod
List**. The plugin is switched on and off with the mod.

**By hand.**

1. Install the [LiD Plugin Loader](https://github.com/NicoMullin/LiD-Plugin-Loader)
   once, if you have not already.
2. Download `LiD-Daily-Quests.zip` from the
   [latest release](https://github.com/NicoMullin/LiD-Daily-Quests/releases/latest).
   Put `lid-plugins\lid-dailyquests.dll` from it in the game's
   `Binaries\Win64\lid-plugins`, and drop `mods\Daily Quests` on the mod
   manager's window.

It works alongside other plugins, such as LiD Death Metals, LiD HUD and LiD
Vending Machine: they change different parts of the game.

## Building it yourself

You need Windows and Python 3. The compiler is Zig, installed with pip; no
Visual Studio needed.

```
py -3 -m pip install ziglang==0.16.0
py -3 build.py --test
```

The DLL lands in `out\lid-dailyquests.dll` and the SHA-256 is printed at the
end. Use Zig 0.16.0 exactly: another version builds a working DLL, but not a
byte-identical one.

The plugin reads `masters.db` with [SQLite](https://www.sqlite.org/) (public
domain). Its source is not kept here: the first build downloads the official
amalgamation (3.53.4) from sqlite.org into `third_party\` and refuses it unless
its SHA3-256 matches the one sqlite.org publishes.

`--test` runs the plugin's own code against a small `masters.db` and checks
that:

- every parked quest is in the pool and nothing else is;
- quests whose decal is console-only or whose icon is missing never come up;
- the counts come from `LID_DAILY_QUEST_COUNT` and `LID_DAILY_DECAL_COUNT`,
  with the split kept when one side runs short; the picks stay the same all
  game day and after a restart, and change the next day;
- with a login count, the picks follow it rather than the clock: a new count
  brings new picks, the same count the same ones;
- a pick gets today 10:00 UTC to tomorrow 10:00 UTC in the server's copy and
  the client's, and every other quest keeps its parking dates or is left alone;
- with none of the game code, nothing is changed and the log says so.

`--exe PATH\BrgGame-Steam.exe` also runs `tests/realexe.c`: the plugin's own
search on the real game file (mapped, not run), which must find every piece.
The game is not in this repository, so GitHub does not run it.

To rebuild the mod's quests after a game update:

```
py -3 tools/build_daily_quests.py --vanilla PATH\TO\stock\masters.db
```

## Making a release

1. Set the new version in [dll/version.h](dll/version.h).
2. Push a tag with the same version, for example `v0.1.0`.

GitHub builds, tests and publishes the release. The build stops if the tag and
`version.h` don't match.

## License

MIT - see [LICENSE](LICENSE). Made by KSFA.
