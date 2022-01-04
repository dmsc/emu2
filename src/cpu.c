#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "dbg.h"
#include "dis.h"
#include "emu.h"

// Forward declarations
static void do_instruction(uint8_t code);

static uint16_t wregs[8];
static uint16_t sregs[4];

static uint16_t ip;
static uint16_t start_ip; // IP at start of instruction, used on interrupts.

/* All the byte flags will either be 1 or 0 */
static int8_t CF, PF, ZF, TF, IF, DF;

/* All the word flags may be either none-zero (true) or zero (false) */
static unsigned AF, OF, SF;

/* Override segment execution */
static int segment_override;

static uint8_t parity_table[256];

static uint16_t irq_mask; // IRQs pending

static uint8_t GetMemAbsB(uint32_t addr)
{
    return get8(addr);
}

static uint16_t GetMemAbsW(uint32_t addr)
{
    return get16(addr);
}

static void SetMemAbsB(uint32_t addr, uint8_t val)
{
    put8(addr, val);
}

static void SetMemAbsW(uint32_t addr, uint16_t val)
{
    put16(addr, val);
}

static void SetMemB(uint16_t seg, uint16_t off, uint8_t val)
{
    SetMemAbsB(sregs[seg] * 16 + off, val);
}

static uint8_t GetMemB(int seg, uint16_t off)
{
    return GetMemAbsB(sregs[seg] * 16 + off);
}

static void SetMemW(uint16_t seg, uint16_t off, uint16_t val)
{
    SetMemAbsW(sregs[seg] * 16 + off, val);
}

static uint16_t GetMemW(uint16_t seg, uint16_t off)
{
    return GetMemAbsW(sregs[seg] * 16 + off);
}

// Read memory via DS, with possible segment override.
static uint8_t GetMemDSB(uint16_t off)
{
    if(segment_override != NoSeg)
        return GetMemB(segment_override, off);
    else
        return GetMemB(DS, off);
}

static uint16_t GetMemDSW(uint16_t off)
{
    if(segment_override != NoSeg)
        return GetMemW(segment_override, off);
    else
        return GetMemW(DS, off);
}

static void PutMemDSB(uint16_t off, uint8_t val)
{
    if(segment_override != NoSeg)
        SetMemB(segment_override, off, val);
    else
        SetMemB(DS, off, val);
}

static void PutMemDSW(uint16_t off, uint16_t val)
{
    if(segment_override != NoSeg)
        SetMemW(segment_override, off, val);
    else
        SetMemW(DS, off, val);
}

static uint32_t GetAbsAddrSeg(int seg, uint16_t off)
{
    if(segment_override != NoSeg && (seg == DS || seg == SS))
        return sregs[segment_override] * 16 + off;
    else
        return sregs[seg] * 16 + off;
}

static void PushWord(uint16_t w)
{
    wregs[SP] -= 2;
    SetMemW(SS, wregs[SP], w);
}

#ifdef CPU_PUSH_80286
#define PUSH_SP()                                                              \
    PushWord(wregs[SP]);                                                       \
    break;
#else
#define PUSH_SP()                                                              \
    PushWord(wregs[SP] - 2);                                                   \
    break;
#endif

static uint16_t PopWord(void)
{
    uint16_t tmp = GetMemW(SS, wregs[SP]);
    wregs[SP] += 2;
    return tmp;
}

#define PUSH_WR(reg)                                                           \
    PushWord(wregs[reg]);                                                      \
    break;
#define POP_WR(reg)                                                            \
    wregs[reg] = PopWord();                                                    \
    break;

#define XCHG_AX_WR(reg)                                                        \
    {                                                                          \
        uint16_t tmp = wregs[reg];                                             \
        wregs[reg] = wregs[AX];                                                \
        wregs[AX] = tmp;                                                       \
        break;                                                                 \
    }

#define INC_WR(reg)                                                            \
    {                                                                          \
        uint16_t tmp = wregs[reg] + 1;                                         \
        OF = tmp == 0x8000;                                                    \
        AF = (tmp ^ (tmp - 1)) & 0x10;                                         \
        SetZFW(tmp);                                                           \
        SetSFW(tmp);                                                           \
        SetPF(tmp);                                                            \
        wregs[reg] = tmp;                                                      \
        break;                                                                 \
    }

#define DEC_WR(reg)                                                            \
    {                                                                          \
        uint16_t tmp = wregs[reg] - 1;                                         \
        OF = tmp == 0x7FFF;                                                    \
        AF = (tmp ^ (tmp + 1)) & 0x10;                                         \
        SetZFW(tmp);                                                           \
        SetSFW(tmp);                                                           \
        SetPF(tmp);                                                            \
        wregs[reg] = tmp;                                                      \
        break;                                                                 \
    }

static uint8_t FETCH_B(void)
{
    uint8_t x = GetMemB(CS, ip);
    ip++;
    return x;
}

static uint16_t FETCH_W(void)
{
    uint16_t x = GetMemW(CS, ip);
    ip += 2;
    return x;
}

#define GET_br8()                                                              \
    int ModRM = FETCH_B();                                                     \
    uint8_t src = GetModRMRegB(ModRM);                                         \
    uint8_t dest = GetModRMRMB(ModRM)

#define SET_br8() SetModRMRMB(ModRM, dest)

#define GET_r8b()                                                              \
    int ModRM = FETCH_B();                                                     \
    uint8_t dest = GetModRMRegB(ModRM);                                        \
    uint8_t src = GetModRMRMB(ModRM)

#define SET_r8b() SetModRMRegB(ModRM, dest)

#define GET_ald8()                                                             \
    uint8_t dest = wregs[AX] & 0xFF;                                           \
    uint8_t src = FETCH_B()

#define SET_ald8() wregs[AX] = (wregs[AX] & 0xFF00) | (dest & 0x00FF)

#define GET_axd16()                                                            \
    uint16_t src = FETCH_W();                                                  \
    uint16_t dest = wregs[AX];

#define SET_axd16() wregs[AX] = dest

#define GET_wr16()                                                             \
    int ModRM = FETCH_B();                                                     \
    uint16_t src = GetModRMRegW(ModRM);                                        \
    uint16_t dest = GetModRMRMW(ModRM)

#define SET_wr16() SetModRMRMW(ModRM, dest)

#define GET_r16w()                                                             \
    int ModRM = FETCH_B();                                                     \
    uint16_t dest = GetModRMRegW(ModRM);                                       \
    uint16_t src = GetModRMRMW(ModRM)

#define SET_r16w() SetModRMRegW(ModRM, dest)

void init_cpu(void)
{
    unsigned i, j, c;

    for(i = 0; i < 4; i++)
    {
        wregs[i] = 0;
        sregs[i] = 0x70;
    }
    for(; i < 8; i++)
        wregs[i] = 0;

    wregs[SP] = 0;
    ip = 0x100;

    for(i = 0; i < 256; i++)
    {
        for(j = i, c = 0; j > 0; j >>= 1)
            if(j & 1)
                c++;
        parity_table[i] = !(c & 1);
    }

    CF = PF = AF = ZF = SF = TF = IF = DF = OF = 0;

    segment_override = NoSeg;
}

static uint8_t GetModRMRegB(unsigned ModRM)
{
    unsigned reg = (ModRM >> 3) & 3;
    if(ModRM & 0x20)
        return wregs[reg] >> 8;
    else
        return wregs[reg] & 0xFF;
}

static void SetModRMRegB(unsigned ModRM, uint8_t val)
{
    unsigned reg = (ModRM >> 3) & 3;
    if(ModRM & 0x20)
        wregs[reg] = (wregs[reg] & 0x00FF) | (val << 8);
    else
        wregs[reg] = (wregs[reg] & 0xFF00) | val;
}

#define GetModRMRegW(ModRM) (wregs[(ModRM & 0x38) >> 3])
#define SetModRMRegW(ModRM, val) wregs[(ModRM & 0x38) >> 3] = val;

// Used on LEA instruction
static uint16_t GetModRMOffset(unsigned ModRM)
{
    switch(ModRM & 0xC7)
    {
    case 0x00: return wregs[BX] + wregs[SI];
    case 0x01: return wregs[BX] + wregs[DI];
    case 0x02: return wregs[BP] + wregs[SI];
    case 0x03: return wregs[BP] + wregs[DI];
    case 0x04: return wregs[SI];
    case 0x05: return wregs[DI];
    case 0x06: return FETCH_W();
    case 0x07: return wregs[BX];
    case 0x40: return wregs[BX] + wregs[SI] + (int8_t)FETCH_B();
    case 0x41: return wregs[BX] + wregs[DI] + (int8_t)FETCH_B();
    case 0x42: return wregs[BP] + wregs[SI] + (int8_t)FETCH_B();
    case 0x43: return wregs[BP] + wregs[DI] + (int8_t)FETCH_B();
    case 0x44: return wregs[SI] + (int8_t)FETCH_B();
    case 0x45: return wregs[DI] + (int8_t)FETCH_B();
    case 0x46: return wregs[BP] + (int8_t)FETCH_B();
    case 0x47: return wregs[BX] + (int8_t)FETCH_B();
    case 0x80: return FETCH_W() + wregs[BX] + wregs[SI];
    case 0x81: return FETCH_W() + wregs[BX] + wregs[DI];
    case 0x82: return FETCH_W() + wregs[BP] + wregs[SI];
    case 0x83: return FETCH_W() + wregs[BP] + wregs[DI];
    case 0x84: return FETCH_W() + wregs[SI];
    case 0x85: return FETCH_W() + wregs[DI];
    case 0x86: return FETCH_W() + wregs[BP];
    case 0x87: return FETCH_W() + wregs[BX];
    default:   return 0; // TODO: illegal instruction
    }
}

