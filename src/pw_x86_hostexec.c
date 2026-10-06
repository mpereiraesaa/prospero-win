/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_x86_hostexec.h"
#include <stddef.h>
#include <string.h>

/* Operand/immediate classes of the primary map. */
enum { N = 0, M = 1, X = 2 };               /* no ModRM, ModRM, refused */
enum { I0 = 0, IB = 1, IW = 2, IZ = 3, IO = 4 }; /* none, 8, 16, 16/32, moffs32 */

/* Primary-map ModRM presence (M), refusal (X) and immediate class. Refused:
 * control transfers, implicit stack users, forms invalid in 64-bit mode
 * (BCD, INC/DEC 40-4F, PUSHA/POPA, BOUND, ARPL, LES/LDS, INTO, 82), segment
 * register loads (stores, 8C, are planned apart), I/O, HLT, CLI/STI, INT, and
 * LEA (whose operand is not a memory access and must not receive the FS
 * base). */
static const uint8_t primary_class[256] = {
/*        0 1 2 3 4 5 6 7 8 9 a b c d e f */
/* 0 */   M,M,M,M,N,N,X,X,M,M,M,M,N,N,X,N,
/* 1 */   M,M,M,M,N,N,X,X,M,M,M,M,N,N,X,X,
/* 2 */   M,M,M,M,N,N,N,X,M,M,M,M,N,N,N,X,
/* 3 */   M,M,M,M,N,N,N,X,M,M,M,M,N,N,N,X,
/* 4 */   X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
/* 5 */   X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
/* 6 */   X,X,X,X,N,N,N,N,X,M,X,M,X,X,X,X,
/* 7 */   X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
/* 8 */   M,M,X,M,M,M,M,M,M,M,M,M,X,X,X,X,
/* 9 */   N,N,N,N,X,N,N,N,N,N,X,N,X,X,N,N,
/* a */   N,N,N,N,N,N,N,N,N,N,N,N,N,N,N,N,
/* b */   N,N,N,N,N,N,N,N,N,N,N,N,N,N,N,N,
/* c */   M,M,X,X,X,X,M,M,X,X,X,X,X,X,X,X,
/* d */   M,M,M,M,X,X,X,N,M,M,M,M,M,M,M,M,
/* e */   X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
/* f */   N,X,N,N,X,N,M,M,N,N,X,X,N,N,M,M,
};
/* 8D (LEA), 8F (POP m) and C6/C7 are refined below; F0/F2/F3 are prefixes
 * and never reach this table as opcodes. */

static unsigned primary_imm(uint8_t op)
{
    if (op < 0x40 && (op & 7) == 4) return IB;
    if (op < 0x40 && (op & 7) == 5) return IZ;
    switch (op) {
    case 0x69: case 0x81: case 0xa9: case 0xc7: return IZ;
    case 0x6b: case 0x80: case 0x83: case 0xa8: case 0xc0: case 0xc1:
    case 0xc6: return IB;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: return IO;
    }
    if (op >= 0xb0 && op <= 0xb7) return IB;
    if (op >= 0xb8 && op <= 0xbf) return IZ;
    return I0;
}

/* Two-byte map. ModRM is present unless listed; refused forms are system,
 * control-transfer, stack and 3DNow! forms. */
static int secondary_class(uint8_t op, uint8_t modrm, int have_modrm)
{
    if (op >= 0x80 && op <= 0x8f) return X;                 /* Jcc rel32 */
    switch (op) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x07: case 0x08: case 0x09: case 0x0b: case 0x0e: case 0x0f:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x30: case 0x32:
    case 0x33: case 0x34: case 0x35: case 0x37: case 0xa0: case 0xa1:
    case 0xa8: case 0xa9: case 0xaa: case 0xff: case 0xb9: case 0x0d:
        return X;
    case 0x01:
        /* Only XGETBV and RDTSCP are unprivileged and stateless here. */
        return have_modrm && (modrm == 0xd0 || modrm == 0xf9) ? N : X;
    case 0x31: case 0x77: return N;                          /* RDTSC EMMS */
    case 0x38: case 0x3a: return M;                          /* checked by caller */
    }
    if (op >= 0xc8 && op <= 0xcf) return X; /* BSWAP r: register in opcode, use translator */
    return M;
}

static unsigned secondary_imm(uint8_t op)
{
    switch (op) {
    case 0x70: case 0x71: case 0x72: case 0x73: case 0xa4: case 0xac:
    case 0xba: case 0xc2: case 0xc4: case 0xc5: case 0xc6: case 0x3a:
        return IB;
    }
    return I0;
}

