
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dis.h"
#include "emu.h"

static char buf[128];
#define IPOS (buf + 17)
#define EPOS (buf + 127)

static const char *byte_reg[] = {"AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH"};
static const char *word_reg[] = {"AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"};
static const char *seg_reg[] = {"ES", "CS", "SS", "DS"};
static const char *index_reg[] = {"BX+SI", "BX+DI", "BP+SI", "BP+DI",
                                  "SI",    "DI",    "BP",    "BX"};
static const char *table_dx[] = {"ROL", "ROR", "RCL", "RCR", "SHL", "SHR", "SHL", "SAR"};
static const char *table_f6[] = {"TEST", "ILL",  "NOT", "NEG",
                                 "MUL",  "IMUL", "DIV", "IDIV"};
static const char *table_fe[] = {"INC", "DEC", "ILL", "ILL", "ILL", "ILL", "ILL", "ILL"};
static const char *table_ff[] = {"INC", "DEC", "CALL", "CALL",
                                 "JMP", "JMP", "PUSH", "ILL"};
static const char *table_8x[] = {"ADD", "OR", "ADC", "SBB", "AND", "SUB", "XOR", "CMP"};

#define BREG byte_reg[(ModRM & 0x38) >> 3]
#define WREG word_reg[(ModRM & 0x38) >> 3]
#define SREG seg_reg[(ModRM & 0x18) >> 3]
#define IXREG index_reg[ModRM & 0x07]

static void fillbytes(const uint8_t *ip, int num)
{
    for(int i = 0; i < num; i++)
        sprintf(buf + 2 * i, "%02X", ip[i]);
    memset(buf + 2 * num, ' ', IPOS - buf - 2 * num);
    memset(IPOS, 0, EPOS - IPOS);
}

static const char *seg_names[] = {"ES:", "CS:", "SS:", "DS:", ""};

static char *get_mem(unsigned ModRM, const uint8_t *ip, const char *rg[],
                     const char *cast, int seg_over)
{
    static char buffer[100];
    unsigned num;
    char ch;
    switch(ModRM & 0xc0)
    {
    case 0x00:
        if((ModRM & 0x07) != 6)
            sprintf(buffer, "%s%s[%s]", cast, seg_names[seg_over], IXREG);
        else
            sprintf(buffer, "%s%s[%02X%02X]", cast, seg_names[seg_over], ip[2], ip[1]);
        break;
    case 0x40:
        if((num = ip[1]) > 127)
        {
            ch = '-';
            num = 256 - num;
        }
        else
            ch = '+';
        sprintf(buffer, "%s%s[%s%c%02X]", cast, seg_names[seg_over], IXREG, ch, num);
        break;
    case 0x80:
        if((num = (ip[2] * 256 + ip[1])) > 0x7fff)
        {
            ch = '-';
            num = 0x10000 - num;
        }
        else
            ch = '+';
        sprintf(buffer, "%s%s[%s%c%04X]", cast, seg_names[seg_over], IXREG, ch, num);
        break;
    case 0xc0:
        strcpy(buffer, rg[ModRM & 7]);
        break;
    }
    return buffer;
}

static int get_mem_len(unsigned ModRM)
{
    switch(ModRM & 0xc0)
    {
    case 0x00:
        return ((ModRM & 0x07) == 6) ? 2 : 0;
    case 0x40:
        return 1;
    case 0x80:
        return 2;
    default:
        return 0;
    }
}

static const char *decode_pushpopseg(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 1);
    sprintf(IPOS, "%-7s %s", ins, seg_reg[(*ip & 0x38) >> 3]);
    return buf;
}

static const char *decode_wordregx(const uint8_t *ip, const char *ins, const char *pre)
{
    fillbytes(ip, 1);
    sprintf(IPOS, "%-7s %s%s", ins, pre, word_reg[*ip & 7]);
    return buf;
}

static const char *decode_wordreg(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 1);
    sprintf(IPOS, "%-7s %s", ins, word_reg[*ip & 7]);
    return buf;
}

static const char *decode_jump(const uint8_t *ip, const char *ins, uint16_t reg_ip)
{
    reg_ip += 3 + ip[2] * 256 + ip[1];
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s %04X", ins, reg_ip);
    return buf;
}

