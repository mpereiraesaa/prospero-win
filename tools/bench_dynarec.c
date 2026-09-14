/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Measured IA-32 dynarec benchmark suite with deterministic state checksums. */
#define _GNU_SOURCE
#include "../src/pw_x86_engine.h"
#include "../src/pw_vm_posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

enum {
    BENCH_PAGE_SIZE = 4096,
    BENCH_CODE_SIZE = 65536,
    BENCH_CACHE_ENTRIES = 256,
    BENCH_MEM_BYTES = 65536,
    BENCH_STACK_BYTES = 65536,
    BENCH_SAMPLES = 21,
    BENCH_BATCH_RUNS = 8,
};

typedef struct BenchWorkload {
    const char *name;
    const uint8_t *code;
    size_t code_bytes;
    uint32_t entry_pc;
    uint32_t expected_steps;
    void (*setup_state)(PwX86State *state, uint32_t mem_addr, uint32_t stack_addr);
    uint32_t expected_checksum;
} BenchWorkload;

typedef struct BenchSource {
    uint32_t base;
    const uint8_t *data;
    size_t bytes;
} BenchSource;

static int bench_source_view(void *opaque, uint32_t pc, const uint8_t **source, size_t *bytes)
{
    const BenchSource *s = (const BenchSource *)opaque;
    if (pc < s->base || (uint64_t)pc >= s->base + s->bytes) return PW_ERR_NOT_FOUND;
    size_t offset = pc - s->base;
    *source = s->data + offset;
    *bytes = s->bytes - offset;
    return PW_OK;
}

static uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int bench_switch(const char *name, unsigned fallback, unsigned *value)
{
    const char *text = getenv(name);
    if (!text || !*text) {
        *value = fallback;
        return PW_OK;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "1") == 0) {
        *value = (unsigned)(text[0] - '0');
        return PW_OK;
    }
    fprintf(stderr, "%s must be 0 or 1\n", name);
    return PW_ERR_PRECONDITION;
}

static int configure_engine(PwX86Engine *engine, unsigned chaining,
                            unsigned residency, unsigned lazy_flags)
{
    return pw_x86_engine_set_chaining(engine, chaining) == PW_OK &&
           pw_x86_engine_set_residency(engine, residency) == PW_OK &&
           pw_x86_engine_set_lazy_flags(engine, lazy_flags) == PW_OK
               ? PW_OK : PW_ERR_STATE;
}

static uint32_t hash_step(uint32_t h, uint32_t val)
{
    h ^= val;
    h *= 16777619u;
    return h;
}

static uint32_t compute_checksum(const PwX86State *s, const uint8_t *mem, size_t mem_bytes)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 8; i++) h = hash_step(h, s->gpr[i]);
    h = hash_step(h, s->eip);
    h = hash_step(h, s->eflags);
    h = hash_step(h, s->fp.x87_status);
    h = hash_step(h, s->fp.x87_control);
    h = hash_step(h, s->fp.x87_tag);
    if (mem && mem_bytes) {
        for (size_t i = 0; i < mem_bytes; i += 4) {
            uint32_t word = (uint32_t)mem[i] | ((uint32_t)mem[i+1]<<8) |
                            ((uint32_t)mem[i+2]<<16) | ((uint32_t)mem[i+3]<<24);
            h = hash_step(h, word);
        }
    }
    return h;
}

/* --- Workload 1: Register ALU --- */
/*
 * Outer loop: ECX = 20000 iterations.
 * Inside loop: ADD, XOR, IMUL, SUB, AND, OR, INC, DEC, SAR.
 */
static const uint8_t CODE_REG_ALU[] = {
    /* 0x1000: loop_start */
    0x05, 0x37, 0x13, 0x00, 0x00,       /* add eax, 0x1337 */
    0x31, 0xc2,                         /* xor edx, eax */
    0x0f, 0xaf, 0xda,                   /* imul ebx, edx */
    0x29, 0xd8,                         /* sub eax, ebx */
    0x81, 0xe2, 0xff, 0xff, 0xff, 0x7f, /* and edx, 0x7fffffff */
    0x81, 0xcb, 0x10, 0x10, 0x10, 0x10, /* or  ebx, 0x10101010 */
    0x40,                               /* inc eax */
    0xc1, 0xfa, 0x01,                   /* sar edx, 1 */
    0x49,                               /* dec ecx */
    0x75, 0xe1,                         /* jnz 0x1000 */
    0xc3                                /* ret (terminal) */
};