/* Which ModRM fields name general registers (so that 4 means ESP). */
enum { F_REG = 1, F_RM = 2 };
static unsigned secondary_gpr_fields(uint8_t op, uint8_t rep)
{
    if ((op >= 0x40 && op <= 0x4f) || op == 0x02 || op == 0x03 ||
        op == 0xa3 || op == 0xa4 || op == 0xa5 || op == 0xab || op == 0xac ||
        op == 0xad || op == 0xaf || op == 0xb0 || op == 0xb1 || op == 0xb3 ||
        (op >= 0xb6 && op <= 0xb8) || (op >= 0xbb && op <= 0xbf) ||
        op == 0xc0 || op == 0xc1)
        return F_REG | F_RM;
    if ((op >= 0x90 && op <= 0x9f) || op == 0xba || op == 0xc4 || op == 0x6e ||
        op == 0xc7 || (op == 0x7e && rep != 0xf3) || (op == 0x2a && rep))
        return F_RM;
    if (op == 0x50 || op == 0xd7 || op == 0xc5 || op == 0xc3 ||
        ((op == 0x2c || op == 0x2d) && rep))
        return F_REG;
    return 0;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Decode a 32-bit ModRM/SIB memory operand; returns its byte length. */
static int modrm_address(const PwX86State *s, const uint8_t *p, size_t n,
                         uint32_t *address)
{
    const uint8_t modrm = p[0], mod = modrm >> 6, rm = modrm & 7;
    size_t len = 1;
    uint32_t ea = 0;

    if (mod == 3) return PW_ERR_MALFORMED;
    if (rm == 4) {
        uint8_t sib, scale, index, base;
        if (n < 2) return PW_ERR_TRUNCATED;
        sib = p[1]; scale = sib >> 6; index = (sib >> 3) & 7; base = sib & 7;
        len = 2;
        if (base == 5 && mod == 0) {
            if (n < 6) return PW_ERR_TRUNCATED;
            ea = rd32(p + 2); len = 6;
        } else ea = s->gpr[base];
        if (index != 4) ea += s->gpr[index] << scale;
    } else if (rm == 5 && mod == 0) {
        if (n < 5) return PW_ERR_TRUNCATED;
        return *address = rd32(p + 1), 5;
    } else ea = s->gpr[rm];
    if (mod == 1) {
        if (n < len + 1) return PW_ERR_TRUNCATED;
        ea += (uint32_t)(int32_t)(int8_t)p[len]; len += 1;
    } else if (mod == 2) {
        if (n < len + 4) return PW_ERR_TRUNCATED;
        ea += rd32(p + len); len += 4;
    }
    *address = ea;
    return (int)len;
}

/* The selectors a WoW64 guest sees, as on Windows: CS 0x23, FS 0x53 (the
 * TEB), and 0x2b for the flat data segments. ES CS SS DS FS GS by ModRM.reg;
 * 6 and 7 are not segment registers. */
static const uint16_t guest_selector[6] = { 0x2b, 0x23, 0x2b, 0x2b, 0x53, 0x2b };

/* The selector the guest sees in a segment register: the host Wine's (the
 * thread context's), or Windows' when the state has none. */
static uint16_t selector_of(const PwX86State *s, unsigned reg)
{
    return s->selector[reg] ? s->selector[reg] : guest_selector[reg];
}

/* MOV r/m16, Sreg (8C /r): the selector is a constant here, so the host runs
 * "mov word [address], selector" or, for a register, "mov r32, selector"
 * (32-bit forms zero-extend it, as every processor since the Pentium Pro
 * does). i386 ntdll's context capture stores all six this way. */
static int segment_store(const PwX86State *s, const uint8_t *src, size_t n, size_t at,
                         unsigned fs, unsigned opsize, PwX86HostExecPlan *plan)
{
    uint8_t modrm, reg;
    uint16_t selector;
    int addr_len;

    if (at + 1 >= n) return PW_ERR_TRUNCATED;
    modrm = src[at + 1];
    reg = (modrm >> 3) & 7;
    if (reg > 5) return PW_ERR_UNSUPPORTED;
    selector = selector_of(s, reg);
    plan->out_bytes = 0;
    if ((modrm >> 6) == 3) {
        if ((modrm & 7) == 4) return PW_ERR_UNSUPPORTED;       /* ESP */
        if (opsize) plan->out[plan->out_bytes++] = 0x66;
        plan->out[plan->out_bytes++] = (uint8_t)(0xb8 | (modrm & 7));
        plan->out[plan->out_bytes++] = (uint8_t)selector;
        plan->out[plan->out_bytes++] = (uint8_t)(selector >> 8);
        if (!opsize) { plan->out[plan->out_bytes++] = 0; plan->out[plan->out_bytes++] = 0; }
        plan->length = (uint8_t)(at + 2);
        return PW_OK;
    }
    addr_len = modrm_address(s, src + at + 1, n - at - 1, &plan->address);
    if (addr_len < 0) return addr_len;
    if (fs) plan->address += s->fs_base;
    plan->mem_form = 1;
    plan->address_reg = 6;
    plan->out[plan->out_bytes++] = 0x67;
    plan->out[plan->out_bytes++] = 0x66;
    plan->out[plan->out_bytes++] = 0xc7;
    plan->out[plan->out_bytes++] = 0x06;                       /* [esi] */
    plan->out[plan->out_bytes++] = (uint8_t)selector;
    plan->out[plan->out_bytes++] = (uint8_t)(selector >> 8);
    if (at + 1 + (size_t)addr_len > 15) return PW_ERR_UNSUPPORTED;
    plan->length = (uint8_t)(at + 1 + (size_t)addr_len);
    return PW_OK;
}

int pw_x86_hostexec_plan(const PwX86State *s, const uint8_t *src, size_t n,
                         PwX86HostExecPlan *plan)
{
    uint8_t prefixes[4], rep = 0;
    size_t count = 0, at = 0, opcode_at, op_bytes = 1;
    unsigned opsize = 0, fs = 0, klass, imm, gpr_fields = 0;
    int have_modrm = 0;
    uint8_t op;

    if (!s || !src || !plan) return PW_ERR_PRECONDITION;
    memset(plan, 0, sizeof(*plan));
    for (;; ++at) {
        if (at >= n || at >= 4) return at >= n ? PW_ERR_TRUNCATED : PW_ERR_UNSUPPORTED;
        switch (src[at]) {
        case 0x66: opsize = 1; prefixes[count++] = 0x66; continue;
        case 0xf2: case 0xf3: rep = src[at]; prefixes[count++] = src[at]; continue;
        case 0xf0: prefixes[count++] = 0xf0; continue;
        case 0x26: case 0x2e: case 0x36: case 0x3e: continue;   /* flat */
        case 0x64: fs = 1; continue;
        case 0x65: case 0x67: return PW_ERR_UNSUPPORTED;
        }
        break;
    }
    opcode_at = at;
    op = src[at];
    if (op == 0x0f) {
        if (at + 1 >= n) return PW_ERR_TRUNCATED;
        op = src[at + 1];
        op_bytes = 2;
        if (op == 0x38 || op == 0x3a) {
            if (at + 2 >= n) return PW_ERR_TRUNCATED;
            /* SSSE3/SSE4 vector forms only; GPR forms (MOVBE, CRC32, PEXTR*,
             * PINSR*) and AES/SHA need their own review. */
            {
                const uint8_t sub = src[at + 2];
                if (op == 0x38 ? !(sub <= 0x0b || (sub >= 0x10 && sub <= 0x41 && sub != 0x1f))
                               : !(sub == 0x0f || (sub >= 0x08 && sub <= 0x0e) ||
                                   sub == 0x40 || sub == 0x41 || sub == 0x42))
                    return PW_ERR_UNSUPPORTED;
            }
            op_bytes = 3;
            klass = M;
            imm = op == 0x3a ? IB : I0;
        } else {
            have_modrm = at + 2 < n;
            klass = secondary_class(op, have_modrm ? src[at + 2] : 0, have_modrm);
            imm = secondary_imm(op);
            gpr_fields = secondary_gpr_fields(op, rep);
            if (op == 0x01) op_bytes = 3; /* XGETBV/RDTSCP carry their ModRM as opcode */
        }
    } else {
        klass = primary_class[op];
        imm = primary_imm(op);
        if (op == 0x8d || op == 0x8f) klass = X;
        if (op >= 0xd8 && op <= 0xdf) gpr_fields = 0;
        else if (op == 0x80 || op == 0x81 || op == 0x83 || op == 0xc0 || op == 0xc1 ||
                 (op >= 0xd0 && op <= 0xd3) || op == 0xf6 || op == 0xf7 ||
                 op == 0xfe || op == 0xff || op == 0xc6 || op == 0xc7)
            gpr_fields = F_RM;
        else gpr_fields = F_REG | F_RM;
    }
    if (op_bytes == 1 && op == 0x8c) return segment_store(s, src, n, at, fs, opsize, plan);
    if (klass == X) return PW_ERR_UNSUPPORTED;
    at += op_bytes;
    if (op_bytes == 3 && src[opcode_at + 1] == 0x01) klass = N;

    plan->out_bytes = 0;
    plan->out[plan->out_bytes++] = 0x67;
    memcpy(plan->out + plan->out_bytes, prefixes, count);
    plan->out_bytes += (uint8_t)count;

    if (klass == M) {
        uint8_t modrm, reg, x;
        int addr_len = 0;

        if (at >= n) return PW_ERR_TRUNCATED;
        modrm = src[at];
        reg = (modrm >> 3) & 7;
        if (op_bytes == 1) {
            /* Group forms whose ModRM digit selects a stack or control op. */
            if ((op == 0xff || op == 0xfe) && reg > 1) return PW_ERR_UNSUPPORTED;
            if ((op == 0xc6 || op == 0xc7) && reg != 0) return PW_ERR_UNSUPPORTED;
            if ((op == 0xf6 || op == 0xf7) && reg <= 1) imm = op == 0xf6 ? IB : IZ;
        }
        if ((gpr_fields & F_REG) && reg == 4) return PW_ERR_UNSUPPORTED;
        if ((modrm >> 6) == 3) {
            if ((gpr_fields & F_RM) && (modrm & 7) == 4) return PW_ERR_UNSUPPORTED;
            memcpy(plan->out + plan->out_bytes, src + opcode_at, (at - opcode_at) + 1);
            plan->out_bytes += (uint8_t)((at - opcode_at) + 1);
            at += 1;
        } else {
            addr_len = modrm_address(s, src + at, n - at, &plan->address);
            if (addr_len < 0) return addr_len;
            if (fs) plan->address += s->fs_base;
            /* ESI carries the address unless the instruction names ESI. */
            x = ((gpr_fields & F_REG) && reg == 6) ? 7 : 6;
            plan->mem_form = 1;
            plan->address_reg = x;
            memcpy(plan->out + plan->out_bytes, src + opcode_at, at - opcode_at);
            plan->out_bytes += (uint8_t)(at - opcode_at);
            plan->out[plan->out_bytes++] = (uint8_t)((reg << 3) | x);
            at += (size_t)addr_len;
        }
    } else {
        if (imm == IO && fs) {
            /* moffs with an FS override: use the equivalent MOV r/m form. */
            static const uint8_t modrm_op[4] = { 0x8a, 0x8b, 0x88, 0x89 };
            if (at + 4 > n) return PW_ERR_TRUNCATED;
            plan->address = rd32(src + at) + s->fs_base;
            plan->mem_form = 1;
            plan->address_reg = 6;
            plan->out[plan->out_bytes++] = modrm_op[op - 0xa0];
            plan->out[plan->out_bytes++] = 0x06;          /* [esi] */
            plan->length = (uint8_t)(at + 4);
            return PW_OK;
        }
        if (fs && op_bytes == 1 && ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf) ||
                                    op == 0xd7))
            return PW_ERR_UNSUPPORTED;            /* FS-relative string or XLAT */
        memcpy(plan->out + plan->out_bytes, src + opcode_at, at - opcode_at);
        plan->out_bytes += (uint8_t)(at - opcode_at);
    }
    {
        size_t imm_bytes = imm == IB ? 1 : imm == IW ? 2 :
                           imm == IZ ? (opsize ? 2 : 4) : imm == IO ? 4 : 0;
        if (at + imm_bytes > n) return PW_ERR_TRUNCATED;
        if (plan->out_bytes + imm_bytes > sizeof(plan->out)) return PW_ERR_UNSUPPORTED;
        memcpy(plan->out + plan->out_bytes, src + at, imm_bytes);
        plan->out_bytes += (uint8_t)imm_bytes;
        at += imm_bytes;
    }
    if (at > 15) return PW_ERR_UNSUPPORTED;
    plan->length = (uint8_t)at;
    return PW_OK;
}

