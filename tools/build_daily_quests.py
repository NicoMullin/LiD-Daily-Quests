"""Build the Daily Quests mod's quest data from a stock masters.db.

A maintenance tool, not something a player runs. The online game ran 808
one-off event quests (anniversaries, download milestones, Valentine's, the
collabs). Offline they survive only in master_quest_online, and with no goal:
not one of them has a row in any master_quest_param_* table, so "Defeat #0 x#1"
has nothing to defeat. This writes them into master_quest as the pool the
Daily Quests plugin (lid-dailyquests.dll) picks from each game day:

  - Each quest keeps its own name, kind, count, difficulty, picture and category.
  - Its goal is borrowed from a weekly quest of the same kind and description
    (the game's own complete goals), the one whose count is closest, so a
    "x40" quest gets a common enemy and a "x2" one a boss. Its floor
    requirement is raised to the template's, so it only shows to players who
    can reach that target. The count shown in the description is its own.
    Tengoku floor quests are given their floor directly (100F = HVN_FLR_0050,
    the NEO routes R00 D.O.D, R01 W.E, R02 C.W, R03 M.I.L.K).
  - Its name's number is its place in its event (8th Anniversary #3).
  - 80 of them, one for each decal that cannot be had any other way offline
    (in no stew, shop, mushroom, death box or quest), pay that decal on the
    first clear. The rest pay Death Metals from the first clear, and the
    decal quests do too after theirs (set by the mod). The plugin picks the
    mod's "decal quests a day" from the 80 and the rest of the day's from
    the others.
  - It waits for no other quest, and is parked with start 1 / end 2: closed
    until the plugin opens it for the day. start and end of 0 would mean
    "always open", so the parking dates are 1 and 2 on purpose.

Also written: the count texts 99 and 999 (no TRG_NUM row exists for them),
names for the two events the game has no text for (9th Anniversary, made from
8th; Valentine's 2026, from 2025), in every language, and the pool's own copy
of each quest name with "[DAILY]" in front, so a daily quest stands out on the
quest board (the game's names are shared with its other quests).

    py -3 tools/build_daily_quests.py --vanilla PATH/TO/stock/masters.db
"""

from __future__ import annotations

import argparse
import math
import random
import re
import sqlite3
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "mods" / "Daily Quests" / "quests.sql"
SEED = 20261024                 # the shuffles below are fixed, so the output is too
PARK_START, PARK_END = 1, 2
DAILY_TAG = "[DAILY]"           # in front of every pool quest's name, in every language
ROUTES = {"NEODOD": "R00", "NEOWE": "R01", "NEOCW": "R02", "NEOMILK": "R03"}

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("--vanilla", required=True, type=Path, help="a stock masters.db")
parser.add_argument("--out", type=Path, default=OUT)
args = parser.parse_args()

con = sqlite3.connect(f"file:{args.vanilla.as_posix()}?mode=ro", uri=True)
con.row_factory = sqlite3.Row
rng = random.Random(SEED)


def rows(sql: str, *params) -> list[sqlite3.Row]:
    return con.execute(sql, params).fetchall()


def text(key: str, lang: str = "int") -> str | None:
    if not key or "." not in key:
        return None
    sct, tid = key.split(".", 1)
    row = con.execute("SELECT txt FROM master_text WHERE sct = ? AND id = ? AND lang = ?", (sct, tid, lang)).fetchone()
    return row[0] if row else None


PARAM_TABLES = ("master_quest_param_str", "master_quest_param_int", "master_quest_param_text",
                "master_quest_param_desc_text")


def params(qid: str) -> dict[str, list[tuple]]:
    return {t: [tuple(r) for r in con.execute(f"SELECT no, val FROM {t} WHERE qid = ? ORDER BY no", (qid,))]
            for t in PARAM_TABLES}


# ------------------------------------------------------------------ the pool
pool = rows("SELECT * FROM master_quest_online WHERE event_type = 'QUESTEVENT_NORMAL' "
            "AND qid NOT IN (SELECT qid FROM master_quest) ORDER BY start_date, qid")
