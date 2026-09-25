/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_guest_process.h"

#include <string.h>

enum { PW_GUEST_PROCESS_TEXT_CAPACITY = 1024u };

/* Writes one UTF-16LE string into the page and returns its offset. */
static int write_wide(uint8_t *page, uint32_t page_bytes, uint32_t *cursor,
                      const char *ascii, uint32_t *offset)
{
    size_t length;

    if (!page || !cursor || !ascii || !offset)
        return PW_ERR_PRECONDITION;
    length = strlen(ascii);
    if (length == 0u || length > (UINT16_MAX / 2u) - 1u ||
        (uint64_t)*cursor + (length + 1u) * 2u > page_bytes)
        return PW_ERR_LIMIT;
    for (size_t index = 0u; index < length; ++index) {
        const unsigned char value = (unsigned char)ascii[index];

        if (value < 0x20u || value > 0x7eu)
            return PW_ERR_UNSUPPORTED;
    }
    *offset = *cursor;

    while (*ascii != '\0') {
        page[*cursor] = (uint8_t)*ascii++;
        page[*cursor + 1u] = 0u;
        *cursor += 2u;
    }
    page[*cursor] = 0u;
    page[*cursor + 1u] = 0u;
    *cursor += 2u;
    return PW_OK;
}

static void set_unicode_string(uint8_t *page, uint32_t field, uint32_t offset,
                               const char *text)
{
    const uint32_t base = (uint32_t)(uintptr_t)page;
    const uint16_t bytes = (uint16_t)(strlen(text) * 2u);
    /*
     * MaximumLength counts the terminating NUL's room, as it does for every
     * caller of RtlInitUnicodeString (Length = chars * 2,
     * MaximumLength = (chars + 1) * 2). Writing the two as the same number
     * leaves the string with nowhere to put its terminator: a caller that
     * copies MaximumLength bytes and NUL-terminates - which is what ntdll's
     * own init_user_process_params does when it rebuilds the parameters it was
     * handed - then writes one word into whatever follows this string. That is
     * measured, not theoretical: with MaximumLength == Length here, ntdll's
     * copy of DllPath came back with its first two characters replaced by a
     * backslash and a NUL, because the current-directory string before it in
     * the same allocation had neither.
     */
    const uint16_t maximum = (uint16_t)(bytes + 2u);
    const uint32_t buffer = base + offset;

    memcpy(page + field, &bytes, 2u);
    memcpy(page + field + 2u, &maximum, 2u);
    memcpy(page + field + 4u, &buffer, 4u);
}

/*
 * A minimal but populated RTL_USER_PROCESS_PARAMETERS: sizes, the current
 * directory, the DLL and image paths, the command line and an environment
 * block, all as UTF-16LE strings inside the same page. ntdll reads these
 * during loader and heap initialisation, and a zeroed page is what makes it
 * dereference a null Buffer.
 */
