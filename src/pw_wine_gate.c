/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_gate.h"

#include "pe_export.h"
#include "pe_reloc.h"
#include "pe_tls.h"
#include "pw_map.h"
#include "pw_module_name.h"
#include "pw_guest_call.h"
#include "pw_guest_vm.h"
#include "pw_guest_process.h"
#include "pw_nt_dispatch.h"
#include "pw_nt_handle.h"
#include "pw_wine_context.h"
#include "pw_wine_file.h"
#include "pw_wine_handle.h"
#include "pw_wine_object.h"
#include "pw_wine_query.h"
#include "pw_wine_section.h"
#include "pw_wine_thread.h"
#include "pw_wine_registry.h"
#include "pw_wine_runner.h"
#include "../include/prospero_win_vm.h"

#include <string.h>

/*
 * The dispatcher's region table is one table with two writers: the process
 * graph is declared before the run (stack, TEB, PEB, parameters page and each
 * module's image and writable span) and the NT allocation handlers append the
 * regions they hand the guest while it runs. Sizing it for either writer
 * alone leaves the other one refusing work the process legitimately asks for,
 * which is what a 6-module graph does to a 16-entry table. Both budgets are
 * gate constants, so the relationship is asserted here - the only place where
 * both headers are visible - instead of being left to arithmetic in a comment.
 */
_Static_assert(PW_X86_MEMORY_REGIONS >= PW_WINE_GATE_MAX_MODULES * 2u + 4u +
                                        PW_WINE_GATE_MAX_CALL_REGIONS,
               "region table must hold the module graph and the NT regions");

/*
 * The guard compares the declared count against the table bound with a 32-bit
 * immediate, so the bound can grow past the 127 a signed imm8 could carry -
 * which it had to, because a process with several mapped images declares one
 * region per section of each and passed 64 the moment this run mapped four.
 */
_Static_assert(PW_X86_MEMORY_REGIONS <= 4096,
               "the region table is a bounded array in the guest state");

static void copy_text(char *out, size_t out_bytes, const char *text)
{
    size_t length;

    if (!out || out_bytes == 0u)
        return;
    if (!text) {
        out[0] = '\0';
        return;
    }
    length = strlen(text);
    if (length >= out_bytes)
        length = out_bytes - 1u;
    memcpy(out, text, length);
    out[length] = '\0';
}

static PwWineModuleRecord *record_for(PwWineGateReport *report,
                                      const char *canonical)
{
    for (uint32_t index = 0; index < report->module_count; ++index)
        if (strcmp(report->modules[index].name, canonical) == 0)
            return &report->modules[index];
    return NULL;
}

/*
 * Fingerprints one module from exactly the namespace its origin names, so a
 * module that only exists in the other namespace is a refusal rather than a
 * silent fallback.
 */
static int hash_module(const PwFileProvider *provider, const char *name,
                       PwWineModuleRecord *record, PwFileNamespace space)
{
    PwFileSpan span;
    int status;

    memset(&span, 0, sizeof(span));
    status = provider->open_namespace(provider->context, space, name, &span);
    if (status != PW_OK)
        return status;
    pw_sha256_hex_span(span.bytes, span.size, record->sha256);
    record->origin_application = space == PW_FILE_APPLICATION ? 1u : 0u;
    record->size = (uint32_t)span.size;
    copy_text(record->path, sizeof(record->path), span.path);
    provider->close(provider->context, &span);
    return PW_OK;
}

/*
 * Executable source span for the DBT.
 *
 * The bytes a block is translated from have to be bytes of a mapped *image*,
 * and the span stops at the end of the executable section that holds the
 * program counter: a block must not run on into a data page that happens to
 * follow in the same mapping.
 *
 * Two inventories hold images here, and both are real. The module graph holds
 * the images this gate mapped before the run, which is where execution starts
 * (ntdll). The section views hold the images the *guest* mapped itself while
 * it ran, and execution reaches those as soon as fixed-up code in one image
 * calls into another: measured, a run that had mapped kernelbase through
 * NtMapViewOfSection stopped as non-code at kernelbase's own entry point
 * because this callback only knew about the first inventory.
 */
static int gate_source_view(void *opaque, uint32_t pc, const uint8_t **source,
                            size_t *bytes)
{
    const PwWineCallContext *calls = opaque;
    const PwLoader *loader = calls->loader;

    for (uint32_t index = 0; index < loader->module_count; ++index) {
        const PwModule *module = &loader->modules[index];

        if (!module->mapped_ok || pc < module->mapped.actual_base)
            continue;
        for (uint32_t section = 0; section < module->layout.section_count;
             ++section) {
            const PeLayoutSection *entry = &module->layout.sections[section];
            const uint64_t start = module->mapped.actual_base + entry->rva;
            const uint64_t end = start + entry->mapped_bytes;

            if ((entry->protection & PW_PROT_EXEC) == 0u)
                continue;
            if ((uint64_t)pc < start || (uint64_t)pc >= end)
                continue;
            *source = (const uint8_t *)(uintptr_t)pc;
            *bytes = (size_t)(end - pc);
            return PW_OK;
        }
    }
    for (uint32_t index = 0u; index < calls->section_count; ++index) {
        const PwWineSection *section = &calls->sections[index];
        const PeImage *image = &section->image;
        const uint64_t view_high = (uint64_t)section->view_base +
                                   section->view_bytes;

        if (section->view_base == 0u || (uint64_t)pc < section->view_base ||
            (uint64_t)pc >= view_high)
            continue;
        for (uint32_t entry = 0u; entry < image->section_count; ++entry) {
            const PeSection *mapped = &image->sections[entry];
            const uint64_t span = mapped->virtual_size > mapped->raw_size
                ? mapped->virtual_size : mapped->raw_size;
            const uint64_t start = section->view_base + mapped->virtual_address;
            const uint64_t end = start + span;

            if ((mapped->characteristics & PE_SCN_MEM_EXECUTE) == 0u)
                continue;
            if ((uint64_t)pc < start || (uint64_t)pc >= end)
                continue;
            *source = (const uint8_t *)(uintptr_t)pc;
            *bytes = (size_t)(end - pc);
            return PW_OK;
        }
    }
    return PW_ERR_NOT_FOUND;
}

/* "jmp dword ptr [disp32]" entries that reference the dispatcher slot. */
static uint32_t find_dispatcher_thunks(const PwModule *module, uint32_t slot_va,
                                       uint32_t *thunk_rvas, uint32_t capacity)
{
    uint8_t target[4];
    uint32_t found = 0u;

    target[0] = (uint8_t)(slot_va & 0xffu);
    target[1] = (uint8_t)((slot_va >> 8) & 0xffu);
    target[2] = (uint8_t)((slot_va >> 16) & 0xffu);
    target[3] = (uint8_t)((slot_va >> 24) & 0xffu);
    for (uint32_t index = 0; index < module->layout.section_count; ++index) {
        const PeLayoutSection *section = &module->layout.sections[index];
        const uint8_t *bytes;

        if ((section->protection & PW_PROT_EXEC) == 0u)
            continue;
        bytes = (const uint8_t *)(uintptr_t)(module->mapped.actual_base +
                                             section->rva);
        for (uint32_t offset = 0;
             offset + 6u <= section->mapped_bytes; ++offset) {
            if (bytes[offset] != 0xffu || bytes[offset + 1u] != 0x25u)
                continue;
            if (memcmp(bytes + offset + 2u, target, 4u) != 0)
                continue;
            if (found < capacity)
                thunk_rvas[found] = section->rva + offset;
            ++found;
        }
    }
    return found;
}

/* "mov eax, imm32" is how a Wine i386 PE syscall stub names its call. */
static uint32_t decode_stub_syscall(const PwModule *module, uint32_t rva)
{
    const uint8_t *bytes;

    if (rva > module->mapped.image_bytes ||
        module->mapped.image_bytes - rva < 5u)
        return 0u;
    bytes = (const uint8_t *)(uintptr_t)(module->mapped.actual_base + rva);
    if (bytes[0] != 0xb8u)
        return 0u;
    return (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8) |
           ((uint32_t)bytes[3] << 16) | ((uint32_t)bytes[4] << 24);
}

static PwWineStop classify(int status)
{
    switch (status) {
    case PW_ERR_UNSUPPORTED: return PW_WINE_STOP_UNSUPPORTED_INSTRUCTION;
    case PW_ERR_VM: return PW_WINE_STOP_MEMORY_BOUNDS;
    case PW_ERR_LIMIT: return PW_WINE_STOP_CACHE_LIMIT;
    case PW_ERR_NOT_FOUND: return PW_WINE_STOP_NON_CODE;
    case PW_ERR_X87_TRAP: return PW_WINE_STOP_X87_TRAP;
    default: return PW_WINE_STOP_DECODE_FAILURE;
    }
}

/*
 * Guest memory accessor handed to a Unix-call handler. It applies exactly
 * the rules the dispatcher's own guard applies, so a handler can never read
 * or write an address the DBT would have faulted on: address zero is never
 * valid, the range must lie inside the stack or a declared region, and a
 * write needs that region to be writable.
 */
static int gate_guest_access(void *context, uint32_t address, void *bytes,
                             uint32_t size, int write)
{
    const PwX86State *state = context;
    uint64_t end;

    if (!state || !bytes || size == 0u)
        return PW_ERR_PRECONDITION;
    end = (uint64_t)address + size;
    if (address == 0u || end > 0x100000000ull)
        return PW_ERR_MALFORMED;
    if (address >= state->stack_low && end <= state->stack_high) {
        if (write)
            memcpy((void *)(uintptr_t)address, bytes, size);
        else
            memcpy(bytes, (const void *)(uintptr_t)address, size);
        return PW_OK;
    }
    for (uint32_t index = 0; index < state->memory_count; ++index) {
        const PwX86Memory *region = &state->memory[index];
        const unsigned needed = write ? (PW_X86_READ | PW_X86_WRITE)
                                      : PW_X86_READ;

        if (region->high > 0x100000000ull)
            continue;
        if (address < region->low || end > region->high)
            continue;
        if ((region->permissions & needed) != needed)
            continue;
        if (write)
            memcpy((void *)(uintptr_t)address, bytes, size);
        else
            memcpy(bytes, (const void *)(uintptr_t)address, size);
        return PW_OK;
    }
    return PW_ERR_VM;
}

/*
 * True when a guest write of `size` bytes at `address` would pass the very
 * guard above. Handlers preflight every output span with this before they
 * change anything, so a failure cannot leave an allocation behind, hand back a
 * released region or account for memory the guest will never be told about:
 * the platform call either commits completely or not at all.
 */
static int guest_span_writable(const PwX86State *state, uint32_t address,
                               uint32_t size)
{
    uint64_t end;

    if (!state || size == 0u)
        return 0;
    end = (uint64_t)address + size;
    if (address == 0u || end > 0x100000000ull)
        return 0;
    if (address >= state->stack_low && end <= state->stack_high)
        return 1;
    for (uint32_t index = 0; index < state->memory_count; ++index) {
        const PwX86Memory *region = &state->memory[index];

        if (region->high > 0x100000000ull)
            continue;
        if (address < region->low || end > region->high)
            continue;
        if ((region->permissions & (PW_X86_READ | PW_X86_WRITE)) !=
            (PW_X86_READ | PW_X86_WRITE))
            continue;
        return 1;
    }
    return 0;
}

/*
 * State the NT call handlers need: the gate's low-address allocator, the
 * per-run accounting and the regions this run mapped on the guest's behalf
 * (released at cleanup).
 */
