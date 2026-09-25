/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The guest process state on its own: the stack, the TEB, the PEB and the
 * process parameters the loader reads.
 *
 * The unit maps four pages and fills the documented NT fields, so this test
 * checks the layout rather than the loader: the TEB's StackBase/StackLimit/
 * Self/PEB and the dispatcher the unix side would publish, the PEB's image
 * base and parameters pointer, and a process-parameters structure whose
 * strings live inside its own page and are self-consistent. It also checks the
 * two things the extraction had to get right: a page whose commit fails leaves
 * nothing mapped, and releasing the process gives back exactly the three pages
 * the unit owns - never the parameters page, which belongs to the guest's call
 * registry.
 */
#include "../src/pw_guest_process.h"
#include "../src/pw_vm_posix.h"

#include <assert.h>
#include <string.h>

/* Reads a UTF-16LE string out of the parameters page into ASCII. */
static void read_wide(const uint8_t *page, uint32_t field, char *out,
                      size_t out_bytes)
{
    uint16_t length = 0u;
    uint32_t buffer = 0u;
    size_t index = 0u;

    memcpy(&length, page + field, 2u);
    memcpy(&buffer, page + field + 4u, 4u);
    for (uint32_t unit = 0u; unit < 2u * (length / 2u) && index + 1u < out_bytes;
         unit += 2u) {
        const uint8_t low = *(const uint8_t *)(uintptr_t)(buffer + unit);

        out[index++] = low == 0u ? '?' : (char)low;
    }
    out[index] = '\0';
}

/* A backend whose commit can be made to fail, counting what it handed out. */
typedef struct FaultyBackend {
    PwVmBackend real;
    unsigned fail_commit;
    unsigned commits;
    unsigned releases;
} FaultyBackend;

static int faulty_commit(void *context, const PwVmRegion *region, size_t offset,
                         size_t bytes, unsigned protection)
{
    FaultyBackend *faults = context;

    faults->commits++;
    if (faults->fail_commit)
        return PW_ERR_VM;
    return faults->real.commit(faults->real.context, region, offset, bytes,
                               protection);
}

static int faulty_release(void *context, PwVmRegion *region)
{
    FaultyBackend *faults = context;

    faults->releases++;
    return faults->real.release(faults->real.context, region);
}

static int faulty_reserve(void *context, size_t bytes, size_t alignment,
                          PwVmRegion *out)
{
    FaultyBackend *faults = context;

    return faults->real.reserve(faults->real.context, bytes, alignment, out);
}

static int faulty_reserve_at(void *context, uint64_t address, size_t bytes,
                             size_t alignment, PwVmRegion *out)
{
    FaultyBackend *faults = context;

    return faults->real.reserve_at(faults->real.context, address, bytes,
                                   alignment, out);
}

static int faulty_protect(void *context, const PwVmRegion *region, size_t offset,
                          size_t bytes, unsigned protection)
{
    FaultyBackend *faults = context;

    return faults->real.protect(faults->real.context, region, offset, bytes,
                                protection);
}

static PwVmBackend faulty_backend(FaultyBackend *faults,
                                  const PwVmBackend *real)
{
    faults->real = *real;
    return (PwVmBackend){
        .context = faults,
        .capabilities = real->capabilities,
        .page_bytes = real->page_bytes,
        .reserve = faulty_reserve,
        .commit = faulty_commit,
        .protect = faulty_protect,
        .release = faulty_release,
        .reserve_at = faulty_reserve_at,
    };
}

