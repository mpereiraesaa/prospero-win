/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_GUEST_FP_H
#define PW_GUEST_FP_H
#include <stdint.h>
/* Raw per-guest-thread state, never installed into the host by these services.
 * x87 values use the architectural 80-bit little-endian memory representation;
 * TOP remains encoded in x87_status and two-bit tags use the hardware layout. */
typedef struct PwGuestFp {
    uint16_t x87_control,x87_status,x87_tag,x87_opcode,x87_pending;
    uint32_t x87_ip,x87_dp,mxcsr;
    uint8_t x87_st[8][10];
    unsigned initialized;
    /* The i386 SSE register file, kept in memory like every other piece of
     * guest state: the emitted code uses host XMM registers only as scratch.
     * Appended after `initialized` so no earlier offset moves. */
    uint8_t xmm[8][16];
} PwGuestFp;
void pw_guest_fp_init(PwGuestFp *);
int pw_guest_fp_control(PwGuestFp *,uint32_t value,uint32_t mask,uint32_t *result);
int pw_guest_x87_push(PwGuestFp *,const uint8_t value[10]);
int pw_guest_x87_peek(const PwGuestFp *,unsigned logical_index,uint8_t value[10]);
int pw_guest_x87_pop(PwGuestFp *,uint8_t value[10]);

/* The FXSAVE image, as a Windows i386 CONTEXT's ExtendedRegisters holds it:
 * ST registers in stack order, the tag word abridged (a restore reclassifies
 * every register it marks valid), MXCSR and XMM0-7. Loading one keeps the
 * pending-exception bits, which the hardware has no place for. */
enum { PW_GUEST_FXSAVE_BYTES=512 };
void pw_guest_fp_to_fxsave(const PwGuestFp *,uint8_t out[PW_GUEST_FXSAVE_BYTES]);
void pw_guest_fp_from_fxsave(PwGuestFp *,const uint8_t in[PW_GUEST_FXSAVE_BYTES]);
#endif