static int populate_parameters(uint8_t *page, uint32_t page_bytes,
                               const char *root_module, uint8_t root_application,
                               const char *image_path_input,
                               const char *current_directory_input,
                               const char *application_directory_input,
                               const char *command_line_input,
                               uint32_t *length_out)
{
    /*
     * The two names a Windows process carries for itself. The image is the
     * process's own file, and it is named in the root it was actually opened
     * from: an application's own image is a component of C:, a system module
     * is a component of the Windows directory. The current directory is the
     * directory that image lives in, which is where a process started from it
     * would run.
     */
    static const char application[] = "C:\\";
    static const char system32[] = "C:\\windows\\system32\\";
    /*
     * The DLL search path, in the order Wine itself builds it
     * (dlls/ntdll/loader.c:2574 get_dll_load_path): the directory of the
     * process's own image first, then the system directories. Wine's own
     * system_path is "C:\\windows\\system32;C:\\windows\\system;C:\\windows";
     * this gate serves the two directories it owns and refuses the third by
     * the same path rule it applies to every other name, so the list names the
     * directories a Windows loader would search and the loader still reaches
     * the application's own modules through the first entry. Measured: with
     * the system directory alone here, the loader found kernelbase.dll and then
     * terminated the process because it could not find the application's own
     * b.dll.
     */
    static const char application_dll_path[] =
        "C:\\;C:\\windows\\system32;C:\\windows\\system;C:\\windows";
    static const char system_dll_path[] =
        "C:\\windows\\system32;C:\\windows\\system;C:\\windows";
    const char *default_directory = root_application ? application : system32;
    const char *current_directory = current_directory_input
        ? current_directory_input : default_directory;
    const char *application_directory = application_directory_input
        ? application_directory_input : application;
    const char *default_dll_path = root_application ? application_dll_path
                                                    : system_dll_path;
    uint32_t cursor = 0x100u;
    char image_path[PW_GUEST_PROCESS_TEXT_CAPACITY];
    char dll_path[PW_GUEST_PROCESS_TEXT_CAPACITY];
    const char *command_line;
    int status;
    uint32_t current_offset;
    uint32_t dll_offset;
    uint32_t image_offset;
    uint32_t command_offset;
    uint32_t environment_offset;

    if (!page || page_bytes < 4096u || !root_module)
        return PW_ERR_PRECONDITION;
    if (image_path_input) {
        if (strlen(image_path_input) >= sizeof(image_path))
            return PW_ERR_LIMIT;
        memcpy(image_path, image_path_input, strlen(image_path_input) + 1u);
    } else if (strlen(root_module) + strlen(default_directory) + 1u >
               sizeof(image_path)) {
        return PW_ERR_LIMIT;
    } else {
        memcpy(image_path, default_directory, strlen(default_directory));
        memcpy(image_path + strlen(default_directory), root_module,
               strlen(root_module) + 1u);
    }
    if (application_directory_input) {
        const size_t directory_length = strlen(application_directory);
        const size_t system_length = strlen(";C:\\windows\\system32;C:\\windows\\system;C:\\windows");

        if (directory_length + system_length + 1u > sizeof(dll_path))
            return PW_ERR_LIMIT;
        memcpy(dll_path, application_directory, directory_length);
        memcpy(dll_path + directory_length,
               ";C:\\windows\\system32;C:\\windows\\system;C:\\windows",
               system_length + 1u);
    } else {
        if (strlen(default_dll_path) >= sizeof(dll_path))
            return PW_ERR_LIMIT;
        memcpy(dll_path, default_dll_path, strlen(default_dll_path) + 1u);
    }
    command_line = command_line_input ? command_line_input : image_path;

    /*
     * Every string here is published the way Windows stores it, with room for
     * the terminator counted in MaximumLength (see set_unicode_string): ntdll
     * copies MaximumLength bytes and terminates what it copied, so a
     * MaximumLength equal to Length writes one word into the string that
     * follows in the same allocation.
     *
     * The current directory is the image's own directory, which is also the
     * first entry of the search path above.
     */
    status = write_wide(page, page_bytes, &cursor, current_directory,
                        &current_offset);
    if (status != PW_OK)
        return status;
    status = write_wide(page, page_bytes, &cursor, dll_path, &dll_offset);
    if (status != PW_OK)
        return status;
    status = write_wide(page, page_bytes, &cursor, image_path, &image_offset);
    if (status != PW_OK)
        return status;
    status = write_wide(page, page_bytes, &cursor, command_line,
                        &command_offset);
    if (status != PW_OK)
        return status;
    status = write_wide(page, page_bytes, &cursor, "SystemRoot=C:\\windows",
                        &environment_offset);
    if (status != PW_OK)
        return status;
    page[cursor] = 0u;
    page[cursor + 1u] = 0u;
    cursor += 2u;

    set_unicode_string(page, 0x24u, current_offset, current_directory);
    set_unicode_string(page, 0x30u, dll_offset, dll_path);
    set_unicode_string(page, 0x38u, image_offset, image_path);
    set_unicode_string(page, 0x40u, command_offset, command_line);
    {
        const uint32_t base = (uint32_t)(uintptr_t)page;
        const uint32_t environment = base + environment_offset;

        memcpy(page + 0x48u, &environment, 4u);
    }
    memcpy(page + 0x00u, &cursor, 4u);      /* MaximumLength */
    memcpy(page + 0x04u, &cursor, 4u);      /* Length */
    if (length_out)
        *length_out = cursor;
    return PW_OK;
}

/*
 * One zeroed page below the 32-bit boundary. A page the caller cannot address
 * is refused before anything is committed, and a page whose commit fails is
 * given straight back, so the unit never leaves a reservation behind.
 */
