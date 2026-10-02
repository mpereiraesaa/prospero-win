/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_x86_reencode.h"
#include <stddef.h>
#include <string.h>

enum {
    ALL_FLAGS = 0x8d5, CF = 0x001,
    MAX_INSTS = 32, MAX_COLD = 2 * MAX_INSTS,
    /* Host scratch: r8 (flag save), r9 (guard), r10 (values), r11
     * (addresses and link slots), r14 (saved flags). */
    R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14,
};

/* Guest GPR -> host register: eax ecx edx ebx esp ebp esi edi. */
static const uint8_t host_of[8] = { 0, 1, 2, 3, R12, 5, 6, R13 };

typedef struct Out {
    uint8_t *p;
    size_t n, cap;
    int failed;
} Out;

static void b(Out *o, uint8_t v)
{
    if (o->n >= o->cap) { o->failed = 1; return; }
    o->p[o->n++] = v;
}
static void w16(Out *o, uint32_t v) { b(o, (uint8_t)v); b(o, (uint8_t)(v >> 8)); }
static void w32(Out *o, uint32_t v) { w16(o, v); w16(o, v >> 16); }
static void w64(Out *o, uint64_t v) { w32(o, (uint32_t)v); w32(o, (uint32_t)(v >> 32)); }
static void put32(Out *o, size_t at, uint32_t v)
{
    if (o->failed || at + 4 > o->n) { o->failed = 1; return; }
    for (unsigned i = 0; i < 4; i++) o->p[at + i] = (uint8_t)(v >> (8 * i));
}
/* Point the rel8 at slot to here. */
static void land8(Out *o, size_t slot)
{
    size_t d = o->n - (slot + 1);
    if (o->failed || d > 127) { o->failed = 1; return; }
    o->p[slot] = (uint8_t)d;
}
static size_t jump8(Out *o, uint8_t opcode)
{
    b(o, opcode); b(o, 0);
    return o->n - 1;
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* op r32, [rdi+offset] and the reverse, for host registers 0..15. */
static void state_op(Out *o, uint8_t opcode, unsigned reg, size_t offset)
{
    if (reg >= 8) b(o, 0x44);
    b(o, opcode);
    if (offset < 128) { b(o, (uint8_t)(0x47 | (reg & 7) << 3)); b(o, (uint8_t)offset); }
    else { b(o, (uint8_t)(0x87 | (reg & 7) << 3)); w32(o, (uint32_t)offset); }
}
static void load_state(Out *o, unsigned reg, size_t offset) { state_op(o, 0x8b, reg, offset); }
static void store_state(Out *o, unsigned reg, size_t offset) { state_op(o, 0x89, reg, offset); }
static void store_state_imm(Out *o, size_t offset, uint32_t value)
{
    b(o, 0xc7);
    if (offset < 128) { b(o, 0x47); b(o, (uint8_t)offset); }
    else { b(o, 0x87); w32(o, (uint32_t)offset); }
    w32(o, value);
}

/* Pinned guest state -> PwX86State: registers, then the arithmetic flags
 * from RFLAGS merged into eflags. Clobbers rax and rcx (already stored). */
/* mov [rdi+offset], rsp or mov rsp, [rdi+offset] */
static void rsp_state(Out *o, uint8_t opcode, size_t offset)
{
    b(o, 0x48); b(o, opcode); b(o, 0xa7); w32(o, (uint32_t)offset);
}

static void emit_leave(Out *o, unsigned call_stack)
{
    for (unsigned g = 0; g < 8; g++) store_state(o, host_of[g], g * 4u);
    b(o, 0x9f);                                         /* lahf */
    b(o, 0x0f); b(o, 0x90); b(o, 0xc0);                 /* seto al */
    b(o, 0x0f); b(o, 0xb6); b(o, 0xcc);                 /* movzx ecx, ah */
    b(o, 0x0f); b(o, 0xb6); b(o, 0xc0);                 /* movzx eax, al */
    b(o, 0xc1); b(o, 0xe0); b(o, 11);                   /* shl eax, 11 */
    b(o, 0x09); b(o, 0xc1);                             /* or ecx, eax */
    b(o, 0x81); b(o, 0xe1); w32(o, ALL_FLAGS);          /* and ecx, flags */
    load_state(o, 0, offsetof(PwX86State, eflags));
    b(o, 0x25); w32(o, ~(uint32_t)ALL_FLAGS);           /* and eax, ~flags */
    b(o, 0x09); b(o, 0xc8);                             /* or eax, ecx */
    store_state(o, 0, offsetof(PwX86State, eflags));
    if (call_stack) rsp_state(o, 0x8b, offsetof(PwX86State, host_rsp));
}

/* PwX86State -> pinned guest state: pending lazy flags committed, the
 * arithmetic flags loaded into RFLAGS without popf, then the registers.
 * With a call stack, rsp moves onto it. */
static void emit_enter(Out *o, unsigned call_stack)
{
    load_state(o, 0, offsetof(PwX86State, eflags));
    load_state(o, 1, offsetof(PwX86State, deferred_flags.known_mask));
    load_state(o, 2, offsetof(PwX86State, deferred_flags.raw_flags));
    b(o, 0x31); b(o, 0xc2);                             /* xor edx, eax */
    b(o, 0x21); b(o, 0xca);                             /* and edx, ecx */
    b(o, 0x31); b(o, 0xd0);                             /* xor eax, edx */
    store_state(o, 0, offsetof(PwX86State, eflags));
    store_state_imm(o, offsetof(PwX86State, deferred_flags.known_mask), 0);
    b(o, 0x89); b(o, 0xc1);                             /* mov ecx, eax */
    b(o, 0xc1); b(o, 0xe9); b(o, 11);                   /* shr ecx, 11 */
    b(o, 0x83); b(o, 0xe1); b(o, 1);                    /* and ecx, 1: OF */
    b(o, 0xc1); b(o, 0xe0); b(o, 8);                    /* shl eax, 8: AH = low flags */
    b(o, 0x88); b(o, 0xc8);                             /* mov al, cl */
    b(o, 0x04); b(o, 0x7f);                             /* add al, 0x7f: OF = al */
    b(o, 0x9e);                                         /* sahf */
    for (unsigned g = 0; g < 8; g++) load_state(o, host_of[g], g * 4u);
    if (call_stack) {
        rsp_state(o, 0x89, offsetof(PwX86State, host_rsp));
        rsp_state(o, 0x8b, offsetof(PwX86State, call_stack_top));
    }
}

/* mov r64, r64 and friends between host registers 0..15. */
static void rr(Out *o, uint8_t opcode, unsigned w, unsigned dst, unsigned src)
{
    uint8_t rex = (uint8_t)(0x40 | (w ? 8 : 0) | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0));
    if (rex != 0x40) b(o, rex);
    b(o, opcode);
    b(o, (uint8_t)(0xc0 | (src & 7) << 3 | (dst & 7)));
}

typedef struct Ea { int base, index; unsigned scale; uint32_t disp; } Ea;

/* lea dst32, [ea] with the guest's 32-bit address arithmetic. */
static void ea_lea(Out *o, unsigned dst, const Ea *e)
{
    int hb = e->base >= 0 ? host_of[e->base] : -1, hi = e->index >= 0 ? host_of[e->index] : -1;
    uint8_t rex = (uint8_t)(0x40 | (dst >= 8 ? 4 : 0) | (hi >= 8 ? 2 : 0) | (hb >= 8 ? 1 : 0));
    unsigned index = hi >= 0 ? (unsigned)hi & 7 : 4;

    b(o, 0x67);
    if (rex != 0x40) b(o, rex);
    b(o, 0x8d);
    if (hb < 0) {
        b(o, (uint8_t)((dst & 7) << 3 | 4));
        b(o, (uint8_t)(e->scale << 6 | index << 3 | 5));
    } else {
        b(o, (uint8_t)(0x80 | (dst & 7) << 3 | 4));
        b(o, (uint8_t)(e->scale << 6 | index << 3 | ((unsigned)hb & 7)));
    }
    w32(o, e->disp);
}

typedef enum Kind {
    K_RM = 1, K_PLAIN, K_INCDEC, K_MOVIMM, K_XCHGA, K_BSWAP, K_NOP, K_LEA,
    K_PUSH, K_PUSHIMM, K_PUSHRM, K_POP, K_LEAVE, K_CALL, K_CALLRM, K_RET,
    K_JMP, K_JMPRM, K_JCC, K_STR,
} Kind;
enum { REG8 = 1, REG32, EXT };      /* what the ModRM reg field names */
enum { RM8 = 1, RMW, RMRAW };       /* what a register-form rm names (RMRAW: xmm, mm, st, as is) */

typedef struct Inst {
    uint8_t native_fp_touch;
    Kind kind;
    uint8_t len, opsize16;
    uint8_t op[3], op_len;
    uint8_t mod, reg, rm, reg_kind, rm_kind;
    Ea ea;
    uint8_t imm[4], imm_len;
    uint8_t width, write;           /* memory access: bytes; 0 read, 1 write, 2 both */
    uint32_t def, use;
    uint32_t target, imm_value;
    uint8_t cond;
    uint8_t lock, fs;               /* prefixes */
    uint8_t rep;                    /* 0xf2 or 0xf3: an SSE instruction's mandatory prefix, or 0 */
    const uint8_t *bytes;
} Inst;

static uint32_t condition_flags(unsigned cond)
{
    static const uint32_t f[8] = { 0x800, 0x001, 0x040, 0x041, 0x080, 0x004, 0x880, 0x8c0 };
    return f[(cond >> 1) & 7];
}