static const char *decode_jump8(const uint8_t *ip, const char *ins, uint16_t reg_ip)
{
    if(ip[1] < 0x80)
        reg_ip += 2 + ip[1];
    else
        reg_ip += 2 + ip[1] + 0xFF00;
    fillbytes(ip, 2);
    sprintf(IPOS, "%-7s %04X", ins, reg_ip);
    return buf;
}

static const char *decode_far(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 5);
    sprintf(IPOS, "%-7s %04X:%04X", ins, ip[4] * 256U + ip[3], ip[2] * 256U + ip[1]);
    return buf;
}

static const char *decode_far_ind(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s FAR %s", ins, get_mem(ModRM, ip + 1, word_reg, "", seg_over));
    return buf;
}

static const char *decode_memal(const uint8_t *ip, const char *ins, int seg_over)
{
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s %s[%02X%02X],AL", ins, seg_names[seg_over], ip[2], ip[1]);
    return buf;
}

static const char *decode_memax(const uint8_t *ip, const char *ins, int seg_over)
{
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s %s[%02X%02X],AX", ins, seg_names[seg_over], ip[2], ip[1]);
    return buf;
}

static const char *decode_almem(const uint8_t *ip, const char *ins, int seg_over)
{
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s AL,%s[%02X%02X]", ins, seg_names[seg_over], ip[2], ip[1]);
    return buf;
}

static const char *decode_axmem(const uint8_t *ip, const char *ins, int seg_over)
{
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s AX,%s[%02X%02X]", ins, seg_names[seg_over], ip[2], ip[1]);
    return buf;
}

static const char *decode_rd8(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 2);
    sprintf(IPOS, "%-7s %s,%02X", ins, byte_reg[*ip & 0x7], ip[1]);
    return buf;
}

static const char *decode_ald8(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 2);
    sprintf(IPOS, "%-7s AL,%02X", ins, ip[1]);
    return buf;
}

static const char *decode_d8al(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 2);
    sprintf(IPOS, "%-7s %02X,AL", ins, ip[1]);
    return buf;
}

static const char *decode_axd8(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 2);
    sprintf(IPOS, "%-7s AX,%02X", ins, ip[1]);
    return buf;
}

static const char *decode_d8ax(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 2);
    sprintf(IPOS, "%-7s %02X,AX", ins, ip[1]);
    return buf;
}

static const char *decode_enter(const uint8_t *ip)
{
    fillbytes(ip, 4);
    sprintf(IPOS, "ENTER   %02X%02X,%02X", ip[2], ip[1], ip[3]);
    return buf;
}

static const char *decode_databyte(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 1);
    sprintf(IPOS, "%-7s %02X", ins, ip[0]);
    return buf;
}

static const char *decode_adjust(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 2);
    if(ip[1] == 10)
        sprintf(IPOS, "%-7s", ins);
    else
        sprintf(IPOS, "%-7s %02X", ins, ip[1]);
    return buf;
}

static const char *decode_d8(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 2);
    sprintf(IPOS, "%-7s %02X", ins, ip[1]);
    return buf;
}

static const char *decode_d16(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s %02X%02X", ins, ip[2], ip[1]);
    return buf;
}

static const char *decode_axd16(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s AX,%02X%02X", ins, ip[2], ip[1]);
    return buf;
}

static const char *decode_rd16(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 3);
    sprintf(IPOS, "%-7s %s,%02X%02X", ins, word_reg[*ip & 0x7], ip[2], ip[1]);
    return buf;
}

static const char *decode_br8(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s", ins, get_mem(ModRM, ip + 1, byte_reg, "", seg_over),
            BREG);
    return buf;
}

static const char *decode_r8b(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s", ins, BREG,
            get_mem(ModRM, ip + 1, byte_reg, "", seg_over));
    return buf;
}

static const char *decode_bd8(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    int ln = get_mem_len(ModRM);
    fillbytes(ip, 3 + ln);
    sprintf(IPOS, "%-7s %s,%02X", ins,
            get_mem(ModRM, ip + 1, byte_reg, "BYTE PTR ", seg_over), ip[ln + 2]);
    return buf;
}