static uint32_t GetModRMAddress(unsigned ModRM)
{
    uint16_t disp = GetModRMOffset(ModRM);
    switch(ModRM & 0xC7)
    {
    case 0x00:
    case 0x01:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x40:
    case 0x41:
    case 0x44:
    case 0x45:
    case 0x47:
    case 0x80:
    case 0x81:
    case 0x84:
    case 0x85:
    case 0x87:
        return GetAbsAddrSeg(DS, disp);
    case 0x02:
    case 0x03:
    case 0x42:
    case 0x43:
    case 0x46:
    case 0x82:
    case 0x83:
    case 0x86:
        return GetAbsAddrSeg(SS, disp);
    default:
        return disp; // TODO: illegal instruction
    }
}

static uint32_t ModRMAddress;
static uint16_t GetModRMRMW(unsigned ModRM)
{
    if(ModRM >= 0xc0)
        return wregs[ModRM & 7];
    ModRMAddress = GetModRMAddress(ModRM);
    return GetMemAbsW(ModRMAddress);
}

static uint8_t GetModRMRMB(unsigned ModRM)
{
    if(ModRM >= 0xc0)
    {
        unsigned reg = ModRM & 3;
        if(ModRM & 4)
            return wregs[reg] >> 8;
        else
            return wregs[reg] & 0xFF;
    }
    ModRMAddress = GetModRMAddress(ModRM);
    return GetMemAbsB(ModRMAddress);
}

static void SetModRMRMW(unsigned ModRM, uint16_t val)
{
    if(ModRM >= 0xc0)
        wregs[ModRM & 7] = val;
    else
        SetMemAbsW(ModRMAddress, val);
}

static void SetModRMRMB(unsigned ModRM, uint8_t val)
{
    if(ModRM >= 0xc0)
    {
        unsigned reg = ModRM & 3;
        if(ModRM & 4)
            wregs[reg] = (wregs[reg] & 0x00FF) | (val << 8);
        else
            wregs[reg] = (wregs[reg] & 0xFF00) | val;
    }
    else
        SetMemAbsB(ModRMAddress, val);
}

static void next_instruction(void)
{
    start_ip = ip;
    if(sregs[CS] == 0 && ip < 0x100) // Handle our BIOS codes
    {
        FETCH_B();
        bios_routine(ip - 1);
        do_instruction(0xCF);
    }
    else
        do_instruction(FETCH_B());
}

static void interrupt(unsigned int_num)
{
    uint16_t dest_seg, dest_off;

    dest_off = GetMemAbsW(int_num * 4);
    dest_seg = GetMemAbsW(int_num * 4 + 2);

    PushWord(CompressFlags());
    PushWord(sregs[CS]);
    PushWord(ip);

    ip = dest_off;
    sregs[CS] = dest_seg;

    TF = IF = 0; /* Turn of trap and interrupts... */
}

static void do_retf(void)
{
    ip = PopWord();
    sregs[CS] = PopWord();
}

static void trap_1(void)
{
    next_instruction();
    interrupt(1);
}

static void do_popf(void)
{
    uint16_t tmp = PopWord();
    ExpandFlags(tmp);
    if(TF)
        trap_1(); // this is the only way the TRAP flag can be set
}

static void do_iret(void)
{
    do_retf();
    do_popf();
}

// BOUND or DIV0
static void cpu_trap(int num)
{
    ip = start_ip;
    interrupt(num);
}

static void handle_irq(void)
{
    if(IF && irq_mask)
    {
        // Get lower set bit (highest priority IRQ)
        uint16_t bit = irq_mask & -irq_mask;
        if(bit)
        {
            uint8_t bp[16] = {0, 1, 2, 5, 3, 9, 6, 11, 15, 4, 8, 10, 14, 7, 13, 12};
            uint8_t irqn = bp[(bit * 0x9af) >> 12];
            debug(debug_int, "handle irq, mask=$%04x irq=%d\n", irq_mask, irqn);
            irq_mask &= ~bit;
            if(irqn < 8)
                interrupt(8 + irqn);
            else
                interrupt(0x68 + irqn);
        }
    }
}

#define ADD_8()                                                                \
    unsigned tmp = dest + src;                                                 \
    OF = (tmp ^ src) & (tmp ^ dest) & 0x80;                                    \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    CF = tmp >> 8;                                                             \
    dest = tmp;                                                                \
    SetZFB(dest);                                                              \
    SetSFB(dest);                                                              \
    SetPF(dest)

#define ADD_16()                                                               \
    unsigned tmp = dest + src;                                                 \
    OF = (tmp ^ src) & (tmp ^ dest) & 0x8000;                                  \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    CF = tmp >> 16;                                                            \
    dest = tmp;                                                                \
    SetZFW(dest);                                                              \
    SetSFW(dest);                                                              \
    SetPF(dest)

#define ADC_8()                                                                \
    unsigned tmp = dest + src + CF;                                            \
    OF = (tmp ^ src) & (tmp ^ dest) & 0x80;                                    \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    CF = tmp >> 8;                                                             \
    dest = tmp;                                                                \
    SetZFB(dest);                                                              \
    SetSFB(dest);                                                              \
    SetPF(dest);

#define ADC_16()                                                               \
    unsigned tmp = dest + src + CF;                                            \
    OF = (tmp ^ src) & (tmp ^ dest) & 0x8000;                                  \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    CF = tmp >> 16;                                                            \
    dest = tmp;                                                                \
    SetZFW(dest);                                                              \
    SetSFW(dest);                                                              \
    SetPF(dest);

#define SBB_8()                                                                \
    unsigned tmp = dest - src - CF;                                            \
    CF = (tmp & 0x100) == 0x100;                                               \
    OF = (dest ^ src) & (dest ^ tmp) & 0x80;                                   \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    dest = tmp;                                                                \
    SetZFB(dest);                                                              \
    SetSFB(dest);                                                              \
    SetPF(dest);

#define SBB_16()                                                               \
    unsigned tmp = dest - src - CF;                                            \
    CF = (tmp & 0x10000) == 0x10000;                                           \
    OF = (dest ^ src) & (dest ^ tmp) & 0x8000;                                 \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    dest = tmp;                                                                \
    SetZFW(dest);                                                              \
    SetSFW(dest);                                                              \
    SetPF(dest);

#define SUB_8()                                                                \
    unsigned tmp = dest - src;                                                 \
    CF = (tmp & 0x100) == 0x100;                                               \
    OF = (dest ^ src) & (dest ^ tmp) & 0x80;                                   \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    dest = tmp;                                                                \
    SetZFB(dest);                                                              \
    SetSFB(dest);                                                              \
    SetPF(dest)

#define SUB_16()                                                               \
    unsigned tmp = dest - src;                                                 \
    CF = (tmp & 0x10000) == 0x10000;                                           \
    OF = (dest ^ src) & (dest ^ tmp) & 0x8000;                                 \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    dest = tmp;                                                                \
    SetZFW(dest);                                                              \
    SetSFW(dest);                                                              \
    SetPF(dest);

#define CMP_8()                                                                \
    uint16_t tmp = dest - src;                                                 \
    CF = (tmp & 0x100) == 0x100;                                               \
    OF = (dest ^ src) & (dest ^ tmp) & 0x80;                                   \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    SetZFB(tmp);                                                               \
    SetSFB(tmp);                                                               \
    SetPF(tmp);

#define CMP_16()                                                               \
    unsigned tmp = dest - src;                                                 \
    CF = (tmp & 0x10000) == 0x10000;                                           \
    OF = (dest ^ src) & (dest ^ tmp) & 0x8000;                                 \
    AF = (tmp ^ src ^ dest) & 0x10 ? 1 : 0;                                    \
    SetZFW(tmp);                                                               \
    SetSFW(tmp);                                                               \
    SetPF(tmp);

#define OR_8(op)                                                               \
    dest |= src;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFB(dest);                                                              \
    SetSFB(dest);                                                              \
    SetPF(dest);

#define OR_16(op)                                                              \
    dest |= src;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFW(dest);                                                              \
    SetSFW(dest);                                                              \
    SetPF(dest);

#define AND_8(op)                                                              \
    dest &= src;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFB(dest);                                                              \
    SetSFB(dest);                                                              \
    SetPF(dest);

#define AND_16(op)                                                             \
    dest &= src;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFW(dest);                                                              \
    SetSFW(dest);                                                              \
    SetPF(dest);

#define XOR_8(op)                                                              \
    dest ^= src;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFB(dest);                                                              \
    SetSFB(dest);                                                              \
    SetPF(dest);

#define XOR_16(op)                                                             \
    dest ^= src;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFW(dest);                                                              \
    SetSFW(dest);                                                              \
    SetPF(dest);

#define TEST_8(op)                                                             \
    src &= dest;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFB(src);                                                               \
    SetSFB(src);                                                               \
    SetPF(src);

#define TEST_16(op)                                                            \
    src &= dest;                                                               \
    CF = OF = AF = 0;                                                          \
    SetZFW(src);                                                               \
    SetSFW(src);                                                               \
    SetPF(src);

#define XCHG_8(op)                                                             \
    uint8_t tmp = dest;                                                        \
    dest = src;                                                                \
    src = tmp;

#define XCHG_16(op)                                                            \
    uint16_t tmp = dest;                                                       \
    dest = src;                                                                \
    src = tmp;

#define MOV_8(op) dest = src;

#define MOV_16(op) dest = src;

#define OP_br8(op)                                                             \
    {                                                                          \
        GET_br8();                                                             \
        op##_8();                                                              \
        SET_br8();                                                             \
    }                                                                          \
    break;