static void setup_reg_alu(PwX86State *s, uint32_t mem, uint32_t stack)
{
    (void)mem;
    s->gpr[0] = 0x12345678; /* eax */
    s->gpr[1] = 20000;      /* ecx (iterations) */
    s->gpr[2] = 0xdeadbeef; /* edx */
    s->gpr[3] = 0x00000003; /* ebx */
    s->gpr[4] = stack + 4096;
    *(uint32_t *)(uintptr_t)(stack + 4096) = 0;
}

/* --- Workload 2: Conditional Branches --- */
/*
 * Branchy loop: ECX = 20000 iterations.
 * Collatz-style / test bit parity branching.
 */
static const uint8_t CODE_COND_BRANCH[] = {
    /* 0x1000: loop_start */
    0xa8, 0x01,                         /* test al, 1 */
    0x74, 0x06,                         /* jz even (0x100a) */
    /* odd */
    0x6b, 0xc0, 0x03,                   /* imul eax, eax, 3 */
    0x40,                               /* inc eax */
    0xeb, 0x02,                         /* jmp next (0x100c) */
    /* 0x100a: even */
    0xd1, 0xe8,                         /* shr eax, 1 */
    /* 0x100c: next */
    0x01, 0xc2,                         /* add edx, eax */
    0x49,                               /* dec ecx */
    0x75, 0xef,                         /* jnz loop_start (0x1000) */
    0xc3                                /* ret */
};

static void setup_cond_branch(PwX86State *s, uint32_t mem, uint32_t stack)
{
    (void)mem;
    s->gpr[0] = 0x0000001b; /* eax = 27 (classic Collatz sequence) */
    s->gpr[1] = 20000;      /* ecx */
    s->gpr[2] = 0;          /* edx accumulator */
    s->gpr[4] = stack + 4096;
    *(uint32_t *)(uintptr_t)(stack + 4096) = 0;
}

/* --- Workload 3: Call / Return & Stack --- */
/*
 * Function calling loop: ECX = 10000 iterations.
 * Caller pushes EAX, calls callee, callee reads arg, returns, caller adjusts stack.
 */
static const uint8_t CODE_CALL_RET[] = {
    /* 0x1000: loop_start */
    0x50,                               /* push eax */
    0xe8, 0x08, 0x00, 0x00, 0x00,       /* call callee (0x100e) */
    0x83, 0xc4, 0x04,                   /* add esp, 4 */
    0x49,                               /* dec ecx */
    0x75, 0xf4,                         /* jnz loop_start (0x1000) */
    0xc3,                               /* ret */
    0x90,                               /* nop padding */
    /* 0x100e: callee */
    0x8b, 0x44, 0x24, 0x04,             /* mov eax, [esp+4] */
    0x83, 0xc0, 0x07,                   /* add eax, 7 */
    0xc3                                /* ret */
};

static void setup_call_ret(PwX86State *s, uint32_t mem, uint32_t stack)
{
    (void)mem;
    s->gpr[0] = 1;          /* eax */
    s->gpr[1] = 10000;      /* ecx */
    s->gpr[4] = stack + 4096; /* esp */
    *(uint32_t *)(uintptr_t)(stack + 4096) = 0;
}

/* --- Workload 4: Memory Load & Store --- */
/*
 * Array read/write:
 * Loop 100 times over a 256-dword array.
 * Reads [ESI + EBX*4], adds EDX, stores to [EDI + EBX*4].
 */