static int reserve_guest_block(PwWineCallContext *calls, uint32_t desired,
                               uint32_t bytes, PwVmRegion *region,
                               uint32_t *base)
{
    const PwVmBackend *backend = &calls->vm->base;
    const uint32_t page = (uint32_t)backend->page_bytes;
    const uint32_t aligned = (bytes + page - 1u) & ~(page - 1u);
    int status;

    if (aligned == 0u || (uint64_t)aligned > PW_WINE_GATE_DEFAULT_ALLOCATION_LIMIT)
        return PW_ERR_LIMIT;
    if (calls->region_count >= PW_WINE_GATE_MAX_CALL_REGIONS)
        return PW_ERR_LIMIT;
    if ((backend->capabilities & PW_VM_CAP_EXACT_ADDRESS) == 0u)
        return PW_ERR_UNSUPPORTED;
    if (desired == 0u) {
        uint32_t candidate = (calls->heap_cursor + page - 1u) & ~(page - 1u);

        for (; (uint64_t)candidate + aligned <= PW_WINE_GATE_HEAP_LIMIT;
             candidate += page) {
            status = backend->reserve_at(backend->context, candidate, aligned,
                                         page, region);
            if (status == PW_OK) {
                calls->heap_cursor = candidate + aligned;
                *base = candidate;
                return PW_OK;
            }
        }
        return PW_ERR_VM;
    }
    if ((desired & (page - 1u)) != 0u)
        return PW_ERR_MALFORMED;
    status = backend->reserve_at(backend->context, desired, aligned, page,
                                 region);
    if (status != PW_OK)
        return status;
    *base = desired;
    return PW_OK;
}

/* True when [base, base+bytes) lies entirely inside a region we mapped. */
static int region_covers(const PwWineCallContext *calls, uint32_t base,
                         uint32_t bytes)
{
    for (uint32_t index = 0; index < calls->region_count; ++index) {
        const PwWineCallContext *context = calls;
        const uint32_t low = (uint32_t)(uintptr_t)context->regions[index].exec_base;
        const uint64_t high = low + context->regions[index].bytes;

        if ((uint64_t)base >= low && (uint64_t)base + bytes <= high)
            return 1;
    }
    return 0;
}

/*
 * A returned region must stop being addressable by the guest: the block
 * leaves the dispatcher's declared ranges together with the mapping, so a
 * guest that keeps touching memory it gave back is refused by the same guard
 * that protects every other access.
 */
static void forget_declared_region(PwX86State *state, uint32_t base)
{
    for (uint32_t index = 0; index < state->memory_count; ++index) {
        if (state->memory[index].low != base)
            continue;
        state->memory[index] = state->memory[--state->memory_count];
        return;
    }
}

/*
 * NtAllocateVirtualMemory for the profile a first boot attempt reaches:
 * the current process, no zero bits, MEM_COMMIT|MEM_RESERVE and
 * PAGE_READWRITE. Value errors are answered with an NTSTATUS, because that
 * is what the guest's own contract expects; a guest pointer that does not
 * pass the accessor is a bridge refusal instead, and the argument index is
 * reported so the evidence names it.
 */
static int gate_nt_allocate_virtual_memory(PwWineCallContext *calls,
                                           const PwUnixCallFrame *frame,
                                           PwUnixCallAccess guest_access,
                                           void *access_context,
                                           uint32_t *status,
                                           uint32_t *argument_index)
{
    const uint32_t process_handle = frame->args[0];
    const uint32_t base_pointer = frame->args[1];
    const uint32_t zero_bits = frame->args[2];
    const uint32_t size_pointer = frame->args[3];
    const uint32_t allocation_type = frame->args[4];
    const uint32_t protect = frame->args[5];
    uint32_t base_value = 0u;
    uint32_t size_value = 0u;
    uint32_t base = 0u;
    PwVmRegion region;
    int result;

    if (process_handle != 0xffffffffu) {
        *status = PW_NT_INVALID_HANDLE;
        return PW_OK;
    }
    if (guest_access(access_context, base_pointer, &base_value, 4u, 0) != PW_OK) {
        *argument_index = 2u;
        return PW_ERR_MALFORMED;
    }
    if (guest_access(access_context, size_pointer, &size_value, 4u, 0) != PW_OK) {
        *argument_index = 4u;
        return PW_ERR_MALFORMED;
    }
    if (zero_bits != 0u) {
        *status = PW_NT_INVALID_PARAMETER;
        return PW_OK;
    }
    /*
     * MEM_RESERVE, MEM_COMMIT and their combination are all accepted; the
     * reserve/commit distinction is not yet modelled, because the dispatcher
     * only knows one kind of guest region. PAGE_READWRITE and the
     * reserve-only "no access yet" value are accepted for the same reason.
     */
    if ((allocation_type != 0x1000u && allocation_type != 0x2000u &&
         allocation_type != 0x3000u) ||
        (protect != 0u && protect != 0x04u) || size_value == 0u) {
        *status = PW_NT_INVALID_PARAMETER;
        return PW_OK;
    }
    if (calls->report->allocated_bytes > calls->limit ||
        size_value > calls->limit - calls->report->allocated_bytes) {
        *status = PW_NT_INVALID_PARAMETER;
        return PW_OK;
    }
    /*
     * Both outputs are written on the success path, so their spans are proved
     * writable before anything is reserved, committed or accounted. A handler
     * that failed after publishing a region would leave the guest owning memory
     * it was never told about, which is exactly the kind of half-mutation the
     * bridge must not have.
     */
    if (!guest_span_writable(access_context, size_pointer, 4u)) {
        *argument_index = 4u;
        return PW_ERR_MALFORMED;
    }
    if (!guest_span_writable(access_context, base_pointer, 4u)) {
        *argument_index = 2u;
        return PW_ERR_MALFORMED;
    }
    /*
     * MEM_COMMIT on a range this run already mapped is the normal second half
     * of "reserve then commit": it is idempotent, not a new reservation.
     */
    if ((allocation_type == 0x1000u || allocation_type == 0x3000u) &&
        base_value != 0u &&
        region_covers(calls, base_value, (size_value + 0xfffu) & ~0xfffu)) {
        uint32_t rounded = (size_value + 0xfffu) & ~0xfffu;

        if (guest_access(access_context, size_pointer, &rounded, 4u, 1) != PW_OK ||
            guest_access(access_context, base_pointer, &base_value, 4u, 1) != PW_OK) {
            *argument_index = 4u;
            return PW_ERR_MALFORMED;
        }
        *status = PW_NT_SUCCESS;
        return PW_OK;
    }
    memset(&region, 0, sizeof(region));
    result = reserve_guest_block(calls, base_value,
                                 (size_value + 0xfffu) & ~0xfffu, &region,
                                 &base);
    if (result == PW_ERR_LIMIT) {
        *status = PW_NT_INVALID_PARAMETER;
        return PW_OK;
    }
    if (result != PW_OK) {
        *status = PW_NT_CONFLICTING_ADDRESSES;
        return PW_OK;
    }
    result = calls->vm->base.commit(calls->vm->base.context, &region, 0u,
                                     region.bytes,
                                     PW_PROT_READ | PW_PROT_WRITE);
    if (result != PW_OK) {
        (void) calls->vm->base.release(calls->vm->base.context, &region);
        *status = PW_NT_INVALID_PARAMETER;
        return PW_OK;
    }
    /*
     * The guest must be able to address what it was just given, so the new
     * block joins the dispatcher's declared regions. Without this the very
     * next access to an NT allocation would be classified as out of bounds.
     */
    {
        PwX86State *state = access_context;

        if (state->memory_count >= PW_X86_MEMORY_REGIONS) {
            (void) calls->vm->base.release(calls->vm->base.context, &region);
            *status = PW_NT_INVALID_PARAMETER;
            return PW_OK;
        }
        state->memory[state->memory_count++] = (PwX86Memory){
            .low = base,
            .high = (uint64_t)base + region.bytes,
            .permissions = PW_X86_READ | PW_X86_WRITE,
        };
    }
    calls->regions[calls->region_count] = region;
    calls->region_owned[calls->region_count] = 1u;
    calls->region_count++;
    calls->report->allocations++;
    calls->report->allocated_bytes += (uint32_t)region.bytes;
    calls->report->call_regions = calls->region_count;
    {
        uint32_t written_size = (uint32_t)region.bytes;

        if (guest_access(access_context, size_pointer, &written_size, 4u, 1) != PW_OK ||
            guest_access(access_context, base_pointer, &base, 4u, 1) != PW_OK) {
            *argument_index = 4u;
            return PW_ERR_MALFORMED;
        }
    }
    *status = PW_NT_SUCCESS;
    return PW_OK;
}

/* The two Windows free types, and the shape this bridge can honour. */
enum {
    PW_WINE_MEM_DECOMMIT = 0x4000u,
    PW_WINE_MEM_RELEASE = 0x8000u,
};

/*
 * NtFreeVirtualMemory for the same profile. The bridge models one kind of
 * guest region - a block this run reserved and mapped - so what it can
 * honour is the release of a whole one, which is exactly what a loader's
 * cleanup path asks for. A partial release and a MEM_DECOMMIT are answered
 * with real NTSTATUS values instead of a wrong success, because the
 * reserve/commit distinction is not modelled here. A successful release
 * removes the block from the dispatcher's declared ranges and returns the
 * mapping to the backend, so the guest cannot keep using memory it gave
 * back, and the base and size are written back the way Wine's own
 * implementation does.
 */
static int gate_nt_free_virtual_memory(PwWineCallContext *calls,
                                       const PwUnixCallFrame *frame,
                                       PwUnixCallAccess guest_access,
                                       void *access_context,
                                       uint32_t *status,
                                       uint32_t *argument_index)
{
    const uint32_t process_handle = frame->args[0];
    const uint32_t base_pointer = frame->args[1];
    const uint32_t size_pointer = frame->args[2];
    const uint32_t free_type = frame->args[3];
    uint32_t base_value = 0u;
    uint32_t size_value = 0u;

    if (process_handle != 0xffffffffu) {
        *status = PW_NT_INVALID_HANDLE;
        return PW_OK;
    }
    if (guest_access(access_context, base_pointer, &base_value, 4u, 0) !=
        PW_OK) {
        *argument_index = 2u;
        return PW_ERR_MALFORMED;
    }
    if (guest_access(access_context, size_pointer, &size_value, 4u, 0) !=
        PW_OK) {
        *argument_index = 3u;
        return PW_ERR_MALFORMED;
    }
    if (free_type != PW_WINE_MEM_RELEASE) {
        *status = free_type == PW_WINE_MEM_DECOMMIT ? PW_NT_NOT_SUPPORTED
                                                    : PW_NT_INVALID_PARAMETER;
        return PW_OK;
    }
    if (base_value == 0u) {
        *status = PW_NT_INVALID_PARAMETER;
        return PW_OK;
    }
    /*
     * A release is irreversible for the backend, so both output spans are
     * proved writable first: the region is only given back once the guest can
     * be told the base and size it gave up.
     */
    if (!guest_span_writable(access_context, base_pointer, 4u)) {
        *argument_index = 2u;
        return PW_ERR_MALFORMED;
    }
    if (!guest_span_writable(access_context, size_pointer, 4u)) {
        *argument_index = 3u;
        return PW_ERR_MALFORMED;
    }
    for (uint32_t index = 0; index < calls->region_count; ++index) {
        PwVmRegion region = calls->regions[index];
        const uint32_t region_base = (uint32_t)(uintptr_t)region.exec_base;
        const uint32_t released_owned = calls->region_owned[index];
        uint32_t region_bytes = (uint32_t)region.bytes;
        uint32_t released_bytes;
        uint32_t written_base = region_base;

        if (region_base != base_value)
            continue;
        /* A size of zero means "the whole region"; anything shorter is a
         * partial release, which this single-kind allocator cannot split. */
        released_bytes = size_value == 0u
            ? region_bytes
            : (size_value + 0xfffu) & ~0xfffu;
        if (released_bytes < region_bytes) {
            *status = PW_NT_UNABLE_TO_FREE_VM;
            return PW_OK;
        }
        if (calls->vm->base.release(calls->vm->base.context, &region) !=
            PW_OK) {
            *status = PW_NT_INVALID_PARAMETER;
            return PW_OK;
        }
        calls->regions[index] = calls->regions[--calls->region_count];
        calls->region_owned[index] = calls->region_owned[calls->region_count];
        calls->report->call_regions = calls->region_count;
        forget_declared_region(access_context, region_base);
        /* Wine writes the released base and size back into the guest's own
         * variables; the size becomes the whole region. */
        region_bytes = released_bytes;
        if (guest_access(access_context, base_pointer, &written_base, 4u,
                         1) != PW_OK ||
            guest_access(access_context, size_pointer, &region_bytes, 4u,
                         1) != PW_OK) {
            *argument_index = 2u;
            return PW_ERR_MALFORMED;
        }
        calls->report->releases++;
        /* A registered page (the process-parameters block the gate plays the
         * parent for) was never counted against the guest's live bytes. */
        if (released_owned && calls->report->allocated_bytes >= region_bytes)
            calls->report->allocated_bytes -= region_bytes;
        *status = PW_NT_SUCCESS;
        return PW_OK;
    }
    *status = PW_NT_MEMORY_NOT_ALLOCATED;
    return PW_OK;
}