static const char *decode_b(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s", ins,
            get_mem(ModRM, ip + 1, byte_reg, "BYTE PTR ", seg_over));
    return buf;
}

static const char *decode_ws(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s", ins, get_mem(ModRM, ip + 1, word_reg, "", seg_over),
            SREG);
    return buf;
}

static const char *decode_sw(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s", ins, SREG,
            get_mem(ModRM, ip + 1, word_reg, "", seg_over));
    return buf;
}

static const char *decode_w(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s", ins,
            get_mem(ModRM, ip + 1, word_reg, "WORD PTR ", seg_over));
    return buf;
}

static const char *decode_wr16(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s", ins, get_mem(ModRM, ip + 1, word_reg, "", seg_over),
            WREG);
    return buf;
}

static const char *decode_r16w(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s", ins, WREG,
            get_mem(ModRM, ip + 1, word_reg, "", seg_over));
    return buf;
}

static const char *decode_wd16(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    int ln = get_mem_len(ModRM);
    fillbytes(ip, 4 + ln);
    sprintf(IPOS, "%-7s %s,%02X%02X", ins,
            get_mem(ModRM, ip + 1, word_reg, "WORD PTR ", seg_over), ip[ln + 3],
            ip[ln + 2]);
    return buf;
}

static const char *decode_wd8(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    int ln = get_mem_len(ModRM);
    fillbytes(ip, 3 + ln);
    sprintf(IPOS, "%-7s %s,%02X", ins,
            get_mem(ModRM, ip + 1, word_reg, "WORD PTR ", seg_over), ip[ln + 2]);
    return buf;
}

static const char *decode_imul_b(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    uint8_t d1 = ip[2 + get_mem_len(ModRM)];
    fillbytes(ip, 3 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s,%c%02X", ins, WREG,
            get_mem(ModRM, ip + 1, word_reg, "", seg_over), d1 > 0x7F ? '-' : '+',
            d1 > 0x7F ? 0x100U - d1 : d1);
    return buf;
}

static const char *decode_imul_w(const uint8_t *ip, const char *ins, int seg_over)
{
    unsigned ModRM = ip[1];
    uint8_t d1 = ip[2 + get_mem_len(ModRM)];
    uint8_t d2 = ip[3 + get_mem_len(ModRM)];
    fillbytes(ip, 4 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,%s,%02X%02X", ins, WREG,
            get_mem(ModRM, ip + 1, word_reg, "", seg_over), d2, d1);
    return buf;
}

static const char *decode_bbitd8(const uint8_t *ip, int seg_over)
{
    unsigned ModRM = ip[1];
    int ln = get_mem_len(ModRM);
    fillbytes(ip, 3 + ln);
    sprintf(IPOS, "%-7s %s,%2x", table_dx[(ModRM & 0x38) >> 3],
            get_mem(ModRM, ip + 1, byte_reg, "BYTE PTR ", seg_over), ip[ln + 2]);
    return buf;
}

static const char *decode_wbitd8(const uint8_t *ip, int seg_over)
{
    unsigned ModRM = ip[1];
    int ln = get_mem_len(ModRM);
    fillbytes(ip, 3 + ln);
    sprintf(IPOS, "%-7s %s,%02x", table_dx[(ModRM & 0x38) >> 3],
            get_mem(ModRM, ip + 1, word_reg, "WORD PTR ", seg_over), ip[ln + 2]);
    return buf;
}

static const char *decode_bbit1(const uint8_t *ip, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,1", table_dx[(ModRM & 0x38) >> 3],
            get_mem(ModRM, ip + 1, byte_reg, "BYTE PTR ", seg_over));
    return buf;
}

static const char *decode_wbit1(const uint8_t *ip, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,1", table_dx[(ModRM & 0x38) >> 3],
            get_mem(ModRM, ip + 1, word_reg, "WORD PTR ", seg_over));
    return buf;
}

static const char *decode_bbitcl(const uint8_t *ip, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,CL", table_dx[(ModRM & 0x38) >> 3],
            get_mem(ModRM, ip + 1, byte_reg, "BYTE PTR ", seg_over));
    return buf;
}