static int map_region(const PwGuestProcessConfig *config, uint32_t base,
                      uint32_t bytes, PwVmRegion *region, uint32_t *address)
{
    const PwVmBackend *backend = config->backend;
    int status;

    if (!backend || !backend->reserve || !backend->commit || bytes == 0u)
        return PW_ERR_PRECONDITION;
    status = PW_ERR_VM;
    if ((backend->capabilities & PW_VM_CAP_EXACT_ADDRESS) != 0u)
        status = backend->reserve_at(backend->context, base, (size_t)bytes,
                                     backend->page_bytes, region);
    if (status != PW_OK)
        status = backend->reserve(backend->context, (size_t)bytes,
                                  backend->page_bytes, region);
    if (status != PW_OK)
        return status;
    if ((uint64_t)(uintptr_t)region->exec_base + region->bytes >
        0x100000000ull) {
        (void)backend->release(backend->context, region);
        return PW_ERR_UNSUPPORTED;
    }
    status = backend->commit(backend->context, region, 0u, region->bytes,
                             PW_PROT_READ | PW_PROT_WRITE);
    if (status != PW_OK) {
        (void)backend->release(backend->context, region);
        return status;
    }
    memset(region->write_base, 0, region->bytes);
    *address = (uint32_t)(uintptr_t)region->exec_base;
    return PW_OK;
}

static int release_pages(PwGuestProcess *process, const PwVmBackend *backend,
                         uint32_t count)
{
    int status = PW_OK;
    uint32_t kept = 0u;

    process->released = 0u;
    for (uint32_t index = 0u; index < count; ++index) {
        if (backend->release(backend->context, &process->pages[index]) !=
            PW_OK) {
            /*
             * The page is still ours: a failed release must not drop the only
             * record of it, or the mapping leaks and nobody can retry. Keep
             * it in the inventory (compacted, so the array stays dense) and
             * report the failure.
             */
            status = PW_ERR_VM;
            if (kept != index)
                process->pages[kept] = process->pages[index];
            kept++;
        } else {
            process->released++;
        }
    }
    /* What the process still owns after this attempt, not what it once had. */
    process->mapped = kept;
    return status;
}

