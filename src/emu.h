#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include <stdio.h>

extern volatile int exit_cpu;
extern uint8_t memory[];

int cpuGetAddress(uint16_t segment, uint16_t offset);
int cpuGetAddrDS(uint16_t offset);
int cpuGetAddrES(uint16_t offset);

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

#endif // EMU_H
