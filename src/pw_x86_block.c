/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_x86_block.h"
#include "pw_x86_padding.h"
#include "pw_x87.h"
#include <string.h>

/* Context accesses below use signed disp8 encodings. Fail at build time if
 * future state-layout changes would silently address the wrong field. */
_Static_assert(offsetof(PwX86State,eflags)<=127,"context exceeds disp8 layout");
_Static_assert(offsetof(PwX86State,memory[0].permissions)<=127,
               "generated memory fast path exceeds disp8 layout");

/* flat_span: when nonzero, every guest access and the stack are known to be
 * valid exactly inside [flat_low, flat_low + flat_span), so the guard is one
 * unsigned compare (PwX86TranslateOptions.flat_high). */
/* exits counts the places emitted so far where a block can stop in the
 * middle (a refused check or a failed helper): an instruction that emits
 * none needs no guest EIP stored before it. */
typedef struct Emitter {
    uint8_t *p; size_t n, cap; int failed; uint32_t flat_low, flat_span; unsigned exits;
    unsigned no_counters; /* PwX86TranslateOptions.no_counters */
    /* rcx holds the rcx_flags_mask bits of the last flag producer's host
     * flags, and nothing has been emitted since (end is e->n + 1; 0: none). */
    size_t rcx_flags_end; uint32_t rcx_flags_mask;
    /* Likewise when the host flags themselves are still the producer's. */
    size_t host_flags_end;
    /* Flat-guard misses, emitted after the block (emit_cold_paths): the
     * rel32 of the branch to patch, where to resume, and the access; a
     * width of 0 is a refused push or pop. */
    struct { size_t patch, resume; uint8_t write, width; } cold[48];
    unsigned cold_count;
} Emitter;
static void byte(Emitter *e, uint8_t value)
{
    if (e->n == e->cap) { e->failed = 1; return; }
    e->p[e->n++] = value;
}
static void word(Emitter *e, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) byte(e, (uint8_t)(value >> (8*i)));
}
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void store(Emitter *e, size_t offset, uint32_t value)
{
    byte(e,0xc7); byte(e,0x47); byte(e,(uint8_t)offset); word(e,value);
}
static void load_eax(Emitter *e, size_t offset)
{
    byte(e,0x8b); byte(e,0x47); byte(e,(uint8_t)offset);
}
static void store_eax(Emitter *e, size_t offset)
{
    byte(e,0x89); byte(e,0x47); byte(e,(uint8_t)offset);
}
/* A failed comparison returns -1. The short branch skips mov eax,-1; ret. */
static void require_condition(Emitter *e, uint8_t condition)
{
    e->exits++;
    byte(e,condition); byte(e,6); byte(e,0xb8); word(e,0xffffffffu); byte(e,0xc3);
}
/* movaps and movdqa fault on an address that is not 16-byte aligned. With
 * the guarded host pointer in rax, a misaligned one stops the block as the
 * guest's access violation at 0xffffffff, which is how Windows reports the
 * CPU's #GP, before the host instruction could fault. */
static void require_aligned16(Emitter *e)
{
    e->exits++;
    byte(e,0xa8); byte(e,0x0f);                             /* test al, 15 */
    byte(e,0x74); byte(e,36);                               /* jz past the fault */
    byte(e,0xc7); byte(e,0x87); word(e,(uint32_t)offsetof(PwX86State,fault_address));
    word(e,0xffffffffu);
    byte(e,0xc7); byte(e,0x87); word(e,(uint32_t)offsetof(PwX86State,fault_width)); word(e,16);
    byte(e,0xc7); byte(e,0x87); word(e,(uint32_t)offsetof(PwX86State,fault_write)); word(e,0);
    byte(e,0xb8); word(e,0xffffffffu); byte(e,0xc3);
}
static int sse_aligned_move(uint8_t prefix, uint8_t opcode)
{
    return (prefix == 0x66u && (opcode == 0x6fu || opcode == 0x7fu)) ||
           (prefix == 0u && (opcode == 0x28u || opcode == 0x29u));
}
/* Flat guard: jbe over whatever follows when eax .. eax+width-1 lies in
 * the flat range; edx is clobbered. Returns the rel8 slot to patch. */
static size_t flat_range_check(Emitter *e,unsigned width)
{
    byte(e,0x8d); byte(e,0x90); word(e,(uint32_t)0u-e->flat_low);   /* lea edx,[rax-low] */
    byte(e,0x81); byte(e,0xfa); word(e,e->flat_span-width);         /* cmp edx,span-width */
    byte(e,0x76);                                                   /* jbe fast_ok */
    return e->n++;
}
/* The same check with the miss out of line: ja to a cold path emitted after
 * the block, which resumes here. 0 when there is no room for one more. */
static int flat_range_cold(Emitter *e,unsigned width,unsigned write,unsigned cold_width)
{
    if (e->cold_count >= sizeof(e->cold) / sizeof(e->cold[0])) return 0;
    byte(e,0x8d); byte(e,0x90); word(e,(uint32_t)0u-e->flat_low);   /* lea edx,[rax-low] */
    byte(e,0x81); byte(e,0xfa); word(e,e->flat_span-width);         /* cmp edx,span-width */
    byte(e,0x0f); byte(e,0x87);                                     /* ja cold */
    e->cold[e->cold_count].patch = e->n;
    word(e,0);
    e->cold[e->cold_count].resume = e->n;
    e->cold[e->cold_count].write = (uint8_t)write;
    e->cold[e->cold_count].width = (uint8_t)cold_width;
    e->cold_count++;
    /* The cold path can stop the block, so this instruction needs EIP. */
    e->exits++;
    return 1;
}
static void stack_bounds(Emitter *e)
{
    if (e->flat_span) {
        if (flat_range_cold(e,4,0,0)) return;
        size_t ok=flat_range_check(e,4);
        e->exits++;
        byte(e,0xb8); word(e,0xffffffffu); byte(e,0xc3);
        e->p[ok]=(uint8_t)(e->n-(ok+1));
        return;
    }
    byte(e,0x3b); byte(e,0x47); byte(e,offsetof(PwX86State,stack_low));
    require_condition(e,0x73);
    byte(e,0x8b); byte(e,0x57); byte(e,offsetof(PwX86State,stack_high));
    byte(e,0x83); byte(e,0xfa); byte(e,4); require_condition(e,0x73);
    byte(e,0x83); byte(e,0xea); byte(e,4);
    byte(e,0x39); byte(e,0xd0); require_condition(e,0x76);
}
static uintptr_t memory_range_pointer(PwX86State *state,uint32_t address,unsigned write,uint64_t width)
{
    uint64_t end=(uint64_t)address+width;
    unsigned permission=write==2?(PW_X86_READ|PW_X86_WRITE):write?PW_X86_WRITE:PW_X86_READ;
    /*
     * The record is written when the access is *refused*, not when it is
     * checked. Writing it at entry made the report name the last access the
     * guard looked at rather than the one that failed - and the guard is
     * called for accesses that then succeed, and the inline fast paths do not
     * call it at all - so a run that stopped on a fault could report an
     * address that was fine a moment earlier. The record is read by the gate's
     * evidence and by tests that pin a running frontier, so it has to be the
     * failing access.
     */
    if (!address || !width || end>0x100000000ull || state->memory_count>PW_X86_MEMORY_REGIONS)
        goto refused;
    if(address>=state->stack_low && end<=state->stack_high)return address;
    for(unsigned i=0;i<state->memory_count;i++) {
        const PwX86Memory *m=&state->memory[i];
        if(m->high<=0x100000000ull && address>=m->low && end<=m->high &&
           (m->permissions&permission)==permission)return address;
    }
refused:
    state->fault_address=address;
    state->fault_width=(uint32_t)width;
    state->fault_write=write;
    return 0;
}
static uintptr_t memory_pointer(PwX86State *state,uint32_t address,unsigned write,unsigned width)
{
    /* 16 is the SSE 128-bit access width: movups/movdqa and friends. */
    if(width!=1 && width!=2 && width!=4 && width!=8 && width!=10 && width!=16)return 0;
    return memory_range_pointer(state,address,write,width);
}
static void memory_slow_path(Emitter *e,unsigned write,unsigned width);
static void memory_address_width(Emitter *e,unsigned write,unsigned width)
{
    if (e->flat_span) {
        /* Inside the flat range the access is valid, as the region table
         * says; outside it the slow helper decides and records the fault. */
        if (flat_range_cold(e,width,write,width)) return;
        size_t ok=flat_range_check(e,width);
        memory_slow_path(e,write,width);
        if (!e->failed) e->p[ok]=(uint8_t)(e->n-(ok+1));
        return;
    }
    /* Preserve the slow helper's state-contract checks before taking either
     * inline path.  In particular, a corrupt registry must not make even the
     * otherwise-valid stack path executable, and guest NULL is never a valid
     * identity-mapped pointer. */
    /*
     * The bound is a 32-bit immediate, not the imm8 this used to encode: the
     * table is compared against a count that a process with several mapped
     * images pushes past 127, and an imm8 there would sign-extend to a
     * negative bound and classify every access as out of bounds.
     */
    byte(e, 0x81); byte(e, 0x7f); byte(e, offsetof(PwX86State, memory_count));
    word(e, PW_X86_MEMORY_REGIONS); /* cmp dword [rdi + memory_count], max */
    byte(e, 0x77); /* ja to slow_path */
    size_t patch_ja_count = e->n++;
    byte(e, 0x85); byte(e, 0xc0); /* test eax, eax */
    byte(e, 0x74); /* jz to slow_path */
    size_t patch_jz_null = e->n++;

    /* 1. Fast path: check stack [stack_low, stack_high).
     * If stack_low <= EAX && EAX + width <= stack_high, address is valid RW stack. */
    byte(e, 0x3b); byte(e, 0x47); byte(e, offsetof(PwX86State, stack_low)); /* cmp eax, [rdi + stack_low] */
    byte(e, 0x72); /* jb to try_mem0 */
    size_t patch_jb_stack = e->n++;

    byte(e, 0x89); byte(e, 0xc2); /* mov edx, eax */
    byte(e, 0x83); byte(e, 0xc2); byte(e, (uint8_t)width); /* add edx, width */
    byte(e, 0x72); /* jc to try_mem0 (unsigned 32-bit wrap) */
    size_t patch_jc_stack = e->n++;

    byte(e, 0x3b); byte(e, 0x57); byte(e, offsetof(PwX86State, stack_high)); /* cmp edx, [rdi + stack_high] */
    byte(e, 0x76); /* jbe to fast_ok */
    size_t patch_jbe_stack = e->n++;

    /* try_mem0: check memory[0] if memory_count >= 1 */
    e->p[patch_jb_stack] = (uint8_t)(e->n - (patch_jb_stack + 1));
    e->p[patch_jc_stack] = (uint8_t)(e->n - (patch_jc_stack + 1));

    unsigned req_perm = write == 2 ? (PW_X86_READ|PW_X86_WRITE) : write ? PW_X86_WRITE : PW_X86_READ;
    byte(e, 0x83); byte(e, 0x7f); byte(e, offsetof(PwX86State, memory_count)); byte(e, 1); /* cmp dword [rdi + memory_count], 1 */
    byte(e, 0x72); /* jb to slow_path */
    size_t patch_jb_count = e->n++;

    byte(e, 0x8b); byte(e, 0x57); byte(e, offsetof(PwX86State, memory[0].permissions)); /* mov edx, [rdi + memory[0].permissions] */
    byte(e, 0x83); byte(e, 0xe2); byte(e, (uint8_t)req_perm); /* and edx, req_perm */
    byte(e, 0x83); byte(e, 0xfa); byte(e, (uint8_t)req_perm); /* cmp edx, req_perm */
    byte(e, 0x75); /* jne to slow_path */
    size_t patch_jne_perm = e->n++;

    byte(e, 0x3b); byte(e, 0x47); byte(e, offsetof(PwX86State, memory[0].low)); /* cmp eax, [rdi + memory[0].low] */
    byte(e, 0x72); /* jb to slow_path */
    size_t patch_jb_low = e->n++;

    /* The generic helper rejects malformed regions whose exclusive high
     * bound exceeds 4 GiB.  Keep the common (<4 GiB) case inline; a valid
     * region ending exactly at 4 GiB takes the slow path. */
    byte(e, 0x83); byte(e, 0x7f); byte(e, offsetof(PwX86State, memory[0].high) + 4);
    byte(e, 0); /* cmp dword [rdi + memory[0].high + 4], 0 */
    byte(e, 0x75); /* jne to slow_path */
    size_t patch_jne_high = e->n++;

    byte(e, 0x89); byte(e, 0xc2); /* mov edx, eax (zero-extends into RDX) */
    byte(e, 0x48); byte(e, 0x83); byte(e, 0xc2); byte(e, (uint8_t)width); /* add rdx, width (64-bit) */
    byte(e, 0x48); byte(e, 0x3b); byte(e, 0x57); byte(e, offsetof(PwX86State, memory[0].high)); /* cmp rdx, [rdi + memory[0].high] */
    byte(e, 0x76); /* jbe to fast_ok */
    size_t patch_jbe_mem = e->n++;

    /* slow_path: */
    e->p[patch_ja_count] = (uint8_t)(e->n - (patch_ja_count + 1));
    e->p[patch_jz_null] = (uint8_t)(e->n - (patch_jz_null + 1));
    e->p[patch_jb_count] = (uint8_t)(e->n - (patch_jb_count + 1));
    e->p[patch_jne_perm] = (uint8_t)(e->n - (patch_jne_perm + 1));
    e->p[patch_jb_low] = (uint8_t)(e->n - (patch_jb_low + 1));
    e->p[patch_jne_high] = (uint8_t)(e->n - (patch_jne_high + 1));

    memory_slow_path(e,write,width);

    /* fast_ok: */
    e->p[patch_jbe_stack] = (uint8_t)(e->n - (patch_jbe_stack + 1));
    e->p[patch_jbe_mem] = (uint8_t)(e->n - (patch_jbe_mem + 1));
}
static void memory_slow_path(Emitter *e,unsigned write,unsigned width);
/* The flat guard's misses, after the block's last exit: each asks the
 * region table (or refuses a push or pop) and jumps back. */
static void emit_cold_paths(Emitter *e)
{
    for (unsigned i = 0; i < e->cold_count; i++) {
        const size_t start = e->n;
        const uint32_t to_cold = (uint32_t)(start - (e->cold[i].patch + 4));
        size_t back;

        if (e->failed) return;
        e->p[e->cold[i].patch] = (uint8_t)to_cold;
        e->p[e->cold[i].patch + 1] = (uint8_t)(to_cold >> 8);
        e->p[e->cold[i].patch + 2] = (uint8_t)(to_cold >> 16);
        e->p[e->cold[i].patch + 3] = (uint8_t)(to_cold >> 24);
        if (!e->cold[i].width) {
            byte(e,0xb8); word(e,0xffffffffu); byte(e,0xc3);          /* refused */
            continue;
        }
        memory_slow_path(e, e->cold[i].write, e->cold[i].width);
        byte(e,0xe9); back = e->n; word(e,0);                         /* jmp resume */
        if (e->failed) return;
        {
            const uint32_t to_resume = (uint32_t)(e->cold[i].resume - (back + 4));
            e->p[back] = (uint8_t)to_resume;
            e->p[back + 1] = (uint8_t)(to_resume >> 8);
            e->p[back + 2] = (uint8_t)(to_resume >> 16);
            e->p[back + 3] = (uint8_t)(to_resume >> 24);
        }
    }
    e->cold_count = 0;
}
/* Ask memory_pointer for the host address of eax, or stop the block. */
static void memory_slow_path(Emitter *e,unsigned write,unsigned width)
{
    byte(e,0x89);byte(e,0xc6); /* esi = address */
    byte(e,0xba);word(e,write);
    byte(e,0xb9);word(e,width);
    byte(e,0x57); /* push rdi */
    byte(e,0x41);byte(e,0x50); /* push r8 */
    byte(e,0x41);byte(e,0x51); /* push r9 */
    byte(e,0x41);byte(e,0x52); /* push r10 */
    byte(e,0x41);byte(e,0x53); /* push r11 */
    byte(e,0x48);byte(e,0xb8);
    uint64_t target=(uint64_t)(uintptr_t)&memory_pointer;
    word(e,(uint32_t)target);word(e,(uint32_t)(target>>32));
    byte(e,0xff);byte(e,0xd0); /* call rax */
    byte(e,0x41);byte(e,0x5b); /* pop r11 */
    byte(e,0x41);byte(e,0x5a); /* pop r10 */
    byte(e,0x41);byte(e,0x59); /* pop r9 */
    byte(e,0x41);byte(e,0x58); /* pop r8 */
    byte(e,0x5f); /* pop rdi */
    byte(e,0x48);byte(e,0x85);byte(e,0xc0);
    require_condition(e,0x75);
}
static void memory_address(Emitter *e,unsigned write){memory_address_width(e,write,4);}
static int branch_condition(PwX86State *s,unsigned condition)
{
    unsigned f=s->eflags,of=!!(f&0x800),sf=!!(f&0x80),zf=!!(f&0x40),cf=f&1,pf=!!(f&4),answer;
    switch(condition>>1) {
    case 0:answer=of;break;case 1:answer=cf;break;case 2:answer=zf;break;
    case 3:answer=cf||zf;break;case 4:answer=sf;break;case 5:answer=pf;break;
    case 6:answer=sf!=of;break;default:answer=zf || sf!=of;break;
    }
    return (int)(answer^(condition&1));
}
static void condition_value(Emitter *e,unsigned condition)
{
    /* branch_condition is an ordinary SysV call.  Keep every host register
     * that may carry a resident guest value alive across it.  Five pushes
     * also preserve the required call-site stack alignment. */
    byte(e,0x57); /* push rdi */
    byte(e,0x41);byte(e,0x50); /* push r8 */
    byte(e,0x41);byte(e,0x51); /* push r9 */
    byte(e,0x41);byte(e,0x52); /* push r10 */
    byte(e,0x41);byte(e,0x53); /* push r11 */
    byte(e,0xbe);word(e,condition);byte(e,0x48);byte(e,0xb8);
    uint64_t fn=(uint64_t)(uintptr_t)&branch_condition;
    word(e,(uint32_t)fn);word(e,(uint32_t)(fn>>32));
    byte(e,0xff);byte(e,0xd0); /* call rax */
    byte(e,0x41);byte(e,0x5b); /* pop r11 */
    byte(e,0x41);byte(e,0x5a); /* pop r10 */
    byte(e,0x41);byte(e,0x59); /* pop r9 */
    byte(e,0x41);byte(e,0x58); /* pop r8 */
    byte(e,0x5f); /* pop rdi */
}
/* Blocks may keep guest values in the callee-saved rbx, rbp and r12-r15,
 * so the C side enters them here. The extra slot keeps the block's entry
 * alignment that of a direct call (rsp = 8 mod 16), which its helper calls
 * rely on. */
__asm__(
    ".text\n"
    ".globl pw_x86_run_block\n"
    ".type pw_x86_run_block,@function\n"
    "pw_x86_run_block:\n"
    "    push %rbx\n"
    "    push %rbp\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    sub $8, %rsp\n"
    "    call *%rsi\n"
    "    add $8, %rsp\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %rbp\n"
    "    pop %rbx\n"
    "    ret\n"
    ".size pw_x86_run_block,.-pw_x86_run_block\n"
    /* The same with the guest's FP state (an FXSAVE image, 16-byte aligned)
     * in the host FPU while the block runs; the host's MXCSR and x87
     * control word come back after, with an empty x87 stack. */
    ".globl pw_x86_run_block_fp\n"
    ".type pw_x86_run_block_fp,@function\n"
    "pw_x86_run_block_fp:\n"
    "    push %rbx\n"
    "    push %rbp\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    sub $24, %rsp\n"
    "    mov %rdx, (%rsp)\n"
    "    stmxcsr 8(%rsp)\n"
    "    fnstcw 12(%rsp)\n"
    "    fxrstor (%rdx)\n"
    "    call *%rsi\n"
    "    mov (%rsp), %rdx\n"
    "    fxsave (%rdx)\n"
    "    fninit\n"
    "    fldcw 12(%rsp)\n"
    "    ldmxcsr 8(%rsp)\n"
    "    add $24, %rsp\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %rbp\n"
    "    pop %rbx\n"
    "    ret\n"
    ".size pw_x86_run_block_fp,.-pw_x86_run_block_fp\n");

static inline int get_resident_host_reg(const PwX86RegContract *c, unsigned gpr)
{
    if (!c || gpr >= 8 || !(c->resident_mask & (1 << gpr))) return -1;
    return c->guest_to_host[gpr];
}

static inline unsigned popcount8(uint8_t m)
{
    unsigned count = 0;
    while (m) { count += (m & 1); m >>= 1; }
    return count;
}

static void emit_spill_dirty(Emitter *e, const PwX86RegContract *c)
{
    if (!c) return;
    for (unsigned g = 0; g < 8; g++) {
        if ((c->resident_mask & (1 << g)) && (c->dirty_mask & (1 << g))) {
            int h = c->guest_to_host[g];
            if (h >= 0 && h < PW_X86_MAX_HOST_REGS) {
                byte(e, 0x44); byte(e, 0x89);
                byte(e, (uint8_t)(0x47 | (h << 3)));
                byte(e, (uint8_t)(g * 4));
            }
        }
    }
}

__attribute__((unused))
static void emit_spill_single(Emitter *e, PwX86RegContract *c, unsigned gpr)
{
    if (!c || gpr >= 8 || !(c->resident_mask & (1 << gpr))) return;
    if (c->dirty_mask & (1 << gpr)) {
        int h = c->guest_to_host[gpr];
        if (h >= 0 && h < PW_X86_MAX_HOST_REGS) {
            byte(e, 0x44); byte(e, 0x89);
            byte(e, (uint8_t)(0x47 | (h << 3)));
            byte(e, (uint8_t)(gpr * 4));
            c->dirty_mask &= ~(1 << gpr);
        }
    }
}

static void emit_load_single(Emitter *e, const PwX86RegContract *c, unsigned gpr)
{
    if (!c || gpr >= 8 || !(c->resident_mask & (1 << gpr))) return;
    int h = c->guest_to_host[gpr];
    if (h >= 0 && h < PW_X86_MAX_HOST_REGS) {
        byte(e, 0x44); byte(e, 0x8b);
        byte(e, (uint8_t)(0x47 | (h << 3)));
        byte(e, (uint8_t)(gpr * 4));
    }
}

static void emit_load_all_resident(Emitter *e, const PwX86RegContract *c)
{
    if (!c) return;
    for (unsigned g = 0; g < 8; g++) {
        if (c->resident_mask & (1 << g)) {
            emit_load_single(e, c, g);
        }
    }
}

static void load_guest_reg(Emitter *e, const PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x44); byte(e, 0x89); byte(e, (uint8_t)(0xc0 | (h << 3)));
    } else {
        load_eax(e, gpr * 4);
    }
}

static void store_guest_reg(Emitter *e, PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x41); byte(e, 0x89); byte(e, (uint8_t)(0xc0 | h));
        if (c) c->dirty_mask |= (1 << gpr);
    } else {
        store_eax(e, gpr * 4);
    }
}

static void store_guest_imm(Emitter *e, PwX86RegContract *c, unsigned gpr, uint32_t val)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x41); byte(e, (uint8_t)(0xb8 + h)); word(e, val);
        if (c) c->dirty_mask |= (1 << gpr);
    } else {
        store(e, gpr * 4, val);
    }
}

