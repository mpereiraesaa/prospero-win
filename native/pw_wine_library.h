/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_LIBRARY_H
#define PW_WINE_LIBRARY_H
/*
 * The games the Wine title offers, read from the console's data partition:
 *
 *   <root>/profiles/<name>.profile   one game each (src/pw_game_profile.h)
 *   <root>/profiles/profiles.lst     optional index, when the directory
 *                                    cannot be listed (src/pw_profile_catalog.h)
 *   <root>/input/<preset>.input      input presets shared between profiles
 *
 * <root> is /data/prospero-win on the console. Nothing is built in: a game
 * appears by adding its profile. An index, when present, says exactly what
 * is offered; otherwise the directory is listed with readdir, which works on
 * /data once it is granted, then with getdents. Files are read with
 * open/read.
 */
#include <stddef.h>
#include <stdint.h>
#include "../src/pw_game_profile.h"

enum {
    PW_WINE_LIBRARY_MAX = 32, PW_WINE_LIBRARY_NAME = 64, PW_WINE_LIBRARY_PATH = 256,
    PW_WINE_LIBRARY_SCANNED = 1, PW_WINE_LIBRARY_INDEXED = 2,
};

typedef struct PwWineLibraryEntry {
    char file[PW_WINE_LIBRARY_NAME];    /* <name>.profile */
    int status;                         /* PW_OK, or why the profile was refused */
    PwGameProfile profile;              /* valid when status is PW_OK */
} PwWineLibraryEntry;

typedef struct PwWineLibrary {
    uint32_t count;                     /* entries, valid or refused, sorted by file */
    int listed_by;                      /* SCANNED, INDEXED, or 0: no profiles directory */
    int scan_error;                     /* errno of the first failed listing, or 0 */
    PwWineLibraryEntry entries[PW_WINE_LIBRARY_MAX];
} PwWineLibrary;

/* Read every profile under root. PW_OK even when some profiles are refused
 * (their entries say why); PW_ERR_NOT_FOUND when neither the directory nor
 * its index can be read. */
int pw_wine_library_load(PwWineLibrary *library, const char *root);
/* The valid profile whose [application] id is id, or NULL. */
const PwGameProfile *pw_wine_library_find(const PwWineLibrary *library, const char *id);
/* A profile's input: its preset from <root>/input, then its own lines. A
 * missing or refused preset leaves only the profile's lines; the status
 * says why. */
int pw_wine_library_input(const PwGameProfile *profile, const char *root, PwGameInput *input);

/* The names in a buffer of FreeBSD 11 directory records (struct dirent:
 * u32 fileno, u16 reclen, u8 type, u8 namlen, name): calls found(name,
 * length, context) for each regular file or unknown-type entry. Returns
 * the number of records, or -1 for a malformed buffer. */
int pw_wine_library_dirents(const uint8_t *buffer, size_t length,
                            void (*found)(const char *name, size_t length, void *context),
                            void *context);
#endif