/*
 * NtQueryVirtualMemory, for the one class ntdll's own loader asks about.
 *
 * build_ntdll_module (dlls/ntdll/loader.c:2355-2372) queries the address of
 * LdrInitializeThunk and takes AllocationBase out of the answer, then builds
 * the ntdll module record from the guest's own mapped image. The question is
 * therefore "where does the module I am running inside actually live", asked
 * before any record of that module exists, and the answer has to come from
 * this run's own mappings: the module graph for an image (its base, its
 * extent, the protection of the section the queried page falls in and
 * MEM_IMAGE) and the declared regions for everything else the run owns (stack,
 * TEB, PEB, parameters page and every NT allocation, as MEM_PRIVATE).
 *
 * A page this run never mapped is refused rather than described as MEM_FREE.
 * Wine answers MEM_FREE for its own address space because the Unix loader owns
 * all of it; this run owns a bounded set of mappings and does not model the
 * rest, so claiming free space there would be an answer about memory nothing
 * here can back.
 */
enum {
    /* MEMORY_BASIC_INFORMATION32, as an i386 guest lays it out and reads it
     * back: BaseAddress, AllocationBase, AllocationProtect, RegionSize, State,
     * Protect, Type. */
    PW_WINE_MBI_BYTES = 28u,
    PW_WINE_MBI_BASE_ADDRESS = 0u,
    PW_WINE_MBI_ALLOCATION_BASE = 4u,
    PW_WINE_MBI_ALLOCATION_PROTECT = 8u,
    PW_WINE_MBI_REGION_SIZE = 12u,
    PW_WINE_MBI_STATE = 16u,
    PW_WINE_MBI_PROTECT = 20u,
    PW_WINE_MBI_TYPE = 24u,
    PW_WINE_MEMORY_BASIC_INFORMATION = 0u,
    PW_WINE_MEM_COMMIT = 0x1000u,
    PW_WINE_MEM_IMAGE = 0x1000000u,
    PW_WINE_MEM_PRIVATE = 0x20000u,
    PW_WINE_PAGE_READONLY = 0x02u,
    PW_WINE_PAGE_READWRITE = 0x04u,
    PW_WINE_PAGE_EXECUTE_READ = 0x20u,
    PW_WINE_PAGE_SIZE = 0x1000u,
};

static void put_le32(uint8_t *out, uint32_t offset, uint32_t value)
{
    out[offset + 0u] = (uint8_t)(value & 0xffu);
    out[offset + 1u] = (uint8_t)((value >> 8) & 0xffu);
    out[offset + 2u] = (uint8_t)((value >> 16) & 0xffu);
    out[offset + 3u] = (uint8_t)((value >> 24) & 0xffu);
}

/*
 * The module image containing the page, with the protection of the section the
 * page falls in. Wine maps an image in one step here, so the allocation
 * protection is the protection the loader installed on the page; the fields
 * are deliberately the same rather than a modelled reserve-then-protect pair.
 */
static const PwModule *gate_image_containing(const PwLoader *loader,
                                             uint32_t address,
                                             uint32_t *region_end,
                                             uint32_t *protect)
{
    for (uint32_t index = 0u; index < loader->module_count; ++index) {
        const PwModule *module = &loader->modules[index];
        const uint32_t base = (uint32_t)module->mapped.actual_base;
        const uint64_t high = (uint64_t)base + module->mapped.image_bytes;
        uint32_t found = PW_WINE_PAGE_READONLY;

        if (!module->mapped_ok || address < base || (uint64_t)address >= high)
            continue;
        for (uint32_t section = 0u; section < module->layout.section_count;
             ++section) {
            const PeLayoutSection *entry = &module->layout.sections[section];
            const uint64_t low = (uint64_t)base + entry->rva;

            if ((uint64_t)address < low ||
                (uint64_t)address >= low + entry->mapped_bytes)
                continue;
            if (entry->protection & PW_PROT_EXEC)
                found = PW_WINE_PAGE_EXECUTE_READ;
            else if (entry->protection & PW_PROT_WRITE)
                found = PW_WINE_PAGE_READWRITE;
            break;
        }
        *region_end = (uint32_t)high;
        *protect = found;
        return module;
    }
    return NULL;
}

static int gate_nt_query_virtual_memory(PwWineCallContext *calls,
                                        const PwUnixCallFrame *frame,
                                        PwUnixCallAccess guest_access,
                                        void *access_context, uint32_t *status,
                                        uint32_t *argument_index)
{
    const uint32_t process_handle = frame->args[0];
    const uint32_t address = frame->args[1];
    const uint32_t information_class = frame->args[2];
    const uint32_t buffer = frame->args[3];
    const uint32_t length = frame->args[4];
    const uint32_t result_pointer = frame->args[5];
    PwX86State *state = access_context;
    const PwModule *image;
    const uint32_t base = address & ~(PW_WINE_PAGE_SIZE - 1u);
    uint32_t region_end = 0u;
    uint32_t allocation_base = 0u;
    uint32_t protect = 0u;
    uint32_t type = 0u;
    uint32_t answered = 0u;
    uint32_t written = PW_WINE_MBI_BYTES;
    uint8_t answer[PW_WINE_MBI_BYTES];

    if (information_class != PW_WINE_MEMORY_BASIC_INFORMATION) {
        *status = PW_NT_INVALID_INFO_CLASS;
        return PW_OK;
    }
    if (process_handle != 0xffffffffu) {
        *status = PW_NT_INVALID_HANDLE;
        return PW_OK;
    }
    calls->report->virtual_queries++;
    /* Wine reports the length it needs instead of answering, and it does so
     * before it looks at the buffer. */
    if (length < PW_WINE_MBI_BYTES) {
        *status = PW_NT_INFO_LENGTH_MISMATCH;
        return PW_OK;
    }
    /*
     * Both output spans are proved before the answer is composed, so a query
     * whose buffer the guest cannot receive into leaves nothing half-written.
     */
    if (!guest_span_writable(access_context, buffer, PW_WINE_MBI_BYTES)) {
        *argument_index = 4u;
        return PW_ERR_MALFORMED;
    }
    if (result_pointer != 0u &&
        !guest_span_writable(access_context, result_pointer, 4u)) {
        *argument_index = 6u;
        return PW_ERR_MALFORMED;
    }
    image = gate_image_containing(calls->loader, base, &region_end, &protect);
    if (image) {
        allocation_base = (uint32_t)image->mapped.actual_base;
        type = PW_WINE_MEM_IMAGE;
    } else {
        for (uint32_t index = 0u; index < state->memory_count; ++index) {
            const PwX86Memory *region = &state->memory[index];

            if ((uint64_t)base < region->low ||
                (uint64_t)base >= region->high)
                continue;
            allocation_base = region->low;
            region_end = (uint32_t)region->high;
            protect = (region->permissions & PW_X86_WRITE)
                ? PW_WINE_PAGE_READWRITE : PW_WINE_PAGE_READONLY;
            type = PW_WINE_MEM_PRIVATE;
            answered = 1u;
            break;
        }
        if (!answered) {
            *status = PW_NT_INVALID_PARAMETER;
            return PW_OK;
        }
    }
    memset(answer, 0, sizeof(answer));
    put_le32(answer, PW_WINE_MBI_BASE_ADDRESS, base);
    put_le32(answer, PW_WINE_MBI_ALLOCATION_BASE, allocation_base);
    put_le32(answer, PW_WINE_MBI_ALLOCATION_PROTECT, protect);
    put_le32(answer, PW_WINE_MBI_REGION_SIZE, region_end - base);
    put_le32(answer, PW_WINE_MBI_STATE, PW_WINE_MEM_COMMIT);
    put_le32(answer, PW_WINE_MBI_PROTECT, protect);
    put_le32(answer, PW_WINE_MBI_TYPE, type);
    if (guest_access(access_context, buffer, answer, sizeof(answer), 1) !=
        PW_OK) {
        *argument_index = 4u;
        return PW_ERR_MALFORMED;
    }
    if (result_pointer != 0u &&
        guest_access(access_context, result_pointer, &written, 4u, 1) !=
            PW_OK) {
        *argument_index = 6u;
        return PW_ERR_MALFORMED;
    }
    *status = PW_NT_SUCCESS;
    return PW_OK;
}

/*
 * NtAreMappedFilesTheSame: the question ntdll's loader asks itself about two
 * addresses it already owns.
 *
 * find_existing_module (dlls/ntdll/loader.c:2798) walks the module list ntdll
 * built, compares the base of each record with the address of an image it has
 * just mapped - address against address - and keeps the record when this call
 * says they are the same file. Wine answers it from its own view table
 * (dlls/ntdll/unix/virtual.c:6897): an address that is not one of its views is
 * STATUS_INVALID_ADDRESS, a view that is not a file view is
 * STATUS_CONFLICTING_ADDRESSES, one view against itself is STATUS_SUCCESS, and
 * two views of different files are STATUS_NOT_SAME_DEVICE, which is what the
 * server's is_same_mapping decides (server/mapping.c:1779) by comparing the
 * file descriptors behind the views.
 *
 * This run has both of the same two universes, so it answers from both: the
 * module graph holds the images this gate mapped before the run (the root
 * module among them) and the sections hold the images the guest mapped itself
 * during it. Two addresses inside one view, or in two views of the same
 * canonical file in the same namespace, are the same file; anything else that
 * is mapped is a different one. A private region - the stack, the TEB, the PEB,
 * an allocation - is the case Wine answers with STATUS_CONFLICTING_ADDRESSES,
 * and an address this run never mapped is refused rather than described: the
 * gate does not model the rest of the address space, so it cannot say what
 * lives there.
 */
enum {
    GATE_VIEW_NONE = 0u,       /* nothing this run mapped backs the address */
    GATE_VIEW_IMAGE = 1u,      /* a mapped image: a section view or a module */
    GATE_VIEW_PRIVATE = 2u,    /* mapped, but not a file view */
};

