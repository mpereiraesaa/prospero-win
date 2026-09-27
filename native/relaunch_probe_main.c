/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Probe: can the title restart itself with arguments?
 *
 * The launcher design runs one game per title process: choosing a game
 * restarts the title with the game's profile as an argument, and closing it
 * restarts the title into the launcher. Each generation logs its argv and the
 * time since the previous generation asked to restart (t0=, CLOCK_REALTIME).
 * Generation 0 tries sceSystemServiceLoadExec on its own eboot, then
 * sceSystemServiceLaunchApp on its own title ID; generation 1 restarts once
 * more the same way; generation 2 exits normally.
 */
#include "ps5log/ps5log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PW_TITLE_ID "PPSA99995"
#define PW_APP_NAME "prospero-win-relaunch-probe"
#define PW_SELF_EBOOT "/app0/eboot.bin"

typedef struct {
    uint32_t structsize, user_id, app_opt, padding;
    uint64_t crash_report;
    uint32_t check_flag, tail_padding;
} LaunchContext;

int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int sceSystemServiceLaunchApp(const char *title_id, char **argv, LaunchContext *context);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(uint32_t *user_id);

static uint64_t now_ns(int clock)
{
    struct timespec value;

    if (clock_gettime(clock, &value) != 0) return 0u;
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

static const char *argument(int argc, char **argv, const char *name)
{
    size_t length = strlen(name);

    for (int i = 0; i < argc; i++)
        if (argv[i] && !strncmp(argv[i], name, length) && argv[i][length] == '=')
            return argv[i] + length + 1;
    return NULL;
}

/* Restart with gen=<next>, via=<method>, t0=<now>, profile=pinball. */
static void restart(int next, const char *method)
{
    static char gen[16], via[32], t0[40];
    char *next_argv[] = { gen, via, t0, "profile=pinball", "path=C:\\Games\\Pinball\\PINBALL.EXE",
                          NULL };
    int rc;

    snprintf(gen, sizeof(gen), "gen=%d", next);
    snprintf(t0, sizeof(t0), "t0=%llu", (unsigned long long)now_ns(CLOCK_REALTIME));
    if (!strcmp(method, "loadexec")) {
        snprintf(via, sizeof(via), "via=loadexec");
        PS5LOG_LOG("PW_RELAUNCH try method=loadexec path=%s", PW_SELF_EBOOT);
        rc = sceSystemServiceLoadExec(PW_SELF_EBOOT, next_argv);
        /* Only reached when the image was not replaced. */
        PS5LOG_LOG("PW_RELAUNCH result method=loadexec rc=0x%08x", (unsigned)rc);
        return;
    }
    {
        LaunchContext context;
        uint32_t user = 0xffffffffu;
        int user_rc = sceUserServiceInitialize(NULL);

        memset(&context, 0, sizeof(context));
        context.structsize = sizeof(context);
        if (sceUserServiceGetForegroundUser(&user) == 0) context.user_id = user;
        snprintf(via, sizeof(via), "via=launchapp");
        PS5LOG_LOG("PW_RELAUNCH try method=launchapp title=%s user_rc=0x%08x user=0x%x",
                   PW_TITLE_ID, (unsigned)user_rc, (unsigned)user);
        rc = sceSystemServiceLaunchApp(PW_TITLE_ID, next_argv, &context);
        PS5LOG_LOG("PW_RELAUNCH result method=launchapp rc=0x%08x", (unsigned)rc);
    }
}

int main(int argc, char **argv)
{
    ps5log_config log_config;
    const char *gen_text = argument(argc, argv, "gen"), *via = argument(argc, argv, "via");
    const char *t0_text = argument(argc, argv, "t0");
    int gen = gen_text ? atoi(gen_text) : 0;

    ps5log_config_defaults(&log_config);
    if (ps5log_load_config(ps5log_default_conf_paths, ps5log_default_conf_path_count,
                           &log_config, NULL) == 0)
        ps5log_init(&log_config, PW_TITLE_ID, PW_APP_NAME, now_ns(CLOCK_MONOTONIC));
    PS5LOG_LOG("PW_RELAUNCH start gen=%d argc=%d pid=%d", gen, argc, (int)getpid());
    for (int i = 0; i < argc; i++) PS5LOG_LOG("PW_RELAUNCH argv[%d]=%s", i, argv[i] ? argv[i] : "(null)");
    if (t0_text) {
        uint64_t t0 = strtoull(t0_text, NULL, 10), now = now_ns(CLOCK_REALTIME);
        PS5LOG_LOG("PW_RELAUNCH restarted gen=%d via=%s after_ms=%llu", gen, via ? via : "?",
                   (unsigned long long)((now - t0) / 1000000u));
    }
    sleep(3);   /* visible on screen, and the log reaches the host */
    if (gen == 0) {
        restart(1, "loadexec");
        restart(1, "launchapp");
        PS5LOG_LOG("PW_RELAUNCH gen=0 exiting after launch attempts");
    } else if (gen == 1) {
        restart(2, via && !strcmp(via, "launchapp") ? "launchapp" : "loadexec");
        PS5LOG_LOG("PW_RELAUNCH gen=1 exiting after restart attempt");
    } else {
        PS5LOG_LOG("PW_RELAUNCH done gen=%d", gen);
    }
    ps5log_close("relaunch-probe");
    exit(0);
}