static const uint8_t CODE_MEM_LOAD_STORE[] = {
    /* 0x1000: outer_start */
    0x31, 0xdb,                         /* xor ebx, ebx (index = 0) */
    /* 0x1002: inner_start */
    0x8b, 0x04, 0x9e,                   /* mov eax, [esi+ebx*4] */
    0x01, 0xd0,                         /* add eax, edx */
    0x89, 0x04, 0x9f,                   /* mov [edi+ebx*4], eax */
    0x43,                               /* inc ebx */
    0x81, 0xfb, 0x00, 0x01, 0x00, 0x00, /* cmp ebx, 256 */
    0x72, 0xef,                         /* jb inner_start (0x1002) */
    0x01, 0xc2,                         /* add edx, eax */
    0x49,                               /* dec ecx */
    0x75, 0xe8,                         /* jnz outer_start (0x1000) */
    0xc3                                /* ret */
};

static void setup_mem_load_store(PwX86State *s, uint32_t mem, uint32_t stack)
{
    s->gpr[1] = 100;                    /* ecx = 100 outer iterations */
    s->gpr[2] = 5;                      /* edx */
    s->gpr[4] = stack + 4096;
    *(uint32_t *)(uintptr_t)(stack + 4096) = 0;
    s->gpr[6] = mem;                    /* esi = source array (1024 bytes) */
    s->gpr[7] = mem + 1024;             /* edi = dest array (1024 bytes) */
}

/* --- Workload 5: x87 Floating Point --- */
/*
 * x87 computation loop: 5000 iterations.
 * Computes a series using FLD, FADD, FMUL and FSTP.
 */
static const uint8_t CODE_X87_FP[] = {
    /* 0x1000: loop_start */
    0xd9, 0x06,                         /* fld dword [esi] */
    0xd8, 0x07,                         /* fadd dword [edi] */
    0xd8, 0x0e,                         /* fmul dword [esi] */
    0xd9, 0x1f,                         /* fstp dword [edi] */
    0x49,                               /* dec ecx */
    0x75, 0xf5,                         /* jnz loop_start (0x1000) */
    0xc3                                /* ret */
};

static void setup_x87_fp(PwX86State *s, uint32_t mem, uint32_t stack)
{
    pw_guest_fp_init(&s->fp);
    s->gpr[1] = 5000;                   /* ecx */
    s->gpr[4] = stack + 4096;
    *(uint32_t *)(uintptr_t)(stack + 4096) = 0;
    s->gpr[6] = mem;                    /* esi: float A */
    s->gpr[7] = mem + 4;                /* edi: float B */
    float *fmem = (float *)(uintptr_t)mem;
    fmem[0] = 1.0001f;
    fmem[1] = 0.5f;
}