/* ---- stub emission ---------------------------------------------------- */

enum { FP_XMM = 112, FP_MXCSR = 240, FP_FLAGS = 244, FP_BYTES = 256 };

typedef struct Buf { uint8_t *p; size_t n, cap; int bad; } Buf;
static void put(Buf *b, const void *bytes, size_t len)
{
    if (b->n + len > b->cap) { b->bad = 1; return; }
    memcpy(b->p + b->n, bytes, len); b->n += len;
}
static void put1(Buf *b, uint8_t v) { put(b, &v, 1); }
static void put32(Buf *b, uint32_t v) { put(b, &v, 4); }
/* mov e<reg>, [r11 + disp32] / mov [r11 + disp32], e<reg> */
static void gpr_io(Buf *b, unsigned store, unsigned reg, uint32_t disp)
{
    put1(b, 0x41); put1(b, store ? 0x89 : 0x8b); put1(b, (uint8_t)(0x83 | (reg << 3)));
    put32(b, disp);
}
static void xmm_io(Buf *b, unsigned store, unsigned reg)
{
    put1(b, 0xf3); put1(b, 0x41); put1(b, 0x0f); put1(b, store ? 0x7f : 0x6f);
    put1(b, (uint8_t)(0x81 | (reg << 3))); put32(b, FP_XMM + reg * 16u);
}

