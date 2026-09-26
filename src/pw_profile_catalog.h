/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PROFILE_CATALOG_H
#define PW_PROFILE_CATALOG_H
#include "pw_app_profile.h"

/* A profiles.lst index names the .profile files in one directory, one bare
 * name per line; '#' and ';' start comments and blank lines are ignored.
 * The index replaces a directory scan, which the application image does
 * not support on this firmware. */
enum { PW_PROFILE_CATALOG_MAX=16,PW_PROFILE_CATALOG_NAME=64 };
typedef struct PwProfileCatalog {
    uint32_t count;
    char names[PW_PROFILE_CATALOG_MAX][PW_PROFILE_CATALOG_NAME];
} PwProfileCatalog;

/* Accepts lower-case [a-z0-9_-] names with a single ".profile" suffix;
 * rejects paths, duplicates and more than PW_PROFILE_CATALOG_MAX entries. */
int pw_profile_catalog_parse(const uint8_t *text,size_t bytes,PwProfileCatalog *catalog);
/* The launcher can start a profile only on the direct PE32/GDI runtime;
 * every other runtime is listed as not available yet. */
int pw_profile_catalog_launchable(const PwAppProfile *profile);

#endif