/* Emit a guest register-to-register ALU operation without round-tripping a
 * resident operand through EAX or canonical state.  The opcode is the normal
 * group number (ADD=0 .. CMP=7); CMP passes write_destination=0. */
static int emit_guest_binary32(Emitter *e, PwX86RegContract *c,
                               unsigned operation, unsigned destination,
                               unsigned source, unsigned write_destination)
{
    int destination_host=get_resident_host_reg(c,destination);
    int source_host=get_resident_host_reg(c,source);

    if(destination_host>=0) {
        if(source_host>=0) {
            byte(e,0x45); /* REX.R | REX.B */
            byte(e,(uint8_t)(operation*8u+1u));
            byte(e,(uint8_t)(0xc0u|((unsigned)source_host<<3)|
                                  (unsigned)destination_host));
        } else {
            byte(e,0x44); /* REX.R: destination is r8d..r10d */
            byte(e,(uint8_t)(operation*8u+3u));
            byte(e,(uint8_t)(0x47u|((unsigned)destination_host<<3)));
            byte(e,(uint8_t)(source*4u));
        }
        if(write_destination)c->dirty_mask|=(uint8_t)(1u<<destination);
        return 1;
    }
    if(source_host>=0) {
        byte(e,0x44); /* REX.R: source is r8d..r10d */
        byte(e,(uint8_t)(operation*8u+1u));
        byte(e,(uint8_t)(0x47u|((unsigned)source_host<<3)));
        byte(e,(uint8_t)(destination*4u));
        return 1;
    }
    return 0;
}

static void load_guest_reg_edx(Emitter *e, const PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x44); byte(e, 0x89); byte(e, (uint8_t)(0xc2 | (h << 3)));
    } else {
        byte(e, 0x8b); byte(e, 0x57); byte(e, (uint8_t)(gpr * 4));
    }
}

/*
 * Guest XMM state lives in PwGuestFp.xmm like every other piece of guest
 * state, so the emitted code moves it through host XMM0/XMM1 and lets the
 * host instruction provide the semantics (including the upper-bit zeroing of
 * movd, movss and movsd). Host XMM registers are caller-saved scratch in this
 * ABI and are never part of a guest register contract.
 */
static void xmm_disp(Emitter *e, unsigned reg, unsigned extra)
{
    const uint32_t offset = (uint32_t)(offsetof(PwX86State, fp) +
                                       offsetof(PwGuestFp, xmm) +
                                       reg * 16u + extra);
    word(e, offset);
}
static void load_guest_xmm(Emitter *e, unsigned reg)
{
    byte(e, 0xf3); byte(e, 0x0f); byte(e, 0x6f); byte(e, 0x87);
    xmm_disp(e, reg, 0u);
}
static void load_guest_xmm1(Emitter *e, unsigned reg)
{
    byte(e, 0xf3); byte(e, 0x0f); byte(e, 0x6f); byte(e, 0x8f);
    xmm_disp(e, reg, 0u);
}
static void store_guest_xmm(Emitter *e, unsigned reg)
{
    byte(e, 0xf3); byte(e, 0x0f); byte(e, 0x7f); byte(e, 0x87);
    xmm_disp(e, reg, 0u);
}

static void load_guest_reg_ecx(Emitter *e, const PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x44); byte(e, 0x89); byte(e, (uint8_t)(0xc1 | (h << 3)));
    } else {
        byte(e, 0x8b); byte(e, 0x4f); byte(e, (uint8_t)(gpr * 4));
    }
}

static void store_guest_reg_edx(Emitter *e, PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x41); byte(e, 0x89); byte(e, (uint8_t)(0xd0 | h));
        if (c) c->dirty_mask |= (1 << gpr);
    } else {
        byte(e, 0x89); byte(e, 0x57); byte(e, (uint8_t)(gpr * 4));
    }
}

static void store_guest_reg_ecx(Emitter *e, PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x41); byte(e, 0x89); byte(e, (uint8_t)(0xc8 | h));
        if (c) c->dirty_mask |= (1 << gpr);
    } else {
        byte(e, 0x89); byte(e, 0x4f); byte(e, (uint8_t)(gpr * 4));
    }
}

__attribute__((unused))
static void load_guest_reg16(Emitter *e, const PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x66); byte(e, 0x44); byte(e, 0x89); byte(e, (uint8_t)(0xc0 | (h << 3)));
    } else {
        byte(e, 0x66); byte(e, 0x8b); byte(e, 0x47); byte(e, (uint8_t)(gpr * 4));
    }
}

__attribute__((unused))
static void store_guest_reg16(Emitter *e, PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x66); byte(e, 0x41); byte(e, 0x89); byte(e, (uint8_t)(0xc0 | h));
        if (c) c->dirty_mask |= (1 << gpr);
    } else {
        byte(e, 0x66); byte(e, 0x89); byte(e, 0x47); byte(e, (uint8_t)(gpr * 4));
    }
}

static void store_guest_reg8_low(Emitter *e, PwX86RegContract *c, unsigned gpr)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x41); byte(e, 0x88); byte(e, (uint8_t)(0xc0 | h));
        if (c) c->dirty_mask |= (1 << gpr);
    } else {
        byte(e, 0x88); byte(e, 0x47); byte(e, (uint8_t)(gpr * 4));
    }
}

/* Load one of AL..BH into a zero-extended host scratch register without
 * consulting a stale canonical dword when its guest owner is resident. */
static void load_guest_byte_eax(Emitter *e, const PwX86RegContract *c,
                                unsigned byte_reg)
{
    unsigned gpr=byte_reg&3u,high=byte_reg>=4u;
    int h=get_resident_host_reg(c,gpr);
    if(h>=0) {
        if(high) {
            load_guest_reg(e,c,gpr);
            byte(e,0xc1);byte(e,0xe8);byte(e,8); /* shr eax, 8 */
            byte(e,0x0f);byte(e,0xb6);byte(e,0xc0); /* movzx eax, al */
        } else {
            byte(e,0x41);byte(e,0x0f);byte(e,0xb6);
            byte(e,(uint8_t)(0xc0|h));
        }
    } else {
        byte(e,0x0f);byte(e,0xb6);byte(e,0x47);
        byte(e,(uint8_t)(gpr*4u+high));
    }
}

static void load_guest_byte_ecx(Emitter *e, const PwX86RegContract *c,
                                unsigned byte_reg)
{
    unsigned gpr=byte_reg&3u,high=byte_reg>=4u;
    int h=get_resident_host_reg(c,gpr);
    if(h>=0) {
        if(high) {
            load_guest_reg_ecx(e,c,gpr);
            byte(e,0xc1);byte(e,0xe9);byte(e,8); /* shr ecx, 8 */
            byte(e,0x0f);byte(e,0xb6);byte(e,0xc9); /* movzx ecx, cl */
        } else {
            byte(e,0x41);byte(e,0x0f);byte(e,0xb6);
            byte(e,(uint8_t)(0xc8|h));
        }
    } else {
        byte(e,0x0f);byte(e,0xb6);byte(e,0x4f);
        byte(e,(uint8_t)(gpr*4u+high));
    }
}

/* Store AL into one of AL..BH.  The caller snapshots arithmetic flags before
 * this helper because merging a high byte uses integer mask operations. */
static void store_guest_byte_eax(Emitter *e, PwX86RegContract *c,
                                 unsigned byte_reg)
{
    unsigned gpr=byte_reg&3u,high=byte_reg>=4u;
    int h=get_resident_host_reg(c,gpr);
    if(h>=0) {
        if(!high) {
            store_guest_reg8_low(e,c,gpr);
            return;
        }
        byte(e,0xc1);byte(e,0xe0);byte(e,8); /* shl eax, 8 */
        load_guest_reg_edx(e,c,gpr);
        byte(e,0x81);byte(e,0xe2);word(e,0xffff00ff);
        byte(e,0x09);byte(e,0xc2);
        store_guest_reg_edx(e,c,gpr);
        return;
    }
    if(!high) {
        byte(e,0x88);byte(e,0x47);byte(e,(uint8_t)(gpr*4u));
        return;
    }
    byte(e,0xc1);byte(e,0xe0);byte(e,8); /* shl eax, 8 */
    byte(e,0x8b);byte(e,0x57);byte(e,(uint8_t)(gpr*4u));
    byte(e,0x81);byte(e,0xe2);word(e,0xffff00ff);
    byte(e,0x09);byte(e,0xc2);
    byte(e,0x89);byte(e,0x57);byte(e,(uint8_t)(gpr*4u));
}

static void emit_chain_exit(Emitter *e, uint32_t count, uint32_t target_pc,
                            const PwX86RegContract *contract,
                            size_t *patch_offset, size_t *stub_offset,
                            size_t *reconcile_offset, size_t *reconcile_patch_offset)
{
    uint8_t n_dirty = contract ? popcount8(contract->dirty_mask) : 0;

    /* 1. add dword ptr [rdi + step_retired], count */
    if (!e->no_counters) { byte(e, 0x83); byte(e, 0x47); byte(e, offsetof(PwX86State, step_retired)); byte(e, (uint8_t)count); }
    /* 2. dec dword ptr [rdi + chain_budget] */
    byte(e, 0xff); byte(e, 0x4f); byte(e, offsetof(PwX86State, chain_budget));
    /* 3. jz safepoint */
    byte(e, 0x74);
    size_t safepoint_patch = e->n++;
    /* 4. inc dword ptr [rdi + step_transitions] */
    if (!e->no_counters) { byte(e, 0xff); byte(e, 0x47); byte(e, offsetof(PwX86State, step_transitions)); }
    /* 5. movabs $0, %r11 (10 bytes: 49 bb <8 bytes>) */
    byte(e, 0x49); byte(e, 0xbb);
    *patch_offset = e->n;
    for (int i = 0; i < 8; i++) byte(e, 0);
    /* 6. test %r11, %r11 (3 bytes: 4d 85 db) */
    byte(e, 0x4d); byte(e, 0x85); byte(e, 0xdb);
    /* 7. jz unlinked_stub */
    byte(e, 0x74);
    size_t unlinked_patch = e->n++;
    /* 8. jmp *(%r11) (3 bytes: 41 ff 23) */
    byte(e, 0x41); byte(e, 0xff); byte(e, 0x23);

    /* 9. safepoint_exit: */
    e->p[safepoint_patch] = (uint8_t)(e->n - (safepoint_patch + 1));
    emit_spill_dirty(e, contract);
    if (n_dirty) {
        if (!e->no_counters) { byte(e, 0x83); byte(e, 0x47); byte(e, offsetof(PwX86State, reg_stores)); byte(e, n_dirty); }
    }
    byte(e, 0x48); byte(e, 0xc7); byte(e, 0x47); byte(e, offsetof(PwX86State, last_exit_slot));
    word(e, 0);
    /* jmp common_exit */
    byte(e, 0xeb);
    size_t safepoint_common_patch = e->n++;

    /* 10. unlinked_stub: */
    *stub_offset = e->n;
    e->p[unlinked_patch] = (uint8_t)(e->n - (unlinked_patch + 1));
    emit_spill_dirty(e, contract);
    if (n_dirty) {
        if (!e->no_counters) { byte(e, 0x83); byte(e, 0x47); byte(e, offsetof(PwX86State, reg_stores)); byte(e, n_dirty); }
    }
    byte(e, 0x4c); byte(e, 0x89); byte(e, 0x5f); byte(e, offsetof(PwX86State, last_exit_slot));

    /* 11. common_exit: */
    e->p[safepoint_common_patch] = (uint8_t)(e->n - (safepoint_common_patch + 1));
    store(e, offsetof(PwX86State, eip), target_pc);
    byte(e, 0x31); byte(e, 0xc0); byte(e, 0xc3); /* xor eax, eax; ret */

    /* 12. reconcile_stub: jumped to when linked block has different contract */
    *reconcile_offset = e->n;
    emit_spill_dirty(e, contract);
    if (n_dirty) {
        if (!e->no_counters) { byte(e, 0x83); byte(e, 0x47); byte(e, offsetof(PwX86State, reg_spills)); byte(e, n_dirty); }
    }
    /* inc dword ptr [rdi + reg_reconciliations] */
    if (!e->no_counters) { byte(e, 0xff); byte(e, 0x47); byte(e, offsetof(PwX86State, reg_reconciliations)); }
    /* movabs $link_slot->canonical_code, %r11 */
    byte(e, 0x49); byte(e, 0xbb);
    *reconcile_patch_offset = e->n;
    for (int i = 0; i < 8; i++) byte(e, 0);
    /* jmp *(%r11) */
    byte(e, 0x41); byte(e, 0xff); byte(e, 0x23);
}

/*
 * The dynamic exit's lookup of state->eip in the indirect table (see
 * pw_x86_block.h). Every guest register is already in PwX86State and the
 * block's instructions are already retired, so a hit enters the target's
 * canonical entry as a linked exit would, and anything else falls through to
 * the return that follows.
 */
static void emit_indirect_lookup(Emitter *e, const PwX86IndirectTarget *table, uint32_t mask)
{
    size_t budget_patch, miss_patch, empty_patch;
    uint64_t base = (uint64_t)(uintptr_t)table;

    byte(e, 0xff); byte(e, 0x4f); byte(e, offsetof(PwX86State, chain_budget)); /* dec budget */
    byte(e, 0x74); byte(e, 0); budget_patch = e->n - 1;                                       /* jz out */
    byte(e, 0x8b); byte(e, 0x47); byte(e, offsetof(PwX86State, eip));        /* mov eax, eip */
    byte(e, 0x89); byte(e, 0xc2);                                              /* mov edx, eax */
    byte(e, 0xc1); byte(e, 0xea); byte(e, 12);                                 /* shr edx, 12 */
    byte(e, 0x31); byte(e, 0xc2);                                              /* xor edx, eax */
    byte(e, 0x81); byte(e, 0xe2); word(e, mask);                               /* and edx, mask */
    byte(e, 0xc1); byte(e, 0xe2); byte(e, 4);                                  /* shl edx, 4 */
    byte(e, 0x49); byte(e, 0xbb); word(e, (uint32_t)base); word(e, (uint32_t)(base >> 32));
    byte(e, 0x49); byte(e, 0x01); byte(e, 0xd3);                               /* add r11, rdx */
    byte(e, 0x41); byte(e, 0x3b); byte(e, 0x03);                               /* cmp eax, [r11] */
    byte(e, 0x75); byte(e, 0); miss_patch = e->n - 1;                                        /* jne out */
    byte(e, 0x4d); byte(e, 0x8b); byte(e, 0x5b);
    byte(e, (uint8_t)offsetof(PwX86IndirectTarget, host_code));                /* mov r11, [r11+8] */
    byte(e, 0x4d); byte(e, 0x85); byte(e, 0xdb);                               /* test r11, r11 */
    byte(e, 0x74); byte(e, 0); empty_patch = e->n - 1;                                       /* jz out */
    if (!e->no_counters) { byte(e, 0xff); byte(e, 0x47); byte(e, offsetof(PwX86State, step_transitions)); }
    byte(e, 0x41); byte(e, 0xff); byte(e, 0xe3);                               /* jmp r11 */
    if (e->failed) return;
    e->p[budget_patch] = (uint8_t)(e->n - (budget_patch + 1));
    e->p[miss_patch] = (uint8_t)(e->n - (miss_patch + 1));
    e->p[empty_patch] = (uint8_t)(e->n - (empty_patch + 1));
}

static void emit_materialize_flags(Emitter *e, uint32_t mask);
static uint32_t branch_condition_flags(unsigned condition);

static void conditional_target(Emitter *e, unsigned condition, uint32_t next, uint32_t target,
                               PwX86Block *block, uint32_t count, uint32_t rcx_flags,
                               unsigned host_flags)
{
    unsigned c = condition >> 1;
    unsigned invert = condition & 1;
    uint8_t jump_op = invert ? 0x84 : 0x85;
    size_t branch_patch = 0;
    static const uint32_t single[6] = { 0x800, 0x001, 0x040, 0x041, 0x080, 0x004 };
    const uint32_t needed = branch_condition_flags(condition);
    if (host_flags) {
        /* The host flags are still the producer's: branch on them. */
        byte(e,0x0f);byte(e,(uint8_t)(0x80u + condition)); branch_patch = e->n; word(e, 0);
    } else if ((needed & ~rcx_flags) == 0) {
        /* The instruction just before produced every flag the condition
         * reads, and rcx still holds them: test them there instead of
         * merging the pending flags into EFLAGS first. The pending flags
         * stay pending, exactly as they were. */
        if (c < 6) {
            byte(e,0xf7);byte(e,0xc1);word(e,single[c]);        /* test ecx, bits */
        } else {
            byte(e,0x89);byte(e,0xc8);                          /* mov eax, ecx */
            byte(e,0xc1);byte(e,0xe8);byte(e,4);                /* shr eax, 4: OF to bit 7 */
            byte(e,0x31);byte(e,0xc8);                          /* xor eax, ecx: SF ^ OF */
            byte(e,0x25);word(e,0x80);                          /* and eax, 0x80 */
            if (c == 7) {
                byte(e,0x83);byte(e,0xe1);byte(e,0x40);         /* and ecx, ZF */
                byte(e,0x09);byte(e,0xc8);                      /* or eax, ecx */
            }
        }
        byte(e,0x0f);byte(e,jump_op); branch_patch = e->n; word(e, 0);
    } else {
    emit_materialize_flags(e,needed);
    if (c == 0) { /* OF: bit 11 in eflags (bit 3 of byte [rdi+53]) */
        byte(e,0xf6);byte(e,0x47);byte(e,offsetof(PwX86State,eflags)+1);byte(e,0x08);
        byte(e,0x0f);byte(e,jump_op); branch_patch = e->n; word(e, 0);
    } else if (c == 1) { /* CF: bit 0 in eflags (byte [rdi+52]) */
        byte(e,0xf6);byte(e,0x47);byte(e,offsetof(PwX86State,eflags));byte(e,0x01);
        byte(e,0x0f);byte(e,jump_op); branch_patch = e->n; word(e, 0);
    } else if (c == 2) { /* ZF: bit 6 in eflags (byte [rdi+52]) */
        byte(e,0xf6);byte(e,0x47);byte(e,offsetof(PwX86State,eflags));byte(e,0x40);
        byte(e,0x0f);byte(e,jump_op); branch_patch = e->n; word(e, 0);
    } else if (c == 3) { /* CF || ZF: bits 0, 6 in eflags (byte [rdi+52]) */
        byte(e,0xf6);byte(e,0x47);byte(e,offsetof(PwX86State,eflags));byte(e,0x41);
        byte(e,0x0f);byte(e,jump_op); branch_patch = e->n; word(e, 0);
    } else if (c == 4) { /* SF: bit 7 in eflags (byte [rdi+52]) */
        byte(e,0xf6);byte(e,0x47);byte(e,offsetof(PwX86State,eflags));byte(e,0x80);
        byte(e,0x0f);byte(e,jump_op); branch_patch = e->n; word(e, 0);
    } else if (c == 5) { /* PF: bit 2 in eflags (byte [rdi+52]) */
        byte(e,0xf6);byte(e,0x47);byte(e,offsetof(PwX86State,eflags));byte(e,0x04);
        byte(e,0x0f);byte(e,jump_op); branch_patch = e->n; word(e, 0);
    } else {
        condition_value(e,condition);
        byte(e,0x85);byte(e,0xc0);
        byte(e,0x0f);byte(e,0x85); branch_patch = e->n; word(e, 0);
    }
    }
    emit_chain_exit(e, count, next, &block->exit_contract,
                    &block->exit.fallthrough_patch_offset, &block->exit.fallthrough_stub_offset,
                    &block->exit.fallthrough_reconcile_offset, &block->exit.fallthrough_reconcile_patch_offset);
    uint32_t disp = (uint32_t)(e->n - (branch_patch + 4));
    e->p[branch_patch] = (uint8_t)disp;
    e->p[branch_patch+1] = (uint8_t)(disp >> 8);
    e->p[branch_patch+2] = (uint8_t)(disp >> 16);
    e->p[branch_patch+3] = (uint8_t)(disp >> 24);
    emit_chain_exit(e, count, target, &block->exit_contract,
                    &block->exit.target_patch_offset, &block->exit.target_stub_offset,
                    &block->exit.target_reconcile_offset, &block->exit.target_reconcile_patch_offset);
}

static void stack_address(Emitter *e, int push, const PwX86RegContract *c)
{
    load_guest_reg(e, c, 4);
    if (push) {
        /* Check before subtracting: an ESP of 0 must not wrap. */
        byte(e,0x83); byte(e,0xf8); byte(e,4);
        require_condition(e,0x73); /* jae */
        byte(e,0x83); byte(e,0xe8); byte(e,4);
    }
    stack_bounds(e);
}
typedef struct Operand {
    unsigned reg, rm, mod, scale;
    int base, index;
    uint32_t displacement;
    size_t bytes;
} Operand;
static int decode_operand(const uint8_t *p, size_t n, Operand *o)
{
    if (!n) return PW_ERR_TRUNCATED;
    memset(o,0,sizeof(*o));
    o->mod=p[0]>>6; o->reg=(p[0]>>3)&7; o->rm=p[0]&7;
    o->base=(int)o->rm; o->index=-1; o->bytes=1;
    if (o->mod==3) return PW_OK;
    unsigned displacement=o->mod==1 ? 1 : o->mod==2 ? 4 : 0;
    if (o->rm==4) {
        if (n<2) return PW_ERR_TRUNCATED;
        o->bytes=2; o->scale=p[1]>>6;
        o->index=(p[1]>>3)&7; o->base=p[1]&7;
        if (o->index==4) o->index=-1;
    }
    if (o->mod==0 && o->base==5) { o->base=-1; displacement=4; }
    if (n-o->bytes < displacement) return PW_ERR_TRUNCATED;
    if (displacement==1) o->displacement=(uint32_t)(int32_t)(int8_t)p[o->bytes];
    else if (displacement==4) o->displacement=read32(p+o->bytes);
    o->bytes+=displacement;
    return PW_OK;
}
/* host = guest register gpr, for host eax (0) or edx (2). */
static void load_address_reg(Emitter *e, const PwX86RegContract *c, unsigned gpr, unsigned host)
{
    int h = get_resident_host_reg(c, gpr);
    if (h >= 0) {
        byte(e, 0x44); byte(e, 0x89); byte(e, (uint8_t)(0xc0 | ((unsigned)h << 3) | host));
    } else {
        byte(e, 0x8b); byte(e, (uint8_t)(0x47 | (host << 3))); byte(e, (uint8_t)(gpr * 4));
    }
}
static void effective_address(Emitter *e, const Operand *o, const PwX86RegContract *c)
{
    /* The address in eax, wrapping at 32 bits: lea with a 32-bit destination
     * keeps the low half of the 64-bit sum. Never RIP-relative. */
    if (o->base < 0 && o->index < 0) {
        byte(e,0xb8); word(e,o->displacement);
        return;
    }
    if (o->base >= 0) load_address_reg(e, c, (unsigned)o->base, 0);
    if (o->index < 0) {
        if (!o->displacement) return;
        if ((int32_t)o->displacement >= -128 && (int32_t)o->displacement <= 127) {
            byte(e,0x8d); byte(e,0x40); byte(e,(uint8_t)o->displacement); /* lea eax,[rax+d8] */
        } else {
            byte(e,0x8d); byte(e,0x80); word(e,o->displacement);          /* lea eax,[rax+d32] */
        }
        return;
    }
    load_address_reg(e, c, (unsigned)o->index, 2);
    if (o->base >= 0) {
        /* lea eax,[rax+rdx*scale+disp32] */
        byte(e,0x8d); byte(e,0x84); byte(e,(uint8_t)((o->scale << 6) | (2u << 3)));
    } else {
        /* lea eax,[rdx*scale+disp32] */
        byte(e,0x8d); byte(e,0x04); byte(e,(uint8_t)((o->scale << 6) | (2u << 3) | 5u));
    }
    word(e,o->displacement);
}
static void push_imm(Emitter *e, uint32_t value, PwX86RegContract *c)
{
    stack_address(e, 1, c);
    byte(e,0xc7); byte(e,0x00); word(e,value); /* mov dword [rax],imm32 */
    store_guest_reg(e, c, 4);
}
static void success(Emitter *e)
{
    byte(e,0x31); byte(e,0xc0); byte(e,0xc3);
}
uint32_t pw_x86_compute_canonical_flags(const PwX86DeferredFlags *df, uint32_t prev_eflags)
{
    if (!df) return prev_eflags;
    return (prev_eflags & ~df->known_mask) |
           (df->raw_flags & df->known_mask);
}