static int emit_stub(const PwX86HostExecPlan *plan, uint8_t *code, size_t cap, size_t *bytes)
{
    static const uint8_t prologue[] = {
        0x53,                         /* push rbx */
        0x55,                         /* push rbp */
        0x48, 0x83, 0xec, 0x18,       /* sub rsp, 24 */
        0xd9, 0x3c, 0x24,             /* fnstcw [rsp] */
        0x0f, 0xae, 0x5c, 0x24, 0x04, /* stmxcsr [rsp+4] */
        0x49, 0x89, 0xfb,             /* mov r11, rdi  (state) */
        0x41, 0x89, 0xf2,             /* mov r10d, esi (address) */
        0x49, 0x89, 0xd1,             /* mov r9, rdx   (fp image) */
        0x41, 0xdd, 0x21,             /* frstor [r9] */
    };
    static const uint8_t epilogue[] = {
        0xfc,                         /* cld */
        0x41, 0xdd, 0x31,             /* fnsave [r9] */
    };
    static const uint8_t restore[] = {
        0x41, 0x0f, 0xae, 0x99, FP_MXCSR, 0, 0, 0, /* stmxcsr [r9+240] */
        0x0f, 0xae, 0x54, 0x24, 0x04, /* ldmxcsr [rsp+4] */
        0xd9, 0x2c, 0x24,             /* fldcw [rsp] */
        0x48, 0x83, 0xc4, 0x18,       /* add rsp, 24 */
        0x5d, 0x5b, 0xc3,             /* pop rbp; pop rbx; ret */
    };
    static const uint8_t ldmxcsr[] = { 0x41, 0x0f, 0xae, 0x91, FP_MXCSR, 0, 0, 0 };
    Buf b = { code, 0, cap, 0 };
    const uint32_t flags = (uint32_t)offsetof(PwX86State, eflags);

    put(&b, prologue, sizeof(prologue));
    for (unsigned i = 0; i < 8; i++) xmm_io(&b, 0, i);
    put(&b, ldmxcsr, sizeof(ldmxcsr));
    put1(&b, 0x41); put1(&b, 0xff); put1(&b, 0xb3); put32(&b, flags); /* push [r11+eflags] */
    put1(&b, 0x9d);                                                    /* popfq */
    for (unsigned r = 0; r < 8; r++)
        if (r != 4) gpr_io(&b, 0, r, r * 4u);
    if (plan->mem_form) { put1(&b, 0x44); put1(&b, 0x89); put1(&b, (uint8_t)(0xd0 | plan->address_reg)); }
    put(&b, plan->out, plan->out_bytes);
    put1(&b, 0x9c);                                                    /* pushfq */
    for (unsigned r = 0; r < 8; r++)
        if (r != 4 && !(plan->mem_form && r == plan->address_reg)) gpr_io(&b, 1, r, r * 4u);
    put1(&b, 0x58);                                                    /* pop rax */
    put1(&b, 0x41); put1(&b, 0x89); put1(&b, 0x81); put32(&b, FP_FLAGS); /* mov [r9+244], eax */
    put(&b, epilogue, sizeof(epilogue));
    for (unsigned i = 0; i < 8; i++) xmm_io(&b, 1, i);
    put(&b, restore, sizeof(restore));
    if (b.bad) return PW_ERR_LIMIT;
    *bytes = b.n;
    return PW_OK;
}

/* ---- FP image ---------------------------------------------------------- */

