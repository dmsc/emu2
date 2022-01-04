#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern volatile int exit_cpu;
extern uint8_t memory[];

int cpuGetAddress(uint16_t segment, uint16_t offset);
int cpuGetAddrDS(uint16_t offset);
int cpuGetAddrES(uint16_t offset);
int cpuGetAddrSS(uint16_t offset);

// Reads a word from the stack at displacement "disp"
uint16_t cpuGetStack(uint16_t disp);

uint8_t read_port(unsigned port);
void write_port(unsigned port, uint8_t value);
void bios_routine(unsigned inum);

// CPU interface
void execute(void); // 1 ins.
void init_cpu(void);

// async HW update
void emulator_update(void);

// Trigger hardware interrupts.
// IRQ-0 to IRQ-7 call INT-08 to INT-0F
// IRQ-8 to IRQ-F call INT-70 to INT-77
void cpuTriggerIRQ(int num);

// Register reading/writing
void cpuSetAL(unsigned v);
void cpuSetAX(unsigned v);
void cpuSetCX(unsigned v);
void cpuSetDX(unsigned v);
void cpuSetBX(unsigned v);
void cpuSetSP(unsigned v);
void cpuSetBP(unsigned v);
void cpuSetSI(unsigned v);
void cpuSetDI(unsigned v);
void cpuSetES(unsigned v);
void cpuSetCS(unsigned v);
void cpuSetSS(unsigned v);
void cpuSetDS(unsigned v);
void cpuSetIP(unsigned v);

unsigned cpuGetAX(void);
unsigned cpuGetCX(void);
unsigned cpuGetDX(void);
unsigned cpuGetBX(void);
unsigned cpuGetSP(void);
unsigned cpuGetBP(void);
unsigned cpuGetSI(void);
unsigned cpuGetDI(void);
unsigned cpuGetES(void);
unsigned cpuGetCS(void);
unsigned cpuGetSS(void);
unsigned cpuGetDS(void);
unsigned cpuGetIP(void);

// Alter flags in the stack, use from interrupt handling
enum cpuFlags
{
    cpuFlag_CF = 1,
    cpuFlag_PF = 4,
    cpuFlag_AF = 16,
    cpuFlag_ZF = 64,
    cpuFlag_SF = 128,
    cpuFlag_TF = 256,
    cpuFlag_IF = 512,
    cpuFlag_DF = 1024,
    cpuFlag_OF = 2048
};

void cpuSetFlag(enum cpuFlags flag);
void cpuClrFlag(enum cpuFlags flag);

// Alter direct CPU flags, only use on startup
void cpuSetStartupFlag(enum cpuFlags flag);
void cpuClrStartupFlag(enum cpuFlags flag);

#ifdef EMS_SUPPORT
#include "ems.h"
#endif

// Helper functions to access memory
// Read 16 bit number
static inline void put8(int addr, int v)
{
#ifdef EMS_SUPPORT
    if (in_ems_pageframe(addr)) {
	ems_put8(addr, v);
	return;
    }
#endif
    memory[0xFFFFF & (addr)] = v;
}

// Read 16 bit number
static inline void put16(int addr, int v)
{
    put8(addr, v);
    put8(addr + 1, v >> 8);
}

// Read 32 bit number
static inline void put32(int addr, unsigned v)
{
    put16(addr, v & 0xFFFF);
    put16(addr + 2, v >> 16);
}

// Write 8 bit number
static inline int get8(int addr)
{
#ifdef EMS_SUPPORT
    if (in_ems_pageframe(addr)) {
	return ems_get8(addr);
    }
#endif
    return memory[0xFFFFF & addr];
}

// Write 16 bit number
static inline int get16(int addr)
{
    return get8(addr) + (get8(addr + 1) << 8);
}

// Write 32 bit number
static inline unsigned get32(int addr)
{
    return get16(addr) + (get16(addr + 2) << 16);
}

// Copy data to CPU memory
static inline int putmem(uint32_t dest, const uint8_t *src, unsigned size)
{
    if(size >= 0x100000 || dest >= 0x100000 || size + dest >= 0x100000)
        return 1;
#ifdef EMS_SUPPORT
    if (in_ems_pageframe(dest)) {
	unsigned i;
	for (i = 0; i < size; i++)
	    ems_put8(dest++, *src++);
	return 0;
    }
#endif
    memcpy(memory + dest, src, size);
    return 0;
}

// Get pointer to CPU memory or null if overflow
static inline uint8_t *getptr(uint32_t addr, unsigned size)
{
    if(size >= 0x100000 || addr >= 0x100000 || size + addr >= 0x100000)
        return 0;
#ifdef EMS_SUPPORT
    if (in_ems_pageframe(addr))
	return 0;
#endif
    return memory + addr;
}

// Get a copy of CPU memory forcing a nul byte at end.
// Four static buffers are used, so at most 4 results can be in use.
static inline char *getstr(uint32_t addr, unsigned size)
{
    static int cbuf = 0;
    static char buf[4][256];

    cbuf = (cbuf + 1) & 3;
    memset(buf[cbuf], 0, 256);
#ifdef EMS_SUPPORT
    if (size < 255 && in_ems_pageframe(addr)) {
	int i;
	char *p = buf[cbuf];
	for (i = 0; i < size; i++) {
	    *p++ = ems_get8(addr++);
	}
    }
    else
#endif
    if(size < 255 && addr < 0x100000 && size + addr < 0x100000)
        memcpy(buf[cbuf], memory + addr, size);
    return buf[cbuf];
}

#endif // EMU_H