#define OP_r8b(op)                                                             \
    {                                                                          \
        GET_r8b();                                                             \
        op##_8();                                                              \
        SET_r8b();                                                             \
    }                                                                          \
    break;

#define OP_wr16(op)                                                            \
    {                                                                          \
        GET_wr16();                                                            \
        op##_16();                                                             \
        SET_wr16();                                                            \
    }                                                                          \
    break;

#define OP_r16w(op)                                                            \
    {                                                                          \
        GET_r16w();                                                            \
        op##_16();                                                             \
        SET_r16w();                                                            \
    }                                                                          \
    break;

#define OP_ald8(op)                                                            \
    {                                                                          \
        GET_ald8();                                                            \
        op##_8();                                                              \
        SET_ald8();                                                            \
    }                                                                          \
    break;

#define OP_axd16(op)                                                           \
    {                                                                          \
        GET_axd16();                                                           \
        op##_16();                                                             \
        SET_axd16();                                                           \
    }                                                                          \
    break;

#define MOV_BRH(reg)                                                           \
    wregs[reg] = ((0x00FF & wregs[reg]) | (FETCH_B() << 8));                   \
    break;
#define MOV_BRL(reg)                                                           \
    wregs[reg] = ((0xFF00 & wregs[reg]) | FETCH_B());                          \
    break;
#define MOV_WRi(reg)                                                           \
    wregs[reg] = FETCH_W();                                                    \
    break;

#define SEG_OVERRIDE(seg)                                                      \
    {                                                                          \
        segment_override = seg;                                                \
        do_instruction(FETCH_B());                                             \
        segment_override = NoSeg;                                              \
    }                                                                          \
    break;

static void i_undefined(void)
{
    // Generate an invalid opcode exception
    cpu_trap(6);
}

static void i_das(void)
{
    uint8_t old_al = wregs[AX] & 0xFF;
    uint8_t old_CF = CF;
    unsigned al = old_al;
    CF = 0;
    if(AF || (old_al & 0x0F) > 9)
    {
        al = al - 6;
        CF = old_CF || al > 0xFF;
        al = al & 0xFF;
        AF = 1;
    }
    else
        AF = 0;
    if(old_CF || old_al > 0x99)
    {
        al = (al - 0x60) & 0xFF;
        CF = 1;
    }
    SetZFB(al);
    SetPF(al);
    SetSFB(al);
    wregs[AX] = (wregs[AX] & 0xFF00) | al;
}

static void i_daa(void)
{
    uint8_t al = wregs[AX] & 0xFF;
    if(AF || ((al & 0xf) > 9))
    {
        al += 6;
        AF = 1;
    }
    else
        AF = 0;

    if(CF || (al > 0x9f))
    {
        al += 0x60;
        CF = 1;
    }
    else
        CF = 0;

    wregs[AX] = (wregs[AX] & 0xFF00) | al;
    SetPF(al);
    SetSFB(al);
    SetZFB(al);
}

static void i_aaa(void)
{
    uint16_t ax = wregs[AX];
    if(AF || (ax & 0xF) > 9)
    {
        ax = ((ax + 0x100) & 0xFF00) | ((ax + 6) & 0x0F);
        AF = 1;
        CF = 1;
    }
    else
    {
        AF = 0;
        CF = 0;
        ax = ax & 0xFF0F;
    }
    SetZFB(ax);
    SetPF(ax);
    SetSFB(ax);
    wregs[AX] = ax;
}

static void i_aas(void)
{
    uint16_t ax = wregs[AX];
    if(AF || (ax & 0xF) > 9)
    {
        ax = (ax - 0x106) & 0xFF0F;
        AF = 1;
        CF = 1;
    }
    else
    {
        AF = 0;
        CF = 0;
        ax = ax & 0xFF0F;
    }
    SetZFB(ax);
    SetPF(ax);
    SetSFB(ax);
    wregs[AX] = ax;
}

#define IMUL_2                                                                 \
    uint32_t result = (int16_t)src * (int16_t)mult;                            \
    dest = result & 0xFFFF;                                                    \
    SetSFW(dest);                                                              \
    SetZFW(dest);                                                              \
    SetPF(dest);                                                               \
    result &= 0xFFFF8000;                                                      \
    CF = OF = ((result != 0) && (result != 0xFFFF8000))

static void i_imul_r16w_d16(void)
{
    GET_r16w();
    int16_t mult = FETCH_W();
    IMUL_2;
    SET_r16w();
}

static void i_imul_r16w_d8(void)
{
    GET_r16w();
    int8_t mult = FETCH_B();
    IMUL_2;
    SET_r16w();
}

static void do_cjump(unsigned cond)
{
    int8_t disp = FETCH_B();
    if(cond)
        ip = ip + disp;
}

static void i_80pre(void)
{
    int ModRM = FETCH_B();
    uint8_t dest = GetModRMRMB(ModRM);
    uint8_t src = FETCH_B();

    switch(ModRM & 0x38)
    {
    case 0x00:
    {
        ADD_8();
        SET_br8();
        break;
    }
    case 0x08:
    {
        OR_8();
        SET_br8();
        break;
    }
    case 0x10:
    {
        ADC_8();
        SET_br8();
        break;
    }
    case 0x18:
    {
        SBB_8();
        SET_br8();
        break;
    }
    case 0x20:
    {
        AND_8();
        SET_br8();
        break;
    }
    case 0x28:
    {
        SUB_8();
        SET_br8();
        break;
    }
    case 0x30:
    {
        XOR_8();
        SET_br8();
        break;
    }
    case 0x38:
    {
        CMP_8();
        break;
    }
    }
}

static void i_81pre(void)
{
    int ModRM = FETCH_B();
    uint16_t dest = GetModRMRMW(ModRM);
    uint16_t src = FETCH_W();

    switch(ModRM & 0x38)
    {
    case 0x00:
    {
        ADD_16();
        SET_wr16();
        break;
    }
    case 0x08:
    {
        OR_16();
        SET_wr16();
        break;
    }
    case 0x10:
    {
        ADC_16();
        SET_wr16();
        break;
    }
    case 0x18:
    {
        SBB_16();
        SET_wr16();
        break;
    }
    case 0x20:
    {
        AND_16();
        SET_wr16();
        break;
    }
    case 0x28:
    {
        SUB_16();
        SET_wr16();
        break;
    }
    case 0x30:
    {
        XOR_16();
        SET_wr16();
        break;
    }
    case 0x38:
    {
        CMP_16();
        break;
    }
    }
}

static void i_82pre(void)
{
    int ModRM = FETCH_B();
    uint8_t dest = GetModRMRMB(ModRM);
    uint8_t src = (int8_t)FETCH_B();

    switch(ModRM & 0x38)
    {
    case 0x00:
    {
        ADD_8();
        SET_br8();
        break;
    }
    case 0x08:
    {
        OR_8();
        SET_br8();
        break;
    }
    case 0x10:
    {
        ADC_8();
        SET_br8();
        break;
    }
    case 0x18:
    {
        SBB_8();
        SET_br8();
        break;
    }
    case 0x20:
    {
        AND_8();
        SET_br8();
        break;
    }
    case 0x28:
    {
        SUB_8();
        SET_br8();
        break;
    }
    case 0x30:
    {
        XOR_8();
        SET_br8();
        break;
    }
    case 0x38:
    {
        CMP_8();
        break;
    }
    }
}

static void i_83pre(void)
{
    int ModRM = FETCH_B();
    uint16_t dest = GetModRMRMW(ModRM);
    uint16_t src = (int8_t)FETCH_B();

    switch(ModRM & 0x38)
    {
    case 0x00:
    {
        ADD_16();
        SET_wr16();
        break;
    }
    case 0x08:
    {
        OR_16();
        SET_wr16();
        break;
    }
    case 0x10:
    {
        ADC_16();
        SET_wr16();
        break;
    }
    case 0x18:
    {
        SBB_16();
        SET_wr16();
        break;
    }
    case 0x20:
    {
        AND_16();
        SET_wr16();
        break;
    }
    case 0x28:
    {
        SUB_16();
        SET_wr16();
        break;
    }
    case 0x30:
    {
        XOR_16();
        SET_wr16();
        break;
    }
    case 0x38:
    {
        CMP_16();
        break;
    }
    }
}

static void i_xchg_br8(void)
{
    GET_br8();
    XCHG_8();
    SET_br8();
    dest = src;
    SET_r8b();
}

static void i_xchg_wr16(void)
{
    GET_wr16();
    XCHG_16();
    SET_wr16();
    dest = src;
    SET_r16w();
}

static void i_mov_wsreg(void)
{
    int ModRM = FETCH_B();
    GetModRMRMW(ModRM);
    SetModRMRMW(ModRM, sregs[(ModRM & 0x18) >> 3]);
}

static void i_mov_sregw(void)
{
    int ModRM = FETCH_B();
    sregs[(ModRM & 0x18) >> 3] = GetModRMRMW(ModRM);
}

static void i_lea(void)
{
    int ModRM = FETCH_B();
    uint16_t offs = GetModRMOffset(ModRM);

    if(ModRM >= 0xc0)
        return; // TODO: ILLEGAL INSTRUCTION!!!

    SetModRMRegW(ModRM, offs);
}

static void i_popw(void)
{
    int ModRM = FETCH_B();
    //    if( GetModRMRegW(ModRM) != 0 )
    //        return; // TODO: illegal instruction - ignored in 8086
    if(ModRM < 0xc0)
        ModRMAddress = GetModRMAddress(ModRM);
    SetModRMRMW(ModRM, PopWord());
}

static void i_call_far(void)
{
    uint16_t tgt_ip = FETCH_W();
    uint16_t tgt_cs = FETCH_W();

    PushWord(sregs[CS]);
    PushWord(ip);

    ip = tgt_ip;
    sregs[CS] = tgt_cs;
}

