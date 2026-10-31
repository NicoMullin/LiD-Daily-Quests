/* The plugin's own logic, without the game: the pool from a test masters.db,
 * the decals the Crossover pack would be needed for, the day's picks, and the
 * dates it writes into the server's and the client's copies of a quest.
 *
 * build.py makes the test masters.db next to this exe (BrgGame\Content two
 * folders up, as in the game) with 60 parked quests: 30 with an ordinary
 * decal, 5 with a console-only one (platform 1), 5 with a decal whose icon is
 * missing, 20 paying Death Metals. The icons sit in BrgGame\CookedPCConsole,
 * all but those 5. 7 a day, 2 of them with a decal. */
#define LID_DAILYQUESTS_NO_DLLMAIN
#include "../dll/lid_dailyquests.c"

#include <stdio.h>

static int fails;

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

static uint64_t fake_open(unsigned char *quest, int floor, void *user)
{
    (void)quest; (void)floor; (void)user;
    return 1;
}

static int picked_count(void)
{
    int i, n = 0;
    for (i = 0; i < g_npool; i++)
        n += g_pool[i].picked;
    return n;
}

static int picked_with_decal(void)
{
    int i, n = 0;
    for (i = 0; i < g_npool; i++)
        n += g_pool[i].picked && g_pool[i].has_decal;
    return n;
}

static int same_as(const int *first)
{
    int i, same = 1;
    for (i = 0; i < g_npool; i++)
        same &= first[i] == g_pool[i].picked;
    return same;
}

static int picks_usable(void)
{
    int i;
    for (i = 0; i < g_npool; i++)
        if (g_pool[i].picked && !g_pool[i].usable)
            return 0;
    return 1;
}

