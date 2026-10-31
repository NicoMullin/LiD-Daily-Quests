/*
 * lid-dailyquests.dll for Let It Die - a plugin for the LiD Plugin Loader that
 * opens a few random quests a day from the Daily Quests mod's pool. It changes
 * game code in memory only.
 *
 * The Daily Quests mod puts the online game's one-off event quests into
 * master_quest "parked": start_date 1 and end_date 2, both long past, so the
 * game shows none of them. Each game day (10:00 UTC) the plugin picks
 * master_const_int LID_DAILY_QUEST_COUNT of them at random - of those,
 * LID_DAILY_DECAL_COUNT from the quests whose first clear pays a decal, the
 * rest from the others - and gives those the day as their dates, wherever
 * the game asks:
 *
 *   - the game's built-in offline server checks a quest is open before it
 *     lists it, takes it or clears it. The plugin takes the one call to that
 *     check, sets the quest's dates in the server's own copy (today for a
 *     pick, the parking dates for the rest), then runs the check;
 *   - the client reads its copy through BrgDatabase.GetQuestList,
 *     GetQuestByIndex and GetQuest. The plugin takes those three slots of the
 *     database's function table and sets the dates in what they hand back.
 *
 * The picks are random for each player and the same all day, game restarts
 * included: they come from the save's login count and a random number made
 * once for this install and kept in lid-dailyquests.ini. The game raises the
 * login count once for every day it counts, so a day started early with the
 * mod manager's "Start a new game day" brings new picks too. If the count is
 * not found, the picks follow the game day instead.
 *
 * A quest whose first-clear decal the game cannot show is never picked: a
 * collab decal needs the LET IT DIE Crossover Content pack, which clears its
 * console-only flag (master_skill.platform) and brings its icon
 * (CookedPCConsole\UI_<decal>_SF.upk). With the pack on, every quest can come
 * up.
 *
 * Safety rules:
 *   - Each piece of game code is found by its exact bytes, exactly once, or
 *     that part changes nothing (the log says which).
 *   - Changed in the game: one 5-byte call (plus a 12-byte stub in a page of
 *     its own), three function-table slots, and the main loop's
 *     GetForegroundWindow import (to do the table slots on the game's thread).
 *     The login count is only read.
 *   - Only quests in the pool are touched, and in the client's copies only
 *     once their parking dates are seen where the dates belong.
 *   - masters.db is opened read-only. No network, no registry. It writes only
 *     lid-dailyquests.log and lid-dailyquests.ini next to itself.
 *   - Delete lid-dailyquests.dll and the game is exactly stock again: the pool
 *     stays parked, so no daily quest shows.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sqlite3.h"
#include "version.h"

/* ------------------------------------------------------------------ log */

static wchar_t g_log_path[MAX_PATH];
static wchar_t g_ini_path[MAX_PATH];
static int g_log_started;