static void fp_to_image(const PwGuestFp *fp, uint8_t *image)
{
    const unsigned top = (fp->x87_status >> 11) & 7;
    uint32_t v;

    memset(image, 0, FP_BYTES);
    v = fp->x87_control | 0xffff0000u; memcpy(image + 0, &v, 4);
    v = fp->x87_status | 0xffff0000u;  memcpy(image + 4, &v, 4);
    v = fp->x87_tag | 0xffff0000u;     memcpy(image + 8, &v, 4);
    memcpy(image + 12, &fp->x87_ip, 4);
    memcpy(image + 20, &fp->x87_dp, 4);
    for (unsigned i = 0; i < 8; i++) memcpy(image + 28 + i * 10, fp->x87_st[(top + i) & 7], 10);
    memcpy(image + FP_XMM, fp->xmm, sizeof(fp->xmm));
    memcpy(image + FP_MXCSR, &fp->mxcsr, 4);
}

static void image_to_fp(const uint8_t *image, PwGuestFp *fp)
{
    unsigned top;
    uint32_t v;

    memcpy(&v, image + 0, 4); fp->x87_control = (uint16_t)v;
    memcpy(&v, image + 4, 4); fp->x87_status = (uint16_t)v;
    memcpy(&v, image + 8, 4); fp->x87_tag = (uint16_t)v;
    memcpy(&fp->x87_ip, image + 12, 4);
    memcpy(&fp->x87_dp, image + 20, 4);
    top = (fp->x87_status >> 11) & 7;
    for (unsigned i = 0; i < 8; i++) memcpy(fp->x87_st[(top + i) & 7], image + 28 + i * 10, 10);
    memcpy(fp->xmm, image + FP_XMM, sizeof(fp->xmm));
    memcpy(&fp->mxcsr, image + FP_MXCSR, 4);
}

/* ---- cache and execution ---------------------------------------------- */

/* Stubs never start at the region's first byte: an instrumented indirect
 * call (clang -fsanitize=function) reads the bytes just before its target. */
enum { FIRST_STUB = 16 };

int pw_x86_hostexec_init(PwX86HostExec *h, const PwVmBackend *backend, size_t code_bytes)
{
    int status;

    if (!h || !backend || !pw_vm_backend_valid(backend) ||
        !(backend->capabilities & PW_VM_CAP_PROTECT) || code_bytes < PW_X86_HOSTEXEC_STUB_MAX)
        return PW_ERR_PRECONDITION;
    memset(h, 0, sizeof(*h));
    status = backend->reserve(backend->context, code_bytes, backend->page_bytes, &h->code);
    if (status != PW_OK) return status;
    status = backend->commit(backend->context, &h->code, 0, h->code.bytes, PW_PROT_READ);
    if (status != PW_OK) { backend->release(backend->context, &h->code); return status; }
    h->backend = backend;
    h->cursor = FIRST_STUB;
    h->initialized = 1;
    return PW_OK;
}

int pw_x86_hostexec_reset(PwX86HostExec *h)
{
    if (!h || !h->initialized) return PW_ERR_PRECONDITION;
    memset(h->slots, 0, sizeof(h->slots));
    h->cursor = FIRST_STUB;
    return PW_OK;
}

int pw_x86_hostexec_destroy(PwX86HostExec *h)
{
    int status;

    if (!h || !h->initialized) return PW_ERR_PRECONDITION;
    status = h->backend->release(h->backend->context, &h->code);
    h->initialized = 0;
    return status;
}

static int publish(PwX86HostExec *h, const uint8_t *stub, size_t bytes, uint32_t *offset)
{
    const size_t page = h->backend->page_bytes;
    size_t start = h->cursor, first, last;
    int status;

    if (bytes > h->code.bytes - start) return PW_ERR_LIMIT;
    first = start / page * page;
    last = (start + bytes + page - 1) / page * page;
    status = h->backend->protect(h->backend->context, &h->code, first, last - first,
                                 PW_PROT_READ | PW_PROT_WRITE);
    if (status != PW_OK) return status;
    memcpy((uint8_t *)h->code.write_base + start, stub, bytes);
    status = h->backend->protect(h->backend->context, &h->code, first, last - first,
                                 PW_PROT_READ | PW_PROT_EXEC);
    if (status != PW_OK) return status;
    *offset = (uint32_t)start;
    h->cursor = (start + bytes + 15) & ~(size_t)15;
    return PW_OK;
}

/* PUSH m32 (FF /6) and POP m32 (8F /0) use the guest stack implicitly, so
 * they are emulated here instead of being rewritten for the host. The SEH
 * prologue/epilogue pair `push fs:[0]` / `pop fs:[0]` is the common case. */
/*
 * CPUID as the host's, less what the guest cannot run: AVX, AVX2, FMA, F16C,
 * BMI1/2 and AVX-512 are VEX or EVEX encoded, which neither translator nor
 * this stepper takes, and XSAVE/OSXSAVE advertise the state they need.
 * SSE4.2 goes too: its CRC32 (F2 0F 38 F0/F1) and PCMPxSTRx forms are
 * refused above, and Qt5Core's qHash takes CRC32 whenever the bit is set
 * (Battle.net stopped on it). Software then takes its SSE paths, as on an
 * older processor (FFmpeg in LAV Filters picked AVX on the PS5's Zen 2).
 */
