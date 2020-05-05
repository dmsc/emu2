
#include "timer.h"
#include "dbg.h"
#include "emu.h"

#include <math.h>
#include <sys/time.h>

static uint16_t last_timer = 0;
static uint32_t bios_timer = 0;
static uint16_t bios_dater = 0;
void update_timer(void)
{
    struct timeval tv;
    struct timezone tz;
    static long start_day = 0;
    gettimeofday(&tv, &tz);
    if(start_day == 0)
        start_day = (tv.tv_sec - tz.tz_minuteswest * 60) / (24 * 60 * 60);

    int isec = (tv.tv_sec - tz.tz_minuteswest * 60) - (24 * 60 * 60) * start_day;
    long cnt = lrint((isec + tv.tv_usec * 0.000001) * 19663.0 / 1080.0);

    bios_timer = cnt % 0x1800B0;
    bios_dater = cnt / 0x1800B0;
    put32(0x46C, bios_timer);
    memory[0x470] = bios_dater & 0xFF;
}

// Returns port timer at 1193179.97HZ
static uint16_t get_port_timer(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    // us in microseconds
    double us = tv.tv_sec * 1000000.0 + tv.tv_usec;
    // Convert to "counts
    us = us * 1.19317997037;
    // And return "low part"
    return (lrint(fmod(us, 16777216.0)) & 0xFFFF);
}

uint32_t get_bios_timer(void)
{
    return bios_timer;
}

// Implement reading/writing to timer ports
static uint16_t port_value;
static uint8_t port_control;
uint8_t port_timer_read(uint16_t port)
{
    if(port == 0x43)
        return port_control;
    int tag = port_control & 0x30;
    if(tag == 0x20)
    {
        debug(debug_int, "timer port read $%02x = %02x (control=%02x)\n", port,
              port_value >> 8, port_control);
        return port_value >> 8;
    }
    else if(tag == 0x10)
    {
        debug(debug_int, "timer port read $%02x = %02x (control=%02x)\n", port,
              port_value & 0xFF, port_control);
        return port_value;
    }
    else if(tag == 0x30)
    {
        debug(debug_int, "timer port read $%02x = %02x (control=%02x)\n", port,
              port_value & 0xFF, port_control);
        port_control &= 0xCF;
        return port_value;
    }
    else // (tag == 0x00)
    {
        debug(debug_int, "timer port read $%02x = %02x (control=%02x)\n", port,
              port_value >> 8, port_control);
        port_control |= 0x30;
        return port_value >> 8;
    }
}

void port_timer_write(uint16_t port, uint8_t val)
{
    if(port == 0x43)
    {
        // Fill timer:
        port_control = val;
        if(0x00 == (port_control & 0x30))
            port_control |= 0x30;
        port_value = get_port_timer() - last_timer;
        debug(debug_int,
              "timer port write $%02x = %02x (latched val=%04x control=%02x)\n", port,
              val, port_value, port_control);
    }
    else if(port == 0x40)
    {
        int tag = port_control & 0x30;
        if(tag == 0x20)
        {
            last_timer = ((get_port_timer() + (val << 8)) & 0xFF00) + (last_timer & 0xFF);
        }
        else if(tag == 0x10)
        {
            last_timer = ((get_port_timer() + val) & 0xFF) + (last_timer & 0xFF00);
        }
        else if(tag == 0x30)
        {
            port_control &= 0xCF; // Go to state 00
            port_value = val;
        }
        else // tag == 0x00
        {
            port_control |= 0x30; // Go to state 30
            port_value = port_value | (val << 8);
            last_timer = get_port_timer() + port_value;
        }
        debug(debug_int,
              "timer port write $%02x = %02x (last=%04x val=%04x control=%02x)\n", port,
              val, last_timer, port_value, port_control);
    }
}

// BIOS TIMER
void int1A(void)
{
    debug(debug_int, "B-1A%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    unsigned ax = cpuGetAX();
    switch(ax >> 8)
    {
    case 0: // TIME
    {
        update_timer();
        cpuSetDX(bios_timer & 0xFFFF);
        cpuSetCX((bios_timer >> 16) & 0xFFFF);
        cpuSetAX(bios_dater);
        debug(debug_int, "GET TIME: %02x:%04x:%04x\n", cpuGetAX(), cpuGetCX(),
              cpuGetDX());
    }
    break;
    default:
        debug(debug_int, "UNHANDLED INT 1A, AX=%04x\n", ax);
    }
}