if len(pool) != 808:
    sys.exit(f"{len(pool)} one-off quests, not the 808 this was written for")
weeklies = rows("SELECT * FROM master_quest WHERE qid IN (SELECT qid FROM master_quest_param_str "
                "UNION SELECT qid FROM master_quest_param_desc_text)")
by_kind: dict[tuple, list] = defaultdict(list)
by_type: dict[str, list] = defaultdict(list)
for w in weeklies:
    by_kind[(w["type"], w["desc"])].append(w)
    by_type[w["type"]].append(w)


def template(q: sqlite3.Row) -> sqlite3.Row:
    """A weekly of the same kind (and description, if there is one) with the closest count."""
    found = by_kind.get((q["type"], q["desc"])) or by_type.get(q["type"])
    if not found:
        sys.exit(f"{q['qid']}: no weekly {q['type']} to borrow a goal from")
    want = math.log(max(q["prgmax"], 1))
    found = sorted(found, key=lambda w: (abs(math.log(max(w["prgmax"], 1)) - want), w["qid"]))
    near = [w for w in found if abs(math.log(max(w["prgmax"], 1)) - want)
            <= abs(math.log(max(found[0]["prgmax"], 1)) - want) + 0.35]
    return rng.choice(near[:12])


trg_have = {int(r[0]) for r in rows("SELECT DISTINCT substr(id, 5) FROM master_text WHERE sct = 'TRG_NUM'")
            if r[0].isdigit()}


def count_text(n: int) -> str:
    return f"TRG_NUM.TXT_{n}"


def floor_goal(q: sqlite3.Row) -> list[tuple] | None:
    m = re.fullmatch(r"QUEST_DES\.TXT_(BARBS|NEODOD|NEOWE|NEOCW|NEOMILK)_(\d+)_DISRUPT", q["desc"])
    if not m:
        return None
    floor = int(m.group(2))
    if m.group(1) == "BARBS":
        return [(0, f"HVN_FLR_{floor - 50:04d}")]
    return [(0, f"HVN_FLR_{ROUTES[m.group(1)]}_{floor - 50:04d}")]


# --------------------------------------------------------------- the decals
skills = {r["id"]: r for r in rows("SELECT * FROM master_skill")}
reach = set()
reach |= {r[0] for r in rows("SELECT sklid FROM master_skillgacha_odds")}
reach |= {r[0] for r in rows("SELECT sklid FROM master_skill_open_mushroom")}
reach |= {k for k, s in skills.items() if s["is_nosale"] == 0
          and (s["buy_money"] or s["buy_spirit"] or s["buy_recycle_point"])}
skill_rewards = {r["rwdid"]: r["val0"] for r in rows("SELECT rwdid, val0 FROM master_reward WHERE type = 'SKILL'")}
reach |= {v for k, v in skill_rewards.items() if k.startswith("RWD_DEATHBOX")}
for r in rows("SELECT first_rwd, rwd FROM master_quest"):
    reach |= {skill_rewards[x] for x in r if x in skill_rewards}
base = lambda s: s[:-2] if s.endswith("_P") else s        # noqa: E731
reach_base = {base(s) for s in reach}
decals = sorted(k for k in skills if base(k) not in reach_base)
if len(decals) != 80:
    sys.exit(f"{len(decals)} unearnable decals, not the 80 this was written for")

# ------------------------------------------------------------------ build it
used_no = {r[0] for r in rows("SELECT no FROM master_quest")}
quest_rows, param_rows, notes = [], defaultdict(list), Counter()
seq: Counter = Counter()
order = list(range(len(pool)))
rng.shuffle(order)
decal_of = {pool[i]["qid"]: decals[n] for n, i in enumerate(order[:len(decals)])}   # one quest per decal
reward_id = {d: f"RWD_LID_DQ_{n + 1:03d}" for n, d in enumerate(decals)}
DM_FIRST = "RWD_MEDAL_1"            # the others' first clear; the mod's settings set the amount
need_counts: set[int] = set()
next_no = max(used_no | {r["no"] for r in pool}) + 1


