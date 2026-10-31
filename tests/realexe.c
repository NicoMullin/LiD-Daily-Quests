/* The plugin's search, run on the real BrgGame-Steam.exe (mapped as a file,
 * nothing runs). Every piece must be found exactly once. Only run by
 * `build.py --exe PATH`: the game is not part of this repository. */
#define LID_DAILYQUESTS_NO_DLLMAIN
#include "../dll/lid_dailyquests.c"

#include <stdio.h>

static unsigned long long rva(void *p, unsigned char *base)
{
    return p ? (unsigned long long)((unsigned char *)p - base) : 0ULL;
}

int main(int argc, char **argv)
{
    struct pieces pc;
    unsigned char *base;
    HMODULE m;
    int fails = 0;

    if (argc < 2) { puts("usage: realexe PATH-TO-BrgGame-Steam.exe"); return 2; }
    m = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!m) { printf("could not map %s (error %lu)\n", argv[1], GetLastError()); return 2; }
    base = (unsigned char *)((ULONG_PTR)m & ~(ULONG_PTR)3);

    find_pieces(base, &pc);
    /* where they are in game build 5.0.5.0 (1.92) */
    printf("server quest check     %-7s game+0x%llX (1.92: game+0x12FDF70), dates at +0x%X/+0x%X "
           "(1.92: +0xD8/+0xE0)\n", pc.open_fn ? "found" : "MISSING", rva(pc.open_fn, base),
           pc.off_start, pc.off_end);
    printf("  its call             %-7s game+0x%llX (1.92: game+0x130F05B), %d found (needs 1)\n",
           pc.open_site ? "found" : "MISSING", rva(pc.open_site, base), pc.open_calls);
    printf("GetQuestList slot      0x%X (1.92: 0x798)\n", pc.vt_list);
    printf("GetQuestByIndex slot   0x%X (1.92: 0x7A0)\n", pc.vt_by_index);
    printf("GetQuest slot          0x%X (1.92: 0x7A8)\n", pc.vt_get);
    printf("StaticFindObject       %-7s game+0x%llX\n", pc.find_object ? "found" : "MISSING",
           rva(pc.find_object, base));
    printf("frame loop call        %-7s game+0x%llX\n", pc.frame_call ? "found" : "MISSING",
           rva(pc.frame_call, base));
    printf("save's login count     %-7s game+0x%llX\n", pc.login_count ? "found" : "MISSING",
           rva(pc.login_count, base));
    if (!server_ok(&pc))
        fails++;
    if (!client_ok(&pc))
        fails++;
    if (!pc.login_count)
        fails++;
    if (pc.vt_list && (pc.vt_by_index != pc.vt_list + 8 || pc.vt_get != pc.vt_list + 16)) {
        puts("FAIL: the three slots are not side by side, as GetQuestList/ByIndex/Get are");
        fails++;
    }
    printf(fails ? "\n%d FAILED\n" : "\nall found\n", fails);
    return fails != 0;
}