void pw_x86_materialize_flag_bits(PwX86State *state, uint32_t demand_mask)
{
    uint32_t mask = demand_mask & state->deferred_flags.known_mask;
    state->eflags = (state->eflags & ~mask) |
                    (state->deferred_flags.raw_flags & mask);
}

void pw_x86_commit_canonical_flags(PwX86State *state)
{
    if (!state->deferred_flags.known_mask) return;
    state->eflags = pw_x86_compute_canonical_flags(&state->deferred_flags, state->eflags);
    state->deferred_flags.known_mask = 0;
}

static void emit_materialize_flags(Emitter *e,uint32_t mask)
{
    if(!mask)return;
    /* Merge only demanded pending bits into canonical guest EFLAGS.  This is
     * branch-free and does not cross the C ABI, so resident r8-r10 survive. */
    load_eax(e,offsetof(PwX86State,eflags));
    byte(e,0x8b);byte(e,0x97);
    word(e,(uint32_t)offsetof(PwX86State,deferred_flags.known_mask));
    byte(e,0x81);byte(e,0xe2);word(e,mask);
    byte(e,0x8b);byte(e,0x8f);word(e,(uint32_t)offsetof(PwX86State,deferred_flags.raw_flags));
    byte(e,0x31);byte(e,0xc1); /* ecx = raw ^ canonical */
    byte(e,0x21);byte(e,0xd1); /* retain changed demanded bits */
    byte(e,0x31);byte(e,0xc8); /* merge into eax */
    store_eax(e,offsetof(PwX86State,eflags));
}

static void emit_commit_flags(Emitter *e)
{
    emit_materialize_flags(e,0x8d5);
    byte(e,0xc7);byte(e,0x87);
    word(e,(uint32_t)offsetof(PwX86State,deferred_flags.known_mask));word(e,0);
}

static void save_arithmetic_flags(Emitter *e,uint32_t mask)
{
    /* Snapshot before any emitter bookkeeping changes flags. Native RSP is
     * balanced; guest control flags (DF/IF/etc.) are never loaded into RFLAGS. */
    byte(e,0x9c); byte(e,0x59); /* pushfq; pop rcx */
    byte(e,0x81); byte(e,0xe1); word(e,mask);
    byte(e,0x8b); byte(e,0x57); byte(e,offsetof(PwX86State,eflags));
    byte(e,0x81); byte(e,0xe2); word(e,~mask);
    byte(e,0x09); byte(e,0xca);
    byte(e,0x89); byte(e,0x57); byte(e,offsetof(PwX86State,eflags));
    e->rcx_flags_end = e->n + 1;
    e->rcx_flags_mask = mask;
}

static void load_edx_disp32(Emitter *e,size_t offset)
{
    byte(e,0x8b);byte(e,0x97);word(e,(uint32_t)offset);
}

static void store_edx_disp32(Emitter *e,size_t offset)
{
    byte(e,0x89);byte(e,0x97);word(e,(uint32_t)offset);
}

static void store_imm_disp32(Emitter *e,size_t offset,uint32_t value)
{
    byte(e,0xc7);byte(e,0x87);word(e,(uint32_t)offset);word(e,value);
}

static void defer_arithmetic_flags(Emitter *e,uint32_t mask)
{
    const size_t result_offset=offsetof(PwX86State,deferred_flags.raw_flags);
    const size_t known_offset=offsetof(PwX86State,deferred_flags.known_mask);

    byte(e,0x9c);byte(e,0x59);                 /* pushfq; pop rcx */
    if(mask==0x8d5) {
        /* Every deferred arithmetic bit is replaced, so no merge is needed.
         * Nothing here changes the host flags. */
        byte(e,0x89);byte(e,0x8f);word(e,(uint32_t)result_offset);
        store_imm_disp32(e,known_offset,mask);
        e->host_flags_end = e->n + 1;
    } else {
        byte(e,0x81);byte(e,0xe1);word(e,mask); /* and ecx, mask */
        load_edx_disp32(e,result_offset);
        byte(e,0x81);byte(e,0xe2);word(e,~mask);
        byte(e,0x09);byte(e,0xca);
        store_edx_disp32(e,result_offset);
        byte(e,0x81);byte(e,0x8f);word(e,(uint32_t)known_offset);word(e,mask);
    }
    e->rcx_flags_end = e->n + 1;
    e->rcx_flags_mask = mask;
}

static void emit_save_flags(Emitter *e, uint32_t mask, unsigned lazy_flags_enabled,
                            int flags_dead)
{
    if (flags_dead) return;
    if (!lazy_flags_enabled)
        save_arithmetic_flags(e, mask);
    else
        defer_arithmetic_flags(e,mask);
}
static void fs_address(Emitter *e, uint32_t offset)
{
    /* Check offset <= size-4 without wraparound. */
    load_eax(e,offsetof(PwX86State,fs_bytes));
    byte(e,0x83); byte(e,0xf8); byte(e,4); require_condition(e,0x73);
    byte(e,0x83); byte(e,0xe8); byte(e,4);
    byte(e,0x3d); word(e,offset); require_condition(e,0x73);
    load_eax(e,offsetof(PwX86State,fs_base));
    byte(e,0x05); word(e,offset); require_condition(e,0x73); /* no carry */
    /* The last byte of the dword must remain in the 32-bit address space. */
    byte(e,0x3d); word(e,0xfffffffcu); require_condition(e,0x76);
}
static int x87_dispatch(PwX86State *state,unsigned action,uintptr_t operand)
{
    uint16_t ax=0;int status=pw_x87_execute(&state->fp,(PwX87Action)action,operand,&ax);
    if(status==PW_OK && action==PW_X87_FNSTSW_AX)
        state->gpr[0]=(state->gpr[0]&0xffff0000u)|ax;
    return status;
}
static void x87_call(Emitter *e,unsigned action,unsigned register_operand)
{
    if(register_operand){byte(e,0xba);word(e,register_operand-1);}
    else {byte(e,0x48);byte(e,0x89);byte(e,0xc2);}
    byte(e,0xbe);word(e,action);byte(e,0x57);byte(e,0x48);byte(e,0xb8);
    uint64_t target=(uint64_t)(uintptr_t)&x87_dispatch;
    word(e,(uint32_t)target);word(e,(uint32_t)(target>>32));
    byte(e,0xff);byte(e,0xd0);byte(e,0x5f);byte(e,0x85);byte(e,0xc0);
    e->exits++;byte(e,0x74);byte(e,1);byte(e,0xc3); /* propagate helper failure */
}
static int string_dispatch(PwX86State *state,unsigned opcode,unsigned width,unsigned repeat)
{
    if(!state || (width!=1 && width!=2 && width!=4) ||
       !((opcode>=0xa4 && opcode<=0xa7) || opcode==0xaa || opcode==0xab))
        return PW_ERR_PRECONDITION;
    uint32_t count=repeat?state->gpr[1]:1;
    if(!count)return PW_OK;
    uint64_t span=(uint64_t)count*width;
    if(span>0x100000000ull)return PW_ERR_VM;
    unsigned backwards=!!(state->eflags&0x400);uint32_t destination=state->gpr[7],source=state->gpr[6];
    uint32_t dst_low=destination;
    if(backwards) {
        uint64_t retreat=(uint64_t)(count-1)*width;
        if(retreat>destination)return PW_ERR_VM;
        dst_low=destination-(uint32_t)retreat;
    } else if((uint64_t)destination+span>0x100000000ull)return PW_ERR_VM;
    unsigned moving=opcode==0xa4 || opcode==0xa5;
    unsigned comparing=opcode==0xa6 || opcode==0xa7;
    if(!memory_range_pointer(state,dst_low,comparing?0:1,span))return PW_ERR_VM;
    uint32_t src_low=source;
    if(moving || comparing) {
        if(backwards) {
            uint64_t retreat=(uint64_t)(count-1)*width;
            if(retreat>source)return PW_ERR_VM;
            src_low=source-(uint32_t)retreat;
        } else if((uint64_t)source+span>0x100000000ull)return PW_ERR_VM;
        if(!memory_range_pointer(state,src_low,0,span))return PW_ERR_VM;
    }
    for(uint32_t i=0;i<count;i++) {
        uint32_t offset=i*width;
        uint32_t dst=backwards?destination-offset:destination+offset;
        if(moving) {
            uint32_t src=backwards?source-offset:source+offset;
            memmove((void *)(uintptr_t)dst,(const void *)(uintptr_t)src,width);
        } else if(comparing) {
            uint32_t src=backwards?source-offset:source+offset,left=0,right=0;
            memcpy(&left,(const void *)(uintptr_t)src,width);
            memcpy(&right,(const void *)(uintptr_t)dst,width);
            uint32_t mask=width==4?UINT32_MAX:(UINT32_C(1)<<(width*8))-1;
            uint32_t sign=UINT32_C(1)<<(width*8-1),result=(left-right)&mask;
            uint32_t flags=(left<right?1u:0u)|(result==0?0x40u:0u)|
                (result&sign?0x80u:0u)|((left^right^result)&0x10u)|
                (((left^right)&(left^result)&sign)?0x800u:0u);
            unsigned parity=0;for(unsigned bit=0;bit<8;bit++)parity^=(result>>bit)&1u;
            if(!parity)flags|=4;
            state->eflags=(state->eflags&~0x8d5u)|(flags&0x8d5u);
            if(repeat && result) {
                count=i+1;
                break;
            }
        } else memcpy((void *)(uintptr_t)dst,&state->gpr[0],width);
    }
    uint32_t delta=count*width;
    state->gpr[7]=backwards?destination-delta:destination+delta;
    if(moving || comparing)state->gpr[6]=backwards?source-delta:source+delta;
    if(repeat)state->gpr[1]-=count;
    return PW_OK;
}
/*
 * CMPXCHG r/m32, r32 for the memory form, with or without LOCK: the word is
 * compared with the accumulator and the source register is stored into it when
 * they are equal, with the flags of that comparison. The host performs the same
 * operation - one compare-and-swap, which is what "lock" means and what a
 * load/compare/store approximation could not give - and the value the guest
 * then finds in its accumulator is the one the host actually saw.
 *
 * The block has already proved the address through the memory guard and spilled
 * the guest's own register values, so this only performs the operation and
 * leaves EAX and the arithmetic flags as i386 defines them.
 */
