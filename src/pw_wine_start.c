/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_start.h"
#include "../include/prospero_win.h"
#include <string.h>

static int fail(PwWineStart *start, int stage, int status)
{
    start->stage = stage;
    start->status = status;
    return status < 0 ? status : PW_ERR_STATE;
}

int pw_wine_start_load(PwWineStart *start, const PwWineStartConfig *config,
                       const PwWineStartOps *ops)
{
    uint8_t info[PW_PRX_MODULE_INFO_BYTES];
    uint64_t size_word = sizeof(info);
    int (*module_start)(size_t, const void *);
    void *(*adopt)(const char *, int32_t);
    int result = 0, status;

    if (!start || !config || !ops || !config->ntdll_path || !ops->load_start ||
        !ops->module_info)
        return PW_ERR_PRECONDITION;
    memset(start, 0, sizeof(*start));

    start->stage = PW_WINE_START_LOAD;
    start->module = ops->load_start(config->ntdll_path, 0, NULL, 0, NULL, &result);
    if (start->module <= 0) return fail(start, PW_WINE_START_LOAD, start->module);

    /* FW 12.02 clears the record's size word on success; the parser checks
     * it, so the expected size is restored before parsing. */
    memset(info, 0, sizeof(info));
    memcpy(info, &size_word, sizeof(size_word));
    if ((status = ops->module_info(start->module, info)) != 0)
        return fail(start, PW_WINE_START_MODULE_INFO, status);
    memcpy(info, &size_word, sizeof(size_word));
    if ((status = pw_prx_parse_module_info(info, NULL, start->segments,
                                           &start->segment_count)) != PW_PRX_OK)
        return fail(start, PW_WINE_START_MODULE_INFO, status);

    if ((status = pw_prx_find_descriptor(start->segments, start->segment_count,
                                         &start->descriptor)) != PW_PRX_OK)
        return fail(start, PW_WINE_START_DESCRIPTOR, status);

    start->wine_main = (void (*)(int, char **))pw_prx_lookup(start->descriptor, "__wine_main");
    start->virtual_stats = (unsigned int (*)(uint64_t *, unsigned int))
        pw_prx_lookup(start->descriptor, "__wine_virtual_stats");
    start->memory_stats = (unsigned int (*)(uint64_t *, unsigned int))
        pw_prx_lookup(start->descriptor, "__wine_ps5_memory_stats");
    start->fault_top = (unsigned int (*)(uint64_t *, unsigned int))
        pw_prx_lookup(start->descriptor, "__wine_virtual_fault_top");
    if (!start->wine_main) return fail(start, PW_WINE_START_ENTRY, PW_ERR_NOT_FOUND);

    /* The firmware loads the module without running its entry. */
    module_start = (int (*)(size_t, const void *))pw_prx_lookup(start->descriptor, "module_start");
    if (module_start && (start->module_start_result = module_start(0, NULL)) != 0)
        return fail(start, PW_WINE_START_MODULE_START, start->module_start_result);

    /* Let dladdr name ntdll, which Wine uses to find its own directory. */
    adopt = (void *(*)(const char *, int32_t))pw_prx_lookup(start->descriptor, "pw_wine_dl_adopt");
    if (adopt && !(start->adopted = adopt(config->ntdll_path, start->module)))
        return fail(start, PW_WINE_START_ADOPT, PW_ERR_STATE);

    start->stage = PW_WINE_START_ADOPT;
    return PW_OK;
}

int pw_wine_start_environment(PwWineStart *start, const PwWineStartConfig *config,
                              const PwWineStartOps *ops)
{
    int status;

    if (!start || !config || !ops || !ops->set_env || !config->prefix || !config->ntdll_dir ||
        config->extra_env_count > PW_WINE_START_MAX_ENV ||
        (config->extra_env_count && !config->extra_env))
        return PW_ERR_PRECONDITION;
    /* WINELOADERNOEXEC keeps __wine_main from re-executing a loader. */
    if ((status = ops->set_env("WINEPREFIX", config->prefix)) != 0 ||
        (status = ops->set_env("WINE_PS5_NTDLL_DIR", config->ntdll_dir)) != 0 ||
        (status = ops->set_env("WINELOADERNOEXEC", "1")) != 0)
        return fail(start, PW_WINE_START_ENVIRONMENT, status);
    for (uint32_t i = 0; i < config->extra_env_count; i++) {
        const PwWineStartEnv *env = &config->extra_env[i];
        if (!env->name || !env->value || !env->name[0])
            return fail(start, PW_WINE_START_ENVIRONMENT, PW_ERR_PRECONDITION);
        if ((status = ops->set_env(env->name, env->value)) != 0)
            return fail(start, PW_WINE_START_ENVIRONMENT, status);
    }
    start->stage = PW_WINE_START_ENVIRONMENT;
    return PW_OK;
}

static void run_wine_main(void *opaque)
{
    PwWineStart *start = opaque;

    start->stage = PW_WINE_START_RUNNING;
    start->wine_main(start->argc, start->argv);   /* does not return */
}

int pw_wine_start_run(PwWineStart *start, const PwWineStartConfig *config,
                      const PwWineStartOps *ops)
{
    int status;

    if (!start || !config || !ops || !ops->start_thread || !start->wine_main ||
        config->argc < 2 || config->argc > PW_WINE_START_MAX_ARGS || !config->argv)
        return PW_ERR_PRECONDITION;
    for (int i = 0; i < config->argc; i++) {
        if (!config->argv[i]) return PW_ERR_PRECONDITION;
        start->argv[i] = (char *)config->argv[i];
    }
    start->argv[config->argc] = NULL;
    start->argc = config->argc;
    start->stage = PW_WINE_START_THREAD;
    if ((status = ops->start_thread(run_wine_main, start, config->stack_bytes)) != 0)
        return fail(start, PW_WINE_START_THREAD, status);
    return PW_OK;
}
