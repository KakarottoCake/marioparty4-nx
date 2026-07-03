#ifndef _GAME_JMP_H
#define _GAME_JMP_H

#include "dolphin.h"

#ifdef __SWITCH__
typedef struct jump_buf {
    u64 lr;
    u64 sp;
    u64 regs[11]; // x19-x29
    double flt_regs[8]; // d8-d15
} jmp_buf;
#else
typedef struct jump_buf {
    u32 lr;
    u32 cr;
    u32 sp;
    u32 r2;
    u32 pad;
    u32 regs[19];
    double flt_regs[19];
} jmp_buf;
#endif

s32 gcsetjmp(jmp_buf *jump);
s32 gclongjmp(jmp_buf *jump, s32 status);

#endif