static void i_sahf(void)
{
    uint16_t tmp = (CompressFlags() & 0xff00) | ((wregs[AX] >> 8) & 0xD5);
    ExpandFlags(tmp);
}

static void i_lahf(void)
{
    wregs[AX] = (wregs[AX] & 0xFF) | (CompressFlags() << 8);
}

static void i_mov_aldisp(void)
{
    uint16_t addr = FETCH_W();
    wregs[AX] = (wregs[AX] & 0xFF00) | GetMemDSB(addr);
}

static void i_mov_axdisp(void)
{
    uint16_t addr = FETCH_W();
    wregs[AX] = GetMemDSW(addr);
}

static void i_mov_dispal(void)
{
    uint16_t addr = FETCH_W();
    PutMemDSB(addr, wregs[AX] & 0xFF);
}

static void i_mov_dispax(void)
{
    uint16_t addr = FETCH_W();
    PutMemDSW(addr, wregs[AX]);
}

static void i_movsb(void)
{
    SetMemB(ES, wregs[DI], GetMemDSB(wregs[SI]));

    wregs[SI] += 1 - 2 * DF;
    wregs[DI] += 1 - 2 * DF;
}

static void i_movsw(void)
{
    SetMemW(ES, wregs[DI], GetMemDSW(wregs[SI]));

    wregs[SI] += 2 - 4 * DF;
    wregs[DI] += 2 - 4 * DF;
}

static void i_cmpsb(void)
{
    unsigned src = GetMemB(ES, wregs[DI]);
    unsigned dest = GetMemDSB(wregs[SI]);
    CMP_8();
    wregs[DI] += 1 - 2 * DF;
    wregs[SI] += 1 - 2 * DF;
}

static void i_cmpsw(void)
{
    unsigned src = GetMemW(ES, wregs[DI]);
    unsigned dest = GetMemDSW(wregs[SI]);
    CMP_16();
    wregs[DI] += -4 * DF + 2;
    wregs[SI] += -4 * DF + 2;
}

static void i_stosb(void)
{
    SetMemB(ES, wregs[DI], wregs[AX]);
    wregs[DI] += 1 - 2 * DF;
}

static void i_stosw(void)
{
    SetMemW(ES, wregs[DI], wregs[AX]);
    wregs[DI] += 2 - 4 * DF;
}

static void i_lodsb(void)
{
    wregs[AX] = (wregs[AX] & 0xFF00) | GetMemDSB(wregs[SI]);
    wregs[SI] += 1 - 2 * DF;
}

static void i_lodsw(void)
{
    wregs[AX] = GetMemDSW(wregs[SI]);
    wregs[SI] += 2 - 4 * DF;
}

static void i_scasb(void)
{
    unsigned src = GetMemB(ES, wregs[DI]);
    unsigned dest = wregs[AX] & 0xFF;
    CMP_8();
    wregs[DI] += 1 - 2 * DF;
}

static void i_scasw(void)
{
    unsigned src = GetMemW(ES, wregs[DI]);
    unsigned dest = wregs[AX];
    CMP_16();
    wregs[DI] += 2 - 4 * DF;
}

static void i_insb(void)
{
    SetMemB(ES, wregs[DI], read_port(wregs[DX]));
    wregs[DI] += 1 - 2 * DF;
}

static void i_insw(void)
{
    uint16_t val = read_port(wregs[DX]);
    val |= read_port(wregs[DX] + 1) << 8;
    SetMemW(ES, wregs[DI], val);
    wregs[DI] += 2 - 4 * DF;
}

static void i_outsb(void)
{
    uint8_t val = (wregs[AX] & 0xFF00) | GetMemDSB(wregs[SI]);
    write_port(wregs[DX], val);
    wregs[SI] += 1 - 2 * DF;
}

static void i_outsw(void)
{
    uint16_t val = GetMemDSW(wregs[SI]);
    write_port(wregs[DX], val & 0xFF);
    write_port(wregs[DX] + 1, val >> 8);
    wregs[SI] += 2 - 4 * DF;
}

static void i_ret_d16(void)
{
    uint16_t count = FETCH_W();
    ip = PopWord();
    wregs[SP] += count;
}

static void i_ret(void)
{
    ip = PopWord();
}

static void i_les_dw(void)
{
    GET_r16w();
    dest = src;
    sregs[ES] = GetMemAbsW(ModRMAddress + 2);
    SET_r16w();
}

static void i_lds_dw(void)
{
    GET_r16w();
    dest = src;
    sregs[DS] = GetMemAbsW(ModRMAddress + 2);
    SET_r16w();
}

static void i_mov_bd8(void)
{
    int ModRM = FETCH_B();
    if(ModRM < 0xc0)
        ModRMAddress = GetModRMAddress(ModRM);
    uint8_t dest = FETCH_B();
    SET_br8();
}

static void i_mov_wd16(void)
{
    int ModRM = FETCH_B();
    if(ModRM < 0xc0)
        ModRMAddress = GetModRMAddress(ModRM);
    uint16_t dest = FETCH_W();
    SET_wr16();
}

static void i_retf_d16(void)
{
    uint16_t count = FETCH_W();
    do_retf();
    wregs[SP] += count;
}

static void i_int3(void)
{
    interrupt(3);
}

static void i_int(void)
{
    interrupt(FETCH_B());
}

static void i_into(void)
{
    if(OF)
        interrupt(4);
}

static uint8_t shift1_b(uint8_t val, int ModRM)
{
    AF = 0;
    switch(ModRM & 0x38)
    {
    case 0x00: /* ROL eb,1 */
        CF = (val & 0x80) != 0;
        val = (val << 1) + CF;
        OF = !(val & 0x80) != !CF;
        break;
    case 0x08: /* ROR eb,1 */
        CF = (val & 0x01) != 0;
        val = (val >> 1) + (CF << 7);
        OF = !(val & 0x40) != !(val & 0x80);
        break;
    case 0x10: /* RCL eb,1 */
    {
        uint8_t oldCF = CF;
        CF = (val & 0x80) != 0;
        val = (val << 1) | oldCF;
        OF = !(val & 0x80) != !CF;
        break;
    }
    case 0x18: /* RCR eb,1 */
    {
        uint8_t oldCF = CF;
        CF = val & 1;
        val = (val >> 1) | (oldCF << 7);
        OF = !(val & 0x40) != !(val & 0x80);
        break;
    }
    case 0x20: /* SHL eb,1 */
    case 0x30:
        CF = (val & 0x80) != 0;
        val = val << 1;
        OF = !(val & 0x80) != !CF;
        SetZFB(val);
        SetSFB(val);
        SetPF(val);
        break;
    case 0x28: /* SHR eb,1 */
        CF = (val & 0x01) != 0;
        OF = (val & 0x80) != 0;
        val = val >> 1;
        SetSFB(val);
        SetZFB(val);
        SetPF(val);
        break;
    case 0x38: /* SAR eb,1 */
        CF = (val & 0x01) != 0;
        OF = 0;
        val = (val >> 1) | (val & 0x80);
        SetSFB(val);
        SetZFB(val);
        SetPF(val);
        break;
    }
    return val;
}

static uint8_t shifts_b(uint8_t val, int ModRM, unsigned count)
{

#ifdef CPU_SHIFT_80186
    count &= 0x1F;
#endif

    if(!count)
        return val; // No flags affected.

    if(count == 1)
        return shift1_b(val, ModRM);

    AF = 0;
    OF = 0;
    switch(ModRM & 0x38)
    {
    case 0x00: /* ROL eb,CL */
        for(; count > 0; count--)
        {
            CF = (val & 0x80) != 0;
            val = (val << 1) | CF;
        }
        OF = !(val & 0x80) != !CF;
        break;
    case 0x08: /* ROR eb,CL */
        for(; count > 0; count--)
        {
            CF = (val & 0x01) != 0;
            val = (val >> 1) | (CF << 7);
        }
        OF = !(val & 0x40) != !(val & 0x80);
        break;
    case 0x10: /* RCL eb,CL */
        for(; count > 0; count--)
        {
            uint8_t oldCF = CF;
            CF = (val & 0x80) != 0;
            val = (val << 1) | oldCF;
        }
        OF = !(val & 0x80) != !CF;
        break;
    case 0x18: /* RCR eb,CL */
        for(; count > 0; count--)
        {
            uint8_t oldCF = CF;
            CF = val & 1;
            val = (val >> 1) | (oldCF << 7);
        }
        OF = !(val & 0x40) != !(val & 0x80);
        break;
    case 0x20:
    case 0x30: /* SHL eb,CL */
        if(count >= 9)
        {
            CF = 0;
            val = 0;
        }
        else
        {
            CF = (val & (0x100 >> count)) != 0;
            val <<= count;
        }
        OF = !(val & 0x80) != !CF;
        SetZFB(val);
        SetSFB(val);
        SetPF(val);
        break;
    case 0x28: /* SHR eb,CL */
        if(count >= 9)
        {
            CF = 0;
            val = 0;
        }
        else
        {
            CF = ((val >> (count - 1)) & 0x1) != 0;
            val >>= count;
        }
        SetSFB(val);
        SetPF(val);
        SetZFB(val);
        break;
    case 0x38: /* SAR eb,CL */
        CF = (((int8_t)val >> (count - 1)) & 0x01) != 0;
        for(; count > 0; count--)
            val = (val >> 1) | (val & 0x80);
        SetSFB(val);
        SetPF(val);
        SetZFB(val);
        break;
    }
    return val;
}

