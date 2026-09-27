/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_wine_launch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const PwWineApp apps[] = {
    { "pinball", "Space Cadet Pinball", "Windows XP", "C:\\Games\\Pinball\\PINBALL.EXE" },
    { "notepad", "Notepad", "Wine", "C:\\windows\\notepad.exe" },
};
enum { COUNT = sizeof(apps) / sizeof(apps[0]) };

static PwWineLaunch parse(int argc, char *const *argv)
{
    PwWineLaunch launch;
    assert(!pw_wine_launch_parse(argc, argv, apps, COUNT, &launch));
    return launch;
}

static void test_parse(void)
{
    char *system[] = { "" };
    char *profile[] = { "profile=pinball", "cycle=2" };
    char *path[] = { "path=D:\\Other\\GAME.EXE" };
    char *both[] = { "path=C:\\Games\\Pinball\\DEBUG.EXE", "profile=pinball" };
    char *unknown[] = { "profile=missing" };
    char *relative[] = { "path=Games\\PINBALL.EXE" };
    char *short_path[] = { "path=C:" };
    char *lower[] = { "path=c:\\x.exe", "noise", "cycle=12x" };
    char *launcher[] = { "launcher=1", "cycle=4294967295" };
    char *overflow[] = { "cycle=4294967296" };
    char *nulls[] = { NULL, "profile=notepad" };
    char long_path[PW_WINE_LAUNCH_PATH_MAX + 8] = "path=C:\\";
    char *too_long[] = { long_path };
    PwWineLaunch l;

    /* The system starts the title with one empty argument: the launcher. */
    l = parse(1, system);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && !l.app && !l.refused && !l.cycle);
    l = parse(0, NULL);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER);

    l = parse(2, profile);
    assert(l.mode == PW_WINE_LAUNCH_GAME && l.app == &apps[0] && l.cycle == 2);
    assert(!strcmp(l.executable, "C:\\Games\\Pinball\\PINBALL.EXE"));
    l = parse(1, path);
    assert(l.mode == PW_WINE_LAUNCH_GAME && !l.app && !strcmp(l.executable, "D:\\Other\\GAME.EXE"));
    l = parse(2, both);
    assert(l.app == &apps[0] && !strcmp(l.executable, "C:\\Games\\Pinball\\DEBUG.EXE"));
    l = parse(2, nulls);
    assert(l.app == &apps[1]);
    l = parse(3, lower);
    assert(l.mode == PW_WINE_LAUNCH_GAME && !l.cycle);
    l = parse(2, launcher);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.cycle == 4294967295u);
    l = parse(1, overflow);
    assert(!l.cycle);

    /* Refusals fall back to the launcher and say so. */
    l = parse(1, unknown);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused && !l.app);
    l = parse(1, relative);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused);
    l = parse(1, short_path);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused);
    memset(long_path + 8, 'a', PW_WINE_LAUNCH_PATH_MAX);
    long_path[sizeof(long_path) - 1] = 0;
    l = parse(1, too_long);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused);

    assert(pw_wine_launch_parse(1, system, apps, COUNT, NULL) == -1);
    assert(pw_wine_launch_parse(1, system, NULL, 1, &l) == -1);
    assert(!pw_wine_launch_parse(1, profile, NULL, 0, &l) && l.refused);
}

static void test_argv(void)
{
    char storage[256], tiny[12], *argv[PW_WINE_LAUNCH_ARGS];
    PwWineApp broken = { NULL, "x", "x", "C:\\x.exe" };
    PwWineLaunch back;

    assert(pw_wine_launch_argv(&apps[0], 0, storage, sizeof(storage), argv, 4) == 3);
    assert(!strcmp(argv[0], "profile=pinball") && !strcmp(argv[1], "path=C:\\Games\\Pinball\\PINBALL.EXE"));
    assert(!strcmp(argv[2], "cycle=0") && !argv[3]);
    /* What the title builds, it parses back. */
    assert(!pw_wine_launch_parse(3, argv, apps, COUNT, &back));
    assert(back.mode == PW_WINE_LAUNCH_GAME && back.app == &apps[0] && !back.cycle);

    assert(pw_wine_launch_argv(NULL, 4294967295u, storage, sizeof(storage), argv, 4) == 2);
    assert(!strcmp(argv[0], "launcher=1") && !strcmp(argv[1], "cycle=4294967295") && !argv[2]);
    assert(!pw_wine_launch_parse(2, argv, apps, COUNT, &back));
    assert(back.mode == PW_WINE_LAUNCH_LAUNCHER && back.cycle == 4294967295u && !back.refused);
    assert(pw_wine_launch_argv(NULL, 17, storage, sizeof(storage), argv, 4) == 2 &&
           !strcmp(argv[1], "cycle=17"));

    assert(!pw_wine_launch_argv(&apps[0], 0, tiny, sizeof(tiny), argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, storage, 30, argv, 4));
    assert(!pw_wine_launch_argv(NULL, 0, tiny, 11, argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, storage, sizeof(storage), argv, 3));
    assert(!pw_wine_launch_argv(&broken, 0, storage, sizeof(storage), argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, NULL, sizeof(storage), argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, storage, sizeof(storage), NULL, 4));
}