typedef struct GateMappedView {
    unsigned kind;
    const char *name;          /* canonical name of the file, when there is one */
    PwFileNamespace file_namespace;
    unsigned is_module;        /* the identity indexes the module graph */
    unsigned identity;
} GateMappedView;

static void gate_address_view(const PwWineCallContext *calls,
                              PwX86State *state, uint32_t address,
                              GateMappedView *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = GATE_VIEW_NONE;
    /*
     * The sections first: they are the views the guest made during this run,
     * and their ranges are the ones this question is about.
     */
    for (uint32_t index = 0u; index < calls->section_count; ++index) {
        const PwWineSection *section = &calls->sections[index];
        const uint64_t high = (uint64_t)section->view_base +
                              (uint64_t)section->view_bytes;

        if (section->view_base == 0u || section->view_bytes == 0u ||
            address < section->view_base || (uint64_t)address >= high)
            continue;
        out->kind = GATE_VIEW_IMAGE;
        out->name = section->name;
        out->file_namespace = section->file_namespace;
        out->is_module = 0u;
        out->identity = index;
        return;
    }
    /* Then the images this gate mapped before the run started. */
    {
        uint32_t region_end = 0u;
        uint32_t protect = 0u;
        const PwModule *module = gate_image_containing(
            calls->loader, address, &region_end, &protect);

        if (module) {
            out->kind = GATE_VIEW_IMAGE;
            out->name = module->name;
            out->file_namespace = module->kind == PW_MODULE_RUNTIME
                ? PW_FILE_RUNTIME : PW_FILE_APPLICATION;
            out->is_module = 1u;
            out->identity = (unsigned)(module - calls->loader->modules);
            return;
        }
    }
    /* Everything else this run mapped is private memory, not a file view. */
    for (uint32_t index = 0u; index < state->memory_count; ++index) {
        const PwX86Memory *region = &state->memory[index];

        if ((uint64_t)address < region->low ||
            (uint64_t)address >= region->high)
            continue;
        out->kind = GATE_VIEW_PRIVATE;
        out->is_module = 0u;
        out->identity = index;
        return;
    }
}

static int gate_nt_are_mapped_files_the_same(PwWineCallContext *calls,
                                             const PwUnixCallFrame *frame,
                                             PwUnixCallAccess guest_access,
                                             void *access_context,
                                             uint32_t *status,
                                             uint32_t *argument_index)
{
    const uint32_t first = frame->args[0];
    const uint32_t second = frame->args[1];
    PwX86State *state = access_context;
    GateMappedView left;
    GateMappedView right;

    (void)guest_access;
    (void)argument_index;
    calls->report->address_comparisons++;
    gate_address_view(calls, state, first, &left);
    gate_address_view(calls, state, second, &right);
    if (left.kind == GATE_VIEW_NONE || right.kind == GATE_VIEW_NONE) {
        *status = PW_NT_INVALID_ADDRESS;
        calls->report->address_comparison_refusals++;
        return PW_OK;
    }
    if (left.kind == GATE_VIEW_PRIVATE || right.kind == GATE_VIEW_PRIVATE) {
        *status = PW_NT_CONFLICTING_ADDRESSES;
        calls->report->address_comparison_refusals++;
        return PW_OK;
    }
    if (left.is_module == right.is_module && left.identity == right.identity) {
        *status = PW_NT_SUCCESS;
        return PW_OK;
    }
    /*
     * Two different views are the same file when they name the same canonical
     * file in the same root: that pair is the identity this run can guarantee,
     * and the one every other answer in this gate is built on.
     */
    if (left.file_namespace == right.file_namespace && left.name != NULL &&
        right.name != NULL && strcmp(left.name, right.name) == 0) {
        *status = PW_NT_SUCCESS;
        return PW_OK;
    }
    *status = PW_NT_NOT_SAME_DEVICE;
    return PW_OK;
}

/*
 * The one place the serviced calls are listed. `classes` names the information
 * classes the handler answers (PW_NT_CLASS_NONE when the call has none) and
 * `test` names the self-contained test that owns it; both are what
 * tests/test_nt_handler_ledger.py checks, and it cross-checks every id against
 * the versioned table in src/pw_unix_call.c, so an entry can never name a call
 * the pinned Wine revision does not have.
 *
 * NtTerminateProcess is deliberately absent: it is a stop, not a service, and
 * the dispatcher handles it before it looks here.
 */
