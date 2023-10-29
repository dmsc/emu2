/*
 * This is based on code by David Hedley, from pcemu.
 *
 * Most of the CPU emulation was rewritten and code was extended to support
 * 80186 and some 81280 instructions.
 */

#pragma once
#include <stdint.h>

// Enable/disable 80286 stack emulation, 80286 and higher push the old value of
// SP, 8086/80186 push new value.
//
// This is used by some software to detect extra instructions that are present
// in the 80186 also, so we emulate this even if no 80286 instructions are
// supported.
#define CPU_PUSH_80286

// Enable 80186 shift behaviour - shift count is modulo 32.
// This is used in some software to detect 80186 and higher.
#define CPU_SHIFT_80186

enum
{
    AX = 0,
    CX,
    DX,
    BX,
    SP,
    BP,
    SI,
    DI
};

enum
{
    ES = 0,
    CS,
    SS,
    DS,
    NoSeg
};

#define SetZFB(x) (ZF = !(uint8_t)(x))
#define SetZFW(x) (ZF = !(uint16_t)(x))
#define SetPF(x)  (PF = parity_table[(uint8_t)(x)])
#define SetSFW(x) (SF = (x)&0x8000)
#define SetSFB(x) (SF = (x)&0x80)

#define CompressFlags()                                                                  \
    (uint16_t)(CF | 2 | (PF << 2) | (!(!AF) << 4) | (ZF << 6) | (!(!SF) << 7) |          \
               (TF << 8) | (IF << 9) | (DF << 10) | (!(!OF) << 11))

#define ExpandFlags(f)                                                                   \
    {                                                                                    \
        CF = (f)&1;                                                                      \
        PF = ((f)&4) == 4;                                                               \
        AF = (f)&16;                                                                     \
        ZF = ((f)&64) == 64;                                                             \
        SF = (f)&128;                                                                    \
        TF = ((f)&256) == 256;                                                           \
        IF = ((f)&512) == 512;                                                           \
        DF = ((f)&1024) == 1024;                                                         \
        OF = (f)&2048;                                                                   \
    }