/* ModRM, SIB and displacement at s[at]: the bytes, or 0 when truncated. */
static size_t modrm(const uint8_t *s, size_t avail, size_t at, Inst *in)
{
    size_t n = 1;
    unsigned base;

    if (at >= avail) return 0;
    in->mod = s[at] >> 6; in->reg = (s[at] >> 3) & 7; in->rm = s[at] & 7;
    if (in->mod == 3) return 1;
    in->ea.base = -1; in->ea.index = -1; in->ea.scale = 0; in->ea.disp = 0;
    base = in->rm;
    if (in->rm == 4) {
        if (at + 1 >= avail) return 0;
        in->ea.scale = s[at + 1] >> 6;
        if (((s[at + 1] >> 3) & 7) != 4) in->ea.index = (s[at + 1] >> 3) & 7;
        base = s[at + 1] & 7;
        n = 2;
    }
    if (base == 5 && in->mod == 0) {
        if (at + n + 4 > avail) return 0;
        in->ea.disp = rd32(s + at + n);
        return n + 4;
    }
    in->ea.base = (int)base;
    if (in->mod == 1) {
        if (at + n >= avail) return 0;
        in->ea.disp = (uint32_t)(int32_t)(int8_t)s[at + n];
        n++;
    } else if (in->mod == 2) {
        if (at + n + 4 > avail) return 0;
        in->ea.disp = rd32(s + at + n);
        n += 4;
    }
    return n;
}

/* Can the register or memory form be encoded? A REX prefix, needed for
 * r11, r12 or r13, turns AH-BH into SPL-DIL. */
static int encodable(const Inst *in)
{
    unsigned rex = 0, high8 = 0;
    if (in->kind != K_RM) return 1;
    if (in->reg_kind == REG32 && host_of[in->reg] >= 8) rex = 1;
    if (in->reg_kind == REG8 && in->reg >= 4) high8 = 1;
    if (in->mod != 3) rex = 1;
    else {
        if (in->rm_kind == RMW && host_of[in->rm] >= 8) rex = 1;
        if (in->rm_kind == RM8 && in->rm >= 4) high8 = 1;
    }
    return !(rex && high8);
}

/* The instructions a lock prefix may carry, all with a memory destination:
 * the ALU group, not/neg, inc/dec, xchg, cmpxchg, xadd, bts/btr/btc by an
 * immediate, and cmpxchg8b. */
static int lockable(const Inst *in)
{
    uint8_t op = in->op[0], x = in->op[1];
    if (in->kind != K_RM || in->mod == 3) return 0;
    if (in->op_len == 1) {
        if (op < 0x40) return (op & 7) < 2 && (op >> 3) != 7;
        if (op >= 0x80 && op <= 0x83) return in->reg != 7;
        if (op == 0xf6 || op == 0xf7) return in->reg == 2 || in->reg == 3;
        if (op == 0xfe || op == 0xff) return in->reg <= 1;
        return op == 0x86 || op == 0x87;
    }
    return x == 0xb0 || x == 0xb1 || x == 0xc0 || x == 0xc1 || x == 0xc7 || (x == 0xba && in->reg >= 5);
}