static uint16_t shift1_w(uint16_t val, int ModRM)
{
    AF = 0;
    switch(ModRM & 0x38)
    {
    case 0x00: /* ROL ew,1 */
        CF = (val & 0x8000) != 0;
        val = (val << 1) + CF;
        OF = !(val & 0x8000) != !CF;
        break;
    case 0x08: /* ROR ew,1 */
        CF = (val & 0x01) != 0;
        val = (val >> 1) + (CF << 15);
        OF = !(val & 0x4000) != !(val & 0x8000);
        break;
    case 0x10: /* RCL ew,1 */
    {
        uint8_t oldCF = CF;
        CF = (val & 0x8000) != 0;
        val = (val << 1) | oldCF;
        OF = !(val & 0x8000) != !CF;
    }
    break;
    case 0x18: /* RCR ew,1 */
    {
        uint8_t oldCF = CF;
        CF = val & 1;
        val = (val >> 1) | (oldCF << 15);
        OF = !(val & 0x4000) != !(val & 0x8000);
    }
    break;
    case 0x20: /* SHL eb,1 */
    case 0x30:
        CF = (val & 0x8000) != 0;
        val = val << 1;
        OF = !(val & 0x8000) != !CF;
        SetZFW(val);
        SetSFW(val);
        SetPF(val);
        break;
    case 0x28: /* SHR eb,1 */
        CF = (val & 0x01) != 0;
        OF = (val & 0x8000) != 0;
        val = val >> 1;
        SetSFW(val);
        SetZFW(val);
        SetPF(val);
        break;
    case 0x38: /* SAR eb,1 */
        CF = (val & 0x01) != 0;
        OF = 0;
        val = (val >> 1) | (val & 0x8000);
        SetSFW(val);
        SetZFW(val);
        SetPF(val);
        break;
    }
    return val;
}

static uint16_t shifts_w(uint16_t val, int ModRM, unsigned count)
{
#ifdef CPU_SHIFT_80186
    count &= 0x1F;
#endif

    if(!count)
        return val; // No flags affected.

    if(count == 1)
        return shift1_w(val, ModRM);

    AF = 0;
    OF = 0;
    switch(ModRM & 0x38)
    {
    case 0x00: /* ROL ew,CL */
        for(; count > 0; count--)
        {
            CF = (val & 0x8000) != 0;
            val = (val << 1) | CF;
        }
        OF = !(val & 0x8000) != !CF;
        break;
    case 0x08: /* ROR ew,CL */
        for(; count > 0; count--)
        {
            CF = (val & 0x01) != 0;
            val = (val >> 1) | (CF << 15);
        }
        OF = !(val & 0x4000) != !(val & 0x8000);
        break;
    case 0x10: /* RCL ew,CL */
        for(; count > 0; count--)
        {
            uint8_t oldCF = CF;
            CF = (val & 0x8000) != 0;
            val = (val << 1) | oldCF;
        }
        OF = !(val & 0x8000) != !CF;
        break;
    case 0x18: /* RCR ew,CL */
        for(; count > 0; count--)
        {
            uint8_t oldCF = CF;
            CF = val & 1;
            val = (val >> 1) | (oldCF << 15);
        }
        OF = !(val & 0x4000) != !(val & 0x8000);
        break;
    case 0x20:
    case 0x30: /* SHL eb,CL */
        if(count > 16)
        {
            CF = 0;
            val = 0;
        }
        else
        {
            CF = (val & (0x10000 >> count)) != 0;
            val <<= count;
        }
        OF = !(val & 0x8000) != !CF;
        SetZFW(val);
        SetSFW(val);
        SetPF(val);
        break;
    case 0x28: /* SHR eb,CL */
        if(count > 16)
        {
            CF = 0;
            val = 0;
        }
        else
        {
            CF = ((val >> (count - 1)) & 0x1) != 0;
            val >>= count;
        }
        SetSFW(val);
        SetZFW(val);
        SetPF(val);
        break;
    case 0x38: /* SAR eb,CL */
        CF = (((int8_t)val >> (count - 1)) & 0x01) != 0;
        for(; count > 0; count--)
            val = (val >> 1) | (val & 0x8000);
        SetSFW(val);
        SetZFW(val);
        SetPF(val);
        break;
    }

    return val;
}

static void i_c0pre(void)
{
    int ModRM = FETCH_B();
    uint8_t dest = GetModRMRMB(ModRM);
    uint8_t count = FETCH_B();

    dest = shifts_b(dest, ModRM, count);

    SetModRMRMB(ModRM, dest);
}

static void i_c1pre(void)
{
    int ModRM = FETCH_B();
    uint16_t dest = GetModRMRMW(ModRM);
    uint8_t count = FETCH_B();

    dest = shifts_w(dest, ModRM, count);

    SetModRMRMW(ModRM, dest);
}

static void i_d0pre(void)
{
    int ModRM = FETCH_B();
    uint8_t dest = GetModRMRMB(ModRM);

    dest = shift1_b(dest, ModRM);

    SetModRMRMB(ModRM, dest);
}

static void i_d1pre(void)
{
    int ModRM = FETCH_B();
    uint16_t dest = GetModRMRMW(ModRM);

    dest = shift1_w(dest, ModRM);

    SetModRMRMW(ModRM, dest);
}

static void i_d2pre(void)
{
    int ModRM = FETCH_B();
    uint8_t dest = GetModRMRMB(ModRM);

    dest = shifts_b(dest, ModRM, wregs[CX] & 0xFF);

    SetModRMRMB(ModRM, dest);
}

static void i_d3pre(void)
{
    int ModRM = FETCH_B();
    uint16_t dest = GetModRMRMW(ModRM);

    dest = shifts_w(dest, ModRM, wregs[CX] & 0xFF);

    SetModRMRMW(ModRM, dest);
}

static void i_aam(void)
{
    unsigned mult = FETCH_B();

    if(mult == 0)
        cpu_trap(0);
    else
    {
        unsigned al = wregs[AX] & 0xFF;
        wregs[AX] = ((al % mult) & 0xFF) | ((al / mult) << 8);

        SetPF(al);
        SetZFW(wregs[AX]);
        SetSFW(wregs[AX]);
    }
}

static void i_aad(void)
{
    unsigned mult = FETCH_B();

    uint16_t ax = wregs[AX];
    ax = 0xFF & ((ax >> 8) * mult + ax);

    wregs[AX] = ax;
    AF = 0;
    OF = 0;
    CF = 0;
    SetPF(ax);
    SetSFB(ax);
    SetZFB(ax);
}

static void i_xlat(void)
{
    wregs[AX] = (wregs[AX] & 0xFF00) | GetMemDSB(wregs[BX] + (wregs[AX] & 0xFF));
}

static void i_escape(void)
{
    /* This is FPU opcodes 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde and 0xdf */
    GetModRMRMB(FETCH_B());
}

static void i_loopne(void)
{
    int disp = (int8_t)FETCH_B();
    wregs[CX]--;
    if(!ZF && wregs[CX])
        ip = ip + disp;
}

static void i_loope(void)
{
    int disp = (int8_t)FETCH_B();
    wregs[CX]--;
    if(ZF && wregs[CX])
        ip = ip + disp;
}

static void i_loop(void)
{
    int disp = (int8_t)FETCH_B();
    wregs[CX]--;
    if(wregs[CX])
        ip = ip + disp;
}

static void i_jcxz(void)
{
    int disp = (int8_t)FETCH_B();
    if(wregs[CX] == 0)
        ip = ip + disp;
}

static void i_inal(void)
{
    unsigned port = FETCH_B();
    wregs[AX] = (wregs[AX] & 0xFF00) | read_port(port);
}

static void i_inax(void)
{
    unsigned port = FETCH_B();
    wregs[AX] = read_port(port);
    wregs[AX] |= read_port(port + 1) << 8;
}

static void i_outal(void)
{
    unsigned port = FETCH_B();
    write_port(port, wregs[AX] & 0xFF);
}

static void i_outax(void)
{
    unsigned port = FETCH_B();
    write_port(port, wregs[AX] & 0xFF);
    write_port(port + 1, wregs[AX] >> 8);
}

static void i_call_d16(void)
{
    uint16_t disp = FETCH_W();
    PushWord(ip);
    ip = ip + disp;
}

static void i_jmp_d16(void)
{
    uint16_t disp = FETCH_W();
    ip = ip + disp;
}

static void i_jmp_far(void)
{
    uint16_t nip = FETCH_W();
    uint16_t ncs = FETCH_W();

    sregs[CS] = ncs;
    ip = nip;
}

static void i_jmp_d8(void)
{
    int8_t disp = FETCH_B();
    ip = ip + disp;
}

static void i_inaldx(void)
{
    wregs[AX] = (wregs[AX] & 0xFF00) | read_port(wregs[DX]);
}

static void i_inaxdx(void)
{
    unsigned port = wregs[DX];
    wregs[AX] = read_port(port);
    wregs[AX] |= read_port(port + 1) << 8;
}

static void i_outdxal(void)
{
    write_port(wregs[DX], wregs[AX] & 0xFF);
}

static void i_outdxax(void)
{
    unsigned port = wregs[DX];
    write_port(port, wregs[AX] & 0xFF);
    write_port(port + 1, wregs[AX] >> 8);
}