def daily_name(key: str) -> str:
    """The pool's own copy of a quest name, shown with DAILY_TAG in front: the
    game's names are shared with its other quests, which keep theirs as they are."""
    sct, tid = key.split(".", 1)
    return f"{sct}.TXT_LID_DAILY_{tid[4:] if tid.startswith('TXT_') else tid}"


for q in pool:
    qid = q["qid"]
    seq[q["name"]] += 1
    name_text = text(q["name"]) or text("QUEST_NAME.TXT_8AN") or ""
    goal = floor_goal(q) if q["type"] == "QUESTTP_CLEAR_SPECIFIC_HVN_FLOORS" else None
    cond_flr = q["cond_flr"]
    desc = q["desc"]
    if goal is not None:
        p = {t: [] for t in PARAM_TABLES}
        p["master_quest_param_str"] = goal
        notes["floor"] += 1
    else:
        w = template(q)
        p = params(w["qid"])
        cond_flr = max(cond_flr, w["cond_flr"])
        if (q["type"], q["desc"]) not in by_kind:
            desc = w["desc"]                # its params fit the template's text, not ours
            notes["template text"] += 1
        notes["borrowed"] += 1
        # the description's count is this quest's own
        fixed = []
        for no, val in p["master_quest_param_desc_text"]:
            if isinstance(val, str) and val.startswith("TRG_NUM.TXT_"):
                val = count_text(q["prgmax"])
                need_counts.add(q["prgmax"])
            fixed.append((no, val))
        p["master_quest_param_desc_text"] = fixed
        target_name = next((v for n, v in params(w["qid"])["master_quest_param_text"]
                            if isinstance(v, str) and not v.startswith("NUMBER.")), None) \
            or next((v for n, v in fixed if isinstance(v, str) and not v.startswith("TRG_NUM.")), None)
    # the name: "<event> #0" takes its number, "Hunting #0 #1" the target and the number
    number = f"NUMBER.TXT_{seq[q['name']]:03d}"
    if "#1" in name_text and goal is None and target_name:
        p["master_quest_param_text"] = [(0, target_name), (1, number)]
    elif "#0" in name_text:
        p["master_quest_param_text"] = [(0, number)]
    else:
        p["master_quest_param_text"] = []
    no = q["no"] if q["no"] not in used_no else next_no
    if no == next_no:
        next_no += 1
    used_no.add(no)
    first = reward_id[decal_of[qid]] if qid in decal_of else DM_FIRST
    quest_rows.append((qid, no, daily_name(q["name"]), desc, q["img"], q["cat"], q["type"], "QUESTEVENT_NORMAL",
                       cond_flr, "", "", q["lvl"], first, "RWD_MEDAL_1", q["prgmax"],
                       PARK_START, PARK_END, 0))
    for table, values in p.items():
        for n, v in values:
            param_rows[table].append((qid, n, v))


def lit(v) -> str:
    if v is None:
        return "NULL"
    if isinstance(v, (int, float)):
        return repr(v)
    return "'" + str(v).replace("'", "''") + "'"


out = [
    "-- Daily Quests: the online game's 808 one-off event quests, as the pool lid-dailyquests.dll",
    "-- picks from each game day. Each is parked (start 1, end 2: closed) until the plugin opens it.",
    "-- Goals borrowed from the game's weekly quests. 80 of them pay an otherwise unearnable decal on",
    "-- the first clear, one quest per decal; the rest pay Death Metals (the mod's settings set how many).",
    "-- Generated by tools/build_daily_quests.py from a stock masters.db - do not edit by hand.",
    "",
]
cols = ("qid", "no", "name", "desc", "img", "cat", "type", "event_type", "cond_flr", "cond_quest_id", "cond",
        "lvl", "first_rwd", "rwd", "prgmax", "start_date", "end_date", "old_flg")