/* Decode one instruction this backend takes; 0 for anything else. */
static int decode(const uint8_t *s, size_t avail, uint32_t pc, Inst *in, unsigned native_fp)
{
    size_t i = 0, m;
    uint8_t op;
    unsigned z, rep_ok = 0;

    memset(in, 0, sizeof(*in));
    in->bytes = s;
    /* Operand size, lock, fs, and the segment overrides that name the flat
     * segments (cs ds es ss, and the branch hints on jcc). */
    for (; i < avail && i < 4; i++) {
        if (s[i] == 0x66 && !in->opsize16) in->opsize16 = 1;
        else if (s[i] == 0xf0 && !in->lock) in->lock = 1;
        else if (s[i] == 0x64 && !in->fs) in->fs = 1;
        else if ((s[i] == 0xf2 || s[i] == 0xf3) && !in->rep) in->rep = s[i];
        else if (s[i] != 0x2e && s[i] != 0x3e && s[i] != 0x26 && s[i] != 0x36) break;
    }
    if (i >= avail) return 0;
    op = s[i++];
    z = in->opsize16 ? 2 : 4;
    in->width = (uint8_t)z;
#define NEED(n) do { if (i + (n) > avail) return 0; } while (0)
#define IMM(n) do { NEED(n); memcpy(in->imm, s + i, (n)); in->imm_len = (uint8_t)(n); i += (n); } while (0)
#define MODRM() do { if (!(m = modrm(s, avail, i, in))) return 0; i += m; } while (0)
#define RM(r, k, w8) do { in->kind = K_RM; in->op[0] = op; in->op_len = 1; in->reg_kind = (r); \
        in->rm_kind = (k); if (w8) in->width = 1; MODRM(); } while (0)

    if (op < 0x40 && (op & 7) < 6 && op != 0x0f) {
        unsigned group = op >> 3, w8 = !(op & 1);
        in->def = ALL_FLAGS;
        in->use = (group == 2 || group == 3) ? CF : 0;
        if ((op & 7) < 4) {
            RM(w8 ? REG8 : REG32, w8 ? RM8 : RMW, w8);
            in->write = (op & 2) || group == 7 ? 0 : 2;
        } else {
            in->kind = K_PLAIN;
            if ((op & 7) == 4) IMM(1); else IMM(z);
        }
    } else if (op >= 0x40 && op <= 0x4f) {
        in->kind = K_INCDEC; in->reg = op & 7; in->def = 0x8d4;
    } else if (op >= 0x50 && op <= 0x5f) {
        if (in->opsize16) return 0;
        in->kind = op < 0x58 ? K_PUSH : K_POP; in->reg = op & 7;
    } else if (op == 0x68 || op == 0x6a) {
        if (in->opsize16) return 0;
        in->kind = K_PUSHIMM;
        if (op == 0x68) { NEED(4); in->imm_value = rd32(s + i); i += 4; }
        else { NEED(1); in->imm_value = (uint32_t)(int32_t)(int8_t)s[i]; i++; }
    } else if (op == 0x69 || op == 0x6b) {
        RM(REG32, RMW, 0);
        if (op == 0x69) IMM(z); else IMM(1);
        in->def = ALL_FLAGS;
    } else if ((op >= 0x70 && op <= 0x7f) || op == 0xeb || op == 0xe9 || op == 0xe8) {
        int32_t rel;
        if (in->opsize16) return 0;
        if (op == 0xe9 || op == 0xe8) { NEED(4); rel = (int32_t)rd32(s + i); i += 4; }
        else { NEED(1); rel = (int8_t)s[i]; i++; }
        in->target = pc + (uint32_t)i + (uint32_t)rel;
        if (op < 0x80) { in->kind = K_JCC; in->cond = op & 15; in->use = condition_flags(op & 15); }
        else in->kind = op == 0xe8 ? K_CALL : K_JMP;
    } else if (op == 0x80 || op == 0x81 || op == 0x83) {
        RM(EXT, op == 0x80 ? RM8 : RMW, op == 0x80);
        if (op == 0x81) IMM(z); else IMM(1);
        in->write = in->reg == 7 ? 0 : 2;
        in->def = ALL_FLAGS;
        in->use = (in->reg == 2 || in->reg == 3) ? CF : 0;
    } else if (op == 0x84 || op == 0x85) {
        RM(op == 0x84 ? REG8 : REG32, op == 0x84 ? RM8 : RMW, op == 0x84);
        in->def = ALL_FLAGS;
    } else if (op == 0x86 || op == 0x87) {
        RM(op == 0x86 ? REG8 : REG32, op == 0x86 ? RM8 : RMW, op == 0x86);
        in->write = 2;                            /* with memory: atomic, as on the guest */
    } else if (op >= 0x88 && op <= 0x8b) {
        RM(op & 1 ? REG32 : REG8, op & 1 ? RMW : RM8, !(op & 1));
        in->write = op & 2 ? 0 : 1;
    } else if (op == 0x8d) {
        MODRM();
        if (in->mod == 3 || in->opsize16) return 0;
        in->kind = K_LEA;
    } else if (op == 0x8f) {
        MODRM();
        if (in->mod != 3 || in->reg || in->opsize16) return 0;
        in->kind = K_POP; in->reg = in->rm;
    } else if (op == 0x90) {
        in->kind = in->rep == 0xf3 ? K_PLAIN : K_NOP;   /* pause */
        rep_ok = in->rep == 0xf3;
    } else if (op >= 0x91 && op <= 0x97) {
        in->kind = K_XCHGA; in->reg = op & 7;
    } else if (op == 0x98 || op == 0x99) {
        in->kind = K_PLAIN;
    } else if ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf)) {
        /* movs cmps stos lods scas, with rep/repe/repne: the host's own on
         * the guest's esi, edi and ecx (emit_string). */
        const unsigned compares = op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf;
        if (in->fs || in->lock) return 0;
        in->kind = K_STR;
        in->op[0] = op; in->op_len = 1;
        in->width = (uint8_t)(!(op & 1) ? 1 : in->opsize16 ? 2 : 4);
        if (compares) {
            in->def = ALL_FLAGS;
            if (in->rep) in->use = ALL_FLAGS;     /* a zero count leaves them */
        }
        rep_ok = 1;
    } else if (op == 0x9e || op == 0x9f) {
        in->kind = K_PLAIN;                       /* sahf, lahf: ah is the guest's */
        if (op == 0x9e) in->def = 0x0d5; else in->use = 0x0d5;
    } else if (op == 0x9b && native_fp) {
        in->native_fp_touch = 1;
        in->kind = K_PLAIN;                       /* fwait, as in fstsw = fwait; fnstsw */
    } else if (op >= 0xa0 && op <= 0xa3) {
        static const uint8_t as[4] = { 0x8a, 0x8b, 0x88, 0x89 };
        NEED(4);
        in->kind = K_RM; in->op[0] = as[op - 0xa0]; in->op_len = 1;
        in->reg_kind = op & 1 ? REG32 : REG8; in->rm_kind = op & 1 ? RMW : RM8;
        if (!(op & 1)) in->width = 1;
        in->mod = 0; in->reg = 0; in->rm = 5;
        in->ea.base = -1; in->ea.index = -1; in->ea.disp = rd32(s + i); i += 4;
        in->write = op >= 0xa2 ? 1 : 0;
    } else if (op == 0xa8 || op == 0xa9) {
        in->kind = K_PLAIN; in->def = ALL_FLAGS;
        if (op == 0xa8) IMM(1); else IMM(z);
    } else if (op >= 0xb0 && op <= 0xb7) {
        in->kind = K_PLAIN; IMM(1);
    } else if (op >= 0xb8 && op <= 0xbf) {
        in->kind = K_MOVIMM; in->reg = op & 7; IMM(z);
    } else if (op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3)) {
        unsigned w8 = !(op & 1), count;
        RM(EXT, w8 ? RM8 : RMW, w8);
        if (in->reg == 6) return 0;
        if (op <= 0xc1) IMM(1);
        in->write = 2;
        count = op <= 0xc1 ? (in->imm[0] & 31u) : op <= 0xd1 ? 1u : 0u;
        if (in->reg == 2 || in->reg == 3) in->use = CF;
        /* A zero or unknown count leaves the flags: they flow through. */
        if (count) in->def = in->reg < 4 ? 0x801 : ALL_FLAGS;
    } else if (op == 0xc2 || op == 0xc3) {
        if (in->opsize16) return 0;
        rep_ok = in->rep == 0xf3;                 /* rep ret */
        in->kind = K_RET;
        if (op == 0xc2) { NEED(2); in->imm_value = (uint32_t)s[i] | (uint32_t)s[i + 1] << 8; i += 2; }
    } else if (op == 0xc6 || op == 0xc7) {
        RM(EXT, op == 0xc6 ? RM8 : RMW, op == 0xc6);
        if (in->reg) return 0;
        if (op == 0xc6) IMM(1); else IMM(z);
        in->write = 1;
    } else if (op == 0xc9) {
        if (in->opsize16) return 0;
        in->kind = K_LEAVE;
    } else if (op == 0xf6 || op == 0xf7) {
        RM(EXT, op == 0xf6 ? RM8 : RMW, op == 0xf6);
        switch (in->reg) {
        case 0: if (op == 0xf6) IMM(1); else IMM(z); in->def = ALL_FLAGS; break;
        case 2: in->write = 2; break;
        case 3: in->write = 2; in->def = ALL_FLAGS; break;
        case 4: case 5: in->def = ALL_FLAGS; break;
        default: return 0;                        /* div faults natively */
        }
    } else if (op == 0xfe || op == 0xff) {
        MODRM();
        if (in->reg <= 1) {
            in->kind = K_RM; in->op[0] = op; in->op_len = 1; in->reg_kind = EXT;
            in->rm_kind = op == 0xfe ? RM8 : RMW;
            if (op == 0xfe) in->width = 1;
            in->write = 2; in->def = 0x8d4;
        } else if (op == 0xff && !in->opsize16 && (in->reg == 2 || in->reg == 4 || in->reg == 6)) {
            in->kind = in->reg == 2 ? K_CALLRM : in->reg == 4 ? K_JMPRM : K_PUSHRM;
            in->width = 4;
        } else return 0;
    } else if (op == 0x0f) {
        uint8_t x;
        NEED(1);
        x = s[i++];
        in->op[0] = 0x0f; in->op[1] = x; in->op_len = 2;
        if (x == 0x1f) {
            MODRM();
            in->kind = K_NOP;
        } else if (x >= 0x40 && x <= 0x4f) {
            in->kind = K_RM; in->reg_kind = REG32; in->rm_kind = RMW; MODRM();
            in->use = condition_flags(x & 15);
        } else if (x >= 0x80 && x <= 0x8f) {
            if (in->opsize16) return 0;
            NEED(4);
            in->target = pc + (uint32_t)i + 4 + rd32(s + i); i += 4;
            in->kind = K_JCC; in->cond = x & 15; in->use = condition_flags(x & 15);
        } else if (x >= 0x90 && x <= 0x9f) {
            in->kind = K_RM; in->reg_kind = EXT; in->rm_kind = RM8; in->width = 1; MODRM();
            in->write = 1; in->use = condition_flags(x & 15);
        } else if (x == 0xa3 || x == 0xab || x == 0xb3 || x == 0xbb) {
            in->kind = K_RM; in->reg_kind = REG32; in->rm_kind = RMW; MODRM();
            if (in->mod != 3) return 0;               /* bit strings reach past the operand */
            in->def = CF;
        } else if (x == 0xba) {
            in->kind = K_RM; in->reg_kind = EXT; in->rm_kind = RMW; MODRM();
            if (in->reg < 4) return 0;
            IMM(1);
            in->write = in->reg == 4 ? 0 : 2; in->def = CF;
        } else if (x == 0xa4 || x == 0xac || x == 0xa5 || x == 0xad) {
            in->kind = K_RM; in->reg_kind = REG32; in->rm_kind = RMW; MODRM();
            if (x == 0xa4 || x == 0xac) IMM(1);
            in->write = 2;
        } else if (x == 0xaf || x == 0xbc || x == 0xbd) {
            in->kind = K_RM; in->reg_kind = REG32; in->rm_kind = RMW; MODRM();
            in->def = ALL_FLAGS;
        } else if (x == 0xb6 || x == 0xbe || x == 0xb7 || x == 0xbf) {
            in->kind = K_RM; in->reg_kind = REG32; MODRM();
            in->rm_kind = (x & 1) ? RMW : RM8;
            in->width = (x & 1) ? 2 : 1;
            if ((x & 1) && in->mod == 3 && in->opsize16) return 0;
        } else if (x == 0xb0 || x == 0xb1 || x == 0xc0 || x == 0xc1) {
            /* cmpxchg (eax implied) and xadd, as the host instruction */
            in->kind = K_RM; in->reg_kind = (x & 1) ? REG32 : REG8; in->rm_kind = (x & 1) ? RMW : RM8;
            if (!(x & 1)) in->width = 1;
            MODRM();
            in->write = 2; in->def = ALL_FLAGS;
        } else if (x == 0xc7) {
            in->kind = K_RM; in->reg_kind = EXT; in->rm_kind = RMW; MODRM();
            if (in->mod == 3 || in->reg != 1 || in->opsize16) return 0;
            in->width = 8; in->write = 2; in->def = 0x040;     /* cmpxchg8b: edx:eax, ecx:ebx */
        } else if (x >= 0xc8 && x <= 0xcf) {
            if (in->opsize16) return 0;
            in->kind = K_BSWAP; in->reg = x & 7;
        } else if (x == 0x31) {
            in->kind = K_PLAIN;                       /* rdtsc: as the host's */
        } else if (x == 0x0d || x == 0x18) {
            in->kind = K_RM; in->reg_kind = EXT; in->rm_kind = RMRAW; MODRM();
            if (in->mod == 3) return 0;
            in->width = 1;                            /* prefetch: never faults */
        } else {
            /* MMX, SSE to SSE4.1 on the guest's own registers: xmm0-7 and
             * mm0-7 are the host's while re-encoded code runs (native FP). */
            if (!native_fp) return 0;
            in->native_fp_touch = 1;
            const unsigned p = in->rep ? in->rep : in->opsize16 ? 0x66 : 0;
            unsigned regk = EXT, rmk = RMRAW, imm = 0, store = 0, width = 16, def = 0, mem_only = 0, reg_only = 0;
            if (x == 0x38 || x == 0x3a) {
                NEED(1);
                in->op[2] = s[i++]; in->op_len = 3;
                if (x == 0x3a) imm = 1;
                if (x == 0x38 && in->op[2] >= 0xf0) return 0;          /* movbe, crc32 */
                if (x == 0x3a && (in->op[2] >= 0x14 && in->op[2] <= 0x17)) { rmk = RMW; store = 1; width = 4; }
                if (x == 0x3a && (in->op[2] == 0x20 || in->op[2] == 0x22)) { rmk = RMW; width = 4; }
            } else if (x >= 0x10 && x <= 0x17) {
                store = x == 0x11 || x == 0x13 || x == 0x17;
                if (x == 0x13 || x == 0x17) mem_only = 1;
                width = p == 0xf3 && x <= 0x11 ? 4 : p == 0xf2 && x <= 0x11 ? 8 : x >= 0x12 && x != 0x14 && x != 0x15 ? 8 : 16;
            } else if (x == 0x28 || x == 0x29 || x == 0x2b) {
                store = x != 0x28; if (x == 0x2b) mem_only = 1;
            } else if (x == 0x2a) {
                if (p == 0xf2 || p == 0xf3) { rmk = RMW; width = 4; } else width = 8;
            } else if (x == 0x2c || x == 0x2d) {
                if (p == 0xf2 || p == 0xf3) { regk = REG32; width = p == 0xf2 ? 8 : 4; } else width = 8;
            } else if (x == 0x2e || x == 0x2f) {
                def = ALL_FLAGS; width = p == 0x66 ? 8 : 4;
            } else if (x == 0x50 || x == 0xd7) {
                regk = REG32; reg_only = 1;
            } else if ((x >= 0x51 && x <= 0x6d) || x == 0x6f || (x >= 0x74 && x <= 0x76) ||
                       x == 0x7c || x == 0x7d || (x >= 0xd0 && x <= 0xd5) || (x >= 0xd8 && x <= 0xe6) ||
                       (x >= 0xe8 && x <= 0xef) || (x >= 0xf1 && x <= 0xf6) || (x >= 0xf8 && x <= 0xfe)) {
                if ((p == 0xf3 || p == 0xf2) && ((x >= 0x51 && x <= 0x5f && x != 0x5b) || x == 0xe6))
                    width = p == 0xf3 ? 4 : 8;
            } else if (x == 0x6e || (x == 0x7e && p != 0xf3)) {
                rmk = RMW; width = 4; store = x == 0x7e;
            } else if (x == 0x7e) {
                width = 8;                                             /* movq xmm, xmm/m64 */
            } else if (x == 0x7f || x == 0xd6 || x == 0xe7) {
                store = 1; if (x == 0xe7) mem_only = 1;
                if (x == 0xd6) width = 8;
            } else if (x == 0x70 || x == 0xc2 || x == 0xc6) {
                imm = 1;
            } else if (x >= 0x71 && x <= 0x73) {
                imm = 1; reg_only = 1;
            } else if (x == 0xc4) {
                rmk = RMW; imm = 1; width = 2;
            } else if (x == 0xc5) {
                regk = REG32; imm = 1; reg_only = 1;
            } else if (x == 0xc3) {
                regk = REG32; rmk = RMW; store = 1; width = 4; mem_only = 1;   /* movnti */
            } else if (x == 0xf0 && p == 0xf2) {
                mem_only = 1;                                          /* lddqu */
            } else if (x == 0x77) {
                in->kind = K_PLAIN; rep_ok = 1;                        /* emms */
                goto done;
            } else if (x == 0xae) {
                MODRM();
                if (in->mod == 3) {
                    if (in->reg < 5) return 0;
                    in->kind = K_PLAIN;                                /* lfence mfence sfence */
                } else {
                    if (in->reg < 2 || in->reg == 4 || in->reg == 6) return 0;   /* fxsave, fxrstor, xsave */
                    in->kind = K_RM; in->reg_kind = EXT; in->rm_kind = RMRAW;
                    in->width = in->reg == 7 ? 1 : 4; in->write = in->reg == 3;  /* ldmxcsr stmxcsr clflush */
                }
                goto done;
            } else return 0;
            in->kind = K_RM; in->reg_kind = (uint8_t)regk; in->rm_kind = (uint8_t)rmk;
            MODRM();
            if ((mem_only && in->mod == 3) || (reg_only && in->mod != 3)) return 0;
            if (imm) IMM(1);
            in->width = (uint8_t)width;
            in->write = (uint8_t)(in->mod != 3 && store);
            in->def = def;
            rep_ok = 1;
        done:;
        }
    } else if (op >= 0xd8 && op <= 0xdf && native_fp) {
        in->native_fp_touch = 1;
        /* x87 on the guest's own stack (native FP): register forms as they
         * are, memory forms with the guest's address. */
        static const uint8_t widths[8][8] = {
            { 4, 4, 4, 4, 4, 4, 4, 4 }, { 4, 0, 4, 4, 28, 2, 28, 2 },
            { 4, 4, 4, 4, 4, 4, 4, 4 }, { 4, 4, 4, 4, 0, 10, 0, 10 },
            { 8, 8, 8, 8, 8, 8, 8, 8 }, { 8, 8, 8, 8, 108, 0, 108, 2 },
            { 2, 2, 2, 2, 2, 2, 2, 2 }, { 2, 2, 2, 2, 10, 8, 10, 8 },
        };
        static const uint8_t stores[8] = { 0x00, 0xcc, 0x00, 0x8e, 0x00, 0xce, 0x00, 0xce };
        MODRM();
        if (in->mod == 3) {
            const unsigned r = s[i - 1];
            in->kind = K_PLAIN;
            if ((op == 0xdb || op == 0xdf) && r >= 0xe8 && r <= 0xf7) in->def = ALL_FLAGS;   /* fcomi, fucomi */
            if ((op == 0xda || op == 0xdb) && r < 0xe0) {                                      /* fcmovcc */
                static const uint32_t use[4] = { 0x001, 0x040, 0x041, 0x004 };
                in->use = use[(r >> 3) & 3];
            }
        } else {
            const unsigned w = widths[op - 0xd8][in->reg];
            if (!w) return 0;
            in->kind = K_RM; in->op[0] = op; in->op_len = 1; in->reg_kind = EXT; in->rm_kind = RMRAW;
            in->width = (uint8_t)(w > 255 ? 255 : w);
            in->write = (uint8_t)((stores[op - 0xd8] >> in->reg) & 1);
        }
    } else {
        return 0;
    }
