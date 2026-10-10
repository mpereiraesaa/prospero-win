/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_PREFIX_H
#define PW_WINE_PREFIX_H
#include "../src/pw_prefix_temp.h"
#include "../src/pw_app_profile.h"

/* Reads <prefix>/user.reg, if there is one, and creates the temp folders it
 * names and C:\windows\temp under <prefix>/drive_c (src/pw_prefix_temp.h).
 * Returns how many folders there are afterwards, or -1 when one could not be
 * created, as for a prefix Wine has not made yet. */
int pw_wine_prefix_temp_create(const char *prefix, PwPrefixTemp *temp);

/* WoW64 loads its CPU backend from the prefix's system32 only, so a prefix
 * made before the native backend shipped lacks it. Copies source (the
 * runtime's x86_64-windows/<name>) to <prefix>/drive_c/windows/system32/<name>
 * when that file is missing or differs, through a temporary file renamed into
 * place. Returns 1 when it copied, 0 when the prefix already had the same
 * file, -1 on any failure (Wine then falls back to the prefix's own CPU). */
int pw_wine_prefix_cpu_install(const char *prefix, const char *source, const char *name);

/* Install a packaged Mesa WGL/Zink architecture directory before Wine starts.
 * The prefix must already exist and have been initialized by Wine; this API
 * does not bootstrap a new prefix. Missing prefixes are refused safely.
 * Requires opengl32.dll and libgallium_wgl.dll; installs every DLL companion.
 * Validates the entire directory before changing the prefix: at most32 DLLs,
 * 127-byte safe names, regular files no larger than128MiB, matching PE machine
 * and optional-header magic. Provider files must remain immutable during this
 * serialized call. PE32 targets syswow64; PE64 targets system32. Each changed
 * DLL is streamed through a temporary file and atomically renamed. A failed
 * installation must prevent launch: the group is not a multi-file transaction.
 * Returns PW_OK or an error; copied reports completed replacements even on
 * failure. Packager provenance/import validation is a separate prerequisite. */
int pw_wine_prefix_zink_install(const char *prefix, const char *provider,
                                PwAppArchitecture architecture, unsigned *copied);

#endif