static const BenchWorkload WORKLOADS[] = {
    {"reg_alu", CODE_REG_ALU, sizeof(CODE_REG_ALU), 0x1000, 0, setup_reg_alu, 0},
    {"cond_branch", CODE_COND_BRANCH, sizeof(CODE_COND_BRANCH), 0x1000, 0, setup_cond_branch, 0},
    {"call_ret", CODE_CALL_RET, sizeof(CODE_CALL_RET), 0x1000, 0, setup_call_ret, 0},
    {"mem_load_store", CODE_MEM_LOAD_STORE, sizeof(CODE_MEM_LOAD_STORE), 0x1000, 0, setup_mem_load_store, 0},
    {"x87_fp", CODE_X87_FP, sizeof(CODE_X87_FP), 0x1000, 0, setup_x87_fp, 0},
};

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;
    unsigned chaining = 0, residency = 1, lazy_flags = 1;
    if (bench_switch("PW_BENCH_CHAINING", 0, &chaining) != PW_OK ||
        bench_switch("PW_BENCH_RESIDENCY", 1, &residency) != PW_OK ||
        bench_switch("PW_BENCH_LAZY_FLAGS", 1, &lazy_flags) != PW_OK)
        return 1;
    PwVmBackend vm;
    if (pw_vm_posix_backend(&vm) != PW_OK) {
        fprintf(stderr, "failed to get posix vm backend\n");
        return 1;
    }

    /* Allocate memory region and stack region in 32-bit address space for guest execution */
    PwVmRegion mem_region, stack_region;
    if (vm.reserve_at(vm.context, 0x02000000, BENCH_MEM_BYTES, vm.page_bytes, &mem_region) != PW_OK ||
        vm.commit(vm.context, &mem_region, 0, mem_region.bytes, PW_PROT_READ|PW_PROT_WRITE) != PW_OK) {
        fprintf(stderr, "failed to allocate memory region at 0x02000000\n");
        return 1;
    }
    if (vm.reserve_at(vm.context, 0x03000000, BENCH_STACK_BYTES, vm.page_bytes, &stack_region) != PW_OK ||
        vm.commit(vm.context, &stack_region, 0, stack_region.bytes, PW_PROT_READ|PW_PROT_WRITE) != PW_OK) {
        fprintf(stderr, "failed to allocate stack region at 0x03000000\n");
        return 1;
    }

    uint32_t mem_base = (uint32_t)(uintptr_t)mem_region.write_base;
    uint32_t stack_base = (uint32_t)(uintptr_t)stack_region.write_base;

    for (size_t w = 0; w < sizeof(WORKLOADS)/sizeof(WORKLOADS[0]); w++) {
        const BenchWorkload *wl = &WORKLOADS[w];
        if (filter && strcmp(filter, wl->name) != 0 && strcmp(filter, "all") != 0)
            continue;

        /* Reset test data */
        memset(mem_region.write_base, 0x5a, mem_region.bytes);
        memset(stack_region.write_base, 0x00, stack_region.bytes);

        BenchSource source = {wl->entry_pc, wl->code, wl->code_bytes};
        PwX86CacheEntry entries[BENCH_CACHE_ENTRIES];
        PwX86Engine engine;

        /* 1. Measure cold translation */
        uint64_t cold_times[BENCH_SAMPLES];
        size_t total_emitted_bytes = 0;
        uint32_t total_compiled_blocks = 0;

        for (int sample = 0; sample < BENCH_SAMPLES; sample++) {
            if (pw_x86_engine_init(&engine, &vm, entries, BENCH_CACHE_ENTRIES,
                                   BENCH_CODE_SIZE, sample + 1, bench_source_view, &source) != PW_OK) {
                fprintf(stderr, "engine init failed\n");
                return 1;
            }
            if (configure_engine(&engine, chaining, residency, lazy_flags) != PW_OK)
                return 1;
            PwX86State s = {.eip = wl->entry_pc, .stack_low = stack_base, .stack_high = stack_base + BENCH_STACK_BYTES};
            s.memory_count = 1;
            s.memory[0] = (PwX86Memory){.low = mem_base, .high = mem_base + BENCH_MEM_BYTES,
                                        .permissions = PW_X86_READ|PW_X86_WRITE};
            wl->setup_state(&s, mem_base, stack_base);

            uint64_t t0 = bench_now_ns();
            PwX86StepReport report;
            int status = pw_x86_engine_step(&engine, &s, &report);
            uint64_t t1 = bench_now_ns();

            if (status != PW_OK) {
                fprintf(stderr, "workload %s cold step failed: %d\n", wl->name, status);
                return 1;
            }
            cold_times[sample] = t1 - t0;
            total_emitted_bytes = report.code_bytes;
            total_compiled_blocks = (uint32_t)engine.compiles;
            pw_x86_engine_destroy(&engine);
        }

        qsort(cold_times, BENCH_SAMPLES, sizeof(uint64_t), compare_u64);
        uint64_t cold_median_ns = cold_times[BENCH_SAMPLES / 2];

        /* 2. Measure warm execution */
        /* Initialize persistent engine */
        if (pw_x86_engine_init(&engine, &vm, entries, BENCH_CACHE_ENTRIES,
                               BENCH_CODE_SIZE, 100, bench_source_view, &source) != PW_OK) {
            fprintf(stderr, "engine init failed\n");
            return 1;
        }
        if (configure_engine(&engine, chaining, residency, lazy_flags) != PW_OK)
            return 1;

        /* Warmup run */
        {
            PwX86State s = {.eip = wl->entry_pc, .stack_low = stack_base, .stack_high = stack_base + BENCH_STACK_BYTES};
            s.memory_count = 1;
            s.memory[0] = (PwX86Memory){.low = mem_base, .high = mem_base + BENCH_MEM_BYTES,
                                        .permissions = PW_X86_READ|PW_X86_WRITE};
            wl->setup_state(&s, mem_base, stack_base);
            while (s.eip >= wl->entry_pc && s.eip < wl->entry_pc + wl->code_bytes) {
                PwX86StepReport rep;
                if (pw_x86_engine_step(&engine, &s, &rep) != PW_OK) break;
            }
        }

        /* Measured warm runs */
        uint64_t warm_times[BENCH_SAMPLES];
        uint64_t total_retired = 0, total_dispatches = 0;
        uint32_t checksum = 0;
        PwX86State last_state = {0};

        for (int sample = 0; sample < BENCH_SAMPLES; sample++) {
            uint64_t sample_retired = 0, sample_dispatches = 0;
            uint64_t elapsed = 0;

            /* A single synthetic run is sub-millisecond on current hosts and
             * therefore dominated by scheduler/frequency noise.  Time a
             * deterministic batch while keeping setup outside each timed
             * interval; checksums still describe the final identical run. */
            for (int batch = 0; batch < BENCH_BATCH_RUNS; ++batch) {
                memset(mem_region.write_base, 0x5a, mem_region.bytes);
                memset(stack_region.write_base, 0x00, stack_region.bytes);

                PwX86State s = {
                    .eip = wl->entry_pc, .stack_low = stack_base,
                    .stack_high = stack_base + BENCH_STACK_BYTES
                };
                s.memory_count = 1;
                s.memory[0] = (PwX86Memory){
                    .low = mem_base, .high = mem_base + BENCH_MEM_BYTES,
                    .permissions = PW_X86_READ|PW_X86_WRITE
                };
                wl->setup_state(&s, mem_base, stack_base);

                uint64_t t0 = bench_now_ns();
                while (s.eip >= wl->entry_pc &&
                       s.eip < wl->entry_pc + wl->code_bytes) {
                    PwX86StepReport rep;
                    int status = pw_x86_engine_step(&engine, &s, &rep);
                    if (status != PW_OK) {
                        fprintf(stderr, "warm step failed: %d\n", status);
                        return 1;
                    }
                    sample_retired += rep.retired;
                    sample_dispatches++;
                }
                elapsed += bench_now_ns() - t0;
                checksum = compute_checksum(&s, mem_region.write_base, 2048);
                last_state = s;
            }
            warm_times[sample] = elapsed;
            total_retired = sample_retired;
            total_dispatches = sample_dispatches;
        }

        qsort(warm_times, BENCH_SAMPLES, sizeof(uint64_t), compare_u64);
        uint64_t warm_min_ns = warm_times[0];
        uint64_t warm_med_ns = warm_times[BENCH_SAMPLES / 2];
        uint64_t warm_p95_ns = warm_times[(BENCH_SAMPLES * 95) / 100];
        uint64_t warm_max_ns = warm_times[BENCH_SAMPLES - 1];

        double ns_per_inst = total_retired ? (double)warm_med_ns / (double)total_retired : 0.0;
        double mips = warm_med_ns ? (double)total_retired * 1000.0 / (double)warm_med_ns : 0.0;

        printf("kind=bench-result workload=%s chaining=%u residency=%u lazy_flags=%u "
               "cold_median_ns=%llu code_bytes=%zu "
               "compiles=%u batch_runs=%u retired=%llu dispatches=%llu "
               "warm_min_ns=%llu warm_med_ns=%llu "
               "warm_p95_ns=%llu warm_max_ns=%llu ns_per_inst=%.3f mips=%.1f "
               "eax=0x%08x ecx=0x%08x edx=0x%08x ebx=0x%08x checksum=0x%08x\n",
               wl->name,
               chaining,residency,lazy_flags,
               (unsigned long long)cold_median_ns,
               total_emitted_bytes,
               total_compiled_blocks,
               BENCH_BATCH_RUNS,
               (unsigned long long)total_retired,
               (unsigned long long)total_dispatches,
               (unsigned long long)warm_min_ns,
               (unsigned long long)warm_med_ns,
               (unsigned long long)warm_p95_ns,
               (unsigned long long)warm_max_ns,
               ns_per_inst,
               mips,
               last_state.gpr[0], last_state.gpr[1], last_state.gpr[2], last_state.gpr[3],
               checksum);

        pw_x86_engine_destroy(&engine);
    }

    vm.release(vm.context, &mem_region);
    vm.release(vm.context, &stack_region);
    return 0;
}
