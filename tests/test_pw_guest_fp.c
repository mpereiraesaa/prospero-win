/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_guest_fp.h"
#include "../include/prospero_win.h"
#include <assert.h>
#include <string.h>
#if defined(__x86_64__)
/* A state built on the host FPU: six registers live (one, pi, a denormal
 * double that loads normalised, an infinity, 1/0 and a zero), a non-default
 * control word, sticky flags, an MXCSR and eight XMM registers. Its FXSAVE
 * image is what the conversions must reproduce. */
static void native_image(uint8_t fxsave[PW_GUEST_FXSAVE_BYTES])
{
    static const uint16_t control=0x0e7f;
    static const uint32_t mxcsr=0x5fa1;
    static const uint64_t denormal=0x0000000000000001ull,infinity=0x7ff0000000000000ull;
    uint8_t xmm[8][16];
    _Alignas(16) uint8_t image[512]={0};

    for(unsigned i=0;i<sizeof(xmm);i++)xmm[i/16][i%16]=(uint8_t)(i*7+3);
    __asm__ volatile("fninit\n\tfldcw %0\n\tfld1\n\tfldpi\n\tfldl %1\n\tfldl %2\n\tfldz\n\t"
                     "fld1\n\tfdivp\n\t"   /* AT&T fdivp is st(1)=st(0)/st(1): 1/0, ZE */
                     "fldz\n\t"
                     : : "m"(control),"m"(denormal),"m"(infinity));
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile("movdqu 0(%1),%%xmm0\n\tmovdqu 16(%1),%%xmm1\n\tmovdqu 32(%1),%%xmm2\n\t"
                     "movdqu 48(%1),%%xmm3\n\tmovdqu 64(%1),%%xmm4\n\tmovdqu 80(%1),%%xmm5\n\t"
                     "movdqu 96(%1),%%xmm6\n\tmovdqu 112(%1),%%xmm7\n\t"
                     "fxsave %0\n\tfninit"
                     : "=m"(image) : "r"(xmm),"m"(xmm)
                     : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7");
    memcpy(fxsave,image,sizeof(image));
}
/* The image agrees with the hardware's in everything but the instruction
 * and operand pointers, which only the hardware has, and MXCSR_MASK; a
 * state loaded from it has the full tag word FNSTENV would report. */
static void test_image_against_hardware(void)
{
    _Alignas(16) uint8_t fxsave[PW_GUEST_FXSAVE_BYTES];
    uint8_t mine[PW_GUEST_FXSAVE_BYTES];
    uint16_t host_cw,environment[14];
    uint32_t host_sse;
    PwGuestFp fp,other;

    __asm__ volatile("fnstcw %0":"=m"(host_cw));
    __asm__ volatile("stmxcsr %0":"=m"(host_sse));
    native_image(fxsave);
    /* The full tag word of the same state, from the hardware. */
    __asm__ volatile("fxrstor %1\n\tfnstenv %0\n\tfninit":"=m"(environment):"m"(*(uint8_t(*)[512])fxsave));
    __asm__ volatile("fldcw %0"::"m"(host_cw));
    __asm__ volatile("ldmxcsr %0"::"m"(host_sse));
    assert(((fxsave[2]|fxsave[3]<<8)>>11&7)==2 && fxsave[4]==0xfc && (fxsave[2]&4)); /* six live, ZE */

    pw_guest_fp_init(&fp);fp.x87_pending=0x4;
    pw_guest_fp_from_fxsave(&fp,fxsave);
    assert(fp.x87_control==0x0e7f && fp.mxcsr==0x5fa1 && fp.x87_pending==0x4);
    assert(fp.x87_tag==environment[4]);
    assert(fp.x87_status==(fxsave[2]|fxsave[3]<<8));
    pw_guest_fp_to_fxsave(&fp,mine);
    assert(!memcmp(mine,fxsave,6) && !memcmp(mine+24,fxsave+24,4));
    assert(!memcmp(mine+32,fxsave+32,256));                /* registers and XMM */
    /* The pointers round-trip. */
    fp.x87_ip=0x401234;fp.x87_dp=0x7ffe0010;fp.x87_opcode=0x5d9;
    pw_guest_fp_to_fxsave(&fp,mine);pw_guest_fp_init(&other);pw_guest_fp_from_fxsave(&other,mine);
    assert(other.x87_ip==0x401234 && other.x87_dp==0x7ffe0010 && other.x87_opcode==0x5d9);
    assert(!memcmp(other.x87_st,fp.x87_st,sizeof(fp.x87_st)) && other.x87_tag==fp.x87_tag);
    assert(!memcmp(other.xmm,fp.xmm,sizeof(fp.xmm)) && other.mxcsr==fp.mxcsr);
}
#endif
int main(void)
{
    uint16_t host_cw,after_cw;uint32_t host_sse,after_sse;
    __asm__ volatile("fnstcw %0":"=m"(host_cw));
    __asm__ volatile("stmxcsr %0":"=m"(host_sse));
    PwGuestFp fp={0};uint32_t result=123;
    assert(pw_guest_fp_control(&fp,0,0,&result)==PW_ERR_STATE && result==123);
    pw_guest_fp_init(&fp);
    assert(pw_guest_fp_control(&fp,0,0,&result)==PW_OK && result==0x9001f);
    assert(fp.x87_control==0x027f && fp.mxcsr==0x1f80);
    assert(fp.x87_status==0 && fp.x87_tag==0xffff);
    const unsigned winbits[]={0x10,0x80000,8,4,2,1};
    for(unsigned i=0;i<6;i++) {
        pw_guest_fp_init(&fp);
        assert(pw_guest_fp_control(&fp,0,winbits[i],&result)==PW_OK);
        assert(result==(i==1?0x9001f:0x9001f&~winbits[i]));
        assert(!!(fp.x87_control&(1u<<i))==(i==1));
        assert(!!(fp.mxcsr&(1u<<(i+7)))==(i==1));
    }
    for(unsigned round=0;round<4;round++)for(unsigned precision=0;precision<3;precision++)
    for(unsigned denormal=0;denormal<4;denormal++) {
        pw_guest_fp_init(&fp);fp.mxcsr|=0x21;
        unsigned value=(round<<8)|(precision<<16)|(denormal<<24)|0x40000;
        assert(pw_guest_fp_control(&fp,value,0x3070300,&result)==PW_OK);
        assert((fp.x87_control&0xc00)==round<<10);
        assert((fp.x87_control&0x300)==(precision==0?0x300:precision==1?0x200:0));
        assert(fp.x87_control&0x1000);
        assert((fp.mxcsr&0x6000)==round<<13);
        unsigned modes[]={0,0x8040,0x40,0x8000};
        assert((fp.mxcsr&0x8040)==modes[denormal]);
        assert((fp.mxcsr&0x3f)==(!round && !denormal?0x21:0));
        assert(result==(value|0x8001f));
        uint32_t query;assert(pw_guest_fp_control(&fp,0,0,&query)==PW_OK && query==result);
    }
    pw_guest_fp_init(&fp);fp.mxcsr^=0x2000;
    assert(pw_guest_fp_control(&fp,0,0,&result)==PW_OK && result==0x8009011f);
    PwGuestFp before=fp;
    assert(pw_guest_fp_control(&fp,0,0,NULL)==PW_ERR_PRECONDITION && !memcmp(&fp,&before,sizeof(fp)));
    __asm__ volatile("fnstcw %0":"=m"(after_cw));
    __asm__ volatile("stmxcsr %0":"=m"(after_sse));
    assert(host_cw==after_cw && host_sse==after_sse);
    pw_guest_fp_init(&fp);
    uint8_t one[10]={0,0,0,0,0,0,0,0x80,0xff,0x3f},zero[10]={0},out[10];
    assert(pw_guest_x87_peek(&fp,0,out)==PW_ERR_NOT_FOUND);
    assert(pw_guest_x87_push(&fp,one)==PW_OK && ((fp.x87_status>>11)&7)==7);
    assert(((fp.x87_tag>>(7*2))&3)==0 && pw_guest_x87_peek(&fp,0,out)==PW_OK);
    assert(!memcmp(out,one,10));
    assert(pw_guest_x87_push(&fp,zero)==PW_OK && ((fp.x87_status>>11)&7)==6);
    assert(((fp.x87_tag>>(6*2))&3)==1 && pw_guest_x87_peek(&fp,1,out)==PW_OK);
    assert(!memcmp(out,one,10));
    assert(pw_guest_x87_pop(&fp,out)==PW_OK && !memcmp(out,zero,10));
    assert(((fp.x87_status>>11)&7)==7 && ((fp.x87_tag>>(6*2))&3)==3);
    for(unsigned i=0;i<7;i++)assert(pw_guest_x87_push(&fp,one)==PW_OK);
    before=fp;assert(pw_guest_x87_push(&fp,one)==PW_ERR_LIMIT && !memcmp(&fp,&before,sizeof(fp)));
    for(unsigned i=0;i<8;i++)assert(pw_guest_x87_pop(&fp,out)==PW_OK);
    before=fp;assert(pw_guest_x87_pop(&fp,out)==PW_ERR_NOT_FOUND && !memcmp(&fp,&before,sizeof(fp)));
#if defined(__x86_64__)
    test_image_against_hardware();
#endif
    return 0;
}