static void test_sync(void)
{
    char storage[64], tiny[8], *argv[PW_WINE_LAUNCH_ARGS];
    char *sync[] = { "sync=1", "cycle=3" };
    char *not_sync[] = { "sync=0" };
    char *game_wins[] = { "sync=1", "profile=notepad" };
    char *refused[] = { "sync=1", "profile=missing" };
    char *system[] = { "" }, *launcher[] = { "launcher=1", "cycle=2" };
    char *path[] = { "path=C:\\x.exe" }, *profile[] = { NULL, "profile=missing" };
    PwWineLaunch l;

    l = parse(2, sync);
    assert(l.mode == PW_WINE_LAUNCH_SYNC && l.cycle == 3 && !l.app && !l.refused);
    l = parse(1, not_sync);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER);
    l = parse(2, game_wins);
    assert(l.mode == PW_WINE_LAUNCH_GAME && l.app == &apps[1]);
    /* A refused game shows the launcher, which says so, rather than syncing. */
    l = parse(2, refused);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused);

    assert(pw_wine_launch_sync_argv(9, storage, sizeof(storage), argv, 4) == 2);
    assert(!strcmp(argv[0], "sync=1") && !strcmp(argv[1], "cycle=9") && !argv[2]);
    l = parse(2, argv);
    assert(l.mode == PW_WINE_LAUNCH_SYNC && l.cycle == 9);
    assert(!pw_wine_launch_sync_argv(0, tiny, sizeof(tiny), argv, 4));
    assert(!pw_wine_launch_sync_argv(0, storage, sizeof(storage), argv, 3));

    /* Only a game or sync needs /data before the catalog is read. */
    assert(pw_wine_launch_needs_data(2, sync) && pw_wine_launch_needs_data(2, game_wins));
    assert(pw_wine_launch_needs_data(1, path) && pw_wine_launch_needs_data(2, profile));
    assert(!pw_wine_launch_needs_data(1, not_sync) && !pw_wine_launch_needs_data(1, system));
    assert(!pw_wine_launch_needs_data(2, launcher) && !pw_wine_launch_needs_data(0, NULL));
}

static void test_split(void)
{
    char storage[64], small[6];
    const char *words[PW_WINE_LAUNCH_WORDS];

    /* 7-Zip's benchmark line: spaces and tabs separate, runs collapse. */
    assert(pw_wine_launch_split("  b -mmt1\t-md22 ", storage, sizeof(storage), words, 8) == 3);
    assert(!strcmp(words[0], "b") && !strcmp(words[1], "-mmt1") && !strcmp(words[2], "-md22"));
    /* Quotes group a word and are dropped, also inside a word. */
    assert(pw_wine_launch_split("x \"C:\\My Games\\a.dat\" -o\"out dir\" \"\"", storage,
                                sizeof(storage), words, 8) == 4);
    assert(!strcmp(words[1], "C:\\My Games\\a.dat") && !strcmp(words[2], "-oout dir") &&
           !strcmp(words[3], ""));
    assert(pw_wine_launch_split("", storage, sizeof(storage), words, 8) == 0);
    assert(pw_wine_launch_split(" \t ", storage, sizeof(storage), words, 0) == 0);

    /* Refusals: an unclosed quote, too many words, storage exhausted. */
    assert(pw_wine_launch_split("a \"b", storage, sizeof(storage), words, 8) == -1);
    assert(pw_wine_launch_split("a b c", storage, sizeof(storage), words, 2) == -1);
    assert(pw_wine_launch_split("abcdef", small, sizeof(small), words, 8) == -1);
    assert(pw_wine_launch_split("ab cde", small, sizeof(small), words, 8) == -1);
    assert(pw_wine_launch_split("ab c", small, sizeof(small), words, 8) == 2 &&
           !strcmp(words[1], "c"));
    assert(pw_wine_launch_split(NULL, storage, sizeof(storage), words, 8) == -1);
    assert(pw_wine_launch_split("a", NULL, sizeof(storage), words, 8) == -1);
    assert(pw_wine_launch_split("a", storage, sizeof(storage), NULL, 8) == -1);
}

int main(void)
{
    test_parse();
    test_argv();
    test_sync();
    test_split();
    printf("wine launch passed: launcher on no game, profile and path, overrides, refusals, "
           "cycle counts, LoadExec argv round trip, bounded storage, sync mode, /data need, "
           "argument words\n");
    return 0;
}
