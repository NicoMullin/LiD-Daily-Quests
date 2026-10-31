"""Build lid-dailyquests.dll, and with --test run the tests against it.

GitHub builds every release with this same script, so anyone can rebuild the
DLL and get the exact same file:

    py -3 -m pip install ziglang==0.16.0
    py -3 build.py --test

The DLL lands in out/lid-dailyquests.dll and its SHA-256 is printed; compare
it with the one on the release page.

The plugin reads masters.db with SQLite. Its source is not kept in this
repository: the first build downloads the official amalgamation from
sqlite.org into third_party/ and refuses it unless its SHA3-256 matches the
one sqlite.org publishes for that release (pinned below).
"""
import argparse
import hashlib
import io
import json
import re
import shutil
import sqlite3
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ZIG_VERSION = "0.16.0"
SQLITE_VERSION = "3.53.4"
SQLITE_URL = "https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip"
SQLITE_SHA3_256 = "628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"
# Only what a read-only lookup needs: no threads (the game calls the plugin on
# one thread), no extensions, no shared cache.
SQLITE_OPTIONS = ["-DSQLITE_THREADSAFE=0", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OMIT_SHARED_CACHE",
                  "-DSQLITE_OMIT_DEPRECATED", "-DSQLITE_DEFAULT_MEMSTATUS=0", "-DSQLITE_DQS=0",
                  "-DSQLITE_OMIT_PROGRESS_CALLBACK", "-DSQLITE_OMIT_AUTHORIZATION"]

REPO = Path(__file__).resolve().parent
DLL = REPO / "dll"
TESTS = REPO / "tests"
OUT = REPO / "out"
THIRD = REPO / "third_party"

# The release package (see package() below): the Daily Quests mod and its plugin.
PACKAGE = "LiD-Daily-Quests"
PACKAGE_NAME = "Daily Quests"
PACKAGE_MODS = ["Daily Quests"]
PACKAGE_REPLACES = []   # older plugin names the manager removes for this one


def zig(*args, cwd):
    subprocess.run([sys.executable, "-m", "ziglang", *map(str, args)], cwd=cwd, check=True)