static const PwNtHandler dispatch_table[] = {
    { 0x0018u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_gate_bridge.c",
      gate_nt_allocate_virtual_memory },
    { 0x001eu, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_gate_bridge.c",
      gate_nt_free_virtual_memory },
    { 0x0033u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_file_service.c",
      pw_wine_file_open },
    { 0x0006u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_file_service.c",
      pw_wine_file_read },
    { 0x0011u, { 5u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_file_service.c",
      pw_wine_file_query_information },
    { 0x0049u, { 4u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_file_service.c",
      pw_wine_file_query_volume_information },
    { 0x000fu, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_file_service.c",
      pw_wine_handle_close },
    { 0x0036u, { 1000u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_registry.c",
      pw_wine_query_system_information },
    { 0x0012u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_registry.c",
      pw_wine_registry_open },
    { 0x00b6u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_registry.c",
      pw_wine_registry_open },
    { 0x001du, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_registry.c",
      pw_wine_registry_create },
    { 0x0017u, { 2u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_registry.c",
      pw_wine_registry_query_value },
    { 0x0060u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_registry.c",
      pw_wine_registry_set_value },
    { 0x0021u, { 1u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_registry.c",
      pw_wine_query_token },
    { 0x0058u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_objects.c",
      pw_wine_object_open_directory },
    { 0x0037u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_objects.c",
      pw_wine_object_open_section },
    { 0x0019u, { 0x25u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_process_info.c",
      pw_wine_query_process_image },
    { 0x0023u, { 0u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_virtual_memory.c",
      gate_nt_query_virtual_memory },
    { 0x0072u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_virtual_memory.c",
      gate_nt_are_mapped_files_the_same },
    { 0x00a0u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_thread.c",
      pw_wine_thread_next },
    { 0x0025u, { 0u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_thread.c",
      pw_wine_thread_query },
    { 0x0043u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_continue.c",
      pw_wine_thread_continue },
    { 0x0039u, { 0x0009009cu, PW_NT_CLASS_NONE },
      "tests/test_pw_wine_file_service.c", pw_wine_file_fs_control },
    { 0x004au, { 0x01000000u, PW_NT_CLASS_NONE },
      "tests/test_pw_wine_section.c", pw_wine_section_create },
    { 0x0051u, { 0u, 1u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_section.c",
      pw_wine_section_query },
    { 0x0028u, { 0x00000020u, PW_NT_CLASS_NONE },
      "tests/test_pw_wine_section.c", pw_wine_section_map_view },
    { 0x0050u, { 0x04u, PW_NT_CLASS_NONE }, "tests/test_pw_wine_section.c",
      pw_wine_section_protect },
    { 0x00a4u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_section.c",
      pw_wine_section_init_nls_files },
    { 0x0044u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_section.c",
      pw_wine_section_query_default_ui_language },
    { 0x0015u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_section.c",
      pw_wine_section_query_default_locale },
    { 0x00c7u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_section.c",
      pw_wine_section_query_install_ui_language },
    { 0x00a1u, { PW_NT_CLASS_NONE }, "tests/test_pw_wine_section.c",
      pw_wine_section_get_nls_section_ptr },
};

static const PwNtHandler *dispatch_find(uint32_t id)
{
    for (uint32_t index = 0u;
         index < sizeof(dispatch_table) / sizeof(dispatch_table[0]); ++index)
        if (dispatch_table[index].id == id)
            return &dispatch_table[index];
    return NULL;
}

/*
 * Writes into a mapped module's image at the RVA the export resolver reported.
 * The write goes to the image's writable alias, which is where the loader put
 * the section; this is the same publication Wine's own Unix loader performs on
 * these two data exports (dlls/ntdll/unix/loader.c:1594-1597), and it is why
 * the gate verifies they are data and not code before writing.
 */
static int module_write(const PwModule *module, uint32_t rva, const void *bytes,
                        uint32_t length)
{
    uint8_t *target;

    if (!module || !bytes || !module->mapped.region.write_base ||
        rva > module->mapped.image_bytes ||
        length > module->mapped.image_bytes - rva)
        return PW_ERR_MALFORMED;
    target = (uint8_t *)module->mapped.region.write_base + rva;
    memcpy(target, bytes, length);
    return PW_OK;
}

/* The same for the gate-owned guest pages, addressed by their guest address. */
static int guest_page_write(const PwGuestProcess *process, uint32_t address,
                            const void *bytes, uint32_t length)
{
    for (uint32_t index = 0u; index < (uint32_t)PW_GUEST_PROCESS_PAGES; ++index) {
        const uint64_t base =
            (uint64_t)(uintptr_t)process->pages[index].exec_base;

        if ((uint64_t)address < base ||
            (uint64_t)address + length > base + process->pages[index].bytes)
            continue;
        memcpy((uint8_t *)process->pages[index].write_base +
                   (address - (uint32_t)base), bytes, length);
        return PW_OK;
    }
    return PW_ERR_MALFORMED;
}

/*
 * The opaque handle published in __wine_unixlib_handle. Wine puts the host
 * address of unix_call_funcs[] there; a guest must never receive, or be able to
 * name, a host pointer, so this is a tag plus 24 bits derived from the entry
 * module's own SHA-256 - an identifier tied to the pinned runtime that the
 * gate can check without any table living in the guest address space.
 */
static uint32_t unixlib_handle_value(const PwWineModuleRecord *record)
{
    uint32_t derived = 0u;

    if (!record)
        return 0u;
    for (uint32_t index = 0u; index < 6u; ++index) {
        const char high = record->sha256[index * 2u];
        const char low = record->sha256[index * 2u + 1u];
        uint32_t nibble;

        nibble = (uint32_t)(high >= 'a' ? high - 'a' + 10 : high - '0');
        derived = (derived << 8) | nibble;
        nibble = (uint32_t)(low >= 'a' ? low - 'a' + 10 : low - '0');
        derived = (derived << 8) | nibble;
    }
    /* The tag keeps the value far from anything a guest would invent, and the
     * "or 1" makes it nonzero even for a pathological all-zero digest. */
    return 0x50000000u | (derived & 0x00ffffffu) | 1u;
}

/*
 * Services one call at the second boundary. The frame is the WINAPI/stdcall
 * frame the PE side leaves for
 * __wine_unix_call_dispatcher(handle, code, args); on return the callee has
 * popped the three arguments and the return PC, which is the sixteen bytes the
 * frame occupies.
 */
static PwWineStop service_unixlib_call(PwWineCallContext *calls,
                                       PwX86State *state,
                                       const PwWineGateConfig *config)
{
    PwWineGateReport *report = calls->report;
    PwWineUnixlibFrame frame;
    const PwUnixlibFunc *func;
    PwWineUnixlibStatus result = PW_WINE_UNIXLIB_UNIMPLEMENTED;
    uint32_t failed = 0u;
    uint32_t length = 0u;

    memset(&frame, 0, sizeof(frame));
    if (pw_wine_unixlib_read_frame(gate_guest_access, state, state->gpr[4],
                                   &frame, &failed) != PW_OK) {
        report->unixlib.malformed++;
        report->unixlib.last_status = PW_WINE_UNIXLIB_MALFORMED;
        return PW_WINE_STOP_UNIXLIB_REFUSED;
    }
    if (frame.handle != report->unixlib_handle || frame.handle_high != 0u) {
        report->unixlib.unknown_handle++;
        report->unixlib.last_status = PW_WINE_UNIXLIB_UNKNOWN_HANDLE;
        return PW_WINE_STOP_UNIXLIB_REFUSED;
    }
    func = pw_unixlib_lookup(frame.code);
    if (!func) {
        report->unixlib.unknown_code++;
        report->unixlib.last_status = PW_WINE_UNIXLIB_UNKNOWN_CODE;
        return PW_WINE_STOP_UNIXLIB_REFUSED;
    }
    report->unixlib.last_code = frame.code;
    report->unixlib.last_return_pc = frame.return_pc;
    report->unixlib.last_args = frame.args;
    switch (frame.code) {
    case PW_UNIXLIB_CODE_LOAD_SO_DLL:
        /*
         * Wine's CU side dlopen()s a Unix shared library. This project has no
         * dlopen and no host Wine libraries, so the honest answer is the one
         * Wine's own loader gives for a library it cannot find
         * (dlls/ntdll/unix/loader.c:862,987): STATUS_DLL_NOT_FOUND. The guest's
         * error path decides what happens next, and the record says this call
         * was answered with a documented failure rather than serviced.
         */
        report->unixlib.unsupported++;
        report->unixlib.last_status = PW_WINE_UNIXLIB_UNIMPLEMENTED;
        state->gpr[0] = PW_NT_DLL_NOT_FOUND;
        state->eip = frame.return_pc;
        state->gpr[4] += (uint32_t)PW_WINE_UNIXLIB_FRAME_BYTES;
        return PW_WINE_STOP_NONE;
    case PW_UNIXLIB_CODE_WINE_DBG_WRITE:
        result = pw_wine_unixlib_debug_write(gate_guest_access, state,
                                             frame.args, config->debug_sink,
                                             &length);
        if (result == PW_WINE_UNIXLIB_MALFORMED) {
            report->unixlib.malformed++;
            report->unixlib.last_status = result;
            return PW_WINE_STOP_UNIXLIB_REFUSED;
        }
        if (result != PW_WINE_UNIXLIB_OK) {
            report->unixlib.service_failed++;
            report->unixlib.last_status = result;
            return PW_WINE_STOP_UNIXLIB_REFUSED;
        }
        report->unixlib.debug_bytes += length;
        break;
    default:
        /* A pinned call this bridge does not serve is a versioned
         * unimplemented record, never a fabricated success. */
        report->unixlib.unimplemented++;
        report->unixlib.last_status = PW_WINE_UNIXLIB_UNIMPLEMENTED;
        return PW_WINE_STOP_UNIXLIB_UNIMPLEMENTED;
    }
    report->unixlib.serviced++;
    report->unixlib.last_status = result;
    /* Wine's CU side returns write(2,...) straight through as the "status":
     * the byte count on success, -1 on failure. */
    state->gpr[0] = result == PW_WINE_UNIXLIB_OK ? length : 0xffffffffu;
    state->eip = frame.return_pc;
    state->gpr[4] += (uint32_t)PW_WINE_UNIXLIB_FRAME_BYTES;
    return PW_WINE_STOP_NONE;
}

static PwWineStop service_unix_call(PwWineCallContext *calls, PwX86State *state,
                                    PwWineGateReport *report)
{
    const uint32_t id = state->gpr[0];
    const PwUnixCallInfo *info = pw_unix_call_lookup(id);
    PwUnixCallFrame frame;
    uint32_t argument_index = 0u;
    uint32_t status = PW_NT_NOT_IMPLEMENTED;
    int result;
    int terminate = 0;

    report->observed_syscall_id = id;
    report->stop_call_id = id;
    if (!info) {
        pw_unix_call_record(&report->calls, NULL, NULL, status, 0u,
                            PW_UNIX_CALL_UNKNOWN);
        return PW_WINE_STOP_UNIX_CALL_UNKNOWN;
    }
    result = pw_unix_call_read(info, gate_guest_access, state, state->gpr[4],
                               &frame, &argument_index);
    memcpy(report->stop_call_args, frame.args, sizeof(report->stop_call_args));
    if (result != PW_OK) {
        pw_unix_call_record(&report->calls, &frame, info, status,
                            argument_index, PW_UNIX_CALL_REJECTED);
        return PW_WINE_STOP_UNIX_CALL_REJECTED;
    }
    /* The switch below services what has a handler; a known number without
     * one falls through to the unimplemented stop, so the frame is still
     * understood and reported and the guest does not continue past it. */
    /*
     * NtTerminateProcess and NtTerminateThread are stops rather than services:
     * a process or its last thread that has ended does not keep executing, so
     * the run ends here instead of handing the guest a status it would never
     * see. This run models one thread, so terminating that thread is the
     * process ending - and that is how a program's own clean exit arrives:
     * the application's entry point returns, kernel32's BaseThreadInitThunk
     * passes the value to RtlExitUserThread, and that calls
     * NtTerminateThread( GetCurrentThread(), status ) and never returns
     * (dlls/ntdll/thread.c:764). The status the guest named is recorded, so
     * the evidence carries the application's own exit code.
     */
    if (info->id == 0x002cu || info->id == 0x0053u) {
        const uint32_t current = info->id == 0x002cu ? 0xffffffffu
                                                     : 0xfffffffeu;

        status = frame.args[0] == current ? PW_NT_SUCCESS
                                          : PW_NT_INVALID_HANDLE;
        if (status != PW_NT_SUCCESS) {
            result = PW_OK;
        } else {
            report->exit_call_id = info->id;
            report->exit_status = frame.args[1];
            pw_unix_call_record(&report->calls, &frame, info, status,
                                info->arg_bytes / 4u, PW_UNIX_CALL_HANDLED);
            report->calls_serviced++;
            return PW_WINE_STOP_PROCESS_TERMINATED;
        }
    } else {
        /* The registry is the list of serviced calls; a known number without
         * an entry falls through to the unimplemented stop, so the frame is
         * still understood and reported and the guest does not continue past
         * it. */
        const PwNtHandler *handler = dispatch_find(info->id);

        if (!handler) {
            result = PW_OK;
        } else {
            result = handler->handle(calls, &frame, gate_guest_access, state,
                                     &status, &argument_index);
        }
    }
    if (result != PW_OK) {
        pw_unix_call_record(&report->calls, &frame, info, status,
                            argument_index, PW_UNIX_CALL_REJECTED);
        return PW_WINE_STOP_UNIX_CALL_REJECTED;
    }
    if (status == PW_NT_NOT_IMPLEMENTED) {
        pw_unix_call_record(&report->calls, &frame, info, status, 0u,
                            PW_UNIX_CALL_UNIMPLEMENTED);
        return PW_WINE_STOP_UNIX_CALL_UNIMPLEMENTED;
    }
    pw_unix_call_record(&report->calls, &frame, info, status,
                        info->arg_bytes / 4u, PW_UNIX_CALL_HANDLED);
    if (terminate)
        return PW_WINE_STOP_PROCESS_TERMINATED;
    /*
     * A handler that installed the guest's whole CPU state (NtContinue) has
     * already named the instruction pointer and the stack the guest resumes
     * at, so the stub's own return must not be synthesized over them: the run
     * continues wherever the context said.
     */
    if (calls->state_installed) {
        calls->state_installed = 0u;
        report->calls_serviced++;
        return PW_WINE_STOP_NONE;
    }
    /* Return to the stub with its stdcall frame popped. */
    state->gpr[0] = status;
    /* [esp] is the stub's own return address and [esp+4] the caller's, so
     * resuming means dropping both plus the arguments the stub's ret would
     * have popped. */
    state->gpr[4] += 8u + info->arg_bytes;
    state->eip = frame.return_pc;
    report->calls_serviced++;
    return PW_WINE_STOP_NONE;
}

/*
 * Declares the guest memory the dispatcher may touch. The DBT's guard is
 * region-granular, so each module contributes its whole image as readable
 * plus the union of its writable sections as writable; a write into code is
 * then classified instead of relying on a native fault. Exact page
 * protection is still the mapper's installed protection, which the loader
 * already validated.
 */
static int declare_guest_memory(PwX86State *state, const PwLoader *loader,
                                uint32_t stack_base, uint32_t stack_bytes,
                                const PwVmRegion *thread_block,
                                const PwVmRegion *process_block,
                                const PwVmRegion *parameters_block)
{
    uint32_t used = 0u;

    if (loader->module_count * 2u + 4u > PW_X86_MEMORY_REGIONS)
        return PW_ERR_LIMIT;
    state->stack_low = stack_base;
    state->stack_high = stack_base + stack_bytes;
    state->memory[used++] = (PwX86Memory){
        .low = stack_base, .high = stack_base + stack_bytes,
        .permissions = PW_X86_READ | PW_X86_WRITE,
    };
    if (thread_block && thread_block->write_base) {
        /* The TEB the guest reaches through FS: its own block, writable. */
        state->memory[used++] = (PwX86Memory){
            .low = (uint32_t)(uintptr_t)thread_block->exec_base,
            .high = (uint32_t)((uintptr_t)thread_block->exec_base +
                               thread_block->bytes),
            .permissions = PW_X86_READ | PW_X86_WRITE,
        };
    }
    if (process_block && process_block->write_base) {
        /* The PEB ntdll reaches through TEB->ProcessEnvironmentBlock. */
        state->memory[used++] = (PwX86Memory){
            .low = (uint32_t)(uintptr_t)process_block->exec_base,
            .high = (uint32_t)((uintptr_t)process_block->exec_base +
                               process_block->bytes),
            .permissions = PW_X86_READ | PW_X86_WRITE,
        };
    }
    if (parameters_block && parameters_block->write_base) {
        /* The zeroed process-parameters page PEB->ProcessParameters points at. */
        state->memory[used++] = (PwX86Memory){
            .low = (uint32_t)(uintptr_t)parameters_block->exec_base,
            .high = (uint32_t)((uintptr_t)parameters_block->exec_base +
                               parameters_block->bytes),
            .permissions = PW_X86_READ | PW_X86_WRITE,
        };
    }
    for (uint32_t index = 0; index < loader->module_count; ++index) {
        const PwModule *module = &loader->modules[index];
        uint32_t writable_low = 0u;
        uint32_t writable_high = 0u;

        if (!module->mapped_ok)
            continue;
        state->memory[used++] = (PwX86Memory){
            .low = (uint32_t)module->mapped.actual_base,
            .high = (uint32_t)(module->mapped.actual_base +
                               module->mapped.image_bytes),
            .permissions = PW_X86_READ,
        };
        for (uint32_t section = 0; section < module->layout.section_count;
             ++section) {
            const PeLayoutSection *entry = &module->layout.sections[section];
            const uint32_t low = (uint32_t)module->mapped.actual_base +
                                 entry->rva;
            const uint32_t high = low + entry->mapped_bytes;

            if ((entry->protection & PW_PROT_WRITE) == 0u)
                continue;
            if (writable_low == 0u || low < writable_low)
                writable_low = low;
            if (high > writable_high)
                writable_high = high;
        }
        if (writable_low != 0u && writable_low < writable_high) {
            if (used == PW_X86_MEMORY_REGIONS)
                return PW_ERR_LIMIT;
            state->memory[used++] = (PwX86Memory){
                .low = writable_low, .high = writable_high,
                .permissions = PW_X86_READ | PW_X86_WRITE,
            };
        }
    }
    state->memory_count = used;
    return PW_OK;
}

int pw_wine_gate_run(const PwWineGateConfig *config, PwWineGateReport *report)
{

    PwWineRunner *runner;
    PwGuestVm guest_vm;
    PwX86Engine engine;
    PwImportBindReport bind;
    PeExportDirectory directory;
    PeExportSymbol symbol;
    PwGuestCall call;
    PwFileSpan root_span;
    PwGuestProcess process;
    PwWineCallContext calls;
    PwX86State state;
    uint32_t thunks[PW_WINE_GATE_MAX_BOUNDARIES];
    const PwModule *entry_module;
    char root_canonical[PW_MODULE_NAME_MAX + 1];
    uint32_t budget;
    int status;
    int cleanup_status = PW_OK;
    int have_loader = 0;
    int have_engine = 0;
    int have_process = 0;
    /* The module that plays the process image: the boundary setup finds it and
     * the second-dispatcher publication needs it later, outside that block. */
    int entry_index = -1;
    const char *stage = "start";
    (void)stage;

    if (!config || !report || !config->provider || !config->backend ||
        config->module_count == 0u ||
        config->module_count > PW_WINE_GATE_MAX_MODULES)
        return PW_ERR_PRECONDITION;
    /*
     * The run's workspace is the caller's object now: a run with none, or with
     * one that was released, is refused rather than silently sharing state with
     * another run in the same process.
     */
    if (!config->runner || !pw_wine_runner_ready(config->runner))
        return PW_ERR_PRECONDITION;
    runner = config->runner;
    memset(report, 0, sizeof(*report));
    memset(&root_span, 0, sizeof(root_span));
    memset(&process, 0, sizeof(process));
    memset(&calls, 0, sizeof(calls));
    memset(&call, 0, sizeof(call));
    budget = config->step_budget != 0u ? config->step_budget
                                       : PW_WINE_GATE_DEFAULT_STEPS;
    if (budget > PW_WINE_GATE_MAX_STEPS)
        return PW_ERR_PRECONDITION;
    report->stop = PW_WINE_STOP_GATE_ERROR;
    pw_guest_vm_init(&guest_vm, config->backend);

    /* Fingerprint every configured module before mapping anything. */
    for (uint32_t index = 0; index < config->module_count; ++index) {
        PwWineModuleRecord *record = &report->modules[report->module_count];

        if (pw_module_name_canonical(record->name, sizeof(record->name),
                                     config->modules[index]) != PW_OK)
            return PW_ERR_PRECONDITION;
        report->module_count++;
        {
            /* record 0 is the configured root; the rest are dependencies whose
             * origin the loader policy decides when it opens them. */
            const PwFileNamespace space =
                (report->module_count == 1u && config->root_application)
                    ? PW_FILE_APPLICATION
                    : (pw_module_is_system(record->name) ? PW_FILE_RUNTIME
                                                         : PW_FILE_APPLICATION);

            if (hash_module(config->provider, record->name, record, space) !=
                PW_OK)
                return PW_ERR_NOT_FOUND;
        }
    }

    stage = "loader-init";
    status = pw_loader_init(&runner->loader, config->provider, pw_guest_vm_backend(&guest_vm));
    if (status != PW_OK)
        return status;
    have_loader = 1;
    stage = "policy";
    status = pw_loader_set_policy(&runner->loader, &(PwModulePolicy){
        .context = NULL, .classify = pw_loader_wine_policy,
    });
    if (status != PW_OK)
        goto done;
    {
        const char *root = config->root_module ? config->root_module
                                               : "kernelbase.dll";

    stage = "root-open";
        status = pw_module_name_canonical(root_canonical,
                                          sizeof(root_canonical), root);
        if (status != PW_OK)
            goto done;
        status = config->provider->open_namespace(
            config->provider->context,
            config->root_application ? PW_FILE_APPLICATION : PW_FILE_RUNTIME,
            root_canonical, &root_span);
        if (status != PW_OK)
            goto done;
        status = pw_loader_load(&runner->loader, root_span.bytes, root_span.size,
                                root_canonical);
        if (status != PW_OK)
            goto done;
    }
    /*
     * The process's own system modules. A Wine process always has ntdll, and
     * the module that carries the boundary must be in the graph because the
     * process needs it - not because the application happens to import it. An
     * application root therefore gets the configured system modules loaded
     * explicitly, from the runtime namespace only, in configuration order and
     * each one once; a module already pulled in as a dependency is left as it
     * is, which is what keeps the Wine-runtime control identical.
     */
    stage = "system-modules";
    for (uint32_t index = 0u; index < config->module_count; ++index) {
        char system_canonical[PW_MODULE_NAME_MAX + 1];
        PwFileSpan span;

        if (index == 0u)
            continue;                        /* the root was loaded above */
        status = pw_module_name_canonical(system_canonical,
                                          sizeof(system_canonical),
                                          config->modules[index]);
        if (status != PW_OK)
            goto done;
        if (!pw_module_is_system(system_canonical))
            continue;                        /* a local module is an import */
        if (pw_loader_find(&runner->loader, system_canonical) >= 0)
            continue;                        /* already in the graph */
        memset(&span, 0, sizeof(span));
        status = config->provider->open_namespace(config->provider->context,
                                                  PW_FILE_RUNTIME,
                                                  system_canonical, &span);
        if (status != PW_OK)
            goto done;
        status = pw_loader_load(&runner->loader, span.bytes, span.size,
                                system_canonical);
        config->provider->close(config->provider->context, &span);
        if (status != PW_OK)
            goto done;
    }

    /* Bind every loaded module's imports through one resolver. */
    stage = "resolver";
    status = pw_export_resolver_init(&runner->resolver, &runner->loader, 4u);
    if (status != PW_OK)
        goto done;
    for (uint32_t index = 0; index < runner->loader.module_count; ++index) {
        PwModule *module = &runner->loader.modules[index];
        PwWineModuleRecord *record = record_for(report, module->name);

        if (record) {
            record->loaded = 1u;
            /* The gate opens every module through PW_FILE_RUNTIME; the root
             * is registered as PW_MODULE_ROOT by the loader, not as a
             * dependency, so its origin is recorded from here. */
            record->runtime =
                (uint8_t)(module->kind == PW_MODULE_RUNTIME ||
                          strcmp(module->name, root_canonical) == 0);
            record->machine = module->machine;
            record->base = (uint32_t)module->mapped.actual_base;
            record->image_bytes = module->mapped.image_bytes;
            record->imports = module->import_symbols;
        }
        if (module->import_symbols == 0u)
            continue;
        if (module->image.machine != PE_MACHINE_I386)
            continue;
    stage = "bind";
        status = pw_import_bind32(&module->image, &module->mapped,
                                  pw_export_import_resolver, &runner->resolver,
                                  &runner->workspace, &bind);
        if (status != PW_OK) {
            report->bind_failures++;
            goto done;
        }
        report->bound_functions += bind.functions;
        report->bound_data += bind.data;
        report->bound_modules++;
        if (record) {
            record->exports_named = 0u;
            record->exports_ordinal = 0u;
        }
    }

    /* TLS is a per-module loader contract; record what the real modules
     * declare instead of assuming every module has a directory. */
    for (uint32_t index = 0; index < runner->loader.module_count; ++index) {
        const PwModule *module = &runner->loader.modules[index];
        PeTlsDirectory tls;
        PwWineModuleRecord *record = record_for(report, module->name);

        if (pe_tls_parse(&tls, &module->image) != PW_OK)
            goto done;
        stage = "tls";
        if (tls.directory_rva != 0u) {
            report->tls_modules++;
            if (record)
                record->tls_present = 1u;
        }
    }

    stage = "boundary";
    /* The Unix-call boundary: exported dispatcher slot, then its thunk. */
    {
        const char *dispatcher = config->dispatcher_symbol
            ? config->dispatcher_symbol : "__wine_syscall_dispatcher";
        const char *entry_name = config->entry_symbol
            ? config->entry_symbol : "NtClose";
        const char *entry_machine = config->entry_module
            ? config->entry_module : "ntdll.dll";
        char canonical[PW_MODULE_NAME_MAX + 1];

        status = pw_module_name_canonical(canonical, sizeof(canonical),
                                          entry_machine);
        if (status != PW_OK)
            goto done;
        entry_index = pw_loader_find(&runner->loader, canonical);
        if (entry_index < 0) {
            status = entry_index;
            goto done;
        }
        entry_module = pw_loader_module(&runner->loader, (uint32_t)entry_index);
        status = pe_export_parse(&directory, &entry_module->image);
        if (status != PW_OK)
            goto done;
        status = pe_export_find_name(&entry_module->image, &directory,
                                     dispatcher, &symbol);
        if (status != PW_OK)
            goto done;
        report->boundary_slot_rva = symbol.rva;
        report->boundary_slot_va =
            (uint32_t)pw_map_exec_address(&entry_module->mapped, symbol.rva);
        report->boundary_count =
            find_dispatcher_thunks(entry_module, report->boundary_slot_va,
                                   thunks, PW_WINE_GATE_MAX_BOUNDARIES);
        if (report->boundary_count == 0u) {
            status = PW_ERR_NOT_FOUND;
            goto done;
        }
        report->boundary_thunk_rva = thunks[0];
        report->boundary_thunk_va =
            (uint32_t)entry_module->mapped.actual_base + thunks[0];
        report->entry_pe_rva = entry_module->mapped.entry_point;

        status = pe_export_find_name(&entry_module->image, &directory,
                                     entry_name, &symbol);
        if (status != PW_OK)
            goto done;
        if (symbol.is_forwarder || symbol.is_code == 0u) {
            status = PW_ERR_UNSUPPORTED;
            goto done;
        }
        report->entry_rva = symbol.rva;
        report->entry_eip =
            (uint32_t)pw_map_exec_address(&entry_module->mapped, symbol.rva);
        report->stub_syscall_id =
            decode_stub_syscall(entry_module, symbol.rva);
        /*
         * ntdll's own thread entry, which is what the kernel puts in the first
         * thread's context as its Eip (dlls/ntdll/unix/signal_i386.c:2474) and
         * what NtContinue therefore installs. A module that does not export it
         * - a synthetic fixture whose entry is a function of its own - leaves
         * both fields zero, so the context carries that zero rather than an
         * invented address. Not finding it is not fatal: a run that reaches
         * NtContinue is a run whose ntdll exports the syscall dispatcher.
         */
        if (pe_export_find_name(&entry_module->image, &directory,
                                "RtlUserThreadStart", &symbol) == PW_OK &&
            symbol.is_forwarder == 0u && symbol.is_code != 0u) {
            report->thread_start_rva = symbol.rva;
            report->thread_start_eip =
                (uint32_t)pw_map_exec_address(&entry_module->mapped,
                                              symbol.rva);
        }
    }

    /*
     * The guest process state: stack, TEB, PEB and process parameters. The
     * unit owns the four pages, so a failure halfway releases exactly what it
     * mapped.
     */
    {
        const PwModule *root_module_loaded = pw_loader_module(&runner->loader, 0u);
        const PwGuestProcessConfig process_config = {
            .backend = config->backend,
            .stack_base = config->stack_base,
            /* The stack the process's own image asks for, when its headers ask
             * for one: the same field Windows and Wine size a process's stack
             * from (SizeOfStackReserve). */
            .stack_reserve = root_module_loaded
                ? root_module_loaded->image.stack_reserve : 0u,
            .image_base = root_module_loaded
                ? (uint32_t)root_module_loaded->mapped.actual_base : 0u,
            .dispatcher_thunk = report->boundary_thunk_va,
            .root_module = root_canonical,
            .root_application = config->root_application,
        };

    stage = "process";
        status = pw_guest_process_create(&process, &process_config);
        if (status != PW_OK)
            goto done;
        have_process = 1;
        report->stack_base = process.layout.stack_base;
        report->stack_bytes = process.layout.stack_bytes;
        /* The root image's own transfer address: the routine the first thread
         * is started with (dlls/ntdll/unix/server.c:1780). */
        report->main_entry_eip = root_module_loaded
            ? (uint32_t)root_module_loaded->mapped.actual_base +
              root_module_loaded->image.entry_point
            : 0u;
        report->teb_base = process.layout.teb_base;
        report->teb_bytes = process.layout.teb_bytes;
        /*
         * The one thread this run models: the one it is executing on, running
         * on the TEB the guest process unit just published. The loader walks
         * this list while it gives every module with a TLS directory its slot
         * (alloc_tls_slot, dlls/ntdll/loader.c:1331), so the run has to be
         * able to name at least the thread it is running on.
         */
        memset(calls.threads, 0, sizeof(calls.threads));
        calls.threads[0].id = PW_GUEST_THREAD_ID;
        calls.threads[0].teb_base = process.layout.teb_base;
        calls.threads[0].mine = 1u;
        calls.thread_count = 1u;
        report->peb_base = process.layout.peb_base;
        report->parameters_base = process.layout.parameters_base;
        report->parameters_length = process.layout.parameters_length;
        /*
         * The parameters block stands in for the memory a parent hands a new
         * process, and ntdll's init_user_process_params replaces it with its
         * own copy and releases the original with NtFreeVirtualMemory. The
         * block is therefore registered as one of this run's guest regions
         * (owned = 0, because the guest never allocated it and it is not
         * counted against its live bytes), so that release finds it instead
         * of being told the address was never allocated.
         */
        if (calls.region_count >= PW_WINE_GATE_MAX_CALL_REGIONS) {
            status = PW_ERR_LIMIT;
            goto done;
        }
        calls.regions[calls.region_count] = process.pages[3];
        calls.region_owned[calls.region_count] = 0u;
        calls.region_count++;

        /*
         * The second dispatcher. Wine's Unix loader publishes the handle and
         * the dispatcher through the two exported data slots; here the handle
         * is an opaque identifier bound to this runtime's identity and the
         * boundary is a gate-owned guest address, so the guest can call
         * through the slot without ever naming host code. Nothing is published
         * unless the run asks for it, which is what keeps the NT-syscall
         * control byte-for-byte identical.
         */
        if (config->unixlib_calls) {
            const PwModule *entry_module_now =
                entry_index >= 0 ? pw_loader_module(&runner->loader,
                                                    (uint32_t)entry_index)
                                 : NULL;
            PeExportSymbol dispatcher = { 0 }, handle = { 0 };
            PeExportDirectory exports;
            uint8_t trap[2] = { 0x0fu, 0x0bu };      /* ud2 */
            const uint32_t boundary =
                process.layout.teb_base +
                (uint32_t)PW_WINE_UNIXLIB_BOUNDARY_OFFSET;
            uint32_t published;

            if (!entry_module_now) {
                status = PW_ERR_NOT_FOUND;
                goto done;
            }
            status = pe_export_parse(&exports, &entry_module_now->image);
            if (status != PW_OK)
                goto done;
            status = pe_export_find_name(&entry_module_now->image, &exports,
                                         "__wine_unix_call_dispatcher",
                                         &dispatcher);
            if (status != PW_OK)
                goto done;
            status = pe_export_find_name(&entry_module_now->image, &exports,
                                         "__wine_unixlib_handle", &handle);
            if (status != PW_OK)
                goto done;
            /* Both are data exports: a code export would mean the layout this
             * publication assumes is not the one in the pinned image. */
            if (dispatcher.is_code || handle.is_code || dispatcher.is_forwarder ||
                handle.is_forwarder) {
                status = PW_ERR_UNSUPPORTED;
                goto done;
            }
            report->unixlib_dispatcher_slot_va =
                (uint32_t)pw_map_exec_address(&entry_module_now->mapped,
                                              dispatcher.rva);
            report->unixlib_handle_slot_va =
                (uint32_t)pw_map_exec_address(&entry_module_now->mapped,
                                              handle.rva);
            report->unixlib_boundary_va = boundary;
            report->unixlib_handle =
                unixlib_handle_value(&report->modules[0]);
            published = report->unixlib_boundary_va;
            status = module_write(entry_module_now, dispatcher.rva, &published,
                                  4u);
            if (status != PW_OK)
                goto done;
            {
                const uint32_t pair[2] = { report->unixlib_handle, 0u };

                /* The handle is 64-bit in the pinned ABI, and the PE call site
                 * loads both halves: publish the low half and a zero high
                 * half, exactly as Wine's Unix loader writes the pointer it
                 * uses. */
                status = module_write(entry_module_now, handle.rva, pair,
                                      (uint32_t)PW_WINE_UNIXLIB_HANDLE_BYTES);
            }
            if (status != PW_OK)
                goto done;
            status = guest_page_write(&process, boundary, trap, sizeof(trap));
            if (status != PW_OK)
                goto done;
        }
        report->call_regions = calls.region_count;
    }

    memset(&state, 0, sizeof(state));
    state.gpr[4] = (report->stack_base + report->stack_bytes - 16u) & ~15u;
    state.eip = report->entry_eip;
    state.fs_base = report->teb_base;
    state.fs_bytes = report->teb_bytes;
    stage = "memory";
    status = declare_guest_memory(&state, &runner->loader, report->stack_base,
                                  report->stack_bytes, &process.pages[1],
                                  &process.pages[2], &process.pages[3]);
    if (status != PW_OK)
        goto done;
    report->guest_regions = state.memory_count;
    {
        const uint32_t token = 0u;

        memcpy((void *)(uintptr_t)state.gpr[4], &token, 4u);
    }

    report->files_configured = config->files != NULL;
    report->registry_configured = config->registry != NULL;
    report->objects_configured = config->objects != NULL;
    calls.vm = &guest_vm;
    calls.config = config;
    calls.report = report;
    calls.root = pw_loader_module(&runner->loader, 0u);
    calls.loader = &runner->loader;
    /*
     * The engine's source view is the run context, because the bytes a block
     * can be translated from are bytes of a mapped image - and there are two
     * inventories of those: the module graph this gate mapped before the run
     * and the section views the guest maps during it.
     */
    stage = "engine";
    status = pw_x86_engine_init(&engine, pw_guest_vm_backend(&guest_vm), runner->cache,
                                PW_WINE_GATE_CACHE_ENTRIES,
                                PW_WINE_GATE_ARENA_BYTES, 1u,
                                gate_source_view, &calls);
    if (status != PW_OK)
        goto done;
    have_engine = 1;
    calls.heap_cursor = PW_WINE_GATE_HEAP_BASE;
    calls.limit = config->allocation_limit != 0u ? config->allocation_limit
                                                 : PW_WINE_GATE_DEFAULT_ALLOCATION_LIMIT;
    /* Mode toggles exist so the same real code can be run with chaining,
     * register residency and lazy flags on and off and compared. */
    if (config->modes_set) {
        status = pw_x86_engine_set_chaining(&engine, config->chaining);
        if (status == PW_OK)
            status = pw_x86_engine_set_residency(&engine, config->residency);
        if (status == PW_OK)
            status = pw_x86_engine_set_lazy_flags(&engine, config->lazy_flags);
        if (status != PW_OK)
            goto done;
    }
    report->chaining = engine.chaining_enabled;
    report->residency = engine.residency_enabled;
    report->lazy_flags = engine.lazy_flags_enabled;
    status = pw_guest_call_begin(&call, &state, PW_GUEST_STDCALL, 4u, 0);
    if (status != PW_OK)
        goto done;
    /*
     * The first thread's frame, exactly as the kernel builds it before ntdll
     * runs (dlls/ntdll/unix/signal_i386.c:2455-2506):
     *
     *   Esp      = StackBase - 16
     *   context  = Esp - sizeof(CONTEXT), ContextFlags = CONTEXT_FULL plus the
     *              floating-point and extended groups, Eip = ntdll's own
     *              RtlUserThreadStart, Eax = the thread's start routine (null
     *              for the first thread, which is what makes loader_init
     *              publish the process image's own entry point through it,
     *              dlls/ntdll/loader.c:4527), Ebx = the thread's argument
     *              (null here), Eflags = 0x202 and the x87/MXCSR control words
     *   below it the call frame LdrInitializeThunk is entered with: a return
     *              address, the context as its first argument and three zero
     *              argument slots
     *
     * LdrInitializeThunk hands that context to loader_init and then to
     * signal_start_thread, which clears the 0xf000 bytes of stack *below* it
     * and enters the thread through NtContinue (dlls/ntdll/signal_i386.c:514).
     * Measured, this run passed the PEB as that argument instead: the loader
     * wrote the image's entry point into the middle of the PEB, and the clear
     * ran down from 0x0d000000 into memory nothing had mapped - a bounds fault
     * at the last instructions of ntdll's own start-up.
     */
    {
        uint8_t context[PW_WINE_CONTEXT_BYTES];
        uint32_t value;
        const uint32_t stack_high = report->stack_base + report->stack_bytes;
        const uint32_t context_va =
            (stack_high - 16u - PW_WINE_CONTEXT_BYTES) & ~3u;

        if (context_va < report->stack_base ||
            context_va - 20u < report->stack_base) {
            stage = "context";
            status = PW_ERR_LIMIT;
            goto done;
        }
        memset(context, 0, sizeof(context));
        put_le32(context, PW_WINE_CONTEXT_OFFSET_FLAGS,
                 PW_WINE_CONTEXT_FLAGS_INITIAL);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_SEG_CS,
                 PW_WINE_CONTEXT_USER_CS);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_SEG_SS,
                 PW_WINE_CONTEXT_USER_SS);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_EFLAGS,
                 PW_WINE_CONTEXT_EFLAGS);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_ESP, stack_high - 16u);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_EIP,
                 report->thread_start_eip);
        /*
         * The thread's start routine and its argument, which are not null for
         * the first thread of a process: the unix side starts it with
         * signal_start_thread( main_image_info.TransferAddress, peb, teb )
         * (dlls/ntdll/unix/server.c:1780), so the process image's own entry
         * point is the routine and the PEB is the argument the image's entry
         * is called with. loader_init only fills this in itself for an
         * IL-only image (dlls/ntdll/loader.c:4527), which is why the kernel
         * side has to say it.
         */
        put_le32(context, PW_WINE_CONTEXT_OFFSET_EAX, report->main_entry_eip);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_EBX, report->peb_base);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_FLOAT_CONTROL, 0x27fu);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_EXTENDED_CONTROL, 0x27fu);
        put_le32(context, PW_WINE_CONTEXT_OFFSET_EXTENDED_MXCSR, 0x1f80u);
        memcpy((void *)(uintptr_t)context_va, context, sizeof(context));
        /*
         * The five dwords below the context: three zero argument slots, the
         * context itself as the first argument, and the address the
         * initialization entry was entered from. That last one is the gate's
         * own caller address - zero, the address this run called the entry
         * from - rather than Wine's poison value (0xdeadbabe), because it is
         * the sentinel this run's loop already recognizes as "the entry
         * returned to its caller" and the real path never reads it
         * (LdrInitializeThunk does not return).
         */
        value = 0u;
        memcpy((void *)(uintptr_t)(context_va - 4u), &value, 4u);
        memcpy((void *)(uintptr_t)(context_va - 8u), &value, 4u);
        memcpy((void *)(uintptr_t)(context_va - 12u), &value, 4u);
        memcpy((void *)(uintptr_t)(context_va - 16u), &context_va, 4u);
        memcpy((void *)(uintptr_t)(context_va - 20u), &value, 4u);
        state.gpr[4] = context_va - 20u;
    }

    report->first_eip = state.eip;
    report->stop = PW_WINE_STOP_STEP_BUDGET;
    stage = "run";
    for (uint32_t step = 0; step < budget; ++step) {
        PwX86StepReport progress;

        if (config->trace)
            config->trace(config->trace_context, &state);
        if (report->unixlib_boundary_va != 0u &&
            state.eip == report->unixlib_boundary_va) {
            /*
             * The second boundary. It is checked before the engine steps, so
             * the boundary address is never executed: the frame on the guest
             * stack is read through the validated accessor and the call is
             * serviced or refused, never guessed.
             */
            const uint32_t budget = config->call_budget != 0u
                ? config->call_budget : PW_WINE_GATE_DEFAULT_CALLS;
            PwWineStop serviced;

            report->stop_address = state.eip;
            if (report->unixlib.serviced >= budget) {
                report->stop = PW_WINE_STOP_STEP_BUDGET;
                break;
            }
            serviced = service_unixlib_call(&calls, &state, config);
            if (serviced == PW_WINE_STOP_NONE)
                continue;
            report->stop = serviced;
            report->stop_address = state.eip;
            break;
        }
        if (state.eip == report->boundary_thunk_va) {
            /* Stopped before executing the dispatcher jump: no unvalidated
             * guest pointer is dereferenced to reach it. */
            report->observed_syscall_id = state.gpr[0];
            if (config->bridge_calls) {
                const uint32_t budget = config->call_budget != 0u
                    ? config->call_budget : PW_WINE_GATE_DEFAULT_CALLS;
                PwWineStop serviced;

                report->stop_address = state.eip;
                if (report->calls_serviced >= budget) {
                    report->stop = PW_WINE_STOP_STEP_BUDGET;
                    break;
                }
                serviced = service_unix_call(&calls, &state, report);
                if (serviced == PW_WINE_STOP_NONE)
                    continue;
                report->stop = serviced;
                report->stop_address = state.eip;
                break;
            }
            report->stop = PW_WINE_STOP_UNIX_CALL_BOUNDARY;
            report->stop_address = state.eip;
            break;
        }
        if (state.eip == 0u || state.eip == call.return_pc) {
            report->stop = PW_WINE_STOP_RETURNED_TO_CALLER;
            report->stop_address = state.eip;
            break;
        }
        {
            /* Engine provenance: the ring keeps the last sixteen dispatched
             * PCs so a stop can name the code around it. */
            if (report->recent_count < 16u)
                report->recent_pcs[report->recent_count++] = state.eip;
            else {
                memmove(report->recent_pcs, report->recent_pcs + 1,
                        15u * sizeof(report->recent_pcs[0]));
                report->recent_pcs[15] = state.eip;
            }
        }
        status = pw_x86_engine_step(&engine, &state, &progress);
        report->dispatches++;
        report->retired += progress.retired;
        if (status != PW_OK) {
            report->stop = classify(status);
            report->stop_address = state.eip;
            break;
        }
    }
    report->last_eip = state.eip;
    report->fault_address = state.fault_address;
    report->fault_width = state.fault_width;
    report->fault_write = state.fault_write;
    report->fault_block_pc = engine.fault_block_pc;
    report->fault_block_instructions = engine.fault_block_instructions;
    report->fault_block_resident_mask = engine.fault_block_resident_mask;
    memcpy(report->fault_block_map, engine.fault_block_map,
           sizeof(report->fault_block_map));
    report->fault_block_code_bytes = engine.fault_block_code_bytes;
    memcpy(report->fault_block_code, engine.fault_block_code,
           sizeof(report->fault_block_code));
    /*
     * Independent identification of the call that reached the boundary. The
     * stub calls the dispatcher, so the guest return address on top of the
     * stack points into that stub; its own "mov eax, id" must name the
     * syscall we observed in EAX. This binds the number to the image instead
     * of trusting the register alone.
     */
    /* Any stop that happened at the thunk leaves the same two-level frame on
     * the guest stack, including the ones a bridged run stops with. */
    if (report->stop_address == report->boundary_thunk_va &&
        report->boundary_thunk_va != 0u && entry_module &&
        state.gpr[4] >= state.stack_low &&
        (uint64_t)state.gpr[4] + 4u <= state.stack_high) {
        uint32_t return_eip = 0u;
        const uint64_t base = entry_module->mapped.actual_base;
        const uint64_t end = base + entry_module->mapped.image_bytes;

        memcpy(&return_eip, (const void *)(uintptr_t)state.gpr[4], 4u);
        report->boundary_return_eip = return_eip;
        if ((uint64_t)return_eip >= base && (uint64_t)return_eip < end) {
            const uint32_t rva = (uint32_t)((uint64_t)return_eip - base);
            const uint8_t *image = entry_module->mapped.region.write_base;

            report->boundary_return_in_module = 1u;
            for (uint32_t back = 0; back <= 16u && back <= rva; back++) {
                const uint32_t here = rva - back;
                uint32_t operand = 0u;

                /* "mov edx, <dispatcher thunk>" then, five bytes earlier,
                 * "mov eax, <syscall id>". */
                if (here + 6u > entry_module->mapped.image_bytes ||
                    image[here] != 0xbau)
                    continue;
                memcpy(&operand, image + here + 1u, 4u);
                if (operand != report->boundary_thunk_va)
                    continue;
                if (here < 5u || image[here - 5u] != 0xb8u)
                    continue;
                memcpy(&report->caller_stub_id, image + here - 4u, 4u);
                report->caller_stub_rva = here - 5u;
                break;
            }
        }
    }
    report->translated_blocks = engine.cache.publishes;
    report->translated_bytes = engine.cache.cursor;
    report->register_loads = engine.reg_loads;
    report->register_stores = engine.reg_stores;
    report->register_reconciliations = engine.reg_reconciliations;
    report->register_spills = engine.reg_spills;
    /* No host Wine entry point is ever called: the gate only translates and
     * executes guest bytes it mapped itself. */
    report->host_calls = 0u;

done:
    /* Which stage refused, for the evidence and for the next bisect: the gate
     * is a pipeline (fingerprint, loader, resolver, bind, TLS, boundary,
     * process, engine, memory, run) and a failure has to say where it stopped
     * instead of leaving the caller to guess. */
    report->gate_stage = stage;
    report->status = status;
    {
        uint32_t failures = 0u;

        /*
         * Every teardown action is attempted even when an earlier one fails,
         * and every failure is counted into the evidence. The verdict has to
         * come from what the releases actually did: an action that fails
         * keeps its unit-level owner in place. This orchestration function is
         * one-shot, so it also propagates a persistent cleanup failure instead
         * of returning a successful run after losing access to the owner.
         */
        for (uint32_t slot = 0u; slot < (uint32_t)PW_NT_HANDLE_MAX; ++slot) {
            const uint32_t value = pw_nt_handle_value_of(&calls.handles, slot);

            if (value != 0u) {
                const int release_status = pw_wine_handle_release(&calls, value);

                if (release_status != PW_OK) {
                    if (cleanup_status == PW_OK)
                        cleanup_status = release_status;
                    failures++;
                }
            }
        }
        if (have_engine) {
            const int engine_status = pw_x86_engine_destroy(&engine);

            if (engine_status == PW_OK)
                report->cleanup_translations++;
            else {
                if (cleanup_status == PW_OK)
                    cleanup_status = engine_status;
                report->cleanup_translations_pending++;
                failures++;
            }
        }
        if (have_process) {
            const int process_status =
                pw_guest_process_release(&process, config->backend);

            if (process_status != PW_OK) {
                if (cleanup_status == PW_OK)
                    cleanup_status = process_status;
                failures++;
            }
            report->cleanup_mappings += process.released;
            report->cleanup_process_pages_pending = process.mapped;
        }
        /* The parameters page is registered among the call regions (it is the
         * block ntdll replaces and releases), so it is released below with
         * them and only once, whether or not the guest already gave it back. */
        for (uint32_t index = 0; index < calls.region_count; ++index) {
            if (calls.vm->base.release(calls.vm->base.context,
                                        &calls.regions[index]) == PW_OK)
                report->cleanup_mappings++;
            else {
                if (cleanup_status == PW_OK)
                    cleanup_status = PW_ERR_VM;
                report->cleanup_call_regions_pending++;
                failures++;
            }
        }
        if (have_loader) {
            /* Counted before the release: a complete release zeroes the
             * loader's inventory, so the pending count has to come from the
             * inventory the run actually held. */
            const uint32_t owned = runner->loader.module_count;
            const int loader_status = pw_loader_release(&runner->loader);

            if (loader_status != PW_OK) {
                if (cleanup_status == PW_OK)
                    cleanup_status = loader_status;
                failures++;
            }
            report->cleanup_modules = runner->loader.released_modules;
            report->cleanup_modules_pending =
                owned - runner->loader.released_modules;
            report->cleanup_mappings += runner->loader.released_modules;
        }
        report->cleanup_failures = failures;
    }
    if (root_span.bytes && config->provider->close)
        config->provider->close(config->provider->context, &root_span);
    if (cleanup_status != PW_OK)
        status = cleanup_status;
    else if (status == PW_OK)
        status = pw_wine_stop_is_acceptance(report->stop) ? PW_OK
                                                          : PW_ERR_UNSUPPORTED;
    report->low_exhausted = (uint32_t)pw_guest_vm_exhausted(&guest_vm);
    report->status = status;
    return status;
}

const char *pw_wine_stop_name(PwWineStop stop)
{
    switch (stop) {
    case PW_WINE_STOP_NONE: return "none";
    case PW_WINE_STOP_UNIX_CALL_BOUNDARY: return "wine-unix-call-boundary";
    case PW_WINE_STOP_UNSUPPORTED_INSTRUCTION: return "unsupported-instruction";
    case PW_WINE_STOP_MEMORY_BOUNDS: return "memory-bounds";
    case PW_WINE_STOP_CACHE_LIMIT: return "cache-limit";
    case PW_WINE_STOP_NON_CODE: return "non-code";
    case PW_WINE_STOP_DECODE_FAILURE: return "decode-failure";
    case PW_WINE_STOP_X87_TRAP: return "x87-trap";
    case PW_WINE_STOP_STEP_BUDGET: return "step-budget";
    case PW_WINE_STOP_RETURNED_TO_CALLER: return "returned-to-caller";
    case PW_WINE_STOP_GATE_ERROR: return "gate-error";
    case PW_WINE_STOP_UNIX_CALL_UNIMPLEMENTED: return "unix-call-unimplemented";
    case PW_WINE_STOP_UNIX_CALL_UNKNOWN: return "unix-call-unknown";
    case PW_WINE_STOP_UNIX_CALL_REJECTED: return "unix-call-rejected";
    case PW_WINE_STOP_PROCESS_TERMINATED: return "process-terminated";
    case PW_WINE_STOP_UNIXLIB_BOUNDARY: return "unixlib-boundary";
    case PW_WINE_STOP_UNIXLIB_REFUSED: return "unixlib-refused";
    case PW_WINE_STOP_UNIXLIB_UNIMPLEMENTED: return "unixlib-unimplemented";
    default: return "unknown";
    }
}

int pw_wine_stop_is_acceptance(PwWineStop stop)
{
    return stop == PW_WINE_STOP_UNIX_CALL_BOUNDARY;
}