#undef RM
#undef MODRM
#undef IMM
#undef NEED
    if (i > 15) return 0;
    /* F2 on a jump, call or return is MPX's bnd, which a processor without
     * MPX ignores: FFmpeg's assembly (LAV Filters) has bnd jcc, jmp and ret.
     * These are emitted afresh, so the prefix is dropped. */
    if (in->rep == 0xf2 && (in->kind == K_JCC || in->kind == K_JMP || in->kind == K_CALL ||
                            in->kind == K_RET || in->kind == K_JMPRM || in->kind == K_CALLRM))
        rep_ok = 1;
    if (in->rep && !rep_ok) return 0;
    in->len = (uint8_t)i;
    /* fs only on a memory operand; lock only on a read-modify-write of one. */
    if (in->fs && !(in->kind == K_RM && in->mod != 3)) return 0;
    if (in->lock && !lockable(in)) return 0;
    return encodable(in);
}

typedef struct Cold {
    size_t patch;   /* the guard's jump to it (0 with fault markers) */
    size_t site;    /* with fault markers: the access that faults to it */
    uint32_t pc;
    uint8_t width, write, saved;
    uint8_t direct; /* the access addressed ea itself; r11 is not set */
    Ea ea;
} Cold;

typedef struct Ctx {
    Out o;
    uint32_t flat_low, flat_span;
    unsigned fault_markers;
    const PwX86IndirectTarget *table, *chain_table;
    uint32_t mask;
    Cold cold[MAX_COLD];
    unsigned cold_count;
    uint32_t here;  /* the guest EIP of the instruction being translated */
    uint32_t block_pc;
    unsigned bounded;  /* chains spend the budget (!unbounded_chains) */
    unsigned call_stack;
    /* Side exits (PwX86TranslateOptions.superblocks): each jcc's rel32 and
     * target. */
    unsigned side_count;
    size_t side_rel[MAX_INSTS];
    uint32_t side_target[MAX_INSTS];
} Ctx;

static void save_flags(Out *o)
{
    rr(o, 0x89, 1, R8, 0);                      /* mov r8, rax */
    b(o, 0x9f);                                 /* lahf */
    b(o, 0x0f); b(o, 0x90); b(o, 0xc0);         /* seto al */
    rr(o, 0x89, 1, R14, 0);                     /* mov r14, rax */
    rr(o, 0x89, 1, 0, R8);                      /* mov rax, r8 */
}
static void restore_flags(Out *o)
{
    rr(o, 0x89, 1, R8, 0);                      /* mov r8, rax */
    rr(o, 0x89, 1, 0, R14);                     /* mov rax, r14 */
    b(o, 0x04); b(o, 0x7f);                     /* add al, 0x7f: OF */
    b(o, 0x9e);                                 /* sahf */
    rr(o, 0x89, 1, 0, R8);                      /* mov rax, r8 */
}

/* r11 = the guest address of e, checked against the flat range; a miss
 * stops the block as a refused access (the cold paths after the block).
 * The flags survive when keep is set. */
static void guard_fs(Ctx *c, const Ea *e, unsigned fs, unsigned width, unsigned write, unsigned keep)
{
    Out *o = &c->o;
    Cold *cold;

    ea_lea(o, R11, e);
    if (fs) {
        /* The guest's fs is a base in PwX86State; add it without flags. */
        load_state(o, R9, offsetof(PwX86State, fs_base));
        b(o, 0x47); b(o, 0x8d); b(o, 0x1c); b(o, 0x0b);                  /* lea r11d, [r11+r9] */
    }
    if (c->cold_count >= MAX_COLD) { o->failed = 1; return; }
    if (c->fault_markers) {
        /* No check: the access emitted next faults instead, and the block's
         * fault table sends that fault to the cold path with the flags
         * still live. */
        cold = &c->cold[c->cold_count++];
        memset(cold, 0, sizeof(*cold));
        cold->site = o->n;
        cold->pc = c->here;
        cold->width = (uint8_t)width; cold->write = (uint8_t)write;
        return;
    }
    if (keep) save_flags(o);
    b(o, 0x45); b(o, 0x8d); b(o, 0x8b); w32(o, 0u - c->flat_low);    /* lea r9d, [r11-low] */
    b(o, 0x41); b(o, 0x81); b(o, 0xf9); w32(o, c->flat_span - width); /* cmp r9d, span-width */
    b(o, 0x0f); b(o, 0x87);                                          /* ja cold */
    cold = &c->cold[c->cold_count++];
    memset(cold, 0, sizeof(*cold));
    cold->patch = o->n; w32(o, 0);
    cold->pc = c->here;
    cold->width = (uint8_t)width; cold->write = (uint8_t)write; cold->saved = (uint8_t)keep;
    if (keep) restore_flags(o);
}
static void guard(Ctx *c, const Ea *e, unsigned width, unsigned write, unsigned keep)
{
    guard_fs(c, e, 0, width, write, keep);
}

/* An instruction's opcode and ModRM with the memory operand at [r11]. */
static void emit_rm(Ctx *c, const Inst *in)
{
    Out *o = &c->o;
    uint8_t rex = 0;
    unsigned regf = in->reg, rmf = in->rm;

    if (in->lock) b(o, 0xf0);
    if (in->opsize16) b(o, 0x66);
    if (in->rep) b(o, in->rep);
    if (in->reg_kind == REG32 && host_of[in->reg] >= 8) rex |= 4;
    if (in->reg_kind == REG32) regf = host_of[in->reg] & 7;
    if (in->mod == 3) {
        if (in->rm_kind == RMW && host_of[in->rm] >= 8) rex |= 1;
        if (in->rm_kind == RMW) rmf = host_of[in->rm] & 7;
    } else {
        rex |= 1;
        rmf = 3;
    }
    if (rex) b(o, (uint8_t)(0x40 | rex));
    for (unsigned k = 0; k < in->op_len; k++) b(o, in->op[k]);
    b(o, (uint8_t)((in->mod == 3 ? 0xc0 : 0) | regf << 3 | rmf));
    for (unsigned k = 0; k < in->imm_len; k++) b(o, in->imm[k]);
}