int main(void)
{
    static unsigned char server_quest[0x100], client[3 * QUEST_SIZE];
    int64_t day1 = (int64_t)20000 * DAY + DAY_START + 3600, park[2] = {PARK_START, PARK_END};
    int i, first[MAX_POOL], same = 1, differ = 0;
    struct fstring sq;
    struct entry *e = NULL, *other = NULL;
    wchar_t name[QID_MAX];
    int32_t from, to;
    int64_t dates[2];

    wcscpy(g_log_path, L"lid-dailyquests.log");
    GetFullPathNameW(L"lid-dailyquests.ini", MAX_PATH, g_ini_path, NULL);   /* the ini wants a full path */
    check(game_paths(), "the game's folders are worked out from the exe");
    load_pool();
    check(g_npool == 60, "all 60 parked quests are in the pool (the open one is not)");
    check(g_nusable == 50, "50 can come up: console-only decals and missing icons are held back");
    check(g_count == 7, "the count comes from LID_DAILY_QUEST_COUNT");
    check(g_decals == 2, "the decal count comes from LID_DAILY_DECAL_COUNT");

    update_picks(day1);
    check(picked_count() == 7, "7 picked today");
    check(picked_with_decal() == 2, "2 of them with a decal, 5 paying Death Metals");
    check(picks_usable(), "only quests whose decal can be shown are picked");
    for (i = 0; i < g_npool; i++)
        first[i] = g_pool[i].picked;
    update_picks(day1 + 5 * 3600);
    for (i = 0; i < g_npool; i++)
        same &= first[i] == g_pool[i].picked;
    check(same, "the same picks later the same game day");
    update_picks(day1 + DAY);
    for (i = 0; i < g_npool; i++)
        differ |= first[i] != g_pool[i].picked;
    check(differ && picked_count() == 7, "new picks the next game day");
    g_key = INT64_MIN;
    update_picks(day1);
    check(same_as(first), "the same picks again for the same day after a restart (the seed is kept)");

    /* the split, when one side runs short */
    g_decals = 7;
    g_key = INT64_MIN;
    update_picks(day1);
    check(picked_count() == 7 && picked_with_decal() == 7, "7 decal quests when all 7 are to have one");
    g_decals = 0;
    g_key = INT64_MIN;
    update_picks(day1);
    check(picked_count() == 7 && picked_with_decal() == 0, "none with a decal when the decal count is 0");
    g_count = 25;
    g_key = INT64_MIN;
    update_picks(day1);
    check(picked_count() == 25 && picked_with_decal() == 5,
          "with 25 a day and 20 Death Metal quests, decal quests make up the rest");
    g_count = 7;
    g_decals = 2;

    /* the save's login count: a new count is a new day, whatever the clock says */
    {
        static int32_t logins = 41;
        g_pc.login_count = &logins;
        g_key = INT64_MIN;
        update_picks(day1);
        for (i = 0; i < g_npool; i++)
            first[i] = g_pool[i].picked;
        check(g_key_login == 1 && g_key == 41 && picked_with_decal() == 2, "the picks follow the login count");
        update_picks(day1 + DAY);
        check(same_as(first), "the same login count keeps its picks, even on another game day");
        logins = 42;
        update_picks(day1);
        check(!same_as(first) && picked_count() == 7, "a new login count (a new game day) brings new picks");
        check(g_day_from == (day1 - DAY_START) / DAY * DAY + DAY_START, "the dates are still the real game day's");
        logins = 41;
        update_picks(day1);
        check(same_as(first), "the same login count gives the same picks again");
        logins = 0;
        update_picks(day1);
        check(g_key_login == 0, "no save loaded yet (a count of 0): the game day is used");
        g_pc.login_count = NULL;
    }

    /* the server's copy: qid at +8, dates at the offsets its code uses */
    g_pc.off_start = 0xD8;
    g_pc.off_end = 0xE0;
    g_open = fake_open;
    g_key = INT64_MIN;
    update_picks(now_utc());
    for (i = 0; i < g_npool; i++) {
        if (g_pool[i].picked && !e)
            e = &g_pool[i];
        if (!g_pool[i].picked && !other)
            other = &g_pool[i];
    }
    wcscpy(name, e->qid);
    sq.data = name;
    sq.count = sq.max = (int32_t)wcslen(name) + 1;
    memcpy(server_quest + 8, &sq, sizeof sq);
    from = PARK_START; to = PARK_END;
    memcpy(server_quest + 0xD8, &from, 4);
    memcpy(server_quest + 0xE0, &to, 4);
    quest_open(server_quest, 1, NULL);
    memcpy(&from, server_quest + 0xD8, 4);
    memcpy(&to, server_quest + 0xE0, 4);
    check(from == (int32_t)g_day_from && to == (int32_t)(g_day_from + DAY),
          "server: a pick gets today 10:00 UTC to tomorrow 10:00 UTC");
    wcscpy(name, other->qid);
    sq.count = sq.max = (int32_t)wcslen(name) + 1;
    memcpy(server_quest + 8, &sq, sizeof sq);
    quest_open(server_quest, 1, NULL);
    memcpy(&from, server_quest + 0xD8, 4);
    memcpy(&to, server_quest + 0xE0, 4);
    check(from == PARK_START && to == PARK_END, "server: a quest not picked keeps its parking dates");

    /* the client's copies: qid first, dates at 0x10C (learnt from the parking dates) */
    {
        static wchar_t names[3][QID_MAX];
        struct fstring fs;
        wcscpy(names[0], L"SOMETHING_ELSE");
        wcscpy(names[1], e->qid);
        wcscpy(names[2], other->qid);
        for (i = 0; i < 3; i++) {
            fs.data = names[i];
            fs.count = fs.max = (int32_t)wcslen(names[i]) + 1;
            memcpy(client + i * QUEST_SIZE, &fs, sizeof fs);
            memcpy(client + i * QUEST_SIZE + 0x10C, park, sizeof park);
        }
        {
            struct tarray arr = {client, 3, 3};
            g_list = (list_fn)NULL;
            for (i = 0; i < arr.count; i++)
                client_quest(arr.data + (size_t)i * QUEST_SIZE);
        }
        check(g_off_dates == 0x10C, "client: the dates' place is learnt from the parking dates");
        memcpy(dates, client + 0x10C, sizeof dates);
        check(memcmp(dates, park, sizeof park) == 0, "client: a quest not in the pool is left alone");
        memcpy(dates, client + QUEST_SIZE + 0x10C, sizeof dates);
        check(dates[0] == g_day_from && dates[1] == g_day_from + DAY, "client: a pick gets today");
        memcpy(dates, client + 2 * QUEST_SIZE + 0x10C, sizeof dates);
        check(memcmp(dates, park, sizeof park) == 0, "client: a quest not picked keeps its parking dates");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