static void rep(int flagval)
{
    /* Handles rep- and repnz- prefixes. flagval is the value of ZF for the
       loop  to continue for CMPS and SCAS instructions. */
    uint8_t next = FETCH_B();
    unsigned count = wregs[CX];

    switch(next)
    {
    case 0x26: /* ES: */
        segment_override = ES;
        rep(flagval);
        segment_override = NoSeg;
        break;
    case 0x2e: /* CS: */
        segment_override = CS;
        rep(flagval);
        segment_override = NoSeg;
        break;
    case 0x36: /* SS: */
        segment_override = SS;
        rep(flagval);
        segment_override = NoSeg;
        break;
    case 0x3e: /* DS: */
        segment_override = DS;
        rep(flagval);
        segment_override = NoSeg;
        break;
    case 0x6c: /* REP INSB */
        for(; count > 0; count--)
            i_insb();
        wregs[CX] = count;
        break;
    case 0x6d: /* REP INSW */
        for(; count > 0; count--)
            i_insw();
        wregs[CX] = count;
        break;
    case 0x6e: /* REP OUTSB */
        for(; count > 0; count--)
            i_outsb();
        wregs[CX] = count;
        break;
    case 0x6f: /* REP OUTSW */
        for(; count > 0; count--)
            i_outsw();
        wregs[CX] = count;
        break;
    case 0xa4: /* REP MOVSB */
        for(; count > 0; count--)
            i_movsb();
        wregs[CX] = count;
        break;
    case 0xa5: /* REP MOVSW */
        for(; count > 0; count--)
            i_movsw();
        wregs[CX] = count;
        break;
    case 0xa6: /* REP(N)E CMPSB */
        for(ZF = flagval; (ZF == flagval) && (count > 0); count--)
            i_cmpsb();
        wregs[CX] = count;
        break;
    case 0xa7: /* REP(N)E CMPSW */
        for(ZF = flagval; (ZF == flagval) && (count > 0); count--)
            i_cmpsw();
        wregs[CX] = count;
        break;
    case 0xaa: /* REP STOSB */
        for(; count > 0; count--)
            i_stosb();
        wregs[CX] = count;
        break;
    case 0xab: /* REP LODSW */
        for(; count > 0; count--)
            i_stosw();
        wregs[CX] = count;
        break;
    case 0xac: /* REP LODSB */
        for(; count > 0; count--)
            i_lodsb();
        wregs[CX] = count;
        break;
    case 0xad: /* REP LODSW */
        for(; count > 0; count--)
            i_lodsw();
        wregs[CX] = count;
        break;
    case 0xae: /* REP(N)E SCASB */
        for(ZF = flagval; (ZF == flagval) && (count > 0); count--)
            i_scasb();
        wregs[CX] = count;
        break;
    case 0xaf: /* REP(N)E SCASW */
        for(ZF = flagval; (ZF == flagval) && (count > 0); count--)
            i_scasw();
        wregs[CX] = count;
        break;
    default: /* Ignore REP */
        do_instruction(next);
    }
}

static void i_f6pre(void)
{
    int ModRM = FETCH_B();
    uint8_t dest = GetModRMRMB(ModRM);

    switch(ModRM & 0x38)
    {
    case 0x00: /* TEST Eb, data8 */
    case 0x08: /* ??? */
        dest &= FETCH_B();
        CF = OF = AF = 0;
        SetZFB(dest);
        SetSFB(dest);
        SetPF(dest);
        break;
    case 0x10: /* NOT Eb */
        SetModRMRMB(ModRM, ~dest);
        break;
    case 0x18: /* NEG Eb */
        dest = 0x100 - dest;
        CF = (dest != 0);
        OF = (dest == 0x80);
        AF = (dest ^ (0x100 - dest)) & 0x10;
        SetZFB(dest);
        SetSFB(dest);
        SetPF(dest);
        SetModRMRMB(ModRM, dest);
        break;
    case 0x20: /* MUL AL, Eb */
    {
        uint16_t result = dest * (wregs[AX] & 0xFF);

        wregs[AX] = result;
        SetSFB(result);
        SetPF(result);
        SetZFW(result);
        CF = OF = (result > 0xFF);
    }
    break;
    case 0x28: /* IMUL AL, Eb */
    {
        uint16_t result = (int8_t)dest * (int8_t)(wregs[AX] & 0xFF);

        wregs[AX] = result;
        SetSFB(result);
        SetPF(result);
        SetZFW(result);
        result &= 0xFF80;
        CF = OF = (result != 0) && (result != 0xFF80);
    }
    break;
    case 0x30: /* DIV AL, Ew */
    {
        if(dest && wregs[AX] / dest < 0x100)
            wregs[AX] = (wregs[AX] % dest) * 256 + (wregs[AX] / dest);
        else
            cpu_trap(0);
    }
    break;
    case 0x38: /* IDIV AL, Ew */
    {
        int16_t numer = wregs[AX];
        int16_t div;

        if(dest && (div = numer / (int8_t)dest) < 0x80 && div >= -0x80)
            wregs[AX] = (numer % (int8_t)dest) * 256 + (uint8_t)div;
        else
            cpu_trap(0);
    }
    break;
    }
}

static void i_f7pre(void)
{
    int ModRM = FETCH_B();
    uint16_t dest = GetModRMRMW(ModRM);

    switch(ModRM & 0x38)
    {
    case 0x00: /* TEST Ew, data16 */
    case 0x08: /* ??? */
        dest &= FETCH_W();
        CF = OF = AF = 0;
        SetZFW(dest);
        SetSFW(dest);
        SetPF(dest);
        break;

    case 0x10: /* NOT Ew */
        SetModRMRMW(ModRM, ~dest);
        break;

    case 0x18: /* NEG Ew */
        dest = 0x10000 - dest;
        CF = (dest != 0);
        OF = (dest == 0x8000);
        AF = (dest ^ (0x10000 - dest)) & 0x10;
        SetZFW(dest);
        SetSFW(dest);
        SetPF(dest);
        SetModRMRMW(ModRM, dest);
        break;
    case 0x20: /* MUL AX, Ew */
    {
        uint32_t result = dest * wregs[AX];

        wregs[AX] = result & 0xFFFF;
        wregs[DX] = result >> 16;

        SetSFW(result);
        SetPF(result);
        SetZFW(wregs[AX] | wregs[DX]);
        CF = OF = (result > 0xFFFF);
    }
    break;

    case 0x28: /* IMUL AX, Ew */
    {
        uint32_t result = (int16_t)dest * (int16_t)wregs[AX];
        wregs[AX] = result & 0xFFFF;
        wregs[DX] = result >> 16;
        SetSFW(result);
        SetPF(result);
        SetZFW(wregs[AX] | wregs[DX]);
        result &= 0xFFFF8000;
        CF = OF = (result != 0) && (result != 0xFFFF8000);
    }
    break;
    case 0x30: /* DIV AX, Ew */
    {
        uint32_t numer = (wregs[DX] << 16) + wregs[AX];
        if(dest && numer / dest < 0x10000)
        {
            wregs[AX] = numer / dest;
            wregs[DX] = numer % dest;
        }
        else
            cpu_trap(0);
    }
    break;
    case 0x38: /* IDIV AL, Ew */
    {
        int32_t numer = (wregs[DX] << 16) + wregs[AX];
        int32_t div;

        if(dest && (div = numer / (int16_t)dest) < 0x8000 && div >= -0x8000)
        {
            wregs[AX] = div;
            wregs[DX] = numer % (int16_t)dest;
        }
        else
            cpu_trap(0);
    }
    break;
    }
}

static void i_sti(void)
{
    IF = 1;
}

static void i_pusha(void)
{
    uint16_t tmp = wregs[SP];
    PushWord(wregs[AX]);
    PushWord(wregs[CX]);
    PushWord(wregs[DX]);
    PushWord(wregs[BX]);
    PushWord(tmp);
    PushWord(wregs[BP]);
    PushWord(wregs[SI]);
    PushWord(wregs[DI]);
}

static void i_popa(void)
{
    wregs[DI] = PopWord();
    wregs[SI] = PopWord();
    wregs[BP] = PopWord();
    PopWord();
    wregs[BX] = PopWord();
    wregs[DX] = PopWord();
    wregs[CX] = PopWord();
    wregs[AX] = PopWord();
}

static void i_bound(void)
{
    int ModRM = FETCH_B();
    uint16_t src = GetModRMRegW(ModRM);
    uint16_t low = GetModRMRMW(ModRM);
    uint16_t hi = GetMemAbsW(ModRMAddress + 2);
    if(src < low || src > hi)
        cpu_trap(5);
}

static void i_fepre(void)
{
    int ModRM = FETCH_B();
    uint8_t dest = GetModRMRMB(ModRM);

    if((ModRM & 0x38) == 0)
    {
        dest = dest + 1;
        OF = (dest == 0x80);
        AF = (dest ^ (dest - 1)) & 0x10;
    }
    else
    {
        dest--;
        OF = (dest == 0x7F);
        AF = (dest ^ (dest + 1)) & 0x10;
    }
    SetZFB(dest);
    SetSFB(dest);
    SetPF(dest);
    SetModRMRMB(ModRM, dest);
}

static void i_ffpre(void)
{
    int ModRM = FETCH_B();
    uint16_t dest = GetModRMRMW(ModRM);

    switch(ModRM & 0x38)
    {
    case 0x00: /* INC ew */
        dest = dest + 1;
        OF = (dest == 0x8000);
        AF = (dest ^ (dest - 1)) & 0x10;
        SetZFW(dest);
        SetSFW(dest);
        SetPF(dest);
        SetModRMRMW(ModRM, dest);
        break;
    case 0x08: /* DEC ew */
        dest = dest - 1;
        OF = (dest == 0x7FFF);
        AF = (dest ^ (dest + 1)) & 0x10;
        SetZFW(dest);
        SetSFW(dest);
        SetPF(dest);
        SetModRMRMW(ModRM, dest);
        break;
    case 0x10: /* CALL ew */
        PushWord(ip);
        ip = dest;
        break;
    case 0x18: /* CALL FAR ea */
        PushWord(sregs[CS]);
        PushWord(ip);
        ip = dest;
        sregs[CS] = GetMemAbsW(ModRMAddress + 2);
        break;
    case 0x20: /* JMP ea */
        ip = dest;
        break;
    case 0x28: /* JMP FAR ea */
        ip = dest;
        sregs[CS] = GetMemAbsW(ModRMAddress + 2);
        break;
    case 0x30: /* PUSH ea */
        PushWord(dest);
        break;
    case 0x38:
        i_undefined();
    }
}