/* With fault markers: the instruction itself, its memory operand addressed
 * with the guest's 32-bit arithmetic (0x67: the 32-bit effective address,
 * zero-extended, is the guest address), recorded in the fault table. */
static void emit_rm_direct(Ctx *c, const Inst *in)
{
    Out *o = &c->o;
    const Ea *e = &in->ea;
    const int hb = e->base >= 0 ? host_of[e->base] : -1, hi = e->index >= 0 ? host_of[e->index] : -1;
    const unsigned index = hi >= 0 ? (unsigned)hi & 7 : 4;
    const int32_t disp = (int32_t)e->disp;
    unsigned regf = in->reg;
    uint8_t rex = 0;
    Cold *cold;

    if (c->cold_count >= MAX_COLD) { o->failed = 1; return; }
    cold = &c->cold[c->cold_count++];
    memset(cold, 0, sizeof(*cold));
    cold->site = o->n;
    cold->pc = c->here;
    cold->width = in->width; cold->write = in->write;
    cold->direct = 1; cold->ea = *e;

    if (in->lock) b(o, 0xf0);
    if (in->opsize16) b(o, 0x66);
    if (in->rep) b(o, in->rep);
    b(o, 0x67);
    if (in->reg_kind == REG32) {
        if (host_of[in->reg] >= 8) rex |= 4;
        regf = host_of[in->reg] & 7;
    }
    if (hi >= 8) rex |= 2;
    if (hb >= 8) rex |= 1;
    if (rex) b(o, (uint8_t)(0x40 | rex));
    for (unsigned k = 0; k < in->op_len; k++) b(o, in->op[k]);
    if (hb < 0) {
        b(o, (uint8_t)(regf << 3 | 4));
        b(o, (uint8_t)(e->scale << 6 | index << 3 | 5));
        w32(o, e->disp);
    } else if (!disp && (hb & 7) != 5) {
        b(o, (uint8_t)(regf << 3 | 4));
        b(o, (uint8_t)(e->scale << 6 | index << 3 | ((unsigned)hb & 7)));
    } else if (disp >= -128 && disp <= 127) {
        b(o, (uint8_t)(0x40 | regf << 3 | 4));
        b(o, (uint8_t)(e->scale << 6 | index << 3 | ((unsigned)hb & 7)));
        b(o, (uint8_t)disp);
    } else {
        b(o, (uint8_t)(0x80 | regf << 3 | 4));
        b(o, (uint8_t)(e->scale << 6 | index << 3 | ((unsigned)hb & 7)));
        w32(o, e->disp);
    }
    for (unsigned k = 0; k < in->imm_len; k++) b(o, in->imm[k]);
}

/* push the 32-bit value in host register src (or imm32 when src < 0). */
/* With fault markers: one stack or frame access, [base + disp] with the
 * guest's 32-bit address, listed in the fault table. opcode is 0x89 (store
 * host register reg), 0x8b (load it) or 0xc7 (store imm32). */
static void emit_frame_access(Ctx *c, uint8_t opcode, unsigned reg, unsigned base, int8_t disp,
                              unsigned write, uint32_t imm)
{
    Out *o = &c->o;
    const unsigned hb = host_of[base];
    const uint8_t rex = (uint8_t)((reg >= 8 ? 4 : 0) | (hb >= 8 ? 1 : 0));
    Cold *cold;

    if (c->cold_count >= MAX_COLD) { o->failed = 1; return; }
    cold = &c->cold[c->cold_count++];
    memset(cold, 0, sizeof(*cold));
    cold->site = o->n;
    cold->pc = c->here;
    cold->width = 4; cold->write = (uint8_t)write;
    cold->direct = 1;
    cold->ea.base = (int)base; cold->ea.index = -1; cold->ea.disp = (uint32_t)(int32_t)disp;
    b(o, 0x67);
    if (rex) b(o, (uint8_t)(0x40 | rex));
    b(o, opcode);
    b(o, (uint8_t)(0x40 | (reg & 7) << 3 | ((hb & 7) == 4 ? 4 : hb & 7)));   /* mod 01: disp8 */
    if ((hb & 7) == 4) b(o, 0x24);                                          /* SIB: base only */
    b(o, (uint8_t)disp);
    if (opcode == 0xc7) w32(o, imm);
}

static void lea_r12_r12(Out *o, int8_t n)
{
    b(o, 0x45); b(o, 0x8d); b(o, 0x64); b(o, 0x24); b(o, (uint8_t)n);   /* lea r12d, [r12+n] */
}

static void emit_push(Ctx *c, int src, uint32_t imm, unsigned keep)
{
    Out *o = &c->o;
    const Ea top = { 4, -1, 0, (uint32_t)-4 };

    if (c->fault_markers) {
        /* Store below esp first: a fault leaves esp as it was. */
        if (src < 0) emit_frame_access(c, 0xc7, 0, 4, -4, 1, imm);
        else emit_frame_access(c, 0x89, (unsigned)src, 4, -4, 1, 0);
        lea_r12_r12(o, -4);
        return;
    }
    guard(c, &top, 4, 1, keep);
    if (src < 0) { b(o, 0x41); b(o, 0xc7); b(o, 0x03); w32(o, imm); }   /* mov [r11], imm32 */
    else { b(o, (uint8_t)(0x41 | (src >= 8 ? 4 : 0))); b(o, 0x89); b(o, (uint8_t)((src & 7) << 3 | 3)); }
    rr(o, 0x89, 0, R12, R11);                                           /* mov r12d, r11d */
}
/* r10d = [r11] */
static void load_r10(Out *o) { b(o, 0x45); b(o, 0x8b); b(o, 0x13); }
/* r10d = the r/m operand of in (a register or memory, guarded). */
static void operand_r10(Ctx *c, const Inst *in, unsigned keep)
{
    if (in->mod == 3) { rr(&c->o, 0x89, 0, R10, host_of[in->rm]); return; }
    guard(c, &in->ea, 4, 0, keep);
    load_r10(&c->o);
}

typedef struct ExitSlots {
    size_t patch, stub, reconcile, reconcile_patch, direct;
} ExitSlots;

/* jmp rel32 to a later offset, patched by land32. */
static size_t jump32(Out *o) { b(o, 0xe9); w32(o, 0); return o->n - 4; }
static void land32(Out *o, size_t rel) { put32(o, rel, (uint32_t)(o->n - (rel + 4))); }

static void mov_r9_rcx(Out *o) { b(o, 0x49); b(o, 0x89); b(o, 0xc9); }
static void mov_rcx_r9(Out *o) { b(o, 0x4c); b(o, 0x89); b(o, 0xc9); }

/* The budget a backward exit spends: the chain returns to the dispatcher
 * when it runs out. Every cycle of linked blocks has an exit whose target
 * is at or before its block's PC, so charging only those bounds a chain.
 * Flag-free: the count goes through ecx for jrcxz, with the guest's rcx
 * kept in r9 (a mov, where an xchg would cost three uops). Returns the
 * rel8 of the jrcxz taken when the budget is spent, with rcx still in r9. */
static size_t emit_budget(Out *o)
{
    const size_t budget = offsetof(PwX86State, chain_budget);
    size_t to_spent;

    mov_r9_rcx(o);
    load_state(o, 1, budget);                                       /* mov ecx, budget */
    b(o, 0x8d); b(o, 0x49); b(o, 0xff);                             /* lea ecx, [rcx-1] */
    store_state(o, 1, budget);
    to_spent = jump8(o, 0xe3);                                      /* jrcxz spent */
    mov_rcx_r9(o);
    return to_spent;
}

/* A direct exit to target. The exit is a rel32 (slots->direct: the jcc's
 * own when jcc_rel is set, else a jmp here) that points at the unlinked
 * stub until the engine links it to the target's chain entry. */
static void emit_chain_exit(Ctx *c, uint32_t target, ExitSlots *slots, size_t jcc_rel)
{
    Out *o = &c->o;
    const int backward = c->bounded && target <= c->block_pc;
    size_t to_spent = 0, spent_rel = 0, stub_store;

    if (backward) {
        if (jcc_rel) { land32(o, jcc_rel); jcc_rel = 0; }
        to_spent = emit_budget(o);
    }
    if (jcc_rel) slots->direct = jcc_rel;
    else slots->direct = jump32(o);
    if (backward) {
        land8(o, to_spent);
        mov_rcx_r9(o);
        b(o, 0x41); b(o, 0xbb); w32(o, 0);                          /* mov r11d, 0 */
        spent_rel = jump32(o);
    }
    /* Unlinked: record the slot for the dispatcher to link. */
    slots->stub = o->n;
    land32(o, slots->direct);
    b(o, 0x49); b(o, 0xbb); slots->patch = o->n; w64(o, 0);         /* movabs r11, slot */
    stub_store = o->n;
    b(o, 0x4c); b(o, 0x89); b(o, 0x5f); b(o, (uint8_t)offsetof(PwX86State, last_exit_slot));
    emit_leave(o, c->call_stack);
    store_state_imm(o, offsetof(PwX86State, eip), target);
    b(o, 0x31); b(o, 0xc0); b(o, 0xc3);                             /* xor eax, eax; ret */
    if (backward) put32(o, spent_rel, (uint32_t)(stub_store - (spent_rel + 4)));
    /* A linked block with another contract: its canonical entry. */
    slots->reconcile = o->n;
    emit_leave(o, c->call_stack);
    b(o, 0x49); b(o, 0xbb); slots->reconcile_patch = o->n; w64(o, 0);
    b(o, 0x41); b(o, 0xff); b(o, 0x23);
}

/* Restore once per invocation, on the first reached FP/SIMD block. This
 * must precede its faulting access and preserve all guest arithmetic flags. */