static const char *decode_wbitcl(const uint8_t *ip, int seg_over)
{
    unsigned ModRM = ip[1];
    fillbytes(ip, 2 + get_mem_len(ModRM));
    sprintf(IPOS, "%-7s %s,CL", table_dx[(ModRM & 0x38) >> 3],
            get_mem(ModRM, ip + 1, word_reg, "WORD PTR ", seg_over));
    return buf;
}

static const char *decode_f6(const uint8_t *ip, int seg_over)
{
    int m = (ip[1] & 0x38) >> 3;
    if(m != 0)
        return decode_b(ip, table_f6[m], seg_over);
    else
        return decode_bd8(ip, table_f6[m], seg_over);
}

static const char *decode_f7(const uint8_t *ip, int seg_over)
{
    int m = (ip[1] & 0x38) >> 3;
    if(m != 0)
        return decode_w(ip, table_f6[m], seg_over);
    else
        return decode_wd16(ip, table_f6[m], seg_over);
}

static const char *decode_ff(const uint8_t *ip, int seg_over)
{
    int m = (ip[1] & 0x38) >> 3;
    if(m == 3 || m == 5)
        return decode_far_ind(ip, table_ff[m], seg_over);
    else
        return decode_w(ip, table_ff[m], seg_over);
}

static const char *show_io(const uint8_t *ip, const char *ins, const char *regs)
{
    fillbytes(ip, 1);
    strcpy(IPOS, ins);
    strcpy(IPOS + 8, regs);
    return buf;
}

static const char *show(const uint8_t *ip, const char *ins)
{
    fillbytes(ip, 1);
    strcpy(IPOS, ins);
    return buf;
}

static const char *show_str(const uint8_t *ip, const char *ins, int segment_override)
{
    fillbytes(ip, 1);
    int ln = strlen(seg_names[segment_override]);
    strcpy(IPOS, seg_names[segment_override]);
    strcpy(IPOS + ln, ins);
    return buf;
}

static const char *show_seg(const uint8_t *ip, uint8_t reg_ip, int seg_over)
{
    const char hx[] = "0123456789ABCDEF";
    // Call recursive
    disa(ip + 1, reg_ip + 1, seg_over);

    // Patch bytes!
    memmove(buf + 2, buf, IPOS - buf - 2);
    buf[0] = hx[*ip >> 4];
    buf[1] = hx[*ip & 0xF];

    // Patch ins if does not already has segment name:
    const char *sn = seg_names[seg_over];
    if(strstr(IPOS, sn) == 0)
    {
        unsigned s = strlen(sn) + 1;
        memmove(IPOS + s, IPOS, EPOS - IPOS - s);
        memcpy(IPOS, sn, s);
        IPOS[s - 1] = ' ';
    }
    return buf;
}

static const char *show_rep(const uint8_t *ip, const char *ins, uint16_t reg_ip,
                            int seg_over)
{
    // Call recursive (only if not REP*)
    if(ip[1] == 0xF2 || ip[1] == 0xF3)
        return show(ip, ins);

    disa(ip + 1, reg_ip + 1, seg_over);

    // Patch bytes!
    memmove(buf + 2, buf, IPOS - buf - 2);
    buf[0] = 'F';
    buf[1] = '0' + (*ip & 0x0F);

    // Patch ins!
    unsigned s = strlen(ins) + 1;
    memmove(IPOS + s, IPOS, EPOS - IPOS - s);
    memcpy(IPOS, ins, s);
    IPOS[s - 1] = ' ';
    return buf;
}

static const char *show_int(const uint8_t num)
{
    memset(buf, '?', 2);
    memset(buf + 2, ' ', IPOS - buf - 2);
    memset(IPOS, 0, EPOS - IPOS);
    sprintf(IPOS, "IRET    (EMU %02X)", num);
    return buf;
}

enum segments
{
    ES = 0,
    CS,
    SS,
    DS,
    NoSeg
};