static void i_enter(void)
{
    uint16_t stk = FETCH_W();
    uint8_t lvl = FETCH_B();
    PushWord(wregs[BP]);         // push BP
    wregs[BP] = wregs[SP];       // BP <- SP
    wregs[SP] = wregs[SP] - stk; // SP -= stk
    if(lvl)
    {
        unsigned i;
        unsigned tmp = wregs[BP];
        for(i = 1; i < lvl; i++)
            PushWord(GetMemW(SS, (tmp - i * 2))); // push SS:[BP - 2*i]
        PushWord(tmp);                            // push BP
    }
}

static void i_leave(void)
{
    wregs[SP] = wregs[BP]; // SP <- BP
    wregs[BP] = PopWord();
}

static void i_halt(void)
{
    printf("HALT instruction!\n");
    exit(0);
}

static void debug_instruction(void)
{
    unsigned nip = (cpuGetIP() + 0xFFFF) & 0xFFFF; // substract 1!
    const uint8_t *ip = memory + sregs[CS] * 16 + nip;

    debug(debug_cpu, "AX=%04X BX=%04X CX=%04X DX=%04X SP=%04X BP=%04X SI=%04X DI=%04X ",
          cpuGetAX(), cpuGetBX(), cpuGetCX(), cpuGetDX(), cpuGetSP(), cpuGetBP(),
          cpuGetSI(), cpuGetDI());
    debug(debug_cpu, "DS=%04X ES=%04X SS=%04X CS=%04X IP=%04X %s %s %s %s %s %s %s %s ",
          cpuGetDS(), cpuGetES(), cpuGetSS(), cpuGetCS(), nip, OF ? "OV" : "NV",
          DF ? "DN" : "UP", IF ? "EI" : "DI", SF ? "NG" : "PL", ZF ? "ZR" : "NZ",
          AF ? "AC" : "NA", PF ? "PE" : "PO", CF ? "CY" : "NC");
    debug(debug_cpu, "%04X:%04X %s\n", sregs[CS], nip, disa(ip, nip, segment_override));
}

static void do_instruction(uint8_t code)
{
    if(debug_active(debug_cpu) && segment_override == NoSeg)
        debug_instruction();
    switch(code)
    {
    case 0x00: OP_br8(ADD);
    case 0x01: OP_wr16(ADD);
    case 0x02: OP_r8b(ADD);
    case 0x03: OP_r16w(ADD);
    case 0x04: OP_ald8(ADD);
    case 0x05: OP_axd16(ADD);
    case 0x06: PushWord(sregs[ES]);                            break;
    case 0x07: sregs[ES] = PopWord();                          break;
    case 0x08: OP_br8(OR);
    case 0x09: OP_wr16(OR);
    case 0x0A: OP_r8b(OR);
    case 0x0B: OP_r16w(OR);
    case 0x0C: OP_ald8(OR);
    case 0x0D: OP_axd16(OR);
    case 0x0e: PushWord(sregs[CS]);                            break;
    case 0x0f: i_undefined();                                  break;
    case 0x10: OP_br8(ADC);
    case 0x11: OP_wr16(ADC);
    case 0x12: OP_r8b(ADC);
    case 0x13: OP_r16w(ADC);
    case 0x14: OP_ald8(ADC);
    case 0x15: OP_axd16(ADC);
    case 0x16: PushWord(sregs[SS]);                            break;
    case 0x17: sregs[SS] = PopWord();                          break;
    case 0x18: OP_br8(SBB);
    case 0x19: OP_wr16(SBB);
    case 0x1A: OP_r8b(SBB);
    case 0x1B: OP_r16w(SBB);
    case 0x1C: OP_ald8(SBB);
    case 0x1D: OP_axd16(SBB);
    case 0x1e: PushWord(sregs[DS]);                            break;
    case 0x1f: sregs[DS] = PopWord();                          break;
    case 0x20: OP_br8(AND);
    case 0x21: OP_wr16(AND);
    case 0x22: OP_r8b(AND);
    case 0x23: OP_r16w(AND);
    case 0x24: OP_ald8(AND);
    case 0x25: OP_axd16(AND);
    case 0x26: SEG_OVERRIDE(ES);
    case 0x27: i_daa();                                        break;
    case 0x28: OP_br8(SUB);
    case 0x29: OP_wr16(SUB);
    case 0x2A: OP_r8b(SUB);
    case 0x2B: OP_r16w(SUB);
    case 0x2C: OP_ald8(SUB);
    case 0x2D: OP_axd16(SUB);
    case 0x2E: SEG_OVERRIDE(CS);
    case 0x2f: i_das();                                        break;
    case 0x30: OP_br8(XOR);
    case 0x31: OP_wr16(XOR);
    case 0x32: OP_r8b(XOR);
    case 0x33: OP_r16w(XOR);
    case 0x34: OP_ald8(XOR);
    case 0x35: OP_axd16(XOR);
    case 0x36: SEG_OVERRIDE(SS);
    case 0x37: i_aaa();                                        break;
    case 0x38: OP_br8(CMP);
    case 0x39: OP_wr16(CMP);
    case 0x3A: OP_r8b(CMP);
    case 0x3B: OP_r16w(CMP);
    case 0x3C: OP_ald8(CMP);
    case 0x3D: OP_axd16(CMP);
    case 0x3E: SEG_OVERRIDE(DS);
    case 0x3f: i_aas();                                        break;
    case 0x40: INC_WR(AX);
    case 0x41: INC_WR(CX);
    case 0x42: INC_WR(DX);
    case 0x43: INC_WR(BX);
    case 0x44: INC_WR(SP);
    case 0x45: INC_WR(BP);
    case 0x46: INC_WR(SI);
    case 0x47: INC_WR(DI);
    case 0x48: DEC_WR(AX);
    case 0x49: DEC_WR(CX);
    case 0x4a: DEC_WR(DX);
    case 0x4b: DEC_WR(BX);
    case 0x4c: DEC_WR(SP);
    case 0x4d: DEC_WR(BP);
    case 0x4e: DEC_WR(SI);
    case 0x4f: DEC_WR(DI);
    case 0x50: PUSH_WR(AX);
    case 0x51: PUSH_WR(CX);
    case 0x52: PUSH_WR(DX);
    case 0x53: PUSH_WR(BX);
    case 0x54: PUSH_SP();
    case 0x55: PUSH_WR(BP);
    case 0x56: PUSH_WR(SI);
    case 0x57: PUSH_WR(DI);
    case 0x58: POP_WR(AX);
    case 0x59: POP_WR(CX);
    case 0x5a: POP_WR(DX);
    case 0x5b: POP_WR(BX);
    case 0x5c: POP_WR(SP);
    case 0x5d: POP_WR(BP);
    case 0x5e: POP_WR(SI);
    case 0x5f: POP_WR(DI);
    case 0x60: i_pusha();                                      break; /* 186 */
    case 0x61: i_popa();                                       break; /* 186 */
    case 0x62: i_bound();                                      break; /* 186 */
    case 0x63: i_undefined();                                  break;
    case 0x64: i_undefined();                                  break;
    case 0x65: i_undefined();                                  break;
    case 0x66: i_undefined();                                  break;
    case 0x67: i_undefined();                                  break;
    case 0x68: PushWord(FETCH_W());                            break; /* 186 */
    case 0x69: i_imul_r16w_d16();                              break; /* 186 */
    case 0x6a: PushWord((int8_t)FETCH_B());                    break; /* 186 */
    case 0x6b: i_imul_r16w_d8();                               break; /* 186 */
    case 0x6c: i_insb();                                       break; /* 186 */
    case 0x6d: i_insw();                                       break; /* 186 */
    case 0x6e: i_outsb();                                      break; /* 186 */
    case 0x6f: i_outsw();                                      break; /* 186 */
    case 0x70: do_cjump(OF);                                   break;
    case 0x71: do_cjump(!OF);                                  break;
    case 0x72: do_cjump(CF);                                   break;
    case 0x73: do_cjump(!CF);                                  break;
    case 0x74: do_cjump(ZF);                                   break;
    case 0x75: do_cjump(!ZF);                                  break;
    case 0x76: do_cjump(CF || ZF);                             break;
    case 0x77: do_cjump(!CF && !ZF);                           break;
    case 0x78: do_cjump(SF);                                   break;
    case 0x79: do_cjump(!SF);                                  break;
    case 0x7a: do_cjump(PF);                                   break;
    case 0x7b: do_cjump(!PF);                                  break;
    case 0x7c: do_cjump((!SF != !OF) && !ZF);                  break;
    case 0x7d: do_cjump((!SF == !OF) || ZF);                   break;
    case 0x7e: do_cjump((!SF != !OF) || ZF);                   break;
    case 0x7f: do_cjump((!SF == !OF) && !ZF);                  break;
    case 0x80: i_80pre();                                      break;
    case 0x81: i_81pre();                                      break;
    case 0x82: i_82pre();                                      break;
    case 0x83: i_83pre();                                      break;
    case 0x84: OP_br8(TEST);
    case 0x85: OP_wr16(TEST);
    case 0x86: i_xchg_br8();                                   break;
    case 0x87: i_xchg_wr16();                                  break;
    case 0x88: OP_br8(MOV);                                    break;
    case 0x89: OP_wr16(MOV);                                   break;
    case 0x8a: OP_r8b(MOV);                                    break;
    case 0x8b: OP_r16w(MOV);                                   break;
    case 0x8c: i_mov_wsreg();                                  break;
    case 0x8d: i_lea();                                        break;
    case 0x8e: i_mov_sregw();                                  break;
    case 0x8f: i_popw();                                       break;
    case 0x90: /* NOP */                                       break;
    case 0x91: XCHG_AX_WR(CX);
    case 0x92: XCHG_AX_WR(DX);
    case 0x93: XCHG_AX_WR(BX);
    case 0x94: XCHG_AX_WR(SP);
    case 0x95: XCHG_AX_WR(BP);
    case 0x96: XCHG_AX_WR(SI);
    case 0x97: XCHG_AX_WR(DI);
    case 0x98: wregs[AX] = (int8_t)(0xFF & wregs[AX]);         break;
    case 0x99: wregs[DX] = (wregs[AX] & 0x8000) ? 0xffff : 0;  break;
    case 0x9a: i_call_far();                                   break;
    case 0x9b: /* WAIT */                                      break;
    case 0x9c: PushWord(CompressFlags());                      break;
    case 0x9d: do_popf();                                      break;
    case 0x9e: i_sahf();                                       break;
    case 0x9f: i_lahf();                                       break;
    case 0xa0: i_mov_aldisp();                                 break;
    case 0xa1: i_mov_axdisp();                                 break;
    case 0xa2: i_mov_dispal();                                 break;
    case 0xa3: i_mov_dispax();                                 break;
    case 0xa4: i_movsb();                                      break;
    case 0xa5: i_movsw();                                      break;
    case 0xa6: i_cmpsb();                                      break;
    case 0xa7: i_cmpsw();                                      break;
    case 0xa8: OP_ald8(TEST);
    case 0xa9: OP_axd16(TEST);
    case 0xaa: i_stosb();                                      break;
    case 0xab: i_stosw();                                      break;
    case 0xac: i_lodsb();                                      break;
    case 0xad: i_lodsw();                                      break;
    case 0xae: i_scasb();                                      break;
    case 0xaf: i_scasw();                                      break;
    case 0xb0: MOV_BRL(AX);
    case 0xb1: MOV_BRL(CX);
    case 0xb2: MOV_BRL(DX);
    case 0xb3: MOV_BRL(BX);
    case 0xb4: MOV_BRH(AX);
    case 0xb5: MOV_BRH(CX);
    case 0xb6: MOV_BRH(DX);
    case 0xb7: MOV_BRH(BX);
    case 0xb8: MOV_WRi(AX);
    case 0xb9: MOV_WRi(CX);
    case 0xba: MOV_WRi(DX);
    case 0xbb: MOV_WRi(BX);
    case 0xbc: MOV_WRi(SP);
    case 0xbd: MOV_WRi(BP);
    case 0xbe: MOV_WRi(SI);
    case 0xbf: MOV_WRi(DI);
    case 0xc0: i_c0pre();                                      break; /* 186 */
    case 0xc1: i_c1pre();                                      break; /* 186 */
    case 0xc2: i_ret_d16();                                    break;
    case 0xc3: i_ret();                                        break;
    case 0xc4: i_les_dw();                                     break;
    case 0xc5: i_lds_dw();                                     break;
    case 0xc6: i_mov_bd8();                                    break;
    case 0xc7: i_mov_wd16();                                   break;
    case 0xc8: i_enter();                                      break;
    case 0xc9: i_leave();                                      break;
    case 0xca: i_retf_d16();                                   break;
    case 0xcb: do_retf();                                      break;
    case 0xcc: i_int3();                                       break;
    case 0xcd: i_int();                                        break;
    case 0xce: i_into();                                       break;
    case 0xcf: do_iret();                                      break;
    case 0xd0: i_d0pre();                                      break;
    case 0xd1: i_d1pre();                                      break;
    case 0xd2: i_d2pre();                                      break;
    case 0xd3: i_d3pre();                                      break;
    case 0xd4: i_aam();                                        break;
    case 0xd5: i_aad();                                        break;
    case 0xd6: i_undefined();                                  break;
    case 0xd7: i_xlat();                                       break;
    case 0xd8: i_escape();                                     break;
    case 0xd9: i_escape();                                     break;
    case 0xda: i_escape();                                     break;
    case 0xdb: i_escape();                                     break;
    case 0xdc: i_escape();                                     break;
    case 0xdd: i_escape();                                     break;
    case 0xde: i_escape();                                     break;
    case 0xdf: i_escape();                                     break;
    case 0xe0: i_loopne();                                     break;
    case 0xe1: i_loope();                                      break;
    case 0xe2: i_loop();                                       break;
    case 0xe3: i_jcxz();                                       break;
    case 0xe4: i_inal();                                       break;
    case 0xe5: i_inax();                                       break;
    case 0xe6: i_outal();                                      break;
    case 0xe7: i_outax();                                      break;
    case 0xe8: i_call_d16();                                   break;
    case 0xe9: i_jmp_d16();                                    break;
    case 0xea: i_jmp_far();                                    break;
    case 0xeb: i_jmp_d8();                                     break;
    case 0xec: i_inaldx();                                     break;
    case 0xed: i_inaxdx();                                     break;
    case 0xee: i_outdxal();                                    break;
    case 0xef: i_outdxax();                                    break;
    case 0xf0: /* LOCK */                                      break;
    case 0xf1: i_undefined();                                  break;
    case 0xf2: rep(0);                                         break;
    case 0xf3: rep(1);                                         break;
    case 0xf4: i_halt();                                       break;
    case 0xf5: CF = !CF;                                       break;
    case 0xf6: i_f6pre();                                      break;
    case 0xf7: i_f7pre();                                      break;
    case 0xf8: CF = 0;                                         break;
    case 0xf9: CF = 1;                                         break;
    case 0xfa: IF = 0;                                         break;
    case 0xfb: i_sti();                                        break;
    case 0xfc: DF = 0;                                         break;
    case 0xfd: DF = 1;                                         break;
    case 0xfe: i_fepre();                                      break;
    case 0xff: i_ffpre();                                      break;
    };
}

