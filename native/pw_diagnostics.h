/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_DIAGNOSTICS_H
#define PW_DIAGNOSTICS_H
#include <stddef.h>
#include <stdint.h>

/* Eight process sessions; two bounded chunks per session. No game data. */
#ifndef PW_DIAGNOSTICS_CHUNK
#define PW_DIAGNOSTICS_CHUNK (1024u * 1024u)
#endif
/* Records wait here for the writer thread; a full buffer drops records
 * rather than stall the game thread that logs them. */
#ifndef PW_DIAGNOSTICS_PENDING
#define PW_DIAGNOSTICS_PENDING (512u * 1024u)
#endif
enum { PW_DIAGNOSTICS_SESSIONS = 8 };
int pw_diagnostics_open(const char *root, const char *build, const char *profile,
                        uint32_t cycle);
/* Any thread: formats the record into the buffer and returns; a writer
 * thread saves it and sends it. Never waits for the file or the network. */
void pw_diagnostics_log(const char *format, ...);
/* Asks the writer for a pass, at most every 100 ms; never waits for it. */
void pw_diagnostics_tick(uint64_t now_ns);
/* Saves every buffered record before returning. */
void pw_diagnostics_close(const char *reason);
void pw_diagnostics_resume(void);
void pw_diagnostics_status(char *buffer, size_t size);
/* Signal path: fixed formatting, write() to the session file and send() to
 * the live channel; never takes a mutex. segment is the ntdll segment holding
 * pc (-1 when none), and segment_offset pc's offset in it. */
void pw_diagnostics_emergency(int signal, uintptr_t address, uint64_t pc,
                              int segment, uint64_t segment_offset);
#ifdef PW_DIAGNOSTICS_TESTING
void pw_diagnostics_test_retry_interval(uint64_t ns);
/* Waits until the writer has saved and sent every buffered record. */
void pw_diagnostics_test_settle(void);
/* While set, a session starts no writer thread, as when its start fails. */
void pw_diagnostics_test_refuse_writer(int refuse);
#endif
#endif