static void emit_fp_prepare(Out *o)
{
    size_t to_prepare, to_done;
    mov_r9_rcx(o);
    load_state(o, 1, offsetof(PwX86State, native_fp_active));
    to_prepare = jump8(o, 0xe3);                         /* jrcxz prepare */
    mov_rcx_r9(o);
    to_done = jump8(o, 0xeb);
    land8(o, to_prepare);
    mov_rcx_r9(o);
    b(o, 0x4c); b(o, 0x8b); b(o, 0x9f); w32(o, offsetof(PwX86State, native_fp_image)); /* mov r11,[rdi+image] */
    b(o, 0x41); b(o, 0x0f); b(o, 0xae); b(o, 0x0b);     /* fxrstor [r11] */
    store_state_imm(o, offsetof(PwX86State, native_fp_active), 1);
    land8(o, to_done);
}

/* pw_x86_chain_slot(r10d), retaining guest arithmetic flags. Dynamic
 * exits and superblock side exits share this published-table index. */
static void emit_chain_slot(Out *o)
{
    b(o, 0x45); b(o, 0x89); b(o, 0xd3);                            /* mov r11d, r10d */
    b(o, 0x41); b(o, 0x0f); b(o, 0xcb);                            /* bswap r11d */
    b(o, 0x47); b(o, 0x8d); b(o, 0x1c); b(o, 0x13);                /* lea r11d, [r11+r10] */
    b(o, 0x45); b(o, 0x0f); b(o, 0xb7); b(o, 0xdb);                /* movzx r11d, r11w */
}

/* The dynamic exit to the guest EIP in r10d: the indirect table when the
 * translation has one, otherwise back to the dispatcher. */
static void emit_dynamic_exit(Ctx *c)
{
    Out *o = &c->o;

    if (c->chain_table) {
        /* A re-encoded target is entered at its chain entry with the guest
         * state still pinned: a full-PC mixed slot, its
         * PC compared as not(slot) + pc + 1 == 0, all without flags. */
        const size_t budget = offsetof(PwX86State, chain_budget);
        uint64_t base = (uint64_t)(uintptr_t)c->chain_table;
        size_t to_hit, to_miss, to_spent = 0;
        emit_chain_slot(o);
        b(o, 0x4e); b(o, 0x8d); b(o, 0x1c); b(o, 0xdd); w32(o, 0);      /* lea r11, [r11*8] */
        b(o, 0x4f); b(o, 0x8d); b(o, 0x1c); b(o, 0x1b);                 /* lea r11, [r11+r11] */
        b(o, 0x49); b(o, 0xb9); w64(o, base);                           /* movabs r9, table */
        b(o, 0x4f); b(o, 0x8d); b(o, 0x1c); b(o, 0x19);                 /* lea r11, [r9+r11] */
        mov_r9_rcx(o);
        b(o, 0x41); b(o, 0x8b); b(o, 0x0b);                             /* mov ecx, [r11] */
        b(o, 0xf7); b(o, 0xd1);                                         /* not ecx */
        b(o, 0x42); b(o, 0x8d); b(o, 0x4c); b(o, 0x11); b(o, 1);        /* lea ecx, [rcx+r10+1] */
        to_hit = jump8(o, 0xe3);                                        /* jrcxz hit */
        mov_rcx_r9(o);
        to_miss = jump8(o, 0xeb);
        land8(o, to_hit);
        if (c->bounded) {
            load_state(o, 1, budget);                                   /* mov ecx, budget */
            b(o, 0x8d); b(o, 0x49); b(o, 0xff);                         /* lea ecx, [rcx-1] */
            store_state(o, 1, budget);
            to_spent = jump8(o, 0xe3);                                  /* jrcxz spent */
        }
        mov_rcx_r9(o);
        b(o, 0x4d); b(o, 0x8b); b(o, 0x5b); b(o, (uint8_t)offsetof(PwX86IndirectTarget, host_code));
        b(o, 0x41); b(o, 0xff); b(o, 0xe3);                             /* jmp r11 */
        if (c->bounded) {
            land8(o, to_spent);
            mov_rcx_r9(o);
        }
        land8(o, to_miss);
    }
    store_state(o, R10, offsetof(PwX86State, eip));
    emit_leave(o, c->call_stack);
    if (c->table) {
        size_t budget_patch, miss_patch, empty_patch;
        uint64_t base = (uint64_t)(uintptr_t)c->table;
        b(o, 0xff); b(o, 0x4f); b(o, (uint8_t)offsetof(PwX86State, chain_budget)); /* dec budget */
        budget_patch = jump8(o, 0x74);
        load_state(o, 0, offsetof(PwX86State, eip));
        b(o, 0x89); b(o, 0xc2);                                     /* mov edx, eax */
        b(o, 0xc1); b(o, 0xea); b(o, 12);                           /* shr edx, 12 */
        b(o, 0x31); b(o, 0xc2);                                     /* xor edx, eax */
        b(o, 0x81); b(o, 0xe2); w32(o, c->mask);                    /* and edx, mask */
        b(o, 0xc1); b(o, 0xe2); b(o, 4);                            /* shl edx, 4 */
        b(o, 0x49); b(o, 0xbb); w64(o, base);                       /* movabs r11, table */
        b(o, 0x49); b(o, 0x01); b(o, 0xd3);                         /* add r11, rdx */
        b(o, 0x41); b(o, 0x3b); b(o, 0x03);                         /* cmp eax, [r11] */
        miss_patch = jump8(o, 0x75);
        b(o, 0x4d); b(o, 0x8b); b(o, 0x5b); b(o, (uint8_t)offsetof(PwX86IndirectTarget, host_code));
        b(o, 0x4d); b(o, 0x85); b(o, 0xdb);                         /* test r11, r11 */
        empty_patch = jump8(o, 0x74);
        b(o, 0x41); b(o, 0xff); b(o, 0xe3);                         /* jmp r11 */
        land8(o, budget_patch); land8(o, miss_patch); land8(o, empty_patch);
    }
    b(o, 0x31); b(o, 0xc0); b(o, 0xc3);
}

/* A guest call on the call stack: the guest's return address pushed, then a
 * host call, to the callee's link (direct) or to the lookup of r10d
 * (dynamic). The callee's ret comes back right after it, with the guest's
 * return address in r10d: when it is next, go on to next through a link,
 * otherwise look it up. */
static void emit_call(Ctx *c, PwX86Block *block, uint32_t next, uint32_t target, int dynamic, unsigned keep)
{
    Out *o = &c->o;
    ExitSlots callee, rest;
    size_t call_rel, to_ok, to_lookup, rest_rel;

    emit_push(c, -1, next, keep);
    b(o, 0xe8); call_rel = o->n; w32(o, 0);                         /* call callee */
    mov_r9_rcx(o);
    b(o, 0x41); b(o, 0x8d); b(o, 0x8a); w32(o, 0u - next);          /* lea ecx, [r10-next] */
    to_ok = jump8(o, 0xe3);                                         /* jrcxz ok */
    mov_rcx_r9(o);
    to_lookup = jump32(o);
    land8(o, to_ok);
    mov_rcx_r9(o);
    rest_rel = jump32(o);                                           /* jmp next */
    emit_chain_exit(c, next, &rest, rest_rel);
    memset(&callee, 0, sizeof(callee));
    if (!dynamic) emit_chain_exit(c, target, &callee, call_rel);
    else land32(o, call_rel);
    land32(o, to_lookup);
    emit_dynamic_exit(c);

    if (dynamic) {
        /* The only link is the return's continuation. */
        block->exit.kind = PW_X86_EXIT_DIRECT_JUMP;
        block->exit.chainable = 1;
        block->exit.target_pc = next;
        block->exit.target_patch_offset = rest.patch;
        block->exit.target_stub_offset = rest.stub;
        block->exit.target_reconcile_offset = rest.reconcile;
        block->exit.target_reconcile_patch_offset = rest.reconcile_patch;
        block->exit.target_direct_offset = rest.direct;
        return;
    }
    /* Two links, as a conditional branch has: the callee and the return's
     * continuation. */
    block->exit.kind = PW_X86_EXIT_CONDITIONAL;
    block->exit.chainable = 1;
    block->exit.target_pc = target;
    block->exit.fallthrough_pc = next;
    block->exit.target_patch_offset = callee.patch;
    block->exit.target_stub_offset = callee.stub;
    block->exit.target_reconcile_offset = callee.reconcile;
    block->exit.target_reconcile_patch_offset = callee.reconcile_patch;
    block->exit.target_direct_offset = callee.direct;
    block->exit.fallthrough_patch_offset = rest.patch;
    block->exit.fallthrough_stub_offset = rest.stub;
    block->exit.fallthrough_reconcile_offset = rest.reconcile;
    block->exit.fallthrough_reconcile_patch_offset = rest.reconcile_patch;
    block->exit.fallthrough_direct_offset = rest.direct;
}

/* 1 for each value of eflags' second byte whose DF bit (bit 10) is set. */
static const uint8_t df_set[256] = {
#define DF4(n) (((n) >> 2) & 1), ((((n) + 1) >> 2) & 1), ((((n) + 2) >> 2) & 1), ((((n) + 3) >> 2) & 1)
#define DF16(n) DF4(n), DF4((n) + 4), DF4((n) + 8), DF4((n) + 12)
#define DF64(n) DF16(n), DF16((n) + 16), DF16((n) + 32), DF16((n) + 48)
    DF64(0), DF64(64), DF64(128), DF64(192)
#undef DF64
#undef DF16
#undef DF4
};

/* A string instruction as the host's: rdi holds the state and edi is r13,
 * so the two swap around it, and a 0x67 prefix makes it use esi, edi and
 * ecx, the guest's own. Re-encoded code never changes DF, so the guest's is
 * PwX86State.eflags': std before it when set (found without flags through
 * df_set and jrcxz, the count kept in r9), cld after it, since the host
 * runs with DF clear. A fault outside the guest range inside it is not
 * redirected (rdi is swapped); one Wine resolves restarts it as usual. */