out.append(f"-- master_quest: {len(quest_rows)} quests")
for r in quest_rows:
    out.append(f"INSERT INTO master_quest ({', '.join(cols)}) VALUES ({', '.join(lit(v) for v in r)});")
for table in PARAM_TABLES:
    out.append(f"\n-- {table}: {len(param_rows[table])} rows")
    for r in param_rows[table]:
        out.append(f"INSERT INTO {table} (qid, no, val) VALUES ({', '.join(lit(v) for v in r)});")
out.append(f"\n-- master_reward: the {len(decals)} decals, one first-clear reward each")
for d in decals:
    nm = text(skills[d]["name"]) or d
    out.append("INSERT INTO master_reward (rwdid, name, type, num, val0, val1, val2) VALUES "
               f"({lit(reward_id[d])}, {lit('Daily quest decal: ' + nm)}, 'SKILL', 1, {lit(d)}, '', '');")

# texts the game lacks
langs = [r[0] for r in rows("SELECT DISTINCT lang FROM master_text WHERE sct = 'TRG_NUM' AND id = 'TXT_100'")]
missing = sorted(n for n in need_counts if n not in trg_have)
sample = rows("SELECT * FROM master_text WHERE sct = 'TRG_NUM' AND id = 'TXT_100'")
text_cols = sample[0].keys()
out.append(f"\n-- master_text: count texts the game has no row for ({', '.join(map(str, missing))}),")
out.append("-- and the names of the two events with no text (9th Anniversary, Valentine's 2026)")
for n in missing:
    for s in sample:
        row = dict(s)
        row["id"], row["txt"] = f"TXT_{n}", str(n)
        out.append(f"INSERT INTO master_text ({', '.join(text_cols)}) VALUES ({', '.join(lit(row[c]) for c in text_cols)});")
made: dict[str, list[dict]] = {}       # QUEST_NAME ids written here, the game having none
for new_id, src_id, old, new in (("TXT_9AN", "TXT_8AN", "8", "9"),
                                  ("TXT_VALENTINE2026", "TXT_VALENTINE2025", "2025", "2026")):
    if rows("SELECT 1 FROM master_text WHERE sct = 'QUEST_NAME' AND id = ?", new_id):
        continue
    for s in rows("SELECT * FROM master_text WHERE sct = 'QUEST_NAME' AND id = ?", src_id):
        row = dict(s)
        row["id"], row["txt"] = new_id, row["txt"].replace(old, new)
        made.setdefault(new_id, []).append(row)
        out.append(f"INSERT INTO master_text ({', '.join(text_cols)}) VALUES ({', '.join(lit(row[c]) for c in text_cols)});")

# the pool's names, each the game's (or one made above) with DAILY_TAG in front, so a
# daily quest is told apart on the quest board from the weekly and event ones
names = sorted({q["name"] for q in pool})
out.append(f"\n-- master_text: the {len(names)} quest names the pool uses, as its own copies marked {DAILY_TAG}")
tagged = 0
for key in names:
    sct, tid = key.split(".", 1)
    source = made.get(tid) or [dict(s) for s in rows("SELECT * FROM master_text WHERE sct = ? AND id = ?", sct, tid)]
    if not source:
        sys.exit(f"{key}: a pool quest's name has no text")
    for row in source:
        row = dict(row)
        row["id"] = daily_name(key).split(".", 1)[1]
        row["txt"] = f"{DAILY_TAG} {row['txt']}"
        out.append(f"INSERT INTO master_text ({', '.join(text_cols)}) VALUES ({', '.join(lit(row[c]) for c in text_cols)});")
        tagged += 1

args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text("\n".join(out) + "\n", encoding="utf-8")
print(f"{len(quest_rows)} quests ({dict(notes)}), {sum(len(v) for v in param_rows.values())} goal rows, "
      f"{len(decals)} decal rewards, count texts added for {missing}, {len(names)} names marked "
      f"{DAILY_TAG} ({tagged} texts)")
print(f"wrote {args.out} ({args.out.stat().st_size // 1024:,} KB)")
