/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_DIAGNOSTICS_H
#define PW_DIAGNOSTICS_H
#include <stddef.h>
#include <stdint.h>

/* Eight process sessions; two bounded chunks per session. No game data. */
#ifndef PW_DIAGNOSTICS_CHUNK
#define PW_DIAGNOSTICS_CHUNK (1024u * 1024u)
#endif
enum { PW_DIAGNOSTICS_SESSIONS = 8 };
int pw_diagnostics_open(const char *root, const char *build, const char *profile,
                        uint32_t cycle);
void pw_diagnostics_log(const char *format, ...);
void pw_diagnostics_tick(uint64_t now_ns);
void pw_diagnostics_close(const char *reason);
void pw_diagnostics_resume(void);
void pw_diagnostics_status(char *buffer, size_t size);
/* Signal path: fixed formatting and write only, never takes a mutex. */
void pw_diagnostics_emergency(int signal, uintptr_t address, uint64_t pc);
#endif