static void emit_string(Ctx *c, const Inst *in)
{
    Out *o = &c->o;
    size_t to_forward;

    mov_r9_rcx(o);
    b(o, 0x49); b(o, 0xbb); w64(o, (uint64_t)(uintptr_t)df_set);   /* movabs r11, df_set */
    b(o, 0x0f); b(o, 0xb6); b(o, 0x8f); w32(o, (uint32_t)offsetof(PwX86State, eflags) + 1);  /* movzx ecx, byte [rdi+eflags+1] */
    b(o, 0x41); b(o, 0x0f); b(o, 0xb6); b(o, 0x0c); b(o, 0x0b);    /* movzx ecx, byte [r11+rcx] */
    to_forward = jump8(o, 0xe3);                                    /* jrcxz forward */
    b(o, 0xfd);                                                     /* std */
    land8(o, to_forward);
    mov_rcx_r9(o);
    b(o, 0x4c); b(o, 0x87); b(o, 0xef);                             /* xchg rdi, r13 */
    if (in->rep) b(o, in->rep);
    if (in->opsize16) b(o, 0x66);
    b(o, 0x67);
    b(o, in->op[0]);
    b(o, 0x4c); b(o, 0x87); b(o, 0xef);                             /* xchg rdi, r13 */
    b(o, 0xfc);                                                     /* cld */
}

/* The side exits: each loads its target into r10d and the address of its
 * jcc's rel32 into r8, then all share one lookup of r10d in the chain
 * table. A hit rewrites that rel32 to the target's chain entry (r11 - (r8 +
 * 4), without flags: lea, not, lea) and goes there, so the jcc links itself
 * the first time; a miss returns to the dispatcher at the target. */
static void emit_side_exits(Ctx *c)
{
    Out *o = &c->o;
    size_t to_common[MAX_INSTS], to_hit, to_miss;
    uint64_t base = (uint64_t)(uintptr_t)c->chain_table;

    if (!c->side_count) return;
    for (unsigned k = 0; k < c->side_count; k++) {
        land32(o, c->side_rel[k]);
        b(o, 0x41); b(o, 0xba); w32(o, c->side_target[k]);         /* mov r10d, target */
        b(o, 0x4c); b(o, 0x8d); b(o, 0x05);                         /* lea r8, [rip+rel] */
        w32(o, (uint32_t)(c->side_rel[k] - (o->n + 4)));
        to_common[k] = jump32(o);
    }
    for (unsigned k = 0; k < c->side_count; k++) land32(o, to_common[k]);
    emit_chain_slot(o);
    b(o, 0x4e); b(o, 0x8d); b(o, 0x1c); b(o, 0xdd); w32(o, 0);      /* lea r11, [r11*8] */
    b(o, 0x4f); b(o, 0x8d); b(o, 0x1c); b(o, 0x1b);                 /* lea r11, [r11+r11] */
    b(o, 0x49); b(o, 0xb9); w64(o, base);                           /* movabs r9, table */
    b(o, 0x4f); b(o, 0x8d); b(o, 0x1c); b(o, 0x19);                 /* lea r11, [r9+r11] */
    mov_r9_rcx(o);
    b(o, 0x41); b(o, 0x8b); b(o, 0x0b);                             /* mov ecx, [r11] */
    b(o, 0xf7); b(o, 0xd1);                                         /* not ecx */
    b(o, 0x42); b(o, 0x8d); b(o, 0x4c); b(o, 0x11); b(o, 1);        /* lea ecx, [rcx+r10+1] */
    to_hit = jump8(o, 0xe3);                                        /* jrcxz hit */
    mov_rcx_r9(o);
    to_miss = jump8(o, 0xeb);
    land8(o, to_hit);
    mov_rcx_r9(o);
    b(o, 0x4d); b(o, 0x8b); b(o, 0x5b); b(o, (uint8_t)offsetof(PwX86IndirectTarget, host_code));
    b(o, 0x4d); b(o, 0x8d); b(o, 0x48); b(o, 4);                    /* lea r9, [r8+4] */
    b(o, 0x49); b(o, 0xf7); b(o, 0xd1);                             /* not r9 */
    b(o, 0x4f); b(o, 0x8d); b(o, 0x4c); b(o, 0x0b); b(o, 1);        /* lea r9, [r11+r9+1] */
    b(o, 0x45); b(o, 0x89); b(o, 0x08);                             /* mov [r8], r9d */
    b(o, 0x41); b(o, 0xff); b(o, 0xe3);                             /* jmp r11 */
    land8(o, to_miss);
    store_state(o, R10, offsetof(PwX86State, eip));
    emit_leave(o, c->call_stack);
    b(o, 0x31); b(o, 0xc0); b(o, 0xc3);                             /* xor eax, eax; ret */
}

static void emit_cold_paths(Ctx *c, PwX86Block *block)
{
    Out *o = &c->o;
    size_t starts[MAX_COLD];

    for (unsigned k = 0; k < c->cold_count; k++) {
        const Cold *cold = &c->cold[k];
        starts[k] = o->n;
        if (cold->patch) put32(o, cold->patch, (uint32_t)(o->n - (cold->patch + 4)));
        /* First, so a fault handler can read it (pw_x86_cold_path_eip). */
        store_state_imm(o, offsetof(PwX86State, eip), cold->pc);
        if (cold->saved) restore_flags(o);
        if (cold->direct) ea_lea(o, R11, &cold->ea);
        store_state(o, R11, offsetof(PwX86State, fault_address));
        store_state_imm(o, offsetof(PwX86State, fault_width), cold->width);
        store_state_imm(o, offsetof(PwX86State, fault_write), cold->write);
        emit_leave(o, c->call_stack);
        b(o, 0xb8); w32(o, 0xffffffffu); b(o, 0xc3);                /* mov eax, -1; ret */
    }
    if (!c->fault_markers) return;
    /* The fault table (pw_x86_block.h): each access that may fault and the
     * path that reports it. */
    block->fault_table_offset = o->n;
    w16(o, c->cold_count);
    for (unsigned k = 0; k < c->cold_count; k++) {
        w16(o, (uint32_t)c->cold[k].site);
        w16(o, (uint32_t)starts[k]);
    }
}

/* Whether an instruction needs its EIP stored: it can stop the block. */
static int can_fault(const Inst *in)
{
    switch (in->kind) {
    case K_RM: return in->mod != 3;
    case K_PUSH: case K_PUSHIMM: case K_PUSHRM: case K_POP: case K_LEAVE:
    case K_CALL: case K_CALLRM: case K_RET: return 1;
    case K_JMPRM: return in->mod != 3;
    default: return 0;
    }
}