int main(void)
{
    PwVmBackend real;
    PwVmBackend backend;
    FaultyBackend faults;
    PwGuestProcess process;
    PwGuestProcessConfig config;
    const uint8_t *teb;
    const uint8_t *peb;
    const uint8_t *parameters;
    char text[128];
    uint32_t value = 0u;
    uint32_t environment = 0u;

    assert(pw_vm_posix_backend(&real) == PW_OK);
    assert(real.reserve && real.commit && real.release);
    backend = faulty_backend(&faults, &real);

    memset(&config, 0, sizeof(config));
    config.backend = &backend;
    config.image_base = 0x10000000u;
    config.dispatcher_thunk = 0x10412344u;
    config.root_module = "ntdll.dll";

    /*
     * A process whose root module is the application's own image: the image is
     * a component of C: and the search path starts in that directory, which is
     * the only way the loader reaches the modules an application ships next to
     * itself. Measured: with the system directory alone in this position, the
     * run loaded kernelbase.dll and then terminated the process with
     * STATUS_DLL_NOT_FOUND, naming the application's own b.dll as the module it
     * could not find. The parameters page is handed to the guest, which frees
     * it itself, so this scenario gives it back here before mapping the next
     * process.
     */
    {
        PwGuestProcessConfig app_config = config;
        uint8_t *app_parameters;

        app_config.root_application = 1u;
        assert(pw_guest_process_create(&process, &app_config) == PW_OK);
        app_parameters = process.pages[3].write_base;
        read_wide(app_parameters, 0x24u, text, sizeof(text));
        assert(strcmp(text, "C:\\") == 0);
        read_wide(app_parameters, 0x30u, text, sizeof(text));
        assert(strcmp(text,
                      "C:\\;C:\\windows\\system32;C:\\windows\\system;C:\\windows")
               == 0);
        read_wide(app_parameters, 0x38u, text, sizeof(text));
        assert(strcmp(text, "C:\\ntdll.dll") == 0);
        read_wide(app_parameters, 0x40u, text, sizeof(text));
        assert(strcmp(text, "C:\\ntdll.dll") == 0);
        assert(pw_guest_process_release(&process, &backend) == PW_OK);
        assert(backend.release(backend.context, &process.pages[3]) == PW_OK);
        assert(process.mapped == 0u);
    }
    {
        PwGuestProcessConfig manifest_config = config;
        uint8_t *manifest_parameters;

        manifest_config.root_application = 1u;
        manifest_config.image_path =
            "C:\\Games\\Pinball\\PINBALL.EXE";
        manifest_config.current_directory = "C:\\Games\\Pinball";
        manifest_config.application_directory = "C:\\Games\\Pinball";
        manifest_config.command_line =
            "\"C:\\Games\\Pinball\\PINBALL.EXE\" -windowed";
        assert(pw_guest_process_create(&process, &manifest_config) == PW_OK);
        manifest_parameters = process.pages[3].write_base;
        read_wide(manifest_parameters, 0x24u, text, sizeof(text));
        assert(strcmp(text, "C:\\Games\\Pinball") == 0);
        read_wide(manifest_parameters, 0x30u, text, sizeof(text));
        assert(strcmp(text,
                      "C:\\Games\\Pinball;C:\\windows\\system32;"
                      "C:\\windows\\system;C:\\windows") == 0);
        read_wide(manifest_parameters, 0x38u, text, sizeof(text));
        assert(strcmp(text, "C:\\Games\\Pinball\\PINBALL.EXE") == 0);
        read_wide(manifest_parameters, 0x40u, text, sizeof(text));
        assert(strcmp(text,
                      "\"C:\\Games\\Pinball\\PINBALL.EXE\" -windowed") ==
               0);
        assert(pw_guest_process_release(&process, &backend) == PW_OK);
        assert(backend.release(backend.context, &process.pages[3]) == PW_OK);
    }

    /*
     * The layout: the stack, the TEB, the PEB and the parameters page at the
     * documented bases. The stack is the size Windows gives a process whose
     * image names no reserve - 1 MiB - because the pinned runtime's own
     * startup needs more than the single 64 KiB page this unit used to map and
     * died on a push at the stack's lower bound.
     */
    assert(pw_guest_process_create(&process, &config) == PW_OK);
    assert(process.mapped == PW_GUEST_PROCESS_PAGES);
    assert(process.layout.stack_base == PW_GUEST_PROCESS_STACK_BASE);
    assert(process.layout.stack_bytes == PW_GUEST_PROCESS_STACK_DEFAULT_BYTES);
    assert(process.pages[0].bytes >= PW_GUEST_PROCESS_STACK_DEFAULT_BYTES);
    assert(process.layout.teb_base == PW_GUEST_PROCESS_TEB_BASE);
    assert(process.layout.peb_base == PW_GUEST_PROCESS_PEB_BASE);
    assert(process.layout.parameters_base == PW_GUEST_PROCESS_PARAMETERS_BASE);
    assert(process.layout.parameters_length > 0x100u);

    /* The TEB: the NT fields ntdll reads through FS, and the dispatcher. */
    teb = process.pages[1].write_base;
    memcpy(&value, teb + 0x04u, 4u);
    assert(value == PW_GUEST_PROCESS_STACK_BASE +
                     PW_GUEST_PROCESS_STACK_DEFAULT_BYTES);
    memcpy(&value, teb + 0x08u, 4u);
    assert(value == PW_GUEST_PROCESS_STACK_BASE);
    memcpy(&value, teb + 0x18u, 4u);
    assert(value == PW_GUEST_PROCESS_TEB_BASE);
    memcpy(&value, teb + 0x30u, 4u);
    assert(value == PW_GUEST_PROCESS_PEB_BASE);
    memcpy(&value, teb + 0xc0u, 4u);
    assert(value == config.dispatcher_thunk);
    memcpy(&value, teb + 0x2cu, 4u);
    assert(value == 0u);
    /*
     * The activation context stack: the TEB points at the copy embedded in
     * itself, exactly as the Unix side sets it up for a real i386 thread, and
     * that copy starts empty (no active frame), because a thread with no
     * manifest has none. ntdll dereferences the pointer before it looks at the
     * frame, so an unset field here is a fault, not an empty answer.
     */
    memcpy(&value, teb + PW_GUEST_PROCESS_TEB_ACTIVATION_POINTER, 4u);
    assert(value == PW_GUEST_PROCESS_TEB_BASE +
                     PW_GUEST_PROCESS_TEB_ACTIVATION_STACK);
    for (uint32_t offset = 0u; offset < 0x18u; offset += 4u) {
        memcpy(&value, teb + PW_GUEST_PROCESS_TEB_ACTIVATION_STACK + offset,
               4u);
        assert(value == 0u);
    }

    /* The PEB: the image it is running and the parameters behind it. */
    peb = process.pages[2].write_base;
    memcpy(&value, peb + 0x08u, 4u);
    assert(value == config.image_base);
    memcpy(&value, peb + 0x10u, 4u);
    assert(value == PW_GUEST_PROCESS_PARAMETERS_BASE);

    /* The parameters: self-consistent sizes and strings inside the page. */
    parameters = process.pages[3].write_base;
    memcpy(&value, parameters + 0x00u, 4u);
    assert(value == process.layout.parameters_length);
    memcpy(&value, parameters + 0x04u, 4u);
    assert(value == process.layout.parameters_length);
    read_wide(parameters, 0x24u, text, sizeof(text));
    /*
     * The process's own names, for a root that came from the system namespace:
     * the image is the module's full path under the Windows directory, the
     * current directory is the one that image lives in, and the search path
     * starts with that directory and then names the system directories Wine
     * itself searches (dlls/ntdll/loader.c:2574 get_dll_load_path). A path the
     * gate does not serve - C:\windows\system here - is named anyway, because
     * that is what a Windows loader would search; the file service refuses it
     * by the same rule it applies to every other name.
     */
    assert(strcmp(text, "C:\\windows\\system32\\") == 0);
    read_wide(parameters, 0x30u, text, sizeof(text));
    assert(strcmp(text, "C:\\windows\\system32;C:\\windows\\system;C:\\windows")
           == 0);
    read_wide(parameters, 0x38u, text, sizeof(text));
    assert(strcmp(text, "C:\\windows\\system32\\ntdll.dll") == 0);
    read_wide(parameters, 0x40u, text, sizeof(text));
    assert(strcmp(text, "C:\\windows\\system32\\ntdll.dll") == 0);
    /*
     * Every string leaves room for its terminator: Windows and Wine both
     * define MaximumLength as Length plus that room, and a string whose
     * MaximumLength equals its Length has none. ntdll's own
     * init_user_process_params copies MaximumLength bytes and terminates what
     * it copied, so publishing the two as the same number lets that terminator
     * land in the string that follows - measured, and the reason DllPath came
     * back with two of its characters replaced.
     */
    {
        /* CurrentDirectory.DosPath, DllPath, ImagePathName, CommandLine. */
        static const uint32_t fields[] = { 0x24u, 0x30u, 0x38u, 0x40u };

        for (uint32_t index = 0u; index < sizeof(fields) / sizeof(fields[0]);
             ++index) {
            uint16_t length = 0u, maximum = 0u;

            memcpy(&length, parameters + fields[index], 2u);
            memcpy(&maximum, parameters + fields[index] + 2u, 2u);
            assert(maximum == length + 2u);
        }
    }
    memcpy(&environment, parameters + 0x48u, 4u);
    assert(environment >= PW_GUEST_PROCESS_PARAMETERS_BASE);
    assert(environment < PW_GUEST_PROCESS_PARAMETERS_BASE +
                         PW_GUEST_PROCESS_PAGE_BYTES);
    {
        /* The environment block is a NUL-terminated UTF-16 string, not a
         * UNICODE_STRING: its field at 0x48 is the pointer itself. */
        const uint8_t *block = (const uint8_t *)(uintptr_t)environment;
        size_t index = 0u;

        while (index + 1u < sizeof(text)) {
            const uint8_t low = block[index * 2u];

            if (low == 0u && block[index * 2u + 1u] == 0u)
                break;
            text[index++] = (char)low;
        }
        text[index] = '\0';
        assert(strcmp(text, "SystemRoot=C:\\windows") == 0);
    }

    /* Releasing gives back the three pages the unit owns, once. */
    assert(pw_guest_process_release(&process, &backend) == PW_OK);
    assert(process.released == 3u && process.mapped == 0u);
    assert(pw_guest_process_release(&process, &backend) == PW_OK);
    assert(process.released == 0u);
    /* The parameters page is the guest's, so the unit leaves it mapped; the
     * scenarios below build more processes, so this one gives it back here. */
    assert(backend.release(backend.context, &process.pages[3]) == PW_OK);

    /* A commit that fails leaves nothing mapped: the reservation goes back. */
    {
        const unsigned before = faults.releases;

        faults.fail_commit = 1u;
        assert(pw_guest_process_create(&process, &config) != PW_OK);
        assert(process.mapped == 0u);
        assert(faults.releases == before + 1u);
        faults.fail_commit = 0u;
    }
    /*
     * A process whose image asks for its own stack gets one: SizeOfStackReserve
     * is the field Windows and Wine size a process's stack from, and this unit
     * rounds it up to the backend's page size and bounds it. The TEB's
     * StackBase follows the stack the process actually got.
     */
    {
        PwGuestProcessConfig reserved_config = config;
        uint32_t reserved;

        reserved_config.stack_reserve = 2u * 1024u * 1024u + 1u;
        assert(pw_guest_process_create(&process, &reserved_config) == PW_OK);
        assert(process.layout.stack_bytes >= 2u * 1024u * 1024u + 1u);
        assert((process.layout.stack_bytes % backend.page_bytes) == 0u);
        memcpy(&reserved, process.pages[1].write_base + 0x04u, 4u);
        assert(reserved == PW_GUEST_PROCESS_STACK_BASE +
                           process.layout.stack_bytes);
        assert(pw_guest_process_release(&process, &backend) == PW_OK);
        /* The parameters page belongs to the guest, so a second process needs
         * it given back before the next create maps its own. */
        assert(backend.release(backend.context, &process.pages[3]) == PW_OK);
        /* A reserve past this run's bound is clamped rather than honoured. */
        reserved_config.stack_reserve = 64u * 1024u * 1024u;
        assert(pw_guest_process_create(&process, &reserved_config) == PW_OK);
        assert(process.layout.stack_bytes == PW_GUEST_PROCESS_STACK_MAX_BYTES);
        assert(pw_guest_process_release(&process, &backend) == PW_OK);
        assert(backend.release(backend.context, &process.pages[3]) == PW_OK);
    }
    return 0;
}
