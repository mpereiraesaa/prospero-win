/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_LAUNCH_H
#define PW_WINE_LAUNCH_H
/*
 * What the wine64 title runs, from its arguments.
 *
 * The title runs one game per process: the launcher restarts the title with
 * sceSystemServiceLoadExec and the chosen game's arguments, and closing the
 * game restarts it into the launcher. Arguments are "key=value" words:
 *   profile=<id>   a game from the title's catalog;
 *   path=<exe>     an absolute Windows path, run directly (overrides the
 *                  profile's executable);
 *   sync=1         copy the library from /data into /download0, then
 *                  restart into the launcher;
 *   cycle=<n>      open/close cycles completed, for unattended validation.
 * With neither profile, path nor sync the title shows the launcher, as when
 * the system starts it (argc 1, empty argv[0]). The launcher stays in the
 * sandbox and reads the copy; a game or sync is granted /data.
 */
#include <stddef.h>
#include <stdint.h>

enum { PW_WINE_LAUNCH_PATH_MAX = 260, PW_WINE_LAUNCH_ARGS = 4, PW_WINE_LAUNCH_WORDS = 32 };

typedef struct PwWineApp {
    const char *id;           /* profile=<id> */
    const char *name;         /* shown in the launcher */
    const char *detail;       /* second line in the launcher */
    const char *executable;   /* absolute Windows path */
} PwWineApp;

typedef enum PwWineLaunchMode {
    PW_WINE_LAUNCH_LAUNCHER = 1, PW_WINE_LAUNCH_GAME, PW_WINE_LAUNCH_SYNC
} PwWineLaunchMode;

typedef struct PwWineLaunch {
    PwWineLaunchMode mode;
    const PwWineApp *app;                         /* NULL for a bare path= */
    char executable[PW_WINE_LAUNCH_PATH_MAX];     /* GAME: what Wine runs */
    uint32_t cycle;
    unsigned refused;   /* an unknown profile or an invalid path: shown in the launcher */
} PwWineLaunch;

/* Decide from argv. Always fills out; a refused game falls back to the
 * launcher. A game wins over sync. 0, or -1 when out or the catalog is
 * invalid. */
int pw_wine_launch_parse(int argc, char *const *argv, const PwWineApp *apps, size_t count,
                         PwWineLaunch *out);

/* The argv for LoadExec: app's profile, path and cycle, or, with app NULL,
 * the launcher with the cycle. Words are built in storage; argv is
 * NULL-terminated. Returns the number of words, or 0 when storage or argv
 * is too small. */
size_t pw_wine_launch_argv(const PwWineApp *app, uint32_t cycle, char *storage, size_t size,
                           char **argv, size_t max);
/* The same for sync=1 with the cycle. */
size_t pw_wine_launch_sync_argv(uint32_t cycle, char *storage, size_t size, char **argv, size_t max);
/* 1 when argv asks for a game or sync, which need /data before the catalog
 * can be read; 0 for the launcher. */
int pw_wine_launch_needs_data(int argc, char *const *argv);
/* A profile's arguments line as words for Wine's argv, which Wine quotes
 * back into the Windows command line: words split at spaces and tabs, and
 * double quotes group a word ("a b" is one word; the quotes are dropped).
 * Words are copied into storage. Returns the number of words (0 for an
 * empty line), or -1 when text is NULL, a quote is unclosed, or the words
 * do not fit storage or max. */
int pw_wine_launch_split(const char *text, char *storage, size_t size, const char **words,
                         size_t max);
#endif