int pw_guest_process_create(PwGuestProcess *process,
                            const PwGuestProcessConfig *config)
{
    uint32_t stack_base;
    uint32_t stack_bytes;

    if (!process || !config || !config->backend || !config->root_module)
        return PW_ERR_PRECONDITION;
    memset(process, 0, sizeof(*process));
    stack_base = config->stack_base != 0u ? config->stack_base
                                          : PW_GUEST_PROCESS_STACK_BASE;
    /*
     * The stack's size, in the order Windows and Wine decide it: what the
     * caller asked for, then what the process's own image asks for in its
     * headers, then the 1 MiB a linker that emits nothing leaves. Measured:
     * the pinned runtime's own locale and registry startup needs more than the
     * 64 KiB this unit used to map and died on a push at the stack's lower
     * bound.
     */
    stack_bytes = config->stack_bytes != 0u ? config->stack_bytes
                 : config->stack_reserve != 0u
                     ? (config->stack_reserve > PW_GUEST_PROCESS_STACK_MAX_BYTES
                            ? PW_GUEST_PROCESS_STACK_MAX_BYTES
                            : config->stack_reserve)
                     : PW_GUEST_PROCESS_STACK_DEFAULT_BYTES;
    if ((stack_bytes & ((uint32_t)config->backend->page_bytes - 1u)) != 0u)
        stack_bytes = (stack_bytes + (uint32_t)config->backend->page_bytes) &
                      ~((uint32_t)config->backend->page_bytes - 1u);

    if (map_region(config, stack_base, stack_bytes, &process->pages[0],
                   &process->layout.stack_base) != PW_OK)
        return PW_ERR_VM;
    process->mapped = 1u;
    process->layout.stack_bytes = (uint32_t)process->pages[0].bytes;
    if (process->layout.stack_bytes < stack_bytes)
        goto failed;
    if (map_region(config, PW_GUEST_PROCESS_TEB_BASE,
                   PW_GUEST_PROCESS_PAGE_BYTES, &process->pages[1],
                   &process->layout.teb_base) != PW_OK)
        goto failed;
    process->mapped = 2u;
    process->layout.teb_bytes = (uint32_t)process->pages[1].bytes;
    if (map_region(config, PW_GUEST_PROCESS_PEB_BASE,
                   PW_GUEST_PROCESS_PAGE_BYTES, &process->pages[2],
                   &process->layout.peb_base) != PW_OK)
        goto failed;
    process->mapped = 3u;
    if (map_region(config, PW_GUEST_PROCESS_PARAMETERS_BASE,
                   PW_GUEST_PROCESS_PAGE_BYTES, &process->pages[3],
                   &process->layout.parameters_base) != PW_OK)
        goto failed;
    process->mapped = 4u;
    process->layout.parameters_bytes = (uint32_t)process->pages[3].bytes;
    if (populate_parameters(process->pages[3].write_base,
                            process->layout.parameters_bytes,
                            config->root_module,
                            config->root_application,
                            config->image_path,
                            config->current_directory,
                            config->application_directory,
                            config->command_line,
                            &process->layout.parameters_length) != PW_OK)
        goto failed;

    /* The TEB: the documented NT fields ntdll reads through FS, and the
     * dispatcher the unix side would have published in WOW32Reserved. */
    {
        uint8_t *teb = process->pages[1].write_base;
        const uint32_t stack_high =
            process->layout.stack_base + process->layout.stack_bytes;
        const uint32_t self = process->layout.teb_base;
        const uint32_t peb = process->layout.peb_base;
        const uint32_t dispatcher = config->dispatcher_thunk;
        const uint32_t thread_local_storage = 0u;
        /*
         * A thread's activation context stack lives inside its own TEB and the
         * TEB points at it; the Unix side sets that up for every real i386
         * thread (dlls/ntdll/unix/virtual.c:4023) and ntdll dereferences the
         * pointer before it checks whether any frame is active - a null there
         * is a fault inside RtlFindActivationContextSectionString, which is
         * how this field was found. The embedded stack stays all zero, which is
         * exactly what a thread with no manifest has: no active frame, an
         * empty frame-list cache and no cookie yet.
         */
        const uint32_t activation_stack =
            self + PW_GUEST_PROCESS_TEB_ACTIVATION_STACK;

        memcpy(teb + 0x04u, &stack_high, 4u);           /* StackBase */
        memcpy(teb + 0x08u, &process->layout.stack_base, 4u); /* StackLimit */
        memcpy(teb + 0x18u, &self, 4u);                 /* Self */
        memcpy(teb + 0x2cu, &thread_local_storage, 4u); /* no TLS modules */
        memcpy(teb + 0x30u, &peb, 4u);                  /* PEB */
        /*
         * The thread names itself here, and the run answers the same pair
         * wherever it is asked. The loader reads ClientId when it walks its
         * own thread list, and this run models exactly one thread: the one it
         * started.
         */
        {
            const uint32_t process_id = PW_GUEST_PROCESS_ID;
            const uint32_t thread_id = PW_GUEST_THREAD_ID;

            memcpy(teb + PW_GUEST_PROCESS_TEB_CLIENT_ID, &process_id, 4u);
            memcpy(teb + PW_GUEST_PROCESS_TEB_CLIENT_ID + 4u, &thread_id, 4u);
        }
        memcpy(teb + PW_GUEST_PROCESS_TEB_ACTIVATION_POINTER,
               &activation_stack, 4u);                  /* ActivationContextStackPointer */
        if (dispatcher != 0u)
            memcpy(teb + 0xc0u, &dispatcher, 4u);       /* WOW32Reserved */
    }
    /* The PEB: the image base and the parameters the loader reads. */
    {
        uint8_t *peb = process->pages[2].write_base;

        memcpy(peb + 0x08u, &config->image_base, 4u);   /* ImageBaseAddress */
        memcpy(peb + 0x10u, &process->layout.parameters_base, 4u);
    }
    return PW_OK;

failed:
    /* Nothing has been handed to the caller yet, so everything this unit
     * mapped goes back, including a parameters page the caller never saw. */
    (void)release_pages(process, config->backend, process->mapped);
    return PW_ERR_VM;
}

int pw_guest_process_release(PwGuestProcess *process,
                             const PwVmBackend *backend)
{
    uint32_t count;

    if (!process || !backend || !backend->release)
        return PW_ERR_PRECONDITION;
    /*
     * The stack, the TEB and the PEB belong to this unit for the whole run.
     * The parameters page does not: the caller hands it to the guest's own
     * call registry (ntdll replaces it with a copy and releases it), so
     * releasing it here as well would unmap the same block twice.
     */
    count = process->mapped < 3u ? process->mapped : 3u;
    return release_pages(process, backend, count);
}