// Show ins disassembly
const char *disa(const uint8_t *ip, uint16_t reg_ip, int segment_override)
{
    if(cpuGetCS() == 0 && (ip - memory) < 0x100)
        return show_int(ip - memory);
    switch(*ip)
    {
    case 0x00: return decode_br8(ip, "ADD", segment_override);
    case 0x01: return decode_wr16(ip, "ADD", segment_override);
    case 0x02: return decode_r8b(ip, "ADD", segment_override);
    case 0x03: return decode_r16w(ip, "ADD", segment_override);
    case 0x04: return decode_ald8(ip, "ADD");
    case 0x05: return decode_axd16(ip, "ADD");
    case 0x06: return decode_pushpopseg(ip, "PUSH");
    case 0x07: return decode_pushpopseg(ip, "POP");
    case 0x08: return decode_br8(ip, "OR", segment_override);
    case 0x09: return decode_wr16(ip, "OR", segment_override);
    case 0x0a: return decode_r8b(ip, "OR", segment_override);
    case 0x0b: return decode_r16w(ip, "OR", segment_override);
    case 0x0c: return decode_ald8(ip, "OR");
    case 0x0d: return decode_axd16(ip, "OR");
    case 0x0e: return decode_pushpopseg(ip, "PUSH");
    case 0x0f: return decode_databyte(ip, "DB");
    case 0x10: return decode_br8(ip, "ADC", segment_override);
    case 0x11: return decode_wr16(ip, "ADC", segment_override);
    case 0x12: return decode_r8b(ip, "ADC", segment_override);
    case 0x13: return decode_r16w(ip, "ADC", segment_override);
    case 0x14: return decode_ald8(ip, "ADC");
    case 0x15: return decode_axd16(ip, "ADC");
    case 0x16: return decode_pushpopseg(ip, "PUSH");
    case 0x17: return decode_pushpopseg(ip, "POP");
    case 0x18: return decode_br8(ip, "SBB", segment_override);
    case 0x19: return decode_wr16(ip, "SBB", segment_override);
    case 0x1a: return decode_r8b(ip, "SBB", segment_override);
    case 0x1b: return decode_r16w(ip, "SBB", segment_override);
    case 0x1c: return decode_ald8(ip, "SBB");
    case 0x1d: return decode_axd16(ip, "SBB");
    case 0x1e: return decode_pushpopseg(ip, "PUSH");
    case 0x1f: return decode_pushpopseg(ip, "POP");
    case 0x20: return decode_br8(ip, "AND", segment_override);
    case 0x21: return decode_wr16(ip, "AND", segment_override);
    case 0x22: return decode_r8b(ip, "AND", segment_override);
    case 0x23: return decode_r16w(ip, "AND", segment_override);
    case 0x24: return decode_ald8(ip, "AND");
    case 0x25: return decode_axd16(ip, "AND");
    case 0x26: return show_seg(ip, reg_ip, ES);
    case 0x27: return show(ip, "DAA");
    case 0x28: return decode_br8(ip, "SUB", segment_override);
    case 0x29: return decode_wr16(ip, "SUB", segment_override);
    case 0x2a: return decode_r8b(ip, "SUB", segment_override);
    case 0x2b: return decode_r16w(ip, "SUB", segment_override);
    case 0x2c: return decode_ald8(ip, "SUB");
    case 0x2d: return decode_axd16(ip, "SUB");
    case 0x2e: return show_seg(ip, reg_ip, CS);
    case 0x2f: return show(ip, "DAS");
    case 0x30: return decode_br8(ip, "XOR", segment_override);
    case 0x31: return decode_wr16(ip, "XOR", segment_override);
    case 0x32: return decode_r8b(ip, "XOR", segment_override);
    case 0x33: return decode_r16w(ip, "XOR", segment_override);
    case 0x34: return decode_ald8(ip, "XOR");
    case 0x35: return decode_axd16(ip, "XOR");
    case 0x36: return show_seg(ip, reg_ip, SS);
    case 0x37: return show(ip, "AAA");
    case 0x38: return decode_br8(ip, "CMP", segment_override);
    case 0x39: return decode_wr16(ip, "CMP", segment_override);
    case 0x3a: return decode_r8b(ip, "CMP", segment_override);
    case 0x3b: return decode_r16w(ip, "CMP", segment_override);
    case 0x3c: return decode_ald8(ip, "CMP");
    case 0x3d: return decode_axd16(ip, "CMP");
    case 0x3e: return show_seg(ip, reg_ip, DS);
    case 0x3f: return show(ip, "AAS");
    case 0x40: return decode_wordreg(ip, "INC");
    case 0x41: return decode_wordreg(ip, "INC");
    case 0x42: return decode_wordreg(ip, "INC");
    case 0x43: return decode_wordreg(ip, "INC");
    case 0x44: return decode_wordreg(ip, "INC");
    case 0x45: return decode_wordreg(ip, "INC");
    case 0x46: return decode_wordreg(ip, "INC");
    case 0x47: return decode_wordreg(ip, "INC");
    case 0x48: return decode_wordreg(ip, "DEC");
    case 0x49: return decode_wordreg(ip, "DEC");
    case 0x4a: return decode_wordreg(ip, "DEC");
    case 0x4b: return decode_wordreg(ip, "DEC");
    case 0x4c: return decode_wordreg(ip, "DEC");
    case 0x4d: return decode_wordreg(ip, "DEC");
    case 0x4e: return decode_wordreg(ip, "DEC");
    case 0x4f: return decode_wordreg(ip, "DEC");
    case 0x50: return decode_wordreg(ip, "PUSH");
    case 0x51: return decode_wordreg(ip, "PUSH");
    case 0x52: return decode_wordreg(ip, "PUSH");
    case 0x53: return decode_wordreg(ip, "PUSH");
    case 0x54: return decode_wordreg(ip, "PUSH");
    case 0x55: return decode_wordreg(ip, "PUSH");
    case 0x56: return decode_wordreg(ip, "PUSH");
    case 0x57: return decode_wordreg(ip, "PUSH");
    case 0x58: return decode_wordreg(ip, "POP");
    case 0x59: return decode_wordreg(ip, "POP");
    case 0x5a: return decode_wordreg(ip, "POP");
    case 0x5b: return decode_wordreg(ip, "POP");
    case 0x5c: return decode_wordreg(ip, "POP");
    case 0x5d: return decode_wordreg(ip, "POP");
    case 0x5e: return decode_wordreg(ip, "POP");
    case 0x5f: return decode_wordreg(ip, "POP");
    case 0x60: return show(ip, "PUSHA");
    case 0x61: return show(ip, "POPA");
    case 0x62: return decode_w(ip, "BOUND", segment_override);
    case 0x63: return decode_databyte(ip, "DB");
    case 0x64: return decode_databyte(ip, "DB");
    case 0x65: return decode_databyte(ip, "DB");
    case 0x66: return decode_databyte(ip, "DB");
    case 0x67: return decode_databyte(ip, "DB");
    case 0x68: return decode_d16(ip, "PUSH");
    case 0x69: return decode_imul_w(ip, "IMUL", segment_override);
    case 0x6a: return decode_d8(ip, "PUSH");
    case 0x6b: return decode_imul_b(ip, "IMUL", segment_override);
    case 0x6c: return show(ip, "INSB");
    case 0x6d: return show(ip, "INSW");
    case 0x6e: return show_str(ip, "OUTSB", segment_override);
    case 0x6f: return show_str(ip, "OUTSW", segment_override);
    case 0x70: return decode_jump8(ip, "JO", reg_ip);
    case 0x71: return decode_jump8(ip, "JNO", reg_ip);
    case 0x72: return decode_jump8(ip, "JB", reg_ip);
    case 0x73: return decode_jump8(ip, "JAE", reg_ip);
    case 0x74: return decode_jump8(ip, "JZ", reg_ip);
    case 0x75: return decode_jump8(ip, "JNZ", reg_ip);
    case 0x76: return decode_jump8(ip, "JBE", reg_ip);
    case 0x77: return decode_jump8(ip, "JA", reg_ip);
    case 0x78: return decode_jump8(ip, "JS", reg_ip);
    case 0x79: return decode_jump8(ip, "JNS", reg_ip);
    case 0x7a: return decode_jump8(ip, "JP", reg_ip);
    case 0x7b: return decode_jump8(ip, "JNP", reg_ip);
    case 0x7c: return decode_jump8(ip, "JL", reg_ip);
    case 0x7d: return decode_jump8(ip, "JGE", reg_ip);
    case 0x7e: return decode_jump8(ip, "JLE", reg_ip);
    case 0x7f: return decode_jump8(ip, "JG", reg_ip);
    case 0x80: return decode_bd8(ip, table_8x[(ip[1] & 0x38) >> 3], segment_override);
    case 0x81: return decode_wd16(ip, table_8x[(ip[1] & 0x38) >> 3], segment_override);
    case 0x82: return decode_bd8(ip, table_8x[(ip[1] & 0x38) >> 3], segment_override);
    case 0x83: return decode_wd8(ip, table_8x[(ip[1] & 0x38) >> 3], segment_override);
    case 0x84: return decode_br8(ip, "TEST", segment_override);
    case 0x85: return decode_wr16(ip, "TEST", segment_override);
    case 0x86: return decode_br8(ip, "XCHG", segment_override);
    case 0x87: return decode_wr16(ip, "XCHG", segment_override);
    case 0x88: return decode_br8(ip, "MOV", segment_override);
    case 0x89: return decode_wr16(ip, "MOV", segment_override);
    case 0x8a: return decode_r8b(ip, "MOV", segment_override);
    case 0x8b: return decode_r16w(ip, "MOV", segment_override);
    case 0x8c: return decode_ws(ip, "MOV", segment_override);
    case 0x8d: return decode_r16w(ip, "LEA", segment_override);
    case 0x8e: return decode_sw(ip, "MOV", segment_override);
    case 0x8f: return decode_w(ip, "POP", segment_override);
    case 0x90: return show(ip, "NOP");
    case 0x91: return decode_wordregx(ip, "XCHG", "AX,");
    case 0x92: return decode_wordregx(ip, "XCHG", "AX,");
    case 0x93: return decode_wordregx(ip, "XCHG", "AX,");
    case 0x94: return decode_wordregx(ip, "XCHG", "AX,");
    case 0x95: return decode_wordregx(ip, "XCHG", "AX,");
    case 0x96: return decode_wordregx(ip, "XCHG", "AX,");
    case 0x97: return decode_wordregx(ip, "XCHG", "AX,");
    case 0x98: return show(ip, "CBW");
    case 0x99: return show(ip, "CWD");
    case 0x9a: return decode_far(ip, "CALL");
    case 0x9b: return show(ip, "WAIT");
    case 0x9c: return show(ip, "PUSHF");
    case 0x9d: return show(ip, "POPF");
    case 0x9e: return show(ip, "SAHF");
    case 0x9f: return show(ip, "LAHF");
    case 0xa0: return decode_almem(ip, "MOV", segment_override);
    case 0xa1: return decode_axmem(ip, "MOV", segment_override);
    case 0xa2: return decode_memal(ip, "MOV", segment_override);
    case 0xa3: return decode_memax(ip, "MOV", segment_override);
    case 0xa4: return show_str(ip, "MOVSB", segment_override);
    case 0xa5: return show_str(ip, "MOVSW", segment_override);
    case 0xa6: return show_str(ip, "CMPSB", segment_override);
    case 0xa7: return show_str(ip, "CMPSW", segment_override);
    case 0xa8: return decode_ald8(ip, "TEST");
    case 0xa9: return decode_axd16(ip, "TEST");
    case 0xaa: return show(ip, "STOSB");
    case 0xab: return show(ip, "STOSW");
    case 0xac: return show_str(ip, "LODSB", segment_override);
    case 0xad: return show_str(ip, "LODSW", segment_override);
    case 0xae: return show(ip, "SCASB");
    case 0xaf: return show(ip, "SCASW");
    case 0xb0: return decode_rd8(ip, "MOV");
    case 0xb1: return decode_rd8(ip, "MOV");
    case 0xb2: return decode_rd8(ip, "MOV");
    case 0xb3: return decode_rd8(ip, "MOV");
    case 0xb4: return decode_rd8(ip, "MOV");
    case 0xb5: return decode_rd8(ip, "MOV");
    case 0xb6: return decode_rd8(ip, "MOV");
    case 0xb7: return decode_rd8(ip, "MOV");
    case 0xb8: return decode_rd16(ip, "MOV");
    case 0xb9: return decode_rd16(ip, "MOV");
    case 0xba: return decode_rd16(ip, "MOV");
    case 0xbb: return decode_rd16(ip, "MOV");
    case 0xbc: return decode_rd16(ip, "MOV");
    case 0xbd: return decode_rd16(ip, "MOV");
    case 0xbe: return decode_rd16(ip, "MOV");
    case 0xbf: return decode_rd16(ip, "MOV");
    case 0xc0: return decode_bbitd8(ip, segment_override);
    case 0xc1: return decode_wbitd8(ip, segment_override);
    case 0xc2: return decode_d16(ip, "RET");
    case 0xc3: return show(ip, "RET");
    case 0xc4: return decode_r16w(ip, "LES", segment_override);
    case 0xc5: return decode_r16w(ip, "LDS", segment_override);
    case 0xc6: return decode_bd8(ip, "MOV", segment_override);
    case 0xc7: return decode_wd16(ip, "MOV", segment_override);
    case 0xc8: return decode_enter(ip);
    case 0xc9: return show(ip, "LEAVE");
    case 0xca: return decode_d16(ip, "RETF");
    case 0xcb: return show(ip, "RETF");
    case 0xcc: return show(ip, "INT 3");
    case 0xcd: return decode_d8(ip, "INT");
    case 0xce: return show(ip, "INTO");
    case 0xcf: return show(ip, "IRET");
    case 0xd0: return decode_bbit1(ip, segment_override);
    case 0xd1: return decode_wbit1(ip, segment_override);
    case 0xd2: return decode_bbitcl(ip, segment_override);
    case 0xd3: return decode_wbitcl(ip, segment_override);
    case 0xd4: return decode_adjust(ip, "AAM");
    case 0xd5: return decode_adjust(ip, "AAD");
    case 0xd6: return decode_databyte(ip, "DB");
    case 0xd7: return show(ip, "XLAT");
    case 0xd8: return show(ip, "ESC");
    case 0xd9: return show(ip, "ESC");
    case 0xda: return show(ip, "ESC");
    case 0xdb: return show(ip, "ESC");
    case 0xdc: return show(ip, "ESC");
    case 0xdd: return show(ip, "ESC");
    case 0xde: return show(ip, "ESC");
    case 0xdf: return show(ip, "ESC");
    case 0xe0: return decode_jump8(ip, "LOOPNE", reg_ip);
    case 0xe1: return decode_jump8(ip, "LOOPE", reg_ip);
    case 0xe2: return decode_jump8(ip, "LOOP", reg_ip);
    case 0xe3: return decode_jump8(ip, "JCXZ", reg_ip);
    case 0xe4: return decode_ald8(ip, "IN");
    case 0xe5: return decode_axd8(ip, "IN");
    case 0xe6: return decode_d8al(ip, "OUT");
    case 0xe7: return decode_d8ax(ip, "OUT");
    case 0xe8: return decode_jump(ip, "CALL", reg_ip);
    case 0xe9: return decode_jump(ip, "JMP", reg_ip);
    case 0xea: return decode_far(ip, "JMP");
    case 0xeb: return decode_jump8(ip, "JMP", reg_ip);
    case 0xec: return show_io(ip, "IN", "AL,DX");
    case 0xed: return show_io(ip, "IN", "AX,DX");
    case 0xee: return show_io(ip, "OUT", "DX,AL");
    case 0xef: return show_io(ip, "OUT", "DX,AX");
    case 0xf0: return show(ip, "LOCK");
    case 0xf1: return decode_databyte(ip, "DB");
    case 0xf2: return show_rep(ip, "REPNZ", reg_ip, segment_override);
    case 0xf3: return show_rep(ip, "REPZ", reg_ip, segment_override);
    case 0xf4: return show(ip, "HLT");
    case 0xf5: return show(ip, "CMC");
    case 0xf6: return decode_f6(ip, segment_override);
    case 0xf7: return decode_f7(ip, segment_override);
    case 0xf8: return show(ip, "CLC");
    case 0xf9: return show(ip, "STC");
    case 0xfa: return show(ip, "CLI");
    case 0xfb: return show(ip, "STI");
    case 0xfc: return show(ip, "CLD");
    case 0xfd: return show(ip, "STD");
    case 0xfe: return decode_b(ip, table_fe[(ip[1] & 0x38) >> 3], segment_override);
    case 0xff: return decode_ff(ip, segment_override);
    }
}