void execute(void)
{
    for(; !exit_cpu;)
    {
        handle_irq();
        next_instruction();
    }
}

// Set CPU registers from outside
void cpuSetAL(unsigned v) { wregs[AX] = (wregs[AX] & 0xFF00) | (v & 0xFF); }
void cpuSetAX(unsigned v) { wregs[AX] = v; }
void cpuSetCX(unsigned v) { wregs[CX] = v; }
void cpuSetDX(unsigned v) { wregs[DX] = v; }
void cpuSetBX(unsigned v) { wregs[BX] = v; }
void cpuSetSP(unsigned v) { wregs[SP] = v; }
void cpuSetBP(unsigned v) { wregs[BP] = v; }
void cpuSetSI(unsigned v) { wregs[SI] = v; }
void cpuSetDI(unsigned v) { wregs[DI] = v; }
void cpuSetES(unsigned v) { sregs[ES] = v; }
void cpuSetCS(unsigned v) { sregs[CS] = v; }
void cpuSetSS(unsigned v) { sregs[SS] = v; }
void cpuSetDS(unsigned v) { sregs[DS] = v; }
void cpuSetIP(unsigned v) { ip = v; }

// Get CPU registers from outside
unsigned cpuGetAX() { return wregs[AX]; }
unsigned cpuGetCX() { return wregs[CX]; }
unsigned cpuGetDX() { return wregs[DX]; }
unsigned cpuGetBX() { return wregs[BX]; }
unsigned cpuGetSP() { return wregs[SP]; }
unsigned cpuGetBP() { return wregs[BP]; }
unsigned cpuGetSI() { return wregs[SI]; }
unsigned cpuGetDI() { return wregs[DI]; }
unsigned cpuGetES() { return sregs[ES]; }
unsigned cpuGetCS() { return sregs[CS]; }
unsigned cpuGetSS() { return sregs[SS]; }
unsigned cpuGetDS() { return sregs[DS]; }
unsigned cpuGetIP() { return ip; }

// Address of flags in stack when in interrupt handler
static uint8_t *flagAddr(void)
{
    return memory + (0xFFFFF & (4 + cpuGetSS() * 16 + cpuGetSP()));
}

// Set flags in the stack
void cpuSetFlag(enum cpuFlags flag)
{
    uint8_t *f = flagAddr();
    f[0] |= flag;
    f[1] |= (flag >> 8);
}

// Get flags in the stack
void cpuClrFlag(enum cpuFlags flag)
{
    uint8_t *f = flagAddr();
    f[0] &= ~flag;
    f[1] &= ((~flag) >> 8);
}

void cpuSetStartupFlag(enum cpuFlags flag)
{
    ExpandFlags(CompressFlags() | flag);
}

void cpuClrStartupFlag(enum cpuFlags flag)
{
    ExpandFlags(CompressFlags() & ~flag);
}

int cpuGetAddress(uint16_t segment, uint16_t offset)
{
    return 0xFFFFF & (segment * 16 + offset);
}

int cpuGetAddrDS(uint16_t offset)
{
    return 0xFFFFF & (sregs[DS] * 16 + offset);
}

int cpuGetAddrES(uint16_t offset)
{
    return 0xFFFFF & (sregs[ES] * 16 + offset);
}

int cpuGetAddrSS(uint16_t offset)
{
    return 0xFFFFF & (sregs[ES] * 16 + offset);
}

uint16_t cpuGetStack(uint16_t disp)
{
    return GetMemW(SS, wregs[SP] + disp);
}

void cpuTriggerIRQ(int num)
{
    irq_mask |= (1 << num);
}
