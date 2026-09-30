/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PREFIX_TEMP_H
#define PW_PREFIX_TEMP_H
/*
 * A game's temp folders. A prefix made on the PC names the PC's user in its
 * temp variables (HKCU\Environment TEMP and TMP, written by wineboot), and
 * one set up without wineboot may lack C:\windows\temp. A program that needs
 * a temp folder then fails: Half-Life's Chromium (CEF) stops on a failed
 * CreateUniqueTempDir. Before Wine starts, the title creates the folders the
 * prefix's registry names, and C:\windows\temp (native/pw_wine_prefix.c
 * reads user.reg and creates them; this part only parses).
 */
#include <stddef.h>

enum { PW_PREFIX_TEMP_MAX = 4, PW_PREFIX_TEMP_PATH = 512 };

typedef struct PwPrefixTemp {
    int in_environment;         /* inside user.reg's [Environment] */
    size_t count;
    /* drive_c-relative, forward slashes, no "." or ".." components */
    char folders[PW_PREFIX_TEMP_MAX][PW_PREFIX_TEMP_PATH];
} PwPrefixTemp;

/* Starts with windows/temp. */
void pw_prefix_temp_init(PwPrefixTemp *temp);
/* One line of user.reg: a C: path in "TEMP" or "TMP" under [Environment]
 * adds its folder. Anything else, including unexpanded %VAR% forms and
 * paths on other drives, is ignored. */
void pw_prefix_temp_line(PwPrefixTemp *temp, const char *line, size_t length);

#endif