static void log_line(const char *fmt, ...)
{
    char msg[1024];
    SYSTEMTIME t;
    va_list ap;
    int n;
    HANDLE f;
    DWORD written;

    if (!g_log_path[0])
        return;
    GetLocalTime(&t);
    n = snprintf(msg, sizeof msg, "%02u:%02u:%02u  ", t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt);
    n += vsnprintf(msg + n, sizeof msg - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof msg - 3)
        n = (int)sizeof msg - 3;
    msg[n++] = '\r';
    msg[n++] = '\n';

    /* a fresh log each time the game starts */
    f = CreateFileW(g_log_path, g_log_started ? FILE_APPEND_DATA : GENERIC_WRITE,
                    FILE_SHARE_READ, NULL, g_log_started ? OPEN_ALWAYS : CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    g_log_started = 1;
    WriteFile(f, msg, (DWORD)n, &written, NULL);
    CloseHandle(f);
}

/* ------------------------------------------------------------ helpers */

struct span {
    unsigned char *start;
    size_t size;
};

static int section(unsigned char *base, const char *name, struct span *out)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    WORD i;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (strncmp((const char *)sec->Name, name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
            out->start = base + sec->VirtualAddress;
            out->size = sec->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

static int inside(struct span s, const void *p, size_t n)
{
    const unsigned char *b = p;
    return b >= s.start && n <= s.size && b - s.start <= (ptrdiff_t)(s.size - n);
}

/* Memory that can be read: committed, not a guard page, not no-access. */
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char *a = p, *end = a + n;

    if ((ULONG_PTR)a < 0x10000 || n == 0 || end < a)
        return 0;
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !mbi.Protect ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return 0;
        a = (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static int32_t rd32(const unsigned char *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

static unsigned char *call_target(unsigned char *call)
{
    return call + 5 + rd32(call + 1);
}

/* A byte pattern like "48 8B 0D ?? ?? ?? ?? E8", found exactly once in `s`. */
static unsigned char *find_once(struct span s, const char *pattern)
{
    unsigned char bytes[160], mask[160];
    int n = 0, first = -1, k;
    const char *c = pattern;
    unsigned char *p, *end, *hit = NULL;

    while (*c && n < (int)sizeof bytes) {
        while (*c == ' ')
            c++;
        if (!*c)
            break;
        if (c[0] == '?') {
            mask[n] = 0;
            bytes[n] = 0;
        } else {
            unsigned v = 0;
            sscanf(c, "%2x", &v);
            mask[n] = 1;
            bytes[n] = (unsigned char)v;
            if (first < 0)
                first = n;
        }
        n++;
        c += 2;
    }
    if (first < 0 || (size_t)n > s.size)
        return NULL;
    end = s.start + s.size - n;
    for (p = s.start + first; p <= end + first; p++) {
        p = memchr(p, bytes[first], (size_t)(end + first - p) + 1);
        if (!p)
            break;
        for (k = 0; k < n; k++)
            if (mask[k] && p[k - first] != bytes[k])
                break;
        if (k == n) {
            if (hit)
                return NULL;        /* twice: not the code we know */
            hit = p - first;
        }
    }
    return hit;
}

static int write_code(unsigned char *at, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

static int write_pointer(void **at, void *value)
{
    DWORD old;
    if (!VirtualProtect(at, sizeof *at, PAGE_READWRITE, &old))
        return 0;
    *at = value;
    VirtualProtect(at, sizeof *at, old, &old);
    return 1;
}

/* --------------------------------------------------------- the pieces */

/* The offline server's "is this quest open" check (on its own copy of a
 * quest: the floor needed, then start and end as ints, -1 for none):
 *   mov [rsp+10],rbx / push rsi / sub rsp,20 / mov rsi,r8 / mov rbx,rcx /
 *   cmp edx,[rcx+FLOOR] / jg / xor al,al / ... ret /
 *   mov [rsp+30],rdi / movsxd rdi,[rcx+START] / cmp edi,-1 / je / call now /
 *   cmp rdi,rax / jg closed / movsxd rdi,[rbx+END] / cmp edi,-1 */
static const char OPEN_PATTERN[] =
    "48 89 5C 24 10 56 48 83 EC 20 49 8B F0 48 8B D9 3B 91 ?? ?? ?? ?? 7F 0D 32 C0 48 8B 5C 24 38 "
    "48 83 C4 20 5E C3 48 89 7C 24 30 48 63 B9 ?? ?? ?? ?? 83 FF FF 74 0E E8 ?? ?? ?? ?? 48 3B F8 "
    "0F 8F ?? ?? ?? ?? 48 63 BB ?? ?? ?? ?? 83 FF FF";
#define OPEN_START_AT 45    /* the disp32 of movsxd rdi,[rcx+START] */
#define OPEN_END_AT   71    /* the disp32 of movsxd rdi,[rbx+END] */

/* The server recording a new login day: push rbx / sub rsp,20 /
 * inc [login count] / mov rbx,rcx / lea rdx,[rcx-86400] / lea rcx,[rsp+30] /
 * call / mov rcx,[rax] / call / mov rcx,[last login] / cmp rax,rcx / jne /
 * inc [days in a row]. The last login sits 8 bytes after the count and the
 * days in a row 4 after it, which is checked. (lid-loginbonus.dll finds it
 * the same way.) */
static const char LOGIN_PATTERN[] =
    "40 53 48 83 EC 20 FF 05 ?? ?? ?? ?? 48 8B D9 48 8D 91 80 AE FE FF 48 8D 4C 24 30 E8 ?? ?? ?? ?? "
    "48 8B 08 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 3B C1 75 08 FF 05 ?? ?? ?? ??";
#define LOGIN_COUNT_AT 8
#define LOGIN_LAST_AT  43
#define LOGIN_ROW_AT   54

struct pieces {
    unsigned char *base;
    struct span text, rdata, data;
    LONG_PTR delta;                 /* where the exe is minus where it was built for */

    unsigned char *open_fn;         /* the server's quest-open check */
    unsigned char *open_site;       /* its one call */
    int open_calls;                 /* how many calls to it were found (needs 1) */
    int32_t off_start, off_end;     /* where its copy of a quest keeps the dates */

    int vt_list, vt_by_index, vt_get;   /* BrgDatabase function-table slots */
    void *find_object;              /* StaticFindObject */
    unsigned char *frame_call;      /* the main loop's GetForegroundWindow call */
    void **frame_import;            /* ... and its import slot */

    int32_t *login_count;           /* the save's login count (NULL: follow the game day) */
};

static struct pieces g_pc;

/* The engine registers each native function with its name, in a table of
 * (name, function) pairs: "UBrgDatabaseexecGetQuestList" leads to the
 * script-facing wrapper, which calls the real function through the object's
 * function table. */
static unsigned char *native_named(struct pieces *pc, const char *name)
{
    size_t n = strlen(name) + 1;
    unsigned char *s = pc->rdata.start, *end = pc->rdata.start + pc->rdata.size;

    while (s && s + n <= end) {
        s = memchr(s, name[0], (size_t)(end - s));
        if (!s || s + n > end)
            break;
        if (memcmp(s, name, n) == 0 && (s == pc->rdata.start || s[-1] == 0)) {
            ULONG_PTR want = (ULONG_PTR)s - pc->delta;  /* as the table stores it */
            unsigned char *q = pc->data.start + ((8 - ((ULONG_PTR)pc->data.start & 7)) & 7);

            for (; q + 16 <= pc->data.start + pc->data.size; q += 8) {
                ULONG_PTR a, f;
                memcpy(&a, q, 8);
                if (a != want)
                    continue;
                memcpy(&f, q + 8, 8);
                f += pc->delta;
                if (inside(pc->text, (void *)f, 1))
                    return (unsigned char *)f;
            }
        }
        s++;
    }
    return NULL;
}

/* The function-table slot a native wrapper calls: its last `call [reg+N]`
 * before its end. */
static int wrapper_slot(struct pieces *pc, const char *name)
{
    unsigned char *f = native_named(pc, name);
    int i, slot = 0;

    if (!f || !inside(pc->text, f, 0x402))
        return 0;
    for (i = 0; i < 0x400; i++) {
        if (f[i] == 0xC3 && f[i + 1] == 0xCC)
            break;
        if (f[i] == 0xFF && f[i + 1] != 0x94 && (f[i + 1] & 0xF8) == 0x90) {
            int32_t d = rd32(f + i + 2);
            if (d >= 0x100 && d < 0x2000)
                slot = d;
        }
    }
    return slot;
}

/* Find everything, without changing anything. `base` is the game's exe in
 * memory (the tests pass a mapped copy instead). */
static void find_pieces(unsigned char *base, struct pieces *pc)
{
    IMAGE_NT_HEADERS64 *nt;
    unsigned char *p;
    size_t i;

    memset(pc, 0, sizeof *pc);
    pc->base = base;
    if (!section(base, ".text", &pc->text) || !section(base, ".rdata", &pc->rdata) ||
        !section(base, ".data", &pc->data))
        return;
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    pc->delta = (LONG_PTR)base - (LONG_PTR)nt->OptionalHeader.ImageBase;

    pc->open_fn = find_once(pc->text, OPEN_PATTERN);
    if (pc->open_fn) {
        pc->off_start = rd32(pc->open_fn + OPEN_START_AT);
        pc->off_end = rd32(pc->open_fn + OPEN_END_AT);
        for (i = 0; i + 5 <= pc->text.size; i++) {
            p = pc->text.start + i;
            if (p[0] == 0xE8 && call_target(p) == pc->open_fn) {
                pc->open_site = p;
                pc->open_calls++;
            }
        }
        if (pc->open_calls != 1)
            pc->open_site = NULL;
    }

    pc->vt_list = wrapper_slot(pc, "UBrgDatabaseexecGetQuestList");
    pc->vt_by_index = wrapper_slot(pc, "UBrgDatabaseexecGetQuestByIndex");
    pc->vt_get = wrapper_slot(pc, "UBrgDatabaseexecGetQuest");
    pc->find_object = find_once(pc->text,
        "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 48 C7 44 24 50 FE FF FF FF 48 89 9C 24 C0 00 00 00");
    p = find_once(pc->text, "FF 15 ?? ?? ?? ?? 48 8B C8 48 8D 94 24 A8 00 00 00 FF 15 ?? ?? ?? ??");
    if (p) {
        pc->frame_call = p;
        pc->frame_import = (void **)(p + 6 + rd32(p + 2));
    }
    p = find_once(pc->text, LOGIN_PATTERN);
    if (p) {
        unsigned char *count = p + LOGIN_COUNT_AT + 4 + rd32(p + LOGIN_COUNT_AT);
        if (p + LOGIN_LAST_AT + 4 + rd32(p + LOGIN_LAST_AT) == count + 8 &&
            p + LOGIN_ROW_AT + 4 + rd32(p + LOGIN_ROW_AT) == count + 4 && inside(pc->data, count, 4))
            pc->login_count = (int32_t *)count;
    }
}

static int server_ok(const struct pieces *pc)
{
    return pc->open_site && pc->off_start > 0 && pc->off_start < 0x400 && pc->off_end > 0 && pc->off_end < 0x400;
}

static int client_ok(const struct pieces *pc)
{
    return pc->vt_list && pc->vt_by_index && pc->vt_get && pc->find_object && pc->frame_import;
}

/* ------------------------------------------------------------ the pool */

#define MAX_POOL  2048
#define QID_MAX   48
#define DAY       86400
#define DAY_START (10 * 3600)       /* a game day starts at 10:00 UTC */
#define PARK_START 1
#define PARK_END   2

struct entry {
    wchar_t qid[QID_MAX];
    char decal[64];
    int has_decal;                  /* its first clear pays a decal */
    int usable;                     /* its decal can be shown (or it has none) */
    int picked;                     /* open today */
};

static struct entry g_pool[MAX_POOL];
static int g_npool, g_nusable, g_count = 5, g_decals = -1, g_loaded;  /* g_decals -1: no split */
static int16_t g_index[MAX_POOL * 2];   /* hash of qid -> entry + 1 (0: empty) */
static uint64_t g_salt;
static int64_t g_key = INT64_MIN, g_day_from;   /* what today's picks were made for */
static int g_key_login = -1;
static char g_db_path[MAX_PATH * 3];
static wchar_t g_cooked[MAX_PATH];  /* the game's CookedPCConsole folder */

static uint64_t mix(uint64_t x)   /* splitmix64 */
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint32_t qid_hash(const wchar_t *s, int len)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < len && s[i]; i++)
        h = (h ^ (uint32_t)(s[i] | 0x20)) * 16777619u;   /* qids compare without case */
    return h;
}

static struct entry *find_entry(const wchar_t *s, int len)
{
    uint32_t h;
    int i, n = (int)(sizeof g_index / sizeof g_index[0]);

    while (len > 0 && s[len - 1] == 0)
        len--;
    if (len <= 0 || len >= QID_MAX)
        return NULL;
    h = qid_hash(s, len) % (uint32_t)n;
    for (i = 0; i < n; i++) {
        int k = g_index[(h + (uint32_t)i) % (uint32_t)n];
        if (!k)
            return NULL;
        if ((int)wcslen(g_pool[k - 1].qid) == len && _wcsnicmp(g_pool[k - 1].qid, s, (size_t)len) == 0)
            return &g_pool[k - 1];
    }
    return NULL;
}

static void index_entry(int e)
{
    int n = (int)(sizeof g_index / sizeof g_index[0]);
    uint32_t h = qid_hash(g_pool[e].qid, QID_MAX) % (uint32_t)n;
    while (g_index[h])
        h = (h + 1) % (uint32_t)n;
    g_index[h] = (int16_t)(e + 1);
}

/* The game's folders, from where the exe is (Binaries\Win64). */
static int game_paths(void)
{
    wchar_t exe[MAX_PATH], path[MAX_PATH], *slash;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);

    if (n == 0 || n >= MAX_PATH)
        return 0;
    slash = wcsrchr(exe, L'\\');
    if (!slash || (size_t)(slash - exe) + 40 >= MAX_PATH)
        return 0;
    wcscpy(slash + 1, L"..\\..\\BrgGame\\Content\\masters.db");
    if (!GetFullPathNameW(exe, MAX_PATH, path, NULL))
        return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, g_db_path, sizeof g_db_path, NULL, NULL) <= 0)
        return 0;
    wcscpy(slash + 1, L"..\\..\\BrgGame\\CookedPCConsole");
    return GetFullPathNameW(exe, MAX_PATH, g_cooked, NULL) > 0;
}

/* A decal's icon package is in the game: UI_<decal without _P>_SF.upk. */
static int icon_present(const char *decal)
{
    wchar_t path[MAX_PATH * 2];
    char base[64];
    size_t n = strlen(decal);

    if (n == 0 || n >= sizeof base)
        return 0;
    memcpy(base, decal, n + 1);
    if (n > 2 && strcmp(base + n - 2, "_P") == 0)
        base[n - 2] = 0;
    if (_snwprintf(path, MAX_PATH * 2, L"%ls\\UI_%hs_SF.upk", g_cooked, base) < 0)
        return 0;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static uint64_t fresh_seed(void)
{
    LARGE_INTEGER qpc;
    FILETIME ft;

    QueryPerformanceCounter(&qpc);
    GetSystemTimeAsFileTime(&ft);
    return mix((uint64_t)qpc.QuadPart ^ ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) ^
               GetTickCount64() ^ (uint64_t)GetCurrentProcessId() << 40);
}

/* This install's random number, made once and kept in lid-dailyquests.ini. */
static void read_salt(void)
{
    wchar_t text[32] = {0};

    GetPrivateProfileStringW(L"daily", L"seed", L"", text, 32, g_ini_path);
    if (wcslen(text) == 16 && swscanf(text, L"%llx", (unsigned long long *)&g_salt) == 1 && g_salt)
        return;
    g_salt = fresh_seed() | 1;
    _snwprintf(text, 32, L"%016llx", (unsigned long long)g_salt);
    if (!WritePrivateProfileStringW(L"daily", L"seed", text, g_ini_path))
        log_line("could not keep the seed in lid-dailyquests.ini - the picks may change if the game restarts");
}

static void load_pool(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int held = 0;

    if (g_loaded)
        return;
    g_loaded = 1;
    read_salt();
    if (!g_db_path[0] && !game_paths()) {
        log_line("could not work out where masters.db is - no daily quests");
        return;
    }
    if (sqlite3_open_v2(g_db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        log_line("could not open %s (%s) - no daily quests", g_db_path, db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 2000);
    if (sqlite3_prepare_v2(db,
            "SELECT q.qid, IFNULL(r.val0, ''), IFNULL(s.platform, 0), r.rwdid IS NOT NULL FROM master_quest q "
            "LEFT JOIN master_reward r ON r.rwdid = q.first_rwd AND r.type = 'SKILL' "
            "LEFT JOIN master_skill s ON s.id = r.val0 "
            "WHERE q.start_date = ?1 AND q.end_date = ?2 ORDER BY q.qid", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, PARK_START);
        sqlite3_bind_int(st, 2, PARK_END);
        while (sqlite3_step(st) == SQLITE_ROW && g_npool < MAX_POOL) {
            struct entry *e = &g_pool[g_npool];
            const char *qid = (const char *)sqlite3_column_text(st, 0);
            const char *decal = (const char *)sqlite3_column_text(st, 1);
            if (!qid || !qid[0] || strlen(qid) >= QID_MAX)
                continue;
            memset(e, 0, sizeof *e);
            MultiByteToWideChar(CP_UTF8, 0, qid, -1, e->qid, QID_MAX);
            if (decal && strlen(decal) < sizeof e->decal)
                strcpy(e->decal, decal);
            e->has_decal = decal && decal[0];
            e->usable = !e->has_decal || (sqlite3_column_int(st, 2) == 0 && icon_present(decal));
            held += !e->usable;
            g_nusable += e->usable;
            index_entry(g_npool);
            g_npool++;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT id, value FROM master_const_int "
                               "WHERE id IN ('LID_DAILY_QUEST_COUNT', 'LID_DAILY_DECAL_COUNT')",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(st, 0);
            if (id && strcmp(id, "LID_DAILY_QUEST_COUNT") == 0)
                g_count = sqlite3_column_int(st, 1);
            else if (id)
                g_decals = sqlite3_column_int(st, 1);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (g_count < 1)
        g_count = 1;
    if (g_count > 30)
        g_count = 30;
    if (g_decals > g_count)
        g_decals = g_count;
    if (!g_npool)
        log_line("no quests are parked in masters.db - is the Daily Quests mod switched on?");
    else if (g_decals < 0)
        log_line("pool: %d quests, %d can come up (%d held back: their decal needs the Crossover pack), "
                 "%d a day", g_npool, g_nusable, held, g_count);
    else
        log_line("pool: %d quests, %d can come up (%d held back: their decal needs the Crossover pack), "
                 "%d a day, %d of them with a decal", g_npool, g_nusable, held, g_count, g_decals);
}

static int64_t now_utc(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

/* What the picks follow: the save's login count, which the game raises once
 * for each day it counts, or the game day if the count was not found (or no
 * save is loaded yet). */
static int64_t pick_key(int64_t now, int *from_login)
{
    if (g_pc.login_count && readable(g_pc.login_count, 4) && *g_pc.login_count > 0) {
        *from_login = 1;
        return *g_pc.login_count;
    }
    *from_login = 0;
    return (now - DAY_START) / DAY;
}

static void shuffle(int *order, int n, uint64_t *seed)
{
    int i;
    for (i = n - 1; i > 0; i--) {           /* Fisher-Yates */
        int j, t;
        *seed = mix(*seed);
        j = (int)(*seed % (uint64_t)(i + 1));
        t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}

/* Today's picks: the usable quests shuffled by this install's seed and the
 * login count (or game day). With LID_DAILY_DECAL_COUNT set, that many come
 * from the quests with a decal and the rest from the others; if one side
 * runs short, the other makes up the count. */
static void update_picks(int64_t now)
{
    static int decal[MAX_POOL], other[MAX_POOL];
    int from_login, i, nd = 0, no = 0, take_d, take_o, shown = 0;
    int64_t key = pick_key(now, &from_login);
    uint64_t seed;
    char list[600];
    size_t at = 0;

    load_pool();
    g_day_from = ((now - DAY_START) / DAY) * DAY + DAY_START;
    if (key == g_key && from_login == g_key_login)
        return;
    g_key = key;
    g_key_login = from_login;
    for (i = 0; i < g_npool; i++) {
        g_pool[i].picked = 0;
        if (!g_pool[i].usable)
            continue;
        if (g_decals >= 0 && g_pool[i].has_decal)
            decal[nd++] = i;
        else
            other[no++] = i;
    }
    seed = mix(g_salt ^ ((uint64_t)key * 0xD1B54A32D192ED03ULL) ^ (from_login ? 0x4C4F47494EULL : 0));
    shuffle(decal, nd, &seed);
    shuffle(other, no, &seed);
    take_d = g_decals < 0 ? 0 : g_decals < nd ? g_decals : nd;
    take_o = g_count - take_d < no ? g_count - take_d : no;
    if (take_d + take_o < g_count)
        take_d = g_count - take_o < nd ? g_count - take_o : nd;
    list[0] = 0;
    for (i = 0; i < take_d + take_o; i++) {
        int e = i < take_d ? decal[i] : other[i - take_d];
        g_pool[e].picked = 1;
        shown++;
        if (at + QID_MAX + 2 < sizeof list)
            at += (size_t)snprintf(list + at, sizeof list - at, "%s%ls", i ? " " : "", g_pool[e].qid);
    }
    if (!g_npool)
        return;
    if (from_login)
        log_line("login day %lld (the save's login count): %d quests open, %d with a decal - %s",
                 (long long)key, shown, take_d, list);
    else if (g_decals >= 0)
        log_line("game day %lld: %d quests open, %d with a decal - %s", (long long)key, shown, take_d, list);
    else
        log_line("game day %lld: %d quests open - %s", (long long)key, shown, list);
}

/* ---------------------------------------------------- the server's side */

struct fstring {
    wchar_t *data;
    int32_t count, max;
};

typedef uint64_t (*open_fn)(unsigned char *quest, int floor, void *user);
static open_fn g_open;

/* Takes the server's one call to its quest-open check: a pool quest gets
 * today's dates (or its parking dates) in the server's copy first. */
static uint64_t quest_open(unsigned char *quest, int floor, void *user)
{
    struct fstring *qid = (struct fstring *)(quest + 8);
    struct entry *e;

    if (readable(qid, sizeof *qid) && qid->data && qid->count > 1 && qid->count < QID_MAX &&
        readable(qid->data, (size_t)qid->count * 2)) {
        update_picks(now_utc());
        e = find_entry(qid->data, qid->count);
        if (e) {
            int32_t from = e->picked ? (int32_t)g_day_from : PARK_START;
            int32_t to = e->picked ? (int32_t)(g_day_from + DAY) : PARK_END;
            memcpy(quest + g_pc.off_start, &from, 4);
            memcpy(quest + g_pc.off_end, &to, 4);
        }
    }
    return g_open(quest, floor, user);
}

/* ---------------------------------------------------- the client's side */

/* The client's copy of a quest (FBrgDbMasterQuest, 0x180 bytes in 1.92): the
 * qid first, the dates as two 64-bit numbers. Where the dates are is learnt
 * from the first parked quest seen: the place holding 1 and 2. */
#define QUEST_SIZE  0x180
#define DATES_FROM  0xF0
#define DATES_TO    0x150

struct tarray {
    unsigned char *data;
    int32_t count, max;
};

static int g_off_dates = -1;

static void client_quest(unsigned char *q)
{
    struct fstring *qid = (struct fstring *)q;
    struct entry *e;
    int64_t park[2] = {PARK_START, PARK_END}, day[2];
    int o;

    if (!readable(q, QUEST_SIZE) || !qid->data || qid->count <= 1 || qid->count >= QID_MAX ||
        !readable(qid->data, (size_t)qid->count * 2))
        return;
    update_picks(now_utc());
    e = find_entry(qid->data, qid->count);
    if (!e)
        return;
    if (g_off_dates < 0) {
        for (o = DATES_FROM; o <= DATES_TO; o += 4)
            if (memcmp(q + o, park, sizeof park) == 0) {
                g_off_dates = o;
                log_line("the client keeps a quest's dates at +0x%X", o);
                break;
            }
        if (g_off_dates < 0)
            return;
    }
    if (!e->picked || memcmp(q + g_off_dates, park, sizeof park) != 0)
        return;
    day[0] = g_day_from;
    day[1] = g_day_from + DAY;
    memcpy(q + g_off_dates, day, sizeof day);
}

typedef uint64_t (*list_fn)(void *db, struct tarray *out);
typedef uint64_t (*one_fn)(void *db, void *key, unsigned char *out);
static list_fn g_list;
static one_fn g_by_index, g_get;

static uint64_t get_quest_list(void *db, struct tarray *out)
{
    uint64_t r = g_list(db, out);
    int i;

    if (readable(out, sizeof *out) && out->data && out->count > 0 && out->count < 100000)
        for (i = 0; i < out->count; i++)
            client_quest(out->data + (size_t)i * QUEST_SIZE);
    return r;
}

static uint64_t get_quest_by_index(void *db, void *index, unsigned char *out)
{
    uint64_t r = g_by_index(db, index, out);
    client_quest(out);
    return r;
}

static uint64_t get_quest(void *db, void *qid, unsigned char *out)
{
    uint64_t r = g_get(db, qid, out);
    client_quest(out);
    return r;
}

/* --------------------------------------------------------- the changes */

typedef void *(*find_object_fn)(void *cls, void *outer, const wchar_t *name, int exact);
typedef HWND (WINAPI *foreground_fn)(void);
static foreground_fn g_real_foreground;
static int g_client_done, g_frames;

static int hook_slot(void **table, int slot, void *mine, void **original)
{
    void **at = (void **)((unsigned char *)table + slot);
    if (!readable(at, sizeof *at) || !*at)
        return 0;
    *original = *at;
    return write_pointer(at, mine);
}

/* The database's function table, from its class's default object, once it
 * exists. Runs on the game's thread. */
static void hook_client(void)
{
    void *db, **table;

    db = ((find_object_fn)g_pc.find_object)(NULL, (void *)(LONG_PTR)-1, L"BrgGame.Default__BrgDatabase", 0);
    if (!db)
        return;
    g_client_done = 1;
    table = *(void ***)db;
    if (!readable(table, (size_t)g_pc.vt_get + 8)) {
        log_line("client: the database's function table could not be read - the quest board only "
                 "lists what the server allows");
        return;
    }
    if (hook_slot(table, g_pc.vt_list, (void *)get_quest_list, (void **)&g_list) &&
        hook_slot(table, g_pc.vt_by_index, (void *)get_quest_by_index, (void **)&g_by_index) &&
        hook_slot(table, g_pc.vt_get, (void *)get_quest, (void **)&g_get))
        log_line("client: ready - the quest board sees the day's picks");
    else
        log_line("client: could not change the database's function table");
}

/* Stands in for GetForegroundWindow. The game's main loop calls it once a
 * frame; that call - and only that one - does the client's hooks, once. */
static HWND WINAPI foreground_hook(void)
{
    HWND h = g_real_foreground();
    void **sp = __builtin_frame_address(0);
    void *want = g_pc.frame_call + 6;
    int i, ours = __builtin_return_address(0) == want;

    if (!ours && readable(sp, 64 * 8))
        for (i = 0; i < 64 && !ours; i++)       /* called through another plugin's hook */
            ours = sp[i] == want;
    if (ours && !g_client_done && ++g_frames % 30 == 0)
        hook_client();
    return h;
}

/* One page within 2 GB of the game code, for the stub. */
static unsigned char *page_near(unsigned char *code)
{
    const ULONG_PTR step = 0x10000;
    ULONG_PTR origin = (ULONG_PTR)code & ~(step - 1);
    ULONG_PTR d;

    for (d = step; d < 0x70000000; d += step) {
        void *p = VirtualAlloc((void *)(origin - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!p)
            p = VirtualAlloc((void *)(origin + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p)
            return p;
    }
    return NULL;
}

static int hook_server(void)
{
    unsigned char stub[12], call[5] = {0xE8}, *page;
    void *target = (void *)quest_open;
    intptr_t d;
    int32_t r;

    page = page_near(g_pc.open_site);
    if (!page)
        return 0;
    d = page - (g_pc.open_site + 5);
    if (d != (int32_t)d) {
        VirtualFree(page, 0, MEM_RELEASE);
        return 0;
    }
    stub[0] = 0x48; stub[1] = 0xB8; memcpy(stub + 2, &target, 8);   /* mov rax,quest_open */
    stub[10] = 0xFF; stub[11] = 0xE0;                               /* jmp rax */
    memcpy(page, stub, sizeof stub);
    FlushInstructionCache(GetCurrentProcess(), page, sizeof stub);
    g_open = (open_fn)g_pc.open_fn;
    r = (int32_t)d;
    memcpy(call + 1, &r, 4);
    if (!write_code(g_pc.open_site, call, sizeof call)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

static void patch_game(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);
    DWORD old;

    find_pieces(base, &g_pc);
    if (!server_ok(&g_pc)) {
        log_line("the server's quest check was not found exactly once (check %d, calls %d) - nothing "
                 "changed. A game update may have moved it.", g_pc.open_fn != NULL, g_pc.open_calls);
        return;
    }
    if (!client_ok(&g_pc)) {
        log_line("the quest board's code was not found exactly once (list %d, index %d, get %d, find %d, "
                 "frame %d) - nothing changed. A game update may have moved it.", g_pc.vt_list != 0,
                 g_pc.vt_by_index != 0, g_pc.vt_get != 0, g_pc.find_object != NULL, g_pc.frame_import != NULL);
        return;
    }
    if (!hook_server()) {
        log_line("could not change the server's quest check - nothing changed");
        return;
    }
    log_line("server: ready - its quest check (game+0x%llX) opens the day's picks",
             (unsigned long long)(g_pc.open_fn - base));
    if (g_pc.login_count)
        log_line("the picks follow the save's login count (game+0x%llX): every day the game counts, "
                 "a real one or a \"Start a new game day\", brings new ones",
                 (unsigned long long)((unsigned char *)g_pc.login_count - base));
    else
        log_line("the save's login count was not found - the picks follow the game day (10:00 UTC)");
    if (!readable(g_pc.frame_import, 8) || !*g_pc.frame_import ||
        !VirtualProtect(g_pc.frame_import, 8, PAGE_READWRITE, &old)) {
        log_line("client: could not hook the frame loop - the quest board only lists what the server allows");
        return;
    }
    g_real_foreground = (foreground_fn)*g_pc.frame_import;
    *g_pc.frame_import = (void *)foreground_hook;
    VirtualProtect(g_pc.frame_import, 8, old, &old);
}

/* ---------------------------------------------------------------- start */

static int running_in_the_game(void)
{
    wchar_t exe[MAX_PATH];
    const wchar_t *name;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);

    if (n == 0 || n >= MAX_PATH)
        return 0;
    name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    return _wcsicmp(name, L"BrgGame-Steam.exe") == 0;
}

static void own_paths(HINSTANCE inst)
{
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(inst, g_log_path, MAX_PATH);

    slash = (n && n < MAX_PATH - 24) ? wcsrchr(g_log_path, L'\\') : NULL;
    if (!slash) {
        g_log_path[0] = g_ini_path[0] = 0;
        return;
    }
    wcscpy(slash + 1, L"lid-dailyquests.ini");
    wcscpy(g_ini_path, g_log_path);
    wcscpy(slash + 1, L"lid-dailyquests.log");
}

#ifndef LID_DAILYQUESTS_NO_DLLMAIN
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        own_paths(inst);
        if (!running_in_the_game())
            return TRUE;    /* some other program - change nothing */
        log_line("LiD Daily Quests lid-dailyquests.dll " LID_DLL_VERSION " loaded");
        patch_game();
    }
    return TRUE;
}
#endif