int pw_x86_reencode(const uint8_t *source, size_t bytes, uint32_t pc,
                    uint8_t *output, size_t capacity, PwX86Block *block,
                    const PwX86TranslateOptions *options)
{
    static const Kind terminals[] = { K_JMP, K_JCC, K_RET, K_JMPRM, K_CALL, K_CALLRM };
    Inst insts[MAX_INSTS];
    uint32_t live[MAX_INSTS];
    unsigned count = 0, superblocks;
    size_t cursor = 0;
    Ctx c;

    if (!source || !bytes || !output || !capacity || !block || !options)
        return PW_ERR_PRECONDITION;
    if (!(options->flat_high > options->flat_low && options->flat_high - options->flat_low >= 16) ||
        !options->no_counters)
        return PW_ERR_UNSUPPORTED;
    memset(block, 0, sizeof(*block));
    superblocks = options->superblocks && options->unbounded_chains && options->indirect_targets &&
                  options->chain_targets;
    while (count < MAX_INSTS && cursor < bytes) {
        Inst *in = &insts[count];
        int terminal = 0;
        if (!decode(source + cursor, bytes - cursor, pc + (uint32_t)cursor, in, options->native_fp)) break;
        cursor += in->len;
        count++;
        for (unsigned k = 0; k < sizeof(terminals) / sizeof(terminals[0]); k++)
            if (in->kind == terminals[k]) terminal = 1;
        /* A superblock goes on past a conditional branch: a side exit. */
        if (terminal && in->kind == K_JCC && superblocks && count < MAX_INSTS) terminal = 0;
        if (terminal) break;
    }
    if (!count) return PW_ERR_UNSUPPORTED;
    /* Flags live after each instruction; every flag is live after the block,
     * and before a side exit. */
    live[count - 1] = ALL_FLAGS;
    for (unsigned k = count - 1; k > 0; k--)
        live[k - 1] = insts[k].kind == K_JCC ? ALL_FLAGS : insts[k].use | (live[k] & ~insts[k].def);

    memset(&c, 0, sizeof(c));
    c.o.p = output; c.o.cap = capacity;
    c.flat_low = options->flat_low;
    c.flat_span = options->flat_high - options->flat_low;
    c.fault_markers = options->fault_markers;
    /* With native FP the indirect table leads only to emitted blocks, which
     * re-encoded code reaches through C. */
    c.table = options->native_fp ? NULL : options->indirect_targets;
    c.chain_table = options->indirect_targets ? options->chain_targets : NULL;
    c.mask = options->indirect_mask;
    c.block_pc = pc;
    c.bounded = !options->unbounded_chains;
    c.call_stack = options->unbounded_chains && options->call_stack && c.chain_table;

    block->entry_contract.resident_mask = 0xff;
    for (unsigned g = 0; g < 8; g++)
        block->entry_contract.guest_to_host[g] = (int8_t)(PW_X86_REENCODE_HOST_BASE + host_of[g]);
    for (unsigned h = 0; h < PW_X86_MAX_HOST_REGS; h++) block->entry_contract.host_to_guest[h] = -1;
    block->exit_contract = block->entry_contract;

    block->canonical_entry_offset = 0;
    emit_enter(&c.o, c.call_stack);
    block->chain_entry_offset = c.o.n;

    unsigned fp_prepared = 0;
    cursor = 0;
    for (unsigned k = 0; k < count; k++) {
        const Inst *in = &insts[k];
        const uint32_t here = pc + (uint32_t)cursor, next = here + in->len;
        /* The guard compares; keep the flags when anything later reads them. */
        const unsigned keep = (in->use | (live[k] & ~in->def)) != 0;
        Out *o = &c.o;

        /* A marked access's refused-access path stores its own EIP, and a
         * fault handler can read it from there; only the guard stores it
         * before the access. */
        c.here = here;
        if (k == count - 1) block->exit_offset = o->n;
        if (options->lazy_native_fp && in->native_fp_touch && !fp_prepared) {
            emit_fp_prepare(o);
            fp_prepared = 1;
        }
        if (!c.fault_markers && can_fault(in)) store_state_imm(o, offsetof(PwX86State, eip), here);
        switch (in->kind) {
        case K_RM:
            if (in->mod != 3 && c.fault_markers && !in->fs) { emit_rm_direct(&c, in); break; }
            if (in->mod != 3) guard_fs(&c, &in->ea, in->fs, in->width, in->write, keep);
            emit_rm(&c, in);
            break;
        case K_PLAIN:
            for (unsigned j = 0; j < in->len; j++) b(o, in->bytes[j]);
            break;
        case K_INCDEC: {
            unsigned h = host_of[in->reg];
            if (in->opsize16) b(o, 0x66);
            if (h >= 8) b(o, 0x41);
            b(o, 0xff); b(o, (uint8_t)(0xc0 | (in->bytes[in->opsize16] >= 0x48 ? 8 : 0) | (h & 7)));
            break;
        }
        case K_MOVIMM: {
            unsigned h = host_of[in->reg];
            if (in->opsize16) b(o, 0x66);
            if (h >= 8) b(o, 0x41);
            b(o, (uint8_t)(0xb8 | (h & 7)));
            for (unsigned j = 0; j < in->imm_len; j++) b(o, in->imm[j]);
            break;
        }
        case K_XCHGA: {
            unsigned h = host_of[in->reg];
            if (in->opsize16) b(o, 0x66);
            if (h >= 8) b(o, 0x41);
            b(o, (uint8_t)(0x90 | (h & 7)));
            break;
        }
        case K_BSWAP: {
            unsigned h = host_of[in->reg];
            if (h >= 8) b(o, 0x41);
            b(o, 0x0f); b(o, (uint8_t)(0xc8 | (h & 7)));
            break;
        }
        case K_NOP:
            break;
        case K_STR:
            emit_string(&c, in);
            break;
        case K_LEA:
            ea_lea(o, host_of[in->reg], &in->ea);
            break;
        case K_PUSH:
            emit_push(&c, host_of[in->reg], 0, keep);
            break;
        case K_PUSHIMM:
            emit_push(&c, -1, in->imm_value, keep);
            break;
        case K_PUSHRM:
            operand_r10(&c, in, keep);
            emit_push(&c, R10, 0, keep);
            break;
        case K_POP: {
            const Ea top = { 4, -1, 0, 0 };
            unsigned h = host_of[in->reg];
            if (c.fault_markers) {
                /* pop esp: esp is the value read. */
                emit_frame_access(&c, 0x8b, h, 4, 0, 0, 0);
                if (h != R12) lea_r12_r12(o, 4);
                break;
            }
            guard(&c, &top, 4, 0, keep);
            load_r10(o);
            lea_r12_r12(o, 4);
            rr(o, 0x89, 0, h, R10);                                     /* mov reg, r10d */
            break;
        }
        case K_LEAVE: {
            const Ea frame = { 5, -1, 0, 0 };
            if (c.fault_markers) emit_frame_access(&c, 0x8b, R10, 5, 0, 0, 0);
            else { guard(&c, &frame, 4, 0, keep); load_r10(o); }
            if (c.fault_markers) {
                b(o, 0x44); b(o, 0x8d); b(o, 0x65); b(o, 4);            /* lea r12d, [rbp+4] */
                rr(o, 0x89, 0, 5, R10);                                 /* mov ebp, r10d */
                break;
            }
            b(o, 0x45); b(o, 0x8d); b(o, 0x63); b(o, 4);                /* lea r12d, [r11+4] */
            rr(o, 0x89, 0, 5, R10);                                     /* mov ebp, r10d */
            break;
        }
        case K_CALL: {
            ExitSlots slots;
            if (c.call_stack) { emit_call(&c, block, next, in->target, 0, keep); break; }
            emit_push(&c, -1, next, keep);
            emit_chain_exit(&c, in->target, &slots, 0);
            block->exit.kind = PW_X86_EXIT_DIRECT_JUMP;
            block->exit.chainable = 1;
            block->exit.target_pc = in->target;
            block->exit.target_patch_offset = slots.patch;
            block->exit.target_stub_offset = slots.stub;
            block->exit.target_reconcile_offset = slots.reconcile;
            block->exit.target_reconcile_patch_offset = slots.reconcile_patch;
            block->exit.target_direct_offset = slots.direct;
            break;
        }
        case K_CALLRM:
            operand_r10(&c, in, keep);
            if (c.call_stack) { emit_call(&c, block, next, 0, 1, keep); break; }
            emit_push(&c, -1, next, keep);
            emit_dynamic_exit(&c);
            block->exit.kind = PW_X86_EXIT_DYNAMIC;
            break;
        case K_RET: {
            const Ea top = { 4, -1, 0, 0 };
            if (c.fault_markers) emit_frame_access(&c, 0x8b, R10, 4, 0, 0, 0);
            else { guard(&c, &top, 4, 0, keep); load_r10(o); }
            b(o, 0x45); b(o, 0x8d); b(o, 0xa4); b(o, 0x24); w32(o, 4 + in->imm_value); /* lea r12d, [r12+n] */
            if (c.call_stack) b(o, 0xc3);                               /* ret: to the call's landing */
            else emit_dynamic_exit(&c);
            block->exit.kind = PW_X86_EXIT_DYNAMIC;
            break;
        }
        case K_JMPRM:
            operand_r10(&c, in, keep);
            emit_dynamic_exit(&c);
            block->exit.kind = PW_X86_EXIT_DYNAMIC;
            break;
        case K_JMP: {
            ExitSlots slots;
            emit_chain_exit(&c, in->target, &slots, 0);
            block->exit.kind = PW_X86_EXIT_DIRECT_JUMP;
            block->exit.chainable = 1;
            block->exit.target_pc = in->target;
            block->exit.target_patch_offset = slots.patch;
            block->exit.target_stub_offset = slots.stub;
            block->exit.target_reconcile_offset = slots.reconcile;
            block->exit.target_reconcile_patch_offset = slots.reconcile_patch;
            block->exit.target_direct_offset = slots.direct;
            break;
        }
        case K_JCC: {
            ExitSlots taken, fall;
            size_t to_taken;
            if (k < count - 1) {
                /* A side exit (superblocks): the block goes on. */
                b(o, 0x0f); b(o, (uint8_t)(0x80 | in->cond));
                c.side_rel[c.side_count] = o->n; w32(o, 0);
                c.side_target[c.side_count++] = in->target;
                break;
            }
            b(o, 0x0f); b(o, (uint8_t)(0x80 | in->cond)); to_taken = o->n; w32(o, 0);
            emit_chain_exit(&c, next, &fall, 0);
            emit_chain_exit(&c, in->target, &taken, to_taken);
            block->exit.kind = PW_X86_EXIT_CONDITIONAL;
            block->exit.chainable = 1;
            block->exit.target_pc = in->target;
            block->exit.fallthrough_pc = next;
            block->exit.target_patch_offset = taken.patch;
            block->exit.target_stub_offset = taken.stub;
            block->exit.target_reconcile_offset = taken.reconcile;
            block->exit.target_reconcile_patch_offset = taken.reconcile_patch;
            block->exit.target_direct_offset = taken.direct;
            block->exit.fallthrough_patch_offset = fall.patch;
            block->exit.fallthrough_stub_offset = fall.stub;
            block->exit.fallthrough_reconcile_offset = fall.reconcile;
            block->exit.fallthrough_reconcile_patch_offset = fall.reconcile_patch;
            block->exit.fallthrough_direct_offset = fall.direct;
            break;
        }
        }
        cursor += in->len;
        block->instruction_ends[k] = (uint16_t)cursor;
    }
    if (!block->exit.kind) {
        /* Ended before an instruction this backend does not take, or at the
         * length limit: continue at the next one, linked like a jump. */
        ExitSlots slots;
        block->exit_offset = c.o.n;
        const uint32_t next = pc + (uint32_t)cursor;
        emit_chain_exit(&c, next, &slots, 0);
        block->exit.kind = PW_X86_EXIT_DIRECT_JUMP;
        block->exit.chainable = 1;
        block->exit.target_pc = next;
        block->exit.target_patch_offset = slots.patch;
        block->exit.target_stub_offset = slots.stub;
        block->exit.target_reconcile_offset = slots.reconcile;
        block->exit.target_reconcile_patch_offset = slots.reconcile_patch;
        block->exit.target_direct_offset = slots.direct;
    }
    emit_side_exits(&c);
    emit_cold_paths(&c, block);
    if (c.o.failed) return PW_ERR_LIMIT;
    block->instructions = count;
    block->source_bytes = cursor;
    block->code_bytes = c.o.n;
    return PW_OK;
}

size_t pw_x86_reencode_return_stub(uint8_t *output, size_t capacity, const PwX86TranslateOptions *options)
{
    Ctx c;

    if (!output || !options || !options->unbounded_chains || !options->call_stack ||
        !options->indirect_targets || !options->chain_targets)
        return 0;
    memset(&c, 0, sizeof(c));
    c.o.p = output; c.o.cap = capacity;
    c.table = options->native_fp ? NULL : options->indirect_targets;
    c.chain_table = options->chain_targets;
    c.mask = options->indirect_mask;
    c.call_stack = 1;
    /* A ret past everything pushed: start the call stack again, then look
     * the guest's return address (r10d) up. */
    rsp_state(&c.o, 0x8b, offsetof(PwX86State, call_stack_top));
    emit_dynamic_exit(&c);
    return c.o.failed ? 0 : c.o.n;
}