def check_zig():
    try:
        found = subprocess.run([sys.executable, "-m", "ziglang", "version"],
                               capture_output=True, text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        sys.exit(f"Zig is missing. Install it with: py -3 -m pip install ziglang=={ZIG_VERSION}")
    if found != ZIG_VERSION:
        sys.exit(f"Zig {found} is installed, but the releases are built with {ZIG_VERSION}; "
                 f"a different version gives a different file. "
                 f"Run: py -3 -m pip install ziglang=={ZIG_VERSION}")


def fetch_sqlite():
    """sqlite3.c and sqlite3.h in third_party/, downloaded once and checked."""
    folder = THIRD / f"sqlite-{SQLITE_VERSION}"
    if (folder / "sqlite3.c").exists() and (folder / "sqlite3.h").exists():
        return folder
    print(f"Downloading SQLite {SQLITE_VERSION} from {SQLITE_URL}", flush=True)
    with urllib.request.urlopen(SQLITE_URL) as reply:
        data = reply.read()
    got = hashlib.sha3_256(data).hexdigest()
    if got != SQLITE_SHA3_256:
        sys.exit(f"The SQLite download's SHA3-256 is {got}, not {SQLITE_SHA3_256} - refused.")
    folder.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for name in ("sqlite3.c", "sqlite3.h"):
            member = next(m for m in z.namelist() if m.endswith("/" + name))
            (folder / name).write_bytes(z.read(member))
    return folder


def version():
    text = (DLL / "version.h").read_text()
    return re.search(r'#define LID_DLL_VERSION\s+"([^"]+)"', text).group(1)


def build_dll(sqlite):
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir()
    zig("cc", "-shared", "-target", "x86_64-windows-gnu", "-O2", "-s", "-o", "lid-dailyquests.dll",
        f"-I{sqlite}", *SQLITE_OPTIONS, DLL / "lid_dailyquests.c", sqlite / "sqlite3.c", DLL / "version.rc",
        cwd=OUT)
    for lib in OUT.glob("*.lib"):   # import library, not needed
        lib.unlink()
    return OUT / "lid-dailyquests.dll"


def make_test_db(root):
    """A masters.db with just what the plugin reads, two folders above the exe,
    and the decal icons it looks for. 60 parked quests: Q00-Q29 with an
    ordinary decal, Q30-Q34 with a console-only one, Q35-Q39 with a decal
    whose icon is missing, Q40-Q59 paying Death Metals; one quest that is not
    parked; a count of 7, 2 of them with a decal."""
    content = root / "BrgGame" / "Content"
    cooked = root / "BrgGame" / "CookedPCConsole"
    content.mkdir(parents=True)
    cooked.mkdir(parents=True)
    db = sqlite3.connect(content / "masters.db")
    db.execute("CREATE TABLE master_quest (qid TEXT, first_rwd TEXT, start_date INTEGER, end_date INTEGER)")
    db.execute("CREATE TABLE master_reward (rwdid TEXT, type TEXT, val0 TEXT)")
    db.execute("CREATE TABLE master_skill (id TEXT, platform INTEGER)")
    db.execute("CREATE TABLE master_const_int (id TEXT, value INTEGER)")
    for i in range(40):
        decal = f"SKL_TEST_{i:02d}_P"
        db.execute("INSERT INTO master_quest VALUES (?, ?, 1, 2)", (f"Q{i:02d}", f"RWD_T{i:02d}"))
        db.execute("INSERT INTO master_reward VALUES (?, 'SKILL', ?)", (f"RWD_T{i:02d}", decal))
        db.execute("INSERT INTO master_skill VALUES (?, ?)", (decal, 1 if 30 <= i < 35 else 0))
        if i < 35:
            (cooked / f"UI_SKL_TEST_{i:02d}_SF.upk").write_bytes(b"icon")
    db.execute("INSERT INTO master_reward VALUES ('RWD_MEDAL_1', 'MEDAL', '')")
    for i in range(40, 60):
        db.execute("INSERT INTO master_quest VALUES (?, 'RWD_MEDAL_1', 1, 2)", (f"Q{i:02d}",))
    db.execute("INSERT INTO master_quest VALUES ('OPEN', 'RWD_T00', 0, 0)")
    db.execute("INSERT INTO master_const_int VALUES ('LID_DAILY_QUEST_COUNT', 7)")
    db.execute("INSERT INTO master_const_int VALUES ('LID_DAILY_DECAL_COUNT', 2)")
    db.commit()
    db.close()


def run_test(name, sources, dll, *args, db=False):
    """Build a stand-in game exe where the real one sits (Binaries/Win64, next to
    a copy of the DLL if it loads it, with BrgGame/Content/masters.db two folders
    up) and run it."""
    root = OUT / "test" / name
    folder = root / "Binaries" / "Win64"
    folder.mkdir(parents=True)
    if dll:
        shutil.copy(dll, folder)
    if db:
        make_test_db(root)
    zig("cc", "-target", "x86_64-windows-gnu", "-O1", "-o", "BrgGame-Steam.exe", *sources, cwd=folder)
    print(f"\n--- test: {name}", flush=True)
    return subprocess.run([folder / "BrgGame-Steam.exe", *map(str, args)], cwd=folder).returncode == 0


# -- the release package -------------------------------------------------------
#
# One zip with everything the LET IT DIE DB Mod Manager needs to install this
# from its "Get mods" window: lid-package.json saying what is in it, the Daily
# Quests mod under mods/, and the plugin under lid-plugins/. Unpacked by hand it
# reads the same way: lid-plugins/ goes in the game's Binaries\Win64, the mod
# folder in the manager's mods folder. Same source, same zip: entries are
# sorted and dated 2026-01-01, so it rebuilds byte for byte.

PACKAGE_TIME = (2026, 1, 1, 0, 0, 0)


def _add(archive, name, data):
    info = zipfile.ZipInfo(name, PACKAGE_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    archive.writestr(info, data)


def package(plugins=()):
    """Write out/<PACKAGE>.zip and check it. ``plugins`` are built files in out/."""
    manifest = {
        "format": 1,
        "name": PACKAGE_NAME,
        "version": version(),
        "mods": list(PACKAGE_MODS),
        "plugins": [Path(p).name for p in plugins],
        "replaces": list(PACKAGE_REPLACES),
        "loader": "",
    }
    entries = {"lid-package.json": (json.dumps(manifest, indent=2) + "\n").encode()}
    for mod in PACKAGE_MODS:
        folder = REPO / "mods" / mod
        if not (folder / "mod.json").is_file():
            sys.exit(f"mods/{mod} has no mod.json - nothing packaged")
        for path in sorted(folder.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                entries[f"mods/{mod}/{path.relative_to(folder).as_posix()}"] = path.read_bytes()
    for plugin in plugins:
        entries[f"lid-plugins/{Path(plugin).name}"] = Path(plugin).read_bytes()

    out = OUT / f"{PACKAGE}.zip"
    with zipfile.ZipFile(out, "w") as archive:
        for name in sorted(entries):
            _add(archive, name, entries[name])
    with zipfile.ZipFile(out) as archive:     # read back what was written
        got = {name: archive.read(name) for name in archive.namelist()}
    if got != entries:
        sys.exit(f"{out.name} does not read back as written")
    print(f"{out.name}: {len(entries)} files, SHA-256 {hashlib.sha256(out.read_bytes()).hexdigest()}")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--test", action="store_true", help="also run the tests")
    parser.add_argument("--tag", help="fail unless this release tag (v1.2.3) matches dll/version.h")
    parser.add_argument("--package", action="store_true",
                        help="also write out/LiD-Daily-Quests.zip, the release package")
    parser.add_argument("--exe", type=Path,
                        help="with --test, also check the plugin finds everything in this BrgGame-Steam.exe")
    args = parser.parse_args()

    if args.tag and args.tag != "v" + version():
        sys.exit(f"Tag {args.tag} does not match dll/version.h ({version()}). Update one of them.")
    check_zig()
    sqlite = fetch_sqlite()
    dll = build_dll(sqlite)

    if args.test:
        with_sqlite = [f"-I{sqlite}", sqlite / "sqlite3.c", *SQLITE_OPTIONS]
        ok = run_test("logic", [TESTS / "logic.c", *with_sqlite], None, db=True)
        ok = run_test("unknown-game", [TESTS / "nocode.c"], dll) and ok
        if args.exe:
            ok = run_test("real-exe", [TESTS / "realexe.c", *with_sqlite], None, args.exe.resolve()) and ok
        shutil.rmtree(OUT / "test")
        if not ok:
            sys.exit("\nTESTS FAILED")

    if args.package:
        package(plugins=[dll])

    digest = hashlib.sha256(dll.read_bytes()).hexdigest()
    print(f"\nlid-dailyquests.dll {version()}  SHA-256 {digest}")


if __name__ == "__main__":
    main()