static int cmpxchg_dispatch(PwX86State *state, uint32_t *address, unsigned source)
{
    uint32_t accumulator, expected, result, flags;
    unsigned parity = 0u;

    if (!state || !address || source > 7u)
        return PW_ERR_PRECONDITION;
    accumulator = state->gpr[0];
    expected = accumulator;
    (void)__atomic_compare_exchange_n(address, &expected, state->gpr[source], 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    /*
     * C11 leaves the value the word held in `expected` on either path: on
     * success it is still the accumulator, and on failure it is the value the
     * guest must now find in EAX.
     */
    state->gpr[0] = expected;
    /*
     * The flags are those of the accumulator minus the word, which is what a
     * real i386 reports for this instruction: measured against native code in
     * tests/test_pw_x86_reference.S, where an accumulator larger than the word
     * leaves CF clear. Taking the subtraction the other way round gives the
     * opposite CF and SF - the differential test caught exactly that.
     */
    result = accumulator - expected;
    flags = (accumulator < expected ? 1u : 0u) |
            (result == 0u ? 0x40u : 0u) |
            (result & 0x80000000u ? 0x80u : 0u) |
            ((accumulator ^ expected ^ result) & 0x10u) |
            (((accumulator ^ expected) & (accumulator ^ result) & 0x80000000u)
                 ? 0x800u : 0u);
    for (unsigned bit = 0u; bit < 8u; ++bit)
        parity ^= (result >> bit) & 1u;
    if (!parity)
        flags |= 4u;                              /* PF is the low byte's parity */
    state->eflags = (state->eflags & ~0x8d5u) | (flags & 0x8d5u);
    return PW_OK;
}

/* Calls cmpxchg_dispatch with the address the block computed (in RAX) and the
 * index of the source register. */
static void cmpxchg_call(Emitter *e, unsigned source)
{
    byte(e,0x48);byte(e,0x89);byte(e,0xc6);       /* mov rsi, rax */
    byte(e,0xba);word(e,source);                  /* mov edx, source */
    byte(e,0x57);byte(e,0x48);byte(e,0xb8);
    uint64_t target=(uint64_t)(uintptr_t)&cmpxchg_dispatch;
    word(e,(uint32_t)target);word(e,(uint32_t)(target>>32));
    byte(e,0xff);byte(e,0xd0);byte(e,0x5f);       /* call rax; pop rdi */
    byte(e,0x85);byte(e,0xc0);e->exits++;byte(e,0x74);byte(e,1);byte(e,0xc3);
}

/*
 * XCHG r/m32, r32: one exchange of the operand with its register, which is
 * what Wine's heap code uses to take an entry off a free list
 * (heap_thread_detach_bin_groups, dlls/ntdll/heap.c). i386 performs this form
 * atomically whether or not LOCK is written, so the helper performs exactly
 * one host exchange rather than a load/store pair, and - unlike the
 * compare-and-swap - it writes no flags at all, which is why the caller only
 * has to protect the pending lazy flags across the C call rather than let the
 * helper announce new ones.
 */
static int xchg_dispatch(PwX86State *state, uint32_t *address, unsigned source)
{
    uint32_t previous;

    if (!state || !address || source > 7u)
        return PW_ERR_PRECONDITION;
    previous = __atomic_exchange_n(address, state->gpr[source],
                                   __ATOMIC_SEQ_CST);
    state->gpr[source] = previous;
    return PW_OK;
}

/* Calls xchg_dispatch with the address the block computed (in RAX) and the
 * index of the register operand. */
static void xchg_call(Emitter *e, unsigned source)
{
    byte(e,0x48);byte(e,0x89);byte(e,0xc6);       /* mov rsi, rax */
    byte(e,0xba);word(e,source);                  /* mov edx, source */
    byte(e,0x57);byte(e,0x48);byte(e,0xb8);
    uint64_t target=(uint64_t)(uintptr_t)&xchg_dispatch;
    word(e,(uint32_t)target);word(e,(uint32_t)(target>>32));
    byte(e,0xff);byte(e,0xd0);byte(e,0x5f);       /* call rax; pop rdi */
    byte(e,0x85);byte(e,0xc0);e->exits++;byte(e,0x74);byte(e,1);byte(e,0xc3);
}

static void string_call(Emitter *e,unsigned opcode,unsigned width,unsigned repeat)
{
    byte(e,0xbe);word(e,opcode);byte(e,0xba);word(e,width);byte(e,0xb9);word(e,repeat);
    byte(e,0x57);byte(e,0x48);byte(e,0xb8);
    uint64_t target=(uint64_t)(uintptr_t)&string_dispatch;
    word(e,(uint32_t)target);word(e,(uint32_t)(target>>32));
    byte(e,0xff);byte(e,0xd0);byte(e,0x5f);byte(e,0x85);byte(e,0xc0);
    e->exits++;byte(e,0x74);byte(e,1);byte(e,0xc3);
}
static int muldiv_dispatch(PwX86State *state,unsigned action,uint32_t operand)
{
    if(!state || action<4 || action>7)return PW_ERR_PRECONDITION;
    uint32_t eax=state->gpr[0],edx=state->gpr[2],result_eax=0,result_edx=0;
    unsigned overflow=0;
    if(action==4) {
        uint64_t product=(uint64_t)eax*operand;
        result_eax=(uint32_t)product;result_edx=(uint32_t)(product>>32);
        overflow=result_edx!=0;
    } else if(action==5) {
        int64_t product=(int64_t)(int32_t)eax*(int32_t)operand;
        result_eax=(uint32_t)product;result_edx=(uint32_t)((uint64_t)product>>32);
        overflow=product<(int64_t)INT32_MIN || product>(int64_t)INT32_MAX;
    } else if(action==6) {
        if(!operand)return PW_ERR_VM;
        uint64_t dividend=((uint64_t)edx<<32)|eax;
        uint64_t quotient=dividend/operand;
        if(quotient>UINT32_MAX)return PW_ERR_VM;
        result_eax=(uint32_t)quotient;result_edx=(uint32_t)(dividend%operand);
    } else {
        int32_t divisor=(int32_t)operand;
        if(!divisor)return PW_ERR_VM;
        uint64_t bits=((uint64_t)edx<<32)|eax;int64_t dividend;
        memcpy(&dividend,&bits,sizeof(dividend));
        if(dividend==INT64_MIN && divisor==-1)return PW_ERR_VM;
        int64_t quotient=dividend/divisor;
        if(quotient<INT32_MIN || quotient>INT32_MAX)return PW_ERR_VM;
        result_eax=(uint32_t)(int32_t)quotient;
        result_edx=(uint32_t)(int32_t)(dividend%divisor);
    }
    state->gpr[0]=result_eax;state->gpr[2]=result_edx;
    if(action<6) {
        state->eflags&=~0x801u;
        if(overflow)state->eflags|=0x801u;
    }
    return PW_OK;
}
static void muldiv_call(Emitter *e,unsigned action)
{
    byte(e,0x89);byte(e,0xc2);byte(e,0xbe);word(e,action);
    byte(e,0x57);byte(e,0x48);byte(e,0xb8);
    uint64_t target=(uint64_t)(uintptr_t)&muldiv_dispatch;
    word(e,(uint32_t)target);word(e,(uint32_t)(target>>32));
    byte(e,0xff);byte(e,0xd0);byte(e,0x5f);byte(e,0x85);byte(e,0xc0);
    e->exits++;byte(e,0x74);byte(e,1);byte(e,0xc3);
}

static uint32_t branch_condition_flags(unsigned condition)
{
    switch ((condition >> 1) & 7) {
        case 0: return 0x800; /* OF */
        case 1: return 0x001; /* CF */
        case 2: return 0x040; /* ZF */
        case 3: return 0x041; /* CF | ZF */
        case 4: return 0x080; /* SF */
        case 5: return 0x004; /* PF */
        case 6: return 0x880; /* SF | OF */
        case 7: return 0x8c0; /* SF | OF | ZF */
        default: return 0x8d5;
    }
}

typedef struct DecodedInst {
    size_t cursor;
    size_t length;
    uint8_t op;
    Operand operand;
    unsigned compare;
    unsigned alu;
    unsigned short_imm;
    unsigned word_operand;
    unsigned lock_prefix;
    unsigned cmpxchg;               /* 0f b1 to memory: compare with EAX */
    unsigned xchg;                  /* 87 to memory: swap with a register */
    unsigned bit_op;                /* 1 bt, 2 bts, 3 btr, 4 btc; 0 none */
    unsigned bit_imm;               /* the bit index is an imm8 */
    unsigned cmov;                  /* 0f 40..4f: conditional move */
    unsigned cmov_condition;
    unsigned sse_kind;              /* PW_SSE_*: the SSE slice this maps to */
    uint8_t sse_prefix;             /* mandatory prefix byte, 0 when none */
    uint8_t sse_opcode;             /* the 0f map opcode */
    uint32_t sse_mem_bytes;         /* width of a memory operand */
    uint8_t sse_imm;
    unsigned sse_has_imm;
    unsigned bit_scan;              /* 0f bc/bd: BSF or BSR */
    uint8_t double_shift;           /* 0f a4/a5/ac/ad: SHLD or SHRD, or 0 */
    unsigned bswap;                 /* 0f c8+rd: byte order of one register */
    unsigned bswap_reg;
    uint8_t bit_scan_opcode;
    unsigned bit_scan_word;         /* the 0x66 (16-bit) form */
    unsigned bit_scan_count;        /* f3 0f bc/bd: TZCNT or LZCNT */
    unsigned shift_word;            /* 0x66 shift/rotate group */
    unsigned lea_prefixed;          /* a segment override on LEA */
    unsigned fs_call;               /* 64 ff 15: call through fs:[disp32] */
    unsigned conditional;
    unsigned extend;
    unsigned setcc;
    unsigned extend_word_destination;
    unsigned x87, x87_width, x87_write, x87_register;
    unsigned string_op, string_width, string_repeat;
    unsigned word_general;
    unsigned byte_alu, byte_direction;
    unsigned imul_general;
    int terminal;
    uint32_t flags_def;
    uint32_t flags_use;
    int flags_dead;
    int can_fault;
} DecodedInst;

/*
 * SSE forms this translator maps. Every one of them is data movement or a
 * lane shuffle, so none of them writes EFLAGS; guest XMM state lives in
 * PwGuestFp.xmm and host XMM registers are only scratch.
 */
enum {
    PW_SSE_NONE = 0,
    PW_SSE_XMM_RM,      /* xmm <- <op> xmm/m128 (loads, punpck, logic) */
    PW_SSE_STORE_RM,    /* xmm/m128 <- xmm */
    PW_SSE_MOVD_LOAD,   /* xmm <- r/m32, upper 96 bits zeroed */
    PW_SSE_MOVD_STORE,  /* r/m32 <- xmm */
    PW_SSE_MOVQ_STORE,  /* m64 <- xmm */
    PW_SSE_PEXTRW,      /* r32 <- xmm[imm]'s low word */
    PW_SSE_PINSRW,      /* xmm[imm] <- r/m16 */
    PW_SSE_XMM_IMM,     /* xmm <- <op> xmm/m128, imm8 (pshufd and friends) */
    PW_SSE_MOVMSK,      /* r32 <- the sign bits of each lane of xmm/m128 */
};

/* Only the (mandatory prefix, 0f opcode) pairs listed here are accepted, so
 * the MMX encodings that share these opcodes stay refused rather than being
 * executed as something they are not. The scalar register forms (movss and
 * movsd with an xmm destination) are refused too: they merge into the
 * destination's upper bits, which a whole-register copy would get wrong.
 */
static int decode_sse(uint8_t prefix, size_t prefix_bytes,
                      const uint8_t *source, size_t available,
                      Operand *operand, unsigned *kind, uint32_t *mem_bytes,
                      size_t *length, uint8_t *imm, unsigned *has_imm)
{
    uint8_t opcode;
    int result;

    if (available < 2u)
        return PW_ERR_TRUNCATED;
    opcode = source[1];
    result = decode_operand(source + 2u, available - 2u, operand);
    if (result != PW_OK)
        return result;
    *length = prefix_bytes + 2u + operand->bytes;
    *kind = PW_SSE_NONE;
    *mem_bytes = 16u;
    *has_imm = 0u;
    *imm = 0u;
    switch (prefix) {
    case 0x66u:
        if (opcode == 0x6eu) {
            *kind = PW_SSE_MOVD_LOAD;
            *mem_bytes = 4u;
        } else if (opcode == 0x7eu) {
            *kind = PW_SSE_MOVD_STORE;
            *mem_bytes = 4u;
        } else if (opcode == 0xd6u) {
            *kind = PW_SSE_MOVQ_STORE;
            *mem_bytes = 8u;
        } else if (opcode == 0x6fu) {
            *kind = PW_SSE_XMM_RM;
        } else if (opcode == 0x7fu) {
            *kind = PW_SSE_STORE_RM;
        } else if ((opcode >= 0x60u && opcode <= 0x6du) ||
                   (opcode >= 0x74u && opcode <= 0x76u) ||
                   (opcode >= 0xd1u && opcode <= 0xd5u) ||
                   (opcode >= 0xd8u && opcode <= 0xdfu) ||
                   (opcode >= 0xe0u && opcode <= 0xe6u) ||
                   (opcode >= 0xe8u && opcode <= 0xefu) ||
                   (opcode >= 0xf1u && opcode <= 0xf6u) ||
                   (opcode >= 0xf8u && opcode <= 0xfeu)) {
            /*
             * Packed-integer arithmetic, comparison and shifts in their
             * xmm <- <op> xmm/m128 form. The host executes the same
             * instruction on host XMM scratch, so lane widths, saturation and
             * the sign semantics of the comparisons come from the CPU. None
             * of them writes EFLAGS.
             *
             * The opcodes that are a *store* rather than a
             * read-modify-write of the destination are deliberately outside
             * these ranges: 0x7e/0x7f are handled as their own kinds, and
             * 0xd6 (movq store), 0xe7 (movntdq) and 0xf7 (maskmovdqu, which
             * writes through a mask) are refused rather than executed as
             * something they are not.
             */
            *kind = PW_SSE_XMM_RM;
        } else if (opcode == 0xd7u) {
            /* pmovmskb r32, xmm/m128: the sign bit of each of the 16 lanes. */
            *kind = PW_SSE_MOVMSK;
            *mem_bytes = 16u;
        } else if (opcode == 0xc5u) {
            *kind = PW_SSE_PEXTRW;
            *mem_bytes = 2u;
        } else if (opcode == 0xc4u) {
            *kind = PW_SSE_PINSRW;
            *mem_bytes = 2u;
        } else if (opcode == 0x70u) {
            *kind = PW_SSE_XMM_IMM;
        }
        break;
    case 0xf3u:
        if (opcode == 0x70u) {
            *kind = PW_SSE_XMM_IMM;
        } else if (opcode == 0x7eu) {
            /* movq xmm, xmm/m64: the 64-bit load zeroes the upper half. */
            *kind = PW_SSE_XMM_RM;
            *mem_bytes = 8u;
        } else if (opcode == 0x6fu || opcode == 0x10u) {
            *kind = PW_SSE_XMM_RM;
            *mem_bytes = opcode == 0x10u ? 4u : 16u;
        } else if (opcode == 0x7fu || opcode == 0x11u) {
            *kind = PW_SSE_STORE_RM;
            *mem_bytes = opcode == 0x11u ? 4u : 16u;
        }
        break;
    case 0xf2u:
        if (opcode == 0x70u) {
            *kind = PW_SSE_XMM_IMM;
        } else if (opcode == 0x10u) {
            *kind = PW_SSE_XMM_RM;
            *mem_bytes = 8u;
        } else if (opcode == 0x11u) {
            *kind = PW_SSE_STORE_RM;
            *mem_bytes = 8u;
        }
        break;
    default:
        if (opcode == 0x10u || opcode == 0x28u || opcode == 0x54u ||
            opcode == 0x55u || opcode == 0x56u || opcode == 0x57u)
            *kind = PW_SSE_XMM_RM;
        else if (opcode == 0x11u || opcode == 0x29u)
            *kind = PW_SSE_STORE_RM;
        break;
    }
    if (*kind == PW_SSE_NONE)
        return PW_ERR_UNSUPPORTED;
    if (*kind == PW_SSE_PEXTRW || *kind == PW_SSE_PINSRW ||
        *kind == PW_SSE_XMM_IMM) {
        if (available < *length + 1u)
            return PW_ERR_TRUNCATED;
        *imm = source[*length];
        *has_imm = 1u;
        *length += 1u;
    }
    if ((*kind == PW_SSE_XMM_RM || *kind == PW_SSE_STORE_RM) &&
        *mem_bytes < 16u && operand->mod == 3)
        return PW_ERR_UNSUPPORTED;
    /*
     * The two packed moves that *require* 16-byte alignment - movaps (0f
     * 28/29) and movdqa (66 0f 6f/7f) - keep their memory forms because the
     * emitter checks the address first (sse_aligned_move): a misaligned one
     * is the guest's own fault, as the CPU's #GP is, never a host fault.
     */
    /*
     * PMOVMSKB (66 0f d7) and PEXTRW (66 0f c5) have no memory form: their
     * r/m operand is an XMM register, so a ModRM that names memory is not an
     * encoding the CPU accepts. Accepting one here is worse than refusing it,
     * because the emitter would re-emit the same ModRM on the host and the
     * translated block would die on an illegal instruction instead of the
     * guest seeing a classification. The SSE form matrix
     * (tests/test_pw_sse_matrix.py) found exactly that for
     * "66 0f d7 03": sigill in the block, no guest-visible fault.
     */
    if (*kind == PW_SSE_MOVMSK && operand->mod != 3)
        return PW_ERR_UNSUPPORTED;
    return PW_OK;
}

/*
 * Shared decode for the BT/BTS/BTR/BTC family, with or without a 0x66
 * operand-size prefix. `prefix` is the number of bytes before the 0f opcode:
 * 0 or 1. The register-index encodings (0f a3/ab/b3/bb) take the index from
 * the ModRM reg field; 0f ba selects the operation through reg 4..7 and the
 * index from the imm8.
 */
static int decode_bit_test(const uint8_t *source, size_t available,
                           size_t prefix, Operand *operand, unsigned *bit_op,
                           unsigned *bit_imm, size_t *length)
{
    uint8_t sub;
    int result;

    if (available < prefix + 3u)
        return PW_ERR_TRUNCATED;
    sub = source[prefix + 1u];
    result = decode_operand(source + prefix + 2u, available - prefix - 2u,
                            operand);
    if (result != PW_OK)
        return result;
    *length = prefix + 2u + operand->bytes;
    if (sub == 0xbau) {
        if (operand->reg < 4u)
            return PW_ERR_UNSUPPORTED;
        *bit_op = operand->reg - 3u;
        *bit_imm = 1u;
        if (available < *length + 1u)
            return PW_ERR_TRUNCATED;
        *length += 1u;
    } else {
        *bit_op = sub == 0xa3u ? 1u : sub == 0xabu ? 2u
                : sub == 0xb3u ? 3u : 4u;
        *bit_imm = 0u;
    }
    return PW_OK;
}

int pw_x86_translate_ext(const uint8_t *source, size_t bytes, uint32_t pc,
                         uint8_t *output, size_t capacity, PwX86Block *block,
                         unsigned residency_enabled, unsigned lazy_flags_enabled)
{
    const PwX86TranslateOptions options = { residency_enabled, lazy_flags_enabled, NULL, 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0, 0, 0 };

    return pw_x86_translate_opts(source, bytes, pc, output, capacity, block, &options);
}


/* A jump, call or return, which the bnd prefix (F2) may precede. */
static int bnd_branch(const uint8_t *code, size_t bytes)
{
    if (!bytes) return 0;
    if ((code[0] >= 0x70 && code[0] <= 0x7f) || code[0] == 0xe8 || code[0] == 0xe9 ||
        code[0] == 0xeb || code[0] == 0xc2 || code[0] == 0xc3)
        return 1;
    if (bytes >= 2 && code[0] == 0x0f && code[1] >= 0x80 && code[1] <= 0x8f) return 1;
    return bytes >= 2 && code[0] == 0xff && (((code[1] >> 3) & 7) == 2 || ((code[1] >> 3) & 7) == 4);
}

int pw_x86_translate_opts(const uint8_t *source, size_t bytes, uint32_t pc,
                          uint8_t *output, size_t capacity, PwX86Block *block,
                          const PwX86TranslateOptions *options)
{
    Emitter e = {output,0,capacity,0,0,0,0,0,0,0,0,{{0,0,0,0}},0};
    size_t cursor = 0;
    unsigned count = 0;
    if (!source || !bytes || !output || !capacity || !block || !options)
        return PW_ERR_PRECONDITION;
    const unsigned residency_enabled = options->residency_enabled;
    const unsigned lazy_flags_enabled = options->lazy_flags_enabled;
    e.no_counters = options->no_counters;
    if (options->flat_high > options->flat_low && options->flat_high - options->flat_low >= 16) {
        e.flat_low = options->flat_low;
        e.flat_span = options->flat_high - options->flat_low;
    }
    /* Set by a ret or an indirect call/jump: the exit that may look its
     * target up in the indirect table. */
    unsigned indirect_exit = 0;
    memset(block,0,sizeof(*block));

    DecodedInst insts[32];
    memset(insts, 0, sizeof(insts));
    unsigned gpr_uses[8] = {0};

#define DECODE_FAIL(err) do { if (count) goto analyze_and_emit; return (err); } while (0)

    /* Pass 1: Instruction boundary and semantic decode */
    while (cursor < bytes && count < 32) {
        DecodedInst *d = &insts[count];
        /* F2 on a jump, call or return is MPX's bnd, which a processor
         * without MPX ignores (FFmpeg's assembly has it): the branch is
         * decoded past it. Its target is relative to its end, which the
         * prefix does not move. */
        if (source[cursor] == 0xf2 && bnd_branch(source + cursor + 1, bytes - cursor - 1))
            cursor++;
        d->cursor = cursor;
        const uint8_t op = source[cursor];
        d->op = op;
        d->alu = 7;
        size_t length = 0;
        Operand operand;
        memset(&operand, 0, sizeof(operand));
        unsigned compare=0,alu=7,short_imm=0,word_operand=0,lock_prefix=0,cmpxchg=0,xchg=0,conditional=0,extend=0,setcc=0;
        unsigned bit_op=0,bit_imm=0;
        unsigned cmov=0,cmov_condition=0;
        unsigned sse_kind=PW_SSE_NONE,sse_mem_bytes=0,sse_has_imm=0;
        uint8_t sse_prefix=0,sse_opcode=0,sse_imm=0;
        unsigned bit_scan=0,bit_scan_word=0,bit_scan_count=0;
        unsigned bswap=0,bswap_reg=0;
        unsigned fs_call=0;
        uint8_t bit_scan_opcode=0;
        uint8_t double_shift=0;
        unsigned shift_word=0;
        unsigned lea_prefixed=0;
        unsigned extend_word_destination=0;
        unsigned x87=0,x87_width=0,x87_write=0,x87_register=0;
        unsigned string_op=0,string_width=0,string_repeat=0;
        unsigned word_general=0;
        unsigned byte_alu=0,byte_direction=0;
        unsigned imul_general=0;
        int terminal = 0;
        uint32_t flags_def = 0, flags_use = 0;
        int can_fault = 0;

        int padding = pw_x86_padding_length(source + cursor, bytes - cursor);
        if (padding < 0) DECODE_FAIL(padding == -1 ? PW_ERR_TRUNCATED : PW_ERR_UNSUPPORTED);
        if (padding) {
            length = (size_t)padding;
            d->op = 0x90;
        } else if(op==0x66 && bytes-cursor>=2 && source[cursor+1]==0xf3) {
            if(bytes-cursor<3)DECODE_FAIL(PW_ERR_TRUNCATED);
            if(source[cursor+2]!=0xa5 && source[cursor+2]!=0xab)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            string_op=source[cursor+2];string_width=2;string_repeat=1;length=3;can_fault=1;
        } else if(op==0xf3 && bytes-cursor<2) {
            DECODE_FAIL(PW_ERR_TRUNCATED);
        } else if(op==0xf3 &&
                  ((source[cursor+1]>=0xa4 && source[cursor+1]<=0xa7) ||
                   source[cursor+1]==0xaa || source[cursor+1]==0xab)) {
            /* REP with a string instruction. An F3 that introduces anything
             * else falls through to the SSE slice below, which is where the
             * mandatory-prefix forms live. */
            string_op=source[cursor+1];string_width=(string_op&1)?4:1;
            string_repeat=1;length=2;can_fault=1;
        } else if((op>=0xa4 && op<=0xa7) || op==0xaa || op==0xab) {
            string_op=op;string_width=(op&1)?4:1;length=1;can_fault=1;
        } else if(op==0x66 && bytes-cursor>=2 &&
                  (source[cursor+1]==0xa5 || source[cursor+1]==0xab)) {
            string_op=source[cursor+1];string_width=2;length=2;can_fault=1;
        } else if(op==0x66 && bytes-cursor==2 && source[cursor+1]==0x0f) {
            DECODE_FAIL(PW_ERR_TRUNCATED);
        } else if(op==0x66 && bytes-cursor>=3 && source[cursor+1]==0x0f &&
                  (source[cursor+2]==0xb6 || source[cursor+2]==0xb7 ||
                   source[cursor+2]==0xbe || source[cursor+2]==0xbf)) {
            int result=decode_operand(source+cursor+3,bytes-cursor-3,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            extend=1;extend_word_destination=1;length=3+operand.bytes;can_fault=(operand.mod!=3);
        } else if(op==0x66 && bytes-cursor>=2 &&
                  (source[cursor+1]==0x01 || source[cursor+1]==0x03 ||
                   source[cursor+1]==0x09 || source[cursor+1]==0x0b ||
                   source[cursor+1]==0x11 || source[cursor+1]==0x13 ||
                   source[cursor+1]==0x19 || source[cursor+1]==0x1b ||
                   source[cursor+1]==0x21 || source[cursor+1]==0x23 ||
                   source[cursor+1]==0x29 || source[cursor+1]==0x2b ||
                   source[cursor+1]==0x31 || source[cursor+1]==0x33 ||
                   source[cursor+1]==0x89 || source[cursor+1]==0x8b ||
                   source[cursor+1]==0x39 || source[cursor+1]==0x3b ||
                   source[cursor+1]==0x85 || source[cursor+1]==0xc7)) {
            word_general=source[cursor+1];
            int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            if(word_general==0xc7 && operand.reg)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            length=2+operand.bytes+(word_general==0xc7?2:0);
            can_fault=(operand.mod!=3);
            unsigned alu_word=word_general<=0x33 && ((word_general&7)==1 || (word_general&7)==3);
            unsigned compare_word=word_general==0x39 || word_general==0x3b;
            unsigned test_word=word_general==0x85;
            if(alu_word) {
                unsigned op_w=word_general>>3;
                unsigned log_w=op_w==1||op_w==4||op_w==6;
                flags_def=log_w?0x8c5:0x8d5;
                if(op_w==2||op_w==3)flags_use=0x001;
            } else if(compare_word) flags_def=0x8d5;
            else if(test_word) flags_def=0x8c5;
        } else if(op>=0xd8 && op<=0xdf) {
            int result=decode_operand(source+cursor+1,bytes-cursor-1,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            length=1+operand.bytes;can_fault=(operand.mod!=3);
            if(operand.mod!=3) {
                if(op==0xd9 && operand.reg==0){x87=PW_X87_FLD_F32+1;x87_width=4;}
                else if(op==0xd9 && operand.reg==2){x87=PW_X87_FST_F32+1;x87_width=4;x87_write=1;}
                else if(op==0xd9 && operand.reg==3){x87=PW_X87_FSTP_F32+1;x87_width=4;x87_write=1;}
                else if(op==0xdd && operand.reg==0){x87=PW_X87_FLD_F64+1;x87_width=8;}
                else if(op==0xdd && operand.reg==2){x87=PW_X87_FST_F64+1;x87_width=8;x87_write=1;}
                else if(op==0xdd && operand.reg==3){x87=PW_X87_FSTP_F64+1;x87_width=8;x87_write=1;}
                else if(op==0xdb && operand.reg==0){x87=PW_X87_FILD_I32+1;x87_width=4;}
                else if(op==0xd8) {
                    static const unsigned actions[]={PW_X87_FADD_F32,PW_X87_FMUL_F32,
                        PW_X87_FCOM_F32,PW_X87_FCOMP_F32,PW_X87_FSUB_F32,
                        PW_X87_FSUBR_F32,PW_X87_FDIV_F32,PW_X87_FDIVR_F32};
                    x87=actions[operand.reg]+1;x87_width=4;
                } else if(op==0xdc) {
                    static const unsigned actions[]={PW_X87_FADD_F64,PW_X87_FMUL_F64,
                        PW_X87_FCOM_F64,PW_X87_FCOMP_F64,PW_X87_FSUB_F64,
                        PW_X87_FSUBR_F64,PW_X87_FDIV_F64,PW_X87_FDIVR_F64};
                    x87=actions[operand.reg]+1;x87_width=8;
                }
                else DECODE_FAIL(PW_ERR_UNSUPPORTED);
            } else if(op==0xd9 && operand.reg==0) {
                x87=PW_X87_FLD_ST+1;x87_register=operand.rm+1;
            } else if(op==0xd9 && operand.reg==5 && operand.rm==0)x87=PW_X87_FLD1+1;
            else if(op==0xd9 && operand.reg==5 && operand.rm==6)x87=PW_X87_FLDZ+1;
            else if(op==0xd9 && operand.reg==4 && operand.rm==0)x87=PW_X87_FCHS+1;
            else if(op==0xd9 && operand.reg==4 && operand.rm==1)x87=PW_X87_FABS+1;
            else if(op==0xd9 && operand.reg==7 && operand.rm==2)x87=PW_X87_FSQRT+1;
            else if(op==0xd9 && operand.reg==7 && operand.rm==6)x87=PW_X87_FSIN+1;
            else if(op==0xd9 && operand.reg==7 && operand.rm==7)x87=PW_X87_FCOS+1;
            else if(op==0xdd && operand.reg==2) {
                x87=PW_X87_FST_ST+1;x87_register=operand.rm+1;
            } else if(op==0xdd && operand.reg==3) {
                x87=PW_X87_FSTP_ST+1;x87_register=operand.rm+1;
            } else if(op==0xdf && operand.reg==4 && operand.rm==0)x87=PW_X87_FNSTSW_AX+1;
            else if(op==0xd8 && (operand.reg==0 || operand.reg==1 || operand.reg==2 ||
                                 operand.reg==3 || operand.reg==4 || operand.reg==5 ||
                                 operand.reg==6)) {
                static const unsigned actions[]={PW_X87_FADD_ST,PW_X87_FMUL_ST,
                    PW_X87_FCOM_ST,PW_X87_FCOMP_ST,PW_X87_FSUB_ST,
                    PW_X87_FSUBR_ST,PW_X87_FDIV_ST};
                x87=actions[operand.reg]+1;x87_register=operand.rm+1;
            } else if(op==0xde && (operand.reg==0 || operand.reg==1 ||
                                   operand.reg==5 || operand.reg==6 || operand.reg==7)) {
                x87=(operand.reg==0?PW_X87_FADDP_ST:
                    operand.reg==1?PW_X87_FMULP_ST:
                    operand.reg==5?PW_X87_FSUBP_ST:
                    operand.reg==6?PW_X87_FDIVRP_ST:PW_X87_FDIVP_ST)+1;
                x87_register=operand.rm+1;
            } else if(op==0xda && operand.reg==5 && operand.rm==1)x87=PW_X87_FUCOMPP+1;
            else if(op==0xde && operand.reg==3 && operand.rm==1)x87=PW_X87_FCOMPP+1;
            else if(op==0xdc && (operand.reg==0 || operand.reg==1 || operand.reg>=4)) {
                static const unsigned actions[]={PW_X87_FADD_TO_ST,PW_X87_FMUL_TO_ST,0,0,
                    PW_X87_FSUBR_TO_ST,PW_X87_FSUB_TO_ST,
                    PW_X87_FDIVR_TO_ST,PW_X87_FDIV_TO_ST};
                x87=actions[operand.reg]+1;x87_register=operand.rm+1;
            }
            else DECODE_FAIL(PW_ERR_UNSUPPORTED);
        } else if(op==0xc1 || op==0xd1 || op==0xd3) {
            int result=decode_operand(source+cursor+1,bytes-cursor-1,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            /* SHL, SHR and SAR, and ROL/ROR, which write only CF (and OF
             * for a count of one) and leave SF, ZF, AF and PF alone. */
            const unsigned rotate=operand.reg<=3;
            const uint32_t one=rotate?0x801:0x8c5, many=rotate?0x001:0x0c5;
            /* RCL/RCR (2, 3) only by a constant: they also read CF. */
            if(operand.reg==6 || ((operand.reg==2 || operand.reg==3) && op==0xd3))
                DECODE_FAIL(PW_ERR_UNSUPPORTED);
            if(operand.reg==2 || operand.reg==3)flags_use=0x001;
            length=1+operand.bytes+(op==0xc1);
            can_fault=(operand.mod!=3);
            /* A masked zero shift count preserves every flag.  Immediate
             * zero can be resolved now; CL is dynamic, so model it as both
             * defining and consuming the deterministic flag subset. */
            if(op==0xc1 && length<=bytes-cursor) {
                unsigned count=source[cursor+length-1]&31;
                flags_def=count==0?0:count==1?one:many;
            } else {
                flags_def=one;
                if(op==0xd3)flags_use=one;
            }
        } else if(op==0x69 || op==0x6b) {
            int result=decode_operand(source+cursor+1,bytes-cursor-1,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            length=1+operand.bytes+(op==0x69?4:1);
            can_fault=(operand.mod!=3);
            flags_def=0x801;
        } else if(op<=0x3a && ((op&7)==0 || (op&7)==2)) {
            byte_alu=(op>>3)+1;byte_direction=!!(op&2);
            int result=decode_operand(source+cursor+1,bytes-cursor-1,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            length=1+operand.bytes;
            can_fault=(operand.mod!=3);
            unsigned op_b=byte_alu-1;
            flags_def=(op_b==1||op_b==4||op_b==6)?0x8c5:0x8d5;
            if(op_b==2||op_b==3)flags_use=0x001;
        } else if(op==0x80 || op==0x88 || op==0x8a || op==0xc6 || op==0x38 || op==0x3a || op==0x84 || op==0xf6) {
            int result=decode_operand(source+cursor+1,bytes-cursor-1,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            if((op==0xc6 || op==0xf6) && operand.reg!=0)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            length=1+operand.bytes+(op==0x80 || op==0xc6 || op==0xf6);
            can_fault=(operand.mod!=3);
            if(op==0x80) {
                flags_def=(operand.reg==1||operand.reg==4||operand.reg==6)?0x8c5:0x8d5;
                if(operand.reg==2||operand.reg==3)flags_use=0x001;
            } else if(op==0x38||op==0x3a) flags_def=0x8d5;
            else if(op==0x84||op==0xf6) flags_def=0x8c5;
        } else if((op<=0x3c && (op&7)==4) || op==0xa8 || (op>=0xb0 && op<=0xb7)) {
            length=2;
            if(op==0xa8) flags_def=0x8c5;
            else if(op<=0x3c && (op&7)==4) {
                flags_def=(op==0x0c||op==0x24||op==0x34)?0x8c5:0x8d5;
                if((op>>3)==2||(op>>3)==3)flags_use=0x001;
            }
        } else if(op>=0x40 && op<=0x4f) {
            length=1; flags_def=0x8d4;
        } else if(op==0x85 || op==0xf7 || op==0xa9) {
            if(op==0xa9){memset(&operand,0,sizeof(operand));operand.mod=3;operand.rm=0;length=5;flags_def=0x8c5;}
            else {
                int result=decode_operand(source+cursor+1,bytes-cursor-1,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                if(op==0xf7 && operand.reg!=0 && operand.reg<2)DECODE_FAIL(PW_ERR_UNSUPPORTED);
                length=1+operand.bytes+(op==0xf7 && operand.reg==0?4:0);
                can_fault=(operand.mod!=3);
                if(op==0x85) flags_def=0x8c5;
                else if(op==0xf7) {
                    if(operand.reg==0) flags_def=0x8c5;
                    else if(operand.reg==3) flags_def=0x8d5;
                }
            }
        } else if(op==0x66 && bytes-cursor>=4 && source[cursor+1]==0x0f &&
                  (source[cursor+2]==0xbc || source[cursor+2]==0xbd)) {
            /* 16-bit BSF/BSR: the host instruction with a 0x66 prefix keeps
             * the destination's upper 16 bits, exactly as the guest expects. */
            int result=decode_operand(source+cursor+3,bytes-cursor-3,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            bit_scan=1;
            bit_scan_word=1;
            bit_scan_opcode=source[cursor+2];
            length=3+operand.bytes;
            can_fault=(operand.mod!=3);
            flags_def=0x40;
        } else if(op==0x66 && bytes-cursor>=4 && source[cursor+1]==0x0f &&
                  (source[cursor+2]==0xa3 || source[cursor+2]==0xab ||
                   source[cursor+2]==0xb3 || source[cursor+2]==0xbb ||
                   source[cursor+2]==0xba)) {
            /* 16-bit BT/BTS/BTR/BTC: one 0x66 prefix, same family. */
            int result=decode_bit_test(source+cursor,bytes-cursor,1,&operand,
                                       &bit_op,&bit_imm,&length);
            if(result!=PW_OK)DECODE_FAIL(result);
            word_operand=1;
            can_fault=1;
            flags_def=0x001;
        } else if(op==0x66 && bytes-cursor>=3 &&
                  (source[cursor+1]==0xc1 || source[cursor+1]==0xd1 ||
                   source[cursor+1]==0xd3)) {
            /* 16-bit shift/rotate group. The destination's upper 16 bits are
             * untouched, and the count is masked to five bits as for 32-bit
             * operands (a 16-bit shift by 16..31 empties the word). */
            int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            if(operand.reg==6 || (operand.reg<=3 && source[cursor+1]==0xd3))
                DECODE_FAIL(PW_ERR_UNSUPPORTED);
            shift_word=1;
            length=2+operand.bytes+(source[cursor+1]==0xc1);
            can_fault=(operand.mod!=3);
            if(operand.reg<=3) {
                /* A rotate by a constant, run as the host instruction: the
                 * count is masked to five bits, and RCL/RCR read CF. */
                unsigned count=source[cursor+1]==0xd1?1u:
                               length<=bytes-cursor?source[cursor+length-1]&31u:0u;
                flags_def=count==0?0:count==1?0x801:0x001;
                if(operand.reg>=2)flags_use=0x001;
            } else if(source[cursor+1]==0xc1 && length<=bytes-cursor) {
                unsigned count=source[cursor+length-1]&31;
                flags_def=count==0?0:count==1?0x8c5:0x0c5;
            } else {
                flags_def=0x8c5;
                if(source[cursor+1]==0xd3)flags_use=0x8c5;
            }
        } else if(op==0x66 && bytes-cursor>=2 && source[cursor+1]==0x90) {
            /* The 16-bit encoding of NOP: compilers emit it as padding. */
            length=2;
        } else if((op==0x2e || op==0x3e || op==0x26 || op==0x36) &&
                  bytes-cursor>=2 && source[cursor+1]==0x8d) {
            /*
             * A segment override on LEA. LEA never accesses memory, so the
             * override is architecturally irrelevant; compilers emit
             * "2e 8d b4 26 ..." as an eight-byte alignment padding, and
             * dropping it is exact rather than an approximation.
             */
            int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            if(operand.mod==3)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            lea_prefixed=1;
            length=2+operand.bytes;
            can_fault=0;
        } else if(op==0x66 && bytes-cursor>=3 && source[cursor+1]==0x0f) {
            /* SSE with a mandatory 0x66 prefix. */
            int result=decode_sse(0x66u,0u,source+cursor+1,bytes-cursor-1,
                                  &operand,&sse_kind,&sse_mem_bytes,&length,
                                  &sse_imm,&sse_has_imm);
            if(result!=PW_OK)DECODE_FAIL(result);
            sse_prefix=0x66u;
            sse_opcode=source[cursor+2];
            length+=1u;                 /* the prefix itself */
            can_fault=(operand.mod!=3);
        } else if(op==0x66 && source[cursor+1]==0x0f) {
            DECODE_FAIL(PW_ERR_TRUNCATED);
        } else if(op==0xf3 && bytes-cursor>=3 && source[cursor+1]==0x0f &&
                  (source[cursor+2]==0xbc || source[cursor+2]==0xbd)) {
            /*
             * TZCNT/LZCNT, the counting forms of BSF/BSR. DXVK's bit
             * iteration runs TZCNT on every set bit of its dirty masks, so a
             * refusal here sent each one through the one-instruction host
             * fallback. Unlike BSF/BSR the result is defined for a zero
             * source (the operand width), CF reports that zero source and ZF
             * a zero result. The other flags are architecturally undefined;
             * they take the host's values, as in re-encoded code, so both
             * backends leave the same flags.
             */
            int result=decode_operand(source+cursor+3,bytes-cursor-3,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            bit_scan=1;
            bit_scan_count=1;
            bit_scan_opcode=source[cursor+2];
            length=3+operand.bytes;
            can_fault=(operand.mod!=3);
            flags_def=0x8d5;
        } else if((op==0xf2 || op==0xf3) && bytes-cursor>=3 &&
                  source[cursor+1]==0x0f) {
            /* SSE with a mandatory F2 or F3 prefix. */
            int result=decode_sse((uint8_t)op,0u,source+cursor+1,
                                  bytes-cursor-1,&operand,&sse_kind,
                                  &sse_mem_bytes,&length,&sse_imm,&sse_has_imm);
            if(result!=PW_OK)DECODE_FAIL(result);
            sse_prefix=(uint8_t)op;
            sse_opcode=source[cursor+2];
            length+=1u;
            can_fault=(operand.mod!=3);
        } else if((op==0xf2 || op==0xf3) && bytes-cursor>=2 &&
                  source[cursor+1]==0x0f) {
            DECODE_FAIL(PW_ERR_TRUNCATED);
        } else if(op==0xf0 && bytes-cursor>=3 && source[cursor+1]==0x0f &&
                  source[cursor+2]==0xb1) {
            /*
             * LOCK CMPXCHG r/m32, r32: the locked compare-and-swap Wine's own
             * atomics use. The memory form is the one the guest needs (the
             * register form has no atom to lock and stays refused), and the
             * emitter calls a helper that performs exactly one
             * compare-and-swap, so the guest sees the host's atomicity.
             */
            int result=decode_operand(source+cursor+3,bytes-cursor-3,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            if(operand.mod==3)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            cmpxchg=1;lock_prefix=1;length=3+operand.bytes;can_fault=1;
        } else if(op==0x0f && bytes-cursor>=2 && source[cursor+1]==0xb1) {
            int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            if(operand.mod==3)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            cmpxchg=1;length=2+operand.bytes;can_fault=1;
        } else if(op==0x87 || (op==0xf0 && bytes-cursor>=2 &&
                               source[cursor+1]==0x87)) {
            /*
             * XCHG r/m32, r32 to memory: Wine's heap code takes an entry off
             * a free list with it (heap_thread_detach_bin_groups,
             * dlls/ntdll/heap.c:1863). The memory form is the one with an
             * atom to exchange - i386 makes it atomic even without LOCK - and
             * it writes no flags; the register form has no memory operand and
             * stays refused rather than being translated as two moves.
             */
            const size_t prefix=op==0xf0?1u:0u;
            int result=decode_operand(source+cursor+prefix+1,
                                      bytes-cursor-prefix-1,&operand);
            if(result!=PW_OK)DECODE_FAIL(result);
            if(operand.mod==3)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            xchg=1;lock_prefix=op==0xf0?1u:0u;
            length=1+prefix+operand.bytes;can_fault=1;
        } else if(op==0x66 || op==0xf0 || op==0x81 || op==0x83 || (op<=0x3d && (op&7)==5)) {
            size_t prefix=(op==0x66 || op==0xf0)?1:0;
            if(bytes-cursor<=prefix)DECODE_FAIL(PW_ERR_TRUNCATED);
            unsigned cmpop=source[cursor+prefix];
            unsigned accumulator=cmpop<=0x3d && (cmpop&7)==5;
            if(cmpop!=0x81 && cmpop!=0x83 && !accumulator)DECODE_FAIL(PW_ERR_UNSUPPORTED);
            /*
             * LOCK on a group-1 memory read-modify-write. The guest needs it
             * for cross-thread synchronisation (Wine's critical sections use
             * "lock add [mem], 1"), so the emitted host instruction carries
             * the same prefix and the host guarantees the atomicity. A
             * register destination or a pure compare is not a legal LOCK
             * form and stays refused.
             */
            lock_prefix=op==0xf0?1u:0u;
            word_operand=op==0x66?1u:0u;short_imm=cmpop==0x83;compare=1;
            if(accumulator) {memset(&operand,0,sizeof(operand));operand.mod=3;operand.rm=0;alu=cmpop>>3;}
            else {
                int result=decode_operand(source+cursor+prefix+1,bytes-cursor-prefix-1,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                alu=operand.reg;
            }
            if(lock_prefix && (operand.mod==3 || alu==7))
                DECODE_FAIL(PW_ERR_UNSUPPORTED);
            length=prefix+1+operand.bytes+(short_imm?1:word_operand?2:4);
            can_fault=(operand.mod!=3);
            flags_def=(alu==1||alu==4||alu==6)?0x8c5:0x8d5;
            if(alu==2||alu==3)flags_use=0x001;
        } else if(op>=0x70 && op<=0x7f) {
            conditional=1;length=2;terminal=1;
            flags_use=branch_condition_flags(op&0xf);
        } else if(op==0x0f) {
            if(bytes-cursor<2)DECODE_FAIL(PW_ERR_TRUNCATED);
            if(source[cursor+1]>=0x80 && source[cursor+1]<=0x8f){
                conditional=1;length=6;terminal=1;
                flags_use=branch_condition_flags(source[cursor+1]&0xf);
            }
            else if(source[cursor+1]==0xaf) {
                int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                imul_general=1;length=2+operand.bytes;can_fault=(operand.mod!=3);flags_def=0x801;
            }
            else if(source[cursor+1]==0xb6 || source[cursor+1]==0xb7 || source[cursor+1]==0xbe || source[cursor+1]==0xbf || (source[cursor+1]>=0x90 && source[cursor+1]<=0x9f)) {
                int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                extend=source[cursor+1]>=0xb6;setcc=!extend;
                if(setcc && operand.mod!=3)DECODE_FAIL(PW_ERR_UNSUPPORTED);
                length=2+operand.bytes;can_fault=(operand.mod!=3);
                if(setcc) flags_use=branch_condition_flags(source[cursor+1]&0xf);
            }
            else if(source[cursor+1]==0xa3 || source[cursor+1]==0xab ||
                    source[cursor+1]==0xb3 || source[cursor+1]==0xbb ||
                    source[cursor+1]==0xba) {
                /*
                 * BT/BTS/BTR/BTC. The register-index encodings take the index
                 * from the ModRM reg field; 0f ba takes an imm8 and selects
                 * the operation through reg 4..7. Only CF is defined; the
                 * other flags are architecturally undefined and are left
                 * untouched, which is deterministic rather than arbitrary.
                 */
                int result=decode_bit_test(source+cursor,bytes-cursor,0,&operand,
                                           &bit_op,&bit_imm,&length);
                if(result!=PW_OK)DECODE_FAIL(result);
                can_fault=1;
                flags_def=0x001;
            }
            else if(source[cursor+1]>=0x40 && source[cursor+1]<=0x4f) {
                /*
                 * CMOVcc r32, r/m32. The destination is the ModRM reg field
                 * and it is written only when the condition holds; CMOV
                 * reads flags and defines none. Only the 32-bit forms are
                 * translated: the 16-bit forms would need a merging store
                 * this emitter does not have for arbitrary destinations.
                 */
                int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                cmov=1;
                cmov_condition=source[cursor+1]&0x0fu;
                length=2+operand.bytes;
                can_fault=(operand.mod!=3);
                flags_use=branch_condition_flags(cmov_condition);
            }
            else if(source[cursor+1]==0x10 || source[cursor+1]==0x11 ||
                    source[cursor+1]==0x28 || source[cursor+1]==0x29 ||
                    source[cursor+1]==0x54 || source[cursor+1]==0x55 ||
                    source[cursor+1]==0x56 || source[cursor+1]==0x57) {
                /* The unprefixed half of the SSE slice: movups, movaps and
                 * the bitwise logic instructions. */
                int result=decode_sse(0u,0u,source+cursor,bytes-cursor,
                                      &operand,&sse_kind,&sse_mem_bytes,&length,
                                      &sse_imm,&sse_has_imm);
                if(result!=PW_OK)DECODE_FAIL(result);
                sse_opcode=source[cursor+1];
                can_fault=(operand.mod!=3);
            }
            else if(source[cursor+1]==0xa4 || source[cursor+1]==0xa5 ||
                    source[cursor+1]==0xac || source[cursor+1]==0xad) {
                /* SHLD/SHRD r/m32, r32, imm8|CL. The count is masked to five
                 * bits; like the one-operand shifts, zero defines no flag and
                 * only a count of one defines OF. AF stays as it was. */
                int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                double_shift=source[cursor+1];
                length=2+operand.bytes+!(double_shift&1);
                if(length>bytes-cursor)DECODE_FAIL(PW_ERR_TRUNCATED);
                can_fault=(operand.mod!=3);
                if(!(double_shift&1)) {
                    unsigned count=source[cursor+length-1]&31;
                    flags_def=count==0?0:count==1?0x8c5:0x0c5;
                } else {
                    flags_def=0x8c5;
                    flags_use=0x8c5;
                }
            }
            else if(source[cursor+1]==0xbc || source[cursor+1]==0xbd) {
                /* BSF/BSR: only ZF is architecturally defined. */
                int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                bit_scan=1;
                bit_scan_opcode=source[cursor+1];
                length=2+operand.bytes;
                can_fault=(operand.mod!=3);
                flags_def=0x40;
            }
            else if(source[cursor+1]==0x1f) {
                /* Multi-byte NOP ("0f 1f /0"): padding only, no effect. */
                int result=decode_operand(source+cursor+2,bytes-cursor-2,&operand);
                if(result!=PW_OK)DECODE_FAIL(result);
                if(operand.reg!=0)DECODE_FAIL(PW_ERR_UNSUPPORTED);
                length=2+operand.bytes;
                can_fault=0;
            }
            else if((source[cursor+1]&0xf8u)==0xc8u) {
                /*
                 * BSWAP r32. The register is named by the opcode byte itself,
                 * so there is no ModRM operand and nothing can fault. The
                 * 16-bit encoding is architecturally undefined, and a 0x66
                 * prefix never reaches here: it is refused with the rest of
                 * the unimplemented prefixed forms.
                 */
                bswap=1;
                bswap_reg=(unsigned)(source[cursor+1]&7u);
                length=2;
                can_fault=0;
            } else DECODE_FAIL(PW_ERR_UNSUPPORTED);
        } else if (op == 0x64) {
            if (bytes-cursor < 2) DECODE_FAIL(PW_ERR_TRUNCATED);
            if (source[cursor+1]==0xa1 || source[cursor+1]==0xa3) {
                length=6;can_fault=1;
            } else if (source[cursor+1]==0xff) {
                /*
                 * "call dword ptr fs:[disp32]": Wine's *other* syscall stub
                 * shape. The stubs declared with -syscall=<id> in ntdll.spec
                 * load the id into EAX and call the dispatcher through
                 * TEB.WOW32Reserved (fs:[0xc0]), which is where the unix side
                 * installs it - the same dispatcher the "mov edx, <thunk>"
                 * stubs reach, so the boundary is identical.
                 */
                if (bytes-cursor < 7) DECODE_FAIL(PW_ERR_TRUNCATED);
                if (source[cursor+2] != 0x15) DECODE_FAIL(PW_ERR_UNSUPPORTED);
                fs_call=1;
                length=7;
                can_fault=1;
                terminal=1;
            } else if (source[cursor+1]==0x8b || source[cursor+1]==0x89) {
                /*
                 * FS-prefixed absolute dword operand: "mov r32, fs:[disp32]"
                 * or "mov fs:[disp32], r32". Wine's ntdll reads the TEB this
                 * way (mov edi, fs:[0x18]) on its very first instructions.
                 * The segment base is the guest's own FS base, never the
                 * host's, and fs_address validates the offset against the
                 * declared FS block before the guard checks the address.
                 */
                if (bytes-cursor < 3) DECODE_FAIL(PW_ERR_TRUNCATED);
                if ((source[cursor+2] & 0xc7u) != 0x05u)
                    DECODE_FAIL(PW_ERR_UNSUPPORTED);
                length=7;can_fault=1;
                operand.reg=(uint8_t)((source[cursor+2]>>3)&7u);
                operand.rm=5u;
            } else
                DECODE_FAIL(PW_ERR_UNSUPPORTED);
        } else if (op == 0x89 || op == 0x8b || op == 0x8d || op==0xc7 ||
                   op==0x01 || op==0x03 || op==0x09 || op==0x0b ||
                   op==0x11 || op==0x13 || op==0x19 || op==0x1b ||
                   op==0x21 || op==0x23 || op==0x29 || op==0x2b ||
                   op==0x31 || op==0x33 || op==0xff || op==0x39 || op==0x3b) {
            int result=decode_operand(source+cursor+1,bytes-cursor-1,&operand);
            if (result!=PW_OK) DECODE_FAIL(result);
            if (op==0x8d && operand.mod==3) DECODE_FAIL(PW_ERR_UNSUPPORTED);
            if (op==0xc7 && operand.reg!=0) DECODE_FAIL(PW_ERR_UNSUPPORTED);
            if (op==0xff && operand.reg!=0 && operand.reg!=1 && operand.reg!=2 && operand.reg!=4 && operand.reg!=6) DECODE_FAIL(PW_ERR_UNSUPPORTED);
            length=1+operand.bytes;
            if (op==0xc7) length+=4;
            can_fault=(operand.mod!=3);
            if(op==0x8d) can_fault=0;
            else if(op==0x09||op==0x0b||op==0x21||op==0x23||op==0x31||op==0x33) flags_def=0x8c5;
            else if(op==0x01||op==0x03||op==0x29||op==0x2b||op==0x39||op==0x3b) flags_def=0x8d5;
            else if(op==0x11||op==0x13||op==0x19||op==0x1b) { flags_def=0x8d5; flags_use=0x001; }
            else if(op==0xff) {
                if(operand.reg==0||operand.reg==1) flags_def=0x8d4;
                else if(operand.reg==2||operand.reg==4) { terminal=1; can_fault=1; }
                else if(operand.reg==6) can_fault=1;
            }
        } else if (op == 0x6a) { length = 2; can_fault = 1; }
        else if (op == 0xeb) { length = 2; terminal = 1; }
        else if (op == 0x68) { length = 5; can_fault = 1; }
        else if (op == 0xe8) { length = 5; terminal = 1; can_fault = 1; }
        else if (op == 0xe9) { length = 5; terminal = 1; }
        /* The accumulator forms with an absolute address: a1/a3 move the
         * whole register, a0/a2 only its low byte, which is what ntdll's own
         * locale code uses to publish the code page it selected. */
        else if (op == 0xa0 || op == 0xa1 || op == 0xa2 || op == 0xa3) {
            length = 5; can_fault = 1;
        }
        else if (op >= 0xb8 && op <= 0xbf) length = 5;
        else if (op == 0xc2) { length = 3; terminal = 1; can_fault = 1; }
        else if (op == 0xc3) { length = 1; terminal = 1; can_fault = 1; }
        else if (op == 0xc9) { length = 1; can_fault = 1; }
        else if (op == 0x99 || op == 0x90) length = 1;
        else if (op >= 0x50 && op <= 0x5f) { length = 1; can_fault = 1; }
        else DECODE_FAIL(PW_ERR_UNSUPPORTED);
        if (length > bytes-cursor) DECODE_FAIL(PW_ERR_TRUNCATED);

        d->length = length;
        d->operand = operand;
        d->compare = compare;
        d->alu = alu;
        d->short_imm = short_imm;
        d->word_operand = word_operand;
        d->lock_prefix = lock_prefix;
        d->cmpxchg = cmpxchg;
        d->xchg = xchg;
        d->bit_op = bit_op;
        d->bit_imm = bit_imm;
        d->cmov = cmov;
        d->cmov_condition = cmov_condition;
        d->sse_kind = sse_kind;
        d->sse_prefix = sse_prefix;
        d->sse_opcode = sse_opcode;
        d->sse_mem_bytes = sse_mem_bytes;
        d->sse_imm = sse_imm;
        d->sse_has_imm = sse_has_imm;
        d->bit_scan = bit_scan;
        d->bit_scan_opcode = bit_scan_opcode;
        d->bit_scan_word = bit_scan_word;
        d->bit_scan_count = bit_scan_count;
        d->double_shift = double_shift;
        d->bswap = bswap;
        d->bswap_reg = bswap_reg;
        d->shift_word = shift_word;
        d->lea_prefixed = lea_prefixed;
        d->fs_call = fs_call;
        d->conditional = conditional;
        d->extend = extend;
        d->setcc = setcc;
        d->extend_word_destination = extend_word_destination;
        d->x87 = x87;
        d->x87_width = x87_width;
        d->x87_write = x87_write;
        d->x87_register = x87_register;
        d->string_op = string_op;
        d->string_width = string_width;
        d->string_repeat = string_repeat;
        d->word_general = word_general;
        d->byte_alu = byte_alu;
        d->byte_direction = byte_direction;
        d->imul_general = imul_general;
        d->terminal = terminal;
        d->flags_def = flags_def;
        d->flags_use = flags_use;
        d->can_fault = can_fault;

        if (operand.mod == 3) {
            if (operand.rm < 8) gpr_uses[operand.rm]++;
        } else if (operand.bytes > 0) {
            if (operand.base >= 0 && operand.base < 8) gpr_uses[operand.base]++;
            if (operand.index >= 0 && operand.index < 8) gpr_uses[operand.index]++;
        }
        if (operand.reg < 8) gpr_uses[operand.reg]++;
        if (op >= 0x40 && op <= 0x4f) gpr_uses[op & 7]++;
        if (op >= 0x50 && op <= 0x57) { gpr_uses[op - 0x50]++; gpr_uses[4] += 2; }
        if (op >= 0x58 && op <= 0x5f) { gpr_uses[op - 0x58]++; gpr_uses[4] += 2; }
        if (op >= 0xb8 && op <= 0xbf) gpr_uses[op - 0xb8]++;
        if (op == 0x99) { gpr_uses[0]++; gpr_uses[2]++; }
        if (op == 0xc9) { gpr_uses[4] += 2; gpr_uses[5] += 2; }
        if (op == 0xa0 || op == 0xa1 || op == 0xa2 || op == 0xa3)
            gpr_uses[0]++;
        if (op == 0x6a || op == 0x68 || op == 0xe8) gpr_uses[4] += 2;
        if (op == 0xc3 || op == 0xc2) gpr_uses[4] += 2;
        if (op == 0xd3 || (double_shift & 1)) gpr_uses[1]++;
        if (string_op) { gpr_uses[1]++; gpr_uses[6]++; gpr_uses[7]++; }
        if (x87 && (op == 0xd9 && operand.reg == 0x07)) gpr_uses[0]++;
        if (imul_general || (op == 0xf7 && operand.reg >= 4)) { gpr_uses[0]++; gpr_uses[2]++; }

        cursor += length;
        ++count;
        if (d->terminal) break;
    }

analyze_and_emit:
    if (!count) return PW_ERR_UNSUPPORTED;

    /* Backward liveness analysis across the basic block (FEX Dead Flag Elimination) */
    uint32_t live = 0x8d5; /* All arithmetic flags live at block exit */
    for (int i = (int)count - 1; i >= 0; i--) {
        if (insts[i].flags_def) {
            if ((insts[i].flags_def & live) == 0) {
                insts[i].flags_dead = 1;
            } else {
                insts[i].flags_dead = 0;
            }
            live = (live & ~insts[i].flags_def) | insts[i].flags_use;
        } else {
            live |= insts[i].flags_use;
        }
        /* Guards run before the guest instruction changes flags.  A fault
         * therefore observes every incoming arithmetic flag, so the barrier
         * applies to live-in (after the def/use transfer), not live-out. */
        if (insts[i].can_fault) live |= 0x8d5;
    }

    /* Allocate resident registers based on use frequencies.  A helper call
     * already requires a full spill/reload ownership boundary, so residency
     * only adds work to a block that contains one. */
    unsigned helper_boundary=0;
    for(unsigned i=0;i<count;i++) {
        if(insts[i].string_op || insts[i].x87 || insts[i].cmpxchg ||
           insts[i].xchg ||
           (insts[i].op==0xf7 && insts[i].operand.reg>=4)) {
            helper_boundary=1;
            break;
        }
    }
    memset(&block->entry_contract, 0, sizeof(block->entry_contract));
    for (int i = 0; i < 8; i++) block->entry_contract.guest_to_host[i] = -1;
    for (int i = 0; i < PW_X86_MAX_HOST_REGS; i++) block->entry_contract.host_to_guest[i] = -1;

    /* Tiny blocks do not contain enough work to repay canonical-entry loads
     * and cross-contract reconciliation.  Four instructions is the measured
     * break-even floor for this first allocator. */
    if (residency_enabled && options->global_resident && !helper_boundary) {
        /* The same assignment in every such block: a linked exit then
         * always meets a matching contract and hands the values over in
         * their registers. A block with a helper call keeps an empty
         * contract, entered and left through the reconciliation stubs. */
        for (unsigned g = 0; g < 8; g++) {
            int h = pw_x86_global_host(options->global_resident, g);
            if (h < 0) continue;
            block->entry_contract.resident_mask |= (uint8_t)(1u << g);
            block->entry_contract.guest_to_host[g] = (int8_t)h;
            block->entry_contract.host_to_guest[h] = (int8_t)g;
        }
    } else if (residency_enabled && !options->global_resident && count >= 4u &&
               !helper_boundary) {
        for (int h = 0; h < PW_X86_LOCAL_HOST_REGS; h++) {
            int best_gpr = -1;
            /* A one-use resident register only pays entry/exit and contract
             * overhead.  Reserve scarce host registers for values reused in
             * the block, where avoiding canonical traffic can amortize it. */
            unsigned max_uses = 1;
            for (int g = 0; g < 8; g++) {
                if (block->entry_contract.guest_to_host[g] == -1 && gpr_uses[g] > max_uses) {
                    max_uses = gpr_uses[g];
                    best_gpr = g;
                }
            }
            if (best_gpr >= 0) {
                block->entry_contract.resident_mask |= (1 << best_gpr);
                block->entry_contract.guest_to_host[best_gpr] = (int8_t)h;
                block->entry_contract.host_to_guest[h] = (int8_t)best_gpr;
            }
        }
    }
    block->exit_contract = block->entry_contract;
    /* A matching chain entry can inherit resident values that are newer than
     * canonical state.  The block has multiple possible predecessors, so a
     * static contract cannot know their per-register dirty masks.  Treat all
     * resident values as dirty until the first emitted spill barrier.  A
     * canonical entry may perform harmless redundant stores; a chain entry
     * must never lose a predecessor's update. */
    block->exit_contract.dirty_mask = block->entry_contract.resident_mask;

    /* Pass 2: Machine code emission, twice. The first emission stores the
     * guest EIP before every instruction and records which instructions can
     * stop the block in the middle; the second, which is kept, stores it only
     * before those (and after the last instruction), since nothing reads it
     * in between. */
    uint8_t needs_eip[32];
    const PwX86Block planned = *block;
    memset(needs_eip, 1, sizeof(needs_eip));
    for (unsigned pass = 0; pass < 2; pass++) {
    if (pass) {
        if (e.failed) break;
        *block = planned;
        e.n = 0;
        e.exits = 0;
        e.rcx_flags_end = 0;
        e.host_flags_end = 0;
        e.cold_count = 0;
        indirect_exit = 0;
    }
    block->canonical_entry_offset = e.n;
    if (block->entry_contract.resident_mask) {
        emit_load_all_resident(&e, &block->entry_contract);
        uint8_t n_res = popcount8(block->entry_contract.resident_mask);
        if (!e.no_counters) { byte(&e, 0x83); byte(&e, 0x47); byte(&e, offsetof(PwX86State, reg_loads)); byte(&e, n_res); }
    }
    block->chain_entry_offset = e.n;


    for (unsigned i = 0; i < count; i++) {
        DecodedInst *d = &insts[i];
        size_t cursor = d->cursor;
        size_t length = d->length;
        uint8_t op = d->op;
        Operand operand = d->operand;
        unsigned compare = d->compare;
        unsigned alu = d->alu;
        unsigned short_imm = d->short_imm;
        unsigned word_operand = d->word_operand;
        unsigned lock_prefix = d->lock_prefix;
        unsigned cmpxchg = d->cmpxchg;
        unsigned xchg = d->xchg;
        unsigned bit_op = d->bit_op;
        unsigned bit_imm = d->bit_imm;
        unsigned cmov = d->cmov;
        unsigned cmov_condition = d->cmov_condition;
        unsigned sse_kind = d->sse_kind;
        unsigned sse_mem_bytes = d->sse_mem_bytes;
        uint8_t sse_imm = d->sse_imm;
        uint8_t sse_prefix = d->sse_prefix;
        uint8_t sse_opcode = d->sse_opcode;
        unsigned bit_scan = d->bit_scan;
        uint8_t bit_scan_opcode = d->bit_scan_opcode;
        unsigned bit_scan_word = d->bit_scan_word;
        unsigned bit_scan_count = d->bit_scan_count;
        uint8_t double_shift = d->double_shift;
        unsigned bswap = d->bswap;
        unsigned bswap_reg = d->bswap_reg;
        unsigned shift_word = d->shift_word;
        unsigned lea_prefixed = d->lea_prefixed;
        unsigned fs_call = d->fs_call;
        unsigned conditional = d->conditional;
        unsigned extend = d->extend;
        unsigned setcc = d->setcc;
        unsigned extend_word_destination = d->extend_word_destination;
        unsigned x87 = d->x87;
        unsigned x87_width = d->x87_width;
        unsigned x87_write = d->x87_write;
        unsigned x87_register = d->x87_register;
        unsigned string_op = d->string_op;
        unsigned string_width = d->string_width;
        unsigned string_repeat = d->string_repeat;
        unsigned word_general = d->word_general;
        unsigned byte_alu = d->byte_alu;
        unsigned byte_direction = d->byte_direction;
        unsigned imul_general = d->imul_general;
        int terminal = d->terminal; (void)terminal;

        uint32_t next = pc + (uint32_t)cursor + (uint32_t)length;
        const unsigned exits_before = e.exits;
        /* What rcx holds of the flags when the previous instruction's code
         * ends with its flag capture: the prologue below leaves rcx alone. */
        const uint32_t rcx_flags = e.rcx_flags_end == e.n + 1 ? e.rcx_flags_mask : 0;
        const unsigned host_flags = e.host_flags_end == e.n + 1;
        /* Fault exits preserve the PC of the faulting guest instruction. */
        if (needs_eip[i]) store(&e,offsetof(PwX86State,eip),pc+(uint32_t)cursor);
        if (d->can_fault || string_op || x87) {
            emit_spill_dirty(&e, &block->exit_contract);
            block->exit_contract.dirty_mask = 0;
        }
        if(string_op) {
            /* The helper can consume and replace arithmetic EFLAGS (CMPS/REP),
             * so it is a descriptor ownership boundary. */
            emit_commit_flags(&e);
            string_call(&e,string_op,string_width,string_repeat);
            emit_load_all_resident(&e, &block->exit_contract);
        }
        else if(cmpxchg) {
            /*
             * One compare-and-swap in the helper, so the boundary is the same
             * as the string helpers': pending lazy flags are committed first
             * (the helper writes the guest's EFLAGS itself, from the
             * comparison it performs) and the resident registers are reloaded
             * afterwards, because a C call clobbers the host registers that
             * hold them.
             */
            emit_commit_flags(&e);
            effective_address(&e,&operand,&block->exit_contract);
            memory_address_width(&e,2,4);
            cmpxchg_call(&e,operand.reg);
            emit_load_all_resident(&e, &block->exit_contract);
        }
        else if(xchg) {
            /*
             * One exchange in the helper, because i386 performs XCHG with a
             * memory operand atomically. The helper writes no flags, but the C
             * call can clobber the host flag register the pending lazy flags
             * live in, so they are committed first and the resident registers
             * reloaded afterwards - the same ownership boundary the
             * compare-and-swap helper uses.
             */
            emit_commit_flags(&e);
            effective_address(&e,&operand,&block->exit_contract);
            memory_address_width(&e,2,4);
            xchg_call(&e,operand.reg);
            emit_load_all_resident(&e, &block->exit_contract);
        }
        else if(byte_alu) {
            unsigned operation=byte_alu-1;
            unsigned destination=byte_direction?operand.reg:operand.rm;
            unsigned source_register=byte_direction?operand.rm:operand.reg;
            /* Materialization calls C and may clobber operand temporaries.  Do it
             * before loading either byte operand, then import CF immediately
             * before the native ADC/SBB instruction. */
            if(operation==2 || operation==3)
                emit_materialize_flags(&e,0x001);
            if(operand.mod!=3) {
                effective_address(&e,&operand,&block->exit_contract);
                memory_address_width(&e,!byte_direction && operation!=7?2:0,1);
                byte(&e,0x49);byte(&e,0x89);byte(&e,0xc3); /* r11 = address */
                if(byte_direction) {
                    byte(&e,0x41);byte(&e,0x0f);byte(&e,0xb6);byte(&e,0x0b);
                    load_guest_byte_eax(&e,&block->exit_contract,destination);
                } else {
                    byte(&e,0x41);byte(&e,0x0f);byte(&e,0xb6);byte(&e,0x03);
                    load_guest_byte_ecx(&e,&block->exit_contract,source_register);
                }
            } else {
                load_guest_byte_eax(&e,&block->exit_contract,destination);
                load_guest_byte_ecx(&e,&block->exit_contract,source_register);
            }
            if(operation==2 || operation==3) {
                byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
            }
            byte(&e,(uint8_t)(operation*8));byte(&e,0xc8); /* op al, cl */
            emit_save_flags(&e, (operation==1 || operation==4 || operation==6)?0x8c5:0x8d5,
                            lazy_flags_enabled, d->flags_dead);
            if(operation!=7) {
                if(operand.mod!=3 && !byte_direction)
                    {byte(&e,0x41);byte(&e,0x88);byte(&e,0x03);}
                else
                    store_guest_byte_eax(&e,&block->exit_contract,destination);
            }
        }
        else if(imul_general) {
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,0,4);byte(&e,0x8b);byte(&e,0x00);}
            int h_reg = get_resident_host_reg(&block->exit_contract, operand.reg);
            if (h_reg >= 0) {
                byte(&e, 0x41); byte(&e, 0x0f); byte(&e, 0xaf); byte(&e, (uint8_t)(0xc0 | h_reg));
            } else {
                byte(&e,0x0f);byte(&e,0xaf);byte(&e,0x47);byte(&e,operand.reg*4);
            }
            store_guest_reg(&e, &block->exit_contract, operand.reg);
            emit_save_flags(&e, 0x801, lazy_flags_enabled, d->flags_dead);
        }
        else if(op==0x69 || op==0x6b) {
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,0,4);byte(&e,0x8b);byte(&e,0x00);}
            byte(&e,op);byte(&e,0xc0);
            if(op==0x69)word(&e,read32(source+cursor+length-4));
            else byte(&e,source[cursor+length-1]);
            store_guest_reg(&e, &block->exit_contract, operand.reg);
            emit_save_flags(&e, 0x801, lazy_flags_enabled, d->flags_dead);
        }
        else if(word_general) {
            unsigned load=word_general==0x8b;
            unsigned alu_word=word_general<=0x33 &&
                ((word_general&7)==1 || (word_general&7)==3);
            unsigned compare_word=word_general==0x39 || word_general==0x3b;
            unsigned test_word=word_general==0x85;
            unsigned immediate_word=word_general==0xc7;
            if(immediate_word) {
                uint16_t value=(uint16_t)source[cursor+length-2]|
                    (uint16_t)source[cursor+length-1]<<8;
                if(operand.mod==3) {
                    emit_spill_single(&e, &block->exit_contract, operand.rm);
                    byte(&e,0x66);byte(&e,0xc7);byte(&e,0x47);byte(&e,operand.rm*4);
                    byte(&e,(uint8_t)value);byte(&e,(uint8_t)(value>>8));
                    if (get_resident_host_reg(&block->exit_contract, operand.rm) >= 0) {
                        emit_load_single(&e, &block->exit_contract, operand.rm);
                        block->exit_contract.dirty_mask |= (1 << operand.rm);
                    }
                } else {
                    effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,1,2);
                    byte(&e,0x66);byte(&e,0xc7);byte(&e,0x00);
                    byte(&e,(uint8_t)value);byte(&e,(uint8_t)(value>>8));
                }
            } else if(alu_word) {
                unsigned memory_destination=(word_general&7)==1;
                unsigned operation=word_general>>3;
                unsigned logical=operation==1 || operation==4 || operation==6;
                if(operation==2 || operation==3)
                    emit_materialize_flags(&e,0x001);
                if(operand.mod==3) {
                    unsigned destination=memory_destination?operand.rm:operand.reg;
                    unsigned source_register=memory_destination?operand.reg:operand.rm;
                    emit_spill_single(&e, &block->exit_contract, destination);
                    emit_spill_single(&e, &block->exit_contract, source_register);
                    load_guest_reg(&e,&block->exit_contract,destination);byte(&e,0x66);
                    if(operation==2 || operation==3) {
                        byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                        byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                    }
                    byte(&e,(uint8_t)(operation*8+3));byte(&e,0x47);
                    byte(&e,source_register*4);
                    byte(&e,0x66);byte(&e,0x89);byte(&e,0x47);byte(&e,destination*4);
                    if (get_resident_host_reg(&block->exit_contract, destination) >= 0) {
                        emit_load_single(&e, &block->exit_contract, destination);
                        block->exit_contract.dirty_mask |= (1 << destination);
                    }
                } else {
                    effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,memory_destination,2);
                    if(memory_destination) {
                        byte(&e,0x8b);byte(&e,0x4f);byte(&e,operand.reg*4);
                        if(operation==2 || operation==3) {
                            byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                            byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                        }
                        byte(&e,0x66);byte(&e,word_general);byte(&e,0x08);
                    } else {
                        byte(&e,0x0f);byte(&e,0xb7);byte(&e,0x08);
                        load_guest_reg(&e,&block->exit_contract,operand.reg);byte(&e,0x66);
                        if(operation==2 || operation==3) {
                            byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                            byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                        }
                        byte(&e,word_general);byte(&e,0xc1); /* ax = ax op cx (reg op mem) */
                        byte(&e,0x66);byte(&e,0x89);byte(&e,0x47);byte(&e,operand.reg*4);
                        if (get_resident_host_reg(&block->exit_contract, operand.reg) >= 0) {
                            emit_load_single(&e, &block->exit_contract, operand.reg);
                            block->exit_contract.dirty_mask |= (1 << operand.reg);
                        }
                    }
                }
                emit_save_flags(&e, logical?0x8c5:0x8d5, lazy_flags_enabled, d->flags_dead);
            } else if(operand.mod==3) {
                emit_spill_single(&e, &block->exit_contract, operand.rm);
                emit_spill_single(&e, &block->exit_contract, operand.reg);
                if(word_general==0x89) {
                    load_guest_reg(&e,&block->exit_contract,operand.reg);byte(&e,0x66);byte(&e,0x89);
                    byte(&e,0x47);byte(&e,operand.rm*4);
                    if (get_resident_host_reg(&block->exit_contract, operand.rm) >= 0) {
                        emit_load_single(&e, &block->exit_contract, operand.rm);
                        block->exit_contract.dirty_mask |= (1 << operand.rm);
                    }
                } else if(word_general==0x8b) {
                    load_guest_reg(&e,&block->exit_contract,operand.rm);byte(&e,0x66);byte(&e,0x89);
                    byte(&e,0x47);byte(&e,operand.reg*4);
                    if (get_resident_host_reg(&block->exit_contract, operand.reg) >= 0) {
                        emit_load_single(&e, &block->exit_contract, operand.reg);
                        block->exit_contract.dirty_mask |= (1 << operand.reg);
                    }
                } else if(compare_word) {
                    load_guest_reg(&e,&block->exit_contract,(word_general==0x39?operand.rm:operand.reg));
                    byte(&e,0x66);byte(&e,0x3b);byte(&e,0x47);
                    byte(&e,(word_general==0x39?operand.reg:operand.rm)*4);
                } else {
                    load_guest_reg(&e,&block->exit_contract,operand.rm);byte(&e,0x66);byte(&e,0x85);
                    byte(&e,0x47);byte(&e,operand.reg*4);
                }
            } else {
                effective_address(&e,&operand,&block->exit_contract);
                memory_address_width(&e,!load && !compare_word && !test_word,2);
                if(word_general==0x89) {
                    byte(&e,0x8b);byte(&e,0x4f);byte(&e,operand.reg*4);
                    byte(&e,0x66);byte(&e,word_general);byte(&e,0x08);
                } else if(word_general==0x8b) {
                    byte(&e,0x0f);byte(&e,0xb7);byte(&e,0x00);
                    byte(&e,0x66);byte(&e,0x89);byte(&e,0x47);byte(&e,operand.reg*4);
                    if (get_resident_host_reg(&block->exit_contract, operand.reg) >= 0) {
                        emit_load_single(&e, &block->exit_contract, operand.reg);
                        block->exit_contract.dirty_mask |= (1 << operand.reg);
                    }
                } else if(word_general==0x39) {
                    byte(&e,0x66);byte(&e,0x8b);byte(&e,0x00);
                    byte(&e,0x66);byte(&e,0x3b);byte(&e,0x47);byte(&e,operand.reg*4);
                } else if(compare_word) {
                    byte(&e,0x0f);byte(&e,0xb7);byte(&e,0x08);
                    load_guest_reg(&e,&block->exit_contract,operand.reg);byte(&e,0x66);byte(&e,0x39);byte(&e,0xc8);
                } else {
                    byte(&e,0x66);byte(&e,0x8b);byte(&e,0x00);
                    byte(&e,0x66);byte(&e,0x85);byte(&e,0x47);byte(&e,operand.reg*4);
                }
            }
            if(compare_word || test_word) emit_save_flags(&e, test_word?0x8c5:0x8d5, lazy_flags_enabled, d->flags_dead);
        }
        else if(x87) {
            if(operand.mod!=3){effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,x87_write,x87_width);}
            emit_spill_dirty(&e, &block->exit_contract);
            block->exit_contract.dirty_mask = 0;
            x87_call(&e,x87-1,x87_register);
            emit_load_all_resident(&e, &block->exit_contract);
        } else if(((op==0xc1 && (source[cursor+length-1]&31)) || op==0xd1) ||
                  (shift_word && operand.reg<=3 && source[cursor+1]!=0xd3)) {
            /* A 32-bit shift or rotate by a nonzero constant: the host
             * instruction itself, and its defined flags deferred like any
             * producer's. SHL/SHR/SAR define CF, PF, ZF and SF (and OF for a
             * count of one); ROL/ROR define CF (and OF for one). The bits
             * they leave undefined keep their guest value, as below. */
            const uint8_t shift_op=(uint8_t)(shift_word?source[cursor+1]:op);
            const unsigned count=shift_op==0xd1?1u:(source[cursor+length-1]&31u);
            const unsigned rotate=operand.reg<=3, carry=operand.reg==2 || operand.reg==3;
            const uint32_t mask=count==1?(rotate?0x801u:0x8c5u):(rotate?0x001u:0x0c5u);
            /* With a carry rotate the operand is in r11 (memory) or loaded
             * after CF is: materializing CF uses eax, ecx and edx. */
            const uint8_t modrm=(uint8_t)((operand.mod==3?0xc0u:carry?0x03u:0u)|(operand.reg<<3));
            if(operand.mod!=3) {
                effective_address(&e,&operand,&block->exit_contract);
                memory_address_width(&e,2,shift_word?2:4);
                if(carry){byte(&e,0x49);byte(&e,0x89);byte(&e,0xc3);}  /* mov r11, rax */
            }
            if(carry) {
                /* The guest CF into the host CF. */
                emit_materialize_flags(&e,0x001);
                byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                byte(&e,offsetof(PwX86State,eflags));byte(&e,0);     /* bt dword [rdi+eflags], 0 */
            }
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            if(shift_word)byte(&e,0x66);
            if(operand.mod!=3 && carry)byte(&e,0x41);                /* [r11] */
            if(count==1){byte(&e,0xd1);byte(&e,modrm);}
            else {byte(&e,0xc1);byte(&e,modrm);byte(&e,(uint8_t)count);}
            if(operand.mod==3)store_guest_reg(&e, &block->exit_contract, operand.rm);
            if(count)emit_save_flags(&e, mask, lazy_flags_enabled, d->flags_dead);
        } else if(op==0xc1 || op==0xd1 || op==0xd3 || shift_word) {
            /* A zero-count shift preserves all arithmetic flags and wider
             * counts retain implementation-policy bits. Canonicalize the
             * previous producer before this eager variable-mask path. */
            const uint8_t shift_op=(uint8_t)(shift_word?source[cursor+1]:op);
            const unsigned shift_width=shift_word?2u:4u;
            emit_commit_flags(&e);
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,2,shift_width);}
            if(shift_op==0xd3){load_guest_reg_ecx(&e, &block->exit_contract, 1);}
            else {byte(&e,0xb9);word(&e,shift_op==0xd1?1:source[cursor+length-1]);}
            /* 16- and 32-bit shifts both mask the count to five bits. */
            byte(&e,0x83);byte(&e,0xe1);byte(&e,31);
            /* esi selects only defined flags: none for zero, OF only for one.
             * Preserve undefined AF and multi-bit OF deterministically. A
             * rotate (ROL/ROR, 32-bit only) defines just CF, and OF for one. */
            const unsigned rotate=!shift_word && operand.reg<=1;
            byte(&e,0xbe);word(&e,rotate?0x001:0xc5);
            byte(&e,0xba);word(&e,0);
            byte(&e,0x85);byte(&e,0xc9);
            byte(&e,0x0f);byte(&e,0x44);byte(&e,0xf2);
            byte(&e,0xba);word(&e,rotate?0x801:0x8c5);
            byte(&e,0x83);byte(&e,0xf9);byte(&e,1);
            byte(&e,0x0f);byte(&e,0x44);byte(&e,0xf2);
            if(shift_word)byte(&e,0x66);
            byte(&e,0xd3);byte(&e,(operand.mod==3?0xc0:0)|(operand.reg<<3));
            if(operand.mod==3)store_guest_reg(&e, &block->exit_contract, operand.rm);
            if (!d->flags_dead) {
                byte(&e,0x9c);byte(&e,0x5a); /* snapshot native flags */
                byte(&e,0x21);byte(&e,0xf2); /* and edx, esi */
                byte(&e,0xf7);byte(&e,0xd6); /* not esi */
                byte(&e,0x23);byte(&e,0x77);byte(&e,offsetof(PwX86State,eflags)); /* and esi, [rdi+eflags] */
                byte(&e,0x09);byte(&e,0xf2); /* or edx, esi */
                byte(&e,0x89);byte(&e,0x57);byte(&e,offsetof(PwX86State,eflags));
            }
        } else if(op>=0x40 && op<=0x4f) {
            unsigned reg=op&7;
            int h=get_resident_host_reg(&block->exit_contract,reg);
            if(h>=0) {
                byte(&e,0x41);byte(&e,0xff);
                byte(&e,(uint8_t)((op<0x48?0xc0:0xc8)|h));
                block->exit_contract.dirty_mask|=(uint8_t)(1u<<reg);
            } else {
                load_guest_reg(&e,&block->exit_contract,reg);
                byte(&e,0xff);byte(&e,op<0x48?0xc0:0xc8);
                store_guest_reg(&e,&block->exit_contract,reg);
            }
            emit_save_flags(&e, 0x8d4, lazy_flags_enabled, d->flags_dead); /* INC/DEC preserve guest CF. */
        } else if(bswap) {
            /*
             * BSWAP r32 on the same place: the guest register is in eax (or
             * in its resident host register), the host instruction swaps its
             * four bytes, and the result goes back. No flags are written, so
             * nothing is saved.
             */
            load_guest_reg(&e, &block->exit_contract, bswap_reg);
            byte(&e,0x0f);byte(&e,0xc8);
            store_guest_reg(&e, &block->exit_contract, bswap_reg);
        } else if(op>=0xb0 && op<=0xb7) {
            unsigned reg=op&7;
            emit_spill_single(&e, &block->exit_contract, reg & 3);
            byte(&e,0xc6);byte(&e,0x47);byte(&e,(reg&3)*4+(reg>>2));byte(&e,source[cursor+1]);
            if (get_resident_host_reg(&block->exit_contract, reg & 3) >= 0) {
                emit_load_single(&e, &block->exit_contract, reg & 3);
                block->exit_contract.dirty_mask |= (1 << (reg & 3));
            }
        } else if((op<=0x3c && (op&7)==4) || op==0xa8) {
            unsigned operation=op>>3;
            if(op!=0xa8 && (operation==2 || operation==3))
                emit_materialize_flags(&e,0x001);
            load_guest_reg(&e, &block->exit_contract, 0);
            if(op!=0xa8 && (operation==2 || operation==3)) {
                /* Reload native CF after operand setup and before ADC/SBB AL. */
                byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
            }
            byte(&e,op);byte(&e,source[cursor+1]);
            if(op!=0x3c && op!=0xa8)store_guest_reg(&e, &block->exit_contract, 0);
            emit_save_flags(&e, (op==0x0c || op==0x24 || op==0x34 || op==0xa8)?0x8c5:0x8d5,
                            lazy_flags_enabled, d->flags_dead);
        } else if(op==0x80 || op==0x88 || op==0x8a || op==0xc6 || op==0x38 || op==0x3a || op==0x84 || op==0xf6) {
            unsigned dest=(operand.rm&3)*4+(operand.rm>>2),reg=(operand.reg&3)*4+(operand.reg>>2);
            unsigned immediate_alu=op==0x80;
            unsigned write=op==0x88 || op==0xc6 || (immediate_alu && operand.reg!=7);
            if(immediate_alu && (operand.reg==2 || operand.reg==3))
                emit_materialize_flags(&e,0x001);
            if(operand.mod!=3){effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,write,1);}
            else emit_spill_single(&e, &block->exit_contract, operand.rm & 3);
            emit_spill_single(&e, &block->exit_contract, operand.reg & 3);
            if(immediate_alu) {
                    if(operand.reg==2 || operand.reg==3) {
                        byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                        byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                    }
                    byte(&e,0x80);byte(&e,operand.mod==3?
                        (uint8_t)(0x47|(operand.reg<<3)):(uint8_t)(operand.reg<<3));
                    if(operand.mod==3)byte(&e,dest);
                    byte(&e,source[cursor+length-1]);
                    if(operand.mod==3 && operand.reg!=7) {
                        if (get_resident_host_reg(&block->exit_contract, operand.rm & 3) >= 0) {
                            emit_load_single(&e, &block->exit_contract, operand.rm & 3);
                            block->exit_contract.dirty_mask |= (1 << (operand.rm & 3));
                        }
                    }
                    emit_save_flags(&e, (operand.reg==1 || operand.reg==4 || operand.reg==6)?0x8c5:0x8d5,
                                    lazy_flags_enabled, d->flags_dead);
            } else if(write) {
                if(op==0xc6) {
                    byte(&e,0xc6);byte(&e,operand.mod==3?0x47:0x00);
                    if(operand.mod==3)byte(&e,dest);
                    byte(&e,source[cursor+length-1]);
                    if(operand.mod==3) {
                        if (get_resident_host_reg(&block->exit_contract, operand.rm & 3) >= 0) {
                            emit_load_single(&e, &block->exit_contract, operand.rm & 3);
                            block->exit_contract.dirty_mask |= (1 << (operand.rm & 3));
                        }
                    }
                } else {
                    byte(&e,0x0f);byte(&e,0xb6);byte(&e,0x4f);byte(&e,reg);
                    byte(&e,0x88);byte(&e,operand.mod==3?0x4f:0x08);
                    if(operand.mod==3)byte(&e,dest);
                    if(operand.mod==3) {
                        if (get_resident_host_reg(&block->exit_contract, operand.rm & 3) >= 0) {
                            emit_load_single(&e, &block->exit_contract, operand.rm & 3);
                            block->exit_contract.dirty_mask |= (1 << (operand.rm & 3));
                        }
                    }
                }
            } else {
                byte(&e,0x0f);byte(&e,0xb6);byte(&e,operand.mod==3?0x47:0x00);
                if(operand.mod==3)byte(&e,dest);
                if(op==0x8a){
                    byte(&e,0x88);byte(&e,0x47);byte(&e,reg);
                    if (get_resident_host_reg(&block->exit_contract, operand.reg & 3) >= 0) {
                        emit_load_single(&e, &block->exit_contract, operand.reg & 3);
                        block->exit_contract.dirty_mask |= (1 << (operand.reg & 3));
                    }
                }
                else {
                    if(op==0xf6){byte(&e,0xa8);byte(&e,source[cursor+length-1]);}
                    else if(op==0x3a) {
                        byte(&e,0x89);byte(&e,0xc1);
                        byte(&e,0x0f);byte(&e,0xb6);byte(&e,0x47);byte(&e,reg);
                        byte(&e,0x38);byte(&e,0xc8);
                    } else {byte(&e,op==0x84?0x84:0x3a);byte(&e,0x47);byte(&e,reg);}
                    emit_save_flags(&e, (op==0x84 || op==0xf6)?0x8c5:0x8d5,
                                    lazy_flags_enabled, d->flags_dead);
                }
            }
        } else if(op==0x99) {
            load_guest_reg(&e, &block->exit_contract, 0);
            byte(&e,0x99); /* CDQ: sign-extend native EAX into native EDX. */
            store_guest_reg_edx(&e, &block->exit_contract, 2);
        } else if(op==0xc9) {
            load_guest_reg(&e, &block->exit_contract, 5);stack_bounds(&e);
            byte(&e,0x8b);byte(&e,0x08);
            byte(&e,0x83);byte(&e,0xc0);byte(&e,4);
            store_guest_reg(&e, &block->exit_contract, 4);
            byte(&e,0x89);byte(&e,0x4f);byte(&e,offsetof(PwX86State,gpr[5]));
            if (get_resident_host_reg(&block->exit_contract, 5) >= 0) {
                emit_load_single(&e, &block->exit_contract, 5);
                block->exit_contract.dirty_mask |= (1 << 5);
            }
        } else if(op==0xf7 && operand.reg>=4) {
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,0,4);byte(&e,0x8b);byte(&e,0x00);}
            emit_spill_dirty(&e, &block->exit_contract);
            block->exit_contract.dirty_mask = 0;
            muldiv_call(&e,operand.reg);
            emit_load_all_resident(&e, &block->exit_contract);
        } else if(op==0xf7 && operand.reg!=0) {
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,2,4);}
            byte(&e,0xf7);byte(&e,(operand.mod==3?0xc0:0)|(operand.reg<<3));
            if(operand.mod==3)store_guest_reg(&e, &block->exit_contract, operand.rm);
            if(operand.reg==3) emit_save_flags(&e, 0x8d5, lazy_flags_enabled, d->flags_dead);
        } else if(op==0x85 || op==0xf7 || op==0xa9) {
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address(&e,0);byte(&e,0x8b);byte(&e,0x00);}
            if(op==0x85){
                int h = get_resident_host_reg(&block->exit_contract, operand.reg);
                if (h >= 0) {
                    byte(&e,0x44);byte(&e,0x85);byte(&e,(uint8_t)(0xc0 | (h << 3)));
                } else {
                    byte(&e,0x85);byte(&e,0x47);byte(&e,operand.reg*4);
                }
            }
            else {byte(&e,0xa9);word(&e,read32(source+cursor+length-4));}
            emit_save_flags(&e, 0x8c5, lazy_flags_enabled, d->flags_dead); /* TEST leaves AF undefined; retain it. */
        } else if(setcc) {
            emit_materialize_flags(&e,branch_condition_flags(source[cursor+1]&15));
            condition_value(&e,source[cursor+1]&15);
            store_guest_byte_eax(&e,&block->exit_contract,operand.rm);
        } else if(cmov) {
            /*
             * CMOVcc. Guest conditions live in PwX86State.eflags, not in the
             * host flags, so the condition is materialised into a 0/1 value
             * (the same helper SETcc uses) and the move is skipped when it
             * is false. The destination is always a register, so the store
             * back is unconditional and correct in both directions.
             */
            emit_materialize_flags(&e,branch_condition_flags(cmov_condition));
            condition_value(&e,cmov_condition);
            /*
             * Keep the 0/1 condition in R11D, which is scratch in every mode:
             * the block's residency contract hands out only three host
             * registers (PW_X86_MAX_HOST_REGS, ids 0..2 = R8, R9, R10), so R11
             * never carries a resident guest value, and both the memory guard
             * and the condition helper save and restore it. EDX cannot be
             * used: the guard computes the address in EAX and uses EDX as its
             * scratch, so a condition kept in EDX is overwritten before the
             * move is selected - which is what made every CMOVcc with a memory
             * source move unconditionally (the form matrix reported exactly
             * the six conditions that are false with EFLAGS zero).
             */
            byte(&e,0x41);byte(&e,0x89);byte(&e,0xc3); /* mov r11d, eax */
            if(operand.mod==3) {
                load_guest_reg_ecx(&e,&block->exit_contract,operand.rm);
            } else {
                effective_address(&e,&operand,&block->exit_contract);
                memory_address(&e,0);
                byte(&e,0x8b);byte(&e,0x08);      /* mov ecx, [rax] */
            }
            load_guest_reg(&e,&block->exit_contract,operand.reg);
            byte(&e,0x45);byte(&e,0x85);byte(&e,0xdb); /* test r11d, r11d */
            /* Skip exactly the two-byte "mov eax, ecx" (89 c8) below; a
             * longer offset would land inside the following instruction. */
            byte(&e,0x74);byte(&e,0x02);
            byte(&e,0x89);byte(&e,0xc8);
            store_guest_reg(&e,&block->exit_contract,operand.reg);
        } else if(op==0x39 || op==0x3b) {
            if(operand.mod==3) {
                const unsigned destination=op==0x39?operand.rm:operand.reg;
                const unsigned source_register=op==0x39?operand.reg:operand.rm;
                if(!emit_guest_binary32(&e,&block->exit_contract,7,
                                        destination,source_register,0)) {
                    load_guest_reg(&e,&block->exit_contract,destination);
                    byte(&e,0x3b);byte(&e,0x47);
                    byte(&e,(uint8_t)(source_register*4u));
                }
            } else {
                effective_address(&e,&operand,&block->exit_contract);
                memory_address(&e,0);byte(&e,0x8b);byte(&e,0x00);
                if(op==0x39){
                    int h = get_resident_host_reg(&block->exit_contract, operand.reg);
                    if (h >= 0) {
                        byte(&e,0x44);byte(&e,0x39);byte(&e,(uint8_t)(0xc0 | (h << 3)));
                    } else {
                        byte(&e,0x3b);byte(&e,0x47);byte(&e,operand.reg*4);
                    }
                } else {
                    byte(&e,0x89);byte(&e,0xc1);
                    load_guest_reg(&e,&block->exit_contract,operand.reg);
                    byte(&e,0x39);byte(&e,0xc8);
                }
            }
            emit_save_flags(&e, 0x8d5, lazy_flags_enabled, d->flags_dead);
        } else if(extend) {
            unsigned opcode=source[cursor+(extend_word_destination?2:1)],width=(opcode&1)?2:1;
            if(operand.mod!=3){effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,0,width);}
            else emit_spill_single(&e, &block->exit_contract, width==1?(operand.rm&3):operand.rm);
            byte(&e,0x0f);byte(&e,opcode);byte(&e,operand.mod==3?0x47:0x00);
            if(operand.mod==3)byte(&e,width==1?(operand.rm&3)*4+(operand.rm>>2):operand.rm*4);
            if(extend_word_destination) {
                emit_spill_single(&e, &block->exit_contract, operand.reg);
                byte(&e,0x66);byte(&e,0x89);byte(&e,0x47);byte(&e,operand.reg*4);
                if (get_resident_host_reg(&block->exit_contract, operand.reg) >= 0) {
                    emit_load_single(&e, &block->exit_contract, operand.reg);
                    block->exit_contract.dirty_mask |= (1 << operand.reg);
                }
            } else store_guest_reg(&e, &block->exit_contract, operand.reg);
        } else if(double_shift) {
            /*
             * SHLD/SHRD, executed by the host instruction on the guest
             * values: destination in EAX (or at [RAX]), source in R11D, count
             * in CL. The flag selection is the one the one-operand shifts
             * use, so a zero count preserves every flag.
             */
            emit_commit_flags(&e);
            if(operand.mod==3)load_guest_reg(&e,&block->exit_contract,operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,2,4);}
            load_guest_reg_ecx(&e,&block->exit_contract,operand.reg);
            byte(&e,0x41);byte(&e,0x89);byte(&e,0xcb);      /* mov r11d, ecx */
            if(double_shift&1)load_guest_reg_ecx(&e,&block->exit_contract,1);
            else {byte(&e,0xb9);word(&e,source[cursor+length-1]);}
            byte(&e,0x83);byte(&e,0xe1);byte(&e,31);
            byte(&e,0xbe);word(&e,0xc5);
            byte(&e,0xba);word(&e,0);
            byte(&e,0x85);byte(&e,0xc9);
            byte(&e,0x0f);byte(&e,0x44);byte(&e,0xf2);
            byte(&e,0xba);word(&e,0x8c5);
            byte(&e,0x83);byte(&e,0xf9);byte(&e,1);
            byte(&e,0x0f);byte(&e,0x44);byte(&e,0xf2);
            /* shld|shrd eax|[rax], r11d, cl */
            byte(&e,0x44);byte(&e,0x0f);byte(&e,(uint8_t)(double_shift|1));
            byte(&e,operand.mod==3?0xd8:0x18);
            if(operand.mod==3)store_guest_reg(&e,&block->exit_contract,operand.rm);
            if (!d->flags_dead) {
                byte(&e,0x9c);byte(&e,0x5a); /* snapshot native flags */
                byte(&e,0x21);byte(&e,0xf2); /* and edx, esi */
                byte(&e,0xf7);byte(&e,0xd6); /* not esi */
                byte(&e,0x23);byte(&e,0x77);byte(&e,offsetof(PwX86State,eflags)); /* and esi, [rdi+eflags] */
                byte(&e,0x09);byte(&e,0xf2); /* or edx, esi */
                byte(&e,0x89);byte(&e,0x57);byte(&e,offsetof(PwX86State,eflags));
            }
        } else if(bit_scan) {
            /*
             * BSF/BSR. The host instruction computes the index on the guest
             * value, and the scan is skipped when the source is zero so the
             * destination is left unchanged deterministically (the ISA
             * leaves it undefined there) while ZF still reports the zero.
             * Only ZF is saved, matching what the architecture defines.
             */
            const unsigned skip=bit_scan_word?4u:3u;
            if(operand.mod==3) {
                load_guest_reg_ecx(&e,&block->exit_contract,operand.rm);
            } else {
                effective_address(&e,&operand,&block->exit_contract);
                memory_address_width(&e,0,bit_scan_word?2u:4u);
                byte(&e,0x8b);byte(&e,0x08);
            }
            if(bit_scan_count) {
                /* TZCNT/LZCNT write the destination for every source, so
                 * the host instruction runs unconditionally and its flags
                 * are the guest's. */
                byte(&e,0xf3);byte(&e,0x0f);byte(&e,bit_scan_opcode);byte(&e,0xc1);
                store_guest_reg(&e,&block->exit_contract,operand.reg);
                emit_save_flags(&e,0x8d5,lazy_flags_enabled,d->flags_dead);
            } else {
                load_guest_reg(&e,&block->exit_contract,operand.reg);
                byte(&e,0x85);byte(&e,0xc9);            /* test ecx, ecx */
                byte(&e,0x74);byte(&e,(uint8_t)skip);
                if(bit_scan_word)byte(&e,0x66);
                byte(&e,0x0f);byte(&e,bit_scan_opcode);byte(&e,0xc1);
                store_guest_reg(&e,&block->exit_contract,operand.reg);
                emit_save_flags(&e,0x40,lazy_flags_enabled,d->flags_dead);
            }
        } else if(sse_kind) {
            /*
             * The SSE slice: data movement, lane unpacking and bitwise logic.
             * The host executes the same operation on host XMM scratch, so
             * the architectural details (upper-bit zeroing on movd/movss,
             * byte lanes of punpck, the 16-byte granularity of movups) come
             * from the CPU rather than from hand-written emulation.
             */
            const unsigned reg=operand.reg;
            if(sse_kind==PW_SSE_MOVD_LOAD) {
                if(operand.mod==3) {
                    load_guest_reg(&e,&block->exit_contract,operand.rm);
                } else {
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,0,4u);
                    byte(&e,0x8b);byte(&e,0x00);
                }
                byte(&e,0x66);byte(&e,0x0f);byte(&e,0x6e);byte(&e,0xc0);
                store_guest_xmm(&e,reg);
            } else if(sse_kind==PW_SSE_MOVD_STORE) {
                load_guest_xmm(&e,reg);
                if(operand.mod==3) {
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0x7e);byte(&e,0xc0);
                    store_guest_reg(&e,&block->exit_contract,operand.rm);
                } else {
                    /*
                     * The store goes straight from host xmm0 to guest memory.
                     * Computing the effective address leaves it in eax, so a
                     * detour through a GPR after that would write the
                     * address itself instead of the value: that is exactly
                     * the shape Wine's RtlFormatCurrentUserKeyPath uses
                     * ("movd %xmm0,(%ebx)") and exactly what this ordering
                     * has to get right.
                     */
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,2,4u);
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0x7e);byte(&e,0x00);
                }
            } else if(sse_kind==PW_SSE_MOVQ_STORE) {
                if(operand.mod==3) {
                    /*
                     * "66 0f d6" with a register operand is the register form
                     * of the same opcode: the low 64 bits move into the *r/m*
                     * register and its upper half is zeroed. Emitting it as a
                     * store, as this path used to, wrote eight bytes to the
                     * address the r/m register happened to hold - memory
                     * corruption that the form matrix found as a translated
                     * window full of another register's value.
                     */
                    load_guest_xmm(&e,operand.rm);          /* destination */
                    load_guest_xmm1(&e,reg);                /* source */
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0xd6);
                    byte(&e,0xc8);                          /* movq xmm0, xmm1 */
                    store_guest_xmm(&e,operand.rm);
                } else {
                    load_guest_xmm(&e,reg);
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,2,8u);
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0xd6);byte(&e,0x00);
                }
            } else if(sse_kind==PW_SSE_XMM_RM) {
                load_guest_xmm(&e,reg);
                if(operand.mod==3) {
                    load_guest_xmm1(&e,operand.rm);
                    if(sse_prefix)byte(&e,sse_prefix);
                    byte(&e,0x0f);byte(&e,sse_opcode);byte(&e,0xc1);
                } else {
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,0,(unsigned)sse_mem_bytes);
                    if(sse_aligned_move(sse_prefix,sse_opcode))require_aligned16(&e);
                    if(sse_prefix)byte(&e,sse_prefix);
                    byte(&e,0x0f);byte(&e,sse_opcode);byte(&e,0x00);
                }
                store_guest_xmm(&e,reg);
            } else if(sse_kind==PW_SSE_MOVMSK) {
                /* pmovmskb r32, xmm/m128: the source is the XMM operand's
                 * lanes, the destination is a guest GPR. */
                if(operand.mod==3) {
                    load_guest_xmm(&e,operand.rm);
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0xd7);byte(&e,0xc0);
                    store_guest_reg(&e,&block->exit_contract,reg);
                } else {
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,0,16u);
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0xd7);byte(&e,0x00);
                    store_guest_reg(&e,&block->exit_contract,reg);
                }
            } else if(sse_kind==PW_SSE_PEXTRW) {
                /* pextrw r32, xmm, imm8: the source is always a register. */
                if(operand.mod!=3) return PW_ERR_UNSUPPORTED;
                load_guest_xmm(&e,operand.rm);
                byte(&e,0x66);byte(&e,0x0f);byte(&e,0xc5);byte(&e,0xc0);
                byte(&e,sse_imm);
                store_guest_reg(&e,&block->exit_contract,reg);
            } else if(sse_kind==PW_SSE_PINSRW) {
                load_guest_xmm(&e,reg);
                if(operand.mod==3) {
                    load_guest_reg_ecx(&e,&block->exit_contract,operand.rm);
                    /* Destination XMM0 in the reg field, the loaded word in
                     * ECX as r/m. */
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0xc4);byte(&e,0xc1);
                } else {
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,0,2u);
                    byte(&e,0x66);byte(&e,0x0f);byte(&e,0xc4);byte(&e,0x00);
                }
                byte(&e,sse_imm);
                store_guest_xmm(&e,reg);
            } else if(sse_kind==PW_SSE_XMM_IMM) {
                load_guest_xmm(&e,reg);
                if(operand.mod==3) {
                    load_guest_xmm1(&e,operand.rm);
                    if(sse_prefix)byte(&e,sse_prefix);
                    byte(&e,0x0f);byte(&e,sse_opcode);byte(&e,0xc1);
                } else {
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,0,(unsigned)sse_mem_bytes);
                    if(sse_prefix)byte(&e,sse_prefix);
                    byte(&e,0x0f);byte(&e,sse_opcode);byte(&e,0x00);
                }
                byte(&e,sse_imm);
                store_guest_xmm(&e,reg);
            } else { /* PW_SSE_STORE_RM */
                load_guest_xmm(&e,reg);
                if(operand.mod==3) {
                    /* Register-to-register movement only (the merging scalar
                     * forms are refused at decode time), so the copy is
                     * symmetric and the destination can be written back. */
                    load_guest_xmm1(&e,operand.rm);
                    if(sse_prefix)byte(&e,sse_prefix);
                    byte(&e,0x0f);byte(&e,sse_opcode);byte(&e,0xc1);
                    store_guest_xmm(&e,operand.rm);
                } else {
                    effective_address(&e,&operand,&block->exit_contract);
                    memory_address_width(&e,2,(unsigned)sse_mem_bytes);
                    if(sse_aligned_move(sse_prefix,sse_opcode))require_aligned16(&e);
                    if(sse_prefix)byte(&e,sse_prefix);
                    byte(&e,0x0f);byte(&e,sse_opcode);byte(&e,0x00);
                }
            }
        } else if(bit_op) {
            /*
             * BT/BTS/BTR/BTC. The host has the same instructions, including
             * the bit-string addressing of a memory operand and the 4/5-bit
             * index masking of a register operand, so the guest instruction
             * is re-emitted on guest values instead of being emulated by
             * hand. BTS/BTR/BTC modify the destination and therefore need
             * write permission; BT only reads.
             */
            const unsigned width=word_operand?2:4;
            const uint8_t opcode=(uint8_t)(bit_op==1?0xa3:bit_op==2?0xab:
                                           bit_op==3?0xb3:0xbb);
            if(operand.mod==3) {
                load_guest_reg(&e,&block->exit_contract,operand.rm);
                if(!bit_imm)
                    load_guest_reg_ecx(&e,&block->exit_contract,operand.reg);
                if(word_operand)byte(&e,0x66);
                byte(&e,0x0f);
                if(bit_imm) {
                    /* mod 11: the destination is host RAX. */
                    byte(&e,0xba);
                    byte(&e,(uint8_t)(0xc0u|((bit_op+3u)<<3)));
                    byte(&e,source[cursor+length-1]);
                } else {
                    byte(&e,opcode);byte(&e,0xc8);   /* mod 11: reg ECX, rm RAX */
                }
                if(bit_op!=1)
                    store_guest_reg(&e,&block->exit_contract,operand.rm);
            } else {
                effective_address(&e,&operand,&block->exit_contract);
                /*
                 * BT/BTS/BTR/BTC address a *bit string*: with a memory
                 * operand the CPU does not touch [base] but the unit at
                 * base + width*(offset SAR (5|4)), so an offset of 40
                 * on a dword operand reads the next dword. The guard has to
                 * validate the address the CPU will really use, not the base
                 * the ModRM names - validating the base alone let a guest
                 * offset of 0x11111111 reach 512 MiB away, which the SSE form
                 * matrix found as a sigill in the host
                 * (tests/test_pw_sse_matrix.py).
                 */
                const unsigned unit_bytes = width;
                const unsigned unit_shift = width == 4u ? 5u : 4u;
                const unsigned unit_mask = width == 4u ? 31u : 15u;

                if(bit_imm) {
                    /* The offset is a translation-time constant, so only the
                     * unit it selects is reachable: at most 28 bytes past the
                     * base for a dword operand. */
                    const unsigned offset = unit_bytes *
                        (source[cursor+length-1] >> unit_shift);
                    if(offset) {
                        byte(&e,0x83);byte(&e,0xc0);byte(&e,(uint8_t)offset);
                    }
                } else {
                    /* The offset is signed: a negative one reaches the
                     * units before the base. The 16-bit forms take the
                     * register's low word, sign-extended. */
                    load_guest_reg_ecx(&e,&block->exit_contract,operand.reg);
                    if(width == 4u) {
                        byte(&e,0x89);byte(&e,0xca);        /* mov edx, ecx */
                    } else {
                        byte(&e,0x0f);byte(&e,0xbf);byte(&e,0xd1);   /* movsx edx, cx */
                    }
                    byte(&e,0xc1);byte(&e,0xfa);
                    byte(&e,(uint8_t)unit_shift);           /* sar edx, 5|4 */
                    byte(&e,0xc1);byte(&e,0xe2);
                    byte(&e,(uint8_t)(unit_bytes == 4u ? 2u : 1u));
                    byte(&e,0x03);byte(&e,0xc2);            /* add eax, edx */
                }
                memory_address_width(&e,bit_op==1?0:2,width);
                if(!bit_imm) {
                    /* The guard may have used ECX for its own call; reload the
                     * index and keep only the bit position inside the unit,
                     * which is what the CPU would have masked for itself. */
                    load_guest_reg_ecx(&e,&block->exit_contract,operand.reg);
                    byte(&e,0x83);byte(&e,0xe1);byte(&e,(uint8_t)unit_mask);
                }
                if(word_operand)byte(&e,0x66);
                byte(&e,0x0f);
                if(bit_imm) {
                    byte(&e,0xba);byte(&e,(uint8_t)((bit_op+3u)<<3));
                    byte(&e,(uint8_t)(source[cursor+length-1] &
                                      (uint8_t)unit_mask));
                } else {
                    byte(&e,opcode);byte(&e,0x08);   /* mod 00: reg ECX, [RAX] */
                }
            }
            emit_save_flags(&e,0x001,lazy_flags_enabled,d->flags_dead);
        } else if(compare) {
            /* A helper call cannot run after EAX contains the destination or
             * effective address.  Resolve pending guest CF first. */
            if(alu==2 || alu==3)
                emit_materialize_flags(&e,0x001);
            int resident_destination=operand.mod==3?
                get_resident_host_reg(&block->exit_contract,operand.rm):-1;
            if(operand.mod==3 && resident_destination<0)
                load_guest_reg(&e,&block->exit_contract,operand.rm);
            else if(operand.mod!=3) {
                effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,alu!=7?2:0,word_operand?2:4);
            }
            {
                const uint8_t *imm=source+cursor+length-(short_imm?1:word_operand?2:4);
                uint32_t value=short_imm?(uint32_t)(int32_t)(int8_t)*imm:
                    word_operand?(uint32_t)imm[0]|(uint32_t)imm[1]<<8:read32(imm);
                /* Import only guest CF for ADC/SBB, never guest control flags. */
                if(alu==2 || alu==3) {
                    byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                    byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                }
                if(word_operand)byte(&e,0x66);
                if(operand.mod==3 && resident_destination>=0) {
                    byte(&e,0x41);
                    byte(&e,(uint8_t)(short_imm?0x83:0x81));
                    byte(&e,(uint8_t)(0xc0u|(alu<<3)|
                                      (unsigned)resident_destination));
                } else if(operand.mod==3)byte(&e,(uint8_t)(5+alu*8));
                else {
                    /* The guest asked for atomicity; ask the host for it. */
                    if(lock_prefix)byte(&e,0xf0);
                    byte(&e,0x81);byte(&e,(uint8_t)(alu*8));
                }
                if(short_imm && operand.mod==3 && resident_destination>=0)
                    byte(&e,(uint8_t)value);
                else if(word_operand){byte(&e,(uint8_t)value);byte(&e,(uint8_t)(value>>8));}
                else word(&e,value);
                if(operand.mod==3 && alu!=7) {
                    if(resident_destination>=0) {
                        block->exit_contract.dirty_mask|=
                            (uint8_t)(1u<<operand.rm);
                    } else if(word_operand) {
                        byte(&e,0x66);
                        byte(&e,0x89);byte(&e,0x47);byte(&e,operand.rm*4);
                        if (get_resident_host_reg(&block->exit_contract, operand.rm) >= 0) {
                            emit_load_single(&e, &block->exit_contract, operand.rm);
                            block->exit_contract.dirty_mask |= (1 << operand.rm);
                        }
                    } else {
                        store_guest_reg(&e, &block->exit_contract, operand.rm);
                    }
                }
                emit_save_flags(&e, (alu==1 || alu==4 || alu==6)?0x8c5:0x8d5,
                                lazy_flags_enabled, d->flags_dead);
            }
        } else if(conditional) {
            unsigned condition=(op==0x0f?source[cursor+1]:op)&15;
            uint32_t delta=op==0x0f?read32(source+cursor+2):(uint32_t)(int32_t)(int8_t)source[cursor+1];
            block->exit.kind = PW_X86_EXIT_CONDITIONAL;
            block->exit.chainable = 1;
            block->exit.target_pc = next + delta;
            block->exit.fallthrough_pc = next;
            conditional_target(&e,condition,next,next+delta,block,count,rcx_flags,host_flags);
            terminal=1;
        } else if(op==0xff && operand.reg<2) {
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,2,4);}
            byte(&e,0xff);byte(&e,(operand.mod==3?0xc0:0)|(operand.reg<<3));
            if(operand.mod==3)store_guest_reg(&e, &block->exit_contract, operand.rm);
            emit_save_flags(&e, 0x8d4, lazy_flags_enabled, d->flags_dead);
            store(&e,offsetof(PwX86State,eip),next);
        } else if (op==0xff) {
            if(operand.mod==3)load_guest_reg(&e, &block->exit_contract, operand.rm);
            else {
                effective_address(&e,&operand,&block->exit_contract);memory_address(&e,0);
                byte(&e,0x8b);byte(&e,0x00);
            }
            byte(&e,0x89);byte(&e,0xc1); /* preserve target across guest push */
            if (operand.reg==2) { /* near indirect call: push next guest PC */
                push_imm(&e,next,&block->exit_contract);
                byte(&e,0x89); byte(&e,0x4f); byte(&e,offsetof(PwX86State,eip));
            } else if (operand.reg==4) { /* near indirect jump */
                byte(&e,0x89); byte(&e,0x4f); byte(&e,offsetof(PwX86State,eip));
            } else { /* FF /6 push r/m32 */
                stack_address(&e,1,&block->exit_contract);
                byte(&e,0x89); byte(&e,0x08);
                store_guest_reg(&e, &block->exit_contract, 4);
                store(&e,offsetof(PwX86State,eip),next);
            }
            if (operand.reg==2 || operand.reg==4) terminal=1, indirect_exit=1;
        } else if (op<=0x33 && ((op&7)==1 || (op&7)==3)) {
            unsigned reverse=(op&7)==1;
            unsigned operation=op>>3;
            unsigned logical=operation==1 || operation==4 || operation==6;
            unsigned dest=reverse?operand.rm:operand.reg;
            unsigned src=reverse?operand.reg:operand.rm;
            if(operation==2 || operation==3)
                emit_materialize_flags(&e,0x001);
            if(operand.mod==3) {
                if(operation==2 || operation==3) {
                    byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                    byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                }
                int direct=emit_guest_binary32(&e,&block->exit_contract,
                                               operation,dest,src,1);
                if(!direct && operation==6 && dest==src) {
                    byte(&e,0x31);byte(&e,0xc0); /* xor eax, eax */
                    store_guest_reg(&e,&block->exit_contract,dest);
                } else if(!direct) {
                    load_guest_reg(&e, &block->exit_contract, dest);
                    int h_src = get_resident_host_reg(&block->exit_contract, src);
                    if (h_src >= 0) {
                        byte(&e,0x41);byte(&e,(uint8_t)(operation*8+3));byte(&e,(uint8_t)(0xc0 | h_src));
                    } else {
                        byte(&e,(uint8_t)(operation*8+3));byte(&e,0x47);byte(&e,src*4);
                    }
                    store_guest_reg(&e, &block->exit_contract, dest);
                }
            } else {
                effective_address(&e,&operand,&block->exit_contract);memory_address_width(&e,reverse?2:0,4);
                if(reverse) {
                    load_guest_reg_ecx(&e, &block->exit_contract, operand.reg);
                    if(operation==2 || operation==3) {
                        byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                        byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                    }
                    byte(&e,op);byte(&e,0x08); /* [rax] op ecx */
                } else {
                    byte(&e,0x8b);byte(&e,0x08); /* read memory before changing its address register */
                    load_guest_reg(&e, &block->exit_contract, operand.reg);
                    if(operation==2 || operation==3) {
                        byte(&e,0x0f);byte(&e,0xba);byte(&e,0x67);
                        byte(&e,offsetof(PwX86State,eflags));byte(&e,0);
                    }
                    byte(&e,(uint8_t)(operation*8+3));byte(&e,0xc1);
                    store_guest_reg(&e, &block->exit_contract, operand.reg);
                }
            }
            /* Logical AF is undefined: retain guest AF deterministically. */
            emit_save_flags(&e, logical?0x8c5:0x8d5, lazy_flags_enabled, d->flags_dead);
        } else if (op==0xc7) {
            uint32_t value=read32(source+cursor+length-4);
            if (operand.mod==3) store_guest_imm(&e, &block->exit_contract, operand.rm, value);
            else {
                effective_address(&e,&operand,&block->exit_contract);memory_address(&e,1);
                byte(&e,0xc7);byte(&e,0x00);word(&e,value);
            }
        } else if (op == 0x64 || op==0xa0 || op==0xa1 || op==0xa2 ||
                   op==0xa3) {
            if (fs_call) {
                /*
                 * The FS-relative indirect call: the target comes from the
                 * guest's own FS block, and the guest return address is pushed
                 * exactly as a register-indirect call pushes it. The block
                 * ends here, so the dispatcher the target names is the next
                 * thing the engine sees - which is where the gate recognises
                 * the Unix-call boundary.
                 */
                fs_address(&e,read32(source+cursor+3));
                byte(&e,0x8b); byte(&e,0x00);          /* mov eax, [rax] */
                byte(&e,0x89); byte(&e,0xc1);          /* preserve the target */
                push_imm(&e,next,&block->exit_contract);
                byte(&e,0x89); byte(&e,0x4f);
                byte(&e,offsetof(PwX86State,eip));
            } else if (op == 0x64 &&
                (source[cursor+1]==0x8b || source[cursor+1]==0x89)) {
                const unsigned reg=(unsigned)((source[cursor+2]>>3)&7u);

                /* fs_address is the bound for every FS form: the offset
                 * must lie inside the guest's FS block, exactly as it does
                 * for the accumulator encodings above. */
                fs_address(&e,read32(source+cursor+3));
                if (source[cursor+1]==0x8b) {
                    byte(&e,0x8b); byte(&e,0x00);
                    store_guest_reg(&e, &block->exit_contract, reg);
                } else {
                    load_guest_reg_ecx(&e, &block->exit_contract, reg);
                    byte(&e,0x89); byte(&e,0x08);
                }
            } else {
                unsigned load=op==0xa0 || op==0xa1 ||
                              (op==0x64 && source[cursor+1]==0xa1);
                unsigned byte_form=op==0xa0 || op==0xa2;
                if(op==0x64)fs_address(&e,read32(source+cursor+2));
                else {byte(&e,0xb8);word(&e,read32(source+cursor+1));memory_address(&e,!load);}
                if (byte_form) {
                    /*
                     * Only the low byte of the accumulator changes, so the
                     * address moves out of EAX first and the guest's own
                     * accumulator is materialised before AL is replaced - the
                     * same ownership the 16-bit forms keep for the upper
                     * half.
                     */
                    byte(&e,0x89); byte(&e,0xc1);       /* mov ecx, eax */
                    load_guest_reg(&e, &block->exit_contract, 0);
                    if (load) {
                        byte(&e,0x8a); byte(&e,0x01);   /* mov al, [rcx] */
                        store_guest_reg(&e, &block->exit_contract, 0);
                    } else {
                        byte(&e,0x88); byte(&e,0x01);   /* mov [rcx], al */
                    }
                } else if (load) {
                    byte(&e,0x8b); byte(&e,0x00);
                    store_guest_reg(&e, &block->exit_contract, 0);
                } else {
                    load_guest_reg_ecx(&e, &block->exit_contract, 0);
                    byte(&e,0x89); byte(&e,0x08);
                }
            }
        } else if (op == 0x89 || op == 0x8b || op == 0x8d || lea_prefixed) {
            if (operand.mod==3) {
                if (operand.reg != operand.rm) {
                    load_guest_reg(&e, &block->exit_contract, (op==0x89 ? operand.reg:operand.rm));
                    store_guest_reg(&e, &block->exit_contract, (op==0x89 ? operand.rm:operand.reg));
                }
            } else {
                effective_address(&e,&operand,&block->exit_contract);
                if (op==0x8d || lea_prefixed)
                    store_guest_reg(&e, &block->exit_contract, operand.reg);
                else {
                    memory_address(&e,op==0x89);
                    if (op==0x8b) {
                        byte(&e,0x8b); byte(&e,0x00);
                        store_guest_reg(&e, &block->exit_contract, operand.reg);
                    } else {
                        load_guest_reg_ecx(&e, &block->exit_contract, operand.reg);
                        byte(&e,0x89); byte(&e,0x08);
                    }
                }
            }
        } else if (op == 0x6a) push_imm(&e,(uint32_t)(int32_t)(int8_t)source[cursor+1], &block->exit_contract);
        else if (op == 0x68) push_imm(&e,read32(source+cursor+1), &block->exit_contract);
        else if (op >= 0x50 && op <= 0x57) {
            stack_address(&e,1,&block->exit_contract);
            load_guest_reg_ecx(&e, &block->exit_contract, op-0x50);
            byte(&e,0x89); byte(&e,0x08); /* [rax] = ecx */
            store_guest_reg(&e, &block->exit_contract, 4);
        } else if (op >= 0x58 && op <= 0x5f) {
            stack_address(&e,0,&block->exit_contract);
            byte(&e,0x8b); byte(&e,0x08); /* ecx = [rax] */
            byte(&e,0x83); byte(&e,0xc0); byte(&e,4);
            store_guest_reg(&e, &block->exit_contract, 4);
            store_guest_reg_ecx(&e, &block->exit_contract, op-0x58);
        }
        else if (op >= 0xb8 && op <= 0xbf)
            store_guest_imm(&e, &block->exit_contract, op-0xb8, read32(source+cursor+1));
        else if (op == 0xe8 || op == 0xe9 || op == 0xeb) {
            if (op == 0xe8) push_imm(&e,next,&block->exit_contract);
            uint32_t delta = op == 0xeb ? (uint32_t)(int32_t)(int8_t)source[cursor+1]
                                      : read32(source+cursor+1);
            next += delta;
            terminal = 1;
            /* A direct call's target is known here, like a jump's: once the
             * return address is pushed it chains the same way. */
            block->exit.kind = PW_X86_EXIT_DIRECT_JUMP;
            block->exit.chainable = 1;
            block->exit.target_pc = next;
            emit_chain_exit(&e, count, next, &block->exit_contract,
                            &block->exit.target_patch_offset, &block->exit.target_stub_offset,
                            &block->exit.target_reconcile_offset, &block->exit.target_reconcile_patch_offset);
        } else if (op == 0xc3 || op==0xc2) {
            stack_address(&e,0,&block->exit_contract);
            byte(&e,0x8b); byte(&e,0x08); /* ecx = guest return */
            uint32_t pop=4+(op==0xc2?((uint32_t)source[cursor+1]|(uint32_t)source[cursor+2]<<8):0);
            byte(&e,0x05);word(&e,pop);
            require_condition(&e,0x73); /* unsigned ESP addition must not wrap */
            byte(&e,0x3b);byte(&e,0x47);byte(&e,offsetof(PwX86State,stack_high));
            require_condition(&e,0x76);
            store_guest_reg(&e, &block->exit_contract, 4);
            byte(&e,0x89); byte(&e,0x4f); byte(&e,offsetof(PwX86State,eip));
            terminal = 1;
            indirect_exit = 1;
        }
        /* fs_call is terminal like the other control transfers: the target it
         * loaded into EIP must not be overwritten by the fall-through. */
        if (op != 0xc3 && op!=0xc2 && op!=0xff && !conditional && op != 0xeb &&
            op != 0xe9 && op != 0xe8 && !fs_call && (!pass || i + 1 == count))
            store(&e,offsetof(PwX86State,eip),next);
        block->instruction_ends[i]=(uint16_t)(cursor + length);
        if (!pass) needs_eip[i] = e.exits != exits_before;
    }
    if (!block->exit.chainable) {
        block->exit.kind = PW_X86_EXIT_DYNAMIC;
        block->exit.chainable = 0;
        emit_spill_dirty(&e, &block->exit_contract);
        uint8_t n_dirty = popcount8(block->exit_contract.dirty_mask);
        if (n_dirty) {
            if (!e.no_counters) { byte(&e, 0x83); byte(&e, 0x47); byte(&e, offsetof(PwX86State, reg_stores)); byte(&e, n_dirty); }
        }
        if (!e.no_counters) { byte(&e, 0x83); byte(&e, 0x47); byte(&e, offsetof(PwX86State, step_retired)); byte(&e, (uint8_t)count); }
        if (indirect_exit && options->indirect_targets)
            emit_indirect_lookup(&e, options->indirect_targets, options->indirect_mask);
        byte(&e, 0x48); byte(&e, 0xc7); byte(&e, 0x47); byte(&e, offsetof(PwX86State, last_exit_slot)); word(&e, 0);
        success(&e);
    }
    emit_cold_paths(&e);
    }
    if (e.failed) return PW_ERR_LIMIT;
    block->source_bytes = insts[count-1].cursor + insts[count-1].length;
    block->code_bytes = e.n;
    block->instructions = count;
    return PW_OK;
#undef DECODE_FAIL
}

int pw_x86_translate(const uint8_t *source, size_t bytes, uint32_t pc,
                     uint8_t *output, size_t capacity, PwX86Block *block)
{
    /* The standalone translator has no engine safepoint at which to commit a
     * pending descriptor, so retain its historical eager-EFLAGS contract. */
    return pw_x86_translate_ext(source, bytes, pc, output, capacity, block, 1, 0);
}