void pw_x86_cpuid_mask(uint32_t leaf, uint32_t subleaf, uint32_t regs[4])
{
    (void)subleaf;
    if (leaf == 1) {
        regs[2] &= ~((1u << 12) | (1u << 20) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29));
    } else if (leaf == 7) {
        regs[1] &= ~((1u << 3) | (1u << 5) | (1u << 8) | (1u << 16) | (1u << 17) | (1u << 21) |
                     (1u << 26) | (1u << 27) | (1u << 28) | (1u << 30) | (1u << 31));
        regs[2] &= ~((1u << 1) | (1u << 6) | (1u << 11) | (1u << 12) | (1u << 14));
        regs[3] &= ~((1u << 2) | (1u << 3) | (1u << 8) | (1u << 23));
    } else if (leaf == 0x0d) {
        regs[0] = regs[1] = regs[2] = regs[3] = 0;        /* no XSAVE state */
    } else if (leaf == 0x80000001u) {
        regs[2] &= ~((1u << 11) | (1u << 16) | (1u << 21));   /* XOP, FMA4, TBM */
    }
}

static int cpuid_form(PwX86State *s, const uint8_t *src, size_t n)
{
    uint32_t regs[4];

    if (n < 2 || src[0] != 0x0f || src[1] != 0xa2) return PW_ERR_UNSUPPORTED;
    __asm__ volatile ("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                      : "a"(s->gpr[0]), "c"(s->gpr[1]));
    pw_x86_cpuid_mask(s->gpr[0], s->gpr[1], regs);
    s->gpr[0] = regs[0]; s->gpr[3] = regs[1]; s->gpr[1] = regs[2]; s->gpr[2] = regs[3];
    s->eip += 2;
    return PW_OK;
}

static int stack_memory_form(PwX86State *s, const uint8_t *src, size_t n)
{
    size_t at = 0;
    unsigned fs = 0;
    uint32_t address, value;
    int len;

    while (at < n && at < 4 && (src[at] == 0x64 || src[at] == 0x2e || src[at] == 0x3e ||
                                src[at] == 0x26 || src[at] == 0x36)) {
        fs |= src[at] == 0x64;
        at++;
    }
    if (at + 1 >= n || (src[at] != 0xff && src[at] != 0x8f)) return PW_ERR_UNSUPPORTED;
    if (src[at] == 0xff ? ((src[at + 1] >> 3) & 7) != 6 : ((src[at + 1] >> 3) & 7) != 0)
        return PW_ERR_UNSUPPORTED;
    if ((src[at + 1] >> 6) == 3) return PW_ERR_UNSUPPORTED;   /* register forms: translator */
    if (src[at] == 0xff) {
        len = modrm_address(s, src + at + 1, n - at - 1, &address);
        if (len < 0) return len;
        if (fs) address += s->fs_base;
        memcpy(&value, (const void *)(uintptr_t)address, 4);
        s->gpr[4] -= 4;
        memcpy((void *)(uintptr_t)s->gpr[4], &value, 4);
    } else {
        memcpy(&value, (const void *)(uintptr_t)s->gpr[4], 4);
        s->gpr[4] += 4;   /* the destination address sees the popped ESP */
        len = modrm_address(s, src + at + 1, n - at - 1, &address);
        if (len < 0) { s->gpr[4] -= 4; return len; }
        if (fs) address += s->fs_base;
        memcpy((void *)(uintptr_t)address, &value, 4);
    }
    s->eip += (uint32_t)(at + 1 + (size_t)len);
    return PW_OK;
}

/* MOV between memory and ESP (89 /4, 8B /4), which the planner refuses
 * since the host runs on its own stack: an SEH prologue stores ESP in
 * fs:[0] this way. Register forms are the translator's. */
static int esp_memory_form(PwX86State *s, const uint8_t *src, size_t n)
{
    size_t at = 0;
    unsigned fs = 0;
    uint32_t address;
    int len;

    while (at < n && at < 4 && (src[at] == 0x64 || src[at] == 0x2e || src[at] == 0x3e ||
                                src[at] == 0x26 || src[at] == 0x36)) {
        fs |= src[at] == 0x64;
        at++;
    }
    if (at + 1 >= n || (src[at] != 0x89 && src[at] != 0x8b)) return PW_ERR_UNSUPPORTED;
    if (((src[at + 1] >> 3) & 7) != 4 || (src[at + 1] >> 6) == 3) return PW_ERR_UNSUPPORTED;
    len = modrm_address(s, src + at + 1, n - at - 1, &address);
    if (len < 0) return len;
    if (fs) address += s->fs_base;
    if (src[at] == 0x89) memcpy((void *)(uintptr_t)address, &s->gpr[4], 4);
    else memcpy(&s->gpr[4], (const void *)(uintptr_t)address, 4);
    s->eip += (uint32_t)(at + 1 + (size_t)len);
    return PW_OK;
}

/* PUSHFD (9C) and POPFD (9D) use the guest stack implicitly too. PUSHFD
 * stores the guest's arithmetic, DF, AC and ID bits with the always-set
 * bit 1 and IF; POPFD takes back only those bits, so the ID toggle that
 * CPUID detection performs (pushfd; btc [esp], 21; popfd) reads back as
 * supported. TF, IOPL, NT, RF and VM stay clear. The 16-bit forms (66 9C,
 * 66 9D) move only FLAGS, the low 16 bits, and leave the upper bits alone;
 * San Andreas's RenderWare runs its CPUID probe with them. */
enum { POPF_BITS = 0x00240cd5u, PUSHF_FIXED = 0x00000202u };
static int flags_stack_form(PwX86State *s, const uint8_t *src, size_t n)
{
    const unsigned narrow = n && src[0] == 0x66;
    const uint8_t op = narrow && n > 1 ? src[1] : n ? src[0] : 0;
    const uint32_t bits = narrow ? POPF_BITS & 0xffffu : POPF_BITS;
    const uint32_t size = narrow ? 2 : 4;
    uint32_t value = 0;

    if (op != 0x9c && op != 0x9d) return PW_ERR_UNSUPPORTED;
    if (op == 0x9c) {
        value = (s->eflags & POPF_BITS) | PUSHF_FIXED;
        s->gpr[4] -= size;
        memcpy((void *)(uintptr_t)s->gpr[4], &value, size);
    } else {
        memcpy(&value, (const void *)(uintptr_t)s->gpr[4], size);
        s->gpr[4] += size;
        s->eflags = (s->eflags & ~bits) | (value & bits);
    }
    s->eip += 1 + narrow;
    return PW_OK;
}

/* PUSHAD (60) and POPAD (61), which 64-bit mode does not have. PUSHAD
 * stores EAX, ECX, EDX, EBX, the ESP it started with, EBP, ESI and EDI,
 * from the highest address down; POPAD loads them back in reverse and skips
 * the saved ESP. The 16-bit forms (66 prefix) are refused. Half-Life's
 * engine brackets its CPUID feature check with them. */
static int all_registers_form(PwX86State *s, const uint8_t *src, size_t n)
{
    uint32_t values[8];   /* lowest address first: EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX */

    if (!n || (src[0] != 0x60 && src[0] != 0x61)) return PW_ERR_UNSUPPORTED;
    if (src[0] == 0x60) {
        const uint32_t esp = s->gpr[4];
        for (unsigned i = 0; i < 8; i++) values[7 - i] = s->gpr[i];   /* gpr[4] is that ESP */
        memcpy((void *)(uintptr_t)(esp - 32), values, sizeof(values));
        s->gpr[4] = esp - 32;
    } else {
        memcpy(values, (const void *)(uintptr_t)s->gpr[4], sizeof(values));
        for (unsigned i = 0; i < 8; i++)
            if (i != 4) s->gpr[i] = values[7 - i];
        s->gpr[4] += 32;
    }
    s->eip += 1;
    return PW_OK;
}

/* The branches on ECX: JECXZ (E3), LOOP (E2), LOOPE (E1) and LOOPNE (E0),
 * rel8, which the translator does not handle and the host cannot run in
 * place (they jump). LOOP* decrement ECX first; none of them changes the
 * flags; LOOPE and LOOPNE also need ZF set or clear, read from the
 * committed EFLAGS. With a 67 prefix they use CX (JCXZ). Counter-Strike
 * 1.6's vgui2.dll uses JECXZ. */
static int ecx_branch_form(PwX86State *s, const uint8_t *src, size_t n)
{
    unsigned at = 0, word = 0, take;
    uint32_t count;

    if (n && src[0] == 0x67) { word = 1; at = 1; }
    if (at + 2 > n || src[at] < 0xe0 || src[at] > 0xe3) return PW_ERR_UNSUPPORTED;
    count = word ? s->gpr[1] & 0xffffu : s->gpr[1];
    if (src[at] != 0xe3) {
        count = word ? (count - 1) & 0xffffu : count - 1;
        s->gpr[1] = word ? (s->gpr[1] & 0xffff0000u) | count : count;
    }
    switch (src[at]) {
    case 0xe3: take = count == 0; break;
    case 0xe2: take = count != 0; break;
    case 0xe1: take = count != 0 && (s->eflags & 0x40u); break;
    default:   take = count != 0 && !(s->eflags & 0x40u); break;
    }
    s->eip += at + 2;
    if (take) s->eip += (uint32_t)(int32_t)(int8_t)src[at + 1];
    return PW_OK;
}

/* PUSH and POP of ES, CS, SS and DS (06 07 0E 16 17 1E 1F), which 64-bit
 * mode does not have, so the host cannot run them. A push stores the
 * selector the guest sees (selector_of), zero-extended to 32 bits, or 16
 * bits with a 66 prefix. The data segments are flat, so a pop only moves ESP;
 * it takes a selector the guest could hold (null for ES and DS, or one of
 * its six) and refuses anything else, where the processor would raise #GP.
 * Miles Sound System (mss32.dll) saves and restores DS and ES this way. */
static int segment_stack_form(PwX86State *s, const uint8_t *src, size_t n)
{
    unsigned at = 0, size = 4, reg, pop;
    uint32_t value = 0;
    uint16_t selector;

    if (n && src[0] == 0x66) { size = 2; at = 1; }
    if (at >= n) return PW_ERR_UNSUPPORTED;
    switch (src[at]) {
    case 0x06: reg = 0; pop = 0; break;
    case 0x07: reg = 0; pop = 1; break;
    case 0x0e: reg = 1; pop = 0; break;
    case 0x16: reg = 2; pop = 0; break;
    case 0x17: reg = 2; pop = 1; break;
    case 0x1e: reg = 3; pop = 0; break;
    case 0x1f: reg = 3; pop = 1; break;
    default: return PW_ERR_UNSUPPORTED;
    }
    if (!pop) {
        value = selector_of(s, reg);
        s->gpr[4] -= size;
        memcpy((void *)(uintptr_t)s->gpr[4], &value, size);
    } else {
        memcpy(&value, (const void *)(uintptr_t)s->gpr[4], size);
        selector = (uint16_t)value;
        if (!(selector == 0 && reg != 2)) {
            unsigned known = 0;
            for (unsigned i = 0; i < 6; i++) known |= selector == selector_of(s, i);
            if (!known) return PW_ERR_UNSUPPORTED;
        }
        s->gpr[4] += size;
    }
    s->eip += at + 1;
    return PW_OK;
}

/* An address-size prefix (67) with a direct 16-bit address, [disp16] or a
 * 16-bit moffs, as old MSVC SEH prologues use on the TIB (67 64 FF 36 00 00
 * pushes fs:[0]); CLEO 4's plugins carry them. 64-bit mode has no 16-bit
 * addressing, but the address is just the zero-extended displacement, so the
 * same instruction with a 32-bit [disp32] does the same. Writes it to wide
 * and returns its length, or 0 when src is not one of these forms: only
 * MOVs, PUSH/POP of memory and two-operand ALU forms, whose lengths are
 * known; other 16-bit modes ([bx+si]...) stay refused. */
static size_t addr16_direct_form(const uint8_t *src, size_t n, uint8_t *wide, size_t *from)
{
    size_t at = 0, out = 0, imm = 0;
    unsigned addr16 = 0, opsize = 0;
    uint8_t op;

    for (; at < n && at < 4; at++) {
        if (src[at] == 0x67) { addr16 = 1; continue; }
        if (src[at] == 0x66) opsize = 1;
        else if (src[at] != 0x26 && src[at] != 0x2e && src[at] != 0x36 && src[at] != 0x3e &&
                 src[at] != 0x64 && src[at] != 0x65 && src[at] != 0xf0)
            break;
        wide[out++] = src[at];
    }
    if (!addr16 || at >= n) return 0;
    op = src[at];
    if (op >= 0xa0 && op <= 0xa3) {
        if (at + 3 > n) return 0;
        wide[out++] = op;
        wide[out++] = src[at + 1]; wide[out++] = src[at + 2]; wide[out++] = 0; wide[out++] = 0;
        *from = at + 3;
        return out;
    }
    switch (op) {
    case 0xc6: case 0x80: case 0x83: imm = 1; break;
    case 0xc7: case 0x81: imm = opsize ? 2 : 4; break;
    case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8f: case 0xff: case 0x85:
    case 0x01: case 0x03: case 0x09: case 0x0b: case 0x21: case 0x23: case 0x29:
    case 0x2b: case 0x31: case 0x33: case 0x39: case 0x3b: break;
    default: return 0;
    }
    if (at + 4 + imm > n || (src[at + 1] & 0xc7) != 0x06) return 0;   /* mod 00, rm 110 */
    wide[out++] = op;
    wide[out++] = (uint8_t)((src[at + 1] & 0x38) | 0x05);          /* [disp32] */
    wide[out++] = src[at + 2]; wide[out++] = src[at + 3]; wide[out++] = 0; wide[out++] = 0;
    memcpy(wide + out, src + at + 4, imm);
    *from = at + 4 + imm;
    return out + imm;
}

int pw_x86_hostexec_step(PwX86HostExec *h, PwX86State *s, const uint8_t *src, size_t n)
{
    PwX86HostExecPlan plan;
    PwX86HostExecSlot *slot;
    uint8_t image[FP_BYTES] __attribute__((aligned(16)));
    uint32_t hash, raw_flags;
    int status;

    if (!h || !h->initialized || !s || !src) return PW_ERR_PRECONDITION;
    {
        uint8_t wide[16];
        size_t from = 0, to = addr16_direct_form(src, n, wide, &from);

        if (to) {
            status = pw_x86_hostexec_step(h, s, wide, to);
            if (status == PW_OK) s->eip -= (uint32_t)(to - from);   /* it stepped over to bytes */
            return status;
        }
    }
    if (cpuid_form(s, src, n) == PW_OK) {
        h->executed++;
        return PW_OK;
    }
    status = pw_x86_hostexec_plan(s, src, n, &plan);
    if (status == PW_ERR_UNSUPPORTED &&
        (stack_memory_form(s, src, n) == PW_OK || esp_memory_form(s, src, n) == PW_OK ||
         flags_stack_form(s, src, n) == PW_OK ||
         segment_stack_form(s, src, n) == PW_OK || all_registers_form(s, src, n) == PW_OK ||
         ecx_branch_form(s, src, n) == PW_OK)) {
        h->executed++;
        return PW_OK;
    }
    if (status != PW_OK) { h->refused++; return status; }
    hash = s->eip * 2654435761u;
    slot = &h->slots[(hash ^ (hash >> 16)) % PW_X86_HOSTEXEC_SLOTS];
    if (!slot->used || slot->pc != s->eip || slot->length != plan.length ||
        memcmp(slot->raw, src, plan.length) != 0) {
        uint8_t stub[PW_X86_HOSTEXEC_STUB_MAX];
        size_t bytes = 0;
        uint32_t offset;

        status = emit_stub(&plan, stub, sizeof(stub), &bytes);
        if (status != PW_OK) return status;
        status = publish(h, stub, bytes, &offset);
        if (status == PW_ERR_LIMIT) {
            pw_x86_hostexec_reset(h);
            status = publish(h, stub, bytes, &offset);
        }
        if (status != PW_OK) return status;
        slot->pc = s->eip; slot->length = plan.length; slot->used = 1;
        memcpy(slot->raw, src, plan.length);
        slot->offset = offset; slot->mem_form = plan.mem_form;
        h->compiled++;
    }
    if (!s->fp.initialized) pw_guest_fp_init(&s->fp);
    fp_to_image(&s->fp, image);
    {
        void (*entry)(PwX86State *, uint32_t, uint8_t *) =
            (void (*)(PwX86State *, uint32_t, uint8_t *))(void *)
            ((uint8_t *)h->code.exec_base + slot->offset);
        const uint32_t saved = s->eflags;
        s->eflags = (saved & 0x00000cd5u) | 0x2u;
        entry(s, plan.address, image);
        memcpy(&raw_flags, image + FP_FLAGS, 4);
        s->eflags = (saved & ~0x00000cd5u) | (raw_flags & 0x00000cd5u);
    }
    image_to_fp(image, &s->fp);
    s->eip += plan.length;
    h->executed++;
    return PW_OK;
}
